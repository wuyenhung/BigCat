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
#include "../include/core/Cia402Mode.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../include/EtherCATMaster.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
#include "../include/MotionLogic.h"
#include "../include/AutoWindow.h"
// Local INI helpers used by connect flow.
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

static bool ReadIniBool(const wchar_t* section, const wchar_t* key, bool defaultValue)
{
    std::wstring ini = GetAppIniPath();
    int val = GetPrivateProfileIntW(section, key, defaultValue ? 1 : 0, ini.c_str());
    return val != 0;
}

static void EnsureSimulationMmf1InputRunningAfterConnect(
    const std::string& ifname,
    const std::function<void(const std::string&)>& post)
{
    if (!EtherCATMaster::IsSimulationAdapterSelection(ifname)) {
        return;
    }

    auto* master = AppController::GetInstance().GetMaster();
    if (!master || !master->IsInitialized() || master->IsMMF1InRunning()) {
        return;
    }

    if (master->StartMMF1In()) {
        post(std::string("[Connect] MMF1_IN started after simulation connect\n"));
    } else {
        post(std::string("[Connect] WARNING: failed to start MMF1_IN after simulation connect\n"));
    }
}

void MainWindow::OnConnect(const std::string* overrideIfName) {
    this->cyclicPdoCycleDetectReady.store(false);
    this->linkDownSinceTick = 0;
    this->linkLossDisconnectRequested = false;
    if (!AppController::GetInstance().GetMaster()) {
        // This case should not happen once AppController has been initialized; report for diagnostics.
        try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: master unavailable\n")); } catch(...) {}
        return;
    }
    try {
        std::ostringstream os;
        os << "[Connect] OnConnect invoked: overrideIfName=" << ((overrideIfName && !overrideIfName->empty()) ? *overrideIfName : std::string("(none)"))
           << " currentIfName=" << this->currentIfName
           << " workerRunning=" << (workerRunning.load() ? "1" : "0")
           << " IsMasterInitialized=" << (IsMasterInitialized() ? "1" : "0")
           << " disconnecting=" << (disconnecting.load() ? "1" : "0")
           << " connectAttemptInProgress=" << (connectAttemptInProgress.load() ? "1" : "0")
           << "\n";
        MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:") + os.str());
    } catch(...) {}
    if (disconnecting.load()) {
        PrependStatus(L"請等待斷開完成後再連接\r\n");
            try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: disconnect in progress\n")); } catch(...) {}
        return;
    }
    // Prevent concurrent attempt invocations: this ensures a single client/worker gets to run
    if (connectAttemptInProgress.exchange(true)) {
        PrependStatus(L"[Connect] 另一個連線嘗試正在進行中，忽略此呼叫\r\n");
            try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: connect in progress\n")); } catch(...) {}
        return;
    }
    if (workerRunning.load()) {
        PrependStatus(L"連線程序已在執行中，請稍候...\r\n");
            try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: workerRunning flag is set\n")); } catch(...) {}
        return;
    }
    // Assign an attempt id for diagnostics and create a guard to clear the attempt flag
    uint64_t attempt = ++connectAttemptId;
    struct ConnectAttemptGuard {
        MainWindow* w; bool released;
        ConnectAttemptGuard(MainWindow* ww) : w(ww), released(false) {}
        void release() { released = true; }
        ~ConnectAttemptGuard() { if (!released) w->connectAttemptInProgress.store(false); }
    } guard(this);

    // If we mark the server-side 'connect' disabled at the start of the flow,
    // ensure we undo that when the connect attempt fails/aborts. On success the
    // guard will be released (so it does not revert the server state).
    struct ServerConnectGuard {
        bool released;
        ServerConnectGuard(): released(false) {}
        void release() { released = true; }
        ~ServerConnectGuard() { if (!released) SetUiDisabled("connect", false); }
    };
    auto serverConnectGuard = std::make_shared<ServerConnectGuard>();

    // Immediately disable cycle-time edit to prevent user modifications while connecting
    if (hEditCycleTime && IsWindow(hEditCycleTime)) {
        SetControlEnabled(hEditCycleTime, false);
        // Mirror to server UI state so web clients can disable their cycle input too
        SetUiDisabled("set_cycle", true);
    }

    // 取得欲連線的介面名稱（優先使用已快取，其次從下拉選單）
    std::string ifname;
    if (!ResolveConnectInterfaceName(overrideIfName, ifname)) {
        return;
    }

    std::string normalizedIf = NormalizeInterfaceName(ifname);
    if (normalizedIf != ifname) {
        if (!overrideIfName) this->currentIfName = normalizedIf;
        std::wstring w = CoreHelpers::Utf8ToWide(std::string("Normalized adapter: ") + normalizedIf + "\n");
        PrependStatus(w.c_str());
        ifname = normalizedIf;
    }

    // Before checking link status, re-resolve ifname in case the adapter list changed.
    TryResolveAdapterPcapName(ifname);
    this->currentIfName = ifname;
    this->webSelectedIfName = ifname;

    // 檢查鏈路狀態 (若首次檢查失敗，自動重試幾次並重新列舉 adapter)
    const bool simulationSelected = EtherCATMaster::IsSimulationAdapterSelection(ifname);
    if (!simulationSelected && !EtherCATMaster::IsPhysicalEthercatAllowed()) {
        std::string reason = EtherCATMaster::GetPhysicalEthercatRestrictionReason();
        if (reason.empty()) reason = "platform policy";
        PrependStatus((std::wstring(L"實體 EtherCAT 通訊已停用：") + CoreHelpers::Utf8ToWide(reason) + L"\r\n").c_str());
        try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: physical EtherCAT disabled: ") + reason + "\n"); } catch(...) {}
        SetUiDisabled("connect", false);
        SetUiDisabled("adapter_select", false);
        SetUiDisabled("set_cycle", false);
        SetControlEnabled(hBtnConnect, true);
        if (hComboAdapter && IsWindow(hComboAdapter)) SetControlEnabled(hComboAdapter, true);
        if (hEditCycleTime && IsWindow(hEditCycleTime)) SetControlEnabled(hEditCycleTime, true);
        return;
    }
    bool linkUp = simulationSelected ? true : CoreHelpers::CheckLinkStatus(ifname.c_str());
    if (!linkUp) {
        const int linkRetries = 3;
        const int linkRetryDelayMs = 500; // 0.5s
        PostUTF8LogToUI(std::string("Link check failed for: ") + ifname + " - attempting retries...\n");
        for (int ri = 0; ri < linkRetries && !linkUp && !disconnecting.load(); ++ri) {
            // Attempt a brief delay to allow NIC to settle
            for (int ms = 0; ms < linkRetryDelayMs && !disconnecting.load(); ms += 50) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            // Re-enumerate adapters and try to re-resolve pcapName if changed.
            TryResolveAdapterPcapName(ifname);
            linkUp = EtherCATMaster::IsSimulationAdapterSelection(ifname) ? true : CoreHelpers::CheckLinkStatus(ifname.c_str());
            if (linkUp) PostUTF8LogToUI(std::string("Link check recovered for: ") + ifname + "\n");
            else PostUTF8LogToUI(std::string("Link check retry failed for: ") + ifname + "\n");
        }
    }
    if (!linkUp) {
        PrependStatus(L"錯誤：網路介面沒有檢測到鏈路連接。\r\n");
        MarkConnectionWorkComplete();
        SetUiDisabled("connect", false);
        SetControlEnabled(hBtnConnect, true);
        if (hComboAdapter && IsWindow(hComboAdapter)) SetControlEnabled(hComboAdapter, true);
        return;
    }

    // --- 自動載入 Par\motion.xml: 若載入失敗則中止連線流程 ---
    if (!LoadDefaultMotionConfigForConnect()) {
        return;
    }

    // UI 綁定與狀態設定
    SetControlEnabled(hBtnConnect, false);
    // Immediately mirror to server UI state so web clients disable their Connect button
    // as soon as the PC Connect flow begins (prevents race where web stays enabled until
    // connect completes).
    SetUiDisabled("connect", true);
    // Allow user to interrupt connection: enable Disconnect during connect
    // so a user can cancel an in-progress connect attempt.
    SetControlEnabled(hBtnDisconnect, true);
    // Ensure web clients see the Disconnect button enabled during the connect flow
    SetUiDisabled("disconnect", false);
    // Immediately disable config Load while connecting to avoid mid-connection changes
    if (hBtnLoadConfig) SetControlEnabled(hBtnLoadConfig, false);
    // Disable adapter selection while connecting/connected
    if (hComboAdapter) SetControlEnabled(hComboAdapter, false);
    SetUiDisabled("adapter_select", true);
                // Keep disconnect enabled during connect so user can cancel
    SetBusyFlagAndNotify(workerRunning, true);
    // Release the guard: the worker thread will take responsibility for clearing the flag
    guard.release();
    // Annotate start with attempt id for diagnostics
    try { PostUTF8LogToUI(std::string("[Connect attempt ") + std::to_string((unsigned long long)attempt) + "] starting\n"); } catch(...) {}
    this->currentIfName = ifname;

    // 讀取欲設定之週期（ms）
    wchar_t cycleTimeW[32];
    GetWindowTextW(hEditCycleTime, cycleTimeW, 32);
    double desiredCycleTimeMs = wcstod(cycleTimeW, NULL);
    
    // 🔧 範圍驗證：0.5~10 ms，超出範圍自動設為 2 ms
    if (desiredCycleTimeMs < 0.5 || desiredCycleTimeMs > 10.0) {
        desiredCycleTimeMs = 2.0;
        PostUTF8LogToUI(std::string("[連線] 週期超出範圍 (0.5~10 ms)，自動設為 2 ms\n"));
        // 同時更新 UI 欄位顯示
        SetWindowTextW(hEditCycleTime, L"2");
    }
    
    // 轉換為微秒（us）給 EtherCAT 使用
    uint32_t desiredCycleTime = (uint32_t)(desiredCycleTimeMs * 1000.0);

    // 背景執行連線流程
    std::thread([this, ifname, desiredCycleTime, attempt, serverConnectGuard]() mutable {
        int prevThreadPrio = GetThreadPriority(GetCurrentThread());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        std::string attemptPrefix = std::string("[Connect attempt ") + std::to_string((unsigned long long)attempt) + "] ";
        HWND hwnd = this->hwnd;
        auto post = [attemptPrefix, hwnd](const std::string &s){ MainWindow::PostUTF8LogToHWND(hwnd, attemptPrefix + s); };

        if (AbortConnectIfDisconnectRequested("Connection aborted by user before initialization\n", post)) { return; }
        OptimizeAdapterForEthercat(ifname, post);

        if (!InitializeMasterWithFallback(ifname, desiredCycleTime, post)) {
            SetThreadPriority(GetCurrentThread(), prevThreadPrio);
            return;
        }

        post(std::string("Initialization successful!\n"));
        if (AbortConnectIfDisconnectRequested("Connection aborted by user after initialization\n", post)) { return; }

    post(std::string("Updating slave list...\n"));
        PostMessage(hwnd, WM_APP, 0, 0);

        if (AbortConnectIfDisconnectRequested("Connection aborted by user before configuring slaves\n", post)) { return; }

        post(std::string("發送初始ProcessData以準備配置...\n"));
        for (int i = 0; i < 5; ++i) { MasterSendProcessDataOnce(); Sleep(5); } // Reduced from 10x10ms

        post(std::string("配置從站使用SOEM自動配置...\n"));
        bool configOk = MasterConfigureSlaves();
        if (!configOk) {
            HandleConfigureSlavesFailure(MasterGetLastError(), post);
            return;
        }

        int slaveCount = MasterGetSlaveCount();
        post(std::string("All ") + std::to_string(slaveCount) + " slaves configured successfully\n");

        {
            post(std::string("All slaves configured, starting cyclic communication...\n"));
            post(std::string("Sending initial processdata (SOEM standard)...\n"));
            for (int i = 0; i < 20; ++i) { if (AbortConnectIfDisconnectRequested("Connection aborted by user during initial processdata\n", post)) { return; } MasterSendProcessDataOnce(); DWORD delayMs = (DWORD)(desiredCycleTime / 1000); if (delayMs == 0) delayMs = 1; Sleep(delayMs); } // Reduced from 50 iterations

            if (!WriteSafeStartupControlword(post)) {
                NotifyEthercatError(true);
                PostAutoStatusUpdate();
                post(std::string("[Connect] 連線異常：安全控制字寫入失敗\n"));
                MarkConnectionWorkComplete();
                PostAutoStatusUpdate();
                SetThreadPriority(GetCurrentThread(), prevThreadPrio);
                return;
            }
            WriteStartupMotionParams(post);

            post(std::string("Requesting OP state for all slaves...\n"));
            if (AbortConnectIfDisconnectRequested("Connection aborted by user before starting cyclic operation\n", post)) { return; }
            if (!MasterStartCyclicOperation()) {
                HandleStartCyclicOperationFailure(MasterGetLastError(), post);
            } else {
                post(std::string("EtherCAT communication active\n"));
                PostUTF8LogToUI(std::string("EtherCAT \u5df2\u555f\u52d5!\n"));
                // Clear ethercat error flag now that EtherCAT is running
                NotifyEthercatError(false);

                VerifyCyclicModeAndPdoReadiness(desiredCycleTime, post);
                
                // 啟動 0x603F 錯誤碼循環檢查
                StartErrorCodeCheck();
                
                StartPostConnectSdoReader();
                
                FinalizeSuccessfulConnection(
                    [serverConnectGuard]() {
                        try { serverConnectGuard->release(); } catch(...) {}
                    },
                    post);
            }
        }

        MarkConnectionWorkComplete();
            PostAutoStatusUpdate();
        SetThreadPriority(GetCurrentThread(), prevThreadPrio);
    }).detach();
}


