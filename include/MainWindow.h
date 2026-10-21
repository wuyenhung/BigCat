#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

// 必須按照特定順序引入標頭檔，避免衝突
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Prevent Windows headers from defining min/max macros which conflict with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
// HAVE_REMOTE and WPCAP are defined in CMake compile definitions; avoid redefining them here
#define BPF_PROGRAM_DEFINED  // 防止 Packet32.h 重複定義 BPF 結構
#define BPF_INSN_DEFINED
#include <winsock2.h>
#include <windows.h>
// Shell API needed for NOTIFYICONDATA and Shell_NotifyIcon macros
#include <shellapi.h>

#include <string>
#include <vector>
#include <array>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <functional>
#include "UIConstants.h"
#include "GuardedString.h"
#include "EsiParser.h"
#include "StatusLogger.h"
#include "PDOFilter.h"
#include <thread>
#include <atomic>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include "MotionController.h"
#include "PDOProxy.h"
#include "MMFReader.h"
#include "core/SystemState.h"

struct PostedUtf8UiMessage {
    std::string text;
    uint64_t statusEpoch = 0;
    bool commandLike = false;
};

// Forward-declare MechanicalUnitMetrics to avoid heavy includes in this header
struct MechanicalUnitMetrics;
// AutoProcedure removed — 0x60FD monitoring now via PDO cache in EtherCATMaster

// Messages used for HTTP/server integration (avoid colliding with tray callback WM_APP+1)
#define WM_APP_GET_STATUS (WM_APP + 50)
#define WM_APP_SET_MAILBOX (WM_APP + 51)

// PDODisplay content moved to include/PDODisplayManager.h. Use PDOProxy alias
// (include/PDOProxy.h) to reference the display manager type in this header.

class MainWindow {
public:
    friend class AppController;

    MainWindow();
    ~MainWindow();

    bool Create(HINSTANCE hInstance, bool showInitially = true);
    void Show(int nCmdShow);
    int MessageLoop();

    // Make the main window stay on top (true=topmost, false=normal)
    void SetAlwaysOnTop(bool on);

    // Accessor for window handle (needed by subclass procs and external helpers)
    HWND GetHwnd() const { return hwnd; }
    // Front-end integration: set selected adapter (pcap ifname, UTF-8)
    // If isWeb==true, record a web-only selection (do NOT change combobox or PC visible selection).
    // Calls should log the selection for visibility (web-originated selections are logged).
    void SetSelectedAdapter(const std::string &ifname, bool isWeb = false);
    // Return the currently selected adapter as UTF-8 (empty if none)
    std::string GetSelectedAdapterUtf8() const;

    // Return the adapter selected by web clients (if any)
    std::string GetWebSelectedAdapterUtf8() const;
    // Return whether the adapter combobox is currently enabled
    bool IsAdapterComboEnabled() const;
    // Accessor for SDO ListView handle so subclass procs can check target safely
    HWND GetHListSDOParams() const { return hListSDOParams; }
    // Accessor for Slave ListView handle so subclass procs can check target safely
    HWND GetHListSlaves() const { return hListSlaves; }
    // Query whether the EtherCAT master is present and initialized
    bool IsMasterInitialized() const;
    // Lightweight accessor for last-read 0x60FD SDO bytes (thread-safe via EtherCATMaster)
    bool TryGetPdo60FD(std::vector<uint8_t>& out) const;
    bool TryGetPdo60FDLowByte(uint8_t& outLowByte) const;
    void GetLastReportedErrorCodes(std::array<uint16_t, 6>& outCodes,
                                   std::array<bool, 6>& outValid);
    // Check if it is safe to perform a disconnect now. Returns true when
    // there are no active EtherCAT cyclic threads, no ongoing busy workflows
    // and no higher-level auto procedure in progress.
    bool IsSafeToDisconnect() const;
    // Get diagnostic info about why disconnect is unsafe (if unsafe). Used for logging.
    std::string GetDisconnectUnsafeReason() const;
    // MMF1 control wrappers (exposed so modeless AutoWindow can invoke master ops)
    bool MasterStartMMF1In();
    void MasterStopMMF1In();
    bool MasterIsMMF1InRunning() const;

    // MMF UI actions are now handled by AutoWindow; main-window no longer exposes OnMMF handlers

    CoreHelpers::SystemStateSnapshot GetSystemStateSnapshot() const;

    CoreHelpers::AutoActionButtonInputs GetAutoActionButtonInputsForWeb() const;

    // Returns whether external E-Stop (0x60FD bit0) detection is enabled via INI config
    bool IsExternalEstopEnabled() const { return externalEstopEnabled_.load(); }

    // 取得目前 AutoWindow 需要的全域狀態旗標（暫定規格，可日後改成實際 PDO/SDO 來源）
    void GetAutoGlobalStatus(
        bool &oEtherCatOk,
        bool &oBusy,
        bool &oError,
        bool &oReady,
        bool &oHomeDone,
        bool &oOnline
    ) const;

