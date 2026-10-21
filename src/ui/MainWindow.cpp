#include "MainWindow.h"
#include "../app/AppController.h"
#include "../include/AutoWindow.h"
#include <vector>
// Windows common controls (ListView, InitCommonControlsEx, SetWindowSubclass, etc.)
#include <commctrl.h>
#include <windowsx.h>
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include <commdlg.h>
#include <sstream>
#include <iomanip>
#include <string>
#include <cwchar>
#include <algorithm>
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
#include "../include/core/ShutdownGuard.h"
#include "../include/MotionConfigIO.h"
#include "Win32Utils.h"
#include "HttpServer.h"
#include "MachineConfigIO.h"
#include "core/AppConfig.h"

// --- INI helpers for persistent UI options ---------------------------------
static std::wstring GetAppIniPath()
{
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, buf, MAX_PATH) == 0) return std::wstring(L".\\BigCat.ini");
    // strip extension
    wchar_t *p = wcsrchr(buf, L'\\');
    wchar_t *dot = wcsrchr(buf, L'.');
    if (dot && (!p || dot > p)) *dot = L'\0';
    std::wstring ini = std::wstring(buf) + L".ini";
    return ini;
}

static bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool defaultValue)
{
    std::wstring ini = GetAppIniPath();
    int val = GetPrivateProfileIntW(section, key, defaultValue ? 1 : 0, ini.c_str());
    return val != 0;
}

static const int sideMargin = 0;
// Default visible rows for SDO parameter table (header + N rows)
static const int DEFAULT_SDO_VISIBLE_ROWS = 8;

static int GetScrollSafeRightInset()
{
    // Reserve room for the main vertical scrollbar plus a small visual gap
    // so right-aligned controls never overlap/cling to the scrollbar track.
    int vsw = GetSystemMetrics(SM_CXVSCROLL);
    if (vsw <= 0) vsw = 17;
    return sideMargin + vsw + 6;
}

namespace {
struct AdapterRowLayout {
    int comboW = 200;
    int buttonW = 100;
    int gap = 10;
};

struct CycleRowLayout {
    int desiredW = 80;
    int currentW = 160;
    int wkcW = 130;
    int pdoW = 200;
    int statusBtnW = 80;
    int autoBtnW = 80;
    int buttonGap = 8;
    int buttonOffset = 20;
};

struct HttpRowLayout {
    int buttonW = 110;
    int checkboxW = 150;
    int urlW = 200;
};

static AdapterRowLayout ComputeAdapterRowLayout(int availableW)
{
    AdapterRowLayout out{};
    if (availableW <= 0) return out;

    constexpr int kButtonCount = 4;
    constexpr int kGap = 10;
    constexpr int kDefaultBtnW = 100;
    constexpr int kMinBtnW = 44;
    constexpr int kPreferredComboW = 220;
    constexpr int kMinComboW = 80;

    out.gap = kGap;

    int buttonW = kDefaultBtnW;
    int comboW = availableW - ((buttonW * kButtonCount) + (kGap * (kButtonCount - 1)));
    if (comboW < kPreferredComboW) {
        int candidate = (availableW - kPreferredComboW - (kGap * (kButtonCount - 1))) / kButtonCount;
        buttonW = std::clamp(candidate, kMinBtnW, kDefaultBtnW);
        comboW = availableW - ((buttonW * kButtonCount) + (kGap * (kButtonCount - 1)));
    }
    if (comboW < kMinComboW) {
        comboW = kMinComboW;
        int candidate = (availableW - comboW - (kGap * (kButtonCount - 1))) / kButtonCount;
        buttonW = std::max(kMinBtnW, std::min(kDefaultBtnW, candidate));
    }
    if (buttonW < kMinBtnW) buttonW = kMinBtnW;
    comboW = availableW - ((buttonW * kButtonCount) + (kGap * (kButtonCount - 1)));
    if (comboW < kMinComboW) comboW = kMinComboW;

    const int totalGap = kGap * (kButtonCount - 1);
    if (comboW + (buttonW * kButtonCount) + totalGap > availableW) {
        buttonW = std::max(32, (availableW - totalGap) / 5);
        comboW = availableW - totalGap - (buttonW * kButtonCount);
        if (comboW < 60) {
            comboW = 60;
            buttonW = std::max(24, (availableW - totalGap - comboW) / kButtonCount);
        }
    }

    out.buttonW = buttonW;
    out.comboW = comboW;
    return out;
}

static CycleRowLayout ComputeCycleRowLayout(int availableW)
{
    CycleRowLayout out{};
    if (availableW <= 0) return out;

    // Buttons moved to adapter area — cycle row uses full width for data fields.
    // Row 1: desired edit (fixed 80px) + current display (fill remaining)
    // Row 2: three equal columns (PDOProc / WKC / MMF)
    out.statusBtnW = 0;
    out.autoBtnW = 0;
    out.buttonGap = 0;
    out.buttonOffset = 0;

    constexpr int kDesiredW = 80;
    constexpr int kMinDesired = 48;
    constexpr int kMinCurrent = 180;

    int desiredW = kDesiredW;
    if (availableW < kDesiredW + kMinCurrent + 100) {
        desiredW = kMinDesired;
    }
    int currentW = availableW - desiredW;
    if (currentW < kMinCurrent) currentW = kMinCurrent;

    // Row 2 columns are computed independently (3 equal columns)
    // wkcW and pdoW stored for backward compat but row 2 uses row2W/3
    int colW = availableW / 3;
    if (colW < 120) colW = 120;

    out.desiredW = desiredW;
    out.currentW = currentW;
    out.wkcW = colW;
    out.pdoW = colW;
    return out;
}

static HttpRowLayout ComputeHttpRowLayout(int rowW)
{
    HttpRowLayout out{};
    if (rowW <= 0) return out;

    constexpr int kGap = 8;
    constexpr int kMinButtonW = 96;
    constexpr int kMinCheckW = 130;
    constexpr int kMinUrlW = 160;

    int buttonW = out.buttonW;
    int checkW = out.checkboxW;
    int urlW = rowW - buttonW - kGap - checkW - kGap;
    if (urlW < kMinUrlW) {
        int shortage = kMinUrlW - urlW;
        int shrinkCheck = std::min(shortage, checkW - kMinCheckW);
        checkW -= shrinkCheck;
        shortage -= shrinkCheck;
        int shrinkButton = std::min(shortage, buttonW - kMinButtonW);
        buttonW -= shrinkButton;
        shortage -= shrinkButton;
        (void)shortage;
        urlW = rowW - buttonW - kGap - checkW - kGap;
    }

    out.buttonW = buttonW;
    out.checkboxW = checkW;
    out.urlW = std::max(100, urlW);
    return out;
}

static int ComputeStartupCheckboxWidth(int rowW)
{
    if (rowW <= 0) return 200;
    int w = (rowW - 8) / 2;
    return std::clamp(w, 170, 260);
}
} // namespace

// One-shot timer to re-focus the in-place editor after ListView click completes
#define TIMER_EDIT_FOCUS 5555

MainWindow::MainWindow()
    : hwnd(NULL)
    , hInstance(NULL)
    , contentHeight(0)
    , scrollPos(0)
    , workerRunning(false)
    , playSoftStartSound(true)
{
    uiThreadId = GetCurrentThreadId();
    // hTrayIcon will be loaded from embedded resource (ICON id 101) in Create()
    // 初始化 ESI 容器
    currentObjects.clear();
    currentPdos.clear();
    // 初始化新增的 HWND 成員
    hComboAdapter = NULL;
    hEditPDOSent = NULL;
    hEditPdoProcFreq = NULL;
    hBtnPDOSentStatus = NULL;
    hStaticRtCore = NULL;
    hStaticHttpUrl = NULL;
    hBtnOpenHttp = NULL;
    hChkHttpServer = NULL;
    hChkStatusLog = NULL;
    hChildStatusWnd = NULL;
    hChildStatusEdit = NULL;

    // Load persisted status.log setting from INI (default = enabled)
    statusLogEnabled_.store(ReadIniBool(L"Logging", L"EnableStatusLog", true));
    // Load external E-Stop enable setting from INI (default = enabled)
    externalEstopEnabled_.store(AppConfig::ReadExternalEstopEnabled());
    // MMF controls moved to AutoWindow; main window no longer holds HWNDs

    // 初始化暗色主題
    isDarkTheme = CoreHelpers::IsSystemDarkMode();
    if (isDarkTheme) {
        hbrDarkBackground = CreateSolidBrush(clrDarkBackground);
    }
    // 建立立即停止按鈕用的紅色 brush（明亮紅）
    hbrStopButton = CreateSolidBrush(RGB(220, 20, 20));
    // 自動程序按鈕樣式：啟動=綠 / 停止=橘
    hbrAutoStartButton = CreateSolidBrush(RGB(0, 200, 0));
    hbrAutoStopButton = CreateSolidBrush(RGB(255, 140, 0));
    // 初始化 MMF1 按鈕
    // MMF1 buttons created in AutoWindow

    // Start anomaly aggregator thread (coalesced anomaly logging)
    try {
        StartAnomalyAggregator();
    } catch (...) {
        // Ensure constructor doesn't throw if thread creation fails; log error if possible
        PostUTF8LogToUI(std::string("[ERROR] Failed to start anomaly aggregator thread\n"));
    }
}

static bool ShouldLogInputAnomalySummary(PDOFilter::InputAnomalyReason reason)
{
    return reason == PDOFilter::InputAnomalyReason::PDOLimitJump ||
           reason == PDOFilter::InputAnomalyReason::MeasurementMismatch;
}

// Aggregator: record a single anomaly occurrence (thread-safe)
void MainWindow::RecordInputAnomaly(int slaveIdx, double originalSet, double replacedSet, double inputChange, double maxAllowedChange, PDOFilter::InputAnomalyReason reason)
{
    InputAnomalySummary s;
    std::lock_guard<std::mutex> lock(anomalyMutex_);
    auto &mapByReason = anomalySummaries_[slaveIdx];
    auto &entry = mapByReason[reason];
    if (entry.count == 0) entry.firstTime = std::chrono::steady_clock::now();
    entry.lastTime = std::chrono::steady_clock::now();
    entry.count += 1;
    entry.lastOriginalSet = originalSet;
    entry.lastReplacedSet = replacedSet;
    entry.lastInputChange = inputChange;
    entry.lastMaxAllowedChange = maxAllowedChange;
}

void MainWindow::StartAnomalyAggregator()
{
    bool expected = false;
    if (!anomalyAggregatorRunning_.compare_exchange_strong(expected, true)) return; // already running

    anomalyAggregatorThread_ = std::thread([this]() {
        while (anomalyAggregatorRunning_.load()) {
            // Sleep between flushes (coalesce interval). Changed from 10s to 60s to reduce UI log spam.
            std::this_thread::sleep_for(std::chrono::seconds(60));

            std::map<int, std::map<PDOFilter::InputAnomalyReason, InputAnomalySummary>> copy;
            {
                std::lock_guard<std::mutex> lock(anomalyMutex_);
                if (anomalySummaries_.empty()) continue;
                copy.swap(anomalySummaries_);
            }

            for (auto &slavePair : copy) {
                int slaveIdx = slavePair.first;
                for (auto &reasonPair : slavePair.second) {
                    auto reason = reasonPair.first;
                    auto &e = reasonPair.second;
                    if (e.count <= 0 || !ShouldLogInputAnomalySummary(reason)) continue;
                    double duration = std::chrono::duration<double>(e.lastTime - e.firstTime).count();
                    std::ostringstream oss;
                    // Simplified summary: reason, count, last original->replaced (compact)
                    oss << "[PDO] Slave " << slaveIdx << " anomaly=" << PDOFilter::InputAnomalyReasonToString(reason)
                        << " x" << e.count
                        << " last=" << std::fixed << std::setprecision(6) << e.lastOriginalSet << "->" << e.lastReplacedSet << "\n";
                    PostUTF8LogToUI(oss.str());
                }
            }
        }
    });
}

void MainWindow::StopAnomalyAggregator()
{
    bool expected = true;
    if (!anomalyAggregatorRunning_.compare_exchange_strong(expected, false)) return; // not running

    // Prompt a final flush synchronously
    std::map<int, std::map<PDOFilter::InputAnomalyReason, InputAnomalySummary>> copy;
    {
        std::lock_guard<std::mutex> lock(anomalyMutex_);
        copy.swap(anomalySummaries_);
    }
    for (auto &slavePair : copy) {
        int slaveIdx = slavePair.first;
        for (auto &reasonPair : slavePair.second) {
            auto reason = reasonPair.first;
            auto &e = reasonPair.second;
            if (e.count <= 0 || !ShouldLogInputAnomalySummary(reason)) continue;
            double duration = std::chrono::duration<double>(e.lastTime - e.firstTime).count();
            std::ostringstream oss;
            // Simplified final summary: reason, count, last original->replaced (compact)
            oss << "[PDO] Slave " << slaveIdx << " anomaly=" << PDOFilter::InputAnomalyReasonToString(reason)
                << " x" << e.count
                << " last=" << std::fixed << std::setprecision(6) << e.lastOriginalSet << "->" << e.lastReplacedSet << "\n";
            PostUTF8LogToUI(oss.str());
        }
    }

    if (anomalyAggregatorThread_.joinable()) {
        anomalyAggregatorThread_.join();
    }
}

bool MainWindow::IsConnectAttemptInProgress() const { return connectAttemptInProgress.load(); }

std::wstring MainWindow::GetStatusBuffer() {
    return statusLogger.GetBuffer();
}

std::wstring MainWindow::GetStatusBufferChronological() {
    return statusLogger.GetBufferChronological();
}

bool MainWindow::IsStatusUserScrolled() const {
    return statusUserScrolled;
}

void MainWindow::MarkStatusUiProcessed() {
    statusLogger.MarkUiUpdateProcessed();
}

bool MainWindow::HasAnyTransientBusyFlag(const std::atomic<bool>* ignoredFlag) const {
    const auto isBusy = [ignoredFlag](const std::atomic<bool>& flag) {
        return (&flag != ignoredFlag) && flag.load();
    };

    return isBusy(disconnecting) || isBusy(homingBusy) || isBusy(autoDownToOriginRunning) ||
           isBusy(autoConnectBusy) || isBusy(jogBusy) || isBusy(faultResetBusy) ||
           isBusy(immediateStopBusy) || isBusy(powerOpBusy);
}

CoreHelpers::SystemStateSnapshot MainWindow::GetSystemStateSnapshot() const
{
    CoreHelpers::SystemStateSnapshot snapshot;

    snapshot.master_initialized = IsMasterInitialized();
    snapshot.ethercat_ok = snapshot.master_initialized && !ethercatError.load();
    snapshot.worker_running = workerRunning.load();
    snapshot.busy = snapshot.worker_running || HasAnyTransientBusyFlag();
    snapshot.manual_reset_required = this->manualResetRequired.load();

    auto* master = AppController::GetInstance().GetMaster();
    snapshot.error =
        (master ? master->HasGlobalError() : false) || this->anySlaveHasInternalError.load();
    snapshot.ready = AreAllSlavesReadyByStatusword();

    if (master && snapshot.ethercat_ok) {
        try {
            snapshot.online = master->IsPDOFilteringEnabled();
        } catch (...) {
            snapshot.online = false;
        }
    } else {
        snapshot.online = false;
    }

    snapshot.home_done = this->autoDownToOriginLastSuccess.load();
    // Only clear home_done on EtherCAT loss — not on transient errors or
    // human-operated stops.  autoDownToOriginLastSuccess is the sole
    // authority and is already cleared by actual homing failures.
    if (!snapshot.ethercat_ok) snapshot.home_done = false;

    return snapshot;
}

CoreHelpers::AutoActionButtonInputs MainWindow::GetAutoActionButtonInputsForWeb() const
{
    CoreHelpers::AutoActionButtonInputs inputs;
    inputs.action_gate_enabled = IsAutoProcedureUiLockActive();
    uint8_t pdo60FD = 0;
    bool have60FD = false;
    if (TryGetPdo60FDLowByte(pdo60FD)) {
        have60FD = true;
    } else {
        const int cached = lastPdo60FDLastByte.load();
        if (cached != -1) {
            have60FD = true;
            pdo60FD = static_cast<uint8_t>(cached & 0xFF);
        }
    }
    if (have60FD) {
        inputs.auto_connect_signal_active = (pdo60FD & 0x02) != 0;
        inputs.down_to_origin_signal_active = (pdo60FD & 0x08) != 0;
        inputs.fault_reset_signal_active = (pdo60FD & 0x40) != 0;
        inputs.immediate_stop_signal_active = (pdo60FD & 0x20) != 0;
        inputs.emergency_stop_signal_active = externalEstopEnabled_.load() && ((pdo60FD & 0x01) == 0);
    }
    inputs.immediate_stop_latched = autoImmediateStopLampLatched.load();
    inputs.emergency_stop_latched = autoEmergencyStopLampLatched.load();
    inputs.stop_active = inputs.immediate_stop_signal_active ||
                         inputs.emergency_stop_signal_active ||
                         inputs.immediate_stop_latched ||
                         inputs.emergency_stop_latched;
    return inputs;
}

void MainWindow::GetAutoGlobalStatus(
    bool &oEtherCatOk,
    bool &oBusy,
    bool &oError,
    bool &oReady,
    bool &oHomeDone,
    bool &oOnline
) const
{
    const auto snapshot = GetSystemStateSnapshot();
    oEtherCatOk = snapshot.ethercat_ok;
    oBusy = snapshot.busy;
    oError = snapshot.error;
    oReady = snapshot.ready;
    oHomeDone = snapshot.home_done;
    oOnline = snapshot.online;
}

void MainWindow::SetPlaySoftStartSound(bool enable) {
    playSoftStartSound.store(enable ? true : false);
}

bool MainWindow::IsAutoProcedureRunning() const {
    return this->autoProcedureRunning.load();
}

bool MainWindow::IsWorkerRunning() const {
    return this->workerRunning.load();
}

