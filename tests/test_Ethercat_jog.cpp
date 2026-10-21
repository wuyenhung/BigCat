// ============================================================================
// LiYang Tech - EtherCAT Jog Test Tool
// Copyright (c) 2025 LiYang Tech. All rights reserved.
// Maintainer: wuyenhung@gmail.com

// Standalone debugging tool. Does NOT depend on BigCat libraries.
// Build: CMake target excluded from smoke-test groups.
// Usage: test_Ethercat_jog [ifname] [-core <N>] [-cycle <us>]
//   -core  <N>  : pin SOEM thread to logical CPU N
//   -cycle <us> : PDO cycle time in us (100..10000, default 2000)
//   -list-cores : print CPU topology and exit
// Log: test_Ethercat_jog.log
// ============================================================================
#include <soem/soem.h>

#include <windows.h>
#include <intrin.h>
#include <conio.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#include <sstream>
#include <cmath>
#include <pcap.h>       // Npcap version detection
#include <mmsystem.h>   // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kCycleUsMin = 100;      // 0.1 ms
constexpr int kCycleUsMax = 10000;    // 10 ms
constexpr int kCycleUsDefault = 2000; // 2 ms
int g_cycle_us = kCycleUsDefault;
constexpr int kEthercatOpTimeoutMs = 12000;
constexpr int kDriveStateTimeoutMs = 1000;

// -- High-resolution QPC-based cycle timing --
// TSC chunked spin-wait with SwitchToThread for NIC DPC drain.
// Sleep(0) / Sleep(1) avoided — both cause unpredictable deschedule
// on Win11 with incomplete CPU isolation.
// OS is auto-detected at init via RtlGetVersion (for diagnostic logging only).
namespace cycle_timer {
    static LARGE_INTEGER g_qpcFreq{};
    static bool          g_qpcValid = false;
    static bool          g_isWin11 = false;   // auto-detected in init()
    static bool          g_win10Forced = false; // BigCat.ini [Compat] ForceWin10Mode=1

    // Read [Compat] ForceWin10Mode from BigCat.ini next to this executable (the
    // same file the main app uses) so this tool can reproduce the app's Win10
    // wait strategy on Win11. Missing key/file => 0 (disabled).
    static bool ReadForcedWin10ModeFromBigCatIni() {
        wchar_t exePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (!slash) return false;
        *(slash + 1) = L'\0';
        std::wstring iniPath = std::wstring(exePath) + L"BigCat.ini";
        return GetPrivateProfileIntW(L"Compat", L"ForceWin10Mode", 0, iniPath.c_str()) != 0;
    }

    void init() {
        g_qpcValid = (QueryPerformanceFrequency(&g_qpcFreq) && g_qpcFreq.QuadPart > 0);
        // Detect Win11 via build number (Win11 = 22000+)
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
            auto fn = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
            if (fn) {
                RTL_OSVERSIONINFOW info = { sizeof(info) };
                if (fn(&info) == 0) g_isWin11 = (info.dwBuildNumber >= 22000);
            }
        }
        // Manual override: behave like Win10 (HR waitable timer + TSC margin).
        if (ReadForcedWin10ModeFromBigCatIni()) {
            g_win10Forced = g_isWin11;
            g_isWin11 = false;
        }
    }

    inline LARGE_INTEGER now() {
        LARGE_INTEGER t{};
        if (g_qpcValid) QueryPerformanceCounter(&t);
        return t;
    }

    // Microseconds elapsed between two QPC ticks
    inline uint32_t us_between(LARGE_INTEGER a, LARGE_INTEGER b) {
        if (!g_qpcValid || b.QuadPart <= a.QuadPart) return 0;
        return static_cast<uint32_t>(((b.QuadPart - a.QuadPart) * 1000000ULL) / (uint64_t)g_qpcFreq.QuadPart);
    }

    // TSC spin-wait for short sub-200us gaps (invariant TSC required)
    static bool g_tscValid = false;
    static double g_tscPerUs = 0.0;

    void calibrate_tsc() {
        if (!g_qpcValid) return;
        // Check invariant TSC via CPUID
        int regs[4] = {};
        __cpuidex(regs, 0x80000007, 0);
        if (!(regs[3] & (1 << 8))) return;  // EDX bit 8 = invariant TSC

        LARGE_INTEGER q1, q2;
        unsigned int aux = 0;
        QueryPerformanceCounter(&q1);
        unsigned __int64 t1 = __rdtscp(&aux);
        // Wait ~1ms of QPC ticks for calibration
        LONGLONG waitTicks = g_qpcFreq.QuadPart / 1000;
        do { QueryPerformanceCounter(&q2); } while (q2.QuadPart - q1.QuadPart < waitTicks);
        unsigned __int64 t2 = __rdtscp(&aux);
        LONGLONG qDiff = q2.QuadPart - q1.QuadPart;
        if (qDiff <= 0) return;
        double us = (double)qDiff * 1000000.0 / (double)g_qpcFreq.QuadPart;
        g_tscPerUs = (double)(t2 - t1) / us;
        g_tscValid = (g_tscPerUs > 0.0);
    }

    inline void spin_us(uint32_t us) {
        if (!g_tscValid || us == 0) return;
        unsigned int aux = 0;
        unsigned __int64 deadline = __rdtscp(&aux) + (unsigned __int64)((double)us * g_tscPerUs);
        unsigned int pollCount = 0;
        while (__rdtscp(&aux) < deadline) {
            // Periodic kernel entry via SwitchToThread to drain NIC DPCs.
            // SwitchToThread returns immediately if no runnable thread on
            // this core (safe for isolated RT cores).  Avoids Sleep(0)
            // which causes 30-40ms deschedule on Win11.
            if ((++pollCount & 0xFFu) == 0u) {
                SwitchToThread();
            }
            YieldProcessor();
        }
    }
}
constexpr int kDefaultJogStep = 200;
constexpr int kJogStepSize   = 200;
constexpr int kDefaultJogSpeed = 500000;
constexpr int kJogSpeedStep   = 100000;
constexpr int kDefaultTorqueLimit = 200;
constexpr int kTorqueLimitStep   = 100;
constexpr int kTorqueLimitMax    = 3000;

// Unified 7-entry PDO (0x1600/0x1A00) for ALL slaves
constexpr int kRxBytes = 17;
constexpr int kTxBytes = 19;
constexpr int kOutTargetPosition = 0, kOutTargetVelocity = 4, kOutTargetTorque = 8;
constexpr int kOutPositiveTorque = 10, kOutNegativeTorque = 12;
constexpr int kOutControlword = 14, kOutModeOfOperation = 16;
constexpr int kInActualPosition = 0, kInActualVelocity = 4, kInActualTorque = 8;
constexpr int kInStatusword = 10, kInErrorCode = 12, kInModeDisplay = 14;
constexpr int kInDigitalInputs = 15;

// RxPDO entries: DVS uses 0x60E0/0x60E1, X1S uses 0x2019/0x201A
constexpr uint32_t kRxDvs[7] = {
    0x607A0020, 0x60FF0020, 0x60710010, 0x60E00010,
    0x60E10010, 0x60400010, 0x60600008};
constexpr uint32_t kRxX1s[7] = {
    0x607A0020, 0x60FF0020, 0x60710010, 0x20190010,
    0x201A0010, 0x60400010, 0x60600008};
constexpr uint32_t kTxEntries7[7] = {
    0x60640020, 0x606C0020, 0x60770010, 0x60410010,
    0x603F0010, 0x60610008, 0x60FD0020};

FILE* g_log = nullptr;
int   g_jog_step  = kDefaultJogStep;
int   g_jog_speed = kDefaultJogSpeed;
int   g_torque_pos = kDefaultTorqueLimit;  // 0x60E0
int   g_torque_neg = kDefaultTorqueLimit;  // 0x60E1
int   g_core_index = -1;  // -1 = no affinity; >=0 = logical CPU to pin SOEM thread to (matches Task Manager)

// Vectored Exception Handler (VEH) for Win11 Npcap crash capture
// VEH fires before SEH and can catch exceptions even when the stack is
// slightly corrupted, which matters for Npcap driver interactions on Win11.
static FILE*      g_vehCrashLog = nullptr;
static const char* g_vehPhase    = "none";
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS exInfo)
{
    DWORD code = exInfo->ExceptionRecord->ExceptionCode;
    void* addr = exInfo->ExceptionRecord->ExceptionAddress;
    // Log to file (fopen inside VEH is risky but acceptable as last-resort)
    FILE* f = fopen("test_Ethercat_jog_crash.log", "a");
    if (f) {
        fprintf(f, "VEH: exception 0x%08lX at 0x%p phase=%s\n", code, addr, g_vehPhase);
        fclose(f);
    }
    if (g_vehCrashLog) {
        fprintf(g_vehCrashLog, "VEH: exception 0x%08lX at 0x%p phase=%s\n", code, addr, g_vehPhase);
        fflush(g_vehCrashLog);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // let SEH handle it next
}

void log_ts(const char* fmt, ...)
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

    va_list ap;
    va_start(ap, fmt);
    va_list file_ap;
    va_copy(file_ap, ap);

    if (g_log) {
        fprintf(g_log, "[%s] ", ts);
        vfprintf(g_log, fmt, file_ap);
        fflush(g_log);
    }

    printf("[%s] ", ts);
    vprintf(fmt, ap);

    va_end(file_ap);
    va_end(ap);
}

