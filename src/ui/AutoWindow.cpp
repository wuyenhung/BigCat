// AutoWindow.cpp - 簡要說明:
// 自主操作視窗 UI 與狀態顯示邏輯，包含急停/啟動/原點設定等按鈕狀態、MMF 監測與背景更新執行緒。

#include "../include/AutoWindow.h"
#include "../include/MainWindow.h"
#include "../include/EtherCATMaster.h"
#include "../include/MmfPose6DOrder.h"
#include "../include/core/SystemState.h"
#include "../include/core/CpuIsolation.h"
#include "../app/AppController.h"
#include <windows.h>
#include <cstdint>
#include "../include/PDOModel.h"
#include <vector>
#include <array>
#include <commctrl.h>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <atomic>
#include <thread>
// NOTE: avoid using std::max when windows.h may define max macro; use ternary.

static const wchar_t AUTO_WINDOW_CLASS[] = L"AutoWindowClass_v1";

// Custom colors for emergency stop button depending on bit5 of SDO 0x60FD
// Bright red when bit5 == 0, dark red when bit5 == 1.
static HBRUSH g_brushBrightRed = NULL;
static HBRUSH g_brushDarkRed = NULL;
static HBRUSH g_brushBrightYellow = NULL;
static HBRUSH g_brushDarkYellow = NULL;
static HBRUSH g_brushBrightBlue = NULL;
static HBRUSH g_brushDarkBlue = NULL;
static HBRUSH g_brushBrightGreen = NULL;
static HBRUSH g_brushDarkGreen = NULL;
// fault-reset override state: when true, the AutoWindow fault-reset button
// is painted bright yellow and disabled until completion message arrives.
static bool g_faultResetOverrideActive = false;
static bool g_autoConnectOverrideActive = false;
// Cache last observed bit to avoid unnecessary repaints
static int g_lastEmergBit5 = -1;
static int g_lastBit0 = 0; // Immediate Stop - 初始為0（暗紅色），避免程式啟動時亮紅色
static int g_lastBit1 = -1; // Connect/Run
static int g_lastBit3 = -1; // Home
static int g_lastBit6 = -1; // Fault Reset
static bool g_localImmediateStopLatched = false;
static bool g_localEmergencyStopLatched = false;
static bool g_immediateStopOverrideActive = false;
static bool g_emergencyStopOverrideActive = false;
static bool g_downToOriginOverrideActive = false;
static bool g_pendingDownToOriginAfterStop = false;  // 等待立即停止完成後執行到下原點
// Track whether immediate/emergency stop buttons are currently pressed
static bool g_immediateStopPressed = false;
static bool g_emergencyStopPressed = false;
// True while a stop action is in progress (from press/request until DONE)
static bool g_stopInProgress = false;
// When true, AutoWindow buttons are globally disabled (e.g. auto procedure not started or stopped).
// Initial value is true because auto procedure is not running at startup.
static bool g_autoDisabledActive = true;
// MMF diagnostic state (moved from MainWindow)
static bool g_mmfPreviouslyMapped = false;
static bool g_mmfFirstSnapshotLogged = false;
// 快取上次更新的值，避免不必要的重繪
static std::array<float, 6> g_lastMMFValues = {0};
static std::array<std::wstring, 6> g_lastMmfDisplayTexts;
static std::array<std::wstring, 6> g_lastActualPositions;
static std::array<std::wstring, 6> g_lastErrorCodeTexts;
static bool g_mmfNonZeroLogged = false;
static unsigned long long g_mmfZeroLogLastTick = 0;
// Background brush owned by AutoWindow TU when matching main-window dark color
static HBRUSH g_autoWindowBgBrush = NULL;
// Smaller font used for text inside square status lamps
static HFONT g_statusLampFont = NULL;
// ImageList used to set MMF ListView row height
static HIMAGELIST g_mmfImageList = NULL;

// AutoWindow 全域狀態旗標（暫存自主視窗回報）
static bool g_oEtherCatOk = false;
static bool g_oBusy = false;
static bool g_oError = false;
static bool g_oManualResetRequired = false;
static bool g_oReady = false;
static bool g_oHomeDone = false;
static bool g_oOnline = false;

// Background status update thread to avoid UI blocking
static std::thread g_statusUpdateThread;
static std::atomic<bool> g_statusUpdateThreadRunning(false);
static std::atomic<bool> g_statusUpdateThreadStop(false);
static HWND g_statusUpdateTargetWindow = NULL;
static HANDLE g_statusUpdateWakeEvent = NULL;
static CoreHelpers::EventSubscription g_statusUpdateSubscription;
static UINT g_autoWindowRefreshTimerMs = 50;
static constexpr UINT kAutoWindowActiveRefreshMs = 50;
static constexpr UINT kAutoWindowIdleRefreshMs = 200;
static constexpr UINT kAutoWindowMinimizedRefreshMs = 250;
// Background -> UI: pending timer actions bitmask. bit N set => request SetTimer/KillTimer for timer N
static std::atomic<int> g_pendingSetTimersMask{0};
// Keep the previous values so we can detect changes and trigger actions
// previous input flags removed (iReset/iGoHome/iOnline/iStop/iEstop removed)
// Track whether owner previously reported busy; used to detect owner-side busy->not-busy transition
static bool g_prev_owner_oBusy = false;
// Track whether owner previously reported error; if owner reports an error we should request immediate stop
static bool g_prev_oError = false;
// Track whether AutoWindow is minimized so we can pause UI updates while minimized
static std::atomic<bool> g_autoWindowIsMinimized(false);

static bool IsPose6DMmfInputModeActive()
{
    return AppController::GetInstance().GetMmf1InputMode() == Mmf1InputMode::Pose6D;
}

static const wchar_t* GetAutoWindowMmfInputHeaderText()
{
    return IsPose6DMmfInputModeActive() ? L"6DOF座標值" : L"輸入角度";
}

static void FormatAutoWindowMmfInputValue(int index, float value, wchar_t* buffer, size_t bufferCount)
{
    if (!buffer || bufferCount == 0) {
        return;
    }

    if (IsPose6DMmfInputModeActive()) {
        static const wchar_t* kPoseNames[6] = {L"Sway", L"Surge", L"Heave", L"Pitch", L"Roll", L"FYaw"};
        const wchar_t* name = (index >= 0 && index < 6) ? kPoseNames[index] : L"N/A";
        const wchar_t* unit = (index >= 0 && index < 3) ? L"mm" : L"deg";
        swprintf_s(buffer, bufferCount, L"%s=%.4f %s", name, static_cast<double>(value), unit);
        return;
    }

    swprintf_s(buffer, bufferCount, L"%.4f deg", static_cast<double>(value));
}

static void FormatAutoWindowMmfInputUnavailableValue(int index, wchar_t* buffer, size_t bufferCount)
{
    if (!buffer || bufferCount == 0) {
        return;
    }

    if (IsPose6DMmfInputModeActive()) {
        static const wchar_t* kPoseNames[6] = {L"Sway", L"Surge", L"Heave", L"Pitch", L"Roll", L"FYaw"};
        const wchar_t* name = (index >= 0 && index < 6) ? kPoseNames[index] : L"N/A";
        const wchar_t* unit = (index >= 0 && index < 3) ? L"mm" : L"deg";
        swprintf_s(buffer, bufferCount, L"%s=N/A %s", name, unit);
        return;
    }

    wcscpy_s(buffer, bufferCount, L"N/A");
}

static void FormatAutoWindowActualValue(int slaveIndex,
                                        const wchar_t* valueText,
                                        wchar_t* buffer,
                                        size_t bufferCount)
{
    if (!buffer || bufferCount == 0) {
        return;
    }

    const wchar_t* text = valueText ? valueText : L"N/A";
    swprintf_s(buffer, bufferCount, L"軸%d=%s", slaveIndex, text);
}

static void FormatAutoWindowErrorCodeValue(bool valid,
                                           uint16_t errorCode,
                                           wchar_t* buffer,
                                           size_t bufferCount)
{
    if (!buffer || bufferCount == 0) {
        return;
    }

    if (!valid) {
        wcscpy_s(buffer, bufferCount, L"N/A");
        return;
    }

    if (errorCode == 0xFFFF) {
        wcscpy_s(buffer, bufferCount, L"N/A");
        return;
    }

    swprintf_s(buffer, bufferCount, L"0x%04X", static_cast<unsigned>(errorCode));
}

static void ComputeAutoWindowMmfColumnWidths(int listWidth,
                                             int& actualWidth,
                                             int& inputWidth,
                                             int& errorCodeWidth)
{
    if (listWidth < 240) {
        listWidth = 240;
    }

    errorCodeWidth = listWidth * 30 / 100;
    if (errorCodeWidth < 100) {
        errorCodeWidth = 100;
    }
    if (errorCodeWidth > listWidth / 3) {
        errorCodeWidth = listWidth / 3;
    }

    const int remainingWidth = listWidth - errorCodeWidth - 2;
    actualWidth = listWidth * 30 / 100;
    if (actualWidth > remainingWidth / 2) {
        actualWidth = remainingWidth / 2;
    }
    inputWidth = remainingWidth - actualWidth;
}

static void UpdateAutoWindowMmfInputColumnHeader(HWND hList)
{
    if (!hList || !IsWindow(hList)) {
        return;
    }

    LVCOLUMNW column;
    ZeroMemory(&column, sizeof(column));
    column.mask = LVCF_TEXT;
    column.pszText = const_cast<LPWSTR>(GetAutoWindowMmfInputHeaderText());
    ListView_SetColumn(hList, 1, &column);
}

static CoreHelpers::SystemStateSnapshot BuildAutoWindowPolicySnapshot()
{
    CoreHelpers::SystemStateSnapshot snapshot;
    snapshot.ethercat_ok = g_oEtherCatOk;
    snapshot.busy = g_oBusy;
    snapshot.error = g_oError;
    snapshot.manual_reset_required = g_oManualResetRequired;
    snapshot.ready = g_oReady;
    snapshot.home_done = g_oHomeDone;
    snapshot.online = g_oOnline;
    return snapshot;
}

static void InitAutoWindowBrushes() {
    if (!g_brushBrightRed) g_brushBrightRed = CreateSolidBrush(RGB(255, 0, 0));
    if (!g_brushDarkRed)   g_brushDarkRed   = CreateSolidBrush(RGB(120, 0, 0));
    if (!g_brushBrightYellow) g_brushBrightYellow = CreateSolidBrush(RGB(255, 215, 0));
    if (!g_brushDarkYellow)   g_brushDarkYellow   = CreateSolidBrush(RGB(160, 140, 0));
    if (!g_brushBrightBlue)   g_brushBrightBlue   = CreateSolidBrush(RGB(0, 120, 255));
    if (!g_brushDarkBlue)     g_brushDarkBlue     = CreateSolidBrush(RGB(0, 0, 120));
    if (!g_brushBrightGreen)  g_brushBrightGreen  = CreateSolidBrush(RGB(0, 200, 0));
    if (!g_brushDarkGreen)    g_brushDarkGreen    = CreateSolidBrush(RGB(0, 120, 0));
}
// Forward declaration so callers above can use the function before its definition.
static void RecomputeOReadyFromPDO(HWND hwnd);
static void UpdateAllAutoButtons(HWND hwnd);

static void ResetAutoWindowStopState()
{
    g_faultResetOverrideActive = false;
    g_autoConnectOverrideActive = false;
    g_lastEmergBit5 = -1;
    g_lastBit0 = 0;
    g_lastBit1 = -1;
    g_lastBit3 = -1;
    g_lastBit6 = -1;
    g_localImmediateStopLatched = false;
    g_localEmergencyStopLatched = false;
    g_immediateStopOverrideActive = false;
    g_emergencyStopOverrideActive = false;
    g_downToOriginOverrideActive = false;
    g_pendingDownToOriginAfterStop = false;
    g_immediateStopPressed = false;
    g_emergencyStopPressed = false;
    g_stopInProgress = false;
}


// Retrieve the owner HWND recorded for this AutoWindow (if any).
// We store the owner handle as a window property named "AutoWindowOwner" when
// creating the window so the AutoWindow can still communicate with the main
// window even though the AutoWindow itself is a top-level, independent window.
static HWND GetAutoWindowOwner(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return NULL;
    HWND hOwner = (HWND)GetPropW(hwnd, L"AutoWindowOwner");
    if (hOwner && IsWindow(hOwner)) return hOwner;
    // fallback to previous heuristics for safety
    hOwner = GetWindow(hwnd, GW_OWNER);
    if (hOwner && IsWindow(hOwner)) return hOwner;
    hOwner = GetParent(hwnd);
    if (hOwner && IsWindow(hOwner)) return hOwner;
    return NULL;
}

