#include "MainWindow.h"
#include "../include/AutoWindow.h"
#include <vector>
#include <commctrl.h>
#include <windowsx.h>
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include <commdlg.h>
#include <sstream>
#include <iomanip>
#include <string>
#include <algorithm>
#include <array>
#include <limits>
#include <cstdint>
#include <exception>
#include <future>
#include <thread>
#include <chrono>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <shellapi.h>
#include "LayoutHelper.h"
#include "EtherCATMaster.h"
#include "../include/core/CoreHelpers.h"
#include "../include/core/CpuIsolation.h"
#include "../include/MotionConfigIO.h"
#include "Win32Utils.h"
#include "HttpServer.h"
#include "../app/AppController.h"
#include "MachineConfigIO.h"

namespace {
constexpr WPARAM kForcedAutoStartupMainWindowRefresh = 1;

static std::wstring ResolvePdoLogPath()
{
    wchar_t exePath[MAX_PATH] = {0};
    std::wstring logDir = L".\\logs";
    if (GetModuleFileNameW(NULL, exePath, MAX_PATH) != 0) {
        std::wstring exeDir(exePath);
        const size_t pos = exeDir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            logDir = exeDir.substr(0, pos) + L"\\logs";
        }
    }
    CreateDirectoryW(logDir.c_str(), nullptr);
    return logDir + L"\\PDO.log";
}

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
}

// ---------------------------------------------------------------------------
// Helper: Configure a background worker thread to run at BELOW_NORMAL priority
// and avoid the PDO cyclic thread's dedicated CPU core. This prevents heavy
// operations (Auto Connect, DownToOrigin, FaultReset) from causing jitter in
// the real-time PDO loop.
// ---------------------------------------------------------------------------
static void ConfigureWorkerThreadForRT() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // Hard-coded avoidance of Core 4 — must match CpuIsolation TARGET_ETHERCAT_CORE.
    // This guarantees the worker never runs on the protected EtherCAT core even
    // if the CyclicTask hasn't stored its mask yet (e.g. master not started).
    constexpr int    kEtherCATTargetCore = 4;
    constexpr DWORD_PTR kEtherCATCoreMask = (DWORD_PTR)1 << kEtherCATTargetCore;

    DWORD_PTR avoidMask = kEtherCATCoreMask;

    // Also avoid whatever core the CyclicTask actually bound to (may differ
    // from Core 4 when the host has fewer logical processors).
    auto* master = AppController::GetInstance().GetMaster();
    if (master) {
        DWORD_PTR stored = master->GetCyclicThreadCoreMask();
        if (stored) avoidMask |= stored;
    }

    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) return;
    DWORD_PTR available = processMask ? processMask : systemMask;
    DWORD_PTR workerMask = available & ~avoidMask;
    if (workerMask == 0) return; // only one core available, can't avoid
    SetThreadAffinityMask(GetCurrentThread(), workerMask);
}

void MainWindow::SetBusyFlagAndNotify(std::atomic<bool> &flag, bool value) {
    const bool previous = flag.exchange(value);
    if (previous == value) {
        return;
    }
    PostAutoStatusUpdate();
    RequestMotionUiRefresh();
}

bool MainWindow::RequestPdoSafetyManualResetStop(int slaveIdx, PDOFilter::ViolationType type, double duration) {
    const bool hardLimitStop =
        PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin) ||
        PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax);
    const bool trackingErrorStop =
        PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError) && duration >= 0.5;

    if (!hardLimitStop && !trackingErrorStop) {
        if (type == PDOFilter::ViolationType::None) {
            std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
            this->pdoSafetyStopRequested_[slaveIdx] = false;
        }
        return false;
    }

    if (trackingErrorStop) {
        if (auto* master = AppController::GetInstance().GetMaster()) {
            try {
                if (master->IsPDOFilterSoftStartActive(slaveIdx)) {
                    PDOFilter::ViolationType currentViolation = PDOFilter::ViolationType::None;
                    bool filterError = false;
                    bool trackingFatal = false;
                    if (!master->GetPDOFilterState(slaveIdx, currentViolation, filterError, trackingFatal) ||
                        (!filterError && !trackingFatal)) {
                        return false;
                    }
                }
            } catch (...) {
            }
        }
    }

    bool requestManualResetStop = false;
    {
        std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
        auto tripIt = this->pdoSafetyStopRequested_.find(slaveIdx);
        const bool alreadyRequested = (tripIt != this->pdoSafetyStopRequested_.end() && tripIt->second);
        if (!alreadyRequested) {
            this->pdoSafetyStopRequested_[slaveIdx] = true;
            requestManualResetStop = true;
        }
    }

    if (!requestManualResetStop) {
        return false;
    }

    this->autoConnectAbortRequested.store(true);
    if (this->autoConnectRunning.load()) {
        if (hardLimitStop) {
            PostUTF8LogToUI(std::string("[AUTO] Soft Start abort requested due to PDO hard limit on Axis ") + std::to_string(slaveIdx) + "\n");
        } else {
            PostUTF8LogToUI(std::string("[AUTO] Soft Start abort requested due to fatal tracking error on Axis ") + std::to_string(slaveIdx) + "\n");
        }
    }

    if (hardLimitStop) {
        const char* limitText = PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax) ? "上限" : "下限";
        PostUTF8LogToUI(std::string("[AutoEmergencyStop] 由 PDO 位置") + limitText + "保護觸發 (Slave " + std::to_string(slaveIdx) + ") - 正在啟動緊急停止，需 Fault Reset 後才能重新啟動。\n");
        PostUTF8LogToUI(std::string("[PDO] Slave ") + std::to_string(slaveIdx) +
            " 超出" + limitText + "，已判定為異常並執行緊急停止\n");
    } else {
        PostUTF8LogToUI(std::string("[AutoEmergencyStop] 由 PDO 偏差值過大逾時觸發 (Slave ") + std::to_string(slaveIdx) + ") - 正在啟動緊急停止，停止後可直接重新啟動。\n");
        PostUTF8LogToUI(std::string("[PDO] Slave ") + std::to_string(slaveIdx) +
            " 偏差值過大持續超過 0.5 秒，已判定為異常並執行緊急停止\n");
    }
    HWND requester = (this->hAutoWindow && IsWindow(this->hAutoWindow)) ? this->hAutoWindow : this->hwnd;
    PostMessageW(this->hwnd, WM_USER_AUTO_EMERGENCY_STOP_REQUEST, reinterpret_cast<WPARAM>(requester), hardLimitStop ? 0 : 1);
    return true;
}

void MainWindow::AbortAutoConnectSoftStartForStop(const char *logPrefix) {
    if (!this->autoConnectRunning.load()) return;

    this->autoConnectAbortRequested.store(true);
    // Clear the busy flag immediately so oBusy turns off without waiting
    // for the auto-connect thread to reach its cleanup code. The thread
    // will still run to completion and clear the flag again (no-op).
    SetBusyFlagAndNotify(this->autoConnectBusy, false);
    this->softStartSoundPlaying_.store(false);
    this->StopSystemAlertSoundIfIdle();
    PostUTF8LogToUI(std::string(logPrefix) + "Requested abort of Auto Connect soft-start\n");
}

void MainWindow::RequestAbortAutoDownToOriginForStop(const char *logPrefix) {
    if (!this->autoDownToOriginRunning.load()) return;

    this->autoDownToOriginAbortRequested.store(true);
    PostUTF8LogToUI(std::string(logPrefix) + "Requested abort of AutoDownToOrigin workflow\n");
}

void MainWindow::DisablePdoFilteringAfterStop(const char *logPrefix) {
    auto* stopMaster = AppController::GetInstance().GetMaster();
    if (!stopMaster) return;

    try { stopMaster->ReleaseAllPDOFiltersHoldPosition(); } catch(...) {}

    const bool flushesClearedMotionCommand = stopMaster->IsRunning();
    stopMaster->EnablePDOFiltering(false);
    {
        std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
        this->lastPDOState_.clear();
        this->pdoSafetyStopRequested_.clear();
        this->lastGlobalLimiting_.store(false);
    }
    this->violationSoundPlaying_.store(false);
    this->softStartSoundPlaying_.store(false);
    NotifyAutoWindowOwnerState((WPARAM)0x0);
    try { stopMaster->EnablePDO607ARecording(false); } catch(...) {}
    PostUTF8LogToUI(std::string(logPrefix) +
        (flushesClearedMotionCommand
            ? "PDO filtering disabled; zeroed cyclic motion commands flushed; cyclic motion command recording stopped\n"
            : "PDO filtering disabled; cyclic motion command recording stopped\n"));
}

void MainWindow::NotifyAutoDownToOriginCompletion(HWND hRequester, bool success, bool fallbackToAutoWindow) {
    if (hRequester && IsWindow(hRequester)) {
        PostMessageW(hRequester, WM_USER_AUTO_DOWN_TO_ORIGIN_DONE, (WPARAM)(success ? 1 : 0), 0);
        return;
    }

    if (fallbackToAutoWindow && hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_DOWN_TO_ORIGIN_DONE, (WPARAM)(success ? 1 : 0), 0);
    }
}

void MainWindow::NotifyAutoStopCompletion(HWND hRequester, UINT doneMessage, bool success, LPARAM doneParam) {
    if (hRequester && IsWindow(hRequester)) {
        PostMessageW(hRequester, doneMessage, (WPARAM)(success ? 1 : 0), doneParam);
        PostMessageW(hRequester, WM_USER_AUTO_DOWN_TO_ORIGIN_DONE, (WPARAM)0, doneParam);
    }
}

void MainWindow::NotifyAutoFaultResetCompletion(HWND hRequester, bool success) {
    if (hRequester && IsWindow(hRequester)) {
        PostMessageW(hRequester, WM_USER_AUTO_FAULT_RESET_DONE, (WPARAM)(success ? 1 : 0), 0);
    }
}

void MainWindow::FinalizeAutoFaultReset(HWND hRequester, bool success) {
    const bool externalStopStillActive = IsExternalImmediateStopActive() || IsExternalEStopActive();
    const bool resetClearedStopLatch = success && !externalStopStillActive;
    if (resetClearedStopLatch) {
        SetManualResetRequired(false);
    } else if (success && externalStopStillActive) {
        PostUTF8LogToUI(std::string("[AutoFaultReset] reset completed but external stop input is still active; release external stop and reset again to unlock motion.\n"));
    }
    NotifyAutoFaultResetCompletion(hRequester, resetClearedStopLatch);
    if (this->errorSoundPlaying_.load()) {
        if (externalStopStillActive) {
            PostUTF8LogToUI(std::string("[AutoFaultReset] external stop remains active; emergency alert sound continues\n"));
        } else {
            this->errorSoundPlaying_.store(false);
            this->StopSystemAlertSoundIfIdle();
        }
    }
    SetBusyFlagAndNotify(this->faultResetBusy, false);
    RequestMotionUiRefresh();
}

bool MainWindow::IsManualResetRequired() const {
    return manualResetRequired.load();
}

void MainWindow::RequestMotionUiRefresh() {
    if (!hwnd || !IsWindow(hwnd)) return;

    if (hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_STATUS_UPDATE, 0, 0);
    }

    if (autoProcedureStartupUiBatchDepth_.load(std::memory_order_acquire) > 0) {
        autoProcedureStartupMainWindowRefreshPending_.store(true, std::memory_order_release);
        return;
    }

    if (!ShouldRefreshMainWindowUi()) {
        return;
    }

    PostMessageW(hwnd, WM_USER_AUTO_REPORT_UI_STATE, 0, 0);
}

void MainWindow::BeginAutoProcedureStartupUiBatch() {
    autoProcedureStartupUiBatchDepth_.fetch_add(1, std::memory_order_acq_rel);
}

void MainWindow::EndAutoProcedureStartupUiBatch(bool forceMainWindowRefresh) {
    const int previousDepth = autoProcedureStartupUiBatchDepth_.fetch_sub(1, std::memory_order_acq_rel);
    if (previousDepth <= 1) {
        autoProcedureStartupUiBatchDepth_.store(0, std::memory_order_release);
        const bool pendingRefresh = autoProcedureStartupMainWindowRefreshPending_.exchange(false, std::memory_order_acq_rel);
        if ((forceMainWindowRefresh || pendingRefresh) && ShouldRefreshMainWindowUi()) {
            PostMessageW(hwnd, WM_USER_AUTO_REPORT_UI_STATE, 0, 0);
        }
    }
}

void MainWindow::BeginMainWindowVisualRefreshBatch() {
    ++mainWindowVisualRefreshBatchDepth_;

    if (mainWindowVisualRefreshBatchDepth_ != 1) {
        return;
    }

    auto suspendRedraw = [](HWND control) {
        if (control && IsWindow(control)) {
            SendMessageW(control, WM_SETREDRAW, FALSE, 0);
        }
    };

    for (const auto &item : pdoDisplay.pdoItems) {
        suspendRedraw(item.hEdit);

        suspendRedraw(item.hButton);
    }
}

void MainWindow::EndMainWindowVisualRefreshBatch() {
    if (mainWindowVisualRefreshBatchDepth_ == 0) {
        return;
    }

    --mainWindowVisualRefreshBatchDepth_;

    if (mainWindowVisualRefreshBatchDepth_ != 0) {
        return;
    }

    auto resumeRedraw = [](HWND control) {
        if (control && IsWindow(control)) {
            SendMessageW(control, WM_SETREDRAW, TRUE, 0);
        }
    };

    for (const auto &item : pdoDisplay.pdoItems) {
        resumeRedraw(item.hEdit);
        resumeRedraw(item.hButton);
    }

    HWND pdoGroup = pdoDisplay.GetGroupBox();
    if (hContentWnd && IsWindow(hContentWnd) && pdoGroup && IsWindow(pdoGroup)) {
        RECT pdoRect{};
        pdoDisplay.GetGroupBoxRect(pdoRect);
        InflateRect(&pdoRect, 8, 8);
        RedrawWindow(hContentWnd, &pdoRect, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        return;
    }

    for (const auto &item : pdoDisplay.pdoItems) {
        if (item.hEdit && IsWindow(item.hEdit)) {
            RedrawWindow(item.hEdit, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        }
        if (item.hButton && IsWindow(item.hButton)) {
            RedrawWindow(item.hButton, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
        }
    }
}

void MainWindow::RefreshConfirmedSlaveControlState(bool rebuildPdoControls) {
    auto* uiMaster = AppController::GetInstance().GetMaster();
    const bool baseEnableMotion = (confirmedSlave > 0) && IsMasterInitialized();
    const auto currentSnapshot = GetSystemStateSnapshot();
    const bool manualResetLockActive = IsManualResetRequired();
    const bool faultResetOnlyMode = currentSnapshot.error || manualResetLockActive;
    bool motionAllowed = baseEnableMotion;
    const bool autoUiLockActive = IsAutoProcedureUiLockActive();

    bool statuswordFault = false;
    if (baseEnableMotion && uiMaster) {
        int outBase = 0;
        int inBase = 0;
        if (uiMaster->GetSlaveIOBaseBytes(confirmedSlave, outBase, inBase)) {
            for (const auto &itm : pdoDisplay.pdoItems) {
                if ((int)itm.index == 0x6041) {
                    int absOff = inBase + itm.byteOffset;
                    uint8_t swBuf[2] = {0};
                    int copied = uiMaster->SnapshotIOMap(absOff, swBuf, 2);
                    if (copied >= 2) {
                        uint16_t sw = (uint16_t)swBuf[0] | ((uint16_t)swBuf[1] << 8);
                        if (sw & (1u << 3)) {
                            statuswordFault = true;
                        }
                    }
                    break;
                }
            }
        }
    }

    if (statuswordFault) {
        motionAllowed = false;
    }
    if (faultResetOnlyMode) {
        motionAllowed = false;
    }

    const bool immediateStopAllowed = baseEnableMotion;

    if (autoUiLockActive) {
        motionAllowed = false;
    }

    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, motionAllowed ? true : false);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, motionAllowed ? true : false);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, motionAllowed ? true : false);
    if (hBtnHome) SetControlEnabled(hBtnHome, motionAllowed ? true : false);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, motionAllowed ? true : false);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, motionAllowed ? true : false);

    const bool faultResetAllowed = IsMasterInitialized() &&
                                   !faultResetBusy.load() &&
                                   !immediateStopBusy.load() &&
                                   !powerOpBusy.load() &&
                                   (!autoUiLockActive || faultResetOnlyMode);
    if (hBtnFaultReset) SetControlEnabled(hBtnFaultReset, faultResetAllowed ? true : false);
    if (hBtnImmediateStop) SetControlEnabled(hBtnImmediateStop, immediateStopAllowed ? true : false);

    if (hBtnImportParams) SetControlEnabled(hBtnImportParams, faultResetOnlyMode ? false : true);
    if (hBtnSaveParams) SetControlEnabled(hBtnSaveParams, (faultResetOnlyMode || autoUiLockActive) ? false : true);
    if (hBtnSendMailbox) SetControlEnabled(hBtnSendMailbox, (IsMasterInitialized() && !autoUiLockActive) ? true : false);
    if (hBtnSaveConfig) SetControlEnabled(hBtnSaveConfig, autoUiLockActive ? false : true);

    for (const auto &it : pdoDisplay.pdoItems) {
        if (it.hButton && IsWindow(it.hButton)) {
            SetControlEnabled(it.hButton, motionAllowed ? true : false);
        }
        if (it.hEdit && IsWindow(it.hEdit)) {
            SetControlEnabled(it.hEdit, motionAllowed ? true : false);
        }
    }

    const bool stopAutoAllowed = IsMasterInitialized() && CanExecuteAutoProcedureStopRequest();
    if (hBtnStopAutoMode) SetControlEnabled(hBtnStopAutoMode, stopAutoAllowed ? true : false);
    if (hButtonStartAutoMode) SetControlEnabled(hButtonStartAutoMode, (baseEnableMotion && !manualResetLockActive && !autoUiLockActive) ? true : false);
    if (hBtnAutoWindow) SetControlEnabled(hBtnAutoWindow, true);

    SetUiDisabled("stop_auto", stopAutoAllowed ? false : true);
    SetUiDisabled("start_auto", (baseEnableMotion && !manualResetLockActive && !autoUiLockActive) ? false : true);
    ApplyAutoProcedureUiLockState();

    {
        bool hasESI_local = !currentObjects.empty();
        bool isConn_local = IsMasterInitialized();
        bool hasSlave_local = (confirmedSlave > 0);
        if (hBtnWriteAllSDO) SetControlEnabled(hBtnWriteAllSDO, (hasESI_local && isConn_local && hasSlave_local) ? true : false);
    }

    if (!rebuildPdoControls || confirmedSlave <= 0) {
        return;
    }

    if (uiMaster && uiMaster->IsInitialized()) {
        if (pdoDisplay.RecalculateOffsetsFromSDO(uiMaster, confirmedSlave)) {
            pdoDisplay.RebuildPDOControls();
            RestorePDOInputsFromGlobal();
        } else {
            pdoDisplay.RebuildPDOControls();
            RestorePDOInputsFromGlobal();
        }
        ApplyStatuswordDisableToPDOControls(confirmedSlave);
    }

    pdoDisplay.Show(true);
    UpdateSelectedSlaveStatus(confirmedSlave);
}

void MainWindow::SetManualResetRequired(bool required) {
    const bool previous = manualResetRequired.exchange(required);
    bool stopLampChanged = false;
    if (!required) {
        stopLampChanged = autoImmediateStopLampLatched.exchange(false) || stopLampChanged;
        stopLampChanged = autoEmergencyStopLampLatched.exchange(false) || stopLampChanged;
    }

    if (previous == required && !stopLampChanged) {
        return;
    }

    PostAutoStatusUpdate();
    RequestMotionUiRefresh();
}

void MainWindow::SetAutoStopLampLatched(bool immediateStop, bool emergencyStop) {
    bool changed = false;
    if (immediateStop) {
        changed = !autoImmediateStopLampLatched.exchange(true) || changed;
    }
    if (emergencyStop) {
        changed = !autoEmergencyStopLampLatched.exchange(true) || changed;
    }

    if (!changed) {
        return;
    }

    PostAutoStatusUpdate();
    RequestMotionUiRefresh();
}

bool MainWindow::IsExternalEStopActive() const {
    bool estopActive = false;
    try {
        std::vector<uint8_t> bytes;
        if (this->TryGetLastSdo60FD(bytes) && !bytes.empty()) {
            uint8_t last = bytes.back();
            estopActive = ((last & 0x20) == 0);
        } else {
            int prev = this->lastSdo60FDLastByte.load();
            if (prev != -1) {
                estopActive = ((prev & 0x20) == 0);
            }
        }
    } catch (...) {
        estopActive = false;
    }
    return estopActive;
}

bool MainWindow::IsExternalImmediateStopActive() const {
    bool immediateStopActive = false;
    try {
        std::vector<uint8_t> bytes;
        if (this->TryGetLastSdo60FD(bytes) && !bytes.empty()) {
            uint8_t last = bytes.back();
            immediateStopActive = ((last & 0x01) != 0);
        } else {
            int prev = this->lastSdo60FDLastByte.load();
            if (prev != -1) {
                immediateStopActive = ((prev & 0x01) != 0);
            }
        }
    } catch (...) {
        immediateStopActive = false;
    }
    return immediateStopActive;
}

bool MainWindow::IsAutoProcedureUiLockActive() const {
    return this->autoProcedureRunning.load() || this->autoProcedureStartPending.load();
}

bool MainWindow::ShouldRefreshMainWindowUi() const {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    if (minimizedToTray || isMinimizedState) {
        return false;
    }

    if (IsIconic(hwnd)) {
        return false;
    }

    return IsWindowVisible(hwnd) != FALSE;
}