// CPU vendor detection via CPUID
const char* cpu_vendor_string()
{
    static char vendor[13] = {};
    if (vendor[0]) return vendor;
    int regs[4] = {};
    __cpuid(regs, 0);
    std::memcpy(vendor + 0, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    vendor[12] = '\0';
    return vendor;
}

// CPU brand string via CPUID 0x80000002..0x80000004
const char* cpu_brand_string()
{
    static char brand[49] = {};
    if (brand[0]) return brand;
    int regs[4] = {};
    for (int leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
        __cpuid(regs, leaf);
        std::memcpy(brand + (leaf - 0x80000002) * 16 + 0, &regs[0], 4);
        std::memcpy(brand + (leaf - 0x80000002) * 16 + 4, &regs[1], 4);
        std::memcpy(brand + (leaf - 0x80000002) * 16 + 8, &regs[2], 4);
        std::memcpy(brand + (leaf - 0x80000002) * 16 + 12, &regs[3], 4);
    }
    brand[48] = '\0';
    // Trim leading spaces
    char* p = brand;
    while (*p == ' ') ++p;
    return p;
}

// Print CPU topology in a human-readable table, matching Task Manager
// numbering. Dynamically queries OS kernel topology no hardcoded
// assumptions about Intel vs AMD core/SMT ordering.
void print_cpu_topology()
{
    DWORD bufSize = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufSize);
    if (bufSize == 0) {
        printf("(CPU topology unavailable)\n");
        return;
    }

    std::vector<BYTE> buffer(bufSize);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bufSize)) {
        printf("(CPU topology query failed, err=%lu)\n", GetLastError());
        return;
    }

 // Detect SMT pattern: interleaved (Intel) vs sequential (AMD)
    // Interleaved: core N has LPs {N, N+Nphys}  LP gap core count
    // Sequential:  core N has LPs {2N, 2N+1}     LP gap = 1
    bool hasHybrid = false;
    int maxThreadsPerCore = 0;
    int minLpGap = 0x7FFFFFFF;
    {
        BYTE* p = buffer.data();
        BYTE* e = p + bufSize;
        while (p < e) {
            auto* en = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(p);
            if (en->Size == 0) break;
            if (en->Relationship == RelationProcessorCore) {
                DWORD_PTR m = 0;
                for (WORD g = 0; g < en->Processor.GroupCount; ++g)
                    m |= en->Processor.GroupMask[g].Mask;
                std::vector<int> lps;
                DWORD_PTR t = m;
                int b = 0;
                while (t) { if (t & 1) lps.push_back(b); t >>= 1; ++b; }
                int n = (int)lps.size();
                if (n > maxThreadsPerCore) maxThreadsPerCore = n;
                if (n >= 2) {
                    int gap = lps[1] - lps[0];
                    if (gap < minLpGap) minLpGap = gap;
                }
                if (en->Processor.EfficiencyClass != 0) hasHybrid = true;
            }
            p += en->Size;
        }
    }

 // Header
    printf("=== CPU Topology (matches Task Manager numbering) ===\n");
    printf("  Model  : %s\n", cpu_brand_string());
    printf("  Vendor : %s\n", cpu_vendor_string());
    if (hasHybrid)
        printf("  Arch   : hybrid (P-core + E-core), EfficiencyClass from kernel\n");
    else if (maxThreadsPerCore >= 2 && minLpGap == 1)
        printf("  SMT    : sequential (consecutive LPs share a core -- typical AMD)\n");
    else if (maxThreadsPerCore >= 2)
        printf("  SMT    : interleaved (LPs spaced by core count -- typical Intel)\n");
    else
        printf("  SMT    : disabled (1 thread per core)\n");
    printf("\n");
    printf("  Legend: * = primary thread (prefer for RT)   HT = SMT sibling\n");
    printf("\n");
    printf("%-16s %-10s %-36s %s\n", "Physical Core", "Type", "Logical CPUs (TaskMgr)", "Mask");
    printf("%-16s %-10s %-36s %s\n", "-------------", "----", "------------------------------------", "----");

    int physIdx = 0;
    int totalLogical = 0;
    BYTE* ptr = buffer.data();
    BYTE* end = ptr + bufSize;

    while (ptr < end) {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
        if (entry->Size == 0) break;

        if (entry->Relationship == RelationProcessorCore) {
            DWORD_PTR fullMask = 0;
            for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
                fullMask |= entry->Processor.GroupMask[g].Mask;

            // Gather logical CPU indices from mask
            std::vector<int> lps;
            DWORD_PTR m = fullMask;
            int bit = 0;
            while (m) {
                if (m & 1) lps.push_back(bit);
                m >>= 1;
                ++bit;
            }
            totalLogical += (int)lps.size();

            // Label: P-core / E-core / Physical
            const char* typeLabel = "Physical";
            if (entry->Processor.EfficiencyClass == 1)
                typeLabel = "E-core";
            else if (entry->Processor.EfficiencyClass >= 2)
                typeLabel = "P-core";
            else if (lps.size() >= 2)
                typeLabel = "SMT";

            // Build LP list string with primary(*)/HT markers for SMT cores
            std::stringstream lpStr;
            bool isHybridCore = (entry->Processor.EfficiencyClass != 0);
            for (size_t k = 0; k < lps.size(); ++k) {
                if (k > 0) lpStr << ", ";
                lpStr << "LP " << lps[k];
                if (!isHybridCore && lps.size() >= 2) {
                    lpStr << (k == 0 ? "*" : "(HT)");
                }
            }

            printf("Core %-11d %-10s %-36s 0x%llX\n",
                   physIdx, typeLabel, lpStr.str().c_str(),
                   (unsigned long long)fullMask);
            ++physIdx;
        }
        ptr += entry->Size;
    }

    printf("-----------------------------------------------------------------\n");
    printf("Total: %d physical cores, %d logical processors\n", physIdx, totalLogical);
    printf("Use -core <N> where N matches LP number above (0..%d)\n", totalLogical - 1);
}

// Pin the calling thread to the exact logical CPU given by the user
// (0-based, matching Task Manager numbering). Also logs which physical
// core owns this LP for reference.
// Returns true on success; logs details to g_log/console.
bool pin_thread_to_physical_core(int logicalCpuIndex)
{
    if (logicalCpuIndex < 0) return true;  // no affinity requested

    DWORD_PTR targetMask = (DWORD_PTR)1 << logicalCpuIndex;

 // Look up which physical core owns this LP (informational only)
    int physCoreIdx = -1;
    DWORD bufSize = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufSize);
    if (bufSize > 0) {
        std::vector<BYTE> buffer(bufSize);
        if (GetLogicalProcessorInformationEx(RelationProcessorCore,
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bufSize)) {
            int idx = 0;
            BYTE* ptr = buffer.data();
            BYTE* end = ptr + bufSize;
            while (ptr < end) {
                auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
                if (entry->Size == 0) break;
                if (entry->Relationship == RelationProcessorCore) {
                    DWORD_PTR fullMask = 0;
                    for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
                        fullMask |= entry->Processor.GroupMask[g].Mask;
                    if (fullMask & targetMask) {
                        physCoreIdx = idx;
                        break;
                    }
                    ++idx;
                }
                ptr += entry->Size;
            }
        }
    }

 // Step 1: Ensure process affinity includes the target LP
    // Child processes (e.g. launched from VS Code terminal) may inherit
    // a restricted affinity that excludes some LPs error 87.
    DWORD_PTR procMask = 0, sysMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask)) {
        if (!(procMask & targetMask)) {
            printf("[Affinity] LP %d not in process mask 0x%llX, expanding to sys mask 0x%llX\n",
                   logicalCpuIndex, (unsigned long long)procMask, (unsigned long long)sysMask);
            if (g_log) {
                log_ts("[Affinity] LP %d not in process mask 0x%llX, expanding to sys mask 0x%llX\n",
                       logicalCpuIndex, (unsigned long long)procMask, (unsigned long long)sysMask);
            }
            if (!SetProcessAffinityMask(GetCurrentProcess(), sysMask)) {
                DWORD err = GetLastError();
                printf("[Affinity] SetProcessAffinityMask(sys=0x%llX) FAILED (err=%lu)\n",
                       (unsigned long long)sysMask, err);
                if (g_log) {
                    log_ts("[Affinity] SetProcessAffinityMask(sys=0x%llX) FAILED (err=%lu)\n",
                           (unsigned long long)sysMask, err);
                }
                return false;
            }
            // Re-read after expansion
            GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
            printf("[Affinity] process mask now 0x%llX\n", (unsigned long long)procMask);
        }
    }

 // Step 2: Set hard affinity mask to the EXACT logical CPU
    DWORD_PTR oldMask = SetThreadAffinityMask(GetCurrentThread(), targetMask);
    if (oldMask == 0) {
        DWORD err = GetLastError();
        printf("[Affinity] SetThreadAffinityMask(LP %d) FAILED (err=%lu)\n", logicalCpuIndex, err);
        if (g_log) {
            log_ts("[Affinity] SetThreadAffinityMask(LP %d) FAILED (err=%lu)\n", logicalCpuIndex, err);
        }
        return false;
    }

 // Step 3: Set ideal processor as fallback hint (belt + suspenders)
    SetThreadIdealProcessor(GetCurrentThread(), (DWORD)logicalCpuIndex);

 // Step 4: Yield to let the scheduler migrate the thread
    Sleep(1);
    // Verify migration actually happened (retry up to ~50 ms)
    int verifyAttempts = 0;
    DWORD actualCpu = GetCurrentProcessorNumber();
    while (actualCpu != (DWORD)logicalCpuIndex && verifyAttempts < 50) {
        Sleep(1);
        actualCpu = GetCurrentProcessorNumber();
        ++verifyAttempts;
    }

    if (physCoreIdx >= 0) {
        printf("[Affinity] LP %d mask=0x%llX physCore=%d cpu=%lu verify=%s\n",
               logicalCpuIndex, (unsigned long long)targetMask, physCoreIdx,
               actualCpu,
               (actualCpu == (DWORD)logicalCpuIndex) ? "OK" : "MISMATCH");
        if (g_log) {
            log_ts("[Affinity] LP %d mask=0x%llX physCore=%d cpu=%lu verify=%s\n",
                   logicalCpuIndex, (unsigned long long)targetMask, physCoreIdx,
                   actualCpu,
                   (actualCpu == (DWORD)logicalCpuIndex) ? "OK" : "MISMATCH");
        }
    } else {
        printf("[Affinity] LP %d mask=0x%llX cpu=%lu verify=%s\n",
               logicalCpuIndex, (unsigned long long)targetMask,
               actualCpu,
               (actualCpu == (DWORD)logicalCpuIndex) ? "OK" : "MISMATCH");
        if (g_log) {
            log_ts("[Affinity] LP %d mask=0x%llX cpu=%lu verify=%s\n",
                   logicalCpuIndex, (unsigned long long)targetMask,
                   actualCpu,
                   (actualCpu == (DWORD)logicalCpuIndex) ? "OK" : "MISMATCH");
        }
    }

    if (actualCpu != (DWORD)logicalCpuIndex) {
        printf("[Affinity] WARNING: thread did not migrate to LP %d (still on cpu %lu)\n",
               logicalCpuIndex, actualCpu);
        if (g_log) {
            log_ts("[Affinity] WARNING: thread did not migrate to LP %d (still on cpu %lu)\n",
                   logicalCpuIndex, actualCpu);
        }
        // Don't fail the hard-affinity mask is set and will take effect
        // on next scheduling decision.
    }

    return true;
}

bool is_state(int state, uint16_t expected)
{
    return (state & expected) == expected;
}