// Helper: post a UTF-8 log message to the main window (owner)
// Debug logging removed: PostAutoWindowDebugLog and in-process debug posts
// were used during development to trace UI timing. These calls were
// intentionally removed for production build to avoid noisy logs.



static bool GetLast60FDLastByte(HWND hwnd, uint8_t &outLast) {
    HWND hOwner = GetAutoWindowOwner(hwnd);
    if (!hOwner) return false;
    auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwner, GWLP_USERDATA));
    if (!win) return false;
    return win->TryGetLastSdo60FDLowByte(outLast);
}

static void LogAutoWindowStopState(HWND hwnd, const char* prefix)
{
    HWND hOwner = GetAutoWindowOwner(hwnd);
    if (!hOwner || !IsWindow(hOwner)) return;

    uint8_t last = 0;
    const bool have60FD = GetLast60FDLastByte(hwnd, last);
    const int extBit0 = have60FD ? (((last & 0x01) != 0) ? 1 : 0) : -1;
    const int extBit5 = have60FD ? (((last & 0x20) == 0) ? 1 : 0) : -1;

    std::ostringstream oss;
    oss << prefix
        << " local_immediate_latched=" << (g_localImmediateStopLatched ? 1 : 0)
        << " local_emergency_latched=" << (g_localEmergencyStopLatched ? 1 : 0)
        << " immediate_override=" << (g_immediateStopOverrideActive ? 1 : 0)
        << " emergency_override=" << (g_emergencyStopOverrideActive ? 1 : 0)
        << " immediate_pressed=" << (g_immediateStopPressed ? 1 : 0)
        << " emergency_pressed=" << (g_emergencyStopPressed ? 1 : 0)
        << " stop_in_progress=" << (g_stopInProgress ? 1 : 0)
        << " ext_bit0=" << extBit0
        << " ext_bit5=" << extBit5;
    if (have60FD) {
        oss << " sdo60FD_last=0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
            << static_cast<int>(last) << std::dec;
    } else {
        oss << " sdo60FD_last=<none>";
    }
    oss << "\n";

    MainWindow::PostUTF8LogToHWND(hOwner, oss.str());
}

struct AutoWindowStopInputState {
    bool external_immediate_stop = false;
    bool external_emergency_stop = false;
};

static AutoWindowStopInputState ReadAutoWindowStopInputState(HWND hwnd)
{
    AutoWindowStopInputState state;
    uint8_t sdo60FD = 0;
    if (GetLast60FDLastByte(hwnd, sdo60FD)) {
        state.external_immediate_stop = (sdo60FD & 0x01) != 0;
        state.external_emergency_stop = (sdo60FD & 0x20) == 0;
    }
    return state;
}

static void ClearAutoWindowLatchedStopState(HWND hwnd)
{
    const auto stopInputs = ReadAutoWindowStopInputState(hwnd);
    if (stopInputs.external_immediate_stop || stopInputs.external_emergency_stop) {
        LogAutoWindowStopState(hwnd, "[AutoWindow] ClearAutoWindowLatchedStopState blocked");
        return;
    }

    LogAutoWindowStopState(hwnd, "[AutoWindow] ClearAutoWindowLatchedStopState before");

    g_immediateStopOverrideActive = false;
    g_emergencyStopOverrideActive = false;
    g_localImmediateStopLatched = false;
    g_localEmergencyStopLatched = false;
    g_immediateStopPressed = false;
    g_emergencyStopPressed = false;
    g_stopInProgress = false;
    g_pendingDownToOriginAfterStop = false;
    g_lastBit0 = 0;
    g_lastEmergBit5 = 1;

    HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP);
    if (hBtn && IsWindow(hBtn)) {
        InvalidateRect(hBtn, NULL, FALSE);
    }
    hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP);
    if (hBtn && IsWindow(hBtn)) {
        InvalidateRect(hBtn, NULL, FALSE);
    }

    LogAutoWindowStopState(hwnd, "[AutoWindow] ClearAutoWindowLatchedStopState after");
}

static bool HasAutoWindowLocalStopActivity()
{
    return g_stopInProgress || g_localImmediateStopLatched || g_localEmergencyStopLatched ||
           g_immediateStopOverrideActive || g_emergencyStopOverrideActive ||
           g_immediateStopPressed || g_emergencyStopPressed;
}

static bool HasAutoWindowStopCondition(const AutoWindowStopInputState& state)
{
    return HasAutoWindowLocalStopActivity() ||
           state.external_immediate_stop ||
           state.external_emergency_stop;
}

static void SetAutoWindowControlEnabled(HWND hwnd, int controlId, bool enabled)
{
    HWND hControl = GetDlgItem(hwnd, controlId);
    if (hControl && IsWindow(hControl)) {
        EnableWindow(hControl, enabled ? TRUE : FALSE);
    }
}

static CoreHelpers::AutoActionButtonInputs BuildAutoWindowActionButtonInputs(HWND hwnd, bool stopActive)
{
    CoreHelpers::AutoActionButtonInputs inputs;
    inputs.action_gate_enabled = !g_autoDisabledActive;
    inputs.stop_active = stopActive;
    inputs.auto_connect_override_active = g_autoConnectOverrideActive;
    inputs.down_to_origin_override_active = g_downToOriginOverrideActive;
    inputs.fault_reset_override_active = g_faultResetOverrideActive;
    inputs.auto_connect_signal_active = (g_lastBit1 == 1);
    inputs.down_to_origin_signal_active = (g_lastBit3 == 1);
    inputs.fault_reset_signal_active = (g_lastBit6 == 1);
    inputs.immediate_stop_signal_active = (g_lastBit0 == 1);
    inputs.emergency_stop_signal_active = (g_lastEmergBit5 == 0);
    inputs.immediate_stop_latched = g_immediateStopPressed ||
                                    g_localImmediateStopLatched ||
                                    g_immediateStopOverrideActive;
    inputs.emergency_stop_latched = g_localEmergencyStopLatched ||
                                    g_emergencyStopOverrideActive;

    HWND hOwner = GetAutoWindowOwner(hwnd);
    if (hOwner && IsWindow(hOwner)) {
        auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwner, GWLP_USERDATA));
        if (win) {
            const auto ownerInputs = win->GetAutoActionButtonInputsForWeb();
            inputs.auto_connect_signal_active = inputs.auto_connect_signal_active ||
                                                ownerInputs.auto_connect_signal_active;
            inputs.down_to_origin_signal_active = inputs.down_to_origin_signal_active ||
                                                  ownerInputs.down_to_origin_signal_active;
            inputs.fault_reset_signal_active = inputs.fault_reset_signal_active ||
                                               ownerInputs.fault_reset_signal_active;
            inputs.immediate_stop_signal_active = inputs.immediate_stop_signal_active ||
                                                  ownerInputs.immediate_stop_signal_active;
            inputs.emergency_stop_signal_active = inputs.emergency_stop_signal_active ||
                                                  ownerInputs.emergency_stop_signal_active;
            inputs.immediate_stop_latched = inputs.immediate_stop_latched ||
                                            ownerInputs.immediate_stop_latched;
            inputs.emergency_stop_latched = inputs.emergency_stop_latched ||
                                            ownerInputs.emergency_stop_latched;
            inputs.stop_active = inputs.stop_active || ownerInputs.stop_active;
        }
    }

    return inputs;
}

static CoreHelpers::AutoActionButtonState BuildAutoWindowActionButtonState(HWND hwnd)
{
    const auto stopInputs = ReadAutoWindowStopInputState(hwnd);
    const bool stopActive = HasAutoWindowStopCondition(stopInputs);
    return CoreHelpers::EvaluateAutoActionButtons(
        BuildAutoWindowPolicySnapshot(),
        BuildAutoWindowActionButtonInputs(hwnd, stopActive));
}

static void SyncAutoWindowActionButtons(HWND hwnd)
{
    if (g_autoConnectOverrideActive && !g_oBusy) {
        g_autoConnectOverrideActive = false;
        KillTimer(hwnd, 5);
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_CONNECT);
        if (hBtn && IsWindow(hBtn)) {
            InvalidateRect(hBtn, NULL, FALSE);
        }
    }
    const auto buttonState = BuildAutoWindowActionButtonState(hwnd);

    SetAutoWindowControlEnabled(hwnd, IDC_BTN_AUTO_CONNECT, buttonState.connect_enabled);
    SetAutoWindowControlEnabled(hwnd, IDC_BTN_AUTO_HOME, buttonState.down_to_origin_enabled);
    SetAutoWindowControlEnabled(hwnd, IDC_BTN_AUTO_FAULT_RESET, buttonState.fault_reset_enabled);
    SetAutoWindowControlEnabled(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP, buttonState.immediate_stop_enabled);
    SetAutoWindowControlEnabled(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP, buttonState.emergency_stop_enabled);
}

static void ClearAutoWindowBusyOnlineState(HWND hwnd)
{
    if (g_oBusy || g_oOnline) {
        g_oBusy = false;
        g_oOnline = false;
        UpdateAllAutoButtons(hwnd);
    }
}

static void BeginAutoWindowStopRequest(HWND hAutoWin,
                                       HWND hOwner,
                                       UINT requestMessage,
                                       LPARAM requestParam,
                                       UINT_PTR timerId)
{
    if (!hOwner || !IsWindow(hOwner)) return;

    PostMessageW(hOwner, requestMessage, (WPARAM)hAutoWin, requestParam);
    ClearAutoWindowBusyOnlineState(hAutoWin);
    g_stopInProgress = true;
    SetAutoWindowControlEnabled(hAutoWin, IDC_BTN_AUTO_CONNECT, false);
    SetAutoWindowControlEnabled(hAutoWin, IDC_BTN_AUTO_HOME, false);
    PostMessageW(hAutoWin, WM_USER + 200, 0, 0);
    KillTimer(hAutoWin, timerId);
    SetTimer(hAutoWin, timerId, 5000, NULL);
}

