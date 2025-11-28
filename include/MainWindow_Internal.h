#ifndef MAIN_WINDOW_INTERNAL_H
#define MAIN_WINDOW_INTERNAL_H

#ifndef MAIN_WINDOW_H
#error "Include MainWindow.h before MainWindow_Internal.h"
#endif

private:
    // Track last observed low-order byte of SDO 0x60FD so we can detect
    // edges on interest bits (bit0 = Immediate Stop, active-high; bit5 = Emergency Stop, active-low).
    // -1 indicates "unknown / not yet observed". Use atomic for thread-safety
    // because SDO reader invokes callbacks from a background thread.
    std::atomic<int> lastSdo60FDLastByte{ -1 };
    std::atomic<int> sdo60FDFailureStreak{0};
    std::atomic<int> sdo60FDConsecutiveFailureCount{0};
    std::atomic<ULONGLONG> sdo60FDFailureSinceTick{0};
    std::atomic<ULONGLONG> lastSdo60FDFailureTick{0};
    std::atomic<bool> sdo60FDDisconnectRequested{false};
    static constexpr ULONGLONG SDO_60FD_FAILURE_WINDOW_MS = 1000;
    // Whether status messages are also written to logs\status.log (persisted via INI)
    std::atomic<bool> statusLogEnabled_{true};

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, 
                                       WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandlePostedUtf8LogMessage(PostedUtf8UiMessage* message);
    void ReportHttpUiState();
    void ApplyConnectedUiControlState(const CoreHelpers::SystemStateSnapshot& snapshot, bool reflowMainControls);

    // Helpers for Auto Down-To-Origin flow and shared motion logic
    struct MotionParamSnapshot {
        bool haveP1 = false;
        double p1 = 0.0;
        bool haveP2 = false;
        long p2 = 0;
        bool haveP3 = false;
        double p3 = 0.0;
        bool haveP4 = false;
        double p4 = 0.0;
        bool haveP5 = false;
        double p5 = 0.0;
        bool haveP6 = false;
        double p6 = 0.0;
        bool haveP8 = false;
        double p8 = 0.0;
        bool haveP9 = false;
        double p9 = 0.0;
        bool haveP10 = false;
        double p10 = 0.0;
        bool haveP11 = false;
        double p11 = 0.0;
        bool haveP12 = false;
        double p12 = 0.0;
        bool haveP13 = false;
        double p13 = 0.0;
        uint32_t rawP13 = 0;
    };
    MotionParamSnapshot CaptureMotionParams(int slaveIdx) const;
    void ApplySavedPdoFilterConfig(class EtherCATMaster *master, int slaveIdx,
                                   const MotionParamSnapshot &snapshot,
                                   const char *logPrefix,
                                   bool emitConfiguredLog);
    uint32_t ResolveJogVelocityCommand(int slaveIdx, bool positiveDirection) const;
    bool PreparePDOForSlave(int slaveIdx);
    bool TryGetMechanicalActualPosition(int slaveIdx, double &outPosition) const;
    bool TryGetMechanicalActualSpeed(int slaveIdx, double &outSpeed) const;
    bool ComputeHomeCommandValue(int slaveIdx, bool &haveValue, uint64_t &value);
    bool ValidateHomePreconditions(int slaveIdx, std::wstring &errorMessage);
    bool RunAutoDownToOriginWorkflow();
    // Jog operation preamble: verify prerequisites and set busy flags
    // Returns true if jog can proceed, false if operation should be aborted
    // Sets jogPressRequested=true, jogBusy=true, posts status update on success
    bool StartJogPreamble();

    // UI 處理函數
    void CreateControls();
    // Small helpers used to split CreateControls into logical sections
    void CreateAdapterControls(class LayoutHelper &helper, int &y, HFONT hFont);
    void CreateCycleControls(class LayoutHelper &helper, int &y, int clientW, HFONT hFont);
    void CreateSlaveListControls(class LayoutHelper &helper, int &y, int clientW, int slaveListHeight, HFONT hFont, int ctrlW);
    void CreateSDOControls(class LayoutHelper &helper, int &y, int clientW, int sdoTableHeight, HFONT hFont, int left, int labelW, int ctrlW);
    // OverrideIfName: if non-null, OnConnect will use that interface name for this connect attempt
    void OnConnect(const std::string* overrideIfName = nullptr);
    void OnDisconnect();
    void CompleteDisconnect();
    void ApplyDisconnectedUiControlState();
    void BeginDisconnectUiTransition();
    std::function<void()> CreateServerDisconnectGuardRelease();
    void DispatchDisconnectShutdown(
        bool hasThreads,
        HWND capturedHwnd,
        const std::function<void()> &releaseServerDisconnectGuard);
    void PrepareDisconnectShutdown();
    void StartAsyncDisconnectShutdown(
        HWND capturedHwnd,
        const std::function<void()> &releaseServerDisconnectGuard);
    void StartDisconnectShutdownWatchdog(
        HWND capturedHwnd,
        const std::function<void()> &releaseServerDisconnectGuard);
    void StartSynchronousDisconnectShutdown(
        HWND capturedHwnd,
        const std::function<void()> &releaseServerDisconnectGuard);
    void PostShutdownCompleteOrClearFlag(HWND capturedHwnd);
    void ReportShutdownExceptionAndComplete(HWND capturedHwnd, const std::string &message);
    void ResetDisconnectedRuntimeState();
    void StopDisconnectAlertSounds();
    // Mark the connection workflow as complete by clearing worker and connect-attempt flags
    // Used in multiple early-exit/abort paths within the connect workflow
    void MarkConnectionWorkComplete();
    std::string NormalizeInterfaceName(const std::string &ifname) const;
    bool ResolveConnectInterfaceName(const std::string* overrideIfName, std::string &ifname);
    bool TryResolveAdapterPcapName(std::string &ifname, std::string *matchKind = nullptr) const;
    std::string FindAdapterDisplayName(const std::string &ifname) const;
    bool LoadDefaultMotionConfigForConnect();
    bool InitializeMasterWithFallback(
        std::string &ifname,
        uint32_t desiredCycleTime,
        const std::function<void(const std::string&)> &post);
    bool WriteSafeStartupControlword(const std::function<void(const std::string&)> &post);
    void WriteStartupMotionParams(const std::function<void(const std::string&)> &post);
    void SyncMotionParamsToUi(int slaveIndex);
    void CacheMotionParamsFromUi(int slaveIndex);
    void OptimizeAdapterForEthercat(
        const std::string &ifname,
        const std::function<void(const std::string&)> &post);
    void VerifyCyclicModeAndPdoReadiness(
        uint32_t desiredCycleTime,
        const std::function<void(const std::string&)> &post);
    void FinalizeSuccessfulConnection(
        const std::function<void()> &releaseServerConnectGuard,
        const std::function<void(const std::string&)> &post);
    void HandleConfigureSlavesFailure(
        const std::string &errorMessage,
        const std::function<void(const std::string&)> &post);
    void HandleStartCyclicOperationFailure(
        const std::string &errorMessage,
        const std::function<void(const std::string&)> &post);
    bool AbortConnectIfDisconnectRequested(
        const char *message,
        const std::function<void(const std::string&)> &post);
    void StartPostConnectSdoReader();
    void HandleSdo60FDCallback(const std::string &message);
    // Extracted WM_USER_AUTO_IMMEDIATE_STOP_REQUEST handler
    void HandleAutoImmediateStopRequest(HWND hRequester, bool isPDOErrorTriggered, int slaveNumber, bool requireManualReset = false, bool skipManualReset = false);
    // Extracted WM_USER_AUTO_EMERGENCY_STOP_REQUEST handler
    void HandleAutoEmergencyStopRequest(HWND hRequester, bool trackingErrorTriggered = false);
    void SetBusyFlagAndNotify(std::atomic<bool> &flag, bool value);
    bool RequestPdoSafetyManualResetStop(int slaveIdx, PDOFilter::ViolationType type, double duration);
    void AbortAutoConnectSoftStartForStop(const char *logPrefix);
    void RequestAbortAutoDownToOriginForStop(const char *logPrefix);
    void DisablePdoFilteringAfterStop(const char *logPrefix);
    void NotifyAutoDownToOriginCompletion(HWND hRequester, bool success, bool fallbackToAutoWindow = true);
    void NotifyAutoStopCompletion(HWND hRequester, UINT doneMessage, bool success, LPARAM doneParam);
    void NotifyAutoFaultResetCompletion(HWND hRequester, bool success);
    void FinalizeAutoFaultReset(HWND hRequester, bool success);
    bool IsExternalImmediateStopActive() const;
    bool IsExternalEStopActive() const;
    void FinalizeAutoImmediateStop(HWND hRequester, bool success, bool isPDOErrorTriggered);
    void FinalizeAutoEmergencyStop(HWND hRequester, bool success);
    void FinalizeErrorAlertSoundForStop(const char *stoppedLog, bool allowStop, const char *continueLog = nullptr);
    void StartErrorAlertSoundForStop(const char *logPrefix);
    void SetFaultResetUiEnabledForStop(bool enabled);
    void NotifyAutoWindowOwnerState(WPARAM state);
    bool ExecuteStopMotionSequence(const char *logPrefix, bool allowSoftStartRetry);
    bool TryPowerOnAllSlaves(const char *logPrefix, const char *partialFailureLog = nullptr, const char *missingControllerLog = nullptr);
    bool ClearMasterErrorAndResetPdoFilters(int slaveNumber = 0);
    void RefreshInternalErrorStateFrom603F(const char *logPrefix = nullptr);
    void OnReadSDO();
    void OnWriteSDO();
    void OnSendMailbox();
    // UpdateSlaveList: refresh the ListView / Combobox entries for detected EtherCAT slaves
    // NOTE: Slave list updates are intended to occur during the connection lifecycle
    // (OnConnect) and are intentionally not triggered by mouse clicks or other
    // UI interactions. This prevents unintended SDO reads and UI refreshes.
    void UpdateSlaveList();
    void UpdateStatusText();
    void UpdateSelectedSlaveStatus(int slaveIdx);  // 更新選定從站的狀態顯示
    void OnTimer();
    void PopulateAdapters();
    bool TryGetAdapterPcapNameFromComboSelection(int selectionIndex, std::string &ifname) const;
    bool IsAutoProcedureUiLockActive() const;
    bool ShouldRefreshMainWindowUi() const;
    void ApplyAutoProcedureUiLockState();

    // Anomaly aggregator API
    void RecordInputAnomaly(int slaveIdx, double originalSet, double replacedSet, double inputChange, double maxAllowedChange, PDOFilter::InputAnomalyReason reason);
    void StartAnomalyAggregator();
    void StopAnomalyAggregator();
    // Logic-extracted: enumerate adapters and publish JSON / default selection
    void OnEnumerateAdapters();

    // Ensure PC 'Connect' button state is correct for given adapter selection.
    // Checks that adapter reports link up and that the system is not busy/connected.
    void UpdateConnectButtonStateForAdapter(const std::string &ifname);
    // Front-end integration: set selected adapter (pcap ifname, UTF-8)
    // ESI handling
    void PopulateESIFiles();
    void PopulateObjectsFromESI(const std::wstring& esiPathW);
    void OnBrowseESI();
    // Logic-extracted: perform ESI dump for a given slave (UI-independent)
    void OnDumpESI(uint16_t slave);
    // Notify AutoWindow (modeless) that auto status lights should refresh
    void PostAutoStatusUpdate();
    void ReflowSDOControls(int dynamicHeight = 200);
    // Adaptive reflow for main controls (combos, list, mailbox, status)
    void ReflowMainControls();
    // Reflow helpers extracted to simplify ReflowMainControls
    void ReflowMailboxControls(int clientW, int &contentH_local, int &availableW, int left, int labelW);
    void ReflowPDOControls(int clientW, int &contentH_local, int left);
    void ReflowMotionControls(int clientW, int &contentH_local, int left, int labelW);
    // Additional CreateControls helpers (extracted to reduce CreateControls size)
    void CreateMailboxControls(class LayoutHelper &helper, int &y, int clientW, int mailboxHeight, HFONT hFont, int labelW, int contentWidth);
    void CreatePDOControls(class LayoutHelper &helper, int &y, int clientW, int pdoHeight, HFONT hFont, int left, int labelW, int ctrlW, int contentWidth);
    void CreateMotionControls(class LayoutHelper &helper, int &y, int clientW, int pdoHeight, HFONT hFont, int left, int labelW, int ctrlW);
    // Lightweight: only update content pane position/size on scroll without reflow/rebuild
    void UpdateContentPaneScrollOnly();
    // Diagnostic: log current SDO control positions to status box
    void LogSDOControlPositions();
    // Force a full repaint of main window and key child controls
    void ForceFullRedraw();
    // Internal immediate redraw helper (bypasses throttle)
    void ForceFullRedrawNow();

    // NOTE: ReadPDOItemValueFromSnapshot moved to include/PDOHelpers.h as a free function
    // 快取/還原：以 PDO 項目名稱為鍵，保存/還原可編輯欄位文字（跨從站切換時維持不變）
    void CaptureCurrentPDOInputsByName();
    void RestorePDOInputsFromGlobal();

