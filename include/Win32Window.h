#ifndef WIN32_WINDOW_H
#define WIN32_WINDOW_H

// 必須按照特定順序引入標頭檔，避免衝突
#define WIN32_LEAN_AND_MEAN
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
#include <map>
#include <unordered_map>
#include "EtherCATMaster.h"
#include "EsiParser.h"
#include "StatusLogger.h"
#include <thread>
#include <atomic>
#include <map>
#include "MotionController.h"

// Custom message for worker thread to post UTF-8 logs back to UI
#define WM_USER_POST_UTF8_LOG (WM_USER + 101)

// --- PDODisplay content moved from include/PDODisplay.h BEGIN ---
// Constants for layout
const int GROUPBOX_INNER_TOP = 20;
const int ROW_HEIGHT = 25;
const int ROW_SPACING = 5;
const int bottomPadding = 10;
const int minGroupHeight = 100;

// EtherCAT PDO 配置結構
struct EtherCATPDOEntry {
    bool isOutput;
    bool isEditable;
    std::wstring name;
    std::wstring dataType;
    std::wstring unit;
    std::wstring minValue;
    std::wstring maxValue;
    std::wstring defaultValue;
    std::wstring currentValue;
};

struct EtherCATPDOConfig {
    std::vector<EtherCATPDOEntry> entries;
};

// PDO項目結構 (單一從站的單一PDO項)
struct PDOItem {
    std::wstring name;        // 顯示名稱 (如 "Control Word")
    uint16_t index;           // 對象字典索引 (如 0x6040)
    uint8_t subindex;         // 子索引
    uint8_t bitLength;        // 位長度 (16, 32)
    bool isOutput;            // true=RxPDO(輸出), false=TxPDO(輸入)
    int byteOffset;           // 在IOmap中的字節偏移
    bool isEditable;          // 用戶是否可編輯
    
    // 當前值 (UI顯示用)
    std::wstring currentValue;
    std::wstring displayValue;  // 格式化後的顯示字串
    
    // 控件句柄
    HWND hLabel;
    HWND hValue;
    HWND hEdit;               // 可編輯項目的輸入框
    HWND hButton;             // 應用按鈕
    HWND hAddress;            // 地址欄位 (e.g. 0x6040:00)
    HWND hTooltip;            // tooltip handle for description
    HWND hDesc;               // description static control

    // 新增的成員
    std::wstring dataType;
    std::wstring unit;
    std::wstring minValue;
    std::wstring maxValue;
    std::wstring defaultValue;
    // 新增: 地址與敘述
    std::wstring address;     // 格式化地址字串 (e.g. "0x6040:00")
    std::wstring description; // 來自 ESI 的敘述文字
    // Recently written value tracking to avoid UI being immediately overwritten by IO snapshot
    std::chrono::steady_clock::time_point lastWriteTime; // timestamp of last successful write
    int32_t lastWrittenInt = 0; // numeric value written (LE parsed)
    bool hasRecentWrite = false; // true if within grace period after a write
};

// PDO顯示管理器
class PDODisplayManager {
public:
    PDODisplayManager();
    ~PDODisplayManager();
    
    // 初始化 (創建UI控件)
    void Initialize(HWND parent, HINSTANCE hInstance, HFONT hFont, 
                   int startY, int leftMargin, int width, int height = 260);
    
    // 依據實際裝置的 PDO 映射 (透過 SDO 0x1C12/0x1C13 與 0x160x/0x1A0x) 重新計算 byteOffset
    // 注意: 此函數會調用 SDO 讀取,必須在 UI 執行緒中調用,不可在 EtherCAT 執行緒中調用
    // 回傳 true 代表更新成功；false 代表保留原本的預設偏移
    bool RecalculateOffsetsFromSDO(class EtherCATMaster* master, int slaveIndex);
    
    // 寫入PDO數據 (用戶編輯後寫回IOmap)
    // 使用 EtherCATMaster 的 thread-safe WriteOutputBytes API,不會阻塞 EtherCAT 執行緒
    bool WriteToIOMap(class EtherCATMaster* master, int slaveIndex, const std::wstring& itemName, int32_t value);
    
    // 獲取可編輯項目列表 (用於 Win32Window 內聯處理)
    std::vector<std::wstring> GetEditableItems() const;
    
    // 顯示/隱藏
    void Show(bool visible);
    
    // 清理
    void Clear();