void MainWindow::OnDisconnect() {
    // Extra safety: verify window handle is still valid
    if (!IsWindow(hwnd)) {
        disconnecting.store(false);
        return;
    }
    
    if (!AppController::GetInstance().GetMaster()) {
        disconnecting.store(false);
        return;
    }

    // Prevent re-entry using atomic exchange
    if (disconnecting.exchange(true)) {
        return;
    }

    BeginDisconnectUiTransition();
    auto releaseServerDisconnectGuard = CreateServerDisconnectGuardRelease();
    
    PrepareDisconnectShutdown();

    bool hasThreads = MasterHasActiveSlaveThreads();
    // Capture current hwnd early so both the worker and the synchronous path
    // can safely reference it without relying on `this` after window destruction.
    HWND capturedHwnd = this->hwnd;
    DispatchDisconnectShutdown(
        hasThreads,
        capturedHwnd,
        releaseServerDisconnectGuard);

    // CompleteDisconnect is executed once the worker posts SHUTDOWN_COMPLETE,
    // so we simply return here.
    return;
}

void MainWindow::BeginDisconnectUiTransition() {
    // Immediately disable disconnect button to prevent re-entry.
    if (hBtnDisconnect && IsWindow(hBtnDisconnect)) {
        SetControlEnabled(hBtnDisconnect, false);
    }
    // Notify server immediately so web clients observe the in-progress disconnect.
    SetUiDisabled("disconnect", true);
}

std::function<void()> MainWindow::CreateServerDisconnectGuardRelease() {
    // Revert the server-side disabled state if disconnect does not complete.
    struct ServerDisconnectGuard {
        bool released;
        ServerDisconnectGuard(): released(false) {}
        void release() { released = true; }
        ~ServerDisconnectGuard() { if (!released) SetUiDisabled("disconnect", false); }
    };

    auto serverDisconnectGuard = std::make_shared<ServerDisconnectGuard>();
    return [serverDisconnectGuard]() {
        try { serverDisconnectGuard->release(); } catch(...) {}
    };
}

void MainWindow::DispatchDisconnectShutdown(
    bool hasThreads,
    HWND capturedHwnd,
    const std::function<void()> &releaseServerDisconnectGuard) {
    if (hasThreads && IsWindow(hwnd)) {
        PrependStatus(L"正在安全退出所有裝置...\r\n");
    }

    if (hasThreads) {
        StartAsyncDisconnectShutdown(capturedHwnd, releaseServerDisconnectGuard);
        StartDisconnectShutdownWatchdog(capturedHwnd, releaseServerDisconnectGuard);
        return;
    }

    // Synchronous shutdown path: run MasterShutdownSync() off the UI thread
    // to avoid blocking the UI. The worker will Post a SHUTDOWN_COMPLETE
    // marker so the UI can finalize via CompleteDisconnect().
    StartSynchronousDisconnectShutdown(capturedHwnd, releaseServerDisconnectGuard);
}

void MainWindow::StartAsyncDisconnectShutdown(
    HWND capturedHwnd,
    const std::function<void()> &releaseServerDisconnectGuard) {
    // Delay calling CompleteDisconnect until the master shutdown completes.
    // Keep `disconnecting` set so UI shows Busy (oBusy) during the operation.
    try {
        std::thread([this, capturedHwnd, releaseServerDisconnectGuard]() {
            try {
                if (!IsWindow(capturedHwnd)) {
                    disconnecting.store(false);
                    return;
                }

                MasterShutdownWithCallback([this, capturedHwnd](const std::string &msg) {
                    if (IsWindow(capturedHwnd)) {
                        PostUTF8LogToUI(msg);
                    }
                });

                releaseServerDisconnectGuard();
                PostShutdownCompleteOrClearFlag(capturedHwnd);
            } catch (const std::exception& e) {
                try {
                    if (IsWindow(capturedHwnd)) {
                        ReportShutdownExceptionAndComplete(
                            capturedHwnd,
                            std::string("[ERROR] Shutdown exception: ") + e.what() + "\n");
                    } else {
                        disconnecting.store(false);
                    }
                } catch (...) {
                    disconnecting.store(false);
                }
            } catch (...) {
                try {
                    if (IsWindow(capturedHwnd)) {
                        ReportShutdownExceptionAndComplete(
                            capturedHwnd,
                            std::string("[ERROR] Unknown shutdown exception\n"));
                    } else {
                        disconnecting.store(false);
                    }
                } catch (...) {
                    disconnecting.store(false);
                }
            }
        }).detach();
    } catch (...) {
        if (IsWindow(hwnd)) {
            PostUTF8LogToUI(std::string("[WARN] Failed to start asynchronous shutdown worker thread\n"));
        }
    }
}

