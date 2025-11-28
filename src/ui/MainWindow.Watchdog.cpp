#include "MainWindow.h"
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <utility>
#include <unordered_map>
#include <array>
#include <map>
#include <future>
#include "Win32Utils.h"
#include "HttpServer.h"
#include "../app/AppController.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../include/EtherCATMaster.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
#include "../include/MotionLogic.h"
#include "../include/AutoWindow.h"

namespace {

constexpr uint16_t kCommunicationFailureSentinel = 0xFFFF;

}

void MainWindow::StartWatchdogRecovery() {
    // bump priority for recovery
    DWORD prevPriority = GetPriorityClass(GetCurrentProcess());
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    int prevThreadPrio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    PostUTF8LogToUI(std::string("Watchdog triggered: starting automatic recovery...\n"));
    // Stop cyclic operation
    MasterStopCyclicOperation();
    PostUTF8LogToUI(std::string("Stopped cyclic operation\n"));

    const int count = MasterGetSlaveCount();
    int failedSlaveCount = 0;
    {
        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
        for (int slaveIndex = 1; slaveIndex <= count; ++slaveIndex) {
            const auto reportedIt = lastReportedErrorCode.find(slaveIndex);
            if (reportedIt != lastReportedErrorCode.end() && reportedIt->second == kCommunicationFailureSentinel) {
                ++failedSlaveCount;
                continue;
            }

            const auto counterIt = errorCodeConsecutiveCounter.find(slaveIndex);
            if (counterIt != errorCodeConsecutiveCounter.end() && counterIt->second >= errorCodeConsecutiveThreshold) {
                ++failedSlaveCount;
            }
        }
    }

    bool linkUp = true;
    if (!currentIfName.empty() && !EtherCATMaster::IsSimulationAdapterSelection(currentIfName)) {
        try {
            linkUp = CoreHelpers::CheckLinkStatus(currentIfName.c_str());
        } catch (...) {}
    }

    const ULONGLONG lastSdoFailureTick = lastSdo60FDFailureTick.load();
    const ULONGLONG nowTick = GetTickCount64();
    const int recentSdoFailureStreak = sdo60FDFailureStreak.load();
    const bool recentSdoFailureActive = (lastSdoFailureTick != 0) && (nowTick >= lastSdoFailureTick)
        && ((nowTick - lastSdoFailureTick) <= SDO_60FD_FAILURE_WINDOW_MS);

    const auto recoveryDecision = CoreHelpers::EvaluateWatchdogRecoveryDecision({
        failedSlaveCount,
        count,
        linkUp,
        anySlaveHasError.load(),
        recentSdoFailureStreak,
        recentSdoFailureActive
    });

    if (recoveryDecision == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately) {
        std::ostringstream oss;
        if (!linkUp) {
            oss << "Watchdog: adapter link down detected - skipping recovery and disconnecting immediately\n";
        } else if (recentSdoFailureActive && recentSdoFailureStreak >= 3) {
            oss << "Watchdog: repeated 0x60FD read failures detected (streak=" << recentSdoFailureStreak
                << ") - skipping recovery and disconnecting immediately\n";
        } else {
            oss << "Watchdog: severe communication loss detected (" << failedSlaveCount << "/" << count
                << " slaves failing 0x603F) - skipping recovery and disconnecting immediately\n";
        }
        const std::string disconnectMessage = oss.str();
        PostUTF8LogToUI(disconnectMessage);
        if (IsAutoProcedureRunning()) {
            PostUTF8LogToUI(std::string("[AutoStop] watchdog detected severe communication loss - 先執行自動程序停止\n"));
            StopAutoProcedure();
        }
        RequestAutomaticDisconnect(disconnectMessage.c_str());
        watchdogCounter.store(0);
        recoveryRunning.store(false);
        SetPriorityClass(GetCurrentProcess(), prevPriority);
        SetThreadPriority(GetCurrentThread(), prevThreadPrio);
        return;
    }

    // Force runtime init for all slaves
    for (int i = 1; i <= count; ++i) {
        std::string slog;
        PostUTF8LogToUI(std::string("Recovery: ForceSlaveOperational for slave ") + std::to_string(i) + "\n");
        MasterForceSlaveOperational(i, slog);
        PostUTF8LogToUI(slog);
        // If the slave fails to reach OP, interpret "Runtime configuration failed"
        // as a communications error and trigger a clean UI-driven disconnect.
        try {
            if (slog.find("Runtime configuration failed") != std::string::npos) {
                PostUTF8LogToUI(std::string("Recovery: detected runtime configuration failure - triggering disconnect\n"));
                // 通訊異常：先停止自動程序，再斷開連接
                if (IsAutoProcedureRunning()) {
                    PostUTF8LogToUI(std::string("[AutoStop] 從站配置失敗 - 先執行自動程序停止\n"));
                    StopAutoProcedure();
                }
                // Perform safe UI-driven disconnect and abort recovery
                RequestAutomaticDisconnect("Recovery: detected runtime configuration failure - issuing disconnect\n");
                watchdogCounter.store(0);
                recoveryRunning.store(false);
                SetPriorityClass(GetCurrentProcess(), prevPriority);
                SetThreadPriority(GetCurrentThread(), prevThreadPrio);
                return;
            }
        } catch(...) {}
    }

    // Re-configure and restart cyclic
    PostUTF8LogToUI(std::string("Recovery: retry ConfigureSlaves()\n"));
    bool ok = MasterConfigureSlaves();
    if (!ok) {
        PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() failed\n"));
        // 通訊異常：先停止自動程序，再斷開連接
        if (IsAutoProcedureRunning()) {
            PostUTF8LogToUI(std::string("[AutoStop] 從站重新配置失敗 - 先執行自動程序停止\n"));
            StopAutoProcedure();
        }
        // If we cannot reconfigure the slaves, perform a safe UI-driven disconnect
        RequestAutomaticDisconnect("Recovery: ConfigureSlaves() failed - issuing disconnect\n");
        watchdogCounter.store(0);
        recoveryRunning.store(false);
        // early return since there's nothing left we can do
        SetPriorityClass(GetCurrentProcess(), prevPriority);
        SetThreadPriority(GetCurrentThread(), prevThreadPrio);
        return;
    }
    else PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() succeeded\n"));

    PostUTF8LogToUI(std::string("Recovery: restarting cyclic operation\n"));
    if (!MasterStartCyclicOperation()) {
        std::string err = MasterGetLastError();
        PostUTF8LogToUI(std::string("Recovery: StartCyclicOperation failed: ") + err + "\n");
        // 通訊異常：先停止自動程序，再斷開連接
        if (IsAutoProcedureRunning()) {
            PostUTF8LogToUI(std::string("[AutoStop] 循環操作恢復失敗 - 先執行自動程序停止\n"));
            StopAutoProcedure();
        }
        // Unable to restore cyclic operation; force a UI disconnect to reset state
        RequestAutomaticDisconnect("Recovery: StartCyclicOperation failed - issuing disconnect\n");
    } else {
        PostUTF8LogToUI(std::string("Recovery: StartCyclicOperation succeeded\n"));
        NotifyEthercatError(false);
    }

    watchdogCounter.store(0);
    recoveryRunning.store(false);
    // restore priorities
    SetThreadPriority(GetCurrentThread(), prevThreadPrio);
    SetPriorityClass(GetCurrentProcess(), prevPriority);
}

