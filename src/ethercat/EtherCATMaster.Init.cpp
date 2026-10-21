#include "EtherCATMaster.h"
#include "../include/core/CpuIsolation.h"
#include "../include/core/CpuTopology.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
#include <intrin.h>
#include <mmsystem.h>
#include <soem/ec_print.h>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "SafeMem.h"
#include "EsiParser.h"
#include "../include/core/CoreHelpers.h"
#include "../include/core/CpuIsolation.h"

namespace {

constexpr int kStableProcessDataSleepUs = 5000;
constexpr int kStableProcessDataMaxRounds = 10;
constexpr int kStableProcessDataMinReadyRounds = 2;
constexpr int kSafeOpPollRounds = 30;
constexpr int kSafeOpPollTimeoutUs = 50000;
constexpr int kSafeOpPollSleepUs = 5000;
constexpr int kSafeOpFallbackTimeoutUs = EC_TIMEOUTSTATE;
constexpr int kOpPollRounds = 60;
constexpr int kOpPollTimeoutUs = 10000;
constexpr DWORD kOpMaxPollSleepMs = 5;
constexpr int kOpFallbackTimeoutUs = EC_TIMEOUTSTATE;

bool HasReachedState(int currentState, uint16 targetState)
{
    return (currentState & targetState) == targetState;
}

bool PumpProcessDataUntilStable(ecx_contextt* context, int maxRounds, int sleepUs)
{
    int readyRounds = 0;
    for (int round = 0; round < maxRounds; ++round) {
        ecx_send_processdata(context);
        const int wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
        readyRounds = (wkc > 0) ? (readyRounds + 1) : 0;
        if (readyRounds >= kStableProcessDataMinReadyRounds) {
            return true;
        }
        osal_usleep(sleepUs);
    }

    return false;
}

int WaitForStateWithProcessData(
    ecx_contextt* context,
    uint16 targetState,
    int maxRounds,
    int pollTimeoutUs,
    int sleepUs,
    int fallbackTimeoutUs)
{
    int currentState = 0;
    for (int round = 0; round < maxRounds; ++round) {
        ecx_send_processdata(context);
        ecx_receive_processdata(context, EC_TIMEOUTRET);
        currentState = ecx_statecheck(context, 0, targetState, pollTimeoutUs);
        if (HasReachedState(currentState, targetState)) {
            return currentState;
        }
        osal_usleep(sleepUs);
    }

    if (fallbackTimeoutUs > 0) {
        currentState = ecx_statecheck(context, 0, targetState, fallbackTimeoutUs);
    }

    return currentState;
}

int WaitForStateWithPolling(
    ecx_contextt* context,
    uint16 targetState,
    int maxRounds,
    DWORD sleepMs,
    int pollTimeoutUs,
    int fallbackTimeoutUs)
{
    int currentState = 0;
    for (int round = 0; round < maxRounds; ++round) {
        Sleep(sleepMs);
        currentState = ecx_statecheck(context, 0, targetState, pollTimeoutUs);
        if (HasReachedState(currentState, targetState)) {
            return currentState;
        }
    }

    if (fallbackTimeoutUs > 0) {
        currentState = ecx_statecheck(context, 0, targetState, fallbackTimeoutUs);
    }

    return currentState;
}

// ---------------------------------------------------------------------------
// ESI-file cache and lookup by (vendorId, productCode)
// ---------------------------------------------------------------------------

static std::unordered_map<uint64_t, std::string> s_esiPathCache;
static std::mutex s_esiCacheMutex;
static bool s_esiCacheScanned = false;

static std::wstring GetEsiDirectoryPath()
{
    wchar_t exePath[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
        return L".\\esi";
    }
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    return std::wstring(exePath) + L"\\esi";
}

static void EnsureEsiCacheScanned()
{
    if (s_esiCacheScanned) return;
    std::lock_guard<std::mutex> lock(s_esiCacheMutex);
    if (s_esiCacheScanned) return;

    std::wstring esiDir = GetEsiDirectoryPath();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(esiDir, ec)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".xml") != 0) continue;

        std::string path = CoreHelpers::WideToUtf8(entry.path().wstring());
        EsiDeviceIdentity id;
        std::string err;
        if (EsiParser::LoadDeviceIdentity(path, id, err) && id.valid) {
            uint64_t key = (static_cast<uint64_t>(id.vendorId) << 32) | id.productCode;
            s_esiPathCache[key] = path;
        }
    }
    s_esiCacheScanned = true;
}

static std::string FindEsiFileForSlave(uint32_t vendorId, uint32_t productCode)
{
    EnsureEsiCacheScanned();
    uint64_t key = (static_cast<uint64_t>(vendorId) << 32) | productCode;
    std::lock_guard<std::mutex> lock(s_esiCacheMutex);
    auto it = s_esiPathCache.find(key);
    return (it != s_esiPathCache.end()) ? it->second : std::string();
}

// ESI-aware PDO index selection: returns the ESI-preferred RxPDO index for SM2,
// or the CiA402-standard 0x1600 when no ESI match is found.
static uint16_t SelectRxPdoIndex(const EsiSmDescriptor* desc)
{
    if (!desc) return 0x1600;
    // Prefer the RxPdo explicitly marked Sm="2"
    for (const auto& rp : desc->rxPdos) {
        if (rp.smIndex == 2 && !rp.fixed) return rp.index;
    }
    // Fall back to first non-fixed RxPdo
    for (const auto& rp : desc->rxPdos) {
        if (!rp.fixed) return rp.index;
    }
    return 0x1600;
}

static uint16_t SelectTxPdoIndex(const EsiSmDescriptor* desc)
{
    if (!desc) return 0x1A00;
    // Prefer the TxPdo explicitly marked Sm="3"
    for (const auto& tp : desc->txPdos) {
        if (tp.smIndex == 3 && !tp.fixed) return tp.index;
    }
    // Fall back to first non-fixed TxPdo
    for (const auto& tp : desc->txPdos) {
        if (!tp.fixed) return tp.index;
    }
    return 0x1A00;
}

// Look up SM DefaultSize from ESI descriptor; returns 0 when unknown
// (0 = dynamic size, SOEM will calculate from PDO mapping)
static uint16_t GetSmDefaultSize(const EsiSmDescriptor* desc, int smIndex)
{
    if (!desc) return 0;
    for (const auto& sm : desc->smList) {
        if (sm.index == smIndex) return sm.defaultSize;
    }
    return 0;
}

// Check if the drive's SM can support a given PDO byte size.
// Returns true if DefaultSize==0 (dynamic/auto-sized), or >= required bytes.
static bool CanSmFitSize(const EsiSmDescriptor* desc, int smIndex, int requiredBytes)
{
    if (!desc) return false;
    for (const auto& sm : desc->smList) {
        if (sm.index != smIndex) continue;
        // DefaultSize==0 means dynamic: SOEM computes from PDO mapping → any size OK
        // DefaultSize>=required means fixed buffer is large enough
        return (sm.defaultSize == 0 || sm.defaultSize >= requiredBytes);
    }
    return false;  // SM not found → assume safe (use small PDO)
}

} // anonymous namespace

