#include "MainWindow.h"
#include "../app/AppController.h"
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
#include <limits>
#include "core/AppConfig.h"
#include "Win32Utils.h"
#include "HttpServer.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../include/EtherCATMaster.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
#include "../include/MotionLogic.h"
#include "../include/AutoWindow.h"
struct PreparePdoRequest {
    int slaveIdx = 0;
    std::promise<bool> completion;
};

MainWindow::MotionParamSnapshot MainWindow::CaptureMotionParams(int slaveIdx) const {
    MotionParamSnapshot snap;
    auto it = savedMotionParamsForSlave.find(slaveIdx);
    if (it == savedMotionParamsForSlave.end()) return snap;
    const auto &rows = it->second;
    for (const auto &row : rows) {
        if (row.size() <= 3) continue;
        const std::wstring &name = row[0];
        const std::wstring &value = row[3];
        if (!snap.haveP1 && name == L"P1") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP1 = true; snap.p1 = v; }
        } else if (!snap.haveP2 && name == L"P2") {
            wchar_t *endp = nullptr;
            long v = wcstol(value.c_str(), &endp, 10);
            if (endp != value.c_str()) { snap.haveP2 = true; snap.p2 = v; }
        } else if (!snap.haveP3 && name == L"P3") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP3 = true; snap.p3 = v; }
        } else if (!snap.haveP4 && name == L"P4") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP4 = true; snap.p4 = v; }
        } else if (!snap.haveP5 && name == L"P5") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP5 = true; snap.p5 = v; }
        } else if (!snap.haveP6 && name == L"P6") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP6 = true; snap.p6 = v; }
        } else if (!snap.haveP8 && name == L"P8") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP8 = true; snap.p8 = v; }
        } else if (!snap.haveP9 && name == L"P9") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP9 = true; snap.p9 = v; }
        } else if (!snap.haveP10 && name == L"P10") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP10 = true; snap.p10 = v; }
        } else if (!snap.haveP11 && name == L"P11") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP11 = true; snap.p11 = v; }
        } else if (!snap.haveP12 && name == L"P12") {
            wchar_t *endp = nullptr;
            double v = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP12 = true; snap.p12 = v; }
        } else if (!snap.haveP13 && name == L"P13") {
            wchar_t *endp = nullptr;
            double dv = wcstod(value.c_str(), &endp);
            if (endp != value.c_str()) { snap.haveP13 = true; snap.p13 = dv; }
            unsigned long raw = wcstoul(value.c_str(), &endp, 10);
            if (endp != value.c_str()) { snap.rawP13 = (uint32_t)raw; }
        }
    }
    return snap;
}

void MainWindow::ApplySavedPdoFilterConfig(EtherCATMaster *master, int slaveIdx,
                                           const MotionParamSnapshot &snap,
                                           const char *logPrefix,
                                           bool emitConfiguredLog) {
    if (!master || slaveIdx <= 0) {
        return;
    }

    const char *prefix = (logPrefix && *logPrefix) ? logPrefix : "[AUTO]";
    auto isPositiveFinite = [](bool haveValue, double value) {
        return haveValue && std::isfinite(value) && value > 0.0;
    };

    const bool validP3 = isPositiveFinite(snap.haveP3, snap.p3);
    const bool validP4 = isPositiveFinite(snap.haveP4, snap.p4);
    const bool validP5 = isPositiveFinite(snap.haveP5, snap.p5);
    const bool validP6 = isPositiveFinite(snap.haveP6, snap.p6);
    const double configuredJerk = validP6 ? snap.p6 : 0.0;
    const auto violationConfig = AppConfig::ReadPdoFilterDynamicViolationConfig();

    if (validP3 && validP4 && validP5) {
        master->ConfigurePDOFilter(slaveIdx, snap.p3, snap.p4, snap.p5, configuredJerk,
                                   violationConfig.velocityThresholdScale,
                                   violationConfig.accelerationThresholdScale,
                                   violationConfig.velocityViolationDelaySec,
                                   violationConfig.accelerationViolationDelaySec);
        if (emitConfiguredLog || (snap.haveP6 && !validP6)) {
            std::ostringstream oss;
            oss << prefix << " Slave " << slaveIdx
                << " PDO filter configured: P3=" << snap.p3
                << " P4=" << snap.p4
                << " P5=" << snap.p5
                << " P6=" << configuredJerk;
            if (snap.haveP6 && !validP6) {
                oss << " (ignored invalid saved P6=" << snap.p6 << ")";
            }
            oss << "\n";
            PostUTF8LogToUI(oss.str());
        }
    } else {
        master->ConfigurePDOFilter(slaveIdx, 0.0, 0.0, 0.0, configuredJerk,
                                   violationConfig.velocityThresholdScale,
                                   violationConfig.accelerationThresholdScale,
                                   violationConfig.velocityViolationDelaySec,
                                   violationConfig.accelerationViolationDelaySec);
        std::ostringstream oss;
        oss << prefix << " Slave " << slaveIdx << " PDO filter passive: ";
        bool firstIssue = true;
        auto appendIssue = [&](const char *name, bool haveValue, double value) {
            if (!firstIssue) {
                oss << ", ";
            }
            firstIssue = false;
            oss << name << '=';
            if (!haveValue) {
                oss << "missing";
            } else if (!std::isfinite(value)) {
                oss << "non-finite";
            } else {
                oss << value;
            }
        };
        if (!validP3) appendIssue("P3", snap.haveP3, snap.p3);
        if (!validP4) appendIssue("P4", snap.haveP4, snap.p4);
        if (!validP5) appendIssue("P5", snap.haveP5, snap.p5);
        if (snap.haveP6 && !validP6) {
            if (!firstIssue) {
                oss << "; ";
            }
            oss << "P6 ignored=" << snap.p6;
        }
        oss << "\n";
        PostUTF8LogToUI(oss.str());
    }

    if (snap.haveP9 || snap.haveP10) {
        double scaledMin = snap.haveP9 ? snap.p9 : std::numeric_limits<double>::quiet_NaN();
        double scaledMax = snap.haveP10 ? snap.p10 : std::numeric_limits<double>::quiet_NaN();
        const double p1Scale = snap.haveP1 ? snap.p1 : 1.0;
        const bool p2Invert = snap.haveP2 && snap.p2 == 1;
        if (snap.haveP9 && snap.haveP10) {
            if (scaledMin > scaledMax) std::swap(scaledMin, scaledMax);
            double sumAbs = std::abs(scaledMin) + std::abs(scaledMax);
            try {
                std::ostringstream sdbg;
                sdbg << std::fixed << std::setprecision(15)
                     << prefix << " Slave " << slaveIdx
                     << " motion params: P1=" << p1Scale
                     << " P2Invert=" << (p2Invert ? 1 : 0)
                     << " P9=" << snap.p9
                     << " P10=" << snap.p10 << "\n";
                PostUTF8LogToUI(sdbg.str());
            } catch (...) {}

            if (sumAbs < 1.0) {
                try {
                    std::string warn = std::string(prefix) + " Slave " + std::to_string(slaveIdx) +
                        " PDO position limits suspicious (scaled): min=" + std::to_string(scaledMin) +
                        " max=" + std::to_string(scaledMax) +
                        " (|min|+|max|=" + std::to_string(sumAbs) + " < 1.0) - skipping SetPDOFilterLimits\n";
                    PostUTF8LogToUI(warn);
                } catch (...) {}
            } else {
                try {
                    master->SetPDOFilterLimits(slaveIdx, scaledMin, scaledMax);
                } catch (...) {
                    PostUTF8LogToUI(std::string(prefix) + " Slave " + std::to_string(slaveIdx) + " Failed to set PDO position limits\n");
                }
            }
        } else {
            PostUTF8LogToUI(std::string(prefix) + " Slave " + std::to_string(slaveIdx) + " P9/P10 incomplete; skipping PDO position limits\n");
        }
    }
}

