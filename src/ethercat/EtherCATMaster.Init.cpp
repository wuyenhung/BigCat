#include "EtherCATMaster.h"
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
#include "SafeMem.h"
#include "EsiParser.h"
#include "../include/core/CoreHelpers.h"

namespace {

constexpr int kStableProcessDataSleepUs = 5000;
constexpr int kStableProcessDataMaxRounds = 20;
constexpr int kStableProcessDataMinReadyRounds = 3;
constexpr int kSafeOpPollRounds = 60;
constexpr int kSafeOpPollTimeoutUs = 50000;
constexpr int kSafeOpPollSleepUs = 5000;
constexpr int kSafeOpFallbackTimeoutUs = EC_TIMEOUTSTATE * 2;
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
    
    // Step 2: Stop cyclic thread (no more processdata)
    log("[退出流程] 停止週期性資料交換...\n");
    StopCyclicOperation();
    log("[退出流程] [OK] 週期性執行緒已停止\n");

    // Step 2.5: Stop PDO processing thread
    log("[退出流程] 停止 PDO 處理執行緒...\n");
    StopPDOProcessingThread();
    log("[退出流程] [OK] PDO 處理執行緒已停止\n");

    // Step 2.6: Stop MMF reader/writer (ensure no MMF access while tearing down)
    log("[退出流程] 停止 MMF 輸出/輸入 (if enabled)...\n");
    try {
        // Stop MMF input reader if running
        StopMMF1In();
        // Disable MMF output (will Close mapping under mmfOutputCS_)
        DisableMMFOutput();
    } catch (...) {
        // Best-effort: do not let MMF cleanup failure prevent shutdown sequence
    }
    log("[退出流程] [OK] MMF 輸出/輸入已停止\n");

    // If caller provided a UI log callback, signal UI to stop timer
    if (logCallback) logCallback(std::string("STOP_TIMER\n"));
    
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
    
