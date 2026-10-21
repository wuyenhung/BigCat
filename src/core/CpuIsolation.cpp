#include "../../include/core/CpuIsolation.h"
#include "../../include/core/CpuTopology.h"
#include <windows.h>
#include <tlhelp32.h>
#include <intrin.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>
#include <string>
#include <set>
#include <winternl.h>   // for NTSTATUS, SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION

// ── Win11 real-time boost: dynamically-loaded API types ──
// All loaded at runtime via GetProcAddress; zero impact on Win10 where these
// pointers remain nullptr and the boost calls become quiet no-ops.

// PROCESS_POWER_THROTTLING_STATE (undocumented layout, stable since Win10 1709)
// SetProcessInformation(ProcessPowerThrottling=4)
// Named with CpuIso_ prefix to avoid collision with SDK definition in
// processthreadsapi.h when _WIN32_WINNT >= 0x0A00.
typedef struct {
    ULONG Version;      // PROCESS_POWER_THROTTLING_CURRENT_VERSION = 1
    ULONG ControlMask;  // 1 = PROCESS_POWER_THROTTLING_EXECUTION_SPEED
    ULONG StateMask;    // 0 = disable throttling
} CpuIso_PROCESS_POWER_THROTTLING_STATE;

typedef BOOL (WINAPI *PFN_SetProcessInformation)(
    HANDLE hProcess,
    int    ProcessInformationClass,   // ProcessPowerThrottling = 4
    LPVOID ProcessInformation,
    DWORD  ProcessInformationSize
);

typedef BOOL (WINAPI *PFN_SetThreadInformation)(
    HANDLE hThread,
    int    ThreadInformationClass,    // ThreadIoPriority = 16
    LPVOID ThreadInformation,
    DWORD  ThreadInformationSize
);

// avrt.dll
typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsW)(
    LPCWSTR TaskName,
    LPDWORD TaskIndex
);

typedef BOOL (WINAPI *PFN_AvRevertMmThreadCharacteristics)(
    HANDLE AvrtHandle
);

namespace CoreHelpers {

    // ── Static member definitions ──

    std::atomic<bool> CpuIsolation::monitorRunning(false);
    std::thread CpuIsolation::monitorThread;
    HANDLE CpuIsolation::monitorStopEvent = NULL;
    HANDLE CpuIsolation::s_repelTriggerEvent = NULL;
    std::atomic<int> CpuIsolation::s_repelIntervalMs{10};  // 預設 10ms（≈ 2ms PDO × 5；縮短窗口期減少 browser process 干擾）

    bool   CpuIsolation::hardIsolationActive_ = false;
    int    CpuIsolation::rtPhysicalCore_ = -1;  // -1 = unset; resolved dynamically at init
    DWORD_PTR CpuIsolation::rtCoreMask_ = 0;
    DWORD_PTR CpuIsolation::rtThreadMask_ = 0;
    DWORD_PTR CpuIsolation::nonRtCoreMask_ = 0;

    // Legacy name list — NOT referenced by any repel path today. The monitor
    // repels by affinity overlap (any process whose mask still contains the RT
    // core), not by name, so keeping the list up to date is not required.
    // Retained only as a candidate list for targeted repelling.
    const std::vector<std::wstring> CpuIsolation::targetProcessNames = {
        L"DCS.exe",
        L"vrserver.exe",
        L"vrmonitor.exe",
        L"OVRServer_x64.exe",
        L"SteamVR.exe",
        L"VirtualDesktop.Streamer.exe",
        // USB 裝置相關程序 — 這些程序可能透過 USB 輪詢/回呼
        // 間接搶佔 RT 核心，雖然 kernel DPC 才是主要干擾源
        L"GameBar.exe",
        L"GameBarFTServer.exe",
        L"XboxGameBar.exe",
        L"GameInputHost.exe",       // Windows GameInput service (USB gamepad)
        L"GameInputSvc.exe",        // GameInput 服務主機
        L"widgets.exe",             // Windows Web Experience Pack (USB audio/video)
        L"SkypeBackgroundHost.exe",
        L"YourPhone.exe",           // Phone Link (USB tethering)
        L"Arc.exe",                 // Arc browser (WebUSB 可能干擾)
        // 常見瀏覽器 — 這些程序可能因為 WebGL/WebUSB 或後台計算
        // 產生高頻中斷或 DPC，應從 RT 核心驅離
        L"chrome.exe",              // Google Chrome
        L"msedge.exe",              // Microsoft Edge (Chromium)
        L"firefox.exe",             // Mozilla Firefox
        L"opera.exe",               // Opera
        L"brave.exe",               // Brave
        L"vivaldi.exe",             // Vivaldi
        L"yandexbrowser.exe",       // Yandex Browser
        L"iexplore.exe",            // Internet Explorer (legacy)
    };

    // ── Public API ──

    bool CpuIsolation::InitializeHardIsolation(int rtPhysicalCoreIndex, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[CpuIsolation] === Initializing OS-level hard core isolation ===\n";
        log << "[CpuIsolation] Target physical core: " << rtPhysicalCoreIndex << "\n";

        if (hardIsolationActive_) {
            log << "[CpuIsolation] Hard isolation already active; skipping\n";
            logOut = log.str();
            return true;
        }

        // Step 1: detect CPU topology (physical → logical core mapping)
        if (!DetectCpuTopology(rtPhysicalCoreIndex, logOut)) {
            log << "[CpuIsolation] CPU topology detection failed\n";
            logOut = log.str() + logOut;
            // Also write to stdout so it's visible without DebugView
            printf("[CpuIsolation] HARD ISOLATION FAILED — RT core %d not available on this system\n",
                   rtPhysicalCoreIndex);
            printf("[CpuIsolation] Check bcdedit numproc, Hyper-V core partitioning, or CPU core count\n");
            printf("[CpuIsolation] Cyclic thread will use legacy single-core affinity as fallback\n");
            return false;
        }

        rtPhysicalCore_ = rtPhysicalCoreIndex;

        // Step 2: apply process-level isolation
        ApplyProcessLevelIsolation(logOut);
        log << logOut;

        hardIsolationActive_ = true;
        log << "[CpuIsolation] RT core mask (process exclusion): 0x" << std::hex << rtCoreMask_
            << "  RT thread mask (cyclic pin): 0x" << rtThreadMask_
            << "  Non-RT mask: 0x" << nonRtCoreMask_ << std::dec << "\n";
        log << "[CpuIsolation] ✓ Hard isolation active — RT physical core reserved, HT sibling idle for clean L1/L2\n";

        // Also write concise status to stdout for console / log capture
        {
            CpuTopology::EnsurePopulated();
            const auto* ci = CpuTopology::FindCoreByPhysIdx(rtPhysicalCore_);
            int primaryLp = (ci && !ci->lps.empty()) ? ci->lps[0] : rtPhysicalCore_;
            printf("[CpuIsolation] HARD ISOLATION ACTIVE: CPU %d (physical core %d)"
                   " → thread mask 0x%llX, process excluded from 0x%llX\n",
                   primaryLp, rtPhysicalCore_,
                   static_cast<unsigned long long>(rtThreadMask_),
                   static_cast<unsigned long long>(rtCoreMask_));
        }

        logOut = log.str();
        return true;
    }