void MainWindow::ApplyAutoProcedureUiLockState() {
    if (!IsAutoProcedureUiLockActive()) {
        return;
    }

    constexpr UINT kEditFocusTimerId = 5555;

    if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditIsMotion) {
        KillTimer(hwnd, kEditFocusTimerId);
        DestroyWindow(hInplaceEdit);
        hInplaceEdit = NULL;
        inplaceEditItem = -1;
        inplaceEditIsMotion = false;
    }

    for (const auto &item : pdoDisplay.pdoItems) {
        if (item.hEdit && IsWindow(item.hEdit)) {
            SetControlEnabled(item.hEdit, false);
        }
        if (item.hButton && IsWindow(item.hButton)) {
            SetControlEnabled(item.hButton, false);
        }
    }

    if (hBtnSaveParams && IsWindow(hBtnSaveParams)) {
        SetControlEnabled(hBtnSaveParams, false);
    }
    if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) {
        SetControlEnabled(hBtnSendMailbox, false);
    }
    SetUiDisabled("send_mailbox", true);
    if (hBtnSaveConfig && IsWindow(hBtnSaveConfig)) {
        SetControlEnabled(hBtnSaveConfig, false);
    }
}

bool MainWindow::CanExecuteAutoProcedureStopRequest() const {
    if (!IsMasterInitialized()) {
        return false;
    }

    if (!this->autoProcedureRunning.load() &&
        !this->autoProcedureStartPending.load() &&
        !this->manualResetRequired.load()) {
        return false;
    }

    return !GetSystemStateSnapshot().busy;
}

void MainWindow::FinalizeAutoImmediateStop(HWND hRequester, bool success, bool isPDOErrorTriggered) {
    if (success && this->autoDownToOriginAbortRequested.load()) {
        if (this->autoDownToOriginRunning.exchange(false)) {
            this->autoDownToOriginLastSuccess.store(false);
            PostUTF8LogToUI(std::string("[AutoImmediateStop] Released AutoDownToOrigin busy state after stop completion\n"));
        }
    }
    SetBusyFlagAndNotify(this->immediateStopBusy, false);
    NotifyAutoStopCompletion(hRequester, WM_USER_AUTO_IMMEDIATE_STOP_DONE, success, isPDOErrorTriggered ? 2 : 1);
    FinalizeErrorAlertSoundForStop("[AutoImmediateStop] Emergency alert sound stopped\n", true);
    RequestMotionUiRefresh();
}

void MainWindow::FinalizeAutoEmergencyStop(HWND hRequester, bool success) {
    if (success && this->autoDownToOriginAbortRequested.load()) {
        if (this->autoDownToOriginRunning.exchange(false)) {
            this->autoDownToOriginLastSuccess.store(false);
            PostUTF8LogToUI(std::string("[AutoEmergencyStop] Released AutoDownToOrigin busy state after stop completion\n"));
        }
    }
    SetBusyFlagAndNotify(this->immediateStopBusy, false);
    NotifyAutoStopCompletion(hRequester, WM_USER_AUTO_EMERGENCY_STOP_DONE, success, 1);
    FinalizeErrorAlertSoundForStop(
        "[AutoEmergencyStop] Emergency alert sound stopped\n",
        !IsExternalEStopActive(),
        "[AutoEmergencyStop] E-Stop active; alert sound continues\n");
    if (IsWindow(this->hwnd)) {
        if (auto* doneMaster = AppController::GetInstance().GetMaster(); doneMaster && doneMaster->IsInitialized()) {
            SetFaultResetUiEnabledForStop(true);
        }
    }
    RequestMotionUiRefresh();
}

void MainWindow::FinalizeErrorAlertSoundForStop(const char *stoppedLog, bool allowStop, const char *continueLog) {
    if (!this->errorSoundPlaying_.load()) return;

    if (allowStop) {
        this->errorSoundPlaying_.store(false);
        StopSystemAlertSoundIfIdle();
        PostUTF8LogToUI(stoppedLog);
        return;
    }

    if (continueLog) {
        PostUTF8LogToUI(continueLog);
    }
}

void MainWindow::StartErrorAlertSoundForStop(const char *logPrefix) {
    if (this->errorSoundPlaying_.load()) return;

    if (!this->softStartSoundPlaying_.load() && !this->violationSoundPlaying_.load()) {
        try { PlaySound(TEXT("SystemAsterisk"), NULL, SND_ALIAS | SND_ASYNC | SND_LOOP); } catch(...) {}
    }
    this->errorSoundPlaying_.store(true);
    PostUTF8LogToUI(std::string(logPrefix) + "Emergency alert sound playing\n");
}

void MainWindow::SetFaultResetUiEnabledForStop(bool enabled) {
    if (hBtnFaultReset && IsWindow(hBtnFaultReset)) {
        SetControlEnabled(hBtnFaultReset, enabled);
    }
    SetUiDisabled("fault_reset", !enabled);
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        HWND hBtnAutoFault = GetDlgItem(hAutoWindow, IDC_BTN_AUTO_FAULT_RESET);
        if (hBtnAutoFault && IsWindow(hBtnAutoFault)) {
            EnableWindow(hBtnAutoFault, enabled ? TRUE : FALSE);
        }
    }
}

void MainWindow::NotifyAutoWindowOwnerState(WPARAM state) {
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_SET_OWNER_STATE, state, 0);
    }
}

bool MainWindow::TryPowerOnAllSlaves(const char *logPrefix, const char *partialFailureLog, const char *missingControllerLog) {
    if (!this->motionController) {
        if (missingControllerLog) {
            PostUTF8LogToUI(missingControllerLog);
        }
        return false;
    }

    try {
        auto* master = AppController::GetInstance().GetMaster();
        if (!master) return false;

        int slaveCount = master->GetSlaveCount();
        if (slaveCount <= 0) return true;

        SetBusyFlagAndNotify(this->powerOpBusy, true);
        MotionResult powerOnResult = this->motionController->PowerOnAllAsync(slaveCount);
        PostUTF8LogToUI(CoreHelpers::WideToUtf8(powerOnResult.message));
        SetBusyFlagAndNotify(this->powerOpBusy, false);

        if (!powerOnResult.success && partialFailureLog) {
            PostUTF8LogToUI(partialFailureLog);
        }
        return powerOnResult.success;
    } catch (const std::exception &ex) {
        PostUTF8LogToUI(std::string(logPrefix) + "PowerOn exception: " + ex.what() + "\n");
        SetBusyFlagAndNotify(this->powerOpBusy, false);
    } catch (...) {
        PostUTF8LogToUI(std::string(logPrefix) + "PowerOn unknown exception\n");
        SetBusyFlagAndNotify(this->powerOpBusy, false);
    }

    return false;
}

bool MainWindow::ClearMasterErrorAndResetPdoFilters(int slaveNumber) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return false;

    master->ClearGlobalError();
    int slaveCount = master->GetSlaveCount();
    if (slaveNumber > 0 && slaveNumber <= slaveCount) {
        master->ResetPDOFilter(slaveNumber);
        return true;
    }

    for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
        try { master->ResetPDOFilter(slaveIdx); } catch(...) {}
    }
    return true;
}

void MainWindow::RefreshInternalErrorStateFrom603F(const char *logPrefix) {
    auto *master = AppController::GetInstance().GetMaster();
    if (!master || !master->IsInitialized()) {
        return;
    }

    int slaveCount = 0;
    try {
        slaveCount = master->GetSlaveCount();
    } catch (...) {
        slaveCount = 0;
    }
    if (slaveCount <= 0) {
        return;
    }

    bool anyNonZero = false;
    bool readFailure = false;
    for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
        uint16_t errorCode = 0;
        int readSize = sizeof(errorCode);
        if (!MasterReadSDO(static_cast<uint16_t>(slaveIdx),
                           static_cast<uint16_t>(0x603F),
                           static_cast<uint8_t>(0x00),
                           &errorCode,
                           readSize,
                           100000)) {
            readFailure = true;
            break;
        }

        {
            std::lock_guard<std::mutex> lock(errorCodeStateMutex);
            lastReportedErrorCode[slaveIdx] = errorCode;
            errorCodeConsecutiveCounter[slaveIdx] = 0;
        }
        if (errorCode != 0) {
            anyNonZero = true;
        }
    }

    if (readFailure) {
        if (logPrefix) {
            PostUTF8LogToUI(std::string(logPrefix) + "Unable to refresh 0x603F state after successful workflow; keep current error latch\n");
        }
        return;
    }

    bool shouldNotify = false;
    if (anyNonZero) {
        const bool previous = this->anySlaveHasInternalError.exchange(true);
        shouldNotify = !previous;
        if (logPrefix) {
            PostUTF8LogToUI(std::string(logPrefix) + "0x603F still reports internal fault after workflow completion; keeping error state\n");
        }
    } else {
        const bool previous = this->anySlaveHasInternalError.exchange(false);
        shouldNotify = previous;
        if (!anySlaveHasError.load()) {
            try {
                master->ClearGlobalError();
                NotifyEthercatError(false);
                shouldNotify = true;
            } catch (...) {
            }
        }
        if (logPrefix) {
            PostUTF8LogToUI(std::string(logPrefix) + "Refreshed 0x603F state after successful workflow; cleared stale internal error latch\n");
        }
    }

    if (shouldNotify) {
        PostAutoStatusUpdate();
        RequestMotionUiRefresh();
    }
}

bool MainWindow::ExecuteStopMotionSequence(const char *logPrefix, bool allowSoftStartRetry) {
    (void)allowSoftStartRetry;
    MotionResult stopResult = this->motionController->SmoothStopAll(0.25);
    PostUTF8LogToUI(CoreHelpers::WideToUtf8(stopResult.message));

    bool success = stopResult.success;
    if (!stopResult.success) {
        MotionResult immediateResult = this->motionController->ImmediateStopAll();
        PostUTF8LogToUI(CoreHelpers::WideToUtf8(immediateResult.message));
        success = immediateResult.success;
    }

    try {
        if (auto* stopMaster = AppController::GetInstance().GetMaster()) {
            PostUTF8LogToUI(std::string(logPrefix) + "Stop-class request: disabling PDO filtering immediately after 0x60FF=0 command\n");
            DisablePdoFilteringAfterStop(logPrefix);
        }
    } catch (...) {
        // swallow
    }

    return success;
}

struct PreparePdoRequest {
    int slaveIdx = 0;
    std::promise<bool> completion;
};

static HANDLE g_launchedBrowserProcess = NULL;
static DWORD g_launchedBrowserPid = 0;
static HANDLE g_launchedBrowserJob = NULL;
static constexpr int kWebMobileWindowWidth = 400;
static constexpr int kWebMobileWindowHeight = 932;

struct EnumDataFind { DWORD pid; HWND result; };

static BOOL CALLBACK EnumWindowsProc_Find(HWND hwnd, LPARAM lParam)
{
    EnumDataFind* data = reinterpret_cast<EnumDataFind*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->pid) {
        if ((GetWindow(hwnd, GW_OWNER) == NULL) && IsWindowVisible(hwnd)) {
            data->result = hwnd;
            return FALSE;
        }
    }
    return TRUE;
}

static HWND FindMainWindowForPid(DWORD pid)
{
    EnumDataFind data;
    data.pid = pid;
    data.result = NULL;
    EnumWindows(EnumWindowsProc_Find, (LPARAM)&data);
    return data.result;
}

static bool FileExistsW(const std::wstring& path)
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static std::wstring GetExecutableDirW()
{
    wchar_t path[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, path, MAX_PATH) == 0) return std::wstring();
    std::wstring dir(path);
    const size_t pos = dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring();
    dir.resize(pos + 1);
    return dir;
}

static bool TryFindWebWindowIconPath(std::wstring& outPath)
{
    const std::wstring exeDir = GetExecutableDirW();
    if (!exeDir.empty()) {
        for (int up = 0; up <= 6; ++up) {
            std::wstring prefix;
            for (int i = 0; i < up; ++i) {
                prefix += L"..\\";
            }

            const std::wstring webCandidate = exeDir + prefix + L"resources\\web\\ka52.ico";
            if (FileExistsW(webCandidate)) {
                outPath = webCandidate;
                return true;
            }

            const std::wstring iconCandidate = exeDir + prefix + L"icons\\ka52.ico";
            if (FileExistsW(iconCandidate)) {
                outPath = iconCandidate;
                return true;
            }
        }
    }

    const std::wstring webRelative = L"resources\\web\\ka52.ico";
    if (FileExistsW(webRelative)) {
        outPath = webRelative;
        return true;
    }

    const std::wstring iconRelative = L"icons\\ka52.ico";
    if (FileExistsW(iconRelative)) {
        outPath = iconRelative;
        return true;
    }

    return false;
}

static void ApplyWebWindowIcon(HWND target)
{
    if (!target || !IsWindow(target)) return;

    static HICON s_bigIcon = NULL;
    static HICON s_smallIcon = NULL;
    if (!s_bigIcon || !s_smallIcon) {
        std::wstring iconPath;
        if (TryFindWebWindowIconPath(iconPath)) {
            if (!s_bigIcon) {
                s_bigIcon = reinterpret_cast<HICON>(LoadImageW(
                    NULL,
                    iconPath.c_str(),
                    IMAGE_ICON,
                    256,
                    256,
                    LR_LOADFROMFILE | LR_DEFAULTCOLOR));
            }
            if (!s_smallIcon) {
                s_smallIcon = reinterpret_cast<HICON>(LoadImageW(
                    NULL,
                    iconPath.c_str(),
                    IMAGE_ICON,
                    GetSystemMetrics(SM_CXSMICON),
                    GetSystemMetrics(SM_CYSMICON),
                    LR_LOADFROMFILE | LR_DEFAULTCOLOR));
            }
        }
    }

    if (s_bigIcon) SendMessageW(target, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(s_bigIcon));
    if (s_smallIcon) SendMessageW(target, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(s_smallIcon));
}

static RECT GetDefaultWorkArea()
{
    RECT work = {0, 0, 0, 0};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return work;
}

static RECT GetWorkAreaForWindow(HWND hwnd)
{
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &mi)) {
        return mi.rcWork;
    }
    return GetDefaultWorkArea();
}

static HWND WaitForBrowserMainWindow(HANDLE process, DWORD pid, DWORD timeoutMs)
{
    if (!pid) return NULL;
    if (process) {
        WaitForInputIdle(process, std::min<DWORD>(timeoutMs, 1500));
    }

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        HWND target = FindMainWindowForPid(pid);
        if (target) return target;
        Sleep(50);
    } while (GetTickCount64() < deadline);

    return FindMainWindowForPid(pid);
}

static void PositionBrowserWindowTopRight(HWND target, bool preferMobileSizedWindow)
{
    if (!target || !IsWindow(target)) return;
    ApplyWebWindowIcon(target);

    RECT windowRect;
    if (!GetWindowRect(target, &windowRect)) return;

    RECT work = GetWorkAreaForWindow(target);
    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    int width = preferMobileSizedWindow ? kWebMobileWindowWidth : std::max(1, static_cast<int>(windowRect.right - windowRect.left));
    int height = preferMobileSizedWindow ? kWebMobileWindowHeight : std::max(1, static_cast<int>(windowRect.bottom - windowRect.top));
    width = std::min(width, workWidth);
    height = std::min(height, workHeight);

    const int x = work.right - width;
    const int y = work.top;
    SetWindowPos(target, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
}

static void CleanupTrackedBrowserHandle(bool forceClose)
{
    if (!g_launchedBrowserProcess) return;

    DWORD wait = WaitForSingleObject(g_launchedBrowserProcess, 0);
    if (forceClose || wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) {
        // If forceClose is requested and the process is still running,
        // terminate it explicitly.  The Job Object (if present) will also
        // terminate all child processes when its handle is closed.
        if (forceClose && wait == WAIT_TIMEOUT) {
            TerminateProcess(g_launchedBrowserProcess, 0);
            WaitForSingleObject(g_launchedBrowserProcess, 2000);
        }
        CloseHandle(g_launchedBrowserProcess);
        g_launchedBrowserProcess = NULL;
        g_launchedBrowserPid = 0;

        // Closing the Job handle with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        // causes Windows to terminate any remaining child processes (GPU,
        // renderer, utility, etc.) that Chromium spawned.
        if (g_launchedBrowserJob) {
            CloseHandle(g_launchedBrowserJob);
            g_launchedBrowserJob = NULL;
        }
    }
}

static bool IsTrackedBrowserRunning()
{
    if (!g_launchedBrowserProcess) return false;
    DWORD wait = WaitForSingleObject(g_launchedBrowserProcess, 0);
    return wait == WAIT_TIMEOUT;
}

static bool ReadExePathFromAppPaths(HKEY root, const wchar_t* exeName, std::wstring& outPath)
{
    std::wstring subKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\";
    subKey += exeName;

    wchar_t value[4096] = {0};
    DWORD bytes = sizeof(value);
    LONG status = RegGetValueW(root, subKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &bytes);
    if (status != ERROR_SUCCESS || value[0] == L'\0') {
        return false;
    }

    DWORD attrs = GetFileAttributesW(value);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    outPath.assign(value);
    return true;
}

static std::wstring GetEnvVarString(const wchar_t* name)
{
    DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) return std::wstring();

    std::wstring value(needed, L'\0');
    DWORD written = GetEnvironmentVariableW(name, &value[0], needed);
    if (written == 0) return std::wstring();
    value.resize(written);
    return value;
}

static bool TryGetKnownBrowserInstallPath(const std::wstring& baseDir, const wchar_t* relativePath, std::wstring& outPath)
{
    if (baseDir.empty()) return false;

    std::wstring candidate = baseDir;
    if (!candidate.empty() && candidate.back() != L'\\') {
        candidate.push_back(L'\\');
    }
    candidate += relativePath;

    DWORD attrs = GetFileAttributesW(candidate.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }

    outPath = std::move(candidate);
    return true;
}

static bool FindChromiumShellBrowser(std::wstring& browserPath)
{
    struct BrowserCandidate {
        const wchar_t* exeName;
        const wchar_t* knownInstallSuffix;
    };

    static const std::array<BrowserCandidate, 3> candidates = {{
        {L"msedge.exe", L"Microsoft\\Edge\\Application\\msedge.exe"},
        {L"chrome.exe", L"Google\\Chrome\\Application\\chrome.exe"},
        {L"chromium.exe", L"Chromium\\Application\\chromium.exe"}
    }};

    const std::wstring programFiles = GetEnvVarString(L"ProgramFiles");
    const std::wstring programFilesX86 = GetEnvVarString(L"ProgramFiles(x86)");
    const std::wstring localAppData = GetEnvVarString(L"LOCALAPPDATA");

    for (const auto& candidate : candidates) {
        if (ReadExePathFromAppPaths(HKEY_CURRENT_USER, candidate.exeName, browserPath) ||
            ReadExePathFromAppPaths(HKEY_LOCAL_MACHINE, candidate.exeName, browserPath)) {
            return true;
        }

        if (TryGetKnownBrowserInstallPath(programFilesX86, candidate.knownInstallSuffix, browserPath) ||
            TryGetKnownBrowserInstallPath(programFiles, candidate.knownInstallSuffix, browserPath) ||
            TryGetKnownBrowserInstallPath(localAppData, candidate.knownInstallSuffix, browserPath)) {
            return true;
        }
    }

    return false;
}

static std::wstring EnsureBigCatShellProfileDir()
{
    std::wstring baseDir = GetEnvVarString(L"LOCALAPPDATA");
    if (baseDir.empty()) {
        return std::wstring(L".\\BigCatBrowserShell");
    }

    std::wstring appDir = baseDir + L"\\WuBigCat";
    std::wstring profileDir = appDir + L"\\browser-shell";
    CreateDirectoryW(appDir.c_str(), nullptr);
    CreateDirectoryW(profileDir.c_str(), nullptr);
    return profileDir;
}

static bool LaunchUrlInChromiumAppMode(const std::wstring& browserPath,
                                       const std::wstring& url,
                                       bool preferMobileSizedWindow,
                                       PROCESS_INFORMATION& pi)
{
    ZeroMemory(&pi, sizeof(pi));

    std::wstring profileDir = EnsureBigCatShellProfileDir();
    std::wstring commandLine = L"\"" + browserPath + L"\"";
    commandLine += L" --app=\"" + url + L"\"";
    commandLine += L" --new-window";
    commandLine += L" --no-first-run --no-default-browser-check --disable-session-crashed-bubble";
    commandLine += L" --disable-features=Translate";
    commandLine += L" --user-data-dir=\"" + profileDir + L"\"";
    if (preferMobileSizedWindow) {
        RECT work = GetDefaultWorkArea();
        const int x = std::max(work.left, work.right - kWebMobileWindowWidth);
        commandLine += L" --window-size=" + std::to_wstring(kWebMobileWindowWidth) + L"," + std::to_wstring(kWebMobileWindowHeight);
        commandLine += L" --window-position=" + std::to_wstring(x) + L"," + std::to_wstring(work.top);
    }

    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (preferMobileSizedWindow) {
        RECT work = GetDefaultWorkArea();
        si.dwFlags |= STARTF_USESIZE | STARTF_USEPOSITION;
        si.dwX = std::max<LONG>(work.left, work.right - kWebMobileWindowWidth);
        si.dwY = work.top;
        si.dwXSize = kWebMobileWindowWidth;
        si.dwYSize = kWebMobileWindowHeight;
    }

    // Create a Job Object so that when our app exits, ALL browser child
    // processes (GPU, renderer, utility, …) are automatically killed.
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    BOOL created = CreateProcessW(
        browserPath.c_str(),
        mutableCommand.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED,
        nullptr,
        nullptr,
        &si,
        &pi);
    if (!created) {
        if (hJob) CloseHandle(hJob);
        return false;
    }

    // Assign to job before resuming so child processes inherit membership.
    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
        g_launchedBrowserJob = hJob;
    }

    // Isolate browser process from EtherCAT RT core (Core 4) and System core (Core 0)
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
        // Exclude Core 0 (0x1) and Core 4 (0x10)
        DWORD_PTR browserMask = systemMask & ~((DWORD_PTR)0x11);
        if (browserMask == 0) browserMask = systemMask; // Fallback if only 0/4 exist
        SetProcessAffinityMask(pi.hProcess, browserMask);
    }

    ResumeThread(pi.hThread);
    return true;
}