bool MainWindow::IsBusyOnlyBecauseConnect() const {
    // True if workerRunning is set and none of the other transient busy flags
    // or long-running auto/procedure/thread conditions are active.
    if (!workerRunning.load()) return false;
    if (HasAnyTransientBusyFlag()) return false;
    if (IsAutoProcedureRunning()) return false;
    if (MasterHasActiveSlaveThreads()) return false;
    return true;
}

bool MainWindow::IsSafeToDisconnectForWeb() const {
    // Mirror PC behaviour: if it's safe to disconnect generally, allow it.
    if (IsSafeToDisconnect()) return true;
    // Otherwise allow only when the only busy condition is the connect worker
    // (i.e. we're in the connect flow and can be interrupted safely).
    if (IsBusyOnlyBecauseConnect()) return true;
    return false;
}

bool MainWindow::IsSafeToDisconnect() const {
    // If master isn't initialized, disconnect is trivially safe.
    if (!IsMasterInitialized()) return true;

    // Check UI-level busy flags (homing, immediate stop, connect/disconnect flows, etc.)
    const auto snapshot = GetSystemStateSnapshot();

    // Consider it unsafe when any busy flag is active, or the higher-level auto procedure is running,
    // or when cyclic threads are active (MasterHasActiveSlaveThreads).
    if (snapshot.busy) return false;
    if (IsAutoProcedureRunning()) return false;
    if (MasterHasActiveSlaveThreads()) return false;
    return true;
}

std::string MainWindow::GetDisconnectUnsafeReason() const {
    // If master isn't initialized, disconnect is trivially safe.
    if (!IsMasterInitialized()) return "";

    // Check UI-level busy flags
    const auto snapshot = GetSystemStateSnapshot();

    std::string reasons;
    if (snapshot.busy) reasons += "oBusy ";
    if (IsAutoProcedureRunning()) reasons += "autoProcedure ";
    if (MasterHasActiveSlaveThreads()) reasons += "slaveThreads ";
    
    return reasons;
}

bool MainWindow::GetPlaySoftStartSound() const {
    return playSoftStartSound.load();
}

// Stop the system alert sound only when no active component requires it.
// This prevents one component from silencing another component's loop sound.
void MainWindow::StopSystemAlertSoundIfIdle() {
    if (!this->violationSoundPlaying_.load() &&
        !this->softStartSoundPlaying_.load() &&
        !this->errorSoundPlaying_.load()) {
        try { PlaySound(NULL, NULL, 0); } catch(...) {}
    }
}

// Out-of-line to avoid incomplete type access in header
bool MainWindow::IsMasterInitialized() const {
    if (auto* svc = AppController::GetInstance().GetEtherCATService()) {
        return svc->IsInitialized();
    }
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->IsInitialized();
    }
    return false;
}

bool MainWindow::TryGetPdo60FD(std::vector<uint8_t>& out) const {
    if (auto* svc = AppController::GetInstance().GetEtherCATService()) {
        return svc->GetPdo60FD(out);
    }
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->GetPdo60FD(out);
    }
    return false;
}

bool MainWindow::TryGetPdo60FDLowByte(uint8_t& outLowByte) const {
    if (auto* svc = AppController::GetInstance().GetEtherCATService()) {
        return svc->GetPdo60FDLowByte(outLowByte);
    }
    if (auto* master = AppController::GetInstance().GetMaster()) {
        return master->GetPdo60FDLowByte(outLowByte);
    }
    return false;
}

void MainWindow::GetLastReportedErrorCodes(std::array<uint16_t, 6>& outCodes,
                                           std::array<bool, 6>& outValid) {
    outCodes.fill(0);
    outValid.fill(false);

    std::lock_guard<std::mutex> lock(errorCodeStateMutex);
    for (int i = 0; i < 6; ++i) {
        const int slaveIndex = i + 1;
        auto it = lastReportedErrorCode.find(slaveIndex);
        if (it != lastReportedErrorCode.end()) {
            const auto counterIt = errorCodeConsecutiveCounter.find(slaveIndex);
            const bool latestReadFailed = counterIt != errorCodeConsecutiveCounter.end() && counterIt->second > 0;
            outCodes[i] = it->second;
            outValid[i] = !latestReadFailed && it->second != 0xFFFF;
        }
    }
}

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

int MainWindow::CalculateSlaveListHeightForRows(int visibleRows) const {
    if (visibleRows <= 0) visibleRows = 1;
    int rowHeight = MeasureTextHeight(hGuiFont);
    if (rowHeight <= 0) rowHeight = LayoutConfig::rowH;

    int headerH = rowHeight + 6;
    if (hListSlaves && IsWindow(hListSlaves)) {
        HWND hHeader = ListView_GetHeader(hListSlaves);
        if (hHeader && IsWindow(hHeader)) {
            RECT rcHeader{};
            GetWindowRect(hHeader, &rcHeader);
            headerH = rcHeader.bottom - rcHeader.top;
        }
        RECT rcItem{};
        if (ListView_GetItemCount(hListSlaves) > 0 &&
            ListView_GetItemRect(hListSlaves, 0, &rcItem, LVIR_BOUNDS)) {
            int actualRow = rcItem.bottom - rcItem.top;
            if (actualRow > 0) rowHeight = actualRow;
        }
    }
    if (headerH <= 0) headerH = rowHeight + 6;
    const int padding = 6;
    return headerH + (rowHeight * visibleRows) + padding;
}

MainWindow::~MainWindow() {
    // Stop anomaly aggregator thread
    StopAnomalyAggregator();

    // Stop CPU isolation monitor
    CoreHelpers::CpuIsolation::StopIsolationMonitor();

    // 停止 0x603F 錯誤碼循環檢查
    StopErrorCodeCheck();
    
    // stop local http server if running
    StopLocalHttpServer();
    // Stop background MMF reader
    // MMFReader is now owned/managed by EtherCATMaster. If the master exists,
    // request it to stop the MMF1_IN reader; otherwise fall back to stopping
    // the local mmfReader instance (for older configs or during early shutdown).
    if (auto* master = AppController::GetInstance().GetMaster()) {
        master->StopMMF1In();
    } else {
        // local mmfReader removed; nothing to stop here
    }
    // MMF state now managed by AutoWindow / EtherCATMaster
    if (hGuiFont) {
        // remove font prop so auxiliaries don't reference freed object
        if (hwnd) RemovePropW(hwnd, L"BigCatGuiFont");
        DeleteObject(hGuiFont);
        hGuiFont = NULL;
    }
    if (hbrDarkBackground) {
        // remove the window properties so other windows don't reference freed resources
        if (hwnd) RemovePropW(hwnd, L"BigCatBgBrush");
        if (hwnd) RemovePropW(hwnd, L"BigCatBgColor");
        DeleteObject(hbrDarkBackground);
        hbrDarkBackground = NULL;
    }
    
    // Ensure aggregator thread has stopped before process exit
    StopAnomalyAggregator();
    if (hbrStopButton) {
        DeleteObject(hbrStopButton);
        hbrStopButton = NULL;
    }
    if (hbrAutoStartButton) {
        DeleteObject(hbrAutoStartButton);
        hbrAutoStartButton = NULL;
    }
    if (hbrAutoStopButton) {
        DeleteObject(hbrAutoStopButton);
        hbrAutoStopButton = NULL;
    }
    if (hChildStatusWnd && IsWindow(hChildStatusWnd)) {
        DestroyWindow(hChildStatusWnd);
        hChildStatusWnd = NULL;
        hChildStatusEdit = NULL;
    }
    // Remove tray icon and destroy loaded hTrayIcon
    // Ensure the notification icon is removed first, then destroy the icon resource
    RemoveTrayIcon();
    if (hTrayIcon) {
        DestroyIcon(hTrayIcon);
        hTrayIcon = NULL;
    }
    // Ensure any redraw throttle timer is removed
    if (hwnd) KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
    // motionController is managed by std::unique_ptr and will be cleaned up automatically
}