bool EtherCATMaster::Initialize(const char* ifname_param) {
    EnterCriticalSection(&cs);

    if (ifname_param && IsSimulationInterfaceName(ifname_param)) {
        const bool ok = InitializeSimulationLocked(ifname_param);
        LeaveCriticalSection(&cs);
        return ok;
    }

    if (!IsPhysicalEthercatAllowed()) {
        std::string reason = GetPhysicalEthercatRestrictionReason();
        if (reason.empty()) reason = "platform policy";
        SetError("Physical EtherCAT communication disabled: " + reason);
        LeaveCriticalSection(&cs);
        return false;
    }
    
    // 如果已經初始化，先進行完整清理以確保網卡處於乾淨狀態
    // 這對於處理「從站後送電」的情況非常重要
    if (isInitialized) {
        SetError("Re-initializing: cleaning up previous state...");
        
        // 關閉之前的 EtherCAT 連接
        ecx_close(&context);
        
        // 清除狀態
        isInitialized = false;
        slaveCount = 0;
        memset(&context, 0, sizeof(context));
        
        // 短暫延遲以確保網卡完全釋放
        LeaveCriticalSection(&cs);
        Sleep(100);  // 100ms 延遲讓網卡完全重置
        EnterCriticalSection(&cs);
        
        SetError("Previous state cleaned, proceeding with initialization...");
    }

    strncpy_s(ifname, ifname_param, sizeof(ifname) - 1);
    
    // 初始化 SOEM Context (新版 API)
    // 添加重試機制以應對網卡瞬態故障
    int initRetries = 3;
    int initResult = 0;
    for (int i = 0; i < initRetries; i++) {
        initResult = ecx_init(&context, ifname);
        if (initResult > 0) {
            break;  // 成功
        }
        
        // 失敗時短暫延遲後重試
        if (i < initRetries - 1) {
            std::stringstream retryMsg;
            retryMsg << "ecx_init attempt " << (i+1) << " failed, retrying...";
            SetError(retryMsg.str());
            
            LeaveCriticalSection(&cs);
            Sleep(200);  // 200ms 延遲
            EnterCriticalSection(&cs);
        }
    }
    
    if (initResult <= 0) {
        SetError("Failed to initialize EtherCAT on " + std::string(ifname) + 
                 " after " + std::to_string(initRetries) + " attempts (ecx_init returned <= 0)");
        LeaveCriticalSection(&cs);
        return false;
    }

    // Apply Npcap immediate-mode optimizations to the opened pcap handle
    // to reduce kernel→userspace packet delivery latency (~1ms savings).
    // Only applies to the user-selected EtherCAT NIC — never touches other NICs.
    {
        std::string npcapLog;
        CoreHelpers::NpcapApplyImmediateMode(context.port.sockhandle, npcapLog);
        (void)npcapLog; // logged via SetError / debug output in production builds
    }

    // Disable RSS on the selected EtherCAT adapter to eliminate cross-core
    // interrupt jitter that degrades PDO cycle stability.
    {
        std::string rssLog;
        // Resolve pcap ifname → Windows friendly name for RSS control
        std::string adapterFriendlyName;
        {
            std::string enumErr;
            auto adapters = CoreHelpers::EnumerateAdapters(enumErr);
            for (const auto& ad : adapters) {
                if (ad.pcapName == ifname) {
                    if (!ad.friendlyName.empty()) {
                        adapterFriendlyName = ad.friendlyName;
                    } else if (!ad.description.empty()) {
                        adapterFriendlyName = ad.description;
                    }
                    break;
                }
            }
        }
        if (!adapterFriendlyName.empty()) {
            CoreHelpers::DisableAdapterRss(adapterFriendlyName, rssLog);
        } else {
            rssLog = "[RSS] Could not resolve adapter friendly name from pcap ifname — skipping RSS disable\n";
        }
        (void)rssLog; // logged via SetError in production builds
    }

    // 尋找並配置從站
    // 同樣添加重試機制
    int configRetries = 2;
    int configResult = 0;
    for (int i = 0; i < configRetries; i++) {
        configResult = ecx_config_init(&context);
        if (configResult > 0) {
            break;  // 成功
        }
        
        // 失敗時短暫延遲後重試
        if (i < configRetries - 1) {
            std::stringstream retryMsg;
            retryMsg << "ecx_config_init attempt " << (i+1) << " failed, retrying...";
            SetError(retryMsg.str());
            
            LeaveCriticalSection(&cs);
            Sleep(200);  // 200ms 延遲
            EnterCriticalSection(&cs);
        }
    }
    
    if (configResult <= 0) {
        SetError("No slaves found on " + std::string(ifname) + 
                 " after " + std::to_string(configRetries) + " attempts (ecx_config_init returned <= 0)");
        ecx_close(&context);
        LeaveCriticalSection(&cs);
        return false;
    }

    slaveCount = context.slavecount;

    // Initialize lock-free soft-start ready cache (index by slave, 1-based)
    try {
        softStartReadyCache_.reset();
        if (slaveCount >= 0) {
            softStartReadyCache_.reset(new std::atomic<char>[slaveCount + 1]);
            softStartReadyCacheSize_ = slaveCount + 1;
            for (int i = 0; i < softStartReadyCacheSize_; ++i) softStartReadyCache_[i].store(0, std::memory_order_relaxed);
        } else {
            softStartReadyCacheSize_ = 0;
        }
    } catch (...) {
        // swallow - non-fatal
        softStartReadyCacheSize_ = 0;
    }

    // Allocate per-slave PDO caches (0x603F Error Code, 0x6061 Mode Display)
    // Updated every cycle by CyclicTask; read lock-free from any thread.
    // Set count=0 first so concurrent GetPdo603F/GetPdo6061 return false during allocation.
    pdoCacheSlaveCount_.store(0, std::memory_order_release);
    try {
        pdo603FCache_.reset(new std::atomic<uint16_t>[slaveCount + 1]);
        pdo6061Cache_.reset(new std::atomic<uint8_t>[slaveCount + 1]);
        for (int i = 0; i <= slaveCount; ++i) {
            pdo603FCache_[i].store(0, std::memory_order_relaxed);
            pdo6061Cache_[i].store(0, std::memory_order_relaxed);
        }
        pdoCacheSlaveCount_.store(slaveCount, std::memory_order_release);
    } catch (...) {
        pdoCacheSlaveCount_.store(0, std::memory_order_release);
    }
    
    std::stringstream ss;
    ss << "Found " << slaveCount << " slaves";
    SetError(ss.str());
    
    isInitialized = true;

    // Auto-tune enabled by default; compute sensible default threshold from desired cycle time
    // Use half the desired cycle as threshold but no less than 200us
    uint32_t halfCycle = (desiredCycleTime > 0) ? (desiredCycleTime / 2) : 200;
    autoTuneJitterThreshold = (halfCycle > 200) ? halfCycle : 200;
    autoTuneIntervalMs = 5000; // default 5 seconds between tuning attempts

    // Real-time optimization: Lock critical memory pages to prevent paging to disk (SSD interference mitigation)
    // This prevents IOmap from being swapped to disk, which would cause severe latency spikes
    if (VirtualLock(IOmap, sizeof(IOmap))) {
        ss.str("");
        ss << "RT-Optimization: IOmap memory locked (" << sizeof(IOmap) << " bytes) - preventing disk paging";
        SetError(ss.str());
    } else {
        DWORD lockError = ::GetLastError();
        ss.str("");
        ss << "Warning: Failed to lock IOmap memory (error: " << lockError << ") - may experience paging delays";
        SetError(ss.str());
        // Non-fatal: continue initialization even if lock fails (may require elevated privileges)
    }

    // Increase process working set to prevent excessive paging during real-time operation
    // This reserves physical memory for the process, reducing page faults and SSD I/O
    SIZE_T minWorkingSetSize = 64 * 1024 * 1024;   // 64 MB minimum
    SIZE_T maxWorkingSetSize = 256 * 1024 * 1024;  // 256 MB maximum
    if (SetProcessWorkingSetSize(GetCurrentProcess(), minWorkingSetSize, maxWorkingSetSize)) {
        ss.str("");
        ss << "RT-Optimization: Process working set configured (min: " << (minWorkingSetSize / (1024*1024)) 
           << " MB, max: " << (maxWorkingSetSize / (1024*1024)) << " MB)";
        SetError(ss.str());
    } else {
        DWORD wsError = ::GetLastError();
        ss.str("");
        ss << "Info: Working set size adjustment not applied (error: " << wsError << ") - using default memory management";
        SetError(ss.str());
        // Non-fatal: Windows may deny this request on some systems
    }

    LeaveCriticalSection(&cs);
    return true;
}


// Lightweight warm‑up routine; used during application startup to load
// network/driver libraries and exercise the interface once.  Does not
// modify the state of any previously initialized master object.
bool EtherCATMaster::PreloadInterface(const char* ifname) {
    if (!ifname || *ifname == '\0') return false;
    if (IsSimulationAdapterSelection(ifname) || !IsPhysicalEthercatAllowed()) return false;

    ecx_contextt tempCtx;
    memset(&tempCtx, 0, sizeof(tempCtx));

    if (ecx_init(&tempCtx, ifname) <= 0) {
        return false;
    }

    // send a few dummy frames so that the driver completes its first
    // transmission and any FIFOs are primed.  We deliberately avoid
    // running ecx_config_init in case there are no slaves attached.
    for (int i = 0; i < 5; ++i) {
        ecx_send_processdata(&tempCtx);
        ecx_receive_processdata(&tempCtx, EC_TIMEOUTRET);
        osal_usleep(1000);
    }

    ecx_close(&tempCtx);
    return true;
}


void EtherCATMaster::Shutdown() {
    Shutdown(nullptr);
}