public:
    // Public data needed by window subclass procedures and helpers
    // Parsed ESI data cache (moved from private so subclass procs can read it)
    std::vector<EsiObjectEntry> currentObjects;
    std::vector<EsiPdoEntry> currentPdos;
    // Per-slave cached SDO value strings (index into currentObjects -> value column text)
    std::map<int, std::vector<std::wstring>> savedSDOValues;
    // Per-slave cached SDO hex (BE) representation for display in new Hex column
    std::map<int, std::vector<std::wstring>> savedSDOHexValues;

    // Per-slave saved motion parameter config file path (if user associated a file with a slave)
    std::map<int, std::wstring> savedMotionConfigForSlave;
    // Last loaded motion config when not connected (global)
    std::wstring savedMotionConfigGlobal;
    // In-memory per-slave motion parameters cache: [slave] -> rows -> cols (strings)
    std::map<int, std::vector<std::vector<std::wstring>>> savedMotionParamsForSlave;

    // In-place editor state exposed so subclass procs can align/commit edits
    HWND hInplaceEdit = NULL;
    int inplaceEditItem = -1; // currently editing ListView item index
    int inplaceEditSubitem = 5; // currently editing subitem (default to Value column)
    std::wstring inplaceEditOriginalText; // original text before in-place edit
    bool inplaceEditIsMotion = false; // true when the in-place editor is for motion list