static void ActivateAutoWindowStopOverride(HWND hwnd,
                                           HWND hAutoWin,
                                           bool& overrideActive,
                                           bool redrawControl,
                                           bool redrawParent)
{
    if (overrideActive) return;

    overrideActive = true;
    InvalidateRect(hwnd, NULL, TRUE);
    if (redrawControl && hwnd && IsWindow(hwnd)) {
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (redrawParent && hAutoWin && IsWindow(hAutoWin)) {
        RedrawWindow(hAutoWin, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

static void ReleaseAutoWindowStopOverride(HWND hwnd,
                                          HWND hAutoWin,
                                          bool& overrideActive,
                                          UINT_PTR timerId,
                                          bool redrawParent)
{
    if (!overrideActive) return;

    overrideActive = false;
    if (hwnd && IsWindow(hwnd)) {
        EnableWindow(hwnd, TRUE);
        InvalidateRect(hwnd, NULL, TRUE);
    }
    if (redrawParent && hAutoWin && IsWindow(hAutoWin)) {
        RedrawWindow(hAutoWin, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    UpdateAllAutoButtons(hAutoWin);
    KillTimer(hAutoWin, timerId);
}

static void CompleteAutoWindowStopRequest(HWND hwnd,
                                          bool& overrideActive,
                                          bool pressed,
                                          int controlId,
                                          UINT_PTR timerId,
                                          LPARAM completionSource)
{
    g_stopInProgress = false;
    if (overrideActive && !pressed) {
        overrideActive = false;
        SetAutoWindowControlEnabled(hwnd, controlId, true);
    }

    if ((int)completionSource != 0) {
        ClearAutoWindowBusyOnlineState(hwnd);
    }

    KillTimer(hwnd, timerId);
    PostMessageW(hwnd, WM_USER + 200, 0, 0);
}

static void BeginAutoWindowAsyncButtonOverride(HWND hwnd,
                                               int controlId,
                                               bool& overrideActive,
                                               UINT_PTR timerId = 0)
{
    HWND hBtn = GetDlgItem(hwnd, controlId);
    overrideActive = true;
    if (hBtn && IsWindow(hBtn)) {
        EnableWindow(hBtn, FALSE);
        InvalidateRect(hBtn, NULL, FALSE);
    }
    if (timerId != 0) {
        KillTimer(hwnd, timerId);
        SetTimer(hwnd, timerId, 5000, NULL);
    }
}

static void CompleteAutoWindowAsyncButtonOverride(HWND hwnd,
                                                  int controlId,
                                                  bool& overrideActive,
                                                  bool syncButtons,
                                                  UINT_PTR timerId = 0)
{
    if (!overrideActive) return;

    overrideActive = false;
    if (syncButtons) {
        SyncAutoWindowActionButtons(hwnd);
    }
    HWND hBtn = GetDlgItem(hwnd, controlId);
    if (hBtn && IsWindow(hBtn)) {
        InvalidateRect(hBtn, NULL, FALSE);
    }
    if (timerId != 0) {
        KillTimer(hwnd, timerId);
    }
}

static void StartAutoWindowDownToOriginRequest(HWND hwnd,
                                               HWND hOwner,
                                               bool clearBusyState)
{
    if (!hOwner || !IsWindow(hOwner)) return;

    BeginAutoWindowAsyncButtonOverride(hwnd,
                                       IDC_BTN_AUTO_HOME,
                                       g_downToOriginOverrideActive);
    if (clearBusyState && g_oBusy) {
        g_oBusy = false;
    }
    if (g_oHomeDone) {
        g_oHomeDone = false;
    }
    if (g_oOnline) {
        g_oOnline = false;
    }
    UpdateAllAutoButtons(hwnd);
    AppController::GetInstance().RequestAutoDownToOrigin(reinterpret_cast<uintptr_t>(hwnd));
}

static void UpdateAllAutoButtons(HWND hwnd) {
    uint8_t last = 0;
    if (!GetLast60FDLastByte(hwnd, last)) {
        ResetAutoWindowStopState();
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
        hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
        hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_CONNECT);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
        hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_HOME);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
        hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_FAULT_RESET);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
        return;
    }

    int b0 = (last & 0x01) ? 1 : 0;
    int b1 = (last & 0x02) ? 1 : 0;
    int b3 = (last & 0x08) ? 1 : 0;
    int b5 = (last & 0x20) ? 1 : 0;
    int b6 = (last & 0x40) ? 1 : 0;

    // Immediate Stop (bit0): 1=bright red, 0=dark red
    if (b0 != g_lastBit0) {
        g_lastBit0 = b0;
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
    }
    // Connect/Run (bit1): 1=bright green, 0=dark green
    if (b1 != g_lastBit1) {
        g_lastBit1 = b1;
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_CONNECT);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
    }
    // Home (bit3): 1=bright blue, 0=dark blue
    if (b3 != g_lastBit3) {
        g_lastBit3 = b3;
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_HOME);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
    }
    // Emergency (bit5): 0=bright red, 1=dark red (note inverted logic)
    if (b5 != g_lastEmergBit5) {
        g_lastEmergBit5 = b5;
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
    }
    // Fault Reset (bit6): 1=bright yellow, 0=dark yellow
    if (b6 != g_lastBit6) {
        g_lastBit6 = b6;
        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_FAULT_RESET);
        if (hBtn) InvalidateRect(hBtn, NULL, FALSE);
    }
}

// Subclass proc for AutoWindow's immediate stop button to trigger action
// on press (WM_LBUTTONDOWN / key-down) instead of release/click.
static LRESULT CALLBACK AutoWinImmediateStopSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    HWND hAutoWin = (HWND)dwRefData;
    HWND hOwner = GetAutoWindowOwner(hAutoWin);
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        // mark pressed state so owner-DONE won't clear override while held
        g_immediateStopPressed = true;
        SetCapture(hwnd);
        // UI override and post request to owner (same as BN_CLICKED previously)
        // Do not disable the control here; keep it enabled so owner-draw
        // painting remains visually bright (some systems render disabled
        // controls differently even after owner-draw). Prevent re-entry
        // by checking the override flag at the top of the handler.
        g_localImmediateStopLatched = true;
        LogAutoWindowStopState(hAutoWin, "[AutoWindow] Immediate stop button pressed");
        ActivateAutoWindowStopOverride(hwnd, hAutoWin, g_immediateStopOverrideActive, true, true);
        if (hOwner && IsWindow(hOwner)) {
            BeginAutoWindowStopRequest(hAutoWin,
                                       hOwner,
                                       WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                                       0,
                                       3);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            // keyboard press: treat as pressed
            g_immediateStopPressed = true;
            g_localImmediateStopLatched = true;
            LogAutoWindowStopState(hAutoWin, "[AutoWindow] Immediate stop key pressed");
            ActivateAutoWindowStopOverride(hwnd, hAutoWin, g_immediateStopOverrideActive, true, false);
            if (hOwner && IsWindow(hOwner)) {
                BeginAutoWindowStopRequest(hAutoWin,
                                           hOwner,
                                           WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                                           0,
                                           3);
                if (hAutoWin && IsWindow(hAutoWin)) RedrawWindow(hAutoWin, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
            }
        }
        break;
    case WM_LBUTTONUP:
    case WM_KEYUP:
        // On release: clear the immediate-stop override and re-enable button
        // so it can be used again immediately if needed
        {
            // mark not pressed
            g_immediateStopPressed = false;
            // 立即停止按鈕永遠保持啟用（緊急情況下必須可用）
            ReleaseAutoWindowStopOverride(hwnd, hAutoWin, g_immediateStopOverrideActive, 3, true);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        break;
    case WM_CAPTURECHANGED:
        g_immediateStopPressed = false;
        ReleaseAutoWindowStopOverride(hwnd, hAutoWin, g_immediateStopOverrideActive, 3, true);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass proc for AutoWindow's emergency stop button (press triggers same action)
static LRESULT CALLBACK AutoWinEmergencyStopSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                         UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    HWND hAutoWin = (HWND)dwRefData;
    HWND hOwner = GetAutoWindowOwner(hAutoWin);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        // mark pressed so owner DONE won't clear while held
        g_emergencyStopPressed = true;
        SetCapture(hwnd);
        // Do not disable the control here to preserve owner-draw appearance
        g_localEmergencyStopLatched = true;
        LogAutoWindowStopState(hAutoWin, "[AutoWindow] Emergency stop button pressed");
        ActivateAutoWindowStopOverride(hwnd, hAutoWin, g_emergencyStopOverrideActive, false, false);
        // Do not set global error here; owner will decide error handling
        UpdateAllAutoButtons(hAutoWin);
        if (hOwner && IsWindow(hOwner)) {
            BeginAutoWindowStopRequest(hAutoWin,
                                       hOwner,
                                       WM_USER_AUTO_EMERGENCY_STOP_REQUEST,
                                       0,
                                       4);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            // keyboard press: treat as pressed
            g_emergencyStopPressed = true;
            g_localEmergencyStopLatched = true;
            LogAutoWindowStopState(hAutoWin, "[AutoWindow] Emergency stop key pressed");
            // Do not disable the control here to preserve owner-draw appearance
            ActivateAutoWindowStopOverride(hwnd, hAutoWin, g_emergencyStopOverrideActive, false, false);
            // Do not set global error here; owner will decide error handling
            UpdateAllAutoButtons(hAutoWin);
            if (hOwner && IsWindow(hOwner)) {
                BeginAutoWindowStopRequest(hAutoWin,
                                           hOwner,
                                           WM_USER_AUTO_EMERGENCY_STOP_REQUEST,
                                           0,
                                           4);
            }
        }
        break;
    case WM_LBUTTONUP:
    case WM_KEYUP:
        // On release: clear the emergency-stop override and re-enable button
        // so it can be used again immediately if needed
        {
            // mark not pressed
            g_emergencyStopPressed = false;
            // 緊急停止按鈕永遠保持啟用（緊急情況下必須可用）
            ReleaseAutoWindowStopOverride(hwnd, hAutoWin, g_emergencyStopOverrideActive, 4, false);
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        break;
    case WM_CAPTURECHANGED:
        g_emergencyStopPressed = false;
        ReleaseAutoWindowStopOverride(hwnd, hAutoWin, g_emergencyStopOverrideActive, 4, false);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass proc for MMF ListView to disable item selection via mouse/keyboard
static LRESULT CALLBACK AutoListViewSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    (void)dwRefData;
    // Ignore left-button clicks/dblclk only when they hit an item (prevent selection).
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK) {
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        LVHITTESTINFO hti; ZeroMemory(&hti, sizeof(hti));
        hti.pt.x = x; hti.pt.y = y;
        int idx = ListView_HitTest(hwnd, &hti);
        // If click landed on an item area, suppress it to avoid selection; otherwise allow default
        if (idx >= 0 && (hti.flags & (LVHT_ONITEM | LVHT_ONITEMICON | LVHT_ONITEMLABEL | LVHT_ONITEMSTATEICON))) {
            return 0;
        }
        // else fallthrough to default handling so scrollbars, header clicks, etc. still work
    }
    // Ignore space/return which might toggle selection via keyboard
    if (uMsg == WM_KEYDOWN) {
        if (wParam == VK_SPACE || wParam == VK_RETURN) return 0;
    }
    // Ignore mouse wheel events so the list does not scroll
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        return 0;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Header subclass to block column resizing and dragging
static LRESULT CALLBACK AutoHeaderSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                               UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass; (void)dwRefData;
    // Block left-button down on header to prevent begin-track/drag
    if (uMsg == WM_LBUTTONDOWN || uMsg == WM_LBUTTONDBLCLK) return 0;
    // Block mouse move while holding left button (dragging)
    if (uMsg == WM_MOUSEMOVE) return 0;
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// 從主視窗取得 AutoWindow 需要的全域狀態旗標（在背景執行緒執行）
static void WakeStatusUpdateThread()
{
    if (g_statusUpdateWakeEvent) {
        SetEvent(g_statusUpdateWakeEvent);
    }
}

static UINT GetAutoWindowRefreshIntervalMs(bool mapped, bool mmfRunning)
{
    if (g_autoWindowIsMinimized.load(std::memory_order_relaxed)) {
        return kAutoWindowMinimizedRefreshMs;
    }
    if (mapped || mmfRunning || g_oBusy || g_oOnline || g_oError) {
        return kAutoWindowActiveRefreshMs;
    }
    return kAutoWindowIdleRefreshMs;
}

static void EnsureAutoWindowRefreshTimer(HWND hwnd, UINT intervalMs)
{
    if (!hwnd || !IsWindow(hwnd) || intervalMs == 0) {
        return;
    }
    if (g_autoWindowRefreshTimerMs == intervalMs) {
        return;
    }
    SetTimer(hwnd, 1, intervalMs, NULL);
    g_autoWindowRefreshTimerMs = intervalMs;
}

static void UpdateGlobalStatusFlags(HWND hwnd)
{
    // This function runs in a background thread and MUST NOT call UI APIs.
    if (!hwnd || !IsWindow(hwnd)) return;
    
    // Verify target window is still set (cleared by StopStatusUpdateThread)
    if (g_statusUpdateTargetWindow != hwnd) return;

    HWND hOwner = GetAutoWindowOwner(hwnd);
    if (!hOwner || !IsWindow(hOwner)) return;
    auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwner, GWLP_USERDATA));
    if (!win) return;

    const bool prevEtherCatOk = g_oEtherCatOk;
    const bool prevBusy = g_oBusy;
    const bool prevError = g_oError;
    const bool prevManualResetRequired = g_oManualResetRequired;
    const bool prevReady = g_oReady;
    const bool prevHomeDone = g_oHomeDone;
    const bool prevOnline = g_oOnline;

    const auto snapshot = win->GetSystemStateSnapshot();

    // Update cached flags (no UI calls here)
    g_oEtherCatOk = snapshot.ethercat_ok;
    if (snapshot.busy) {
        g_prev_owner_oBusy = true;
        g_oBusy = true;
    } else {
        if (g_prev_owner_oBusy) {
            if (g_oBusy) g_oBusy = false;
            g_prev_owner_oBusy = false;
        }
    }
    g_oError = snapshot.error;
    g_oManualResetRequired = snapshot.manual_reset_required;
    g_oReady = snapshot.ready;
    // Mirror owner's homing / online state to local cached flags so
    // the AutoWindow lamps reflect the authoritative server state
    // (this ensures web-initiated flows immediately update the PC lamps).
    g_oHomeDone = snapshot.home_done;
    g_oOnline = snapshot.online;
    RecomputeOReadyFromPDO(hwnd);

    // Ensure homing cleared on error or loss of EtherCAT (no UI calls)
    if (g_oError || g_oManualResetRequired || !g_oEtherCatOk) {
        if (g_oHomeDone) g_oHomeDone = false;
        // Also clear online state when error or EtherCAT disconnected
        if (g_oOnline) g_oOnline = false;
    }

    // If owner reports an error, request immediate stop as a safety fallback.
    if (g_oError != g_prev_oError) {
        if (g_oError) {
            const auto stopInputs = ReadAutoWindowStopInputState(hwnd);
            if (!HasAutoWindowStopCondition(stopInputs)) {
                g_immediateStopOverrideActive = true;
                g_stopInProgress = true;
                PostMessageW(hOwner,
                             WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                             (WPARAM)hwnd,
                             AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
                if (g_oBusy || g_oOnline) { g_oBusy = false; g_oOnline = false; }
                // request UI thread to start fallback timer id=3
                g_pendingSetTimersMask.fetch_or(1 << 3);
            }
        }
    }

    // store previous values
    g_prev_oError = g_oError;

    const bool statusChanged =
        prevEtherCatOk != g_oEtherCatOk ||
        prevBusy != g_oBusy ||
        prevError != g_oError ||
        prevManualResetRequired != g_oManualResetRequired ||
        prevReady != g_oReady ||
        prevHomeDone != g_oHomeDone ||
        prevOnline != g_oOnline;

    // Only wake the UI thread when something actually changed or when the
    // background thread scheduled a timer action that must be applied on the UI thread.
    if ((statusChanged || g_pendingSetTimersMask.load(std::memory_order_relaxed) != 0) &&
        hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_USER + 200, 0, 0);
    }
}