void MainWindow::OnFaultReset(bool assert)
{
    if (IsExternalImmediateStopActive() || IsExternalEStopActive()) {
        PrependStatus(L"[FaultReset] 外部立即停止或緊急停止輸入仍作用中，無法執行異常重置\r\n");
        return;
    }

    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) {
        PrependStatus(L"[FaultReset] No slave selected\r\n");
        return;
    }
    if (!IsMasterInitialized()) {
        PrependStatus(L"[FaultReset] EtherCAT master not available\r\n");
        NotifyEthercatError(true);
        return;
    }

    // Prevent fault-reset while system is busy (Immediate/Emergency Stop unaffected)
    {
        const auto snapshot = GetSystemStateSnapshot();
        if (snapshot.busy) {
            PrependStatus(L"[FaultReset] 系統忙碌，無法執行 Fault Reset (避免衝突操作)\r\n");
            return;
        }
    }

    if (motionController) {
        // Mark UI busy while FaultResetAsync runs
        SetBusyFlagAndNotify(this->faultResetBusy, true);
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->FaultResetAsync(slaveIdx, assert);
            std::string utf = CoreHelpers::WideToUtf8(r.message);
            PostUTF8LogToUI(utf);
            if (!assert && r.success) {
                SetManualResetRequired(false);
                try {
                    ClearMasterErrorAndResetPdoFilters(slaveIdx);
                    RefreshInternalErrorStateFrom603F("[FaultReset] ");
                } catch (...) {
                    PostUTF8LogToUI(std::string("[FaultReset] Failed to clear master error or refresh 0x603F state after reset\n"));
                }
            }
            SetBusyFlagAndNotify(this->faultResetBusy, false);
        }).detach();
    } else {
        PrependStatus(L"[FaultReset] MotionController not initialized\r\n");
    }
}


void MainWindow::OnImmediateStop(bool assert)
{
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[ImmediateStop] No slave selected\r\n"); return; }
    if (!IsMasterInitialized()) { PrependStatus(L"[ImmediateStop] EtherCAT master not available\r\n"); NotifyEthercatError(true); return; }
    SetManualResetRequired(true);

    if (motionController) {
        // Stop PDO 0x607A logical target position recording immediately when user requests Immediate Stop
        try {
            if (auto* master = AppController::GetInstance().GetMaster()) {
                master->EnablePDO607ARecording(false);
            }
        } catch(...) {}
        SetBusyFlagAndNotify(this->immediateStopBusy, true);
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->ImmediateStopAsync(slaveIdx, assert);
            std::string utf = CoreHelpers::WideToUtf8(r.message);
            PostUTF8LogToUI(utf);
            SetBusyFlagAndNotify(this->immediateStopBusy, false);
        }).detach();
    } else {
        PrependStatus(L"[ImmediateStop] MotionController not initialized\r\n");
    }
}


void MainWindow::OnPowerOff(bool assert)
{
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[PowerOff] No slave selected\r\n"); return; }
    if (!IsMasterInitialized()) { PrependStatus(L"[PowerOff] EtherCAT master not available\r\n"); NotifyEthercatError(true); return; }
    if (IsManualResetRequired()) { PrependStatus(L"[PowerOff] 已停止且需先異常重置，無法執行 PowerOff\r\n"); return; }

    // Guard: prevent power-off while other long operations are running
    {
        const auto snapshot = GetSystemStateSnapshot();
        if (snapshot.busy) {
            PrependStatus(L"[PowerOff] 系統忙碌，無法執行 PowerOff (避免衝突操作)\r\n");
            return;
        }
    }

    bool expectedPowerOpBusy = false;
    if (!this->powerOpBusy.compare_exchange_strong(expectedPowerOpBusy, true)) {
        PrependStatus(L"[PowerOff] 電源操作已在執行中，忽略重複請求\r\n");
        return;
    }

    if (motionController) {
        PostAutoStatusUpdate();
        RequestMotionUiRefresh();
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->PowerOffAsync(slaveIdx, assert);
            std::string utf = CoreHelpers::WideToUtf8(r.message);
            PostUTF8LogToUI(utf);
            SetBusyFlagAndNotify(this->powerOpBusy, false);
        }).detach();
    } else {
        SetBusyFlagAndNotify(this->powerOpBusy, false);
        PrependStatus(L"[PowerOff] MotionController not initialized\r\n");
    }
}


