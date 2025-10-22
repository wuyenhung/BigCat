#include "EtherCATMaster.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
// for YieldProcessor
#include <intrin.h>
// timeBeginPeriod/timeEndPeriod
#include <mmsystem.h>
// for ALstatus code to string
#include <soem/ec_print.h>

// stagedOutputs holds persistent desired output byte patterns keyed by absolute IOmap offset
#include <unordered_map>

EtherCATMaster::EtherCATMaster() 
    : slaveCount(0)
    , isRunning(false)
    , isInitialized(false)
    , cyclicThread(NULL)
    , stopEvent(NULL)
    , cycleTime(1000)
    , desiredCycleTime(1000)
    , workingCounter(0)
    , pdosSentCount(0)
    , operationStartTime(0)
    , pdoProcessingThread(NULL)
    , pdoStopEvent(NULL)
    , pdoThreadRunning(false)
{
    memset(&context, 0, sizeof(context));
    memset(ifname, 0, sizeof(ifname));
    memset(IOmap, 0, sizeof(IOmap));
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&pdoDataCS);
}

EtherCATMaster::~EtherCATMaster() {
    Shutdown();
    DeleteCriticalSection(&cs);
    DeleteCriticalSection(&pdoDataCS);
}

bool EtherCATMaster::Initialize(const char* ifname_param) {
    EnterCriticalSection(&cs);
    
    if (isInitialized) {
        SetError("Already initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    strncpy_s(ifname, ifname_param, sizeof(ifname) - 1);
    
    // 初始化 SOEM Context (新版 API)
    if (ecx_init(&context, ifname) <= 0) {
        SetError("Failed to initialize EtherCAT on " + std::string(ifname) + " (ecx_init returned <= 0)");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 尋找並配置從站
    if (ecx_config_init(&context) <= 0) {
        SetError("No slaves found on " + std::string(ifname) + " (ecx_config_init returned <= 0)");
        ecx_close(&context);
        LeaveCriticalSection(&cs);
        return false;
    }

    slaveCount = context.slavecount;
    
    std::stringstream ss;
    ss << "Found " << slaveCount << " slaves";
    SetError(ss.str());
    
    isInitialized = true;
    LeaveCriticalSection(&cs);
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
        
        // 2. 清空舊的PDO映射
        uint8_t zero = 0;
        uint8_t one = 1;
        
        // 停用RxPDO分配 (0x1C12:00 = 0)
        if (ecx_SDOwrite(&context, i, 0x1C12, 0, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法停用RxPDO分配\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }
        
        // 停用TxPDO分配 (0x1C13:00 = 0)
        if (ecx_SDOwrite(&context, i, 0x1C13, 0, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法停用TxPDO分配\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }
        
        // 3. 設置伺服驅動器為CSP (循環同步位置) 模式
        int8_t opMode = 8;  // CSP模式 = 8
        if (ecx_SDOwrite(&context, i, 0x6060, 0, FALSE, sizeof(opMode), &opMode, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法設置操作模式\n";
            SetError(ss.str());
            // 非致命錯誤，繼續嘗試
        }
        
        // 4. 設置基本的RxPDO映射 (0x1600)
        // 清空舊映射
        if (ecx_SDOwrite(&context, i, 0x1600, 0, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空RxPDO映射\n";
            SetError(ss.str());
        }
        
        // 添加標準CSP對象到RxPDO
        uint32_t rxEntries[4] = {
            0x60400010,  // 控制字 (16位)
            0x607A0020,  // 目標位置 (32位)
            0x60FF0020,  // 目標速度 (32位)
            0x60710010   // 目標力矩 (16位)
        };
        
        for (int j = 0; j < 4; j++) {
            if (ecx_SDOwrite(&context, i, 0x1600, j+1, FALSE, sizeof(uint32_t), &rxEntries[j], EC_TIMEOUTRXM) <= 0) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置RxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }
        
        // 啟用4個RxPDO項目
        uint8_t rxCount = 4;
        if (ecx_SDOwrite(&context, i, 0x1600, 0, FALSE, sizeof(rxCount), &rxCount, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用RxPDO映射\n";
            SetError(ss.str());
        }
        
        // 5. 設置基本的TxPDO映射 (0x1A00)
        // 清空舊映射
        if (ecx_SDOwrite(&context, i, 0x1A00, 0, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法清空TxPDO映射\n";
            SetError(ss.str());
        }
        
        // 添加標準CSP對象到TxPDO
        uint32_t txEntries[4] = {
            0x60410010,  // 狀態字 (16位)
            0x60640020,  // 實際位置 (32位)
            0x606C0020,  // 實際速度 (32位)
            0x60770010   // 實際力矩 (16位)
        };
        
        for (int j = 0; j < 4; j++) {
            if (ecx_SDOwrite(&context, i, 0x1A00, j+1, FALSE, sizeof(uint32_t), &txEntries[j], EC_TIMEOUTRXM) <= 0) {
                ss.str("");
                ss << "從站 " << i << ": 無法設置TxPDO映射項目 " << j+1 << "\n";
                SetError(ss.str());
            }
        }
        
        // 啟用4個TxPDO項目
        uint8_t txCount = 4;
        if (ecx_SDOwrite(&context, i, 0x1A00, 0, FALSE, sizeof(txCount), &txCount, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用TxPDO映射\n";
            SetError(ss.str());
        }
        
        // 6. 分配PDO到同步管理器
        // 分配RxPDO到SM2
        uint16_t rxPdo = 0x1600;
        if (ecx_SDOwrite(&context, i, 0x1C12, 1, FALSE, sizeof(rxPdo), &rxPdo, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法分配RxPDO到同步管理器\n";
            SetError(ss.str());
        }
        
        // 啟用1個RxPDO
        if (ecx_SDOwrite(&context, i, 0x1C12, 0, FALSE, sizeof(one), &one, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用RxPDO分配\n";
            SetError(ss.str());
        }
        
        // 分配TxPDO到SM3
        uint16_t txPdo = 0x1A00;
        if (ecx_SDOwrite(&context, i, 0x1C13, 1, FALSE, sizeof(txPdo), &txPdo, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法分配TxPDO到同步管理器\n";
            SetError(ss.str());
        }
        
        // 啟用1個TxPDO
        if (ecx_SDOwrite(&context, i, 0x1C13, 0, FALSE, sizeof(one), &one, EC_TIMEOUTRXM) <= 0) {
            ss.str("");
            ss << "從站 " << i << ": 無法啟用TxPDO分配\n";
            SetError(ss.str());
        }
        
        ss.str("");
        ss << "從站 " << i << " PDO配置完成\n";
        SetError(ss.str());
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
    
    // 發送一些處理數據，確保從站處於穩定狀態
    ss.str("");
    ss << "發送處理數據循環以穩定從站狀態...\n";
    SetError(ss.str());
    
    for (int i = 0; i < 20; i++) {
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        osal_usleep(5000); // 5ms延遲
    }
    
    // 7. 請求SAFE_OP狀態
    ss.str("");
    ss << "請求所有從站進入SAFE_OP狀態...\n";
    SetError(ss.str());
    
    context.slavelist[0].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, 0);
    
    // 等待較長時間以確保所有從站有足夠時間進入SAFE_OP
    int timeout_multiplier = 15; // 給足夠的時間
    int chk = ecx_statecheck(&context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * timeout_multiplier);
    
    ss.str("");
    ss << "SAFE_OP檢查結果: 當前狀態=0x" << std::hex << chk << std::dec << "\n";
    SetError(ss.str());
    
    // 8. 再次發送處理數據以確保穩定
    for (int i = 0; i < 10; i++) {
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        osal_usleep(10000); // 10ms延遲
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
    
    // 發送初始化的數據到從站
    for (int i = 0; i < 20; i++) {
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        osal_usleep(5000); // 5ms延遲
    }
    
    ss.str("");
    ss << "PDO 數據初始化完成！\n";
    SetError(ss.str());

    LeaveCriticalSection(&cs);
    return true;
}

bool EtherCATMaster::StartCyclicOperation() {
    EnterCriticalSection(&cs);
    
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
    
    // 提高 cyclic 執行緒與程序優先級以改善 PDO 即時性
    // 注意: TIME_CRITICAL 可能會影響系統穩定性，使用時小心
    if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
        DWORD err = ::GetLastError();
        std::stringstream ss; ss << "SetPriorityClass failed (err=" << err << ")";
        printf("%s\n", ss.str().c_str());
    }
    if (!SetThreadPriority(cyclicThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        DWORD err = ::GetLastError();
        std::stringstream ss; ss << "SetThreadPriority(cyclic) failed (err=" << err << ")";
        printf("%s\n", ss.str().c_str());
    }

    // Bind cyclic thread to one available processor to avoid costly thread migration
    {
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            if (processMask != 0) {
                // choose a logical processor that represents a physical core (avoid SMT siblings)
                // pick the first logical processor of the first available physical core
                auto getFirstPhysicalProcessor = [](DWORD_PTR availableMask)->DWORD_PTR {
                    DWORD_PTR result = 0;
                    // Query logical processor information to find processor cores
                    DWORD len = 0;
                    BOOL ok = GetLogicalProcessorInformation(NULL, &len);
                    if (!ok && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
                        if (info) {
                            if (GetLogicalProcessorInformation(info, &len)) {
                                DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                                for (DWORD i = 0; i < count; ++i) {
                                    if (info[i].Relationship == RelationProcessorCore) {
                                        DWORD_PTR mask = info[i].ProcessorMask & availableMask;
                                        if (mask) {
                                            // pick lowest set bit in this core's mask
                                            DWORD_PTR pick = mask & (~mask + 1);
                                            result |= pick;
                                        }
                                    }
                                }
                            }
                            free(info);
                        }
                    }
                    if (result == 0) {
                        // fallback: pick lowest available logical processor
                        if (availableMask) result = availableMask & (~availableMask + 1);
                    }
                    return result;
                };

                DWORD_PTR pick = getFirstPhysicalProcessor(processMask);
                DWORD_PTR prev = SetThreadAffinityMask(cyclicThread, pick);
                if (prev == 0) {
                    DWORD err = ::GetLastError();
                    std::stringstream ss; ss << "SetThreadAffinityMask(cyclic) failed (err=" << err << ") pick=0x" << std::hex << pick;
                    std::string msg = ss.str();
                    printf("%s\n", msg.c_str());
                } else {
                    std::stringstream ss; ss << "Cyclic thread affinity set pick=0x" << std::hex << pick << " prev=0x" << prev;
                    std::string msg = ss.str();
                    printf("%s\n", msg.c_str());
                }
            } else {
                printf("GetProcessAffinityMask returned empty process mask\n");
            }
        } else {
            DWORD err = ::GetLastError();
            std::stringstream ss; ss << "GetProcessAffinityMask failed (err=" << err << ")";
            std::string msg = ss.str();
            printf("%s\n", msg.c_str());
        }
    }

    // Wait briefly for cyclic thread to start sending processdata
    // The cyclic thread will continuously send/receive, ensuring slaves get PDOs
    int cycleMs = (int)((desiredCycleTime / 1000) > 0 ? (desiredCycleTime / 1000) : 1);
    LeaveCriticalSection(&cs);
    Sleep(cycleMs * 10); // wait 10 cycles for cyclic thread to establish communication
    EnterCriticalSection(&cs);

    // All slaves should be in SAFE_OP (from ConfigureSlaveRuntime)
    // Now request OP for all slaves together while cyclic thread continuously sends processdata
    printf("Requesting OP state for all slaves (cyclic thread priority=TIME_CRITICAL)...\n");
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_writestate(&context, 0);

    // 等待所有從站達到 OP 狀態
    int wait = 40;
    while (wait-- > 0) {
        Sleep(50); // Let cyclic thread handle processdata
        ecx_statecheck(&context, 0, EC_STATE_OPERATIONAL, 50000);
        if (context.slavelist[0].state == EC_STATE_OPERATIONAL) {
            break;
        }
    }

    if (context.slavelist[0].state != EC_STATE_OPERATIONAL) {
        // collect detailed per-slave information for diagnosis
        std::stringstream badop;
        badop << "Failed to reach OP state. Group state=0x" << std::hex << context.slavelist[0].state << std::dec << "\n";
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
}

DWORD WINAPI EtherCATMaster::CyclicThreadFunc(LPVOID param) {
    EtherCATMaster* master = static_cast<EtherCATMaster*>(param);
    master->CyclicTask();
    return 0;
}

void EtherCATMaster::CyclicTask() {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);

    // Thread CPU sampling state (low-frequency diagnostic)
    unsigned long sampleCounter = 0;
    unsigned long sampleInterval = 500; // log every 500 cycles (~ depends on cycle time)
    unsigned long lastSampleTid = 0;
    unsigned long long lastSampleTotalUs = 0ULL;
    
    // 診斷: (file logging disabled) 原先會記錄 timer 初始化狀態到 logs/thread_cpu.log
    // #if 0
    // FILE* fLog = NULL;
    // fopen_s(&fLog, "logs\\thread_cpu.log", "a");
    // if (fLog) {
    //     fprintf(fLog, "[CyclicTask] Starting with desiredCycleTime=%u us\n", desiredCycleTime);
    //     fclose(fLog);
    // }
    // #endif

    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        QueryPerformanceCounter(&start);

        // 發送和接收過程數據
    ecx_send_processdata(&context);
    // count sends for diagnostics/UI
    pdosSentCount++;
    
    // 記錄首次PDO發送
    if (pdosSentCount == 1) {
        SetError("[PDO啟動] 首次PDO發送成功，開始連續發送模式");
    }
    
    // 每10次PDO發送記錄一次log (更頻繁的記錄)
    if (pdosSentCount % 10 == 0) {
        uint64_t runningTime = GetTickCount64() - operationStartTime;
        
        // 獲取當前時間戳
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        
        struct tm timeinfo;
        localtime_s(&timeinfo, &time_t);
        
            // File-based detailed PDO logging disabled (pdo_detailed.log)
            // #if 0
            // CreateDirectoryA("logs", NULL);
            // FILE* logFile = NULL;
            // fopen_s(&logFile, "logs\\pdo_detailed.log", "a");
            // if (logFile) { ... fprintf ...; fclose(logFile); }
            // #endif
        
        // 2. 簡化版統計記錄
            // File-based PDO stats logging disabled (pdo_stats.log)
            // #if 0
            // FILE* statsFile = NULL; fopen_s(&statsFile, "logs\\pdo_stats.log", "a"); if (statsFile) { fprintf(...); fclose(statsFile); }
            // #endif
        
        // 3. UI顯示 (每50次)
        if (pdosSentCount % 50 == 0) {
            char uiMsg[256];
            snprintf(uiMsg, sizeof(uiMsg), 
                "[PDO統計] #%llu WKC=%u 通訊=%uμs 運行=%llums",
                pdosSentCount, workingCounter, commsTime, runningTime);
            
            std::string tempError = lastError;
            SetError(uiMsg);
            if (tempError.empty()) {
                lastError = "";
            }
        }
    }
    
        workingCounter = ecx_receive_processdata(&context, EC_TIMEOUTRET);

        // 檢查從站狀態
        CheckSlaveStates();

        // 執行使用者回調
        if (cyclicCallback) {
            cyclicCallback();
        }

        QueryPerformanceCounter(&end);

        // 計算到目前為止的執行時間（微秒）
        uint32_t workMicros = (uint32_t)(((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart);

        // 計算剩餘要等待的微秒數（如果工作時間小於期望週期）
        int64_t remainingMicros = (int64_t)desiredCycleTime - (int64_t)workMicros;
        if (remainingMicros > 0) {
            // Windows高精度Waitable Timer方案 (Win10 1803+)
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 提供微秒級精度且低CPU
            static HANDLE hTimer = NULL;
            static bool timerInitFailed = false;
            
            if (hTimer == NULL && !timerInitFailed) {
                // 創建高精度計時器 (僅初始化一次)
                hTimer = CreateWaitableTimerExW(
                    NULL,                                   // 默認安全屬性
                    NULL,                                   // 未命名
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,  // 高精度標誌
                    TIMER_ALL_ACCESS
                );
                
                if (hTimer == NULL) {
                    // 不支持高精度計時器 (Windows 10 < 1803)
                    timerInitFailed = true;
                }
            }
            
            if (hTimer != NULL) {
                // 使用高精度計時器等待
                // 負值表示相對時間, 單位100納秒
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -(remainingMicros * 10LL);  // 微秒轉100ns
                
                if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    // 等待計時器或停止事件
                    HANDLE events[2] = { hTimer, stopEvent };
                    DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    
                    if (result == WAIT_OBJECT_0 + 1) {
                        // stopEvent觸發
                        goto exit_cycle_loop;
                    }
                    // result == WAIT_OBJECT_0: 計時器正常到期
                }
            } else {
                // 降級方案: Sleep(1)混合
                // 適用於不支持高精度計時器的舊系統
                if (remainingMicros > 1500) {
                    Sleep(1);  // 粗略等待
                } else {
                    // 短時間busy-wait
                    QueryPerformanceCounter(&end);
                    long long target = start.QuadPart + ((int64_t)desiredCycleTime * frequency.QuadPart) / 1000000;
                    while (end.QuadPart < target) {
                        YieldProcessor();
                        QueryPerformanceCounter(&end);
                    }
                }
            }
        }
exit_cycle_loop:
        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;

    // 記錄通訊工作時間 (send/receive + checks)
    commsTime = workMicros;

    // 最後再取一次時間，計算整個週期（包含等待）
        QueryPerformanceCounter(&end);
        cycleTime = (uint32_t)(((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart);

    // Low-frequency thread CPU sampling to help identify which thread consumes CPU
    sampleCounter++;
    if (sampleCounter >= sampleInterval) {
        sampleCounter = 0;
        // Get current thread id
        DWORD tid = GetCurrentThreadId();
        // Get kernel + user times for this thread (pseudo handle is valid for current thread)
        FILETIME ftCreate, ftExit, ftKernel, ftUser;
        if (GetThreadTimes(GetCurrentThread(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
            // Convert FILETIME (100-ns units) to microseconds
            ULARGE_INTEGER k, u;
            k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
            u.LowPart = ftUser.dwLowDateTime; u.HighPart = ftUser.dwHighDateTime;
            unsigned long long total100ns = k.QuadPart + u.QuadPart;
            unsigned long long totalUs = total100ns / 10ULL;

            unsigned long long deltaUs = 0ULL;
            if (lastSampleTid == tid) {
                if (totalUs > lastSampleTotalUs) deltaUs = totalUs - lastSampleTotalUs;
            }
            lastSampleTid = tid;
            lastSampleTotalUs = totalUs;

            // write to logs/thread_cpu.log under exe working folder (disabled)
            // #if 0
            // FILE* f = NULL;
            // fopen_s(&f, "logs\\thread_cpu.log", "a");
            // if (f) { SYSTEMTIME st; GetLocalTime(&st); fprintf(f, "[%02d:%02d:%02d.%03d] tid=%u commsUs=%u cycleUs=%u threadTotalUs=%llu deltaUs=%llu\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned)tid, (unsigned)commsTime, (unsigned)cycleTime, totalUs, deltaUs); fclose(f); }
            // #endif
        } else {
            // if GetThreadTimes fails, still log tid
            DWORD err = ::GetLastError();
            // FILE* f = NULL; fopen_s(&f, "logs\\thread_cpu.log", "a"); if (f) { SYSTEMTIME st; GetLocalTime(&st); fprintf(f, "[%02d:%02d:%02d.%03d] tid=%u commsUs=%u cycleUs=%u GetThreadTimes_err=%u\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned)GetCurrentThreadId(), (unsigned)commsTime, (unsigned)cycleTime, err); fclose(f); }
        }
    }

    // ...existing code...
    }
}

void EtherCATMaster::CheckSlaveStates() {
    // 檢查是否有從站不在 OP 狀態
    if (context.slavelist[0].state != EC_STATE_OPERATIONAL) {
        ecx_readstate(&context);
        for (int i = 1; i <= slaveCount; i++) {
            if (context.slavelist[i].state != EC_STATE_OPERATIONAL) {
                // 嘗試恢復
                context.slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context, i);
            }
        }
    }
}

void EtherCATMaster::InitializePDOData() {
    // 初始化 PDO 輸出數據，為所有伺服驅動器設置安全的初始值
    // 這可以防止從站在轉換到 OP 模式時因無效數據觸發看門狗錯誤
    
    printf("InitializePDOData: 為 %d 個從站設置安全初始值...\n", slaveCount);
    
    // 遍歷所有從站並設置初始輸出值
    for (int i = 1; i <= slaveCount; i++) {
        ec_slavet* slave = &context.slavelist[i];
        
        // 取得此從站的輸出數據起始位置（字節偏移）
        int outputOffset = slave->outputs - IOmap;  // 輸出數據在 IOmap 中的偏移
        int outputBytes = (slave->Obits + 7) / 8;   // 輸出字節數
        
        if (outputBytes > 0 && outputOffset >= 0 && outputOffset < (int)sizeof(IOmap)) {
            printf("  從站 %d (%s): 輸出偏移=%d, 字節數=%d\n", 
                   i, slave->name, outputOffset, outputBytes);
            
            // 為 CSP (循環同步位置) 模式設置典型的初始值
            // 假設 RxPDO 結構: Controlword(2字節) + Target Position(4字節) + ...
            
            if (outputBytes >= 2) {
                // 設置 Controlword (0x6040)
                // 初始值: 0x0006 = "Shutdown" 狀態，準備啟用
                uint16_t controlword = 0x0006;
                memcpy(IOmap + outputOffset, &controlword, sizeof(controlword));
                printf("    設置 Controlword = 0x%04X\n", controlword);
            }
            
            if (outputBytes >= 6) {
                // 設置 Target Position (0x607A) = 0 (起始位置)
                int32_t targetPosition = 0;
                memcpy(IOmap + outputOffset + 2, &targetPosition, sizeof(targetPosition));
                printf("    設置 Target Position = %d\n", targetPosition);
            }
            
            if (outputBytes >= 10) {
                // 設置 Digital Outputs (0x60FE:01) = 0
                uint32_t digitalOutputs = 0;
                memcpy(IOmap + outputOffset + 6, &digitalOutputs, sizeof(digitalOutputs));
                printf("    設置 Digital Outputs = 0x%08X\n", digitalOutputs);
            }
            
            // 如果有更多字節，設置為 0
            if (outputBytes > 10) {
                memset(IOmap + outputOffset + 10, 0, outputBytes - 10);
            }
        } else {
            printf("  從站 %d (%s): 無輸出數據或偏移無效\n", i, slave->name);
        }
    }
    
    printf("InitializePDOData: 完成\n");
}

bool EtherCATMaster::ReadProcessData() {
    // 數據已經在週期任務中讀取
    // 這裡只是提供一個介面讓外部知道數據已更新
    return isRunning;
}

bool EtherCATMaster::WriteProcessData() {
    // 數據會在下一個週期任務中發送
    // 這裡只是提供一個介面讓外部修改輸出數據
    return isRunning;
}

int EtherCATMaster::SnapshotIOMap(int offset, uint8_t* outBuf, int len) {
    if (!outBuf || offset < 0 || len <= 0) return 0;
    EnterCriticalSection(&cs);
    int maxCopy = (int)sizeof(IOmap) - offset;
    if (maxCopy <= 0) { LeaveCriticalSection(&cs); return 0; }
    int toCopy = (len < maxCopy) ? len : maxCopy;
    memcpy(outBuf, IOmap + offset, toCopy);
    LeaveCriticalSection(&cs);
    return toCopy;
}

bool EtherCATMaster::WriteOutputBytes(int absOffset, const uint8_t* data, int len) {
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to WriteOutputBytes");
        return false;
    }
    
    // File-based write debug logging disabled (pdo_write_debug.log)
    // #if 0
    // CreateDirectoryA("logs", NULL);
    // FILE* debugFile = NULL;
    // fopen_s(&debugFile, "logs\\pdo_write_debug.log", "a");
    // if (debugFile) {
    //     auto now = std::chrono::system_clock::now();
    //     auto time_t = std::chrono::system_clock::to_time_t(now);
    //     auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    //     struct tm timeinfo; localtime_s(&timeinfo, &time_t);
    //     fprintf(debugFile, "[%02d:%02d:%02d.%03d] WriteOutputBytes: 偏移%d, 長度%d, 資料: ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, (int)ms.count(), absOffset, len);
    //     for (int i = 0; i < len; i++) fprintf(debugFile, "%02X ", data[i]);
    //     if (len >= 2) { uint16_t value16 = data[0] | (data[1] << 8); fprintf(debugFile, " (16位值: %u)", value16); }
    //     fprintf(debugFile, "\n"); fclose(debugFile);
    // }
    // #endif
    
    EnterCriticalSection(&cs);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) { 
        LeaveCriticalSection(&cs); 
    // 記錄錯誤到 internal lastError
    std::stringstream ss_err; ss_err << "WriteOutputBytes: error maxWrite<=0 absOffset=" << absOffset << " IOmap_size=" << sizeof(IOmap);
    SetError(ss_err.str());
        return false; 
    }
    int toWrite = (len < maxWrite) ? len : maxWrite;
    memcpy(IOmap + absOffset, data, toWrite);
    LeaveCriticalSection(&cs);
    
    // Confirmation logging disabled (pdo_write_debug.log)
    // #if 0
    // fopen_s(&debugFile, "logs\\pdo_write_debug.log", "a");
    // if (debugFile) { fprintf(debugFile, "  -> 成功寫入 %d 字節到IOmap[%d..%d]: ", toWrite, absOffset, absOffset + toWrite - 1); for (int i = 0; i < toWrite; i++) fprintf(debugFile, "%02X ", IOmap[absOffset + i]); fprintf(debugFile, "\n"); fclose(debugFile); }
    // #endif
    
    return true;
}

// Stage output bytes into a persistent buffer that PDOProcessingTask will copy into IOmap
bool EtherCATMaster::StageOutputBytes(int absOffset, const uint8_t* data, int len) {
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to StageOutputBytes");
        return false;
    }
    EnterCriticalSection(&pdoDataCS);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) {
        LeaveCriticalSection(&pdoDataCS);
        SetError("StageOutputBytes: absOffset out of range");
        return false;
    }
    int toWrite = (len < maxWrite) ? len : maxWrite;
    std::vector<uint8_t> v(data, data + toWrite);
    // Debug: append previous and new staged bytes to a debug file to trace races/overwrites
    {
        FILE* f = NULL;
        errno_t e = fopen_s(&f, "logs\\pdo_stage_debug.log", "a");
        if (f) {
            // timestamp
            SYSTEMTIME st; GetLocalTime(&st);
            fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] StageOutputBytes absOffset=%d len=%d\n",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                    absOffset, toWrite);
            auto it = stagedOutputs.find(absOffset);
            if (it != stagedOutputs.end()) {
                fprintf(f, "  previous: ");
                for (size_t i = 0; i < it->second.size(); ++i) fprintf(f, "%02X ", it->second[i]);
                fprintf(f, "\n");
            } else {
                fprintf(f, "  previous: <none>\n");
            }
            fprintf(f, "  new:      ");
            for (int i = 0; i < toWrite; ++i) fprintf(f, "%02X ", v[i]);
            fprintf(f, "\n");
            fclose(f);
        }
    }
    // Store/replace the staged output at the absolute offset key
    stagedOutputs[absOffset] = std::move(v);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}

bool EtherCATMaster::StageOutputIfNotExists(int absOffset, const uint8_t* data, int len) {
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to StageOutputIfNotExists");
        return false;
    }
    EnterCriticalSection(&pdoDataCS);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) {
        LeaveCriticalSection(&pdoDataCS);
        SetError("StageOutputIfNotExists: absOffset out of range");
        return false;
    }
    if (stagedOutputs.find(absOffset) != stagedOutputs.end()) {
        // already staged; do not overwrite
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }
    int toWrite = (len < maxWrite) ? len : maxWrite;
    std::vector<uint8_t> v(data, data + toWrite);
    stagedOutputs[absOffset] = std::move(v);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}

bool EtherCATMaster::HasPendingPDOOutput(const std::string& itemName) {
    EnterCriticalSection(&pdoDataCS);
    bool found = (pendingPDOOutputs.find(itemName) != pendingPDOOutputs.end());
    LeaveCriticalSection(&pdoDataCS);
    return found;
}

bool EtherCATMaster::GetSlaveIOBaseBytes(int slaveIndex, int &outBase, int &inBase) {
    outBase = -1; inBase = -1;
    EnterCriticalSection(&cs);
    if (!isInitialized || slaveIndex <= 0 || slaveIndex > slaveCount) {
        LeaveCriticalSection(&cs);
        return false;
    }
    if (&context && context.slavelist) {
        // Prefer exact byte offsets from SOEM-provided IOmap pointers when available
        if (context.slavelist[slaveIndex].outputs) {
            outBase = (int)(context.slavelist[slaveIndex].outputs - IOmap);
        } else {
            outBase = context.slavelist[slaveIndex].Ostartbit / 8;
        }
        if (context.slavelist[slaveIndex].inputs) {
            inBase = (int)(context.slavelist[slaveIndex].inputs - IOmap);
        } else {
            inBase  = context.slavelist[slaveIndex].Istartbit / 8;
        }

        // Fallback: if startbits are zero for this slave (and it's not the first),
        // try to compute bases by accumulating previous slaves' bit lengths.
        if ((outBase == 0 && inBase == 0) && slaveIndex > 1) {
            int accOutBits = 0;
            int accInBits = 0;
            for (int i = 1; i < slaveIndex; ++i) {
                accOutBits += context.slavelist[i].Obits;
                accInBits  += context.slavelist[i].Ibits;
            }
            // convert bits -> bytes (round down)
            int fallbackOut = accOutBits / 8;
            int fallbackIn  = accInBits / 8;
            // Only use fallback if it yields non-zero values
            if (fallbackOut > 0 || fallbackIn > 0) {
                outBase = fallbackOut;
                inBase = fallbackIn;
            }
        }

        LeaveCriticalSection(&cs);
        return true;
    }
    LeaveCriticalSection(&cs);
    return false;
}

bool EtherCATMaster::ReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                             void* data, int &size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    int wkc = ecx_SDOread(&context, slave, index, subindex, FALSE, &size, data, EC_TIMEOUTRXM);
    
    if (wkc <= 0) {
        std::stringstream ss;
        ss << "Failed to read SDO from slave " << slave 
           << ", Index: 0x" << std::hex << index 
           << ", Subindex: 0x" << (int)subindex;
        SetError(ss.str());
        LeaveCriticalSection(&cs);
        return false;
    }

    LeaveCriticalSection(&cs);
    return true;
}

bool EtherCATMaster::WriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                              void* data, int size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // Mark parameters as used to silence unused-parameter warnings in some build configs
    (void)data;
    (void)size;
    int wkc = ecx_SDOwrite(&context, slave, index, subindex, FALSE, size, data, EC_TIMEOUTRXM);
    
    if (wkc <= 0) {
        std::stringstream ss;
        ss << "Failed to write SDO to slave " << slave 
           << ", Index: 0x" << std::hex << index 
           << ", Subindex: 0x" << (int)subindex;
        SetError(ss.str());
        LeaveCriticalSection(&cs);
        return false;
    }

    LeaveCriticalSection(&cs);
    return true;
}

bool EtherCATMaster::SendMailbox(uint16_t slave, uint8_t* data, int size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 使用 CoE (CAN over EtherCAT) mailbox
    // 注意：完整的 mailbox 實作需要使用 ec_mbxbuft 結構
    // 標記參數為已使用以避免未使用參數警告
    (void)data;
    (void)size;
    // 這裡暫時返回未實作
    SetError("Mailbox send requires ec_mbxbuft structure - use SDO instead");
    LeaveCriticalSection(&cs);
    return false;
}

bool EtherCATMaster::ReceiveMailbox(uint16_t slave, uint8_t* data, int* size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 注意：完整的 mailbox 實作需要使用 ec_mbxbuft 結構
    // 標記參數為已使用以避免未使用參數警告
    (void)data;
    (void)size;
    // 這裡暫時返回未實作
    SetError("Mailbox receive requires ec_mbxbuft structure - use SDO instead");
    LeaveCriticalSection(&cs);
    return false;
}

std::vector<SlaveInfo> EtherCATMaster::GetSlaveInfo() {
    std::vector<SlaveInfo> info;
    
    EnterCriticalSection(&cs);
    
    for (int i = 1; i <= slaveCount; i++) {
        SlaveInfo si;
        si.position = i;
        si.name = context.slavelist[i].name;
        si.aliasAddress = context.slavelist[i].aliasadr;
        si.vendorId = context.slavelist[i].eep_man;
        si.productCode = context.slavelist[i].eep_id;
        si.state = context.slavelist[i].state;
        si.outputBits = context.slavelist[i].Obits;
        si.inputBits = context.slavelist[i].Ibits;
        info.push_back(si);
    }
    
    LeaveCriticalSection(&cs);
    return info;
}

void EtherCATMaster::SetError(const std::string& error) {
    lastError = error;
}

bool EtherCATMaster::DumpESI(uint16_t slave, std::string &outDump)
{
    EnterCriticalSection(&cs);
    outDump.clear();
    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // Allocate buffer for EEPROM dump (size EC_MAXEEPBUF from SOEM)
    const int BUFSZ = EC_MAXEEPBUF;
    std::vector<uint8_t> buf(BUFSZ, 0);

    // Use SOEM helper to dump eeprom into buffer
    ecx_esidump(&context, slave, buf.data());

    // Convert binary dump to hex lines
    std::ostringstream oss;
    oss << "ESI/EEPROM dump for slave " << slave << "\n";
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < BUFSZ; i += 16) {
        oss << std::setw(4) << i << ": ";
        for (int j = 0; j < 16; ++j) {
            if (i + j < BUFSZ) oss << std::setw(2) << (int)buf[i + j] << ' ';
            else oss << "   ";
        }
        oss << " ";
        for (int j = 0; j < 16; ++j) {
            char c = (char)buf[i + j];
            if (i + j < BUFSZ && isprint((unsigned char)c)) oss << c;
            else oss << '.';
        }
        oss << "\n";
    }

    // Also read a few EEPROM registers via ecx_readeeprom to provide summary
    uint32_t manuf = ecx_readeeprom(&context, slave, ECT_SII_MANUF, EC_TIMEOUTEEP);
    uint32_t id = ecx_readeeprom(&context, slave, ECT_SII_ID, EC_TIMEOUTEEP);
    uint32_t rev = ecx_readeeprom(&context, slave, ECT_SII_REV, EC_TIMEOUTEEP);
    oss << "Manufacturer: 0x" << std::hex << manuf << " Product ID: 0x" << id << " Revision: 0x" << rev << "\n";

    outDump = oss.str();
    LeaveCriticalSection(&cs);
    return true;
}

bool EtherCATMaster::ForceSlaveOperational(uint16_t slave, std::string &outLog)
{
    // Use timeout to prevent infinite blocking
    DWORD waitResult = TryEnterCriticalSection(&cs) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (waitResult == WAIT_TIMEOUT) {
        // Try for up to 5 seconds
        for (int retry = 0; retry < 50; ++retry) {
            Sleep(100);
            if (TryEnterCriticalSection(&cs)) {
                waitResult = WAIT_OBJECT_0;
                break;
            }
        }
    }
    
    if (waitResult == WAIT_TIMEOUT) {
        outLog = "ERROR: Timeout acquiring lock (possible deadlock)\n";
        return false;
    }
    
    outLog.clear();
    std::ostringstream oss;

    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        oss << "Invalid slave or not initialized\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return false;
    }

    // Read current state and AL status
    ecx_readstate(&context);
    int st = context.slavelist[slave].state;
    oss << "Slave " << slave << " current state=0x" << std::hex << st << std::dec << "\n";
    const char* alstr = ec_ALstatuscode2string(context.slavelist[slave].ALstatuscode);
    oss << "ALstatuscode=0x" << std::hex << context.slavelist[slave].ALstatuscode << std::dec;
    if (alstr) oss << " (" << alstr << ")";
    oss << "\n";

    // If slave already OP, done
    if ((st & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL) {
        oss << "Already OP\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return true;
    }

    // Try to map/processdata again for this slave (re-run state transitions)
    // First request SAFE_OP then OP
    context.slavelist[slave].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    oss << "Requested SAFE_OP, new state=0x" << std::hex << context.slavelist[slave].state << std::dec << "\n";

    // If IO bits are zero, report and attempt to re-config map for this slave
    if (context.slavelist[slave].Obits == 0 && context.slavelist[slave].Ibits == 0) {
        oss << "Note: Outputs/Inputs bits are zero. Attempting esidump/read eeprom for diagnostics...\n";
        // perform esidump into buffer (non-destructive)
        const int BUFSZ = EC_MAXEEPBUF;
        std::vector<uint8_t> buf(BUFSZ);
        ecx_esidump(&context, slave, buf.data());
        // include short summary of first 32 bytes
        oss << "EEPROM head: ";
        for (int i = 0; i < 32 && i < BUFSZ; ++i) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i] << " ";
        }
        oss << std::dec << "\n";
    }

    // Try to set OP at group level
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_send_processdata(&context);
    ecx_receive_processdata(&context, EC_TIMEOUTRET);
    ecx_writestate(&context, 0);

    // Wait briefly and check
    ecx_statecheck(&context, slave, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
    ecx_readstate(&context);
    int st2 = context.slavelist[slave].state;
    oss << "After OP request, slave state=0x" << std::hex << st2 << std::dec << "\n";

    bool ok = ((st2 & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL);
    if (!ok) {
        // Slave failed initial transition - attempt runtime PDO configuration
        oss << "Attempting runtime PDO configuration...\n";
        
        // Step 1: Reset to INIT to clear any error state
        context.slavelist[slave].state = EC_STATE_INIT;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE);
        
        // Step 2: Return to PRE-OP for SDO configuration
        context.slavelist[slave].state = EC_STATE_PRE_OP;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 5);
        
        // Step 3: Configure PDO mappings for CSP (Cyclic Sync Position) mode
        uint8_t zero = 0, one = 1, pdoCount = 4;
        
        // Disable assignments temporarily
        ecx_SDOwrite(&context, slave, 0x1C12, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
        ecx_SDOwrite(&context, slave, 0x1C13, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
        
        // Configure RxPDO 0x1600 (Master->Slave)
        uint32_t rxMappings[] = {0x60400010, 0x607A0020, 0x60810020, 0x60FF0020};
        ecx_SDOwrite(&context, slave, 0x1600, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
        for (int i = 0; i < pdoCount; ++i) {
            ecx_SDOwrite(&context, slave, 0x1600, i+1, FALSE, sizeof(uint32_t), &rxMappings[i], EC_TIMEOUTRXM);
        }
        ecx_SDOwrite(&context, slave, 0x1600, 0x00, FALSE, sizeof(pdoCount), &pdoCount, EC_TIMEOUTRXM);
        
        // Configure TxPDO 0x1A00 (Slave->Master)
        uint32_t txMappings[] = {0x60410010, 0x60640020, 0x60770010, 0x606C0020};
        ecx_SDOwrite(&context, slave, 0x1A00, 0x00, FALSE, sizeof(zero), &zero, EC_TIMEOUTRXM);
        for (int i = 0; i < pdoCount; ++i) {
            ecx_SDOwrite(&context, slave, 0x1A00, i+1, FALSE, sizeof(uint32_t), &txMappings[i], EC_TIMEOUTRXM);
        }
        ecx_SDOwrite(&context, slave, 0x1A00, 0x00, FALSE, sizeof(pdoCount), &pdoCount, EC_TIMEOUTRXM);
        
        // Assign PDOs to Sync Managers
        uint16_t rxPdoAssign = 0x1600, txPdoAssign = 0x1A00;
        ecx_SDOwrite(&context, slave, 0x1C12, 0x01, FALSE, sizeof(rxPdoAssign), &rxPdoAssign, EC_TIMEOUTRXM);
        ecx_SDOwrite(&context, slave, 0x1C12, 0x00, FALSE, sizeof(one), &one, EC_TIMEOUTRXM);
        ecx_SDOwrite(&context, slave, 0x1C13, 0x01, FALSE, sizeof(txPdoAssign), &txPdoAssign, EC_TIMEOUTRXM);
        ecx_SDOwrite(&context, slave, 0x1C13, 0x00, FALSE, sizeof(one), &one, EC_TIMEOUTRXM);
        
        // Step 4: Set operation mode to CSP (Cyclic Sync Position = 8)
        int8_t opMode = 8;
        ecx_SDOwrite(&context, slave, 0x6060, 0x00, FALSE, sizeof(opMode), &opMode, EC_TIMEOUTRXM);
        
        // Step 5: Configure EtherCAT cycle time (0x60C2:01 - Interpolation time period value)
        // Convert from microseconds to units expected by device (typically 0.1ms units)
        // For 2000us (2ms), we need 20 units of 0.1ms
        uint8_t cycleUnits = (uint8_t)((desiredCycleTime + 50) / 100); // Round to 0.1ms units
        if (cycleUnits == 0) cycleUnits = 20; // Default 2ms = 20 * 0.1ms
        int8_t cycleIndex = -3; // Unit: 10^-3 seconds (1ms)
        oss << "Configuring cycle time: " << (int)cycleUnits << " * 0.1ms = " 
            << (cycleUnits * 0.1) << "ms\n";
        ecx_SDOwrite(&context, slave, 0x60C2, 0x01, FALSE, sizeof(cycleUnits), &cycleUnits, EC_TIMEOUTRXM);
        ecx_SDOwrite(&context, slave, 0x60C2, 0x02, FALSE, sizeof(cycleIndex), &cycleIndex, EC_TIMEOUTRXM);
        
        oss << "PDO configuration complete. Retrying state transitions...\n";
        
        // Step 5: Attempt state transitions with new configuration
        context.slavelist[slave].state = EC_STATE_SAFE_OP;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 3);
        
        context.slavelist[0].state = EC_STATE_OPERATIONAL;
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        ecx_writestate(&context, 0);
        ecx_statecheck(&context, slave, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
        
        ecx_readstate(&context);
        st2 = context.slavelist[slave].state;
        ok = ((st2 & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL);
        
        if (ok) {
            oss << "Runtime configuration successful - slave reached OP\n";
            outLog = oss.str();
            LeaveCriticalSection(&cs);
            return ok;
        }
        
        oss << "Runtime configuration failed. Running diagnostics...\n";
        uint32_t vendor = 0;
        int sz = sizeof(vendor);
        int wkc = ecx_SDOread(&context, slave, 0x1018, 0x00, FALSE, (int*)&sz, &vendor, EC_TIMEOUTRXM);
        if (wkc > 0) {
            oss << "Read SDO 0x1018:vendor=0x" << std::hex << vendor << std::dec << "\n";
        } else {
            oss << "SDO 0x1018 read failed or not present\n";
        }
        
        // Read PDO assignment 0x1C12 (RxPDO) and 0x1C13 (TxPDO) for additional diagnostics
        oss << "Reading PDO assignment (0x1C12 RxPDO, 0x1C13 TxPDO)...\n";
        // helper lambda to read assignment
        auto readAssignment = [&](uint16_t idx, const char* name){
            uint8_t count8 = 0;
            int szz = sizeof(count8);
            int w = ecx_SDOread(&context, slave, idx, 0x00, FALSE, &szz, &count8, EC_TIMEOUTRXM);
            if (w > 0) {
                oss << name << " count=" << (int)count8 << "\n";
                for (uint8_t i = 1; i <= count8; ++i) {
                    uint32_t entry = 0;
                    int szz2 = sizeof(entry);
                    int w2 = ecx_SDOread(&context, slave, idx, i, FALSE, &szz2, &entry, EC_TIMEOUTRXM);
                    if (w2 > 0) {
                        oss << "  " << name << "[" << (int)i << "]=0x" << std::hex << entry << std::dec << "\n";
                    } else {
                        oss << "  " << name << "[" << (int)i << "]=<read fail>\n";
                    }
                }
            } else {
                oss << name << " not readable or absent\n";
            }
        };

        readAssignment(0x1C12, "RxPDO(0x1C12)");
        readAssignment(0x1C13, "TxPDO(0x1C13)");

        // Exhaustive read: iterate 0x1600..0x160F (RxPDO areas) and 0x1A00..0x1A0F (TxPDO areas)
        auto readFullPDORange = [&](uint16_t baseIdx, const char* nameBase){
            for (uint16_t off = 0; off < 0x10; ++off) {
                uint16_t idx = baseIdx + off;
                char nm[32];
                sprintf_s(nm, sizeof(nm), "%s(0x%04X)", nameBase, idx);
                uint8_t cnt = 0;
                int szz = sizeof(cnt);
                int w = ecx_SDOread(&context, slave, idx, 0x00, FALSE, &szz, &cnt, EC_TIMEOUTRXM);
                if (w > 0 && cnt > 0) {
                    oss << nm << " count=" << (int)cnt << "\n";
                    for (uint8_t si = 1; si <= cnt; ++si) {
                        uint32_t map = 0;
                        int szz2 = sizeof(map);
                        int w2 = ecx_SDOread(&context, slave, idx, si, FALSE, &szz2, &map, EC_TIMEOUTRXM);
                        if (w2 > 0) {
                            oss << "  [" << (int)si << "]=0x" << std::hex << map << std::dec << "\n";
                        } else {
                            oss << "  [" << (int)si << "]=<read fail>\n";
                        }
                    }
                } else if (w > 0) {
                    oss << nm << " count=0\n";
                } else {
                    // not present or not readable
                    // Do not spam if absent; show once per index
                    oss << nm << " not readable/absent\n";
                }
            }
        };

        readFullPDORange(0x1600, "RxPDO");
        readFullPDORange(0x1A00, "TxPDO");
    }

    outLog = oss.str();
    LeaveCriticalSection(&cs);
    return ok;
}

bool EtherCATMaster::ConfigureSlaveRuntime(uint16_t slave, std::string &outLog)
{
    // Configure slave runtime PDO but stay in SAFE_OP (don't transition to OP yet)
    // This allows all slaves to be configured first, then switched to OP together
    
    DWORD waitResult = TryEnterCriticalSection(&cs) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (waitResult == WAIT_TIMEOUT) {
        for (int retry = 0; retry < 50; ++retry) {
            Sleep(100);
            if (TryEnterCriticalSection(&cs)) {
                waitResult = WAIT_OBJECT_0;
                break;
            }
        }
    }
    
    if (waitResult == WAIT_TIMEOUT) {
        outLog = "ERROR: Timeout acquiring lock\n";
        return false;
    }
    
    outLog.clear();
    std::ostringstream oss;

    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        oss << "Invalid slave or not initialized\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return false;
    }

    // Read current state
    ecx_readstate(&context);
    int st = context.slavelist[slave].state;
    oss << "Slave " << slave << " current state=0x" << std::hex << st << std::dec << "\n";
    
    // If slave has error flag (bit 4), acknowledge and clear it
    if (st & 0x10) {
        oss << "Slave has error flag set - reading AL Status Code...\n";
        uint16_t alStatusCode = 0;
        // Read AL Status Code (0x0134) to get error details
        ecx_FPRD(&context.port, context.slavelist[slave].configadr, 
                 ECT_REG_ALSTATCODE, sizeof(alStatusCode), &alStatusCode, EC_TIMEOUTRET);
        oss << "AL Status Code: 0x" << std::hex << alStatusCode << std::dec << "\n";
        
        // Clear error by writing to AL Control register with error acknowledge bit
        uint16_t alControl = 0x0011;  // Error ACK (bit 4) + INIT state (0x01)
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 ECT_REG_ALCTL, sizeof(alControl), &alControl, EC_TIMEOUTRET);
        oss << "Sent error acknowledge to AL Control\n";
        
        // Wait a bit for error to clear
        osal_usleep(10000);
    }

    // Reset to INIT to clear any error state
    context.slavelist[slave].state = EC_STATE_INIT;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE * 2);
    
    // Verify INIT state was reached
    ecx_readstate(&context);
    if ((context.slavelist[slave].state & 0x0F) != EC_STATE_INIT) {
        oss << "WARNING: Failed to reach INIT state, current=0x" 
            << std::hex << context.slavelist[slave].state << std::dec << "\n";
        // If still in error, try one more time with explicit error clear
        if (context.slavelist[slave].state & 0x10) {
            uint16_t alControl = 0x0011;
            ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                     ECT_REG_ALCTL, sizeof(alControl), &alControl, EC_TIMEOUTRET);
            osal_usleep(20000);
            context.slavelist[slave].state = EC_STATE_INIT;
            ecx_writestate(&context, slave);
            ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE * 3);
        }
    }
    
    // Clear all FMMU and SM configurations to ensure clean state
    oss << "Clearing FMMU and SM configurations...\n";
    uint8_t zeroBlock[16] = {0};
    // Clear FMMU 0-3 (each FMMU is 16 bytes at 0x0600, 0x0610, 0x0620, 0x0630)
    for (int i = 0; i < 4; i++) {
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 0x0600 + (i * 0x10), 16, zeroBlock, EC_TIMEOUTRET);
    }
    // Clear SM 0-7 (each SM is 8 bytes at 0x0800, 0x0808, etc.)
    for (int i = 0; i < 8; i++) {
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 0x0800 + (i * 0x08), 8, zeroBlock, EC_TIMEOUTRET);
    }
    osal_usleep(10000);
    
    // Move to PRE-OP (SOEM will use slave's EEPROM configuration)
    oss << "Moving to PRE-OP (using EEPROM defaults)...\n";
    context.slavelist[slave].state = EC_STATE_PRE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 3);
    
    // Only set operation mode to CSP (8) - don't modify PDO mappings
    int8_t opMode = 8;
    int wkc = ecx_SDOwrite(&context, slave, 0x6060, 0x00, FALSE, sizeof(opMode), &opMode, EC_TIMEOUTRXM);
    if (wkc > 0) {
        oss << "Set operation mode to CSP (8)\n";
    } else {
        oss << "WARNING: Failed to set operation mode (wkc=" << wkc << ")\n";
    }
    
    // Move to SAFE_OP (but don't go to OP yet)
    context.slavelist[slave].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 3);
    
    ecx_readstate(&context);
    int finalState = context.slavelist[slave].state;
    bool ok = ((finalState & EC_STATE_SAFE_OP) == EC_STATE_SAFE_OP);
    
    if (ok) {
        oss << "Runtime configuration OK - slave at SAFE_OP (state=0x" << std::hex << finalState << std::dec << ")\n";
    } else {
        oss << "Configuration failed - final state=0x" << std::hex << finalState << std::dec << "\n";
    }
    
    outLog = oss.str();
    LeaveCriticalSection(&cs);
    return ok;
}