// Cyclic PDO exchange with closed-loop QPC-based timing.
// Each call: send -> recv -> wait until cycle boundary.
//
// Win11 & Win10: TSC chunked spin with SwitchToThread for DPC drain.
//   Sleep(1) and Sleep(0) avoided — both cause unpredictable deschedule
//   on Win11 with incomplete CPU isolation.
int cycle_exchange(ecx_contextt& ctx, uint32_t cycleUs, LARGE_INTEGER& phaseRef)
{
    ecx_send_processdata(&ctx);
    int wkc = ecx_receive_processdata(&ctx, EC_TIMEOUTRET);

    LONGLONG periodTicks = ((LONGLONG)cycleUs * cycle_timer::g_qpcFreq.QuadPart) / 1000000LL;
    phaseRef.QuadPart += periodTicks;

    LARGE_INTEGER cur = cycle_timer::now();
    LONGLONG remainTicks = phaseRef.QuadPart - cur.QuadPart;

    if (remainTicks > 0) {
        int64_t remainUs = (remainTicks * 1000000LL) / cycle_timer::g_qpcFreq.QuadPart;
        // TSC spin-wait in 500us chunks with SwitchToThread for DPC drain
        while (remainUs > 0 && remainTicks > 0) {
            uint32_t chunkUs = (uint32_t)(remainUs > 500 ? 500 : remainUs);
            cycle_timer::spin_us(chunkUs);
            cur = cycle_timer::now();
            remainTicks = phaseRef.QuadPart - cur.QuadPart;
            remainUs = (remainTicks > 0) ? (int64_t)((remainTicks * 1000000LL) / cycle_timer::g_qpcFreq.QuadPart) : 0;
        }
    } else if (remainTicks < -periodTicks) {
        phaseRef = cur;
    }

    return wkc;
}

// QPC-closed-loop pump for state-machine paths
// Uses cycle_exchange() internally so that PDO timing stays consistent
// with the DC SYNC0 period.  On Win11, the old osal_usleep-based pump
// produced irregular cycle periods (SleepEx granularity) that caused
// drives to ignore CiA402 state transitions (stuck at ReadyToSwOn).
// A function-local static phaseRef keeps phase continuity across
// consecutive calls from the same caller (e.g. wait_axis_status loop).
int pump_processdata(ecx_contextt& ctx, int rounds, int /*sleep_us*/)
{
    static LARGE_INTEGER phaseRef = cycle_timer::now();
    // Reset phase reference if called after a long gap (> 5? cycle period)
    // to avoid accumulating a huge negative phase offset.
    {
        LARGE_INTEGER cur = cycle_timer::now();
        LONGLONG maxGap = (5000LL * cycle_timer::g_qpcFreq.QuadPart) / 1000000LL;
        if (cur.QuadPart - phaseRef.QuadPart > maxGap) {
            phaseRef = cur;
        }
    }
    int wkc = 0;
    for (int r = 0; r < rounds; ++r) {
        wkc = cycle_exchange(ctx, g_cycle_us, phaseRef);
    }
    return wkc;
}

// Pump PDO for `duration_ms` milliseconds using `cycle_us` per round.
// Uses QPC-closed-loop timing (cycle_exchange) for stable periods on Win11.
// Logs WKC + cycle statistics periodically so we can see if frames are flowing.
void pump_stabilize(ecx_contextt& ctx, int duration_ms, int cycle_us, const char* label)
{
    constexpr int kLogIntervalMs = 1000;
    const int totalRounds = (duration_ms * 1000) / cycle_us;
    int lastLogRound = -1;
    int minWkc = 999, maxWkc = 0;

    // Initialize cycle phase reference for closed-loop timing
    LARGE_INTEGER phaseRef = cycle_timer::now();

    for (int r = 0; r < totalRounds; ++r) {
        int wkc = cycle_exchange(ctx, (uint32_t)cycle_us, phaseRef);
        if (wkc < minWkc) minWkc = wkc;
        if (wkc > maxWkc) maxWkc = wkc;

        int elapsedMs = (r * cycle_us) / 1000;
        if (elapsedMs / kLogIntervalMs != lastLogRound / kLogIntervalMs) {
            lastLogRound = elapsedMs;
            // Measure actual cycle period for diagnostics
            static LARGE_INTEGER prevQpc{};
            LARGE_INTEGER curQpc = cycle_timer::now();
            uint32_t actualUs = (prevQpc.QuadPart > 0)
                ? cycle_timer::us_between(prevQpc, curQpc) : cycle_us;
            prevQpc = curQpc;
            log_ts("[%s] %d/%dms WKC=%d (min=%d max=%d) cycle=%uus\n",
                   label, elapsedMs, duration_ms, wkc, minWkc, maxWkc, actualUs);
            minWkc = 999; maxWkc = 0;
        }
    }
    log_ts("[%s] complete: %dms total\n", label, duration_ms);
}

void log_slave_states(ecx_contextt& ctx)
{
    ecx_readstate(&ctx);
    for (int si = 1; si <= ctx.slavecount; ++si) {
        const char* al = ec_ALstatuscode2string(ctx.slavelist[si].ALstatuscode);
        log_ts("  S%d: state=0x%02X AL=0x%04X %s O=%u I=%u SM2=%u SM3=%u\n",
               si,
               ctx.slavelist[si].state,
               ctx.slavelist[si].ALstatuscode,
               al ? al : "",
               ctx.slavelist[si].Obytes,
               ctx.slavelist[si].Ibytes,
               etohs(ctx.slavelist[si].SM[2].SMlength),
               etohs(ctx.slavelist[si].SM[3].SMlength));
    }
}

bool all_slaves_in_state(ecx_contextt& ctx, uint16_t expected)
{
    ecx_readstate(&ctx);
    for (int si = 1; si <= ctx.slavecount; ++si) {
        if (!is_state(ctx.slavelist[si].state, expected)) {
            return false;
        }
    }
    return true;
}

bool wait_all_slaves_op(ecx_contextt& ctx, int timeout_ms)
{
    const auto start = std::chrono::steady_clock::now();
    int last_log_ms = -1;
    int minWkc = 999, maxWkc = 0;

    // Closed-loop cycle phase reference
    LARGE_INTEGER phaseRef = cycle_timer::now();

    while (true) {
        int wkc = cycle_exchange(ctx, g_cycle_us, phaseRef);
        if (wkc < minWkc) minWkc = wkc;
        if (wkc > maxWkc) maxWkc = wkc;

        const auto elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        if (elapsed_ms / 500 != last_log_ms / 500) {
            last_log_ms = elapsed_ms;
            ecx_readstate(&ctx);
            char line[512];
            int used = std::snprintf(line, sizeof(line), "[%dms]", elapsed_ms);
            for (int si = 1; si <= ctx.slavecount; ++si) {
                if (used < 0 || used >= static_cast<int>(sizeof(line))) break;
                used += std::snprintf(line + used,
                                      sizeof(line) - static_cast<size_t>(used),
                                      " S%d:0x%02X", si, ctx.slavelist[si].state);
            }
            // Append WKC range since last log
            if (used < (int)sizeof(line) - 30) {
                used += std::snprintf(line + used, sizeof(line) - used,
                                      " WKC=%d..%d", minWkc, maxWkc);
            }
            log_ts("%s\n", line);
            minWkc = 999; maxWkc = 0;
        }

        if (all_slaves_in_state(ctx, EC_STATE_OPERATIONAL)) {
            return true;
        }
        if (elapsed_ms >= timeout_ms) {
            return false;
        }
    }
}

bool write_sdo_u8(ecx_contextt& ctx, int slave, uint16_t index, uint8_t subindex, uint8_t value)
{
    const int rc = ecx_SDOwrite(&ctx, slave, index, subindex, FALSE, sizeof(value), &value, EC_TIMEOUTRXM);
    if (rc <= 0) {
        log_ts("S%d SDO write 0x%04X:%02X failed rc=%d\n", slave, index, subindex, rc);
        return false;
    }
    return true;
}

bool write_sdo_u16(ecx_contextt& ctx, int slave, uint16_t index, uint8_t subindex, uint16_t value)
{
    const int rc = ecx_SDOwrite(&ctx, slave, index, subindex, FALSE, sizeof(value), &value, EC_TIMEOUTRXM);
    if (rc <= 0) {
        log_ts("S%d SDO write 0x%04X:%02X failed rc=%d\n", slave, index, subindex, rc);
        return false;
    }
    return true;
}

bool write_sdo_u32(ecx_contextt& ctx, int slave, uint16_t index, uint8_t subindex, uint32_t value)
{
    const int rc = ecx_SDOwrite(&ctx, slave, index, subindex, FALSE, sizeof(value), &value, EC_TIMEOUTRXM);
    if (rc <= 0) {
        log_ts("S%d SDO write 0x%04X:%02X failed rc=%d\n", slave, index, subindex, rc);
        return false;
    }
    return true;
}