void MainWindow::StartDisconnectShutdownWatchdog(
    HWND capturedHwnd,
    const std::function<void()> &releaseServerDisconnectGuard) {
    // Ensure UI doesn't hang waiting for SHUTDOWN_COMPLETE indefinitely.
    try {
        const int kWatchdogMs = 2000;
        std::thread([this, capturedHwnd, kWatchdogMs, releaseServerDisconnectGuard]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(kWatchdogMs));
            try {
                if (disconnecting.load()) {
                    if (IsWindow(capturedHwnd)) {
                        PostUTF8LogToUI(std::string("[WARN] Shutdown watchdog: forcing SHUTDOWN_COMPLETE\n"));
                        releaseServerDisconnectGuard();
                        PostShutdownCompleteOrClearFlag(capturedHwnd);
                    } else {
                        disconnecting.store(false);
                    }
                }
            } catch (...) {}
        }).detach();
    } catch (...) {}
}

void MainWindow::StartSynchronousDisconnectShutdown(
    HWND capturedHwnd,
    const std::function<void()> &releaseServerDisconnectGuard) {
    try {
        std::thread([this, capturedHwnd, releaseServerDisconnectGuard]() {
            try {
                MasterShutdownSync();
                releaseServerDisconnectGuard();
                PostShutdownCompleteOrClearFlag(capturedHwnd);
            } catch (const std::exception& e) {
                if (IsWindow(capturedHwnd)) {
                    PostUTF8LogToUI(std::string("[ERROR] Shutdown sync exception: ") + std::string(e.what()) + "\n");
                }
            } catch (...) {
                if (IsWindow(capturedHwnd)) {
                    PostUTF8LogToUI(std::string("[ERROR] Unknown shutdown sync exception\n"));
                }
            }
        }).detach();
    } catch (...) {
        if (IsWindow(hwnd)) {
            PostUTF8LogToUI(std::string("[WARN] Failed to start synchronous shutdown worker thread\n"));
        }
    }
}

void MainWindow::PostShutdownCompleteOrClearFlag(HWND capturedHwnd) {
    if (IsWindow(capturedHwnd)) {
        PostUTF8LogToUI(std::string("SHUTDOWN_COMPLETE\n"));
    } else {
        disconnecting.store(false);
    }
}

void MainWindow::ReportShutdownExceptionAndComplete(
    HWND capturedHwnd,
    const std::string &message) {
    PostUTF8LogToUI(message);
    PostShutdownCompleteOrClearFlag(capturedHwnd);
}

void MainWindow::ResetDisconnectedRuntimeState() {
    this->linkDownSinceTick = 0;
    this->linkLossDisconnectRequested = false;
    this->sdo60FDFailureStreak.store(0);
    this->sdo60FDConsecutiveFailureCount.store(0);
    this->sdo60FDFailureSinceTick.store(0);
    this->lastSdo60FDFailureTick.store(0);
    this->sdo60FDDisconnectRequested.store(false);
    PostAutoStatusUpdate();
    this->maxObservedCycleTime = 0;
    this->cyclicPdoCycleDetectReady.store(false);
    if (hEditCurrentCycleTime && IsWindow(hEditCurrentCycleTime)) {
        SetWindowTextW(hEditCurrentCycleTime, L"0.00 ms / 0.00 ms");
    }
    MasterResetMaxObservedCommsTime();
    MasterResetMaxObservedCycleMicros();
}

void MainWindow::PrepareDisconnectShutdown() {
    // Abort any running auto workflows before starting shutdown.
    this->autoDownToOriginAbortRequested.store(true);

    // Stop error-code polling while the master still exists.
    if (IsWindow(hwnd)) {
        PrependStatus(L"停止錯誤碼檢查...\r\n");
    }
    StopErrorCodeCheck();

    // Stop auto procedure if running. StopAutoProcedure() is UI-safe and
    // already owns the related UI state transitions.
    bool isSdoReaderRunning = false;
    if (auto* master = AppController::GetInstance().GetMaster()) {
        isSdoReaderRunning = master->IsSdoReaderRunning();
    }
    if (IsAutoProcedureRunning() || isSdoReaderRunning) {
        if (IsWindow(hwnd)) {
            PrependStatus(L"停止自動程序...\r\n");
        }
        StopAutoProcedure();
    }

    // Stop timer first so OnTimer cannot race the shutdown path.
    if (IsWindow(hwnd)) {
        KillTimer(hwnd, TIMER_ID);
    }

    // Publish disconnect state to UI/AutoWindow before worker-side teardown.
    NotifyEthercatError(true);
    PostAutoStatusUpdate();

    if (workerRunning.load()) {
        if (IsWindow(hwnd)) {
            PrependStatus(L"等待連線程序結束中...\r\n");
        }
        MarkConnectionWorkComplete();
        PostAutoStatusUpdate();
    }
}


void MainWindow::CompleteDisconnect() {
    // Safety: verify window is still valid
    if (!IsWindow(hwnd)) {
        disconnecting.store(false);
        return;
    }
    
    // Ensure timer is stopped before clearing PDO items
    KillTimer(hwnd, TIMER_ID);
    
    // Avoid Sleep on UI thread; we simply proceed to clear state and update the UI
    // (previous Sleep was to allow pending WM_SIZE/timers to settle; this is
    // unnecessary when shutdown is complete and avoids freezing the UI.)
    
    // Clear disconnecting flag BEFORE status update to prevent race conditions
    disconnecting.store(false);
    
    if (IsWindow(hwnd)) {
        PrependStatus(L"已斷開連接\r\n");
    }
    
    // NOTE: StopErrorCodeCheck() and StopAutoProcedure() are now called in OnDisconnect()
    // BEFORE MasterShutdown() to prevent crash when threads access destroyed EtherCATMaster
    // No need to call them again here.

    try {
        if (auto* master = AppController::GetInstance().GetMaster()) {
            master->ClearGlobalError();
        }
    } catch (...) {}

    this->anySlaveHasInternalError.store(false);
    SetManualResetRequired(false);
    NotifyEthercatError(false);

    this->currentIfName.clear();

    ApplyDisconnectedUiControlState();

    StopDisconnectAlertSounds();

    ResetDisconnectedRuntimeState();
}