void MainWindow::OnPowerOn(bool assert)
{
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[PowerOn] No slave selected\r\n"); return; }
    if (!IsMasterInitialized()) { PrependStatus(L"[PowerOn] EtherCAT master not available\r\n"); NotifyEthercatError(true); return; }
    if (IsManualResetRequired()) { PrependStatus(L"[PowerOn] 已停止且需先異常重置，無法執行 PowerOn\r\n"); return; }

    // Guard: prevent power-on while other long operations are running
    {
        const auto snapshot = GetSystemStateSnapshot();
        if (snapshot.busy) {
            PrependStatus(L"[PowerOn] 系統忙碌，無法執行 PowerOn (避免衝突操作)\r\n");
            return;
        }
    }

    bool expectedPowerOpBusy = false;
    if (!this->powerOpBusy.compare_exchange_strong(expectedPowerOpBusy, true)) {
        PrependStatus(L"[PowerOn] 電源操作已在執行中，忽略重複請求\r\n");
        return;
    }

    if (motionController) {
        PostAutoStatusUpdate();
        RequestMotionUiRefresh();
        std::thread([this, slaveIdx, assert]() {
            // Compute P2/P11/P12 from savedMotionParamsForSlave and pass to MotionController
            bool haveVals = false;
            uint32_t p2 = 0, p11 = 0, p12 = 0;
            auto it = savedMotionParamsForSlave.find(slaveIdx);
            if (it != savedMotionParamsForSlave.end()) {
                const auto &mat = it->second;
                double dp11 = 0.0, dp12 = 0.0; long lp2 = 0;
                bool haveP11 = false, haveP12 = false;
                for (size_t rr = 0; rr < mat.size(); ++rr) {
                    if (mat[rr].size() == 0) continue;
                    if (!haveP11 && mat[rr][0] == L"P11" && mat[rr].size() > 3) { wchar_t* e = nullptr; double vv = wcstod(mat[rr][3].c_str(), &e); if (e != mat[rr][3].c_str()) { dp11 = vv; haveP11 = true; } }
                    if (!haveP12 && mat[rr][0] == L"P12" && mat[rr].size() > 3) { wchar_t* e = nullptr; double vv = wcstod(mat[rr][3].c_str(), &e); if (e != mat[rr][3].c_str()) { dp12 = vv; haveP12 = true; } }
                    if (mat[rr][0] == L"P2" && mat[rr].size() > 3) { wchar_t* e = nullptr; long vv = wcstol(mat[rr][3].c_str(), &e, 10); if (e != mat[rr][3].c_str()) lp2 = vv; }
                    if (haveP11 && haveP12) break;
                }
                if (haveP11 && haveP12) {
                    p11 = (uint32_t)llround(dp11);
                    p12 = (uint32_t)llround(dp12);
                    p2 = (uint32_t)lp2;
                    haveVals = true;
                }
            }

            // Before calling controller: optionally log when params are available
            if (haveVals) {
                // Produce a source timestamp so this UI-originated log carries the true UI time
                SYSTEMTIME st; GetLocalTime(&st);
                wchar_t timebuf[64]; swprintf_s(timebuf, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
                std::wstringstream wlog;
                // Mark this message as originating from the UI so operator can distinguish duplicate logs
                wlog << timebuf << L"[PowerOn][UI] params available: P2=" << p2 << L" P11=" << p11 << L" P12=" << p12 << L"\n";
                std::string utf = CoreHelpers::WideToUtf8(wlog.str());
                PostUTF8LogToUI(utf);
            }

            // Call MotionController::PowerOnAsync with computed params (or without if not available)
            MotionResult r;
            if (haveVals) r = this->motionController->PowerOnAsync(slaveIdx, assert, true);
            else r = this->motionController->PowerOnAsync(slaveIdx, assert, false);

            std::string utf = CoreHelpers::WideToUtf8(r.message);
            PostUTF8LogToUI(utf);
            SetBusyFlagAndNotify(this->powerOpBusy, false);
        }).detach();
    } else {
        SetBusyFlagAndNotify(this->powerOpBusy, false);
        PrependStatus(L"[PowerOn] MotionController not initialized\r\n");
    }
}


void MainWindow::OnHome(bool assert)
{
    if (!assert) return;
    
    // Get slave index on main thread (fast operation)
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    bool expected = false;
    if (!this->homingBusy.compare_exchange_strong(expected, true)) {
        PrependStatus(L"[OriginSet] 原點設定已在執行中，忽略重複請求\r\n");
        return;
    }
    PostAutoStatusUpdate();

    // Move ALL validation and execution to background thread
    // Main thread returns immediately without any UI updates or blocking operations
    std::thread([this, slaveIdx]() {
        struct HomingBusyGuard {
            MainWindow* window;
            ~HomingBusyGuard() {
                window->SetBusyFlagAndNotify(window->homingBusy, false);
            }
        } homingBusyGuard{this};

        // Validation in background thread - does not block main UI
        if (slaveIdx <= 0) { PrependStatus(L"[OriginSet] No slave selected\r\n"); return; }
        if (!IsMasterInitialized()) { PrependStatus(L"[OriginSet] EtherCAT master not available\r\n"); return; }
        if (IsManualResetRequired()) { PrependStatus(L"[OriginSet] 已停止且需先異常重置，無法執行原點設定\r\n"); return; }

        // Guard: prevent origin-setting while other long operations are running.
        // homingBusy was already latched on the UI thread to block duplicate starts,
        // so exclude that one flag here or the request rejects itself immediately.
        if (workerRunning.load() || HasAnyTransientBusyFlag(&this->homingBusy)) {
            PrependStatus(L"[OriginSet] 系統忙碌，無法執行原點設定 (避免衝突操作)\r\n");
            return;
        }

        // Execute origin-setting operation: validate, prepare PDOs, compute 0x607C and call controller
        do {
            if (!IsMasterInitialized() || !motionController) {
                PostUTF8LogToUI(std::string("[OriginSet] Master or controller unavailable\n"));
                break;
            }

            // Ensure PDO mapping is prepared on UI thread
            if (!PreparePDOForSlave(slaveIdx)) {
                std::wstringstream ss; ss << L"[OriginSet] Failed to prepare PDO mapping for slave " << slaveIdx << L"\r\n";
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(ss.str()));
                break;
            }

            // Validate preconditions (controlword bit4 must be clear, etc.)
            std::wstring vErr;
            if (!ValidateHomePreconditions(slaveIdx, vErr)) {
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(vErr));
                break;
            }

            // Compute optional 0x607C value from motion params
            bool have607c = false;
            uint64_t v607c = 0;
            if (!ComputeHomeCommandValue(slaveIdx, have607c, v607c)) {
                PostUTF8LogToUI(std::string("[OriginSet] Failed to compute 0x607C value\n"));
                break;
            }

            // Call controller to perform the DS402 origin-setting sequence
            MotionResult r = this->motionController->HomeAsync(slaveIdx, true, have607c, (uint32_t)v607c);
            PostUTF8LogToUI(CoreHelpers::WideToUtf8(r.message));
        } while (false);
    }).detach();
}


uint32_t MainWindow::ResolveJogVelocityCommand(int slaveIdx, bool positiveDirection) const {
    MotionParamSnapshot snap = CaptureMotionParams(slaveIdx);
    int32_t base = positiveDirection ? 20000 : -20000;
    if (snap.haveP1 && snap.haveP8 && snap.p1 != 0.0) {
        double ratio = snap.p8 / snap.p1;
        base = (int32_t)llround(ratio);
    }
    bool invert = snap.haveP2 && snap.p2 == 1;
    if ((positiveDirection && invert) || (!positiveDirection && !invert)) {
        base = -base;
    }
    return (uint32_t)base;
}


namespace {

bool TryParseMechanicalMetricValue(const std::wstring& text, double& outValue)
{
    outValue = 0.0;
    if (text.empty()) {
        return false;
    }
    if (_wcsicmp(text.c_str(), L"N/A") == 0) {
        return false;
    }
    wchar_t* endp = nullptr;
    const double value = wcstod(text.c_str(), &endp);
    if (endp == text.c_str() || !std::isfinite(value)) {
        return false;
    }
    outValue = value;
    return true;
}

} // namespace