bool configure_slave_pdo(ecx_contextt& ctx, int slave)
{
    if ((ctx.slavelist[slave].state & 0x0F) != EC_STATE_PRE_OP) {
        ctx.slavelist[slave].state = EC_STATE_INIT;
        ecx_writestate(&ctx, slave);
        ecx_statecheck(&ctx, slave, EC_STATE_INIT, EC_TIMEOUTSTATE);
        ctx.slavelist[slave].state = EC_STATE_PRE_OP;
        ecx_writestate(&ctx, slave);
        ecx_statecheck(&ctx, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
    }

    bool ok = true;
    ok &= write_sdo_u8(ctx, slave, 0x1C12, 0, 0);
    ok &= write_sdo_u8(ctx, slave, 0x1C13, 0, 0);

    const bool isX1s = (ctx.slavelist[slave].eep_man == 0x766);
    const uint32_t* rx = isX1s ? kRxX1s : kRxDvs;
    ok &= write_sdo_u8(ctx, slave, 0x1600, 0, 0);
    for (int j = 0; j < 7; ++j)
        ok &= write_sdo_u32(ctx, slave, 0x1600, (uint8_t)(j+1), rx[j]);
    ok &= write_sdo_u8(ctx, slave, 0x1600, 0, 7);
    ok &= write_sdo_u8(ctx, slave, 0x1A00, 0, 0);
    for (int j = 0; j < 7; ++j)
        ok &= write_sdo_u32(ctx, slave, 0x1A00, (uint8_t)(j+1), kTxEntries7[j]);
    ok &= write_sdo_u8(ctx, slave, 0x1A00, 0, 7);

    ok &= write_sdo_u16(ctx, slave, 0x1C12, 1, 0x1600);
    ok &= write_sdo_u8(ctx, slave, 0x1C12, 0, 1);
    ok &= write_sdo_u16(ctx, slave, 0x1C13, 1, 0x1A00);
    ok &= write_sdo_u8(ctx, slave, 0x1C13, 0, 1);

    if (etohs(ctx.slavelist[slave].SM[2].SMlength) < (unsigned)kRxBytes)
        ctx.slavelist[slave].SM[2].SMlength = htoes(kRxBytes);
    if (etohs(ctx.slavelist[slave].SM[3].SMlength) < (unsigned)kTxBytes)
        ctx.slavelist[slave].SM[3].SMlength = htoes(kTxBytes);

    return ok;
}

uint8_t* slave_outputs(ecx_contextt& ctx, int slave)
{
    return reinterpret_cast<uint8_t*>(ctx.slavelist[slave].outputs);
}

uint8_t* slave_inputs(ecx_contextt& ctx, int slave)
{
    return reinterpret_cast<uint8_t*>(ctx.slavelist[slave].inputs);
}

bool has_axis_pdo(ecx_contextt& ctx, int slave)
{
    return slave >= 1 && slave <= ctx.slavecount &&
           slave_outputs(ctx, slave) != nullptr &&
           slave_inputs(ctx, slave) != nullptr &&
           ctx.slavelist[slave].Obytes >= static_cast<unsigned>(kRxBytes) &&
           ctx.slavelist[slave].Ibytes >= static_cast<unsigned>(kTxBytes);
}

void write_u16(uint8_t* base, int offset, uint16_t value)
{
    std::memcpy(base + offset, &value, sizeof(value));
}

void write_i32(uint8_t* base, int offset, int32_t value)
{
    std::memcpy(base + offset, &value, sizeof(value));
}

uint16_t read_u16(const uint8_t* base, int offset)
{
    uint16_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

uint32_t read_u32(const uint8_t* base, int offset)
{
    uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

int32_t read_i32(const uint8_t* base, int offset)
{
    int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

int8_t read_i8(const uint8_t* base, int offset)
{
    int8_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

uint16_t axis_statusword(ecx_contextt& ctx, int slave)
{
    return read_u16(slave_inputs(ctx, slave), kInStatusword);
}

uint16_t axis_error_code(ecx_contextt& ctx, int slave)
{
    return read_u16(slave_inputs(ctx, slave), kInErrorCode);
}

int8_t axis_mode_display(ecx_contextt& ctx, int slave)
{
    return read_i8(slave_inputs(ctx, slave), kInModeDisplay);
}

int32_t axis_actual_position(ecx_contextt& ctx, int slave)
{
    return read_i32(slave_inputs(ctx, slave), kInActualPosition);
}

int32_t axis_actual_velocity(ecx_contextt& ctx, int slave)
{
    if (kInActualVelocity < 0) return 0;
    return read_i32(slave_inputs(ctx, slave), kInActualVelocity);
}

bool status_matches(uint16_t statusword, uint16_t mask, uint16_t value)
{
    return (statusword & mask) == value;
}

bool is_fault(uint16_t statusword)
{
    return (statusword & 0x0008) != 0;
}

bool is_ready_to_switch_on(uint16_t statusword)
{
    return status_matches(statusword, 0x006F, 0x0021);
}

bool is_switched_on(uint16_t statusword)
{
    return status_matches(statusword, 0x006F, 0x0023) ||
           status_matches(statusword, 0x006F, 0x0027);
}

bool is_operation_enabled(uint16_t statusword)
{
    return status_matches(statusword, 0x006F, 0x0027);
}

void stage_axis_command_csp(ecx_contextt& ctx, int slave, int32_t target_position, uint16_t controlword)
{
    uint8_t* out = slave_outputs(ctx, slave);
    write_i32(out, kOutTargetPosition, target_position);
    write_i32(out, kOutTargetVelocity, 0);
    write_u16(out, kOutTargetTorque, 0);
    if (kOutPositiveTorque >= 0) write_u16(out, kOutPositiveTorque, static_cast<uint16_t>(g_torque_pos));
    if (kOutNegativeTorque >= 0) write_u16(out, kOutNegativeTorque, static_cast<uint16_t>(g_torque_neg));
    write_u16(out, kOutControlword, controlword);
    out[kOutModeOfOperation] = 8; // CSP
}

void stage_axis_command_csv(ecx_contextt& ctx, int slave, int32_t target_velocity, uint16_t controlword)
{
    uint8_t* out = slave_outputs(ctx, slave);
    write_i32(out, kOutTargetPosition, 0);
    write_i32(out, kOutTargetVelocity, target_velocity);
    write_u16(out, kOutTargetTorque, 0);
    if (kOutPositiveTorque >= 0) write_u16(out, kOutPositiveTorque, static_cast<uint16_t>(g_torque_pos));
    if (kOutNegativeTorque >= 0) write_u16(out, kOutNegativeTorque, static_cast<uint16_t>(g_torque_neg));
    write_u16(out, kOutControlword, controlword);
    out[kOutModeOfOperation] = 9; // CSV
}

template <typename Pred>
bool wait_axis_status(ecx_contextt& ctx, int slave, int timeout_ms, Pred pred, const char* label)
{
    const auto start = std::chrono::steady_clock::now();
    int last_log_ms = -1;

    while (true) {
        pump_processdata(ctx, 1, g_cycle_us);
        const uint16_t sw = axis_statusword(ctx, slave);
        if (pred(sw)) {
            log_ts("Axis %d %s reached: statusword=0x%04X mode_display=%d error=0x%04X\n",
                   slave,
                   label,
                   sw,
                   static_cast<int>(axis_mode_display(ctx, slave)),
                   axis_error_code(ctx, slave));
            return true;
        }

        const auto elapsed_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start)
                .count());

        if (elapsed_ms / 100 != last_log_ms / 100) {
            last_log_ms = elapsed_ms;
            log_ts("Axis %d waiting %s: %dms sw=0x%04X mode_display=%d error=0x%04X\n",
                   slave,
                   label,
                   elapsed_ms,
                   sw,
                   static_cast<int>(axis_mode_display(ctx, slave)),
                   axis_error_code(ctx, slave));
        }

        if (elapsed_ms >= timeout_ms) {
            log_ts("Axis %d timeout waiting %s: sw=0x%04X mode_display=%d error=0x%04X\n",
                   slave,
                   label,
                   sw,
                   static_cast<int>(axis_mode_display(ctx, slave)),
                   axis_error_code(ctx, slave));
            return false;
        }
    }
}

// Decode CiA402 statusword bits for diagnostics
const char* decode_sw_brief(uint16_t sw)
{
    if ((sw & 0x0008))  return "Fault";
    if ((sw & 0x006F) == 0x0027) return "OpEnabled";
    if ((sw & 0x006F) == 0x0023) return "SwitchedOn";
    if ((sw & 0x006F) == 0x0021) return "ReadyToSwOn";
    if ((sw & 0x0040))  return "SwOnDisabled";
    if ((sw & 0x006F) == 0x0000 && (sw & 0x0200)) return "NotReady(Remote)";
    if ((sw & 0x006F) == 0x0000) return "NotReady";
    return "?";
}

bool enable_axis_csp(ecx_contextt& ctx, int axis)
{
    if (!has_axis_pdo(ctx, axis)) {
        log_ts("Axis %d PDO is not mapped as expected (need O>=%d I>=%d)\n",
               axis, kRxBytes, kTxBytes);
        return false;
    }

    pump_processdata(ctx, 10, g_cycle_us);

 // Pre-requisites: 0x607F defaults to 0 on DVS/X1S drives
    log_ts("Axis %d CSP setup: 0x607F/6083/6084/60E0/60E1\n", axis);
    write_sdo_u32(ctx, axis, 0x607F, 0, 1000000000u);
    write_sdo_u32(ctx, axis, 0x6083, 0, 10000000u);
    write_sdo_u32(ctx, axis, 0x6084, 0, 10000000u);
    write_sdo_u16(ctx, axis, 0x60E0, 0, 3000);  // positive torque limit
    write_sdo_u16(ctx, axis, 0x60E1, 0, 3000);  // negative torque limit

    int32_t actual_position = axis_actual_position(ctx, axis);
    log_ts("Axis %d actual position=%d; CSP enable mode=8\n", axis, actual_position);

    stage_axis_command_csp(ctx, axis, actual_position, 0x0006);
    pump_processdata(ctx, 5, g_cycle_us);

    uint16_t sw = axis_statusword(ctx, axis);
    if (is_fault(sw)) {
        log_ts("Axis %d fault detected sw=0x%04X error=0x%04X; sending fault reset\n",
               axis, sw, axis_error_code(ctx, axis));
        stage_axis_command_csp(ctx, axis, actual_position, 0x0080);
        pump_processdata(ctx, 10, g_cycle_us);
        stage_axis_command_csp(ctx, axis, actual_position, 0x0006);
        pump_processdata(ctx, 10, g_cycle_us);
    }

    log_ts("Axis %d CiA402: 0x0006 Shutdown\n", axis);
    stage_axis_command_csp(ctx, axis, actual_position, 0x0006);
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_ready_to_switch_on, "Ready to Switch On")) {
        uint16_t finalSw = axis_statusword(ctx, axis);
        const char* state = decode_sw_brief(finalSw);
        log_ts("Axis %d stuck in state %s (sw=0x%04X). Check:\n", axis, state, finalSw);
        log_ts("  - Servo main power (3-phase / DC bus) is ON?\n");
        log_ts("  - STO inputs are active (both channels HIGH)?\n");
        log_ts("  - Servo drive display shows 'rdy' or similar?\n");
        return false;
    }

    log_ts("Axis %d CiA402: 0x0007 Switch On\n", axis);
    stage_axis_command_csp(ctx, axis, actual_position, 0x0007);
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_switched_on, "Switched On")) {
        return false;
    }

    pump_processdata(ctx, 15, g_cycle_us);

    log_ts("Axis %d CiA402: 0x000F Enable Operation\n", axis);
    {
        int32_t latest = axis_actual_position(ctx, axis);
        if (latest != actual_position) {
            log_ts("Axis %d actual drifted from %d to %d, updating target\n",
                   axis, actual_position, latest);
            actual_position = latest;
        }
        stage_axis_command_csp(ctx, axis, actual_position, 0x000F);
    }
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_operation_enabled, "Operation Enabled")) {
        return false;
    }

    pump_processdata(ctx, 20, g_cycle_us);
    log_ts("*** AXIS %d CSP OPERATION ENABLED holding position ***\n", axis);
    return true;
}

