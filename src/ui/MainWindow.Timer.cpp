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
#include "../include/EtherCATMaster.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
#include "../include/MotionLogic.h"
#include "../include/AutoWindow.h"

namespace {

void FormatErrorCode603FText(bool valid, uint16_t value, wchar_t* buffer, size_t bufferCount)
{
    if (!buffer || bufferCount == 0) {
        return;
    }

    if (!valid) {
        wcscpy_s(buffer, bufferCount, L"N/A");
        return;
    }

    swprintf_s(buffer, bufferCount, L"0x%04X", static_cast<unsigned>(value));
}

std::string BuildWatchdogConditionReason(int hits, int threshold)
{
    std::ostringstream oss;
    auto* master = AppController::GetInstance().GetMaster();
    int32_t rawWkc = 0;
    if (master) {
        rawWkc = master->GetWorkingCounterRaw();
    }
    oss << "watchdog threshold reached"
        << " (hits=" << hits << "/" << threshold;

    // Distinguish EC_NOFRAME (NIC didn't receive anything) from real WKC=0 (slave not responding)
    if (rawWkc < 0) {
        oss << ", WKC=EC_NOFRAME(NIC no-frame)";
    } else {
        oss << ", WKC=" << (uint32_t)rawWkc;
    }

    if (master) {
        const auto slaves = master->GetSlaveInfo();
        for (const auto& slave : slaves) {
            if (slave.state == 0x14) {
                oss << ", Slave " << slave.position
                    << " ALstate=0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << slave.state
                    << std::dec << std::setfill(' ');
                break;
            }
        }
    }

    oss << ")";
    return oss.str();
}

bool HasPersistentEthercatErrorCause()
{
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->HasGlobalError();
    }
    return false;
}

} // namespace

void MainWindow::RequestAutomaticDisconnect(const char* logMessage, const wchar_t* statusText) {
    if (statusText && IsWindow(hwnd)) {
        PrependStatus(statusText);
    }
    if (logMessage && *logMessage) {
        PostUTF8LogToUI(std::string(logMessage));
    }
    if (IsAutoProcedureRunning()) {
        PostUTF8LogToUI(std::string("[AutoStop] Automatic disconnect requested - stopping auto procedure first\n"));
        StopAutoProcedure();
    }
    if (IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_USER_REQUEST_DISCONNECT, 0, 0);
        return;
    }
    try {
        PostCommandToUIOrCache(std::string("HTTP_CMD:DISCONNECT"));
    } catch (...) {
    }
}

bool MainWindow::CheckAndHandleSustainedLinkLoss(ULONGLONG nowTick, const wchar_t* statusText) {
    // Snapshot once: the selection cache is written by the UI thread and HTTP
    // workers, so every read must be a single locked copy.
    const std::string monitoredIfName = this->currentIfName.Copy();
    if (EtherCATMaster::IsSimulationAdapterSelection(monitoredIfName)) {
        linkDownSinceTick = 0;
        linkLossDisconnectRequested = false;
        return false;
    }

    const auto monitoringPhase = CoreHelpers::ResolveLinkLossMonitoringPhase(
        disconnecting.load(),
        !monitoredIfName.empty(),
        IsMasterInitialized(),
        workerRunning.load());
    const bool monitoringEnabled = monitoringPhase != CoreHelpers::LinkLossMonitoringPhase::Disabled;
    const char* phaseLabel = CoreHelpers::DescribeLinkLossMonitoringPhase(monitoringPhase);

    bool linkUp = false;
    if (monitoringEnabled) {
        try {
            linkUp = CoreHelpers::CheckLinkStatus(monitoredIfName.c_str());
        } catch (...) {
            linkUp = false;
        }
    }

    CoreHelpers::LinkLossState linkLossState{
        static_cast<uint64_t>(linkDownSinceTick),
        linkLossDisconnectRequested,
    };
    const auto decision = CoreHelpers::EvaluateSustainedLinkLoss(
        monitoringEnabled,
        linkUp,
        static_cast<uint64_t>(nowTick),
        LINK_LOSS_DISCONNECT_DELAY_MS,
        linkLossState);
    linkDownSinceTick = static_cast<ULONGLONG>(linkLossState.down_since_tick);
    linkLossDisconnectRequested = linkLossState.disconnect_requested;

    switch (decision.decision) {
    case CoreHelpers::LinkLossDecision::StartedTiming:
        PostUTF8LogToUI(std::string("[LinkMonitor]") + "[" + phaseLabel + "] EtherCAT link down detected on adapter '" + monitoredIfName + "' - waiting 1000 ms before auto-disconnect\n");
        return false;
    case CoreHelpers::LinkLossDecision::Recovered:
        PostUTF8LogToUI(std::string("[LinkMonitor]") + "[" + phaseLabel + "] EtherCAT link recovered after " + std::to_string(decision.down_duration_ms) + " ms - clearing auto-disconnect timer\n");
        return false;
    case CoreHelpers::LinkLossDecision::TriggerDisconnect:
        NotifyEthercatError(true,
            std::string("EtherCAT link down ") + std::to_string(decision.down_duration_ms) +
            " ms (threshold=" + std::to_string(LINK_LOSS_DISCONNECT_DELAY_MS) + " ms)");
        {
            std::string logMessage = std::string("Auto-disconnect[") + phaseLabel + "]: EtherCAT link down persisted for " +
                std::to_string(decision.down_duration_ms) +
                " ms - issuing disconnect\n";
            RequestAutomaticDisconnect(logMessage.c_str(), statusText);
        }
        return true;
    case CoreHelpers::LinkLossDecision::AlreadyTriggered:
        return true;
    case CoreHelpers::LinkLossDecision::None:
    case CoreHelpers::LinkLossDecision::ResetWhileDisabled:
    default:
        return false;
    }
}