void MainWindow::ApplyDisconnectedUiControlState() {
    // Safely update UI controls with window validity checks.
    if (!IsWindow(hwnd)) {
        return;
    }

    if (hBtnConnect && IsWindow(hBtnConnect)) SetControlEnabled(hBtnConnect, true);
    if (hBtnDisconnect && IsWindow(hBtnDisconnect)) SetControlEnabled(hBtnDisconnect, false);
    if (hBtnReadSDO && IsWindow(hBtnReadSDO)) SetControlEnabled(hBtnReadSDO, false);
    if (hBtnWriteSDO && IsWindow(hBtnWriteSDO)) SetControlEnabled(hBtnWriteSDO, false);
    if (hComboAdapter && IsWindow(hComboAdapter)) SetControlEnabled(hComboAdapter, true);
    SetUiDisabled("adapter_select", false);
    if (hBtnReadAllSDO && IsWindow(hBtnReadAllSDO)) SetControlEnabled(hBtnReadAllSDO, false);
    if (hBtnSaveParams && IsWindow(hBtnSaveParams)) SetControlEnabled(hBtnSaveParams, true);
    if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) SetControlEnabled(hBtnSendMailbox, false);
    if (hBtnWriteAllSDO && IsWindow(hBtnWriteAllSDO)) SetControlEnabled(hBtnWriteAllSDO, false);
    if (hBtnFollowMode && IsWindow(hBtnFollowMode)) SetControlEnabled(hBtnFollowMode, false);
    if (hBtnPowerOn && IsWindow(hBtnPowerOn)) SetControlEnabled(hBtnPowerOn, false);
    if (hBtnPowerOff && IsWindow(hBtnPowerOff)) SetControlEnabled(hBtnPowerOff, false);
    if (hBtnHome && IsWindow(hBtnHome)) SetControlEnabled(hBtnHome, false);
    if (hBtnJogCW && IsWindow(hBtnJogCW)) SetControlEnabled(hBtnJogCW, false);
    if (hBtnJogCCW && IsWindow(hBtnJogCCW)) SetControlEnabled(hBtnJogCCW, false);
    if (hBtnFaultReset && IsWindow(hBtnFaultReset)) {
        if (!IsMasterInitialized() || disconnecting.load()) {
            SetControlEnabled(hBtnFaultReset, false);
        }
    }
    if (hBtnImmediateStop && IsWindow(hBtnImmediateStop)) SetControlEnabled(hBtnImmediateStop, false);
    if (hBtnStopAutoMode && IsWindow(hBtnStopAutoMode)) SetControlEnabled(hBtnStopAutoMode, false);
    if (hButtonStartAutoMode && IsWindow(hButtonStartAutoMode)) SetControlEnabled(hButtonStartAutoMode, false);

    // Mirror to server ui_state so web clients see disabled buttons after disconnect.
    SetUiDisabled("start_auto", true);
    SetUiDisabled("stop_auto", true);

    for (auto &it : pdoDisplay.pdoItems) {
        if (it.hButton && IsWindow(it.hButton)) SetControlEnabled(it.hButton, false);
        if (it.hEdit && IsWindow(it.hEdit)) SetControlEnabled(it.hEdit, false);
        it.currentValue.clear();
        it.displayValue.clear();
        if (it.hValue && IsWindow(it.hValue)) {
            SetWindowTextW(it.hValue, L"");
            InvalidateRect(it.hValue, NULL, TRUE);
        }
    }

    if (hEditCycleTime && IsWindow(hEditCycleTime)) {
        SetControlEnabled(hEditCycleTime, true);
        SetUiDisabled("set_cycle", false);
    }

    SetUiDisabled("connect", false);
    SetUiDisabled("disconnect", true);
    SetUiDisabled("send_mailbox", true);
    SetUiDisabled("dump_esi", true);

    if (hListSlaves && IsWindow(hListSlaves)) {
        ListView_DeleteAllItems(hListSlaves);
    }

    if (hComboSlaves && IsWindow(hComboSlaves)) {
        SendMessageW(hComboSlaves, CB_RESETCONTENT, 0, 0);
        for (int si = 1; si <= 16; ++si) {
            wchar_t txt[8]; swprintf_s(txt, L"%d", si);
            LRESULT idx = SendMessageW(hComboSlaves, CB_ADDSTRING, 0, (LPARAM)txt);
            if (idx != CB_ERR && idx != CB_ERRSPACE) {
                SendMessageW(hComboSlaves, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)si);
            }
        }
        SendMessageW(hComboSlaves, CB_SETCURSEL, 0, 0);
    }

    if (hBtnLoadConfig && IsWindow(hBtnLoadConfig)) SetControlEnabled(hBtnLoadConfig, true);
}

void MainWindow::StopDisconnectAlertSounds() {
    // Clear component flags first so the shared alert sound can actually stop.
    try {
        this->softStartSoundPlaying_.store(false);
        this->violationSoundPlaying_.store(false);
        this->errorSoundPlaying_.store(false);
        StopSystemAlertSoundIfIdle();
        PostUTF8LogToUI(std::string("[DISCONNECT] Alert sounds stopped\n"));
    } catch(...) {}
}

void MainWindow::MarkConnectionWorkComplete() {
    SetBusyFlagAndNotify(workerRunning, false);
    this->connectAttemptInProgress.store(false);
}

std::string MainWindow::NormalizeInterfaceName(const std::string &ifname) const {
    std::string normalized;
    normalized.reserve(ifname.size());
    for (size_t i = 0; i < ifname.size(); ++i) {
        const char ch = ifname[i];
        if (ch == '\\') {
            normalized.push_back('\\');
            while (i + 1 < ifname.size() && ifname[i + 1] == '\\') {
                ++i;
            }
            continue;
        }
        normalized.push_back(ch);
    }
    return normalized;
}

bool MainWindow::ResolveConnectInterfaceName(const std::string* overrideIfName, std::string &ifname) {
    ifname = (overrideIfName && !overrideIfName->empty()) ? *overrideIfName : this->currentIfName;
    if (!ifname.empty()) {
        return true;
    }

    if (hComboAdapter && IsWindow(hComboAdapter)) {
        int sel = (int)SendMessageW(hComboAdapter, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) {
            TryGetAdapterPcapNameFromComboSelection(sel, ifname);
            if (ifname.empty()) {
                wchar_t buf[512] = {0};
                GetWindowTextW(hComboAdapter, buf, (int)_countof(buf));
                std::string visible = CoreHelpers::WideToUtf8(std::wstring(buf));
                if (!visible.empty()) {
                    ifname = EtherCATMaster::IsSimulationAdapterSelection(visible)
                        ? EtherCATMaster::GetSimulationInterfaceName()
                        : visible;
                }
            }
            if (!ifname.empty() && !overrideIfName) {
                this->currentIfName = ifname;
            }
        }
    }

    if (!ifname.empty()) {
        return true;
    }

    try {
        std::string enumerateError;
        std::vector<AdapterEntry> fresh = CoreHelpers::EnumerateAdapters(enumerateError);
        if (enumerateError.empty()) {
            for (const auto &adapter : fresh) {
                bool linkUp = false;
                try {
                    linkUp = CoreHelpers::CheckLinkStatus(adapter.pcapName.c_str());
                } catch (...) {
                    linkUp = false;
                }
                if (!linkUp) {
                    continue;
                }
                ifname = adapter.pcapName;
                try {
                    MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] auto-selected adapter: ") + ifname + "\n");
                } catch (...) {
                }
                break;
            }
        }
    } catch (...) {
    }

    if (!ifname.empty()) {
        return true;
    }

    PrependStatus(L"請先在前端介面選擇一個網路介面\r\n");
    try {
        MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[Connect] rejected: missing adapter (ifname empty)\n"));
    } catch (...) {
    }
    return false;
}

bool MainWindow::TryResolveAdapterPcapName(std::string &ifname, std::string *matchKind) const {
    if (matchKind) {
        matchKind->clear();
    }
    if (ifname.empty()) {
        return false;
    }

    if (EtherCATMaster::IsSimulationAdapterSelection(ifname)) {
        ifname = EtherCATMaster::GetSimulationInterfaceName();
        if (matchKind) {
            *matchKind = "simulation";
        }
        return true;
    }

    try {
        std::string enumerateError;
        std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(enumerateError);
        if (!enumerateError.empty() || adapters.empty()) {
            return false;
        }

        for (const auto &adapter : adapters) {
            if (adapter.pcapName == ifname) {
                if (matchKind) {
                    *matchKind = "pcap";
                }
                return true;
            }
        }

        std::string normalizedAdapterName = ifname;
        if (normalizedAdapterName.size() > 1 &&
            normalizedAdapterName.front() == '{' &&
            normalizedAdapterName.back() == '}') {
            normalizedAdapterName = normalizedAdapterName.substr(1, normalizedAdapterName.size() - 2);
        }

        for (const auto &adapter : adapters) {
            if (!adapter.adapterName.empty() &&
                (adapter.adapterName == ifname || adapter.adapterName == normalizedAdapterName)) {
                ifname = adapter.pcapName;
                if (matchKind) {
                    *matchKind = "adapter";
                }
                return true;
            }
        }

        auto toLower = [](const std::string &value) {
            std::string lowered = value;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
            return lowered;
        };

        const std::string needle = toLower(ifname);
        for (const auto &adapter : adapters) {
            const std::string display = toLower(adapter.description.empty() ? adapter.friendlyName : adapter.description);
            if (!display.empty() && needle.find(display) != std::string::npos) {
                ifname = adapter.pcapName;
                if (matchKind) {
                    *matchKind = "display";
                }
                return true;
            }
        }
    } catch (...) {
    }

    return false;
}

std::string MainWindow::FindAdapterDisplayName(const std::string &ifname) const {
    if (ifname.empty()) {
        return std::string();
    }

    if (EtherCATMaster::IsSimulationAdapterSelection(ifname)) {
        return EtherCATMaster::GetSimulationAdapterDisplayName();
    }

    std::string resolvedIfname = ifname;
    TryResolveAdapterPcapName(resolvedIfname);

    try {
        std::string enumerateError;
        std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(enumerateError);
        if (!enumerateError.empty()) {
            return std::string();
        }

        for (const auto &adapter : adapters) {
            if (adapter.pcapName == resolvedIfname) {
                std::string display = CoreHelpers::MakeAdapterDisplayString(adapter);
                if (!adapter.adapterName.empty()) {
                    std::string shortId = adapter.adapterName.size() > 8 ? adapter.adapterName.substr(adapter.adapterName.size() - 8) : adapter.adapterName;
                    display += " [" + shortId + "]";
                }
                return display;
            }
        }
    } catch (...) {
    }

    return std::string();
}