bool enable_axis_csv(ecx_contextt& ctx, int axis)
{
    if (!has_axis_pdo(ctx, axis)) {
        log_ts("Axis %d PDO is not mapped as expected (need O>=%d I>=%d)\n",
               axis, kRxBytes, kTxBytes);
        return false;
    }

    pump_processdata(ctx, 10, g_cycle_us);

 // CSV pre-requisites: 0x607F Max Profile Velocity defaults to 0
    // Without setting this, the drive clamps all velocity commands to zero.
    log_ts("Axis %d CSV setup: 0x607F/6083/6084/60E0/60E1\n", axis);
    write_sdo_u32(ctx, axis, 0x607F, 0, 1000000000u);
    write_sdo_u32(ctx, axis, 0x6083, 0, 10000000u);
    write_sdo_u32(ctx, axis, 0x6084, 0, 10000000u);
    write_sdo_u16(ctx, axis, 0x60E0, 0, 3000);  // positive torque limit
    write_sdo_u16(ctx, axis, 0x60E1, 0, 3000);  // negative torque limit

    log_ts("Axis %d CiA402 CSV enable: mode=9 velocity=0\n", axis);

    stage_axis_command_csv(ctx, axis, 0, 0x0006);
    pump_processdata(ctx, 5, g_cycle_us);

    uint16_t sw = axis_statusword(ctx, axis);
    if (is_fault(sw)) {
        log_ts("Axis %d fault detected sw=0x%04X error=0x%04X; sending fault reset\n",
               axis, sw, axis_error_code(ctx, axis));
        stage_axis_command_csv(ctx, axis, 0, 0x0080);
        pump_processdata(ctx, 10, g_cycle_us);
        stage_axis_command_csv(ctx, axis, 0, 0x0006);
        pump_processdata(ctx, 10, g_cycle_us);
    }

    log_ts("Axis %d CiA402: 0x0006 Shutdown\n", axis);
    stage_axis_command_csv(ctx, axis, 0, 0x0006);
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_ready_to_switch_on, "Ready to Switch On")) {
        uint16_t finalSw = axis_statusword(ctx, axis);
        const char* state = decode_sw_brief(finalSw);
        log_ts("Axis %d stuck in state %s (sw=0x%04X). Check:\n", axis, state, finalSw);
        log_ts("  - Servo main power (3-phase / DC bus) is ON?\n");
        log_ts("  - STO inputs are active (both channels HIGH)?\n");
        return false;
    }

    log_ts("Axis %d CiA402: 0x0007 Switch On\n", axis);
    stage_axis_command_csv(ctx, axis, 0, 0x0007);
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_switched_on, "Switched On")) {
        return false;
    }

    pump_processdata(ctx, 15, g_cycle_us);

    log_ts("Axis %d CiA402: 0x000F Enable Operation\n", axis);
    stage_axis_command_csv(ctx, axis, 0, 0x000F);
    if (!wait_axis_status(ctx, axis, kDriveStateTimeoutMs, is_operation_enabled, "Operation Enabled")) {
        return false;
    }

    pump_processdata(ctx, 20, g_cycle_us);
    log_ts("*** AXIS %d CSV OPERATION ENABLED velocity=0 ***\n", axis);
    return true;
}

// Non-blocking numeric input: returns -1 if cancelled, else 0..3000
int read_torque_input(ecx_contextt& ctx, int axis, const char* label)
{
    char buf[8] = {};
    int  pos = 0;
    printf("\n%s (0-%d, Enter=ok Esc=cancel): ", label, kTorqueLimitMax);
    while (true) {
        pump_processdata(ctx, 1, g_cycle_us);
        if (_kbhit()) {
            int ch = _getch();
            if (ch == '\r' || ch == '\n') {
                if (pos == 0) { printf("\n"); return -1; }
                int val = std::atoi(buf);
                if (val < 0 || val > kTorqueLimitMax) {
                    printf(" out of range - ignored\n");
                    return -1;
                }
                printf(" %d\n", val);
                return val;
            }
            if (ch == 27) { printf(" cancelled\n"); return -1; }
            if (ch == '\b' || ch == 127) {
                if (pos > 0) { --pos; buf[pos] = 0; printf("\b \b"); }
                continue;
            }
            if (ch >= '0' && ch <= '9' && pos < 6) {
                buf[pos++] = (char)ch; buf[pos] = 0;
                printf("%c", ch);
            }
        }
    }
}
// Cycle time input (non-blocking, PDO keeps pumping)
// Returns -1 if cancelled, else value clamped to [kCycleUsMin, kCycleUsMax].
int read_cycle_input(ecx_contextt& ctx)
{
    char buf[8] = {};
    int  pos = 0;
    printf("\nCycle time (%d..%d us, Enter=ok Esc=cancel): ", kCycleUsMin, kCycleUsMax);
    while (true) {
        pump_processdata(ctx, 1, g_cycle_us);
        if (_kbhit()) {
            int ch = _getch();
            if (ch == '\r' || ch == '\n') {
                if (pos == 0) { printf("\n"); return -1; }
                int val = std::atoi(buf);
                if (val < kCycleUsMin) val = kCycleUsMin;
                if (val > kCycleUsMax) val = kCycleUsMax;
                printf(" %d\n", val);
                return val;
            }
            if (ch == 27) { printf(" cancelled\n"); return -1; }
            if (ch == '\b' || ch == 127) {
                if (pos > 0) { --pos; buf[pos] = 0; printf("\b \b"); }
                continue;
            }
            if (ch >= '0' && ch <= '9' && pos < 6) {
                buf[pos++] = (char)ch; buf[pos] = 0;
                printf("%c", ch);
            }
        }
    }
}
void jog_loop_csp(ecx_contextt& ctx, int axis)
{

    printf("\n");
    printf("=== Axis %d Jog Control (CSP mode) ===\n", axis);
    printf("  Arrow UP/DOWN = jog +/- position step\n");
    printf("  E = set 0x60E0  R = set 0x60E1\n");
    printf("  0x60E0=%d  0x60E1=%d\n", g_torque_pos, g_torque_neg);
    printf("  +/- = step (%d)   S = status snapshot   Q / Esc = quit\n", g_jog_step);
    printf("  C = set cycle time   now: %d us (%.1f ms)\n",
           g_cycle_us, g_cycle_us / 1000.0);
    printf("==========================================\n\n");

    pump_processdata(ctx, 5, g_cycle_us);
    int32_t target_pos = axis_actual_position(ctx, axis);

    bool running = true;
    int round = 0;

    LARGE_INTEGER phaseRef = cycle_timer::now();  // closed-loop timing

    while (running) {
        while (_kbhit()) {
            int ch = _getch();
            switch (ch) {
            case 'q': case 'Q': running = false; break;
            case 27:            running = false; break;
            case '+': case '=':
                g_jog_step += kJogStepSize;
                printf("Jog step: %d\n", g_jog_step);
                break;
            case '-': case '_':
                if (g_jog_step > kJogStepSize) g_jog_step -= kJogStepSize;
                printf("Jog step: %d\n", g_jog_step);
                break;
            case 'e': case 'E': {
                int v = read_torque_input(ctx, axis, "0x60E0 positive torque");
                if (v >= 0) { g_torque_pos = v; printf("0x60E0 = %d\n", g_torque_pos); }
                break;
            }
            case 'r': case 'R': {
                int v = read_torque_input(ctx, axis, "0x60E1 negative torque");
                if (v >= 0) { g_torque_neg = v; printf("0x60E1 = %d\n", g_torque_neg); }
                break;
            }
            case 'c': case 'C': {
                int v = read_cycle_input(ctx);
                if (v >= kCycleUsMin) {
                    g_cycle_us = v;
                    phaseRef = cycle_timer::now();
                    printf("Cycle set: %d us (%.1f ms)\n", g_cycle_us, g_cycle_us / 1000.0);
                }
                break;
            }
            case 's': case 'S': {
                // Full PDO snapshot (Rx + Tx, hex dump + decoded fields)
                const uint8_t* rx = slave_outputs(ctx, axis);
                const uint8_t* tx = slave_inputs(ctx, axis);
 printf("\n??? PDO Snapshot Axis %d ---\n", axis);
                printf("RxPDO[%d]:", kRxBytes);
                for (int i = 0; i < kRxBytes; ++i) printf(" %02X", rx[i]);
                printf("\n  TgtPos=%-8d TgtVel=%-8d TgtTrq=%-6d PTrq=%-5d NTrq=%-5d Ctrl=0x%04X Mode=%d\n",
                       read_i32(rx, kOutTargetPosition), read_i32(rx, kOutTargetVelocity),
                       (int)read_u16(rx, kOutTargetTorque),
                       (int)read_u16(rx, kOutPositiveTorque), (int)read_u16(rx, kOutNegativeTorque),
                       read_u16(rx, kOutControlword), (int)rx[kOutModeOfOperation]);
                printf("TxPDO[%d]:", kTxBytes);
                for (int i = 0; i < kTxBytes; ++i) printf(" %02X", tx[i]);
                uint16_t sw = read_u16(tx, kInStatusword);
                printf("\n  ActPos=%-8d ActVel=%-8d ActTrq=%-6d Stat=0x%04X(%-12s) Err=0x%04X ModeDisp=%-3d DigIn=0x%08X\n",
                       read_i32(tx, kInActualPosition), read_i32(tx, kInActualVelocity),
                       (int)read_u16(tx, kInActualTorque),
                       sw, decode_sw_brief(sw),
                       read_u16(tx, kInErrorCode), (int)tx[kInModeDisplay],
                       read_u32(tx, kInDigitalInputs));
                break;
            }
            }
        }

        if (GetAsyncKeyState(VK_UP)   & 0x8000) target_pos += g_jog_step;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) target_pos -= g_jog_step;

        uint8_t* out = slave_outputs(ctx, axis);
        write_i32(out, kOutTargetPosition, target_pos);
        write_i32(out, kOutTargetVelocity, 0);
        write_u16(out, kOutControlword, 0x000F);
        out[kOutModeOfOperation] = 8;

        // Torque limits via PDO (all axes share 7-entry layout)
        if (kOutPositiveTorque >= 0) {
            write_u16(out, kOutPositiveTorque, static_cast<uint16_t>(g_torque_pos));
            write_u16(out, kOutNegativeTorque, static_cast<uint16_t>(g_torque_neg));
        } else {
            static auto lastSdo = std::chrono::steady_clock::now();
            static int  lastPos = -1, lastNeg = -1;
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSdo).count();
            if ((g_torque_pos != lastPos || g_torque_neg != lastNeg) && ms >= 500) {
                write_sdo_u16(ctx, axis, 0x60E0, 0, (uint16_t)g_torque_pos);
                write_sdo_u16(ctx, axis, 0x60E1, 0, (uint16_t)g_torque_neg);
                lastPos = g_torque_pos; lastNeg = g_torque_neg; lastSdo = now;
            }
        }

        cycle_exchange(ctx, g_cycle_us, phaseRef);
        ++round;

        // Compact PDO status while arrow keys held (throttled ~50ms)
        bool moving = (GetAsyncKeyState(VK_UP) | GetAsyncKeyState(VK_DOWN)) & 0x8000;
        if (moving && round % 25 == 0) {
            const uint8_t* tx = slave_inputs(ctx, axis);
            uint16_t sw = read_u16(tx, kInStatusword);
            printf("\rpos=%d tgt=%d vel=%d trq=%d sw=0x%04X(%-10s) mode=%d   ",
                   read_i32(tx, kInActualPosition), target_pos,
                   read_i32(tx, kInActualVelocity), (int)read_u16(tx, kInActualTorque),
                   sw, decode_sw_brief(sw), (int)tx[kInModeDisplay]);
        }

        // Fault check every 10 cycles (no status spam)
        if (round % 10 == 0) {
            uint16_t sw = axis_statusword(ctx, axis);
            if (sw & 0x0008) {
                int32_t pos = axis_actual_position(ctx, axis);
                uint16_t err = axis_error_code(ctx, axis);
                const char* state = decode_sw_brief(sw);
                printf("\n*** FAULT on axis %d: sw=0x%04X(%s) err=0x%04X  pos=%d  tgt=%d ***\n",
                       axis, sw, state, err, pos, target_pos);
                log_ts("*** FAULT on axis %d: sw=0x%04X(%s) err=0x%04X  pos=%d  tgt=%d ***\n",
                       axis, sw, state, err, pos, target_pos);
                running = false;
            }
        }
    }

    printf("\nStopping axis %d...\n", axis);
    log_ts("Stopping axis %d jog...\n", axis);
    pump_processdata(ctx, 30, g_cycle_us);
    log_ts("Axis %d jog finished\n", axis);
}