void EtherCATMaster::SendProcessDataOnce() {
    // Send one processdata roundtrip (standard SOEM practice before OP request)
    ecx_send_processdata(&context);
    ecx_receive_processdata(&context, EC_TIMEOUTRET);
}

// Per-slave cyclic thread implementation
DWORD WINAPI EtherCATMaster::PerSlaveCyclicThreadFunc(LPVOID param) {
    SlaveThreadData* data = (SlaveThreadData*)param;
    data->master->PerSlaveCyclicTask(data->slaveIndex, data->stopEvent);
    return 0;
}

void EtherCATMaster::PerSlaveCyclicTask(uint16_t slaveIndex, HANDLE slaveStopEvent) {
    // Continuously send/receive processdata for this specific slave until stop signal
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    
    // Cache cycle time to avoid repeated access
    uint32_t cycleMicros = desiredCycleTime;
    if (cycleMicros == 0) cycleMicros = 2000; // default 2ms if not set
    
    while (WaitForSingleObject(slaveStopEvent, 0) == WAIT_TIMEOUT) {
        QueryPerformanceCounter(&start);
        
        // Try to acquire lock without blocking to avoid deadlock
        // If lock is held by another operation (e.g., ForceSlaveOperational), skip this cycle
        if (TryEnterCriticalSection(&cs)) {
            // Send/receive processdata for all slaves (SOEM requires group-level operation)
            ecx_send_processdata(&context);
            ecx_receive_processdata(&context, EC_TIMEOUTRET);
            
            // Quick check: if slave lost OP, mark for recovery but don't block here
            if ((context.slavelist[slaveIndex].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                context.slavelist[slaveIndex].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context, slaveIndex);
            }
            
            LeaveCriticalSection(&cs);
        }
        // If couldn't get lock, just sleep and try next cycle
        
        // Sleep for cycle time (use UI-configured value)
        QueryPerformanceCounter(&end);
        LONGLONG elapsed_us = ((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart;
        LONGLONG sleep_us = (LONGLONG)cycleMicros - elapsed_us;
        if (sleep_us > 0) {
            // 如果剩餘時間 >= 1ms，Sleep 那麼多毫秒；若小於 1ms，用 Sleep(0) 讓出時片以避免忙等
            DWORD sleepMs = (DWORD)(sleep_us / 1000);
            if (sleepMs > 1) {
                Sleep(sleepMs);
            } else {
                // 0 或 1 ms 的情況下使用 Sleep(0) 讓出處理器
                Sleep(0);
            }
        }
    }
}

bool EtherCATMaster::StartSlaveThread(uint16_t slaveIndex) {
    EnterCriticalSection(&cs);
    
    // Check if thread already exists for this slave
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex) {
            if (st.isRunning) {
                LeaveCriticalSection(&cs);
                return true; // Already running
            }
            // Thread exists but not running - clean up first
            if (st.thread) CloseHandle(st.thread);
            if (st.stopEvent) CloseHandle(st.stopEvent);
            st.thread = NULL;
            st.stopEvent = NULL;
        }
    }
    
    // Create new thread data
    SlaveThreadData data;
    data.slaveIndex = slaveIndex;
    data.master = this;
    data.stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    data.isRunning = false;
    data.thread = NULL;
    
    if (!data.stopEvent) {
        LeaveCriticalSection(&cs);
        return false;
    }
    
    // Store in vector first to get stable address
    bool found = false;
    SlaveThreadData* dataPtr = nullptr;
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex) {
            st = data;
            dataPtr = &st;
            found = true;
            break;
        }
    }
    if (!found) {
        slaveThreads.push_back(data);
        dataPtr = &slaveThreads.back();
    }
    
    // Now create thread with stable pointer from vector
    dataPtr->thread = CreateThread(NULL, 0, PerSlaveCyclicThreadFunc, dataPtr, 0, NULL);
    if (!dataPtr->thread) {
        CloseHandle(dataPtr->stopEvent);
        dataPtr->stopEvent = NULL;
        LeaveCriticalSection(&cs);
        return false;
    }
    
    dataPtr->isRunning = true;

    // Raise per-slave cyclic thread priority to improve PDO real-time responsiveness
    if (!SetThreadPriority(dataPtr->thread, THREAD_PRIORITY_HIGHEST)) {
        DWORD err = ::GetLastError();
        std::stringstream ss; ss << "SetThreadPriority for slave thread failed (err=" << err << ")";
        std::string msg = ss.str();
        printf("%s\n", msg.c_str());
    }

    // Bind cyclic thread to one available processor to avoid costly thread migration
    {
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            if (processMask != 0) {
                // choose a logical processor representing a physical core (avoid SMT siblings)
                auto getFirstPhysicalProcessor = [](DWORD_PTR availableMask)->DWORD_PTR {
                    DWORD_PTR result = 0;
                    DWORD len = 0;
                    BOOL ok = GetLogicalProcessorInformation(NULL, &len);
                    if (!ok && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
                        if (info) {
                            if (GetLogicalProcessorInformation(info, &len)) {
                                DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                                for (DWORD i = 0; i < count; ++i) {
                                    if (info[i].Relationship == RelationProcessorCore) {
                                        DWORD_PTR mask = info[i].ProcessorMask & availableMask;
                                        if (mask) {
                                            DWORD_PTR pick = mask & (~mask + 1);
                                            result |= pick;
                                        }
                                    }
                                }
                            }
                            free(info);
                        }
                    }
                    if (result == 0) {
                        if (availableMask) result = availableMask & (~availableMask + 1);
                    }
                    return result;
                };

                DWORD_PTR pick = getFirstPhysicalProcessor(processMask);
                // apply affinity to the created thread
                DWORD_PTR prev = SetThreadAffinityMask(dataPtr->thread, pick);
                if (prev == 0) {
                    DWORD err = ::GetLastError();
                    std::stringstream ss; ss << "SetThreadAffinityMask failed (err=" << err << ") pick=0x" << std::hex << pick;
                    std::string msg = ss.str();
                    printf("%s\n", msg.c_str());
                } else {
                    std::stringstream ss; ss << "Cyclic thread affinity set pick=0x" << std::hex << pick << " prev=0x" << prev;
                    std::string msg = ss.str();
                    printf("%s\n", msg.c_str());
                }
            } else {
                printf("GetProcessAffinityMask returned empty process mask\n");
            }
        } else {
            DWORD err = ::GetLastError();
            std::stringstream ss; ss << "GetProcessAffinityMask failed (err=" << err << ")";
            std::string msg = ss.str();
            printf("%s\n", msg.c_str());
        }
    }
    
    LeaveCriticalSection(&cs);
    return true;
}