bool MainWindow::TryGetMechanicalActualPosition(int slaveIdx, double &outPosition) const {
    outPosition = 0.0;
    if (slaveIdx <= 0) return false;
    auto* service = AppController::GetInstance().GetEtherCATService();
    if (!service) return false;
    MechanicalUnitMetrics metrics;
    bool gotMetrics = service->TryGetMechanicalUnitMetricsLockFree(slaveIdx, metrics);
    if (!gotMetrics) {
        static thread_local std::array<ULONGLONG, 128> lastLockedReadTick{};
        ULONGLONG nowTick = GetTickCount64();
        bool allowFallback = true;
        if (slaveIdx > 0 && slaveIdx < static_cast<int>(lastLockedReadTick.size())) {
            const ULONGLONG fallbackIntervalMs = 200;
            ULONGLONG &lastTick = lastLockedReadTick[slaveIdx];
            if (nowTick - lastTick < fallbackIntervalMs) {
                allowFallback = false;
            } else {
                lastTick = nowTick;
            }
        }
        if (allowFallback) {
            gotMetrics = service->GetMechanicalUnitMetrics(slaveIdx, metrics);
        }
    }
    if (!gotMetrics || !metrics.valid) return false;
    return TryParseMechanicalMetricValue(metrics.actualPosition, outPosition);
}


bool MainWindow::TryGetMechanicalActualSpeed(int slaveIdx, double &outSpeed) const {
    outSpeed = 0.0;
    if (slaveIdx <= 0) return false;
    auto* service = AppController::GetInstance().GetEtherCATService();
    if (!service) return false;
    MechanicalUnitMetrics metrics;
    // Prefer lock-free path to avoid contending with RT cyclic locks.
    bool gotMetrics = service->TryGetMechanicalUnitMetricsLockFree(slaveIdx, metrics);
    if (!gotMetrics) {
        // Throttle locked fallback reads to reduce contention during high-load workflows.
        static thread_local std::array<ULONGLONG, 128> lastLockedReadTick{};
        ULONGLONG nowTick = GetTickCount64();
        bool allowFallback = true;
        if (slaveIdx > 0 && slaveIdx < static_cast<int>(lastLockedReadTick.size())) {
            const ULONGLONG fallbackIntervalMs = 200;
            ULONGLONG &lastTick = lastLockedReadTick[slaveIdx];
            if (nowTick - lastTick < fallbackIntervalMs) {
                allowFallback = false;
            } else {
                lastTick = nowTick;
            }
        }
        if (allowFallback) {
            gotMetrics = service->GetMechanicalUnitMetrics(slaveIdx, metrics);
        }
    }
    if (!gotMetrics || !metrics.valid) return false;
    return TryParseMechanicalMetricValue(metrics.actualSpeed, outSpeed);
}


bool MainWindow::ComputeHomeCommandValue(int slaveIdx, bool &haveValue, uint64_t &value) {
    MotionParamSnapshot snap = CaptureMotionParams(slaveIdx);
    haveValue = false;
    value = 0;
    if (snap.haveP13 && snap.haveP1 && snap.p1 != 0.0) {
        double dv = snap.p13 / snap.p1;
        if (snap.haveP2 && snap.p2 == 1) dv = -dv;
        long long iv = llround(dv);
        value = (uint64_t)(int64_t)iv;
        haveValue = true;
#if defined(_DEBUG)
        std::wstringstream wlog;
        wlog << L"[AutoDownToOrigin] computed 0x607C value=" << (int64_t)iv
             << L" (P13=" << snap.p13 << L" P1=" << snap.p1 << L" P2=" << (snap.haveP2 ? snap.p2 : 0) << L")\n";
        PostUTF8LogToUI(CoreHelpers::WideToUtf8(wlog.str()));
#endif
        return true;
    }
    if (snap.haveP13) {
        int64_t signedVal = (int64_t)(int32_t)snap.rawP13;
        if (snap.haveP2 && snap.p2 == 1) signedVal = -signedVal;
        value = (uint64_t)signedVal;
        haveValue = true;
#if defined(_DEBUG)
        std::wstringstream wlog;
        wlog << L"[AutoDownToOrigin] using raw P13 for 0x607C=" << signedVal << L" (P2=" << (snap.haveP2 ? snap.p2 : 0) << L")\n";
        PostUTF8LogToUI(CoreHelpers::WideToUtf8(wlog.str()));
#endif
        return true;
    }
#if defined(_DEBUG)
    PostUTF8LogToUI(std::string("[AutoDownToOrigin] no P13 found; defaulting 0x607C to 0\n"));
#endif
    haveValue = false;
    value = 0;
    return true;
}


bool MainWindow::ValidateHomePreconditions(int slaveIdx, std::wstring &errorMessage) {
    errorMessage.clear();
    const PDOItem* cwItem = nullptr;
    const PDOItem* statusItem = nullptr;
    for (const auto &it : pdoDisplay.pdoItems) {
        if (it.index == 0x6040 && it.isOutput) cwItem = &it;
        else if (it.index == 0x6041 && !it.isOutput) statusItem = &it;
    }
    if (!cwItem || !statusItem) {
        errorMessage = L"[AutoDownToOrigin] PDO mapping missing for 0x6040/0x6041";
        return false;
    }
    std::vector<uint8_t> snap;
    if (!GetPDODataSnapshotWrapper(slaveIdx, snap)) {
        errorMessage = L"[AutoDownToOrigin] Failed to obtain PDO snapshot";
        return false;
    }
    int32_t cwVal = 0;
    auto* master = AppController::GetInstance().GetMaster();
    if (!master || !ReadPDOItemValueFromSnapshot(master, slaveIdx, snap, *cwItem, cwVal)) {
        errorMessage = L"[AutoDownToOrigin] Failed to decode controlword from snapshot";
        return false;
    }
    if ((static_cast<uint32_t>(cwVal) & 0x0010u) != 0) {
        std::wstringstream ss;
        ss << L"[AutoDownToOrigin] Controlword bit4 already set for slave " << slaveIdx;
        errorMessage = ss.str();
        return false;
    }
    return true;
}