bool MainWindow::LoadDefaultMotionConfigForConnect() {
    wchar_t szExePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, szExePath, MAX_PATH);
    wchar_t* pLast = wcsrchr(szExePath, L'\\');
    if (pLast) {
        *pLast = L'\0';
    }

    std::wstring parDir = std::wstring(szExePath) + L"\\Par";
    CreateDirectoryW(parDir.c_str(), NULL);
    std::wstring targetFile = parDir + L"\\motion.xml";

    std::string loadedUtf8;
    std::map<int, std::vector<std::vector<std::wstring>>> tmpCache;
    MotionConfigIO::MotionCacheLoadDiagnostics loadDiagnostics;
    if (MotionConfigIO::LoadMotionCacheFromFileAtPathWithDefaults(tmpCache, targetFile, loadedUtf8, &loadDiagnostics)) {
        savedMotionParamsForSlave = std::move(tmpCache);
        savedMotionConfigGlobal = targetFile;
        SyncMotionParamsToUi((confirmedSlave > 0) ? confirmedSlave : 1);

        if (loadDiagnostics.usedDefaultTemplate || loadDiagnostics.repairedInvalidValues) {
            std::wstring status = L"motion.xml 已自動修復並套用預設值: ";
            status += targetFile;
            status += L"\r\n";
            PrependStatus(status.c_str());
        }

        try {
            size_t slaveCount = savedMotionParamsForSlave.size();
            size_t totalRows = 0;
            for (const auto &entry : savedMotionParamsForSlave) {
                totalRows += entry.second.size();
            }
            std::string cacheLog = std::string("MOTION_CACHE_LOADED: ") + loadedUtf8 +
                std::string(" slaves=") + std::to_string(slaveCount) +
                std::string(" rows=") + std::to_string(totalRows) + "\n";
            PostUTF8LogToUI(cacheLog);
            if (loadDiagnostics.usedDefaultTemplate || loadDiagnostics.repairedInvalidValues) {
                std::string repairLog = std::string("MOTION_CACHE_RECOVERED: ") + loadedUtf8 +
                    std::string(" used_defaults=") + (loadDiagnostics.usedDefaultTemplate ? "1" : "0") +
                    std::string(" repaired=") + (loadDiagnostics.repairedInvalidValues ? "1" : "0") +
                    std::string(" rewrote=") + (loadDiagnostics.rewroteTargetFile ? "1" : "0") +
                    std::string(" issues=") + std::to_string(loadDiagnostics.messages.size()) + "\n";
                PostUTF8LogToUI(repairLog);
            }
        } catch (...) {
        }
        return true;
    }

    if (MotionConfigIO::LoadMotionParamsFromFileAtPath(hListMotionParams, targetFile, loadedUtf8)) {
        savedMotionConfigGlobal = targetFile;
        try {
            const int targetSlave = (confirmedSlave > 0 ? confirmedSlave : 1);
            const int rows = (hListMotionParams && IsWindow(hListMotionParams)) ? ListView_GetItemCount(hListMotionParams) : 0;
            std::string paramsLog = std::string("MOTION_PARAMS_LOADED: ") + loadedUtf8 +
                std::string(" slave=") + std::to_string(targetSlave) +
                std::string(" rows=") + std::to_string(rows) + "\n";
            PostUTF8LogToUI(paramsLog);
        } catch (...) {
        }
        CacheMotionParamsFromUi((confirmedSlave > 0) ? confirmedSlave : 1);
        return true;
    }

    std::wstring msg = L"設定檔讀取失敗: ";
    msg += targetFile;
    msg += L"\r\n";
    PrependStatus(msg.c_str());
    try {
        std::string utf = CoreHelpers::WideToUtf8(targetFile);
        PostUTF8LogToUI(std::string("MOTION_LOAD_FAILED: ") + utf + "\n");
    } catch (...) {
    }
    return false;
}

bool MainWindow::InitializeMasterWithFallback(
    std::string &ifname,
    uint32_t desiredCycleTime,
    const std::function<void(const std::string&)> &post) {
    post(std::string("正在初始化 EtherCAT...\n"));
    ifname = NormalizeInterfaceName(ifname);

    Sleep(50);
    MasterSetCycleTime(desiredCycleTime);
    if (IsMasterInitialized()) {
        post(std::string("Previous master still initialized, forcing shutdown before re-init...\n"));
        MasterShutdownSync();
    }

    if (MasterInitialize(ifname)) {
        return true;
    }

    post(std::string("Initialization failed: ") + MasterGetLastError() + "\n");

    std::vector<std::string> attempts;
    attempts.push_back(ifname);
    if (ifname.rfind("rpcap://", 0) == 0) {
        attempts.push_back(ifname.substr(strlen("rpcap://")));
    } else {
        attempts.push_back(std::string("rpcap://") + ifname);
    }
    std::string stripped = ifname;
    size_t pos = stripped.find("\\\\Device\\");
    if (pos != std::string::npos) {
        std::string devicePath = stripped.substr(pos);
        attempts.push_back(devicePath);
        attempts.push_back(std::string("rpcap://") + devicePath);
    } else {
        pos = stripped.find("\\Device\\");
        if (pos != std::string::npos) {
            std::string devicePath = stripped.substr(pos);
            attempts.push_back(devicePath);
            attempts.push_back(std::string("rpcap://") + devicePath);
        }
    }

    bool initialized = false;
    for (const auto &tryName : attempts) {
        if (tryName == ifname) {
            continue;
        }
        post(std::string("Initialization retry with: ") + tryName + "\n");
        Sleep(100);
        if (MasterInitialize(tryName)) {
            post(std::string("Initialization retry succeeded with: ") + tryName + "\n");
            ifname = tryName;
            this->currentIfName = tryName;
            this->webSelectedIfName = tryName;
            initialized = true;
            break;
        }
        post(std::string("Initialization retry failed for: ") + tryName + "\n");
    }

    if (!initialized) {
        std::string rematchedIfname = ifname;
        std::string matchKind;
        if (TryResolveAdapterPcapName(rematchedIfname, &matchKind) && rematchedIfname != ifname) {
            const std::string matchLabel = (matchKind == "display") ? "display" : "adapter";
            post(std::string("Retry Initialize with re-matched ") + matchLabel + ": " + rematchedIfname + "\n");
            if (MasterInitialize(rematchedIfname)) {
                post(std::string("Initialization retry succeeded with: ") + rematchedIfname + "\n");
                ifname = rematchedIfname;
                this->currentIfName = rematchedIfname;
                this->webSelectedIfName = rematchedIfname;
                initialized = true;
            }
        }
    }

    if (!initialized) {
        auto shouldAutoRetryInitError = [](const std::string &error) {
            if (error.empty()) {
                return false;
            }
            if (error.find("Timeout") != std::string::npos) {
                return true;
            }
            if (error.find("Permission") != std::string::npos) {
                return true;
            }
            return false;
        };

        const std::string lastInitError = MasterGetLastError();
        if (shouldAutoRetryInitError(lastInitError) && !disconnecting.load()) {
            const int retryCount = 12;
            const int retryDelayMs = 5000;
            post(std::string("Initialization reported 'No slaves found'. Will attempt auto-retry ") +
                 std::to_string(retryCount) + std::string(" times every ") +
                 std::to_string(retryDelayMs / 1000) + std::string("s before failing.\n"));
            for (int retry = 0; retry < retryCount && !disconnecting.load(); ++retry) {
                try {
                    std::string enumerateError;
                    std::vector<AdapterEntry> freshAdapters = CoreHelpers::EnumerateAdapters(enumerateError);
                    if (enumerateError.empty() && !freshAdapters.empty()) {
                        for (const auto &adapter : freshAdapters) {
                            std::vector<std::string> tryList;
                            tryList.push_back(adapter.pcapName);
                            tryList.push_back(std::string("rpcap://") + adapter.pcapName);
                            if (!adapter.adapterName.empty()) {
                                tryList.push_back(adapter.adapterName);
                            }
                            for (const auto &tryName : tryList) {
                                if (tryName.empty()) {
                                    continue;
                                }
                                if (MasterInitialize(tryName)) {
                                    ifname = tryName;
                                    this->currentIfName = tryName;
                                    this->webSelectedIfName = tryName;
                                    initialized = true;
                                    break;
                                }
                            }
                            if (initialized) {
                                break;
                            }
                        }
                    }
                } catch (...) {
                }
                if (initialized) {
                    break;
                }

                int waitedMs = 0;
                while (waitedMs < retryDelayMs && !disconnecting.load()) {
                    if (CoreHelpers::CheckLinkStatus(ifname.c_str())) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    waitedMs += 100;
                }
            }
        }
    }

    if (initialized) {
        return true;
    }

    RequestAutomaticDisconnect("Connect: initialization failed - issuing disconnect\n");
    this->currentIfName.clear();
    PostAutoStatusUpdate();
    post(std::string("請檢查網路介面或權限\n"));
    NotifyEthercatError(true);
    SetControlEnabled(hBtnConnect, true);
    if (hBtnLoadConfig) {
        SetControlEnabled(hBtnLoadConfig, true);
    }
    SetUiDisabled("connect", false);
    SetUiDisabled("disconnect", true);
    if (hComboAdapter && IsWindow(hComboAdapter)) {
        SetControlEnabled(hComboAdapter, true);
    }
    MarkConnectionWorkComplete();
    return false;
}