void EtherCATMaster::StopSlaveThread(uint16_t slaveIndex) {
    EnterCriticalSection(&cs);
    
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex && st.isRunning) {
            SetEvent(st.stopEvent);
            LeaveCriticalSection(&cs);
            
            WaitForSingleObject(st.thread, 5000);
            
            EnterCriticalSection(&cs);
            CloseHandle(st.thread);
            CloseHandle(st.stopEvent);
            st.thread = NULL;
            st.stopEvent = NULL;
            st.isRunning = false;
            LeaveCriticalSection(&cs);
            return;
        }
    }
    
    LeaveCriticalSection(&cs);
}

bool EtherCATMaster::HasActiveSlaveThreads() const {
    // Check if global cyclic thread is running
    return isRunning;
}

void EtherCATMaster::StartPDOProcessingThread() {
    if (pdoThreadRunning) return;

    pdoStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (pdoStopEvent == NULL) {
        SetError("Failed to create PDO stop event");
        return;
    }

    pdoProcessingThread = CreateThread(NULL, 0, PDOProcessingThreadFunc, this, 0, NULL);
    if (pdoProcessingThread == NULL) {
        SetError("Failed to create PDO processing thread");
        CloseHandle(pdoStopEvent);
        pdoStopEvent = NULL;
        return;
    }

    // Set high priority for PDO processing thread (TIME_CRITICAL for real-time performance)
    if (!SetThreadPriority(pdoProcessingThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        DWORD err = ::GetLastError();
        std::stringstream ss; ss << "SetThreadPriority(PDO) failed (err=" << err << ")";
        printf("%s\n", ss.str().c_str());
    }

    // Bind PDO thread to one available processor to avoid costly thread migration
    {
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            if (processMask != 0) {
                // choose a logical processor that represents a physical core (avoid SMT siblings)
                // pick the first logical processor of the first available physical core
                auto getFirstPhysicalProcessor = [](DWORD_PTR availableMask)->DWORD_PTR {
                    DWORD_PTR result = 0;
                    // Query logical processor information to find processor cores
                    DWORD len = 0;
                    BOOL ok = GetLogicalProcessorInformation(NULL, &len);
                    if (!ok && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
                        if (info) {
                            if (GetLogicalProcessorInformation(info, &len)) {
                                DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                                for (DWORD i = 0; i < count; ++i) {
                                    if (info[i].Relationship == RelationProcessorCore) {
                                        DWORD_PTR mask = info[i].ProcessorMask & availableMask;
                                        if (mask) {
                                            // pick lowest set bit in this core's mask
                                            DWORD_PTR pick = mask & (~mask + 1);
                                            result |= pick;
                                        }
                                    }
                                }
                            }
                            free(info);
                        }
                    }
                    if (result == 0) {
                        // fallback: pick lowest available logical processor
                        if (availableMask) result = availableMask & (~availableMask + 1);
                    }
                    return result;
                };

                DWORD_PTR pick = getFirstPhysicalProcessor(processMask);
                DWORD_PTR prev = SetThreadAffinityMask(pdoProcessingThread, pick);
                if (prev == 0) {
                    DWORD err = ::GetLastError();
                    std::stringstream ss; ss << "SetThreadAffinityMask(PDO) failed (err=" << err << ") pick=0x" << std::hex << pick;
                    printf("%s\n", ss.str().c_str());
                } else {
                    std::stringstream ss; ss << "PDO thread affinity set pick=0x" << std::hex << pick << " prev=0x" << prev;
                    printf("%s\n", ss.str().c_str());
                }
            } else {
                printf("GetProcessAffinityMask returned empty process mask for PDO thread\n");
            }
        } else {
            DWORD err = ::GetLastError();
            std::stringstream ss; ss << "GetProcessAffinityMask(PDO) failed (err=" << err << ")";
            printf("%s\n", ss.str().c_str());
        }
    }

    // 記錄PDO操作開始時間
    operationStartTime = GetTickCount64();
    
    pdoThreadRunning = true;
}