void MainWindow::OpenHttpOperationInterface(bool forceLogout, bool autoLoginDefaultPassword, bool preferMobileSizedWindow)
{
    std::string url = GetHttpServerUrl();
    if (url.empty()) url = std::string("http://localhost:8080/");
    if (!url.empty() && url.back() != '/') {
        url.push_back('/');
    }
    url += "auto.html";
    if (autoLoginDefaultPassword) {
        url += "#autologin=1234";
    }
    std::wstring wurl = CoreHelpers::Utf8ToWide(url);

    if (forceLogout) {
        SetForceLogoutFlag();
    }

    CleanupTrackedBrowserHandle(false);

    if (IsTrackedBrowserRunning()) {
        bool broughtToFront = false;
        if (g_launchedBrowserPid) {
            HWND hTarget = FindMainWindowForPid(g_launchedBrowserPid);
            if (hTarget) {
                ShowWindow(hTarget, SW_RESTORE);
                PositionBrowserWindowTopRight(hTarget, preferMobileSizedWindow);
                SetForegroundWindow(hTarget);
                broughtToFront = true;
            }
        }
        if (broughtToFront) {
            MinimizeToTray();
            return;
        }

        CleanupTrackedBrowserHandle(true);
    }

    std::wstring browserPath;
    if (FindChromiumShellBrowser(browserPath)) {
        PROCESS_INFORMATION pi;
        if (LaunchUrlInChromiumAppMode(browserPath, wurl, preferMobileSizedWindow, pi)) {
            CleanupTrackedBrowserHandle(true);
            g_launchedBrowserProcess = pi.hProcess;
            g_launchedBrowserPid = pi.dwProcessId;
            if (HWND hTarget = WaitForBrowserMainWindow(g_launchedBrowserProcess, g_launchedBrowserPid, 2000)) {
                PositionBrowserWindowTopRight(hTarget, preferMobileSizedWindow);
            }
            CloseHandle(pi.hThread);
            PrependStatus(autoLoginDefaultPassword
                ? L"[UI] 已用 app 視窗模式開啟操作介面並嘗試預設登入\r\n"
                : L"[UI] 已用 app 視窗模式開啟操作介面\r\n");
        } else {
            ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
            CleanupTrackedBrowserHandle(true);
            PrependStatus(L"[UI] Chromium app 視窗啟動失敗，已退回一般瀏覽器\r\n");
        }
    } else {
        ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOWNORMAL);
        CleanupTrackedBrowserHandle(true);
        PrependStatus(L"[UI] 找不到支援 app 模式的瀏覽器，已退回一般瀏覽器\r\n");
    }

    MinimizeToTray();
}

static std::wstring GetAppIniPath()
{
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, buf, MAX_PATH) == 0) return std::wstring(L".\\WuBigCat.ini");
    wchar_t *p = wcsrchr(buf, L'\\');
    wchar_t *dot = wcsrchr(buf, L'.');
    if (dot && (!p || dot > p)) *dot = L'\0';
    std::wstring ini = std::wstring(buf) + L".ini";
    return ini;
}

static void WriteIniBool(const wchar_t* section, const wchar_t* key, bool value)
{
    std::wstring ini = GetAppIniPath();
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", ini.c_str());
}

static const int sideMargin = 0;
#define TIMER_EDIT_FOCUS 5555

// Forward declarations for subclass procedures used in this TU
LRESULT CALLBACK FaultResetButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK ImmediateStopButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK PowerOnButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK PowerOffButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK HomeButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK JogButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                       UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK MotionListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK InplaceEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK SDOListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                     UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK ContentPaneSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