bool MainWindow::WriteSafeStartupControlword(const std::function<void(const std::string&)> &post) {
    post(std::string("[Connect] Writing safe controlword 0x6040=0x0006 via SDO...\n"));
    try {
        const int totalSlaves = MasterGetSlaveCount();
        std::vector<int> successSlaves;
        std::vector<int> failedSlaves;

        for (int slave = 1; slave <= totalSlaves; ++slave) {
            uint8_t controlword[2] = {0x06, 0x00};
            if (MasterWriteSDO(slave, 0x6040, 0x00, controlword, 2)) {
                successSlaves.push_back(slave);
                continue;
            }

            post(std::string("[Connect] FAILED SDO write 0x6040=0x0006 for slave=") +
                 std::to_string(slave) + " err=" + MasterGetLastError() + "\n");
            failedSlaves.push_back(slave);
        }

        if (!failedSlaves.empty()) {
            std::ostringstream errorSummary;
            errorSummary << "[Connect] CRITICAL: failed to write 0x6040=0x0006 for slaves: ";
            for (size_t index = 0; index < failedSlaves.size(); ++index) {
                if (index) {
                    errorSummary << ",";
                }
                errorSummary << failedSlaves[index];
            }
            errorSummary << " - ABORTING CONNECT\n";
            post(errorSummary.str());
            return false;
        }

        if (successSlaves.empty()) {
            post(std::string("[Connect] CRITICAL: no slaves available for SDO write - ABORTING CONNECT\n"));
            return false;
        }

        post(std::string("[Connect] ALL SLAVES wrote 0x6040=0x0006 via SDO SUCCESS\n"));
        return true;
    } catch (...) {
        post(std::string("[Connect] exception while writing 0x6040=0x0006 via SDO - ABORTING CONNECT\n"));
        return false;
    }
}

void MainWindow::WriteStartupMotionParams(const std::function<void(const std::string&)> &post) {
    try {
        const int totalSlaves = MasterGetSlaveCount();
        int skippedNoParams = 0;
        int skippedMissingP = 0;
        int wroteCount = 0;
        std::vector<int> failed60e0;
        std::vector<int> failed60e1;

        for (int slave = 1; slave <= totalSlaves; ++slave) {
            auto saved = savedMotionParamsForSlave.find(slave);
            if (saved == savedMotionParamsForSlave.end()) {
                ++skippedNoParams;
                continue;
            }

            MotionParamSnapshot motionParams = CaptureMotionParams(slave);

            if (!motionParams.haveP11 || !motionParams.haveP12) {
                ++skippedMissingP;
                continue;
            }

            uint32_t value60e0 = static_cast<uint32_t>(llround(motionParams.p11));
            uint32_t value60e1 = static_cast<uint32_t>(llround(motionParams.p12));
            if (motionParams.haveP2 && motionParams.p2 == 1) {
                value60e0 = static_cast<uint32_t>(llround(motionParams.p12));
                value60e1 = static_cast<uint32_t>(llround(motionParams.p11));
            }

            uint8_t bytes60e0[4] = {
                static_cast<uint8_t>(value60e0 & 0xFF),
                static_cast<uint8_t>((value60e0 >> 8) & 0xFF),
                static_cast<uint8_t>((value60e0 >> 16) & 0xFF),
                static_cast<uint8_t>((value60e0 >> 24) & 0xFF)
            };
            uint8_t bytes60e1[4] = {
                static_cast<uint8_t>(value60e1 & 0xFF),
                static_cast<uint8_t>((value60e1 >> 8) & 0xFF),
                static_cast<uint8_t>((value60e1 >> 16) & 0xFF),
                static_cast<uint8_t>((value60e1 >> 24) & 0xFF)
            };

            const bool ok60e0 = MasterWriteSDO(slave, 0x60E0, 0x00, bytes60e0, 4);
            if (!ok60e0) {
                failed60e0.push_back(slave);
            }

            const bool ok60e1 = MasterWriteSDO(slave, 0x60E1, 0x00, bytes60e1, 4);
            if (!ok60e1) {
                failed60e1.push_back(slave);
            }

            if (ok60e0 && ok60e1) {
                ++wroteCount;
            }
        }

        std::ostringstream summary;
        summary << "[Connect] 0x60E0/0x60E1 writes: wrote=" << wroteCount;
        if (skippedNoParams) {
            summary << " skipped_no_params=" << skippedNoParams;
        }
        if (skippedMissingP) {
            summary << " skipped_missing_P11_P12=" << skippedMissingP;
        }
        if (!failed60e0.empty() || !failed60e1.empty()) {
            summary << " failures:";
            if (!failed60e0.empty()) {
                summary << " e0[";
                for (size_t index = 0; index < failed60e0.size(); ++index) {
                    if (index) {
                        summary << ",";
                    }
                    summary << failed60e0[index];
                }
                summary << "]";
            }
            if (!failed60e1.empty()) {
                summary << " e1[";
                for (size_t index = 0; index < failed60e1.size(); ++index) {
                    if (index) {
                        summary << ",";
                    }
                    summary << failed60e1[index];
                }
                summary << "]";
            }
            summary << "\n";
        } else {
            summary << " OK\n";
        }
        post(summary.str());
    } catch (...) {
        post(std::string("[Connect] exception while writing 0x60E0/0x60E1 for slaves\n"));
    }
}

void MainWindow::SyncMotionParamsToUi(int slaveIndex) {
    auto saved = savedMotionParamsForSlave.find(slaveIndex);
    if (saved == savedMotionParamsForSlave.end()) {
        return;
    }
    if (!hListMotionParams || !IsWindow(hListMotionParams)) {
        return;
    }

    ListView_DeleteAllItems(hListMotionParams);
    for (size_t rowIndex = 0; rowIndex < saved->second.size(); ++rowIndex) {
        LVITEM item;
        ZeroMemory(&item, sizeof(item));
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(rowIndex);
        item.iSubItem = 0;
        std::wstring firstColumn = (saved->second[rowIndex].size() > 0) ? saved->second[rowIndex][0] : L"";
        item.pszText = const_cast<LPWSTR>(firstColumn.c_str());
        ListView_InsertItem(hListMotionParams, &item);
        for (size_t columnIndex = 1; columnIndex < saved->second[rowIndex].size() && columnIndex < 6; ++columnIndex) {
            ListView_SetItemText(
                hListMotionParams,
                static_cast<int>(rowIndex),
                static_cast<int>(columnIndex),
                const_cast<LPWSTR>(saved->second[rowIndex][columnIndex].c_str()));
        }
    }
}

void MainWindow::CacheMotionParamsFromUi(int slaveIndex) {
    if (!hListMotionParams || !IsWindow(hListMotionParams)) {
        return;
    }

    const int rows = ListView_GetItemCount(hListMotionParams);
    std::vector<std::vector<std::wstring>> matrix;
    matrix.reserve(rows);
    for (int rowIndex = 0; rowIndex < rows; ++rowIndex) {
        std::vector<std::wstring> columns(6);
        for (int columnIndex = 0; columnIndex < 6; ++columnIndex) {
            wchar_t buffer[512] = {0};
            ListView_GetItemText(hListMotionParams, rowIndex, columnIndex, buffer, static_cast<int>(_countof(buffer)));
            columns[columnIndex] = buffer;
        }
        matrix.push_back(std::move(columns));
    }
    savedMotionParamsForSlave[slaveIndex] = std::move(matrix);
}

void MainWindow::OptimizeAdapterForEthercat(
    const std::string &ifname,
    const std::function<void(const std::string&)> &post) {
    if (EtherCATMaster::IsSimulationAdapterSelection(ifname)) {
        post(std::string("[Simulation] skipping NIC optimization and adapter isolation\n"));
        return;
    }

    post(std::string("正在優化網卡中斷親和性設定...\n"));
    std::string adapterDisplay;
    try {
        adapterDisplay = FindAdapterDisplayName(ifname);
    } catch (...) {
        adapterDisplay.clear();
    }

    try {
        if (!adapterDisplay.empty()) {
            std::string optimLog;
            CoreHelpers::OptimizeNicIrqAffinity(adapterDisplay, optimLog);
            if (!optimLog.empty()) {
                post(optimLog);
            }
        } else {
            post(std::string("[NIC Optimization] 未找到匹配的網卡資訊，跳過 IRQ 優化\n"));
        }
    } catch (const std::exception &ex) {
        post(std::string("網卡優化時發生異常: ") + ex.what() + "\n");
    } catch (...) {
        post(std::string("網卡優化時發生未知異常\n"));
    }

    try {
        std::string wirelessLog;
        CoreHelpers::RebalanceWirelessIrqAffinityAwayFromRtCore(wirelessLog);
        if (!wirelessLog.empty()) {
            post(wirelessLog);
        }
    } catch (const std::exception &ex) {
        post(std::string("Wireless IRQ 重平衡時發生異常: ") + ex.what() + "\n");
    } catch (...) {
        post(std::string("Wireless IRQ 重平衡時發生未知異常\n"));
    }

    try {
        if (!adapterDisplay.empty()) {
            post(std::string("[Adapter Isolation] 正在隔離網卡以防止其他軟體干擾...\n"));
            std::string isolateLog;
            bool isolated = CoreHelpers::IsolateEtherCATAdapter(adapterDisplay, false, isolateLog);
            if (!isolateLog.empty()) {
                post(isolateLog);
            }
            if (isolated) {
                post(std::string("[Adapter Isolation] ✓ 網卡隔離完成，其他軟體無法干擾 EtherCAT 通訊\n"));
            } else {
                post(std::string("[Adapter Isolation] ⚠ 網卡隔離未完成（可能需要管理員權限）\n"));
            }
        } else {
            post(std::string("[Adapter Isolation] 未找到匹配的網卡資訊，跳過隔離\n"));
        }
    } catch (const std::exception &ex) {
        post(std::string("[Adapter Isolation] 異常: ") + ex.what() + "\n");
    } catch (...) {
        post(std::string("[Adapter Isolation] 未知異常\n"));
    }
}