void MainWindow::OnTimer() {
    // When minimized to the tray we must skip timer-driven work because
    // the window is hidden and timers were intentionally killed earlier.
    // While minimized or hidden, background/diagnostic work may continue,
    // but the main window itself should not spend time repainting.
    if (minimizedToTray && !this->forceRunWhenMinimized) return;

    const bool suppressMainWindowLiveUi = !ShouldRefreshMainWindowUi();

    // Refresh displayed HTTP URL with throttling to reduce CPU churn
    static ULONGLONG lastHttpUpdateTick = 0;
    const ULONGLONG nowTick = GetTickCount64();
    const ULONGLONG httpUpdateIntervalMs = 1000;
    if (!suppressMainWindowLiveUi && nowTick - lastHttpUpdateTick >= httpUpdateIntervalMs) {
        UpdateHttpServerUrl();
        lastHttpUpdateTick = nowTick;
    }
    // MMF table refresh moved to AutoWindow: main window must not directly update AutoWindow controls.
    // AutoWindow hosts its own list and periodically updates its displayed values from the master.

    // MMF mapping/diagnostic logging moved to AutoWindow (AutoWindow monitors mapping
    // and uses owner's Master wrapper and PrependStatus APIs to log diagnostics).

    // AutoProcedure actual(ms) UI removed. EtherCATMaster exposes GetSdoReaderLastObservedCycleMs()
    // for diagnostics if needed; no UI field is updated here.

    if (CheckAndHandleSustainedLinkLoss(nowTick, L"錯誤：EtherCAT 網路斷線超過 1 秒，正在執行斷開程序...\r\n")) {
        return;
    }

    if (!MasterIsRunning()) {
        return;
    }

    if (!suppressMainWindowLiveUi && hListSlaves && IsWindow(hListSlaves)) {
        std::array<uint16_t, 6> errorCodes603F = {0, 0, 0, 0, 0, 0};
        std::array<bool, 6> errorCodeValid603F = {false, false, false, false, false, false};
        GetLastReportedErrorCodes(errorCodes603F, errorCodeValid603F);
        const int rowCount = ListView_GetItemCount(hListSlaves);
        const int updateCount = std::min(rowCount, 6);
        for (int i = 0; i < updateCount; ++i) {
            wchar_t text[16];
            FormatErrorCode603FText(errorCodeValid603F[i], errorCodes603F[i], text, _countof(text));
            ListView_SetItemText(hListSlaves, i, 2, text);
        }
    }

    const bool downToOriginHotPath = autoDownToOriginRunning.load();
    const bool highLoadWorkflow = IsAutoProcedureRunning()
        || autoDownToOriginRunning.load()
        || autoConnectBusy.load();

    auto* ethercatService = AppController::GetInstance().GetEtherCATService();
    auto* masterRaw = AppController::GetInstance().GetMaster();

    // Only communication failures should trigger automatic EtherCAT disconnect.
    // Other global-error uses (for example immediate/emergency stop surfacing oError)
    // must keep the bus connected so shared fault-reset/start-auto recovery can run.
    try {
        if (anySlaveHasError.load()) {
            int gcount = ++globalErrorCounter;
            int gthr = globalErrorThreshold;
            int masterThr = MasterGetNetworkFailureThreshold();
            bool autoDisc = MasterIsAutoDisconnectOnNetworkFailureEnabled();
            int effectiveThr = (masterThr > 0) ? std::max(masterThr, gthr) : gthr;
            NotifyEthercatError(true);
            if (autoDisc && gcount >= effectiveThr) {
                std::string logMessage = std::string("Auto-disconnect: persistent communication error (") +
                    std::to_string(gcount) + " hits) - issuing disconnect\n";
                RequestAutomaticDisconnect(logMessage.c_str());
                globalErrorCounter.store(0);
                return;
            }
        } else {
            globalErrorCounter.store(0);
        }
    } catch(...) {}

    if (!suppressMainWindowLiveUi && hEditCurrentCycleTime == NULL) {
        return;
    }

    if (!suppressMainWindowLiveUi) {
        // Cache previous values to avoid unnecessary UI updates
        static uint32_t lastCommsUs = 0;
        static uint32_t lastMaxCommsUs = 0;
        static int lastWKC = -1;
        static bool lastCycleDetectReady = true;

        std::wstringstream ss; // for WKC display
        {
        uint32_t comms_us = 0;
        uint32_t maxComms_us = 0;
            bool cycleDetectReady = this->cyclicPdoCycleDetectReady.load();
            if (!cycleDetectReady) {
                const uint32_t wkcNow = MasterGetWorkingCounter();
                const uint64_t pdoSentNow = MasterGetPDOSentCount();
                if (wkcNow > 0 && pdoSentNow > 0) {
                    this->cyclicPdoCycleDetectReady.store(true);
                    cycleDetectReady = true;
                }
            }
            if (!cycleDetectReady) {
                if (lastCycleDetectReady && hEditCurrentCycleTime && IsWindow(hEditCurrentCycleTime)) {
                    SendMessageW(hEditCurrentCycleTime, WM_SETREDRAW, FALSE, 0);
                    SendMessageW(hEditCurrentCycleTime, WM_SETTEXT, 0, (LPARAM)L"等待 Cyclic Mode 與 PDO 就緒中...");
                    SendMessageW(hEditCurrentCycleTime, WM_SETREDRAW, TRUE, 0);
                    InvalidateRect(hEditCurrentCycleTime, NULL, TRUE);
                }
                lastCycleDetectReady = false;
            } else {
                lastCycleDetectReady = true;
                comms_us = MasterGetCommsTime();
                maxComms_us = MasterGetMaxObservedCommsTime();
                // Retrieve the TRUE cycle-to-cycle period (measured between consecutive PDO sends)
                uint32_t cyclePeriod_us = MasterGetObservedCycleMicros();
                uint32_t maxCyclePeriod_us = MasterGetMaxObservedCycleMicros();
            const bool warmupCollecting = (this->maxObservedCollectStartTick != 0 && nowTick < this->maxObservedCollectStartTick);
            if (warmupCollecting) {
                maxComms_us = 0;
                maxCyclePeriod_us = 0;
            } else if (this->maxObservedCollectStartTick != 0) {
                // Warm-up ended: reset once to start max observation from steady-state.
                MasterResetMaxObservedCommsTime();
                MasterResetMaxObservedCycleMicros();
                this->maxObservedCollectStartTick = 0;
                maxComms_us = 0;
                maxCyclePeriod_us = 0;
            }
            // Only update if values changed
            static uint32_t lastCyclePeriodUs = 0;
            static uint32_t lastMaxCyclePeriodUs = 0;
            if (comms_us != lastCommsUs || maxComms_us != lastMaxCommsUs ||
                cyclePeriod_us != lastCyclePeriodUs || maxCyclePeriod_us != lastMaxCyclePeriodUs) {
                lastCommsUs = comms_us;
                lastMaxCommsUs = maxComms_us;
                lastCyclePeriodUs = cyclePeriod_us;
                lastMaxCyclePeriodUs = maxCyclePeriod_us;
                double comms_ms = static_cast<double>(comms_us) / 1000.0;
                double cycle_ms = static_cast<double>(cyclePeriod_us) / 1000.0;
                double maxCycle_ms = static_cast<double>(maxCyclePeriod_us) / 1000.0;
                // Display format: "通訊: X.XX ms | 週期: Y.YY ms | 峰值: Z.ZZ ms"
                // PDOProc frequency shown on separate row 2 control
                std::wstringstream cycss;
                uint64_t pdoProcCount = 0;
                uint32_t pdoProcElapsed = 0;
                if (auto* m = AppController::GetInstance().GetMaster()) {
                    pdoProcCount = m->GetPdoProcCycleCount();
                    pdoProcElapsed = m->GetPdoProcElapsedUs();
                }
                static uint64_t lastPdoProcCount = 0;
                static auto lastPdoProcTime = std::chrono::steady_clock::now();
                static double lastPdoProcHz = -1.0;
                // Carry forward last known Hz to avoid flashing "等待中..."
                // when elapsed < 200ms between ticks after first estimation.
                double pdoProcHz = lastPdoProcHz;
                if (pdoProcCount > 0) {
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPdoProcTime).count();
                    if (elapsed >= 200 && lastPdoProcCount > 0) {
                        if (pdoProcCount > lastPdoProcCount) {
                            pdoProcHz = static_cast<double>(pdoProcCount - lastPdoProcCount) * 1000.0 / static_cast<double>(elapsed);
                            lastPdoProcCount = pdoProcCount;
                            lastPdoProcTime = now;
                        } else if (elapsed >= 1000) {
                            // No new PDO cycles for 1s — show "等待中..."
                            pdoProcHz = 0.0;
                        }
                    } else if (lastPdoProcCount == 0) {
                        // First tick after PDO processing starts — initialise baseline
                        // and immediately estimate frequency from configured cycle time
                        // so the display does not sit at "等待中..." for 200ms.
                        lastPdoProcCount = pdoProcCount;
                        lastPdoProcTime = now;
                        uint32_t cycleUs = MasterGetCycleTime();
                        if (cycleUs > 0) {
                            pdoProcHz = 1000000.0 / static_cast<double>(cycleUs);
                        } else {
                            pdoProcHz = 0.0;   // no cycle time configured yet
                        }
                    }
                } else if (lastPdoProcCount > 0) {
                    // PDO processing stopped or reset — after a grace period show "等待中..."
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPdoProcTime).count();
                    if (elapsed >= 1000) {
                        pdoProcHz = 0.0;
                    }
                }
                // Row 1: 通訊 / 週期 / 峰值 / spike (no PDOProc)
                cycss << std::fixed << std::setprecision(2)
                      << L"通訊: " << comms_ms << L" ms | "
                      << L"週期: " << cycle_ms << L" ms | "
                      << L"峰值: " << maxCycle_ms << L" ms";
                {
                    uint32_t spikePhase = MasterGetLastSpikePhase();
                    uint32_t spikeCount = MasterGetSpikeCount();
                    if (spikeCount > 0) {
                        const wchar_t* spikeLabel = (spikePhase == 1) ? L"comms" : ((spikePhase == 2) ? L"idle" : L"?");
                        cycss << L" | Spike# " << spikeCount << L" (" << spikeLabel << L")";
                    }
                }
                // Suppress redraw during text update
                SendMessageW(hEditCurrentCycleTime, WM_SETREDRAW, FALSE, 0);
                SendMessageW(hEditCurrentCycleTime, WM_SETTEXT, 0, (LPARAM)cycss.str().c_str());
                SendMessageW(hEditCurrentCycleTime, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(hEditCurrentCycleTime, NULL, TRUE);

                // Row 2: PDOProc frequency (separate control)
                if (std::abs(pdoProcHz - lastPdoProcHz) > 0.01) {
                    lastPdoProcHz = pdoProcHz;
                    std::wstringstream pdoProcSs;
                    if (pdoProcHz > 0.01) {
                        pdoProcSs << std::fixed << std::setprecision(0) << pdoProcHz << L" Hz";
                    } else {
                        pdoProcSs << L"等待中...";
                    }
                    if (hEditPdoProcFreq && IsWindow(hEditPdoProcFreq)) {
                        SendMessageW(hEditPdoProcFreq, WM_SETREDRAW, FALSE, 0);
                        SendMessageW(hEditPdoProcFreq, WM_SETTEXT, 0, (LPARAM)pdoProcSs.str().c_str());
                        SendMessageW(hEditPdoProcFreq, WM_SETREDRAW, TRUE, 0);
                        InvalidateRect(hEditPdoProcFreq, NULL, TRUE);
                    }
                }
            }
            }
        uint32_t desired_us = MasterGetCycleTime();
            SetCycleInfo(desired_us, comms_us, comms_us);
            // Push actual max cycle-to-cycle period (not max comms) to web globals
            SetMaxObservedCycleTime(MasterGetMaxObservedCycleMicros());
            // ── Push spike diagnostics to HTTP globals ──
            SetSpikeDiagnostics(
                MasterGetLastSpikePhase(),
                MasterGetLastSpikeCycleUs(),
                MasterGetLastSpikeCommsUs(),
                MasterGetSpikeCount());
        }

        int currentWKC = MasterGetWorkingCounter();
        if (currentWKC != lastWKC) {
            lastWKC = currentWKC;
            ss.str(L""); ss.clear();
            ss << currentWKC;
            SendMessageW(hEditWKC, WM_SETREDRAW, FALSE, 0);
            SendMessageW(hEditWKC, WM_SETTEXT, 0, (LPARAM)ss.str().c_str());
            SendMessageW(hEditWKC, WM_SETREDRAW, TRUE, 0);
            InvalidateRect(hEditWKC, NULL, TRUE);
        }

        HWND hEditPDOSentLocal = GetDlgItem(hContentWnd, 2010);
        if (hEditPDOSentLocal) {
            static double lastMMFHz = 0.0;
            double mmfHz = MasterGetMMF1UpdateFrequencyHz();
            // Only update if values changed (with small tolerance for frequency)
            if (std::abs(mmfHz - lastMMFHz) > 0.01) {
                lastMMFHz = mmfHz;
                wchar_t buf[64];
                if (mmfHz > 0.0001) {
                    swprintf_s(buf, L"%6.2f Hz", mmfHz);
                } else {
                    // fallback: show 0 if frequency unavailable
                    swprintf_s(buf, L"   --   ");
                }
                SendMessageW(hEditPDOSentLocal, WM_SETREDRAW, FALSE, 0);
                SendMessageW(hEditPDOSentLocal, WM_SETTEXT, 0, (LPARAM)buf);
                SendMessageW(hEditPDOSentLocal, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(hEditPDOSentLocal, NULL, TRUE);
            }
        }

        static ULONGLONG lastPdoUiUpdateTick = 0;
        const ULONGLONG pdoUiIntervalMs = downToOriginHotPath ? 1200 : (highLoadWorkflow ? 400 : 200);
        const bool shouldUpdatePdoUi = (nowTick - lastPdoUiUpdateTick >= pdoUiIntervalMs);
        if (shouldUpdatePdoUi) {
            lastPdoUiUpdateTick = nowTick;
        }

        if (shouldUpdatePdoUi && confirmedSlave > 0 && MasterGetSlaveCount() > 0) {
        int slaveIdx = confirmedSlave;
        std::vector<PDOChannelValue> channelValues;
        
        // ====================================================================
        // UI Performance Optimization: Use lock-free accessor to avoid blocking
        // the real-time EtherCAT thread. This prevents UI timer from causing
        // jitter in the PDO cycle.
        // ====================================================================
        uint64_t cacheVersion = 0;
        bool haveValues = false;
        if (ethercatService) {
            haveValues = ethercatService->TryGetPDOChannelValuesLockFree(slaveIdx, channelValues, &cacheVersion);
        }
        
        // Fallback to locked version if lock-free cache is empty.
        // During high-load workflows, avoid lock-based fallback to minimize RT contention.
        if (!haveValues && !highLoadWorkflow) {
            if (ethercatService) {
                haveValues = ethercatService->GetPDOChannelValues(slaveIdx, channelValues);
            }
        }
        
        if (!haveValues || channelValues.empty()) {
            // Only log once per 10 seconds to avoid spam
            static uint64_t lastLogTime = 0;
            uint64_t now = GetTickCount64();
            if (now - lastLogTime > 10000) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(std::wstring(L"[PDO] No PDO channel values available from master thread\r\n")));
                lastLogTime = now;
            }
            return;
        }

        static int lastDiagSlave = -1;
        bool slaveChanged = (lastDiagSlave != slaveIdx);
        if (slaveChanged) {
            CaptureCurrentPDOInputsByName();
            if (masterRaw && pdoDisplay.RecalculateOffsetsFromSDO(masterRaw, slaveIdx)) {
                // RecalculateOffsetsFromSDO already calls RebuildPDOControls + ApplyCachedValues
                RestorePDOInputsFromGlobal();
                ApplyAutoProcedureUiLockState();
                HWND hGroupBox = pdoDisplay.GetGroupBox();
                if (hGroupBox && IsWindow(hGroupBox)) {
                    InvalidateRect(hGroupBox, NULL, TRUE);
                    UpdateWindow(hGroupBox);
                }
            }
            lastDiagSlave = slaveIdx;
        }

        int outBase = 0, inBase = 0;
        if (GetSlaveIOBaseBytesWrapper(slaveIdx, outBase, inBase)) {
            if (slaveChanged) {
                std::wstringstream diagSS;
                diagSS << L"[PDO診斷] 從站=" << slaveIdx << L" outBase=" << outBase << L" inBase=" << inBase << L" 項目數=" << pdoDisplay.pdoItems.size() << L"\r\n";
                for (size_t i = 0; i < pdoDisplay.pdoItems.size(); ++i) {
                    const auto& itm = pdoDisplay.pdoItems[i];
                    diagSS << L"  [" << i << L"] " << itm.name << L" idx=0x" << std::hex << std::setw(4) << std::setfill(L'0') << itm.index
                           << L" " << (itm.isOutput ? L"OUT" : L"IN")
                           << L" offset=" << std::dec << itm.byteOffset
                           << L" bits=" << (int)itm.bitLength
                           << L" absOff=" << (itm.isOutput ? outBase : inBase) + itm.byteOffset << L"\r\n";
                }
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(diagSS.str()));
            }
        }

        std::unordered_map<std::wstring, const PDOChannelValue*> valueByName;
        valueByName.reserve(channelValues.size());
        for (const auto &val : channelValues) {
            valueByName[val.descriptor.name] = &val;
        }

        // ====================================================================
        // UI Performance: Only update controls when value actually changed
        // ====================================================================
        for (auto& item : pdoDisplay.pdoItems) {
            auto itVal = valueByName.find(item.name);
            if (itVal == valueByName.end() || !(itVal->second->valid)) {
                // Only update if previously had a value
                if (item.displayValue != L"N/A") {
                    if (item.hValue && IsWindow(item.hValue)) {
                        SetWindowTextW(item.hValue, L"N/A");
                        InvalidateRect(item.hValue, NULL, TRUE);
                    }
                    item.currentValue = L"";
                    item.displayValue = L"N/A";
                }
                continue;
            }
            
            const PDOChannelValue* channelVal = itVal->second;
            std::wstring newCurrentValue = std::to_wstring(channelVal->rawValue);
            
            // Skip update if value hasn't changed
            if (item.currentValue == newCurrentValue) {
                continue;
            }
            
            item.currentValue = newCurrentValue;
            
            // 格式化顯示值（十進制 + 十六進制）
            std::wstring formatted = pdoDisplay.FormatValue(channelVal->rawValue, channelVal->descriptor.bitLength);
            item.displayValue = formatted;
            
            // Update UI only when value changed
            if (item.hValue && IsWindow(item.hValue)) {
                // Suppress redraw during text update to reduce flicker
                SendMessageW(item.hValue, WM_SETREDRAW, FALSE, 0);
                SetWindowTextW(item.hValue, formatted.c_str());
                SendMessageW(item.hValue, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(item.hValue, NULL, TRUE);
            }
        }
        }

        // -----------------------------------------------------------------
        // Update Mechanical Units table: for each slave, read PDO 0x6064 and
        // multiply by motion parameter P1 (from motion params ListView col '值')
        // then display the result in hListMechUnits column 0 (目標位置).
        // -----------------------------------------------------------------
        static ULONGLONG lastMechUiUpdateTick = 0;
        const ULONGLONG mechUiIntervalMs = downToOriginHotPath ? 1200 : (highLoadWorkflow ? 500 : 200);
        const bool shouldUpdateMechUi = (nowTick - lastMechUiUpdateTick >= mechUiIntervalMs);
        if (shouldUpdateMechUi) {
            lastMechUiUpdateTick = nowTick;
        }

        if (shouldUpdateMechUi && hListMechUnits && IsWindow(hListMechUnits)) {
        int slaveCount = MasterGetSlaveCount();
        if (slaveCount < 0) slaveCount = 0;
        int existRows = ListView_GetItemCount(hListMechUnits);
        for (int r = existRows; r < slaveCount; ++r) {
            wchar_t stationBuf[32] = { 0 };
            swprintf_s(stationBuf, L"%d", r + 1);
            LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = r;
            lvi.iSubItem = 0;
            lvi.pszText = stationBuf;
            ListView_InsertItem(hListMechUnits, &lvi);
            for (int ci = 1; ci < 7; ++ci) ListView_SetItemText(hListMechUnits, r, ci, const_cast<LPWSTR>(L""));
        }

        auto DetermineMotionScaling = [&](int slave) -> std::pair<double, bool> {
            double p1Scale = 1.0;
            bool p2Invert = false;
            auto itSaved = savedMotionParamsForSlave.find(slave);
            if (itSaved != savedMotionParamsForSlave.end()) {
                const auto &mat = itSaved->second;
                bool foundP1 = false, foundP2 = false;
                for (size_t rr = 0; rr < mat.size(); ++rr) {
                    if (mat[rr].size() == 0) continue;
                    if (!foundP1 && mat[rr][0] == L"P1" && mat[rr].size() > 3) {
                        const std::wstring &valW = mat[rr][3];
                        wchar_t *endptr = nullptr;
                        double v = wcstod(valW.c_str(), &endptr);
                        if (endptr != valW.c_str()) p1Scale = v;
                        foundP1 = true;
                    }
                    if (!foundP2 && mat[rr][0] == L"P2" && mat[rr].size() > 3) {
                        const std::wstring &valW = mat[rr][3];
                        wchar_t *endptr = nullptr;
                        long v = wcstol(valW.c_str(), &endptr, 10);
                        if (endptr != valW.c_str() && v == 1) p2Invert = true;
                        foundP2 = true;
                    }
                    if (foundP1 && foundP2) break;
                }
            }
            return std::make_pair(p1Scale, p2Invert);
        };

        // Cache previous mechanical unit values to avoid unnecessary ListView updates
        static std::map<int, std::array<std::wstring, 4>> lastMechValues; // [slave] -> [target, actual, targetSpeed, actualSpeed]
        
        const LPWSTR naText = const_cast<LPWSTR>(L"N/A");
        for (int s = 1; s <= slaveCount; ++s) {
            auto scaling = DetermineMotionScaling(s);
            if (ethercatService) {
                ethercatService->SetMechanicalScalingFactors(s, scaling.first, scaling.second);
            }

            const wchar_t* targetText = naText;
            const wchar_t* actualText = naText;
            const wchar_t* targetSpeedText = naText;
            const wchar_t* actualSpeedText = naText;

            MechanicalUnitMetrics metrics;
            // ================================================================
            // UI Performance: Use lock-free accessor for mechanical metrics
            // ================================================================
            bool haveMetrics = false;
            if (ethercatService) {
                haveMetrics = ethercatService->TryGetMechanicalUnitMetricsLockFree(s, metrics);
                if (!haveMetrics && !highLoadWorkflow) {
                    haveMetrics = ethercatService->GetMechanicalUnitMetrics(s, metrics);
                }
            }
            if (haveMetrics && metrics.valid) {
                if (!metrics.targetPosition.empty()) targetText = metrics.targetPosition.c_str();
                if (!metrics.actualPosition.empty()) actualText = metrics.actualPosition.c_str();
                if (!metrics.targetSpeed.empty()) targetSpeedText = metrics.targetSpeed.c_str();
                if (!metrics.actualSpeed.empty()) actualSpeedText = metrics.actualSpeed.c_str();
            }

            // Only update ListView if values changed
            std::array<std::wstring, 4> currentValues = {
                std::wstring(targetText),
                std::wstring(actualText),
                std::wstring(targetSpeedText),
                std::wstring(actualSpeedText)
            };
            
            auto& lastValues = lastMechValues[s];
            bool needUpdate = (lastValues.empty() || lastValues != currentValues);
            
            if (needUpdate) {
                lastValues = currentValues;
                // Suppress redraw during batch update
                SendMessageW(hListMechUnits, WM_SETREDRAW, FALSE, 0);
                ListView_SetItemText(hListMechUnits, s - 1, 1, const_cast<LPWSTR>(targetText));
                ListView_SetItemText(hListMechUnits, s - 1, 2, const_cast<LPWSTR>(actualText));
                ListView_SetItemText(hListMechUnits, s - 1, 3, const_cast<LPWSTR>(targetSpeedText));
                ListView_SetItemText(hListMechUnits, s - 1, 4, const_cast<LPWSTR>(actualSpeedText));
                SendMessageW(hListMechUnits, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(hListMechUnits, NULL, FALSE);
            }
        }
        }
    }

    // ── Watchdog cooldown: skip detection for WATCHDOG_COOLDOWN_MS after a
    // recovery completes, to avoid rapid re-trigger loops when WKC=0 persists
    // transiently without actual slave faults.
    const ULONGLONG cooldownUntil = watchdogCooldownUntil.load();
    if (cooldownUntil != 0) {
        if (nowTick < cooldownUntil) {
            // Still in cooldown — reset any in-progress counter
            if (watchdogCounter.load() != 0) {
                watchdogCounter.store(0);
            }
        } else {
            // Cooldown expired
            watchdogCooldownUntil.store(0);
        }
    }

    if (MasterDetectWatchdogCondition() && watchdogCooldownUntil.load() == 0) {
        int hits = ++watchdogCounter;
        // If master configured to auto-disconnect on repeated network failures,
        // and we've reached the master's threshold, issue a UI-driven disconnect
        // (this allows UI cleanup and preserves consistent states)
        bool autoDisc = false; int masterThr = 0;
        try { autoDisc = MasterIsAutoDisconnectOnNetworkFailureEnabled(); masterThr = MasterGetNetworkFailureThreshold(); } catch(...) {}
        const bool autoDisconnectThresholdReached = autoDisc && masterThr > 0 && hits >= masterThr;
        const bool watchdogThresholdReached = hits >= watchdogThreshold;
        const int reasonThreshold = autoDisconnectThresholdReached ? masterThr : watchdogThreshold;
        if (autoDisconnectThresholdReached || watchdogThresholdReached) {
            NotifyEthercatError(true, BuildWatchdogConditionReason(hits, reasonThreshold));
        } else if (hits == 1) {
            PostUTF8LogToUI(std::string("[Watchdog] transient condition detected - waiting for threshold (hits=") +
                             std::to_string(hits) + "/" + std::to_string(watchdogThreshold) +
                             ", WKC=" + std::to_string(MasterGetWorkingCounter()) + ")\n");
        }
        if (autoDisc && masterThr > 0 && hits >= masterThr) {
            RequestAutomaticDisconnect("Auto-disconnect: network failure threshold reached - issuing disconnect\n");
            watchdogCounter.store(0);
            // Do not attempt recovery; UI will perform full disconnect and restore state
            return;
        }
        // ── Escalation warning: if too many recovery attempts within the window,
        // log a diagnostic warning but continue attempting recovery (do not disconnect).
        if (hits >= watchdogThreshold) {
            const ULONGLONG winStart = recoveryWindowStartTick.load();
            const int attempts = recoveryAttemptCount.load();
            if (winStart != 0 && nowTick >= winStart &&
                (nowTick - winStart) <= WATCHDOG_RECOVERY_WINDOW_MS &&
                attempts >= WATCHDOG_MAX_RECOVERY_ATTEMPTS) {
                std::ostringstream escOss;
                escOss << "[Watchdog] WARNING: " << attempts
                       << " recovery attempts within " << WATCHDOG_RECOVERY_WINDOW_MS
                       << " ms — WKC=0 persists despite repeated recoveries;"
                       << " check network cable, switch, NIC driver, or slave power";
                PostUTF8LogToUI(escOss.str() + "\n");
                // Reset window to allow future warnings (not a disconnect)
                recoveryAttemptCount.store(0);
                recoveryWindowStartTick.store(0);
            }
            if (!recoveryRunning.load()) {
                recoveryRunning.store(true);
                if (recoveryThread.joinable()) {
                    recoveryThread.join();
                }
                recoveryThread = std::thread([this]() { StartWatchdogRecovery(); });
            }
        }
    } else if (!MasterDetectWatchdogCondition()) {
        const int previousHits = watchdogCounter.exchange(0);
        if (previousHits > 0) {
            PostUTF8LogToUI(std::string("[Watchdog] condition recovered before threshold (hits=") +
                             std::to_string(previousHits) + "/" + std::to_string(watchdogThreshold) +
                             ") - no emergency stop required\n");
            if (ethercatError.load() && !anySlaveHasError.load() && !anySlaveHasInternalError.load() &&
                !HasPersistentEthercatErrorCause()) {
                NotifyEthercatError(false);
            }
        }
    }

    // ── WKC/EMCY-triggered global error → emergency stop (one-shot) ──
    // ProcessEmcyErrors() in the RT cyclic thread sets persistentGlobalError_
    // when WKC deviation or EMCY event is detected.  The UI timer picks this
    // up asynchronously and triggers a safe motion stop when auto-procedure
    // is active.  Reads lastError from the master to include the specific
    // trigger reason (WKC value, slave#, error code) in the status window.
    //
    // wkcStopTriggered_ latch prevents repeated re-triggering while the
    // global error persists.  Resets when HasGlobalError() clears.
    if (HasPersistentEthercatErrorCause()) {
        if (!wkcStopTriggered_.load() && IsAutoProcedureRunning() &&
            !immediateStopBusy.load() && !disconnecting.load()) {
            wkcStopTriggered_.store(true);

            // Build diagnostic message with trigger source details
            std::string reason;
            if (auto* diagMaster = AppController::GetInstance().GetMaster()) {
                std::string lastErr = diagMaster->GetLastError();
                int32_t rawWkc = diagMaster->GetWorkingCounterRaw();
                uint32_t expWkc = diagMaster->GetExpectedWkc();
                char buf[256];
                if (rawWkc < 0) {
                    snprintf(buf, sizeof(buf),
                        "[AutoStop] EC_NOFRAME (NIC no-frame) — triggering emergency stop"
                        " | lastEMCY=%s\n", lastErr.c_str());
                } else if (rawWkc == 0) {
                    snprintf(buf, sizeof(buf),
                        "[AutoStop] WKC=0 (no slave response) — triggering emergency stop"
                        " | lastEMCY=%s\n", lastErr.c_str());
                } else if (expWkc > 0 && static_cast<uint32_t>(rawWkc) < expWkc) {
                    snprintf(buf, sizeof(buf),
                        "[AutoStop] WKC=%d < expected=%u (partial slave loss) — triggering emergency stop"
                        " | lastEMCY=%s\n", rawWkc, expWkc, lastErr.c_str());
                } else {
                    snprintf(buf, sizeof(buf),
                        "[AutoStop] EMCY event detected — triggering emergency stop"
                        " | lastEMCY=%s\n", lastErr.c_str());
                }
                reason = buf;
            } else {
                reason = "[AutoStop] global error latch — triggering emergency stop\n";
            }
            PostUTF8LogToUI(reason);
            PostMessageW(this->GetHwnd(),
                         WM_USER_AUTO_EMERGENCY_STOP_REQUEST,
                         (WPARAM)0, 0);
        }
    } else {
        // Global error cleared — reset latch for next occurrence
        wkcStopTriggered_.store(false);
    }

    // ========================================================================
    // Pose6D 前置驗證層：警告音與自動恢復
    // ========================================================================
    if (masterRaw) {
        bool poseWarning = masterRaw->IsPose6DLimitWarningActive();
        static bool prevPoseWarning = false;
        if (poseWarning && !prevPoseWarning) {
            // 邊緣觸發：首次進入違規狀態 → 記錄訊息 + 警告音
            std::wstring msg = masterRaw->ConsumePose6DLimitWarningMessage();
            if (!msg.empty()) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(
                    L"[Pose6D 限制] " + msg + L"\r\n"));
            }
            MessageBeep(MB_ICONWARNING);
        } else if (!poseWarning && prevPoseWarning) {
            // 邊緣觸發：恢復正常 → 記錄恢復訊息 + 關閉警告音
            std::wstring msg = masterRaw->ConsumePose6DLimitWarningMessage();
            if (!msg.empty()) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(
                    L"[Pose6D 限制] " + msg + L"\r\n"));
            }
            // 清除警告狀態（不再發出提示音）
        }
        prevPoseWarning = poseWarning;

        // Pose6D 動態行程縮放（Z 觸發）：單次警報聲 + 訊息 log
        bool dynScaling = masterRaw->IsPose6DDynamicScalingActive();
        static bool prevDynScaling = false;
        if (dynScaling && !prevDynScaling) {
            // 邊緣觸發：動態縮放剛啟動
            std::wstring msg = masterRaw->ConsumePose6DDynamicScalingMessage();
            if (!msg.empty()) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(
                    L"[動態縮放] " + msg + L"\r\n"));
            }
            MessageBeep(MB_ICONINFORMATION);
        } else if (!dynScaling && prevDynScaling) {
            // 邊緣觸發：動態縮放已關閉
            std::wstring msg = masterRaw->ConsumePose6DDynamicScalingMessage();
            if (!msg.empty()) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(
                    L"[動態縮放] " + msg + L"\r\n"));
            }
        }
        prevDynScaling = dynScaling;
    }

}