    // 調整寬度（自適應窗體大小時呼叫）
    void ResizeWidth(int width);
    // 調整高度（動態高度分配時呼叫）
    void ResizeHeight(int height);
    // 設定群組框的頂端 Y 座標（允許外部根據其他控件重新定位）
    void SetTop(int top);
    // 回傳群組框的高度供佈局使用
    int GetHeight() const;
    
    // Public access to PDO items for Win32Window inline updates
    std::vector<PDOItem> pdoItems;
    
    // Public method to rebuild UI controls (called from Win32Window)
    void RebuildPDOControls(); // 重建所有 PDO UI 控制項
    
    // Public access to group box for parent window operations
    HWND GetGroupBox() const { return hGroupBox; }

    // 新增的成員函數
    void UpdateItemValue(size_t index, const std::wstring &newValue);
    void SetItemEditable(size_t index, bool editable);
    void SetItemButtonText(size_t index, const std::wstring &btnText);
    void SelectSlave(int slaveIndex);
    void BuildPDOItemsFromSDO(const struct EtherCATPDOConfig &pdoConfig);
    void RefreshItemValues(const std::vector<std::wstring> &newValues);
    void SetItemUnit(size_t index, const std::wstring &unit);
    void SetItemMinMax(size_t index, const std::wstring &minVal, const std::wstring &maxVal);
    void SetItemDefault(size_t index, const std::wstring &defVal);
    void SetItemDataType(size_t index, const std::wstring &dataType);
    void EnableItem(size_t index, bool enable);
    void DisableAllItems();
    void EnableAllItems();
    void SetItemColor(size_t index, COLORREF color);
    void SetItemBackgroundColor(size_t index, COLORREF bgColor);
    void ResetItemColors();
    void UpdateItemDisplay(size_t index);
    void RefreshDisplay();
    void SetParent(HWND parent);
    void SetFont(HFONT hFont);
    void SetGroupBoxText(const std::wstring &text);
    void GetItemRect(size_t index, RECT &rect);
    void GetGroupBoxRect(RECT &rect);
    void SetItemPosition(size_t index, int x, int y);
    void MoveItem(size_t index, int dx, int dy);
    void ResizeItem(size_t index, int newWidth, int newHeight);
    void SetItemRange(size_t index, const std::wstring &minVal, const std::wstring &maxVal);
    void SetItemValue(size_t index, const std::wstring &value);
    void SetItemName(size_t index, const std::wstring &name);
    
private:
    void CreatePDOItem(HWND parent, HINSTANCE hInst, HFONT font,
                      PDOItem& item, int itemIndex, int y, int leftMargin, int width, 
                      const std::wstring& initialValue = L"0");
    std::wstring FormatValue(int32_t value, uint8_t bitLength);
    HWND hParent;
    HWND hGroupBox;           // 分組框
    HWND hOutputHeader;       // 左欄：輸出 (RxPDO)
    HWND hInputHeader;        // 右欄：輸入 (TxPDO)
    int currentY;
    bool isInitialized;
    int selectedSlave; // 目前顯示的從站 (1-based), -1 表示未指定
    int lastMappedSlaveOffsets = -1; // 用於避免重複印出映射資訊
    // Cache last IO dump per slave to avoid spamming status box when unchanged
    std::unordered_map<int, std::wstring> lastIODump;

    // Layout cache for adaptive resizing
    int groupLeftMargin = 0;
    int groupWidth = 0;
    int groupHeight = 0;
};

// --- PDODisplay content moved from include/PDODisplay.h END ---

class Win32Window {
public:
    Win32Window();
    ~Win32Window();

    bool Create(HINSTANCE hInstance);
    void Show(int nCmdShow);
    int MessageLoop();

    // Accessor for window handle (needed by subclass procs and external helpers)
    HWND GetHwnd() const { return hwnd; }
    // Accessor for SDO ListView handle so subclass procs can check target safely
    HWND GetHListSDOParams() const { return hListSDOParams; }