void MainWindow::VerifyCyclicModeAndPdoReadiness(
    uint32_t desiredCycleTime,
    const std::function<void(const std::string&)> &post) {
    constexpr int kModeVerifyTimeoutUs = 10000;
    constexpr int kModeVerifyRetries = 2;
    constexpr DWORD kModeVerifyRetryDelayMs = 5;
    bool modeReady = true;
    int verifySlaveCount = MasterGetSlaveCount();
    for (int slaveIndex = 1; slaveIndex <= verifySlaveCount; ++slaveIndex) {
        bool modeOk = false;
        int32_t modeDisplay = 0;
        for (int retry = 0; retry < kModeVerifyRetries && !modeOk; ++retry) {
            int modeSize = (int)sizeof(modeDisplay);
            modeDisplay = 0;
            if (MasterReadSDO((uint16_t)slaveIndex, 0x6061, 0x00, &modeDisplay, modeSize, kModeVerifyTimeoutUs)) {
                if ((int8_t)(modeDisplay & 0xFF) == CoreHelpers::kDefaultCyclicOperationMode) {
                    modeOk = true;
                    break;
                }
            }
            if (retry + 1 < kModeVerifyRetries) {
                Sleep(kModeVerifyRetryDelayMs);
            }
        }
        if (!modeOk) {
            modeReady = false;
            std::ostringstream css;
            css << "[Connect] "
                << CoreHelpers::DescribeOperationModeShortName(CoreHelpers::kDefaultCyclicOperationMode)
                << " verify failed: slave=" << slaveIndex
                << " modeDisplay=" << (int)((int8_t)(modeDisplay & 0xFF)) << "\n";
            post(css.str());
        }
    }

    bool pdoReady = false;
    DWORD pdoCheckSleepMs = (DWORD)(desiredCycleTime / 1000);
    if (pdoCheckSleepMs == 0) {
        pdoCheckSleepMs = 1;
    }
    for (int retry = 0; retry < 20; ++retry) {
        uint32_t wkcNow = MasterGetWorkingCounter();
        uint64_t pdoSentNow = MasterGetPDOSentCount();
        if (wkcNow > 0 && pdoSentNow > 0) {
            pdoReady = true;
            break;
        }
        Sleep(pdoCheckSleepMs);
    }

    this->cyclicPdoCycleDetectReady.store(pdoReady);
    if (pdoReady) {
        if (!modeReady) {
            post(std::string("[Connect] NOTE: cyclic mode verify incomplete, but PDO is active -> enable actual cycle detection\n"));
        } else {
            post(std::string("[Connect] cyclic mode verified and PDO active -> enable actual cycle detection\n"));
        }
    } else {
        post(std::string("[Connect] WARNING: PDO not active yet, actual cycle detection remains gated\n"));
    }
}

void MainWindow::FinalizeSuccessfulConnection(
    const std::function<void()> &releaseServerConnectGuard,
    const std::function<void(const std::string&)> &post) {
    if (EtherCATMaster::IsSimulationAdapterSelection(this->currentIfName)) {
        currentPdos.clear();
        currentObjects.clear();
    }

    EnsureSimulationMmf1InputRunningAfterConnect(this->currentIfName, post);

    PostUTF8LogToUI(std::string("UPDATE_SLAVE_LIST\n"));
    PostUTF8LogToUI(std::string("ENABLE_CONTROLS\n"));
    PostUTF8LogToUI(std::string("START_TIMER\n"));
    SetControlEnabled(hEditCycleTime, false);
    SetUiDisabled("set_cycle", true);
    if (hBtnFaultReset && IsWindow(hBtnFaultReset)) {
        SetControlEnabled(hBtnFaultReset, true);
    }
    SetUiDisabled("connect", true);
    SetUiDisabled("disconnect", false);
    SetUiDisabled("start_auto", false);
    SetUiDisabled("send_mailbox", false);
    SetUiDisabled("dump_esi", false);

    // ---- Auto-populate P0 Device Name from actual slave info ----
    {
        auto* master = AppController::GetInstance().GetMaster();
        if (master && master->IsInitialized()) {
            int slaveCount = master->GetSlaveCount();
            for (int idx = 1; idx <= slaveCount; ++idx) {
                // P0 Value = SDO 0x1008 真實設備名稱 (e.g. DS_P100E-7), fallback 未知
                std::wstring deviceNameW = L"\u672a\u77e5";   // 未知
                {
                    char deviceName[128] = {0};
                    int nameSize = sizeof(deviceName);
                    if (master->ReadSDO(idx, 0x1008, 0x00, deviceName, nameSize)) {
                        std::string s(deviceName);
                        if (!s.empty()) {
                            deviceNameW = CoreHelpers::Utf8ToWide(s);
                        }
                    }
                }
                // P0 Desc = "裝置名稱 ESI檔=" + 實際匹配的 ESI 檔名, fallback 未知
                std::wstring esiFileW = L"\u88dd\u7f6e\u540d\u7a31 ESI\u6a94=\u672a\u77e5";  // 裝置名稱 ESI檔=未知
                std::string esi = master->GetSlaveEsiFile(idx);
                if (!esi.empty()) {
                    esiFileW = L"\u88dd\u7f6e\u540d\u7a31 ESI\u6a94=" + CoreHelpers::Utf8ToWide(esi);  // 裝置名稱 ESI檔=xxx.xml
                }

                auto it = savedMotionParamsForSlave.find(idx);
                if (it != savedMotionParamsForSlave.end()) {
                    for (auto& row : it->second) {
                        if (!row.empty() && row[0] == L"P0") {
                            if (row.size() > 3) row[3] = deviceNameW;   // Value
                            if (row.size() > 5) row[5] = esiFileW;      // Desc
                            break;
                        }
                    }
                }
            }
            // Refresh motion-params UI for the currently selected slave
            int targetSlave = (confirmedSlave > 0) ? confirmedSlave : 1;
            SyncMotionParamsToUi(targetSlave);
        }
    }

    try {
        releaseServerConnectGuard();
    } catch (...) {}

    bool startupAutoStart = this->autoStartAfterConnect.exchange(false);
    bool shouldAutoStart = startupAutoStart;
    if (!shouldAutoStart && ReadIniBool(L"Startup", L"AutoTrigger", false)) {
        shouldAutoStart = true;
    }
    if (shouldAutoStart) {
        post(std::string("[AutoStartup] 連接成功，準備自動啟動程序...\n"));
        Sleep(50);
        const bool suppressPcAutoWindow = startupAutoStart && this->suppressAutoWindowForStartupAuto.exchange(false);
        PostCommandToUIOrCache(suppressPcAutoWindow
            ? std::string("HTTP_CMD:START_AUTO")
            : std::string("HTTP_CMD:START_AUTO_WITH_WINDOW"));
    } else {
        this->suppressAutoWindowForStartupAuto.store(false);
    }
}

void MainWindow::HandleConfigureSlavesFailure(
    const std::string &errorMessage,
    const std::function<void(const std::string&)> &post) {
    post(std::string("錯誤: ConfigureSlaves() 失敗!\n"));
    post(std::string("詳細資訊: ") + errorMessage + "\n");
    post(std::string("請斷開連接，重新上電所有從站，然後重新連接。\n"));
    post(std::string("ENABLE_CONTROLS\n"));
    SetControlEnabled(hEditCycleTime, true);
    SetUiDisabled("set_cycle", false);

    try {
        if (errorMessage.find("Runtime configuration failed") != std::string::npos) {
            bool autoDisconnect = false;
            try { autoDisconnect = MasterIsAutoDisconnectOnNetworkFailureEnabled(); } catch(...) {}
            if (autoDisconnect) {
                post(std::string("Connect: runtime configuration failed - performing automatic disconnect\n"));
                RequestAutomaticDisconnect("Connect: runtime configuration failed - issuing disconnect\n");
            }
        }
    } catch (...) {}

    MarkConnectionWorkComplete();
    PostAutoStatusUpdate();
}