// Background thread function for status updates (non-UI thread)
static void StatusUpdateThreadFunc() {
    // Lower priority to avoid interfering with EtherCAT real-time threads
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    
    // Explicitly avoid Core 4 (reserved for CyclicTask / CpuIsolation).
    {
        constexpr int    kEtherCATTargetCore = 4;
        constexpr DWORD_PTR kEtherCATCoreMask = (DWORD_PTR)1 << kEtherCATTargetCore;
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            DWORD_PTR available = processMask ? processMask : systemMask;
            DWORD_PTR workerMask = available & ~kEtherCATCoreMask;
            if (workerMask) SetThreadAffinityMask(GetCurrentThread(), workerMask);
        }
    }
    
    while (!g_statusUpdateThreadStop.load()) {
        const DWORD waitMs = g_autoWindowIsMinimized.load(std::memory_order_relaxed)
            ? kAutoWindowMinimizedRefreshMs
            : 100;
        if (g_statusUpdateWakeEvent) {
            WaitForSingleObject(g_statusUpdateWakeEvent, waitMs);
        } else {
            Sleep(waitMs);
        }
        if (g_statusUpdateThreadStop.load()) {
            break;
        }
        HWND hwnd = g_statusUpdateTargetWindow;
        if (hwnd && IsWindow(hwnd)) {
            UpdateGlobalStatusFlags(hwnd);
        }
    }
}

// Start background status update thread
static void StartStatusUpdateThread(HWND hwnd) {
    if (g_statusUpdateThreadRunning.load()) return;

    g_statusUpdateTargetWindow = hwnd;
    g_statusUpdateThreadStop.store(false);
    g_statusUpdateThreadRunning.store(true);
    g_statusUpdateWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_statusUpdateSubscription = AppController::GetInstance().SubscribeStatusEvent(
        [](const CoreHelpers::StatusEvent&) {
            WakeStatusUpdateThread();
        });

    g_statusUpdateThread = std::thread(StatusUpdateThreadFunc);
    WakeStatusUpdateThread();
}

// Stop background status update thread
static void StopStatusUpdateThread() {
    if (!g_statusUpdateThreadRunning.load()) return;

    // Clear target window to prevent background thread from posting messages
    g_statusUpdateTargetWindow = NULL;

    g_statusUpdateThreadStop.store(true);
    g_statusUpdateSubscription.Reset();
    WakeStatusUpdateThread();
    if (g_statusUpdateThread.joinable()) {
        g_statusUpdateThread.join();
    }
    g_statusUpdateThreadRunning.store(false);
    if (g_statusUpdateWakeEvent) {
        CloseHandle(g_statusUpdateWakeEvent);
        g_statusUpdateWakeEvent = NULL;
    }
}

// Recompute global oReady by reading PDO snapshot 0x6041 from all slaves.
// If every readable slave reports 0x6041 == 0x1637 then set g_oReady = true,
// otherwise set g_oReady = false. Missing/failed reads are treated as not-ready.
static void RecomputeOReadyFromPDO(HWND hwnd)
{
    // Safety: verify window is still valid
    if (!hwnd || !IsWindow(hwnd)) return;
    
    HWND hOwner = GetAutoWindowOwner(hwnd);
    if (!hOwner || !IsWindow(hOwner)) return;
    auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwner, GWLP_USERDATA));
    if (!win) return;

    int slaveCount = win->MasterGetSlaveCountPublic();
    if (slaveCount <= 0) {
        g_oReady = false;
        return;
    }

    g_oReady = win->AreAllSlavesReadyByStatusword();
}

// Force-hide scrollbars on MMF ListView (called repeatedly to override any system attempts to show them)
static void ForceHideMMFScrollbars(HWND hwnd)
{
    HWND hList = GetDlgItem(hwnd, IDC_LIST_MMF);
    if (hList && IsWindow(hList)) {
        LONG_PTR st = GetWindowLongPtrW(hList, GWL_STYLE);
        st &= ~(WS_VSCROLL | WS_HSCROLL);
        SetWindowLongPtrW(hList, GWL_STYLE, st);
        ShowScrollBar(hList, SB_BOTH, FALSE);
        EnableScrollBar(hList, SB_BOTH, ESB_DISABLE_BOTH);
        SCROLLINFO si; ZeroMemory(&si, sizeof(si)); si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE; si.nMin = 0; si.nMax = 0; si.nPage = 1;
        SetScrollInfo(hList, SB_VERT, &si, FALSE);
        SetScrollInfo(hList, SB_HORZ, &si, FALSE);
    }
}

