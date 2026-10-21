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

    // ── Track recovery attempt for escalation ──
    {
        const ULONGLONG nowTick = GetTickCount64();
        const ULONGLONG winStart = recoveryWindowStartTick.load();
        if (winStart == 0 || nowTick < winStart || (nowTick - winStart) > WATCHDOG_RECOVERY_WINDOW_MS) {
            // New window
            recoveryWindowStartTick.store(nowTick);
            recoveryAttemptCount.store(1);
        } else {
            recoveryAttemptCount.fetch_add(1);
        }
    }

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
    // Snapshot once: the selection cache is shared with other threads.
    const std::string watchdogIfName = this->currentIfName.Copy();
    if (!watchdogIfName.empty() && !EtherCATMaster::IsSimulationAdapterSelection(watchdogIfName)) {
        try {
            linkUp = CoreHelpers::CheckLinkStatus(watchdogIfName.c_str());
        } catch (...) {}
    }

    const ULONGLONG lastPdoFailureTick = lastPdo60FDFailureTick.load();
    const ULONGLONG nowTick = GetTickCount64();
    const int recentPdoFailureStreak = pdo60FDFailureStreak.load();
    const bool recentPdoFailureActive = (lastPdoFailureTick != 0) && (nowTick >= lastPdoFailureTick)
        && ((nowTick - lastPdoFailureTick) <= SDO_60FD_FAILURE_WINDOW_MS);

    // EC_NOFRAME (SOEM WKC < 0) means the NIC got nothing back — no slave is
    // reachable, so ForceSlaveOperational/ConfigureSlaves would only block the
    // master lock for tens of seconds doing doomed SDO round-trips and freeze
    // the UI (disconnect button becomes unresponsive).  Detect it and go
    // straight to disconnect.
    bool noFrame = false;
    if (auto* wkcMaster = AppController::GetInstance().GetMaster()) {
        noFrame = (wkcMaster->GetWorkingCounterRaw() < 0);
    }

    const auto recoveryDecision = CoreHelpers::EvaluateWatchdogRecoveryDecision({
        failedSlaveCount,
        count,
        linkUp,
        anySlaveHasError.load(),
        recentPdoFailureStreak,
        recentPdoFailureActive,
        noFrame
    });

    if (recoveryDecision == CoreHelpers::WatchdogRecoveryDecision::DisconnectImmediately) {
        std::ostringstream oss;
        if (!linkUp) {
            oss << "Watchdog: adapter link down detected - skipping recovery and disconnecting immediately\n";
        } else if (noFrame) {
            oss << "Watchdog: EC_NOFRAME (NIC received no EtherCAT frame) - skipping recovery and disconnecting immediately\n";
        } else if (recentPdoFailureActive && recentPdoFailureStreak >= 3) {
            oss << "Watchdog: repeated 0x60FD read failures detected (streak=" << recentPdoFailureStreak
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

    // ── Fast path: if all slaves are already OP with no AL errors, skip the
    // entire ForceSlaveOperational + ConfigureSlaves rebuild and restart
    // cyclic directly.  This saves ~6s (264 SDO round-trips) and also avoids
    // the per-slave ForceSlaveOperational overhead when healthy. ──
    bool allSlavesHealthy = MasterAllSlavesOperationalNoAlarm();
    bool ok = true;
    if (allSlavesHealthy) {
        PostUTF8LogToUI(std::string("Recovery: all slaves OP+no-alarm — fast-restarting cyclic\n"));
    } else {
        // Force runtime init for all slaves
        bool forceFailed = false;
        for (int i = 1; i <= count; ++i) {
            std::string slog;
            PostUTF8LogToUI(std::string("Recovery: ForceSlaveOperational for slave ") + std::to_string(i) + "\n");
            MasterForceSlaveOperational(i, slog);
            PostUTF8LogToUI(slog);
            try {
                if (slog.find("Runtime configuration failed") != std::string::npos) {
                    PostUTF8LogToUI(std::string("Recovery: detected runtime configuration failure - triggering disconnect\n"));
                    if (IsAutoProcedureRunning()) {
                        PostUTF8LogToUI(std::string("[AutoStop] 從站配置失敗 - 先執行自動程序停止\n"));
                        StopAutoProcedure();
                    }
                    RequestAutomaticDisconnect("Recovery: detected runtime configuration failure - issuing disconnect\n");
                    watchdogCounter.store(0);
                    recoveryRunning.store(false);
                    SetPriorityClass(GetCurrentProcess(), prevPriority);
                    SetThreadPriority(GetCurrentThread(), prevThreadPrio);
                    return;
                }
            } catch(...) {}
        }

        if (!forceFailed) {
            PostUTF8LogToUI(std::string("Recovery: retry ConfigureSlaves()\n"));
            ok = MasterConfigureSlaves();
        }
    }

    if (!ok) {
        PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() failed\n"));
        if (IsAutoProcedureRunning()) {
            PostUTF8LogToUI(std::string("[AutoStop] 從站重新配置失敗 - 先執行自動程序停止\n"));
            StopAutoProcedure();
        }
        RequestAutomaticDisconnect("Recovery: ConfigureSlaves() failed - issuing disconnect\n");
        watchdogCounter.store(0);
        recoveryRunning.store(false);
        SetPriorityClass(GetCurrentProcess(), prevPriority);
        SetThreadPriority(GetCurrentThread(), prevThreadPrio);
        return;
    }
    if (!allSlavesHealthy) {
        PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() succeeded\n"));
    }

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
        // ── Set cooldown to prevent immediate re-trigger ──
        watchdogCooldownUntil.store(GetTickCount64() + WATCHDOG_COOLDOWN_MS);
        // Reset escalation window on successful recovery
        recoveryAttemptCount.store(0);
        recoveryWindowStartTick.store(0);
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
    
    PostUTF8LogToUI(std::string("[ErrorCheck] 啟動 PDO 0x603F 錯誤碼循環檢查 (每50ms檢查一站, PDO cache)\n"));
    
    if (errorCodeCheckThread.joinable()) {
        errorCodeCheckThread.join();
    }
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
            // PDO-based monitoring: 0x60FD is now updated every cycle from PDO
            // and available via GetPdo60FDLowByte(). External auto-input
            // processing uses PollExternalAutoInputsPdo() which reads directly
            // from the atomic PDO cache (no SDO dependency).
            try {
                this->PollExternalAutoInputsPdo();
            } catch (...) {}
            
            // 取得當前要檢查的從站索引
            int currentSlave = currentErrorCheckSlaveIndex.load();
            if (currentSlave > slaveCount) {
                currentSlave = 1; // 循環回第一個從站
                currentErrorCheckSlaveIndex.store(1);
            }
            
            // Read 0x603F Error Code from per-slave PDO cache (lock-free, updated every cycle)
            uint16_t errorCode = 0;
            bool readSuccess = master->GetPdo603F(static_cast<uint16_t>(currentSlave), errorCode);
            
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
                    // 這在連線/啟動流程中可能是正常現象，故不應觸發自動斷線。
                    // 但必須立即停止運動以確保安全。
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
                    // 由無到有時觸發立即停止與 AutoWindow 更新
                    if (!prevWasInternal) {
                        if (!this->suppressAutoStopDuringStartup_.load()) {
                            // 非零 0x603F → 立即停止運動，但不自動斷線
                            PostMessageW(this->GetHwnd(),
                                         WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                                         (WPARAM)0,
                                         AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
                        }
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
                // PDO cache 不可用 (pdoCacheSlaveCount_==0 或 pdo603FCache_==null)。
                // 嘗試 SDO fallback 讀取 0x603F，避免因本地快取問題誤判為通訊異常。
                bool sdoFallbackOk = false;
                uint16_t sdoErrorCode = 0;
                {
                    uint16_t sdoSlave = static_cast<uint16_t>(currentSlave);
                    int sdoSize = 2;
                    sdoFallbackOk = master->ReadSDO(sdoSlave, 0x603F, 0x00, &sdoErrorCode, sdoSize);
                }
                if (sdoFallbackOk) {
                    // SDO fallback 成功：當作正常 PDO 讀取處理
                    bool errorCodeChanged2 = false;
                    bool hadPreviousError2 = false;
                    {
                        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                        auto it = lastReportedErrorCode.find(currentSlave);
                        errorCodeChanged2 = (it == lastReportedErrorCode.end()) || (it->second != sdoErrorCode);
                        hadPreviousError2 = (it != lastReportedErrorCode.end() && it->second != 0);
                        lastReportedErrorCode[currentSlave] = sdoErrorCode;
                        errorCodeConsecutiveCounter[currentSlave] = 0;
                    }
                    // 重置 anySlaveHasError（此為 PDO cache 問題，非通訊異常）
                    if (anySlaveHasError.load()) {
                        anySlaveHasError.store(false);
                        if (auto* clearMaster2 = AppController::GetInstance().GetMaster()) {
                            clearMaster2->ClearGlobalError();
                        }
                    }
                    if (sdoErrorCode != 0) {
                        bool prevWasInternal2 = this->anySlaveHasInternalError.load();
                        if (errorCodeChanged2) {
                            char hexBuf[16];
                            snprintf(hexBuf, sizeof(hexBuf), "0x%04X", sdoErrorCode);
                            std::string msg = "從站 " + std::to_string(currentSlave) +
                                             " 0x603F 異常代碼 " + std::string(hexBuf) +
                                             " (SDO fallback，需故障重置)\n";
                            PostUTF8LogToUI(msg);
                        }
                        this->anySlaveHasInternalError.store(true);
                        if (!prevWasInternal2) {
                            if (!this->suppressAutoStopDuringStartup_.load()) {
                                PostMessageW(this->GetHwnd(),
                                             WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                                             (WPARAM)0,
                                             AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
                            }
                            PostAutoStatusUpdate();
                        }
                    } else {
                        if (hadPreviousError2) {
                            std::string msg = "從站 " + std::to_string(currentSlave) +
                                             " 0x603F 異常已清除 (SDO fallback)\n";
                            PostUTF8LogToUI(msg);
                        }
                        bool anyNonZero2 = false;
                        {
                            std::lock_guard<std::mutex> lock2(errorCodeStateMutex);
                            for (const auto &p : lastReportedErrorCode) {
                                if (p.second != 0) { anyNonZero2 = true; break; }
                            }
                        }
                        if (!anyNonZero2 && this->anySlaveHasInternalError.load()) {
                            this->anySlaveHasInternalError.store(false);
                            PostAutoStatusUpdate();
                        }
                    }
                    // SDO fallback 成功，跳過後續的 PDO 失敗處理
                } else {
                // PDO + SDO 皆失敗：判定為通訊異常
                bool alreadyReportedSentinel = false;
                bool prevWasError = anySlaveHasError.load();
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

                if (cnt2 >= errorCodeConsecutiveThreshold) {
                    {
                        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
                        lastReportedErrorCode[currentSlave] = kCommunicationFailureSentinel;
                    }
                    anySlaveHasError.store(true);
                    if (auto* setMaster = AppController::GetInstance().GetMaster()) { setMaster->SetGlobalError(); }
                }

                if (!alreadyReportedSentinel && cnt2 >= errorCodeConsecutiveThreshold) {
                    std::string msg = "[ErrorCheck] 從站 " + std::to_string(currentSlave) +
                                     " PDO/SDO 讀取 0x603F 皆失敗 - 持續 " + std::to_string(cnt2) + " 次\n";
                    PostUTF8LogToUI(msg);
                    if (!prevWasError) {
                        PostAutoStatusUpdate();
                    }
                } else if (cnt2 < errorCodeConsecutiveThreshold) {
                    std::string msg = "[ErrorCheck] 從站 " + std::to_string(currentSlave) +
                                     " PDO/SDO 讀取 0x603F 失敗 (暫時) - 計數 " + std::to_string(cnt2) + "/" + std::to_string(errorCodeConsecutiveThreshold) + "\n";
                    PostUTF8LogToUI(msg);
                    if (cnt2 == 2) {
                        if (auto* recMaster = AppController::GetInstance().GetMaster()) {
                            recMaster->ResetSlaveSDOState(static_cast<uint16_t>(currentSlave));
                            PostUTF8LogToUI(std::string("[ErrorCheck] 從站 " + std::to_string(currentSlave) +
                                                       " 計數器已重置 (P1 PDO 恢復)\n"));
                        }
                    }
                }
                } // end SDO fallback else
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