    // Fault reset handler: exposed public so subclass proc can call into instance
    void OnFaultReset(bool assertFlag);
    void OnFaultResetAsync(int slaveIdx, bool assertFlag);
    // Immediate stop handler (Quick Stop): public so subclass proc can call
    void OnImmediateStop(bool assertFlag);
    void OnImmediateStopAsync(int slaveIdx, bool assertFlag);
    // Power On handler: press -> set controlword bits 0..3 via PDO staging first, SDO fallback
    void OnPowerOn(bool assertFlag);
    void OnPowerOnAsync(int slaveIdx, bool assertFlag);
    // Power Off handler: press -> write controlword buffer = 0x0006 via PDO staging only
    void OnPowerOff(bool assertFlag);
    void OnPowerOffAsync(int slaveIdx, bool assertFlag);
    // Home (Homing) handler: press -> perform homing sequence via SDO+PDO
    void OnHome(bool assertFlag);
    void OnHomeAsync(int slaveIdx, bool assertFlag);

    void SetEtherCATMaster(EtherCATMaster* master) { 
        etherCATMaster = master; 
        if (motionController) motionController->SetMaster(master);
    }
    void UpdateStatus();
    void PrependStatus(const wchar_t* text);
    // Append to status UI only (do NOT write to on-disk log). Used for frequent
    // UI-only messages that shouldn't bloat the status.log file.
    void PrependStatusNoFile(const wchar_t* text);
    void MarkStatusScrolled();
    // Handle SDO ListView scroll (after default processing) to keep in-place editor aligned
    void HandleSDOListScroll();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, 
                                       WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam);

    // UI 處理函數
    void CreateControls();
    void OnConnect();
    void OnDisconnect();
    void CompleteDisconnect();
    void OnReadSDO();
    void OnWriteSDO();
    void OnSendMailbox();
    void UpdateSlaveList();
    void UpdateStatusText();
    void UpdateSelectedSlaveStatus(int slaveIdx);  // 更新選定從站的狀態顯示
    void OnTimer();
    void PopulateAdapters();
    // ESI handling
    void PopulateESIFiles();
    void PopulateObjectsFromESI(const std::wstring& esiPathW);
    void OnBrowseESI();
    void ReflowSDOControls(int dynamicHeight = 200);
    // Adaptive reflow for main controls (combos, list, mailbox, status)
    void ReflowMainControls();
    // Lightweight: only update content pane position/size on scroll without reflow/rebuild
    void UpdateContentPaneScrollOnly();
    // Diagnostic: log current SDO control positions to status box
    void LogSDOControlPositions();

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

    // In-place editor state exposed so subclass procs can align/commit edits
    HWND hInplaceEdit = NULL;
    int inplaceEditItem = -1; // currently editing ListView item index
    int inplaceEditSubitem = 5; // currently editing subitem (default to Value column)
    std::wstring inplaceEditOriginalText; // original text before in-place edit