static void LayoutAutoWindowControls(HWND hwnd)
{
    const int btnW = 90;
        const int btnH = 30; // Height of buttons
    const int gap = 8;
    const int margin = 8; // vertical margin/spacing
    const int sidePad = 2; // 左右預留 2px
    // Status lamps should match buttons' size so they don't look mismatched.
    const int statusWidth = btnW;
    const int statusHeight = btnH;

    // Compute client area and button-group start X so we can center the group
    RECT rcClient; GetClientRect(hwnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientHNow = rcClient.bottom - rcClient.top;
    // button group width computed later using effectiveGap
    // Available width for buttons (excluding side padding)
    int availableWidth = clientW - sidePad * 2;
    if (availableWidth < 0) availableWidth = 0;

    // If available width is smaller than default (btnW*3 + 2*gap), reduce gap
    int effectiveGap = gap;
    int minRequired = btnW * 3;
    if (availableWidth <= minRequired) {
        // Not enough room for gaps; set gaps to zero (buttons will pack)
        effectiveGap = 0;
    } else {
        // Distribute remaining space into two gaps, but don't exceed original gap
        int extra = availableWidth - minRequired;
        int perGap = extra / 2;
        if (perGap < 0) perGap = 0;
        if (perGap < effectiveGap) effectiveGap = perGap;
        // clamp to original gap
        if (effectiveGap > gap) effectiveGap = gap;
    }

    int totalBtnWidth = btnW * 3 + effectiveGap * 2;
    int desiredStart = (clientW - totalBtnWidth) / 2;
    int minStart = sidePad;
    int maxStart = clientW - sidePad - totalBtnWidth;
    if (maxStart < minStart) maxStart = minStart;
    if (desiredStart < minStart) desiredStart = minStart;
    if (desiredStart > maxStart) desiredStart = maxStart;
    int btnStartX = desiredStart;

    // --- Top Section: Buttons (2 Rows) ---
    int row1Y = margin;
    int row2Y = row1Y + btnH + gap;
    
    // Row 1 and Row 2 - use absolute positions for each button (recompute deterministically)
    int bx0 = btnStartX;
    int bx1 = bx0 + (btnW + effectiveGap);
    int bx2 = bx1 + (btnW + effectiveGap);

    HWND h;
    h = GetDlgItem(hwnd, IDC_BTN_AUTO_CONNECT); if (h && IsWindow(h)) SetWindowPos(h, NULL, bx0, row1Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    h = GetDlgItem(hwnd, IDC_BTN_AUTO_HOME);    if (h && IsWindow(h)) SetWindowPos(h, NULL, bx1, row1Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    h = GetDlgItem(hwnd, IDC_BTN_AUTO_FAULT_RESET); if (h && IsWindow(h)) SetWindowPos(h, NULL, bx2, row1Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);

    h = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP); if (h && IsWindow(h)) SetWindowPos(h, NULL, bx0, row2Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    h = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP); if (h && IsWindow(h)) SetWindowPos(h, NULL, bx1, row2Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
    h = GetDlgItem(hwnd, IDC_BTN_AUTO_STATUS_WINDOW); if (h && IsWindow(h)) SetWindowPos(h, NULL, bx2, row2Y, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);

    // --- Middle Section: Status Lamps (4 Rows, 3 Columns) ---
    int lampsStartY = row2Y + btnH + gap;
    
    // Status lamps: compute absolute positions in a 3-column x 4-row grid aligned to buttons
    int sx0 = bx0;
    int sx1 = bx1;
    int sx2 = bx2;
    auto setLamp = [&](int id, int col, int row) {
        HWND hs = GetDlgItem(hwnd, id);
        if (hs && IsWindow(hs)) {
            int x = (col == 0) ? sx0 : (col == 1) ? sx1 : sx2;
            int y = lampsStartY + row * (statusHeight + gap);
            SetWindowPos(hs, NULL, x, y, statusWidth, statusHeight, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    };

    // Outputs (6 items) -> Rows 0-1
    setLamp(IDC_AUTO_STATUS_OETHERCATOK, 0, 0);
    setLamp(IDC_AUTO_STATUS_OBUSY,       1, 0);
    setLamp(IDC_AUTO_STATUS_OERROR,      2, 0);
    setLamp(IDC_AUTO_STATUS_OREADY,      0, 1);
    setLamp(IDC_AUTO_STATUS_OHOMEDONE,   1, 1);
    setLamp(IDC_AUTO_STATUS_OONLINE,     2, 1);

    // Inputs removed: no additional lamps to position here

    // --- Bottom Section: MMF Table ---
    // Place the MMF table directly under the status-lamp area (no
    // dynamic window resizing or complex positioning). The table will
    // span the content width (full client area minus side padding) and
    // be positioned immediately after the status lamps' last row.
    // NOTE: this intentionally keeps layout simple and avoids resizing
    // the top-level window.
    // Move the MMF table up by 20 pixels relative to the baseline under the lamps
    int mmfTop = lampsStartY + (statusHeight + gap) * 2 - 20; // -20px upward offset
    int mmfX = sidePad; // align to content left edge
    int mmfW = clientW - sidePad * 2; // fill content width
    if (mmfW < 50) mmfW = 50;
    // no-op: clientHNow kept for further calculations
    int mmfH = clientHNow - mmfTop - margin; // remaining client area below lamps
    if (mmfH < 100) mmfH = 100; // ensure minimal visible table height

    // Label "MMF Data"
    int mmfLabelH = 20;
    HWND hLbl = GetDlgItem(hwnd, IDC_STATIC_MMF);
    if (hLbl && IsWindow(hLbl)) SetWindowPos(hLbl, NULL, mmfX, mmfTop, mmfW, mmfLabelH, SWP_NOZORDER | SWP_NOACTIVATE);
    
    HWND hList = GetDlgItem(hwnd, IDC_LIST_MMF);
    if (hList && IsWindow(hList)) {
        int listY = mmfTop + mmfLabelH + 4;
        int availableListH = mmfH - (mmfLabelH + 4);

        // Calculate desired height so the ListView shows exactly 7 rows
        // including the header: headerHeight + rowHeight * (7 - 1).
        int headerH = 20; // fallback
        HWND hHeader = (HWND)SendMessageW(hList, LVM_GETHEADER, 0, 0);
        if (hHeader && IsWindow(hHeader)) {
            RECT hr; GetWindowRect(hHeader, &hr);
            headerH = (hr.bottom - hr.top);
        }

        int rowH = 18; // fallback
        if (g_mmfImageList) {
            int cx = 0; int cy = 0;
            ImageList_GetIconSize(g_mmfImageList, &cx, &cy);
            if (cy > 0) rowH = cy;
        } else {
            // As a fallback, query an item's bounds if present
            RECT ir; if (ListView_GetItemRect(hList, 0, &ir, LVIR_BOUNDS)) {
                int itemH = ir.bottom - ir.top;
                if (itemH > 0) rowH = itemH;
            }
        }

        const int rowsIncludingHeader = 7;
        int desiredListH = headerH + rowH * (rowsIncludingHeader - 1);
        // Add a small extra padding to ensure the last visible row isn't clipped
        // (some systems render borders/padding that reduce visible area slightly).
        const int extraPadding = 4;
        desiredListH += extraPadding;

        // Do not resize the window — clamp desired height to the available area
        int listH = desiredListH;
        if (listH > availableListH) listH = availableListH;
        // Increase total list height by 2 pixels per request
        if (listH + 2 <= availableListH) listH += 2; else listH = availableListH;
        SetWindowPos(hList, NULL, mmfX, listY, mmfW, listH, SWP_NOZORDER | SWP_NOACTIVATE);
        
        // Adjust columns
        int listClientW = mmfW;
        int col1 = 0;
        int col2 = 0;
        int col3 = 0;
        ComputeAutoWindowMmfColumnWidths(listClientW, col1, col2, col3);
        
        ListView_SetColumnWidth(hList, 0, col1);
        ListView_SetColumnWidth(hList, 1, col2);
        ListView_SetColumnWidth(hList, 2, col3);
    }
    ForceHideMMFScrollbars(hwnd);
}



static LRESULT CALLBACK AutoWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_WINDOWPOSCHANGED:
        // Ensure AutoWindow remains on top of other windows when Z-order changes.
        // SWP_NOACTIVATE to avoid stealing focus from the user.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        break;
    case WM_SYSCOMMAND:
        {
            UINT cmd = (UINT)(wParam & 0xFFF0);
            // 攔截並忽略使用者觸發的 Close / Maximize 命令（包含 Alt+F4）
            if (cmd == SC_CLOSE || cmd == SC_MAXIMIZE) {
                return 0; // 不傳給 DefWindowProc，忽略
            }
        }
        break;
    case WM_CLOSE:
        // 禁止使用者關閉視窗（例如 Alt+F4 或系統 Close）
        return 0;
    case WM_DESTROY:
        // Stop all timers before destroying window
        KillTimer(hwnd, 1); // MMF update timer
        KillTimer(hwnd, 2); // Fault reset timeout
        KillTimer(hwnd, 3); // Immediate stop timeout
        KillTimer(hwnd, 4); // Emergency stop timeout
        KillTimer(hwnd, 5); // Connect/Run timeout
        // Stop background status update thread
        StopStatusUpdateThread();
        if (g_mmfImageList) {
            ImageList_Destroy(g_mmfImageList);
            g_mmfImageList = NULL;
        }
        return 0;
    case WM_CREATE:
        InitAutoWindowBrushes();
        ResetAutoWindowStopState();
        // Explicitly load and set the Ka-52 icon (ID 102) for this window instance to ensure 
        // it doesn't fall back to the main application icon (ah64.ico).
        {
            HINSTANCE hInst = (HINSTANCE)GetModuleHandle(NULL);
            HICON hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(102), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
            HICON hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(102), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            if (hIcon) SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            if (hIconSm) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);
        }
        // Start background status update thread to avoid UI blocking
        StartStatusUpdateThread(hwnd);
        UpdateAllAutoButtons(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wParam == 1) {
            // Safety: verify AutoWindow is still valid before processing timer
            if (!hwnd || !IsWindow(hwnd)) {
                KillTimer(hwnd, 1);
                return 0;
            }
            HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
            if (!hOwnerLocal || !IsWindow(hOwnerLocal)) {
                // Owner window is gone or invalid - stop timer
                KillTimer(hwnd, 1);
                return 0;
            }
            auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwnerLocal, GWLP_USERDATA));
            if (!win) {
                // Owner window has no valid user data - stop timer
                KillTimer(hwnd, 1);
                return 0;
            }
            
            // Process MMF updates with owner window
            {
                const bool mapped = win->MasterMMF1IsMapped();
                const bool mmfRunning = win->MasterIsMMF1InRunning();
                EnsureAutoWindowRefreshTimer(
                    hwnd,
                    GetAutoWindowRefreshIntervalMs(mapped, mmfRunning));
                // mapping state transition
                if (mapped != g_mmfPreviouslyMapped) {
                    if (mapped) {
                            win->PrependStatus(L"[MMF] MMF已映射\r\n");
                            // reset per-map flags
                            g_mmfFirstSnapshotLogged = false;
                            g_mmfNonZeroLogged = false;
                            g_mmfZeroLogLastTick = 0;
                    } else {
                            win->PrependStatus(L"[WARN] MMF 未映射\r\n");
                            // clear internal flags
                            g_mmfFirstSnapshotLogged = false;
                            g_mmfNonZeroLogged = false;
                            g_mmfZeroLogLastTick = 0;
                        }
                    g_mmfPreviouslyMapped = mapped;
                    }

                    HWND hList = GetDlgItem(hwnd, IDC_LIST_MMF);
                    
                    // Fetch mechanical actual position from main window and display
                    // in the '實際角度' column (column index 1). Use slave indices 1..6
                    // This runs independently of MMF mapping status
                    for (int i = 0; i < 6; ++i) {
                        MechanicalUnitMetrics m;
                        int slaveIdx = i + 1;
                        const wchar_t* posText = L"N/A";
                        bool gotMetrics = false;
                        if (win) {
                            gotMetrics = win->MasterGetMechanicalUnitMetrics(slaveIdx, m);
                            if (gotMetrics && m.valid) {
                                posText = m.actualPosition.c_str();
                            }
                        }
                        wchar_t actualBuf[128];
                        FormatAutoWindowActualValue(slaveIdx,
                                                    posText,
                                                    actualBuf,
                                                    _countof(actualBuf));
                        // 若尚未初始化快取（空字串），或數值有改變則更新顯示
                        if (g_lastActualPositions[i].empty() ||
                            g_lastActualPositions[i] != actualBuf) {
                            if (hList && IsWindow(hList)) {
                                ListView_SetItemText(hList, i, 0, actualBuf);
                            }
                            g_lastActualPositions[i].assign(actualBuf);
                        }
                    }
                    (void)0;

                    std::array<uint16_t, 6> errorCodes603F = {0};
                    std::array<bool, 6> errorCodeValid603F = {false};
                    win->GetLastReportedErrorCodes(errorCodes603F, errorCodeValid603F);
                    for (int i = 0; i < 6; ++i) {
                        wchar_t errorCodeBuf[32];
                        FormatAutoWindowErrorCodeValue(errorCodeValid603F[i],
                                                       errorCodes603F[i],
                                                       errorCodeBuf,
                                                       _countof(errorCodeBuf));
                        if (g_lastErrorCodeTexts[i].empty() ||
                            g_lastErrorCodeTexts[i] != errorCodeBuf) {
                            if (hList && IsWindow(hList)) {
                                ListView_SetItemText(hList, i, 2, errorCodeBuf);
                            }
                            g_lastErrorCodeTexts[i].assign(errorCodeBuf);
                        }
                    }
                    
                    if (mapped && mmfRunning) {
                        // Update numeric values in the ListView (first 6 entries)
                        auto vals = win->MasterGetMMF1Latest();
                        wchar_t buf[64];
                        bool anyChanged = false;
                        UpdateAutoWindowMmfInputColumnHeader(hList);
                        for (int i = 0; i < 6; ++i) {
                            FormatAutoWindowMmfInputValue(i, vals[i], buf, _countof(buf));
                            if (g_lastMmfDisplayTexts[i].empty() || g_lastMmfDisplayTexts[i] != buf) {
                                if (hList && IsWindow(hList)) ListView_SetItemText(hList, i, 1, buf);
                                g_lastMMFValues[i] = vals[i];
                                g_lastMmfDisplayTexts[i].assign(buf);
                                anyChanged = true;
                            }
                        }
                        if (anyChanged) {
                            ForceHideMMFScrollbars(hwnd);
                        }

                        if (!g_mmfFirstSnapshotLogged) {
                            std::wstringstream ss;
                            ss << L"[MMF] 初始快照: ";
                            ss << std::fixed << std::setprecision(4);
                            for (int i = 0; i < 6; ++i) {
                                if (i) ss << L", ";
                                ss << static_cast<double>(vals[i]);
                            }
                            ss << L"\r\n";
                            std::wstring s = ss.str();
                            win->PrependStatus(s.c_str());
                            g_mmfFirstSnapshotLogged = true;
                        }

                        if (!g_mmfNonZeroLogged) {
                            bool anyNonZero = false;
                            for (int i = 0; i < 6; ++i) { if (vals[i] != 0.0f) { anyNonZero = true; break; } }
                            if (anyNonZero) {
                                win->PrependStatus(L"[OK] MMF 已接收非零值\r\n");
                                g_mmfNonZeroLogged = true;
                                g_mmfZeroLogLastTick = 0; // reset throttling
                            } else {
                                // all-zero snapshot: throttle warnings to once per 30s
                                unsigned long long now = GetTickCount64();
                                const unsigned long long throttleMs = 30000ULL;
                                if (g_mmfZeroLogLastTick == 0 || now - g_mmfZeroLogLastTick > throttleMs) {
                                    win->PrependStatusNoFile(L"[WARN] MMF 快照全部為 0\r\n");
                                    g_mmfZeroLogLastTick = now;
                                }
                            }
                        }
                    } else {
                        wchar_t buf[64];
                        bool anyChanged = false;
                        UpdateAutoWindowMmfInputColumnHeader(hList);
                        for (int i = 0; i < 6; ++i) {
                            FormatAutoWindowMmfInputUnavailableValue(i, buf, _countof(buf));
                            if (g_lastMmfDisplayTexts[i].empty() || g_lastMmfDisplayTexts[i] != buf) {
                                if (hList && IsWindow(hList)) ListView_SetItemText(hList, i, 1, buf);
                                g_lastMmfDisplayTexts[i].assign(buf);
                                anyChanged = true;
                            }
                        }
                        if (anyChanged) {
                            ForceHideMMFScrollbars(hwnd);
                        }
                    }
            }
        }
        // User/system timers below (2/3) are used for override timeouts
        // Event-driven status update message handling is separate (WM_USER_...)
        // Timer id 2: fault-reset timeout fallback (clear override if stuck)
            if (wParam == 2) {
                    if (g_faultResetOverrideActive) {
                g_faultResetOverrideActive = false;
                HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_FAULT_RESET);
                if (hBtn && IsWindow(hBtn)) EnableWindow(hBtn, TRUE);
                // ensure UI repaints to restore SDO-driven color
                UpdateAllAutoButtons(hwnd);
            }
            KillTimer(hwnd, 2);
        }
        if (wParam == 3) {
            // fallback timer for immediate-stop override expired
            if (g_immediateStopOverrideActive) {
                g_immediateStopOverrideActive = false;
                HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP);
                if (hBtn && IsWindow(hBtn)) EnableWindow(hBtn, TRUE);
                UpdateAllAutoButtons(hwnd);
            }
            KillTimer(hwnd, 3);
        }
        if (wParam == 5) {
            if (g_autoConnectOverrideActive) {
                g_autoConnectOverrideActive = false;
                SyncAutoWindowActionButtons(hwnd);
                UpdateAllAutoButtons(hwnd);
            }
            KillTimer(hwnd, 5);
        }
        return 0;
    case WM_NOTIFY:
        {
            LPNMHDR nh = (LPNMHDR)lParam;
            if (nh) {
                // Prevent header column tracking (resizing) for the MMF ListView
                if (nh->idFrom == IDC_LIST_MMF) {
                    if (nh->code == HDN_BEGINTRACKA || nh->code == HDN_BEGINTRACKW) {
                        // Returning a non-zero value cancels the track operation
                        return TRUE;
                    }
                }
            }
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        {
            // Read owner-exposed color property and create a local brush from it.
            HWND hOwnerLocal = (HWND)GetPropW(hwnd, L"AutoWindowOwner");
            HBRUSH bg = NULL;
            if (hOwnerLocal && IsWindow(hOwnerLocal)) {
                HANDLE hCol = GetPropW(hOwnerLocal, L"BigCatBgColor");
                if (hCol) {
                    COLORREF col = (COLORREF)(UINT_PTR)hCol;
                    if (g_autoWindowBgBrush) { DeleteObject(g_autoWindowBgBrush); g_autoWindowBgBrush = NULL; }
                    g_autoWindowBgBrush = CreateSolidBrush(col);
                    bg = g_autoWindowBgBrush;
                }
            }
            if (!bg) bg = GetSysColorBrush(COLOR_WINDOW);
            SetClassLongPtrW(hwnd, GCLP_HBRBACKGROUND, (LONG_PTR)bg);
            // Reinitialize custom brushes if needed and force repaint
            InitAutoWindowBrushes();
            InvalidateRect(hwnd, NULL, TRUE);
            UpdateWindow(hwnd);
        }
        return 0;
    case WM_CTLCOLORBTN:
        {
            HWND hCtl = (HWND)lParam;
            int id = GetDlgCtrlID(hCtl);
            if (id == IDC_BTN_AUTO_EMERGENCY_STOP) {
                // Decide brush based on last cached bit value
                HDC hdc = (HDC)wParam;
                if (g_lastEmergBit5 == 0) { SetBkColor(hdc, RGB(255,0,0)); return (INT_PTR)g_brushBrightRed; }
                else { SetBkColor(hdc, RGB(120,0,0)); return (INT_PTR)g_brushDarkRed; }
            }
        }
        break;
    case WM_DRAWITEM:
        {
            LPDRAWITEMSTRUCT dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (dis) {
                HBRUSH br = nullptr;
                bool   isStatusLamp = false;
                bool   isStatusWindowButton = (dis->CtlID == IDC_BTN_AUTO_STATUS_WINDOW);
                const auto buttonState = BuildAutoWindowActionButtonState(hwnd);

                switch (dis->CtlID) {
                case IDC_BTN_AUTO_EMERGENCY_STOP:
                    br = buttonState.emergency_stop_lit ? g_brushBrightRed : g_brushDarkRed;
                    break;
                case IDC_BTN_AUTO_IMMEDIATE_STOP:
                    br = buttonState.immediate_stop_lit ? g_brushBrightRed : g_brushDarkRed;
                    break;
                case IDC_BTN_AUTO_FAULT_RESET:
                    br = buttonState.fault_reset_lit ? g_brushBrightYellow : g_brushDarkYellow;
                    break;
                case IDC_BTN_AUTO_STATUS_WINDOW:
                    // Status display button: white background, black text
                    br = (HBRUSH)GetStockObject(WHITE_BRUSH);
                    break;
                case IDC_BTN_AUTO_HOME:
                    br = buttonState.down_to_origin_lit ? g_brushBrightBlue : g_brushDarkBlue;
                    break;
                case IDC_BTN_AUTO_CONNECT:
                    br = buttonState.connect_lit ? g_brushBrightGreen : g_brushDarkGreen;
                    break;

                // 以下為全區變數狀態燈號（方形），全部使用 owner-draw STATIC/BUTTON
                case IDC_AUTO_STATUS_OETHERCATOK: isStatusLamp = true; br = g_oEtherCatOk ? g_brushBrightGreen : g_brushDarkBlue; break;
                case IDC_AUTO_STATUS_OBUSY:       isStatusLamp = true; br = g_oBusy       ? g_brushBrightYellow : g_brushDarkBlue; break;
                case IDC_AUTO_STATUS_OERROR:      isStatusLamp = true; br = g_oError      ? g_brushBrightRed    : g_brushDarkBlue; break;
                case IDC_AUTO_STATUS_OREADY:      isStatusLamp = true; br = g_oReady      ? g_brushBrightGreen  : g_brushDarkBlue; break;
                case IDC_AUTO_STATUS_OHOMEDONE:   isStatusLamp = true; br = g_oHomeDone   ? g_brushBrightBlue    : g_brushDarkBlue; break;
                case IDC_AUTO_STATUS_OONLINE:     isStatusLamp = true; br = g_oOnline     ? g_brushBrightGreen  : g_brushDarkBlue; break;

                // input-status lamps removed
                default:
                    break;
                }

                if (br) {
                    FillRect(dis->hDC, &dis->rcItem, br);
                    // Draw a thin border to distinguish outputs vs inputs
                    // Input lamps removed; use a simple black border for status lamps
                    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0,0,0));
                    HGDIOBJ oldPen = SelectObject(dis->hDC, hPen);
                    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
                    SelectObject(dis->hDC, oldBrush);
                    SelectObject(dis->hDC, oldPen);
                    DeleteObject(hPen);
                    if (!isStatusLamp) {
                        // 一般按鈕：畫邊框與文字
                        DrawEdge(dis->hDC, const_cast<RECT*>(&dis->rcItem), (dis->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
                        wchar_t text[64] = L"";
                        GetWindowTextW(dis->hwndItem, text, (int)(sizeof(text) / sizeof(text[0])));
                        // For the Status Display button use black text on white background
                        if (isStatusWindowButton) SetTextColor(dis->hDC, RGB(0, 0, 0));
                        else SetTextColor(dis->hDC, RGB(255, 255, 255));
                        SetBkMode(dis->hDC, TRANSPARENT);
                        RECT rcText = dis->rcItem;
                        if (dis->itemState & ODS_SELECTED) { rcText.left += 1; rcText.top += 1; }
                        DrawTextW(dis->hDC, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                        if (dis->itemState & ODS_FOCUS) {
                            RECT rcFocus = dis->rcItem; InflateRect(&rcFocus, -3, -3);
                            DrawFocusRect(dis->hDC, &rcFocus);
                        }
                    } else {
                        // 狀態燈號：中央顯示變數名稱（控制項文字）
                        wchar_t text[64] = L"";
                        GetWindowTextW(dis->hwndItem, text, (int)(sizeof(text) / sizeof(text[0])));
                        SetTextColor(dis->hDC, RGB(255, 255, 255));
                        SetBkMode(dis->hDC, TRANSPARENT);
                        RECT rcText = dis->rcItem;
                        // Add a tiny interior margin so the text doesn't touch edges
                        // and use ellipsis if it still doesn't fit.
                        InflateRect(&rcText, -2, -2);
                        DrawTextW(dis->hDC, text, -1, &rcText, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    }
                    return TRUE;
                }
            }
        }
        break;
    case WM_SIZE:
        {
            if (wParam == SIZE_MINIMIZED) {
                // mark minimized, but keep timers running so updates continue
                g_autoWindowIsMinimized = true;
                EnsureAutoWindowRefreshTimer(hwnd, kAutoWindowMinimizedRefreshMs);
            } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
                if (g_autoWindowIsMinimized.load(std::memory_order_relaxed)) {
                    g_autoWindowIsMinimized = false;
                    EnsureAutoWindowRefreshTimer(hwnd, kAutoWindowActiveRefreshMs);
                    WakeStatusUpdateThread();
                    // ensure UI is refreshed on restore (background thread continues updating)
                    UpdateAllAutoButtons(hwnd);
                    PostMessageW(hwnd, WM_USER_AUTO_STATUS_UPDATE, 0, 0);
                    InvalidateRect(hwnd, NULL, FALSE);
                }
                // Re-layout controls when window is resized/restored
                LayoutAutoWindowControls(hwnd);
            }
        }
        break;
    case WM_COMMAND:
                if (wParam == 4) {
                    if (g_emergencyStopOverrideActive) {
                        g_emergencyStopOverrideActive = false;
                        HWND hBtn = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP);
                        if (hBtn && IsWindow(hBtn)) EnableWindow(hBtn, TRUE);
                        UpdateAllAutoButtons(hwnd);
                    }
                    KillTimer(hwnd, 4);
                }
        {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == IDC_BTN_AUTO_FAULT_RESET && code == BN_CLICKED) {
                if (g_autoDisabledActive) break;
                const auto decision =
                    CoreHelpers::EvaluateAutoFaultReset(BuildAutoWindowPolicySnapshot());
                if (!decision.allowed) break;
                // Start visual override and request main window to perform the fault reset
                HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
                if (hOwnerLocal && IsWindow(hOwnerLocal)) {
                    BeginAutoWindowAsyncButtonOverride(hwnd,
                                                       IDC_BTN_AUTO_FAULT_RESET,
                                                       g_faultResetOverrideActive,
                                                       2);
                    AppController::GetInstance().RequestAutoFaultReset(reinterpret_cast<uintptr_t>(hwnd));
                }
            }
            // Auto Connect button has no action by default (pending planning)
            if (id == IDC_BTN_AUTO_CONNECT && code == BN_CLICKED) {
                // User pressed AutoWindow Connect button -> attempt to set busy/online
                // only if AutoWindow is enabled and preconditions satisfied.
                if (g_autoDisabledActive) break;
                if (g_autoConnectOverrideActive) break;
                if (HasAutoWindowStopCondition(ReadAutoWindowStopInputState(hwnd))) break;
                const auto decision = CoreHelpers::EvaluateAutoConnect(BuildAutoWindowPolicySnapshot());
                if (!decision.allowed) break;
                HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
                if (!(hOwnerLocal && IsWindow(hOwnerLocal))) break;
                BeginAutoWindowAsyncButtonOverride(hwnd,
                                                   IDC_BTN_AUTO_CONNECT,
                                                   g_autoConnectOverrideActive,
                                                   5);
                g_oBusy = true;
                g_oOnline = true;
                SyncAutoWindowActionButtons(hwnd);
                UpdateAllAutoButtons(hwnd);
                AppController::GetInstance().RequestAutoConnect();
                break;
            }
            // Status Window button: toggle the main window's child status window
            if (id == IDC_BTN_AUTO_STATUS_WINDOW && code == BN_CLICKED) {
                HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
                if (hOwnerLocal && IsWindow(hOwnerLocal)) {
                    auto* win = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hOwnerLocal, GWLP_USERDATA));
                    if (win) {
                        win->ToggleChildStatusWindow();
                    }
                }
                break;
            }
            // Immediate Stop: handled on press via subclass proc so ignore clicks
            if (id == IDC_BTN_AUTO_HOME && code == BN_CLICKED) {
                if (g_downToOriginOverrideActive) break;
                if (HasAutoWindowStopCondition(ReadAutoWindowStopInputState(hwnd))) {
                    SyncAutoWindowActionButtons(hwnd);
                    break;
                }
                const auto downToOriginDecision =
                    CoreHelpers::EvaluateAutoDownToOrigin(BuildAutoWindowPolicySnapshot());
                if (!downToOriginDecision.allowed) break;
                HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
                if (hOwnerLocal && IsWindow(hOwnerLocal)) {
                    // 如果當前處於 online 狀態，先執行立即停止，等待減速停止完成後再繼續
                    if (g_oOnline) {
                        g_pendingDownToOriginAfterStop = true;
                        BeginAutoWindowAsyncButtonOverride(hwnd,
                                                           IDC_BTN_AUTO_HOME,
                                                           g_downToOriginOverrideActive);
                        // 觸發立即停止程序
                        if (!g_immediateStopOverrideActive) {
                            g_immediateStopOverrideActive = true;
                        }
                        PostMessageW(hOwnerLocal,
                                     WM_USER_AUTO_IMMEDIATE_STOP_REQUEST,
                                     (WPARAM)hwnd,
                                     AUTO_IMMEDIATE_STOP_FLAG_SKIP_MANUAL_RESET);
                        UpdateAllAutoButtons(hwnd);
                        break;
                    }
                    
                    // 正常流程：不在 online 狀態，直接啟動到下原點
                    StartAutoWindowDownToOriginRequest(hwnd, hOwnerLocal, false);
                }
                // MMF handling is handled here at top-level for MMF buttons
            }

            // MMF1 enable/disable buttons removed — handling moved to master/auto-procedure logic.
        }
        break;
    case WM_USER_AUTO_FAULT_RESET_DONE:
        {
            // Owner signals completion (wParam == success(1)/failure(0)). We always clear override and restore UI.
            int success = (int)wParam;
            LogAutoWindowStopState(hwnd, success == 1 ? "[AutoWindow] Fault reset done success" : "[AutoWindow] Fault reset done failure");
            // If fault-reset completed successfully, clear global error flag
            if (success == 1) {
                g_oError = false;
                g_oManualResetRequired = false;
                ClearAutoWindowLatchedStopState(hwnd);
            }
            CompleteAutoWindowAsyncButtonOverride(hwnd,
                                                  IDC_BTN_AUTO_FAULT_RESET,
                                                  g_faultResetOverrideActive,
                                                  true,
                                                  2);
            // refresh UI to show updated g_oError
            UpdateAllAutoButtons(hwnd);
        }
        return 0;
    case WM_USER_AUTO_IMMEDIATE_STOP_DONE:
        {
            CompleteAutoWindowStopRequest(hwnd,
                                          g_immediateStopOverrideActive,
                                          g_immediateStopPressed,
                                          IDC_BTN_AUTO_IMMEDIATE_STOP,
                                          3,
                                          lParam);
            
            // 如果有待執行的到下原點請求，現在立即停止已完成，繼續執行到下原點
            if (g_pendingDownToOriginAfterStop) {
                g_pendingDownToOriginAfterStop = false;
                HWND hOwnerLocal = GetAutoWindowOwner(hwnd);
                StartAutoWindowDownToOriginRequest(hwnd, hOwnerLocal, true);
            }
        }
        return 0;
    case WM_USER_AUTO_STATUS_UPDATE:
        // Owner requested an immediate status refresh: update UI (background thread handles status)
        WakeStatusUpdateThread();
        UpdateAllAutoButtons(hwnd);
        // 只重繪狀態燈區域，避免全視窗閃爍
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_USER_AUTO_ENABLE_ALL:
        // Owner requests AutoWindow to allow its buttons to be enabled according to status
        g_autoDisabledActive = false;
        SyncAutoWindowActionButtons(hwnd);
        UpdateAllAutoButtons(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_USER_AUTO_DISABLE_ALL:
        // Owner requests AutoWindow to disable all actionable child buttons
        // 注意：立即停止和緊急停止按鈕永遠保持啟用，以便緊急情況下使用
        g_autoDisabledActive = true;
        // 清除 online 狀態（自動程序退出時）
        if (g_oOnline) {
            g_oOnline = false;
        }
        SyncAutoWindowActionButtons(hwnd);
        UpdateAllAutoButtons(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
        case WM_USER_AUTO_EMERGENCY_STOP_DONE:
            {
                (void)wParam;
                    CompleteAutoWindowStopRequest(hwnd,
                                                  g_emergencyStopOverrideActive,
                                                  g_emergencyStopPressed,
                                                  IDC_BTN_AUTO_EMERGENCY_STOP,
                                                  4,
                                                  lParam);
            }
            return 0;
    case WM_USER + 200: // Status update complete from background thread
        // Background thread has updated global status flags, now perform UI updates
        if (!g_autoWindowIsMinimized.load(std::memory_order_relaxed) &&
            (g_oBusy || g_oOnline || g_oError)) {
            EnsureAutoWindowRefreshTimer(hwnd, kAutoWindowActiveRefreshMs);
        }
        // 1) Refresh SDO-driven buttons/lights
        UpdateAllAutoButtons(hwnd);

        // 2) Apply enable/disable logic on UI thread (safe to call EnableWindow here)
        {
            SyncAutoWindowActionButtons(hwnd);

            // Notify owner (MainWindow) to refresh web ui_state after button updates
            HWND hOwner = GetAutoWindowOwner(hwnd);
            if (hOwner && IsWindow(hOwner)) {
                PostMessageW(hOwner, WM_USER_AUTO_REPORT_UI_STATE, 0, 0);
            }

            // If any override flags were set by the background thread, ensure the corresponding
            // controls are disabled/repainted immediately so the user sees the override state.
            if (g_faultResetOverrideActive) {
                HWND h = GetDlgItem(hwnd, IDC_BTN_AUTO_FAULT_RESET); if (h && IsWindow(h)) { EnableWindow(h, FALSE); InvalidateRect(h, NULL, FALSE); }
            }
            // 停止按鈕不應該被禁用，只需要 override 狀態的視覺效果（透過 owner-draw）
            // if (g_immediateStopOverrideActive) {
            //     HWND h = GetDlgItem(hwnd, IDC_BTN_AUTO_IMMEDIATE_STOP); if (h && IsWindow(h)) { EnableWindow(h, FALSE); InvalidateRect(h, NULL, FALSE); }
            // }
            // if (g_emergencyStopOverrideActive) {
            //     HWND h = GetDlgItem(hwnd, IDC_BTN_AUTO_EMERGENCY_STOP); if (h && IsWindow(h)) { EnableWindow(h, FALSE); InvalidateRect(h, NULL, FALSE); }
            // }
        }

        // 3) Handle any pending timer requests signalled by background thread
        {
            int mask = g_pendingSetTimersMask.exchange(0);
            if (mask != 0) {
                for (int tid = 0; tid < 32; ++tid) {
                    if (mask & (1 << tid)) {
                        // restart fallback timer for id=tid with 5s timeout
                        KillTimer(hwnd, tid);
                        SetTimer(hwnd, tid, 5000, NULL);
                    }
                }
            }
        }

        // 4) Request main repaint of status region
        InvalidateRect(hwnd, NULL, FALSE);

        // 5) Invalidate status lamps to ensure they repaint with updated flags
        {
            const int statusLamps[] = {
                IDC_AUTO_STATUS_OETHERCATOK,
                IDC_AUTO_STATUS_OBUSY,
                IDC_AUTO_STATUS_OERROR,
                IDC_AUTO_STATUS_OREADY,
                IDC_AUTO_STATUS_OHOMEDONE,
                IDC_AUTO_STATUS_OONLINE
            };
            for (int id : statusLamps) {
                HWND hLamp = GetDlgItem(hwnd, id);
                if (hLamp && IsWindow(hLamp)) {
                    InvalidateRect(hLamp, NULL, FALSE);
                }
            }
        }
        return 0;
    case WM_USER_AUTO_DOWN_TO_ORIGIN_DONE:
        {
            // wParam == 1 -> success, 0 -> failure/exception
            int success = (int)wParam;
            // lParam == 1 -> completion resulted from an immediate/emergency stop
            // (owner requested ImmediateStop/ EmergencyStop to abort workflow). In
            // that case do not clear g_oHomeDone to avoid treating an operator
            // stop as a homing failure.
            bool abortedByStop = (int)lParam != 0;

            if (abortedByStop) {
                // leave g_oHomeDone unchanged when aborted by stop
            } else {
                // If EtherCAT is not OK, consider homing failed regardless of reported success
                if (!g_oEtherCatOk) {
                    if (g_oHomeDone) {
                        g_oHomeDone = false;
                    }
                } else {
                    bool newHomeDone = (success == 1);
                    if (g_oHomeDone != newHomeDone) {
                        g_oHomeDone = newHomeDone;
                    }
                }

                // Ensure busy flag is cleared when DownToOrigin thread finishes
                if (g_oBusy) {
                    g_oBusy = false;
                }
                // Clear online flag when DownToOrigin completes (到下原點完成時)
                if (g_oOnline) {
                    g_oOnline = false;
                }
            }
            // Clear override and restore UI
            CompleteAutoWindowAsyncButtonOverride(hwnd,
                                                  IDC_BTN_AUTO_HOME,
                                                  g_downToOriginOverrideActive,
                                                  true);
            // Refresh status lamps to reflect new `g_oHomeDone` value
            UpdateAllAutoButtons(hwnd);
        }
        return 0;
    case WM_USER_AUTO_SET_OWNER_STATE:
        {
            // wParam bits: bit0 -> BUSY, bit1 -> ONLINE
            bool setBusy = ((wParam & 0x1) != 0);
            bool setOnline = ((wParam & 0x2) != 0);

            // Update local cached flags and refresh UI immediately
            g_oBusy = setBusy;
            g_oOnline = setOnline;
            UpdateAllAutoButtons(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_USER + 299:
        // Web-originated down-to-origin start notification: clear g_oHomeDone before workflow begins
        // This ensures PC AutoWindow state stays synchronized when Web initiates homing
        if (g_oHomeDone) {
            g_oHomeDone = false;
            UpdateAllAutoButtons(hwnd);
        }
        return 0;
    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    // Defensive fallback: ensure all control paths return a value.
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

HWND CreateAutoWindow(HWND hOwner) {
    ResetAutoWindowStopState();
    g_autoDisabledActive = true;

    HINSTANCE hInst = (HINSTANCE)GetModuleHandle(NULL);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = AutoWindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = AUTO_WINDOW_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    // Use the current system window background brush so the AutoWindow matches
    // the system theme/color. We'll refresh this at runtime when settings/theme change.
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);

    // Use a dedicated icon for AutoWindow (resource ID 102 -> ka52.ico)
    wc.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(102), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(102), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);

    // register class if needed
    if (!GetClassInfoExW(hInst, AUTO_WINDOW_CLASS, &wc)) {
        wc.style = CS_HREDRAW | CS_VREDRAW;
        if (!RegisterClassExW(&wc)) {
            return NULL;
        }
    }

    // Default size: compute width based on three buttons' total width (+ side padding) and clamp to screen
    // Reduce default AutoWindow height by 100 as requested
    int height = 350;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;

    // Position the AutoWindow at the top-right of the primary screen with a small margin
    const int rightMargin = 8;
    int screenW = GetSystemMetrics(SM_CXSCREEN);

    // 左右各預留 2px，讓視窗寬度等於三個按鈕跨距總寬 + 左右預留
    const int btnW = 105;
    const int gap = 8;
    const int sidePad = 2; // 左右預留 2px
    int threeBtnW = btnW * 3 + gap * 2;
    int width = threeBtnW + sidePad * 2;
    // Ensure it doesn't exceed available screen width (leave rightMargin free)
    int maxAllowed = screenW - rightMargin;
    if (width > maxAllowed) width = maxAllowed;
    if (width < threeBtnW + sidePad * 2) width = threeBtnW + sidePad * 2;

    x = screenW - width - rightMargin;
    if (x < 0) x = rightMargin;
    y = rightMargin; // top with margin

    // Create a fixed-size AutoWindow: remove sizing border and maximize box
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE | WS_CLIPCHILDREN;
    // Create as topmost so AutoWindow stays above normal application windows.
    HWND h = CreateWindowExW(WS_EX_TOPMOST, AUTO_WINDOW_CLASS, L"BigCat_6Dof_V1.0", style,
        x, y, width, height, NULL, NULL, hInst, NULL);
    if (!h) return NULL;

    // 移除系統選單中的關閉與最大化命令，隱藏右上角的關閉/最大化按鈕
    HMENU hSys = GetSystemMenu(h, FALSE);
    if (hSys) {
        // 刪除 Close 與 Maximize 命令（如果存在）
        DeleteMenu(hSys, SC_CLOSE, MF_BYCOMMAND);
        DeleteMenu(hSys, SC_MAXIMIZE, MF_BYCOMMAND);
        DrawMenuBar(h);
    }

    // Record the original owner so the AutoWindow can still post messages and
    // query cached data from the owner. We store it as a window property so
    // the AutoWindow remains a top-level independent window (not owned).
    if (hOwner && IsWindow(hOwner)) {
        SetPropW(h, L"AutoWindowOwner", (HANDLE)hOwner);
    }

    // 建立自動視窗控制項（按鈕 + 燈號）
    auto createOwndButton = [&](int id, const wchar_t* text) {
        CreateWindowExW(0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
            0, 0, 0, 0, h, (HMENU)(INT_PTR)id, hInst, NULL);
    };

    auto createStatusLamp = [&](int id, const wchar_t* name) {
        CreateWindowExW(0, L"BUTTON", name,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_OWNERDRAW,
            0, 0, 0, 0, h, (HMENU)(INT_PTR)id, hInst, NULL);
    };

    createOwndButton(IDC_BTN_AUTO_CONNECT, L"連線運轉");
    createOwndButton(IDC_BTN_AUTO_HOME, L"到下原點");
    createOwndButton(IDC_BTN_AUTO_FAULT_RESET, L"異常重置");
    createOwndButton(IDC_BTN_AUTO_STATUS_WINDOW, L"狀態顯示");
    createOwndButton(IDC_BTN_AUTO_IMMEDIATE_STOP, L"立即停止");
    createOwndButton(IDC_BTN_AUTO_EMERGENCY_STOP, L"緊急停止");

    createStatusLamp(IDC_AUTO_STATUS_OETHERCATOK, L"oEtherCatOk");
    createStatusLamp(IDC_AUTO_STATUS_OBUSY,       L"oBusy");
    createStatusLamp(IDC_AUTO_STATUS_OERROR,      L"oError");
    createStatusLamp(IDC_AUTO_STATUS_OREADY,      L"oReady");
    createStatusLamp(IDC_AUTO_STATUS_OHOMEDONE,   L"oHomeDone");
    createStatusLamp(IDC_AUTO_STATUS_OONLINE,     L"oOnline");

    // Input-status lamps removed

    // labels will be created after default font is assigned

    // Create a slightly smaller font for text inside the square status lamps
    // so the label fits better. Use 9pt Segoe UI as a compact readable size.
    HFONT hStatusFont = NULL;
    HDC hdc = GetDC(h);
    if (hdc) {
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        int fontHeight = -MulDiv(9, dpi, 72); // 9pt
        hStatusFont = CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        ReleaseDC(h, hdc);
    }
    if (hStatusFont) {
        g_statusLampFont = hStatusFont; // keep for deletion later
        HWND hs = GetDlgItem(h, IDC_AUTO_STATUS_OETHERCATOK); if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        hs = GetDlgItem(h, IDC_AUTO_STATUS_OBUSY);       if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        hs = GetDlgItem(h, IDC_AUTO_STATUS_OERROR);      if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        hs = GetDlgItem(h, IDC_AUTO_STATUS_OREADY);      if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        hs = GetDlgItem(h, IDC_AUTO_STATUS_OHOMEDONE);   if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        hs = GetDlgItem(h, IDC_AUTO_STATUS_OONLINE);     if (hs) SendMessageW(hs, WM_SETFONT, (WPARAM)hStatusFont, TRUE);
        // input-status lamps removed; no font assignment needed
    }

    // If owner indicates dark theme, create a local brush matching the main-window color
    if (hOwner && IsWindow(hOwner)) {
        HANDLE hCol = GetPropW(hOwner, L"BigCatBgColor");
        if (hCol) {
            COLORREF col = (COLORREF)(UINT_PTR)hCol;
            if (g_autoWindowBgBrush) { DeleteObject(g_autoWindowBgBrush); g_autoWindowBgBrush = NULL; }
            g_autoWindowBgBrush = CreateSolidBrush(col);
            SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)g_autoWindowBgBrush);
        } else {
            SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)GetSysColorBrush(COLOR_WINDOW));
        }
    } else {
        SetClassLongPtrW(h, GCLP_HBRBACKGROUND, (LONG_PTR)GetSysColorBrush(COLOR_WINDOW));
    }

    // Copy the owner's window icons (big and small) so the AutoWindow looks
    // visually identical to the main window in titlebar/taskbar.
    if (hOwner && IsWindow(hOwner)) {
        HICON hBig = (HICON)SendMessageW(hOwner, WM_GETICON, ICON_BIG, 0);
        if (!hBig) hBig = (HICON)GetClassLongPtrW(hOwner, GCLP_HICON);
        HICON hSmall = (HICON)SendMessageW(hOwner, WM_GETICON, ICON_SMALL, 0);
        if (!hSmall) hSmall = (HICON)GetClassLongPtrW(hOwner, GCLP_HICONSM);
        if (hBig) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)hBig);
        if (hSmall) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    }

    // Apply default GUI font to buttons if available
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND hb = GetDlgItem(h, IDC_BTN_AUTO_CONNECT); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_HOME); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_FAULT_RESET); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_STATUS_WINDOW); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_IMMEDIATE_STOP); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_EMERGENCY_STOP); if (hb) SendMessageW(hb, WM_SETFONT, (WPARAM)hFont, TRUE);
    
    // Disable all buttons by default (will be enabled when auto procedure starts)
    hb = GetDlgItem(h, IDC_BTN_AUTO_CONNECT); if (hb) EnableWindow(hb, FALSE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_HOME); if (hb) EnableWindow(hb, FALSE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_FAULT_RESET); if (hb) EnableWindow(hb, FALSE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_STATUS_WINDOW); if (hb) EnableWindow(hb, TRUE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_IMMEDIATE_STOP); if (hb) EnableWindow(hb, FALSE);
    hb = GetDlgItem(h, IDC_BTN_AUTO_EMERGENCY_STOP); if (hb) EnableWindow(hb, FALSE);
    
    // Create MMF ListView
    // Create the MMF ListView without scrollbars (LVS_NOSCROLL) so
    // the UI never shows H/V scroll bars — we also call ForceHideMMFScrollbars
    // after creation to double-ensure scrollbars are disabled at runtime.
    HWND hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_NOSCROLL,
        0, 0, 360, 200, h, (HMENU)(INT_PTR)IDC_LIST_MMF, hInst, NULL);
    if (hList) {
        ListView_SetExtendedListViewStyle(hList, LVS_EX_GRIDLINES);
        SetWindowSubclass(hList, AutoListViewSubclassProc, 0, (DWORD_PTR)h);
        HFONT hF = g_statusLampFont ? g_statusLampFont : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        if (hF) SendMessageW(hList, WM_SETFONT, (WPARAM)hF, TRUE);
        // Insert columns
        LVCOLUMN lvcm; ZeroMemory(&lvcm, sizeof(lvcm)); lvcm.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvcm.fmt = LVCFMT_LEFT;
        RECT rc; GetClientRect(h, &rc);
        int lvW = (rc.right - rc.left) - 16; if (lvW < 240) lvW = 240;
        int col1 = 0;
        int col2 = 0;
        int col3 = 0;
        ComputeAutoWindowMmfColumnWidths(lvW, col1, col2, col3);
        lvcm.cx = col1; lvcm.pszText = (LPWSTR)L"實際角度"; ListView_InsertColumn(hList, 0, &lvcm);
        lvcm.cx = col2; lvcm.pszText = (LPWSTR)GetAutoWindowMmfInputHeaderText(); ListView_InsertColumn(hList, 1, &lvcm);
        lvcm.cx = col3; lvcm.pszText = (LPWSTR)L"0x603F"; ListView_InsertColumn(hList, 2, &lvcm);
        // Insert 6 rows
        for (int ri = 0; ri < 6; ++ri) {
            LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi)); lvi.mask = LVIF_TEXT; lvi.iItem = ri;
            wchar_t initialActual[128];
            FormatAutoWindowActualValue(ri + 1, L"N/A", initialActual, _countof(initialActual));
            lvi.pszText = initialActual;
            ListView_InsertItem(hList, &lvi);
            wchar_t initialInput[64];
            FormatAutoWindowMmfInputUnavailableValue(ri, initialInput, _countof(initialInput));
            ListView_SetItemText(hList, ri, 1, initialInput);
            wchar_t initialErrorCode[32];
            FormatAutoWindowErrorCodeValue(false, 0, initialErrorCode, _countof(initialErrorCode));
            ListView_SetItemText(hList, ri, 2, initialErrorCode);
            g_lastActualPositions[ri] = initialActual;
            g_lastMmfDisplayTexts[ri] = initialInput;
            g_lastErrorCodeTexts[ri] = initialErrorCode;
        }
        ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        HWND hHeader = (HWND)SendMessageW(hList, LVM_GETHEADER, 0, 0);
        if (hHeader && IsWindow(hHeader)) {
            SetWindowSubclass(hHeader, AutoHeaderSubclassProc, 0, (DWORD_PTR)h);
        }
        // Create ImageList to set row height
        if (!g_mmfImageList) {
            TEXTMETRIC tm; ZeroMemory(&tm, sizeof(tm));
            HDC hdcList = GetDC(hList);
            if (hdcList) {
                HFONT hPrev = (HFONT)SelectObject(hdcList, hF);
                GetTextMetricsW(hdcList, &tm);
                SelectObject(hdcList, hPrev);
                ReleaseDC(hList, hdcList);
            }
            // Reduce row padding so more rows fit without scrollbars
            int rowHeight = (int)tm.tmHeight + 4;
            if (rowHeight < 14) rowHeight = 14;
            g_mmfImageList = ImageList_Create(1, rowHeight, ILC_COLOR32 | ILC_MASK, 0, 1);
            if (g_mmfImageList) ListView_SetImageList(hList, g_mmfImageList, LVSIL_SMALL);
        }
        COLORREF listBg = GetSysColor(COLOR_WINDOW);
        if (hOwner && IsWindow(hOwner)) {
            HANDLE hCol2 = GetPropW(hOwner, L"BigCatBgColor");
            if (hCol2) listBg = (COLORREF)(UINT_PTR)hCol2;
        }
        ListView_SetBkColor(hList, listBg);
        ListView_SetTextBkColor(hList, listBg);
        int r = GetRValue(listBg), g = GetGValue(listBg), b = GetBValue(listBg);
        double lum = 0.299 * r + 0.587 * g + 0.114 * b;
        COLORREF textCol = (lum < 128.0) ? RGB(255,255,255) : RGB(0,0,0);
        ListView_SetTextColor(hList, textCol);
        // Ensure scrollbars remain hidden for this ListView (extra safety)
        ForceHideMMFScrollbars(h);
    }

    LayoutAutoWindowControls(h);
    // Subclass the immediate stop button to trigger on press (WM_LBUTTONDOWN)
    {
        HWND hBtnIm = GetDlgItem(h, IDC_BTN_AUTO_IMMEDIATE_STOP);
        if (hBtnIm) SetWindowSubclass(hBtnIm, AutoWinImmediateStopSubclassProc, 0, (DWORD_PTR)h);
    }
    // Subclass the emergency stop button (press triggers emergency stop)
    {
        HWND hBtnEm = GetDlgItem(h, IDC_BTN_AUTO_EMERGENCY_STOP);
        if (hBtnEm) SetWindowSubclass(hBtnEm, AutoWinEmergencyStopSubclassProc, 0, (DWORD_PTR)h);
    }
    
    // 初始化筆刷，確保第一次繪製時顏色正確
    InitAutoWindowBrushes();
    
    // Force first status update via event-driven message
    PostMessageW(h, WM_USER_AUTO_STATUS_UPDATE, 0, 0);

    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);

    // AutoWindow is created as WS_EX_TOPMOST and will re-assert TOPMOST in
    // WM_WINDOWPOSCHANGED; no need to call SetWindowPos here.
    // Start MMF/status refresh timer. The interval is adjusted dynamically
    // while the window is idle, active, or minimized.
    g_autoWindowIsMinimized = false;
    g_autoWindowRefreshTimerMs = kAutoWindowActiveRefreshMs;
    SetTimer(h, 1, g_autoWindowRefreshTimerMs, NULL);
    return h;
}