private:
    HWND hwnd;
    HINSTANCE hInstance;
    DWORD uiThreadId = 0;

    // 控制項句柄
    HWND hComboAdapter;
    HWND hComboSlaves; // 新增: 從站下拉選單（依從站數量填充）
    HWND hBtnConnect;
    HWND hBtnDisconnect;
    HWND hListSlaves;
    // HTTP server URL display + open button (placed above slave list)
    HWND hStaticHttpUrl;
    HWND hBtnOpenHttp;
    // Checkbox: enable/disable local HTTP server (persisted to INI)
    HWND hChkHttpServer;
    // Startup option checkboxes (persisted to INI)
    HWND hChkStartupShowMain;   // 啟動時顯示主視窗
    HWND hChkStartupAutoSeq;    // 啟動後自動執行 AutoStartupSequence
    // SDO: 使用參數讀寫表格取代原先的下拉選單與整列操作區
    HWND hListSDOParams; // ListView for SDO parameter read/write (替代 hComboSDO + SDO 操作列)
    HWND hEditMailbox;
    HWND hBtnSendMailbox;
    HWND hEditCycleTime;        // Desired cycle time (editable)
    HWND hEditCurrentCycleTime; // Current cycle time (display only)
    HWND hEditWKC;              // Working Counter (display only)
    // Auto cycle UI removed: controls for editable auto-cycle and measured-ms
    // were intentionally removed as SDO polling is now managed by EtherCATMaster
    // and operates at a fixed 10 ms interval. No HWND members remain here.
    
    // PDO UI controls
    HWND hCheckPDOEnable;
    HWND hButtonPDOStart;
    HWND hButtonPDOStop;
    HWND hListIOmap; // ListView for IOmap (Offset, Bitlen, Name, Index:Sub)
    // PDO I/O controls
    HWND hEditPDOTargetPosition; // input
    HWND hEditPDOCurrentPosition; // readonly output
    HWND hEditPDOSent; // PDO_sent numeric edit (originally created locally)
    HWND hBtnPDOSentStatus; // button to toggle child status window
    HWND hBtnAutoWindow; // button to toggle the Auto Window
    // ESI UI controls
    HWND hBtnBrowseESI;
    HWND hBtnClearStatus; // 狀態清除按鈕（清除狀態訊息並截斷日誌）
    HWND hChkStatusLog; // 狀態欄：寫入檔案勾選框（控制是否同步寫入 logs\status.log）

    // Motion control UI controls
    HWND hBtnFollowMode;    // 跟隨模式
    HWND hBtnPowerOn;       // 電源啟動
    HWND hBtnPowerOff;      // 電源關閉
    HWND hBtnHome;          // 原點設定
    HWND hBtnJogCW;         // 寸動正轉
    HWND hBtnJogCCW;        // 寸動反轉
    HWND hBtnFaultReset;    // 異常重置
    HWND hBtnImmediateStop; // 立即停止（寬 2 倍）
    HWND hBtnStopAutoMode;    // 手動模式
    HWND hButtonStartAutoMode;      // 自動模式
    // Motion config buttons
    HWND hBtnLoadConfig;    // 設定檔讀取
    HWND hBtnSaveConfig;    // 設定檔存檔
    // MMF 可視化輸出勾選框
    HWND hChkMMFOutput;     // MMF 即時輸出
    // 靜態文字：顯示 MMF 名稱與連線資訊，位於勾選框右側
    
    HWND hStaticMMFInfo;
    // Motion parameters ListView (新增：顯示運動參數名稱/值/單位/說明)
    HWND hListMotionParams;
    // Mechanical units list (目標/實際 位置/速度/扭力)
    HWND hListMechUnits;
    // Static label above mechanical units list
    HWND hStaticMechUnitsLabel;
    // Static label below mechanical units list for MMF shared memory section
    // Cached last adapters JSON to avoid repeated identical log posting
    std::string lastAdapterJson;
    // MMF shared memory UI moved into AutoWindow (controls removed from main window)
    // SDO operation buttons placed next to the parameter ListView
    HWND hBtnReadSDO;
    HWND hBtnWriteSDO;
    HWND hBtnWriteAllSDO; // 新增: 全部寫入按鈕
    HWND hBtnReadAllSDO; // 新增: 一鍵讀取所有 SDO 的按鈕
    HWND hBtnSaveParams; // 新增: 存檔按鈕（將 SDO 參數存為 ESI 格式 XML）
    HWND hBtnImportParams; // 新增: 匯入參數按鈕（從 XML 讀回 hex 欄）
    std::atomic<bool> readAllRunning{false}; // 防止重入
    void OnDumpESIForSelectedSlave(); // Function remains for server-triggered Dump ESI
    void OnReadAllSDO();
    void OnSaveParams();
    void OnImportParams();
    void OnWriteAllSDO();

    // Validate a user's input for SDO Value column according to ESI dataType/bitlen
    bool ValidateSDOValueInput(int itemIndex, const std::wstring &input, std::wstring &outError);

    
    // PDO 顯示管理器
    // NOTE: the display manager was renamed from `PDODisplayManager` to
    // `PDOProxy`. We keep a concrete `PDOProxy pdoDisplay` member to preserve
    // stable object layout for the main window. Other TUs should include
    // `PDOProxy.h` and reference `PDOProxy`.
    PDOProxy pdoDisplay;

    // Motion controller handles non-UI thread-safe motion operations
    // Use unique_ptr for RAII ownership
    std::unique_ptr<MotionController> motionController;

    // Unified GUI font for consistent appearance (supports CJK and Latin)
    HFONT hGuiFont = NULL;

    // Dark theme support
    bool isDarkTheme = false;
    HBRUSH hbrDarkBackground = NULL;
    // Brush for Immediate Stop button (red)
    HBRUSH hbrStopButton = NULL;
    // Brushes for Auto Start / Auto Stop buttons
    HBRUSH hbrAutoStartButton = NULL;
    HBRUSH hbrAutoStopButton = NULL;
    COLORREF clrDarkBackground = RGB(32, 32, 32);
    COLORREF clrDarkText = RGB(240, 240, 240);
    COLORREF clrDarkControl = RGB(45, 45, 45);
    COLORREF clrDarkBorder = RGB(80, 80, 80);

    // Scrolling support
    int contentHeight;  // Total height of all controls
    int scrollPos;      // Current scroll position
    bool statusUserScrolled; // true if user manually scrolled status box
    // True while user is interactively resizing/moving the window (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE)
    bool isInteractiveResize = false;
    // Tracks whether the current interactive resize/move session actually changed window size
    bool interactiveSizeChanged = false;
    // Throttle state for full redraws: if a redraw is pending, we will delay
    // the next full redraw to avoid multiple rapid repaints.
    bool redrawThrottlePending = false;
    unsigned long long lastFullRedrawTick = 0; // ms from GetTickCount64
    // Cache fixed heights so we can freeze certain list heights when window
    // is resized. If zero, the layout will compute heights dynamically as before.
    int cachedSlaveListHeight = 0; // if >0, keep slave list at this height on resize
    int cachedSDOListHeight = 0;   // if >0, keep SDO parameter list at this height on resize
    // Track maximum observed cycle time (microseconds) while connected
    uint32_t maxObservedCycleTime = 0;
    // Gate for "actual cycle" UI detection: true only after connect flow confirms
    // the default cyclic operation mode (0x6061) and PDO activity (WKC/PDO counters).
    std::atomic<bool> cyclicPdoCycleDetectReady{false};
    // When the UI timer is started we wait a short warm-up period before
    // beginning to record the maximum to avoid capturing transient large
    // values from the initialization phase. This is a tick (ms) target
    // expressed as GetTickCount64() absolute value (0 = disabled).
    unsigned long long maxObservedCollectStartTick = 0;
    // Warm-up delay in milliseconds before starting to collect max values
    // (reasonable default: 3000 ms). Can be adjusted if needed.
    uint32_t maxObservedCollectDelayMs = 3000;
    // Status logger manages in-memory buffer and on-disk log
    StatusLogger statusLogger;

    // Update the displayed HTTP server URL (reads HttpServer::GetHttpServerUrl())
    void UpdateHttpServerUrl();
    void OpenHttpOperationInterface(bool forceLogout, bool autoLoginDefaultPassword, bool preferMobileSizedWindow = false);
    void RequestAutomaticDisconnect(const char* logMessage, const wchar_t* statusText = nullptr);
    bool CheckAndHandleSustainedLinkLoss(ULONGLONG nowTick, const wchar_t* statusText);

    static const int TIMER_ID = 1;
    static const int TIMER_INTERVAL = 200; // ms (reduce UI CPU while keeping responsive updates)
    // Timer ID for throttled redraw
    static const int TIMER_REDRAW_THROTTLE = 7777;
    // Time window for throttled redraws (ms)
    static const int REDRAW_THROTTLE_MS = 100;
    // MMF diagnostics moved to AutoWindow
    // Jog-related timer and state
    static const int TIMER_JOG_POLL = 2; // ms timer id for jog monitoring
    static const int TIMER_JOG_POLL_INTERVAL_MS = 50; // poll interval while BUSY
    std::atomic<bool> jogBusy{false}; // UI-managed BUSY flag for jog operations
    std::atomic<bool> jogButtonPressed{false}; // true if jog button was pressed and started
    // New: true if user pressed the jog button (press event seen) even if
    // the motion start is still in progress. This allows release to interrupt
    // a start that hasn't completed yet.
    std::atomic<bool> jogPressRequested{false};

    // Background worker thread used for long-running connect/initialize
    std::atomic<bool> workerRunning;
    // Prevent concurrent connect attempts; used to avoid overlapping OnConnect calls
    std::atomic<bool> connectAttemptInProgress{false};
    // Monotonic id for connect attempts for logging / diagnostics
    std::atomic<uint64_t> connectAttemptId{0};
    // Soft-start sound control flag (default false). Use atomic to allow
    // concurrent access from worker thread without extra locking.
    std::atomic<bool> playSoftStartSound{true};
    std::atomic<bool> disconnecting{false};  // Prevent duplicate disconnect
    std::atomic<bool> homingBusy{false}; // true when a homing sequence is running
    std::atomic<bool> autoDownToOriginRunning{false};
    // Was the last Auto Down-To-Origin workflow completed successfully?
    std::atomic<bool> autoDownToOriginLastSuccess{false};
    // Flag: set when EtherCAT master suffers an error or disconnect to force
    // AutoWindow's `oEtherCatOk` to false even if master remains partially initialized.
    std::atomic<bool> ethercatError{false};
    // UI-managed BUSY flags for other controller operations
    std::atomic<bool> faultResetBusy{false}; // true while FaultResetAsync / FaultResetAll in progress
    std::atomic<bool> immediateStopBusy{false}; // true while ImmediateStopAsync/ImmediateStopAll in progress
    std::atomic<bool> powerOpBusy{false}; // true while PowerOn/PowerOff async operations are running
    std::atomic<bool> manualResetRequired{false}; // true after stop until Fault Reset succeeds
    std::atomic<bool> autoImmediateStopLampLatched{false};
    std::atomic<bool> autoEmergencyStopLampLatched{false};
    // Prevent concurrent Auto Connect / AutoWindow Connect background runs
    std::atomic<bool> autoConnectRunning{false};
    // Short-lived request latches used before the UI-thread handlers flip the
    // real busy/running flags, preventing duplicate button presses or external
    // triggers from queueing the same workflow repeatedly.
    std::atomic<bool> autoConnectRequestPending{false};
    std::atomic<bool> autoFaultResetRequestPending{false};
    std::atomic<bool> autoDownToOriginRequestPending{false};
    // Busy flag specifically for the Auto Connect (soft-start) workflow.
    // When true, `oBusy` will be true so the UI locks operations until
    // the Auto Connect either completes successfully or fails.
    std::atomic<bool> autoConnectBusy{false};
    // Request cancellation of the Auto Connect (soft-start) background workflow.
    // Set by Immediate Stop handler so the Auto Connect soft-start loop can abort
    // immediately (stop sound, stop polling) and allow stop handling to proceed.
    std::atomic<bool> autoConnectAbortRequested{false};
    // Indicates that the higher-level Auto Procedure (full auto workflow)
    // is currently running. This is used to drive the AutoWindow oBusy flag
    // consistently while the auto procedure is active.
    std::atomic<bool> autoProcedureRunning{false};
    // Latches the UI request while StartAutoProcedure is dispatching its
    // background bootstrap thread so repeated button presses cannot queue
    // duplicate auto-procedure starts before autoProcedureRunning flips true.
    std::atomic<bool> autoProcedureStartPending{false};
    // Request cancellation of the Auto Down-To-Origin background workflow.
    // Set by Immediate Stop handler so the workflow can abort cleanly.
    std::atomic<bool> autoDownToOriginAbortRequested{false};
    // Shared stop-then-home sequencing for web/external down-to-origin requests
    // that arrive while PDO filtering is still online.
    bool autoDownToOriginPendingAfterStop{false};
    HWND autoDownToOriginPendingRequester{NULL};
    // 連線成功後是否自動觸發自動程序（由啟動流程控制）
    // 由 TriggerAutoStartupSequence() 設定；連線完成後檢查並執行
    std::atomic<bool> autoStartAfterConnect{false};
    std::atomic<bool> suppressAutoWindowForStartupAuto{false};
    // Close request handling: if user attempts to close while EtherCAT is running
    // we perform a safe disconnect first and only close the window afterwards.
    std::atomic<bool> closePending{false};
    // Currently used pcap interface name (UTF-8). Populated on connect, cleared on disconnect.
    std::string currentIfName;
    // Current native adapter combobox mapping: combo item data -> pcap name.
    std::vector<std::string> adapterComboPcapNames;
    ULONGLONG linkDownSinceTick = 0;
    bool linkLossDisconnectRequested = false;
    static constexpr ULONGLONG LINK_LOSS_DISCONNECT_DELAY_MS = 1000;
    // Adapter selected from web UI (separate from `currentIfName` which is the PC's selection)
    std::string webSelectedIfName;
    // Watchdog/recovery support
    std::atomic<int> watchdogCounter{0};        // current consecutive watchdog hits
    int watchdogThreshold = 3;                  // number of consecutive hits before recovery
    std::atomic<bool> recoveryRunning{false};
    std::thread recoveryThread;
    void StartWatchdogRecovery();
    // Persistent global error detection: count consecutive OnTimer ticks where the
    // master reports a global error before auto-disconnect is triggered (to avoid falses)
    std::atomic<int> globalErrorCounter{0};
    int globalErrorThreshold = 5; // number of consecutive timer ticks (100ms each) before auto-disconnect

    // 0x603F Error Code 循環檢查機制（每50ms檢查一個從站）
    std::atomic<bool> errorCodeCheckRunning{false};
    std::atomic<int> currentErrorCheckSlaveIndex{1}; // 當前檢查的從站索引
    std::atomic<bool> anySlaveHasError{false};       // 是否有任何從站報告錯誤
    // Indicates presence of non-zero 0x603F internal error code on any slave.
    // This is used to reflect errors in the UI (AutoWindow oError) but
    // does NOT imply a communications failure and therefore should NOT
    // by itself trigger automatic disconnects. See StartErrorCodeCheck().
    std::atomic<bool> anySlaveHasInternalError{false};
    std::map<int, uint16_t> lastReportedErrorCode;   // 記錄每個從站上次報告的錯誤碼
    std::thread errorCodeCheckThread;
    std::mutex errorCodeStateMutex;
    std::mutex errorCodeCheckWakeMutex;
    std::condition_variable errorCodeCheckWakeCv;
    void StartErrorCodeCheck();
    void StopErrorCodeCheck();
    // per-slave consecutive 0x603F readings counters (to avoid transient readings setting global error)
    std::map<int, int> errorCodeConsecutiveCounter;
    int errorCodeConsecutiveThreshold = 3; // defaults: require 3 consecutive non-zero reads to mark persistent error

    // 自動程序按鈕現在透過 EtherCATMaster 直接啟/停 SDO 讀取
    // Start the higher-level auto procedure.
    // If showAutoWindow==true the PC AutoWindow will be created/activated
    // and receive UI enable/disable messages. When started remotely (from
    // web/HTTP), callers should pass false to avoid popping the AutoWindow.
    void StartAutoProcedure(bool showAutoWindow = true); // 啟動並接管 UI 切換
    void StopAutoProcedure();  // 停止並接管 UI 切換
    // When true, AutoWindow (or external inputs) must NOT be allowed to stop
    // the auto procedure; only main-window StopAutoProcedure() may stop it.
    void SetAutoStopLockedToMainWindow(bool locked) { autoStopLockedToMainWindow = locked; }
    bool IsAutoStopLockedToMainWindow() const { return autoStopLockedToMainWindow; }

    // Query whether the higher-level auto procedure is currently running.
    bool IsAutoProcedureRunning() const;
    // Auto-procedure stop follows the shared oBusy rule: once the auto flow is
    // active or latched for reset, Stop Auto remains disabled while the system
    // is busy and becomes available again only after oBusy clears.
    bool CanExecuteAutoProcedureStopRequest() const;

    // Query whether a short-lived worker (connect/disconnect/setup) is running.
    // Exposed so external threads (HTTP handler) can decide whether to allow
    // a web-initiated disconnect to cancel an in-progress connect.
    bool IsWorkerRunning() const;
    // True when a connect attempt has been started but not yet completed/cleared.
    bool IsConnectAttemptInProgress() const;
    // Check if any transient busy flag is set (disconnecting, homing, auto downs, jog, fault reset,
    // immediate stop, power op). Used to determine overall busy state and to check if connect is
    // the only active operation. The optional ignored flag lets a caller exclude its own just-latched
    // busy bit while still checking for conflicting work.
    bool HasAnyTransientBusyFlag(const std::atomic<bool>* ignoredFlag = nullptr) const;
    // Return true when the only busy condition is the connect worker.
    // Used by HTTP handler to allow interrupting an in-progress connect
    // (the PC Disconnect button already permits this), while still
    // preventing disconnects during auto procedures or active slave threads.
    bool IsBusyOnlyBecauseConnect() const;
    // Web-specific disconnect safety check. Returns true when a web client
    // is allowed to request a disconnect. This mirrors PC behavior: allow
    // interrupting an in-progress connect, but still disallow disconnects
    // during auto procedures or when cyclic slave threads are active.
    bool IsSafeToDisconnectForWeb() const;

    // Cache latest text typed in each PDO edit control (keyed by control ID 5000+idx)
    std::map<int, std::wstring> pdoEditTextCache;
    // Global cache keyed by PDO item name; values persist across slave switching
    std::unordered_map<std::wstring, std::wstring> globalPDOInputByName;

    // Selected slave state: confirmedSlave is applied for display; pending selection in combobox
    int confirmedSlave = -1; // 1-based slave index that was confirmed by user
    int lastReportedSelectedSlave = -1; // avoid spamming selected-slave status repeatedly

    // If true, keep the window HWND_TOPMOST and re-assert when Z-order changes
    bool alwaysOnTopEnabled = true;

    // If true, only the main window's StopAutoProcedure() may stop the auto procedure.
    // When set, AutoWindow requests or external inputs (iStop/iEstop) should not
    // trigger StopAutoProcedure(). Defaults to false.
    bool autoStopLockedToMainWindow{false};

    // Helper used by background thread to post UTF-8 logs to UI thread
    void PostUTF8LogToUI(std::string utf8);
    bool IsSimulationModeSelectedForMessages() const;
    bool IsManualResetRequired() const;
    void SetManualResetRequired(bool required);
    void SetAutoStopLampLatched(bool immediateStop, bool emergencyStop);
    void RequestMotionUiRefresh();
    void RefreshConfirmedSlaveControlState(bool rebuildPdoControls);
    void BeginAutoProcedureStartupUiBatch();
    void EndAutoProcedureStartupUiBatch(bool forceMainWindowRefresh = false);
    void BeginMainWindowVisualRefreshBatch();
    void EndMainWindowVisualRefreshBatch();
    std::atomic<uint64_t> statusLogEpoch_{1};
    std::atomic<int> autoProcedureStartupUiBatchDepth_{0};
    std::atomic<bool> autoProcedureStartupMainWindowRefreshPending_{false};
    unsigned int mainWindowVisualRefreshBatchDepth_ = 0;
    // Helper: check selected slave 0x6041 statusword bit3 and enable/disable PDO edits/buttons
    void ApplyStatuswordDisableToPDOControls(int slaveIndex);
    // Helper: safely enable/disable a child control (null/iswindow-checked)
    // Use this instead of calling EnableWindow(...) directly from multiple places.
    // Inline, small wrapper defined in-class to avoid separate CPP placement.
    inline void SetControlEnabled(HWND hCtrl, bool enable) {
        if (!hCtrl) return;
        if (!IsWindow(hCtrl)) return;
        const bool currentlyEnabled = (::IsWindowEnabled(hCtrl) != FALSE);
        if (currentlyEnabled == enable) return;
        ::EnableWindow(hCtrl, enable ? TRUE : FALSE);
    }
    // (moved to public section)
    // Thin wrappers around EtherCATMaster to avoid including heavy headers in Logic.cpp
    bool ReadDeviceNameUtf8(std::string &outUtf8);
    bool GetPDODataSnapshotWrapper(int slaveIndex, std::vector<uint8_t>& snapshot);
    int SnapshotIOMapWrapper(int offset, uint8_t* outBuf, int len);
    bool GetSlaveIOBaseBytesWrapper(int slaveIndex, int &outBase, int &inBase);
    // Additional master wrappers for connection lifecycle (used by Logic.cpp)
    void MasterSetCycleTime(uint32_t microseconds);
    void MasterSetAutoDisconnectOnNetworkFailure(bool enable, int threshold = 5);
    bool MasterIsAutoDisconnectOnNetworkFailureEnabled() const;
    int MasterGetNetworkFailureThreshold() const;
    bool MasterInitialize(const std::string &ifnameUtf8);
    std::string MasterGetLastError() const;
    bool MasterConfigureSlaves();
    // SDO wrappers so Logic.cpp can avoid including EtherCATMaster.h
    // MasterReadSDO - timeoutUs in microseconds (default uses EC_TIMEOUTRXM)
    // timeoutUs: microseconds; if negative, use default EC_TIMEOUTRXM inside master
    bool MasterReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int &size, int timeoutUs = -1);
    bool MasterWriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int size);
    int MasterGetSlaveCount() const;
    void MasterSendProcessDataOnce();
    bool MasterStartCyclicOperation();
    bool MasterHasActiveSlaveThreads() const;
    bool MasterHasGlobalError() const;
    void MasterShutdownWithCallback(const std::function<void(const std::string&)> &cb);
    void MasterShutdownSync();
    void MasterResetMaxObservedCommsTime();
    void MasterStopCyclicOperation();
    void MasterForceSlaveOperational(int slaveIndex, std::string &outLog);
    bool MasterSendMailbox(uint16_t slave, uint8_t* data, int size);
    // Expose MMF1 accessors publicly so modeless AutoWindow can query them