// Small safe wrappers
static inline void SafeSetPos(HWND hWnd, HWND hInsertAfter, int x, int y, int w, int h, UINT flags) {
    if (hWnd && IsWindow(hWnd)) SetWindowPos(hWnd, hInsertAfter, x, y, w, h, flags);
}
static inline void SafeSetTextW(HWND hWnd, const std::wstring &text) {
    if (hWnd && IsWindow(hWnd)) SetWindowTextW(hWnd, text.c_str());
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT uMsg,
                                         WPARAM wParam, LPARAM lParam) {
    MainWindow* window = nullptr;

    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        window = reinterpret_cast<MainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)window);
    } else {
        window = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(uMsg, wParam, lParam);
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Reflow SDO controls when window size changes or initial layout

LRESULT MainWindow::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_KEYDOWN:
        // Ctrl+T toggles always-on-top for quick testing
        if ((wParam == 'T' || wParam == 't') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // flip current topmost state using GetWindowLongPtr for style check
            bool isTop = (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            SetAlwaysOnTop(!isTop);
            return 0;
        }
        break;
    case WM_WINDOWPOSCHANGED:
        // When Z-order changes (other windows try to cover), re-assert TOPMOST
        // if the user requested the window to stay always on top.
            if (this->alwaysOnTopEnabled) {
            SafeSetPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        {
            // If the window's size actually changed (not just moved), trigger a reflow
            // but only when not in interactive resize/move to avoid continuous redraws.
            WINDOWPOS* pwp = reinterpret_cast<WINDOWPOS*>(lParam);
            bool sizeChanged = (pwp && !(pwp->flags & SWP_NOSIZE));
            if (sizeChanged) {
                if (!this->isInteractiveResize) {
                    ReflowMainControls();
                    ForceFullRedrawNow();
                } else {
                    // while interactively resizing, update content pane size only
                    this->interactiveSizeChanged = true;
                    UpdateContentPaneScrollOnly();
                }
            }
        }
        break;
    case WM_ENTERSIZEMOVE:
        // User started interactive resize/move - we can defer heavy reflow until end
        this->isInteractiveResize = true;
        this->interactiveSizeChanged = false;
        break;
    case WM_SIZING:
        // While user drags to resize, avoid expensive reflow and repaint; only adjust content pane
        if (!this->isInteractiveResize) this->isInteractiveResize = true;
        this->interactiveSizeChanged = true;
        UpdateContentPaneScrollOnly();
        return 0;
    case WM_EXITSIZEMOVE:
        // User finished interactive resize/move - only reflow if the size actually changed
        {
            bool didResize = this->interactiveSizeChanged;
            this->isInteractiveResize = false;
            this->interactiveSizeChanged = false;
            if (didResize) {
                ReflowMainControls();
                // After user finished resizing, ensure UI is redrawn immediately
                ForceFullRedrawNow();
            }
        }
        break;
    case WM_DPICHANGED:
        {
            // wParam: new DPI (LOWORD = X, HIWORD = Y). When DPI changes, recreate GUI font and reflow
            int newDpiX = LOWORD(wParam);
            int newDpiY = HIWORD(wParam);
            (void)newDpiY;
            if (newDpiX <= 0) newDpiX = 96;
            // recreate hGuiFont based on same point size but new DPI
            const int pt = 10; // keep same point size as CreateControls
            int lfHeight = -MulDiv(pt, newDpiX, 72);
            HFONT hNew = CreateFontW(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                           VARIABLE_PITCH, L"Microsoft JhengHei UI");
            if (!hNew) hNew = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            if (hGuiFont) { DeleteObject(hGuiFont); hGuiFont = NULL; }
            hGuiFont = hNew;
            // Resize window suggested rect is in lParam (RECT*) - honor suggested new size if provided
            if (lParam) {
                RECT* prc = reinterpret_cast<RECT*>(lParam);
                if (prc) SetWindowPos(hwnd, NULL, prc->left, prc->top, prc->right - prc->left, prc->bottom - prc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            ReflowMainControls();
        }
        break;
        case WM_USER_AUTO_FAULT_RESET_REQUEST:
                {
                    // wParam: HWND of requester (AutoWindow) so we can post DONE back
                    // Always perform reset-all via MotionController::FaultResetAll()
                    HWND hRequester = (HWND)wParam;

                    const auto decision = CoreHelpers::EvaluateAutoFaultReset(GetSystemStateSnapshot());
                    if (!decision.allowed) {
                        this->autoFaultResetRequestPending.store(false);
                        PostUTF8LogToUI(std::string("[AutoFaultReset] rejected: ") +
                                        CoreHelpers::DescribeCommandPolicyRejection(decision.rejection) + "\n");
                        NotifyAutoFaultResetCompletion(hRequester, false);
                        return 0;
                    }

                    if (!AppController::GetInstance().GetMaster()) {
                        this->autoFaultResetRequestPending.store(false);
                        PostUTF8LogToUI(std::string("[AutoFaultReset] EtherCAT master not available\n"));
                        NotifyEthercatError(true);
                        NotifyAutoFaultResetCompletion(hRequester, false);
                        return 0;
                    }

                    // Launch a detached thread that performs ASSERT then RELEASE and posts completion back
                    // Set UI busy flag while FaultResetAll runs
                    bool expectedFaultResetBusy = false;
                    if (!this->faultResetBusy.compare_exchange_strong(expectedFaultResetBusy, true)) {
                        this->autoFaultResetRequestPending.store(false);
                        PostUTF8LogToUI(std::string("[AutoFaultReset] rejected: already running\n"));
                        NotifyAutoFaultResetCompletion(hRequester, false);
                        return 0;
                    }
                    PostAutoStatusUpdate();
                    this->autoFaultResetRequestPending.store(false);
                    std::thread([this, hRequester]() {
                        ConfigureWorkerThreadForRT();
                        bool overallSuccess = false;
                        if (!this->motionController) {
                            PostUTF8LogToUI(std::string("[AutoFaultReset] MotionController not initialized\n"));
                            FinalizeAutoFaultReset(hRequester, false);
                            return;
                        }
                        try {
                            // Always call FaultResetAll() - legacy single-slave logic removed
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
                        // If reset completed (or completed with no action), attempt to power on
                        // all slaves so device is ready to run. This keeps AutoWindow in override
                        // state until both reset and power-on operations finish.
                        // Use PowerOnAllAsync to batch all PDO staging and send atomically.
                        bool overallPowerOnSuccess = true;
                        if (IsExternalImmediateStopActive() || IsExternalEStopActive()) {
                            overallPowerOnSuccess = false;
                            PostUTF8LogToUI(std::string("[AutoFaultReset] external stop input is still active; skipping power-on until release and another reset.\n"));
                        } else if (this->motionController) {
                            overallPowerOnSuccess = TryPowerOnAllSlaves("[AutoFaultReset] ");
                        } else {
                            PostUTF8LogToUI(std::string("[AutoFaultReset] MotionController not initialized; skipping power-on\n"));
                        }

                        // Notify requester once reset+power-on attempt complete. Use success only
                        // if fault-reset succeeded and power-on succeeded.
                        // If overall reset + power-on succeed, clear master-level persistent
                        // error flags and reset per-slave PDOFilter error state so the
                        // UI and runtime return to a clean state.
                        if (overallSuccess && overallPowerOnSuccess) {
                            try {
                                ClearMasterErrorAndResetPdoFilters();
                                this->anySlaveHasInternalError.store(false);
                                PostAutoStatusUpdate();
                            } catch(...) {
                                // don't let minor cleanup failures prevent posting completion
                            }
                        }
                        FinalizeAutoFaultReset(hRequester, overallSuccess && overallPowerOnSuccess);
                    }).detach();
                    // Note: the thread will clear faultResetBusy when finished
                }
                return 0;
    case WM_USER_AUTO_STATUS_UPDATE:
        {
            // wParam==1 -> AutoWindow Connect pressed (request to enable PDO filtering)
            if ((int)wParam == 1) {
                const auto decision = CoreHelpers::EvaluateAutoConnect(GetSystemStateSnapshot());
                if (!decision.allowed) {
                    this->autoConnectRequestPending.store(false);
                    PostUTF8LogToUI(std::string("[AUTO] Auto Connect request ignored: ") +
                                    CoreHelpers::DescribeCommandPolicyRejection(decision.rejection) + "\n");
                    PostAutoStatusUpdate();
                    return 0;
                }
                // Prevent concurrent runs
                if (this->autoConnectRunning.exchange(true)) {
                    this->autoConnectRequestPending.store(false);
                    PostUTF8LogToUI(std::string("[AUTO] Auto Connect already running\n"));
                    PostAutoStatusUpdate();
                    return 0;
                }
                // Mark Auto Connect busy so UI reflects the running soft-start
                SetBusyFlagAndNotify(this->autoConnectBusy, true);
                // Inform AutoWindow immediately that owner started busy work
                NotifyAutoWindowOwnerState((WPARAM)0x1);
                this->autoConnectRequestPending.store(false);

                // Launch background thread to perform long-running connect/setup work
                std::thread([this]() {
                    ConfigureWorkerThreadForRT();
                    try {
                        auto* master = AppController::GetInstance().GetMaster();
                        if (!master || !master->IsInitialized()) {
                            PostUTF8LogToUI(std::string("[AUTO] PDO enable requested but master not initialized\n"));
                        } else {
                            // Reset per-run PDO violation dedupe/sound state so stale
                            // states from a previous run cannot keep alarms latched.
                            {
                                std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                                this->lastPDOState_.clear();
                                this->pdoSafetyStopRequested_.clear();
                                this->lastGlobalLimiting_.store(false);
                            }
                            if (this->violationSoundPlaying_.load()) {
                                this->violationSoundPlaying_.store(false);
                                this->StopSystemAlertSoundIfIdle();
                            }

                            // Ensure MMF1_IN is running before first soft-start.
                            // Without this, the first Auto Connect can consume startup/default
                            // setpoints (often near zero), causing an initial stutter.
                            try {
                                if (!master->IsMMF1InRunning()) {
                                    bool mmfStarted = master->StartMMF1In();
                                    if (mmfStarted) {
                                        PostUTF8LogToUI(std::string("[AUTO] MMF1_IN started for Auto Connect\n"));
                                    } else {
                                        PostUTF8LogToUI(std::string("[AUTO] WARNING: failed to start MMF1_IN before Auto Connect\n"));
                                    }
                                }

                                // Warm up reader for a few cycles so first filter iterations
                                // consume fresh MMF samples.
                                uint32_t cycleUs = master->GetDesiredCycleTime();
                                if (cycleUs == 0) cycleUs = 2000;
                                int warmupMs = static_cast<int>((cycleUs * 20) / 1000); // ~20 cycles
                                if (warmupMs < 20) warmupMs = 20;
                                if (warmupMs > 80) warmupMs = 80;
                                std::this_thread::sleep_for(std::chrono::milliseconds(warmupMs));
                            } catch (...) {
                                // Keep flow resilient even if MMF warm-up path fails.
                            }

                            int slaveCount = master->GetSlaveCount();
                            for (int s = 1; s <= slaveCount; ++s) {
                                MotionParamSnapshot motionParams = CaptureMotionParams(s);
                                double p1Scale = motionParams.haveP1 ? motionParams.p1 : 1.0;
                                bool p2Invert = motionParams.haveP2 && motionParams.p2 == 1;
                                // Apply scaling and filter configuration per-slave
                                master->SetMechanicalScalingFactors(s, p1Scale, p2Invert);
                                ApplySavedPdoFilterConfig(master, s, motionParams, "[AUTO]", false);
                                
                                // 設定違規狀態回調，用於在狀態視窗顯示 log
                                {
                                    int slaveIdx = s;  // capture by value for lambda
                                        master->SetPDOFilterViolationCallback(slaveIdx, 
                [this, slaveIdx](PDOFilter::ViolationType type, double vel, double accel, bool limiting, double duration) {
                                            // Deduplicate logs and avoid per-slave "[限制仍啟用中]" noise
                                            // when the limiting is due to a global forcing by the master.
                                            auto* callbackMaster = AppController::GetInstance().GetMaster();
                                            bool globalLimiting = callbackMaster && callbackMaster->IsGlobalLimitingEnabled();
                                            {
                                                std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                                                bool prevGlobal = this->lastGlobalLimiting_.load();
                                                if (globalLimiting != prevGlobal) {
                                                    std::ostringstream goss;
                                                    if (globalLimiting) goss << "[PDO] 全域限制已啟用\n";
                                                    else goss << "[PDO] 全域限制已解除\n";
                                                    PostUTF8LogToUI(goss.str());
                                                    this->lastGlobalLimiting_.store(globalLimiting);
                                                }
                                                auto it = this->lastPDOState_.find(slaveIdx);
                                                PDOFilter::ViolationType prevType = PDOFilter::ViolationType::None;
                                                bool prevLim = false;
                                                if (it != this->lastPDOState_.end()) {
                                                    prevType = static_cast<PDOFilter::ViolationType>(it->second.first);
                                                    prevLim = it->second.second;
                                                }
                                                // If nothing changed, skip logging
                                                if (static_cast<int>(type) == static_cast<int>(prevType) && prevLim == limiting) return;

                                                if (limiting && prevLim &&
                                                    IsDynamicPdoViolationOnly(prevType) &&
                                                    (type == PDOFilter::ViolationType::None || IsDynamicPdoViolationOnly(type))) {
                                                    this->lastPDOState_[slaveIdx] = { static_cast<int>(prevType), limiting };
                                                    return;
                                                }

                                                // Start/stop violation alert sound on state transitions.
                                                // Stale-input-only warnings are logged, but do not own the audible alarm.
                                                auto IsAudibleViolation = [](PDOFilter::ViolationType vt) -> bool {
                                                    if (vt == PDOFilter::ViolationType::None) return false;
                                                    if (PDOFilter::HasFlag(vt, PDOFilter::ViolationType::Velocity)) return true;
                                                    if (PDOFilter::HasFlag(vt, PDOFilter::ViolationType::Acceleration)) return true;
                                                    if (PDOFilter::HasFlag(vt, PDOFilter::ViolationType::Deceleration)) return true;
                                                    if (PDOFilter::HasFlag(vt, PDOFilter::ViolationType::HardLimitMin)) return true;
                                                    if (PDOFilter::HasFlag(vt, PDOFilter::ViolationType::HardLimitMax)) return true;
                                                    return false;
                                                };
                                                bool prevAudible = IsAudibleViolation(prevType);
                                                bool curAudible = IsAudibleViolation(type);
                                                if (!prevAudible && curAudible) {
                                                    if (!this->violationSoundPlaying_.load()) {
                                                        // If soft-start is already playing the same sound, don't restart it
                                                        if (!this->softStartSoundPlaying_.load()) {
                                                            try { PlaySound(TEXT("SystemAsterisk"), NULL, SND_ALIAS | SND_ASYNC | SND_LOOP); } catch(...) {}
                                                        }
                                                        this->violationSoundPlaying_.store(true);
                                                        // Sound state log removed for cleaner output
                                                    }
                                                } else if (prevAudible && !curAudible) {
                                                    // Check if any other slave still has an audible violation.
                                                    bool anyAudible = false;
                                                    for (const auto &kv : this->lastPDOState_) {
                                                        PDOFilter::ViolationType checkType =
                                                            (kv.first == slaveIdx)
                                                                ? type
                                                                : static_cast<PDOFilter::ViolationType>(kv.second.first);
                                                        if (IsAudibleViolation(checkType)) { anyAudible = true; break; }
                                                    }
                                                    if (!anyAudible && this->violationSoundPlaying_.load()) {
                                                        // Only stop the system sound if soft-start is not playing.
                                                        this->violationSoundPlaying_.store(false);
                                                        if (!this->softStartSoundPlaying_.load()) {
                                                            try { PlaySound(NULL, NULL, 0); } catch(...) {}
                                                        }
                                                        // Sound state log removed for cleaner output
                                                    }
                                                }
                                                // If this slave never had an actual violation and the only
                                                // reason it's in limiting is global forcing, skip a per-slave
                                                // "[限制仍啟用中]" message (global message already printed above).
                                                if (type == PDOFilter::ViolationType::None && limiting && globalLimiting && prevType == PDOFilter::ViolationType::None) {
                                                    this->lastPDOState_[slaveIdx] = { static_cast<int>(type), limiting };
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
                                            auto* thresholdMaster = AppController::GetInstance().GetMaster();
                                            bool gotThresh = thresholdMaster && thresholdMaster->GetPDOFilterViolationThresholds(slaveIdx, velThresh, accPosThresh, accNegThresh);
                                            bool gotTrackingErrorLimit = thresholdMaster && thresholdMaster->GetPDOFilterTrackingErrorLimit(slaveIdx, trackingErrorLimit);
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
                                            
                                            // 顯示機械單位：若為硬位置限制，顯示角度限制值；否則顯示速度/加速度
                                            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax) || PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) {
                                                double minPos=std::numeric_limits<double>::quiet_NaN(), maxPos=std::numeric_limits<double>::quiet_NaN();
                                                auto* limitsMaster = AppController::GetInstance().GetMaster();
                                                bool gotLimits = limitsMaster && limitsMaster->GetPDOLimits(slaveIdx, minPos, maxPos);
                                                if (gotLimits) {
                                                    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax)) {
                                                        oss << "(限值=" << std::fixed << std::setprecision(3) << maxPos << "°)";
                                                    } else {
                                                        oss << "(限值=" << std::fixed << std::setprecision(3) << minPos << "°)";
                                                    }
                                                } else {
                                                    // fallback: show raw vel/acc if we couldn't obtain limits
                                                    oss << std::fixed << std::setprecision(2);
                                                    oss << " (V=" << vel << "°/s A=" << accel << "°/s²)";
                                                }
                                            } else {
                                                oss << std::fixed << std::setprecision(2);
                                                oss << " (V=" << vel << "°/s A=" << accel << "°/s²)";
                                            }
                                            
                                            if (limiting) {
                                                oss << " [限制已啟用]";
                                            }
                                            oss << "\n";
                                            
                                            PostUTF8LogToUI(oss.str());
                                            {
                                                std::lock_guard<std::mutex> lock(this->pdoLogStateMutex_);
                                                this->lastPDOState_[slaveIdx] = { static_cast<int>(type), limiting };
                                            }
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

                            // Start recording PDO 0x607A logical target position automatically when Auto Connect pressed
                            try {
                                master->EnablePDO607ARecording(true);
                                // Recording started (debug log removed for production)
                            } catch(...) {}
                            // Enable feedback fusion for this slave's PDOFilter (AutoWindow Connect)
                            try { /* per-slave loop handled above */ } catch (...) {}
                            // Note: PDOFilter callbacks were registered above inside the per-slave loop

                            // Attempt a one-time power-on for all slaves BEFORE enabling PDO filtering
                            // Use PowerOnAllAsync to batch all PDO staging and send atomically,
                            // preventing race conditions where one axis is enabled before others
                            // have their target positions staged.
                            TryPowerOnAllSlaves(
                                "[AUTO] ",
                                "[AUTO] Some slaves failed to power on during Auto Connect\n");

                            // CRITICAL: Wait for at least 2 PDO cycles to ensure PowerOn staged
                            // outputs (0x607A, 0x6040) have been applied to IOmap before enabling
                            // PDO filtering. This prevents the PDO filter from initializing with
                            // stale actual position values, which can cause position error faults.
                            // Default cycle time is 2ms, so we wait ~10ms (5 cycles) for safety.
                            try {
                                uint32_t cycleTimeUs = master->GetDesiredCycleTime();
                                if (cycleTimeUs == 0) cycleTimeUs = 2000; // default 2ms
                                int waitCycles = 5; // wait 5 cycles for safety margin
                                int waitMs = (int)((cycleTimeUs * waitCycles) / 1000);
                                if (waitMs < 10) waitMs = 10; // minimum 10ms
                                if (waitMs > 50) waitMs = 50; // cap at 50ms
                                // Debug wait log removed for cleaner output
                                std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
                            } catch (...) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            }

                            // Trigger soft-start for all slaves
                            try {
                                master->TriggerAllPDOFiltersSoftStart();
                                PostUTF8LogToUI(std::string("[AUTO] Soft Start 已觸發（連線運轉開始）\n"));
                                master->ClearGlobalError();
                                try {
                                    master->EnablePDOFiltering(true);
                                    if (!master->IsPDOFilteringEnabled()) {
                                        std::string lastErr = master->GetLastError();
                                        PostUTF8LogToUI(std::string("[AUTO] PDO filtering NOT enabled: ")
                                            + (lastErr.empty() ? "unknown reason" : lastErr) + "\n");
                                    } else {
                                        NotifyAutoWindowOwnerState((WPARAM)0x2);
                                    }
                                } catch(...) {
                                    PostUTF8LogToUI(std::string("[AUTO] Failed to enable PDO filtering\n"));
                                }
                            } catch (...) {
                                PostUTF8LogToUI(std::string("[AUTO] Failed to trigger Soft Start\n"));
                            }

                            // Clear any previous abort request for Auto Connect soft-start
                            this->autoConnectAbortRequested.store(false);

                            // Start coordination wait: poll for all-ready with timeout
                            try {
                                const double pollIntervalSec = 0.02; // 20 ms
                                const double timeoutSec = 15.0; // configurable timeout
                                double waited = 0.0;
                                bool allReady = false;

                                int sc_local = master->GetSlaveCount();
                                std::vector<char> prevReady(sc_local + 1, 0);
                                std::vector<double> trackingErrorSeenSince(sc_local + 1, -1.0);

                                auto checkPersistentTrackingError = [&](double elapsedSec) -> bool {
                                    for (int si = 1; si <= sc_local; ++si) {
                                        PDOFilter::ViolationType currentViolation = PDOFilter::ViolationType::None;
                                        bool filterError = false;
                                        bool trackingFatal = false;
                                        const bool gotFilterState = master->GetPDOFilterState(si, currentViolation, filterError, trackingFatal);
                                        const bool trackingActive = gotFilterState && PDOFilter::HasFlag(currentViolation, PDOFilter::ViolationType::TrackingError);
                                        bool softStartActive = false;
                                        try { softStartActive = master->IsPDOFilterSoftStartActive(si); } catch (...) { softStartActive = false; }

                                        if (softStartActive && !filterError && !trackingFatal) {
                                            trackingErrorSeenSince[si] = -1.0;
                                            continue;
                                        }

                                        if (trackingActive) {
                                            if (trackingErrorSeenSince[si] < 0.0) {
                                                trackingErrorSeenSince[si] = elapsedSec;
                                            }
                                        } else {
                                            trackingErrorSeenSince[si] = -1.0;
                                        }

                                        const bool persistentTrackingError = trackingFatal || filterError ||
                                            (trackingErrorSeenSince[si] >= 0.0 && (elapsedSec - trackingErrorSeenSince[si]) >= 0.5);
                                        if (persistentTrackingError) {
                                            return RequestPdoSafetyManualResetStop(si, PDOFilter::ViolationType::TrackingError, 0.5);
                                        }
                                    }
                                    return false;
                                };

                                if (playSoftStartSound.load() && sc_local > 0) {
                                    PlaySound(TEXT("SystemAsterisk"), NULL, SND_ALIAS | SND_ASYNC | SND_LOOP);
                                    this->softStartSoundPlaying_.store(true);
                                    // Sound state log removed for cleaner output
                                }

                                double lastStatusLogSec = -3.0; // force first log at t=0
                                double allCurrentlyReadySince = -1.0; // elapsed sec when all axes became simultaneously ready
                                const double SIMULTANEOUS_READY_SETTLE_SEC = 0.20; // require all axes simultaneously ready for 0.2s
                                while (waited < timeoutSec) {
                                    if (this->autoConnectAbortRequested.load()) {
                                        PostUTF8LogToUI(std::string("[AUTO] Soft Start aborted by Immediate/Emergency Stop\n"));
                                        break;
                                    }
                                    // Check current ready state of every axis (not one-way latch).
                                    // With the PDOFilter fix, ioSoftStartReady_ now resets when an
                                    // axis leaves the ready zone, so we can trust the live cached value.
                                    int currentlyReadyCount = 0;
                                    for (int si = 1; si <= sc_local; ++si) {
                                        bool r = false;
                                        try { r = master->IsPDOFilterSoftStartReady_Cached(si); } catch (...) { r = false; }
                                        if (r) {
                                            ++currentlyReadyCount;
                                            prevReady[si] = 1; // track for diagnostic logging
                                        }
                                    }
                                    if (checkPersistentTrackingError(waited)) {
                                        break;
                                    }
                                    // Require ALL axes to be simultaneously ready for a short
                                    // settle duration. This prevents declaring completion when
                                    // an axis briefly crosses the ready threshold while still
                                    // oscillating or overshooting.
                                    const bool allCurrentlyReady = (currentlyReadyCount == sc_local);
                                    if (allCurrentlyReady) {
                                        if (allCurrentlyReadySince < 0.0) {
                                            allCurrentlyReadySince = waited;
                                        } else if ((waited - allCurrentlyReadySince) >= SIMULTANEOUS_READY_SETTLE_SEC) {
                                            allReady = true; break;
                                        }
                                    } else {
                                        allCurrentlyReadySince = -1.0;
                                    }
                                    // Periodic status: log every ~3s which axes are still not ready
                                    if (waited - lastStatusLogSec >= 3.0) {
                                        lastStatusLogSec = waited;
                                        int notReadyCount = sc_local - currentlyReadyCount;
                                        if (notReadyCount > 0) {
                                            uint32_t wkc = 0;
                                            try { wkc = master->GetWorkingCounter(); } catch (...) {}
                                            std::ostringstream oss;
                                            oss << "[AUTO] Soft Start waiting t=" << std::fixed << std::setprecision(0) << waited
                                                << "s ready=" << currentlyReadyCount << "/" << sc_local
                                                << " WKC=" << wkc << "\n";
                                            PostUTF8LogToUI(oss.str());
                                        }
                                    }
                                    std::this_thread::sleep_for(std::chrono::duration<double>(pollIntervalSec));
                                    waited += pollIntervalSec;
                                }

                                bool abortedByGlobalError = false;
                                auto shouldAbortSoftStart = [this, master, &abortedByGlobalError]() -> bool {
                                    if (this->autoConnectAbortRequested.load()) return true;
                                    if (master && master->HasGlobalError()) {
                                        this->autoConnectAbortRequested.store(true);
                                        abortedByGlobalError = true;
                                        return true;
                                    }
                                    return false;
                                };

                                if (shouldAbortSoftStart()) {
                                    // Stop sound on external abort (only if no other alert requires it)
                                    this->softStartSoundPlaying_.store(false);
                                    if (!this->violationSoundPlaying_.load()) {
                                        try { PlaySound(NULL, NULL, 0); } catch(...) {}
                                    }
                                    PostUTF8LogToUI(abortedByGlobalError ?
                                        std::string("[AUTO] Soft Start aborted due to persistent global error\n") :
                                        std::string("[AUTO] Soft Start aborted by request\n"));
                                } else if (allReady) {
                                    // All axes simultaneously ready — clear the busy
                                    // flag BEFORE notifying AutoWindow so any subsequent
                                    // background poll sees snapshot.busy=false and does
                                    // not re-assert g_oBusy=true over our notification.
                                    this->autoConnectBusy.store(false);
                                    try {
                                        master->EnablePDOFiltering(true);
                                        NotifyAutoWindowOwnerState((WPARAM)0x2);
                                    } catch(...) {
                                        PostUTF8LogToUI(std::string("[AUTO] Failed to enable PDO filtering after readiness\n"));
                                    }
                                    master->RequestCompleteAllPDOFiltersSoftStart();
                                    this->softStartSoundPlaying_.store(false);
                                    if (!this->violationSoundPlaying_.load()) {
                                        try { PlaySound(NULL, NULL, 0); } catch(...) {}
                                    }
                                    // Force UI refresh now that busy is cleared
                                    PostAutoStatusUpdate();
                                    RequestMotionUiRefresh();
                                    PostUTF8LogToUI(std::string("[AUTO] All axes ready — requested master to complete Soft Start\n"));
                                } else {
                                    // Did not complete within initial timeout: log and continue
                                    // waiting until either all axes are ready or an abort is requested.
                                    PostUTF8LogToUI(std::string("[AUTO] Soft Start did not complete within timeout — continuing to wait until completion or abort\n"));

                                    // Continue polling but enforce an extended timeout (e.g., 5 seconds) to prevent
                                    // indefinite hanging if an axis is slightly out of tolerance or stuck.
                                    // Soft-start will persist until readiness, abort, or extended timeout.
                                    auto extendedWaitStart = std::chrono::steady_clock::now();
                                    const double EXTENDED_TIMEOUT_SEC = 5.0;
                                    // Log which axes were briefly ready during the initial wait
                                    // so we can distinguish "stuck far away" from "close but oscillating".
                                    {
                                        int readyCount = 0;
                                        for (int si = 1; si <= sc_local; ++si) {
                                            if (prevReady[si]) ++readyCount;
                                        }
                                        if (readyCount > 0) {
                                            std::ostringstream oss;
                                            oss << "[AUTO] " << readyCount << "/" << sc_local
                                                << " axes reached ready at least once during initial wait — extending close-range settle\n";
                                            PostUTF8LogToUI(oss.str());
                                        }
                                    }

                                    double extendedAllReadySince = -1.0;
                                    while (!this->autoConnectAbortRequested.load()) {
                                        if (shouldAbortSoftStart()) {
                                            break;
                                        }
                                        std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - extendedWaitStart;
                                        int extReadyCount = 0;
                                        for (int si = 1; si <= sc_local; ++si) {
                                            bool r = false;
                                            try { r = master->IsPDOFilterSoftStartReady_Cached(si); } catch (...) { r = false; }
                                            if (r) {
                                                ++extReadyCount;
                                                prevReady[si] = 1; // track for diagnostic logging
                                            }
                                        }

                                        if (checkPersistentTrackingError(timeoutSec + elapsed.count())) {
                                            abortedByGlobalError = true;
                                        }

                                        if (this->autoConnectAbortRequested.load()) {
                                            break;
                                        }

                                        // Require ALL axes to be simultaneously ready for a short
                                        // settle duration (same logic as the initial-wait loop).
                                        const bool extAllCurrentlyReady = (extReadyCount == sc_local);
                                        if (extAllCurrentlyReady) {
                                            if (extendedAllReadySince < 0.0) {
                                                extendedAllReadySince = elapsed.count();
                                            } else if ((elapsed.count() - extendedAllReadySince) >= SIMULTANEOUS_READY_SETTLE_SEC) {
                                                allReady = true; break;
                                            }
                                        } else {
                                            extendedAllReadySince = -1.0;
                                        }
                                        
                                        // Check for extended timeout
                                        if (elapsed.count() > EXTENDED_TIMEOUT_SEC) {
                                            PostUTF8LogToUI(std::string("[AUTO] Soft Start extended wait timed out (5s) — aborting and starting Emergency Stop\n"));
                                            // Log which axes are not ready with diagnostic detail
                                            auto inputAngles = master->GetPDOFilterInputAngles();
                                            for (int si = 1; si <= sc_local; ++si) {
                                                bool r = false;
                                                try { r = master->IsPDOFilterSoftStartReady_Cached(si); } catch (...) { r = false; }
                                                if (!r) {
                                                    bool softStartActive = false;
                                                    bool filterError = false;
                                                    bool trackingFatal = false;
                                                    PDOFilter::ViolationType vtype = PDOFilter::ViolationType::None;
                                                    try { softStartActive = master->IsPDOFilterSoftStartActive(si); } catch (...) {}
                                                    try { master->GetPDOFilterState(si, vtype, filterError, trackingFatal); } catch (...) {}
                                                    
                                                    std::ostringstream diag;
                                                    diag << "[AUTO] WARNING: Axis " << si << " was not ready before soft-start timeout stop";
                                                    diag << " softStartActive=" << (softStartActive ? 1 : 0);
                                                    diag << " filterError=" << (filterError ? 1 : 0);
                                                    diag << " trackingFatal=" << (trackingFatal ? 1 : 0);
                                                    diag << " violation=" << static_cast<int>(vtype);
                                                    if (si >= 1 && si <= 6) {
                                                        diag << std::fixed << std::setprecision(3);
                                                        diag << " mmfInputAngle=" << inputAngles[si - 1];
                                                    }
                                                    // Also log the actual mechanical position for comparison
                                                    try {
                                                        MechanicalUnitMetrics metrics;
                                                        if (master->TryGetMechanicalUnitMetricsLockFree(si, metrics) && metrics.valid) {
                                                            std::string actualUtf8 = CoreHelpers::WideToUtf8(metrics.actualPosition);
                                                            diag << " actualPos=" << actualUtf8;
                                                        }
                                                    } catch (...) {}
                                                    PostUTF8LogToUI(diag.str() + "\n");
                                                    PostUTF8LogToUI(std::string("[AUTO] WARNING: Axis ") + std::to_string(si) + " was not ready before soft-start timeout stop\n");
                                                }
                                            }
                                            this->autoConnectAbortRequested.store(true);
                                            HWND requester = (this->hAutoWindow && IsWindow(this->hAutoWindow)) ? this->hAutoWindow : this->hwnd;
                                            PostMessageW(this->hwnd, WM_USER_AUTO_EMERGENCY_STOP_REQUEST, reinterpret_cast<WPARAM>(requester), 0);
                                            break;
                                        }

                                        std::this_thread::sleep_for(std::chrono::duration<double>(pollIntervalSec));
                                    }
                                    if (this->autoConnectAbortRequested.load()) {
                                        // Stop sound on external abort during extended wait
                                        this->softStartSoundPlaying_.store(false);
                                        if (!this->violationSoundPlaying_.load()) {
                                            try { PlaySound(NULL, NULL, 0); } catch(...) {}
                                        }
                                        PostUTF8LogToUI(abortedByGlobalError ?
                                            std::string("[AUTO] Soft Start aborted due to persistent global error during extended wait\n") :
                                            std::string("[AUTO] Soft Start aborted by request during extended wait\n"));
                                    } else if (allReady) {
                                        // All ready after extended wait: clear busy first,
                                        // then enable filtering and request completion.
                                        this->autoConnectBusy.store(false);
                                        try {
                                            master->EnablePDOFiltering(true);
                                            NotifyAutoWindowOwnerState((WPARAM)0x2);
                                        } catch(...) {
                                            PostUTF8LogToUI(std::string("[AUTO] Failed to enable PDO filtering after extended readiness\n"));
                                        }
                                        master->RequestCompleteAllPDOFiltersSoftStart();
                                        this->softStartSoundPlaying_.store(false);
                                        if (!this->violationSoundPlaying_.load()) {
                                            try { PlaySound(NULL, NULL, 0); } catch(...) {}
                                        }
                                        PostAutoStatusUpdate();
                                        RequestMotionUiRefresh();
                                        PostUTF8LogToUI(std::string("[AUTO] All axes ready (after extended wait) — requested master to complete Soft Start\n"));
                                    }
                                }
                            } catch (...) {
                                this->softStartSoundPlaying_.store(false);
                                this->StopSystemAlertSoundIfIdle();
                                PostUTF8LogToUI(std::string("[AUTO] Error while coordinating Soft Start completion\n"));
                            }
                        }
                    } catch (...) {
                        PostUTF8LogToUI(std::string("[AUTO] Auto Connect thread unexpected error\n"));
                    }
                    // Ensure UI refresh and clear running/busy flags.
                    // Always force-clear autoConnectBusy and notify, even if
                    // it was already cleared by AbortAutoConnectSoftStartForStop
                    // (which can race with this cleanup). Without an explicit
                    // PostAutoStatusUpdate the AutoWindow may keep showing
                    // oBusy=true from a stale background poll until the next
                    // timer tick.
                    this->autoConnectRunning.store(false);
                    this->autoConnectBusy.store(false);
                    PostAutoStatusUpdate();
                    RequestMotionUiRefresh();
                    WPARAM ownerState = 0;
                    if (auto* doneMaster = AppController::GetInstance().GetMaster()) {
                        try {
                            if (doneMaster->IsInitialized() && doneMaster->IsPDOFilteringEnabled()) {
                                ownerState |= 0x2;
                            }
                        } catch (...) {
                        }
                    }
                    // Inform AutoWindow immediately the owner cleared busy while preserving online state.
                    NotifyAutoWindowOwnerState(ownerState);
                    // Clear any abort request flag so subsequent Auto Connect runs start clean
                    this->autoConnectAbortRequested.store(false);
                }).detach();
                return 0;
            }
            // Always refresh AutoWindow status lights
            PostAutoStatusUpdate();
        }
        return 0;
    case WM_USER_PDO_PREPARE_FOR_SLAVE:
        {
            auto req = reinterpret_cast<PreparePdoRequest*>(lParam);
            bool ok = false;
            if (req) {
                if (auto* master = AppController::GetInstance().GetMaster()) {
                    ok = pdoDisplay.RecalculateOffsetsFromSDO(master, req->slaveIdx);
                }
                try {
                    req->completion.set_value(ok);
                } catch (...) {}
                delete req;
            }
        }
        return 0;
    case WM_APP + 1: // tray icon message
        {
            switch (lParam) {
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONUP:
                // restore from tray
                minimizedToTray = false;
                isMinimizedState = false;
                RemoveTrayIcon();
                // First make window visible, then restore to normal size
                ShowWindow(hwnd, SW_SHOW);
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
                // Resume UI timers
                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                // Force reflow to ensure layout is correct after restore
                ReflowMainControls();
                break;
            case WM_RBUTTONUP:
                // show context menu: Restore / Exit
                {
                    POINT p; GetCursorPos(&p);
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuW(hMenu, MF_STRING, 20001, L"Restore");
                    // If the main window is currently minimized (to tray or taskbar),
                    // disable the Exit menu item to prevent accidental process exit.
                    UINT exitFlags = MF_STRING;
                    if (minimizedToTray || isMinimizedState) exitFlags |= MF_GRAYED;
                    AppendMenuW(hMenu, exitFlags, 20002, L"Exit");
                    SetForegroundWindow(hwnd);
                    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, p.x, p.y, 0, hwnd, NULL);
                    DestroyMenu(hMenu);
                }
                break;
            }
        }
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        // Special-case: Immediate Stop button should always appear red with white text
        if (uMsg == WM_CTLCOLORBTN) {
            HWND hw = (HWND)lParam;
            if (hw) {
                int id = GetDlgCtrlID(hw);
                if (id == IDC_BTN_IMMEDIATE_STOP && hbrStopButton) {
                    HDC hdc = (HDC)wParam;
                    SetBkMode(hdc, OPAQUE);
                    SetBkColor(hdc, RGB(220,20,20));
                    SetTextColor(hdc, RGB(255,255,255));
                    return (LRESULT)hbrStopButton;
                }
                // Auto Start (green) / Auto Stop (orange) special styling
                if (id == IDC_BTN_AUTO_MODE && hbrAutoStartButton) {
                    HWND hctrl = hw;
                    HDC hdc = (HDC)wParam;
                    SetBkMode(hdc, OPAQUE);
                    if (IsWindowEnabled(hctrl)) {
                        SetBkColor(hdc, RGB(0,200,0));
                        SetTextColor(hdc, RGB(255,255,255));
                        return (LRESULT)hbrAutoStartButton;
                    } else {
                        // disabled: use system button face
                        SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
                        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
                    }
                }
                if (id == IDC_BTN_MANUAL_MODE && hbrAutoStopButton) {
                    HWND hctrl = hw;
                    HDC hdc = (HDC)wParam;
                    SetBkMode(hdc, OPAQUE);
                    if (IsWindowEnabled(hctrl)) {
                        SetBkColor(hdc, RGB(255,140,0));
                        SetTextColor(hdc, RGB(255,255,255));
                        return (LRESULT)hbrAutoStopButton;
                    } else {
                        SetBkColor(hdc, GetSysColor(COLOR_BTNFACE));
                        SetTextColor(hdc, GetSysColor(COLOR_GRAYTEXT));
                        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
                    }
                }
            }
        }
        if (isDarkTheme) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, clrDarkControl);
            SetTextColor(hdc, clrDarkText);
            return (LRESULT)hbrDarkBackground;
        }
        break;
    case WM_USER_BEGIN_VALUE_EDIT:
        if (hListSDOParams && IsWindow(hListSDOParams)) {
            int sel = (int)wParam;
            int sub = (int)lParam; // subitem to edit (5=Value,6=Hex)
            if (sub < 0) sub = 5;
            if (sel >= 0 && sel < (int)currentObjects.size()) {
                // Destroy previous editor if any
                if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                    DestroyWindow(hInplaceEdit);
                    hInplaceEdit = NULL;
                    inplaceEditItem = -1;
                }

                // Get precise rect of requested subitem
                RECT rcSub{};
                int targetSub = sub; // subitem to edit (passed in lParam)
                if (ListView_GetSubItemRect(hListSDOParams, sel, targetSub, LVIR_BOUNDS, &rcSub)) {
                    // 確保目標列可見，避免初始即不在可視區造成錯位感
                    ListView_EnsureVisible(hListSDOParams, sel, FALSE);
                    // Temporarily clear selection/focus to avoid blue highlight under editor
                    ListView_SetItemState(hListSDOParams, sel, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    InvalidateRect(hListSDOParams, &rcSub, TRUE);
                    // Expand edit rect slightly to cover grid/selection lines
                    RECT rcEdit = rcSub;
                    rcEdit.left = (rcEdit.left > 1) ? rcEdit.left - 1 : 0;
                    rcEdit.top = (rcEdit.top > 1) ? rcEdit.top - 1 : 0;
                    rcEdit.right += 1;
                    rcEdit.bottom += 2; // a bit more to hide bottom blue line
                    // Convert to content-pane client coords (hContentWnd) so the
                    // in-place edit can be a child of the content pane and follow
                    // its scrolling/translation.
                    POINT pt = { rcEdit.left, rcEdit.top };
                    ClientToScreen(hListSDOParams, &pt);
                    ScreenToClient(hContentWnd, &pt);

                    // Read existing text for requested subitem
                    wchar_t buf[512] = {0};
                    ListView_GetItemText(hListSDOParams, sel, targetSub, buf, (int)_countof(buf));

                    // Create edit as child of the content pane so it moves with
                    // the content when the pane is scrolled.
                    hInplaceEdit = CreateChildControlEx(WS_EX_CLIENTEDGE, hContentWnd, hInstance, L"EDIT", buf,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
                        pt.x, pt.y, rcEdit.right - rcEdit.left, rcEdit.bottom - rcEdit.top,
                        10001, hGuiFont);
                    if (hInplaceEdit) {
                        // Explicitly clear read-only just in case
                        SendMessageW(hInplaceEdit, EM_SETREADONLY, FALSE, 0);
                        // Store subitem being edited
                        this->inplaceEditSubitem = sub;
                        // Save original text so we can detect whether user actually changed it
                        this->inplaceEditOriginalText = std::wstring(buf);
                        SafeSetPos(hInplaceEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                        SetFocus(hInplaceEdit);
                        SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
                        inplaceEditItem = sel;
                        SetWindowSubclass(hInplaceEdit, InplaceEditSubclassProc, 1, (DWORD_PTR)this);
                        // Re-assert focus shortly after, in case ListView steals it back
                        SetTimer(hwnd, TIMER_EDIT_FOCUS, 30, NULL);
                    }
                }
            }
        }
        break;
    case WM_USER_BEGIN_MOTION_VALUE_EDIT:
        if (IsAutoProcedureUiLockActive()) {
            ApplyAutoProcedureUiLockState();
            break;
        }
        if (hListMotionParams && IsWindow(hListMotionParams)) {
            int sel = (int)wParam;
            int sub = (int)lParam;
            if (sel >= 0) {
                // Block editing of P0 (Device Name) — auto-populated from drive info
                {
                    wchar_t idxBuf[32] = {0};
                    ListView_GetItemText(hListMotionParams, sel, 0, idxBuf, (int)_countof(idxBuf));
                    if (wcscmp(idxBuf, L"P0") == 0) {
                        break;
                    }
                }
                // destroy previous
                if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                    DestroyWindow(hInplaceEdit);
                    hInplaceEdit = NULL;
                    inplaceEditItem = -1;
                    inplaceEditIsMotion = false;
                }
                RECT rcSub{};
                // Use requested subitem if valid, otherwise default to Value column (3)
                int targetSub = (sub >= 0 && sub <= 5) ? sub : 3;
                if (ListView_GetSubItemRect(hListMotionParams, sel, targetSub, LVIR_BOUNDS, &rcSub)) {
                    ListView_EnsureVisible(hListMotionParams, sel, FALSE);
                    ListView_SetItemState(hListMotionParams, sel, 0, LVIS_SELECTED | LVIS_FOCUSED);
                    RECT rcEdit = rcSub;
                    rcEdit.left = (rcEdit.left > 1) ? rcEdit.left - 1 : 0;
                    rcEdit.top = (rcEdit.top > 1) ? rcEdit.top - 1 : 0;
                    rcEdit.right += 1;
                    rcEdit.bottom += 2;
                    POINT pt = { rcEdit.left, rcEdit.top };
                    ClientToScreen(hListMotionParams, &pt);
                    ScreenToClient(hContentWnd, &pt);
                    wchar_t buf[256] = {0};
                    ListView_GetItemText(hListMotionParams, sel, targetSub, buf, (int)_countof(buf));
                    hInplaceEdit = CreateChildControlEx(WS_EX_CLIENTEDGE, hContentWnd, hInstance, L"EDIT", buf,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
                        pt.x, pt.y, rcEdit.right - rcEdit.left, rcEdit.bottom - rcEdit.top,
                        10001, hGuiFont);
                    if (hInplaceEdit) {
                        SendMessageW(hInplaceEdit, EM_SETREADONLY, FALSE, 0);
                        this->inplaceEditSubitem = sub;
                        this->inplaceEditOriginalText = std::wstring(buf);
                        SetWindowPos(hInplaceEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                        SetFocus(hInplaceEdit);
                        SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
                        inplaceEditItem = sel;
                        inplaceEditIsMotion = true;
                        SetWindowSubclass(hInplaceEdit, InplaceEditSubclassProc, 1, (DWORD_PTR)this);
                        SetTimer(hwnd, TIMER_EDIT_FOCUS, 30, NULL);
                    }
                }
            }
        }
        break;
    case WM_CLOSE:
        // 禁用在連接或連接中關閉視窗
        if (disconnecting.load() || workerRunning.load() || IsMasterInitialized()) {
            MessageBoxW(hwnd, L"連接中或已連接，無法關閉視窗。請先斷開連接。", L"警告", MB_OK | MB_ICONWARNING);
            return 0;
    case WM_USER_AUTO_IMMEDIATE_STOP_REQUEST:
        {
            HWND hRequester = (HWND)wParam;
            bool isPDOErrorTriggered = ((lParam & AUTO_IMMEDIATE_STOP_FLAG_PDO_ERROR) != 0);
            bool requireManualReset = ((lParam & AUTO_IMMEDIATE_STOP_FLAG_REQUIRE_MANUAL_RESET) != 0);
            bool skipManualReset = ((lParam & AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET) != 0);
            int slaveNumber = (lParam >> 16) & 0xFFFF;
            HandleAutoImmediateStopRequest(hRequester, isPDOErrorTriggered, slaveNumber, requireManualReset, skipManualReset);
        }
        return 0;
    case WM_USER_AUTO_IMMEDIATE_STOP_DONE:
        if (this->autoDownToOriginPendingAfterStop) {
            HWND pendingRequester = this->autoDownToOriginPendingRequester;
            this->autoDownToOriginPendingAfterStop = false;
            this->autoDownToOriginPendingRequester = NULL;

            if ((int)wParam == 1) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Immediate Stop completed -> starting shared DownToOrigin workflow\n"));
                PostMessageW(hwnd,
                             WM_USER_AUTO_DOWN_TO_ORIGIN_REQUEST,
                             reinterpret_cast<WPARAM>(pendingRequester),
                             0);
            } else {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Immediate Stop failed -> cancelling shared DownToOrigin workflow\n"));
                NotifyAutoDownToOriginCompletion(pendingRequester, false);
            }
            return 0;
        }
        return 0;
    case WM_USER_AUTO_EMERGENCY_STOP_REQUEST:
        {
            HWND hRequester = (HWND)wParam;
            bool trackingErrorTriggered = ((lParam & 1) != 0);
            HandleAutoEmergencyStopRequest(hRequester, trackingErrorTriggered);
        }
        return 0;
    case WM_USER_AUTO_DOWN_TO_ORIGIN_REQUEST:
        {
            HWND hRequester = (HWND)wParam;
            const auto snapshot = GetSystemStateSnapshot();
            if (snapshot.online && !snapshot.busy) {
                if (this->autoDownToOriginPendingAfterStop) {
                    this->autoDownToOriginRequestPending.store(false);
                    PostUTF8LogToUI(std::string("[AutoDownToOrigin] stop-to-home sequence already pending\n"));
                    NotifyAutoDownToOriginCompletion(hRequester, false);
                    return 0;
                }

                this->autoDownToOriginPendingAfterStop = true;
                this->autoDownToOriginPendingRequester = hRequester;
                this->autoDownToOriginRequestPending.store(false);
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Online active -> requesting shared Immediate Stop before homing\n"));
                PostMessageW(hwnd,
                             WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                             reinterpret_cast<WPARAM>(hwnd),
                             AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
                return 0;
            }

            const auto decision = CoreHelpers::EvaluateAutoDownToOrigin(snapshot);
            if (!decision.allowed) {
                this->autoDownToOriginRequestPending.store(false);
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] rejected: ") +
                                CoreHelpers::DescribeCommandPolicyRejection(decision.rejection) + "\n");
                NotifyAutoDownToOriginCompletion(hRequester, false, false);
                PostAutoStatusUpdate();
                return 0;
            }
            if (autoDownToOriginRunning.exchange(true)) {
                this->autoDownToOriginRequestPending.store(false);
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] sequence already running\n"));
                NotifyAutoDownToOriginCompletion(hRequester, false, false);
                return 0;
            }
            this->autoDownToOriginRequestPending.store(false);
            // Stop main window timer to improve performance during homing
            KillTimer(hwnd, TIMER_ID);
            // NOTE: Removed WM_SETREDRAW disable/enable - it causes blank window issues
            // when background thread finishes and window needs to be repainted.
            // The original purpose was to prevent flashing, but it's not worth the tradeoff.
            // Clear last-success flag because a new DownToOrigin run is beginning
            this->autoDownToOriginLastSuccess.store(false);
            std::thread([this, hRequester]() {
                ConfigureWorkerThreadForRT();
                bool success = false;
                try {
                    success = this->RunAutoDownToOriginWorkflow();
                } catch (const std::exception &ex) {
                    std::string msg = std::string("[AutoDownToOrigin] exception: ") + ex.what() + "\n";
                    PostUTF8LogToUI(msg);
                } catch (...) {
                    PostUTF8LogToUI(std::string("[AutoDownToOrigin] unknown exception\n"));
                }
                this->autoDownToOriginRunning.store(false);
                // Record the result so GetAutoGlobalStatus can expose authoritative oHomeDone
                this->autoDownToOriginLastSuccess.store(success);
                if (success) {
                    SetManualResetRequired(false);
                    RefreshInternalErrorStateFrom603F("[AutoDownToOrigin] ");
                }
                // NOTE: WM_SETREDRAW re-enable removed - no longer needed since we don't disable it
                // Resume main window timer after homing completes
                // Only resume timer if window is NOT minimized (either to tray or to taskbar)
                // isMinimizedState covers minimization to taskbar
                // minimizedToTray covers minimization to tray
                // Extra safety: also check IsIconic to be absolutely sure
                if (hwnd && IsWindow(hwnd) && !minimizedToTray && !isMinimizedState && !IsIconic(hwnd)) {
                    SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                }
                // notify owner that auto-workflow finished
                // Removed: PostAutoStatusUpdate() - to avoid triggering AutoWindow updates
                // that could cause main window to show unexpectedly
                // Ensure abort flag is reset so subsequent runs are not immediately aborted.
                this->autoDownToOriginAbortRequested.store(false);
                // Notify requester (AutoWindow) if present; otherwise notify AutoWindow directly for web-originated requests.
                NotifyAutoDownToOriginCompletion(hRequester, success);
            }).detach();
        }
        return 0;
        }
        // 正常關閉視窗
        DestroyWindow(hwnd);
        return 0;
    case WM_USER + 250:
        // 查詢 MMF 輸出狀態
        if (hChkMMFOutput && IsWindow(hChkMMFOutput)) {
            LRESULT checked = SendMessage(hChkMMFOutput, BM_GETCHECK, 0, 0);
            return (checked == BST_CHECKED) ? 1 : 0;
        }
        return 0;
    case WM_USER_REQUEST_DISCONNECT:
        // Route disconnect requests through AppController so timer/watchdog and
        // UI button paths share the same public entry point.
        if (!disconnecting.load()) {
            AppController::GetInstance().Disconnect();
        }
        return 0;
    case WM_USER_RESTORE_WINDOW:
        if (minimizedToTray) {
            minimizedToTray = false;
            RemoveTrayIcon();
        }
        if (IsIconic(hwnd) || isMinimizedState) {
            ShowWindow(hwnd, SW_RESTORE);
        } else {
            ShowWindow(hwnd, SW_SHOW);
        }
        SetForegroundWindow(hwnd);
        return 0;
    case WM_USER_AUTO_REPORT_UI_STATE:
        if (ShouldRefreshMainWindowUi()) {
            BeginMainWindowVisualRefreshBatch();
            RefreshConfirmedSlaveControlState(false);
            EndMainWindowVisualRefreshBatch();
        }
        // AutoWindow requests UI state refresh for web clients
        PostCommandToUIOrCache(std::string("HTTP_CMD:REPORT_UI_STATE"));
        return 0;
    case WM_USER_POST_UTF8_LOG: {
        PostedUtf8UiMessage* message = reinterpret_cast<PostedUtf8UiMessage*>(lParam);
        return HandlePostedUtf8LogMessage(message);
    }
    case WM_APP_GET_STATUS: {
        // Delegate to non-UI implementation
        OnAppGetStatus();
        return 0;
    }
    case WM_USER_SDO_ITEM_READ: {
        std::string* ps = reinterpret_cast<std::string*>(lParam);
        if (ps) {
            std::wstring wmsg = CoreHelpers::Utf8ToWide(*ps);

                // Helper to process a single ITEM:... or ITEM_ERR:... message (in wide string form)
                auto processSingleEntry = [&](const std::wstring &entryW) {
                    // split by ':'
                    std::vector<std::wstring> parts;
                    size_t start = 0;
                    while (start < entryW.size()) {
                        size_t pos = entryW.find(L':', start);
                        if (pos == std::wstring::npos) { parts.push_back(entryW.substr(start)); break; }
                        parts.push_back(entryW.substr(start, pos - start));
                        start = pos + 1;
                    }

                    if (parts.size() >= 5 && parts[0] == L"ITEM") {
                        int s_slave = (int)std::stoul(parts[1]);
                        unsigned int idx = (unsigned)std::stoul(parts[2]);
                        unsigned int sub = (unsigned)std::stoul(parts[3]);
                        // join remainder parts into a single token string (usually contains hex bytes like "0x12 0x34")
                        std::wstring tokenStr = parts[4];
                        for (size_t pi = 5; pi < parts.size(); ++pi) { tokenStr += L":"; tokenStr += parts[pi]; }
                        // Parse tokenStr (BE hex bytes or decimal) and convert to a numeric value (LE interpretation)
                        std::vector<uint8_t> bytesBE;
                        bool anyHexTok = false;
                        CoreHelpers::ParseTokensToBytes(tokenStr, bytesBE, anyHexTok);
                        // Determine expected byte count from ESI (bitlen) for this index/sub
                        int expectedBytes = 0;
                        int matchedBitlen = 0;
                        for (const auto &obj : currentObjects) {
                            if (obj.index == (uint16_t)idx && obj.subindex == (uint8_t)sub && obj.bitlen > 0) {
                                matchedBitlen = obj.bitlen;
                                expectedBytes = (obj.bitlen + 7) / 8;
                                break;
                            }
                        }
                        if (matchedBitlen == 0) {
                            // No ESI bitlen available: default to at least 4 bytes so small responses are shown padded
                            expectedBytes = ((int)bytesBE.size() > 4 ? (int)bytesBE.size() : 4);
                        }
                        if (expectedBytes < 1) expectedBytes = 1;
                        // If parsed bytes are fewer than expected, pad leading MSB bytes with 0
                        std::vector<uint8_t> paddedBE(expectedBytes, 0);
                        if ((int)bytesBE.size() >= expectedBytes) {
                            // take the least-significant expectedBytes from bytesBE (align right)
                            int startIdx = (int)bytesBE.size() - expectedBytes;
                            for (int i = 0; i < expectedBytes; ++i) paddedBE[i] = bytesBE[startIdx + i];
                        } else {
                            // pad left with zeros and copy existing bytes to the right
                            int pad = expectedBytes - (int)bytesBE.size();
                            for (size_t i = 0; i < bytesBE.size(); ++i) paddedBE[pad + i] = bytesBE[i];
                        }

                        // Compute and format display according to ESI metadata if available
                        // Build BE hex display. For explicit hex tokens (anyHexTok==true)
                        // show a continuous 0xHEX string; otherwise show a padded,
                        // space-separated display.
                        std::wstring hexDisplay;
                        if (anyHexTok && !bytesBE.empty()) {
                            // Remove leading MSB 0x00 bytes to avoid padded zeros while
                            // keeping at least one byte. bytesBE[0] is MSB because
                            // ParseTokensToBytes preserves the textual order (BE
                            // tokens from Read-All are emitted MSB..LSB).
                            size_t startIdx = 0;
                            while (startIdx + 1 < bytesBE.size() && bytesBE[startIdx] == 0) ++startIdx;
                            std::wstringstream scont; scont << std::hex << std::uppercase << std::setfill(L'0');
                            scont << L"0x";
                            for (size_t bi = startIdx; bi < bytesBE.size(); ++bi) {
                                scont << std::setw(2) << (int)bytesBE[bi];
                            }
                            hexDisplay = scont.str();
                        } else {
                            // Build BE hex display from paddedBE (ensures full expected byte length shown)
                            std::wstringstream shex;
                            shex << std::hex << std::uppercase;
                            for (int bi = 0; bi < expectedBytes; ++bi) {
                                if (bi) shex << L" ";
                                shex << L"0x" << std::setw(2) << std::setfill(L'0') << (int)paddedBE[bi];
                            }
                            hexDisplay = shex.str();
                        }

                        for (int i = 0; i < (int)currentObjects.size(); ++i) {
                            if (currentObjects[i].index == (uint16_t)idx && currentObjects[i].subindex == (uint8_t)sub) {
                                // Determine value display based on currentObjects[i].dataType / bitlen
                                std::wstring valueDisplay;
                                const EsiObjectEntry &obj = currentObjects[i];
                                std::string dtype = obj.dataType; std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
                                // Compute LE integer first
                                unsigned long long u = 0; int maxb = expectedBytes; if (maxb > 8) maxb = 8;
                                for (int bi = 0; bi < maxb; ++bi) {
                                    int idxFromEnd = expectedBytes - 1 - bi;
                                    if (idxFromEnd >= 0) u |= (unsigned long long)paddedBE[idxFromEnd] << (8 * bi);
                                }
                                // BOOL
                                if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
                                    valueDisplay = (u ? std::wstring(L"1") : std::wstring(L"0"));
                                }
                                // REAL/FLOAT
                                else if (du.find("REAL") != std::string::npos || du.find("FLOAT") != std::string::npos) {
                                    if (obj.bitlen == 32 && expectedBytes >= 4) {
                                        
                                        // convert to LE ordering for memcpy
                                        uint32_t le = 0; for (int b = 0; b < 4; ++b) le |= (uint32_t)paddedBE[b] << (8*b);
                                        float f; memcpy(&f, &le, sizeof(f)); std::wstringstream ss; ss << std::fixed << std::setprecision(6) << f; valueDisplay = ss.str();
                                    } else if (obj.bitlen == 64 && expectedBytes >= 8) {
                                        uint64_t le = 0; for (int b = 0; b < 8; ++b) le |= (uint64_t)paddedBE[b] << (8*b);
                                        double d; memcpy(&d, &le, sizeof(d)); std::wstringstream ss; ss << std::fixed << std::setprecision(9) << d; valueDisplay = ss.str();
                                    } else {
                                        valueDisplay = std::to_wstring(u);
                                    }
                                }
                                // Signed int
                                else if (du.find("INT") != std::string::npos || du.find("SINT") != std::string::npos || du.find("SIGNED") != std::string::npos) {
                                    int bits = (obj.bitlen > 0 ? obj.bitlen : (expectedBytes * 8));
                                    long long sval = 0;
                                    if (bits <= 64) {
                                        if (bits < 64) {
                                            unsigned long long mask = 1ULL << (bits - 1);
                                            if (u & mask) {
                                                unsigned long long signmask = (~0ULL) << bits;
                                                sval = (long long)(u | signmask);
                                            } else sval = (long long)u;
                                        } else sval = (long long)u;
                                    }
                                    valueDisplay = std::to_wstring(sval);
                                }
                                else {
                                    valueDisplay = std::to_wstring(u);
                                }

                                // store into caches
                                auto &vec = savedSDOValues[s_slave]; if ((int)vec.size() < (int)currentObjects.size()) vec.resize(currentObjects.size());
                                vec[i] = valueDisplay;
                                auto &hexvec = savedSDOHexValues[s_slave]; if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                                hexvec[i] = hexDisplay;

                                // if currently viewing this slave, update ListView Value and Hex columns
                                if (confirmedSlave == s_slave && hListSDOParams && IsWindow(hListSDOParams)) {
                                    ListView_SetItemText(hListSDOParams, i, 5, const_cast<LPWSTR>(vec[i].c_str()));
                                    ListView_SetItemText(hListSDOParams, i, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
                                }
                                break;
                            }
                        }
                    } else if (parts.size() >= 5 && parts[0] == L"ITEM_ERR") {
                        int s_slave = (int)std::stoul(parts[1]);
                        unsigned int idx_unused = (unsigned)std::stoul(parts[2]);
                        unsigned int sub_unused = (unsigned)std::stoul(parts[3]);
                        (void)idx_unused; (void)sub_unused; // parsed but not used for errors
                        std::wstring err = parts[4];
                        for (size_t pi = 5; pi < parts.size(); ++pi) { err += L":"; err += parts[pi]; }
                        // Show error in status with slave context
                        std::wstringstream wss; wss << L"[從站 " << s_slave << L"] SDO 讀取錯誤: " << err.c_str();
                        PrependStatus(wss.str().c_str());
                    } else {
                        PrependStatus(entryW.c_str());
                    }
                };

                // If message is a BATCH:, split on '|' and process each entry. Otherwise handle single ITEM/ITEM_ERR or raw text.
                if (wmsg.rfind(L"BATCH:", 0) == 0) {
                    std::wstring rest = wmsg.substr(6); // after "BATCH:"
                    size_t pos = 0;
                    while (pos < rest.size()) {
                        size_t next = rest.find(L'|', pos);
                        std::wstring entry = (next == std::wstring::npos) ? rest.substr(pos) : rest.substr(pos, next - pos);
                        if (!entry.empty()) processSingleEntry(entry);
                        if (next == std::wstring::npos) break; pos = next + 1;
                    }
                } else {
                    // single message: pass through
                    processSingleEntry(wmsg);
                }
                delete ps;
        }
        return 0;
    }
    // ... existing cases ...
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_CONNECT:
            AppController::GetInstance().Connect(std::string());
            break;
        case IDC_BTN_DISCONNECT:
            AppController::GetInstance().Disconnect();
            break;
        case IDC_BTN_SAVE_PARAMS:
            OnSaveParams();
            break;
        case IDC_BTN_READ_SDO:
            OnReadSDO();
            break;
        case IDC_BTN_WRITE_SDO:
            OnWriteSDO();
            break;
        case IDC_BTN_READ_ALL_SDO:
            OnReadAllSDO();
            break;
        case IDC_BTN_WRITE_ALL_SDO:
            OnWriteAllSDO();
            break;
        case IDC_BTN_SEND_MAILBOX:
            OnSendMailbox();
            break;
        case 2050: // 網頁操作介面
            OpenHttpOperationInterface(true, false, true);
        break;
    
    // via HTTP command bridging (server posts HTTP_CMD:DUMP_ESI handled elsewhere).
        case IDC_BTN_BROWSE_ESI:
            OnBrowseESI();
            break;
        case IDC_BTN_LOAD_CONFIG:
        {
            // Allow user to browse for an explicit XML file. If the user cancels, do nothing.

            // Determine default filename/path: prefer saved per-slave path, then global saved path,
            // otherwise use motion.xml as default filename.
            int slaveIdx = -1;
            if (hComboSlaves && IsWindow(hComboSlaves)) {
                LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) slaveIdx = (int)sel;
            }
            std::wstring defaultPath = L"motion.xml";
            int targetSlaveForDefault = (confirmedSlave > 0) ? confirmedSlave : ((slaveIdx >= 0) ? (slaveIdx + 1) : -1);
            if (targetSlaveForDefault > 0) {
                auto itcfg = savedMotionConfigForSlave.find(targetSlaveForDefault);
                if (itcfg != savedMotionConfigForSlave.end() && !itcfg->second.empty()) defaultPath = itcfg->second;
            }
            if (defaultPath == L"motion.xml" && !savedMotionConfigGlobal.empty()) {
                defaultPath = savedMotionConfigGlobal;
            }
            
            // Get the directory of the executable and build the path to \Par
            wchar_t szExePath[MAX_PATH] = {0};
            wchar_t szInitialDir[MAX_PATH] = {0};
            GetModuleFileNameW(NULL, szExePath, MAX_PATH);
            
            // Remove filename to get directory
            wchar_t* pLastBackslash = wcsrchr(szExePath, L'\\');
            if (pLastBackslash) {
                *pLastBackslash = L'\0';
            }
            
            // Construct the Par directory path
            swprintf_s(szInitialDir, MAX_PATH, L"%s\\Par", szExePath);
            CreateDirectoryW(szInitialDir, NULL); // Create if not exists
            
            wchar_t szFile[MAX_PATH] = {0};
            // copy either full path or filename into buffer
            wcscpy_s(szFile, defaultPath.c_str());
            OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"XML Files\0*.xml\0All Files\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = _countof(szFile);
            ofn.lpstrInitialDir = szInitialDir;
            ofn.nFilterIndex = 1; // Default to XML Files filter
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            ofn.lpstrDefExt = L"xml";

            std::string loadedUtf8;
            if (GetOpenFileNameW(&ofn)) {
                // User selected a file: load that explicit path
                std::wstring selW(szFile);
                // Remember selected path: if connected and slave selected, associate with that slave; else store globally
                if (IsMasterInitialized() && confirmedSlave > 0) {
                    savedMotionConfigForSlave[confirmedSlave] = selW;
                } else {
                    savedMotionConfigGlobal = selW;
                }
                // 先嘗試以整包快取格式載入 (MotionCache)。若失敗再回退到單一表格格式 (MotionParams)。
                {
                    std::map<int, std::vector<std::vector<std::wstring>>> tmpCache;
                    MotionConfigIO::MotionCacheLoadDiagnostics loadDiagnostics;
                    if (MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(tmpCache, selW, loadedUtf8, &loadDiagnostics)) {
                        // 替換整個記憶體快取
                        savedMotionParamsForSlave = std::move(tmpCache);
                        // 重繪目前站
                        if (hListMotionParams && IsWindow(hListMotionParams)) {
                            ListView_DeleteAllItems(hListMotionParams);
                            int current = (confirmedSlave > 0) ? confirmedSlave : 1;
                            auto mit = savedMotionParamsForSlave.find(current);
                            if (mit != savedMotionParamsForSlave.end()) {
                                for (size_t r = 0; r < mit->second.size(); ++r) {
                                    LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.mask = LVIF_TEXT; lvi.iItem = (int)r; lvi.iSubItem = 0;
                                    std::wstring c0 = (mit->second[r].size() > 0 ? mit->second[r][0] : L"");
                                    lvi.pszText = const_cast<LPWSTR>(c0.c_str());
                                    ListView_InsertItem(hListMotionParams, &lvi);
                                    for (size_t c = 1; c < mit->second[r].size() && c < 6; ++c) {
                                        ListView_SetItemText(hListMotionParams, (int)r, (int)c, const_cast<LPWSTR>(mit->second[r][c].c_str()));
                                    }
                                }
                            }
                        }
                        if (loadDiagnostics.usedDefaultTemplate || loadDiagnostics.repairedInvalidValues) {
                            std::wstring msg = L"motion.xml 已自動修復並套用預設值: ";
                            msg += selW;
                            msg += L"\r\n";
                            PrependStatus(msg.c_str());
                        }
                        // Log handled by MainWindow.Logic.cpp to avoid duplicate messages
                    } else if (MotionConfigIO::LoadMotionParamsFromFileAtPath(hListMotionParams, selW, loadedUtf8)) {
                        // 單一表格載入：同步寫回目前/選定站的快取
                        // Single-table load: message handled in logic layer; skip duplicate PrependStatus here
                        int targetSlave = (confirmedSlave > 0 ? confirmedSlave : ((slaveIdx >= 0) ? (slaveIdx + 1) : 1));
                        if (hListMotionParams && IsWindow(hListMotionParams)) {
                            int rows = ListView_GetItemCount(hListMotionParams);
                            std::vector<std::vector<std::wstring>> mat;
                            mat.reserve(rows);
                            for (int r = 0; r < rows; ++r) {
                                std::vector<std::wstring> cols(6);
                                for (int c = 0; c < 6; ++c) {
                                    wchar_t buf[512] = {0};
                                    ListView_GetItemText(hListMotionParams, r, c, buf, (int)_countof(buf));
                                    cols[c] = buf;
                                }
                                mat.push_back(std::move(cols));
                            }
                            savedMotionParamsForSlave[targetSlave] = std::move(mat);
                        }
                    } else {
                        std::wstring msg = L"設定檔讀取失敗: ";
                        msg += selW;
                        msg += L"\r\n";
                        PrependStatus(msg.c_str());
                    }
                }
            } else {
                // User cancelled: do not auto-load any default file
                PrependStatus(L"設定檔讀取已取消\r\n");
            }
        }
        break;
        case IDC_BTN_IMPORT_PARAMS:
            OnImportParams();
            break;
        case IDC_CHK_MMF_OUTPUT:
        {
            // 處理 MMF 輸出勾選框的狀態變更
            if (hChkMMFOutput && IsWindow(hChkMMFOutput)) {
                LRESULT checked = SendMessage(hChkMMFOutput, BM_GETCHECK, 0, 0);
                bool enable = (checked == BST_CHECKED);
                auto* master = AppController::GetInstance().GetMaster();
                
                if (enable) {
                    // 啟用 MMF 輸出
                    const std::wstring mappingName = L"Local\\WuEtherCAT_PDO";
                    const std::wstring pdoLogPath = ResolvePdoLogPath();
                    if (!DeleteFileW(pdoLogPath.c_str())) {
                        const DWORD deleteError = GetLastError();
                        if (deleteError != ERROR_FILE_NOT_FOUND && deleteError != ERROR_PATH_NOT_FOUND) {
                            wchar_t msg[256] = {0};
                            swprintf_s(msg,
                                       L"啟用 MMF 前刪除舊 PDO.log 失敗，請等待背景寫檔完成後再試 (錯誤 %lu)\r\n",
                                       static_cast<unsigned long>(deleteError));
                            PrependStatus(msg);
                            SendMessage(hChkMMFOutput, BM_SETCHECK, BST_UNCHECKED, 0);
                            if (hStaticMMFInfo && IsWindow(hStaticMMFInfo)) {
                                SetWindowTextW(hStaticMMFInfo, L"MMF: disabled  資料存檔在\\logs\\PDO.log");
                            }
                            break;
                        }
                    }
                    if (master && master->EnableMMFOutput(mappingName)) {
                        PrependStatus(L"MMF 即時輸出已啟用 (Local\\WuEtherCAT_PDO)\r\n");
                        // 顯示 MMF 連線資訊於靜態文字
                        if (hStaticMMFInfo && IsWindow(hStaticMMFInfo)) {
                            std::wstring info = L"Mapping: ";
                            info += mappingName;
                            info += L"  Magic:WUMF v1  資料存檔在\\logs\\PDO.log";
                            SetWindowTextW(hStaticMMFInfo, info.c_str());
                        }
                        // 開始在記憶體中循環紀錄 PDO 資料 (不會在此執行寫檔)
                        if (master) master->StartPDORecording();
                        // 廣播狀態給 SSE 客戶端
                        BroadcastSSE("{\"mmfOutput\":true}");
                    } else {
                        PrependStatus(L"MMF 即時輸出啟用失敗\r\n");
                        // 失敗時取消勾選
                        SendMessage(hChkMMFOutput, BM_SETCHECK, BST_UNCHECKED, 0);
                        if (hStaticMMFInfo && IsWindow(hStaticMMFInfo)) SetWindowTextW(hStaticMMFInfo, L"MMF: disabled  資料存檔在\\logs\\PDO.log");
                    }
                } else {
                    // 停用 MMF 輸出
                    if (master) {
                        master->DisableMMFOutput();
                        // 停止紀錄並非同步寫檔至 logs\PDO.log
                        master->StopPDORecordingAndFlushToFile(ResolvePdoLogPath());
                        PrependStatus(L"MMF 即時輸出已停用，PDO 記錄寫入中（背景處理）\r\n");
                        if (hStaticMMFInfo && IsWindow(hStaticMMFInfo)) SetWindowTextW(hStaticMMFInfo, L"MMF: disabled  資料存檔在\\logs\\PDO.log");
                        // 廣播狀態給 SSE 客戶端
                        BroadcastSSE("{\"mmfOutput\":false}");
                    }
                }
            }
        }
        break;
        case IDC_CHK_HTTP_SERVER:
        {
            if (hChkHttpServer && IsWindow(hChkHttpServer)) {
                LRESULT checked = SendMessage(hChkHttpServer, BM_GETCHECK, 0, 0);
                bool enable = (checked == BST_CHECKED);
                // persist selection to INI
                WriteIniBool(L"Server", L"EnableLocalHttpServer", enable);
                if (enable) {
                    StartLocalHttpServer(8080, hwnd);
                    PrependStatus(L"Web 伺服器啟動 (埠 8080)\r\n");
                } else {
                    StopLocalHttpServer();
                    // Close the browser and release all web resources when disabling the server
                    CleanupTrackedBrowserHandle(true);
                    PrependStatus(L"Web 伺服器已停止 (資源已釋放)\r\n");
                }
                UpdateHttpServerUrl();
            }
        }
        break;
        case IDC_CHK_STARTUP_SHOWMAIN:
        {
            if (hChkStartupShowMain && IsWindow(hChkStartupShowMain)) {
                LRESULT checked = SendMessage(hChkStartupShowMain, BM_GETCHECK, 0, 0);
                bool enable = (checked == BST_CHECKED);
                WriteIniBool(L"Startup", L"ShowMainWindow", enable);
                if (enable) {
                    // show main window immediately
                    Show(SW_RESTORE);
                    PrependStatus(L"設定已儲存：啟動時顯示主視窗（立即套用）\r\n");
                } else {
                    // minimize to tray immediately
                    MinimizeToTray();
                    PrependStatus(L"設定已儲存：啟動時最小化至系統匣（立即套用）\r\n");
                }
            }
        }
        break;
        case IDC_CHK_STARTUP_AUTOSEQ:
        {
            if (hChkStartupAutoSeq && IsWindow(hChkStartupAutoSeq)) {
                LRESULT checked = SendMessage(hChkStartupAutoSeq, BM_GETCHECK, 0, 0);
                bool enable = (checked == BST_CHECKED);
                WriteIniBool(L"Startup", L"AutoTrigger", enable);

                if (enable) {
                    // 勾選時：若 EtherCAT 已連線且通訊正常，無條件立即執行自動程序啟動
                    const auto snapshot = GetSystemStateSnapshot();
                    if (IsMasterInitialized() && snapshot.ethercat_ok && !snapshot.busy &&
                        !IsAutoProcedureRunning()) {
                        PrependStatus(L"設定已儲存：啟動時自動執行 Auto — EtherCAT 通訊正常，立即執行自動程序啟動\r\n");
                        PostUTF8LogToUI(std::string("[AutoStartup] 勾選自動執行 Auto，EtherCAT 已連線，立即啟動自動程序\n"));
                        AppController::GetInstance().RequestStartAutoProcedure(true);
                    } else {
                        PrependStatus(L"設定已儲存：啟動時自動執行 Auto（EtherCAT 連線後將自動執行）\r\n");
                    }
                } else {
                    PrependStatus(L"設定已儲存：停用啟動時自動執行 Auto\r\n");
                }
            }
        }
        break;
        case IDC_BTN_SAVE_CONFIG:
        {
            // 此按鈕改為：將「運動參數表」的記憶體陣列（所有從站）存成 XML 檔。
            // 需可指定存檔路徑：顯示另存新檔對話框，預設檔名 motion.xml
            std::string savedUtf8;
            // 若有尚未提交的運動表單內嵌編輯，先提交並同步到快取
            if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditIsMotion && hListMotionParams && IsWindow(hListMotionParams)) {
                wchar_t newVal[512] = {0};
                GetWindowTextW(hInplaceEdit, newVal, (int)_countof(newVal));
                std::wstring newStr(newVal);
                if (inplaceEditItem >= 0 && inplaceEditSubitem >= 0) {
                    ListView_SetItemText(hListMotionParams, inplaceEditItem, inplaceEditSubitem, const_cast<LPWSTR>(newStr.c_str()));
                    if (confirmedSlave > 0) {
                        auto &mat = savedMotionParamsForSlave[confirmedSlave];
                        int rows = ListView_GetItemCount(hListMotionParams);
                        if ((int)mat.size() < rows) mat.resize(rows);
                        if (inplaceEditItem >= 0 && inplaceEditItem < (int)mat.size()) {
                            if ((int)mat[inplaceEditItem].size() < 6) mat[inplaceEditItem].resize(6);
                            if (inplaceEditSubitem >= 0 && inplaceEditSubitem < 6) mat[inplaceEditItem][inplaceEditSubitem] = newStr;
                        }
                    }
                }
                DestroyWindow(hInplaceEdit);
                hInplaceEdit = NULL;
                inplaceEditItem = -1;
                inplaceEditSubitem = 3;
                inplaceEditIsMotion = false;
            }

            // Save As dialog
            wchar_t szFile[MAX_PATH] = L"motion.xml";
            OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"XML Files\0*.xml\0All Files\0*.*\0";
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = _countof(szFile);
            ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
            ofn.lpstrDefExt = L"xml";

            if (GetSaveFileNameW(&ofn)) {
                std::wstring selW(szFile);
                if (MotionConfigIO::SaveMotionCacheToFileAtPath(savedMotionParamsForSlave, selW, savedUtf8)) {
                    std::wstring msg = L"已將記憶體陣列儲存為: ";
                    msg += CoreHelpers::Utf8ToWide(savedUtf8);
                    msg += L"\r\n";
                    PrependStatus(msg.c_str());
                } else {
                    PrependStatus(L"運動控制表存檔失敗\r\n");
                }
            } else {
                PrependStatus(L"運動控制表存檔已取消\r\n");
            }
        }
        break;
    // Use IDC_BTN_LOAD_CONFIG for full package load (IDC_BTN_LOAD_ALL_CACHE removed)
        case IDC_BTN_AUTO_MODE: // 自動程序啟動
            AppController::GetInstance().RequestStartAutoProcedure(true);
            break;
        case IDC_BTN_MANUAL_MODE: // 自動程序停止
            AppController::GetInstance().RequestStopAutoProcedure();
            break;
        case IDC_BTN_HOME:
            // Button click via WM_COMMAND - treat as press
            AppController::GetInstance().Home(true);
            break;
        case 1032:
            OnClearStatus();
            break;
        case 20001: // Tray Restore
            minimizedToTray = false;
            isMinimizedState = false;
            RemoveTrayIcon();
            ShowWindow(hwnd, SW_RESTORE);
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            // Resume periodic UI timer immediately so diagnostics refresh after restore
            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
            // Ensure layout is recalculated and painted after restoring from tray
            cachedSlaveListHeight = 0;
            cachedSDOListHeight = 0;
            ReflowMainControls();
            ForceFullRedrawNow();
            if (hListSDOParams && IsWindow(hListSDOParams)) UpdateWindow(hListSDOParams);
            if (hEditMailbox && IsWindow(hEditMailbox)) UpdateWindow(hEditMailbox);
            if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) UpdateWindow(hBtnSendMailbox);
            break;
        case 20002: // Tray Exit
            // Prevent exit when the main window is minimized to tray or minimized to taskbar.
            if (minimizedToTray || isMinimizedState) {
                // Inform the user to restore the window first.
                MessageBoxW(hwnd, L"請先還原主視窗後再退出程式。", L"無法退出 (視窗已縮小)", MB_OK | MB_ICONINFORMATION);
            } else {
                DestroyWindow(hwnd);
            }
            break;

    case WM_NOTIFY:
        {
            LPNMHDR nm = (LPNMHDR)lParam;
            if (!nm) break;
            // Auto-cycle spin handling removed; SDO polling period fixed in EtherCATMaster.
            // Click/Double-click on SDO ListView -> start in-place edit for Value column (subitem 4)
            if (nm->hwndFrom == hListSDOParams && (nm->code == NM_CLICK || nm->code == NM_DBLCLK)) {
                LPNMITEMACTIVATE lpnmitem = (LPNMITEMACTIVATE)lParam;
                // Use hit-test to robustly detect subitem under cursor
                LVHITTESTINFO hti{};
                hti.pt = lpnmitem->ptAction; // point in ListView client coords
                int hitItem = ListView_SubItemHitTest(hListSDOParams, &hti);
                if (hitItem < 0) break; // no item
                // Only allow editing when clicking Value (4) or Hex (5) columns
                int sel = hti.iItem;
                int clickedSub = hti.iSubItem;
                if (sel < 0 || sel >= (int)currentObjects.size()) break;
                // ensure selection follows the clicked row
                ListView_SetItemState(hListSDOParams, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                if (clickedSub == 5 || clickedSub == 6) {
                    PostMessage(hwnd, WM_USER_BEGIN_VALUE_EDIT, (WPARAM)sel, (LPARAM)clickedSub);
                } else {
                    // Non-editable column clicked: allow selection/copy but do not open editor
                }
                return 0;
            }
            // Motion list: handled by subclass to allow single-click edit without selection; ignore here
        }
        break;
        // SDO selection is handled via the parameter ListView (hListSDOParams)
        case 2001: // hComboSlaves selection changed
            // Only apply selection when user confirms choice (CBN_SELENDOK)
            if (HIWORD(wParam) == CBN_SELENDOK) {
                // User changed slave selection - always attempt to update PDO display
                LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    // Ensure any in-place motion edit is committed before we read/save current table
                    if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditIsMotion && hListMotionParams && IsWindow(hListMotionParams)) {
                        wchar_t newVal[512] = {0};
                        GetWindowTextW(hInplaceEdit, newVal, (int)_countof(newVal));
                        std::wstring newStr(newVal);
                        if (inplaceEditItem >= 0 && inplaceEditSubitem >= 0) {
                            ListView_SetItemText(hListMotionParams, inplaceEditItem, inplaceEditSubitem, const_cast<LPWSTR>(newStr.c_str()));
                        }
                        DestroyWindow(hInplaceEdit);
                        hInplaceEdit = NULL;
                        inplaceEditItem = -1;
                        inplaceEditSubitem = 3;
                        inplaceEditIsMotion = false;
                    }

                    // Prefer item data (bound slave position) when available
                    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
                    int slaveIdx = (int)itemData;
                    if (slaveIdx <= 0) slaveIdx = (int)(sel + 1); // fallback to index-based mapping

                    // Before changing confirmedSlave, save current displayed values for previous confirmedSlave
                    if (confirmedSlave > 0 && hListSDOParams && IsWindow(hListSDOParams)) {
                        std::vector<std::wstring> vals;
                        for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
                            wchar_t buf[512] = {0};
                            ListView_GetItemText(hListSDOParams, ii, 5, buf, (int)_countof(buf));
                            vals.push_back(std::wstring(buf));
                        }
                        savedSDOValues[confirmedSlave] = vals;
                        // 保存目前 PDO 可編輯輸入到全域名稱快取
                        CaptureCurrentPDOInputsByName();
                    }

                    // NOTE: 按需求，切換選定從站時，不得更改運動參數記憶體陣列。
                    // 因此不在此處將目前 UI 的運動參數回寫到 savedMotionParamsForSlave。

                    // User confirmed selection: store as confirmedSlave and update display
                    confirmedSlave = slaveIdx;

                    // Restore motion parameters for the newly selected slave
                    if (hListMotionParams && IsWindow(hListMotionParams)) {
                        // Always clear the table first (remove all rows)
                        ListView_DeleteAllItems(hListMotionParams);
                        // Then load strictly from in-memory cache for this slave
                        auto mit = savedMotionParamsForSlave.find(confirmedSlave);
                        if (mit != savedMotionParamsForSlave.end()) {
                            for (size_t r = 0; r < mit->second.size(); ++r) {
                                // insert row with column 0 text
                                LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.mask = LVIF_TEXT; lvi.iItem = (int)r; lvi.iSubItem = 0;
                                std::wstring c0 = (mit->second[r].size() > 0 ? mit->second[r][0] : L"");
                                lvi.pszText = const_cast<LPWSTR>(c0.c_str());
                                ListView_InsertItem(hListMotionParams, &lvi);
                                // set remaining columns
                                for (size_t c = 1; c < mit->second[r].size() && c < 6; ++c) {
                                    ListView_SetItemText(hListMotionParams, (int)r, (int)c, const_cast<LPWSTR>(mit->second[r][c].c_str()));
                                }
                            }
                        } else {
                            // No cached data -> initialize UI defaults ONLY (不寫入快取，避免在切換時改變記憶體)
                            const int defaultRows = 10;
                            for (int ri = 0; ri < defaultRows; ++ri) {
                                std::wstring p = L"P" + std::to_wstring(ri + 1);
                                LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
                                lvi.mask = LVIF_TEXT; lvi.iItem = ri; lvi.iSubItem = 0; lvi.pszText = const_cast<LPWSTR>(p.c_str());
                                ListView_InsertItem(hListMotionParams, &lvi);
                                ListView_SetItemText(hListMotionParams, ri, 1, const_cast<LPWSTR>(L""));
                                ListView_SetItemText(hListMotionParams, ri, 2, const_cast<LPWSTR>(L" "));
                                ListView_SetItemText(hListMotionParams, ri, 3, const_cast<LPWSTR>(L""));
                                ListView_SetItemText(hListMotionParams, ri, 4, const_cast<LPWSTR>(L""));
                                ListView_SetItemText(hListMotionParams, ri, 5, const_cast<LPWSTR>(L""));
                            }
                        }
                    }

                    // If we have saved values for this slave, restore them into the ListView value column
                    auto savedIt = savedSDOValues.find(confirmedSlave);
                    auto ithex = savedSDOHexValues.find(confirmedSlave);
                    if (savedIt != savedSDOValues.end() && hListSDOParams && IsWindow(hListSDOParams)) {
                        const std::vector<std::wstring>& vals = savedIt->second;
                        for (int ii = 0; ii < (int)vals.size() && ii < (int)currentObjects.size(); ++ii) {
                            // restore decimal value into Value column (now col 5)
                            ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(vals[ii].c_str()));
                            // restore hex if available into Hex column (col 6)
                            if (ithex != savedSDOHexValues.end() && (int)ithex->second.size() > ii) {
                                ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(ithex->second[ii].c_str()));
                            } else {
                                ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
                            }
                        }
                    } else if (hListSDOParams && IsWindow(hListSDOParams)) {
                        // No saved values for this slave - clear the value and hex columns
                        for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
                            ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(L""));
                            ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
                        }
                    }
                    RefreshConfirmedSlaveControlState(true);
                }
            }
            break;
        case IDC_EDIT_ADAPTER:
            // user changed adapter selection in native combobox
            if (HIWORD(wParam) == CBN_SELENDOK) {
                int sel = (int)SendMessageW(hComboAdapter, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
                    std::string ifname;
                    TryGetAdapterPcapNameFromComboSelection(sel, ifname);
                    if (!ifname.empty()) {
                        SetSelectedAdapter(ifname);
                        // Also post a short marker so the HTTP server can scan status logs for a selected adapter
                        PostUTF8LogToUI(std::string("HTTP_LOCAL:SELECTED_ADAPTER:") + ifname + "\n");
                    } else {
                        // fallback: use visible text
                        wchar_t buf[512] = {0};
                        GetWindowTextW(hComboAdapter, buf, (int)_countof(buf));
                        std::string vis = CoreHelpers::WideToUtf8(std::wstring(buf));
                        if (!vis.empty()) {
                            SetSelectedAdapter(vis);
                            PostUTF8LogToUI(std::string("HTTP_LOCAL:SELECTED_ADAPTER:") + vis + "\n");
                        }
                    }
                }
            }
            break;
        case IDC_CHECK_PDO_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED) {
                // Toggle enable state
                LRESULT checked = SendMessageW(hCheckPDOEnable, BM_GETCHECK, 0, 0);
                if (checked == BST_CHECKED) {
                    PrependStatus(L"PDO enabled\r\n");
                } else {
                    PrependStatus(L"PDO disabled\r\n");
                }
            }
            break;
        case IDC_BTN_PDO_START:
            // Start PDO operation (UI only - read Target and update Current display)
            {
                wchar_t targetW[64] = {0};
                GetWindowTextW(hEditPDOTargetPosition, targetW, (int)_countof(targetW));
                // For now, simulate immediate movement by copying target to current
                SetWindowTextW(hEditPDOCurrentPosition, targetW);
                std::wstring msg = L"PDO Start requested. Target=" + std::wstring(targetW) + L"\r\n";
                PrependStatus(msg.c_str());
                SetControlEnabled(hButtonPDOStart, false);
                SetControlEnabled(hButtonPDOStop, true);
            }
            break;
        case IDC_BTN_PDO_STOP:
            PrependStatus(L"PDO Stop requested\r\n");
                SetControlEnabled(hButtonPDOStart, true);
            SetControlEnabled(hButtonPDOStop, false);
            break;
        // In-place edit control notifications (control id 10001)
        case 10001:
            if (HIWORD(wParam) == EN_KILLFOCUS || HIWORD(wParam) == EN_UPDATE || HIWORD(wParam) == EN_CHANGE) {
                // We'll handle commit on EN_KILLFOCUS; ignore EN_UPDATE/EN_CHANGE here
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                        if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditItem >= 0) {
                            wchar_t newVal[512] = {0};
                            GetWindowTextW(hInplaceEdit, newVal, (int)_countof(newVal));
                            std::wstring newStr(newVal);
                            // If nothing changed, just destroy and restore selection
                            if (newStr == this->inplaceEditOriginalText) {
                                DestroyWindow(hInplaceEdit);
                                hInplaceEdit = NULL;
                                int wroteItem = inplaceEditItem;
                                inplaceEditItem = -1;
                                // Restore selection/focus to the appropriate list
                                if (inplaceEditIsMotion) {
                                    ListView_SetItemState(hListMotionParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                                    RECT rcPaint{};
                                    int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 3);
                                    if (ListView_GetSubItemRect(hListMotionParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                                        InvalidateRect(hListMotionParams, &rcPaint, TRUE);
                                    }
                                } else {
                                    ListView_SetItemState(hListSDOParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                                    RECT rcPaint{};
                                    int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
                                    if (ListView_GetSubItemRect(hListSDOParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                                        InvalidateRect(hListSDOParams, &rcPaint, TRUE);
                                    }
                                }
                                inplaceEditIsMotion = false;
                                break;
                            }

                            // For motion edits, validate floats only when editing the Value subitem (3)
                            if (inplaceEditIsMotion) {
                                if (inplaceEditSubitem == 3) {
                                    // trim spaces
                                    auto s = newStr;
                                    while (!s.empty() && iswspace(s.front())) s.erase(s.begin());
                                    while (!s.empty() && iswspace(s.back())) s.pop_back();
                                    if (!s.empty()) {
                                        wchar_t* endp = nullptr; errno = 0;
                                        double v = wcstod(s.c_str(), &endp); (void)v;
                                        if (endp == s.c_str() || (*endp != L'\0' && !iswspace(*endp))) {
                                            PrependStatus(L"數值格式錯誤：請輸入浮點數或整數。\r\n");
                                            if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                                                SetFocus(hInplaceEdit);
                                                SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
                                            }
                                            break; // do not commit
                                        }
                                    }
                                }
                                // Commit to the edited subitem for motion list
                                ListView_SetItemText(hListMotionParams, inplaceEditItem, inplaceEditSubitem, const_cast<LPWSTR>(newStr.c_str()));
                                // Update in-memory cache as the single source of truth
                                if (confirmedSlave > 0) {
                                    auto &mat = savedMotionParamsForSlave[confirmedSlave];
                                    // ensure matrix size
                                    int rows = ListView_GetItemCount(hListMotionParams);
                                    if ((int)mat.size() < rows) mat.resize(rows);
                                    if ((int)mat[inplaceEditItem].size() < 6) mat[inplaceEditItem].resize(6);
                                    mat[inplaceEditItem][inplaceEditSubitem] = newStr;
                                }
                            } else {
                                // Existing SDO handling preserved
                                if (inplaceEditSubitem == 6) {
                                    std::wstring s(newVal);
                                    std::vector<uint8_t> typedBE;
                                    bool anyHex = false;
                                    CoreHelpers::ParseTokensToBytes(s, typedBE, anyHex);
                                    std::wstring hexDisplay;
                                    if (anyHex && !typedBE.empty()) {
                                        std::wstringstream s2; s2 << std::hex << std::uppercase;
                                        for (size_t i = 0; i < typedBE.size(); ++i) {
                                            if (i) s2 << L" ";
                                            s2 << L"0x" << std::setw(2) << std::setfill(L'0') << (int)typedBE[i];
                                        }
                                        hexDisplay = s2.str();
                                    } else {
                                        hexDisplay = s;
                                        while (!hexDisplay.empty() && iswspace(hexDisplay.front())) hexDisplay.erase(hexDisplay.begin());
                                        while (!hexDisplay.empty() && iswspace(hexDisplay.back())) hexDisplay.pop_back();
                                    }
                                    ListView_SetItemText(hListSDOParams, inplaceEditItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
                                    if (confirmedSlave > 0) {
                                        auto &hexvec = savedSDOHexValues[confirmedSlave];
                                        if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                                        hexvec[inplaceEditItem] = hexDisplay;
                                    }
                                } else {
                                    ListView_SetItemText(hListSDOParams, inplaceEditItem, 5, newVal);
                                    if (confirmedSlave > 0) {
                                        auto &vec = savedSDOValues[confirmedSlave];
                                        if ((int)vec.size() < (int)currentObjects.size()) vec.resize(currentObjects.size());
                                        vec[inplaceEditItem] = std::wstring(newVal);
                                    }
                                }
                            }

                            // Destroy edit
                            DestroyWindow(hInplaceEdit);
                            hInplaceEdit = NULL;
                            // Mark no inplace active
                            int wroteItem = inplaceEditItem;
                            // Preserve which list we edited for repaint before resetting flag
                            bool wasMotion = inplaceEditIsMotion;
                            inplaceEditItem = -1;
                            // Reset motion flag
                            inplaceEditIsMotion = false;

                            // Repaint and restore selection/focus
                            if (wasMotion) {
                                ListView_SetItemState(hListMotionParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                                RECT rcPaint{};
                                int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 3);
                                if (ListView_GetSubItemRect(hListMotionParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                                    InvalidateRect(hListMotionParams, &rcPaint, TRUE);
                                }
                            } else {
                                ListView_SetItemState(hListSDOParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                                RECT rcPaint{};
                                int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
                                if (ListView_GetSubItemRect(hListSDOParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                                    InvalidateRect(hListSDOParams, &rcPaint, TRUE);
                                }
                            }
                        }
                }
            }
            break;
        // MMF1 enable/disable handled inside AutoWindow when AutoWindow is present.
        // Legacy main-window buttons removed; ignore these command IDs here.
        case IDC_BTN_PDO_SENT_STATUS:
            if (HIWORD(wParam) == BN_CLICKED) {
                ToggleChildStatusWindow();
            }
            break;
        case IDC_BTN_AUTO_WINDOW:
            if (HIWORD(wParam) == BN_CLICKED) {
                ToggleAutoWindow();
            }
            break;
        
        default:
            // 檢查是否是PDO控件的按鈕事件 - INLINE IMPLEMENTATION to bypass linking issues
            {
                int cmdID = LOWORD(wParam);
                // Cache live edits from PDO edit boxes
                if (cmdID >= 5000 && cmdID < 5100) {
                    if (HIWORD(wParam) == EN_CHANGE) {
                        HWND hEdit = (HWND)lParam;
                        if (hEdit && IsWindow(hEdit)) {
                            wchar_t buf[64] = {0};
                            GetWindowTextW(hEdit, buf, (int)_countof(buf));
                            pdoEditTextCache[cmdID] = buf;
                            // 同步更新全域名稱快取，確保切換從站時輸入不變
                            for (const auto &it : pdoDisplay.pdoItems) {
                                if (!it.isEditable || !it.hEdit) continue;
                                if (GetDlgCtrlID(it.hEdit) == cmdID) {
                                    globalPDOInputByName[it.name] = buf;
                                    break;
                                }
                            }
                        }
                    }
                }
                if (cmdID >= 6000 && cmdID < 6100) {
                    // Only react to BN_CLICKED notifications from buttons.
                    if (HIWORD(wParam) != BN_CLICKED) {
                        // not a click event - ignore but restore timer
                        if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        break;
                    }

                    // Button event: pause timer, read/write values, restart timer
                    auto editableItems = pdoDisplay.GetEditableItems();
                    // 暫停定時器，避免處理期間 UI 更新插入
                    KillTimer(hwnd, TIMER_ID);

                    // Calculate item index
                    int itemIndex = cmdID - 6000;
                    if (itemIndex >= 0 && itemIndex < (int)editableItems.size()) {
                        // INLINE: 直接依照對應可編輯項目寫入
                        // Get the item name
                        std::wstring itemName = editableItems[itemIndex];

                        // Diagnostic: if itemName is empty, dump editableItems and pdoItems for investigation
                        if (itemName.empty()) {
                            // itemName unexpectedly empty; show brief diagnostic to status
                            PrependStatus(L"PDO 內部錯誤：找不到對應可編輯項目\r\n");
                        }
                        
                        // 立即讀取編輯框值（優先 EN_CHANGE 快取，其次直接 GetWindowText）
                        HWND hEdit = NULL;
                        int foundIndex = -1;
                        int cnt = 0;
                        int32_t userInputValue = 0;
                        for (size_t pi = 0; pi < pdoDisplay.pdoItems.size(); ++pi) {
                            const auto &it = pdoDisplay.pdoItems[pi];
                            if (!it.isEditable) continue;
                            if (cnt == itemIndex) {
                                foundIndex = (int)pi;
                                hEdit = it.hEdit;
                                itemName = it.name;
                                // 讀取優先序：EN_CHANGE 快取 -> 直接 GetWindowText
                                // 1) 嘗試 EN_CHANGE 快取（以控制 ID 為鍵）
                                int ctrlId = (hEdit && IsWindow(hEdit)) ? GetDlgCtrlID(hEdit) : -1;
                                auto itCache = pdoEditTextCache.find(ctrlId);
                                if (itCache != pdoEditTextCache.end()) {
                                    std::wstring cached = itCache->second;
                                    userInputValue = _wtoi(cached.c_str());
                                } else if (hEdit && IsWindow(hEdit)) {
                                    // 2) 直接讀控制項
                                    wchar_t buffer[64] = {0};
                                    GetWindowTextW(hEdit, buffer, (int)_countof(buffer));
                                    userInputValue = _wtoi(buffer);
                                } else if (hEdit && IsWindow(hEdit)) {
                                    // fallthrough safety (shouldn't reach here)
                                }
                                break;
                            }
                            ++cnt;
                        }
                            
                        // Get current selected slave from combo box (ID 2001)
                        int slaveIdx = -1;
                        HWND hCombo = GetDlgItem(hContentWnd, 2001);
                        if (hCombo) {
                            LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
                            if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
                        }
                        
                        if (slaveIdx <= 0) {
                            
                            if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        } else if (!AppController::GetInstance().GetMaster()) {
                            
                            if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        } else {
                            auto* timerMaster = AppController::GetInstance().GetMaster();
                            if (hEdit && foundIndex >= 0) {
                                // 寫入PDO值到IOMap
                                {
                                    std::wstringstream wlog; 
                                    wlog << L"[PDO] Attempt write slave=" << slaveIdx << L" item='" << itemName << L"' value=" << userInputValue << L"\r\n";
                                    PrependStatus(wlog.str().c_str());
                                }
                                bool writeOk = timerMaster && pdoDisplay.WriteToIOMap(timerMaster, slaveIdx, itemName, userInputValue);

                                if (writeOk) {
                                    if (timerMaster && timerMaster->IsRunning()) {
                                        timerMaster->SendProcessDataOnce();
                                    }
                                    std::wstringstream ok;
                                    ok << L"[PDO] ✓ 成功寫入 " << itemName << L" = " << userInputValue << L"\r\n";
                                    PrependStatus(ok.str().c_str());
                                } else {
                                    std::wstringstream ng;
                                    ng << L"[PDO] ✗ 寫入失敗 " << itemName << L"\r\n";
                                    PrependStatus(ng.str().c_str());
                                }
                                
                                // 重新啟動定時器
                                if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                                return 0;
                            } else {
                                
                                if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                            }
                        }
                    } else {
                        
                                if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                    }
                    
                    // 確保重新啟動定時器
                    if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                }
                // All PDO commands are handled inline above
            }
            break;
        }
        break;

    case WM_SYSCOMMAND:
        {
            UINT cmd = (UINT)(wParam & 0xFFF0);
            if (cmd == SC_MINIMIZE && useTray) {
                // Intercept minimize to hide to tray instead of minimizing to taskbar
                MinimizeToTray();
                return 0; // handled, don't call DefWindowProc
            }
            if (cmd == SC_MAXIMIZE || cmd == SC_RESTORE) {
                minimizedToTray = false;
                isMinimizedState = false;
                LRESULT res = DefWindowProc(hwnd, uMsg, wParam, lParam);
                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                cachedSlaveListHeight = 0;
                cachedSDOListHeight = 0;
                ReflowMainControls();
                ForceFullRedrawNow();
                if (hListSDOParams && IsWindow(hListSDOParams)) UpdateWindow(hListSDOParams);
                if (hEditMailbox && IsWindow(hEditMailbox)) UpdateWindow(hEditMailbox);
                if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) UpdateWindow(hBtnSendMailbox);
                return res;
            }
            // For SC_MOVE, SC_SIZE, and all other system commands: pass to DefWindowProc
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
        }

    case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED) {
                isMinimizedState = true;
                break; // no reflow while minimized
            }
            
            if (wParam == SIZE_RESTORED) {
                if (minimizedToTray) {
                    minimizedToTray = false;
                    RemoveTrayIcon();
                }
                isMinimizedState = false;
                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                cachedSlaveListHeight = 0;
                cachedSDOListHeight = 0;
            }
            
            // For any resize (SIZE_RESTORED, SIZE_MAXIMIZED, or interactive resize):
            // Recalculate layout to fill new client area
            if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED || !this->isInteractiveResize) {
                ReflowMainControls();
                ForceFullRedrawNow();
                if (hListSDOParams && IsWindow(hListSDOParams)) UpdateWindow(hListSDOParams);
                if (hEditMailbox && IsWindow(hEditMailbox)) UpdateWindow(hEditMailbox);
                if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) UpdateWindow(hBtnSendMailbox);
            } else {
                // During interactive resize, use lightweight scroll update to avoid lag
                UpdateContentPaneScrollOnly();
            }
        }
        break;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            OnTimer();
        } else if (wParam == TIMER_REDRAW_THROTTLE) {
            // Delayed full redraw scheduled by ForceFullRedraw when calls were
            // throttled. Clear pending flag and perform immediate redraw.
            KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
            redrawThrottlePending = false;
            ForceFullRedrawNow();
        } else if (wParam == TIMER_JOG_POLL) {
            // Polling while a jog is active. UI owns jogBusy state.
            if (!jogBusy.load()) {
                KillTimer(hwnd, TIMER_JOG_POLL);
            } else if (!IsMasterInitialized() || confirmedSlave <= 0) {
                // Safety: stop jog if master not available or no confirmed slave
                if (motionController) motionController->JogStop(confirmedSlave);
                SetBusyFlagAndNotify(jogBusy, false);
                KillTimer(hwnd, TIMER_JOG_POLL);
                SetControlEnabled(hBtnJogCW, true);
                SetControlEnabled(hBtnJogCCW, true);
            } else {
                auto* pollMaster = AppController::GetInstance().GetMaster();
                // Obtain per-slave snapshot and inspect 0x6041 statusword bit3
                std::vector<uint8_t> snap;
                if (pollMaster && pollMaster->GetPDODataSnapshot(confirmedSlave, snap)) {
                    bool stopCond = false;
                    const PDOItem* statusItem = nullptr;
                    for (const auto &it : pdoDisplay.pdoItems) {
                        if (it.index == 0x6041 && !it.isOutput) { statusItem = &it; break; }
                    }
                    int outputSize = 0;
                    const ecx_contextt* ctx = pollMaster->GetContext();
                    if (ctx && confirmedSlave > 0 && confirmedSlave <= ctx->slavecount) outputSize = ctx->slavelist[confirmedSlave].Obytes;

                    if (statusItem) {
                        int bytes = (statusItem->bitLength > 0) ? ((statusItem->bitLength + 7) / 8) : 2;
                        if (bytes > 4) bytes = 4;
                        int idxInSnap = statusItem->isOutput ? statusItem->byteOffset : (outputSize + statusItem->byteOffset);
                        if (idxInSnap >= 0 && idxInSnap + bytes <= (int)snap.size()) {
                            uint32_t val = 0;
                            for (int b = 0; b < bytes; ++b) val |= (uint32_t)snap[idxInSnap + b] << (8 * b);
                            if (val & (1u << 3)) stopCond = true;
                        }
                    } else {
                        // Fallback: read first two input bytes after outputs
                        if (outputSize + 2 <= (int)snap.size()) {
                            uint16_t sw = (uint16_t)snap[outputSize] | ((uint16_t)snap[outputSize + 1] << 8);
                            if (sw & (1u << 3)) stopCond = true;
                        }
                    }

                    if (stopCond) {
                        if (motionController) motionController->JogStop(confirmedSlave);
                        SetBusyFlagAndNotify(jogBusy, false);
                        KillTimer(hwnd, TIMER_JOG_POLL);
                        SetControlEnabled(hBtnJogCW, true);
                        SetControlEnabled(hBtnJogCCW, true);
                        PrependStatus(L"[Jog] 偵測到 0x6041.bit3，已停止\r\n");
                    }
                }
            }
        } else if (wParam == TIMER_EDIT_FOCUS) {
            KillTimer(hwnd, TIMER_EDIT_FOCUS);
            if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                SetWindowPos(hInplaceEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetFocus(hInplaceEdit);
                SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
            }
        }
        break;

    case WM_VSCROLL:
        {
            int code = LOWORD(wParam);
            int pos = HIWORD(wParam);
            RECT rc; GetClientRect(hwnd, &rc);
            int clientH = rc.bottom - rc.top;
            SCROLLINFO si; ZeroMemory(&si, sizeof(si)); si.cbSize = sizeof(si); si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int maxPos = si.nMax - (int)si.nPage + 1;
            if (maxPos < 0) maxPos = 0;
            switch (code) {
            case SB_TOP: scrollPos = si.nMin; break;
            case SB_BOTTOM: scrollPos = si.nMax; break;
            case SB_LINEUP: scrollPos -= 20; break;
            case SB_LINEDOWN: scrollPos += 20; break;
            case SB_PAGEUP: scrollPos -= clientH; break;
            case SB_PAGEDOWN: scrollPos += clientH; break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: scrollPos = pos; break;
            default: break;
            }
            if (scrollPos < 0) scrollPos = 0;
            if (scrollPos > maxPos) scrollPos = maxPos;
            // update scrollinfo
            si.fMask = SIF_POS; si.nPos = scrollPos; SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            UpdateContentPaneScrollOnly();
        }
        break;

    case WM_MOUSEWHEEL:
        {
            // Delta positive = scroll up; negative = scroll down
            // If the cursor is over a child control inside the content pane,
            // skip main-window scrolling to avoid double-scroll (child control
            // will handle its own internal scrolling). This prevents listviews
            // such as PDO/Slave tables from changing their internal top index
            // while the main content pane is also moved.
            POINT ptScreen = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            HWND hw = WindowFromPoint(ptScreen);
            if (hw && IsWindow(hw)) {
                // If the hit window is a descendant of hContentWnd (but not the
                // content wnd itself), let the child handle the wheel and skip
                // moving the content pane here.
                if (hContentWnd && hw != hContentWnd && IsChild(hContentWnd, hw)) {
                    return 0; // let child window's own handler process this wheel
                }
            }

            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            // Each notch is WHEEL_DELTA (120); scroll ~3 lines per notch
            int lines = delta / WHEEL_DELTA;
            scrollPos -= lines * 40; // 40 pixels per line approx
            // Clamp against scroll range reported by scrollbar
            SCROLLINFO si; ZeroMemory(&si, sizeof(si)); si.cbSize = sizeof(si); si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int maxPos = si.nMax - (int)si.nPage + 1;
            if (maxPos < 0) maxPos = 0;
            if (scrollPos < si.nMin) scrollPos = si.nMin;
            if (scrollPos > maxPos) scrollPos = maxPos;
            si.fMask = SIF_POS; si.nPos = scrollPos; SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
            UpdateContentPaneScrollOnly();
        }
        break;

    //（保留精簡版滾動處理：WM_VSCROLL/WM_MOUSEWHEEL 僅移動內容面板，不觸發重排）

    case WM_DESTROY:
    // Ensure safe shutdown of EtherCAT before exiting
    KillTimer(hwnd, TIMER_ID);
    KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
    // Only disconnect if not already disconnecting/disconnected
    // Use SYNCHRONOUS shutdown to avoid race conditions during program exit
    if (MasterHasActiveSlaveThreads()) {
        PrependStatus(L"視窗關閉中,正在安全退出所有裝置...\r\n");
        // Don't use OnDisconnect() here as it's async - use direct synchronous shutdown
        try {
            MasterShutdownWithCallback([this](const std::string &msg){ PostUTF8LogToUI(msg); });
            PrependStatus(L"裝置已安全退出\r\n");
        } catch (const std::exception& e) {
            std::string err = std::string("[WM_DESTROY] Shutdown exception: ") + e.what() + "\n";
            PostUTF8LogToUI(err);
        } catch (...) {
            PostUTF8LogToUI(std::string("[WM_DESTROY] Unknown shutdown exception\n"));
        }
    }
    PrependStatus(L"程式已退出\r\n");
    // Clean up any launched browser process handle to avoid leaking handles
    CleanupTrackedBrowserHandle(true);
    // Ensure modeless AutoWindow is destroyed during shutdown so it does not
    // remain after the main window exits. This calls AutoWindow cleanup
    // (timers, background thread stop) via DestroyAutoWindow.
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        DestroyAutoWindow(hAutoWindow);
        hAutoWindow = NULL;
    }
    // Ensure ChildStatusWindow (modeless status window) is safely destroyed
    // during shutdown to avoid orphaned windows. This mirrors the AutoWindow cleanup.
    if (hChildStatusWnd && IsWindow(hChildStatusWnd)) {
        DestroyWindow(hChildStatusWnd);
        hChildStatusWnd = NULL;
        hChildStatusEdit = NULL;
    }
    closePending.store(false);
    // Ensure tray icon is removed on shutdown to avoid orphaned icons in the notification area
    RemoveTrayIcon();
    PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Extracted from WM_USER_AUTO_IMMEDIATE_STOP_REQUEST case block.