void MainWindow::HandleStartCyclicOperationFailure(
    const std::string &errorMessage,
    const std::function<void(const std::string&)> &post) {
    this->cyclicPdoCycleDetectReady.store(false);
    post(std::string("StartCyclicOperation failed: ") + errorMessage + "\n");
    PostUTF8LogToUI(std::string("ENABLE_CONTROLS\n"));
    SetControlEnabled(hEditCycleTime, true);
    SetUiDisabled("set_cycle", false);
}

bool MainWindow::AbortConnectIfDisconnectRequested(
    const char *message,
    const std::function<void(const std::string&)> &post) {
    if (!disconnecting.load()) {
        return false;
    }
    post(message);
    MarkConnectionWorkComplete();
    return true;
}

void MainWindow::StartPostConnectSdoReader() {
    if (auto* master = AppController::GetInstance().GetMaster()) {
        sdo60FDFailureStreak.store(0);
        sdo60FDConsecutiveFailureCount.store(0);
        sdo60FDFailureSinceTick.store(0);
        lastSdo60FDFailureTick.store(0);
        sdo60FDDisconnectRequested.store(false);
        master->StartSdoReader(10, [this](const std::string &message) {
            this->HandleSdo60FDCallback(message);
        });
    }
}

void MainWindow::HandleSdo60FDCallback(const std::string &message) {
    const ULONGLONG nowTick = GetTickCount64();
    if (message.rfind("Read 0x60FD failed:", 0) == 0) {
        sdo60FDFailureStreak.fetch_add(1);
        const int failureCount = sdo60FDConsecutiveFailureCount.fetch_add(1) + 1;
        ULONGLONG failureSinceTick = sdo60FDFailureSinceTick.load();
        if (failureSinceTick == 0) {
            failureSinceTick = nowTick;
            sdo60FDFailureSinceTick.store(failureSinceTick);
            PostUTF8LogToUI(std::string("[AUTO] Read 0x60FD failed - monitoring sustained communication failure\n"));
            PostUTF8LogToUI(std::string("[AUTO] ") + message + "\n");
        } else if ((failureCount % 25) == 0) {
            const ULONGLONG elapsedMs = (nowTick >= failureSinceTick) ? (nowTick - failureSinceTick) : 0;
            PostUTF8LogToUI(std::string("[AUTO] Read 0x60FD still failing - count=") + std::to_string(failureCount)
                + std::string(" elapsed=") + std::to_string(elapsedMs) + std::string(" ms\n"));
        }
        lastSdo60FDFailureTick.store(nowTick);
        PostAutoStatusUpdate();

        if (!sdo60FDDisconnectRequested.load() && failureSinceTick != 0 && nowTick >= failureSinceTick
            && (nowTick - failureSinceTick) >= SDO_60FD_FAILURE_WINDOW_MS) {
            sdo60FDDisconnectRequested.store(true);
            RequestAutomaticDisconnect(
                "[AUTO] Read 0x60FD failed continuously for over 1000 ms - issuing disconnect\n",
                L"0x60FD 連續讀取失敗超過 1 秒，執行自動斷開...\r\n");
        }
        return;
    } else {
        sdo60FDFailureStreak.store(0);
        const int failureCount = sdo60FDConsecutiveFailureCount.exchange(0);
        const ULONGLONG failureSinceTick = sdo60FDFailureSinceTick.exchange(0);
        lastSdo60FDFailureTick.store(0);
        sdo60FDDisconnectRequested.store(false);
        if (failureCount > 0 && failureSinceTick != 0) {
            const ULONGLONG elapsedMs = (nowTick >= failureSinceTick) ? (nowTick - failureSinceTick) : 0;
            PostUTF8LogToUI(std::string("[AUTO] 0x60FD read recovered after ") + std::to_string(elapsedMs)
                + std::string(" ms (count=") + std::to_string(failureCount) + std::string(")\n"));
        }
    }

    this->PostUTF8LogToUI(std::string("[AUTO] ") + message + "\n");
    this->PostAutoStatusUpdate();
    try {
        std::vector<uint8_t> bytes;
        if (!this->TryGetLastSdo60FD(bytes) || bytes.empty()) {
            return;
        }

        uint8_t last = bytes.back();
        int prev = this->lastSdo60FDLastByte.load();
        const auto snapshot = GetSystemStateSnapshot();
        const auto autoConnectDecision = CoreHelpers::EvaluateAutoConnect(snapshot);
        const auto downToOriginDecision = CoreHelpers::EvaluateAutoDownToOrigin(snapshot);
        auto logRejectedExternalAutoAction = [this](const char* actionName,
                                const CoreHelpers::CommandPolicyDecision& decision) {
            PostUTF8LogToUI(std::string("[AUTO] ") + actionName +
                    " ignored: " +
                    CoreHelpers::DescribeCommandPolicyRejection(decision.rejection) +
                    "\n");
        };

        int newb6 = (last & 0x40) ? 1 : 0;
        int prevb6 = (prev & 0x40) ? 1 : 0;
        if (prev != -1 && newb6 == 1 && prevb6 == 0) {
            HWND hRequester = (this->hAutoWindow && IsWindow(this->hAutoWindow)) ? this->hAutoWindow : NULL;
            PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit6 asserted -> requesting Fault Reset via MainWindow auto flow\n"));
            AppController::GetInstance().RequestAutoFaultReset(reinterpret_cast<uintptr_t>(hRequester));
        }

        int newb1 = (last & 0x02) ? 1 : 0;
        int prevb1 = (prev & 0x02) ? 1 : 0;
        int newb0 = (last & 0x01) ? 1 : 0;
        int prevb0 = (prev & 0x01) ? 1 : 0;
        if (prev != -1 && newb0 == 1 && prevb0 == 0) {
            if (!this->immediateStopBusy.load()) {
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit0 asserted -> auto-triggering immediate stop\n"));
                PostMessageW(this->GetHwnd(), WM_USER_AUTO_IMMEDIATE_STOP_REQUEST, (WPARAM)0, 0);
            } else {
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit0 asserted but immediate stop already busy\n"));
            }
        }

        int newb5 = (last & 0x20) ? 1 : 0;
        int prevb5 = (prev & 0x20) ? 1 : 0;
        if (newb5 == 0 && !this->errorSoundPlaying_.load()) {
            if (!this->softStartSoundPlaying_.load() && !this->violationSoundPlaying_.load()) {
                try { PlaySound(TEXT("SystemAsterisk"), NULL, SND_ALIAS | SND_ASYNC | SND_LOOP); } catch(...) {}
            }
            this->errorSoundPlaying_.store(true);
            PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit5=0 (E-Stop pressed) -> emergency alert sound ON\n"));
        }

        if ((prev == -1 && newb5 == 0) || (prev != -1 && prevb5 == 1 && newb5 == 0)) {
            if (!this->immediateStopBusy.load()) {
                if (prev == -1) {
                    PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit5 active (0) at connection -> auto-triggering emergency stop\n"));
                } else {
                    PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit5 active (0) -> auto-triggering emergency stop\n"));
                }
                PostMessageW(this->GetHwnd(), WM_USER_AUTO_EMERGENCY_STOP_REQUEST, (WPARAM)0, 0);
            } else {
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit5 active but immediate/emergency stop already busy\n"));
            }
        }
        if (prev != -1 && prevb5 == 0 && newb5 == 1) {
            if (this->errorSoundPlaying_.load()) {
                this->errorSoundPlaying_.store(false);
                StopSystemAlertSoundIfIdle();
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit5 released -> stopping emergency alert sound\n"));
            }
        }

        int newb3 = (last & 0x08) ? 1 : 0;
        int prevb3 = (prev & 0x08) ? 1 : 0;
        if (prev != -1 && newb3 == 1 && prevb3 == 0) {
            if (downToOriginDecision.allowed) {
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit3 asserted -> requesting DownToOrigin via MainWindow auto flow\n"));
                AppController::GetInstance().RequestAutoDownToOrigin(0, true);
            } else {
                logRejectedExternalAutoAction("SDO 0x60FD bit3 DownToOrigin request", downToOriginDecision);
            }
        }

        if (prev != -1 && newb1 == 1 && prevb1 == 0) {
            if (autoConnectDecision.allowed) {
                PostUTF8LogToUI(std::string("[AUTO] SDO 0x60FD bit1 asserted -> requesting Connect/Run via MainWindow auto flow\n"));
                AppController::GetInstance().RequestAutoConnect();
            } else {
                logRejectedExternalAutoAction("SDO 0x60FD bit1 Connect/Run request", autoConnectDecision);
            }
        }

        this->lastSdo60FDLastByte.store((int)last);
    } catch (...) {}
}

// -----------------------------------------------------------------------------
// SDO single read/write and batch operations (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------





