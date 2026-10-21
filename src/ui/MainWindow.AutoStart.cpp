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

static bool IsDynamicPdoViolationOnly(PDOFilter::ViolationType type)
{
    const int value = static_cast<int>(type);
    if (value == static_cast<int>(PDOFilter::ViolationType::None)) {
        return false;
    }
    const int dynamicMask = static_cast<int>(PDOFilter::ViolationType::Velocity) |
                            static_cast<int>(PDOFilter::ViolationType::Acceleration) |
                            static_cast<int>(PDOFilter::ViolationType::Deceleration);
    return (value & ~dynamicMask) == 0;
}

} // namespace

void MainWindow::StartAutoProcedure(bool showAutoWindow)
{
    // Lift the auto-stop suppression that was set in FinalizeSuccessfulConnection
    // to prevent SDO/PDO callbacks from re-asserting immediateStopBusy between
    // the busy-flag clear and this function's busy check. From this point on,
    // normal 0x60FD / 0x603F monitoring resumes — if a real stop condition
    // exists, the next callback cycle will trigger it.
    this->suppressAutoStopDuringStartup_.store(false);

    auto* master = AppController::GetInstance().GetMaster();
    if (!master || !master->IsRunning()) {
        PrependStatus(L"[AUTO] 需先連線並啟動主迴圈 (PDO) 才能啟動自動程序\r\n");
        return;
    }
    if (IsExternalImmediateStopActive() || IsExternalEStopActive()) {
        PrependStatus(L"[AUTO] 外部立即停止或緊急停止訊號仍有效，無法啟動自動程序\r\n");
        return;
    }
    {
        const auto snapshot = GetSystemStateSnapshot();
        const auto decision = CoreHelpers::EvaluateStartAutoProcedure(snapshot);
        if (!decision.allowed) {
            if (decision.rejection == CoreHelpers::CommandPolicyRejection::kSystemBusy) {
                // Diagnostic: log which specific flags are causing the busy state
                std::wstring busyInfo = L"[AUTO] 系統忙碌 —";
                if (snapshot.worker_running) busyInfo += L" workerRunning";
                if (disconnecting.load()) busyInfo += L" disconnecting";
                if (homingBusy.load()) busyInfo += L" homingBusy";
                if (autoDownToOriginRunning.load()) busyInfo += L" autoDownToOrigin";
                if (autoConnectBusy.load()) busyInfo += L" autoConnectBusy";
                if (jogBusy.load()) busyInfo += L" jogBusy";
                if (faultResetBusy.load()) busyInfo += L" faultReset";
                if (immediateStopBusy.load()) busyInfo += L" immediateStop";
                if (powerOpBusy.load()) busyInfo += L" powerOp";
                busyInfo += L"\r\n";
                PrependStatus(busyInfo.c_str());
                PrependStatus(L"[AUTO] 系統目前忙碌，無法啟動自動程序 (已阻止重複或衝突操作)\r\n");
            } else if (decision.rejection == CoreHelpers::CommandPolicyRejection::kMasterNotInitialized) {
                PrependStatus(L"[AUTO] 主站尚未初始化，無法啟動自動程序\r\n");
            } else {
                const std::wstring reason = CoreHelpers::Utf8ToWide(
                    std::string("[AUTO] 無法啟動自動程序: ") +
                    CoreHelpers::DescribeCommandPolicyRejection(decision.rejection) + "\n");
                PrependStatus(reason.c_str());
            }
            return;
        }
    }
    // Note: autoProcedureRunning will be set when real long-running
    // auto subtasks begin (see AutoFaultReset background thread).
    // Start SDO reader in EtherCATMaster. The reader runs at a fixed 10ms period
    // (enforced inside EtherCATMaster). The UI no longer controls the polling period.
    if (!master) {
        PrependStatus(L"[AUTO] EtherCAT master not available\r\n");
        NotifyEthercatError(true);
        return;
    }

    if (this->autoProcedureRunning.load()) {
        PrependStatus(L"[AUTO] 自動程序已在執行中，忽略重複啟動請求\r\n");
        return;
    }

    bool expectedStartPending = false;
    if (!this->autoProcedureStartPending.compare_exchange_strong(expectedStartPending, true)) {
        PrependStatus(L"[AUTO] 自動程序啟動已在進行中，忽略重複請求\r\n");
        return;
    }

    BeginAutoProcedureStartupUiBatch();
    
    // Ensure PDO descriptors and mechanical scaling factors are set for all slaves
    // so AutoWindow can display actual positions. This is critical because
    // UpdateMechanicalUnitsForSlave needs both descriptors and scaling to work.
    int slaveCount = master->GetSlaveCount();
    PostUTF8LogToUI(std::string("[AUTO] Preparing PDO descriptors and scaling for ") + std::to_string(slaveCount) + " slaves\n");
    for (int s = 1; s <= slaveCount; ++s) {
        // Register PDO descriptors for this slave (required for mechanical units calculation)
        bool pdoOk = PreparePDOForSlave(s);
        PostUTF8LogToUI(std::string("[AUTO] Slave ") + std::to_string(s) + " PDO prepare: " + (pdoOk ? "OK" : "FAILED") + "\n");
        
        MotionParamSnapshot motionParams = CaptureMotionParams(s);
        double p1Scale = motionParams.haveP1 ? motionParams.p1 : 1.0;
        bool p2Invert = motionParams.haveP2 && motionParams.p2 == 1;
        master->SetMechanicalScalingFactors(s, p1Scale, p2Invert);
        std::stringstream ss;
        ss << std::fixed << std::setprecision(15) << p1Scale;
        PostUTF8LogToUI(std::string("[AUTO] Slave ") + std::to_string(s) + " scaling: P1=" + ss.str() + " P2Invert=" + std::to_string(p2Invert) + "\n");
        ApplySavedPdoFilterConfig(master, s, motionParams, "[AUTO]", true);

        // 設定違規狀態回調，用於在狀態視窗顯示 log
        {
            int slaveIdx = s;  // capture by value for lambda
            master->SetPDOFilterViolationCallback(slaveIdx, 
                [this, slaveIdx](PDOFilter::ViolationType type, double vel, double accel, bool limiting, double duration) {
                    {
                        std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                        auto it = this->lastPDOState_.find(slaveIdx);
                        PDOFilter::ViolationType prevType = PDOFilter::ViolationType::None;
                        bool prevLimiting = false;
                        if (it != this->lastPDOState_.end()) {
                            prevType = static_cast<PDOFilter::ViolationType>(it->second.first);
                            prevLimiting = it->second.second;
                        }
                        if (static_cast<int>(type) == static_cast<int>(prevType) && prevLimiting == limiting) {
                            return;
                        }
                        if (limiting && prevLimiting &&
                            IsDynamicPdoViolationOnly(prevType) &&
                            (type == PDOFilter::ViolationType::None || IsDynamicPdoViolationOnly(type))) {
                            this->lastPDOState_[slaveIdx] = { static_cast<int>(prevType), limiting };
                            return;
                        }
                    }

                    RequestPdoSafetyManualResetStop(slaveIdx, type, duration);
                    std::ostringstream oss;
                    oss << "[PDO] Slave " << slaveIdx << " ";
                    
                    // 違規解除時顯示恢復訊息
                    if (type == PDOFilter::ViolationType::None) {
                        oss << "違規解除";
                        if (limiting) {
                            oss << " [限制仍啟用中]";
                        }
                        oss << "\n";
                        PostUTF8LogToUI(oss.str());
                        std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                        this->lastPDOState_[slaveIdx] = { static_cast<int>(type), limiting };
                        return;
                    }
                    
                    // 區分違規項目，並顯示各自門檻值（若可取得）
                    double velThresh=0.0, accPosThresh=0.0, accNegThresh=0.0, trackingErrorLimit=0.0;
                    auto* callbackMaster = AppController::GetInstance().GetMaster();
                    bool gotThresh = callbackMaster && callbackMaster->GetPDOFilterViolationThresholds(slaveIdx, velThresh, accPosThresh, accNegThresh);
                    bool gotTrackingErrorLimit = callbackMaster && callbackMaster->GetPDOFilterTrackingErrorLimit(slaveIdx, trackingErrorLimit);
                    bool first = true;
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Velocity)) {
                        oss << (first ? "" : "|") << "速度過大";
                        if (gotThresh) oss << "(閥值=" << std::fixed << std::setprecision(2) << velThresh << "°/s)";
                        first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Acceleration)) {
                        oss << (first ? "" : "|") << "加速度過大";
                        if (gotThresh) oss << "(閥值=" << std::fixed << std::setprecision(2) << accPosThresh << "°/s²)";
                        first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Deceleration)) {
                        oss << (first ? "" : "|") << "減速度過大";
                        if (gotThresh) oss << "(閥值=" << std::fixed << std::setprecision(2) << accNegThresh << "°/s²)";
                        first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) {
                        oss << (first ? "" : "|") << "超出下限"; first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax)) {
                        oss << (first ? "" : "|") << "超出上限"; first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::InputAnomaly)) {
                        oss << (first ? "" : "|") << "MMF輸入異常"; first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::StaleInput)) {
                        oss << (first ? "" : "|") << "MMF輸入停滞"; first = false;
                    }
                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                        oss << (first ? "" : "|") << "偏差值過大";
                        if (gotTrackingErrorLimit) oss << "(閥值=" << std::fixed << std::setprecision(2) << trackingErrorLimit << "°)";
                        first = false;
                    }
                    
                    // 顯示機械單位（度/秒、度/秒²）
                    oss << std::fixed << std::setprecision(2);
                    oss << " (V=" << vel << "°/s A=" << accel << "°/s²)";
                    
                    if (limiting) {
                        oss << " [限制已啟用]";
                    }
                    oss << "\n";
                    
                    PostUTF8LogToUI(oss.str());
                    std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                    this->lastPDOState_[slaveIdx] = { static_cast<int>(type), limiting };
                });
        }

        // 設定輸入異常（跳變）回調，用於在狀態視窗顯示 log
        {
            int slaveIdx = s;  // capture by value for lambda
            master->SetPDOFilterInputAnomalyCallback(slaveIdx,
                [this, slaveIdx](double originalSet, double replacedSet, double inputChange, double maxAllowedChange, PDOFilter::InputAnomalyReason reason) {
                    // Aggregate and defer logging (coalesced once-per-second)
                    this->RecordInputAnomaly(slaveIdx, originalSet, replacedSet, inputChange, maxAllowedChange, reason);
                });
        }
    }
    PostUTF8LogToUI(std::string("[AUTO] PDO descriptors and scaling setup complete\n"));
    
    // SDO reader callback已在連接時設定，此處不需要重複設定。
    // 只需確保 SDO reader 已啟用即可。
    if (master && !master->IsSdoReaderRunning()) {
        PostUTF8LogToUI(std::string("[AUTO] Warning: SDO reader not running, attempting to start\n"));
        // 這種情況不應該發生，但作為備用，重新啟動（使用連接時的完整 callback）
    }
    
    if (showAutoWindow) {
        // 自動程序啟動時建立 AutoWindow 並最小化顯示
        // 這確保操作員能看到自動狀態，同時不會遮擋主視窗
        if (!hAutoWindow || !IsWindow(hAutoWindow)) {
            HWND h = CreateAutoWindow(hwnd);
            if (h) {
                hAutoWindow = h;
                // 最小化顯示 AutoWindow（不影響主視窗操作）
                ShowWindow(h, SW_SHOWMINIMIZED);
                // Give the AutoWindow an immediate status refresh
                PostAutoStatusUpdate();
            }
        } else {
            // AutoWindow 已存在，確保它是可見的（可能是最小化或正常狀態）
            if (!IsWindowVisible(hAutoWindow)) {
                ShowWindow(hAutoWindow, SW_SHOWMINIMIZED);
            }
        }
    }
    const bool stopAutoAllowed = CanExecuteAutoProcedureStopRequest();
    BeginMainWindowVisualRefreshBatch();
    SetControlEnabled(hButtonStartAutoMode, false);
    SetControlEnabled(hBtnStopAutoMode, stopAutoAllowed);
    // Mirror start/stop button enablement into server ui_state
    SetUiDisabled("start_auto", true);
    SetUiDisabled("stop_auto", !stopAutoAllowed);
    // Disable the Disconnect button while auto procedure is running
    if (hBtnDisconnect) SetControlEnabled(hBtnDisconnect, false);
    SetUiDisabled("disconnect", true);  // Notify server to disable web disconnect button
    // Disable slave selection combobox while auto procedure runs
    if (hComboSlaves) SetControlEnabled(hComboSlaves, false);
    // Let the shared auto-procedure lock helper own PDO input/apply enablement.
    ApplyAutoProcedureUiLockState();

    // Disable all main window motion control buttons while auto procedure is running.
    // Keep Immediate Stop enabled so operator can always request a stop.
    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, false);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, false);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, false);
    if (hBtnHome) SetControlEnabled(hBtnHome, false);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, false);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, false);
    if (hBtnFaultReset) SetControlEnabled(hBtnFaultReset, false);
    EndMainWindowVisualRefreshBatch();
    
    // Do not send control messages to AutoWindow. AutoWindow must manage its own controls.

    // Also ensure MMF1_IN is started when auto procedure begins
    if (IsMasterInitialized()) {
        if (!MasterIsMMF1InRunning()) {
            bool mmfStarted = MasterStartMMF1In();
            if (mmfStarted) {
                PrependStatus(L"[MMF1_IN] 已於自動程序啟動時啟用\r\n");
            } else {
                PrependStatus(L"[MMF1_IN] 無法於自動程序啟動時啟用\r\n");
            }
        } else {
            PrependStatusNoFile(L"[MMF1_IN] 已在執行中\r\n");
        }
    }
    // 註: 0x603F 錯誤碼循環檢查已在連接完成時啟動,此處不需重複啟動
    
    // If the AutoWindow was shown, notify it to enable its actionable buttons.
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_ENABLE_ALL, 0, 0);
    }
    // Immediately perform a fault-reset sequence in a background thread
    // (do not notify AutoWindow). This ensures the auto procedure starts
    // from a clean state even when AutoWindow is not open.
    try {
        std::thread([this]() {
        struct AutoStartupUiBatchGuard {
            MainWindow* window;

            ~AutoStartupUiBatchGuard() {
                if (window) {
                    window->EndAutoProcedureStartupUiBatch(true);
                }
            }
        } uiBatchGuard{this};

        if (!this->autoProcedureStartPending.exchange(false)) {
            return;
        }

        auto* bgMaster = AppController::GetInstance().GetMaster();
        if (!bgMaster) {
            PostUTF8LogToUI(std::string("[AutoFaultReset] EtherCAT master not available\n"));
            NotifyEthercatError(true);
            return;
        }
        // Mark overall auto-procedure running and UI busy while FaultResetAll runs
        this->autoProcedureRunning.store(true);
        SetBusyFlagAndNotify(this->faultResetBusy, true);

        if (!this->motionController) {
            PostUTF8LogToUI(std::string("[AutoFaultReset] MotionController not initialized\n"));
            SetBusyFlagAndNotify(this->faultResetBusy, false);
            return;
        }

        bool overallSuccess = false;
        try {
            MotionResult mr = this->motionController->FaultResetAll();
            PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr.message));
            overallSuccess = mr.success;
        } catch (const std::exception &ex) {
            std::string s = std::string("[AutoFaultReset] exception: ") + ex.what() + "\n";
            PostUTF8LogToUI(s);
            overallSuccess = false;
        } catch (...) {
            PostUTF8LogToUI(std::string("[AutoFaultReset] unknown exception\n"));
            overallSuccess = false;
        }

        // 檢查所有從站是否已在 Operation Enabled 狀態，
        // 若已是則跳過 PowerOnAll 以避免不必要的穩定化等待與狀態機序列。
        bool overallPowerOnSuccess = false;
        if (AreAllSlavesReadyByStatusword()) {
            PostUTF8LogToUI(std::string("[AutoFaultReset] all slaves already Operation Enabled, skipping PowerOnAll\n"));
            overallPowerOnSuccess = true;
            // 清除可能遺留的 powerOnInProgress 旗標，避免 CyclicTask 被阻擋
            if (bgMaster) bgMaster->SetPowerOnInProgress(false);
        } else {
            overallPowerOnSuccess = TryPowerOnAllSlaves(
                "[AutoFaultReset] ",
                nullptr,
                "[AutoFaultReset] MotionController not initialized; skipping power-on\n");
        }

        if (overallSuccess && overallPowerOnSuccess) {
            try {
                ClearMasterErrorAndResetPdoFilters();
                this->anySlaveHasInternalError.store(false);
                PostAutoStatusUpdate();
            } catch (const std::exception &ex) {
                PostUTF8LogToUI(std::string("[AutoFaultReset] Failed to clear error/reset PDO filters after PowerOn: ") + ex.what() + "\n");
            } catch (...) {
                PostUTF8LogToUI(std::string("[AutoFaultReset] Unknown error clearing error/resetting PDO filters after PowerOn\n"));
            }

            // Soft-start is now triggered by the AutoConnect monitoring
            // thread AFTER EnablePDOFiltering(true).  Triggering it here
            // (before filtering is enabled) creates a gap where the
            // filter is uninitialised but Filter() is never called,
            // allowing the actual position to drift and corrupting the
            // initial soft-start state.
        }
        
        // Log combined result
        if (overallSuccess && overallPowerOnSuccess) PostUTF8LogToUI(std::string("[AutoFaultReset] completed successfully\n"));
        else PostUTF8LogToUI(std::string("[AutoFaultReset] completed with failures\n"));

        HWND hFaultResetNotify = (this->hAutoWindow && IsWindow(this->hAutoWindow)) ? this->hAutoWindow : NULL;
        FinalizeAutoFaultReset(hFaultResetNotify, overallSuccess && overallPowerOnSuccess);
    }).detach();
    } catch (const std::exception &ex) {
        this->autoProcedureStartPending.store(false);
        this->autoProcedureRunning.store(false);
        PostAutoStatusUpdate();
        EndAutoProcedureStartupUiBatch(true);
        const std::wstring reason = CoreHelpers::Utf8ToWide(
            std::string("[AUTO] 無法啟動自動程序背景執行緒: ") + ex.what() + "\n");
        PrependStatus(reason.c_str());
    } catch (...) {
        this->autoProcedureStartPending.store(false);
        this->autoProcedureRunning.store(false);
        PostAutoStatusUpdate();
        EndAutoProcedureStartupUiBatch(true);
        PrependStatus(L"[AUTO] 無法啟動自動程序背景執行緒\r\n");
    }
}