    // Fault reset handler: exposed public so subclass proc can call into instance
    void OnFaultReset(bool assertFlag);
    // Immediate stop handler (Quick Stop): public so subclass proc can call
    void OnImmediateStop(bool assertFlag);
    // Power On handler: press -> set controlword bits 0..3 via PDO staging first, SDO fallback
    void OnPowerOn(bool assertFlag);
    // Power Off handler: press -> write controlword buffer = 0x0006 via PDO staging only
    void OnPowerOff(bool assertFlag);
    // Home (Homing) handler: press -> perform homing sequence via SDO+PDO
    void OnHome(bool assertFlag);

    // UI wrappers to start/stop jog actions. These manage UI BUSY and timers
    // and call into MotionController for SDO/PDO work. Kept public so subclass
    // procs can invoke without accessing private members.
    void StartJogPositiveUI();
    void StartJogNegativeUI();
    void StopJogUI();
    // Public helpers for jog button press/release events (encapsulate internal state)
    void OnJogButtonPressed();
    void OnJogButtonReleased();
    // Direction-specific public helpers so subclass proc can indicate source
    void OnJogCWPressed();
    void OnJogCWReleased();
    void OnJogCCWPressed();
    void OnJogCCWReleased();

    // Helpers for subclass procs to query whether a HWND belongs to CW/CCW jog buttons
    bool IsJogCWHandle(HWND h) const { return (h == hBtnJogCW); }
    bool IsJogCCWHandle(HWND h) const { return (h == hBtnJogCCW); }

    // Track last PDO violation/log state per slave to avoid spamming duplicate
    // messages when nothing changed. Protected by mutex because callbacks
    // may come from master worker thread.
    std::mutex pdoLogStateMutex_;
    // Store (violationType as integer, limiting flag) to avoid including PDOFilter.h here
    std::map<int, std::pair<int, bool>> lastPDOState_;
    std::map<int, bool> pdoSafetyStopRequested_;
    std::atomic<bool> lastGlobalLimiting_{false};
    // Whether the violation alert sound is currently playing
    std::atomic<bool> violationSoundPlaying_{false};
    // Whether the soft-start sound is currently playing
    std::atomic<bool> softStartSoundPlaying_{false};
    // Whether the error/emergency alert sound is currently active
    std::atomic<bool> errorSoundPlaying_{false};

    // === Aggregated input-anomaly logging (coalesce repeated messages) ===
    // Aggregates identical anomaly events and flushes once-per-second with counts.
    struct InputAnomalySummary {
        int count = 0;
        double lastOriginalSet = 0.0;
        double lastReplacedSet = 0.0;
        double lastInputChange = 0.0;
        double lastMaxAllowedChange = 0.0;
        std::chrono::steady_clock::time_point firstTime;
        std::chrono::steady_clock::time_point lastTime;
    };

    // Protects anomalySummaries_
    std::mutex anomalyMutex_;
    // Map: slaveIdx -> (reason -> summary)
    std::map<int, std::map<PDOFilter::InputAnomalyReason, InputAnomalySummary>> anomalySummaries_;
    std::atomic<bool> anomalyAggregatorRunning_{false};
    std::thread anomalyAggregatorThread_;

    // Helper to stop the system sound only when no component requests it
    void StopSystemAlertSoundIfIdle();

    void UpdateStatus();
    void PrependStatus(const wchar_t* text);
    // Append to status UI only (do NOT write to on-disk log). Used for frequent
    // UI-only messages that shouldn't bloat the status.log file.
    void PrependStatusNoFile(const wchar_t* text);
    void MarkStatusScrolled();
    void OnClearStatus();
    // Log-to-file control (status.log)
    void SetStatusLogEnabled(bool enabled);
    bool IsStatusLogEnabled() const;
    // Handle SDO ListView scroll (after default processing) to keep in-place editor aligned
    void HandleSDOListScroll();
    // Handle Motion ListView scroll to keep in-place editor aligned
    void HandleMotionListScroll();
    void OnAppGetStatus();

    // Public static helper: allow non-instance contexts (callbacks, server threads)
    // to post UTF-8 log strings to a specific HWND with the same timestamping logic
    // used by instance-level PostUTF8LogToUI. This avoids needing an instance
    // pointer in callbacks and centralizes timestamp prefix behavior.
    static void PostUTF8LogToHWND(HWND target, std::string utf8);
    // Post SDO ITEM/BATCH messages to UI thread using WM_USER_SDO_ITEM_READ.
    // This preserves the existing specialized handler which expects the
    // lParam to point to a heap-allocated std::string (caller/new alloc).
    static void PostSDOItemToHWND(HWND target, std::string utf8);

    

    #include "MainWindow_Internal.h"
};

#endif // MAIN_WINDOW_H