bool MainWindow::Create(HINSTANCE hInst, bool showInitially) {
    hInstance = hInst;

    // 嘗試將整個 process 綁定到 CPU0 和 CPU1（若系統支援）
    // 目的：確保關鍵執行緒在指定的邏輯 CPU 上執行。
    // 若系統上可用的處理器數量小於 2，會退回到可用的 mask。
    {
    // Best-effort: set process affinity to CPU0+CPU1 using core helper
    // Set process to high priority and lock to Core 4 (or available core > 0) to avoid system interrupts
    CoreHelpers::SetProcessHighPriorityAndCore4Affinity();
    
    // Lower the main UI message-loop thread to BELOW_NORMAL so it never
    // preempts the CyclicTask (TIME_CRITICAL) or PDOProcessingTask threads.
    // The process class is HIGH/REALTIME, so BELOW_NORMAL = base-1 = 12,
    // well below TIME_CRITICAL at base+15 = 28.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    
    // Start background monitor to repel high-load games (DCS/VR) from our core
    CoreHelpers::CpuIsolation::StartIsolationMonitor();
    }

    // 初始化 Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // 註冊視窗類別
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = isDarkTheme ? hbrDarkBackground : (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"EtherCATWindowClass";
    // Load ICON resource (ID 101) and set as class icon (large and small)
    HICON hLarge = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
    HICON hSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    if (hLarge) wc.hIcon = hLarge; else wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    if (hSmall) wc.hIconSm = hSmall; else wc.hIconSm = (HICON)LoadImage(NULL, IDI_APPLICATION, IMAGE_ICON, 16, 16, LR_SHARED);

    // preserve a copy for tray usage
    if (!hTrayIcon) {
        if (hLarge) hTrayIcon = CopyIcon(hLarge);
        else hTrayIcon = CopyIcon(wc.hIcon);
    }

    if (!RegisterClassEx(&wc)) {
        return false;
    }

    // 創建視窗（使用寬字元字串避免中文字在不同編碼設定下亂碼）
    wchar_t titleBuf[256] = {0};
    swprintf_s(titleBuf, L"BigCat \u7acb\u6d0b\u79d1\u6280 wuyenhung@gmail.com built: %hs %hs", __DATE__, __TIME__);

    // Create main window (default scrollbar on right side)
    hwnd = CreateWindowEx(
        WS_EX_WINDOWEDGE,
        L"EtherCATWindowClass",
        titleBuf,
        WS_OVERLAPPEDWINDOW,  // 允許調整大小
        CW_USEDEFAULT, CW_USEDEFAULT,
        1080, 1080,  // 初始視窗大小
        NULL, NULL, hInstance, this
    );

    if (!hwnd) {
        return false;
    }

    // If we have a dark theme brush, expose it as a window property so
    // auxiliary windows (AutoWindow / StatusPane) can adopt the same background.
    if (isDarkTheme && hbrDarkBackground) {
        SetPropW(hwnd, L"BigCatBgBrush", (HANDLE)hbrDarkBackground);
        // Expose a raw color value so other compilation units can create
        // their own HBRUSH from the color (avoids sharing HBRUSH across TUs).
        SetPropW(hwnd, L"BigCatBgColor", (HANDLE)(UINT_PTR)clrDarkBackground);
    }

    // 設為最上層但暫不顯示，等完成所有子控制項建立與首次重排後再顯示，
    // 以避免使用者在初始階段看到重疊/跳動的畫面。
    SafeSetPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

    CreateControls();
    // 初次重排已在 CreateControls() 末端呼叫 ReflowMainControls() 完成
    // 只有在啟動模式需要顯示主視窗時才立即顯示，避免 tray 模式先閃現再最小化。
    if (showInitially) {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    // Start local HTTP control server only when enabled in INI
    bool httpEnabled = ReadIniBool(L"Server", L"EnableLocalHttpServer", false);
    if (httpEnabled) {
        StartLocalHttpServer(8080, hwnd);
        OpenHttpOperationInterface(false, true, true);
    }
    // Update displayed HTTP URL immediately after server start (or remain stopped)
    UpdateHttpServerUrl();

    // ========================================================================
    // \u95dc\u6a5f / \u91cd\u65b0\u555f\u52d5\u9632\u8b77\uff1a\u8a3b\u518a shutdown block reason
    // \u4e4b\u5f8c\u4efb\u4f55\u95dc\u6a5f\u3001\u91cd\u65b0\u555f\u52d5\u6216\u767b\u51fa\u90fd\u6703\u5148\u8a62\u554f\u672c\u8996\u7a97\uff08WM_QUERYENDSESSION\uff09\uff0c
    // \u7531 ShutdownGuard \u6c7a\u5b9a\u662f\u5426\u6514\u622a\u3002\u958b\u95dc\u70ba BigCat.ini [Safety] BlockSystemShutdown\u3002
    // ========================================================================
    {
        std::string guardLog;
        const bool reasonInstalled = CoreHelpers::ShutdownGuard::Install(hwnd, guardLog);
        if (!guardLog.empty()) {
            OutputDebugStringA((guardLog + "\n").c_str());
        }
        if (!CoreHelpers::ShutdownGuard::IsEnabled()) {
            PrependStatus(L"[Safety] \u95dc\u6a5f\u5c01\u9396\u672a\u555f\u7528\uff08INI [Safety] BlockSystemShutdown=0\uff09\r\n");
        } else if (reasonInstalled) {
            PrependStatus(L"[Safety] \u2713 \u5df2\u5c01\u9396 Windows \u95dc\u6a5f/\u91cd\u65b0\u555f\u52d5\uff08\u7a0b\u5f0f\u57f7\u884c\u671f\u9593\uff1b\u8acb\u5148\u95dc\u9589 BigCat \u518d\u95dc\u6a5f\uff09\r\n");
        } else {
            // \u5c01\u9396\u4ecd\u6709\u6548\uff08WM_QUERYENDSESSION \u56de FALSE\uff09\uff0c\u53ea\u662f\u7cfb\u7d71\u95dc\u6a5f\u756b\u9762\u7121\u6cd5\u986f\u793a\u539f\u56e0\u3002
            PrependStatus(L"[Safety] \u2713 \u5df2\u5c01\u9396 Windows \u95dc\u6a5f/\u91cd\u65b0\u555f\u52d5\uff08\u5c01\u9396\u539f\u56e0\u6587\u5b57\u8a3b\u518a\u5931\u6557\uff09\r\n");
        }
        PrependStatus(CoreHelpers::ShutdownGuard::IsKeepSystemAwake()
            ? L"[Safety] \u2713 \u5df2\u9632\u6b62\u7cfb\u7d71\u9592\u7f6e\u7761\u7720\uff08\u87a2\u5e55\u4ecd\u53ef\u95dc\u9589\uff09\r\n"
            : L"[Safety] \u9592\u7f6e\u7761\u7720\u9632\u8b77\u672a\u555f\u7528\uff08INI [Safety] KeepSystemAwake=0\uff09\r\n");
    }

    // Use PrependStatus so file-write respects the runtime INI/UI toggle
    PrependStatus(L"[OK] \u5df2\u5b8c\u6210\u521d\u59cb\u5316\r\n");

    // Start UI timer even before EtherCAT connect so MMF UI/診斷能持續更新
    if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
    // Trigger one immediate tick to update UI
    OnTimer();
    // Initialize adapter select as enabled
    SetUiDisabled("adapter_select", false);
    return true;
}

void MainWindow::CreateControls() {
    // Use LayoutHelper to progressively migrate from manual x/y math
    // Keep existing constants for compatibility with older code.
    // We'll use helper to advance rows and compute positions; y is kept
    // for portions of code not yet migrated.
    // layout constants (aligned columns) - use centralized LayoutConfig
    const int left = LayoutConfig::left;
    // symmetric side margin applied to left and right content edges (use file-local `sideMargin`)
    const int labelW = LayoutConfig::labelW;
    const int rowH = LayoutConfig::rowH;
    const int vpad = LayoutConfig::vpad;  // 每列之間保留 8px 邊界感

    // Determine client width and height for dynamic allocation
    RECT clientRc;
    GetClientRect(hwnd, &clientRc);
    int clientW = clientRc.right - clientRc.left;
    int clientH = clientRc.bottom - clientRc.top;
    const int rightInset = GetScrollSafeRightInset();
    int contentWidth = (clientW - left - rightInset) < 400 ? 400 : (clientW - left - rightInset); // available space for content
    // ctrlW is the main control column width; keep a reasonable max and min
    int tmpCtrl = (contentWidth - labelW - 200);
    if (tmpCtrl < 220) tmpCtrl = 220;
    int ctrlW = (tmpCtrl < 540) ? tmpCtrl : 540;

    // Calculate dynamic heights based on available client height
    // Reserve space for fixed elements (adapters, cycle time, ESI, slave selector)
    const int fixedTopHeight = 120; // approximate space for adapter + cycle time + ESI + slave selector
    const int fixedBottomHeight = 50; // space for status area
    int availableContentHeight = clientH - fixedTopHeight - fixedBottomHeight;
    if (availableContentHeight < 360) availableContentHeight = 360; // minimum height (slightly reduced)

    // Allocate heights proportionally
    int slaveListHeight = cachedSlaveListHeight > 0
        ? cachedSlaveListHeight
        : CalculateSlaveListHeightForRows(SLAVE_LIST_VISIBLE_ROWS);
    // SDO table height: make it show 10 visible rows by default (header + 10 rows + padding)
    int sdoTableHeight = 0;
    {
        // estimate row height from GUI font
        int rowHeight = MeasureTextHeight(hGuiFont);
        int headerH = rowHeight + 6; // small extra for header
        sdoTableHeight = headerH + (rowHeight * DEFAULT_SDO_VISIBLE_ROWS) + 10; // padding
    }
    if (cachedSDOListHeight > 0) {
        sdoTableHeight = cachedSDOListHeight;
    }
    int pdoHeight = (int)(availableContentHeight * 0.22f);       // 22% for PDO display (reduced)
    int mailboxHeight = (int)(availableContentHeight * 0.12f);   // 12% for mailbox

    // Ensure minimum heights
    if (slaveListHeight < 100) slaveListHeight = 100;
    // Limit slave list to at most 6 visible rows (estimate row height from GUI font)
    {
        int maxRows = 6;
        int rowHeight = MeasureTextHeight(hGuiFont);
        // Header height estimate
        int headerH = rowHeight + 6;
        int maxListH = headerH + (rowHeight * maxRows) + 10; // add safety padding
        if (slaveListHeight > maxListH) slaveListHeight = maxListH;
    }
    // Keep a smaller minimum after halving to avoid excessive clipping
    if (sdoTableHeight < 70) sdoTableHeight = 70;
    if (pdoHeight < 100) pdoHeight = 100;
    if (mailboxHeight < 36) mailboxHeight = 36;

    LayoutHelper helper(left, labelW, ctrlW, rowH, vpad);
    int y = helper.Y();

    // Create a content pane that will host the majority of UI controls.
    // This allows scrolling by moving the content pane instead of individually
    // offsetting every control. We'll size it initially to client area.
    if (!hContentWnd) {
        // Add WS_CLIPCHILDREN to reduce redraw/overdraw artifacts when
        // child controls are repositioned during window resize/scroll.
        hContentWnd = CreateChildControlEx(WS_EX_CLIENTEDGE, hwnd, hInstance, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, clientW, clientH, 0, NULL);
        if (hContentWnd) {
            // Subclass content pane so it forwards commands/notifications to main window
            SetWindowSubclass(hContentWnd, ContentPaneSubclassProc, 1, (DWORD_PTR)this);
        }
    } else {
        // Ensure it's sized to client area (only when content pane exists)
        if (hContentWnd != hwnd) {
            SafeSetPos(hContentWnd, NULL, 0, 0, clientW, clientH, SWP_NOZORDER | SWP_SHOWWINDOW);
        }
    }
    // Fallback: if content pane creation failed, attach controls to main window
    if (!hContentWnd || !IsWindow(hContentWnd)) {
        hContentWnd = hwnd;
    }

    // Create a DPI-aware font that supports Chinese and Latin glyphs
    // Prefer Microsoft JhengHei UI (common CJK + Latin), fallback to DEFAULT_GUI_FONT
    if (!hGuiFont) {
        int dpi = 96;
        typedef UINT(WINAPI *GetDpiForWindow_t)(HWND);
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (user32) {
            GetDpiForWindow_t pGetDpiForWindow = (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow");
            if (pGetDpiForWindow) dpi = (int)pGetDpiForWindow(hwnd);
            FreeLibrary(user32);
        }
        // desired point size
        const int pt = 10;
        int lfHeight = -MulDiv(pt, dpi, 72);
        // Try to create Microsoft JhengHei UI first
        hGuiFont = CreateFontW(lfHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               VARIABLE_PITCH, L"Microsoft JhengHei UI");
        if (!hGuiFont) {
            hGuiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        }
        // Expose the GUI font so auxiliary windows can adopt the same font
        if (hGuiFont && hwnd) SetPropW(hwnd, L"BigCatGuiFont", (HANDLE)hGuiFont);
    }

    // Adapter + Connect controls
    CreateAdapterControls(helper, y, hGuiFont);
    // 移動到下一行（CreateAdapterControls 已處理 NextRow）
    y = helper.Y();

    // Cycle controls (Distributed columns + buttons)
    CreateCycleControls(helper, y, clientW, hGuiFont);
    y = helper.Y();
    
    // Slave list + selector
    CreateSlaveListControls(helper, y, clientW, slaveListHeight, hGuiFont, ctrlW);
    // S l a v e  & S D O controls
    CreateSDOControls(helper, y, clientW, sdoTableHeight, hGuiFont, left, labelW, ctrlW);

    // 計算 Mailbox 的 Y 位置：依照實際 SDO ListView 的底部來定位，避免上方空白
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcList{};
        if (GetWindowRect(hListSDOParams, &rcList)) {
            POINT ptBottom = { rcList.left, rcList.bottom };
            HWND anchorWnd = (hContentWnd && IsWindow(hContentWnd)) ? hContentWnd : hwnd;
            if (anchorWnd) ScreenToClient(anchorWnd, &ptBottom);
            // 最小間距 0px，完全緊貼
            y = ptBottom.y + 0;
        } else {
            // fallback to helper cursor
            y = helper.Y();
        }
    } else {
        y = helper.Y();
    }

    // Mailbox, PDO and Motion sections extracted to helpers
    CreateMailboxControls(helper, y, clientW, mailboxHeight, hGuiFont, labelW, contentWidth);
    CreatePDOControls(helper, y, clientW, pdoHeight, hGuiFont, left, labelW, ctrlW, contentWidth);
    CreateMotionControls(helper, y, clientW, pdoHeight, hGuiFont, left, labelW, ctrlW);
    // 運動參數表格由 CreateMotionControls() 負責建立，避免在此重複建立導致重疊

    
    // Conditional checks elsewhere (if (hListIOmap && IsWindow(hListIOmap))) prevent use of this control.

    // Status text box (full width, 調整高度更緊湊)
    // Status text box (full width, 調整高度更緊湊)
    statusLogger.Clear(false, hChildStatusEdit, hChildStatusWnd);
    statusUserScrolled = false;
    
    // Show N/A hint in slave list when not connected
    LVITEM lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = 0;
    lvi.iSubItem = 0;
    // Slave list remains empty until UpdateSlaveList() populates it; do not insert placeholder rows here.

    // Populate lists after all controls are created

    // Local helper implementation should be in the header to avoid splitting
    // the large CreateControls() function in this CPP file.
    
    // Ensure initial controls are reflowed to current client width
    ReflowMainControls();
    
    // Note: showing/positioning of the slave list is handled by ReflowMainControls()
    // Reflow handles visibility; avoid duplicate Show/Update calls
    // here to keep a single, final place that controls visibility/positioning.
}

// Helper: create adapter selection and connect/disconnect buttons
void MainWindow::CreateAdapterControls(LayoutHelper &helper, int &y, HFONT hFont) {
    RECT rLabel = helper.LabelRect();
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"網路介面卡:", WS_VISIBLE | WS_CHILD,
        rLabel.left, rLabel.top, LayoutConfig::labelW, 20, 0, hFont);
    int ctrlXpos = helper.ControlX();

    AdapterRowLayout adapterLayout = ComputeAdapterRowLayout(helper.ControlW());
    const int adapterGap = adapterLayout.gap;
    const int btnConnectW = adapterLayout.buttonW;
    const int btnDisconnectW = adapterLayout.buttonW;
    const int btnAutoStartW = adapterLayout.buttonW;
    const int btnAutoStopW = adapterLayout.buttonW;
    int adapterComboW = adapterLayout.comboW;

    hComboAdapter = CreateChildControl(hContentWnd, hInstance, WC_COMBOBOX, NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
        ctrlXpos, helper.Y(), adapterComboW, 200, IDC_EDIT_ADAPTER, hFont);
    PopulateAdapters();

    int adapterBtnX = ctrlXpos + adapterComboW + adapterGap;
    hBtnConnect = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"連接", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnConnectW, 22, IDC_BTN_CONNECT, hFont);
    adapterBtnX += btnConnectW + adapterGap;
    hBtnDisconnect = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"斷開", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnDisconnectW, 22, IDC_BTN_DISCONNECT, hFont);
    SetControlEnabled(hBtnDisconnect, false);
    adapterBtnX += btnDisconnectW + adapterGap;
    hButtonStartAutoMode = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"自動程序啟動", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnAutoStartW, 22, IDC_BTN_AUTO_MODE, hFont);
    SetControlEnabled(hButtonStartAutoMode, false);
    adapterBtnX += btnAutoStartW + adapterGap;
    hBtnStopAutoMode = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"\u81ea\u52d5\u7a0b\u5e8f\u505c\u6b62", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnAutoStopW, 22, IDC_BTN_MANUAL_MODE, hFont);
    SetControlEnabled(hBtnStopAutoMode, false);
    // Reflect initial start/stop disabled state to server UI state
    SetUiDisabled("start_auto", true);
    SetUiDisabled("stop_auto", true);

    // 狀態視窗 / 自動視窗 buttons — placed on their own row, right-aligned
    {
        const int btnW = 80;
        const int btnGap = 8;
        int totalBtnW = btnW + btnGap + btnW;
        int btnX = ctrlXpos + helper.ControlW() - totalBtnW;
        if (btnX < ctrlXpos) btnX = ctrlXpos;
        hBtnPDOSentStatus = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"狀態視窗", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, helper.Y(), btnW, 22, IDC_BTN_PDO_SENT_STATUS, hFont);
        btnX += btnW + btnGap;
        hBtnAutoWindow = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"自動視窗", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, helper.Y(), btnW, 22, IDC_BTN_AUTO_WINDOW, hFont);
        SetControlEnabled(hBtnAutoWindow, true);
        hBtnClearStatus = NULL;
    }
    helper.NextRow(); y = helper.Y();

    // IRQ CPU display for selected adapter (shows effective IRQ CPU when connected)
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"選定網卡 IRQ:", WS_VISIBLE | WS_CHILD,
        rLabel.left, helper.Y(), LayoutConfig::labelW, 18, 0, hFont);
    hStaticAdapterIrqCpu = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"N/A", WS_VISIBLE | WS_CHILD | SS_LEFT,
        ctrlXpos, helper.Y(), helper.ControlW(), 36, IDC_STATIC_ADAPTER_IRQ, hFont);
    // Multi-line IRQ display needs extra row advancement to avoid overlap with next row
    helper.MoveDown(18);
    helper.NextRow(); y = helper.Y();

    // Show actual effective other IRQ assignments on the main screen
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"其他 IRQ:", WS_VISIBLE | WS_CHILD,
        rLabel.left, helper.Y(), LayoutConfig::labelW, 18, 0, hFont);
    hStaticOtherIrqAssignments = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"N/A", WS_VISIBLE | WS_CHILD | SS_LEFT,
        ctrlXpos, helper.Y(), helper.ControlW(), 36, IDC_STATIC_OTHER_IRQS, hFont);
    // Multi-line IRQ display needs extra row advancement to avoid overlap with next row
    helper.MoveDown(18);
    helper.NextRow(); y = helper.Y();

    // RT core display — auto-detected physical core for SOEM cyclic thread
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"RT 核心:", WS_VISIBLE | WS_CHILD,
        rLabel.left, helper.Y(), LayoutConfig::labelW, 18, 0, hFont);
    hStaticRtCore = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"偵測中...", WS_VISIBLE | WS_CHILD | SS_LEFTNOWORDWRAP,
        ctrlXpos, helper.Y(), helper.ControlW(), 18, IDC_STATIC_RT_CORE, hFont);
    helper.NextRow(); y = helper.Y();
    helper.NextRow(); y = helper.Y();

    // PopulateAdapters() above may have called SetSelectedAdapter() before these
    // controls existed.  Force an IRQ display update for the currently selected
    // adapter so the startup view shows actual IRQ info immediately.
    PostMessageW(hwnd, WM_USER_REFRESH_IRQ_DISPLAY, 0, 0);
}

// Helper: create cycle row controls — split across two rows (buttons moved to adapter area)
void MainWindow::CreateCycleControls(LayoutHelper &helper, int &y, int clientW, HFONT hFont) {
    const int left = LayoutConfig::left;
    const int labelW = LayoutConfig::labelW;

    int rowX = helper.ControlX();
    const int rightInset = GetScrollSafeRightInset();
    int rowW = clientW - rowX - rightInset;
    if (rowW < 400) rowW = 400;
    CycleRowLayout cycleLayout = ComputeCycleRowLayout(rowW);
    int desiredW = cycleLayout.desiredW;
    int currentW = cycleLayout.currentW;

    // --- Row 1: 設定期望週期 + 即時通訊與週期量測 ---
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"設定期望週期 (ms):", WS_VISIBLE | WS_CHILD, left, helper.Y(), labelW, 20, 0, hFont);

    int x = rowX;
    int desiredEditW = std::max(32, desiredW - 8);
    hEditCycleTime = CreateChildControl(hContentWnd, hInstance, L"EDIT", L"2", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, x, helper.Y(), desiredEditW, 22, IDC_EDIT_CYCLE_TIME, hFont);
    x += desiredW;

    int curLabelW = 120;
    int curEditW = std::max(8, currentW - curLabelW - 6);
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"即時通訊與週期量測:", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), curLabelW, 22, IDC_STATIC_CYCLE_ACTUAL_LABEL, hFont);
    hEditCurrentCycleTime = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"N/A", WS_VISIBLE | WS_CHILD | SS_SIMPLE | SS_LEFT, x + 4 + curLabelW + 6, helper.Y(), curEditW, 22, IDC_EDIT_CURRENT_CYCLE_TIME, hFont);

    helper.NextRow(); y = helper.Y();

    // --- Row 2: PDOProc 頻率 + 工作計數器 (WKC) + MMF 更新頻率 ---
    // Column widths weighted by label length: PDOProc 8 : WKC 11 : MMF 9
    int row2W = rowW;
    const int weightSum = 8 + 11 + 9;
    int colW[3] = {
        row2W * 8 / weightSum,
        row2W * 11 / weightSum,
        row2W * 9 / weightSum
    };
    // Distribute rounding remainder to the widest column (WKC)
    int colSum = colW[0] + colW[1] + colW[2];
    if (colSum < row2W) colW[1] += (row2W - colSum);
    // Enforce minimum per-column to prevent text clipping/wrapping
    for (int i = 0; i < 3; ++i) if (colW[i] < 180) colW[i] = 180;

    x = rowX;

    // PDOProc 頻率 (col 0)
    int pdoProcLabelW = 90;
    int pdoProcEditW = std::max(8, colW[0] - pdoProcLabelW - 6);
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"PDOProc 頻率:", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), pdoProcLabelW, 22, IDC_STATIC_PDO_PROC_LABEL, hFont);
    hEditPdoProcFreq = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"等待中...", WS_VISIBLE | WS_CHILD | SS_SIMPLE | SS_LEFT, x + 4 + pdoProcLabelW + 6, helper.Y(), pdoProcEditW, 22, IDC_EDIT_PDO_PROC_FREQ, hFont);
    x += colW[0];

    // 工作計數器 (WKC) (col 1) — wider label to prevent wrapping
    int wkcLabelW = 130;
    int wkcEditW = std::max(8, colW[1] - wkcLabelW - 6);
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"工作計數器 (WKC):", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), wkcLabelW, 22, IDC_STATIC_CYCLE_WKC_LABEL, hFont);
    hEditWKC = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"0", WS_VISIBLE | WS_CHILD | SS_SIMPLE | SS_LEFT, x + 4 + wkcLabelW + 6, helper.Y(), wkcEditW, 22, IDC_EDIT_WKC, hFont);
    x += colW[1];

    // MMF 更新頻率 (col 2) — wider label to prevent wrapping
    int pdoLabelW = 100;
    int pdoEditW = std::max(8, colW[2] - pdoLabelW - 6);
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"MMF 更新頻率:", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), pdoLabelW, 22, IDC_STATIC_PDO_SENT_LABEL, hFont);
    hEditPDOSent = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"等待連線...", WS_VISIBLE | WS_CHILD | SS_SIMPLE | SS_LEFT, x + 4 + pdoLabelW + 6, helper.Y(), pdoEditW, 22, 2010, hFont);

    helper.NextRow(); y = helper.Y();

    // --- Row 3: 除錯 URL 提醒 (診斷用 /ui_state) ---
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"除錯:", WS_VISIBLE | WS_CHILD | SS_RIGHT, rowX + 4, helper.Y(), 50, 22, 0, hFont);
    CreateChildControl(hContentWnd, hInstance, L"STATIC",
        L"http://localhost:8080/ui_state （遠端使用電腦IP:8080/ui_state）",
        WS_VISIBLE | WS_CHILD | SS_SIMPLE | SS_LEFT,
        rowX + 60, helper.Y(), rowW - 60, 22, 0, hFont);

    helper.NextRow(); y = helper.Y();
}