void MainWindow::StopAutoProcedure()
{
    const bool wasStarting = this->autoProcedureStartPending.exchange(false);
    const bool wasRunning = this->autoProcedureRunning.exchange(false);
    if (!wasStarting && !wasRunning) {
        return;
    }

    // 註: 0x603F 錯誤碼循環檢查在斷開連接時才停止,此處不停止
    
    if (auto* master = AppController::GetInstance().GetMaster()) {
        // 0x60FD 現由 PDO 每週期自動更新，保證 Auto Stop 後 E-Stop 燈號不會凍結
        master->SetSdoReaderEnabled(true);
        StartPostConnectSdoReader();
    }
    // Clear the auto-procedure running flag and update AutoWindow immediately
    PostAutoStatusUpdate();
    BeginMainWindowVisualRefreshBatch();
    SetControlEnabled(hButtonStartAutoMode, true);
    SetControlEnabled(hBtnStopAutoMode, false);
    // Mirror start/stop state to server so web clients see the correct enablement
    SetUiDisabled("start_auto", false);  // Enable start button
    SetUiDisabled("stop_auto", true);    // Disable stop button
    // Re-enable the Disconnect button when auto procedure stops
    if (hBtnDisconnect) SetControlEnabled(hBtnDisconnect, true);
    SetUiDisabled("disconnect", false);  // Notify server to enable web disconnect button
    
    // Re-enable all main window motion control buttons when auto procedure stops
    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, true);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, true);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, true);
    if (hBtnHome) SetControlEnabled(hBtnHome, true);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, true);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, true);
    if (hBtnFaultReset) SetControlEnabled(hBtnFaultReset, true);
    if (hBtnImmediateStop) SetControlEnabled(hBtnImmediateStop, true);
    // Re-enable slave selection combobox after auto procedure stops
    if (hComboSlaves) SetControlEnabled(hComboSlaves, true);
    EndMainWindowVisualRefreshBatch();
    RequestMotionUiRefresh();
    
    // Do not send control messages to AutoWindow. AutoWindow must manage its own controls.
    // Also stop MMF1_IN when auto procedure stops
    if (IsMasterInitialized()) {
        if (MasterIsMMF1InRunning()) {
            MasterStopMMF1In();
            PrependStatus(L"[MMF1_IN] 已於自動程序停止時停用\r\n");
        }
    }
    // Notify AutoWindow to disable its actionable buttons / pause UI actions
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_DISABLE_ALL, 0, 0);
    }
    // Stopping the auto procedure should decelerate/stop motion without
    // latching a manual Fault Reset requirement.
    PostMessageW(hwnd,
                 WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                 (WPARAM)hAutoWindow,
                 AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
}


void MainWindow::TriggerAutoStartupSequence(bool suppressPcAutoWindow)
{
    // 設置標誌：連接成功後自動啟動「自動程序啟動」
    autoStartAfterConnect.store(true);
    suppressAutoWindowForStartupAuto.store(suppressPcAutoWindow);
    
    PostUTF8LogToUI(std::string("[AutoStartup] 自動啟動序列已觸發，開始連接...\n"));
    
    // 調用連接流程，連接成功後會檢查 autoStartAfterConnect 標誌
    // 並自動調用 StartAutoProcedure()
    AppController::GetInstance().Connect(std::string());
}

// -----------------------------------------------------------------------------
// Additional non-UI helpers moved from MainWindow.cpp
// -----------------------------------------------------------------------------