void EtherCATMaster::Shutdown(std::function<void(const std::string&)> logCallback) {
    auto log = [&](const std::string& msg) {
        if (logCallback) logCallback(msg);
        printf("%s", msg.c_str());
    };

    EnterCriticalSection(&cs);
    const bool simulationMode = simulationMode_.load(std::memory_order_relaxed);
    LeaveCriticalSection(&cs);

    if (simulationMode) {
        ShutdownSimulationLocked(logCallback);
        return;
    }

    // Check if the physical link is already down.  When the cable is
    // unplugged every ecx_statecheck() blocks for EC_TIMEOUTSTATE (2 s);
    // skipping futile state transitions shrinks shutdown from ~6 s to
    // < 1 s and avoids the UI appearing hung during auto-disconnect.
    bool linkIsDown = false;
    {
        EnterCriticalSection(&cs);
        if (isInitialized && ifname[0] != '\0') {
            LeaveCriticalSection(&cs);
            try {
                linkIsDown = !CoreHelpers::CheckLinkStatus(ifname);
            } catch (...) {
                linkIsDown = true;   // assume down on any error
            }
        } else {
            LeaveCriticalSection(&cs);
        }
    }

    if (!linkIsDown) {

    // Step 1: If cyclic is running, first request SAFE_OP while processdata still flowing
    EnterCriticalSection(&cs);
    if (isRunning && isInitialized) {
        log("[退出流程] OP -> SAFE_OP 轉換中...\n");
        context.slavelist[0].state = EC_STATE_SAFE_OP;
        ecx_writestate(&context, 0);
        int chk = ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
        if (chk == EC_STATE_SAFE_OP) {
            log("[退出流程] [OK] 已轉換至 SAFE_OP\n");
        } else {
            char buf[128];
            sprintf_s(buf, "[退出流程] [WARN] SAFE_OP 轉換未完全成功 (狀態=0x%x)\n", chk);
            log(buf);
        }
    }
    LeaveCriticalSection(&cs);

    } else {
        log("[退出流程] 實體鏈路已斷開，略過 OP→SAFE_OP 狀態轉換\n");
    }
    
    // Step 2: Stop cyclic thread (no more processdata)
    log("[退出流程] 停止週期性資料交換...\n");
    StopCyclicOperation();
    log("[退出流程] [OK] 週期性執行緒已停止\n");

    // Step 2.5: Stop PDO processing thread
    log("[退出流程] 停止 PDO 處理執行緒...\n");
    StopPDOProcessingThread();
    log("[退出流程] [OK] PDO 處理執行緒已停止\n");

    // Step 2.55: Stop SDO reader (prevent background SDO access during teardown)
    log("[退出流程] 停止 SDO 讀取器...\n");
    StopSdoReader();
    log("[退出流程] [OK] SDO 讀取器已停止\n");

    // Step 2.56: Release EMCY cache and clear callbacks (no more mailbox activity)
    emcyCache_.reset();
    {
        EnterCriticalSection(&emcyCS_);
        emcyCallbacks_.clear();
        globalEmcyCallback_ = nullptr;
        LeaveCriticalSection(&emcyCS_);
    }
    log("[退出流程] [OK] EMCY 回呼已清除\n");

    // Step 2.6: Stop MMF reader/writer (ensure no MMF access while tearing down)
    log("[退出流程] 停止 MMF 輸出/輸入 (if enabled)...\n");
    try {
        // Stop MMF receiver thread first (non-RT, polls MMF independently)
        StopMMFReceiver();
        // Stop MMF input reader if running
        StopMMF1In();
        // Disable MMF output (will Close mapping under mmfOutputCS_)
        DisableMMFOutput();
        // Disable IK actual pose MMF output (BigCat)
        DisablePoseMMFOutput();
    } catch (...) {
        // Best-effort: do not let MMF cleanup failure prevent shutdown sequence
    }
    log("[退出流程] [OK] MMF 輸出/輸入已停止\n");

    // If caller provided a UI log callback, signal UI to stop timer
    if (logCallback) logCallback(std::string("STOP_TIMER\n"));

    if (!linkIsDown) {
    
    // Step 3: Now safely transition slaves from SAFE_OP to INIT
    log("[退出流程] 開始狀態轉換: SAFE_OP → PRE_OP → INIT\n");
    
    EnterCriticalSection(&cs);
    if (isInitialized) {
        ecx_readstate(&context);
        int currentState = context.slavelist[0].state;
        
        // If in OP, go to SAFE-OP first
        if ((currentState & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL) {
            log("[退出流程]   -> OP -> SAFE_OP...\n");
            context.slavelist[0].state = EC_STATE_SAFE_OP;
            ecx_writestate(&context, 0);
            int chk = ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
            if (chk == EC_STATE_SAFE_OP) {
                log("[退出流程]     [OK] SAFE_OP\n");
            }
        }
        
        // SAFE_OP to PRE_OP
        log("[退出流程]   -> SAFE_OP -> PRE_OP...\n");
        context.slavelist[0].state = EC_STATE_PRE_OP;
        ecx_writestate(&context, 0);
        int chk2 = ecx_statecheck(&context, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
        if (chk2 == EC_STATE_PRE_OP) {
            log("[退出流程]     [OK] PRE_OP\n");
        }
        
        // PRE_OP to INIT
        log("[退出流程]   -> PRE_OP -> INIT...\n");
        context.slavelist[0].state = EC_STATE_INIT;
        ecx_writestate(&context, 0);
        int chk3 = ecx_statecheck(&context, 0, EC_STATE_INIT, EC_TIMEOUTSTATE);
        if (chk3 == EC_STATE_INIT) {
            log("[退出流程]     [OK] INIT\n");
        }
        
        log("[退出流程] [OK] 所有裝置已回到 INIT 狀態\n");
    }
    LeaveCriticalSection(&cs);

    } else {
        log("[退出流程] 實體鏈路已斷開，略過 SAFE_OP→PRE_OP→INIT 狀態轉換\n");
    }
    
    // Step 4: Close EtherCAT port
    EnterCriticalSection(&cs);
    if (isInitialized) {
    log("[退出流程] 關閉 EtherCAT 連接埠...\n");
    ecx_close(&context);
    isInitialized = false;
    slaveCount = 0;
    // Reset PDO cache state to prevent stale access after disconnect
    pdoCacheSlaveCount_.store(0, std::memory_order_release);
    pdo603FCache_.reset();
    pdo6061Cache_.reset();
    pdo60FDValid_.store(false, std::memory_order_release);
    log("[退出流程] [OK] 連接埠已關閉\n");
    }
    LeaveCriticalSection(&cs);
}


void EtherCATMaster::StopAllSlaves() {
    // This function is now deprecated - state transitions are handled in Shutdown()
    // Keeping for backward compatibility
    EnterCriticalSection(&cs);
    if (!isInitialized) {
        LeaveCriticalSection(&cs);
        return;
    }
    
    // Read current state to determine starting point
    ecx_readstate(&context);
    int currentState = context.slavelist[0].state;
    
    // Step 1: If in OP, go to SAFE-OP first
    if ((currentState & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL) {
        context.slavelist[0].state = EC_STATE_SAFE_OP;
        ecx_writestate(&context, 0);
        ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    }
    
    // Step 2: Request all slaves to go to PRE-OP
    context.slavelist[0].state = EC_STATE_PRE_OP;
    ecx_writestate(&context, 0);
    ecx_statecheck(&context, 0, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
    
    // Step 3: Request all slaves to go to INIT (initial state)
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0);
    ecx_statecheck(&context, 0, EC_STATE_INIT, EC_TIMEOUTSTATE);
    
    LeaveCriticalSection(&cs);
}


bool EtherCATMaster::ConfigureSlaves() {
    EnterCriticalSection(&cs);

    if (simulationMode_.load(std::memory_order_relaxed)) {
        const bool ok = ConfigureSimulationSlavesLocked();
        LeaveCriticalSection(&cs);
        return ok;
    }
    
    if (!isInitialized) {
        SetError("Not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 特殊配置方案，處理PDO配置問題
    std::stringstream ss;
    
    // 1. 完全初始化 - 將所有從站回到INIT狀態
    ss << "將所有從站重置為INIT狀態...\n";
    SetError(ss.str());
    
    // 首先清除所有可能存在的錯誤狀態
    for (int i = 1; i <= context.slavecount; i++) {
        // 無論當前狀態如何，都發送錯誤確認+INIT請求
        uint16_t alControl = 0x0011; // Error ACK (bit 4) + INIT state (0x01)
        ecx_FPWR(&context.port, context.slavelist[i].configadr, 
                ECT_REG_ALCTL, sizeof(alControl), &alControl, EC_TIMEOUTRET);
    }
    
    // 等待所有從站回到INIT狀態（使用自訂輪詢替代阻塞 ecx_statecheck）
    ss.str("");
    ss << "等待所有從站回到 INIT...\n";
    SetError(ss.str());
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0);

    LeaveCriticalSection(&cs);
    {
        const bool preInitStable = PumpProcessDataUntilStable(&context,
            kStableProcessDataMaxRounds, kStableProcessDataSleepUs);
        if (!preInitStable) {
            SetError("Warning: processdata did not stabilize before INIT request");
        }
        int initChk = WaitForStateWithProcessData(&context,
            EC_STATE_INIT, kSafeOpPollRounds, kSafeOpPollTimeoutUs,
            kSafeOpPollSleepUs, kSafeOpFallbackTimeoutUs);
        ss.str("");
        ss << "INIT 檢查結果: 0x" << std::hex << initChk << std::dec << "\n";
        SetError(ss.str());
    }
    EnterCriticalSection(&cs);
    
    // 2. 重新自動發現從站
    ss.str("");
    ss << "重新自動發現從站...\n";
    SetError(ss.str());
    
    if (ecx_config_init(&context) <= 0) {
        ss.str("");
        ss << "自動發現失敗: 找不到從站\n";
        SetError(ss.str());
        LeaveCriticalSection(&cs);
        return false;
    }
    
    ss.str("");
    ss << "找到 " << context.slavecount << " 台從站\n";
    SetError(ss.str());
    
    // 3. 顯示從站信息
    ss.str("");
    ss << "從站詳細資訊:\n";
    for (int i = 1; i <= context.slavecount; i++) {
        ss << "從站 " << i << ": " << context.slavelist[i].name 
           << " 製造商=0x" << std::hex << context.slavelist[i].eep_man << std::dec
           << " 產品代碼=0x" << std::hex << context.slavelist[i].eep_id << std::dec
           << " 修訂版本=0x" << std::hex << context.slavelist[i].eep_rev << std::dec
           << "\n";
    }
    SetError(ss.str());
    
    // 4. 進行手動PDO配置 (配置伺服驅動器的通用PDO映射)
    ss.str("");
    ss << "為從站執行手動PDO配置...\n";
    SetError(ss.str());
    
    bool configFailed = false;
    std::stringstream configErrors;
    
    for (int i = 1; i <= context.slavecount; i++) {
        // 對每個從站執行以下操作:
        // 1. 先移動到PRE-OP狀態以允許SDO操作（自訂輪詢替代阻塞 ecx_statecheck）
        context.slavelist[i].state = EC_STATE_PRE_OP;
        ecx_writestate(&context, i);

        // Release cs during state-polling to avoid blocking other threads
        LeaveCriticalSection(&cs);
        {
            // Warm up processdata before checking state
            for (int r = 0; r < 3; ++r) {
                ecx_send_processdata(&context);
                ecx_receive_processdata(&context, EC_TIMEOUTRET);
                osal_usleep(3000);
            }
            // Poll for PRE_OP with short timeout (max ~150ms vs 2s)
            int preopState = 0;
            for (int r = 0; r < 20; ++r) {
                ecx_send_processdata(&context);
                ecx_receive_processdata(&context, EC_TIMEOUTRET);
                preopState = ecx_statecheck(&context, i, EC_STATE_PRE_OP, 5000);
                if ((preopState & EC_STATE_PRE_OP) == EC_STATE_PRE_OP) break;
                osal_usleep(3000);
            }
            if ((preopState & EC_STATE_PRE_OP) != EC_STATE_PRE_OP) {
                // Fallback: use original timeout
                preopState = ecx_statecheck(&context, i, EC_STATE_PRE_OP, EC_TIMEOUTSTATE);
            }
            EnterCriticalSection(&cs);
            if ((preopState & EC_STATE_PRE_OP) != EC_STATE_PRE_OP) {
                configFailed = true;
                configErrors << "從站 " << i << " 無法達到PRE-OP狀態\n";
                continue;
            }
        }

        // ---- ESI-aware PDO index selection ----
        uint32_t slaveVendorId = context.slavelist[i].eep_man;
        uint32_t slaveProductCode = context.slavelist[i].eep_id;
        std::string esiPath = FindEsiFileForSlave(slaveVendorId, slaveProductCode);

        EsiSmDescriptor esiDesc;
        const EsiSmDescriptor* pEsi = nullptr;
        if (!esiPath.empty()) {
            std::string esiErr;
            if (EsiParser::LoadSmDescriptor(esiPath, esiDesc, esiErr)) {
                pEsi = &esiDesc;
                // Store matched ESI filename and device name per slave (1-indexed)
                if ((int)slaveEsiFiles_.size() <= i) {
                    slaveEsiFiles_.resize(i + 1);
                    slaveEsiDeviceNames_.resize(i + 1);
                }
                std::filesystem::path p(esiPath);
                slaveEsiFiles_[i] = p.filename().string();
                slaveEsiDeviceNames_[i] = esiDesc.deviceName;
                ss.str("");
                ss << "從站 " << i << ": ESI 匹配 " << esiPath
                   << " (vendor=0x" << std::hex << slaveVendorId
                   << " product=0x" << slaveProductCode << std::dec << ")\n";
                SetError(ss.str());
            }
        }

        uint16_t rxPdoIndex = SelectRxPdoIndex(pEsi);
        uint16_t txPdoIndex = SelectTxPdoIndex(pEsi);
        uint16_t sm2Size = GetSmDefaultSize(pEsi, 2);
        uint16_t sm3Size = GetSmDefaultSize(pEsi, 3);

        // X1S (Renesas ESC, vid=0x766 pid=0x10000): TwinCAT uses 0x1600/0x1A00,
        // not the ESI-preferred 0x1601/0x1A01.  Force the TwinCAT-compatible
        // indices so the 7-entry PDO works correctly on this drive.
        if (context.slavelist[i].eep_man == 0x766 &&
            context.slavelist[i].eep_id  == 0x10000) {
            rxPdoIndex = 0x1600;
            txPdoIndex = 0x1A00;
        }

        if (pEsi) {
            ss.str("");
            ss << "從站 " << i << ": RxPDO=0x" << std::hex << rxPdoIndex
               << " TxPDO=0x" << txPdoIndex
               << " SM2_size=" << std::dec << sm2Size
               << " SM3_size=" << sm3Size << "\n";
            SetError(ss.str());
        }

        // 2. 清空舊的PDO映射
        uint8_t zero = 0;
        uint8_t one = 1;

        // 停用RxPDO分配 (0x1C12:00 = 0)
    if (!WriteSDO(i, 0x1C12, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法停用RxPDO分配\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }

        // 停用TxPDO分配 (0x1C13:00 = 0)
    if (!WriteSDO(i, 0x1C13, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法停用TxPDO分配\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }

        // 3. 設置 cyclic motion RxPDO 映射（固定 7-entry，與 Par/Startup.csv 一致）
        constexpr int rxEntryCount = 7;
        constexpr int txEntryCount = 7;

        ss.str("");
        ss << "從站 " << i << ": 使用 7-entry PDO (Rx=17B Tx=19B, Startup.csv)\n";
        SetError(ss.str());

        // 清空舊映射
    if (!WriteSDO(i, rxPdoIndex, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空RxPDO映射 (0x" << std::hex << rxPdoIndex << std::dec << ")\n";
            SetError(ss.str());
        }

        // RxPDO entries: 7-entry (order matches Par/Startup.csv)
        // X1S (Renesas ESC) uses 0x2019/0x201A for torque limits;
        // DVS uses 0x60E0/0x60E1.  Byte layout is identical.
        uint32_t rxEntries[7];
        int rxIdx = 0;
        rxEntries[rxIdx++] = 0x607A0020;  // 1. 目標位置 (32位)
        rxEntries[rxIdx++] = 0x60FF0020;  // 2. 目標速度 (32位)
        rxEntries[rxIdx++] = 0x60710010;  // 3. 目標力矩 (16位)
        const bool isX1s = EtherCATMaster::IsX1sDrive(context.slavelist[i].eep_man);
        rxEntries[rxIdx++] = isX1s ? 0x20190010 : 0x60E00010;  // 4. 正轉矩限制 (16位)
        rxEntries[rxIdx++] = isX1s ? 0x201A0010 : 0x60E10010;  // 5. 負轉矩限制 (16位)
        rxEntries[rxIdx++] = 0x60400010;  // 6. 控制字 (16位)
        rxEntries[rxIdx++] = 0x60600008;  // 7. 操作模式 (8位)

        for (int j = 0; j < rxEntryCount; j++) {
            if (!WriteSDO(i, rxPdoIndex, j+1, &rxEntries[j], sizeof(uint32_t))) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置RxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }

        uint8_t rxCount = static_cast<uint8_t>(rxEntryCount);
    if (!WriteSDO(i, rxPdoIndex, 0, &rxCount, sizeof(rxCount))) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用RxPDO映射\n";
            SetError(ss.str());
        }

        // 5. TxPDO 映射
        // 清空舊映射
    if (!WriteSDO(i, txPdoIndex, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空TxPDO映射 (0x" << std::hex << txPdoIndex << std::dec << ")\n";
            SetError(ss.str());
        }

        // TxPDO entries: 7-entry (order matches Par/Startup.csv)
        uint32_t txEntries[7];
        int txIdx = 0;
        txEntries[txIdx++] = 0x60640020;  // 1. 實際位置 (32位)
        txEntries[txIdx++] = 0x606C0020;  // 2. 實際速度 (32位)
        txEntries[txIdx++] = 0x60770010;  // 3. 實際力矩 (16位)
        txEntries[txIdx++] = 0x60410010;  // 4. 狀態字 (16位)
        txEntries[txIdx++] = 0x603F0010;  // 5. 錯誤碼 (16位)
        txEntries[txIdx++] = 0x60610008;  // 6. 操作模式顯示 (8位)
        txEntries[txIdx++] = 0x60FD0020;  // 7. 數位輸入 (32位)

        for (int j = 0; j < txEntryCount; j++) {
            if (!WriteSDO(i, txPdoIndex, j+1, &txEntries[j], sizeof(uint32_t))) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置TxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }

        uint8_t txCount = static_cast<uint8_t>(txEntryCount);
    if (!WriteSDO(i, txPdoIndex, 0, &txCount, sizeof(txCount))) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用TxPDO映射\n";
            SetError(ss.str());
        }

        // 6. 分配PDO到同步管理器
        // 分配RxPDO到SM2
    if (!WriteSDO(i, 0x1C12, 1, &rxPdoIndex, sizeof(rxPdoIndex))) {
            ss.str("");
            ss << "從站 " << i << ": 無法分配RxPDO到同步管理器\n";
            SetError(ss.str());
        }

        // 啟用1個RxPDO
    if (!WriteSDO(i, 0x1C12, 0, &one, sizeof(one))) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用RxPDO分配\n";
            SetError(ss.str());
        }

        // 分配TxPDO到SM3
    if (!WriteSDO(i, 0x1C13, 1, &txPdoIndex, sizeof(txPdoIndex))) {
            ss.str("");
            ss << "從站 " << i << ": 無法分配TxPDO到同步管理器\n";
            SetError(ss.str());
        }

        // 啟用1個TxPDO
    if (!WriteSDO(i, 0x1C13, 0, &one, sizeof(one))) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用TxPDO分配\n";
            SetError(ss.str());
        }
        
        ss.str("");
        ss << "從站 " << i << " PDO配置完成\n";
        SetError(ss.str());

        // Quick count-only verification — per-entry readback skipped for speed.
        // Per-entry integrity is guaranteed by WriteAndVerifySDO on the critical
        // entries (controlword, op-mode) later in the connect flow.
        {
            ss.str("");
            ss << "從站 " << i << " PDO驗證: ";
            bool ok = true;

            uint8_t rxCountVer = 0;
            int sz = sizeof(rxCountVer);
            if (ReadSDO((uint16_t)i, rxPdoIndex, 0x00, &rxCountVer, sz) && rxCountVer == rxEntryCount) {
                ss << "Rx=" << (int)rxCountVer << " ";
            } else {
                ss << "Rx count=" << (int)rxCountVer << " (expected " << rxEntryCount << ") ";
                ok = false;
            }

            uint8_t txCountVer = 0;
            sz = sizeof(txCountVer);
            if (ReadSDO((uint16_t)i, txPdoIndex, 0x00, &txCountVer, sz) && txCountVer == txEntryCount) {
                ss << "Tx=" << (int)txCountVer << " ";
            } else {
                ss << "Tx count=" << (int)txCountVer << " (expected " << txEntryCount << ") ";
                ok = false;
            }

            if (ok) ss << "OK";
            SetError(ss.str());
        }
    }
    if (configFailed) {
        // 彙總配置錯誤
        SetError(configErrors.str());
    }
    
    // 4.5 修正 SM2/SM3 大小以符合 7-entry PDO 需求 (Rx=17B/Tx=19B)。
    {
        constexpr uint16_t kReqSM2 = 17;
        constexpr uint16_t kReqSM3 = 19;
        for (int i = 1; i <= context.slavecount; i++) {
            uint16_t curSM2 = etohs(context.slavelist[i].SM[2].SMlength);
            if (curSM2 < kReqSM2) {
                context.slavelist[i].SM[2].SMlength = htoes(kReqSM2);
            }
            uint16_t curSM3 = etohs(context.slavelist[i].SM[3].SMlength);
            if (curSM3 < kReqSM3) {
                context.slavelist[i].SM[3].SMlength = htoes(kReqSM3);
            }
        }
    }
    
    // 5. 配置IOMap
    ss.str("");
    ss << "重新映射IOMap...\n";
    SetError(ss.str());
    
    int ioMapResult = ecx_config_map_group(&context, IOmap, 0);
    ss.str("");
    ss << "映射結果: " << ioMapResult << " bytes\n";

    // Dump per-slave IOmap/FMMU after mapping for diagnostics
    {
        std::stringstream fmmuLog;
        fmmuLog << "[FMMU] IOmap/FMMU after ecx_config_map_group:\n";
        for (int i = 1; i <= context.slavecount; i++) {
            auto& sl = context.slavelist[i];
            fmmuLog << "[FMMU]   Slave " << i << ": Obytes=" << sl.Obytes << " Ibytes=" << sl.Ibytes
                    << " Obits=" << sl.Obits << " Ibits=" << sl.Ibits
                    << " SM2=" << etohs(sl.SM[2].SMlength) << "B SM3=" << etohs(sl.SM[3].SMlength) << "B";
            for (int f = 0; f < EC_MAXFMMU; f++) {
                if (sl.FMMU[f].FMMUactive) {
                    fmmuLog << " FMMU" << f << ":type=" << (int)sl.FMMU[f].FMMUtype
                            << " Log=0x" << std::hex << etohl(sl.FMMU[f].LogStart)
                            << "+" << std::dec << etohs(sl.FMMU[f].LogLength)
                            << " Phys=0x" << std::hex << etohs(sl.FMMU[f].PhysStart) << std::dec;
                }
            }
            fmmuLog << " out=" << (void*)sl.outputs << " in=" << (void*)sl.inputs << "\n";
        }
        OutputDebugStringA(fmmuLog.str().c_str());
    }

    // Sync slave count and re-allocate PDO caches after re-mapping.
    // CRITICAL: set pdoCacheSlaveCount_=0 FIRST so concurrent GetPdo603F/GetPdo6061
    // return false instead of accessing freed memory during the reset window.
    slaveCount = context.slavecount;
    pdoCacheSlaveCount_.store(0, std::memory_order_release);  // gate concurrent readers BEFORE freeing old arrays
    try {
        pdo603FCache_.reset(new std::atomic<uint16_t>[slaveCount + 1]);
        pdo6061Cache_.reset(new std::atomic<uint8_t>[slaveCount + 1]);
        for (int i = 0; i <= slaveCount; ++i) {
            pdo603FCache_[i].store(0, std::memory_order_relaxed);
            pdo6061Cache_[i].store(0, std::memory_order_relaxed);
        }
        pdoCacheSlaveCount_.store(slaveCount, std::memory_order_release);  // re-enable after arrays are ready
    } catch (...) {
        pdoCacheSlaveCount_.store(0, std::memory_order_release);
    }
    
    // 檢查每個從站的映射結果
    ss << "從站映射結果:\n";
    for (int i = 1; i <= context.slavecount; i++) {
        ss << "從站 " << i << ": " << context.slavelist[i].name 
           << " 輸出=" << context.slavelist[i].Obits 
           << " 位元, 輸入=" << context.slavelist[i].Ibits << " 位元"
           << " Ostartbit=" << context.slavelist[i].Ostartbit
           << " Istartbit=" << context.slavelist[i].Istartbit << "\n";
    }
    // Emit the detailed mapping info
    SetError(ss.str());

    // Quick check: if any startbit is zero, warn succinctly to draw attention
    bool anyZero = false;
    for (int i = 1; i <= context.slavecount; ++i) {
        if (context.slavelist[i].Ostartbit == 0 || context.slavelist[i].Istartbit == 0) {
            anyZero = true;
            break;
        }
    }
    if (anyZero) {
        std::stringstream ss2;
        ss2 << "WARNING: One or more slaves have Ostartbit/Istartbit == 0 after mapping.\n";
        SetError(ss2.str());
    }
    
    // 6. 配置分布式時鐘
    ss.str("");
    ss << "配置分布式時鐘...\n";
    SetError(ss.str());
    
    ecx_configdc(&context);

    // DC diagnostic
    {
        std::stringstream dclog;
        for (int i = 1; i <= context.slavecount; ++i) {
            dclog << "  Slave " << i << ": hasdc=" << (int)context.slavelist[i].hasdc
                  << " vendor=0x" << std::hex << context.slavelist[i].eep_man << std::dec << "\n";
        }
        OutputDebugStringA(dclog.str().c_str());
    }

    // 若從站支援 DC，預先啟用 SYNC0 以使其對齊總線時間
    // 使用期望週期（us）轉 ns；相位採用可配置百分比。
    //
    // BigCat.exe CyclicTask sends PDO mid-cycle (recv→work→send).  The frame
    // reaches slaves at ~40-50% of the cycle.  SYNC0 must fire AFTER the frame
    // arrives so slaves latch fresh outputs.  Default 60% leaves margin:
    //   frame arrives ~850us → SYNC0 at 1200us (60%) → 350us safety.
    // Win10 15% works because HR-timer jitter adds slack; Win11 TSC-spin is
    // precise enough to expose the misalignment as AL=0x001A.
    {
        uint32_t cycletime_ns = (desiredCycleTime > 0 ? desiredCycleTime : 2000) * 1000u;
        int pct = dcSyncPhasePercent;
        // 環境變數覆蓋：WUSOEM_SYNC0_PHASE_PCT
        {
            wchar_t buf[32] = {0};
            DWORD n = GetEnvironmentVariableW(L"WUSOEM_SYNC0_PHASE_PCT", buf, _countof(buf));
            if (n > 0 && n < _countof(buf)) {
                int v = _wtoi(buf);
                if (v >= 10 && v <= 90) pct = v;
            }
        }
        if (pct < 10) pct = 10; if (pct > 90) pct = 90;
        int32_t phaseShift = (int32_t)((cycletime_ns * (uint32_t)pct) / 100u);
        ecx_readstate(&context);
        for (int i = 1; i <= context.slavecount; ++i) {
            if (context.slavelist[i].hasdc) {
                // 啟用 SYNC0，SYNC1 關閉
                ecx_dcsync0(&context, i, TRUE, cycletime_ns, phaseShift);
            }
        }
    }
    
    // 6.5 X1S (Renesas ESC, vid=0x766) per-slave reconfig.
    // TwinCAT uses <PS> transition-period SDO writes for this drive; SOEM's
    // group broadcast ecx_writestate does not satisfy it.  ecx_reconfig_slave
    // replays INIT → SM program → PREOP → PO2SOconfig → SAFE_OP → FMMU for
    // each X1S, matching the TwinCAT behaviour that the ESC expects.
    {
        int x1sCount = 0;
        for (int i = 1; i <= context.slavecount; ++i) {
            if (context.slavelist[i].eep_man == 0x766 &&
                context.slavelist[i].eep_id  == 0x10000) {
                ++x1sCount;
                ss.str("");
                ss << "X1S slave " << i << ": per-slave reconfig INIT→SAFE_OP ...\n";
                SetError(ss.str());

                LeaveCriticalSection(&cs);
                int state = ecx_reconfig_slave(&context, i, EC_TIMEOUTSTATE * 3);
                EnterCriticalSection(&cs);

                const char* al = ec_ALstatuscode2string(context.slavelist[i].ALstatuscode);
                ss.str("");
                ss << "X1S slave " << i << " reconfig result: state=0x" << std::hex << state
                   << " AL=0x" << context.slavelist[i].ALstatuscode
                   << (al ? " " : "") << (al ? al : "") << std::dec << "\n";
                SetError(ss.str());

                if (!((state & EC_STATE_SAFE_OP) == EC_STATE_SAFE_OP)) {
                    ss.str("");
                    ss << "FATAL: X1S slave " << i << " did not reach SAFE_OP after reconfig\n";
                    SetError(ss.str());
                    LeaveCriticalSection(&cs);
                    return false;
                }
            }
        }
        if (x1sCount > 0) {
            ss.str("");
            ss << x1sCount << " X1S slave(s) reconfigured via per-slave path\n";
            SetError(ss.str());
        }
    }

    // 發送一些處理數據，確保從站處於穩定狀態（釋放鎖以避免長持有）
    ss.str("");
    ss << "發送處理數據循環以穩定從站狀態...\n";
    SetError(ss.str());
    LeaveCriticalSection(&cs);
    const bool preSafeOpStable = PumpProcessDataUntilStable(&context, kStableProcessDataMaxRounds, kStableProcessDataSleepUs);
    EnterCriticalSection(&cs);
    if (!preSafeOpStable) {
        SetError("Warning: processdata did not stabilize before SAFE_OP request");
    }
    
    // 7. 請求SAFE_OP狀態
    ss.str("");
    ss << "請求所有從站進入SAFE_OP狀態...\n";
    SetError(ss.str());
    
    context.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, 0);
    
    LeaveCriticalSection(&cs);
    int chk = WaitForStateWithProcessData(
        &context,
        EC_STATE_SAFE_OP,
        kSafeOpPollRounds,
        kSafeOpPollTimeoutUs,
        kSafeOpPollSleepUs,
        kSafeOpFallbackTimeoutUs);
    EnterCriticalSection(&cs);
    
    ss.str("");
    ss << "SAFE_OP檢查結果: 當前狀態=0x" << std::hex << chk << std::dec << "\n";
    SetError(ss.str());
    
    // 8. 再次發送處理數據以確保穩定（釋放鎖以避免長持有）
    LeaveCriticalSection(&cs);
    const bool postSafeOpStable = PumpProcessDataUntilStable(&context, kStableProcessDataMaxRounds, kStableProcessDataSleepUs);
    EnterCriticalSection(&cs);
    if (!postSafeOpStable) {
        SetError("Warning: processdata did not stabilize after SAFE_OP request");
    }
    
    // 8.5 在 SAFE_OP 狀態下設置各從站操作模式 (X5_7 startup order)
    {
        int8_t opMode = GetCyclicOperationMode();
        ss.str("");
        ss << "在 SAFE_OP 設置操作模式=" << static_cast<int>(opMode) << "...\n";
        SetError(ss.str());
        for (int i = 1; i <= context.slavecount; i++) {
            if (!WriteSDO(i, 0x6060, 0, &opMode, sizeof(opMode))) {
                ss.str("");
                ss << "從站 " << i << ": 無法在 SAFE_OP 設置操作模式\n";
                SetError(ss.str());
            }
        }
    }

    // 8.6 在 SAFE_OP 狀態下設置 Max Profile Velocity (0x607F)、
    //     Profile Acceleration (0x6083)、Profile Deceleration (0x6084)
    //     DVS/X1S 驅動器預設 0x607F=0，導致 CSV 模式所有速度命令被歸零。
    //     寫入高上限值以解除箝位（與 test_Ethercat_jog 一致）。
    {
        uint32_t maxProfileVelocity = 1000000000u; // essentially unlimited
        uint32_t profileAccel        = 10000000u;
        uint32_t profileDecel        = 10000000u;

        ss.str("");
        ss << "在 SAFE_OP 設置 0x607F/0x6083/0x6084...\n";
        SetError(ss.str());

        for (int i = 1; i <= context.slavecount; i++) {
            if (!WriteSDO(i, 0x607F, 0, &maxProfileVelocity, sizeof(maxProfileVelocity))) {
                ss.str("");
                ss << "從站 " << i << ": 無法在 SAFE_OP 設置 0x607F (Max Profile Velocity)\n";
                SetError(ss.str());
            }
            if (!WriteSDO(i, 0x6083, 0, &profileAccel, sizeof(profileAccel))) {
                ss.str("");
                ss << "從站 " << i << ": 無法在 SAFE_OP 設置 0x6083 (Profile Acceleration)\n";
                SetError(ss.str());
            }
            if (!WriteSDO(i, 0x6084, 0, &profileDecel, sizeof(profileDecel))) {
                ss.str("");
                ss << "從站 " << i << ": 無法在 SAFE_OP 設置 0x6084 (Profile Deceleration)\n";
                SetError(ss.str());
            }
        }
    }
    
    // 9. 檢查所有從站是否已成功達到SAFE_OP
    ecx_readstate(&context);
    bool allInSafeOp = true;
    std::stringstream errorss;
    
    for (int i = 1; i <= context.slavecount; i++) {
        if ((context.slavelist[i].state & EC_STATE_SAFE_OP) != EC_STATE_SAFE_OP) {
            allInSafeOp = false;
            errorss << "Slave " << i << " did not reach SAFE_OP. "
                    << "state=0x" << std::hex << context.slavelist[i].state
                    << " ALstatuscode=0x" << context.slavelist[i].ALstatuscode << std::dec;
            
            const char* alstr = ec_ALstatuscode2string(context.slavelist[i].ALstatuscode);
            if (alstr) errorss << " (" << alstr << ")";
            errorss << "\n";
        }
    }
    
    if (!allInSafeOp) {
        SetError(errorss.str());
        LeaveCriticalSection(&cs);
        return false;
    }
    
    ss.str("");
    ss << "所有從站已成功進入SAFE_OP狀態！\n";
    SetError(ss.str());

    // 10. 初始化 PDO 輸出數據（防止看門狗錯誤）
    ss.str("");
    ss << "初始化 PDO 輸出數據...\n";
    SetError(ss.str());
    
    InitializePDOData();
    
    // 發送初始化的數據到從站（釋放鎖以避免長持有）
    LeaveCriticalSection(&cs);
    const bool postInitOutputStable = PumpProcessDataUntilStable(&context, kStableProcessDataMaxRounds, kStableProcessDataSleepUs);
    EnterCriticalSection(&cs);
    if (!postInitOutputStable) {
        SetError("Warning: processdata did not stabilize after PDO initialization");
    }
    
    ss.str("");
    ss << "PDO 數據初始化完成！\n";
    SetError(ss.str());

    LeaveCriticalSection(&cs);
    return true;
}


bool EtherCATMaster::StartCyclicOperation() {
    EnterCriticalSection(&cs);

    if (simulationMode_.load(std::memory_order_relaxed)) {
        const bool ok = StartSimulationCyclicOperationLocked();
        LeaveCriticalSection(&cs);
        return ok;
    }
    
    if (!isInitialized) {
        SetError("Not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    if (isRunning) {
        SetError("Already running");
        LeaveCriticalSection(&cs);
        return false;
    }

    // DO NOT clear IOmap here - ConfigureSlaves/ForceSlaveOperational may have set initial values
    // that slaves expect to see. Clearing can trigger watchdog.
    
    // Create stop event and start cyclic thread BEFORE requesting OP so that
    // continuous processdata is sent while slaves transition to OP.
    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) {
        SetError("Failed to create stop event");
        LeaveCriticalSection(&cs);
        return false;
    }

    isRunning = true;
    // Request higher timer resolution to improve Sleep precision for sub-ms waits
    timePeriodActive = false;
    // Request 1ms resolution; if it succeeds, remember to end it on stop
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        timePeriodActive = true;
    }
    cyclicThread = CreateThread(NULL, 0, CyclicThreadFunc, this, 0, NULL);
    if (cyclicThread == NULL) {
        isRunning = false;
        CloseHandle(stopEvent);
        stopEvent = NULL;
        SetError("Failed to create cyclic thread");
        LeaveCriticalSection(&cs);
        return false;
    }
    
    // 提高 cyclic 執行緒與 process 優先級以改善 PDO 即時性
    // 固定使用 REALTIME_PRIORITY_CLASS（等級 31），無管理員權限時降級至 HIGH_PRIORITY_CLASS（等級 13）。
    DWORD currentClass = GetPriorityClass(GetCurrentProcess());
    if (currentClass != 0) {
        prevProcessPriorityClass = currentClass;
        prevProcessPriorityClassSet = true;
    }
    
    if (SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        printf("RT-Optimization: Process priority class elevated to REALTIME_PRIORITY\n");
    } else {
        DWORD classErr = ::GetLastError(); (void)classErr;
        if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
            printf("RT-Optimization: Process priority class elevated to HIGH_PRIORITY (REALTIME denied)\n");
        } else {
            printf("RT-Optimization: SetPriorityClass failed (err=%lu)\n", ::GetLastError());
        }
    }
    if (!SetThreadPriority(cyclicThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        DWORD err = ::GetLastError(); (void)err; // Preserve error handling
        std::stringstream ss; ss << "SetThreadPriority(cyclic) failed (err=" << err << ")";
        printf("%s\n", ss.str().c_str());
    }

    // 停用 Cyclic 執行緒的優先權自動提升，避免被 OS 干預造成相位漂移
    SetThreadPriorityBoost(cyclicThread, TRUE);

    // CPU 親和性設定
    // 使用 CpuIsolation 的拓撲感知遮罩（而非手動計算 1 << rtCore），
    // 避免實體核心索引 ≠ 邏輯 CPU 位元時 (AMD interleaved SMT / Intel hybrid) 綁錯核心。
    {
        if (CoreHelpers::CpuIsolation::IsHardIsolationActive()) {
            DWORD_PTR threadMask = CoreHelpers::CpuIsolation::GetRtThreadMask();
            if (threadMask != 0) {
                DWORD_PTR prevMask = SetThreadAffinityMask(cyclicThread, threadMask);
                if (prevMask != 0) {
                    DWORD_PTR coreMask = CoreHelpers::CpuIsolation::GetRtCoreMask();
                    int physCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
                    std::stringstream ss;
                    ss << "[Affinity] Cyclic thread pinned to RT mask 0x"
                       << std::hex << threadMask << std::dec
                       << " (physical core " << physCore
                       << ", full mask 0x" << std::hex << coreMask << std::dec << ")";
                    printf("%s\n", ss.str().c_str());
                } else {
                    std::stringstream ess;
                    ess << "[Affinity] SetThreadAffinityMask failed (err="
                        << static_cast<unsigned long>(::GetLastError())
                        << "), falling back";
                    printf("%s\n", ess.str().c_str());
                }
            }
        }
        // Fallback: if hard isolation not active, use legacy affinity
        if (!CoreHelpers::CpuIsolation::IsHardIsolationActive() ||
            CoreHelpers::CpuIsolation::GetRtThreadMask() == 0) {
            DWORD_PTR processMask = 0;
            DWORD_PTR systemMask = 0;
            if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
                int rtCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
                DWORD_PTR pick = 0;
                if (rtCore >= 0 && rtCore < 64) {
                    // Prefer topology-aware mask (physical → logical CPU mapping)
                    pick = CoreHelpers::CpuTopology::GetPrimaryLpMask(rtCore);
                }
                if (pick == 0 && rtCore >= 0 && rtCore < 64) {
                    // Last resort: treat physIdx as logical CPU (works without SMT)
                    pick = (DWORD_PTR)1 << rtCore;
                }
                if (pick != 0 && (systemMask & pick)) {
                    SetThreadAffinityMask(cyclicThread, pick);
                    std::stringstream ss;
                    ss << "[Affinity] Cyclic thread pinned to Core " << rtCore
                       << " (legacy, mask 0x" << std::hex << pick << std::dec << ")";
                    printf("%s\n", ss.str().c_str());
                }
            }
        }
    }


    // Wait briefly for the cyclic thread to start sending processdata.
    // Instead of sleeping a fixed amount we watch for real activity so we
    // only proceed once we have actually observed a PDO exchange.  This
    // removes arbitrary delays and makes the handshake deterministic.
    {
        int cycleMs = (int)((desiredCycleTime / 1000) > 0 ? (desiredCycleTime / 1000) : 1);
        // Allow enough cycles for pipeline priming (5 blocking receives at
        // ~2 ms each) plus a few extra for the first valid response.
        const uint32_t maxCycles = 20;
        uint32_t waited = 0;
        LeaveCriticalSection(&cs);
        while (waited < maxCycles) {
            // Require positive workingCounter (not just pdosSentCount) —
            // pdosSentCount increments every cycle even when WKC=EC_NOFRAME.
            if (workingCounter > 0) {
                break;
            }
            Sleep(cycleMs);
            waited++;
        }
        EnterCriticalSection(&cs);
        std::stringstream ss;
        if (workingCounter > 0) {
            ss << "Cyclic thread active after " << waited << " cycles (WKC=" << workingCounter << ")";
        } else {
            ss << "[Warn] cyclic thread WKC still <= 0 after " << maxCycles << " cycles"
               << " (pdosSent=" << pdosSentCount.load(std::memory_order_relaxed) << ")";
        }
        SetError(ss.str());
    }

    // All slaves should be in SAFE_OP (from ConfigureSlaves)
    // ── DC SYNC0 pre-stabilization (Win11 fix) ──
    // Let the CyclicTask pump PDO frames in SAFE_OP so slaves' distributed
    // clocks can converge before requesting OP.  Without this, Win11 HR-timer
    // jitter during initial DC convergence causes slaves to report AL=0x001A.
    // Adaptive: minimum 2 s, then check AL status every 1 s; break early when
    // no slave reports AL=0x001A (max 4 s).
    {
        constexpr int kDcMinSec = 2;
        constexpr int kDcMaxSec = 4;
        constexpr int kDcCheckIntervalSec = 1;
        const DWORD kDcPollMs = 200;
        std::stringstream ss;
        ss << "DC SYNC0 pre-stabilization (" << kDcMinSec << "-" << kDcMaxSec
           << " s adaptive, CyclicTask pumping in SAFE_OP)...";
        SetError(ss.str());
        LeaveCriticalSection(&cs);

        int waitedSec = 0;
        bool dcStable = false;
        while (waitedSec < kDcMaxSec) {
            int slicesUntilCheck = kDcCheckIntervalSec * (1000 / (int)kDcPollMs);
            for (int s = 0; s < slicesUntilCheck && waitedSec < kDcMaxSec; ++s) {
                Sleep(kDcPollMs);
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            }
            waitedSec += kDcCheckIntervalSec;
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

            if (waitedSec >= kDcMinSec) {
                ecx_readstate(&context);
                dcStable = true;
                for (int i = 1; i <= slaveCount; ++i) {
                    if (context.slavelist[i].ALstatuscode == 0x001A) {
                        dcStable = false;
                        break;
                    }
                }
                if (dcStable) break;
            }
        }
        EnterCriticalSection(&cs);
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            SetError("DC stabilization interrupted (stop event)");
            LeaveCriticalSection(&cs);
            return false;
        }
        ss.str("");
        ss << "DC SYNC0 pre-stabilization complete (" << waitedSec << " s)";
        SetError(ss.str());
    }
    
    // Now request OP.  Hold pdoExchangeCS so the CyclicTask cannot send/receive
    // PDO frames during the writestate — prevents SOEM port race corruption
    // that causes WKC=0 / EC_NOFRAME.
    printf("Requesting OP state for all slaves (cyclic thread priority=TIME_CRITICAL)...\n");
    LeaveCriticalSection(&cs);
    EnterCriticalSection(&pdoExchangeCS);
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0);
    LeaveCriticalSection(&pdoExchangeCS);
    EnterCriticalSection(&cs);

    // OP wait with periodic early-exit: ecx_readstate sends broadcast frames
    // that can interfere with CyclicTask PDO timing, so we only check every
    // 1 s instead of every 200 ms.  Once all slaves report OP, break immediately
    // instead of waiting the full timeout.
    // CyclicTask's CheckSlaveStates monitors and recovers slave states during
    // normal operation — the main thread only needs to confirm initial OP.
    {
        constexpr int kOpWaitSec = 8;  // upper bound — early exit when all slaves OP
        constexpr int kCheckIntervalSec = 1;  // check all-slaves-OP every 1 s
        std::stringstream ss2;
        ss2 << "Waiting up to " << kOpWaitSec << " s for DC SYNC0 stabilisation + OP transition...";
        SetError(ss2.str());
        LeaveCriticalSection(&cs);

        bool allOp = false;
        int waitedSec = 0;
        while (waitedSec < kOpWaitSec) {
            // Sleep in 200 ms slices until next check point
            int slicesUntilCheck = kCheckIntervalSec * 5;
            for (int s = 0; s < slicesUntilCheck && waitedSec < kOpWaitSec; ++s) {
                Sleep(200);
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            }
            waitedSec += kCheckIntervalSec;
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

            // Periodic state check — minimal interference at 1 s intervals
            EnterCriticalSection(&pdoExchangeCS);
            ecx_readstate(&context);
            allOp = true;
            for (int i = 1; i <= slaveCount; ++i) {
                if ((context.slavelist[i].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                    allOp = false;
                    break;
                }
            }
            LeaveCriticalSection(&pdoExchangeCS);
            if (allOp) break;
        }
        EnterCriticalSection(&cs);
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
            SetError("OP wait interrupted (stop event)");
            LeaveCriticalSection(&cs);
            return false;
        }

        // Final readstate to verify and collect per-slave diagnostics
        ecx_readstate(&context);
        allOp = true;
        std::vector<int> failedSlaves;
        for (int i = 1; i <= slaveCount; ++i) {
            if ((context.slavelist[i].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                allOp = false;
                failedSlaves.push_back(i);
            }
        }

        // Retry OP with a single broadcast — individual per-slave writestate
        // blocks the CyclicTask for 6 ms × N slaves and inflates the observed
        // max cycle to ~75 ms.  A broadcast is one 6 ms block.
        if (!allOp && !failedSlaves.empty()) {
            std::stringstream retryMsg;
            retryMsg << failedSlaves.size() << " slave(s) not yet in OP — retrying broadcast...";
            SetError(retryMsg.str());

            LeaveCriticalSection(&cs);
            EnterCriticalSection(&pdoExchangeCS);
            context.slavelist[0].state = EC_STATE_OPERATIONAL;
            ecx_writestate(&context, 0);
            LeaveCriticalSection(&pdoExchangeCS);

            // Wait up to 3 more seconds for DC convergence after retry — early exit
            // when all slaves reach OP (check every 1 s, same as primary loop).
            constexpr int kRetryWaitSec = 3;
            constexpr int kRetryCheckIntervalSec = 1;
            int retryWaitedSec = 0;
            bool retryAllOp = false;
            while (retryWaitedSec < kRetryWaitSec) {
                int slicesUntilCheck = kRetryCheckIntervalSec * 5;
                for (int s = 0; s < slicesUntilCheck && retryWaitedSec < kRetryWaitSec; ++s) {
                    Sleep(200);
                    if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
                }
                retryWaitedSec += kRetryCheckIntervalSec;
                if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

                EnterCriticalSection(&pdoExchangeCS);
                ecx_readstate(&context);
                retryAllOp = true;
                for (int i = 1; i <= slaveCount; ++i) {
                    if ((context.slavelist[i].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                        retryAllOp = false;
                        break;
                    }
                }
                LeaveCriticalSection(&pdoExchangeCS);
                if (retryAllOp) break;
            }
            EnterCriticalSection(&cs);
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                SetError("OP retry wait interrupted (stop event)");
                LeaveCriticalSection(&cs);
                return false;
            }

            // Recheck
            ecx_readstate(&context);
            allOp = true;
            failedSlaves.clear();
            for (int i = 1; i <= slaveCount; ++i) {
                if ((context.slavelist[i].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                    allOp = false;
                    failedSlaves.push_back(i);
                }
            }
        }

        // Build per-slave diagnostic log
        std::stringstream opDiag;
        opDiag << "OP verification complete:\n";
        bool anyFailed = false;
        for (int i = 1; i <= slaveCount; ++i) {
            int st = context.slavelist[i].state;
            bool inOp = ((st & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL);
            if (!inOp) anyFailed = true;
            opDiag << "  Slave " << i << ": state=0x" << std::hex << st << std::dec;
            if (!inOp) {
                opDiag << " [NOT OP!]";
                const char* alstr = ec_ALstatuscode2string(context.slavelist[i].ALstatuscode);
                if (alstr) opDiag << " AL=" << alstr;
                opDiag << " code=0x" << std::hex << context.slavelist[i].ALstatuscode << std::dec;
            }
            opDiag << " Obits=" << context.slavelist[i].Obits
                   << " Ibits=" << context.slavelist[i].Ibits
                   << " SM2=" << etohs(context.slavelist[i].SM[2].SMlength)
                   << "B SM3=" << etohs(context.slavelist[i].SM[3].SMlength) << "B";
            // FMMU diagnostics (for SM watchdog debugging)
            for (int f = 0; f < EC_MAXFMMU; f++) {
                auto& fm = context.slavelist[i].FMMU[f];
                if (fm.FMMUactive) {
                    opDiag << " FMMU" << f << ":t" << (int)fm.FMMUtype
                           << " L=0x" << std::hex << etohl(fm.LogStart)
                           << "+" << std::dec << etohs(fm.LogLength)
                           << " P=0x" << std::hex << etohs(fm.PhysStart) << std::dec;
                }
            }
            opDiag << " out=" << (void*)context.slavelist[i].outputs
                   << " in=" << (void*)context.slavelist[i].inputs
                   << " hasdc=" << (int)context.slavelist[i].hasdc << "\n";
        }

        if (anyFailed) {
            opDiag << "OP FAILED for some slaves (see above)\n";
            SetError(opDiag.str());
            LeaveCriticalSection(&cs);
            return false;
        }
        SetError(std::string("Cyclic operation started — all slaves in OP\n") + opDiag.str());

        // Reset cycle-time max counters so the UI shows post-OP jitter,
        // not the one-time 70 ms spike from pdoDataCS contention during
        // the OP transition (broadcast + individual retries).
        maxObservedCommsTime.store(0, std::memory_order_relaxed);
        maxObservedCycleMicros_.store(0, std::memory_order_relaxed);
        maxRecvTimeUs_.store(0, std::memory_order_relaxed);
    }

    // Start PDO processing thread for real-time data handling
    StartPDOProcessingThread();

    LeaveCriticalSection(&cs);
    return true;
}


void EtherCATMaster::StopCyclicOperation() {
    if (!isRunning) {
        return;
    }

    // 通知線程停止
    SetEvent(stopEvent);
    
    // 等待線程結束
    if (cyclicThread != NULL) {
        WaitForSingleObject(cyclicThread, INFINITE);
        CloseHandle(cyclicThread);
        cyclicThread = NULL;
    }

    CloseHandle(stopEvent);
    stopEvent = NULL;
    
    isRunning = false;
    pdoPipelinePrimed_.store(false, std::memory_order_release);
    SetError("Cyclic operation stopped");
    // Restore timer resolution if we changed it
    if (timePeriodActive) {
        timeEndPeriod(1);
        timePeriodActive = false;
    }
    // Restore previous process priority class if we changed it
    if (prevProcessPriorityClassSet && prevProcessPriorityClass != 0) {
        if (SetPriorityClass(GetCurrentProcess(), prevProcessPriorityClass)) {
            std::stringstream ss; ss << "RT-Optimization: Restored previous process priority class";
            printf("%s\n", ss.str().c_str());
        } else {
            DWORD err = ::GetLastError(); (void)err;
            std::stringstream ss; ss << "Warning: Failed to restore previous process priority class (err=" << err << ")";
            printf("%s\n", ss.str().c_str());
        }
        prevProcessPriorityClass = 0;
        prevProcessPriorityClassSet = false;
    }
}