// Runs pre-stop preamble on the UI thread, then launches a background thread
// to execute smooth/immediate stop, PDO filter teardown, and requester
// notification.
// ---------------------------------------------------------------------------
void MainWindow::HandleAutoImmediateStopRequest(HWND hRequester, bool isPDOErrorTriggered, int slaveNumber, bool requireManualReset, bool skipManualReset) {
    auto* requestMaster = AppController::GetInstance().GetMaster();
    const bool shouldRequireManualReset = requireManualReset || (!isPDOErrorTriggered && !skipManualReset);

    if (!requestMaster) {
        std::string logMsg = isPDOErrorTriggered ?
            "[AutoImmediateStop] EtherCAT master not available (PDO feedback error)\n" :
            "[AutoImmediateStop] EtherCAT master not available\n";
        PostUTF8LogToUI(logMsg);
        NotifyEthercatError(true);
        if (hRequester && IsWindow(hRequester)) PostMessageW(hRequester, WM_USER_AUTO_IMMEDIATE_STOP_DONE, (WPARAM)0, 0);
        return;
    }

    if (this->immediateStopBusy.load()) {
        PostUTF8LogToUI(std::string("[AutoImmediateStop] Duplicate stop request ignored because a stop sequence is already running\n"));
        NotifyAutoStopCompletion(hRequester, WM_USER_AUTO_IMMEDIATE_STOP_DONE, true, isPDOErrorTriggered ? 2 : 1);
        return;
    }

    if (!isPDOErrorTriggered && this->manualResetRequired.load()) {
        bool pdoFilteringEnabled = true;
        try { pdoFilteringEnabled = requestMaster->IsPDOFilteringEnabled(); } catch (...) {}
        if (!pdoFilteringEnabled) {
            PostUTF8LogToUI(std::string("[AutoImmediateStop] Duplicate manual-reset stop request ignored because PDO filtering is already offline\n"));
            NotifyAutoStopCompletion(hRequester, WM_USER_AUTO_IMMEDIATE_STOP_DONE, false, 1);
            PostAutoStatusUpdate();
            RequestMotionUiRefresh();
            return;
        }
    }

    // Log the source of the immediate stop request
    if (isPDOErrorTriggered) {
        std::string msg = requireManualReset ?
            std::string("[AutoImmediateStop] 由 PDO 偏差值過大逾時觸發 (Slave ") + std::to_string(slaveNumber) + ") - 正在啟動緊急停止，需 Fault Reset 後才能繼續。\n" :
            std::string("[AutoImmediateStop] 由 PDO 回授錯誤觸發 (Slave ") + std::to_string(slaveNumber) + ") - 正在啟動緊急停止，並嘗試自動復原。\n";
        PostUTF8LogToUI(msg);

        try {
            if (auto* setMaster = AppController::GetInstance().GetMaster()) {
                setMaster->SetGlobalError();
            }
        } catch (...) {}

        if (!requireManualReset) {
        try {
            if (auto* recoveryMaster = AppController::GetInstance().GetMaster()) {
                recoveryMaster->ClearGlobalError();
                int sc = recoveryMaster->GetSlaveCount();
                if (slaveNumber > 0 && slaveNumber <= sc) {
                    recoveryMaster->ResetPDOFilter(slaveNumber);
                    PostUTF8LogToUI(std::string("[AutoImmediateStop] 已嘗試重置 PDOFilter (Slave ") + std::to_string(slaveNumber) + ")\n");
                } else {
                    for (int si = 1; si <= sc; ++si) {
                        try { recoveryMaster->ResetPDOFilter(si); } catch (...) {}
                    }
                    PostUTF8LogToUI(std::string("[AutoImmediateStop] 已嘗試重置所有 PDOFilter (all slaves)\n"));
                }
            } else {
                PostUTF8LogToUI(std::string("[AutoImmediateStop] 無法存取 EtherCAT master，無法自動重置 PDOFilter\n"));
            }
        } catch (const std::exception &ex) {
            PostUTF8LogToUI(std::string("[AutoImmediateStop] 自動復原時發生例外: ") + ex.what() + "\n");
        } catch (...) {
            PostUTF8LogToUI(std::string("[AutoImmediateStop] 自動復原時發生未知錯誤\n"));
        }
        PostUTF8LogToUI(std::string("[AutoImmediateStop] 若自動復原無效，請嘗試使用 AutoWindow 的 Fault Reset 或重啟系統。\n"));
        } else {
            PostUTF8LogToUI(std::string("[AutoImmediateStop] 已保留 PDOFilter 錯誤鎖定，請執行 Fault Reset 後再重新啟動。\n"));
        }
    }

    if (!isPDOErrorTriggered && shouldRequireManualReset) {
        try {
            if (auto* setMaster = AppController::GetInstance().GetMaster()) {
                setMaster->SetGlobalError();
            }
        } catch (...) {}
    }

    // If Auto Connect soft-start thread is running, request it to abort
    AbortAutoConnectSoftStartForStop("[AutoImmediateStop] ");
    if (shouldRequireManualReset) {
        SetAutoStopLampLatched(true, false);
        SetManualResetRequired(true);
    }
    // Start continuous alert sound for the immediate stop
    StartErrorAlertSoundForStop("[AutoImmediateStop] ");
    SetBusyFlagAndNotify(this->immediateStopBusy, true);

    std::thread([this, hRequester, isPDOErrorTriggered, slaveNumber, requireManualReset]() {
        ConfigureWorkerThreadForRT();
        bool success = false;
        auto attemptPdoErrorRecovery = [this, isPDOErrorTriggered, requireManualReset, slaveNumber](const std::string &prefixMessage,
                                            const std::string &successMessage) {
            if (!isPDOErrorTriggered || requireManualReset) return;

            PostUTF8LogToUI(prefixMessage);
            try {
                if (ClearMasterErrorAndResetPdoFilters(slaveNumber)) {
                    PostUTF8LogToUI(successMessage);
                } else {
                    PostUTF8LogToUI(std::string("[AutoImmediateStop] 無法存取 EtherCAT master，請使用 AutoWindow Fault Reset 或重啟系統。\n"));
                }
            } catch (...) {
                PostUTF8LogToUI(std::string("[AutoImmediateStop] 自動復原嘗試失敗，請使用 AutoWindow Fault Reset 或重啟系統。\n"));
            }
        };

        if (!this->motionController) {
            std::string logMsg = isPDOErrorTriggered ?
                std::string("[AutoImmediateStop] MotionController not initialized (PDO feedback error, Slave ") + std::to_string(slaveNumber) + ")\n" :
                "[AutoImmediateStop] MotionController not initialized\n";
            PostUTF8LogToUI(logMsg);
            attemptPdoErrorRecovery(
                std::string("[AutoImmediateStop] PDO 回授不一致 (Slave ") + std::to_string(slaveNumber) + ") - 嘗試在 master/filters 層做自動復原。\n",
                std::string("[AutoImmediateStop] 已嘗試在 master 層清除錯誤並重置 PDOFilter。\n"));

            FinalizeAutoImmediateStop(hRequester, false, isPDOErrorTriggered);
            return;
        }
        try {
            RequestAbortAutoDownToOriginForStop("[AutoImmediateStop] ");
            success = ExecuteStopMotionSequence("[AutoImmediateStop] ", true);

        } catch (const std::exception &ex) {
            std::string s = std::string("[AutoImmediateStop] exception: ") + ex.what() + "\n";
            PostUTF8LogToUI(s);
            attemptPdoErrorRecovery(
                std::string("[AutoImmediateStop] PDO 回授錯誤 (Slave ") + std::to_string(slaveNumber) + ") - 嘗試進行自動復原 (ClearGlobalError/ResetPDOFilter)。\n",
                std::string("[AutoImmediateStop] 自動復原已嘗試 - 請觀察系統狀態或使用 AutoWindow Fault Reset。\n"));
            success = false;
        } catch (...) {
            PostUTF8LogToUI(std::string("[AutoImmediateStop] unknown exception\n"));
            attemptPdoErrorRecovery(
                std::string("[AutoImmediateStop] PDO 回授錯誤 (Slave ") + std::to_string(slaveNumber) + ") - 嘗試進行自動復原 (ClearGlobalError/ResetPDOFilter)。\n",
                std::string("[AutoImmediateStop] 自動復原已嘗試 - 請觀察系統狀態或使用 AutoWindow Fault Reset。\n"));
            success = false;
        }
        FinalizeAutoImmediateStop(hRequester, success, isPDOErrorTriggered);
    }).detach();
}