void DestroyAutoWindow(HWND hWnd) {
    if (!hWnd) return;
    KillTimer(hWnd, 1);
    g_autoWindowIsMinimized = false;
    g_autoWindowRefreshTimerMs = kAutoWindowActiveRefreshMs;
    ResetAutoWindowStopState();
    // clear stored owner prop
    RemovePropW(hWnd, L"AutoWindowOwner");
    if (IsWindow(hWnd)) DestroyWindow(hWnd);
    if (g_brushBrightRed) { DeleteObject(g_brushBrightRed); g_brushBrightRed = NULL; }
    if (g_brushDarkRed)   { DeleteObject(g_brushDarkRed);   g_brushDarkRed   = NULL; }
    if (g_brushBrightYellow) { DeleteObject(g_brushBrightYellow); g_brushBrightYellow = NULL; }
    if (g_brushDarkYellow)   { DeleteObject(g_brushDarkYellow);   g_brushDarkYellow   = NULL; }
    if (g_brushBrightBlue)   { DeleteObject(g_brushBrightBlue);   g_brushBrightBlue   = NULL; }
    if (g_brushDarkBlue)     { DeleteObject(g_brushDarkBlue);     g_brushDarkBlue     = NULL; }
    if (g_brushBrightGreen)  { DeleteObject(g_brushBrightGreen);  g_brushBrightGreen  = NULL; }
    if (g_brushDarkGreen)    { DeleteObject(g_brushDarkGreen);    g_brushDarkGreen    = NULL; }
    if (g_autoWindowBgBrush) { DeleteObject(g_autoWindowBgBrush); g_autoWindowBgBrush = NULL; }
    if (g_statusLampFont) { DeleteObject(g_statusLampFont); g_statusLampFont = NULL; }
}