// Helper: create slave list and slave selector
void MainWindow::CreateSlaveListControls(LayoutHelper &helper, int &y, int clientW, int slaveListHeight, HFONT hFont, int ctrlW) {
    const int left = LayoutConfig::left;
    const int rightInset = GetScrollSafeRightInset();
    int listWidth = clientW - left - rightInset;
    if (listWidth < 400) listWidth = 400;
    // HTTP link: show server URL and an open button above slave list
    HttpRowLayout httpLayout = ComputeHttpRowLayout(listWidth);
    const int httpBtnW = httpLayout.buttonW;
    const int httpH = 22;
    const int httpChkW = httpLayout.checkboxW;
    // Button to open the web UI in browser (aligned left)
    hBtnOpenHttp = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"網頁操作介面", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, left, helper.Y(), httpBtnW, httpH, 2050, hFont);
    // Checkbox to enable local HTTP server (persisted to INI)
    hChkHttpServer = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"Web 伺服器", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, left + httpBtnW + 8, helper.Y(), httpChkW, httpH, IDC_CHK_HTTP_SERVER, hFont);
    // Static text showing URL (will be updated periodically) — placed to the right of the checkbox
    hStaticHttpUrl = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"Web UI: (not running)", WS_VISIBLE | WS_CHILD | SS_LEFT, left + httpBtnW + 8 + httpChkW + 8, helper.Y(), httpLayout.urlW, httpH, 0, hFont);
    // Set initial checkbox state from INI
    if (hChkHttpServer && IsWindow(hChkHttpServer)) {
        bool enabled = ReadIniBool(L"Server", L"EnableLocalHttpServer", false);
        SendMessage(hChkHttpServer, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        SetControlEnabled(hChkHttpServer, true);
    }
    helper.NextRow(); // leave one row empty for the http link

    // Startup option checkboxes (placed on the blank row above the slave list)
    const int chkW = ComputeStartupCheckboxWidth(listWidth);
    const int chkH = 20;
    int chkX = left;
    if (hChkStartupShowMain == NULL) {
        hChkStartupShowMain = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"啟動時顯示主視窗", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, chkX, helper.Y(), chkW, chkH, IDC_CHK_STARTUP_SHOWMAIN, hFont);
    }
    // 初始化勾選狀態（從 INI）
    if (hChkStartupShowMain && IsWindow(hChkStartupShowMain)) {
        bool showMain = ReadIniBool(L"Startup", L"ShowMainWindow", false);
        SendMessage(hChkStartupShowMain, BM_SETCHECK, showMain ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    chkX += chkW + 8;
    if (hChkStartupAutoSeq == NULL) {
        hChkStartupAutoSeq = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"啟動時自動執行 Auto", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, chkX, helper.Y(), chkW, chkH, IDC_CHK_STARTUP_AUTOSEQ, hFont);
    }
    if (hChkStartupAutoSeq && IsWindow(hChkStartupAutoSeq)) {
        bool autoSeq = ReadIniBool(L"Startup", L"AutoTrigger", true);
        SendMessage(hChkStartupAutoSeq, BM_SETCHECK, autoSeq ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    chkX += chkW + 8;
    // IK 實際 6DoF 姿態 MMF 輸出勾選框
    // 標籤使用 INI 中的 PoseOutputName 以便識別 MMF 映射名稱
    if (hChkMMFPoseOutput == NULL) {
        wchar_t nameBuf[256] = {0};
        GetPrivateProfileStringW(L"MMF", L"PoseOutputName", L"motionRigPose", nameBuf, 256, GetAppIniPath().c_str());
        std::wstring mmfLabel(nameBuf);
        if (mmfLabel.empty()) mmfLabel = L"motionRigPose";
        std::wstring label = L"6DoF姿態輸出MMF(" + mmfLabel + L")";
        hChkMMFPoseOutput = CreateChildControl(hContentWnd, hInstance, L"BUTTON", label.c_str(), WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, chkX, helper.Y(), chkW, chkH, IDC_CHK_MMF_POSE_OUTPUT, hFont);
    }
    if (hChkMMFPoseOutput && IsWindow(hChkMMFPoseOutput)) {
        bool poseMmf = ReadIniBool(L"MMF", L"EnablePoseOutput", false);
        SendMessage(hChkMMFPoseOutput, BM_SETCHECK, poseMmf ? BST_CHECKED : BST_UNCHECKED, 0);
        SetControlEnabled(hChkMMFPoseOutput, true);
        // EnablePoseMMFOutput will be called after homing completes (in AutoWindow)
    }

    // Move to next row after startup checkboxes before creating the slave list
    helper.NextRow();

    // create slave list below the startup checkbox row
    hListSlaves = CreateChildControl(hContentWnd, hInstance, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_TABSTOP, left, helper.Y(), listWidth, slaveListHeight, IDC_LIST_SLAVES, hFont);
    if (hListSlaves) {
        SetWindowSubclass(hListSlaves, SDOListSubclassProc, 3, (DWORD_PTR)this);
    }
    if (isDarkTheme) {
        ListView_SetBkColor(hListSlaves, clrDarkControl);
        ListView_SetTextColor(hListSlaves, clrDarkText);
        ListView_SetTextBkColor(hListSlaves, clrDarkControl);
    }
    LVCOLUMN lvc; ZeroMemory(&lvc, sizeof(lvc)); lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc.fmt = LVCFMT_LEFT;
    const int slaveColCount = 8;
    std::vector<int> colWidths = CoreHelpers::DistributeColumns(listWidth, slaveColCount);
    // Make the "站號" (station number) column narrower (0.5x) and redistribute the freed
    // width across the other columns so the overall width stays the same.
    if (!colWidths.empty()) {
        int orig0 = colWidths[0];
        int new0 = std::max(24, orig0 / 2); // ensure small minimum width so contents aren't completely clipped
        int diff = orig0 - new0;
        colWidths[0] = new0;
        if (diff > 0) {
            int remCols = (int)colWidths.size() - 1;
            if (remCols > 0) {
                int addPer = diff / remCols;
                int remainder = diff % remCols;
                for (int i = 1; i < (int)colWidths.size(); ++i) {
                    colWidths[i] += addPer + ((i == (int)colWidths.size() - 1) ? remainder : 0);
                }
            } else {
                // fallback: if no other columns, add diff to last column
                colWidths[colWidths.size() - 1] += diff;
            }
        }
    }
    lvc.cx = colWidths[0]; lvc.pszText = (LPWSTR)L"站號"; ListView_InsertColumn(hListSlaves, 0, &lvc);
    lvc.cx = colWidths[1]; lvc.pszText = (LPWSTR)L"名稱"; ListView_InsertColumn(hListSlaves, 1, &lvc);
    lvc.cx = colWidths[2]; lvc.pszText = const_cast<LPWSTR>(L"0x603F"); ListView_InsertColumn(hListSlaves, 2, &lvc);
    lvc.cx = colWidths[3]; lvc.pszText = const_cast<LPWSTR>(L"Vendor ID"); ListView_InsertColumn(hListSlaves, 3, &lvc);
    lvc.cx = colWidths[4]; lvc.pszText = const_cast<LPWSTR>(L"Product Code"); ListView_InsertColumn(hListSlaves, 4, &lvc);
    lvc.cx = colWidths[5]; lvc.pszText = const_cast<LPWSTR>(L"State"); ListView_InsertColumn(hListSlaves, 5, &lvc);
    lvc.cx = colWidths[6]; lvc.pszText = const_cast<LPWSTR>(L"Outputs"); ListView_InsertColumn(hListSlaves, 6, &lvc);
    lvc.cx = colWidths[7]; lvc.pszText = const_cast<LPWSTR>(L"Inputs"); ListView_InsertColumn(hListSlaves, 7, &lvc);
    ListView_SetExtendedListViewStyle(hListSlaves, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    helper.MoveDown(slaveListHeight); y = helper.Y();
    cachedSlaveListHeight = CalculateSlaveListHeightForRows(SLAVE_LIST_VISIBLE_ROWS);

    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"選定從站:", WS_VISIBLE | WS_CHILD, left, helper.Y(), LayoutConfig::labelW, 20, 0, hFont);
    hComboSlaves = CreateChildControl(hContentWnd, hInstance, WC_COMBOBOX, NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, helper.ControlX(), helper.Y(), ctrlW, 200, 2001, hFont);
    // Prepopulate with fixed station numbers 1..16 so startup shows placeholders
    if (hComboSlaves && IsWindow(hComboSlaves)) {
        SendMessageW(hComboSlaves, CB_RESETCONTENT, 0, 0);
        for (int si = 1; si <= 16; ++si) {
            wchar_t txt[8]; swprintf_s(txt, L"%d", si);
            LRESULT idx = SendMessageW(hComboSlaves, CB_ADDSTRING, 0, (LPARAM)txt);
            if (idx != CB_ERR && idx != CB_ERRSPACE) SendMessageW(hComboSlaves, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)si);
        }
        SendMessageW(hComboSlaves, CB_SETCURSEL, 0, 0);
    }
    helper.NextRow(); y = helper.Y();

    // PDO Mapping status display (below slave selector)
    {
        int pdoMapW = listWidth;
        if (pdoMapW < 200) pdoMapW = 200;
        hStaticPdoMapping = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_LEFT, left, helper.Y(), pdoMapW, 36, 0, hFont);
        helper.MoveDown(36); y = helper.Y();
    }
}

// Helper: create SDO controls and listview (buttons + ESI controls)
void MainWindow::CreateSDOControls(LayoutHelper &helper, int &y, int clientW, int sdoTableHeight, HFONT hFont, int left, int labelW, int ctrlW) {
    (void)ctrlW; (void)clientW; (void)helper;
    RECT rSdoLabel = helper.LabelRect();
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"SDO參數表:", WS_VISIBLE | WS_CHILD, rSdoLabel.left, helper.Y(), labelW, 20, 0, hFont);
    int sdoBtnGap = 8; int sdoBtnW = 80; int sdoBtnH = 22; int sdoBtnAllW = sdoBtnW + 20;
    int sdoBtnX = rSdoLabel.left + labelW + sdoBtnGap;
    int sdoBtnY = helper.Y();
    hBtnReadSDO = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX, sdoBtnY, sdoBtnW, sdoBtnH, IDC_BTN_READ_SDO, hFont);
    hBtnWriteSDO = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"寫入", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX + sdoBtnW + sdoBtnGap, sdoBtnY, sdoBtnW, sdoBtnH, IDC_BTN_WRITE_SDO, hFont);
    int sdoBtnReadAllX = sdoBtnX + (sdoBtnW + sdoBtnGap) * 2;
    hBtnReadAllSDO = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"全部讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnReadAllX, sdoBtnY, sdoBtnAllW, sdoBtnH, IDC_BTN_READ_ALL_SDO, hFont);
    SetControlEnabled(hBtnReadAllSDO, false);
    int sdoBtnWriteAllX = sdoBtnReadAllX + sdoBtnAllW + sdoBtnGap;
    hBtnWriteAllSDO = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"全部寫入", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnWriteAllX, sdoBtnY, sdoBtnAllW, sdoBtnH, IDC_BTN_WRITE_ALL_SDO, hFont);
    SetControlEnabled(hBtnWriteAllSDO, false);
    SetControlEnabled(hBtnReadSDO, false); SetControlEnabled(hBtnWriteSDO, false);
    {
        const int esiGap = 6; const int btnBrowseW = 80; const int esiLabelW = 80;
        int esiRight = sdoBtnX - 10; int esiComboW = 280; int maxComboW = esiRight - (helper.ControlX() + esiLabelW + 8) - btnBrowseW - esiGap;
        if (maxComboW < 120) maxComboW = 120; if (esiComboW > maxComboW) esiComboW = maxComboW;
        int btnX = helper.ControlX();
    hBtnBrowseESI = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"ESI檔讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, sdoBtnY, btnBrowseW, 22, IDC_BTN_BROWSE_ESI, hFont);
        int importBtnX = btnX + btnBrowseW + esiGap;
    hBtnImportParams = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"參數檔讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, importBtnX, sdoBtnY, sdoBtnAllW, 22, IDC_BTN_IMPORT_PARAMS, hFont);
        int saveBtnX = importBtnX + sdoBtnAllW + esiGap;
    hBtnSaveParams = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"參數檔存檔", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, saveBtnX, sdoBtnY, sdoBtnAllW, sdoBtnH, IDC_BTN_SAVE_PARAMS, hFont); SetControlEnabled(hBtnSaveParams, true); // Keep Save Params enabled by default
    }
    helper.NextRow(); y = helper.Y();
    const int sdoRightMargin = GetScrollSafeRightInset(); int sdoListX = left; int sdoListW = clientW - left - sdoRightMargin; if (sdoListW < 300) sdoListW = 300;
    hListSDOParams = CreateChildControl(hContentWnd, hInstance, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL, sdoListX, helper.Y(), sdoListW, sdoTableHeight, 2015, hGuiFont);
    ListView_SetExtendedListViewStyle(hListSDOParams, LVS_EX_GRIDLINES);
    if (hListSDOParams) { SetWindowSubclass(hListSDOParams, SDOListSubclassProc, 2, (DWORD_PTR)this); if (isDarkTheme) { ListView_SetBkColor(hListSDOParams, clrDarkControl); ListView_SetTextColor(hListSDOParams, clrDarkText); ListView_SetTextBkColor(hListSDOParams, clrDarkControl); } }
    LVCOLUMN lvc2; ZeroMemory(&lvc2, sizeof(lvc2)); lvc2.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc2.fmt = LVCFMT_LEFT;
    const int sdoCols = 7;
    std::vector<int> sdoWidths(sdoCols, 0);
    int base_sdo = sdoListW / sdoCols;
    for (int i = 0; i < sdoCols; ++i) sdoWidths[i] = base_sdo;
    int sumS = 0; for (int v : sdoWidths) sumS += v;
    if (sumS < sdoListW) sdoWidths[sdoCols - 1] += (sdoListW - sumS);
    // Make SDO Index and Sub columns narrower (0.5x) and redistribute freed width to remaining columns
    if ((int)sdoWidths.size() >= 2) {
        int orig0 = sdoWidths[0];
        int orig1 = sdoWidths[1];
        int new0 = std::max(24, orig0 / 2);
        int new1 = std::max(24, orig1 / 2);
        int diffTotal = (orig0 - new0) + (orig1 - new1);
        sdoWidths[0] = new0;
        sdoWidths[1] = new1;
        if (diffTotal > 0) {
            int remCols = (int)sdoWidths.size() - 2; // distribute to columns 2..n-1
            if (remCols > 0) {
                int addPer = diffTotal / remCols;
                int remainder = diffTotal % remCols;
                for (int i = 2; i < (int)sdoWidths.size(); ++i) {
                    sdoWidths[i] += addPer + ((i == (int)sdoWidths.size() - 1) ? remainder : 0);
                }
            } else {
                // fallback: put diff into last column
                sdoWidths[sdoWidths.size() - 1] += diffTotal;
            }
        }
    }
    // Shrink DataType (col 3) and Access (col 4) to half width; give space to Value column (col 5)
    if ((int)sdoWidths.size() > 5) {
        const int targets[] = {3, 4};
        int reclaimed = 0;
        for (int idx : targets) {
            if (idx < 0 || idx >= (int)sdoWidths.size()) continue;
            int orig = sdoWidths[idx];
            int newW = std::max(36, orig / 2);
            reclaimed += (orig - newW);
            sdoWidths[idx] = newW;
        }
        if (reclaimed > 0) sdoWidths[5] += reclaimed;
    }
    lvc2.cx = sdoWidths[0]; lvc2.pszText = (LPWSTR)L"Index"; ListView_InsertColumn(hListSDOParams,0,&lvc2);
    lvc2.cx = sdoWidths[1]; lvc2.pszText = (LPWSTR)L"Sub"; ListView_InsertColumn(hListSDOParams,1,&lvc2);
    lvc2.cx = sdoWidths[2]; lvc2.pszText = (LPWSTR)L"Name"; ListView_InsertColumn(hListSDOParams,2,&lvc2);
    lvc2.cx = sdoWidths[3]; lvc2.pszText = (LPWSTR)L"DataType"; ListView_InsertColumn(hListSDOParams,3,&lvc2);
    lvc2.cx = sdoWidths[4]; lvc2.pszText = (LPWSTR)L"Access"; ListView_InsertColumn(hListSDOParams,4,&lvc2);
    lvc2.cx = sdoWidths[5]; lvc2.pszText = (LPWSTR)L"Value"; ListView_InsertColumn(hListSDOParams,5,&lvc2);
    lvc2.cx = sdoWidths[6]; lvc2.pszText = (LPWSTR)L"Hex"; ListView_InsertColumn(hListSDOParams,6,&lvc2);
    y = helper.Y();
    ReflowSDOControls(sdoTableHeight);

}

// Extracted: create mailbox static/edit/button, and advance to PDO area
void MainWindow::CreateMailboxControls(LayoutHelper &helper, int &y, int clientW, int mailboxHeight, HFONT hFont, int labelW, int contentWidth) {
    (void)clientW; (void)mailboxHeight;
    const int left = LayoutConfig::left;
    // 計算 Mailbox 的 Y 位置：依照實際 SDO ListView 的底部來定位，避免上方空白
    if (hListSDOParams && IsWindow(hListSDOParams)) {
    RECT rcList{};
        if (GetWindowRect(hListSDOParams, &rcList)) {
            POINT ptBottom = { rcList.left, rcList.bottom };
            ScreenToClient(hContentWnd, &ptBottom);
            // 最小間距 0px，完全緊貼
            y = ptBottom.y + 0;
        } else {
            // fallback to helper cursor
            y = helper.Y();
        }
    } else {
        y = helper.Y();
    }

    // Mailbox（直接緊接於 SDO 列表之下）
    RECT rMailboxLabel = { left, y, labelW, 22 };
    CreateChildControl(hContentWnd, hInstance, L"STATIC", L"Mailbox 資料 (hex):", WS_VISIBLE | WS_CHILD | SS_LEFT,
        rMailboxLabel.left, rMailboxLabel.top, labelW, 22, IDC_STATIC_MAILBOX, hFont);

    // 計算 Mailbox edit 寬度（為按鈕預留空間）
    const int mailboxGap = 10;
    const int btnSendMailboxW = 105;
    int mailboxButtonsW = btnSendMailboxW + mailboxGap;
    int mailboxEditW = (contentWidth - labelW - 200) - mailboxButtonsW;
    if (mailboxEditW < 200) mailboxEditW = 200;

    hEditMailbox = CreateChildControl(hContentWnd, hInstance, L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
        left + labelW + 8, y, mailboxEditW, 22, IDC_EDIT_MAILBOX, hFont);

    // 按鈕位置：緊跟在 edit box 後面（避免重疊）
    int mailboxBtnX = left + labelW + 8 + mailboxEditW + mailboxGap;
    hBtnSendMailbox = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"發送 Mailbox", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
        mailboxBtnX, y, btnSendMailboxW, 22, IDC_BTN_SEND_MAILBOX, hFont);
    SetControlEnabled(hBtnSendMailbox, false);

    // 為 Mailbox 區域預留一行高度空間（22像素 + 適當邊距）
    y += 26; // 增加垂直間距以避免下方擠壓
}

// Extracted: initialize PDODisplay area and ensure motionController exists
void MainWindow::CreatePDOControls(LayoutHelper &helper, int &y, int clientW, int pdoHeight, HFONT hFont, int left, int labelW, int ctrlW, int contentWidth) {
    (void)clientW; (void)labelW; (void)ctrlW; (void)helper;
    int pdoWidth = (contentWidth < 860) ? contentWidth : 860;
    pdoDisplay.Initialize(hContentWnd, hInstance, hFont, y, left, pdoWidth, pdoHeight);
    pdoDisplay.Show(true);
    // 在 PDO 顯示區左上偏下的位置新增 MMF 即時輸出勾選框（置於表框內）
    {
        // 放置於 PDO 區塊內，距上邊緣下移約 24px 以免與標題/欄位重疊
        int chkX = left + 12;
        int chkY = y + 36;
        int chkW = 220; // increase width so the full label is visible
        int chkH = 20;
        hChkMMFOutput = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"PDO即時記錄與MMF輸出", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, chkX, chkY, chkW, chkH, IDC_CHK_MMF_OUTPUT, hFont);
        if (hChkMMFOutput) {
            // 預設不勾選（功能預設關閉）
            SendMessage(hChkMMFOutput, BM_SETCHECK, BST_UNCHECKED, 0);
            SetControlEnabled(hChkMMFOutput, true);
        }
        // 在勾選框右側建立靜態文字顯示 MMF 名稱與重要資訊
        {
            int infoX = chkX + chkW + 8;
            int infoY = chkY;
            int infoW = 480; // reduce slightly to keep layout balanced
            int infoH = chkH;
            hStaticMMFInfo = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"", WS_VISIBLE | WS_CHILD | SS_LEFT, infoX, infoY, infoW, infoH, -1, hFont);
            if (hStaticMMFInfo) {
                // 預設顯示為停用狀態，並提示外部程式日誌位置
                SetWindowTextW(hStaticMMFInfo, L"MMF: disabled  資料存檔在\\logs\\PDO.log");
            }
        }
    }
    if (auto* master = AppController::GetInstance().GetMaster();
        confirmedSlave > 0 && master && master->IsInitialized()) {
        ApplyStatuswordDisableToPDOControls(confirmedSlave);
    }
    int pdoH = pdoDisplay.GetHeight();
    if (pdoH <= 0) pdoH = pdoHeight;
    y += pdoH;

    // Create motion controller (non-UI logic) so other threads can call into it
    if (!motionController) {
        motionController = std::make_unique<MotionController>(AppController::GetInstance().GetMaster(), &pdoDisplay);
    }
}