// ---------------------------------------------------------------------------
// Extracted from WM_USER_AUTO_EMERGENCY_STOP_REQUEST case block.
// Similar to HandleAutoImmediateStopRequest but additionally disables Fault
// Reset during the stop and checks E-Stop (0x60FD bit5) before stopping alert.
// ---------------------------------------------------------------------------
void MainWindow::HandleAutoEmergencyStopRequest(HWND hRequester, bool trackingErrorTriggered) {
    auto* requestMaster = AppController::GetInstance().GetMaster();
    if (!requestMaster) {
        PostUTF8LogToUI(std::string("[AutoEmergencyStop] EtherCAT master not available\n"));
        NotifyEthercatError(true);
        if (hRequester && IsWindow(hRequester)) PostMessageW(hRequester, WM_USER_AUTO_EMERGENCY_STOP_DONE, (WPARAM)0, 0);
        return;
    }
    try {
        requestMaster->SetGlobalError();
    } catch (...) {}
    AbortAutoConnectSoftStartForStop("[AutoEmergencyStop] ");
    SetAutoStopLampLatched(false, true);
    SetManualResetRequired(true);
    StartErrorAlertSoundForStop("[AutoEmergencyStop] ");
    SetBusyFlagAndNotify(this->immediateStopBusy, true);
    // 禁用 Fault Reset 按鈕，防止在緊急停止期間執行異常重置
    SetFaultResetUiEnabledForStop(false);

    std::thread([this, hRequester, trackingErrorTriggered]() {
        ConfigureWorkerThreadForRT();
        bool success = false;
        if (!IsWindow(this->hwnd)) {
            SetBusyFlagAndNotify(this->immediateStopBusy, false);
            return;
        }
        if (!this->motionController) {
            if (IsWindow(this->hwnd)) PostUTF8LogToUI(std::string("[AutoEmergencyStop] MotionController not initialized\n"));
            FinalizeAutoEmergencyStop(hRequester, false);
            return;
        }
        try {
            RequestAbortAutoDownToOriginForStop("[AutoEmergencyStop] ");
            success = ExecuteStopMotionSequence("[AutoEmergencyStop] ", false);
            if (trackingErrorTriggered) {
                try {
                    if (ClearMasterErrorAndResetPdoFilters()) {
                        if (IsWindow(this->hwnd)) {
                            PostUTF8LogToUI(std::string("[AutoEmergencyStop] 已自動清除 PDO 偏差保護狀態，可直接重新啟動。\n"));
                        }
                    } else if (IsWindow(this->hwnd)) {
                        PostUTF8LogToUI(std::string("[AutoEmergencyStop] 無法自動重置 PDOFilter，若下次啟動異常再執行 Fault Reset。\n"));
                    }
                } catch (...) {
                    if (IsWindow(this->hwnd)) {
                        PostUTF8LogToUI(std::string("[AutoEmergencyStop] 自動清除 PDO 偏差保護狀態失敗，若下次啟動異常再執行 Fault Reset。\n"));
                    }
                }
            }
        } catch (const std::exception &ex) {
            std::string s = std::string("[AutoEmergencyStop] exception: ") + ex.what() + "\n";
            if (IsWindow(this->hwnd)) PostUTF8LogToUI(s);
            success = false;
        } catch (...) {
            if (IsWindow(this->hwnd)) PostUTF8LogToUI(std::string("[AutoEmergencyStop] unknown exception\n"));
            success = false;
        }
        FinalizeAutoEmergencyStop(hRequester, success);
    }).detach();
}