bool MainWindow::RunAutoDownToOriginWorkflow() {
    auto* master = AppController::GetInstance().GetMaster();
    if (motionController) {
        motionController->SetMaster(master);
    }

    if (!master || !master->IsInitialized() || !motionController) {
        PostUTF8LogToUI(std::string("[AutoDownToOrigin] Master or controller unavailable\n"));
        return false;
    }
    int slaveCount = MasterGetSlaveCount();
    if (slaveCount <= 0) {
        PostUTF8LogToUI(std::string("[AutoDownToOrigin] No slaves detected\n"));
        return false;
    }
    struct TorqueLimitRestoreEntry {
        int slave = 0;
        uint16_t index = 0;
        uint32_t value = 0;
    };

    std::vector<TorqueLimitRestoreEntry> torqueLimitRestoreEntries;
    auto writeTorqueLimitValue = [&](int slave, uint16_t index, uint32_t value) {
        uint8_t bytes[4] = {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF)
        };
        return this->MasterWriteSDO(static_cast<uint16_t>(slave), index, 0x00, bytes, 4);
    };
    auto restoreNegativeTorqueLimits = [&]() {
        if (torqueLimitRestoreEntries.empty()) {
            return;
        }

        int restoredCount = 0;
        for (const auto &entry : torqueLimitRestoreEntries) {
            if (writeTorqueLimitValue(entry.slave, entry.index, entry.value)) {
                ++restoredCount;
                continue;
            }

            std::ostringstream oss;
            oss << "[AutoDownToOrigin] Failed to restore torque limit slave=" << entry.slave
                << " index=0x" << std::hex << std::uppercase << entry.index
                << " value=" << std::dec << entry.value << "\n";
            PostUTF8LogToUI(oss.str());
        }

        std::ostringstream summary;
        summary << "[AutoDownToOrigin] Restored configured P12 torque limit for "
                << restoredCount << "/" << torqueLimitRestoreEntries.size() << " slaves\n";
        PostUTF8LogToUI(summary.str());
        torqueLimitRestoreEntries.clear();
    };
    struct ScopeExit {
        std::function<void()> fn;
        ~ScopeExit() {
            if (fn) {
                fn();
            }
        }
    } restoreTorqueLimitGuard{restoreNegativeTorqueLimits};

    for (int s = 1; s <= slaveCount; ++s) {
        MotionParamSnapshot snap = CaptureMotionParams(s);
        if (!snap.haveP12) {
            std::ostringstream oss;
            oss << "[AutoDownToOrigin] Missing configured P12 for slave " << s << "\n";
            PostUTF8LogToUI(oss.str());
            return false;
        }

        const uint32_t configuredP12 = static_cast<uint32_t>(llround(snap.p12));
        const uint32_t reducedP12 = static_cast<uint32_t>(llround(static_cast<double>(configuredP12) * 0.5));
        const uint16_t targetIndex = (snap.haveP2 && snap.p2 == 1) ? 0x60E0 : 0x60E1;
        if (!writeTorqueLimitValue(s, targetIndex, reducedP12)) {
            std::ostringstream oss;
            oss << "[AutoDownToOrigin] Failed to apply temporary 50% P12 torque limit"
                << " slave=" << s
                << " index=0x" << std::hex << std::uppercase << targetIndex
                << " configured=" << std::dec << configuredP12
                << " temporary=" << reducedP12 << "\n";
            PostUTF8LogToUI(oss.str());
            return false;
        }

        torqueLimitRestoreEntries.push_back({s, targetIndex, configuredP12});
    }
    {
        std::ostringstream oss;
        oss << "[AutoDownToOrigin] Applied temporary 50% P12 torque limit to "
            << torqueLimitRestoreEntries.size() << " slaves\n";
        PostUTF8LogToUI(oss.str());
    }

    std::vector<bool> jogStarted(slaveCount + 1, false);
    std::vector<bool> jogStopped(slaveCount + 1, false);
    struct DownToOriginCompressionTarget {
        bool havePositioningPoint = false;
        double positioningPoint = 0.0;
        bool haveCompressionPoint = false;
        double compressionPoint = 0.0;
    };
    std::vector<DownToOriginCompressionTarget> compressionTargets(slaveCount + 1);
    std::vector<bool> positioningPointReached(slaveCount + 1, false);
    std::vector<bool> compressionPointReached(slaveCount + 1, false);
    std::vector<bool> haveStartPosition(slaveCount + 1, false);
    std::vector<double> startPositions(slaveCount + 1, 0.0);
    auto hasReachedMechanicalTarget = [](double startPosition, double currentPosition, double targetPosition, double tolerance) {
        if (!std::isfinite(startPosition) || !std::isfinite(currentPosition) || !std::isfinite(targetPosition)) {
            return false;
        }
        if (targetPosition <= startPosition) {
            return currentPosition <= (targetPosition + tolerance);
        }
        return currentPosition >= (targetPosition - tolerance);
    };
    auto resolveCompressionTargets = [&](int slaveIdx) {
        DownToOriginCompressionTarget target;
        MotionParamSnapshot snap = CaptureMotionParams(slaveIdx);
        if (snap.haveP9 && std::isfinite(snap.p9)) {
            target.havePositioningPoint = true;
            target.positioningPoint = snap.p9;
        }
        if (snap.haveP13 && std::isfinite(snap.p13)) {
            target.haveCompressionPoint = true;
            target.compressionPoint = snap.p13;
        }
        if (!target.haveCompressionPoint && target.havePositioningPoint) {
            target.haveCompressionPoint = true;
            target.compressionPoint = target.positioningPoint;
        }
        if (!target.havePositioningPoint && target.haveCompressionPoint) {
            target.havePositioningPoint = true;
            target.positioningPoint = target.compressionPoint;
        }
        if (target.havePositioningPoint && target.haveCompressionPoint) {
            const bool positioningBeforeCompression =
                std::fabs(target.positioningPoint - target.compressionPoint) <= 1e-6 ||
                target.positioningPoint >= target.compressionPoint;
            if (!positioningBeforeCompression) {
                target.positioningPoint = target.compressionPoint;
            }
        }
        return target;
    };
    auto cleanupJoggers = [&]() {
        int stoppedCount = 0;
        for (int s = 1; s <= slaveCount; ++s) {
            if (jogStarted[s] && !jogStopped[s]) {
                if (PreparePDOForSlave(s)) {
                    MotionResult stopRes = motionController->JogStop(s);
                    // 只在失敗時記錄
                    if (!stopRes.success) {
                        PostUTF8LogToUI(CoreHelpers::WideToUtf8(stopRes.message));
                    } else {
                        stoppedCount++;
                    }
                }
                jogStopped[s] = true;
            }
        }
        if (stoppedCount > 0) {
            PostUTF8LogToUI(std::string("[AutoDownToOrigin] Cleanup: 停止 ") + std::to_string(stoppedCount) + " 個從站的 jog\n");
        }
    };
    auto failWide = [&](const std::wstring &msg) {
        PostUTF8LogToUI(CoreHelpers::WideToUtf8(msg + L"\n"));
        cleanupJoggers();
        return false;
    };
    auto failUtf8 = [&](const std::string &msg) {
        if (!msg.empty() && msg.back() != '\n') PostUTF8LogToUI(msg + "\n");
        else PostUTF8LogToUI(msg);
        cleanupJoggers();
        return false;
    };

    for (int s = 1; s <= slaveCount; ++s) {
        if (this->autoDownToOriginAbortRequested.load()) {
            PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop before start\n"));
            cleanupJoggers();
            return false;
        }
        compressionTargets[s] = resolveCompressionTargets(s);
        double initialPosition = 0.0;
        if (TryGetMechanicalActualPosition(s, initialPosition)) {
            haveStartPosition[s] = true;
            startPositions[s] = initialPosition;
        }
        if (!PreparePDOForSlave(s)) {
            std::wstringstream ss; ss << L"[AutoDownToOrigin] Failed to prepare PDO mapping for slave " << s;
            return failWide(ss.str());
        }
        uint32_t staged = ResolveJogVelocityCommand(s, false);
        MotionResult startRes = motionController->JogNegativeStart(s, staged);
        // 只在失敗時記錄，減少日誌輸出頻率
        if (!startRes.success) {
            PostUTF8LogToUI(CoreHelpers::WideToUtf8(startRes.message));
            return failUtf8("[AutoDownToOrigin] Jog CCW start failed\n");
        }
        jogStarted[s] = true;
    }

    Sleep(500); // brief delay to allow jog to initiate

    const double SPEED_THRESHOLD = -2.5;
    const double POSITION_TOLERANCE = 0.5;
    const DWORD SPEED_POLL_INTERVAL_MS = 20;  // 20ms reduces CPU load vs 10ms; still fast enough for speed check
    const DWORD SPEED_TIMEOUT_MS = 30000;
    DWORD waitStart = GetTickCount();
    while (true) {
        bool allReleased = true;
        for (int s = 1; s <= slaveCount; ++s) {
            if (!jogStarted[s] || jogStopped[s]) continue;
            allReleased = false;
            bool stopByPosition = false;
            double actualPosition = 0.0;
            if (TryGetMechanicalActualPosition(s, actualPosition)) {
                if (!haveStartPosition[s]) {
                    haveStartPosition[s] = true;
                    startPositions[s] = actualPosition;
                }
                const auto &target = compressionTargets[s];
                if (haveStartPosition[s] && target.havePositioningPoint && !positioningPointReached[s] &&
                    hasReachedMechanicalTarget(startPositions[s], actualPosition, target.positioningPoint, POSITION_TOLERANCE)) {
                    positioningPointReached[s] = true;
                    if (target.haveCompressionPoint &&
                        std::fabs(target.compressionPoint - target.positioningPoint) > POSITION_TOLERANCE) {
                        std::ostringstream oss;
                        oss << "[AutoDownToOrigin] Slave " << s
                            << " reached positioning point " << std::fixed << std::setprecision(4) << target.positioningPoint
                            << "; simulating compression toward " << target.compressionPoint << "\n";
                        PostUTF8LogToUI(oss.str());
                    }
                }
                if (haveStartPosition[s] && target.haveCompressionPoint &&
                    hasReachedMechanicalTarget(startPositions[s], actualPosition, target.compressionPoint, POSITION_TOLERANCE)) {
                    compressionPointReached[s] = true;
                    stopByPosition = true;
                }
            }
            double speed = 0;
            // Ensure we only compare speed if the read succeeded.
            const bool speedReady = TryGetMechanicalActualSpeed(s, speed);
            const bool stopBySpeed = speedReady && speed >= SPEED_THRESHOLD;
            if (stopByPosition || stopBySpeed) {
                if (!PreparePDOForSlave(s)) return failWide(L"[AutoDownToOrigin] Failed to prepare PDO before jog stop");
                MotionResult stopRes = motionController->JogStop(s);
                if (!stopRes.success) {
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(stopRes.message));
                } else if (stopByPosition) {
                    const auto &target = compressionTargets[s];
                    std::ostringstream oss;
                    oss << "[AutoDownToOrigin] Slave " << s
                        << " completed simulated compression at " << std::fixed << std::setprecision(4)
                        << (target.haveCompressionPoint ? target.compressionPoint : actualPosition)
                        << " (actual=" << actualPosition << ")\n";
                    PostUTF8LogToUI(oss.str());
                } else {
                    std::ostringstream oss;
                    oss << "[AutoDownToOrigin] Slave " << s
                        << " released by speed threshold (actualSpeed=" << std::fixed << std::setprecision(4)
                        << speed << ")\n";
                    PostUTF8LogToUI(oss.str());
                }
                jogStopped[s] = true;
            }
        }
        if (allReleased) break;
        if (this->autoDownToOriginAbortRequested.load()) {
            PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop during jog release wait\n"));
            cleanupJoggers();
            return false;
        }
        // If any slave reports an error, force an immediate stop and abort
                if (this->anySlaveHasError.load()) {
            PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by slave error during jog release wait\n"));
            if (this->motionController) {
                MotionResult mr = this->motionController->SmoothStopAll(0.25);
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr.message));
                if (!mr.success) {
                    MotionResult mr2 = this->motionController->ImmediateStopAll();
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr2.message));
                }
            }
            cleanupJoggers();
            return false;
        }
        if (GetTickCount() - waitStart > SPEED_TIMEOUT_MS) {
            PostUTF8LogToUI(std::string("[AutoDownToOrigin] Timeout while waiting for jog to settle; forcing stop\n"));
            cleanupJoggers();
            break;
        }
        Sleep(SPEED_POLL_INTERVAL_MS);
    }

    // Power off all slaves before homing (sequential, deterministic for RT cycle stability)
    PostUTF8LogToUI(std::string("[AutoDownToOrigin] Powering off all slaves before homing (sequential)\n"));
    {
        int successCount = 0;
        int failCount = 0;
        for (int s = 1; s <= slaveCount; ++s) {
            if (this->autoDownToOriginAbortRequested.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop during power-off\n"));
                cleanupJoggers();
                return false;
            }
            if (this->anySlaveHasError.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by slave error during power-off\n"));
                if (this->motionController) {
                    MotionResult mr = this->motionController->SmoothStopAll(0.25);
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr.message));
                    if (!mr.success) {
                        MotionResult mr2 = this->motionController->ImmediateStopAll();
                        PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr2.message));
                    }
                }
                cleanupJoggers();
                return false;
            }

            MotionResult r = motionController->PowerOffAsync(s, true);
            if (r.success) {
                successCount++;
            } else {
                failCount++;
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(r.message));
            }
        }

        std::string summary = "[AutoDownToOrigin] PowerOff 完成: 成功=" + std::to_string(successCount) +
                             ", 失敗=" + std::to_string(failCount) + "\n";
        PostUTF8LogToUI(summary);
    }

    // Homing is intentionally sequential to avoid RT cycle spikes from concurrent SDO workers.
    const bool parallelHome = false;
    if (!parallelHome) {
        int homingSuccess = 0;
        int homingFail = 0;
        for (int s = 1; s <= slaveCount; ++s) {
            if (this->autoDownToOriginAbortRequested.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop before homing\n"));
                cleanupJoggers();
                return false;
            }
            if (this->anySlaveHasError.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by slave error before homing\n"));
                if (this->motionController) {
                    MotionResult mr = this->motionController->ImmediateStopAll();
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr.message));
                }
                cleanupJoggers();
                return false;
            }
            if (!PreparePDOForSlave(s)) {
                std::wstringstream ss; ss << L"[AutoDownToOrigin] Failed to prepare PDO mapping before homing slave " << s;
                return failWide(ss.str());
            }
            std::wstring err;
            if (!ValidateHomePreconditions(s, err)) {
                return failWide(err);
            }
            bool have607c = false;
            uint64_t v607c = 0;
            if (!ComputeHomeCommandValue(s, have607c, v607c)) {
                return failUtf8("[AutoDownToOrigin] Failed to compute 0x607C value\n");
            }
            MotionResult homeRes = motionController->HomeAsync(s, true, have607c, v607c);
            if (homeRes.success) {
                homingSuccess++;
            } else {
                homingFail++;
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(homeRes.message));
                return failUtf8("[AutoDownToOrigin] Homing failed\n");
            }
        }
        std::string summary = "[AutoDownToOrigin] Homing 完成: 成功=" + std::to_string(homingSuccess) +
                             ", 失敗=" + std::to_string(homingFail) + "\n";
        PostUTF8LogToUI(summary);
    } else {
        std::vector<std::future<MotionResult>> futures;
        futures.reserve(slaveCount);
        for (int s = 1; s <= slaveCount; ++s) {
            if (this->autoDownToOriginAbortRequested.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop before homing\n"));
                cleanupJoggers();
                return false;
            }
            if (!PreparePDOForSlave(s)) {
                std::wstringstream ss; ss << L"[AutoDownToOrigin] Failed to prepare PDO mapping before homing slave " << s;
                return failWide(ss.str());
            }
            std::wstring err;
            if (!ValidateHomePreconditions(s, err)) {
                return failWide(err);
            }
            bool have607c = false;
            uint64_t v607c = 0;
            if (!ComputeHomeCommandValue(s, have607c, v607c)) {
                return failUtf8("[AutoDownToOrigin] Failed to compute 0x607C value\n");
            }

            int slaveLocal = s;
            bool hv = have607c;
            uint64_t valLocal = v607c;
            futures.push_back(std::async(std::launch::async, [this, slaveLocal, hv, valLocal]() {
                MotionResult r = this->motionController->HomeAsync(slaveLocal, true, hv, (uint32_t)valLocal);
                return r;
            }));
        }

        // Wait and check for errors
        int homingSuccess = 0;
        int homingFail = 0;
        for (auto &fut : futures) {
            if (this->autoDownToOriginAbortRequested.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by Immediate Stop while waiting for homing\n"));
                cleanupJoggers();
                return false;
            }
            if (this->anySlaveHasError.load()) {
                PostUTF8LogToUI(std::string("[AutoDownToOrigin] Aborted by slave error while waiting for homing\n"));
                if (this->motionController) {
                    MotionResult mr = this->motionController->ImmediateStopAll();
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(mr.message));
                }
                cleanupJoggers();
                return false;
            }
            MotionResult r = fut.get();
            if (r.success) {
                homingSuccess++;
            } else {
                homingFail++;
                // 只記錄失敗的詳細訊息
                PostUTF8LogToUI(CoreHelpers::WideToUtf8(r.message));
            }
        }
        // 統計結果
        std::string summary = "[AutoDownToOrigin] Homing 完成: 成功=" + std::to_string(homingSuccess) + 
                             ", 失敗=" + std::to_string(homingFail) + "\n";
        PostUTF8LogToUI(summary);
        if (homingFail > 0) {
            cleanupJoggers();
            return false;
        }
    }

    PostUTF8LogToUI(std::string("[AutoDownToOrigin] Completed jog reverse and homing for all slaves\n"));
    // Clear abort flag if we completed normally
    this->autoDownToOriginAbortRequested.store(false);
    return true;
}