// Extracted: create motion controls (label + buttons + mode/config) and motion params placeholder
void MainWindow::CreateMotionControls(LayoutHelper &helper, int &y, int clientW, int pdoHeight, HFONT hFont, int left, int labelW, int ctrlW) {
    (void)clientW; (void)pdoHeight; (void)helper; (void)ctrlW;
    // 新增：運動控制區塊（放在 PDO 表格下方）
    {
        // 在建立運動控制區塊前，略微往下移動游標以避免與上方表單重疊
        // 使用 LayoutConfig::vpad 以維持與其他列一致的間距
        y += LayoutConfig::vpad;

        // 標題
        RECT rMotionLabel = { left, y, labelW, 22 };
        CreateChildControl(hContentWnd, hInstance, L"STATIC", L"運動控制:", WS_VISIBLE | WS_CHILD | SS_LEFT,
            rMotionLabel.left, rMotionLabel.top, labelW, 22, IDC_STATIC_MOTION_CONTROL, hFont);

        // 設定按鈕樣式與間距
        const int motionBtnH = 24;
        const int motionBtnGap = 8;
        // 使用較小的按鈕寬度以容納多個按鈕
        const int motionBtnW = 80;

        int btnMotionX = left + labelW + 8;
        int btnY = y;

    hBtnFollowMode = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"跟隨模式", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_FOLLOW_MODE, hFont);
    SetControlEnabled(hBtnFollowMode, false);
        btnMotionX += motionBtnW + motionBtnGap;

    hBtnPowerOn = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"電源啟動", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_POWER_ON, hFont);
    SetControlEnabled(hBtnPowerOn, false);
        if (hBtnPowerOn) { SetWindowSubclass(hBtnPowerOn, PowerOnButtonSubclassProc, 0, (DWORD_PTR)this); }
        btnMotionX += motionBtnW + motionBtnGap;

    hBtnPowerOff = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"電源關閉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_POWER_OFF, hFont);
    SetControlEnabled(hBtnPowerOff, false);
        if (hBtnPowerOff) { SetWindowSubclass(hBtnPowerOff, PowerOffButtonSubclassProc, 0, (DWORD_PTR)this); }
        btnMotionX += motionBtnW + motionBtnGap;

    hBtnHome = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"原點設定", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_HOME, hFont);
    SetControlEnabled(hBtnHome, false);
        if (hBtnHome) { SetWindowSubclass(hBtnHome, HomeButtonSubclassProc, 0, (DWORD_PTR)this); }
        btnMotionX += motionBtnW + motionBtnGap;

    hBtnJogCW = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"寸動正轉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_JOG_CW, hFont);
    SetControlEnabled(hBtnJogCW, false);
    btnMotionX += motionBtnW + motionBtnGap;
    if (hBtnJogCW) { SetWindowSubclass(hBtnJogCW, JogButtonSubclassProc, 0, (DWORD_PTR)this); }

    hBtnJogCCW = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"寸動反轉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_JOG_CCW, hFont);
    SetControlEnabled(hBtnJogCCW, false);
    btnMotionX += motionBtnW + motionBtnGap;
    if (hBtnJogCCW) { SetWindowSubclass(hBtnJogCCW, JogButtonSubclassProc, 0, (DWORD_PTR)this); }

    hBtnFaultReset = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"異常重置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, IDC_BTN_FAULT_RESET, hFont);
    SetControlEnabled(hBtnFaultReset, false);
        if (hBtnFaultReset) { SetWindowSubclass(hBtnFaultReset, FaultResetButtonSubclassProc, 0, (DWORD_PTR)this); }

        // 把立即停止按鈕放在目前的按鈕序列末端（與上方按鈕同行）
        const int stopBtnW = motionBtnW * 2; // 2 倍寬
        // Use btnMotionX as X coordinate for proper alignment
    hBtnImmediateStop = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"立即停止", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_DEFPUSHBUTTON, btnMotionX, btnY, stopBtnW, motionBtnH, IDC_BTN_IMMEDIATE_STOP, hFont);
    SetControlEnabled(hBtnImmediateStop, false);
        if (hBtnImmediateStop) { SetWindowSubclass(hBtnImmediateStop, ImmediateStopButtonSubclassProc, 0, (DWORD_PTR)this); }
        
    // 在運動參數列表創建前設定 y 位置
    y = y + motionBtnH + motionBtnGap;
    }

    // 建立設定檔按鈕（在運動參數列表上方）
    {
        const int cfgBtnW = 100;
        const int cfgBtnH = 24;
        const int cfgBtnGap = 8;
        int cfgBtnY = y;
        int cfgBtnX = left + labelW + 8;
    hBtnLoadConfig = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"設定檔讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, cfgBtnX, cfgBtnY, cfgBtnW, cfgBtnH, IDC_BTN_LOAD_CONFIG, hFont);
    // Only enable load when master is NOT initialized (i.e. disconnected)
    if (hBtnLoadConfig) SetControlEnabled(hBtnLoadConfig, !IsMasterInitialized());
        cfgBtnX += cfgBtnW + cfgBtnGap;
    hBtnSaveConfig = CreateChildControl(hContentWnd, hInstance, L"BUTTON", L"設定檔存檔", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, cfgBtnX, cfgBtnY, cfgBtnW, cfgBtnH, IDC_BTN_SAVE_CONFIG, hFont); SetControlEnabled(hBtnSaveConfig, true);
        
        // MMF 輸出勾選框：移至 PDO 表格區塊左上（此處保留空間）
    
    y = cfgBtnY + cfgBtnH + cfgBtnGap;
    }

    // 建立運動參數表格（ListView）: 名稱 / 值 / 單位 / 說明
    {
        const int listX = left;
        const int rightInset = GetScrollSafeRightInset();
        int listW = clientW - left - rightInset;
        if (listW < 300) listW = 300;
        int listH = 140; // 初始高度，可在 ReflowMainControls 調整
    hListMotionParams = CreateChildControl(hContentWnd, hInstance, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_TABSTOP, listX, y, listW, listH, 2025, hGuiFont);
        if (hListMotionParams) {
            ListView_SetExtendedListViewStyle(hListMotionParams, LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT);
            if (isDarkTheme) {
                ListView_SetBkColor(hListMotionParams, clrDarkControl);
                ListView_SetTextColor(hListMotionParams, clrDarkText);
                ListView_SetTextBkColor(hListMotionParams, clrDarkControl);
            }

            // Insert columns
            LVCOLUMN lvc; ZeroMemory(&lvc, sizeof(lvc)); lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc.fmt = LVCFMT_LEFT;
            int colCount = 6;
            // insert columns with placeholder widths; ApplyDistributedColumns will adjust them
            lvc.cx = listW / colCount; lvc.pszText = (LPWSTR)L"編號"; ListView_InsertColumn(hListMotionParams, 0, &lvc);
            lvc.cx = listW / colCount; lvc.pszText = (LPWSTR)L"名稱"; ListView_InsertColumn(hListMotionParams, 1, &lvc);
            lvc.cx = listW / colCount; lvc.pszText = (LPWSTR)L"DataType"; ListView_InsertColumn(hListMotionParams, 2, &lvc);
            lvc.cx = listW / colCount; lvc.pszText = (LPWSTR)L"值"; ListView_InsertColumn(hListMotionParams, 3, &lvc);
            lvc.cx = listW / colCount; lvc.pszText = (LPWSTR)L"單位"; ListView_InsertColumn(hListMotionParams, 4, &lvc);
            lvc.cx = listW - ((listW / colCount) * 5); lvc.pszText = (LPWSTR)L"說明"; ListView_InsertColumn(hListMotionParams, 5, &lvc);
            // adjust actual widths now that the ListView exists
            ApplyDistributedColumns(hListMotionParams, colCount);
            ShrinkColumnsWithRedistribute(hListMotionParams, {0, 2, 4}, 0.5, 5);
            // Install subclass to start in-place edit on mouse click/dblclick reliably
            SetWindowSubclass(hListMotionParams, MotionListSubclassProc, 0, (DWORD_PTR)this);
        }

        // Populate default rows (P0..P20)
        const int defaultRows = 21;
        for (int ri = 0; ri < defaultRows; ++ri) {
            LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = ri;
            std::wstring id = L"P" + std::to_wstring(ri);
            lvi.pszText = const_cast<LPWSTR>(id.c_str());
            ListView_InsertItem(hListMotionParams, &lvi);
            std::wstring empty = L"";
            ListView_SetItemText(hListMotionParams, ri, 1, const_cast<LPWSTR>(empty.c_str()));
            ListView_SetItemText(hListMotionParams, ri, 2, const_cast<LPWSTR>(L" "));
            ListView_SetItemText(hListMotionParams, ri, 3, const_cast<LPWSTR>(empty.c_str()));
            ListView_SetItemText(hListMotionParams, ri, 4, const_cast<LPWSTR>(empty.c_str()));
            ListView_SetItemText(hListMotionParams, ri, 5, const_cast<LPWSTR>(empty.c_str()));
        }

        // Seed in-memory cache for the initially selected station (combobox current selection)
        // so that the first group's motion params are not empty on startup/after connect.
        int initStation = 1;
        if (hComboSlaves && IsWindow(hComboSlaves)) {
            LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
            if (sel != CB_ERR) initStation = (int)sel + 1;
        }
        if (initStation < 1) initStation = 1;
        if (savedMotionParamsForSlave.find(initStation) == savedMotionParamsForSlave.end()) {
            std::vector<std::vector<std::wstring>> mat;
            mat.reserve(defaultRows);
            for (int r = 0; r < defaultRows; ++r) {
                std::vector<std::wstring> row(6);
                wchar_t b0[64] = {0}; swprintf_s(b0, L"P%d", r);
                row[0] = b0; row[1] = L""; row[2] = L" "; row[3] = L""; row[4] = L""; row[5] = L"";
                mat.push_back(std::move(row));
            }
            savedMotionParamsForSlave[initStation] = std::move(mat);
        }

        // advance y by initial height (ReflowMainControls will recalc more precisely)
        y += listH + 8;
    }

    // 新增：機械單位表格（顯示目標/實際 位置/速度/扭力）放在運動參數表格下方
    {
        // 靜態標題 (會在 Reflow 時定位於 mech list 上方)
        hStaticMechUnitsLabel = CreateChildControl(hContentWnd, hInstance, L"STATIC", L"機械單位表格[顯示所有軸換算機械單位後的值]", WS_VISIBLE | WS_CHILD | SS_LEFT,
            left, y, 400, 20, 0, hGuiFont);
        // MMF section moved to AutoWindow. AutoWindow creates the MMF label,
        // MMF1 enable/disable buttons and the MMF ListView (IDC_LIST_MMF).
        // The main window no longer creates these controls.
        // reserve vertical space as before
        y += 28;
        const int mechListX = left;
        const int rightInset = GetScrollSafeRightInset();
        int mechListW = clientW - left - rightInset;
        if (mechListW < 300) mechListW = 300;
    // Compute initial mech list height to show 6 rows (header + 6 items)
    int rowH_init = MeasureTextHeight(hGuiFont);
    int headerH_init = rowH_init + 6;
    int mechListH = headerH_init + (rowH_init * 6) + 4; // small padding
        hListMechUnits = CreateChildControl(hContentWnd, hInstance, WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_TABSTOP, mechListX, y, mechListW, mechListH, 2026, hGuiFont);
        if (hListMechUnits) {
            ListView_SetExtendedListViewStyle(hListMechUnits, LVS_EX_GRIDLINES | LVS_EX_FULLROWSELECT);
            if (isDarkTheme) {
                ListView_SetBkColor(hListMechUnits, clrDarkControl);
                ListView_SetTextColor(hListMechUnits, clrDarkText);
                ListView_SetTextBkColor(hListMechUnits, clrDarkControl);
            }

            // Insert columns: add '站號' as first column and shift others right (7 cols)
            LVCOLUMN lvc; ZeroMemory(&lvc, sizeof(lvc)); lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc.fmt = LVCFMT_LEFT;
            int colCount = 7;
            int baseW = mechListW / colCount;
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"站號"; ListView_InsertColumn(hListMechUnits, 0, &lvc);
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"目標位置"; ListView_InsertColumn(hListMechUnits, 1, &lvc);
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"實際位置"; ListView_InsertColumn(hListMechUnits, 2, &lvc);
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"目標速度"; ListView_InsertColumn(hListMechUnits, 3, &lvc);
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"實際速度"; ListView_InsertColumn(hListMechUnits, 4, &lvc);
            lvc.cx = baseW; lvc.pszText = (LPWSTR)L"目標扭力"; ListView_InsertColumn(hListMechUnits, 5, &lvc);
            lvc.cx = mechListW - baseW * 6; lvc.pszText = (LPWSTR)L"實際扭力"; ListView_InsertColumn(hListMechUnits, 6, &lvc);
            ShrinkColumnWidthWithRedistribute(hListMechUnits, 0, 0.5, 6);

            // Insert default rows: expand to 8 rows and populate station numbers
            const int defaultRows = 8;
            for (int ri = 0; ri < defaultRows; ++ri) {
                LVITEM lvi; ZeroMemory(&lvi, sizeof(lvi));
                lvi.mask = LVIF_TEXT;
                lvi.iItem = ri;
                // Station number column (1-based)
                std::wstring id = std::to_wstring(ri + 1);
                lvi.pszText = const_cast<LPWSTR>(id.c_str());
                ListView_InsertItem(hListMechUnits, &lvi);
                // Clear other columns
                std::wstring empty = L"";
                for (int ci = 1; ci < colCount; ++ci) ListView_SetItemText(hListMechUnits, ri, ci, const_cast<LPWSTR>(empty.c_str()));
            }
        }
    // advance y by initial size
    y += mechListH + 8;

        // MMF ListView and rows are hosted in the AutoWindow (IDC_LIST_MMF).
        // The AutoWindow is responsible for creating the ListView, columns
        // and populating the initial rows. Reserve vertical space similar to
        // previous layout to keep main content alignment consistent.
        int mmfRowH = rowH_init;
        int mmfHeaderH = headerH_init;
        // Reserve vertical space to show header + 6 rows (match AutoWindow)
        int mmfListH = mmfHeaderH + (mmfRowH * 6) + 4;
        y += mmfListH + 8;
    }
}