void MainWindow::HandleSDOListScroll()
{
    if (!hListSDOParams || !IsWindow(hListSDOParams)) return;
    if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditItem >= 0) {
        int item = inplaceEditItem;
        int top = ListView_GetTopIndex(hListSDOParams);
        int perPage = ListView_GetCountPerPage(hListSDOParams);
        if (perPage <= 0) perPage = 1;
        if (item < top || item >= top + perPage) {
            // 滾出可視範圍 -> 提交並關閉
            PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(10001, EN_KILLFOCUS), (LPARAM)hInplaceEdit);
            return;
        }
    RECT rcSub{};
    int subToUse2 = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
    if (ListView_GetSubItemRect(hListSDOParams, item, subToUse2, LVIR_BOUNDS, &rcSub)) {
            RECT rcEdit = rcSub;
            rcEdit.left   = (rcEdit.left > 1) ? rcEdit.left - 1 : 0;
            rcEdit.top    = (rcEdit.top > 1) ? rcEdit.top - 1 : 0;
            rcEdit.right += 1;
            rcEdit.bottom+= 2;
            POINT pt = { rcEdit.left, rcEdit.top };
            ClientToScreen(hListSDOParams, &pt);
            ScreenToClient(hContentWnd, &pt);
            SetWindowPos(hInplaceEdit, HWND_TOP, pt.x, pt.y,
                         rcEdit.right - rcEdit.left, rcEdit.bottom - rcEdit.top,
                         SWP_SHOWWINDOW);
        }
    }
}

// ToggleChildStatusWindow implementation moved to StatusPane.cpp

// OnClearStatus and MarkStatusScrolled moved to StatusPane.cpp

// OnTimer is implemented in MainWindow.Logic.cpp

// StartWatchdogRecovery moved to MainWindow.Logic.cpp

void MainWindow::Show(int nCmdShow) {
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
}

int MainWindow::MessageLoop() {
    MSG msg{};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}