// -----------------------------------------------------------------------------
// Mailbox send logic (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------

void MainWindow::StartJogPositiveUI() {
    if (!StartJogPreamble()) return;
    // Do NOT disable the jog buttons here; keeping them enabled ensures
    // the WM_LBUTTONUP is delivered so the release handler can stop the jog.
    if (motionController) {
        PrependStatus(L"[Jog] Starting jog positive sequence...\r\n");
        uint32_t stagedVal = ResolveJogVelocityCommand(confirmedSlave, true);
        MotionResult mr = motionController->JogPositiveStart(confirmedSlave, stagedVal);
        PrependStatus(mr.message.c_str());
        // If start succeeded, only mark the button as pressed when the user
        // is still holding it. If the user released while start was running
        // (jogPressRequested == false), stop immediately.
        if (mr.success) {
            if (jogPressRequested.load()) {
                PrependStatus(L"[Jog] MotionController reported start success\r\n");
                jogButtonPressed.store(true);
            } else {
                PrependStatus(L"[Jog] Start succeeded but user already released; stopping immediately\r\n");
                MotionResult stopMr = motionController->JogStop(confirmedSlave);
                PrependStatus(stopMr.message.c_str());
                // Clear state and restore UI
                SetBusyFlagAndNotify(jogBusy, false);
                jogButtonPressed.store(false);
                jogPressRequested.store(false);
                KillTimer(hwnd, TIMER_JOG_POLL);
                SetControlEnabled(hBtnJogCW, true);
                SetControlEnabled(hBtnJogCCW, true);
                PrependStatus(L"[Jog] Jog cancelled after start due to release\r\n");
                return;
            }
        } else {
            PrependStatus(L"[Jog] MotionController reported start failure\r\n");
            // clear request on failure
            jogPressRequested.store(false);
            SetBusyFlagAndNotify(jogBusy, false);
            KillTimer(hwnd, TIMER_JOG_POLL);
            SetControlEnabled(hBtnJogCW, true);
            SetControlEnabled(hBtnJogCCW, true);
            return;
        }
    }
    SetTimer(hwnd, TIMER_JOG_POLL, TIMER_JOG_POLL_INTERVAL_MS, NULL);
}