public:
    std::array<float,6> MasterGetMMF1Latest();
    void MasterGetMMF1LatestWithVelocities(std::array<float,6>& outValues, std::array<double,6>& outVelocities);
    bool MasterMMF1IsMapped();
    bool MasterMMF1GetRawSnapshot(std::vector<uint8_t> &out);
    // Access the aggregated in-memory status buffer (thread-safe)
    std::wstring GetStatusBuffer();
    std::wstring GetStatusBufferChronological();
    // Query whether the status view has been manually scrolled by the user
    bool IsStatusUserScrolled() const;
    // Notify the window that the status UI has processed a pending batch update
    void MarkStatusUiProcessed();
    uint64_t MasterMMF1GetAndResetUpdateCount();
    uint64_t MasterMMF1GetAndResetReadCount();
    uint64_t MasterMMF1GetAndResetNonZeroReadCount();
    // Mechanical unit metrics (target/actual position & speed) for a slave
    bool MasterGetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& out);
    // Control whether soft-start should play an audible looping sound.
    // Default: false (no sound) to avoid driver/DPC interference with real-time cycles.
    void SetPlaySoftStartSound(bool enable);
    bool GetPlaySoftStartSound() const;
    // Public wrappers to allow external TUs to query master/PD O status
    int MasterGetSlaveCountPublic() const;
    // Read 0x6041 (statusword) from the PDO snapshot for a slave.
    // Returns true if the call succeeded (snapshot available). If succeeded,
    // `hasValue` indicates whether a statusword value was decoded; the
    // decoded value is written to outVal.
    bool MasterGetStatusword6041(int slaveIndex, uint32_t &outVal, bool &hasValue);    // Check if all slaves have the ready statusword (0x1637u, indicating CiA402 Operation Enabled).
    // Returns true if all slaves report the expected statusword, false otherwise.
    // Thread-safe; may be called from any context once PDO snapshot is available.
    bool AreAllSlavesReadyByStatusword() const;private:
    // Additional master helper used by Logic.cpp
    bool MasterDumpESI(uint16_t slave, std::string &dump);
    // Additional getters for Logic.cpp
    bool MasterIsRunning() const;
    uint32_t MasterGetCommsTime() const;
    uint32_t MasterGetMaxObservedCommsTime() const;
    uint32_t MasterGetCycleTime() const;
    // Return the last measured actual cycle time (microseconds)
    uint32_t MasterGetActualCycleTime() const;
    // Return the observed cycle-to-cycle period (microseconds)
    uint32_t MasterGetObservedCycleMicros() const;
    // Return the max observed cycle-to-cycle period (microseconds)
    uint32_t MasterGetMaxObservedCycleMicros() const;
    void MasterResetMaxObservedCycleMicros();
    uint32_t MasterGetWorkingCounter() const;
    uint64_t MasterGetPDOSentCount() const;
    double MasterGetPDOFrequencyHz() const;
    double MasterGetMMF1UpdateFrequencyHz() const;
    int MasterGetSlaveOutputBytes(int slaveIndex) const; // returns Obytes from SOEM context; -1 if unknown
    bool MasterDetectWatchdogCondition() const; // true if any slave ALstate=0x14 or WKC==0
    // Child status window for low-level messages (modeless)
    HWND hChildStatusWnd = NULL;    // modeless child/status window
    HWND hChildStatusEdit = NULL;   // edit control inside child window
    // Modeless Auto Window handle (created/destroyed via ToggleAutoWindow)
    HWND hAutoWindow = NULL;
    // Content pane for scrollable UI (all main controls are children of this)
    HWND hContentWnd = NULL;
    // Tray (notification area) support
    bool useTray = true; // enable minimize-to-tray behaviour by default
    UINT WM_TRAYICON = 0; // registered message id for tray notifications
    NOTIFYICONDATAW nid{};
    bool minimizedToTray = false; // true when window is hidden to tray
    bool isMinimizedState = false; // true when window is minimized (taskbar or tray)
    // If true, keep running timer-driven logic and non-UI actions even when window is minimized
    // (used to make main window's background logic identical in both visible and minimized states)
    bool forceRunWhenMinimized = true;
    HICON hTrayIcon = NULL; // loaded tray icon (from AIRWOLF.png if available)
    void AddTrayIcon();
    void RemoveTrayIcon();
    // Toggle the modeless Auto Window (create if not present, destroy if present)
    void ToggleAutoWindow();
public:
    void ToggleChildStatusWindow(); // show/hide child status window (public for AutoWindow access)
    // Hide window to system tray (for startup minimized mode)
    void MinimizeToTray();
    // 啟動序列：連線成功且啟用設定時，觸發自動程序（避免重複觸發）
    // 由啟動設定與連線流程共同控制
    void TriggerAutoStartupSequence(bool suppressPcAutoWindow = false);
    // Allow external toggling of behavior (for testing or config). Default true.
    void SetForceRunWhenMinimized(bool enable) { forceRunWhenMinimized = enable; }
private:

    // Notify the UI that an EtherCAT error occurred/cleared and post update
    // to the AutoWindow so status lamps refresh immediately.
    void NotifyEthercatError(bool active, const std::string& reasonOverride = std::string());
    std::string BuildEthercatErrorReasonSummary();
    
    static constexpr int SLAVE_LIST_VISIBLE_ROWS = 6;
    int CalculateSlaveListHeightForRows(int visibleRows) const;
    

#endif // MAIN_WINDOW_INTERNAL_H