private:
    HWND hwnd;
    HINSTANCE hInstance;
    EtherCATMaster* etherCATMaster;

    // 控制項句柄
    HWND hComboAdapter;
    HWND hComboSlaves; // 新增: 從站下拉選單（依從站數量填充）
    HWND hBtnConnect;
    HWND hBtnDisconnect;
    HWND hListSlaves;
    // SDO: 使用參數讀寫表格取代原先的下拉選單與整列操作區
    HWND hListSDOParams; // ListView for SDO parameter read/write (替代 hComboSDO + SDO 操作列)
    HWND hEditMailbox;
    HWND hBtnSendMailbox;
    HWND hEditCycleTime;        // Desired cycle time (editable)
    HWND hEditCurrentCycleTime; // Current cycle time (display only)
    HWND hEditWKC;              // Working Counter (display only)
    HWND hStaticSDOReadResult;  // Separate read-only display for SDO read results
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
    // ESI UI controls
    HWND hComboESI;
    HWND hBtnBrowseESI;
    HWND hBtnDumpESI; // Dump ESI/EEPROM button
    HWND hBtnClearStatus; // 狀態清除 按鈕（清除狀態訊息並截斷日誌）

    // Motion control UI controls
    HWND hBtnFollowMode;    // 跟隨模式
    HWND hBtnPowerOn;       // 電源啟動
    HWND hBtnPowerOff;      // 電源關閉
    HWND hBtnHome;          // 回原點
    HWND hBtnJogCW;         // 寸動正轉
    HWND hBtnJogCCW;        // 寸動反轉
    HWND hBtnFaultReset;    // 異常重置
    HWND hBtnImmediateStop; // 立即停止（寬 2 倍）
    HWND hBtnManualMode;    // 手動模式
    HWND hBtnAutoMode;      // 自動模式
    // SDO operation buttons placed next to the parameter ListView
    HWND hBtnReadSDO;
    HWND hBtnWriteSDO;
    HWND hBtnWriteAllSDO; // 新增: 全部寫入按鈕
    HWND hBtnReadAllSDO; // 新增: 一鍵讀取所有 SDO 的按鈕
    HWND hBtnSaveParams; // 新增: 存檔按鈕（將 SDO 參數存為 ESI 格式 XML）
    HWND hBtnImportParams; // 新增: 匯入參數按鈕（從 XML 讀回 hex 欄）
    std::atomic<bool> readAllRunning{false}; // 防止重入
    void OnDumpESIForSelectedSlave();
    void OnClearStatus();
    void OnReadAllSDO();
    void OnSaveParams();
    void OnImportParams();
    void OnWriteAllSDO();

    // Validate a user's input for SDO Value column according to ESI dataType/bitlen
    bool ValidateSDOValueInput(int itemIndex, const std::wstring &input, std::wstring &outError);

    
    // PDO顯示管理器
    PDODisplayManager pdoDisplay;

    // Motion controller handles non-UI thread-safe motion operations
    MotionController* motionController = nullptr;

    // Unified GUI font for consistent appearance (supports CJK and Latin)
    HFONT hGuiFont = NULL;

    // Dark theme support
    bool isDarkTheme = false;
    HBRUSH hbrDarkBackground = NULL;
    // Brush for Immediate Stop button (red)
    HBRUSH hbrStopButton = NULL;
    COLORREF clrDarkBackground = RGB(32, 32, 32);
    COLORREF clrDarkText = RGB(240, 240, 240);
    COLORREF clrDarkControl = RGB(45, 45, 45);
    COLORREF clrDarkBorder = RGB(80, 80, 80);

    // Scrolling support
    int contentHeight;  // Total height of all controls
    int scrollPos;      // Current scroll position
    bool statusUserScrolled; // true if user manually scrolled status box
    // Status logger manages in-memory buffer and on-disk log
    StatusLogger statusLogger;

    static const int TIMER_ID = 1;
    static const int TIMER_INTERVAL = 100; // ms

    // Background worker thread used for long-running connect/initialize
    std::atomic<bool> workerRunning;
    std::atomic<bool> disconnecting{false};  // Prevent duplicate disconnect
    std::atomic<bool> homingBusy{false}; // true when a homing sequence is running
    // Close request handling: if user attempts to close while EtherCAT is running
    // we perform a safe disconnect first and only close the window afterwards.
    std::atomic<bool> closePending{false};
    // Currently used pcap interface name (UTF-8). Populated on connect, cleared on disconnect.
    std::string currentIfName;
    // Watchdog/recovery support
    std::atomic<int> watchdogCounter{0};        // current consecutive watchdog hits
    int watchdogThreshold = 3;                  // number of consecutive hits before recovery
    std::atomic<bool> recoveryRunning{false};
    std::thread recoveryThread;
    void StartWatchdogRecovery();

    // Cache latest text typed in each PDO edit control (keyed by control ID 5000+idx)
    std::map<int, std::wstring> pdoEditTextCache;
    // Global cache keyed by PDO item name; values persist across slave switching
    std::unordered_map<std::wstring, std::wstring> globalPDOInputByName;

    // Selected slave state: confirmedSlave is applied for display; pending selection in combobox
    int confirmedSlave = -1; // 1-based slave index that was confirmed by user
    int lastReportedSelectedSlave = -1; // avoid spamming selected-slave status repeatedly

    // Helper used by background thread to post UTF-8 logs to UI thread
    void PostUTF8LogToUI(const std::string &utf8);
    // Child status window for low-level messages (modeless)
    HWND hChildStatusWnd = NULL;    // modeless child/status window
    HWND hChildStatusEdit = NULL;   // edit control inside child window
    // Content pane for scrollable UI (all main controls are children of this)
    HWND hContentWnd = NULL;
    // Tray (notification area) support
    bool useTray = true; // enable minimize-to-tray behaviour by default
    UINT WM_TRAYICON = 0; // registered message id for tray notifications
    NOTIFYICONDATAW nid = {0};
    bool minimizedToTray = false; // true when window is hidden to tray
    HICON hTrayIcon = NULL; // loaded tray icon (from AIRWOLF.png if available)
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ToggleChildStatusWindow(); // show/hide child status window
    
};

#endif // WIN32_WINDOW_H