void EtherCATMaster::StopPDOProcessingThread() {
    if (!pdoThreadRunning) return;

    SetEvent(pdoStopEvent);

    if (pdoProcessingThread != NULL) {
        WaitForSingleObject(pdoProcessingThread, INFINITE);
        CloseHandle(pdoProcessingThread);
        pdoProcessingThread = NULL;
    }

    if (pdoStopEvent != NULL) {
        CloseHandle(pdoStopEvent);
        pdoStopEvent = NULL;
    }

    pdoThreadRunning = false;

    // Clear PDO data snapshots
    EnterCriticalSection(&pdoDataCS);
    pdoDataSnapshots.clear();
    pendingPDOOutputs.clear();
    LeaveCriticalSection(&pdoDataCS);
}

DWORD WINAPI EtherCATMaster::PDOProcessingThreadFunc(LPVOID param) {
    EtherCATMaster* master = static_cast<EtherCATMaster*>(param);
    master->PDOProcessingTask();
    return 0;
}

void EtherCATMaster::PDOProcessingTask() {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    uint32_t cycleMicros = desiredCycleTime; // Use same cycle time as cyclic task for consistency
    if (cycleMicros == 0) cycleMicros = 2000; // Default 2ms if not set

    while (WaitForSingleObject(pdoStopEvent, 0) != WAIT_OBJECT_0) {
        QueryPerformanceCounter(&start);

        if (!isRunning || !isInitialized) {
            Sleep(10);
            continue;
        }

        // Apply staged output byte writes (if any) into IOmap under lock
        EnterCriticalSection(&pdoDataCS);
        if (!stagedOutputs.empty()) {
            for (const auto &entry : stagedOutputs) {
                int absOffset = entry.first;
                const std::vector<uint8_t> &data = entry.second;
                if (absOffset >= 0 && absOffset < (int)sizeof(IOmap)) {
                    int maxWrite = (int)sizeof(IOmap) - absOffset;
                    int dataSize = (int)data.size();
                    int toWrite = (dataSize < maxWrite) ? dataSize : maxWrite;
                    if (toWrite > 0) {
                        memcpy(IOmap + absOffset, data.data(), toWrite);
                    }
                }
            }
            // Keep stagedOutputs persistent (they represent desired continuous outputs).
            // If we wanted single-shot writes, we would clear them here.
        }
        // existing pendingPDOOutputs is legacy/name-based queue; process as before for backward compatibility
        for (const auto& output : pendingPDOOutputs) {
            const std::string& itemName = output.first;
            int slaveIndex = output.second.first;
            int32_t value = output.second.second;
            // Legacy behavior: try best-effort write based on object name (kept for compatibility)
            uint16_t objectIndex = 0;
            if (itemName.find("0x6040") != std::string::npos) objectIndex = 0x6040;
            else if (itemName.find("0x607A") != std::string::npos) objectIndex = 0x607A;
            else if (itemName.find("0x6081") != std::string::npos) objectIndex = 0x6081;
            else if (itemName.find("0x60FF") != std::string::npos) objectIndex = 0x60FF;
            else if (itemName.find("0x6071") != std::string::npos) objectIndex = 0x6071;
            else if (itemName.find("0x6083") != std::string::npos) objectIndex = 0x6083;
            else if (itemName.find("0x6084") != std::string::npos) objectIndex = 0x6084;
            else if (itemName.find("0x6060") != std::string::npos) objectIndex = 0x6060;

            if (objectIndex != 0 && slaveIndex > 0 && slaveIndex <= slaveCount) {
                int outBase = 0, inBase = 0;
                if (GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
                    bool is16bit = (objectIndex == 0x6040 || objectIndex == 0x6060);
                    int writeSize = is16bit ? 2 : 4;
                    int writeOffset = outBase;
                    if (writeOffset + writeSize <= sizeof(IOmap)) {
                        if (is16bit) {
                            int16_t val16 = (int16_t)value;
                            memcpy(IOmap + writeOffset, &val16, 2);
                        } else {
                            int32_t val32 = value;
                            memcpy(IOmap + writeOffset, &val32, 4);
                        }
                    }
                }
            }
        }
        pendingPDOOutputs.clear();
        LeaveCriticalSection(&pdoDataCS);

        // Create snapshots of current IOmap data for all slaves
        EnterCriticalSection(&pdoDataCS);
        for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
            std::vector<uint8_t> snapshot;
            int outBase = 0, inBase = 0;
            if (GetSlaveIOBaseBytes(slaveIdx, outBase, inBase)) {
                // Copy slave's IO data (simplified - copy a reasonable amount)
                int outputSize = context.slavelist[slaveIdx].Obytes;
                int inputSize = context.slavelist[slaveIdx].Ibytes;
                int totalSize = outputSize + inputSize;

                if (totalSize > 0 && totalSize <= 1024) { // Reasonable limit
                    snapshot.resize(totalSize);
                    if (outBase + outputSize <= sizeof(IOmap)) {
                        memcpy(snapshot.data(), IOmap + outBase, outputSize);
                    }
                    if (inBase + inputSize <= sizeof(IOmap)) {
                        memcpy(snapshot.data() + outputSize, IOmap + inBase, inputSize);
                    }
                }
            }
            pdoDataSnapshots[slaveIdx] = std::move(snapshot);
        }
        LeaveCriticalSection(&pdoDataCS);

        // High-precision timing to maintain consistent cycle time
        QueryPerformanceCounter(&end);
        LONGLONG elapsed_us = ((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart;
        LONGLONG remaining_us = (LONGLONG)cycleMicros - elapsed_us;
        if (remaining_us > 0) {
            // Windows high-resolution Waitable Timer solution (Win10 1803+)
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION provides microsecond precision with low CPU usage
            static HANDLE hTimer = NULL;
            static bool timerInitFailed = false;
            
            if (hTimer == NULL && !timerInitFailed) {
                // Create high-resolution timer (initialize once)
                hTimer = CreateWaitableTimerExW(
                    NULL,                                   // default security attributes
                    NULL,                                   // unnamed
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,  // high-resolution flag
                    TIMER_ALL_ACCESS
                );
                
                if (hTimer == NULL) {
                    // High-resolution timer not supported (Windows 10 < 1803)
                    timerInitFailed = true;
                }
            }
            
            if (hTimer != NULL) {
                // Use high-resolution timer to wait
                // Negative value indicates relative time, units of 100 nanoseconds
                LARGE_INTEGER dueTime;
                dueTime.QuadPart = -(remaining_us * 10LL);  // convert microseconds to 100ns
                
                if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    // Wait for timer or stop event
                    HANDLE events[2] = { hTimer, pdoStopEvent };
                    DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    
                    if (result == WAIT_OBJECT_0 + 1) {
                        // pdoStopEvent triggered
                        break;
                    }
                    // result == WAIT_OBJECT_0: timer expired normally
                }
            } else {
                // Fallback solution: Sleep(1) mixed with busy-wait
                // For systems that don't support high-resolution timers
                if (remaining_us > 1500) {
                    Sleep(1);  // coarse wait
                } else {
                    // Short busy-wait
                    QueryPerformanceCounter(&end);
                    long long target = start.QuadPart + ((int64_t)cycleMicros * frequency.QuadPart) / 1000000;
                    while (end.QuadPart < target) {
                        YieldProcessor();
                        QueryPerformanceCounter(&end);
                    }
                }
            }
        }
    }
}

bool EtherCATMaster::GetPDODataSnapshot(int slaveIndex, std::vector<uint8_t>& snapshot) {
    EnterCriticalSection(&pdoDataCS);
    auto it = pdoDataSnapshots.find(slaveIndex);
    if (it != pdoDataSnapshots.end()) {
        snapshot = it->second;
        LeaveCriticalSection(&pdoDataCS);
        return true;
    }
    LeaveCriticalSection(&pdoDataCS);
    return false;
}

bool EtherCATMaster::WritePDOData(int slaveIndex, const std::string& itemName, int32_t value) {
    EnterCriticalSection(&pdoDataCS);
    pendingPDOOutputs[itemName] = std::make_pair(slaveIndex, value);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}

bool EtherCATMaster::WritePDODataOverwrite(int slaveIndex, const std::string& itemName, int32_t value) {
    // Explicitly overwrite any pending entry for itemName
    EnterCriticalSection(&pdoDataCS);
    pendingPDOOutputs[itemName] = std::make_pair(slaveIndex, value);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}