    bool CpuIsolation::PinCurrentThreadToRtCores() {
        if (!hardIsolationActive_ || rtThreadMask_ == 0) {
            return false;
        }

        HANDLE hThread = GetCurrentThread();

        // ── Win11+ additional thread-level boosts (safe no-op on older OS) ──
        // ThreadIoPriority → VERY_LOW + MMCSS "Pro Audio" registration.
        // Applied BEFORE hard affinity pin so that MMCSS soft scheduling hints
        // cannot override the subsequent SetThreadAffinityMask. On Win11 the
        // MMCSS scheduler may influence core selection; hard affinity must be
        // the final word to guarantee the thread stays on the RT core.
        {
            std::string boostLog;
            ApplyRealtimeThreadBoost(boostLog);
        }

        // Pin to the FIRST logical core of the physical core ONLY.
        // The HT/SMT sibling is left idle to keep L1/L2 cache clean —
        // no cache-line bouncing between logical cores on the same physical core.
        DWORD_PTR prev = SetThreadAffinityMask(hThread, rtThreadMask_);
        if (prev == 0) {
            return false;
        }

        // Boost priority to real-time time-critical — the highest user-mode priority.
        // With REALTIME_PRIORITY_CLASS process: level 31 (absolute highest user mode).
        // With HIGH_PRIORITY_CLASS process: level 15 (still above most threads).
        // Also disable dynamic priority boost to prevent OS from adjusting it.
        SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
        SetThreadPriorityBoost(hThread, TRUE);  // TRUE = disable boosting

        // ── Non-destructive verification ──
        // Re-assert the RT mask and verify the return value. Because
        // SetThreadAffinityMask returns the PREVIOUS mask on success,
        // a second call with the same mask should return rtThreadMask_
        // if the first call truly succeeded. If it was silently clamped
        // (e.g. RT core not in process mask), the returned "previous"
        // mask will differ.
        //
        // IMPORTANT: Do NOT widen to sysMask and narrow back — the
        // temporary widening gives the Win11 scheduler a window to
        // migrate the thread off the RT core, and the subsequent
        // narrowing does not force an immediate migration back.
        // Thread stays on the wrong core until next quantum.
        {
            DWORD_PTR reassert = SetThreadAffinityMask(hThread, rtThreadMask_);
            if (reassert != rtThreadMask_) {
                // The first call may have been silently clamped.
                // Try to restore the actual previous mask, then fail.
                if (reassert != 0) {
                    SetThreadAffinityMask(hThread, reassert);
                }
                return false;
            }
        }

        // Force an immediate context switch so the scheduler migrates
        // this thread to the RT core before we enter the PDO loop.
        // Without this, a TIME_CRITICAL thread may stay on the wrong
        // core for an entire quantum (especially on Win11 where the
        // scheduler defers migration longer).
        SwitchToThread();

        return true;
    }

    bool CpuIsolation::ExcludeCurrentThreadFromRtCores() {
        if (!hardIsolationActive_ || nonRtCoreMask_ == 0) {
            return false;
        }

        HANDLE hThread = GetCurrentThread();
        DWORD_PTR prev = SetThreadAffinityMask(hThread, nonRtCoreMask_);
        // nonRtCoreMask_ excludes ALL logical cores of the RT physical core
        // (both physical and HT/SMT sibling). This keeps L1/L2 cache clean
        // for the cyclic thread.
        return (prev != 0);
    }

    DWORD_PTR CpuIsolation::GetRtCoreMask() {
        return rtCoreMask_;
    }

    DWORD_PTR CpuIsolation::GetRtThreadMask() {
        return rtThreadMask_;
    }

    DWORD_PTR CpuIsolation::GetNonRtCoreMask() {
        return nonRtCoreMask_;
    }

    int CpuIsolation::GetRtPhysicalCoreIndex() {
        return rtPhysicalCore_;
    }

    bool CpuIsolation::IsHardIsolationActive() {
        return hardIsolationActive_;
    }

    // ── CPU topology detection ──
    // Uses CpuTopology (shared cache over GetLogicalProcessorInformationEx)
    // to find which logical cores belong to the same physical core.

