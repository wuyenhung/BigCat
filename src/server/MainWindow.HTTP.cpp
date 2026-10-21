#include "MainWindow.h"
#include "../app/AppController.h"
#include "../include/core/CoreHelpers.h"
#include "HttpServer.h"
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstring>

static inline void SafeSetTextW(HWND hWnd, const std::wstring &text) {
    if (hWnd && IsWindow(hWnd)) SetWindowTextW(hWnd, text.c_str());
}

static inline void SetUiDisabledFromControlOrFallback(const char* key, HWND hWnd, bool fallbackDisabled) {
    if (hWnd && IsWindow(hWnd)) {
        SetUiDisabled(key, !IsWindowEnabled(hWnd));
    } else {
        SetUiDisabled(key, fallbackDisabled);
    }
}

static bool IsCommandLikeUiMessage(const std::string& text) {
    static const char* prefixes[] = {
        "ENABLE_CONTROLS",
        "ENABLE_CONTROLS_NOREFLOW",
        "ENABLE_HOME",
        "UPDATE_SLAVE_LIST",
        "SHUTDOWN_COMPLETE",
        "STOP_TIMER",
        "START_TIMER",
        "HTTP_LOCAL:",
        "HTTP_CMD:",
        "ADAPTERS_JSON:",
        "HTTP_LOCAL:STATUS:",
    };
    for (const char* prefix : prefixes) {
        const size_t len = strlen(prefix);
        if (text.size() >= len && text.rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
}

static void SyncAutoProcedureUiDisabledStates(const CoreHelpers::SystemStateSnapshot& snapshot,
                                              HWND hStartAuto,
                                              HWND hStopAuto,
                                              bool canStopAutoFallback) {
    const auto startAutoDecision = CoreHelpers::EvaluateStartAutoProcedure(snapshot);
    SetUiDisabledFromControlOrFallback("start_auto", hStartAuto, !startAutoDecision.allowed);
    SetUiDisabledFromControlOrFallback("stop_auto", hStopAuto, !canStopAutoFallback);
}

static void SyncAutoPageUiDisabledStates(const CoreHelpers::SystemStateSnapshot& snapshot,
                                         HWND hAutoWindow,
                                         bool actionGateEnabled,
                                         bool stopActive) {
    CoreHelpers::AutoActionButtonInputs inputs;
    inputs.action_gate_enabled = actionGateEnabled;
    inputs.stop_active = stopActive;
    const auto buttonState = CoreHelpers::EvaluateAutoActionButtons(snapshot, inputs);

    if (hAutoWindow && IsWindow(hAutoWindow)) {
        SetUiDisabledFromControlOrFallback(
            "auto_connect",
            GetDlgItem(hAutoWindow, IDC_BTN_AUTO_CONNECT),
            !buttonState.connect_enabled);
        SetUiDisabledFromControlOrFallback(
            "down_to_origin",
            GetDlgItem(hAutoWindow, IDC_BTN_AUTO_HOME),
            !buttonState.down_to_origin_enabled);
        SetUiDisabledFromControlOrFallback(
            "fault_reset",
            GetDlgItem(hAutoWindow, IDC_BTN_AUTO_FAULT_RESET),
            !buttonState.fault_reset_enabled);
        return;
    }

    SetUiDisabled("auto_connect", !buttonState.connect_enabled);
    SetUiDisabled("down_to_origin", !buttonState.down_to_origin_enabled);
    SetUiDisabled("fault_reset", !buttonState.fault_reset_enabled);
}

enum class HttpConnectAdapterSource {
    kUiSelection,
    kCachedWebSelection,
    kExplicitCommand,
};

struct HttpConnectCommandPlan {
    CoreHelpers::SystemStateSnapshot snapshot;
    std::string adapterOverride;
    HttpConnectAdapterSource adapterSource = HttpConnectAdapterSource::kUiSelection;
};

static const char* DescribeHttpConnectAdapterSource(HttpConnectAdapterSource adapterSource) {
    switch (adapterSource) {
    case HttpConnectAdapterSource::kCachedWebSelection:
        return "cached-web-selection";
    case HttpConnectAdapterSource::kExplicitCommand:
        return "explicit-command";
    case HttpConnectAdapterSource::kUiSelection:
    default:
        return "ui-selection";
    }
}

static bool BuildHttpConnectCommandPlan(HWND hwnd,
                                        const char* commandName,
                                        const std::string* explicitAdapterOverride,
                                        const std::string& cachedWebAdapterOverride,
                                        HttpConnectCommandPlan* planOut) {
    HttpConnectCommandPlan plan;
    plan.snapshot = AppController::GetInstance().GetStatusSnapshot();

    if (plan.snapshot.worker_running || plan.snapshot.master_initialized || plan.snapshot.busy) {
        try {
            MainWindow::PostUTF8LogToHWND(
                hwnd,
                std::string("HTTP_LOCAL:[HTTP->UI] ") + commandName +
                    " ignored: workerRunning=" + (plan.snapshot.worker_running ? "1" : "0") +
                    " masterInitialized=" + (plan.snapshot.master_initialized ? "1" : "0") +
                    " busy=" + (plan.snapshot.busy ? "1" : "0") + "\n");
        } catch (...) {
        }
        return false;
    }

    if (explicitAdapterOverride) {
        plan.adapterOverride = *explicitAdapterOverride;
        plan.adapterSource = HttpConnectAdapterSource::kExplicitCommand;
    } else if (!cachedWebAdapterOverride.empty()) {
        plan.adapterOverride = cachedWebAdapterOverride;
        plan.adapterSource = HttpConnectAdapterSource::kCachedWebSelection;
    }

    if (planOut) {
        *planOut = plan;
    }

    return true;
}

static void ExecuteHttpConnectCommand(MainWindow* window,
                                      HWND hwnd,
                                      const wchar_t* statusLine,
                                      const HttpConnectCommandPlan& plan) {
    if (!window) return;

    window->PrependStatus(statusLine);
    try {
        MainWindow::PostUTF8LogToHWND(
            hwnd,
            std::string("HTTP_LOCAL:[HTTP->UI] CONNECT preconditions: workerRunning=") +
                (plan.snapshot.worker_running ? "1" : "0") +
                " IsMasterInitialized=" + (plan.snapshot.master_initialized ? "1" : "0") +
                " busy=" + (plan.snapshot.busy ? "1" : "0") + "\n");
    } catch (...) {
    }

    try {
        MainWindow::PostUTF8LogToHWND(
            hwnd,
            std::string("HTTP_LOCAL:[HTTP->UI] AppController::Connect() CALL this=") +
                std::to_string((uintptr_t)window) +
                " hwnd=" + std::to_string((uintptr_t)hwnd) +
                " adapterSource=" + DescribeHttpConnectAdapterSource(plan.adapterSource) +
                " override=" + (plan.adapterOverride.empty() ? std::string("(none)") : plan.adapterOverride) +
                "\n");
    } catch (...) {
    }

    if (plan.adapterSource == HttpConnectAdapterSource::kUiSelection) {
        AppController::GetInstance().Connect(std::string());
        return;
    }

    AppController::GetInstance().Connect(plan.adapterOverride);
}

enum class HttpAppControllerCommandType {
    kDisconnect,
    kAutoConnect,
    kStartAuto,
    kStopAuto,
    kAutoDownToOrigin,
    kAutoFaultReset,
};

struct HttpAppControllerCommandPlan {
    HttpAppControllerCommandType type = HttpAppControllerCommandType::kDisconnect;
    bool showAutoWindow = false;
};

static bool BuildHttpAppControllerCommandPlan(const std::string& cmd,
                                              HttpAppControllerCommandPlan* planOut) {
    HttpAppControllerCommandPlan plan;

    if (cmd == "DISCONNECT") {
        plan.type = HttpAppControllerCommandType::kDisconnect;
    } else if (cmd == "AUTO_CONNECT") {
        plan.type = HttpAppControllerCommandType::kAutoConnect;
    } else if (cmd == "START_AUTO") {
        plan.type = HttpAppControllerCommandType::kStartAuto;
        plan.showAutoWindow = false;
    } else if (cmd == "START_AUTO_WITH_WINDOW") {
        plan.type = HttpAppControllerCommandType::kStartAuto;
        plan.showAutoWindow = true;
    } else if (cmd == "STOP_AUTO") {
        plan.type = HttpAppControllerCommandType::kStopAuto;
    } else if (cmd == "AUTO_DOWN_TO_ORIGIN") {
        plan.type = HttpAppControllerCommandType::kAutoDownToOrigin;
    } else if (cmd == "AUTO_FAULT_RESET") {
        plan.type = HttpAppControllerCommandType::kAutoFaultReset;
    } else {
        return false;
    }

    if (planOut) {
        *planOut = plan;
    }
    return true;
}

static void ExecuteHttpAppControllerCommand(const HttpAppControllerCommandPlan& plan) {
    auto& controller = AppController::GetInstance();

    switch (plan.type) {
    case HttpAppControllerCommandType::kDisconnect:
        controller.Disconnect();
        return;
    case HttpAppControllerCommandType::kAutoConnect:
        controller.RequestAutoConnectFromHttp();
        return;
    case HttpAppControllerCommandType::kStartAuto:
        controller.StartAutoProcedureFromHttp(plan.showAutoWindow);
        return;
    case HttpAppControllerCommandType::kStopAuto:
        controller.StopAutoProcedureFromHttp();
        return;
    case HttpAppControllerCommandType::kAutoDownToOrigin:
        controller.RequestAutoDownToOriginFromHttp();
        return;
    case HttpAppControllerCommandType::kAutoFaultReset:
        controller.AutoFaultResetAllFromHttp();
        return;
    }
}

// NOTE: the helper below (and the combo helpers further down) send messages to
// native controls, so they must only ever run on the UI thread. They are reached
// exclusively through the posted WM_USER_POST_UTF8_LOG command pipeline
// (MainWindow::HandlePostedUtf8LogMessage), never directly from an HTTP worker.
static void ApplyHttpMmfOutputToggle(HWND hwnd, HWND hChkMMFOutput, bool enabled) {
    if (!hChkMMFOutput || !IsWindow(hChkMMFOutput)) return;

    SendMessage(
        hChkMMFOutput,
        BM_SETCHECK,
        enabled ? BST_CHECKED : BST_UNCHECKED,
        0);
    PostMessage(
        hwnd,
        WM_COMMAND,
        MAKEWPARAM(IDC_CHK_MMF_OUTPUT, BN_CLICKED),
        (LPARAM)hChkMMFOutput);
}

static void NotifySlaveComboSelectionChanged(HWND hwnd, HWND hComboSlaves) {
    if (!hComboSlaves || !IsWindow(hComboSlaves)) return;

    PostMessage(
        hwnd,
        WM_COMMAND,
        MAKEWPARAM(GetDlgCtrlID(hComboSlaves), CBN_SELENDOK),
        (LPARAM)hComboSlaves);
}

static bool SelectSlaveComboIndex(HWND hwnd, HWND hComboSlaves, int index) {
    if (!hComboSlaves || !IsWindow(hComboSlaves)) return false;

    SendMessageW(hComboSlaves, CB_SETCURSEL, (WPARAM)index, 0);
    NotifySlaveComboSelectionChanged(hwnd, hComboSlaves);
    return true;
}

static bool TrySelectSlaveFromHttpValue(HWND hwnd, HWND hComboSlaves, const std::string& val) {
    if (!hComboSlaves || !IsWindow(hComboSlaves) || val.empty()) return false;

    try {
        int pos = std::stoi(val);
        if (pos > 0) {
            int count = (int)SendMessageW(hComboSlaves, CB_GETCOUNT, 0, 0);
            for (int i = 0; i < count; ++i) {
                LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, i, 0);
                if ((int)itemData == pos) {
                    return SelectSlaveComboIndex(hwnd, hComboSlaves, i);
                }
            }
        }
    } catch (...) {
    }

    std::wstring wval = CoreHelpers::Utf8ToWide(val);
    if (wval.empty()) return false;

    int count = (int)SendMessageW(hComboSlaves, CB_GETCOUNT, 0, 0);
    wchar_t buf[1024];
    for (int i = 0; i < count; ++i) {
        ZeroMemory(buf, sizeof(buf));
        SendMessageW(hComboSlaves, CB_GETLBTEXT, i, (LPARAM)buf);
        std::wstring itemText(buf);
        if (itemText == wval || itemText.find(wval) != std::wstring::npos) {
            return SelectSlaveComboIndex(hwnd, hComboSlaves, i);
        }

        std::wstring prefix = wval + L" - ";
        if (itemText.rfind(prefix, 0) == 0) {
            return SelectSlaveComboIndex(hwnd, hComboSlaves, i);
        }
    }

    return false;
}

void MainWindow::ReportHttpUiState()
{
    try {
        auto snapshot = AppController::GetInstance().GetStatusSnapshot();
        const bool isConnected = snapshot.master_initialized;
        const bool hasESI = !currentObjects.empty();
        const bool hasSlave = (confirmedSlave > 0);
        const bool canStopAuto = CanExecuteAutoProcedureStopRequest();

        SetUiDisabledFromControlOrFallback("connect", hBtnConnect, isConnected);
        SetUiDisabledFromControlOrFallback("disconnect", hBtnDisconnect, !isConnected);

        if (hEditCycleTime && IsWindow(hEditCycleTime)) {
            SetUiDisabled("set_cycle", !IsWindowEnabled(hEditCycleTime));
        } else {
            SetUiDisabled("set_cycle", false);
        }

        SyncAutoProcedureUiDisabledStates(
            snapshot, hButtonStartAutoMode, hBtnStopAutoMode, canStopAuto);
        SyncAutoPageUiDisabledStates(snapshot,
                                     hAutoWindow,
                                     IsAutoProcedureUiLockActive(),
                                     IsExternalImmediateStopActive() || IsExternalEStopActive());

        if (hBtnSendMailbox && IsWindow(hBtnSendMailbox)) {
            SetUiDisabled("send_mailbox", !IsWindowEnabled(hBtnSendMailbox));
        } else {
            SetUiDisabled("send_mailbox", false);
        }

        SetUiDisabled("dump_esi", !(hasESI && hasSlave));

        if (hBtnBrowseESI && IsWindow(hBtnBrowseESI)) {
            SetUiDisabled("browse_esi", !IsWindowEnabled(hBtnBrowseESI));
        } else {
            SetUiDisabled("browse_esi", false);
        }
    } catch (...) {
    }
}

void MainWindow::ApplyConnectedUiControlState(const CoreHelpers::SystemStateSnapshot& snapshot,
                                              bool reflowMainControls)
{
    const bool autoUiLockActive = IsAutoProcedureUiLockActive();
    SetControlEnabled(hBtnConnect, false);
    SetControlEnabled(hBtnDisconnect, autoUiLockActive ? false : true);
    SetUiDisabled("connect", true);
    SetUiDisabled("disconnect", autoUiLockActive ? true : false);

    const bool hasESI = !currentObjects.empty();
    const bool isConnected = snapshot.master_initialized;
    const bool hasSlave = (confirmedSlave > 0);
    const bool enableMotion = hasESI && isConnected && hasSlave;
    const bool motionControlsEnabled = enableMotion && !autoUiLockActive;
    const bool canStopAuto = CanExecuteAutoProcedureStopRequest();

    SetControlEnabled(hBtnReadSDO, hasESI && isConnected);
    SetControlEnabled(hBtnWriteSDO, hasESI && isConnected);
    SetControlEnabled(hBtnReadAllSDO, hasESI && isConnected);
    if (hBtnSaveParams) SetControlEnabled(hBtnSaveParams, autoUiLockActive ? false : true);
    SetControlEnabled(hBtnSendMailbox, autoUiLockActive ? false : true);
    SetUiDisabled("send_mailbox", autoUiLockActive ? true : false);
    if (hBtnSaveConfig) SetControlEnabled(hBtnSaveConfig, autoUiLockActive ? false : true);

    if (hBtnBrowseESI) SetControlEnabled(hBtnBrowseESI, true);
    if (hBtnImportParams) SetControlEnabled(hBtnImportParams, true);
    if (hBtnClearStatus) {
        HWND btn = (GetParent(hBtnClearStatus) == hContentWnd)
            ? hBtnClearStatus
            : (hChildStatusWnd ? GetDlgItem(hChildStatusWnd, 1032) : NULL);
        if (btn) SetControlEnabled(btn, true);
    }
    if (hBtnPDOSentStatus) SetControlEnabled(hBtnPDOSentStatus, true);
    if (hButtonPDOStart) SetControlEnabled(hButtonPDOStart, true);
    if (hButtonPDOStop) SetControlEnabled(hButtonPDOStop, true);
    if (hComboSlaves) SetControlEnabled(hComboSlaves, autoUiLockActive ? false : true);
    if (hBtnLoadConfig) SetControlEnabled(hBtnLoadConfig, false);
    if (hBtnWriteAllSDO) SetControlEnabled(hBtnWriteAllSDO, hasESI && isConnected && hasSlave);

    if (reflowMainControls) {
        ReflowMainControls();
    }

    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, motionControlsEnabled);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, motionControlsEnabled);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, motionControlsEnabled);
    if (hBtnHome) SetControlEnabled(hBtnHome, motionControlsEnabled);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, motionControlsEnabled);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, motionControlsEnabled);
    if (hBtnFaultReset) SetControlEnabled(hBtnFaultReset, (isConnected || enableMotion) && !autoUiLockActive);
    if (hBtnImmediateStop) SetControlEnabled(hBtnImmediateStop, enableMotion);
    if (hBtnStopAutoMode) SetControlEnabled(hBtnStopAutoMode, canStopAuto);
    if (hButtonStartAutoMode) SetControlEnabled(hButtonStartAutoMode, isConnected && !autoUiLockActive);
    SyncAutoProcedureUiDisabledStates(snapshot, hButtonStartAutoMode, hBtnStopAutoMode, canStopAuto);
    ApplyAutoProcedureUiLockState();
    if (hBtnAutoWindow) SetControlEnabled(hBtnAutoWindow, true);

    std::string dbg = std::string("MOTION_ENABLE slave=") + std::to_string(confirmedSlave) +
                      (enableMotion ? " TRUE\n" : " FALSE\n");
    MainWindow::PostUTF8LogToHWND(hwnd, dbg);
    PostCommandToUIOrCache(std::string("HTTP_CMD:REPORT_UI_STATE"));
}