// -----------------------------------------------------------------------------
// ESI Dump logic (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------

void MainWindow::StartErrorCodeCheck()
{
    if (errorCodeCheckRunning.load()) {
        return; // 已在運行
    }
    
    errorCodeCheckRunning.store(true);
    currentErrorCheckSlaveIndex.store(1);
    anySlaveHasError.store(false);
    {
        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
        lastReportedErrorCode.clear(); // 清空上次記錄
        errorCodeConsecutiveCounter.clear();
    }
    
    // 清除之前可能遺留的全域錯誤標誌（0x603F 非零不代表網路故障）
    if (auto* master = AppController::GetInstance().GetMaster()) {
        master->ClearGlobalError();
    }
    
    PostUTF8LogToUI(std::string("[ErrorCheck] 啟動 0x603F 循環檢查 (每50ms檢查一站)\n"));
    
    errorCodeCheckThread = std::thread([this]() {
        const auto waitForNextTick = [this](std::chrono::milliseconds delay) {
            std::unique_lock<std::mutex> wakeLock(errorCodeCheckWakeMutex);
            return errorCodeCheckWakeCv.wait_for(
                wakeLock,
                delay,
                [this]() { return !errorCodeCheckRunning.load(); });
        };
        constexpr auto kActivePollInterval = std::chrono::milliseconds(50);
        constexpr auto kIdlePollInterval = std::chrono::milliseconds(200);

        while (errorCodeCheckRunning.load()) {
            auto* master = AppController::GetInstance().GetMaster();
            if (!master || !master->IsInitialized()) {
                // EtherCAT 未初始化,等待後繼續
                if (waitForNextTick(kIdlePollInterval)) {
                    break;
                }
                continue;
            }
            
            int slaveCount = master->GetSlaveCount();
            if (slaveCount <= 0) {
                if (waitForNextTick(kIdlePollInterval)) {
                    break;
                }
                continue;
            }
            // Consolidated tick: drive EtherCATMaster's SDO reader from the same
            // error-check thread so we have a single periodic task. The
            // SdoReaderTick() is responsible for enforcing its own period
            // (e.g. 100ms) so calling it here at 50ms intervals is safe.
            try {
                master->SdoReaderTick();
            } catch (...) {}
            
            // 取得當前要檢查的從站索引
            int currentSlave = currentErrorCheckSlaveIndex.load();
            if (currentSlave > slaveCount) {
                currentSlave = 1; // 循環回第一個從站
                currentErrorCheckSlaveIndex.store(1);
            }
            
            // 讀取該從站的 0x603F Error Code
            uint16_t errorCode = 0;
            int readSize = sizeof(errorCode);
            // Read 0x603F with shortened timeout (100ms) to detect failures faster
            const int READ_603F_TIMEOUT_US = 100000; // 100 ms in microseconds
            bool readSuccess = MasterReadSDO((uint16_t)currentSlave, (uint16_t)0x603F, (uint8_t)0x00, 
                                            (uint8_t*)&errorCode, readSize, READ_603F_TIMEOUT_US);
            
            if (readSuccess) {
                bool errorCodeChanged = false;
                bool hadPreviousError = false;
                {
                    std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                    auto it = lastReportedErrorCode.find(currentSlave);
                    errorCodeChanged = (it == lastReportedErrorCode.end()) || (it->second != errorCode);
                    hadPreviousError = (it != lastReportedErrorCode.end() && it->second != 0);
                    lastReportedErrorCode[currentSlave] = errorCode;
                    errorCodeConsecutiveCounter[currentSlave] = 0;
                }

                if (errorCode != 0) {
                    // 0x603F 非零表示從站內部錯誤狀態（需要故障重置）。
                    // 這在連線/啟動流程中可能是正常現象（不應導致自動斷線），
                    // 但仍應在 UI 上提示使用者（AutoWindow 的 oError 應顯示）。
                    //
                    // 因此：只記錄日誌並設置 `anySlaveHasInternalError` 以供 UI 顯示，
                    // 不設定 EtherCAT 全域錯誤，也不觸發自動斷線流程。
                    bool prevWasInternal = this->anySlaveHasInternalError.load();
                    // 只有錯誤碼變化時才顯示日誌
                    if (errorCodeChanged) {
                        char hexBuf[16];
                        snprintf(hexBuf, sizeof(hexBuf), "0x%04X", errorCode);
                        std::string msg = "從站 " + std::to_string(currentSlave) + 
                                         " 0x603F 異常代碼 " + std::string(hexBuf) + " (內部狀態，需故障重置)\n";
                        PostUTF8LogToUI(msg);
                    }
                    // 標記 UI 可見的內部錯誤（不表示通訊異常，故不呼叫 SetGlobalError）
                    this->anySlaveHasInternalError.store(true);
                    // 由無到有時觸發 AutoWindow 更新以顯示 oError
                    if (!prevWasInternal) {
                        PostAutoStatusUpdate();
                    }
                } else {
                    // 錯誤碼為 0 (正常),如果之前有錯誤,顯示恢復訊息
                    if (hadPreviousError) {
                        std::string msg = "從站 " + std::to_string(currentSlave) + 
                                         " 0x603F 異常已清除\n";
                        PostUTF8LogToUI(msg);
                    }

                    // 檢查是否所有從站都沒有錯誤碼，若無則清除對應的狀態旗標
                    bool anyNonZero = false;
                    {
                        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                        for (const auto &p : lastReportedErrorCode) {
                            if (p.second != 0) { anyNonZero = true; break; }
                        }
                    }
                    // 如果沒有任何非零錯誤碼，清除內部錯誤 UI 標誌與（如適用）通訊錯誤標誌。
                    if (!anyNonZero) {
                        // Clear UI-visible internal error flag if it was set
                        if (this->anySlaveHasInternalError.load()) {
                            this->anySlaveHasInternalError.store(false);
                            PostAutoStatusUpdate();
                        }
                        // If a communications-induced error flag was set, clear it and the master global error
                        if (anySlaveHasError.load()) {
                            anySlaveHasError.store(false);
                            if (auto* clearMaster = AppController::GetInstance().GetMaster()) {
                                clearMaster->ClearGlobalError();
                            }
                            NotifyEthercatError(false);
                            PostAutoStatusUpdate();
                        }
                    }
                }
            } else {
                // SDO 讀取失敗: 這才是真正的通訊異常（網路故障）
                bool alreadyReportedSentinel = false;
                bool prevWasError = anySlaveHasError.load();
                // 增加讀取失敗的連續計數器
                int cnt2 = 0;
                {
                    std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                    auto it2 = lastReportedErrorCode.find(currentSlave);
                    alreadyReportedSentinel = (it2 != lastReportedErrorCode.end() && it2->second == kCommunicationFailureSentinel);
                    auto itc2 = errorCodeConsecutiveCounter.find(currentSlave);
                    if (itc2 == errorCodeConsecutiveCounter.end()) {
                        errorCodeConsecutiveCounter[currentSlave] = 1;
                        cnt2 = 1;
                    } else {
                        cnt2 = ++errorCodeConsecutiveCounter[currentSlave];
                    }
                }
                
                // 只有達到連續失敗閾值時，才判定為通訊異常
                if (cnt2 >= errorCodeConsecutiveThreshold) {
                    {
                        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                        lastReportedErrorCode[currentSlave] = kCommunicationFailureSentinel;
                    }
                    anySlaveHasError.store(true);
                    if (auto* setMaster = AppController::GetInstance().GetMaster()) { setMaster->SetGlobalError(); }
                }

                // 顯示讀取失敗訊息
                if (!alreadyReportedSentinel && cnt2 >= errorCodeConsecutiveThreshold) {
                    std::string msg = "[ErrorCheck] 從站 " + std::to_string(currentSlave) +
                                     " 讀取 0x603F 失敗 (網路通訊異常) - 持續 " + std::to_string(cnt2) + " 次\n";
                    PostUTF8LogToUI(msg);
                    // 若先前沒有錯誤，更新 UI 狀態
                    if (!prevWasError) {
                        PostAutoStatusUpdate();
                    }
                } else if (cnt2 < errorCodeConsecutiveThreshold) {
                    // 讀取失敗但未達閾值，顯示過渡訊息
                    std::string msg = "[ErrorCheck] 從站 " + std::to_string(currentSlave) +
                                     " 讀取 0x603F 失敗 (暫時) - 計數 " + std::to_string(cnt2) + "/" + std::to_string(errorCodeConsecutiveThreshold) + "\n";
                    PostUTF8LogToUI(msg);
                }
            }
            
            // 移動到下一個從站
            currentErrorCheckSlaveIndex.store(currentSlave + 1);
            
            // 等待 50ms 後檢查下一個從站
            if (waitForNextTick(kActivePollInterval)) {
                break;
            }
        }
    });
}