    // Step 4: Close EtherCAT port
    EnterCriticalSection(&cs);
    if (isInitialized) {
    log("[退出流程] 關閉 EtherCAT 連接埠...\n");
    ecx_close(&context);
    isInitialized = false;
    slaveCount = 0;
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
    
    // 等待所有從站回到INIT狀態
    context.slavelist[0].state = EC_STATE_INIT;
    ecx_writestate(&context, 0);
    ecx_statecheck(&context, 0, EC_STATE_INIT, EC_TIMEOUTSTATE * 3);
    
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
        // 1. 先移動到PRE-OP狀態以允許SDO操作
        context.slavelist[i].state = EC_STATE_PRE_OP;
        ecx_writestate(&context, i);
        if (ecx_statecheck(&context, i, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 2) != EC_STATE_PRE_OP) {
            configFailed = true;
            configErrors << "從站 " << i << " 無法達到PRE-OP狀態\n";
            continue;
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

        // 3. 設置伺服驅動器為預設 cyclic mode
        int8_t opMode = CoreHelpers::kDefaultCyclicOperationMode;
    if (!WriteSDO(i, 0x6060, 0, &opMode, sizeof(opMode))) {
            ss.str("");
            ss << "從站 " << i << ": 無法設置操作模式\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }

        // 4. 設置基本的 cyclic motion RxPDO 映射 (ESI-aware index)
        // 清空舊映射
    if (!WriteSDO(i, rxPdoIndex, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空RxPDO映射 (0x" << std::hex << rxPdoIndex << std::dec << ")\n";
            SetError(ss.str());
        }

        // 添加標準 cyclic motion 對象到 RxPDO
        uint32_t rxEntries[4] = {
            0x60400010,  // 控制字 (16位)
            0x607A0020,  // 目標位置 (32位)
            0x60FF0020,  // 目標速度 (32位)
            0x60710010   // 目標力矩 (16位)
        };

        for (int j = 0; j < 4; j++) {
            if (!WriteSDO(i, rxPdoIndex, j+1, &rxEntries[j], sizeof(uint32_t))) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置RxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }

        // 啟用4個RxPDO項目
        uint8_t rxCount = 4;
    if (!WriteSDO(i, rxPdoIndex, 0, &rxCount, sizeof(rxCount))) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用RxPDO映射\n";
            SetError(ss.str());
        }

        // 5. 設置基本的 cyclic motion TxPDO 映射 (ESI-aware index)
        // 清空舊映射
    if (!WriteSDO(i, txPdoIndex, 0, &zero, sizeof(zero))) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空TxPDO映射 (0x" << std::hex << txPdoIndex << std::dec << ")\n";
            SetError(ss.str());
        }

        // 添加標準 cyclic motion 對象到 TxPDO
        uint32_t txEntries[4] = {
            0x60410010,  // 狀態字 (16位)
            0x60640020,  // 實際位置 (32位)
            0x606C0020,  // 實際速度 (32位)
            0x60770010   // 實際力矩 (16位)
        };

        for (int j = 0; j < 4; j++) {
            if (!WriteSDO(i, txPdoIndex, j+1, &txEntries[j], sizeof(uint32_t))) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置TxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }

        // 啟用4個TxPDO項目
        uint8_t txCount = 4;
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
    }
    if (configFailed) {
        // 彙總配置錯誤
        SetError(configErrors.str());
    }
    
    // 5. 配置IOMap
    ss.str("");
    ss << "重新映射IOMap...\n";
    SetError(ss.str());
    
    int ioMapResult = ecx_config_map_group(&context, IOmap, 0);
    ss.str("");
    ss << "映射結果: " << ioMapResult << " bytes\n";
    
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

    // 若從站支援 DC，預先啟用 SYNC0 以使其對齊總線時間
    // 使用期望週期（us）轉 ns；相位採用可配置百分比（預設 15%，限制 10~20%）
    {
        uint32_t cycletime_ns = (desiredCycleTime > 0 ? desiredCycleTime : 2000) * 1000u;
        int pct = dcSyncPhasePercent;
        // 環境變數覆蓋：WUSOEM_SYNC0_PHASE_PCT（10..20）
        {
            wchar_t buf[32] = {0};
            DWORD n = GetEnvironmentVariableW(L"WUSOEM_SYNC0_PHASE_PCT", buf, _countof(buf));
            if (n > 0 && n < _countof(buf)) {
                int v = _wtoi(buf);
                if (v >= 10 && v <= 20) pct = v;
            }
        }
        if (pct < 10) pct = 10; if (pct > 20) pct = 20;
        int32_t phaseShift = (int32_t)((cycletime_ns * (uint32_t)pct) / 100u);
        ecx_readstate(&context);
        for (int i = 1; i <= context.slavecount; ++i) {
            if (context.slavelist[i].hasdc) {
                // 啟用 SYNC0，SYNC1 關閉
                ecx_dcsync0(&context, i, TRUE, cycletime_ns, phaseShift);
            }
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
    // ========================================================================
    // 遊戲共存模式檢查：
    // - 若啟用遊戲共存模式 (gameCoexistenceMode_)，使用 HIGH_PRIORITY_CLASS
    // - 若停用（專用機模式），嘗試 REALTIME_PRIORITY_CLASS
    // ========================================================================
    // 注意: REALTIME_PRIORITY_CLASS 會導致與 DCS/MSFS 等遊戲嚴重競爭，造成遊戲卡頓
    DWORD currentClass = GetPriorityClass(GetCurrentProcess());
    if (currentClass != 0) {
        prevProcessPriorityClass = currentClass;
        prevProcessPriorityClassSet = true;
    }
    
    DWORD targetPriorityClass = gameCoexistenceMode_ ? HIGH_PRIORITY_CLASS : REALTIME_PRIORITY_CLASS;
    const char* priorityName = gameCoexistenceMode_ ? "HIGH_PRIORITY (遊戲共存模式)" : "REALTIME_PRIORITY";
    
    if (SetPriorityClass(GetCurrentProcess(), targetPriorityClass)) {
        std::stringstream ss; 
        ss << "RT-Optimization: Process priority class elevated to " << priorityName;
        printf("%s\n", ss.str().c_str());
    } else {
        DWORD classErr = ::GetLastError(); (void)classErr;
        // 若目標是 REALTIME 且失敗，回退到 HIGH_PRIORITY_CLASS
        if (!gameCoexistenceMode_ && !SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
            DWORD err = ::GetLastError(); (void)err;
            std::stringstream ss; ss << "SetPriorityClass failed (fallback)";
            printf("%s\n", ss.str().c_str());
        } else if (!gameCoexistenceMode_) {
            std::stringstream ss; ss << "RT-Optimization: Process priority class elevated to HIGH_PRIORITY (fallback)";
            printf("%s\n", ss.str().c_str());
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
    // Process 層級的 Affinity 已在 MainWindow 初始化時設定，不包含 Core 4。
    // 因此我們在此將 Cyclic thread 明確綁定到 Core 4 (mask 0x10) 以實現真正的 CPU 隔離。
    {
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            // Core 4 mask is 0x10
            DWORD_PTR pick = (DWORD_PTR)1 << 4;
            if (systemMask & pick) {
                SetThreadAffinityMask(cyclicThread, pick);
                std::stringstream ss; 
                ss << "[Affinity] Cyclic thread pinned to isolated Core 4 (mask 0x10)";
                printf("%s\n", ss.str().c_str());
            }
        }
    }


    // Wait briefly for the cyclic thread to start sending processdata.
    // Instead of sleeping a fixed amount we watch for real activity so we
    // only proceed once we have actually observed a PDO exchange.  This
    // removes arbitrary delays and makes the handshake deterministic.
    {
        int cycleMs = (int)((desiredCycleTime / 1000) > 0 ? (desiredCycleTime / 1000) : 1);
        const uint32_t maxCycles = 10;
        uint32_t waited = 0;
        LeaveCriticalSection(&cs);
        while (waited < maxCycles) {
            if (workingCounter > 0 || pdosSentCount.load(std::memory_order_relaxed) > 0) {
                break;
            }
            Sleep(cycleMs);
            waited++;
        }
        EnterCriticalSection(&cs);
        std::stringstream ss;
        if (workingCounter > 0 || pdosSentCount.load(std::memory_order_relaxed) > 0) {
            ss << "Cyclic thread active after " << waited << " cycles";
        } else {
            ss << "[Warn] cyclic thread did not exchange PDOs after " << maxCycles << " cycles";
        }
        SetError(ss.str());
    }

    // All slaves should be in SAFE_OP (from ConfigureSlaveRuntime)
    // Now request OP for all slaves together while cyclic thread continuously sends processdata
    printf("Requesting OP state for all slaves (cyclic thread priority=TIME_CRITICAL)...\n");
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0);

    DWORD opPollSleepMs = (DWORD)((desiredCycleTime / 1000) > 0 ? (desiredCycleTime / 1000) : 1);
    if (opPollSleepMs > kOpMaxPollSleepMs) {
        opPollSleepMs = kOpMaxPollSleepMs;
    }
    const int opState = WaitForStateWithPolling(
        &context,
        EC_STATE_OPERATIONAL,
        kOpPollRounds,
        opPollSleepMs,
        kOpPollTimeoutUs,
        kOpFallbackTimeoutUs);

    if (!HasReachedState(opState, EC_STATE_OPERATIONAL)) {
        // collect detailed per-slave information for diagnosis
        ecx_readstate(&context);
        std::stringstream badop;
        badop << "Failed to reach OP state. Group state=0x" << std::hex << opState << std::dec << "\n";
        for (int i = 1; i <= slaveCount; ++i) {
            int st = context.slavelist[i].state;
            const char* alstr = ec_ALstatuscode2string(context.slavelist[i].ALstatuscode);
            badop << "Slave " << i << ": state=0x" << std::hex << st << std::dec;
            badop << " ALstatuscode=0x" << std::hex << context.slavelist[i].ALstatuscode << std::dec;
            if (alstr) badop << " (" << alstr << ")";
            if ((st & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                badop << " [NOT OP]";
            }
            badop << "\n";
        }
        SetError(badop.str());
        LeaveCriticalSection(&cs);
        return false;
    }

    SetError("Cyclic operation started");

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