    bool CpuIsolation::DetectCpuTopology(int rtPhysicalCoreIndex, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;

        // Ensure topology is populated (idempotent, thread-safe)
        CpuTopology::EnsurePopulated();
        int physCoreCount = CpuTopology::PhysicalCoreCount();
        int totalLps      = CpuTopology::LogicalProcessorCount();

        // ── bcdedit / system-level CPU restriction pre-check ──
        {
            DWORD activeCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            WORD activeGroupCount = GetActiveProcessorGroupCount();
            log << "[CpuIsolation] OS-visible logical processors: " << activeCount
                << " (across " << activeGroupCount << " group(s))\n";

            // Attempt to query actual hardware logical core count via CPUID leaf 0xB.
            int cpuInfo[4] = {0};
            __cpuidex(cpuInfo, 0x0B, 0);
            DWORD hwLogicalCount = static_cast<DWORD>(cpuInfo[1] & 0xFFFF);
            if (hwLogicalCount > 0 && hwLogicalCount < 256) {
                log << "[CpuIsolation] Hardware-reported logical cores per package: "
                    << hwLogicalCount << "\n";
                if (activeCount < hwLogicalCount) {
                    log << "[CpuIsolation] ⚠ bcdedit numproc (or similar) appears active: "
                        << "OS sees " << activeCount << " cores but hardware has "
                        << hwLogicalCount << "+ logical cores\n";
                    log << "[CpuIsolation] This may prevent the RT physical core from being visible\n";
                } else {
                    log << "[CpuIsolation] ✓ No bcdedit restriction detected (OS sees all hardware cores)\n";
                }
            } else {
                log << "[CpuIsolation] Could not query hardware core count via CPUID\n";
            }
        }

        log << "[CpuIsolation] Topology: " << physCoreCount << " physical cores, "
            << totalLps << " logical processors"
            << (CpuTopology::IsSmtEnabled() ? " (SMT/HT enabled)" : " (SMT/HT disabled)")
            << "\n";

        // Look up the target physical core via CpuTopology
        const CpuTopology::CoreInfo* rtInfo = CpuTopology::FindCoreByPhysIdx(rtPhysicalCoreIndex);
        if (!rtInfo || rtInfo->lps.empty()) {
            log << "[CpuIsolation] Physical core " << rtPhysicalCoreIndex
                << " not found (system reports " << physCoreCount << " physical cores)\n";
            log << "[CpuIsolation] Possible causes:\n";
            log << "[CpuIsolation]   1. bcdedit numproc restriction (run 'bcdedit /deletevalue numproc')\n";
            log << "[CpuIsolation]   2. CPU has fewer than " << (rtPhysicalCoreIndex + 1) << " physical cores\n";
            log << "[CpuIsolation]   3. Hyper-V / WSL2 / VBS core partitioning\n";
            log << "[CpuIsolation] Hard isolation will be disabled; cyclic thread falls back to legacy affinity\n";
            logOut = log.str();
            return false;
        }

        // Build logical core mask from topology data
        DWORD_PTR coreMask = 0;
        for (int lp : rtInfo->lps) {
            coreMask |= ((DWORD_PTR)1 << lp);
        }

        log << "[CpuIsolation] Physical core " << rtPhysicalCoreIndex
            << " → logical CPUs:";
        for (size_t i = 0; i < rtInfo->lps.size(); ++i) {
            log << " " << rtInfo->lps[i];
        }
        log << " (mask 0x" << std::hex << coreMask << std::dec << ")";
        if (rtInfo->efficiencyClass == 1)      log << " [E-core]";
        else if (rtInfo->efficiencyClass >= 2) log << " [P-core]";
        if (rtInfo->lps.size() >= 2)           log << " [SMT]";
        log << "\n";

        // Calculate masks
        DWORD_PTR processMask = 0, systemMask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);

        rtCoreMask_    = coreMask & systemMask;

        // non-RT thread affinity: confine to the physical core immediately
        // before the RT core (near-RT core). Benefits:
        //   • cache locality with Cyclic thread (adjacent core)
        //   • leaves middle cores completely clean for DCS/game processes
        //   • EtherCAT IRQ on this core keeps IRQ <-> non-RT processing close
        {
            int nearRtPhys = rtPhysicalCoreIndex - 1;
            DWORD_PTR nearRtMask = 0;
            if (nearRtPhys >= 0) {
                const auto* nearInfo = CpuTopology::FindCoreByPhysIdx(nearRtPhys);
                if (nearInfo) {
                    for (int lp : nearInfo->lps)
                        nearRtMask |= ((DWORD_PTR)1 << lp);
                    nearRtMask &= systemMask;
                }
            }
            nonRtCoreMask_ = (nearRtMask != 0) ? nearRtMask
                                               : (systemMask & ~rtCoreMask_);
        }

        // Extract the FIRST (lowest-set-bit) logical core for thread pinning.
        if (rtCoreMask_ != 0) {
            DWORD_PTR lowestBit = rtCoreMask_ & (~rtCoreMask_ + 1);
            rtThreadMask_ = lowestBit;
        } else {
            rtThreadMask_ = 0;
        }

        if (rtCoreMask_ == 0) {
            log << "[CpuIsolation] RT core mask is zero — system doesn't allow access to target core\n";
            logOut = log.str();
            return false;
        }

        log << "[CpuIsolation] Non-RT threads confined to near-RT core mask 0x"
            << std::hex << nonRtCoreMask_ << std::dec
            << " (physical core " << (rtPhysicalCoreIndex - 1) << ", adjacent to RT)\n";