void MainWindow::StartJogNegativeUI() {
    if (!StartJogPreamble()) return;
    // Keep pressed button enabled so release is delivered
    if (motionController) {
        PrependStatus(L"[Jog] Starting jog negative sequence...\r\n");
        uint32_t stagedVal = ResolveJogVelocityCommand(confirmedSlave, false);
        MotionResult mr = motionController->JogNegativeStart(confirmedSlave, stagedVal);
        PrependStatus(mr.message.c_str());
        if (mr.success) {
            if (jogPressRequested.load()) {
                PrependStatus(L"[Jog] MotionController reported negative start success\r\n");
                jogButtonPressed.store(true);
            } else {
                PrependStatus(L"[Jog] Negative start succeeded but user already released; stopping immediately\r\n");
                MotionResult stopMr = motionController->JogStop(confirmedSlave);
                PrependStatus(stopMr.message.c_str());
                SetBusyFlagAndNotify(jogBusy, false);
                jogButtonPressed.store(false);
                jogPressRequested.store(false);
                KillTimer(hwnd, TIMER_JOG_POLL);
                SetControlEnabled(hBtnJogCW, true);
                SetControlEnabled(hBtnJogCCW, true);
                PrependStatus(L"[Jog] Jog cancelled after negative start due to release\r\n");
                return;
            }
        } else {
            PrependStatus(L"[Jog] MotionController reported negative start failure\r\n");
            jogPressRequested.store(false);
            SetBusyFlagAndNotify(jogBusy, false);
            KillTimer(hwnd, TIMER_JOG_POLL);
            SetControlEnabled(hBtnJogCW, true);
            SetControlEnabled(hBtnJogCCW, true);
            return;
        }
    }
    SetTimer(hwnd, TIMER_JOG_POLL, TIMER_JOG_POLL_INTERVAL_MS, NULL);
}