void jog_loop_csv(ecx_contextt& ctx, int axis)
{

    printf("\n");
    printf("=== Axis %d Jog Control (CSV mode) ===\n", axis);
    printf("  Arrow UP/DOWN = jog +/- velocity\n");
    printf("  E = set 0x60E0  R = set 0x60E1\n");
    printf("  0x60E0=%d  0x60E1=%d\n", g_torque_pos, g_torque_neg);
    printf("  +/- = speed (%d)   S = status snapshot   Q / Esc = quit\n", g_jog_speed);
    printf("  C = set cycle time   now: %d us (%.1f ms)\n",
           g_cycle_us, g_cycle_us / 1000.0);
    printf("==========================================\n\n");

    bool running = true;
    int32_t target_vel = 0;
    int round = 0;

    LARGE_INTEGER phaseRef = cycle_timer::now();  // closed-loop timing

    while (running) {
        while (_kbhit()) {
            int ch = _getch();
            switch (ch) {
            case 'q': case 'Q': running = false; break;
            case 27:            running = false; break;
            case '+': case '=':
                g_jog_speed += kJogSpeedStep;
                printf("Jog speed: %d\n", g_jog_speed);
                break;
            case '-': case '_':
                if (g_jog_speed > kJogSpeedStep) g_jog_speed -= kJogSpeedStep;
                printf("Jog speed: %d\n", g_jog_speed);
                break;
            case 'e': case 'E': {
                int v = read_torque_input(ctx, axis, "0x60E0 positive torque");
                if (v >= 0) { g_torque_pos = v; printf("0x60E0 = %d\n", g_torque_pos); }
                break;
            }
            case 'r': case 'R': {
                int v = read_torque_input(ctx, axis, "0x60E1 negative torque");
                if (v >= 0) { g_torque_neg = v; printf("0x60E1 = %d\n", g_torque_neg); }
                break;
            }
            case 'c': case 'C': {
                int v = read_cycle_input(ctx);
                if (v >= kCycleUsMin) {
                    g_cycle_us = v;
                    phaseRef = cycle_timer::now();
                    printf("Cycle set: %d us (%.1f ms)\n", g_cycle_us, g_cycle_us / 1000.0);
                }
                break;
            }
            case 's': case 'S': {
                // Full PDO snapshot (Rx + Tx, hex dump + decoded fields)
                const uint8_t* rx = slave_outputs(ctx, axis);
                const uint8_t* tx = slave_inputs(ctx, axis);
 printf("\n??? PDO Snapshot Axis %d ---\n", axis);
                printf("RxPDO[%d]:", kRxBytes);
                for (int i = 0; i < kRxBytes; ++i) printf(" %02X", rx[i]);
                printf("\n  TgtPos=%-8d TgtVel=%-8d TgtTrq=%-6d PTrq=%-5d NTrq=%-5d Ctrl=0x%04X Mode=%d\n",
                       read_i32(rx, kOutTargetPosition), read_i32(rx, kOutTargetVelocity),
                       (int)read_u16(rx, kOutTargetTorque),
                       (int)read_u16(rx, kOutPositiveTorque), (int)read_u16(rx, kOutNegativeTorque),
                       read_u16(rx, kOutControlword), (int)rx[kOutModeOfOperation]);
                printf("TxPDO[%d]:", kTxBytes);
                for (int i = 0; i < kTxBytes; ++i) printf(" %02X", tx[i]);
                uint16_t sw = read_u16(tx, kInStatusword);
                printf("\n  ActPos=%-8d ActVel=%-8d ActTrq=%-6d Stat=0x%04X(%-12s) Err=0x%04X ModeDisp=%-3d DigIn=0x%08X\n",
                       read_i32(tx, kInActualPosition), read_i32(tx, kInActualVelocity),
                       (int)read_u16(tx, kInActualTorque),
                       sw, decode_sw_brief(sw),
                       read_u16(tx, kInErrorCode), (int)tx[kInModeDisplay],
                       read_u32(tx, kInDigitalInputs));
                break;
            }
            }
        }

        target_vel = 0;
        if (GetAsyncKeyState(VK_UP)   & 0x8000) target_vel =  g_jog_speed;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) target_vel = -g_jog_speed;

        uint8_t* out = slave_outputs(ctx, axis);
        write_i32(out, kOutTargetVelocity, target_vel);
        write_u16(out, kOutControlword, 0x000F);
        out[kOutModeOfOperation] = 9;

        // Torque limits via PDO (all axes share 7-entry layout)
        if (kOutPositiveTorque >= 0) {
            write_u16(out, kOutPositiveTorque, static_cast<uint16_t>(g_torque_pos));
            write_u16(out, kOutNegativeTorque, static_cast<uint16_t>(g_torque_neg));
        } else {
            static auto lastSdo = std::chrono::steady_clock::now();
            static int  lastPos = -1, lastNeg = -1;
            auto now = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSdo).count();
            if ((g_torque_pos != lastPos || g_torque_neg != lastNeg) && ms >= 500) {
                write_sdo_u16(ctx, axis, 0x60E0, 0, (uint16_t)g_torque_pos);
                write_sdo_u16(ctx, axis, 0x60E1, 0, (uint16_t)g_torque_neg);
                lastPos = g_torque_pos; lastNeg = g_torque_neg; lastSdo = now;
            }
        }

        cycle_exchange(ctx, g_cycle_us, phaseRef);
        ++round;

        // Compact PDO status while arrow keys held (throttled ~50ms)
        bool moving = (GetAsyncKeyState(VK_UP) | GetAsyncKeyState(VK_DOWN)) & 0x8000;
        if (moving && round % 25 == 0) {
            const uint8_t* tx = slave_inputs(ctx, axis);
            uint16_t sw = read_u16(tx, kInStatusword);
            printf("\rpos=%d vel=%d tgt=%d trq=%d sw=0x%04X(%-10s) mode=%d   ",
                   read_i32(tx, kInActualPosition), read_i32(tx, kInActualVelocity),
                   target_vel, (int)read_u16(tx, kInActualTorque),
                   sw, decode_sw_brief(sw), (int)tx[kInModeDisplay]);
        }

        // Fault check every 10 cycles (no status spam)
        if (round % 10 == 0) {
            uint16_t sw = axis_statusword(ctx, axis);
            if (sw & 0x0008) {
                int32_t pos = axis_actual_position(ctx, axis);
                int32_t vel = axis_actual_velocity(ctx, axis);
                uint16_t err = axis_error_code(ctx, axis);
                const char* state = decode_sw_brief(sw);
                printf("\n*** FAULT on axis %d: sw=0x%04X(%s) err=0x%04X  pos=%d  vel=%d ***\n",
                       axis, sw, state, err, pos, vel);
                log_ts("*** FAULT on axis %d: sw=0x%04X(%s) err=0x%04X  pos=%d  vel=%d ***\n",
                       axis, sw, state, err, pos, vel);
                running = false;
            }
        }
    }

    printf("\nStopping axis %d...\n", axis);
    log_ts("Stopping axis %d jog...\n", axis);
    {
        uint8_t* out = slave_outputs(ctx, axis);
        write_i32(out, kOutTargetVelocity, 0);
        write_u16(out, kOutControlword, 0x000F);
    }
    pump_processdata(ctx, 30, g_cycle_us);
    log_ts("Axis %d jog finished\n", axis);
}

void shutdown_to_safeop(ecx_contextt& ctx, int axis)
{
    if (ctx.slavecount <= 0) {
        return;
    }

    if (axis >= 1 && axis <= ctx.slavecount && has_axis_pdo(ctx, axis)) {
        int32_t pos = axis_actual_position(ctx, axis);
        stage_axis_command_csp(ctx, axis, pos, 0x0006);
        pump_processdata(ctx, 10, g_cycle_us);
    }

    ctx.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&ctx, 0);
    ecx_statecheck(&ctx, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    pump_processdata(ctx, 5, g_cycle_us);
}

} // namespace