        logOut = log.str();
        return true;
    }

    // ── Apply process-level isolation ──
    // Keeps the process affinity mask at the FULL system mask and pins only the
    // calling (main/UI) thread to the non-RT mask. The process mask is NOT
    // narrowed — see the IMPORTANT note in the function body for why.

    void CpuIsolation::ApplyProcessLevelIsolation(std::string &logOut) {
        std::ostringstream log;

        // ── IMPORTANT: Do NOT exclude RT cores from the process affinity mask ──
        // Windows requires thread affinity masks to be a SUBSET of the process
        // affinity mask. If SetProcessAffinityMask excludes the RT core, then
        // SetThreadAffinityMask for the cyclic thread will silently fail, and
        // the cyclic thread ends up on a shared core — causing SOEM disconnects.
        //
        // Instead, keep the process mask at the full system mask and use
        // per-thread affinity:
        //   • Cyclic thread → PinCurrentThreadToRtCores() (subset of process mask ✓)
        //   • All non-RT threads → ExcludeCurrentThreadFromRtCores() pins them to
        //     the near-RT core (physical core just before RT, not the full non-RT set)
        //   • Near-RT core includes the EtherCAT IRQ — keeps IRQ processing close
        //     to where non-RT work happens, avoiding cross-core cache bouncing.
        //   • Middle cores are left completely clean for DCS World game processes.
        //   • Per-slave cyclic threads use AllocateCoreMaskRoundRobin() which draws
        //     from the process mask; BuildCoreCandidates intersects with nonRtCoreMask_
        //     to keep per-slave work also on the near-RT core.

        DWORD_PTR processMask = 0, systemMask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);

        // Pin the calling (main/UI) thread to non-RT cores so UI/HTTP/logging
        // work stays off the RT physical core (ALL logical cores including HT sibling).
        if (!ExcludeCurrentThreadFromRtCores()) {
            log << "[CpuIsolation] WARNING: Cannot pin main thread to non-RT cores (err="
                << GetLastError() << ")\n";
        } else {
            log << "[CpuIsolation] Main thread pinned to non-RT cores (0x"
                << std::hex << nonRtCoreMask_ << std::dec << ")\n";
        }

        log << "[CpuIsolation] Process affinity kept at full system mask (0x"
            << std::hex << systemMask << std::dec
            << ") — cyclic thread pins to RT core via per-thread affinity\n";
        log << "[CpuIsolation] RT cores 0x" << std::hex << rtCoreMask_
            << std::dec << " are reserved via per-thread + monitor repel\n";

        // Elevate process priority to REALTIME for the guard process itself.
        // Fallback to HIGH_PRIORITY_CLASS if Admin privileges are not available.
        // REALTIME_PRIORITY_CLASS threads preempt even system services — use only
        // after hard core isolation is in place to avoid starving the rest of the system.
        if (SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
            log << "[CpuIsolation] Process priority elevated to REALTIME_PRIORITY_CLASS\n";
        } else {
            DWORD err = GetLastError();
            if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
                log << "[CpuIsolation] REALTIME_PRIORITY_CLASS denied (err=" << err
                    << "), using HIGH_PRIORITY_CLASS fallback\n";
            } else {
                log << "[CpuIsolation] WARNING: Cannot elevate process priority (err="
                    << GetLastError() << ")\n";
            }
        }

        // ── Win11+ process-level power throttling disable ──
        // Must be called AFTER process priority elevation so the kernel sees
        // the correct priority class when evaluating throttling policy.
        {
            std::string ptLog;
            ApplyProcessPowerThrottling(ptLog);
            log << ptLog;
        }

        logOut = log.str();
    }

    // ── Win11+ process power throttling disable ──
    // Win11 (and Win10 1709+) applies CPU power throttling to every process by
    // default.  For a near-real-time cyclic thread this can cause the OS to
    // silently cap CPU frequency on the RT core, adding unpredictable latency.
    // This call tells the kernel to leave our process alone.
    void CpuIsolation::ApplyProcessPowerThrottling(std::string &logOut) {
        logOut.clear();
        std::ostringstream log;

        HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!hKernel32) {
            log << "[CpuIsolation] PowerThrottling: kernel32.dll not loaded — skipping\n";
            logOut = log.str();
            return;
        }

        auto pfnSetProcessInfo = reinterpret_cast<PFN_SetProcessInformation>(
            GetProcAddress(hKernel32, "SetProcessInformation"));
        if (!pfnSetProcessInfo) {
            log << "[CpuIsolation] PowerThrottling: SetProcessInformation not available"
                << " (pre-1709 Win10) — skipping\n";
            logOut = log.str();
            return;
        }

        CpuIso_PROCESS_POWER_THROTTLING_STATE throttling = {};
        throttling.Version     = 1;   // PROCESS_POWER_THROTTLING_CURRENT_VERSION
        throttling.ControlMask = 1;   // PROCESS_POWER_THROTTLING_EXECUTION_SPEED
        throttling.StateMask   = 0;   // disable throttling

        if (pfnSetProcessInfo(GetCurrentProcess(), 4 /* ProcessPowerThrottling */,
                              &throttling, sizeof(throttling))) {
            log << "[CpuIsolation] PowerThrottling: disabled (ProcessPowerThrottling)\n";
            printf("[CpuIsolation] PowerThrottling: disabled\n");
        } else {
            DWORD err = GetLastError();
            log << "[CpuIsolation] PowerThrottling: SetProcessInformation failed"
                << " (err=" << err << ") — may not be supported on this OS build\n";
        }

        logOut = log.str();
    }

    // ── Win11+ per-thread real-time boost ──
    // 1. ThreadIoPriority → VERY_LOW: prevents Npcap I/O completion callbacks
    //    (DPC-level) from preempting the cyclic thread on the RT core.
    // 2. AvSetMmThreadCharacteristics("Pro Audio"): tells the Win11 scheduler
    //    this is a latency-sensitive thread → larger scheduling quantum, less
    //    likely to be nudged by power-management heuristics.
    //
    // Both APIs are loaded dynamically.  On older Windows builds where they
    // don't exist, the function simply logs "not available" and returns.
    void CpuIsolation::ApplyRealtimeThreadBoost(std::string &logOut) {
        logOut.clear();
        std::ostringstream log;

        HANDLE hThread = GetCurrentThread();

        // ── 1. ThreadIoPriority ──
        {
            HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
            if (hKernel32) {
                auto pfnSetThreadInfo = reinterpret_cast<PFN_SetThreadInformation>(
                    GetProcAddress(hKernel32, "SetThreadInformation"));
                if (pfnSetThreadInfo) {
                    // IO_PRIORITY_VERY_LOW = 2 → minimal IO completion interference
                    ULONG ioPrio = 2;
                    if (pfnSetThreadInfo(hThread, 16 /* ThreadIoPriority */,
                                         &ioPrio, sizeof(ioPrio))) {
                        log << "[CpuIsolation] ThreadBoost: IO priority → VERY_LOW\n";
                    } else {
                        DWORD err = GetLastError();
                        log << "[CpuIsolation] ThreadBoost: SetThreadInformation(IO) failed"
                            << " (err=" << err << ")\n";
                    }
                } else {
                    log << "[CpuIsolation] ThreadBoost: SetThreadInformation not available"
                        << " — skipping IO priority\n";
                }
            }
        }

        // ── 2. AvSetMmThreadCharacteristics("Pro Audio") ──
        {
            HMODULE hAvrt = LoadLibraryW(L"avrt.dll");
            if (hAvrt) {
                auto pfnAvSet = reinterpret_cast<PFN_AvSetMmThreadCharacteristicsW>(
                    GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsW"));
                if (pfnAvSet) {
                    DWORD taskIndex = 0;
                    HANDLE hAvrtHandle = pfnAvSet(L"Pro Audio", &taskIndex);
                    if (hAvrtHandle) {
                        log << "[CpuIsolation] ThreadBoost: registered as 'Pro Audio'"
                            << " (taskIndex=" << taskIndex
                            << ") — scheduler quantum boosted\n";
                        // We intentionally do NOT call AvRevertMmThreadCharacteristics
                        // for the cyclic thread — it stays in Pro Audio class for life.
                        // Store handle so we could revert on shutdown if needed.
                        // (handle closed by OS on thread exit.)
                    } else {
                        DWORD err = GetLastError();
                        log << "[CpuIsolation] ThreadBoost: AvSetMmThreadCharacteristics"
                            << " failed (err=" << err
                            << ") — already registered by another caller?\n";
                    }
                } else {
                    log << "[CpuIsolation] ThreadBoost: AvSetMmThreadCharacteristics"
                        << " not found in avrt.dll\n";
                }
                // Keep avrt.dll loaded for the lifetime of the thread
                // (FreeLibrary would invalidate the MMCSS registration).
            } else {
                log << "[CpuIsolation] ThreadBoost: avrt.dll not available"
                    << " — skipping MMCSS registration\n";
            }
        }

        logOut = log.str();
        printf("%s", log.str().c_str());  // also emit to stdout for console capture
    }

    // ── Isolation monitor (software-level) ──

    void CpuIsolation::StartIsolationMonitor() {
        if (monitorRunning.load()) {
            return;
        }
        if (!monitorStopEvent) {
            monitorStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        }
        if (!s_repelTriggerEvent) {
            s_repelTriggerEvent = CreateEventW(NULL, FALSE, FALSE, NULL);  // auto-reset
        }
        monitorRunning.store(true);
        monitorThread = std::thread(MonitorThreadLoop);
    }

    void CpuIsolation::StopIsolationMonitor() {
        if (monitorRunning.load()) {
            monitorRunning.store(false);
            if (monitorStopEvent) {
                SetEvent(monitorStopEvent);
            }
            if (s_repelTriggerEvent) {
                SetEvent(s_repelTriggerEvent);  // unblock monitor if waiting
            }
            if (monitorThread.joinable()) {
                monitorThread.join();
            }
            if (monitorStopEvent) {
                CloseHandle(monitorStopEvent);
                monitorStopEvent = NULL;
            }
            if (s_repelTriggerEvent) {
                CloseHandle(s_repelTriggerEvent);
                s_repelTriggerEvent = NULL;
            }
        }
    }

    void CpuIsolation::TriggerRepel() {
        if (s_repelTriggerEvent) {
            SetEvent(s_repelTriggerEvent);
        }
    }

    void CpuIsolation::MonitorThreadLoop() {
        // Exclude this monitor thread from the RT physical core (ALL logical
        // cores including HT/SMT sibling) so the repel scan itself doesn't
        // pollute the cyclic thread's L1/L2 cache.
        ExcludeCurrentThreadFromRtCores();

        // Use the hard isolation mask if available; fall back to legacy single-bit
        // mask only when rtPhysicalCore_ is valid (>= 0).  If rtPhysicalCore_ == -1
        // (init never ran), the monitor has nothing to guard and returns immediately.
        DWORD_PTR guardMask = rtCoreMask_;
        if (guardMask == 0 && rtPhysicalCore_ >= 0) {
            guardMask = CpuTopology::GetPrimaryLpMask(rtPhysicalCore_);
        }
        if (guardMask == 0 && rtPhysicalCore_ >= 0) {
            // Last resort: treat physIdx as logical CPU (legacy, works without SMT)
            guardMask = (DWORD_PTR)1 << rtPhysicalCore_;
        }

        // Verify that the guard mask cores exist on this system
        DWORD_PTR processAffinity = 0, systemAffinity = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processAffinity, &systemAffinity);
        if ((systemAffinity & guardMask) == 0) {
            return;
        }

        // ── Configurable repel parameters ──
        // The interval is read from s_repelIntervalMs every cycle, so a future
        // caller of SetRepelIntervalMs takes effect immediately — but no caller
        // exists today, so it stays at the 10ms default. Values are clamped to
        // 5..100ms (a 2ms request is therefore honored as 5ms).
        // Default 10ms ≈ 5× a 2ms PDO cycle.
        // 5ms is the practical floor: below it the per-scan cost (one Toolhelp
        // snapshot + one OpenProcess per process) starts to dominate the near-RT
        // core this monitor shares with the EtherCAT NIC IRQ and the other
        // non-RT threads.
        constexpr int kRepelSliceMinMs = 5;   // finest sleep slice
        constexpr int kRepelSliceMaxMs = 10;  // coarsest sleep slice
        constexpr int kFullScanInterval = 5;  // every N repels → process + thread repel
        constexpr int kThreadRepelInterval = 3; // every N repels → thread-level force-migrate
        int repelCycle = 0;

        while (monitorRunning.load()) {
            // Read repel interval each cycle for dynamic updates
            int intervalMs = s_repelIntervalMs.load(std::memory_order_relaxed);
            if (intervalMs < 5) intervalMs = 5;
            if (intervalMs > 100) intervalMs = 100;
            const int sliceMs = (intervalMs <= 20) ? kRepelSliceMinMs : kRepelSliceMaxMs;
            const int slices = (intervalMs + sliceMs - 1) / sliceMs;

            ++repelCycle;
            const bool doFullSweep = (repelCycle % kFullScanInterval == 0);
            const bool doThreadRepel = (repelCycle % kThreadRepelInterval == 0);

            if (doFullSweep) {
                // Full repel: sweep all processes + thread-level force-migrate
                // for processes whose affinity was just changed.
                RepelOtherProcesses(guardMask);
            } else if (doThreadRepel) {
                // Mid-level: fast process repel + immediate thread-level
                // force-migrate for the processes whose mask was just changed.
                // Reacts faster than the full sweep (kThreadRepelInterval <
                // kFullScanInterval) at the cost of the extra thread snapshot.
                RepelOtherProcessesFast(guardMask, /*forceThreadMigrate=*/true);
            } else {
                // Fast repel: process-affinity-only scan
                RepelOtherProcessesFast(guardMask);
            }

            for (int i = 0; i < slices; ++i) {
                if (!monitorRunning.load()) break;
                if (monitorStopEvent) {
                    WaitForSingleObject(monitorStopEvent, sliceMs);
                } else {
                    Sleep(sliceMs);
                }
            }
        }
    }

    // System processes that should never be repelled (critical to OS stability).
    // These run essential kernel-mode or session services; restricting their
    // affinity can cause instability or access-denied errors.
    //
    // NOTE: svchost.exe (Service Host) is deliberately NOT in this list.
    // svchost.exe hosts Windows Defender, firewall (mpssvc), task scheduler
    // (schedsvc), and many other services. All of these are safe to restrict
    // from the RT core — they have many other cores available on modern
    // systems, and keeping them off the RT core improves PDO cycle stability.
    // If a service truly requires the RT core for timing, it should use
    // SetThreadAffinityMask explicitly rather than depending on process-wide
    // affinity inheritance.
    static bool IsCriticalSystemProcess(const wchar_t* exeName) {
        if (!exeName || exeName[0] == L'\0') return false;
        // System pseudo-process (PID 4) has no exe name in snapshot
        static const wchar_t* criticalList[] = {
            L"System",              // PID 4, NT kernel
            L"System Idle Process", // PID 0
            L"smss.exe",            // Session Manager
            L"csrss.exe",           // Client/Server Runtime
            L"wininit.exe",         // Windows Initialization
            L"services.exe",        // Service Control Manager
            L"lsass.exe",           // Local Security Authority
            L"winlogon.exe",        // Windows Logon
            // svchost.exe intentionally excluded — see NOTE above
        };
        for (const auto* name : criticalList) {
            if (_wcsicmp(exeName, name) == 0) return true;
        }
        return false;
    }

    void CpuIsolation::RepelOnce() {
        DWORD_PTR mask = rtCoreMask_;
        if (mask == 0 && rtPhysicalCore_ >= 0) {
            mask = CpuTopology::GetPrimaryLpMask(rtPhysicalCore_);
        }
        if (mask == 0 && rtPhysicalCore_ >= 0) {
            mask = (DWORD_PTR)1 << rtPhysicalCore_;
        }
        if (mask == 0) return;
        RepelOtherProcesses(mask);
    }

    std::string CpuIsolation::VerifyRtCoreIsolation() {
        std::ostringstream report;

        DWORD_PTR guardMask = rtCoreMask_;
        if (guardMask == 0) {
            report << "[Verify] RT core mask is zero — cannot verify\n";
            return report.str();
        }

        DWORD ourPid = GetCurrentProcessId();
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) {
            report << "[Verify] Failed to create process snapshot (err="
                   << GetLastError() << ")\n";
            return report.str();
        }

        struct Violator {
            DWORD pid;
            std::string name;         // UTF-8 process name
            DWORD_PTR affinity;
        };
        std::vector<Violator> violators;

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == ourPid) continue;
                if (pe32.th32ProcessID == 0) continue;  // System Idle Process

                // Convert wide process name to UTF-8 for reporting
                std::wstring wname(pe32.szExeFile);
                int len = WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                              nullptr, 0, nullptr, nullptr);
                std::string utf8Name;
                if (len > 0) {
                    utf8Name.resize(static_cast<size_t>(len) - 1);
                    WideCharToMultiByte(CP_UTF8, 0, wname.c_str(), -1,
                                        &utf8Name[0], len, nullptr, nullptr);
                }

                HANDLE hProcess = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe32.th32ProcessID);
                if (!hProcess) continue;

                DWORD_PTR procAffinity = 0, sysAffinity = 0;
                if (GetProcessAffinityMask(hProcess, &procAffinity, &sysAffinity)) {
                    if ((procAffinity & guardMask) != 0) {
                        violators.push_back({pe32.th32ProcessID, utf8Name, procAffinity});
                    }
                }
                CloseHandle(hProcess);
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);

        if (violators.empty()) {
            report << "[Verify] ✓ RT core 0x" << std::hex << guardMask << std::dec
                   << " is clear — no foreign process affinity overlap detected\n";
        } else {
            report << "[Verify] ⚠ " << violators.size()
                   << " foreign process(es) still have RT core 0x"
                   << std::hex << guardMask << std::dec << " in affinity:\n";
            for (const auto& v : violators) {
                report << "[Verify]   • " << v.name << " (PID " << v.pid
                       << ")  affinity=0x" << std::hex << v.affinity << std::dec << "\n";
            }
            report << "[Verify] These processes may interfere with cyclic thread timing.\n";
            report << "[Verify] Run as Administrator to enable automatic repel at startup.\n";
        }

        return report.str();
    }

    // ── Runtime RT core diagnostics ──
    // Uses NtQuerySystemInformation(SystemProcessorPerformanceInformation)
    // to read per-core DPC and interrupt counters, then compares the RT core
    // against other cores to prove isolation effectiveness.

    // Forward-declare NtQuerySystemInformation (ntdll.dll)
    typedef LONG NTSTATUS;
    #ifndef SystemProcessorPerformanceInformation
    #define SystemProcessorPerformanceInformation 0x08
    #endif

    typedef struct _CPU_PERF_INFO {
        LARGE_INTEGER IdleTime;
        LARGE_INTEGER KernelTime;
        LARGE_INTEGER UserTime;
        LARGE_INTEGER DpcTime;
        LARGE_INTEGER InterruptTime;
        ULONG         InterruptCount;
    } CPU_PERF_INFO;

    typedef NTSTATUS (WINAPI *pNtQuerySystemInformation)(
        ULONG SystemInformationClass,
        PVOID SystemInformation,
        ULONG SystemInformationLength,
        PULONG ReturnLength
    );

    std::string CpuIsolation::DiagnoseRtCoreState() {
        std::ostringstream rpt;
        rpt << "[Diagnose] ========== RT Core Isolation Diagnostic ==========\n";

        // ── 1. CPU topology & masks ──
        int physCore = rtPhysicalCore_;
        bool active = hardIsolationActive_;
        rpt << "[Diagnose] Hard isolation active: " << (active ? "YES" : "NO") << "\n";
        rpt << "[Diagnose] RT physical core index: " << physCore << "\n";
        rpt << "[Diagnose] RT core mask (process exclusion): 0x" << std::hex << rtCoreMask_ << std::dec << "\n";
        rpt << "[Diagnose] RT thread mask (cyclic pin): 0x" << std::hex << rtThreadMask_ << std::dec << "\n";
        rpt << "[Diagnose] Non-RT mask (main/UI thread): 0x" << std::hex << nonRtCoreMask_ << std::dec << "\n";

        // ── 2. Process & current thread affinity ──
        {
            DWORD_PTR procMask = 0, sysMask = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
            rpt << "[Diagnose] Process affinity: 0x" << std::hex << procMask
                << "  System affinity: 0x" << sysMask << std::dec << "\n";

            // Check: is RT core IN the process mask? (critical — must be YES)
            bool rtInProcess = (procMask & rtCoreMask_) != 0;
            rpt << "[Diagnose] RT core in process mask: " << (rtInProcess ? "✓ YES" : "✗ NO — CYCLIC THREAD CANNOT PIN!")
                << "\n";

            // Current thread affinity (query by setting+restoring)
            HANDLE hCur = GetCurrentThread();
            DWORD_PTR curAffinity = 0;
            {
                // SetThreadAffinityMask returns the PREVIOUS mask
                DWORD_PTR prev = SetThreadAffinityMask(hCur, sysMask);
                if (prev != 0) {
                    curAffinity = prev;
                    SetThreadAffinityMask(hCur, curAffinity);  // restore
                }
            }
            rpt << "[Diagnose] This (main/UI) thread affinity: 0x" << std::hex << curAffinity << std::dec;
            if (!active || (curAffinity & rtThreadMask_) == 0) {
                rpt << " (RT core excluded ✓)\n";
            } else {
                rpt << " ⚠ OVERLAPS RT CORE!\n";
            }
        }

        // ── 3. Logical → physical core mapping (full topology dump via CpuTopology) ──
        {
            rpt << "[Diagnose] Physical → Logical core mapping (Task Manager numbering):\n";
            rpt << CoreHelpers::CpuTopology::DumpTable();
        }

        // ── 4. Per-core DPC / Interrupt counters (NtQuerySystemInformation) ──
        {
            HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
            if (hNtdll) {
                auto pQuery = reinterpret_cast<pNtQuerySystemInformation>(
                    GetProcAddress(hNtdll, "NtQuerySystemInformation"));
                if (pQuery) {
                    DWORD activeCount = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
                    std::vector<CPU_PERF_INFO> perf(activeCount);
                    memset(perf.data(), 0, sizeof(CPU_PERF_INFO) * activeCount);
                    ULONG retLen = 0;
                    NTSTATUS st = pQuery(SystemProcessorPerformanceInformation,
                                        perf.data(),
                                        static_cast<ULONG>(sizeof(CPU_PERF_INFO) * activeCount),
                                        &retLen);
                    if (st == 0 && retLen >= sizeof(CPU_PERF_INFO)) {
                        ULONG actualCount = retLen / sizeof(CPU_PERF_INFO);
                        rpt << "[Diagnose] Per-core DPC / Interrupt counters"
                            << " (" << actualCount << " logical CPUs):\n";
                        rpt << "[Diagnose]   CPU | DPC (100ns) | Interrupt (100ns) | IntCount\n";

                        // Find ALL logical CPUs belonging to the RT physical core
                        std::vector<int> rtLogicalCpus;
                        if (rtCoreMask_ != 0) {
                            DWORD_PTR m = rtCoreMask_;
                            int idx = 0;
                            while (m != 0) {
                                if (m & 1) rtLogicalCpus.push_back(idx);
                                m >>= 1;
                                ++idx;
                            }
                        }

                        // Aggregate RT core stats across ALL its logical CPUs
                        LONGLONG rtTotalDpc = 0, rtTotalIntr = 0;
                        ULONG rtTotalIntCount = 0;
                        LONGLONG maxDpc = 0, maxIntr = 0;
                        int maxDpcCpu = -1, maxIntrCpu = -1;

                        for (ULONG i = 0; i < actualCount && i < activeCount; ++i) {
                            LONGLONG dpc = perf[i].DpcTime.QuadPart;
                            LONGLONG intr = perf[i].InterruptTime.QuadPart;
                            ULONG ic = perf[i].InterruptCount;

                            bool isRt = false;
                            for (int rc : rtLogicalCpus) {
                                if (static_cast<int>(i) == rc) { isRt = true; break; }
                            }
                            if (isRt) {
                                rtTotalDpc += dpc; rtTotalIntr += intr; rtTotalIntCount += ic;
                            }
                            if (dpc > maxDpc) { maxDpc = dpc; maxDpcCpu = static_cast<int>(i); }
                            if (intr > maxIntr) { maxIntr = intr; maxIntrCpu = static_cast<int>(i); }

                            // Show every CPU with the RT cores marked
                            const char* tag = "";
                            if (isRt) tag = " ← RT";
                            else if (static_cast<int>(i) == maxDpcCpu) tag = " (max DPC)";
                            else if (static_cast<int>(i) == maxIntrCpu) tag = " (max IRQ)";

                            rpt << "[Diagnose]   " << std::setw(3) << i
                                << " | " << std::setw(10) << dpc
                                << " | " << std::setw(15) << intr
                                << " | " << std::setw(8) << ic
                                << tag << "\n";
                        }

                        // ── RT core health verdict (ALL logical CPUs) ──
                        rpt << "[Diagnose] --- RT physical core (logical CPUs:";
                        for (size_t j = 0; j < rtLogicalCpus.size(); ++j) {
                            rpt << " " << rtLogicalCpus[j];
                        }
                        rpt << ") health ---\n";
                        rpt << "[Diagnose]   Total DPC time: " << rtTotalDpc
                            << " (max system: " << maxDpc << " on CPU " << maxDpcCpu << ")\n";
                        rpt << "[Diagnose]   Total Interrupt time: " << rtTotalIntr
                            << " (max system: " << maxIntr << " on CPU " << maxIntrCpu << ")\n";
                        rpt << "[Diagnose]   Total Interrupt count: " << rtTotalIntCount << "\n";

                        if (rtTotalDpc == 0 && rtTotalIntr == 0 && rtTotalIntCount == 0) {
                            rpt << "[Diagnose] ✓ VERDICT: ALL RT logical CPUs have ZERO DPC/IRQ — perfect isolation\n";
                        } else if (rtTotalDpc < maxDpc / 10 && rtTotalIntr < maxIntr / 10) {
                            rpt << "[Diagnose] ✓ VERDICT: RT core DPC/IRQ well below system average — good isolation\n";
                        } else if (rtTotalIntr > 0) {
                            rpt << "[Diagnose] ⚠ WARNING: RT logical CPUs have interrupt activity!\n";
                            rpt << "[Diagnose]   Check IRQ affinity (requires reboot after registry write)\n";
                            rpt << "[Diagnose]   System interrupts (clock/IPI/DPC watchdog) may still fire here\n";
                        } else {
                            rpt << "[Diagnose] ⚠ NOTE: RT core has DPC activity — monitor for jitter\n";
                        }

                        // ── Compare RT core IRQ count vs system total ──
                        ULONG totalIntCount = 0;
                        for (ULONG i = 0; i < actualCount && i < activeCount; ++i) {
                            totalIntCount += perf[i].InterruptCount;
                        }
                        double rtPct = (totalIntCount > 0)
                            ? (100.0 * rtTotalIntCount / totalIntCount) : 0.0;
                        rpt << "[Diagnose]   RT core interrupt share: " << rtTotalIntCount
                            << " / " << totalIntCount << " total interrupts ("
                            << std::fixed << std::setprecision(1) << rtPct << "%)\n";
                    } else {
                        rpt << "[Diagnose] NtQuerySystemInformation failed (st=" << st
                            << ", retLen=" << retLen << ")\n";
                    }
                } else {
                    rpt << "[Diagnose] Cannot resolve NtQuerySystemInformation\n";
                }
            } else {
                rpt << "[Diagnose] Cannot load ntdll.dll\n";
            }
        }

        rpt << "[Diagnose] ========== End Diagnostic ==========\n";
        return rpt.str();
    }

    // ── Fast repel: affinity-only scan ──
    // Cheapest tier of the three-tier monitor: one process snapshot, one
    // OpenProcess per process, and a SetProcessAffinityMask write only for
    // processes that still overlap the RT core.
    //
    // forceThreadMigrate = true is the mid-level tier used by
    // MonitorThreadLoop() every kThreadRepelInterval cycles. Processes whose
    // mask was just changed also get their threads force-migrated immediately
    // via RepelProcessThreadsFromRtCore(), instead of lingering on the RT core
    // until the scheduler applies the new process mask at the next quantum.
    //
    // The full RepelOtherProcesses() sweep runs every kFullScanInterval cycles.
    void CpuIsolation::RepelOtherProcessesFast(DWORD_PTR rtCoreMask, bool forceThreadMigrate) {
        if (rtCoreMask == 0) return;

        DWORD ourPid = GetCurrentProcessId();
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) return;

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                if (pe32.th32ProcessID == ourPid) continue;
                if (pe32.th32ProcessID == 0) continue;
                if (IsCriticalSystemProcess(pe32.szExeFile)) continue;

                HANDLE hProcess = OpenProcess(
                    PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
                    FALSE, pe32.th32ProcessID);
                if (!hProcess) continue;

                DWORD_PTR pMask = 0, sMask = 0;
                if (GetProcessAffinityMask(hProcess, &pMask, &sMask)) {
                    if ((pMask & rtCoreMask) != 0) {
                        DWORD_PTR newMask = pMask & ~rtCoreMask;
                        if (newMask == 0) {
                            newMask = sMask & ~rtCoreMask;
                        }
                        if (newMask != 0) {
                            if (SetProcessAffinityMask(hProcess, newMask) && forceThreadMigrate) {
                                // Thread-level force-migrate: without this, threads
                                // already running on the RT core can stay there until
                                // quantum expiry (up to several ms) — long enough to
                                // spike a PDO cycle.
                                RepelProcessThreadsFromRtCore(pe32.th32ProcessID, rtCoreMask);
                            }
                        }
                    }
                }
                CloseHandle(hProcess);
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }

    void CpuIsolation::RepelOtherProcesses(DWORD_PTR rtCoreMask) {
        if (rtCoreMask == 0) return;

        DWORD ourPid = GetCurrentProcessId();
        DWORD_PTR procAffinity = 0, sysAffinity = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &procAffinity, &sysAffinity);
        DWORD_PTR safeNonRtMask = sysAffinity & ~rtCoreMask;
        if (safeNonRtMask == 0) return;  // no non-RT cores available

        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (hSnapshot == INVALID_HANDLE_VALUE) return;

        PROCESSENTRY32W pe32;
        pe32.dwSize = sizeof(PROCESSENTRY32W);

        if (Process32FirstW(hSnapshot, &pe32)) {
            do {
                // Skip our own process
                if (pe32.th32ProcessID == ourPid) continue;
                // Skip critical system processes
                if (IsCriticalSystemProcess(pe32.szExeFile)) continue;
                // Skip idle / system pseudo-processes (PID 0)
                if (pe32.th32ProcessID == 0) continue;

                HANDLE hProcess = OpenProcess(
                    PROCESS_SET_INFORMATION | PROCESS_QUERY_INFORMATION,
                    FALSE, pe32.th32ProcessID);
                if (!hProcess) continue;  // access denied → skip

                DWORD_PTR pMask = 0, sMask = 0;
                if (GetProcessAffinityMask(hProcess, &pMask, &sMask)) {
                    if ((pMask & rtCoreMask) != 0) {
                        // Remove RT cores from this process's allowed set
                        DWORD_PTR newMask = pMask & ~rtCoreMask;
                        if (newMask == 0) {
                            // If the process was ONLY on RT cores, give it
                            // all system cores except RT cores
                            newMask = sMask & ~rtCoreMask;
                        }
                        if (newMask != 0) {
                            if (SetProcessAffinityMask(hProcess, newMask)) {
                                // ── Thread-level force-migrate ──
                                // After fixing process mask, immediately migrate
                                // all threads of this process off the RT core.
                                // Without this, threads already running on RT core
                                // may stay there until quantum expiry (up to several ms),
                                // which is enough to spike a PDO cycle.
                                RepelProcessThreadsFromRtCore(pe32.th32ProcessID, rtCoreMask);
                            }
                        }
                    }
                }
                CloseHandle(hProcess);
            } while (Process32NextW(hSnapshot, &pe32));
        }
        CloseHandle(hSnapshot);
    }

    // ── Thread-level repel: force-migrate all threads of a single process ──
    // Uses CreateToolhelp32Snapshot with process-ID filtering to enumerate
    // only threads belonging to the target process (efficient per-process scan).
    // For each thread, sets thread affinity to exclude the RT core mask.
    // This guarantees that even threads with explicit SetThreadAffinityMask
    // (e.g. Chrome GPU compositor) are immediately moved off the RT core,
    // rather than waiting for the scheduler to respect the updated process mask
    // at the next quantum boundary.
    void CpuIsolation::RepelProcessThreadsFromRtCore(DWORD pid, DWORD_PTR rtCoreMask) {
        if (rtCoreMask == 0 || pid == 0) return;

        // Compute safe non-RT mask (all available cores except RT)
        DWORD_PTR procAffinity = 0, sysAffinity = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &procAffinity, &sysAffinity);
        DWORD_PTR safeMask = sysAffinity & ~rtCoreMask;
        if (safeMask == 0) return;

        // Thread snapshot filtered to this process only (efficient, no system-wide scan)
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, pid);
        if (hThreadSnap == INVALID_HANDLE_VALUE) return;

        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);

        if (Thread32First(hThreadSnap, &te32)) {
            do {
                // Safety: only touch threads belonging to the target process
                if (te32.th32OwnerProcessID != pid) continue;

                HANDLE hThread = OpenThread(
                    THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION,
                    FALSE, te32.th32ThreadID);
                if (!hThread) continue;

                // SetThreadAffinityMask returns the PREVIOUS affinity.
                // We set to safeMask (all cores except RT) — this both queries
                // the old mask AND forces migration in one call.
                // If the thread was already off RT core, this is a no-op
                // (the scheduler doesn't need to migrate).
                DWORD_PTR prev = SetThreadAffinityMask(hThread, safeMask);
                (void)prev;  // old mask not needed — we already fixed it

                CloseHandle(hThread);
            } while (Thread32Next(hThreadSnap, &te32));
        }
        CloseHandle(hThreadSnap);
    }

}