void MainWindow::ReflowSDOControls(int dynamicHeight)
{
    if (!hwnd) return;
    RECT clientRc; GetClientRect(hwnd, &clientRc);
    int clientW = clientRc.right - clientRc.left;
    const int left = LayoutConfig::left;
    const int labelW = LayoutConfig::labelW;
    const int rightMargin = GetScrollSafeRightInset();
    
    // 計算可用寬度（控制欄與全寬均需）
    int ctrlX = left + labelW + 8;
    int availableW = clientW - ctrlX - rightMargin;
    if (availableW < 400) availableW = 400;

    // Start layout below top padding; now use content-pane client coords (hContentWnd)
    int listY = 10;
    int listX = ctrlX; (void)listX;
    if (hListSDOParams && IsWindow(hListSDOParams)) {
    RECT rcExisting{};
        if (GetWindowRect(hListSDOParams, &rcExisting)) {
            POINT pt = { rcExisting.left, rcExisting.top };
            ScreenToClient(hContentWnd, &pt);
            listY = pt.y;
        }
    } else {
        // Fallback: find a static label that mentions SDO and use its Y so the
        // list stays on the same row when the handle is not yet available.
        HWND child = GetWindow(hContentWnd, GW_CHILD);
        while (child) {
            wchar_t buf[128] = {0};
            GetClassNameW(child, buf, _countof(buf));
            if (wcscmp(buf, L"Static") == 0) {
                wchar_t text[128] = {0};
                GetWindowTextW(child, text, _countof(text));
                if (wcsstr(text, L"SDO") != NULL) {
                    RECT rcLabel{};
                    if (GetWindowRect(child, &rcLabel)) {
                        POINT pt = { rcLabel.left, rcLabel.top };
                        ScreenToClient(hContentWnd, &pt);
                        listY = pt.y;
                    }
                    break;
                }
            }
            child = GetWindow(child, GW_HWNDNEXT);
        }
    }

    // Layout the SDO parameter ListView - align to global left and span full width
    // (minus a small right margin) so it matches the slave list/mailbox.
    int listW = clientW - left - rightMargin;
    if (listW < 300) listW = 300; // 保持最小寬度

    // Compute list height to show exactly 10 full rows (header + 10 items)
    int listH = 0;
    
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        // Measure header and item heights, fallback to font metrics via helper
        int headerH = 0;
        HWND hHeader = ListView_GetHeader(hListSDOParams);
        if (hHeader && IsWindow(hHeader)) {
            RECT rcH; GetWindowRect(hHeader, &rcH);
            headerH = rcH.bottom - rcH.top;
        }
        int itemH = 0;
    RECT rcItem{};
        int itemCount = ListView_GetItemCount(hListSDOParams);
        if (itemCount > 0) {
            if (ListView_GetItemRect(hListSDOParams, 0, &rcItem, LVIR_BOUNDS)) itemH = rcItem.bottom - rcItem.top;
        } else {
            // fallback to font metric
            itemH = MeasureTextHeight(hGuiFont);
        }
        if (itemH <= 0) itemH = MeasureTextHeight(hGuiFont);
        if (headerH <= 0) headerH = itemH + 6;
        listH = headerH + (itemH * DEFAULT_SDO_VISIBLE_ROWS);
    }
    // Use provided dynamic height if measurement failed
    if (listH <= 0) listH = dynamicHeight;

    // Ensure the list is aligned with the global left margin for visual consistency
    int listX_aligned = left;
    if (hListSDOParams) SafeSetPos(hListSDOParams, NULL, listX_aligned, listY, listW, listH, SWP_NOZORDER | SWP_SHOWWINDOW);

    // Recalculate SDO ListView column widths so Hex column remains wide enough
    if (hListSDOParams && IsWindow(hListSDOParams)) {
    // Use the actual ListView client width (accounts for borders/vertical scrollbar)
    RECT lvClientRc{};
    GetClientRect(hListSDOParams, &lvClientRc);
    int lvClientW = lvClientRc.right - lvClientRc.left;
    // Account for vertical scrollbar width if present
    int vsw_sdo = GetSystemMetrics(SM_CXVSCROLL);
    BOOL hasVScroll = (GetWindowLong(hListSDOParams, GWL_STYLE) & WS_VSCROLL) != 0;
    if (hasVScroll) lvClientW -= vsw_sdo;
    // Fallback to available listW if client query failed or is too small
    if (lvClientW <= 0) lvClientW = listW;
    if (lvClientW < 200) lvClientW = 200;

    std::vector<int> colW = CoreHelpers::DistributeColumns(lvClientW, 7);
    // Ensure Index and Sub columns are compressed to half width during reflow
    if ((int)colW.size() >= 2) {
        int orig0 = colW[0];
        int orig1 = colW[1];
        int new0 = std::max(24, orig0 / 2);
        int new1 = std::max(24, orig1 / 2);
        int diff = (orig0 - new0) + (orig1 - new1);
        colW[0] = new0; colW[1] = new1;
        if (diff > 0) {
            int remCols = (int)colW.size() - 2;
            if (remCols > 0) {
                int addPer = diff / remCols;
                int remainder = diff % remCols;
                for (int i = 2; i < (int)colW.size(); ++i) {
                    colW[i] += addPer + ((i == (int)colW.size() - 1) ? remainder : 0);
                }
            } else {
                colW[colW.size() - 1] += diff;
            }
        }
    }
    // Shrink DataType (col 3) and Access (col 4) during reflow; donate space to Value column (5)
    if ((int)colW.size() > 5) {
        const int targets[] = {3, 4};
        int reclaimed = 0;
        for (int idx : targets) {
            if (idx < 0 || idx >= (int)colW.size()) continue;
            int orig = colW[idx];
            int newW = std::max(36, orig / 2);
            reclaimed += (orig - newW);
            colW[idx] = newW;
        }
        if (reclaimed > 0) colW[5] += reclaimed;
    }
    for (int i = 0; i < (int)colW.size(); ++i) ListView_SetColumnWidth(hListSDOParams, i, colW[i]);
    }

    // Update contentHeight using the list height we actually applied
    if ((listY + listH) > contentHeight) contentHeight = listY + listH;

    // Position the SDO Read/Write buttons to the right of the SDO label if present.
    int btnW = 80;
    int btnSpacing = 10;
    int btnAllW = btnW + 20; // matches sdoBtnAllW used during creation
    int btnY = listY; // align vertically with the SDO label/list top by default
    int btnX_from_label = -1;

    // Try to find the SDO static label and compute its right edge in content-pane client coords
    HWND child = GetWindow(hContentWnd, GW_CHILD);
    while (child) {
        wchar_t cls[64] = {0};
        GetClassNameW(child, cls, _countof(cls));
        if (wcscmp(cls, L"Static") == 0) {
            wchar_t text[128] = {0};
            GetWindowTextW(child, text, _countof(text));
            if (wcsstr(text, L"SDO") != NULL) {
                RECT rcLabel{};
                if (GetWindowRect(child, &rcLabel)) {
                    POINT pt = { rcLabel.left, rcLabel.top };
                    ScreenToClient(hContentWnd, &pt);
                    int labelRight = pt.x + (rcLabel.right - rcLabel.left);
                    btnX_from_label = labelRight + 8; // small gap after label
                    // place buttons aligned with label top
                    btnY = pt.y;
                }
                break;
            }
        }
        child = GetWindow(child, GW_HWNDNEXT);
    }

    // If SDO label was not found (e.g., during early creation), fall back to positioning
    // the buttons relative to the existing ESI browse button so they remain on-screen.
    if (btnX_from_label < 0 && hBtnBrowseESI && IsWindow(hBtnBrowseESI)) {
        RECT rcBrowse; GetWindowRect(hBtnBrowseESI, &rcBrowse);
        POINT ptB = { rcBrowse.left, rcBrowse.top };
        ScreenToClient(hContentWnd, &ptB);
        int browseRight = ptB.x + (rcBrowse.right - rcBrowse.left);
        btnX_from_label = browseRight + 8;
        btnY = ptB.y;
    }

    // Determine a safe X for the SDO buttons. Prefer the label-derived X
    // (btnX_from_label) but ensure they fit in a reserved right-side area
    // so they won't be overlapped by wide controls like the ESI combo.
    int totalButtonsW = btnW + btnSpacing + btnW + btnSpacing + btnAllW + btnSpacing + btnAllW; // read, write, all, save
    // Ensure buttons stay inside client area leaving sideMargin on the right
    int rightBasedX = clientW - rightMargin - totalButtonsW;

    // Determine whether buttons fit on the same row as the list (left-aligned).
    int leftAlignedX = listX_aligned;
    bool fitsLeft = (leftAlignedX + totalButtonsW <= clientW - rightMargin);

    int finalBtnX = leftAlignedX;
    int finalBtnY = btnY;

    if (!fitsLeft) {
        // Not enough horizontal room: place the buttons under the list to avoid overlap.
        finalBtnX = listX_aligned;
        finalBtnY = listY + listH + 6; // small vertical gap under the list
    } else {
        // Enough room: keep them on the same row. However if a label-derived X
        // or browse button was available and is further right, prefer it while
        // ensuring we don't overflow the reserved area.
        if (btnX_from_label >= 0) {
            int candidateX = (std::max)(btnX_from_label, rightBasedX);
            // Only use candidate if it doesn't overflow
            if (candidateX + totalButtonsW <= clientW - rightMargin) finalBtnX = candidateX;
        }
    }

    if (finalBtnY < 0) finalBtnY = listY + listH + 5;

    if (hBtnReadSDO) SafeSetPos(hBtnReadSDO, NULL, finalBtnX, finalBtnY, btnW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (hBtnWriteSDO) SafeSetPos(hBtnWriteSDO, NULL, finalBtnX + btnW + btnSpacing, finalBtnY, btnW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (hBtnReadAllSDO) SafeSetPos(hBtnReadAllSDO, NULL, finalBtnX + (btnW + btnSpacing) * 2, finalBtnY, btnAllW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    // Explicitly place the "全部寫入" button to the right of "全部讀取"
    if (hBtnWriteAllSDO) SafeSetPos(hBtnWriteAllSDO, NULL, finalBtnX + (btnW + btnSpacing) * 2 + btnAllW + btnSpacing, finalBtnY, btnAllW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    // hBtnSaveParams is intentionally positioned next to the Import button
    // and should not be reflowed here to avoid overwriting its X coordinate.

    // Update contentHeight: include either the list bottom or the buttons bottom
    int sdoBottom = listY + listH;
    int buttonsBottom = finalBtnY + 22;
    if (buttonsBottom > sdoBottom) sdoBottom = buttonsBottom;
    if (sdoBottom > contentHeight) contentHeight = sdoBottom;
}

// Reflow main controls horizontally to reduce margins and use available width
void MainWindow::ReflowMainControls()
{
    if (minimizedToTray || isMinimizedState) return; // skip reflow while minimized to tray or minimized
    if (!hwnd) return;
    RECT mainClientRc; GetClientRect(hwnd, &mainClientRc);
    int clientW = mainClientRc.right - mainClientRc.left;
    int clientH = mainClientRc.bottom - mainClientRc.top;
    const int left = LayoutConfig::left;
    const int labelW = LayoutConfig::labelW;
    const int rightMargin = GetScrollSafeRightInset();  // 使用對稱邊距 + 滾動條保留
    int ctrlX = left + labelW + 8;
    
    // 計算可用寬度（使用更多空間）
    int availableW = clientW - ctrlX - rightMargin;
    if (availableW < 300) availableW = 300;  // 最小寬度
    
    // 主控件寬度（不設上限，讓它自適應）
    int ctrlW = availableW;
    (void)ctrlW; // mark as intentionally unused in this reflow path
    
    // Calculate dynamic heights based on available client height (same logic as CreateControls)
    const int fixedTopHeight = 120; // approximate space for adapter + cycle time + ESI + slave selector
    const int fixedBottomHeight = 50; // space for status area
    int availableContentHeight = clientH - fixedTopHeight - fixedBottomHeight;
    if (availableContentHeight < 400) availableContentHeight = 400; // minimum height

    // Allocate heights proportionally (same as CreateControls)
    // Use a fixed height for the slave table so its row layout stays stable
    // and controls below (ESI/SDO/mailbox) don't jump around when window height changes.
    // Compute a reasonable slave list height but cap to at most 6 visible rows
    int slaveListHeight = CalculateSlaveListHeightForRows(SLAVE_LIST_VISIBLE_ROWS);
    cachedSlaveListHeight = slaveListHeight;
    // SDO table height: show 10 visible rows by default (header + 10 rows + padding)
    int sdoTableHeight = 0;
    {
    int desiredRows = DEFAULT_SDO_VISIBLE_ROWS;
        int rowHeight = MeasureTextHeight(hGuiFont);
        int headerH = rowHeight + 6;
        sdoTableHeight = headerH + (rowHeight * desiredRows) + 10;
    }
    // NOTE: do not auto-apply cachedSDOListHeight here so the default visible
    // rows (DEFAULT_SDO_VISIBLE_ROWS) take effect. cachedSDOListHeight may be
    // set by explicit user actions in the future.
    int pdoHeight = (int)(availableContentHeight * 0.25f);       // 25% for PDO display
    int mailboxHeight = (int)(availableContentHeight * 0.20f);   // 20% for mailbox and buttons

    // Ensure minimum heights
    if (slaveListHeight < 100) slaveListHeight = 100;
    // Lower the minimum height since we've halved the allocated proportion
    if (sdoTableHeight < 75) sdoTableHeight = 75;
    if (pdoHeight < 120) pdoHeight = 120;
    if (mailboxHeight < 40) mailboxHeight = 40;
    
    // 計算按鈕區域寬度
    int btnAreaW = 100 + 10 + 100 + 10 + 100 + 10 + 100 + 10 + 100 + 10 + 100;  // Connect + Disconnect + Dump + Clear + AutoStart + AutoStop + 間距
    
    // 如果視窗足夠寬，擴展控件寬度
    if (availableW > btnAreaW + 300) {
        ctrlW = availableW - btnAreaW - 20;
    }

    // Reset contentHeight accumulator (we'll compute bottom-most control in content coords)
    contentHeight = 0;

    // 1) Adapter combobox 和連接按鈕（確保不重疊）
    AdapterRowLayout adapterLayout = ComputeAdapterRowLayout(availableW);
    const int adapterGap = adapterLayout.gap;
    const int btnConnectW = adapterLayout.buttonW;
    const int btnDisconnectW = adapterLayout.buttonW;
    const int btnAutoStartW = adapterLayout.buttonW;
    const int btnAutoStopW = adapterLayout.buttonW;
    int adapterComboW = adapterLayout.comboW;
    
    if (hComboAdapter) {
        RECT rcTmp; GetWindowRect(hComboAdapter, &rcTmp);
        POINT ptTmp = { rcTmp.left, rcTmp.top }; ScreenToClient(hContentWnd, &ptTmp);
        SafeSetPos(hComboAdapter, NULL, ctrlX, ptTmp.y, adapterComboW, 200, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptTmp.y + 200) > contentHeight) contentHeight = ptTmp.y + 200;
    }
    
    // 按鈕位置：緊跟在 combo box 後面（保持 Y 座標和大小）
    int adapterBtnX = ctrlX + adapterComboW + adapterGap;
    if (hBtnConnect) {
        RECT rcBtn; GetWindowRect(hBtnConnect, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SafeSetPos(hBtnConnect, NULL, adapterBtnX, ptBtn.y, btnConnectW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }
    adapterBtnX += btnConnectW + adapterGap;
    if (hBtnDisconnect) {
        RECT rcBtn; GetWindowRect(hBtnDisconnect, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SafeSetPos(hBtnDisconnect, NULL, adapterBtnX, ptBtn.y, btnDisconnectW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }
    adapterBtnX += btnDisconnectW + adapterGap;
    if (hButtonStartAutoMode) {
        RECT rcBtn; GetWindowRect(hButtonStartAutoMode, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SafeSetPos(hButtonStartAutoMode, NULL, adapterBtnX, ptBtn.y, btnAutoStartW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }
    adapterBtnX += btnAutoStartW + adapterGap;
    if (hBtnStopAutoMode) {
        RECT rcBtn; GetWindowRect(hBtnStopAutoMode, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SafeSetPos(hBtnStopAutoMode, NULL, adapterBtnX, ptBtn.y, btnAutoStopW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }

    // 狀態視窗 / 自動視窗 buttons — positioned on their own row, right-aligned
    {
        int btnY = 0;
        if (hComboAdapter && IsWindow(hComboAdapter)) {
            RECT rcTmp; GetWindowRect(hComboAdapter, &rcTmp);
            POINT ptTmp = { rcTmp.left, rcTmp.top }; ScreenToClient(hContentWnd, &ptTmp);
            btnY = ptTmp.y + 22 + LayoutConfig::vpad;
        } else {
            btnY = LayoutConfig::vpad * 2;
        }
        const int btnW = 80;
        const int btnGap = 8;
        int totalBtnW = btnW + btnGap + btnW;
        int btnX = clientW - rightMargin - totalBtnW;
        if (btnX < ctrlX) btnX = ctrlX;
        if (hBtnPDOSentStatus && IsWindow(hBtnPDOSentStatus))
            SafeSetPos(hBtnPDOSentStatus, NULL, btnX, btnY, btnW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        btnX += btnW + btnGap;
        if (hBtnAutoWindow && IsWindow(hBtnAutoWindow))
            SafeSetPos(hBtnAutoWindow, NULL, btnX, btnY, btnW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((btnY + 22) > contentHeight) contentHeight = btnY + 22;
    }

    // --- Cycle-row reflow: Row 1 (Desired + Current), Row 2 (PDOProc + WKC + MMF) ---
    {
        int cycleY1 = 0;
        // attempt to find current Y of existing Desired edit
        if (hEditCycleTime && IsWindow(hEditCycleTime)) {
            RECT clientRc; GetWindowRect(hEditCycleTime, &clientRc);
            POINT pt = { clientRc.left, clientRc.top }; ScreenToClient(hContentWnd, &pt);
            cycleY1 = pt.y;
        } else {
            // fallback: place it after adapter + button rows
            cycleY1 = LayoutConfig::vpad;
            if (hBtnPDOSentStatus && IsWindow(hBtnPDOSentStatus)) {
                RECT rcBtn{}; GetWindowRect(hBtnPDOSentStatus, &rcBtn);
                POINT ptBtn = { rcBtn.left, rcBtn.top }; ScreenToClient(hContentWnd, &ptBtn);
                cycleY1 = ptBtn.y + 22 + LayoutConfig::vpad;
            } else if (hBtnConnect && IsWindow(hBtnConnect)) {
                RECT rcBtn{}; GetWindowRect(hBtnConnect, &rcBtn);
                POINT ptBtn = { rcBtn.left, rcBtn.top }; ScreenToClient(hContentWnd, &ptBtn);
                cycleY1 = ptBtn.y + 22 + LayoutConfig::vpad;
            }
        }

        int cycleAvailableW = clientW - ctrlX - rightMargin;
        if (cycleAvailableW < 400) cycleAvailableW = 400;
        CycleRowLayout cycleLayout = ComputeCycleRowLayout(cycleAvailableW);
        int desiredW = cycleLayout.desiredW;
        int currentW = cycleLayout.currentW;

        // Row 2 column widths: weighted by label length (8:11:9)
        const int weightSum = 8 + 11 + 9;
        int colW[3] = {
            cycleAvailableW * 8 / weightSum,
            cycleAvailableW * 11 / weightSum,
            cycleAvailableW * 9 / weightSum
        };
        int colSum = colW[0] + colW[1] + colW[2];
        if (colSum < cycleAvailableW) colW[1] += (cycleAvailableW - colSum);
        for (int i = 0; i < 3; ++i) if (colW[i] < 180) colW[i] = 180;

        int rowX = ctrlX;
        HDWP hdwp = BeginDeferWindowPos(12);

        // --- Row 1: Desired edit + Current display ---
        int x = rowX;
        if (hEditCycleTime && IsWindow(hEditCycleTime)) {
            hdwp = DeferWindowPos(hdwp, hEditCycleTime, NULL, x, cycleY1, std::max(32, desiredW - 8), 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        x += desiredW;

        int curLabelW = 120;
        int curEditW = std::max(8, currentW - curLabelW - 6);
        HWND hLbl = GetDlgItem(hContentWnd, IDC_STATIC_CYCLE_ACTUAL_LABEL);
        if (hLbl && IsWindow(hLbl))
            hdwp = DeferWindowPos(hdwp, hLbl, NULL, x + 4, cycleY1, curLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hEditCurrentCycleTime && IsWindow(hEditCurrentCycleTime))
            hdwp = DeferWindowPos(hdwp, hEditCurrentCycleTime, NULL, x + 4 + curLabelW + 6, cycleY1, curEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);

        int cycleY2 = cycleY1 + 22 + LayoutConfig::vpad;

        // --- Row 2: PDOProc | WKC | MMF (weighted columns) ---
        x = rowX;

        // PDOProc frequency (col 0)
        int pdoProcLabelW = 90;
        int pdoProcEditW = std::max(8, colW[0] - pdoProcLabelW - 6);
        hLbl = GetDlgItem(hContentWnd, IDC_STATIC_PDO_PROC_LABEL);
        if (hLbl && IsWindow(hLbl))
            hdwp = DeferWindowPos(hdwp, hLbl, NULL, x + 4, cycleY2, pdoProcLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hEditPdoProcFreq && IsWindow(hEditPdoProcFreq))
            hdwp = DeferWindowPos(hdwp, hEditPdoProcFreq, NULL, x + 4 + pdoProcLabelW + 6, cycleY2, pdoProcEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        x += colW[0];

        // WKC (col 1) — wider label to prevent wrapping
        int wkcLabelW = 130;
        int wkcEditW = std::max(8, colW[1] - wkcLabelW - 6);
        hLbl = GetDlgItem(hContentWnd, IDC_STATIC_CYCLE_WKC_LABEL);
        if (hLbl && IsWindow(hLbl))
            hdwp = DeferWindowPos(hdwp, hLbl, NULL, x + 4, cycleY2, wkcLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hEditWKC && IsWindow(hEditWKC))
            hdwp = DeferWindowPos(hdwp, hEditWKC, NULL, x + 4 + wkcLabelW + 6, cycleY2, wkcEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        x += colW[1];

        // MMF frequency (col 2) — wider label to prevent wrapping
        int pdoLabelW = 100;
        int pdoEditW = std::max(8, colW[2] - pdoLabelW - 6);
        hLbl = GetDlgItem(hContentWnd, IDC_STATIC_PDO_SENT_LABEL);
        if (hLbl && IsWindow(hLbl))
            hdwp = DeferWindowPos(hdwp, hLbl, NULL, x + 4, cycleY2, pdoLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if (hEditPDOSent && IsWindow(hEditPDOSent))
            hdwp = DeferWindowPos(hdwp, hEditPDOSent, NULL, x + 4 + pdoLabelW + 6, cycleY2, pdoEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);

        // Commit deferred positions
        if (hdwp) EndDeferWindowPos(hdwp);
    }

    // 2) ESI row: actual positioning is deferred until after SDO controls
    //    are reflowed so we can anchor the ESI controls to the SDO Read
    //    button and avoid overlap. Actual repositioning happens after
    //    calling ReflowSDOControls().

    // 3) SDO parameter ListView - sizing/positioning is handled by ReflowSDOControls()
    ReflowSDOControls(sdoTableHeight);

    // Position ESI controls relative to the SDO Read button so the
    // SDO Read/Write/All buttons remain visible and are not overlapped
    // by the (resizable) ESI combo box.
    
    if (hBtnBrowseESI && IsWindow(hBtnBrowseESI)) {
        const int esiGap = 6;
        const int btnBrowseW = 80;
        const int esiLabelW = 80; // width used for the label

        auto positionEsiLabel = [&](int targetX, int targetY) {
            HWND child = GetWindow(hContentWnd, GW_CHILD);
            while (child) {
                wchar_t cls[64] = {0};
                GetClassNameW(child, cls, _countof(cls));
                if (wcscmp(cls, L"Static") == 0) {
                    wchar_t txt[128] = {0};
                    GetWindowTextW(child, txt, _countof(txt));
                    if (wcsstr(txt, L"ESI") != NULL) {
                        SafeSetPos(child, NULL, targetX, targetY, esiLabelW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
                        break;
                    }
                }
                child = GetWindow(child, GW_HWNDNEXT);
            }
        };

        auto placeBrowseButton = [&](int yPos) {
            int browseW = (btnBrowseW > 60) ? btnBrowseW : 60;
            SafeSetPos(hBtnBrowseESI, NULL, ctrlX, yPos, browseW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
            if ((yPos + 22) > contentHeight) contentHeight = yPos + 22;
        };

        bool haveReadRect = false;
        int readX = 0;
        int readY = 0;
        RECT rcBtn{};
        if (hBtnReadSDO && IsWindow(hBtnReadSDO)) {
            if (GetWindowRect(hBtnReadSDO, &rcBtn) && rcBtn.right > rcBtn.left && rcBtn.bottom > rcBtn.top) {
                POINT ptBtn = { rcBtn.left, rcBtn.top };
                ScreenToClient(hContentWnd, &ptBtn);
                readX = ptBtn.x;
                readY = ptBtn.y;
                haveReadRect = true;
            }
        }

        if (haveReadRect) {
            // Compute the rightmost X available for ESI (leave a small gap)
            int esiRight = readX - 10;
            // compute a maximum allowed combo width so it won't reach the read button
            int maxComboW = esiRight - (ctrlX + esiLabelW + 8) - btnBrowseW - esiGap;
            if (maxComboW < 120) maxComboW = 120;

            // Prefer a reasonable default but cap to maxComboW
            int desiredComboW = availableW - (btnBrowseW + esiGap);
            if (desiredComboW < 120) desiredComboW = 120;
            int esiComboW = (desiredComboW > maxComboW) ? maxComboW : desiredComboW;

            int esiComboX = esiRight - (esiComboW + btnBrowseW + esiGap);
            int esiLabelX = esiComboX - esiLabelW - 8;
            if (esiLabelX < ctrlX) {
                esiLabelX = ctrlX;
                esiComboX = esiLabelX + esiLabelW + 8;
            }

            positionEsiLabel(esiLabelX, readY);
            placeBrowseButton(readY);
        } else {
            // Fallback: derive Y from the SDO label so minimized windows still restore correctly.
            int fallbackY = 0;
            bool haveFallback = false;
            HWND child = GetWindow(hContentWnd, GW_CHILD);
            while (child) {
                wchar_t cls[64] = {0};
                GetClassNameW(child, cls, _countof(cls));
                if (wcscmp(cls, L"Static") == 0) {
                    wchar_t txt[128] = {0};
                    GetWindowTextW(child, txt, _countof(txt));
                    if (wcsstr(txt, L"SDO") != NULL) {
                        RECT rcLabel{};
                        if (GetWindowRect(child, &rcLabel)) {
                            POINT pt = { rcLabel.left, rcLabel.top };
                            ScreenToClient(hContentWnd, &pt);
                            fallbackY = pt.y;
                            haveFallback = true;
                        }
                        break;
                    }
                }
                child = GetWindow(child, GW_HWNDNEXT);
            }
            if (!haveFallback && hListSDOParams && IsWindow(hListSDOParams)) {
                RECT rcList{};
                if (GetWindowRect(hListSDOParams, &rcList)) {
                    POINT pt = { rcList.left, rcList.top };
                    ScreenToClient(hContentWnd, &pt);
                    fallbackY = pt.y;
                    haveFallback = true;
                }
            }
            positionEsiLabel(ctrlX, fallbackY);
            placeBrowseButton(fallbackY);
        }
    }

    // 4) Slave selector combobox
    int slaveComboW = (availableW < 400) ? availableW : 400;
    if (hComboSlaves) {
        // Preserve current height to avoid collapsing the combobox (height=0 makes it invisible)
        RECT rcCombo;
        GetWindowRect(hComboSlaves, &rcCombo);
        int currentH = rcCombo.bottom - rcCombo.top;
        if (currentH <= 0) currentH = 200; // reasonable fallback for dropdown list height
        RECT rcTmp3; GetWindowRect(hComboSlaves, &rcTmp3);
        POINT ptTmp3 = { rcTmp3.left, rcTmp3.top }; ScreenToClient(hContentWnd, &ptTmp3);
        SafeSetPos(hComboSlaves, NULL, ctrlX, ptTmp3.y, slaveComboW, currentH, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptTmp3.y + currentH) > contentHeight) contentHeight = ptTmp3.y + currentH;
    }

    // 5) Slave list width and height - 使用動態高度，靠近視窗右側並填滿可用寬度
    int listWidth = clientW - left - rightMargin; // 使用對稱邊距 + 滾動條保留
    if (listWidth < 200) listWidth = 200;

    // 4.5) HTTP controls and startup checkboxes reflow
    HttpRowLayout httpLayout = ComputeHttpRowLayout(listWidth);
    const int httpBtnW = httpLayout.buttonW;
    const int httpH = 22;
    const int httpChkW = httpLayout.checkboxW;
    if (hBtnOpenHttp && IsWindow(hBtnOpenHttp)) {
        RECT rc; GetWindowRect(hBtnOpenHttp, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hBtnOpenHttp, NULL, left, pt.y, httpBtnW, httpH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hChkHttpServer && IsWindow(hChkHttpServer)) {
        RECT rc; GetWindowRect(hChkHttpServer, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hChkHttpServer, NULL, left + httpBtnW + 8, pt.y, httpChkW, httpH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hStaticHttpUrl && IsWindow(hStaticHttpUrl)) {
        RECT rc; GetWindowRect(hStaticHttpUrl, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hStaticHttpUrl, NULL, left + httpBtnW + 8 + httpChkW + 8, pt.y, httpLayout.urlW, httpH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Startup option checkboxes reflow
    const int chkW = ComputeStartupCheckboxWidth(listWidth);
    const int chkH = 20;
    if (hChkStartupShowMain && IsWindow(hChkStartupShowMain)) {
        RECT rc; GetWindowRect(hChkStartupShowMain, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hChkStartupShowMain, NULL, left, pt.y, chkW, chkH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hChkStartupAutoSeq && IsWindow(hChkStartupAutoSeq)) {
        RECT rc; GetWindowRect(hChkStartupAutoSeq, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hChkStartupAutoSeq, NULL, left + chkW + 8, pt.y, chkW, chkH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hChkMMFPoseOutput && IsWindow(hChkMMFPoseOutput)) {
        RECT rc; GetWindowRect(hChkMMFPoseOutput, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        SafeSetPos(hChkMMFPoseOutput, NULL, left + (chkW + 8) * 2, pt.y, chkW, chkH, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (hListSlaves) {
        // 設定 list 的 X 為 left（維持左側標籤列對齊），寬度更新為計算後的 listWidth
        RECT rcList; GetWindowRect(hListSlaves, &rcList);
        POINT ptList = { rcList.left, rcList.top }; ScreenToClient(hContentWnd, &ptList);
        int newX = left; // 保持與其他控件左邊對齊
    int useSlaveH = slaveListHeight;
    SafeSetPos(hListSlaves, NULL, newX, ptList.y, listWidth, useSlaveH, SWP_NOZORDER | SWP_SHOWWINDOW);
    if ((ptList.y + useSlaveH) > contentHeight) contentHeight = ptList.y + useSlaveH;

        // Recalculate column widths to evenly distribute across ListView client area.
        const int colCount = 8; // known number of columns

        // Get the actual client width of the ListView (accounts for borders)
    RECT rcClientSlaves; GetClientRect(hListSlaves, &rcClientSlaves);
    int lvClientW = rcClientSlaves.right - rcClientSlaves.left;
    // Subtract vertical scrollbar width if present (prevents horizontal gap)
    int vsw_slaves = GetSystemMetrics(SM_CXVSCROLL);
    BOOL hasVScroll_slaves = (GetWindowLong(hListSlaves, GWL_STYLE) & WS_VSCROLL) != 0;
    if (hasVScroll_slaves) lvClientW -= vsw_slaves;
    if (lvClientW < 200) lvClientW = 200;

    // Apply evenly distributed widths but keep station number column narrower (0.5x)
    std::vector<int> colW = CoreHelpers::DistributeColumns(lvClientW, colCount);
    if (!colW.empty()) {
        int orig0 = colW[0];
        int new0 = std::max(24, orig0 / 2);
        int diff = orig0 - new0;
        colW[0] = new0;
        if (diff > 0) {
            int remCols = (int)colW.size() - 1;
            if (remCols > 0) {
                int addPer = diff / remCols;
                int remainder = diff % remCols;
                for (int i = 1; i < (int)colW.size(); ++i) {
                    colW[i] += addPer + ((i == (int)colW.size() - 1) ? remainder : 0);
                }
            } else {
                colW[colW.size() - 1] += diff;
            }
        }
    }
    for (int i = 0; i < (int)colW.size(); ++i) ListView_SetColumnWidth(hListSlaves, i, colW[i]);
    }

    // 5.1) IOmap / PDO ListView column reflow: compute widths from the ListView's
    // actual client width and subtract vertical scrollbar width if present.
    if (hListIOmap && IsWindow(hListIOmap)) {
        RECT rcList; GetWindowRect(hListIOmap, &rcList);
        POINT ptList = { rcList.left, rcList.top }; ScreenToClient(hContentWnd, &ptList);
        // Keep its X aligned with global left and width to available client width
        int newX = left;
        int newW = clientW - left - rightMargin;
        if (newW < 300) newW = 300;
        // Height will be managed elsewhere; keep current height
        int listH = rcList.bottom - rcList.top;
    SafeSetPos(hListIOmap, NULL, newX, ptList.y, newW, listH, SWP_NOZORDER | SWP_SHOWWINDOW);

        // Now recalc columns based on actual ListView client area
    RECT rcClient2{}; GetClientRect(hListIOmap, &rcClient2);
        int lvClientW2 = rcClient2.right - rcClient2.left;
        int vsw2 = GetSystemMetrics(SM_CXVSCROLL);
        BOOL hasVScroll2 = (GetWindowLong(hListIOmap, GWL_STYLE) & WS_VSCROLL) != 0;
        if (hasVScroll2) lvClientW2 -= vsw2;
        if (lvClientW2 < 200) lvClientW2 = 200;

        const int ioCols = 4;
    // Use helper to apply distributed widths for IOmap
    ApplyDistributedColumns(hListIOmap, ioCols);
        if ((ptList.y + listH) > contentHeight) contentHeight = ptList.y + listH;
    }

    // 6) Clear Status and Dump ESI button positions (next to 狀態 button)
    if (hBtnPDOSentStatus && hBtnClearStatus && GetParent(hBtnClearStatus) == hContentWnd) {
        RECT rcStatus; GetWindowRect(hBtnPDOSentStatus, &rcStatus);
        POINT ptStatus = { rcStatus.left, rcStatus.top };
        ScreenToClient(hContentWnd, &ptStatus);
        int clearX = ptStatus.x + (rcStatus.right - rcStatus.left) + 8;
    SafeSetPos(hBtnClearStatus, NULL, clearX, ptStatus.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if ((ptStatus.y + 22) > contentHeight) contentHeight = ptStatus.y + 22;
        
        // Position Dump ESI button next to the selected-slave combobox instead
        // so it's clear the action targets the selected slave.
    
    if (hComboSlaves && IsWindow(hComboSlaves)) {
            RECT rcCombo; GetWindowRect(hComboSlaves, &rcCombo);
            POINT ptCombo = { rcCombo.left, rcCombo.top };
            ScreenToClient(hContentWnd, &ptCombo);
            int comboW = rcCombo.right - rcCombo.left;
            int dumpBtnW = 100; // default width if not measured
            // try to get current width from window rect
                
            int desiredX = ptCombo.x + comboW + 8; // right of combobox
            // ensure we don't overflow into the reserved right area
            int rightBasedX = clientW - rightMargin - dumpBtnW;
            // If not enough horizontal space, wrap button to next line under combobox
            if (desiredX + dumpBtnW > rightBasedX) {
                int wrapX = ptCombo.x; (void)wrapX; // align left with combobox
                int wrapY = ptCombo.y + (rcCombo.bottom - rcCombo.top) + 4; // small vertical gap
                
                if ((wrapY + 22) > contentHeight) contentHeight = wrapY + 22;
            } else {
                int finalX = (desiredX > rightBasedX) ? rightBasedX : desiredX;
                if (finalX < ptCombo.x + comboW + 8) finalX = ptCombo.x + comboW + 8; // keep at least adjacent
                
                if ((ptCombo.y + 22) > contentHeight) contentHeight = ptCombo.y + 22;
            }
        }
    }

    // 7) Mailbox / PDO / Motion reflow extracted
    ReflowMailboxControls(clientW, contentHeight, availableW, left, labelW);
    ReflowPDOControls(clientW, contentHeight, left);
    ReflowMotionControls(clientW, contentHeight, left, labelW);

    

    // Position the content pane according to scrollPos (move the entire content child)
    if (hContentWnd && IsWindow(hContentWnd)) {
        SafeSetPos(hContentWnd, NULL, 0, -scrollPos, clientW, contentHeight > clientH ? contentHeight : clientH, SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    // Setup vertical scrollbar range based on contentHeight vs clientH
    SCROLLINFO si; memset(&si,0,sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (contentHeight > 0) ? (contentHeight) : 0;
    si.nPage = clientH;
    // clamp scrollPos
    if (scrollPos < si.nMin) scrollPos = si.nMin;
    if (scrollPos > si.nMax - (int)si.nPage) scrollPos = si.nMax - (int)si.nPage;
    if (scrollPos < 0) scrollPos = 0;
    si.nPos = scrollPos;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

// Force repaint of main window and several important children
void MainWindow::ForceFullRedraw()
{
    // Skip if minimized or hidden
    if (minimizedToTray || isMinimizedState) return;
    // Throttle full redraws so multiple layout changes in quick succession
    // don't cause repeated repaints. If a redraw occurred recently, schedule
    // one delayed redraw instead of executing again immediately.
    unsigned long long now = GetTickCount64();
    unsigned long long delta = (now > lastFullRedrawTick) ? (now - lastFullRedrawTick) : 0;
    if (delta >= REDRAW_THROTTLE_MS) {
        ForceFullRedrawNow();
        return;
    }
    // Too soon: schedule a delayed redraw once if not already scheduled
    if (!redrawThrottlePending && hwnd && IsWindow(hwnd)) {
        unsigned int remaining = (unsigned int)(REDRAW_THROTTLE_MS - delta + 1);
        redrawThrottlePending = true;
        SetTimer(hwnd, TIMER_REDRAW_THROTTLE, remaining, NULL);
    }
    if (hListSlaves && IsWindow(hListSlaves)) {
        RedrawWindow(hListSlaves, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RedrawWindow(hListSDOParams, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (hListMotionParams && IsWindow(hListMotionParams)) {
        RedrawWindow(hListMotionParams, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    // Note: main window should not redraw or manipulate AutoWindow's internal controls.
    // AutoWindow manages and redraws its own controls; avoid calling GetDlgItem on hAutoWindow here.
    if (hChildStatusWnd && IsWindow(hChildStatusWnd)) {
        RedrawWindow(hChildStatusWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

// Immediate full redraw that bypasses throttle (used by timer callback)
void MainWindow::ForceFullRedrawNow()
{
    // Do nothing if minimized or hidden
    if (minimizedToTray || isMinimizedState) {
        // Cancel any scheduled throttle timer if present
        if (hwnd) KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
        return;
    }
    // Cancel any scheduled throttle timer as we are forcibly redrawing now
    if (hwnd) KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
    if (hwnd && IsWindow(hwnd)) {
        RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }
    if (hContentWnd && IsWindow(hContentWnd)) {
        RedrawWindow(hContentWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    if (hListSlaves && IsWindow(hListSlaves)) {
        RedrawWindow(hListSlaves, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RedrawWindow(hListSDOParams, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    if (hListMotionParams && IsWindow(hListMotionParams)) {
        RedrawWindow(hListMotionParams, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    // Note: main window should not redraw or manipulate AutoWindow's internal controls.
    // AutoWindow manages and redraws its own controls; avoid calling GetDlgItem on hAutoWindow here.
    if (hChildStatusWnd && IsWindow(hChildStatusWnd)) {
        RedrawWindow(hChildStatusWnd, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    }
    lastFullRedrawTick = GetTickCount64();
    redrawThrottlePending = false;
}

// Avoid heavy updates (OnTimer etc) while minimized
// OnTimer/other places should check minimizedToTray || isMinimizedState and skip

// Add a tray icon to the notification area. Safe to call multiple times.
void MainWindow::AddTrayIcon()
{
    if (!useTray || !hwnd) return;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 101;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    nid.hIcon = hTrayIcon ? hTrayIcon : LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"BigCat");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void MainWindow::RemoveTrayIcon()
{
    // Only attempt to remove tray icon if we have a window and tray support
    if (!hwnd || !useTray) return;
    // Attempt to delete the notification icon; ignore failures
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Public method to minimize window to system tray
void MainWindow::MinimizeToTray()
{
    if (!hwnd || !useTray) return;
    isMinimizedState = true;
    minimizedToTray = true;
    AddTrayIcon();
    ShowWindow(hwnd, SW_HIDE);
    // Kill UI timers to pause UI updates while hidden only when not forcing logic while minimized
    if (!this->forceRunWhenMinimized) {
        KillTimer(hwnd, TIMER_ID);
        KillTimer(hwnd, TIMER_JOG_POLL);
        KillTimer(hwnd, TIMER_REDRAW_THROTTLE);
        KillTimer(hwnd, TIMER_EDIT_FOCUS);
    }
}

// Public: Set or clear the topmost (always-on-top) window flag
void MainWindow::SetAlwaysOnTop(bool on)
{
    if (!hwnd) return;
    if (on) {
        // Preserve position and size
        SafeSetPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    } else {
        SafeSetPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
}

// Lightweight scroll updater: avoid heavy reflow/rebuild during wheel/scroll
void MainWindow::UpdateContentPaneScrollOnly()
{
    if (!hwnd || !hContentWnd || !IsWindow(hContentWnd)) return;
    RECT rcClient; GetClientRect(hwnd, &rcClient);
    int clientW = rcClient.right - rcClient.left;
    int clientH = rcClient.bottom - rcClient.top;

    // Keep contentHeight as last computed; only move pane vertically by scrollPos
    int paneH = (contentHeight > 0 ? contentHeight : clientH);
    if (paneH < clientH) paneH = clientH;

    // Use safe wrapper in case content pane was destroyed concurrently
    SafeSetPos(hContentWnd, NULL, 0, -scrollPos, clientW, paneH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Optional: minimal invalidation for smoother experience without full redraw
    // Do not Invalidate the whole window; children will paint as needed
}

// Reflow helper: mailbox
void MainWindow::ReflowMailboxControls(int clientW, int &contentH_local, int &availableW, int left, int labelW) {
    (void)clientW;
    // 7) 重新計算 SDO 列表底部，依序放置 Mailbox -> PDO，避免重疊
    int sdoBottomY = 0;
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcList; GetWindowRect(hListSDOParams, &rcList);
        POINT pt = { rcList.left, rcList.bottom }; ScreenToClient(hContentWnd, &pt);
        sdoBottomY = pt.y;
    } else {
        sdoBottomY = 10 + 200; // fallback
    }

    // Mailbox 區塊：緊貼 SDO 下方 + 足夠間距
    const int mailboxGap = 10;            // edit 與按鈕間
    const int verticalGap = 12;           // 區塊之間垂直間距（增加以防止重疊）
    const int btnSendMailboxW = 105;
    int mailboxY = sdoBottomY + verticalGap;
    int mailboxX = left + labelW + 8;
    int mailboxEditW = availableW - btnSendMailboxW - mailboxGap;
    if (mailboxEditW < 200) mailboxEditW = 200;

    HWND hMailboxLabelHandle = GetDlgItem(hContentWnd, IDC_STATIC_MAILBOX);
    if (hMailboxLabelHandle && IsWindow(hMailboxLabelHandle)) {
        SafeSetPos(hMailboxLabelHandle, NULL, left, mailboxY, labelW, 22, SWP_NOZORDER);
        if ((mailboxY + 22) > contentH_local) contentH_local = mailboxY + 22;
    }
    if (hEditMailbox) SafeSetPos(hEditMailbox, NULL, mailboxX, mailboxY, mailboxEditW, 22, SWP_NOZORDER);
    if (hBtnSendMailbox) {
        int btnMailboxX = mailboxX + mailboxEditW + mailboxGap;
        SafeSetPos(hBtnSendMailbox, NULL, btnMailboxX, mailboxY, btnSendMailboxW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((mailboxY + 22) > contentH_local) contentH_local = mailboxY + 22;
    }
}

// Reflow helper: PDO
void MainWindow::ReflowPDOControls(int clientW, int &contentH_local, int left) {
    (void)clientW;
    const int verticalGap = 12;  // Increased spacing to prevent overlap with motion controls
    // Determine mailbox bottom similar to ReflowMailboxControls
    int sdoBottomY = 0;
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcList; GetWindowRect(hListSDOParams, &rcList);
        POINT pt = { rcList.left, rcList.bottom }; ScreenToClient(hContentWnd, &pt);
        sdoBottomY = pt.y;
    } else {
        sdoBottomY = 10 + 200;
    }
    int mailboxY = sdoBottomY + verticalGap;
    int pdoTop = mailboxY + 22 + verticalGap; // edit 高度 22 + additional spacing
    int pdoRightMargin = GetScrollSafeRightInset();
    int pdoWidth = clientW - left - pdoRightMargin;
    if (pdoWidth < 300) pdoWidth = 300;
    pdoDisplay.ResizeWidth(pdoWidth);
    pdoDisplay.SetTop(pdoTop);
    // ResizeWidth already repositions all PDO item controls; do NOT call
    // RebuildPDOControls here — it would destroy/recreate every HWND and
    // lose the live display values.
    ApplyAutoProcedureUiLockState();
    if ((pdoTop + pdoDisplay.GetHeight()) > contentH_local) contentH_local = pdoTop + pdoDisplay.GetHeight();
}

// Reflow helper: Motion
void MainWindow::ReflowMotionControls(int clientW, int &contentH_local, int left, int labelW) {
    (void)clientW;
    // Reposition motion control area so it follows the PDO display
    int motionBtnH = 24;
    int motionBtnGap = 8;
    int motionBtnW = 80;
    int stopBtnW = motionBtnW * 2;

    // Additional vertical spacing between PDO and motion controls to prevent overlap
    const int pdo_motion_spacing = 12; // pixels between PDO bottom and motion control top

    int pdoTop = 0;
    int motionListBottom = 0; // bottom Y of motion params list in content coords
    // compute pdoTop using PDODisplay top (try GetTop via groupbox)
    // Best-effort: if PDODisplay exposes GetGroupBox, use it to compute top
    HWND gb = pdoDisplay.GetGroupBox();
    if (gb && IsWindow(gb)) {
        RECT rc; GetWindowRect(gb, &rc);
        POINT pt = { rc.left, rc.top }; ScreenToClient(hContentWnd, &pt);
        pdoTop = pt.y;
    }
    int motionTop = pdoTop + pdoDisplay.GetHeight() + pdo_motion_spacing;
    int firstRowY = motionTop;
    // use motionBtnGap so spacing follows the motion button layout (從動關係)
    int secondRowY = firstRowY + motionBtnH + motionBtnGap;

    HWND hMotionLabel = GetDlgItem(hContentWnd, IDC_STATIC_MOTION_CONTROL);
    if (hMotionLabel && IsWindow(hMotionLabel)) {
        SafeSetPos(hMotionLabel, NULL, left, firstRowY, labelW, 22, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if ((firstRowY + 22) > contentH_local) contentH_local = firstRowY + 22;
    }

    // First row buttons
    int bx = left + labelW + 8;
    if (hBtnFollowMode && IsWindow(hBtnFollowMode)) { SafeSetPos(hBtnFollowMode, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnPowerOn && IsWindow(hBtnPowerOn)) { SafeSetPos(hBtnPowerOn, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnPowerOff && IsWindow(hBtnPowerOff)) { SafeSetPos(hBtnPowerOff, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnHome && IsWindow(hBtnHome)) { SafeSetPos(hBtnHome, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnJogCW && IsWindow(hBtnJogCW)) { SafeSetPos(hBtnJogCW, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnJogCCW && IsWindow(hBtnJogCCW)) { SafeSetPos(hBtnJogCCW, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }
    if (hBtnFaultReset && IsWindow(hBtnFaultReset)) { SafeSetPos(hBtnFaultReset, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }

    if (hBtnImmediateStop && IsWindow(hBtnImmediateStop)) { SafeSetPos(hBtnImmediateStop, NULL, bx, firstRowY, stopBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += stopBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentH_local) contentH_local = firstRowY + motionBtnH; }

    if ((secondRowY + motionBtnH + motionBtnGap) > contentH_local) contentH_local = secondRowY + motionBtnH + motionBtnGap;

    // Config buttons position (directly under motion button row; no extra blank line)
    const int cfgBtnW = 100;
    const int cfgBtnH = 24;
    const int cfgBtnGap = 8;
    // Align directly below the first row of motion buttons
    int cfgBtnY = secondRowY;
    int cfgBtnX = left + labelW + 8;
    
    if (hBtnLoadConfig && IsWindow(hBtnLoadConfig)) {
        SafeSetPos(hBtnLoadConfig, NULL, cfgBtnX, cfgBtnY, cfgBtnW, cfgBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        cfgBtnX += cfgBtnW + cfgBtnGap;
        if ((cfgBtnY + cfgBtnH) > contentH_local) contentH_local = cfgBtnY + cfgBtnH;
    }
    if (hBtnSaveConfig && IsWindow(hBtnSaveConfig)) {
        SafeSetPos(hBtnSaveConfig, NULL, cfgBtnX, cfgBtnY, cfgBtnW, cfgBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if ((cfgBtnY + cfgBtnH) > contentH_local) contentH_local = cfgBtnY + cfgBtnH;
    }

    // Motion params list position (below config buttons)
    if (hListMotionParams && IsWindow(hListMotionParams)) {
    // compute listTop using motionBtnGap so it's dependent on button layout
    int listTop = cfgBtnY + cfgBtnH + cfgBtnGap;
        int listLeft = left;
        const int rightInset = GetScrollSafeRightInset();
        int listW = clientW - left - rightInset;
        if (listW < 300) listW = 300;
        int listH = 140;
    SafeSetPos(hListMotionParams, NULL, listLeft, listTop, listW, listH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    motionListBottom = listTop + listH;
    if (motionListBottom > contentH_local) contentH_local = motionListBottom;

        if (hListMotionParams && IsWindow(hListMotionParams)) {
            RECT rcClientMotion{};
            if (GetClientRect(hListMotionParams, &rcClientMotion)) {
                int lvW_motion = rcClientMotion.right - rcClientMotion.left;
                int vsw_motion = GetSystemMetrics(SM_CXVSCROLL);
                BOOL hasV_motion = (GetWindowLong(hListMotionParams, GWL_STYLE) & WS_VSCROLL) != 0;
                if (hasV_motion) lvW_motion -= vsw_motion;
                if (lvW_motion < 200) lvW_motion = 200;
                const int colCount = 6;
                int base_motion = lvW_motion / colCount;
                for (int i = 0; i < colCount; ++i) {
                    if (i == colCount - 1) {
                        int sum = base_motion * (colCount - 1);
                        int lastW = lvW_motion - sum;
                        ListView_SetColumnWidth(hListMotionParams, i, lastW);
                    } else {
                        ListView_SetColumnWidth(hListMotionParams, i, base_motion);
                    }
                }
                ShrinkColumnsWithRedistribute(hListMotionParams, {0, 2, 4}, 0.5, colCount - 1);
            }
        }
    }

    // Reflow mechanical units list (placed under motion params list)
    if (hListMechUnits && IsWindow(hListMechUnits)) {
        // compute top based on motion params list bottom
        int mechTop = 0;
        if (motionListBottom > 0) {
            mechTop = motionListBottom + motionBtnGap;
        } else {
            RECT rcMotion{};
            if (GetWindowRect(hListMotionParams, &rcMotion)) {
                POINT pt = { rcMotion.left, rcMotion.bottom }; ScreenToClient(hContentWnd, &pt);
                mechTop = pt.y + motionBtnGap; // small gap under motion params
            } else {
                mechTop = secondRowY + motionBtnH + motionBtnGap;
            }
        }
        // Stronger safety: ensure mechTop is at least below both the
        // already-computed content bottom and the motion list bottom
        // with an increased gap so the two listviews won't touch.
        const int extraSafety = motionBtnGap + 8; // slightly larger gap
        if (contentH_local + extraSafety > mechTop) mechTop = contentH_local + extraSafety;
        if (motionListBottom > 0) {
            int minFromMotion = motionListBottom + motionBtnGap + 8; // extra padding after motion list
            if (mechTop < minFromMotion) mechTop = minFromMotion;
        }
        int mechLeft = left;
        const int rightInset = GetScrollSafeRightInset();
        int mechW = clientW - left - rightInset;
        if (mechW < 300) mechW = 300;
        // Compute mech list height to show exactly 6 rows (header + 6 items)
        int mechH = 0;
        // Measure header and item heights if possible
        if (hListMechUnits && IsWindow(hListMechUnits)) {
            int headerH = 0;
            HWND hHeader = ListView_GetHeader(hListMechUnits);
            if (hHeader && IsWindow(hHeader)) {
                RECT rcH; GetWindowRect(hHeader, &rcH);
                headerH = rcH.bottom - rcH.top;
            }
            int itemH = 0;
            RECT rcItem{};
            int itemCount = ListView_GetItemCount(hListMechUnits);
            if (itemCount > 0) {
                if (ListView_GetItemRect(hListMechUnits, 0, &rcItem, LVIR_BOUNDS)) itemH = rcItem.bottom - rcItem.top;
            }
            if (itemH <= 0) itemH = MeasureTextHeight(hGuiFont);
            if (headerH <= 0) headerH = itemH + 6;
            mechH = headerH + (itemH * 6) + 4; // small padding
        } else {
            int rowH = MeasureTextHeight(hGuiFont);
            mechH = (rowH + 6) + (rowH * 6) + 4;
        }
        // Position the static label above the list if present
        const int mechLabelH = 20;
        const int mechLabelGap = 6; // gap between label and list
        int labelTop = mechTop; // place label at mechTop
        int listTopFinal = labelTop + mechLabelH + mechLabelGap;
        if (hStaticMechUnitsLabel && IsWindow(hStaticMechUnitsLabel)) {
            SafeSetPos(hStaticMechUnitsLabel, NULL, mechLeft, labelTop, mechW, mechLabelH, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);
            if ((listTopFinal + mechH) > contentH_local) contentH_local = listTopFinal + mechH;
        } else {
            // If no label, keep previous placement semantics
            listTopFinal = mechTop;
            if ((listTopFinal + mechH) > contentH_local) contentH_local = listTopFinal + mechH;
        }
        SafeSetPos(hListMechUnits, NULL, mechLeft, listTopFinal, mechW, mechH, SWP_NOZORDER | SWP_SHOWWINDOW | SWP_NOACTIVATE);

        // adjust columns evenly using actual client width
        RECT rcClientMech{}; GetClientRect(hListMechUnits, &rcClientMech);
        int lvW_mech = rcClientMech.right - rcClientMech.left;
        int vsw_mech = GetSystemMetrics(SM_CXVSCROLL);
        BOOL hasV_mech = (GetWindowLong(hListMechUnits, GWL_STYLE) & WS_VSCROLL) != 0;
        if (hasV_mech) lvW_mech -= vsw_mech;
        if (lvW_mech < 200) lvW_mech = 200;
        const int mechCols = 7; // now includes Station column
        int base_mech = lvW_mech / mechCols;
        for (int i = 0; i < mechCols; ++i) {
            if (i == mechCols - 1) {
                int sum = base_mech * (mechCols - 1);
                int lastW = lvW_mech - sum;
                ListView_SetColumnWidth(hListMechUnits, i, lastW);
            } else {
                ListView_SetColumnWidth(hListMechUnits, i, base_mech);
            }
        }
        ShrinkColumnWidthWithRedistribute(hListMechUnits, 0, 0.5, mechCols - 1);

        if ((mechTop + mechH) > contentH_local) contentH_local = mechTop + mechH;
        // Position MMF static label immediately below mechanical units list
        // (previously used layout variables were removed as they are not needed here)
        // AutoWindow 管理自身控件位置與大小；主視窗不再為 AutoWindow 的控件預留或調整 content 高度。
    }
}

// OnConnect moved to MainWindow.Logic.cpp

// 使用 pcap 列出可用的網路介面並填入下拉選單