LRESULT MainWindow::HandlePostedUtf8LogMessage(PostedUtf8UiMessage* message)
{
            if (message) {
                std::unique_ptr<PostedUtf8UiMessage> posted(message);
                std::string& text = posted->text;
                const bool commandLike = posted->commandLike || IsCommandLikeUiMessage(text);
                if (!commandLike && posted->statusEpoch != 0) {
                    const uint64_t currentEpoch = statusLogEpoch_.load(std::memory_order_acquire);
                    if (posted->statusEpoch != currentEpoch) {
                        return 0;
                    }
                }
                // HTTP command messages posted from HttpServer.cpp use the form:
                // "HTTP_CMD:COMMAND" or "HTTP_CMD:MAILBOX:<utf8-body>"
                if (text.rfind("HTTP_CMD:", 0) == 0) {
                    std::string cmd = text.substr(strlen("HTTP_CMD:"));
                        // Support web-initiated connect with explicit adapter: CONNECT_WEB:<ifname>
                        if (cmd.rfind("CONNECT_WEB:", 0) == 0) {
                            std::string adapter = cmd.substr(strlen("CONNECT_WEB:"));
                            HttpConnectCommandPlan plan;
                            if (!BuildHttpConnectCommandPlan(
                                    hwnd,
                                    "CONNECT_WEB",
                                    &adapter,
                                    this->webSelectedIfName,
                                    &plan)) {
                                return 0;
                            }
                            ExecuteHttpConnectCommand(this, hwnd, L"[HTTP->UI] Received CONNECT (web) command\r\n", plan);
                            return 0;
                        } else if (cmd == "CONNECT") {
                            HttpConnectCommandPlan plan;
                            if (!BuildHttpConnectCommandPlan(
                                    hwnd,
                                    "CONNECT",
                                    nullptr,
                                    this->webSelectedIfName,
                                    &plan)) {
                                return 0;
                            }
                            ExecuteHttpConnectCommand(this, hwnd, L"[HTTP->UI] Received CONNECT command\r\n", plan);
                        return 0;
                    }

                    HttpAppControllerCommandPlan appControllerPlan;
                    if (BuildHttpAppControllerCommandPlan(cmd, &appControllerPlan)) {
                        ExecuteHttpAppControllerCommand(appControllerPlan);
                        return 0;
                    } else if (cmd == "CLEAR_STATUS") {
                        OnClearStatus();
                        return 0;
                    } else if (cmd == "TOGGLE_STATUS") {
                        // Toggle the child status window (web / HTTP toggle should open the status pane)
                        ToggleChildStatusWindow();
                        return 0;
                    } else if (cmd == "REPORT_UI_STATE") {
                        ReportHttpUiState();
                        return 0;
                    } else if (cmd == "DUMP_ESI") {
                        OnDumpESIForSelectedSlave();
                        return 0;
                    } else if (cmd == "BROWSE_ESI") {
                        OnBrowseESI();
                        return 0;
                    } else if (cmd.rfind("SET_PERIOD:",0) == 0) {
                            // Format: SET_PERIOD:<microseconds>
                            std::string val = cmd.substr(strlen("SET_PERIOD:"));
                            try {
                                uint32_t us = (uint32_t)std::stoul(val);
                                // 🔧 範圍驗證：0.5~10 ms，超出範圍自動設為 2 ms (2000 us)
                                double ms = ((double)us) / 1000.0;
                                if (ms < 0.5 || ms > 10.0) {
                                    us = 2000; // 2 ms in microseconds
                                    ms = 2.0;
                                    PostUTF8LogToUI(std::string("[HTTP] 週期超出範圍 (0.5~10 ms)，自動設為 2 ms\n"));
                                }
                                
                                // update UI edit control (convert to ms, format with 2 decimals)
                                char buf[64]; snprintf(buf, sizeof(buf), "%.2f", ms);
                                std::wstring w = CoreHelpers::Utf8ToWide(std::string(buf));
                                // safely set desired cycle time text
                                SafeSetTextW(hEditCycleTime, w);
                                // also update server-side mirror if applicable
                                // (the server already set g_desiredCycleTimeUs)
                            } catch(...) { }
                            return 0;
                        } else if (cmd.rfind("LOAD_ESI:",0) == 0) {
                            std::string val = cmd.substr(strlen("LOAD_ESI:"));
                            // val is an ANSI path (UTF-8 on Windows), convert to wide and import
                            std::wstring wpath = CoreHelpers::Utf8ToWide(val);
                            PopulateObjectsFromESI(wpath);
                            return 0;
                    } else if (cmd == "SEND_MAILBOX") {
                        OnSendMailbox();
                        return 0;
                    } else if (cmd == "MMF_ENABLE") {
                        ApplyHttpMmfOutputToggle(hwnd, hChkMMFOutput, true);
                        return 0;
                    } else if (cmd == "MMF_DISABLE") {
                        ApplyHttpMmfOutputToggle(hwnd, hChkMMFOutput, false);
                        return 0;
                    } else if (cmd.rfind("SELECT_SLAVE:",0) == 0) {
                        // Format: SELECT_SLAVE:<value>
                        std::string val = cmd.substr(strlen("SELECT_SLAVE:"));
                        (void)TrySelectSlaveFromHttpValue(hwnd, hComboSlaves, val);

                        return 0;
                    } else if (cmd == "READ_SDO") {
                        OnReadSDO(); return 0;

                    // SET_ADAPTER commands from HTTP are ignored when bidirectional
                    // sync is disabled; do not apply adapter selections from remote
                    // clients. Previously handled here by calling SetSelectedAdapter().
                    
                    } else if (cmd == "WRITE_SDO") {
                        OnWriteSDO(); return 0;
                    } else if (cmd == "READ_ALL_SDO") {
                        OnReadAllSDO(); return 0;
                    } else if (cmd == "WRITE_ALL_SDO") {
                        OnWriteAllSDO(); return 0;
                    } else if (cmd.rfind("MAILBOX:",0) == 0) {
                        std::string body = cmd.substr(strlen("MAILBOX:"));
                        // Store mailbox in server-owned cache
                        SetMailboxFromHttp(body);
                        // Trigger UI to fetch mailbox and send
                        MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:APPLY_MAILBOX"));
                        return 0;
                    }
                    else if (cmd == "ENABLE_CONTROLS") {
                        // Bridge HTTP command to existing UI handler which expects
                        // the plain "ENABLE_CONTROLS\n" string. Post back to hwnd
                        // so the centralized handler runs the same code path.
                        PostUTF8LogToHWND(hwnd, std::string("ENABLE_CONTROLS\n"));
                        return 0;
                    } else if (cmd == "ENABLE_CONTROLS_NOREFLOW") {
                        PostUTF8LogToHWND(hwnd, std::string("ENABLE_CONTROLS_NOREFLOW\n"));
                        return 0;
                    }
                    
                }
            // Check for special commands
            if (text == "ENABLE_CONTROLS_NOREFLOW\n") {
                // Enable controls like ENABLE_CONTROLS but avoid invoking ReflowMainControls()
                auto snapshot_nr = AppController::GetInstance().GetStatusSnapshot();
                ApplyConnectedUiControlState(snapshot_nr, false);

                return 0;
            }

            if (text == "ENABLE_CONTROLS\n") {
                auto snapshot = AppController::GetInstance().GetStatusSnapshot();
                ApplyConnectedUiControlState(snapshot, true);

                return 0;
            }
            
            if (text == "UPDATE_SLAVE_LIST\n") {
                UpdateSlaveList();
                return 0;
            }
            
            if (text == "SHUTDOWN_COMPLETE\n") {
                CompleteDisconnect();
                return 0;
            }
            if (text == "STOP_TIMER\n") {
                KillTimer(hwnd, TIMER_ID);
                return 0;
            }
            if (text == "START_TIMER\n") {
                // Start a UI timer for OnTimer updates (100 ms)
                if ((!minimizedToTray && !isMinimizedState) || this->forceRunWhenMinimized) SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                // 顯示PDO面板
                pdoDisplay.Show(true);
                // Reset observed max comms time to avoid carryover from prior session
                if (auto* svc = AppController::GetInstance().GetEtherCATService()) {
                    svc->ResetMaxObservedCommsTime();
                    svc->ResetMaxObservedCycleMicros();
                }
                // Reset UI-level maxObservedCycleTime too
                this->maxObservedCycleTime = 0;
                SafeSetTextW(hEditCurrentCycleTime, std::wstring(L"C0/P0M0 F0Hz"));
                // Start warm-up delay before collecting maxObservedCycleTime
                // Record absolute tick when collection should start
                this->maxObservedCollectStartTick = GetTickCount64() + this->maxObservedCollectDelayMs;
                return 0;
            }
            if (text == "ENABLE_HOME\n") {
                // Only re-enable if homingBusy==false (defensive)
                if (!homingBusy.load()) {
                    if (hBtnHome) SetControlEnabled(hBtnHome, true);
                }
                return 0;
            }

            // Handle local HTTP-local commands posted by server thread
            if (text.rfind("HTTP_LOCAL:APPLY_MAILBOX", 0) == 0) {
                // Fetch mailbox text from server cache and apply to edit and send
                std::string mb = GetMailboxUtf8();
                if (!mb.empty()) {
                    std::wstring wmb = CoreHelpers::Utf8ToWide(mb);
                    if (hEditMailbox && IsWindow(hEditMailbox)) SafeSetTextW(hEditMailbox, wmb);
                    OnSendMailbox();
                }
                return 0;
            }
            if (text.rfind("HTTP_LOCAL:STATUS:", 0) == 0) {
                std::string payload = text.substr(strlen("HTTP_LOCAL:STATUS:"));
                // Publish status text for /status endpoint
                SetLastStatusUtf8(payload);
                return 0;
            }

            // Update status window directly without triggering main window repaint
            std::wstring wmsg = CoreHelpers::Utf8ToWide(text);
            // Only update if status window is visible to avoid unnecessary work
            if (hChildStatusWnd && IsWindow(hChildStatusWnd) && IsWindowVisible(hChildStatusWnd) && !IsIconic(hChildStatusWnd)) {
                PrependStatus(wmsg.c_str());
            } else {
                // Status window not visible: just update in-memory buffer (file write controlled by PrependStatus)
                PrependStatus(wmsg.c_str());
            }
        }
        return 0;
}