// Update the displayed HTTP server URL and enable/disable the Open button.

void MainWindow::UpdateHttpServerUrl()
{
    if (hStaticHttpUrl && IsWindow(hStaticHttpUrl)) {
        std::string url = GetHttpServerUrl();
        std::wstring display;
        if (url.empty()) {
            display = L"Web UI: (not running)";
        } else {
            display = L"Web UI: ";
            display += CoreHelpers::Utf8ToWide(url);
            // Show labeled counts: 已登入會話數 / TCP 活躍連線數 / 最大允許連線數
            int sessions = GetActiveSessionCount();
            int active = GetActiveHttpConnections();
            int maxc = GetMaxHttpConnections();
            std::wstring connInfo = L" (已登入會話數:" + std::to_wstring(sessions)
                                   + L" / TCP 活躍連線數:" + std::to_wstring(active)
                                   + L" / 最大允許連線數:" + std::to_wstring(maxc) + L")";
            display += connInfo;
        }
        SetWindowTextW(hStaticHttpUrl, display.c_str());
    }
    if (hBtnOpenHttp && IsWindow(hBtnOpenHttp)) {
        bool enabled = !GetHttpServerUrl().empty();
        SetControlEnabled(hBtnOpenHttp, enabled);
    }
}

// -----------------------------------------------------------------------------
// Watchdog recovery logic (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------