bool MainWindow::StartJogPreamble() {
    // Check if another jog operation is already in progress
    if (jogBusy.load()) return false;
    
    // Check mandatory preconditions: master must be initialized and slave selected
    if (!IsMasterInitialized() || confirmedSlave <= 0) return false;
    if (IsManualResetRequired()) {
        PrependStatus(L"[Jog] 已停止且需先異常重置，無法執行寸動\r\n");
        return false;
    }
    
    // Mark a pending user press request so release can cancel an in-progress start
    jogPressRequested.store(true);
    SetBusyFlagAndNotify(jogBusy, true);
    
    return true;
}

void MainWindow::StopJogUI() {
    // Allow Stop to be called even if start was in-progress (jogBusy may be true)
    if (!jogBusy.load() && !jogPressRequested.load()) return;
    PrependStatus(L"[Jog] Stopping jog sequence...\r\n");
    if (motionController) {
        MotionResult mr = motionController->JogStop(confirmedSlave);
        PrependStatus(mr.message.c_str());
    }
    SetBusyFlagAndNotify(jogBusy, false);
    // clear pressed/request flags when we stop
    jogButtonPressed.store(false);
    jogPressRequested.store(false);
    KillTimer(hwnd, TIMER_JOG_POLL);
    SetControlEnabled(hBtnJogCW, true);
    SetControlEnabled(hBtnJogCCW, true);
    PrependStatus(L"[Jog] Jog stopped and UI restored\r\n");
}


bool MainWindow::PreparePDOForSlave(int slaveIdx) {
    if (slaveIdx <= 0) return false;
    if (!hwnd) return false;
    if (GetCurrentThreadId() == uiThreadId) {
        auto* master = AppController::GetInstance().GetMaster();
        return (master && pdoDisplay.RecalculateOffsetsFromSDO(master, slaveIdx));
    }
    auto req = new PreparePdoRequest();
    req->slaveIdx = slaveIdx;
    auto fut = req->completion.get_future();
    if (!PostMessageW(hwnd, WM_USER_PDO_PREPARE_FOR_SLAVE, 0, reinterpret_cast<LPARAM>(req))) {
        delete req;
        return false;
    }
    try {
        return fut.get();
    } catch (...) {
        return false;
    }
}


void MainWindow::OnJogButtonPressed()
{
    PrependStatus(L"[Jog] Button pressed (OnJogButtonPressed)\r\n");
    // Only attempt to start if not already busy and we have a selected slave and master
    if (jogBusy.load()) {
        PrependStatus(L"[Jog] Ignored press: jog already busy\r\n");
        return;
    }
    if (!IsMasterInitialized() || confirmedSlave <= 0) {
        PrependStatus(L"[Jog] Ignored press: master not initialized or no slave selected\r\n");
        return;
    }
    // Mark that a press has been requested so a subsequent release can cancel
    // the start if it hasn't completed yet.
    jogPressRequested.store(true);
    AppController::GetInstance().StartJogPositive();
}


void MainWindow::OnJogButtonReleased()
{
    PrependStatus(L"[Jog] Button released (OnJogButtonReleased)\r\n");
    // If the user released while a start was still in progress (request exists
    // but jogButtonPressed not yet set), cancel the pending start and stop
    // any partial activity. This allows a release to interrupt a long
    // start operation.
    if (jogPressRequested.load() && !jogButtonPressed.load()) {
        PrependStatus(L"[Jog] Release received during pending start: cancelling request and stopping if needed\r\n");
        // Clear the request immediately so Start handlers see it
        jogPressRequested.store(false);
        if (jogBusy.load()) {
            StopJogUI();
        }
        return;
    }

    // Only act on release when the button was the one to start the jog and jog is active
    if (!jogButtonPressed.load()) {
        PrependStatus(L"[Jog] Release ignored: no prior press registered\r\n");
        return;
    }
    if (!jogBusy.load()) {
        PrependStatus(L"[Jog] Release ignored: jog not active\r\n");
        jogButtonPressed.store(false);
        return;
    }
    AppController::GetInstance().StopJog();
}

// Direction-specific handlers

void MainWindow::OnJogCWPressed()
{
    PrependStatus(L"[Jog] CW pressed: disabling CCW button and starting CW jog\r\n");
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, false);
    // Mark pending press and start positive jog
    jogPressRequested.store(true);
    AppController::GetInstance().StartJogPositive();
}


void MainWindow::OnJogCWReleased()
{
    PrependStatus(L"[Jog] CW released: stopping CW jog and re-enabling CCW button\r\n");
    AppController::GetInstance().StopJog();
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, true);
}


void MainWindow::OnJogCCWPressed()
{
    PrependStatus(L"[Jog] CCW pressed: disabling CW button and starting CCW jog\r\n");
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, false);
    // Mirror positive start UI behavior: use StartJogNegativeUI wrapper
    // Mark pending press and start negative jog
    jogPressRequested.store(true);
    AppController::GetInstance().StartJogNegative();
}


void MainWindow::OnJogCCWReleased()
{
    PrependStatus(L"[Jog] CCW released: stopping CCW jog and re-enabling CW button\r\n");
    AppController::GetInstance().StopJog();
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, true);
}




// OnSendMailbox moved to MainWindow.Logic.cpp