void MainWindow::StopErrorCodeCheck()
{
    if (!errorCodeCheckRunning.load()) {
        return; // 未在運行
    }
    
    PostUTF8LogToUI(std::string("[ErrorCheck] 停止 0x603F 循環檢查\n"));
    
    errorCodeCheckRunning.store(false);
    errorCodeCheckWakeCv.notify_all();

    // Joining the errorCodeCheckThread may block for a short while; to avoid
    // freezing the UI thread, perform the join in a background thread if
    // we're currently on the UI thread. This keeps UI interactions responsive.
    std::thread localJoin;
    if (errorCodeCheckThread.joinable()) {
        localJoin = std::move(errorCodeCheckThread);
        if (GetCurrentThreadId() == uiThreadId) {
            std::thread([t = std::move(localJoin)]() mutable {
                try { if (t.joinable()) t.join(); } catch(...) {}
            }).detach();
        } else {
            try { if (localJoin.joinable()) localJoin.join(); } catch(...) {}
        }
    }

    anySlaveHasError.store(false);
    {
        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
        errorCodeConsecutiveCounter.clear();
        lastReportedErrorCode.clear();
    }
    PostAutoStatusUpdate(); // 更新狀態
}

// ============================================================================
// TriggerAutoStartupSequence: 程式啟動時沒有參數，自動執行連接和自動程序啟動
// ============================================================================