int main(int argc, char** argv)
{
 // Disable stdout buffering so all output appears immediately
    setvbuf(stdout, NULL, _IONBF, 0);

 // Emergency crash log to file (survives console close)
    FILE* crashLog = fopen("test_Ethercat_jog_crash.log", "w");
    if (crashLog) {
        fprintf(crashLog, "main() entered, argc=%d\n", argc);
        // Log Npcap runtime version for Win11 diagnostics
        fprintf(crashLog, "Npcap version: %s\n", pcap_lib_version());
        fclose(crashLog);
    }

 // Install VEH for early crash capture (Win11 Npcap compat)
    PVOID vehHandle = AddVectoredExceptionHandler(1, veh_handler);
    g_vehCrashLog = fopen("test_Ethercat_jog_crash.log", "a");

 // Log Npcap version to console
    printf("[Npcap] runtime version: %s\n", pcap_lib_version());

    // Helper: pause before exit so user can read output
    auto pause_and_return = [](int code) -> int {
        printf("\n--- Press any key to exit (code=%d) ---\n", code);
        fflush(stdout);
        _getch();
        return code;
    };

    __try {

 // Handle -list-cores: print topology and exit
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-list-cores") == 0) {
            print_cpu_topology();
            return 0;
        }
    }

    // Parse CLI arguments: [ifname] [-core <N>] [-cycle <us>]
    char adapterName[128] = {0};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-core") == 0 && i + 1 < argc) {
            g_core_index = std::atoi(argv[++i]);
            continue;  // skip consuming the value as adapter name
        }
        if (std::strcmp(argv[i], "-cycle") == 0 && i + 1 < argc) {
            int val = std::atoi(argv[++i]);
            if (val >= kCycleUsMin && val <= kCycleUsMax) {
                g_cycle_us = val;
            } else {
                printf("WARNING: -cycle %d out of range (%d..%d us), using default %d us\n",
                       val, kCycleUsMin, kCycleUsMax, kCycleUsDefault);
            }
            continue;
        }
        if (adapterName[0] == '\0') {
            std::strncpy(adapterName, argv[i], sizeof(adapterName) - 1);
        }
    }

 // System timer resolution: 1ms (critical for 2ms PDO cycle)
    timeBeginPeriod(1);
    printf("[Timer] timeBeginPeriod(1) ??system timer resolution set to 1ms\n");

 // QPC + TSC calibration for closed-loop cycle timing
    cycle_timer::init();
    if (cycle_timer::g_qpcValid) {
        printf("[QPC] frequency = %lld Hz\n", cycle_timer::g_qpcFreq.QuadPart);
    } else {
        printf("[QPC] WARNING: QueryPerformanceFrequency failed -- timing will drift\n");
    }
    printf("[OS] detected %s -> %s\n",
           cycle_timer::g_win10Forced ? "Win11 (forced to Win10 mode by BigCat.ini [Compat] ForceWin10Mode=1)"
                                      : (cycle_timer::g_isWin11 ? "Win11" : "Win10"),
           "TSC spin + SwitchToThread (DPC drain)");
    cycle_timer::calibrate_tsc();
    if (cycle_timer::g_tscValid) {
        printf("[TSC] calibrated: %.3f ticks/us ??precision spin-wait enabled\n",
               cycle_timer::g_tscPerUs);
    } else {
        printf("[TSC] invariant TSC not available ??fallback to Sleep-only timing\n");
    }

 // Console setup: disable QuickEdit BEFORE any interactive prompts
    // QuickEdit freezes the process if the user clicks the console window,
    // which looks like a crash/flash-exit during adapter selection.
    {
        HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(hIn, &mode)) {
            mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_MOUSE_INPUT | ENABLE_WINDOW_INPUT);
            SetConsoleMode(hIn, mode);
        }
        if (GetConsoleMode(hOut, &mode)) {
            mode |= DISABLE_NEWLINE_AUTO_RETURN;
            SetConsoleMode(hOut, mode);
        }
    }

 // Interactive CPU core selection (if not specified via CLI)
    if (g_core_index < 0) {
        printf("\n=== SOEM CPU Core Binding (reduces PDO jitter) ===\n\n");
        printf("[DBG] calling print_cpu_topology...\n");
        print_cpu_topology();
        printf("[DBG] print_cpu_topology done\n");
        printf("\n");
        printf("Enter LP number to pin SOEM thread (Enter = skip, no binding): ");
        fflush(stdout);

        char buf[16] = {};
        int  pos = 0;
        // Non-blocking read until Enter or Esc
        while (true) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == '\r' || ch == '\n') {
                    printf("\n");
                    break;
                }
                if (ch == 27) {  // Esc
                    printf("skip\n");
                    pos = 0; buf[0] = 0;
                    break;
                }
                if (ch == '\b' || ch == 127) {
                    if (pos > 0) { --pos; buf[pos] = 0; printf("\b \b"); }
                    continue;
                }
                if (ch >= '0' && ch <= '9' && pos < 6) {
                    buf[pos++] = (char)ch; buf[pos] = 0;
                    printf("%c", ch);
                }
            }
            Sleep(10);
        }
        if (pos > 0 && buf[0] != '\0') {
            g_core_index = std::atoi(buf);
            printf("Pinning to LP %d\n\n", g_core_index);
        } else {
            printf("Skipping core binding\n\n");
        }
    }

    if (adapterName[0] == '\0') {
        printf("[DBG] calling ec_find_adapters...\n");
        fflush(stdout);

 // Fine-grained SEH: ec_find_adapters calls pcap_findalldevs which
 // can crash on Win11 with incompatible Npcap runtime.
        ec_adaptert* list = nullptr;
        g_vehPhase = "ec_find_adapters";
        __try {
            list = ec_find_adapters();
        } __except (GetExceptionCode()) {
            DWORD code = GetExceptionCode();
            printf("FATAL: ec_find_adapters crashed with SEH 0x%08lX\n", code);
            if (g_vehCrashLog) {
                fprintf(g_vehCrashLog, "SEH: ec_find_adapters crashed 0x%08lX\n", code);
                fflush(g_vehCrashLog);
            }
            list = nullptr;
        }
        g_vehPhase = "none";

        printf("[DBG] ec_find_adapters returned %p\n", (void*)list);
        if (!list) {
            printf("FATAL: no EtherCAT-compatible adapters found\n");
            printf("  Check: Npcap installed? NIC driver loaded?\n");
            fflush(stdout);
            return pause_and_return(1);
        }
        printf("Available adapters:\n");
        int count = 0;
        ec_adaptert* saved[16] = {};
        for (ec_adaptert* a = list; a && count < 16; a = a->next) {
            saved[count] = a;
            printf("  %d: %s\n", count + 1, a->desc);
            printf("     %s\n", a->name);
            ++count;
        }
        printf("[DBG] %d adapters listed, entering selection prompt\n", count);

        if (count == 0) {
            printf("FATAL: no adapters enumerated (Npcap may need reinstall)\n");
            fflush(stdout);
            ec_free_adapters(list);
            return pause_and_return(1);
        }

        // Flush adapter list output and any stale keystrokes before prompting
        fflush(stdout);
        while (_kbhit()) (void)_getch();

        printf("Select adapter (1-%d): ", count);
        fflush(stdout);
        int sel = 0;
        while (sel < 1 || sel > count) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch >= '1' && ch <= '0' + count) { sel = ch - '0'; printf("%d\n", sel); }
                else if (ch == 27) { printf("quit\n"); break; }
            }
            Sleep(10);
        }
        if (sel == 0) { ec_free_adapters(list); return pause_and_return(1); }
        std::strncpy(adapterName, saved[sel - 1]->name, sizeof(adapterName) - 1);
        ec_free_adapters(list);
    }
    printf("Using adapter: %s\n\n", adapterName);
    fflush(stdout);

    printf("=== EtherCAT Jog Test ===\n");
    printf("Cycle time: %d us (%.1f ms)\n", g_cycle_us, g_cycle_us / 1000.0);
    printf("DO NOT resize or click this window -- PDO will drop.\n");
    // Interactive cycle time prompt (tests full initOP flow at target cycle)
    {
        printf("Enter cycle time (%d..%d us, Enter=keep %d): ",
               kCycleUsMin, kCycleUsMax, g_cycle_us);
        fflush(stdout);
        char cycBuf[8] = {};
        int  cycPos = 0;
        while (true) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == '\r' || ch == '\n') {
                    printf("\n");
                    if (cycPos > 0) {
                        int v = std::atoi(cycBuf);
                        if (v >= kCycleUsMin && v <= kCycleUsMax) g_cycle_us = v;
                        else printf("  out of range, keeping %d us\n", g_cycle_us);
                    }
                    break;
                }
                if (ch == 27) { printf("keep\n"); break; }
                if (ch == '\b' || ch == 127) {
                    if (cycPos > 0) { --cycPos; cycBuf[cycPos] = 0; printf("\b \b"); }
                    continue;
                }
                if (ch >= '0' && ch <= '9' && cycPos < 6) {
                    cycBuf[cycPos++] = (char)ch; cycBuf[cycPos] = 0;
                    printf("%c", ch);
                }
            }
            Sleep(10);
        }
        printf("PDO cycle: %d us (%.1f ms)\n\n", g_cycle_us, g_cycle_us / 1000.0);
    }
    fflush(stdout);

    // Pin SOEM thread to specified physical core (if -core <N> given)
    if (g_core_index >= 0) {
        if (!pin_thread_to_physical_core(g_core_index)) {
            printf("FATAL: Failed to pin thread to LP %d. Aborting.\n", g_core_index);
            return pause_and_return(1);
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    g_log = fopen("test_Ethercat_jog.log", "w");
    log_ts("=== EtherCAT Jog Test ===\n");
    log_ts("Cycle time: %d us (%.1f ms)\n", g_cycle_us, g_cycle_us / 1000.0);
    if (g_core_index >= 0) {
        log_ts("SOEM thread pinned to logical CPU %d (TIME_CRITICAL)\n", g_core_index);
    }

    ecx_contextt ctx;
    std::memset(&ctx, 0, sizeof(ctx));

    bool ok = false;
    bool port_open = false;
    do {
 // Fine-grained SEH: ecx_init calls pcap_open which can crash
 // on Win11 with incompatible Npcap runtime/driver.
        {
            int initRc = 0;
            g_vehPhase = "ecx_init";
            __try {
                initRc = ecx_init(&ctx, adapterName);
            } __except (GetExceptionCode()) {
                DWORD code = GetExceptionCode();
                printf("FATAL: ecx_init crashed with SEH 0x%08lX for adapter '%s'\n",
                       code, adapterName);
                log_ts("FATAL: ecx_init crashed with SEH 0x%08lX\n", code);
                if (g_vehCrashLog) {
                    fprintf(g_vehCrashLog, "SEH: ecx_init('%s') crashed 0x%08lX\n",
                            adapterName, code);
                    fflush(g_vehCrashLog);
                }
                initRc = 0;
            }
            g_vehPhase = "none";

            if (initRc <= 0) {
                log_ts("FATAL: ecx_init failed\n");
                printf("FATAL: ecx_init failed for adapter '%s'\n", adapterName);
                fflush(stdout);
                break;
            }
        }
        port_open = true;

 // Apply Npcap immediate-mode optimizations (Win11 DC SYNC0 fix)
        // On Win11 the default Npcap buffering introduces ~1ms+ extra latency
        // that breaks DC SYNC0 timing slaves fail with AL=0x001A sync error.
        // pcap_set_immediate_mode(1) + pcap_setmintocopy(1) eliminate this.
        {
            pcap_t* ph = ctx.port.sockhandle;
            if (ph) {
                typedef int (*pcap_set_immediate_mode_t)(pcap_t*, int);
                typedef int (*pcap_setmintocopy_t)(pcap_t*, int);
                HMODULE hWpcap = GetModuleHandleA("wpcap.dll");
                if (!hWpcap) hWpcap = LoadLibraryA("wpcap.dll");
                if (hWpcap) {
                    auto fnImm = (pcap_set_immediate_mode_t)
                        GetProcAddress(hWpcap, "pcap_set_immediate_mode");
                    auto fnMin = (pcap_setmintocopy_t)
                        GetProcAddress(hWpcap, "pcap_setmintocopy");
                    if (fnImm) {
                        int rc = fnImm(ph, 1);
                        printf("[Npcap] pcap_set_immediate_mode(1) ??%d\n", rc);
                        log_ts("[Npcap] pcap_set_immediate_mode(1) ??%d\n", rc);
                    }
                    if (fnMin) {
                        int rc = fnMin(ph, 1);
                        printf("[Npcap] pcap_setmintocopy(1) ??%d\n", rc);
                        log_ts("[Npcap] pcap_setmintocopy(1) ??%d\n", rc);
                    }
                    if (!fnImm && !fnMin) {
                        printf("[Npcap] WARNING: no immediate-mode APIs ??upgrade Npcap\n");
                        log_ts("[Npcap] WARNING: no immediate-mode APIs ??upgrade Npcap\n");
                    }
                }
            }
        }

        if (ecx_config_init(&ctx) <= 0) {
            log_ts("FATAL: no slaves found\n");
            printf("FATAL: no slaves found on adapter '%s'\n", adapterName);
            fflush(stdout);
            break;
        }

        log_ts("%d slaves found\n", ctx.slavecount);

        for (int si = 1; si <= ctx.slavecount; ++si) {
            log_ts("  S%d: %s vid=0x%X pid=0x%X SM2=%u SM3=%u hasdc=%d\n",
                   si,
                   ctx.slavelist[si].name,
                   ctx.slavelist[si].eep_man,
                   ctx.slavelist[si].eep_id,
                   etohs(ctx.slavelist[si].SM[2].SMlength),
                   etohs(ctx.slavelist[si].SM[3].SMlength),
                   static_cast<int>(ctx.slavelist[si].hasdc));
        }

        log_ts("\n=== Configure PDO per slave type ===\n");
        bool pdo_ok = true;
        for (int si = 1; si <= ctx.slavecount; ++si) {
            pdo_ok &= configure_slave_pdo(ctx, si);
        }
        if (!pdo_ok) {
            log_ts("FATAL: PDO configuration had SDO failures\n");
            break;
        }

        char iomap[4096];
        std::memset(iomap, 0, sizeof(iomap));
        const int map_bytes = ecx_config_map_group(&ctx, iomap, 0);
        log_ts("IOmap mapped: %d bytes\n", map_bytes);
        for (int si = 1; si <= ctx.slavecount; ++si) {
            log_ts("  S%d: O=%u I=%u out=%p in=%p\n",
                   si,
                   ctx.slavelist[si].Obytes,
                   ctx.slavelist[si].Ibytes,
                   ctx.slavelist[si].outputs,
                   ctx.slavelist[si].inputs);
        }

        for (int si = 1; si <= ctx.slavecount; ++si) {
            if (ctx.slavelist[si].outputs && ctx.slavelist[si].Obytes >= kRxBytes) {
                write_u16(slave_outputs(ctx, si), kOutControlword, 0x0006);
                slave_outputs(ctx, si)[kOutModeOfOperation] = 8;
                write_u16(slave_outputs(ctx, si), kOutPositiveTorque, static_cast<uint16_t>(g_torque_pos));
                write_u16(slave_outputs(ctx, si), kOutNegativeTorque, static_cast<uint16_t>(g_torque_neg));
            }
        }
        pump_processdata(ctx, 20, g_cycle_us);

        ecx_configdc(&ctx);
        for (int si = 1; si <= ctx.slavecount; ++si) {
            if (ctx.slavelist[si].hasdc) {
                ecx_dcsync0(&ctx, si, TRUE, g_cycle_us * 1000, (g_cycle_us * 1000) * 15 / 100);
            }
        }
        log_ts("DC + SYNC0 configured for DC-capable slaves\n");

 // Per-slave reconfig for ALL slaves (INIT?SM?PREOP?SAFE_OP)
        // matching the X1S/TwinCAT per-slave startup sequence.
 log_ts("Reconfiguring all slaves via per-slave INIT?SAFE_OP path...\n");
        bool allReconfigOk = true;
        for (int si = 1; si <= ctx.slavecount; ++si) {
            int state = ecx_reconfig_slave(&ctx, si, EC_TIMEOUTSTATE * 3);
            log_ts("  S%d reconfig: state=0x%02X AL=0x%04X %s\n",
                   si, state,
                   ctx.slavelist[si].ALstatuscode,
                   ec_ALstatuscode2string(ctx.slavelist[si].ALstatuscode));
            if (!is_state(state, EC_STATE_SAFE_OP)) {
                log_ts("FATAL: S%d did not reach SAFE_OP after reconfig\n", si);
                allReconfigOk = false;
                break;
            }
        }
        if (!allReconfigOk) break;

        // All slaves already in SAFE_OP no group transition needed.
        pump_processdata(ctx, 30, 5000);
        log_slave_states(ctx);

        for (int si = 1; si <= ctx.slavecount; ++si) {
            write_sdo_u8(ctx, si, 0x6060, 0, 8);
        }
        log_ts("0x6060=8 (CSP) written in SAFE_OP for all slaves\n");
        pump_processdata(ctx, 20, g_cycle_us);

 // DC SYNC0 pre-stabilization (Win11 fix)
        // On Win11 the NIC may have higher inherent latency/jitter than
        // Win10 with the same hardware.  Pump PDO frames in SAFE_OP for
        // several seconds so the slaves' distributed clocks can converge
        // before we request OP.  Without this, slaves report AL=0x001A
        // "Synchronization error" because SYNC0 timing is still drifting.
        {
            const int kDcStableSec = 4;  // seconds to let DC settle
            log_ts("DC SYNC0 pre-stabilization (%d s PDO pump in SAFE_OP)...\n",
                   kDcStableSec);
            pump_stabilize(ctx, kDcStableSec * 1000, g_cycle_us,
                           "DC-pre-stab");
            log_slave_states(ctx);
        }

        log_ts("Requesting EtherCAT OP for all slaves...\n");
        ctx.slavelist[0].state = EC_STATE_OPERATIONAL;
        ecx_writestate(&ctx, 0);

        if (!wait_all_slaves_op(ctx, kEthercatOpTimeoutMs)) {
            log_ts("FATAL: EtherCAT OP timeout\n");
            log_slave_states(ctx);
            break;
        }
        log_ts("*** ALL SLAVES ETHERCAT OP ***\n");

        // Post-OP stabilization: pump PDO frames for 1 s so DC SYNC locks
        // and drive firmware finishes internal boot.
        log_ts("Post-OP stabilization (1 s PDO pump)...\n");
        {
            const int kPostOpStableRounds = 1000000 / g_cycle_us;
            int lastLogRound = -1;
            LARGE_INTEGER postOpPhaseRef = cycle_timer::now();
            for (int r = 0; r < kPostOpStableRounds; ++r) {
                cycle_exchange(ctx, g_cycle_us, postOpPhaseRef);
                if (r / 100 != lastLogRound) {
                    lastLogRound = r / 100;
                    char buf[512];
                    int used = std::snprintf(buf, sizeof(buf), "  [%d/%d]", r + 1, kPostOpStableRounds);
                    for (int si = 1; si <= ctx.slavecount && used < (int)sizeof(buf) - 20; ++si) {
                        uint16_t sw = axis_statusword(ctx, si);
                        used += std::snprintf(buf + used, sizeof(buf) - used,
                                              " S%d=0x%04X(%s)", si, sw, decode_sw_brief(sw));
                    }
                    log_ts("%s\n", buf);
                }
            }
        }

 // ???Interactive axis/mode selection loop ???
        ok = false;
        while (true) {
            printf("\nSelect axis (1-%d) [Q=quit]: ", ctx.slavecount);
            int axis = 0;
            while (axis < 1 || axis > ctx.slavecount) {
                if (_kbhit()) {
                    int ch = _getch();
                    if (ch >= '1' && ch <= '0' + ctx.slavecount) {
                        axis = ch - '0';
                        printf("%d\n", axis);
                    } else if (ch == 27 || ch == 'q' || ch == 'Q') {
                        printf("quit\n");
                        break;
                    }
                }
                pump_processdata(ctx, 1, g_cycle_us);
            }
            if (axis == 0) break;  // quit program

            log_ts("Axis %d selected (7-entry PDO 0x1600/0x1A00)\n", axis);

            if (!has_axis_pdo(ctx, axis)) {
                log_ts("Axis %d has no usable PDO ??skip\n", axis);
                continue;
            }

 // ???Mode selection ???
            printf("Select mode: [C]SP  [V]CSV  [B]ack: ");
            char mode = 0;
            while (!mode) {
                if (_kbhit()) {
                    int ch = _getch();
                    switch (ch) {
                    case 'c': case 'C': mode = 'C'; printf("CSP\n"); break;
                    case 'v': case 'V': mode = 'V'; printf("CSV\n"); break;
                    case 'b': case 'B': case 27: printf("back\n"); break;
                    }
                    if (ch == 'b' || ch == 'B' || ch == 27) break;
                }
                pump_processdata(ctx, 1, g_cycle_us);
            }
            if (!mode) continue;  // back to axis selection

            if (mode == 'C') {
                ok = enable_axis_csp(ctx, axis);
                if (ok) {
                    log_ts("\n=== Axis %d CSP Operation Enabled ===\n", axis);
                    jog_loop_csp(ctx, axis);
                }
            } else {
                ok = enable_axis_csv(ctx, axis);
                if (ok) {
                    log_ts("\n=== Axis %d CSV Operation Enabled ===\n", axis);
                    jog_loop_csv(ctx, axis);
                }
            }
            // Axis stays enabled after jog ??keep holding position/zero-velocity.
        }
    } while (false);

    if (port_open) {
        shutdown_to_safeop(ctx, ok ? 1 : 0);
        ecx_close(&ctx);
    }

    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }

 // Cleanup VEH and crash log
    if (vehHandle) RemoveVectoredExceptionHandler(vehHandle);
    if (g_vehCrashLog) { fclose(g_vehCrashLog); g_vehCrashLog = nullptr; }

    timeEndPeriod(1);
    return ok ? 0 : 1;

    } __except (GetExceptionCode()) {
        DWORD code = GetExceptionCode();
        // Try to log the crash before the process dies
        FILE* f = fopen("test_Ethercat_jog_crash.log", "a");
        if (f) {
            fprintf(f, "CRASH: SEH exception 0x%08lX at runtime phase=%s\n", code, g_vehPhase);
            fclose(f);
        }
        printf("\n*** FATAL CRASH: exception 0x%08lX ***\n", code);
        printf("Details saved to test_Ethercat_jog_crash.log\n");

     // Cleanup VEH and crash log
        if (vehHandle) RemoveVectoredExceptionHandler(vehHandle);
        if (g_vehCrashLog) { fclose(g_vehCrashLog); g_vehCrashLog = nullptr; }

        timeEndPeriod(1);
        return pause_and_return(2);
    }
}
