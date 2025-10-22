#include "Win32Window.h"
#include <vector>
// Windows common controls (ListView, InitCommonControlsEx, SetWindowSubclass, etc.)
#include <commctrl.h>
#include <windowsx.h>
#include "../include/PDOHelpers.h"
// Common dialogs (OpenFileName)
#include <commdlg.h>
// Standard utilities used across this file
#include <sstream>
#include <iomanip>
#include <string>
#include <algorithm>
// (Icon resource is loaded from compiled resources)
static const int sideMargin = 0;
// Layout helper moved to include/LayoutHelper.h
#include "LayoutHelper.h"
// Non-UI utility functions (moved to Win32Utils.{h,cpp})
#include "Win32Utils.h"
// tinyxml2 for XML import/export
#include "../lib/tinyxml2/tinyxml2.h"

// Forward declare subclass proc so it can be used before its definition
LRESULT CALLBACK FaultResetButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declare ImmediateStop subclass proc
LRESULT CALLBACK ImmediateStopButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declare PowerOn subclass proc
LRESULT CALLBACK PowerOnButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declare PowerOff subclass proc
LRESULT CALLBACK PowerOffButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declare Home subclass proc
LRESULT CALLBACK HomeButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

// 控制項 ID (提前定義，供下面的 subclass callback 使用)
#define IDC_EDIT_ADAPTER        1001
#define IDC_BTN_CONNECT         1002
#define IDC_BTN_DISCONNECT      1003
#define IDC_LIST_SLAVES         1004
#define IDC_EDIT_STATUS         1005
#define IDC_EDIT_SDO_INDEX      1006
#define IDC_EDIT_SDO_SUBINDEX   1007
#define IDC_EDIT_SDO_DATA       1008
#define IDC_BTN_READ_SDO        1009
#define IDC_BTN_WRITE_SDO       1010
#define IDC_EDIT_MAILBOX        1011
#define IDC_BTN_SEND_MAILBOX    1012
#define IDC_EDIT_CYCLE_TIME     1013
#define IDC_EDIT_CURRENT_CYCLE_TIME 1014
#define IDC_EDIT_WKC            1015
// ESI controls
#define IDC_COMBO_ESI           1015
#define IDC_BTN_BROWSE_ESI      1016
#define IDC_COMBO_SDO           1017
// Dump ESI button
#define IDC_BTN_DUMP_ESI        1030
// Read All SDO button
#define IDC_BTN_READ_ALL_SDO    1031
// Save parameters (export to ESI-style XML)
#define IDC_BTN_SAVE_PARAMS     1035
// Import parameter file (XML)
#define IDC_BTN_IMPORT_PARAMS   1060
#define IDC_BTN_WRITE_ALL_SDO   1061
// PDO control IDs
#define IDC_CHECK_PDO_ENABLE    1020
#define IDC_BTN_PDO_START       1021
#define IDC_BTN_PDO_STOP        1022
#define IDC_EDIT_IOMAP_DISPLAY  1023
// New IDC for PDO_sent status toggle button
#define IDC_BTN_PDO_SENT_STATUS 1050

// Mailbox static label ID
#define IDC_STATIC_MAILBOX 1040
// Cycle-row static labels (for dynamic reflow)
#define IDC_STATIC_CYCLE_ACTUAL_LABEL 1080
#define IDC_STATIC_CYCLE_WKC_LABEL 1081
#define IDC_STATIC_PDO_SENT_LABEL 1082

// Motion control IDs
#define IDC_STATIC_MOTION_CONTROL 1100
#define IDC_BTN_FOLLOW_MODE 1101
#define IDC_BTN_POWER_ON 1102
#define IDC_BTN_POWER_OFF 1103
#define IDC_BTN_HOME 1104
#define IDC_BTN_JOG_CW 1105
#define IDC_BTN_JOG_CCW 1106
#define IDC_BTN_FAULT_RESET 1107
#define IDC_BTN_IMMEDIATE_STOP 1108
// Manual/Auto mode buttons
#define IDC_BTN_MANUAL_MODE 1110
#define IDC_BTN_AUTO_MODE 1111

// Worker -> UI: single SDO item read result
#define WM_USER_SDO_ITEM_READ (WM_USER + 110)
// Defer creating in-place editor to avoid ListView focus stealing
#define WM_USER_BEGIN_VALUE_EDIT (WM_USER + 120)
// One-shot timer to re-focus the in-place editor after ListView click completes
#define TIMER_EDIT_FOCUS 5555

// Subclass proc to log WM_CHAR for edit controls
// EditSubclassProc removed: previously used for logging WM_CHAR on edits but is unused now.

// Subclass proc for status box to detect user scrolling/interaction
// StatusSubclassProc removed: status scrolling handled via MarkStatusScrolled() and explicit hooks.

// pcap for automatic adapter enumeration (Npcap/WinPcap)
// Adapter enumeration implementation moved to Win32Utils.cpp where these
// platform/network headers are included to avoid polluting every file.

// Forward declaration for in-place edit subclass proc used before its definition


static LRESULT CALLBACK InplaceEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declaration: subclass for SDO ListView to capture mouse clicks reliably
static LRESULT CALLBACK SDOListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
// Forward declaration: subclass for content pane to forward commands to main window
static LRESULT CALLBACK ContentPaneSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

// Link libraries: iphlpapi is required (added in CMake)

#pragma comment(lib, "comctl32.lib")

// IsSystemDarkMode moved to Win32Utils.cpp

// Capture current PDO editable inputs (by item name) into global cache
void Win32Window::CaptureCurrentPDOInputsByName()
{
    int ecnt = 0;
            for (const auto &it : pdoDisplay.pdoItems) {
                if (!it.isEditable) continue;
                std::wstring name = it.name;
                std::wstring val;
                int ctrlId = (it.hEdit ? GetDlgCtrlID(it.hEdit) : -1);
                auto itc = pdoEditTextCache.find(ctrlId);
                if (itc != pdoEditTextCache.end()) {
                    val = itc->second;
                } else if (it.hEdit && IsWindow(it.hEdit)) {
                    wchar_t buf[64] = {0};
                    GetWindowTextW(it.hEdit, buf, (int)_countof(buf));
                    val = buf;
                } else {
                    val = L"0";
                }
                globalPDOInputByName[name] = val;
                ++ecnt;
            }
}

// Restore PDO editable inputs from global cache by item name
void Win32Window::RestorePDOInputsFromGlobal()
{
    for (auto &it : pdoDisplay.pdoItems) {
                if (!it.isEditable) continue;
                auto f = globalPDOInputByName.find(it.name);
                if (f != globalPDOInputByName.end()) {
                    const std::wstring &val = f->second;
                    if (it.hEdit && IsWindow(it.hEdit)) {
                        SetWindowTextW(it.hEdit, val.c_str());
                        int id = it.hEdit ? GetDlgCtrlID(it.hEdit) : -1;
                        if (id >= 5000 && id < 6100) {
                            pdoEditTextCache[id] = val;
                        }
                    }
    }
    // end for
}
}


Win32Window::Win32Window()
    : hwnd(NULL)
    , hInstance(NULL)
    , etherCATMaster(nullptr)
    , contentHeight(0)
    , scrollPos(0)
    , workerRunning(false)
{
    // hTrayIcon will be loaded from embedded resource (ICON id 101) in Create()
    // 初始化 ESI 容器
    currentObjects.clear();
    currentPdos.clear();
    // 初始化新增的 HWND 成員
    hEditPDOSent = NULL;
    hBtnPDOSentStatus = NULL;
    hChildStatusWnd = NULL;
    hChildStatusEdit = NULL;

    // 初始化暗色主題
    isDarkTheme = IsSystemDarkMode();
    if (isDarkTheme) {
        hbrDarkBackground = CreateSolidBrush(clrDarkBackground);
    }
    // 建立立即停止按鈕用的紅色 brush（明亮紅）
    hbrStopButton = CreateSolidBrush(RGB(220, 20, 20));
}

Win32Window::~Win32Window() {
    if (hGuiFont) {
        DeleteObject(hGuiFont);
        hGuiFont = NULL;
    }
    if (hbrDarkBackground) {
        DeleteObject(hbrDarkBackground);
        hbrDarkBackground = NULL;
    }
    if (hbrStopButton) {
        DeleteObject(hbrStopButton);
        hbrStopButton = NULL;
    }
    if (hChildStatusWnd && IsWindow(hChildStatusWnd)) {
        DestroyWindow(hChildStatusWnd);
        hChildStatusWnd = NULL;
        hChildStatusEdit = NULL;
    }
    // Remove tray icon and destroy loaded hTrayIcon
    if (hTrayIcon) {
        DestroyIcon(hTrayIcon);
        hTrayIcon = NULL;
    }
    // destroy motion controller if allocated
    if (motionController) {
        delete motionController;
        motionController = nullptr;
    }
}

bool Win32Window::Create(HINSTANCE hInst) {
    hInstance = hInst;

    // 初始化 Common Controls
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    // 註冊視窗類別
    WNDCLASSEX wc = { 0 };
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

    // 創建視窗
    // Build-time stamp in title
    char titleBuf[256];
    snprintf(titleBuf, sizeof(titleBuf), "BigCat 立洋科技 wuyenhung@gmail.com built: %s %s", __DATE__, __TIME__);
    // convert to wide
    std::wstring titleW = Utf8ToWide(std::string(titleBuf));

    // Create main window (default scrollbar on right side)
    hwnd = CreateWindowEx(
        WS_EX_WINDOWEDGE,
        L"EtherCATWindowClass",
        titleW.c_str(),
        WS_OVERLAPPEDWINDOW,  // 允許調整大小
        CW_USEDEFAULT, CW_USEDEFAULT,
        1024, 768,  // 初始視窗大小
        NULL, NULL, hInstance, this
    );

    if (!hwnd) {
        return false;
    }

    CreateControls();
    return true;
}

void Win32Window::CreateControls() {
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
    int contentWidth = (clientW - left - sideMargin) < 400 ? 400 : (clientW - left - sideMargin); // available space for content
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
    int slaveListHeight = (int)(availableContentHeight * 0.25f); // 25% for slave list
    // SDO table height: originally 35% of available content. Reduce to half as requested.
    int sdoTableHeight = (int)(availableContentHeight * 0.35f * 0.5f);  // ~17.5% for SDO table (halved)
    int pdoHeight = (int)(availableContentHeight * 0.22f);       // 22% for PDO display (reduced)
    int mailboxHeight = (int)(availableContentHeight * 0.12f);   // 12% for mailbox

    // Ensure minimum heights
    if (slaveListHeight < 100) slaveListHeight = 100;
    // Limit slave list to at most 6 visible rows (estimate row height from GUI font)
    {
        int maxRows = 6;
        int rowHeight = LayoutConfig::rowH; // fallback
        if (hGuiFont) {
            HDC hdcScreen = GetDC(NULL);
            if (hdcScreen) {
                HFONT oldF = (HFONT)SelectObject(hdcScreen, hGuiFont);
                TEXTMETRIC tm; ZeroMemory(&tm, sizeof(tm));
                if (GetTextMetrics(hdcScreen, &tm)) rowHeight = tm.tmHeight + tm.tmExternalLeading;
                SelectObject(hdcScreen, oldF);
                ReleaseDC(NULL, hdcScreen);
            }
        }
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
    hContentWnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, clientW, clientH, hwnd, NULL, hInstance, NULL);
        if (hContentWnd) {
            // Subclass content pane so it forwards commands/notifications to main window
            SetWindowSubclass(hContentWnd, ContentPaneSubclassProc, 1, (DWORD_PTR)this);
        }
    } else {
        // Ensure it's sized to client area
        SetWindowPos(hContentWnd, NULL, 0, 0, clientW, clientH, SWP_NOZORDER | SWP_SHOWWINDOW);
    }

    // Create a DPI-aware font that supports Chinese and Latin glyphs
    // Prefer Microsoft JhengHei UI (common CJK + Latin), fallback to DEFAULT_GUI_FONT
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

    // Row 0: Adapter (using LayoutHelper)
    RECT rLabel = helper.LabelRect();
    HWND hStatic = CreateWindowW(L"STATIC", L"網路介面卡:", WS_VISIBLE | WS_CHILD, rLabel.left, rLabel.top, labelW, 20, hContentWnd, NULL, hInstance, NULL);
    SendMessageW(hStatic, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    int ctrlXpos = helper.ControlX();
    
    // 計算 Adapter combo box 寬度（為按鈕預留空間）
    const int adapterGap = 10;
    const int btnConnectW = 100;
    const int btnDisconnectW = 100;
    int adapterButtonsW = btnConnectW + adapterGap + btnDisconnectW + adapterGap;
    int adapterComboW = helper.ControlW() - adapterButtonsW;
    if (adapterComboW < 200) adapterComboW = 200;
    
    hComboAdapter = CreateWindowW(WC_COMBOBOX, NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, ctrlXpos, helper.Y(), adapterComboW, 200, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_ADAPTER)), hInstance, NULL);
    SendMessageW(hComboAdapter, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));

    // Connect buttons - 緊跟在 combo box 後面（避免重疊）
    int adapterBtnX = ctrlXpos + adapterComboW + adapterGap;
    hBtnConnect = CreateWindowW(L"BUTTON", L"連接", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnConnectW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CONNECT)), hInstance, NULL);
    SendMessageW(hBtnConnect, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    adapterBtnX += btnConnectW + adapterGap;
    hBtnDisconnect = CreateWindowW(L"BUTTON", L"斷開", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, adapterBtnX, helper.Y(), btnDisconnectW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DISCONNECT)), hInstance, NULL);
    EnableWindow(hBtnDisconnect, FALSE);
    SendMessageW(hBtnDisconnect, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));

    // 移動到下一行（重要：避免下一行控件覆蓋此行）
    helper.NextRow(); y = helper.Y();

    // Cycle time controls: Desired (editable), Current (readonly), WKC (readonly)
    // Label
    HWND hStaticCycle = CreateWindowW(L"STATIC", L"週期 (μs):", WS_VISIBLE | WS_CHILD, left, helper.Y(), labelW, 20, hContentWnd, NULL, hInstance, NULL);
    SendMessageW(hStaticCycle, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));

    // Layout the cycle-row components by distributing available width
    // Compute available area to the right of the label column
    int rowX = helper.ControlX();
    int rowW = clientW - rowX - sideMargin;
    if (rowW < 400) rowW = 400;

    // We'll distribute into logical columns: Desired edit, Current(label+edit), WKC(label+edit), PDO_sent(label+edit), Buttons area
    const int buttonsAreaW = 240; // reserve some width at the right for buttons (status/clear/dump)

    int contentW = rowW - buttonsAreaW;
    if (contentW <= 0) contentW = rowW; // fall back to full row width if buttons area too large

    // Column counts: Desired (1), Current (label+edit), WKC (label+edit), PDO_sent (label+edit) = 4 logical columns
    int colCount = 4;
    int baseColW = contentW / colCount;
    // assign widths purely by division (no enforced minimums)
    int desiredW = baseColW;
    int currentW = baseColW;
    int wkcW = baseColW;
    int pdoW = contentW - (desiredW + currentW + wkcW);
    // guard against tiny/negative widths
    if (desiredW < 1) desiredW = 1;
    if (currentW < 1) currentW = 1;
    if (wkcW < 1) wkcW = 1;
    if (pdoW < 1) pdoW = 1;

    // Desired
    int x = rowX;
    int desiredEditW = desiredW - 8; // small padding
    hEditCycleTime = CreateWindowW(L"EDIT", L"1000", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, x, helper.Y(), desiredEditW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_CYCLE_TIME)), hInstance, NULL);
    SendMessageW(hEditCycleTime, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    x += desiredW;

    // Current (label + edit)
    int curLabelW = (currentW / 6 > 6) ? (currentW / 6) : 6; // small fraction of column for label
    int curEditW = currentW - curLabelW - 6;
    HWND hStaticCycleActualLabel = CreateWindowW(L"STATIC", L"實際", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), curLabelW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_CYCLE_ACTUAL_LABEL)), hInstance, NULL);
    SendMessageW(hStaticCycleActualLabel, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    hEditCurrentCycleTime = CreateWindowW(L"EDIT", L"N/A", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, x + 4 + curLabelW + 6, helper.Y(), curEditW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_CURRENT_CYCLE_TIME)), hInstance, NULL);
    SendMessageW(hEditCurrentCycleTime, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    x += currentW;

    // WKC (label + edit)
    int wkcLabelW = (wkcW / 6 > 6) ? (wkcW / 6) : 6;
    int wkcEditW = wkcW - wkcLabelW - 6;
    HWND hStaticCycleWKCLabel = CreateWindowW(L"STATIC", L"WKC", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), wkcLabelW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_CYCLE_WKC_LABEL)), hInstance, NULL);
    SendMessageW(hStaticCycleWKCLabel, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    hEditWKC = CreateWindowW(L"EDIT", L"0", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, x + 4 + wkcLabelW + 6, helper.Y(), wkcEditW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_WKC)), hInstance, NULL);
    SendMessageW(hEditWKC, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    x += wkcW;

    // PDO_sent (label + edit)
    int pdoLabelW = ((pdoW / 6) > 8) ? (pdoW / 6) : 8;
    int pdoEditW = pdoW - pdoLabelW - 6;
    HWND hStaticPDOSent = CreateWindowW(L"STATIC", L"PDO_sent:", WS_VISIBLE | WS_CHILD | SS_RIGHT, x + 4, helper.Y(), pdoLabelW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_PDO_SENT_LABEL)), hInstance, NULL);
    SendMessageW(hStaticPDOSent, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // use class member hEditPDOSent and create a button to toggle child status window
    hEditPDOSent = CreateWindowW(L"EDIT", L"00000000", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, x + 4 + pdoLabelW + 6, helper.Y(), pdoEditW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2010)), hInstance, NULL);
    SendMessageW(hEditPDOSent, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    x += pdoW;

    // Buttons area on the right - distribute inside buttonsAreaW
    int btnAreaX = rowX + rowW - buttonsAreaW + 8;
    hBtnPDOSentStatus = CreateWindowW(L"BUTTON", L"狀態視窗", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnAreaX, helper.Y(), 60, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_PDO_SENT_STATUS)), hInstance, NULL);
    SendMessageW(hBtnPDOSentStatus, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    btnAreaX += 60 + 8;
    hBtnClearStatus = CreateWindowW(L"BUTTON", L"狀態清除", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnAreaX, helper.Y(), 100, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1032)), hInstance, NULL);
    SendMessageW(hBtnClearStatus, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    btnAreaX += 100 + 8;
    hBtnDumpESI = CreateWindowW(L"BUTTON", L"讀從站ESI", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnAreaX, helper.Y(), 100, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_DUMP_ESI)), hInstance, NULL);
    SendMessageW(hBtnDumpESI, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    if (hBtnDumpESI) EnableWindow(hBtnDumpESI, FALSE);

    helper.NextRow(); y = helper.Y();
    
    // Slave list - place this row before ESI row
    // Make the list width follow the client width so there's no blank area on the right.
    int listWidth = clientW - left - sideMargin;
    if (listWidth < 400) listWidth = 400;
    hListSlaves = CreateWindowW(WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL | WS_TABSTOP, left, helper.Y(), listWidth, slaveListHeight, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST_SLAVES)), hInstance, NULL);
    SendMessageW(hListSlaves, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Subclass the slave list so we can reliably capture scroll events and
    // apply the same scroll-handling logic used for the SDO parameter list.
    if (hListSlaves) {
        SetWindowSubclass(hListSlaves, SDOListSubclassProc, 3, (DWORD_PTR)this);
    }
    // Set dark theme colors for ListView
    if (isDarkTheme) {
        ListView_SetBkColor(hListSlaves, clrDarkControl);
        ListView_SetTextColor(hListSlaves, clrDarkText);
        ListView_SetTextBkColor(hListSlaves, clrDarkControl);
    }
    LVCOLUMN lvc; lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc.fmt = LVCFMT_LEFT;

    // Distribute column widths evenly across available listWidth (no preset minima)
    const int slaveColCount = 7;
    std::vector<int> colWidths(slaveColCount, 0);
    int slaveBase = listWidth / slaveColCount;
    for (int i = 0; i < slaveColCount; ++i) colWidths[i] = slaveBase;
    int sumW = slaveBase * slaveColCount;
    if (sumW < listWidth) colWidths[slaveColCount - 1] += (listWidth - sumW);

    // Insert columns using computed widths (labels stay the same)
    lvc.cx = colWidths[0]; lvc.pszText = (LPWSTR)L"位置"; ListView_InsertColumn(hListSlaves, 0, &lvc);
    lvc.cx = colWidths[1]; lvc.pszText = (LPWSTR)L"名稱"; ListView_InsertColumn(hListSlaves, 1, &lvc);
    lvc.cx = colWidths[2]; lvc.pszText = const_cast<LPWSTR>(L"Vendor ID"); ListView_InsertColumn(hListSlaves, 2, &lvc);
    lvc.cx = colWidths[3]; lvc.pszText = const_cast<LPWSTR>(L"Product Code"); ListView_InsertColumn(hListSlaves, 3, &lvc);
    lvc.cx = colWidths[4]; lvc.pszText = const_cast<LPWSTR>(L"State"); ListView_InsertColumn(hListSlaves, 4, &lvc);
    lvc.cx = colWidths[5]; lvc.pszText = const_cast<LPWSTR>(L"Outputs"); ListView_InsertColumn(hListSlaves, 5, &lvc);
    lvc.cx = colWidths[6]; lvc.pszText = const_cast<LPWSTR>(L"Inputs"); ListView_InsertColumn(hListSlaves, 6, &lvc);
    ListView_SetExtendedListViewStyle(hListSlaves, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    
    // Move down by the allocated height
    helper.MoveDown(slaveListHeight); y = helper.Y();

    // (ESI controls moved to the SDO row to reduce vertical space)

    // Slave selector - place this row after ESI row
    HWND hStaticSlave = CreateWindowW(L"STATIC", L"選定從站:", WS_VISIBLE | WS_CHILD, left, helper.Y(), labelW, 20, hContentWnd, NULL, hInstance, NULL);
    SendMessageW(hStaticSlave, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Use ctrlW so the slave combobox width adapts to available space immediately
    hComboSlaves = CreateWindowW(WC_COMBOBOX, NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, helper.ControlX(), helper.Y(), ctrlW, 200, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2001)), hInstance, NULL);
    SendMessageW(hComboSlaves, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));

    // Move down after slave selector (single row advance)
    helper.NextRow(); y = helper.Y();
    RECT rSdoLabel = helper.LabelRect();
    // Keep SDO label aligned with other labels (no extra margin)
    HWND hStaticSdo = CreateWindowW(L"STATIC", L"SDO (參數表):", WS_VISIBLE | WS_CHILD, rSdoLabel.left, helper.Y(), labelW, 20, hContentWnd, NULL, hInstance, NULL);
    SendMessageW(hStaticSdo, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Create SDO Read/Write buttons to the right of the SDO title label
    // Position them relative to the label so they remain next to the title.
    int sdoBtnGap = 8;
    int sdoBtnW = 80;
    int sdoBtnH = 22;
    int sdoBtnAllW = sdoBtnW + 20; // wider button for "全部讀取" / "參數存檔"
    int sdoBtnX = rSdoLabel.left + labelW + sdoBtnGap;
    int sdoBtnY = helper.Y();
    hBtnReadSDO = CreateWindowW(L"BUTTON", L"讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX, sdoBtnY, sdoBtnW, sdoBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_READ_SDO)), hInstance, NULL);
    SendMessageW(hBtnReadSDO, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    hBtnWriteSDO = CreateWindowW(L"BUTTON", L"寫入", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX + sdoBtnW + sdoBtnGap, sdoBtnY, sdoBtnW, sdoBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_WRITE_SDO)), hInstance, NULL);
    SendMessageW(hBtnWriteSDO, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Read All SDO button: placed next to Read/Write buttons
    hBtnReadAllSDO = CreateWindowW(L"BUTTON", L"全部讀取", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX + (sdoBtnW + sdoBtnGap) * 2, sdoBtnY, sdoBtnAllW, sdoBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_READ_ALL_SDO)), hInstance, NULL);
    SendMessageW(hBtnReadAllSDO, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Disabled by default; enabled after successful connect
    EnableWindow(hBtnReadAllSDO, FALSE);
    // Write All button: write all RW parameters to selected slave
    hBtnWriteAllSDO = CreateWindowW(L"BUTTON", L"全部寫入", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX + (sdoBtnW + sdoBtnGap) * 2 + sdoBtnAllW + 8, sdoBtnY, sdoBtnAllW, sdoBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_WRITE_ALL_SDO)), hInstance, NULL);
    SendMessageW(hBtnWriteAllSDO, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnWriteAllSDO, FALSE);
    // Save Params button to the right of Read All
    hBtnSaveParams = CreateWindowW(L"BUTTON", L"參數存檔", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, sdoBtnX + (sdoBtnW + sdoBtnGap) * 3 + 8, sdoBtnY, sdoBtnAllW, sdoBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE_PARAMS)), hInstance, NULL);
    SendMessageW(hBtnSaveParams, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnSaveParams, FALSE);
    // Import Params button was moved next to the ESI browse button to improve layout.
    // Default disabled until a slave/parameter is selected
    EnableWindow(hBtnReadSDO, FALSE);
    EnableWindow(hBtnWriteSDO, FALSE);
    // Place ESI controls immediately before the SDO Read button to avoid overlapping
    {
        const int esiGap = 6;
        const int btnBrowseW = 80;
        const int esiLabelW = 80;
        // compute ESI right edge to be just left of the Read button
        int esiRight = sdoBtnX - 10; // 10px gap before read button
        int esiComboW = 280; // default width
        // ensure combo doesn't push into SDO buttons: cap by available space
        int maxComboW = esiRight - (helper.ControlX() + esiLabelW + 8) - btnBrowseW - esiGap;
        if (maxComboW < 120) maxComboW = 120; // minimum for readability
        if (esiComboW > maxComboW) esiComboW = maxComboW;

        int esiComboX = esiRight - (esiComboW + btnBrowseW + esiGap);
        int esiLabelX = esiComboX - esiLabelW - 8;
        if (esiLabelX < helper.ControlX()) {
            // fallback: place at helper.ControlX
            esiLabelX = helper.ControlX();
            esiComboX = esiLabelX + esiLabelW + 8;
        }

    // ESI label removed per UI update (label text moved out)

        // Create only the ESI Browse button (combobox removed)
        {
            int btnX = helper.ControlX();
            hBtnBrowseESI = CreateWindowW(L"BUTTON", L"ESI檔案匯入", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnX, sdoBtnY, btnBrowseW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_BROWSE_ESI)), hInstance, NULL);
            SendMessageW(hBtnBrowseESI, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
            // Create Import Params button to the right of the ESI browse button
            int importBtnX = btnX + btnBrowseW + esiGap;
            hBtnImportParams = CreateWindowW(L"BUTTON", L"匯入參數", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, importBtnX, sdoBtnY, sdoBtnAllW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_IMPORT_PARAMS)), hInstance, NULL);
            SendMessageW(hBtnImportParams, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
            // Preserve original enabled state
            EnableWindow(hBtnImportParams, TRUE);
        }
    }
    // Place the SDO ListView on the next row so the Read/Write buttons are visible above it
    helper.NextRow(); y = helper.Y();
    
    // Create a ListView to display SDO parameters (Index, Subindex, Name, Access, Value)
    // Align SDO parameter table to the global left so it matches slave list/mailbox
    const int sdoRightMargin = sideMargin;
    int sdoListX = left;
    int sdoListW = clientW - left - sdoRightMargin;
    if (sdoListW < 300) sdoListW = 300;
    hListSDOParams = CreateWindowW(WC_LISTVIEW, L"", WS_VISIBLE | WS_CHILD | WS_BORDER | WS_CLIPSIBLINGS | LVS_REPORT | LVS_SINGLESEL | WS_VSCROLL, sdoListX, helper.Y(), sdoListW, sdoTableHeight, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2015)), hInstance, NULL);
    SendMessageW(hListSDOParams, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    // Remove LVS_EX_FULLROWSELECT to allow clicking on individual cells (especially Value column)
    ListView_SetExtendedListViewStyle(hListSDOParams, LVS_EX_GRIDLINES);
    // Subclass the ListView to reliably catch click/dblclk and start in-place editor
    if (hListSDOParams) {
        SetWindowSubclass(hListSDOParams, SDOListSubclassProc, 2, (DWORD_PTR)this);
        // Set dark theme colors for ListView
        if (isDarkTheme) {
            ListView_SetBkColor(hListSDOParams, clrDarkControl);
            ListView_SetTextColor(hListSDOParams, clrDarkText);
            ListView_SetTextBkColor(hListSDOParams, clrDarkControl);
        }
    }
    
    // Add columns - evenly distribute widths across available sdoListW
    LVCOLUMN lvc2; lvc2.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT; lvc2.fmt = LVCFMT_LEFT;
    const int sdoCols = 7;
    // New column order: Index, Sub, Name, ESI Type, Access, Value, Hex
    // Distribute available width evenly across columns without preset minimums
    std::vector<int> sdoWidths(sdoCols, 0);
    int base = sdoListW / sdoCols;
    for (int i = 0; i < sdoCols; ++i) sdoWidths[i] = base;
    // Give any remainder to the last column so total equals sdoListW
    int sumS = 0; for (int v : sdoWidths) sumS += v;
    if (sumS < sdoListW) sdoWidths[sdoCols - 1] += (sdoListW - sumS);

    lvc2.cx = sdoWidths[0]; lvc2.pszText = (LPWSTR)L"Index"; ListView_InsertColumn(hListSDOParams,0,&lvc2);
    lvc2.cx = sdoWidths[1]; lvc2.pszText = (LPWSTR)L"Sub"; ListView_InsertColumn(hListSDOParams,1,&lvc2);
    lvc2.cx = sdoWidths[2]; lvc2.pszText = (LPWSTR)L"Name"; ListView_InsertColumn(hListSDOParams,2,&lvc2);
    lvc2.cx = sdoWidths[3]; lvc2.pszText = (LPWSTR)L"DataType"; ListView_InsertColumn(hListSDOParams,3,&lvc2);
    lvc2.cx = sdoWidths[4]; lvc2.pszText = (LPWSTR)L"Access"; ListView_InsertColumn(hListSDOParams,4,&lvc2);
    lvc2.cx = sdoWidths[5]; lvc2.pszText = (LPWSTR)L"Value"; ListView_InsertColumn(hListSDOParams,5,&lvc2);
    lvc2.cx = sdoWidths[6]; lvc2.pszText = (LPWSTR)L"Hex"; ListView_InsertColumn(hListSDOParams,6,&lvc2);
    
    // Advance layout helper past SDO area (no extra NextRow to avoid blank)
    // NOTE: removed helper.MoveDown(sdoTableHeight) to avoid reserving
    // vertical space below the SDO ListView. Mailbox Y is computed from
    // the actual ListView window rect below, so the manual move caused
    // unwanted blank gap.
    // 同步本地 y 變數，避免後續控件使用過期 y 值導致重疊
    y = helper.Y();

    // Initial adaptive reflow for SDO controls (will position list and buttons)
    ReflowSDOControls(sdoTableHeight);

    // 計算 Mailbox 的 Y 位置：依照實際 SDO ListView 的底部來定位，避免上方空白
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcList = {0};
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
    // Use height 22 to match other single-line controls (避免字元被剪裁)
    HWND hMailboxLabel = CreateWindowW(L"STATIC", L"Mailbox 資料 (hex):", WS_VISIBLE | WS_CHILD | SS_LEFT, rMailboxLabel.left, rMailboxLabel.top, labelW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_MAILBOX)), hInstance, NULL);
    SendMessageW(hMailboxLabel, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    
    // 計算 Mailbox edit 寬度（為按鈕預留空間）
    const int mailboxGap = 10;
    const int btnSendMailboxW = 105;
    int mailboxButtonsW = btnSendMailboxW + mailboxGap;
    int mailboxEditW = (contentWidth - labelW - 200) - mailboxButtonsW;
    if (mailboxEditW < 200) mailboxEditW = 200;
    
    hEditMailbox = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, left + labelW + 8, y, mailboxEditW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_MAILBOX)), hInstance, NULL);
    SendMessageW(hEditMailbox, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    
    // 按鈕位置：緊跟在 edit box 後面（避免重疊）
    int mailboxBtnX = left + labelW + 8 + mailboxEditW + mailboxGap;
    hBtnSendMailbox = CreateWindowW(L"BUTTON", L"發送 Mailbox", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, mailboxBtnX, y, btnSendMailboxW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SEND_MAILBOX)), hInstance, NULL);
    SendMessageW(hBtnSendMailbox, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0)); 
    EnableWindow(hBtnSendMailbox, FALSE);

    // 為 Mailbox 區域預留一行高度空間（22像素 + 適當邊距）
    y += 26; // 增加垂直間距以避免下方擠壓
    int pdoWidth = (contentWidth < 860) ? contentWidth : 860;
    // Make PDODisplay children part of the content pane so controls (edit/button)
    // can be found via GetDlgItem(hContentWnd, id).
    pdoDisplay.Initialize(hContentWnd, hInstance, hGuiFont, y, left, pdoWidth, pdoHeight);
    // Show PDO area by default so the PDO columns are visible on startup.
    // Previously this was hidden until a slave was selected; changing to visible
    // makes the PDO fields immediately accessible.
    pdoDisplay.Show(true);
    // Advance layout by actual PDO display height
    int pdoH = pdoDisplay.GetHeight();
    if (pdoH <= 0) pdoH = pdoHeight; // fallback to calculated height
    y += pdoH;

    // Create motion controller (non-UI logic) so other threads can call into it
    if (!motionController) {
        motionController = new MotionController(etherCATMaster, &pdoDisplay);
    }

    // 新增：運動控制區塊（放在 PDO 表格下方）
    {
        // 標題
        RECT rMotionLabel = { left, y, labelW, 22 };
        HWND hMotionLabel = CreateWindowW(L"STATIC", L"運動控制:", WS_VISIBLE | WS_CHILD | SS_LEFT, rMotionLabel.left, rMotionLabel.top, labelW, 22, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_MOTION_CONTROL)), hInstance, NULL);
        SendMessageW(hMotionLabel, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));

        // 設定按鈕樣式與間距
        const int motionBtnH = 24;
        const int motionBtnGap = 8;
        // 使用較小的按鈕寬度以容納多個按鈕
        const int motionBtnW = 80;

        int btnMotionX = left + labelW + 8;
        int btnY = y;

        hBtnFollowMode = CreateWindowW(L"BUTTON", L"跟隨模式", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FOLLOW_MODE)), hInstance, NULL);
        SendMessageW(hBtnFollowMode, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnFollowMode, FALSE);
        btnMotionX += motionBtnW + motionBtnGap;

        hBtnPowerOn = CreateWindowW(L"BUTTON", L"電源啟動", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_POWER_ON)), hInstance, NULL);
        SendMessageW(hBtnPowerOn, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnPowerOn, FALSE);
        if (hBtnPowerOn) {
            SetWindowSubclass(hBtnPowerOn, PowerOnButtonSubclassProc, 0, (DWORD_PTR)this);
        }
        btnMotionX += motionBtnW + motionBtnGap;

        hBtnPowerOff = CreateWindowW(L"BUTTON", L"電源關閉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_POWER_OFF)), hInstance, NULL);
        SendMessageW(hBtnPowerOff, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnPowerOff, FALSE);
        if (hBtnPowerOff) {
            SetWindowSubclass(hBtnPowerOff, PowerOffButtonSubclassProc, 0, (DWORD_PTR)this);
        }
        btnMotionX += motionBtnW + motionBtnGap;

        hBtnHome = CreateWindowW(L"BUTTON", L"回原點", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_HOME)), hInstance, NULL);
        SendMessageW(hBtnHome, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnHome, FALSE);
        if (hBtnHome) {
            SetWindowSubclass(hBtnHome, HomeButtonSubclassProc, 0, (DWORD_PTR)this);
        }
        btnMotionX += motionBtnW + motionBtnGap;

        hBtnJogCW = CreateWindowW(L"BUTTON", L"寸動正轉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_JOG_CW)), hInstance, NULL);
        SendMessageW(hBtnJogCW, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnJogCW, FALSE);
        btnMotionX += motionBtnW + motionBtnGap;

        hBtnJogCCW = CreateWindowW(L"BUTTON", L"寸動反轉", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_JOG_CCW)), hInstance, NULL);
        SendMessageW(hBtnJogCCW, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnJogCCW, FALSE);
        btnMotionX += motionBtnW + motionBtnGap;

    hBtnFaultReset = CreateWindowW(L"BUTTON", L"異常重置", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, btnMotionX, btnY, motionBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_FAULT_RESET)), hInstance, NULL);
        SendMessageW(hBtnFaultReset, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnFaultReset, FALSE);

    // Subclass fault reset button to capture press/release (separate UI from implementation)
    if (hBtnFaultReset) {
        SetWindowSubclass(hBtnFaultReset, FaultResetButtonSubclassProc, 0, (DWORD_PTR)this);
    }

    // 將 y 往下移動到下一列，並以 [立即停止] [手動模式] [自動模式] 的順序排列在同一列
    y += motionBtnH + 6;

    // 立即停止按鈕（放在最左，寬為 2 倍）
    const int stopBtnW = motionBtnW * 2; // 2 倍寬
    hBtnImmediateStop = CreateWindowW(L"BUTTON", L"立即停止", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON | BS_DEFPUSHBUTTON, left + labelW + 8, y, stopBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_IMMEDIATE_STOP)), hInstance, NULL);
    SendMessageW(hBtnImmediateStop, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnImmediateStop, FALSE);

    // Subclass immediate stop button to capture press/release (separate UI from implementation)
    if (hBtnImmediateStop) {
        SetWindowSubclass(hBtnImmediateStop, ImmediateStopButtonSubclassProc, 0, (DWORD_PTR)this);
    }

    // 手動 / 自動 模式按鈕（置於立即停止右側）
    const int modeBtnW = 100;
    int modeX = left + labelW + 8 + stopBtnW + motionBtnGap;
    hBtnManualMode = CreateWindowW(L"BUTTON", L"手動模式", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, modeX, y, modeBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_MANUAL_MODE)), hInstance, NULL);
    SendMessageW(hBtnManualMode, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnManualMode, FALSE);
    modeX += modeBtnW + motionBtnGap;
    hBtnAutoMode = CreateWindowW(L"BUTTON", L"自動模式", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, modeX, y, modeBtnW, motionBtnH, hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_AUTO_MODE)), hInstance, NULL);
    SendMessageW(hBtnAutoMode, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
    EnableWindow(hBtnAutoMode, FALSE);

    // 增加額外間距以避免與後續區塊重疊
    y += motionBtnH + 10;
    }

    // IOmap/PDO ListView creation removed - ESI-imported duplicate table disabled.
    // Conditional checks elsewhere (if (hListIOmap && IsWindow(hListIOmap))) prevent use of this control.

    // Status text box (full width, 調整高度更緊湊)
    // 原先的 "Status:" 靜態標籤已移除；不再使用額外 NextRow()
    // 主視窗底部狀態欄已移除（使用 StatusLogger 管理內存緩衝與檔案）
    statusLogger.Clear(false, hChildStatusEdit, hChildStatusWnd);
    statusUserScrolled = false;
    
    // Show N/A hint in slave list when not connected
    LVITEM lvi = {0};
    lvi.mask = LVIF_TEXT;
    lvi.iItem = 0;
    lvi.iSubItem = 0;
    // Do not insert default placeholder values for Position/Name columns here.
    // Leave the list empty until UpdateSlaveList() populates actual slaves.

    // Populate lists after all controls are created
    PopulateAdapters();
    PopulateESIFiles();
    
    // Ensure initial controls are reflowed to current client width
    ReflowMainControls();
    
    // 最終確保 Slave List 可見
    if (hListSlaves) {
        ShowWindow(hListSlaves, SW_SHOW);
        UpdateWindow(hListSlaves);
    }
}

LRESULT CALLBACK Win32Window::WindowProc(HWND hwnd, UINT uMsg,
                                         WPARAM wParam, LPARAM lParam) {
    Win32Window* window = nullptr;

    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        window = reinterpret_cast<Win32Window*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)window);
    } else {
        window = reinterpret_cast<Win32Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (window) {
        return window->HandleMessage(uMsg, wParam, lParam);
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// Reflow SDO controls when window size changes or initial layout
void Win32Window::ReflowSDOControls(int dynamicHeight)
{
    if (!hwnd) return;
    RECT clientRc; GetClientRect(hwnd, &clientRc);
    int clientW = clientRc.right - clientRc.left;
    const int left = LayoutConfig::left;
    const int labelW = LayoutConfig::labelW;
    const int rightMargin = sideMargin;
    
    // 計算可用寬度（控制欄與全寬均需）
    int ctrlX = left + labelW + 8;
    int availableW = clientW - ctrlX - rightMargin;
    if (availableW < 400) availableW = 400;

    // Start layout below top padding; now use content-pane client coords (hContentWnd)
    int listY = 10;
    int listX = ctrlX;
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcExisting = {0};
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
                    RECT rcLabel = {0};
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

    // Use the provided dynamic height
    int listH = dynamicHeight;

    // Ensure the list is aligned with the global left margin for visual consistency
    int listX_aligned = left;
    if (hListSDOParams) SetWindowPos(hListSDOParams, NULL, listX_aligned, listY, listW, listH, SWP_NOZORDER | SWP_SHOWWINDOW);

    // Recalculate SDO ListView column widths so Hex column remains wide enough
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        // Use the actual ListView client width (accounts for borders/vertical scrollbar)
        RECT rcClient = {0};
        GetClientRect(hListSDOParams, &rcClient);
        int lvClientW = rcClient.right - rcClient.left;
        // Account for vertical scrollbar width if present
        int vsw = GetSystemMetrics(SM_CXVSCROLL);
        BOOL hasVScroll = (GetWindowLong(hListSDOParams, GWL_STYLE) & WS_VSCROLL) != 0;
        if (hasVScroll) lvClientW -= vsw;
        // Fallback to available listW if client query failed or is too small
        if (lvClientW <= 0) lvClientW = listW;
        if (lvClientW < 200) lvClientW = 200;

    const int colCount = 7;
    int base = lvClientW / colCount;
    int colW[7];
    for (int i = 0; i < colCount; ++i) colW[i] = base;
    int sum = base * colCount;
    if (sum < lvClientW) colW[colCount - 1] += (lvClientW - sum);
    for (int i = 0; i < colCount; ++i) ListView_SetColumnWidth(hListSDOParams, i, colW[i]);
    }

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
                RECT rcLabel = {0};
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

    // If an ESI Browse button exists, place the SDO buttons after it
    if (hBtnBrowseESI && IsWindow(hBtnBrowseESI)) {
        RECT rcBrowse; GetWindowRect(hBtnBrowseESI, &rcBrowse);
        POINT ptB = { rcBrowse.left, rcBrowse.top };
        ScreenToClient(hContentWnd, &ptB);
        int browseRight = ptB.x + (rcBrowse.right - rcBrowse.left);
        // prefer placing after Browse button to ensure SDO buttons are visible
        btnX_from_label = browseRight + 8;
        // align vertical position with the browse button row
        btnY = ptB.y;
    }

    // Determine a safe X for the SDO buttons. Prefer the label-derived X
    // (btnX_from_label) but ensure they fit in a reserved right-side area
    // so they won't be overlapped by wide controls like the ESI combo.
    int totalButtonsW = btnW + btnSpacing + btnW + btnSpacing + btnAllW + btnSpacing + btnAllW; // read, write, all, save
    // Ensure buttons stay inside client area leaving sideMargin on the right
    int rightBasedX = clientW - sideMargin - totalButtonsW;

    // Determine whether buttons fit on the same row as the list (left-aligned).
    int leftAlignedX = listX_aligned;
    bool fitsLeft = (leftAlignedX + totalButtonsW <= clientW - sideMargin);

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
            if (candidateX + totalButtonsW <= clientW - sideMargin) finalBtnX = candidateX;
        }
    }

    if (finalBtnY < 0) finalBtnY = listY + listH + 5;

    if (hBtnReadSDO) SetWindowPos(hBtnReadSDO, NULL, finalBtnX, finalBtnY, btnW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (hBtnWriteSDO) SetWindowPos(hBtnWriteSDO, NULL, finalBtnX + btnW + btnSpacing, finalBtnY, btnW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (hBtnReadAllSDO) SetWindowPos(hBtnReadAllSDO, NULL, finalBtnX + (btnW + btnSpacing) * 2, finalBtnY, btnAllW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);
    if (hBtnSaveParams) SetWindowPos(hBtnSaveParams, NULL, finalBtnX + (btnW + btnSpacing) * 2 + btnAllW + btnSpacing, finalBtnY, btnAllW, 22, SWP_NOZORDER | SWP_SHOWWINDOW);

    // Update contentHeight: include either the list bottom or the buttons bottom
    int sdoBottom = listY + listH;
    int buttonsBottom = finalBtnY + 22;
    if (buttonsBottom > sdoBottom) sdoBottom = buttonsBottom;
    if (sdoBottom > contentHeight) contentHeight = sdoBottom;
}

// Reflow main controls horizontally to reduce margins and use available width
void Win32Window::ReflowMainControls()
{
    if (minimizedToTray) return; // skip reflow while minimized to tray
    if (!hwnd) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;
    const int left = LayoutConfig::left;
    const int labelW = LayoutConfig::labelW;
    const int rightMargin = sideMargin;  // 使用對稱邊距
    int ctrlX = left + labelW + 8;
    
    // 計算可用寬度（使用更多空間）
    int availableW = clientW - ctrlX - rightMargin;
    if (availableW < 300) availableW = 300;  // 最小寬度
    
    // 主控件寬度（不設上限，讓它自適應）
    int ctrlW = availableW;
    
    // Calculate dynamic heights based on available client height (same logic as CreateControls)
    const int fixedTopHeight = 120; // approximate space for adapter + cycle time + ESI + slave selector
    const int fixedBottomHeight = 50; // space for status area
    int availableContentHeight = clientH - fixedTopHeight - fixedBottomHeight;
    if (availableContentHeight < 400) availableContentHeight = 400; // minimum height

    // Allocate heights proportionally (same as CreateControls)
    // Use a fixed height for the slave table so its row layout stays stable
    // and controls below (ESI/SDO/mailbox) don't jump around when window height changes.
    // Compute a reasonable slave list height but cap to at most 6 visible rows
    int slaveListHeight = (int)(availableContentHeight * 0.25f);
    if (slaveListHeight < 100) slaveListHeight = 100;
    {
        int maxRows = 6;
        int rowHeight = LayoutConfig::rowH;
        if (hGuiFont) {
            HDC hdc = GetDC(hContentWnd ? hContentWnd : hwnd);
            if (hdc) {
                HFONT oldF = (HFONT)SelectObject(hdc, hGuiFont);
                TEXTMETRIC tm; ZeroMemory(&tm, sizeof(tm));
                if (GetTextMetrics(hdc, &tm)) rowHeight = tm.tmHeight + tm.tmExternalLeading;
                SelectObject(hdc, oldF);
                ReleaseDC(hContentWnd ? hContentWnd : hwnd, hdc);
            }
        }
        int headerH = 0;
        if (hListSlaves && IsWindow(hListSlaves)) {
            HWND hHeader = ListView_GetHeader(hListSlaves);
            if (hHeader && IsWindow(hHeader)) {
                RECT rcHeader; GetWindowRect(hHeader, &rcHeader);
                headerH = rcHeader.bottom - rcHeader.top;
            }
        }
        if (headerH <= 0) headerH = rowHeight + 6;
            // Compute actual item height if possible and set a final height that
            // aligns to whole rows to avoid partially clipped last row.
            int actualRowH = rowHeight;
            if (hListSlaves && IsWindow(hListSlaves)) {
                int cnt = ListView_GetItemCount(hListSlaves);
                RECT rcItem = {0};
                if (cnt > 0 && ListView_GetItemRect(hListSlaves, 0, &rcItem, LVIR_BOUNDS)) {
                    actualRowH = rcItem.bottom - rcItem.top;
                    if (actualRowH <= 0) actualRowH = rowHeight;
                }
            }
            int maxListH = headerH + (actualRowH * maxRows) + 10; // add safety padding
            if (slaveListHeight > maxListH) slaveListHeight = maxListH;
    }
    // SDO table height reduced by half as requested
    int sdoTableHeight = (int)(availableContentHeight * 0.30f * 0.5f);  // ~15% for SDO table
    int pdoHeight = (int)(availableContentHeight * 0.25f);       // 25% for PDO display
    int mailboxHeight = (int)(availableContentHeight * 0.20f);   // 20% for mailbox and buttons

    // Ensure minimum heights
    if (slaveListHeight < 100) slaveListHeight = 100;
    // Lower the minimum height since we've halved the allocated proportion
    if (sdoTableHeight < 75) sdoTableHeight = 75;
    if (pdoHeight < 120) pdoHeight = 120;
    if (mailboxHeight < 40) mailboxHeight = 40;
    
    // 計算按鈕區域寬度
    int btnAreaW = 100 + 10 + 100 + 10 + 100 + 10 + 100;  // Connect + Disconnect + Dump + Clear + 間距
    
    // 如果視窗足夠寬，擴展控件寬度
    if (availableW > btnAreaW + 300) {
        ctrlW = availableW - btnAreaW - 20;
    }

    // Reset contentHeight accumulator (we'll compute bottom-most control in content coords)
    contentHeight = 0;

    // 1) Adapter combobox 和連接按鈕（確保不重疊）
    const int adapterGap = 10;
    const int btnConnectW = 100;
    const int btnDisconnectW = 100;
    
    // 計算 adapter combo 可用寬度（為按鈕預留空間）
    int adapterButtonsW = btnConnectW + adapterGap + btnDisconnectW + adapterGap;
    int adapterComboW = availableW - adapterButtonsW;
    if (adapterComboW < 200) adapterComboW = 200;  // 最小寬度
    
    if (hComboAdapter) {
        RECT rcTmp; GetWindowRect(hComboAdapter, &rcTmp);
        POINT ptTmp = { rcTmp.left, rcTmp.top }; ScreenToClient(hContentWnd, &ptTmp);
        SetWindowPos(hComboAdapter, NULL, ctrlX, ptTmp.y, adapterComboW, 200, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptTmp.y + 200) > contentHeight) contentHeight = ptTmp.y + 200;
    }
    
    // 按鈕位置：緊跟在 combo box 後面（保持 Y 座標和大小）
    int adapterBtnX = ctrlX + adapterComboW + adapterGap;
    if (hBtnConnect) {
        RECT rcBtn; GetWindowRect(hBtnConnect, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SetWindowPos(hBtnConnect, NULL, adapterBtnX, ptBtn.y, btnConnectW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }
    adapterBtnX += btnConnectW + adapterGap;
    if (hBtnDisconnect) {
        RECT rcBtn; GetWindowRect(hBtnDisconnect, &rcBtn);
        POINT ptBtn = {rcBtn.left, rcBtn.top}; ScreenToClient(hContentWnd, &ptBtn);
        SetWindowPos(hBtnDisconnect, NULL, adapterBtnX, ptBtn.y, btnDisconnectW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
    }

    // --- Cycle-row reflow: Desired / Current / WKC / PDO_sent / Buttons ---
    {
        int cycleY = 0;
        // attempt to find current Y of existing Desired edit
        if (hEditCycleTime && IsWindow(hEditCycleTime)) {
            RECT clientRc; GetWindowRect(hEditCycleTime, &clientRc);
            POINT pt = { clientRc.left, clientRc.top }; ScreenToClient(hContentWnd, &pt);
            cycleY = pt.y;
        } else {
            // fallback: place it after adapter row
            cycleY = ctrlX - 0; // relative fallback (will be overridden by helper in CreateControls)
        }

        int rowX = ctrlX;
        int rowW = clientW - rowX - rightMargin;
        if (rowW < 400) rowW = 400;
        const int buttonsAreaW = 240;
        int contentW = rowW - buttonsAreaW;
        if (contentW < 400) contentW = rowW;
        int colCount = 4;
        int baseColW = contentW / colCount;
        int desiredW = (baseColW > 70) ? baseColW : 70;
        int currentW = (baseColW > 100) ? baseColW : 100;
        int wkcW = (baseColW > 80) ? baseColW : 80;
        int pdoW = contentW - (desiredW + currentW + wkcW);

        int x = rowX;
        if (hEditCycleTime && IsWindow(hEditCycleTime)) {
            SetWindowPos(hEditCycleTime, NULL, x, cycleY, desiredW - 8, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        x += desiredW;

        // Current label + edit
        int curLabelW = 36;
        int curEditW = currentW - curLabelW - 6;
        HWND hStaticCycleActualLabelLocal = GetDlgItem(hContentWnd, IDC_STATIC_CYCLE_ACTUAL_LABEL);
        if (hStaticCycleActualLabelLocal && IsWindow(hStaticCycleActualLabelLocal)) {
            SetWindowPos(hStaticCycleActualLabelLocal, NULL, x + 4, cycleY, curLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (hEditCurrentCycleTime && IsWindow(hEditCurrentCycleTime)) {
            SetWindowPos(hEditCurrentCycleTime, NULL, x + 4 + curLabelW + 6, cycleY, curEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        x += currentW;

        // WKC
        int wkcLabelW = 36;
        int wkcEditW = wkcW - wkcLabelW - 6;
        HWND hStaticCycleWKCLabelLocal = GetDlgItem(hContentWnd, IDC_STATIC_CYCLE_WKC_LABEL);
        if (hStaticCycleWKCLabelLocal && IsWindow(hStaticCycleWKCLabelLocal)) {
            SetWindowPos(hStaticCycleWKCLabelLocal, NULL, x + 4, cycleY, wkcLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (hEditWKC && IsWindow(hEditWKC)) {
            SetWindowPos(hEditWKC, NULL, x + 4 + wkcLabelW + 6, cycleY, wkcEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        x += wkcW;

        // PDO_sent
        int pdoLabelW = 46;
        int pdoEditW = pdoW - pdoLabelW - 6;
        HWND hStaticPDOSentLocal = GetDlgItem(hContentWnd, IDC_STATIC_PDO_SENT_LABEL);
        if (hStaticPDOSentLocal && IsWindow(hStaticPDOSentLocal)) {
            SetWindowPos(hStaticPDOSentLocal, NULL, x + 4, cycleY, pdoLabelW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (hEditPDOSent && IsWindow(hEditPDOSent)) {
            SetWindowPos(hEditPDOSent, NULL, x + 4 + pdoLabelW + 6, cycleY, pdoEditW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // Buttons area
        int btnX = rowX + rowW - buttonsAreaW + 8;
        if (hBtnPDOSentStatus && IsWindow(hBtnPDOSentStatus)) SetWindowPos(hBtnPDOSentStatus, NULL, btnX, cycleY, 60, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        btnX += 60 + 8;
        if (hBtnClearStatus && IsWindow(hBtnClearStatus)) SetWindowPos(hBtnClearStatus, NULL, btnX, cycleY, 100, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        btnX += 100 + 8;
        if (hBtnDumpESI && IsWindow(hBtnDumpESI)) SetWindowPos(hBtnDumpESI, NULL, btnX, cycleY, 100, 22, SWP_NOZORDER | SWP_NOACTIVATE);
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
    // Combo removed: position Browse button relative to SDO Read button or ctrlX
    if (hBtnBrowseESI && IsWindow(hBtnBrowseESI)) {
        const int esiGap = 6;
        const int btnBrowseW = 80;
        const int esiLabelW = 80; // width used for the label

        // Try to find the SDO Read button's client X position
        if (hBtnReadSDO && IsWindow(hBtnReadSDO)) {
            RECT rcBtn; GetWindowRect(hBtnReadSDO, &rcBtn);
            POINT ptBtn = { rcBtn.left, rcBtn.top }; ScreenToClient(hContentWnd, &ptBtn);
            int readX = ptBtn.x;
            int readY = ptBtn.y;

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

            // Move the ESI label (find by matching its text)
            HWND child = GetWindow(hContentWnd, GW_CHILD);
            while (child) {
                wchar_t cls[64] = {0};
                GetClassNameW(child, cls, _countof(cls));
                if (wcscmp(cls, L"Static") == 0) {
                    wchar_t txt[128] = {0};
                    GetWindowTextW(child, txt, _countof(txt));
                    if (wcsstr(txt, L"ESI") != NULL) {
                        SetWindowPos(child, NULL, esiLabelX, readY, esiLabelW, 20, SWP_NOZORDER | SWP_NOACTIVATE);
                        break;
                    }
                }
                child = GetWindow(child, GW_HWNDNEXT);
            }

            // Place browse button left-aligned to the control column (ctrlX)
            int btnX = ctrlX; // align to left control column
            int browseW = (btnBrowseW > 60) ? btnBrowseW : 60;
            SetWindowPos(hBtnBrowseESI, NULL, btnX, readY, browseW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
            if ((readY + 22) > contentHeight) contentHeight = readY + 22;
        } else {
            // Fallback: align ESI to the ctrlX column if SDO Read button not found
            int esiButtonsW = btnBrowseW + esiGap;
            int esiComboW = availableW - esiButtonsW;
            if (esiComboW < 120) esiComboW = 120;
            // Fallback: align browse button to ctrlX
            RECT rcBtn; GetWindowRect(hBtnBrowseESI, &rcBtn);
            POINT ptBtn = { rcBtn.left, rcBtn.top }; ScreenToClient(hContentWnd, &ptBtn);
            SetWindowPos(hBtnBrowseESI, NULL, ctrlX, ptBtn.y, btnBrowseW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
            if ((ptBtn.y + 22) > contentHeight) contentHeight = ptBtn.y + 22;
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
        SetWindowPos(hComboSlaves, NULL, ctrlX, ptTmp3.y, slaveComboW, currentH, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((ptTmp3.y + currentH) > contentHeight) contentHeight = ptTmp3.y + currentH;
    }

    // 5) Slave list width and height - 使用動態高度，靠近視窗右側並填滿可用寬度
    int listWidth = clientW - left - sideMargin; // 使用對稱邊距
    if (listWidth < 200) listWidth = 200;
    if (hListSlaves) {
        // 設定 list 的 X 為 left（維持左側標籤列對齊），寬度更新為計算後的 listWidth
        RECT rcList; GetWindowRect(hListSlaves, &rcList);
        POINT ptList = { rcList.left, rcList.top }; ScreenToClient(hContentWnd, &ptList);
        int newX = left; // 保持與其他控件左邊對齊
    SetWindowPos(hListSlaves, NULL, newX, ptList.y, listWidth, slaveListHeight, SWP_NOZORDER | SWP_SHOWWINDOW);
    if ((ptList.y + slaveListHeight) > contentHeight) contentHeight = ptList.y + slaveListHeight;

        // Recalculate column widths to evenly distribute across ListView client area.
        const int colCount = 7; // known number of columns

        // Get the actual client width of the ListView (accounts for borders)
        RECT rcClient; GetClientRect(hListSlaves, &rcClient);
        int lvClientW = rcClient.right - rcClient.left;
        // Subtract vertical scrollbar width if present (prevents horizontal gap)
        int vsw = GetSystemMetrics(SM_CXVSCROLL);
        BOOL hasVScroll = (GetWindowLong(hListSlaves, GWL_STYLE) & WS_VSCROLL) != 0;
        if (hasVScroll) lvClientW -= vsw;
        if (lvClientW < 200) lvClientW = 200;

        int widths[colCount];
        int lvBase = lvClientW / colCount;
        for (int i = 0; i < colCount; ++i) widths[i] = lvBase;
        int sum = lvBase * colCount;
        if (sum < lvClientW) widths[colCount - 1] += (lvClientW - sum);

        // Apply widths to ListView columns
        for (int i = 0; i < colCount; ++i) {
            ListView_SetColumnWidth(hListSlaves, i, widths[i]);
        }
    }

    // 5.1) IOmap / PDO ListView column reflow: compute widths from the ListView's
    // actual client width and subtract vertical scrollbar width if present.
    if (hListIOmap && IsWindow(hListIOmap)) {
        RECT rcList; GetWindowRect(hListIOmap, &rcList);
        POINT ptList = { rcList.left, rcList.top }; ScreenToClient(hContentWnd, &ptList);
        // Keep its X aligned with global left and width to available client width
        int newX = left;
        int newW = clientW - left - sideMargin;
        if (newW < 300) newW = 300;
        // Height will be managed elsewhere; keep current height
        int listH = rcList.bottom - rcList.top;
        SetWindowPos(hListIOmap, NULL, newX, ptList.y, newW, listH, SWP_NOZORDER | SWP_SHOWWINDOW);

        // Now recalc columns based on actual ListView client area
        RECT rcClient2 = {0}; GetClientRect(hListIOmap, &rcClient2);
        int lvClientW2 = rcClient2.right - rcClient2.left;
        int vsw2 = GetSystemMetrics(SM_CXVSCROLL);
        BOOL hasVScroll2 = (GetWindowLong(hListIOmap, GWL_STYLE) & WS_VSCROLL) != 0;
        if (hasVScroll2) lvClientW2 -= vsw2;
        if (lvClientW2 < 200) lvClientW2 = 200;

        const int ioCols = 4;
    int avail2 = lvClientW2;
    int base2 = avail2 / ioCols;
    int colW2[ioCols];
    for (int i = 0; i < ioCols; ++i) colW2[i] = base2;
    int sum2 = base2 * ioCols;
    if (sum2 < avail2) colW2[ioCols - 1] += (avail2 - sum2);
    for (int i = 0; i < ioCols; ++i) ListView_SetColumnWidth(hListIOmap, i, colW2[i]);
        if ((ptList.y + listH) > contentHeight) contentHeight = ptList.y + listH;
    }

    // 6) Clear Status and Dump ESI button positions (next to 狀態 button)
    if (hBtnPDOSentStatus && hBtnClearStatus) {
        RECT rcStatus; GetWindowRect(hBtnPDOSentStatus, &rcStatus);
        POINT ptStatus = { rcStatus.left, rcStatus.top };
        ScreenToClient(hContentWnd, &ptStatus);
        int clearX = ptStatus.x + (rcStatus.right - rcStatus.left) + 8;
    SetWindowPos(hBtnClearStatus, NULL, clearX, ptStatus.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if ((ptStatus.y + 22) > contentHeight) contentHeight = ptStatus.y + 22;
        
        // Position Dump ESI button next to the selected-slave combobox instead
        // so it's clear the action targets the selected slave.
        if (hBtnDumpESI && hComboSlaves && IsWindow(hComboSlaves)) {
            RECT rcCombo; GetWindowRect(hComboSlaves, &rcCombo);
            POINT ptCombo = { rcCombo.left, rcCombo.top };
            ScreenToClient(hContentWnd, &ptCombo);
            int comboW = rcCombo.right - rcCombo.left;
            int dumpBtnW = 100; // default width if not measured
            // try to get current width from window rect
            RECT rcBtn; if (GetWindowRect(hBtnDumpESI, &rcBtn)) dumpBtnW = rcBtn.right - rcBtn.left;
            int desiredX = ptCombo.x + comboW + 8; // right of combobox
            // ensure we don't overflow into the reserved right area
            int rightBasedX = clientW - sideMargin - dumpBtnW;
            // If not enough horizontal space, wrap button to next line under combobox
            if (desiredX + dumpBtnW > rightBasedX) {
                int wrapX = ptCombo.x; // align left with combobox
                int wrapY = ptCombo.y + (rcCombo.bottom - rcCombo.top) + 4; // small vertical gap
                SetWindowPos(hBtnDumpESI, NULL, wrapX, wrapY, dumpBtnW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
                if ((wrapY + 22) > contentHeight) contentHeight = wrapY + 22;
            } else {
                int finalX = (desiredX > rightBasedX) ? rightBasedX : desiredX;
                if (finalX < ptCombo.x + comboW + 8) finalX = ptCombo.x + comboW + 8; // keep at least adjacent
                SetWindowPos(hBtnDumpESI, NULL, finalX, ptCombo.y, dumpBtnW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
                if ((ptCombo.y + 22) > contentHeight) contentHeight = ptCombo.y + 22;
            }
        }
    }

    // 7) 重新計算 SDO 列表底部，依序放置 Mailbox -> PDO，避免重疊
    int sdoBottomY = 0;
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        RECT rcList; GetWindowRect(hListSDOParams, &rcList);
        POINT pt = { rcList.left, rcList.bottom }; ScreenToClient(hContentWnd, &pt);
        sdoBottomY = pt.y;
    } else {
        sdoBottomY = 10 + 200; // fallback
    }

    // Mailbox 區塊：緊貼 SDO 下方 + 少量間距
    const int mailboxGap = 10;            // edit 與按鈕間
    const int verticalGap = 6;            // 區塊之間垂直間距
    const int btnSendMailboxW = 105;
    int mailboxY = sdoBottomY + verticalGap;
    int mailboxX = left + labelW + 8;
    int mailboxEditW = availableW - btnSendMailboxW - mailboxGap;
    if (mailboxEditW < 200) mailboxEditW = 200;
    if (mailboxEditW < 200) mailboxEditW = 200;
    // Reposition mailbox static label so it is vertically aligned with edit and button
    HWND hMailboxLabelHandle = GetDlgItem(hContentWnd, IDC_STATIC_MAILBOX);
    if (hMailboxLabelHandle && IsWindow(hMailboxLabelHandle)) {
        SetWindowPos(hMailboxLabelHandle, NULL, left, mailboxY, labelW, 22, SWP_NOZORDER);
        if ((mailboxY + 22) > contentHeight) contentHeight = mailboxY + 22;
    }
    if (hEditMailbox) SetWindowPos(hEditMailbox, NULL, mailboxX, mailboxY, mailboxEditW, 22, SWP_NOZORDER);
    if (hBtnSendMailbox) {
        int btnMailboxX = mailboxX + mailboxEditW + mailboxGap;
        SetWindowPos(hBtnSendMailbox, NULL, btnMailboxX, mailboxY, btnSendMailboxW, 22, SWP_NOZORDER | SWP_NOACTIVATE);
        if ((mailboxY + 22) > contentHeight) contentHeight = mailboxY + 22;
    }

    // PDO 顯示區：放在 Mailbox 區塊之後
    int pdoTop = mailboxY + 22 + verticalGap; // edit 高度 22
    int pdoRightMargin = sideMargin;
    int pdoWidth = clientW - left - pdoRightMargin;
    if (pdoWidth < 300) pdoWidth = 300;
    // Let PDODisplay compute its needed height based on content when resized.
    pdoDisplay.ResizeWidth(pdoWidth);
    // PDODisplay::ResizeWidth will recompute and apply groupHeight; just set its top (content coords)
    pdoDisplay.SetTop(pdoTop);
    // Immediately rebuild controls to ensure positions are consistent
    // before the content pane is moved/shown.
    pdoDisplay.RebuildPDOControls();
    if ((pdoTop + pdoDisplay.GetHeight()) > contentHeight) contentHeight = pdoTop + pdoDisplay.GetHeight();

    // Reposition motion control area (label + two rows of buttons) so it
    // follows the PDO display when the window is resized. The controls are
    // created in CreateControls() with known IDs / member HWNDs; find the
    // static label via IDC_STATIC_MOTION_CONTROL and reposition members.
    {
        int motionBtnH = 24;
        int motionBtnGap = 8;
        int motionBtnW = 80;
        int stopBtnW = motionBtnW * 2;
        int modeBtnW = 100;

        int motionTop = pdoTop + pdoDisplay.GetHeight();
        int firstRowY = motionTop;
        int secondRowY = firstRowY + motionBtnH + 6; // matches CreateControls spacing

        // Motion label (static) may not be stored as member; fetch by ID
        HWND hMotionLabel = GetDlgItem(hContentWnd, IDC_STATIC_MOTION_CONTROL);
        if (hMotionLabel && IsWindow(hMotionLabel)) {
            SetWindowPos(hMotionLabel, NULL, left, firstRowY, labelW, 22, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            if ((firstRowY + 22) > contentHeight) contentHeight = firstRowY + 22;
        }

        // First row: follow/power/home/jog/fault buttons
        int bx = left + labelW + 8;
        if (hBtnFollowMode && IsWindow(hBtnFollowMode)) { SetWindowPos(hBtnFollowMode, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnPowerOn && IsWindow(hBtnPowerOn)) { SetWindowPos(hBtnPowerOn, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnPowerOff && IsWindow(hBtnPowerOff)) { SetWindowPos(hBtnPowerOff, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnHome && IsWindow(hBtnHome)) { SetWindowPos(hBtnHome, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnJogCW && IsWindow(hBtnJogCW)) { SetWindowPos(hBtnJogCW, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnJogCCW && IsWindow(hBtnJogCCW)) { SetWindowPos(hBtnJogCCW, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }
        if (hBtnFaultReset && IsWindow(hBtnFaultReset)) { SetWindowPos(hBtnFaultReset, NULL, bx, firstRowY, motionBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); bx += motionBtnW + motionBtnGap; if ((firstRowY + motionBtnH) > contentHeight) contentHeight = firstRowY + motionBtnH; }

        // Second row: Immediate Stop, Manual, Auto
        int sx = left + labelW + 8;
        if (hBtnImmediateStop && IsWindow(hBtnImmediateStop)) { SetWindowPos(hBtnImmediateStop, NULL, sx, secondRowY, stopBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); if ((secondRowY + motionBtnH) > contentHeight) contentHeight = secondRowY + motionBtnH; }
        sx += stopBtnW + motionBtnGap;
        if (hBtnManualMode && IsWindow(hBtnManualMode)) { SetWindowPos(hBtnManualMode, NULL, sx, secondRowY, modeBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); sx += modeBtnW + motionBtnGap; if ((secondRowY + motionBtnH) > contentHeight) contentHeight = secondRowY + motionBtnH; }
        if (hBtnAutoMode && IsWindow(hBtnAutoMode)) { SetWindowPos(hBtnAutoMode, NULL, sx, secondRowY, modeBtnW, motionBtnH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW); if ((secondRowY + motionBtnH) > contentHeight) contentHeight = secondRowY + motionBtnH; }

        // Add a small bottom padding similar to CreateControls final increment
        if ((secondRowY + motionBtnH + 10) > contentHeight) contentHeight = secondRowY + motionBtnH + 10;
    }

    // (已移除底部狀態欄，改為使用內存 statusBuffer 與 訊息視窗)

    // Position the content pane according to scrollPos (move the entire content child)
    if (hContentWnd && IsWindow(hContentWnd)) {
        SetWindowPos(hContentWnd, NULL, 0, -scrollPos, clientW, contentHeight > clientH ? contentHeight : clientH, SWP_NOZORDER | SWP_SHOWWINDOW);
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

// Add a tray icon to the notification area. Safe to call multiple times.
void Win32Window::AddTrayIcon()
{
    if (!useTray || !hwnd) return;
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 101;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP + 1;
    if (hTrayIcon) nid.hIcon = hTrayIcon; else nid.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(101));
    wcscpy_s(nid.szTip, L"BigCat");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Win32Window::RemoveTrayIcon()
{
    if (!hwnd) return;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// Lightweight scroll updater: avoid heavy reflow/rebuild during wheel/scroll
void Win32Window::UpdateContentPaneScrollOnly()
{
    if (!hwnd || !hContentWnd || !IsWindow(hContentWnd)) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    // Keep contentHeight as last computed; only move pane vertically by scrollPos
    int paneH = (contentHeight > 0 ? contentHeight : clientH);
    if (paneH < clientH) paneH = clientH;

    SetWindowPos(hContentWnd, NULL, 0, -scrollPos, clientW, paneH,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    // Optional: minimal invalidation for smoother experience without full redraw
    // Do not Invalidate the whole window; children will paint as needed
}

LRESULT Win32Window::HandleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_APP + 1: // tray icon message
        {
            switch (lParam) {
            case WM_LBUTTONDBLCLK:
            case WM_LBUTTONUP:
                // restore
                ShowWindow(hwnd, SW_RESTORE);
                ShowWindow(hwnd, SW_SHOW);
                RemoveTrayIcon();
                break;
            case WM_RBUTTONUP:
                // show context menu: Restore / Exit
                {
                    POINT p; GetCursorPos(&p);
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuW(hMenu, MF_STRING, 20001, L"Restore");
                    AppendMenuW(hMenu, MF_STRING, 20002, L"Exit");
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
                RECT rcSub = {0};
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
                    hInplaceEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf,
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL,
                        pt.x, pt.y, rcEdit.right - rcEdit.left, rcEdit.bottom - rcEdit.top,
                        hContentWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(10001)), hInstance, NULL);
                    if (hInplaceEdit) {
                        SendMessageW(hInplaceEdit, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
                        // Explicitly clear read-only just in case
                        SendMessageW(hInplaceEdit, EM_SETREADONLY, FALSE, 0);
                        // Store subitem being edited
                        this->inplaceEditSubitem = sub;
                        // Save original text so we can detect whether user actually changed it
                        this->inplaceEditOriginalText = std::wstring(buf);
                        SetWindowPos(hInplaceEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
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
    case WM_CLOSE:
        // 禁用在連接或連接中關閉視窗
        if (disconnecting.load() || workerRunning.load() || (etherCATMaster && etherCATMaster->IsInitialized())) {
            MessageBoxW(hwnd, L"連接中或已連接，無法關閉視窗。請先斷開連接。", L"警告", MB_OK | MB_ICONWARNING);
            return 0;
        }
        // 正常關閉視窗
        DestroyWindow(hwnd);
        return 0;
    case WM_USER_POST_UTF8_LOG: {
        // lParam is a pointer to a heap-allocated std::string (UTF-8)
        std::string* s = reinterpret_cast<std::string*>(lParam);
        if (s) {
            // Check for special commands
            if (*s == "ENABLE_CONTROLS\n") {
                // Always update connect/disconnect state
                EnableWindow(hBtnConnect, FALSE);
                EnableWindow(hBtnDisconnect, TRUE);
                // Only enable SDO-related buttons if an ESI was successfully parsed
                // and the EtherCAT master is initialized (connected).
                bool hasESI = !currentObjects.empty();
                bool isConnected = (etherCATMaster && etherCATMaster->IsInitialized());
                EnableWindow(hBtnReadSDO, (hasESI && isConnected) ? TRUE : FALSE);
                EnableWindow(hBtnWriteSDO, (hasESI && isConnected) ? TRUE : FALSE);
                EnableWindow(hBtnReadAllSDO, (hasESI && isConnected) ? TRUE : FALSE);
                // Save Params button is enabled under the same conditions as other SDO actions
                if (hBtnSaveParams) EnableWindow(hBtnSaveParams, (hasESI && isConnected) ? TRUE : FALSE);
                // Mailbox send can be enabled independently
                EnableWindow(hBtnSendMailbox, TRUE);

                // Re-enable other UI controls that may have been disabled during long operations
                if (hBtnBrowseESI) EnableWindow(hBtnBrowseESI, TRUE);
                // Import Params button should be enabled only when we have an ESI and no long operation running
                if (hBtnImportParams) EnableWindow(hBtnImportParams, (hasESI ? TRUE : FALSE));
                // Only enable Dump ESI when connected
                if (hBtnDumpESI) EnableWindow(hBtnDumpESI, isConnected ? TRUE : FALSE);
                if (hBtnClearStatus) EnableWindow(hBtnClearStatus, TRUE);
                if (hBtnPDOSentStatus) EnableWindow(hBtnPDOSentStatus, TRUE);
                if (hButtonPDOStart) EnableWindow(hButtonPDOStart, TRUE);
                if (hButtonPDOStop) EnableWindow(hButtonPDOStop, TRUE);
                if (hComboSlaves) EnableWindow(hComboSlaves, TRUE);

                // Enable "全部寫入" only when we have an ESI, are connected, and a slave is selected
                bool hasSlave = (confirmedSlave > 0);
                if (hBtnWriteAllSDO) EnableWindow(hBtnWriteAllSDO, (hasESI && isConnected && hasSlave) ? TRUE : FALSE);

                // Reflow main controls now that connection completed so PDO area and list columns
                // adapt to the current client width (even distribution of column widths).
                ReflowMainControls();
                // Diagnostic: report whether motion controls will be enabled after ENABLE_CONTROLS
                {
                    bool hasESI = !currentObjects.empty();
                    bool isConnected = (etherCATMaster && etherCATMaster->IsInitialized());
                    bool hasSlave = (confirmedSlave > 0);
                    bool enableMotion = (hasESI && isConnected && hasSlave);
                    std::string dbg = std::string("MOTION_ENABLE slave=") + std::to_string(confirmedSlave) + (enableMotion ? " TRUE\n" : " FALSE\n");
                    PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string(dbg));
                }

                delete s;
                return 0;
            }
            
            if (*s == "UPDATE_SLAVE_LIST\n") {
                UpdateSlaveList();
                delete s;
                return 0;
            }
            
            if (*s == "SHUTDOWN_COMPLETE\n") {
                CompleteDisconnect();
                delete s;
                return 0;
            }
            if (*s == "STOP_TIMER\n") {
                KillTimer(hwnd, TIMER_ID);
                delete s;
                return 0;
            }
            if (*s == "START_TIMER\n") {
                // Start a UI timer for OnTimer updates (100 ms)
                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                // 顯示PDO面板
                pdoDisplay.Show(true);
                delete s;
                return 0;
            }
            if (*s == "ENABLE_HOME\n") {
                // Only re-enable if homingBusy==false (defensive)
                if (!homingBusy.load()) {
                    if (hBtnHome) EnableWindow(hBtnHome, TRUE);
                }
                delete s;
                return 0;
            }

            std::wstring wmsg = Utf8ToWide(*s);
            PrependStatus(wmsg.c_str());
            delete s;
        }
        return 0;
    }
    case WM_USER_SDO_ITEM_READ: {
        std::string* ps = reinterpret_cast<std::string*>(lParam);
        if (ps) {
            std::wstring wmsg = Utf8ToWide(*ps);
            // split by ':'
            std::vector<std::wstring> parts;
            size_t start = 0;
            while (start < wmsg.size()) {
                size_t pos = wmsg.find(L':', start);
                if (pos == std::wstring::npos) { parts.push_back(wmsg.substr(start)); break; }
                parts.push_back(wmsg.substr(start, pos - start));
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
                ParseTokensToBytes(tokenStr, bytesBE, anyHexTok);
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
                // Build BE hex display from paddedBE (ensures full expected byte length shown)
                std::wstringstream shex;
                shex << std::hex << std::uppercase;
                for (int bi = 0; bi < expectedBytes; ++bi) {
                    if (bi) shex << L" ";
                    shex << L"0x" << std::setw(2) << std::setfill(L'0') << (int)paddedBE[bi];
                }
                std::wstring hexDisplay = shex.str();

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
                                uint32_t raw = 0; for (int b = 0; b < 4; ++b) raw |= (uint32_t)paddedBE[expectedBytes - 1 - b] << (8*b); // careful: paddedBE is BE
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
                PrependStatus(wmsg.c_str());
            }
            delete ps;
        }
        return 0;
    }
    // ... existing cases ...
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_CONNECT:
            OnConnect();
            break;
        case IDC_BTN_DISCONNECT:
            OnDisconnect();
            break;
        case IDC_BTN_SAVE_PARAMS:
            OnSaveParams();
            break;
        case IDC_BTN_IMPORT_PARAMS:
            OnImportParams();
            break;
        case IDC_BTN_WRITE_ALL_SDO:
            OnWriteAllSDO();
            break;
        case IDC_BTN_READ_SDO:
            OnReadSDO();
            break;
        case IDC_BTN_READ_ALL_SDO:
            OnReadAllSDO();
            break;
        case IDC_BTN_WRITE_SDO:
            OnWriteSDO();
            break;
        case IDC_BTN_SEND_MAILBOX:
            OnSendMailbox();
            break;
        case IDC_BTN_DUMP_ESI:
            // Ensure we are connected before attempting to read slave ESI
            if (!(etherCATMaster && etherCATMaster->IsInitialized())) {
                MessageBoxW(hwnd, L"請先連接網路介面卡，完成連線後才能讀取從站 ESI。", L"未連線", MB_OK | MB_ICONWARNING);
                break;
            }
            OnDumpESIForSelectedSlave();
            break;
        case IDC_BTN_BROWSE_ESI:
            OnBrowseESI();
            break;
        case IDC_BTN_HOME:
            // Button click via WM_COMMAND - treat as press
            OnHome(true);
            break;
        case 1032:
            OnClearStatus();
            break;
        case 20001: // Tray Restore
            RemoveTrayIcon();
            ShowWindow(hwnd, SW_RESTORE);
            ShowWindow(hwnd, SW_SHOW);
            break;
        case 20002: // Tray Exit
            DestroyWindow(hwnd);
            break;
        // IDC_COMBO_ESI handling removed (combo disabled)
    case WM_SIZE:
        // Window resized: recalc layout to remove right-side blank space and
        // resize listview/columns to fill the client area.
        ReflowMainControls();
        break;

    case WM_NOTIFY:
        {
            LPNMHDR nm = (LPNMHDR)lParam;
            if (!nm) break;
            // Click/Double-click on SDO ListView -> start in-place edit for Value column (subitem 4)
            if (nm->hwndFrom == hListSDOParams && (nm->code == NM_CLICK || nm->code == NM_DBLCLK)) {
                LPNMITEMACTIVATE lpnmitem = (LPNMITEMACTIVATE)lParam;
                // Use hit-test to robustly detect subitem under cursor
                LVHITTESTINFO hti = {0};
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
        }
        break;
        // SDO selection is handled via the parameter ListView (hListSDOParams)
        case 2001: // hComboSlaves selection changed
            // Only apply selection when user confirms choice (CBN_SELENDOK)
            if (HIWORD(wParam) == CBN_SELENDOK) {
                // User changed slave selection - always attempt to update PDO display
                LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
                if (sel != CB_ERR) {
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

                    // User confirmed selection: store as confirmedSlave and update display
                    confirmedSlave = slaveIdx;

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


                    // 啟用或停用運動控制按鈕：必須有選定從站且 EtherCAT 已初始化
                    bool enableMotion = (confirmedSlave > 0) && (etherCATMaster && etherCATMaster->IsInitialized());
                    if (hBtnFollowMode) EnableWindow(hBtnFollowMode, enableMotion ? TRUE : FALSE);
                    if (hBtnPowerOn) EnableWindow(hBtnPowerOn, enableMotion ? TRUE : FALSE);
                    if (hBtnPowerOff) EnableWindow(hBtnPowerOff, enableMotion ? TRUE : FALSE);
                    if (hBtnHome) EnableWindow(hBtnHome, enableMotion ? TRUE : FALSE);
                    if (hBtnJogCW) EnableWindow(hBtnJogCW, enableMotion ? TRUE : FALSE);
                    if (hBtnJogCCW) EnableWindow(hBtnJogCCW, enableMotion ? TRUE : FALSE);
                    if (hBtnFaultReset) EnableWindow(hBtnFaultReset, enableMotion ? TRUE : FALSE);
                    if (hBtnImmediateStop) EnableWindow(hBtnImmediateStop, enableMotion ? TRUE : FALSE);
                    if (hBtnManualMode) EnableWindow(hBtnManualMode, enableMotion ? TRUE : FALSE);
                    if (hBtnAutoMode) EnableWindow(hBtnAutoMode, enableMotion ? TRUE : FALSE);
                        // Diagnostic: post an UTF-8 log indicating whether motion controls (Home, Power, etc.) are enabled
                        {
                            std::string dbg = std::string("MOTION_ENABLE slave=") + std::to_string(confirmedSlave) + (enableMotion ? " TRUE\n" : " FALSE\n");
                            PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string(dbg));
                        }

                    // If we have a master instance and it's initialized, recalc offsets
                    // and immediately rebuild the PDO UI so the controls are laid out
                    // correctly for the newly selected slave.
            if (etherCATMaster && etherCATMaster->IsInitialized()) {
                        // Recalculate offsets from SDO assignment (may perform SDO reads)
                        if (pdoDisplay.RecalculateOffsetsFromSDO(etherCATMaster, confirmedSlave)) {
                            pdoDisplay.RebuildPDOControls();
                // 以名稱為鍵還原輸入值
                RestorePDOInputsFromGlobal();
                        }
                    }

                    // Take an IOmap snapshot if possible and update display; fallback to N/A view
                    // PDO 更新現在由 OnTimer 自動處理,這裡只需確保顯示區域可見
                    // Ensure PDO area is visible when user confirms a slave
                    pdoDisplay.Show(true);

                    // Update detailed status area for selected slave (SDO-based info)
                    UpdateSelectedSlaveStatus(confirmedSlave);

                    // Update Write All button enabled state: require ESI, connection and selected slave
                    {
                        bool hasESI_local = !currentObjects.empty();
                        bool isConn_local = (etherCATMaster && etherCATMaster->IsInitialized());
                        bool hasSlave_local = (confirmedSlave > 0);
                        if (hBtnWriteAllSDO) EnableWindow(hBtnWriteAllSDO, (hasESI_local && isConn_local && hasSlave_local) ? TRUE : FALSE);
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
                EnableWindow(hButtonPDOStart, FALSE);
                EnableWindow(hButtonPDOStop, TRUE);
            }
            break;
        case IDC_BTN_PDO_STOP:
            PrependStatus(L"PDO Stop requested\r\n");
            EnableWindow(hButtonPDOStart, TRUE);
            EnableWindow(hButtonPDOStop, FALSE);
            break;
        // In-place edit control notifications (control id 10001)
        case 10001:
            if (HIWORD(wParam) == EN_KILLFOCUS || HIWORD(wParam) == EN_UPDATE || HIWORD(wParam) == EN_CHANGE) {
                // We'll handle commit on EN_KILLFOCUS; ignore EN_UPDATE/EN_CHANGE here
                if (HIWORD(wParam) == EN_KILLFOCUS) {
                        if (hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditItem >= 0 && inplaceEditItem < (int)currentObjects.size()) {
                            wchar_t newVal[512] = {0};
                            GetWindowTextW(hInplaceEdit, newVal, (int)_countof(newVal));
                            // If text did not change compared to original, skip updating values/caches
                            std::wstring newStr(newVal);
                            if (newStr == this->inplaceEditOriginalText) {
                                // No change: simply destroy editor and restore selection/focus
                                DestroyWindow(hInplaceEdit);
                                hInplaceEdit = NULL;
                                int wroteItem = inplaceEditItem;
                                inplaceEditItem = -1;
                                // Restore selection/focus
                                ListView_SetItemState(hListSDOParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                                RECT rcPaint = {0};
                                int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
                                if (ListView_GetSubItemRect(hListSDOParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                                    InvalidateRect(hListSDOParams, &rcPaint, TRUE);
                                }
                                // Do not enable write button since nothing changed
                                break;
                            }
                            // Validate input according to ESI dataType before accepting commit
                            if (inplaceEditSubitem != 5) {
                                // if not Value column, skip validation here
                            } else {
                                std::wstring err;
                                if (!ValidateSDOValueInput(inplaceEditItem, newStr, err)) {
                                    // Show error and keep edit active
                                    if (!err.empty()) PrependStatus(err.c_str());
                                    // Return focus to edit and select all text
                                    if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                                        SetFocus(hInplaceEdit);
                                        SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
                                    }
                                    break; // do not commit / destroy editor
                                }
                            }
                            // Treat editing according to column: Value is column 5, Hex is column 6.
                            if (inplaceEditSubitem == 6) {
                                // Hex column edited: normalize and store hex display only; do NOT compute or overwrite Value column
                                std::wstring s(newVal);
                                std::vector<uint8_t> typedBE;
                                bool anyHex = false;
                                ParseTokensToBytes(s, typedBE, anyHex);

                                // If parsing produced bytes, normalize to BE hex tokens; otherwise keep user's raw input trimmed
                                std::wstring hexDisplay;
                                if (anyHex && !typedBE.empty()) {
                                    std::wstringstream s2; s2 << std::hex << std::uppercase;
                                    for (size_t i = 0; i < typedBE.size(); ++i) {
                                        if (i) s2 << L" ";
                                        s2 << L"0x" << std::setw(2) << std::setfill(L'0') << (int)typedBE[i];
                                    }
                                    hexDisplay = s2.str();
                                } else {
                                    // keep user's raw text (trim)
                                    hexDisplay = s;
                                    // trim
                                    while (!hexDisplay.empty() && iswspace(hexDisplay.front())) hexDisplay.erase(hexDisplay.begin());
                                    while (!hexDisplay.empty() && iswspace(hexDisplay.back())) hexDisplay.pop_back();
                                }

                                // Update only Hex column
                                ListView_SetItemText(hListSDOParams, inplaceEditItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
                                if (confirmedSlave > 0) {
                                    auto &hexvec = savedSDOHexValues[confirmedSlave];
                                    if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                                    hexvec[inplaceEditItem] = hexDisplay;
                                }
                            } else {
                                // Value column edited: preserve user's input as-is for Value column and update savedSDOValues only
                                ListView_SetItemText(hListSDOParams, inplaceEditItem, 5, newVal);
                                if (confirmedSlave > 0) {
                                    auto &vec = savedSDOValues[confirmedSlave];
                                    if ((int)vec.size() < (int)currentObjects.size()) vec.resize(currentObjects.size());
                                    vec[inplaceEditItem] = std::wstring(newVal);
                                }
                            }
                        // Destroy edit
                        DestroyWindow(hInplaceEdit);
                        hInplaceEdit = NULL;
                        // Mark no inplace active
                        int wroteItem = inplaceEditItem;
                        inplaceEditItem = -1;
                        // Enable Write button only when we have a valid connection and an ESI loaded
                        {
                            bool canEnableWrite = (etherCATMaster && etherCATMaster->IsInitialized() && !currentObjects.empty());
                            EnableWindow(hBtnWriteSDO, canEnableWrite ? TRUE : FALSE);
                        }
                        // Restore selection/focus and repaint cell to clear any residual highlight
                        ListView_SetItemState(hListSDOParams, wroteItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                        RECT rcPaint = {0};
                        int paintSub = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
                        if (ListView_GetSubItemRect(hListSDOParams, wroteItem, paintSub, LVIR_BOUNDS, &rcPaint)) {
                            InvalidateRect(hListSDOParams, &rcPaint, TRUE);
                        }
                    }
                }
            }
            break;
        case IDC_BTN_PDO_SENT_STATUS:
            if (HIWORD(wParam) == BN_CLICKED) {
                ToggleChildStatusWindow();
            }
            break;
        // Index/Subindex edit handling removed; selection is via ListView
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
                        SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        break;
                    }
                    // Debug: log apply button press
                    {
                        std::wstringstream dbg; dbg << L"[PDO] Apply button clicked: id=" << cmdID << L"\r\n";
                        PrependStatus(dbg.str().c_str());
                    }

                    // 按鈕事件：僅暫停計時器、讀值、寫入、重啟計時器（移除冗長除錯輸出）
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
                            std::wstringstream dd;
                            dd << L"[PDO DEBUG] editableItems (index:name):\r\n";
                            for (size_t ei = 0; ei < editableItems.size(); ++ei) dd << L"  [" << ei << L"] '" << editableItems[ei] << L"'\r\n";
                            dd << L"[PDO DEBUG] pdoDisplay.pdoItems (index:name:addr:offset:bits):\r\n";
                            for (size_t pi = 0; pi < pdoDisplay.pdoItems.size(); ++pi) {
                                const auto &it = pdoDisplay.pdoItems[pi];
                                dd << L"  [" << pi << L"] '" << it.name << L"' addr='" << it.address << L"' offset=" << it.byteOffset << L" bits=" << (int)it.bitLength << L"\r\n";
                            }
                            PrependStatus(dd.str().c_str());
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
                            // [INLINE PDO] diagnostic removed: 未選擇從站
                            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        } else if (!etherCATMaster) {
                            // [INLINE PDO] diagnostic removed: EtherCAT master 為 null
                            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                        } else {
                            if (hEdit && foundIndex >= 0) {
                                // 寫入PDO值到IOMap
                                {
                                    std::wstringstream wlog; 
                                    wlog << L"[PDO] Attempt write slave=" << slaveIdx << L" item='" << itemName << L"' value=" << userInputValue << L"\r\n";
                                    PrependStatus(wlog.str().c_str());
                                }
                                bool writeOk = pdoDisplay.WriteToIOMap(etherCATMaster, slaveIdx, itemName, userInputValue);

                                if (writeOk) {
                                    if (etherCATMaster && etherCATMaster->IsRunning()) {
                                        etherCATMaster->SendProcessDataOnce();
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
                                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                                return 0;
                            } else {
                                // [INLINE PDO] diagnostic removed: 找不到編輯框控件
                                SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                            }
                        }
                    } else {
                        // [INLINE PDO] diagnostic removed: 項目索引超出範圍
                        SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                    }
                    
                    // 確保重新啟動定時器
                    SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, NULL);
                }
                // All PDO commands are handled inline above
            }
            break;
        }
        break;

    case WM_TIMER:
        if (wParam == TIMER_ID) {
            OnTimer();
        } else if (wParam == TIMER_EDIT_FOCUS) {
            KillTimer(hwnd, TIMER_EDIT_FOCUS);
            if (hInplaceEdit && IsWindow(hInplaceEdit)) {
                SetWindowPos(hInplaceEdit, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetFocus(hInplaceEdit);
                SendMessageW(hInplaceEdit, EM_SETSEL, 0, -1);
            }
        }
        break;

    case WM_SIZE:
        {
            // Minimize to tray behavior
            if (wParam == SIZE_MINIMIZED) {
                if (useTray) {
                    minimizedToTray = true;
                    AddTrayIcon();
                    ShowWindow(hwnd, SW_HIDE);
                    return 0; // no further reflow while hidden
                }
            } else if (wParam == SIZE_RESTORED) {
                // remove tray icon on restore
                minimizedToTray = false;
                RemoveTrayIcon();
                ShowWindow(hwnd, SW_SHOW);
            }
            // Reflow adaptive controls after resizing
            ReflowMainControls();  // 重排所有主要控件
            // 若有內嵌編輯，隨著尺寸改變重新定位或關閉
            if (hListSDOParams && IsWindow(hListSDOParams) && hInplaceEdit && IsWindow(hInplaceEdit) && inplaceEditItem >= 0) {
                int item = inplaceEditItem;
                // 檢查是否仍可見
                int top = ListView_GetTopIndex(hListSDOParams);
                int perPage = ListView_GetCountPerPage(hListSDOParams);
                if (perPage <= 0) perPage = 1;
                if (item < top || item >= top + perPage) {
                    // 不可見就提交並關閉
                    PostMessage(hwnd, WM_COMMAND, MAKEWPARAM(10001, EN_KILLFOCUS), (LPARAM)hInplaceEdit);
                } else {
                    RECT rcSub = {0};
                    int subToUse = (inplaceEditSubitem >= 0 ? inplaceEditSubitem : 5);
                    if (ListView_GetSubItemRect(hListSDOParams, item, subToUse, LVIR_BOUNDS, &rcSub)) {
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
            // After reflow, ensure scrollPos remains within new range
            RECT rc; GetClientRect(hwnd, &rc);
            SCROLLINFO si; ZeroMemory(&si, sizeof(si)); si.cbSize = sizeof(si); si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
            GetScrollInfo(hwnd, SB_VERT, &si);
            int maxPos = si.nMax - (int)si.nPage + 1;
            if (maxPos < 0) maxPos = 0;
            if (scrollPos < si.nMin) scrollPos = si.nMin;
            if (scrollPos > maxPos) scrollPos = maxPos;
            si.fMask = SIF_POS; si.nPos = scrollPos; SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
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
    // Only disconnect if not already disconnecting/disconnected
    if (etherCATMaster && etherCATMaster->HasActiveSlaveThreads()) {
        PrependStatus(L"視窗關閉中...\r\n");
        OnDisconnect();
        // Give shutdown thread a moment to start (it's detached so we can't join)
        Sleep(100);
    }
    PrependStatus(L"程式已退出\r\n");
    closePending.store(false);
    PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    return 0;
}

void Win32Window::OnConnect() {
    if (!etherCATMaster) return;
    // 若正在執行斷開程序，請等待完成再連接
    if (disconnecting.load()) {
        PrependStatus(L"請等待斷開完成後再連接\r\n");
        return;
    }

    // Prevent starting multiple workers
    if (workerRunning.load()) {
        PrependStatus(L"連線程序已在執行中，請稍候...\r\n");
        return;
    }

    // 取得網路介面卡名稱 (從下拉選單選取)
    int sel = (int)SendMessageW(hComboAdapter, CB_GETCURSEL, WPARAM(0), 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個網路介面\r\n");
        return;
    }

    // 直接使用 UTF-8 編碼的介面名稱，避免編碼轉換問題
    int textLen = SendMessageW(hComboAdapter, CB_GETLBTEXTLEN, sel, 0);
    if (textLen <= 0) {
    PrependStatus(L"無法取得介面名稱\r\n");
        return;
    }

    std::vector<wchar_t> wText(textLen + 1);
    SendMessageW(hComboAdapter, CB_GETLBTEXT, sel, (LPARAM)wText.data());

    // 從描述中提取 pcap 介面名稱 (在括號中的部分)
    std::wstring fullDesc(wText.data());
    size_t startPos = fullDesc.rfind(L'(');
    size_t endPos = fullDesc.rfind(L')');
    std::string ifname;

    if (startPos != std::wstring::npos && endPos != std::wstring::npos && endPos > startPos) {
        std::wstring pcapPart = fullDesc.substr(startPos + 1, endPos - startPos - 1);
        // Use shared utility to convert wide string to UTF-8 consistently
        if (!pcapPart.empty()) {
            ifname = WideToUtf8(pcapPart);
        }
    }

    if (ifname.empty()) {
    PrependStatus(L"無法解析介面名稱\r\n");
        return;
    }

    // 檢查網路鏈路狀態
    if (!CheckLinkStatus(ifname.c_str())) {
    PrependStatus(L"錯誤：網路介面沒有檢測到鏈路連接。\r\n");
        return;  // 中斷初始化程序
    }

    // Disable connect button now and run entire connect flow in worker thread
    EnableWindow(hBtnConnect, FALSE);
    EnableWindow(hBtnDisconnect, TRUE);
    workerRunning.store(true);
    std::string ifnameCopy = ifname; // copy for thread
    // remember interface for link monitoring
    this->currentIfName = ifnameCopy;

    // Read desired cycle time now and copy
    wchar_t cycleTimeW[32];
    GetWindowTextW(hEditCycleTime, cycleTimeW, 32);
    uint32_t desiredCycleTime = (uint32_t)wcstoul(cycleTimeW, NULL, 10);
    if (desiredCycleTime == 0) desiredCycleTime = 1000;

    // Launch connect flow as detached thread to avoid overwriting joinable member thread
    std::thread([this, ifnameCopy, desiredCycleTime]() {
    // Temporarily increase worker thread priority during init to improve timing
    int prevThreadPrio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        // Helper to post utf8 logs
        auto post = [this](const std::string &s){ PostUTF8LogToUI(s); };

        // Small delay to ensure previous shutdown is complete
        Sleep(500);
        // Abort connect if user requested disconnect
        if (disconnecting.load()) {
            post("Connection aborted by user before initialization\n");
            workerRunning.store(false);
            return;
        }
        
        post(std::string("正在初始化 EtherCAT...\n"));
        etherCATMaster->SetCycleTime(desiredCycleTime);
        if (!etherCATMaster->Initialize(ifnameCopy.c_str())) {
            std::string err = etherCATMaster->GetLastError();
            post(std::string("Initialization failed: ") + err + "\n");
            this->workerRunning.store(false);
            // Re-enable connect button on UI thread
            PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("請檢查網路介面或權限\n"));
            EnableWindow(hBtnConnect, TRUE);
            return;
        }

        post(std::string("Initialization successful!\n"));
        // Abort if disconnect requested immediately after initialization
        if (disconnecting.load()) {
            post("Connection aborted by user after initialization\n");
            workerRunning.store(false);
            return;
        }
        // Update slave list on UI thread
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("Updating slave list...\n"));
        PostMessage(hwnd, WM_APP, 0, 0); // dummy to trigger UI repaint if needed

        // 確保連接過程每個步驟都有足夠的時間，避免連接失敗
        // Abort if disconnect requested before configuring slaves
        if (disconnecting.load()) {
            post("Connection aborted by user before configuring slaves\n");
            workerRunning.store(false);
            return;
        }
        
        // 首先發送一些初始的ProcessData以確保通信穩定
        post(std::string("發送初始ProcessData以準備配置...\n"));
        for (int i = 0; i < 10; ++i) {
            etherCATMaster->SendProcessDataOnce();
            Sleep(10); // 每次發送後短暫等待
        }
        
        post(std::string("配置從站使用SOEM自動配置...\n"));
        bool configOk = etherCATMaster->ConfigureSlaves();
        
        if (!configOk) {
            std::string errMsg = etherCATMaster->GetLastError();
            post(std::string("錯誤: ConfigureSlaves() 失敗!\n"));
            post(std::string("詳細資訊: ") + errMsg + "\n");
            post(std::string("請斷開連接，重新上電所有從站，然後重新連接。\n"));
            PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_CONTROLS\n"));
            EnableWindow(hEditCycleTime, TRUE);
            workerRunning.store(false);
            return;
        }

        int slaveCount = etherCATMaster->GetSlaveCount();
        post(std::string("All ") + std::to_string(slaveCount) + " slaves configured successfully\n");
        
        // Configuration succeeded, continue with cyclic operation
        {
            // Step 2: All slaves configured in SAFE_OP, now start cyclic communication
            // Start cyclic thread and send processdata, then request OP for all slaves together
            post(std::string("All slaves configured, starting cyclic communication...\n"));
            
            // Send initial processdata (SOEM standard practice)
            post(std::string("Sending initial processdata (SOEM standard)...\n"));
            for (int i = 0; i < 50; ++i) {
                // Abort if disconnect requested during initial processdata
                if (disconnecting.load()) {
                    post("Connection aborted by user during initial processdata\n");
                    workerRunning.store(false);
                    return;
                }
                etherCATMaster->SendProcessDataOnce();
                // Delay between each processdata cycle based on UI-configured cycle time
                DWORD delayMs = (DWORD)(desiredCycleTime / 1000);
                if (delayMs == 0) delayMs = 1;
                Sleep(delayMs);
            }
            
            // Step 3: Request OP state for all slaves together
            post(std::string("Requesting OP state for all slaves...\n"));
            // Abort if disconnect requested before starting cyclic operation
            if (disconnecting.load()) {
                post("Connection aborted by user before starting cyclic operation\n");
                workerRunning.store(false);
                return;
            }
            if (!etherCATMaster->StartCyclicOperation()) {
                post(std::string("StartCyclicOperation failed: ") + etherCATMaster->GetLastError() + "\n");
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_CONTROLS\n"));
                EnableWindow(hEditCycleTime, TRUE);
            } else {
                post(std::string("EtherCAT communication active\n"));
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("EtherCAT \u5df2\u555f\u52d5!\n"));
                
                // Update slave list to show connected slaves
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("UPDATE_SLAVE_LIST\n"));
                
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_CONTROLS\n"));
                // Request UI thread to start timer for periodic updates
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("START_TIMER\n"));
                EnableWindow(hEditCycleTime, FALSE);
            }
        }

    // StartCyclicOperation was already attempted above when configuration succeeded.
    // Avoid calling it a second time here to prevent "Already running" errors.

    this->workerRunning.store(false);
    // restore thread priority
    SetThreadPriority(GetCurrentThread(), prevThreadPrio);
    }).detach();
    // Previous workerThread member is unused when using detached thread
}

// 使用 pcap 列出可用的網路介面並填入下拉選單
void Win32Window::PopulateAdapters() {
    // Use helper to enumerate pcap-visible adapters (keeps UI thin)
    std::string err;
    std::vector<AdapterEntry> adapters = EnumerateAdapters(err);

    // Clear combobox
    SendMessageW(hComboAdapter, CB_RESETCONTENT, WPARAM(0), 0);

    if (!err.empty()) {
        std::wstring werr = Utf8ToWide(err);
        if (!werr.empty()) {
            PrependStatus(werr.c_str());
            PrependStatus(L"\r\n");
        }
    }

    bool anyAdded = false;
    for (const auto &d : adapters) {
    std::string entry = MakeAdapterDisplayString(d);

        auto toLowerStr = [](const std::string &s){ std::string r = s; std::transform(r.begin(), r.end(), r.begin(), ::tolower); return r; };
        std::string entryLow = toLowerStr(entry);
        if (entryLow.find("bluetooth") != std::string::npos ||
            entryLow.find("personal area network") != std::string::npos ||
            entryLow.find("wireless") != std::string::npos) {
            continue;
        }

        std::wstring wentry = Utf8ToWide(entry);
        if (!wentry.empty()) {
            SendMessageW(hComboAdapter, CB_ADDSTRING, 0, (LPARAM)wentry.c_str());
            anyAdded = true;
        }
    }

    // Select TwinCAT adapter if present, otherwise first
    int selectedIndex = 0;
    int itemCount = SendMessageW(hComboAdapter, CB_GETCOUNT, WPARAM(0), 0);
    for (int i = 0; i < itemCount; ++i) {
        wchar_t itemText[512] = {0};
        SendMessageW(hComboAdapter, CB_GETLBTEXT, i, (LPARAM)itemText);
        std::wstring itemStr(itemText);
        if (itemStr.find(L"TwinCAT") != std::wstring::npos) { selectedIndex = i; break; }
    }
    SendMessageW(hComboAdapter, CB_SETCURSEL, WPARAM(selectedIndex), 0);
}

// CheckLinkStatus moved to Win32Utils.cpp

void Win32Window::OnDisconnect() {
    if (!etherCATMaster) return;
    
    // Prevent duplicate disconnect operations
    if (disconnecting.exchange(true)) {
        return;  // Already disconnecting, skip
    }

    // If worker thread is running, wait for it to finish
    // No join needed since worker thread is detached, just flag running false
    if (workerRunning.load()) {
        PrependStatus(L"等待連線程序結束中...\r\n");
        workerRunning.store(false);
    }

    // Only show shutdown message if EtherCAT has active slave threads
    bool hasThreads = etherCATMaster->HasActiveSlaveThreads();
    if (hasThreads) {
        PrependStatus(L"正在安全退出所有裝置...\r\n");
    }
    
    KillTimer(hwnd, TIMER_ID);
    
    // Shutdown can take time (stopping multiple threads)
    // Immediately update UI controls, then perform shutdown in background
    if (hasThreads) {
        // Reset UI for disconnect
        CompleteDisconnect();
        // Run shutdown in background to cleanup resources and log progress
        std::thread([this]() {
            etherCATMaster->Shutdown([this](const std::string& msg) {
                PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string(msg));
            });
            // Notify when devices back to initial state
            PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("裝置已回到初始狀態\n"));
        }).detach();
        return;
    }
    // No active threads: synchronous shutdown and UI reset
    etherCATMaster->Shutdown();
    CompleteDisconnect();
}

void Win32Window::CompleteDisconnect() {
    PrependStatus(L"已斷開連接\r\n");

    // clear monitored interface name
    this->currentIfName.clear();

    EnableWindow(hBtnConnect, TRUE);
    EnableWindow(hBtnDisconnect, FALSE);
    EnableWindow(hBtnReadSDO, FALSE);
    EnableWindow(hBtnWriteSDO, FALSE);
    EnableWindow(hBtnReadAllSDO, FALSE);
    if (hBtnSaveParams) EnableWindow(hBtnSaveParams, FALSE);
    EnableWindow(hBtnSendMailbox, FALSE);

    // Enable cycle time edit when disconnected
    EnableWindow(hEditCycleTime, TRUE);

    ListView_DeleteAllItems(hListSlaves);
    // Reset slave combobox
    SendMessageW(hComboSlaves, CB_RESETCONTENT, 0, 0);
    // Removed N/A placeholder in slave list after disconnect
    
    // Reset disconnecting flag
    disconnecting.store(false);
}


void Win32Window::OnReadSDO() {
    if (!etherCATMaster) return;
    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Get selected parameter from ListView
    if (!hListSDOParams) {
        PrependStatus(L"SDO 參數表未建立\r\n");
        return;
    }
    int selItem = ListView_GetNextItem(hListSDOParams, -1, LVNI_SELECTED);
    if (selItem < 0 || selItem >= (int)currentObjects.size()) {
        PrependStatus(L"請先在參數表中選擇一個參數\r\n");
        return;
    }
    uint16_t index = currentObjects[selItem].index;
    uint8_t subindex = currentObjects[selItem].subindex;

    // Prepare textual index/subindex for debug output
    wchar_t indexW[32] = {0};
    wchar_t subindexW[32] = {0};
    swprintf_s(indexW, L"0x%04X", index);
    swprintf_s(subindexW, L"0x%02X", subindex);

    // Read SDO
    uint8_t buf[256] = {0};
    int size = sizeof(buf);
    if (etherCATMaster->ReadSDO(slave, index, subindex, buf, size)) {
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 讀取成功 - Index: 0x"
           << std::hex << std::uppercase << index
           << L" Subindex: 0x" << (int)subindex
           << std::dec << L" 長度: " << size << L" bytes\r\n";
        // 顯示位元組（LE 與 BE 表示）
        ss << L"  LE 位元組: " << BytesToHexDisplay(buf, size, false, true, true, true) << L"\r\n";
        ss << L"  BE 位元組: " << BytesToHexDisplay(buf, size, true, true, true, true) << L"\r\n";
        // 顯示整數（LE 解析）
    ss << L"  整數(LE 解析): ";
    // 以 64-bit 為主輸出（若 size>8，僅截取低 64 位）
    unsigned long long v64 = 0;
    int maxBytes = (size < 8 ? size : 8);
        for (int i = 0; i < maxBytes; ++i) v64 |= (unsigned long long)buf[i] << (8 * i);
        ss << std::dec << v64 << L" (0x" << std::hex << std::uppercase << v64 << L")\r\n";
        PrependStatus(ss.str().c_str());

        // Format display and hex using centralized helper to keep UI thin
    {
        const EsiObjectEntry* pobj = nullptr;
        if (selItem >= 0 && selItem < (int)currentObjects.size()) pobj = &currentObjects[selItem];
        auto formatted = FormatSDOValue(buf, size, pobj);
        const std::wstring &valueDisplay = formatted.first;
        const std::wstring &hexDisplay = formatted.second;

        // Update ListView columns
        if (hListSDOParams && IsWindow(hListSDOParams)) {
            ListView_SetItemText(hListSDOParams, selItem, 5, const_cast<LPWSTR>(valueDisplay.c_str()));
            ListView_SetItemText(hListSDOParams, selItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
        }

        // Update caches
        {
            auto &vec = savedSDOValues[slave];
            if ((int)vec.size() < (int)currentObjects.size()) vec.resize(currentObjects.size());
            vec[selItem] = valueDisplay;
            auto &hexvec = savedSDOHexValues[slave];
            if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
            hexvec[selItem] = hexDisplay;
        }
    }
    } else {
        std::string error = etherCATMaster->GetLastError();
        std::wstring errorW(error.begin(), error.end());
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 讀取失敗 - " << errorW << L"\r\n";
        PrependStatus(ss.str().c_str());
    }
}

void Win32Window::OnReadAllSDO()
{
    if (!etherCATMaster) return;
    if (readAllRunning.load()) {
        PrependStatus(L"全部讀取已在執行中，請稍候...");
        return;
    }
    // Ensure a slave is selected
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) { PrependStatus(L"請先選擇一個從站\r\n"); return; }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Mark running and disable button to avoid re-entry
    readAllRunning.store(true);
    EnableWindow(hBtnReadAllSDO, FALSE);

    // Clear UI value column immediately to reflect fresh read-all operation
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
                            ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(L""));
                            ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
        }
    }
    // Clear any cached saved values for this slave so previous values don't reappear
    savedSDOValues[slave].clear();

    // Disable most UI controls to avoid concurrent commands while read-all is running
    if (hBtnReadSDO) EnableWindow(hBtnReadSDO, FALSE);
    if (hBtnWriteSDO) EnableWindow(hBtnWriteSDO, FALSE);
    if (hBtnSaveParams) EnableWindow(hBtnSaveParams, FALSE);
    if (hBtnSendMailbox) EnableWindow(hBtnSendMailbox, FALSE);
    if (hBtnBrowseESI) EnableWindow(hBtnBrowseESI, FALSE);
    if (hBtnImportParams) EnableWindow(hBtnImportParams, FALSE);
    if (hBtnDumpESI) EnableWindow(hBtnDumpESI, FALSE);
    if (hBtnClearStatus) EnableWindow(hBtnClearStatus, FALSE);
    if (hBtnConnect) EnableWindow(hBtnConnect, FALSE);
    if (hBtnDisconnect) EnableWindow(hBtnDisconnect, FALSE);
    if (hButtonPDOStart) EnableWindow(hButtonPDOStart, FALSE);
    if (hButtonPDOStop) EnableWindow(hButtonPDOStop, FALSE);
    if (hBtnPDOSentStatus) EnableWindow(hBtnPDOSentStatus, FALSE);
    // Also disable ESI and slave combos to prevent changing context mid-read
    if (hComboSlaves) EnableWindow(hComboSlaves, FALSE);

    // Launch background reader thread
    std::thread([this, slave]() {
        // Lower thread priority so it doesn't preempt real-time PDO threads
        int prevPrio = GetThreadPriority(GetCurrentThread());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if (!readAllRunning.load() || disconnecting.load()) break;
            uint16_t idx = currentObjects[i].index;
            uint8_t sub = currentObjects[i].subindex;
            uint8_t buf[256] = {0}; int size = sizeof(buf);
            bool ok = etherCATMaster->ReadSDO((int)slave, idx, sub, buf, size);
            if (ok) {
                // format BE hex string (human friendly)
                std::ostringstream oss;
                oss << std::hex << std::uppercase;
                for (int b = size - 1; b >= 0; --b) {
                    oss << "0x" << std::setw(2) << std::setfill('0') << (int)buf[b];
                    if (b > 0) oss << ' ';
                }
                std::string s = std::string("ITEM:") + std::to_string(slave) + ":" + std::to_string(idx) + ":" + std::to_string(sub) + ":" + oss.str();
                // send as utf8
                PostMessage(hwnd, WM_USER_SDO_ITEM_READ, 0, (LPARAM)new std::string(s));
            } else {
                std::string err = etherCATMaster->GetLastError();
                std::string s = std::string("ITEM_ERR:") + std::to_string(slave) + ":" + std::to_string(idx) + ":" + std::to_string(sub) + ":" + err;
                PostMessage(hwnd, WM_USER_SDO_ITEM_READ, 0, (LPARAM)new std::string(s));
            }
            // Small sleep to yield to PDO and other threads; tuned low to keep reasonable speed
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // restore
        SetThreadPriority(GetCurrentThread(), prevPrio);
        // notify UI that operation finished
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("SDO 全部讀取完成\n"));
        // re-enable on UI thread
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_CONTROLS\n"));
        // clear running flag and re-enable button via UI thread in Post handler
        // But also post a direct message to re-enable the button: Use a small lambda via heap string marker
        // We'll use a special message in WM_USER_POST_UTF8_LOG to re-enable controls.
        readAllRunning.store(false);
        return;
    }).detach();
}

void Win32Window::OnWriteSDO() {
    if (!etherCATMaster) return;

    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // 取得選中的參數（從 SDO 參數表）並使用其 Index/Subindex
    wchar_t indexW[32] = {0}, subindexW[32] = {0}, dataW[256] = {0};
    if (!hListSDOParams) {
        PrependStatus(L"SDO 參數表未建立\r\n");
        return;
    }
    int selItem = ListView_GetNextItem(hListSDOParams, -1, LVNI_SELECTED);
    if (selItem < 0 || selItem >= (int)currentObjects.size()) {
        PrependStatus(L"請先在參數表中選擇一個參數\r\n");
        return;
    }
    uint16_t index = currentObjects[selItem].index;
    uint8_t subindex = currentObjects[selItem].subindex;
    swprintf_s(indexW, L"0x%04X", index);
    swprintf_s(subindexW, L"0x%02X", subindex);
    // Read existing Value column text (if user previously read/edited it)
    ListView_GetItemText(hListSDOParams, selItem, 5, dataW, (int)_countof(dataW));
    // 解析輸入的資料，可接受 0x.. 0x.. 串列或十進制
    uint8_t wbuf[256] = {0};
    int wlen = 0;
    {
        // Basic DataType validation: ensure input roughly matches ESI dataType
        std::string dtype = currentObjects[selItem].dataType;
        std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        // If BOOL, accept only 0/1 or true/false
        if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
            std::wstring sdata(dataW);
            std::wstring trimmed = sdata;
            // trim
            auto is_space = [](wchar_t c){ return iswspace(c); };
            while (!trimmed.empty() && is_space(trimmed.front())) trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && is_space(trimmed.back())) trimmed.pop_back();
            if (trimmed.empty()) { PrependStatus(L"BOOL 欄位不能為空，請輸入 0 或 1\r\n"); return; }
            if (!(trimmed == L"0" || trimmed == L"1" || _wcsicmp(trimmed.c_str(), L"TRUE") == 0 || _wcsicmp(trimmed.c_str(), L"FALSE") == 0)) {
                PrependStatus(L"輸入錯誤: 布林欄位僅接受 0/1 或 true/false\r\n"); return;
            }
        }
        // 先嘗試以空白分隔的 hex bytes（視為 BE，高位在前），內部轉為 LE 寫入
        std::wstring s(dataW);
        std::wstringstream ss(s);
        std::wstring tok;
        bool anyHex = false;
        std::vector<uint8_t> typedBE;
        while (ss >> tok) {
            if (tok.rfind(L"0x",0)==0 || tok.rfind(L"0X",0)==0) {
                unsigned v = 0; swscanf_s(tok.c_str(), L"0x%x", &v);
                typedBE.push_back((uint8_t)(v & 0xFF));
                anyHex = true;
            }
        }
        if (anyHex) {
            // BE -> LE
            size_t beCount = typedBE.size();
            if (beCount > sizeof(wbuf)) beCount = sizeof(wbuf);
            wlen = (int)beCount;
            for (int i = 0; i < wlen; ++i) {
                wbuf[i] = typedBE[(int)beCount - 1 - i];
            }
        }
        if (!anyHex) {
            // 依據 ESI 的 bitlen 決定位元組數，預設 4 bytes（小端）
            int expectedBytes = 4;
            for (const auto &obj : currentObjects) {
                if (obj.index == index && obj.subindex == subindex && obj.bitlen > 0) {
                    expectedBytes = (obj.bitlen + 7) / 8;
                    break;
                }
            }
            if (expectedBytes < 1) expectedBytes = 1; if (expectedBytes > 8) expectedBytes = 8;
            uint64_t v = (uint64_t)wcstoull(dataW, NULL, 0);
            // Enforce ESI bitlen capacity and truncate if necessary
            uint64_t maxValW = 0ULL;
            if (expectedBytes >= 8) {
                maxValW = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                maxValW = ((1ULL << (8 * expectedBytes)) - 1ULL);
            }
            if (v > maxValW) {
                std::wstringstream warn; warn << L"警告: 輸入值超出 ESI 定義的 " << expectedBytes << L" bytes，將以低位截斷。\r\n";
                PrependStatus(warn.str().c_str());
                v &= maxValW;
            }
            for (int i = 0; i < expectedBytes; ++i) {
                wbuf[i] = (uint8_t)((v >> (8*i)) & 0xFF);
            }
            wlen = expectedBytes;
        }
    }

    // Write SDO
    if (etherCATMaster->WriteSDO(slave, index, subindex, wbuf, wlen)) {
        // 在狀態欄顯示寫入成功信息，不彈跳視窗
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 寫入成功 - Index: 0x" 
           << std::hex << std::uppercase << index 
           << L" Subindex: 0x" << (int)subindex 
           << L" 長度: " << std::dec << wlen << L" bytes  實際送出(LE): ";
        ss << std::hex << std::uppercase;
        for (int i = 0; i < wlen; ++i) {
            ss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)wbuf[i];
            if (i + 1 < wlen) ss << L" ";
        }
        // 顯示 LE 解析數值
    unsigned long long v64w = 0; for (int i = 0; i < (wlen < 8 ? wlen : 8); ++i) v64w |= (unsigned long long)wbuf[i] << (8*i);
        ss << L"  | 整數(LE 解析)= " << std::dec << v64w << L" (0x" << std::hex << std::uppercase << v64w << L")\r\n";
        PrependStatus(ss.str().c_str());

        // 立刻讀回確認
    // 小延遲，讓裝置處理寫入
    Sleep(15);
    uint8_t rbuf[256] = {0};
    int rlen = sizeof(rbuf);
        if (etherCATMaster->ReadSDO(slave, index, subindex, rbuf, rlen)) {
            std::wstringstream rss;
            rss << L"[從站 " << slave << L"] 寫入後讀回 - Index: 0x" 
                << std::hex << std::uppercase << index 
                << L" Subindex: 0x" << (int)subindex 
                << std::dec << L" 長度: " << rlen << L" bytes\r\n";
            // LE bytes
            rss << L"  LE 位元組: ";
            rss << std::hex << std::uppercase;
            for (int i = 0; i < rlen; ++i) {
                rss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                if (i + 1 < rlen) rss << L" ";
            }
            rss << L"\r\n";
            // BE bytes
            rss << L"  BE 位元組: ";
            for (int i = rlen - 1; i >= 0; --i) {
                rss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                if (i > 0) rss << L" ";
            }
            rss << L"\r\n";
            // 整數解析（LE）
            unsigned long long v64r = 0; for (int i = 0; i < (rlen < 8 ? rlen : 8); ++i) v64r |= (unsigned long long)rbuf[i] << (8*i);
            rss << L"  整數(LE 解析): " << std::dec << v64r << L" (0x" << std::hex << std::uppercase << v64r << L")\r\n";

            // 比對（以 LE 位元組 compare，長度取 min）
            int cmpLen = (wlen < rlen ? wlen : rlen);
            bool eq = (cmpLen > 0 && memcmp(wbuf, rbuf, cmpLen) == 0 && wlen == rlen);
            rss << L"  比對: " << (eq ? L"相同" : L"不同") << L"（以 LE 位元組逐位比較" << (wlen == rlen ? L"、長度相同" : L"、長度不同") << L"）\r\n";
            if (!eq) {
                rss << L"  提示: 若為 0x60FF 等馬達目標值，需確認 0x6060 模式與 0x6040 狀態允許；部分裝置 SDO 回讀不反映 PDO 目標。\r\n";
                // 進一步讀取常見 CiA402 狀態以協助診斷
                auto read_u32 = [&](uint16_t idx, uint8_t sub, unsigned long long &out, int &lenOut) -> bool {
                    uint8_t tmp[8] = {0}; int L = sizeof(tmp);
                    bool ok = etherCATMaster->ReadSDO(slave, idx, sub, tmp, L);
                    if (!ok) return false; lenOut = L; out = 0ULL; int mx = (L < 8 ? L : 8);
                    for (int i = 0; i < mx; ++i) out |= (unsigned long long)tmp[i] << (8*i);
                    return true;
                };
                unsigned long long mo = 0, md = 0, sw = 0, ec = 0, vact = 0; int lmo=0,lmd=0,lsw=0,lec=0,lv=0;
                read_u32(0x6060, 0x00, mo, lmo); // Mode of Operation (commanded)
                read_u32(0x6061, 0x00, md, lmd); // Mode Display (actual)
                read_u32(0x6041, 0x00, sw, lsw); // Statusword
                read_u32(0x603F, 0x00, ec, lec); // Error Code
                read_u32(0x606C, 0x00, vact, lv); // Velocity Actual Value
                rss << L"  診斷: 0x6060(MOO)=" << mo << L" 0x6061(MOD)=" << md
                    << L" 0x6041(SW)=0x" << std::hex << std::uppercase << sw << std::dec
                    << L"  0x603F(ErrorCode)=0x" << std::hex << std::uppercase << ec << std::dec
                    << L"  0x606C(VelAct)=" << vact << L"\r\n";
            }
            PrependStatus(rss.str().c_str());
            // Update caches for Hex only; do NOT overwrite the user's Value input nor savedSDOValues here
            {
                // Build BE hex display from readback buffer (rbuf)
                std::wstringstream s2; s2 << std::hex << std::uppercase;
                for (int i = rlen - 1; i >= 0; --i) {
                    s2 << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                    if (i > 0) s2 << L" ";
                }
                std::wstring hexDisplay = s2.str();

                auto &hexvec = savedSDOHexValues[slave];
                if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                hexvec[selItem] = hexDisplay;

                // Update only Hex column in the ListView, keep Value column as user-entered
                if (hListSDOParams && IsWindow(hListSDOParams)) {
                    ListView_SetItemText(hListSDOParams, selItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
                }
            }
        } else {
            std::string err = etherCATMaster->GetLastError();
            std::wstring errW(err.begin(), err.end());
            std::wstringstream rss;
            rss << L"[從站 " << slave << L"] 寫入後讀回失敗 - " << errW << L"\r\n";
            PrependStatus(rss.str().c_str());
        }
    } else {
        std::string error = etherCATMaster->GetLastError();
        std::wstring errorW(error.begin(), error.end());
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 寫入失敗 - " << errorW << L"\r\n";
        PrependStatus(ss.str().c_str());
    }
}

void Win32Window::OnWriteAllSDO()
{
    if (!etherCATMaster) return;
    // Ensure a slave is selected
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
    MessageBoxW(hwnd, L"請先選擇一個從站\r\n", L"未選擇從站", MB_OK | MB_ICONWARNING);
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Disable UI controls to avoid concurrent operations
    if (hBtnReadSDO) EnableWindow(hBtnReadSDO, FALSE);
    if (hBtnWriteSDO) EnableWindow(hBtnWriteSDO, FALSE);
    if (hBtnReadAllSDO) EnableWindow(hBtnReadAllSDO, FALSE);
    if (hBtnSaveParams) EnableWindow(hBtnSaveParams, FALSE);
    if (hBtnImportParams) EnableWindow(hBtnImportParams, FALSE);
    if (hBtnWriteAllSDO) EnableWindow(hBtnWriteAllSDO, FALSE);

    // Launch background thread to perform writes
    std::thread([this, slave]() {
        PostUTF8LogToUI(std::string("開始全部寫入 (RW) 參數...\n"));
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if (!etherCATMaster) break;
            const auto &obj = currentObjects[i];
            // Determine if object is writable (access contains 'w' or 'W')
            std::string access = obj.access; std::transform(access.begin(), access.end(), access.begin(), [](unsigned char c){ return (char)std::tolower(c); });
            if (access.find('w') == std::string::npos) continue;

            // Prepare buffer to write: prefer savedSDOHexValues if present, otherwise try savedSDOValues
            std::vector<uint8_t> writeLE;
            bool haveData = false;
            auto itHexs = savedSDOHexValues.find(slave);
            if (itHexs != savedSDOHexValues.end() && i < itHexs->second.size() && !itHexs->second[i].empty()) {
                std::vector<uint8_t> bytesBE; bool any = false;
                ParseTokensToBytes(itHexs->second[i], bytesBE, any);
                // Convert BE -> LE for write buffer
                int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : (int)bytesBE.size();
                if (expectedBytes < 1) expectedBytes = (int)bytesBE.size();
                if (expectedBytes < 1) expectedBytes = 1;
                // pad left (BE) then convert to LE
                std::vector<uint8_t> paddedBE(expectedBytes, 0);
                if ((int)bytesBE.size() >= expectedBytes) {
                    int startIdx = (int)bytesBE.size() - expectedBytes;
                    for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                } else {
                    int pad = expectedBytes - (int)bytesBE.size();
                    for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                }
                writeLE.resize(expectedBytes);
                for (int b = 0; b < expectedBytes; ++b) writeLE[b] = paddedBE[expectedBytes - 1 - b];
                haveData = true;
            }

            if (!haveData) {
                // try savedSDOValues decimal string
                auto itVals = savedSDOValues.find(slave);
                if (itVals != savedSDOValues.end() && i < itVals->second.size() && !itVals->second[i].empty()) {
                    // parse as either hex tokens or decimal according to ESI
                    std::wstring s = itVals->second[i];
                    std::vector<uint8_t> bytesBE; bool anyHex=false;
                    // reuse ParseTokensToBytes via wide->string conversion
                    ParseTokensToBytes(s, bytesBE, anyHex);
                    if (anyHex && !bytesBE.empty()) {
                        int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : (int)bytesBE.size();
                        if (expectedBytes < 1) expectedBytes = (int)bytesBE.size();
                        std::vector<uint8_t> paddedBE(expectedBytes, 0);
                        if ((int)bytesBE.size() >= expectedBytes) {
                            int startIdx = (int)bytesBE.size() - expectedBytes;
                            for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                        } else {
                            int pad = expectedBytes - (int)bytesBE.size();
                            for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                        }
                        writeLE.resize(expectedBytes);
                        for (int b = 0; b < expectedBytes; ++b) writeLE[b] = paddedBE[expectedBytes - 1 - b];
                        haveData = true;
                    } else {
                        // try decimal parsing into LE bytes
                        unsigned long long v = 0;
                        try { v = std::stoull(std::string(s.begin(), s.end())); } catch(...) { v = 0; }
                        int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : 4;
                        if (expectedBytes < 1) expectedBytes = 1; if (expectedBytes > 8) expectedBytes = 8;
                        writeLE.resize(expectedBytes);
                        for (int b = 0; b < expectedBytes; ++b) writeLE[b] = (uint8_t)((v >> (8*b)) & 0xFF);
                        haveData = true;
                    }
                }
            }

            if (!haveData) {
                // nothing to write for this object
                continue;
            }

            // Perform SDO write (LE buffer) - WriteSDO expects LE bytes
            bool ok = etherCATMaster->WriteSDO((int)slave, obj.index, obj.subindex, writeLE.data(), (int)writeLE.size());
            if (ok) {
                std::wstringstream ss; ss << L"寫入成功: 0x" << std::hex << std::uppercase << obj.index << L":" << (int)obj.subindex << L"\r\n";
                PostUTF8LogToUI(WideToUtf8(ss.str()));
                // Update cache and UI for this slave
                auto &hexvec = savedSDOHexValues[slave]; if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                std::wstringstream shex; shex << std::hex << std::uppercase;
                for (int b = (int)writeLE.size()-1; b >= 0; --b) { if (b < (int)writeLE.size()-1) shex << L" "; shex << L"0x" << std::setw(2) << std::setfill(L'0') << (int)writeLE[b]; }
                hexvec[i] = shex.str();
                auto &valvec = savedSDOValues[slave]; if ((int)valvec.size() < (int)currentObjects.size()) valvec.resize(currentObjects.size());
                // compute decimal LE integer
                unsigned long long u = 0; for (int b = 0; b < (int)writeLE.size() && b < 8; ++b) u |= (unsigned long long)writeLE[b] << (8*b);
                valvec[i] = std::to_wstring(u);
                if (confirmedSlave == slave && hListSDOParams && IsWindow(hListSDOParams)) {
                    ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(hexvec[i].c_str()));
                    ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(valvec[i].c_str()));
                }
            } else {
                std::string err = etherCATMaster->GetLastError();
                PostUTF8LogToUI(std::string("寫入失敗: ") + err + "\n");
            }

            // small delay to avoid hammering the network
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        PostUTF8LogToUI(std::string("全部寫入完成\n"));
        // Re-enable controls on UI thread
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_CONTROLS\n"));
        return;
    }).detach();
}

void Win32Window::OnSendMailbox() {
    if (!etherCATMaster) return;

    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // 取得 Mailbox 資料
    wchar_t mailboxDataW[512];
    GetWindowTextW(hEditMailbox, mailboxDataW, 512);

    // 轉換 hex 字串為 bytes
    // 簡化範例：假設輸入格式為 "01 02 03 04"
    uint8_t data[256];
    int dataSize = 0;

    wchar_t* context = NULL;
    wchar_t* token = wcstok_s(mailboxDataW, L" ", &context);
    while (token != NULL && dataSize < 256) {
        data[dataSize++] = (uint8_t)wcstoul(token, NULL, 16);
        token = wcstok_s(NULL, L" ", &context);
    }

    if (dataSize > 0) {
        if (etherCATMaster->SendMailbox(slave, data, dataSize)) {
            PrependStatus(L"Mailbox sent successfully\r\n");
        } else {
            std::string error = etherCATMaster->GetLastError();
            std::wstring errorW(error.begin(), error.end());
            PrependStatus(errorW.c_str());
            PrependStatus(L"\r\n");
        }
    }
}

void Win32Window::UpdateSlaveList() {
    if (!etherCATMaster) return;
    if (minimizedToTray) return; // skip heavy UI updates when minimized to tray

    ListView_DeleteAllItems(hListSlaves);
    
    // 同時更新從站下拉選單
    SendMessageW(hComboSlaves, CB_RESETCONTENT, 0, 0);

    auto slaves = etherCATMaster->GetSlaveInfo();
    
    // If no slaves found, keep the ListView and combobox empty (no placeholder entries)
    if (slaves.empty()) {
        // Ensure combobox has no selection
        SendMessageW(hComboSlaves, CB_SETCURSEL, (WPARAM)CB_ERR, 0);
        return;
    }

    for (const auto& slave : slaves) {
        LVITEM lvi = { 0 };
        lvi.mask = LVIF_TEXT;
        lvi.iItem = slave.position - 1;

    // 位置
    wchar_t posBuf[32];
    swprintf_s(posBuf, L"%d", slave.position);
    lvi.pszText = posBuf;
    lvi.iSubItem = 0;
    ListView_InsertItem(hListSlaves, &lvi);

        // 名稱 - 嘗試讀取實際設備名稱
        std::wstring nameW;
        if (etherCATMaster->IsRunning()) {
            char deviceName[128] = {0};
            int nameSize = sizeof(deviceName);
                if (etherCATMaster->ReadSDO(slave.position, 0x1008, 0x00, deviceName, nameSize)) {
                // 成功讀取設備名稱，轉換為寬字元
                nameW = Utf8ToWide(std::string(deviceName));
            }
        }
        // 如果沒有讀到設備名稱，使用 SOEM 識別的名稱
        if (nameW.empty()) {
            nameW = std::wstring(slave.name.begin(), slave.name.end());
        }
        ListView_SetItemText(hListSlaves, lvi.iItem, 1, (LPWSTR)nameW.c_str());

    // Vendor ID
    wchar_t buf[64];
    swprintf_s(buf, L"0x%08X", (unsigned int)slave.vendorId);
    ListView_SetItemText(hListSlaves, lvi.iItem, 2, buf);

    // Product Code
    swprintf_s(buf, L"0x%08X", (unsigned int)slave.productCode);
    ListView_SetItemText(hListSlaves, lvi.iItem, 3, buf);

    // 狀態
    swprintf_s(buf, L"0x%02X", (unsigned int)slave.state);
    ListView_SetItemText(hListSlaves, lvi.iItem, 4, buf);

    // Outputs
    swprintf_s(buf, L"%d", (int)slave.outputBits);
    ListView_SetItemText(hListSlaves, lvi.iItem, 5, buf);

    // Inputs
    swprintf_s(buf, L"%d", (int)slave.inputBits);
    ListView_SetItemText(hListSlaves, lvi.iItem, 6, buf);
    }

    // Populate slave combobox and select first
    // 嘗試讀取每個從站的實際設備名稱
    for (const auto& s : slaves) {
        std::wstring displayName;
        
        // 嘗試從從站讀取設備名稱 (0x1008:00)
        if (etherCATMaster->IsRunning()) {
            char deviceName[128] = {0};
            int nameSize = sizeof(deviceName);
            if (etherCATMaster->ReadSDO(s.position, 0x1008, 0x00, deviceName, nameSize)) {
                // 成功讀取設備名稱，轉換為寬字元
                displayName = Utf8ToWide(std::string(deviceName));
            }
        }
        
        // 如果沒有讀到設備名稱，使用 SOEM 識別的名稱
        if (displayName.empty()) {
            displayName = std::wstring(s.name.begin(), s.name.end());
        }
        
        // 格式化顯示：站號: 設備名稱
        std::wstringstream ss;
        ss << s.position << L": " << displayName;
        int addIdx = (int)SendMessageW(hComboSlaves, CB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
        // Bind the real slave position to the combobox item data so selection -> position mapping is robust
        SendMessageW(hComboSlaves, CB_SETITEMDATA, (WPARAM)addIdx, (LPARAM)s.position);
    }
    if (!slaves.empty()) {
        SendMessageW(hComboSlaves, CB_SETCURSEL, 0, 0);
        // 初始選擇第一個從站並完整更新顯示（包括 SDO 和 PDO）
        if (etherCATMaster->IsRunning()) {
            LRESULT cur = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
            int pos = (int)SendMessageW(hComboSlaves, CB_GETITEMDATA, cur, 0);
            if (pos <= 0) pos = 1; // fallback
            
            // Set confirmed slave to trigger full display update
            confirmedSlave = pos;
            
            // Restore saved SDO values if any, or clear the value column
            auto savedIt = savedSDOValues.find(confirmedSlave);
                if (savedIt != savedSDOValues.end() && hListSDOParams && IsWindow(hListSDOParams)) {
                const std::vector<std::wstring>& vals = savedIt->second;
                for (int ii = 0; ii < (int)vals.size() && ii < (int)currentObjects.size(); ++ii) {
                    // Value column moved to 5; restore decimal values there
                    ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(vals[ii].c_str()));
                    // also restore hex column if available (now column 6)
                    auto ithex4 = savedSDOHexValues.find(confirmedSlave);
                    if (ithex4 != savedSDOHexValues.end() && (int)ithex4->second.size() > ii) {
                        ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(ithex4->second[ii].c_str()));
                    }
                }
            } else if (hListSDOParams && IsWindow(hListSDOParams)) {
                // No saved values - clear the value and hex columns
                for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
                    ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(L""));
                    ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
                }
            }
            
            // Build PDO items dynamically from parsed ESI `currentPdos` and
            // only show/rebuild the PDO area after successful connection.
            if (etherCATMaster->IsInitialized()) {
                // If no ESI was loaded by the user, attempt to request the ESI
                // from the slave via master->DumpESI and parse it. This allows
                // dynamic population of PDOs immediately after connect.
                if (currentPdos.empty()) {
                    if (etherCATMaster) {
                        // Attempt to discover PDOs directly via SDOs when no ESI is loaded.
                        // Read RxPDO assignment (0x1C12) and TxPDO assignment (0x1C13)
                        currentPdos.clear();
                        // Helper lambda to read assignment and mappings
                        auto readPdoAssignment = [&](uint16_t assignIndex, const char* pdoTypePrefix) {
                            // read number of PDOs assigned (subindex 0)
                                    uint8_t countBuf = 0;
                                    int countBufLen = (int)sizeof(countBuf);
                                    if (!etherCATMaster->ReadSDO(confirmedSlave, assignIndex, 0x00, (char*)&countBuf, countBufLen)) return;
                                    int count = (int)countBuf;
                            for (int p = 1; p <= count; ++p) {
                                uint32_t pdoIdx = 0;
                                int readSz = (int)sizeof(pdoIdx);
                                if (!etherCATMaster->ReadSDO(confirmedSlave, assignIndex, (uint8_t)p, (char*)&pdoIdx, readSz)) continue;
                                // pdoIdx may be little-endian on wire; ensure host order
                                // Interpret as little-endian 32-bit
                                // On little-endian hosts this is a no-op
                                // Extract index (low 16 bits)
                                uint16_t pdoIndex = (uint16_t)(pdoIdx & 0xFFFF);

                                // Build a Pdo entry by reading its mapping count and entries
                                EsiPdoEntry pdoe;
                                pdoe.pdoType = std::string(pdoTypePrefix);
                                // create a simple name from index
                                {
                                    char buf[32]; sprintf_s(buf, sizeof(buf), "%s_0x%04X", pdoTypePrefix, pdoIndex);
                                    pdoe.name = std::string(buf);
                                }

                                // read mapping count at subindex 0 of the PDO index
                                uint8_t mapCount = 0;
                                int mapCountLen = (int)sizeof(mapCount);
                                if (!etherCATMaster->ReadSDO(confirmedSlave, pdoIndex, 0x00, (char*)&mapCount, mapCountLen)) {
                                    // fallback: try reading as 16-bit count
                                    uint16_t mapCount16 = 0;
                                    int mapCount16Len = (int)sizeof(mapCount16);
                                    if (etherCATMaster->ReadSDO(confirmedSlave, pdoIndex, 0x00, (char*)&mapCount16, mapCount16Len)) mapCount = (uint8_t)mapCount16;
                                }

                                for (int me = 1; me <= (int)mapCount; ++me) {
                                    uint32_t mapping = 0;
                                    int mappingLen = (int)sizeof(mapping);
                                    if (!etherCATMaster->ReadSDO(confirmedSlave, pdoIndex, (uint8_t)me, (char*)&mapping, mappingLen)) continue;
                                    // mapping format: 0xIIIISSBB where IIII=index, SS=subindex, BB=bitlength
                                    uint16_t objIdx = (uint16_t)((mapping >> 16) & 0xFFFF);
                                    uint8_t objSub = (uint8_t)((mapping >> 8) & 0xFF);
                                    uint8_t bitlen = (uint8_t)(mapping & 0xFF);
                                    EsiObjectEntry oe;
                                    oe.index = objIdx;
                                    oe.subindex = objSub;
                                    oe.bitlen = bitlen;
                                    // name unknown without ESI; leave empty
                                    oe.name = "";
                                    pdoe.entries.push_back(oe);
                                }

                                currentPdos.push_back(pdoe);
                            }
                        };

                        // Read RxPDOs (master->slave outputs)
                        readPdoAssignment(0x1C12, "RxPdo");
                        // Read TxPDOs (slave->master inputs)
                        readPdoAssignment(0x1C13, "TxPdo");

                        // If still empty, fall back to DumpESI attempt (legacy behaviour)
                        if (currentPdos.empty()) {
                            std::string dump;
                            if (etherCATMaster->DumpESI(confirmedSlave, dump)) {
                                // DumpESI prints EEPROM/ESI hex dump, not XML. Keep for diagnostics only.
                                std::wstring w = Utf8ToWide(dump);
                                PrependStatus(w.c_str());
                            }
                        }
                    }
                }
                // Clear any previous items
                pdoDisplay.Clear();
                // Populate pdoDisplay.pdoItems from currentPdos (ESI parsed)
                for (const auto &p : currentPdos) {
                    for (const auto &e : p.entries) {
                        PDOItem itm;
                                // Determine item name: prefer ESI-provided name; if absent try currentObjects lookup; otherwise use address
                                std::wstring name;
                                if (!e.name.empty()) {
                                    name = std::wstring(e.name.begin(), e.name.end());
                                } else {
                                    // try to find matching object entry parsed earlier
                                    for (const auto &o : currentObjects) {
                                        if (o.index == (uint16_t)e.index && o.subindex == (uint8_t)e.subindex) {
                                            name = std::wstring(o.name.begin(), o.name.end());
                                            break;
                                        }
                                    }
                                }
                                itm.name = name; // may be empty for now
                                itm.index = (uint16_t)e.index;
                                itm.subindex = (uint8_t)e.subindex;
                        itm.bitLength = (uint8_t)e.bitlen;
                        itm.isOutput = (p.pdoType.find("Rx") != std::string::npos); // RxPDO -> outputs
                        // editable if access contains 'w' (rw or wo)
                        std::string acc = e.access;
                        for (auto &c : acc) c = (char)tolower(c);
                        itm.isEditable = (acc.find('w') != std::string::npos);
                        // Additionally, allow outputs (RxPDO) to be editable so user can write outputs
                        if (itm.isOutput) itm.isEditable = true;
                        itm.byteOffset = 0; // will be set by RecalculateOffsetsFromSDO
                        itm.currentValue = L"";
                        itm.displayValue = L"";
                        itm.hLabel = itm.hValue = itm.hEdit = itm.hButton = NULL;
                        itm.dataType = std::wstring(e.dataType.begin(), e.dataType.end());
                        itm.unit = L""; itm.minValue = L""; itm.maxValue = L""; itm.defaultValue = L"";
                            // format address and description from ESI entry
                            {
                                wchar_t buf[32];
                                swprintf_s(buf, L"0x%04X:%02X", itm.index, itm.subindex);
                                itm.address = buf;
                                // if name still empty, use address as visible name
                                if (itm.name.empty()) itm.name = buf;
                            }
                            itm.description = std::wstring(e.description.begin(), e.description.end());
                        pdoDisplay.pdoItems.push_back(itm);
                    }
                }

                // Recalculate offsets using master-provided IO base and then rebuild UI
                if (pdoDisplay.RecalculateOffsetsFromSDO(etherCATMaster, confirmedSlave)) {
                    pdoDisplay.RebuildPDOControls();
                    // Restore any cached edit texts by name
                    RestorePDOInputsFromGlobal();
                } else {
                    // still rebuild controls so UI shows items even without absolute offsets
                    pdoDisplay.RebuildPDOControls();
                    RestorePDOInputsFromGlobal();
                }
            }

            // Ensure PDO area visible after building
            pdoDisplay.Show(true);
            UpdateSelectedSlaveStatus(pos);
        }
    }
}

void Win32Window::UpdateStatusText() {
    // If child status window is visible, scroll it to top (newest first)
    if (hChildStatusEdit && IsWindow(hChildStatusEdit) && hChildStatusWnd && IsWindowVisible(hChildStatusWnd)) {
        // place caret at start so newest message is visible
        SendMessageW(hChildStatusEdit, EM_SETSEL, 0, 0);
    }
}

void Win32Window::UpdateSelectedSlaveStatus(int slaveIdx) {
    if (!etherCATMaster || !etherCATMaster->IsInitialized()) return;
    
    // 獲取從站信息
    auto slaves = etherCATMaster->GetSlaveInfo();
    if (slaveIdx < 1 || slaveIdx > (int)slaves.size()) return;
    
    const auto& slave = slaves[slaveIdx - 1];
    
    // 嘗試讀取設備名稱 (0x1008:00 - Device Name)
    char deviceName[128] = {0};
    std::wstring deviceNameW;
    int nameSize = sizeof(deviceName);
    
        if (etherCATMaster->ReadSDO(slaveIdx, 0x1008, 0x00, deviceName, nameSize)) {
        // 轉換為寬字元
        deviceNameW = Utf8ToWide(std::string(deviceName));
    }
    
    // 如果沒有讀到設備名稱，使用預設名稱
    if (deviceNameW.empty()) {
        deviceNameW = std::wstring(slave.name.begin(), slave.name.end());
    }
    
    // 獲取狀態字串
    const wchar_t* stateStr = L"未知";
    switch (slave.state) {
        case 0x01: stateStr = L"INIT"; break;
        case 0x02: stateStr = L"PRE-OP"; break;
        case 0x04: stateStr = L"SAFE-OP"; break;
        case 0x08: stateStr = L"OP"; break;
        default: stateStr = L"未知"; break;
    }
    
    // Previously printed a detailed selected-slave summary to the status window.
    // This debug/UI noise is disabled: only update the last reported index to
    // avoid repeated future prints.
    lastReportedSelectedSlave = slaveIdx;

    // PDO 更新現在由 OnTimer 自動處理,這裡只需確保顯示區域可見
    if (etherCATMaster && etherCATMaster->IsRunning()) {
        pdoDisplay.Show(true); // ensure PDO area is visible when a slave is selected
    }
}

void Win32Window::PrependStatus(const wchar_t* text) {
    if (!text) return;
    statusLogger.Prepend(std::wstring(text), hChildStatusEdit, hChildStatusWnd, true);
}

// UI-only status prepend: same behavior as PrependStatus for the UI control
// but does not append to on-disk status.log. Useful for frequent messages
// (like selected-slave summary) that would otherwise bloat the log file.
void Win32Window::PrependStatusNoFile(const wchar_t* text)
{
    if (!text) return;
    statusLogger.PrependNoFile(std::wstring(text), hChildStatusEdit, hChildStatusWnd);
}

// Validate a user's input for SDO Value column according to ESI dataType/bitlen
bool Win32Window::ValidateSDOValueInput(int itemIndex, const std::wstring &input, std::wstring &outError) {
    outError.clear();
    if (itemIndex < 0 || itemIndex >= (int)currentObjects.size()) return true; // nothing to validate
    const EsiObjectEntry &obj = currentObjects[itemIndex];
    std::string dtype = obj.dataType;
    std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
    std::wstring s = input;
    // trim
    auto is_space = [](wchar_t c){ return iswspace(c); };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    if (s.empty()) {
        outError = L"輸入不能為空";
        return false;
    }

    bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos);
    bool isBool = (du.find("BOOL") != std::string::npos) || (du.find("BOOLEAN") != std::string::npos);
    bool isUnsigned = (du.find("U") != std::string::npos && du.find("INT") != std::string::npos) || (du.find("BYTE") != std::string::npos) || (du.find("WORD") != std::string::npos) || (du.find("DWORD") != std::string::npos) || (du.find("LWORD") != std::string::npos) || (du.find("USINT") != std::string::npos) || (du.find("UINT") != std::string::npos) || (du.find("UDINT") != std::string::npos) || (du.find("ULINT") != std::string::npos);
    bool isSigned = (du.find("SINT") != std::string::npos) || (du.find("DINT") != std::string::npos) || (du.find("LINT") != std::string::npos) || ((du.find("INT") != std::string::npos) && !isUnsigned);

    // If input looks like hex token list (0x..), accept and skip numeric validation
    bool anyHexTok = false;
    std::vector<uint8_t> typedBE;
    // Use shared utility to parse tokenized hex/decimal sequences
    ParseTokensToBytes(s, typedBE, anyHexTok);
    if (anyHexTok && !typedBE.empty()) return true; // allow hex token input

    if (isBool) {
        if (!(s == L"0" || s == L"1" || _wcsicmp(s.c_str(), L"TRUE") == 0 || _wcsicmp(s.c_str(), L"FALSE") == 0)) {
            outError = L"布林欄位僅接受 0/1 或 true/false";
            return false;
        }
        return true;
    }

    if (isFloat) {
        // try to parse as double
        wchar_t *endp = nullptr; errno = 0;
        double v = wcstod(s.c_str(), &endp);
        if (endp == s.c_str() || *endp != L'\0') {
            outError = L"浮點數格式不正確";
            return false;
        }
        (void)v; return true;
    }

    // Signed/Unsigned integer validation
    if (isUnsigned || isSigned) {
        // remove separators and whitespace
        std::wstring t = s;
        for (auto it = t.begin(); it != t.end();) {
            if (iswspace(*it) || *it == L',' || *it == L';' || *it == L':' || *it == L'|') it = t.erase(it); else ++it;
        }

        // Hex form (0x...) accepted for both signed and unsigned (we allow user hex tokens)
        if (t.size() >= 2 && (t[0]==L'0') && (t[1]==L'x' || t[1]==L'X')) {
            for (size_t i = 2; i < t.size(); ++i) if (!iswxdigit(t[i])) { outError = L"十六進位格式錯誤"; return false; }
            return true;
        }

        if (isSigned) {
            // parse as signed decimal (allow leading + / -)
            wchar_t *endp = nullptr;
            errno = 0;
            long long sv = wcstoll(t.c_str(), &endp, 10);
            if (endp == t.c_str() || *endp != L'\0') {
                outError = L"整數格式不正確";
                return false;
            }
            // If bitlen provided, enforce signed range
            if (obj.bitlen > 0 && obj.bitlen <= 64) {
                int bits = obj.bitlen;
                if (bits >= 1 && bits <= 64) {
                    // compute signed min/max safely
                    if (bits == 64) {
                        // range covers full signed 64-bit
                    } else {
                        long long minVal = -(1LL << (bits - 1));
                        long long maxVal = ((1LL << (bits - 1)) - 1LL);
                        if (sv < minVal || sv > maxVal) {
                            outError = L"輸入值超出 ESI 定義的帶符號位元長度範圍";
                            return false;
                        }
                    }
                }
            }
            (void)sv; return true;
        } else {
            // unsigned: do not accept leading '-'
            if (!t.empty() && t[0] == L'-') { outError = L"無符號整數欄位不接受負值"; return false; }
            // parse as unsigned decimal
            wchar_t *endp = nullptr; errno = 0;
            unsigned long long v = wcstoull(t.c_str(), &endp, 10);
            if (endp == t.c_str() || *endp != L'\0') { outError = L"整數格式不正確"; return false; }
            if (obj.bitlen > 0 && obj.bitlen <= 64) {
                int expectedBytes = (obj.bitlen + 7) / 8;
                if (expectedBytes >= 1 && expectedBytes <= 8) {
                    unsigned long long maxVal = (expectedBytes >= 8) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (8*expectedBytes)) - 1ULL);
                    if (v > maxVal) { outError = L"輸入值超出 ESI 定義的位元長度"; return false; }
                }
            }
            (void)v; return true;
        }
    }

    // Default: accept any non-empty input
    return true;
}

// Child/status window proc moved to StatusPane.cpp

// Subclass proc for in-place edit control to capture Enter/Esc and forward commit
static LRESULT CALLBACK InplaceEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* w = reinterpret_cast<Win32Window*>(dwRefData);
    (void)uIdSubclass;
    if (!w) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    switch (uMsg) {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) {
            // Send EN_KILLFOCUS to parent (main window) to trigger commit handling
            HWND parent = GetParent(hwnd);
            if (parent) PostMessage(parent, WM_COMMAND, MAKEWPARAM(10001, EN_KILLFOCUS), (LPARAM)hwnd);
            return 0;
        } else if (wParam == VK_ESCAPE) {
            // Cancel editing: destroy edit
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CHAR:
        {
            wchar_t ch = (wchar_t)wParam;
            // If editing Hex column (subitem 6), keep permissive token-friendly behavior
            int sub = (w->inplaceEditSubitem >= 0 ? w->inplaceEditSubitem : 5);
            if (sub == 6) {
                if (ch >= 0x20) {
                    if (!iswalnum(ch) && !iswspace(ch) && ch != L',' && ch != L';' && ch != L':' && ch != L'|') {
                        return 0;
                    }
                }
                break;
            }

            // For Value column, restrict allowed characters according to ESI DataType
            int idx = w->inplaceEditItem;
            std::string dtype;
            if (idx >= 0 && idx < (int)w->currentObjects.size()) dtype = w->currentObjects[idx].dataType;
            // Uppercase copy for simple contains checks
            std::string du = dtype;
            std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });

            auto isSeparator = [&](wchar_t c){ return c == L',' || c == L';' || c == L':' || c == L'|'; };

            bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos);
            bool isBool = (du.find("BOOL") != std::string::npos) || (du.find("BOOLEAN") != std::string::npos);
            bool isUnsigned = (du.find("U") != std::string::npos && du.find("INT") != std::string::npos) || (du.find("BYTE") != std::string::npos) || (du.find("WORD") != std::string::npos) || (du.find("DWORD") != std::string::npos) || (du.find("LWORD") != std::string::npos) || (du.find("USINT") != std::string::npos) || (du.find("UINT") != std::string::npos) || (du.find("UDINT") != std::string::npos) || (du.find("ULINT") != std::string::npos);
            bool isSigned = (du.find("SINT") != std::string::npos) || (du.find("DINT") != std::string::npos) || (du.find("LINT") != std::string::npos) || ((du.find("INT") != std::string::npos) && !isUnsigned);

            if (ch < 0x20) break; // control chars allowed

            if (isFloat) {
                // allow digits, sign, decimal point, exponent, whitespace and separators
                if (!iswdigit(ch) && ch != L'-' && ch != L'+' && ch != L'.' && ch != L'e' && ch != L'E' && !iswspace(ch) && !isSeparator(ch)) return 0;
            } else if (isBool) {
                if (!(ch == L'0' || ch == L'1' || iswspace(ch) || isSeparator(ch))) return 0;
            } else if (isUnsigned) {
                // allow hex digits (0-9A-F), optional 0x marker (x/X), whitespace and separators
                if (!iswxdigit(ch) && ch != L'x' && ch != L'X' && !iswspace(ch) && !isSeparator(ch)) return 0;
            } else if (isSigned) {
                // signed integers: allow digits, sign, and also hex tokens (0x..) if user prefers
                if (!iswxdigit(ch) && ch != L'-' && ch != L'+' && ch != L'x' && ch != L'X' && !iswspace(ch) && !isSeparator(ch)) return 0;
            } else {
                // default: be permissive but filter out control/unexpected punctuation
                if (!iswalnum(ch) && !iswspace(ch) && !isSeparator(ch)) return 0;
            }
        }
        break;
    case WM_PASTE:
        {
            // Intercept paste and sanitize based on DataType of the edited item
            if (!w) break;
            int sub = (w->inplaceEditSubitem >= 0 ? w->inplaceEditSubitem : 5);
            // If Hex column, preserve previous permissive behavior
            if (sub == 6) {
                if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(hwnd)) {
                    HANDLE h = GetClipboardData(CF_UNICODETEXT);
                    if (h) {
                        wchar_t* clip = (wchar_t*)GlobalLock(h);
                        if (clip) {
                            std::wstring s(clip);
                            GlobalUnlock(h);
                            std::wstring out;
                            out.reserve(s.size());
                            for (wchar_t c : s) {
                                if (iswalnum(c) || iswspace(c) || c == L',' || c == L';' || c == L':' || c == L'|') out.push_back(c);
                            }
                            if (!out.empty()) SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)out.c_str());
                        }
                    }
                    CloseClipboard();
                }
                return 0;
            }

            // For Value column, sanitize according to DataType
            int idx = w->inplaceEditItem;
            std::string dtype;
            if (idx >= 0 && idx < (int)w->currentObjects.size()) dtype = w->currentObjects[idx].dataType;
            std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
            auto isSep = [&](wchar_t c){ return c == L',' || c == L';' || c == L':' || c == L'|'; };
            bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos);
            bool isBool = (du.find("BOOL") != std::string::npos) || (du.find("BOOLEAN") != std::string::npos);
            bool isUnsigned = (du.find("U") != std::string::npos && du.find("INT") != std::string::npos) || (du.find("BYTE") != std::string::npos) || (du.find("WORD") != std::string::npos) || (du.find("DWORD") != std::string::npos) || (du.find("LWORD") != std::string::npos) || (du.find("USINT") != std::string::npos) || (du.find("UINT") != std::string::npos) || (du.find("UDINT") != std::string::npos) || (du.find("ULINT") != std::string::npos);
            bool isSigned = (du.find("SINT") != std::string::npos) || (du.find("DINT") != std::string::npos) || (du.find("LINT") != std::string::npos) || ((du.find("INT") != std::string::npos) && !isUnsigned);

            if (IsClipboardFormatAvailable(CF_UNICODETEXT) && OpenClipboard(hwnd)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    wchar_t* clip = (wchar_t*)GlobalLock(h);
                    if (clip) {
                        std::wstring s(clip);
                        GlobalUnlock(h);
                        std::wstring out; out.reserve(s.size());
                        for (wchar_t c : s) {
                            if (isFloat) {
                                if (iswdigit(c) || c == L'-' || c == L'+' || c == L'.' || c == L'e' || c == L'E' || iswspace(c) || isSep(c)) out.push_back(c);
                            } else if (isBool) {
                                if (c == L'0' || c == L'1' || iswspace(c) || isSep(c)) out.push_back(c);
                            } else if (isUnsigned) {
                                if (iswxdigit(c) || c == L'x' || c == L'X' || iswspace(c) || isSep(c)) out.push_back(c);
                            } else if (isSigned) {
                                if (iswxdigit(c) || c == L'-' || c == L'+' || c == L'x' || c == L'X' || iswspace(c) || isSep(c)) out.push_back(c);
                            } else {
                                if (iswalnum(c) || iswspace(c) || isSep(c)) out.push_back(c);
                            }
                        }
                        if (!out.empty()) SendMessageW(hwnd, EM_REPLACESEL, TRUE, (LPARAM)out.c_str());
                    }
                }
                CloseClipboard();
            }
            return 0;
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, InplaceEditSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass procedure for content pane: forward WM_COMMAND/WM_NOTIFY to the parent window
static LRESULT CALLBACK ContentPaneSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    // Retrieve parent (main window) from subclass data (dwRefData holds pointer to Win32Window)
    Win32Window* pThis = reinterpret_cast<Win32Window*>(dwRefData);
    if (!pThis) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_COMMAND:
    case WM_NOTIFY:
        // Forward to main window procedure for centralized handling
        return SendMessageW(pThis->GetHwnd(), uMsg, wParam, lParam);
    default:
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass for SDO parameter ListView to detect mouse clicks on Value column
static LRESULT CALLBACK SDOListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* w = reinterpret_cast<Win32Window*>(dwRefData);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        // Compute hit-test at mouse point
        POINT pt; pt.x = (short)LOWORD(lParam); pt.y = (short)HIWORD(lParam);
        LVHITTESTINFO hti = {0};
        hti.pt = pt;
        int hit = ListView_SubItemHitTest(hwnd, &hti);
            if (hit >= 0) {
                // Only allow editing Value (5) or Hex (6) via mouse click (columns updated)
                int editSub = hti.iSubItem;
                if (editSub == 5 || editSub == 6) {
                    // Only trigger in-place editing for the SDO parameter ListView.
                    if (w && w->GetHwnd() && hwnd == w->GetHListSDOParams()) {
                        PostMessage(w->GetHwnd(), WM_USER_BEGIN_VALUE_EDIT, (WPARAM)hti.iItem, (LPARAM)editSub);
                    }
                }
            // Let default processing continue (selection, etc.)
        }
        break;
    }
    case WM_VSCROLL:
    case WM_HSCROLL:
    case WM_MOUSEWHEEL:
    {
        // 先由 ListView 完成預設滾動
        LRESULT ret = DefSubclassProc(hwnd, uMsg, wParam, lParam);
        // 再請 Window 物件對齊/處理內嵌編輯框
        if (w) w->HandleSDOListScroll();
        return ret;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SDOListSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Public: 由 ListView 子類別在滾動後呼叫，保持內嵌編輯與 Value 欄對齊，或在不可視時提交關閉
void Win32Window::HandleSDOListScroll()
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
        RECT rcSub = {0};
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

void Win32Window::OnTimer() {
    if (!etherCATMaster || !etherCATMaster->IsRunning()) {
        return;
    }

    // When minimized to tray, avoid heavy UI updates. Perform only essential
    // background checks (link status and watchdog) to keep PDO running.
    if (minimizedToTray) {
        // If link is lost while minimized, still handle disconnect
        if (!currentIfName.empty()) {
            if (!CheckLinkStatus(currentIfName.c_str())) {
                // Post a UI-visible status when restored; here just log to in-memory buffer
                PrependStatus(L"錯誤：網路斷線（在托盤模式），正在執行斷開程序...");
                if (!disconnecting.load()) {
                    OnDisconnect();
                }
            }
        }
        // keep PDO background operation; do not touch UI controls
        return;
    }

    // If we have a monitored interface, verify the OS reports the link up.
    // If the link is lost, update UI and trigger an orderly disconnect.
    if (!currentIfName.empty()) {
        // CheckLinkStatus expects UTF-8 interface name
        if (!CheckLinkStatus(currentIfName.c_str())) {
            PrependStatus(L"錯誤：網路斷線，正在執行斷開程序...\r\n");
            // Ensure we don't re-enter disconnect while already handling it
            if (!disconnecting.load()) {
                OnDisconnect();
            }
            return;
        }
    }

    // Check if cycle time controls exist
    if (hEditCurrentCycleTime == NULL) {
        return;
    }

    // 更新當前週期時間 (顯示實際測量的週期時間，格式："<μs> μs (<ms> ms)")
    std::wstringstream ss; // 用於後續 WKC 顯示

    {
    uint32_t comms = etherCATMaster->GetCommsTime();
    // Display only the numeric microseconds for EtherCAT comms work time
    std::wstring val = std::to_wstring(comms);
        SetWindowTextW(hEditCurrentCycleTime, val.c_str());
    }

    // 更新 Working Counter（使用獨立的 ss，避免格式狀態影響）
    ss.str(L"");
    ss.clear();
    ss << etherCATMaster->GetWorkingCounter();
    SetWindowTextW(hEditWKC, ss.str().c_str());

    // Update PDO sent counter (8 digits)
    HWND hEditPDOSentLocal = GetDlgItem(hContentWnd, 2010); // avoid hiding member hEditPDOSent
    if (hEditPDOSentLocal) {
        uint64_t cnt = etherCATMaster->GetPDOSentCount();
        wchar_t buf[32];
        swprintf_s(buf, L"%08llu", (unsigned long long)cnt);
        SetWindowTextW(hEditPDOSentLocal, buf);
    }
    
    // 更新PDO顯示：只在使用者已確認選定 (confirmedSlave) 時才更新，避免下拉選單在瀏覽時立即切換
    if (confirmedSlave > 0 && etherCATMaster->GetSlaveCount() > 0) {
        const ecx_contextt* ctx = etherCATMaster->GetContext();
        if (ctx && ctx->slavelist) {
            int slaveIdx = confirmedSlave;
            // Request per-slave PDO snapshot from EtherCATMaster (PDO thread provides this)
            std::vector<uint8_t> snap;
            if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snap) || snap.empty()) {
                // No snapshot available from PDO thread -> debug only
                PostUTF8LogToUI(WideToUtf8(std::wstring(L"[PDO] No PDO snapshot available from PDO thread\r\n")));
                return;
            }
            int copied = (int)snap.size();
            if (copied > 0) {
                // 一次性診斷輸出 (僅首次或從站改變時)
                static int lastDiagSlave = -1;
                bool slaveChanged = (lastDiagSlave != slaveIdx);
                
                if (slaveChanged) {
                    // 從站改變時才重建UI控件並回存上次輸入值
                    // 針對新選定從站同步 PDO 位移並重建UI控件
                    // 先快取舊從站的可編輯輸入（以名稱為鍵）
                    CaptureCurrentPDOInputsByName();
                    if (pdoDisplay.RecalculateOffsetsFromSDO(etherCATMaster, slaveIdx)) {
                        pdoDisplay.RebuildPDOControls();
                        // 切換後依名稱還原輸入值
                        RestorePDOInputsFromGlobal();
                        
                        // 強制重繪PDO顯示區域
                        HWND hGroupBox = pdoDisplay.GetGroupBox();
                        if (hGroupBox && IsWindow(hGroupBox)) {
                            InvalidateRect(hGroupBox, NULL, TRUE);
                            UpdateWindow(hGroupBox);
                        }
                    }
                    lastDiagSlave = slaveIdx;
                }
                
                // 內聯 PDO 更新邏輯 (直接讀取 IOmap 並更新 UI)
                int outBase = 0, inBase = 0;
                if (etherCATMaster->GetSlaveIOBaseBytes(slaveIdx, outBase, inBase)) {
                    if (slaveChanged) {
               std::wstringstream diagSS;
               diagSS << L"[PDO診斷] 從站=" << slaveIdx 
                   << L" outBase=" << outBase << L" inBase=" << inBase 
                   << L" 項目數=" << pdoDisplay.pdoItems.size() << L"\r\n";
               for (size_t i = 0; i < pdoDisplay.pdoItems.size(); ++i) {
                const auto& itm = pdoDisplay.pdoItems[i];
                diagSS << L"  [" << i << L"] " << itm.name 
                    << L" idx=0x" << std::hex << std::setw(4) << std::setfill(L'0') << itm.index
                    << L" " << (itm.isOutput ? L"OUT" : L"IN")
                    << L" offset=" << std::dec << itm.byteOffset
                    << L" bits=" << (int)itm.bitLength
                    << L" absOff=" << (itm.isOutput ? outBase : inBase) + itm.byteOffset << L"\r\n";
               }
               // diagnostic output should not spam user's visible status; log as UTF-8 debug
               PostUTF8LogToUI(WideToUtf8(diagSS.str()));
                    }
                    
                    // snapshot layout: outputs first (outputSize bytes), then inputs
                    int outputSize = 0;
                    if (ctx && ctx->slavelist && slaveIdx <= ctx->slavecount) {
                        outputSize = ctx->slavelist[slaveIdx].Obytes;
                    } else if (etherCATMaster) {
                        // fallback: ask master for base bytes and infer outputSize from snapshot length
                        outputSize = (int)copied; // best-effort, will be checked per-item
                    }

                    for (auto& item : pdoDisplay.pdoItems) {
                        // compute index inside per-slave snapshot: outputs first
                        int idxInSnapshot = item.isOutput ? item.byteOffset : (outputSize + item.byteOffset);
                        // compute required bytes rounding up for non-8-multiple bit lengths
                        int neededBytes = (item.bitLength + 7) / 8;
                        int requiredEnd = idxInSnapshot + neededBytes;

                        // 驗證偏移量範圍 - 使用 snapshot 長度 (copied)
                        if (idxInSnapshot < 0 || requiredEnd > copied) {
                            if (item.hValue) SetWindowTextW(item.hValue, L"N/A");
                            continue;
                        }

                        // 從 snapshot 讀取值 (只支援 16/32 位元顯示)
                        int32_t value = 0;
                        if (item.bitLength == 16 && neededBytes >= 2) {
                            uint16_t v = (uint16_t)snap[idxInSnapshot] | ((uint16_t)snap[idxInSnapshot + 1] << 8);
                            value = (int16_t)v;
                        } else if (item.bitLength == 32 && neededBytes >= 4) {
                            uint32_t v = (uint32_t)snap[idxInSnapshot] |
                                         ((uint32_t)snap[idxInSnapshot + 1] << 8) |
                                         ((uint32_t)snap[idxInSnapshot + 2] << 16) |
                                         ((uint32_t)snap[idxInSnapshot + 3] << 24);
                            value = (int32_t)v;
                        } else {
                            // unsupported bitlength for numeric parse: show N/A
                            if (item.hValue) SetWindowTextW(item.hValue, L"N/A");
                            continue;
                        }
                        
                        // 診斷: 僅針對 0x607A 輸出詳細信息 (每 100 次循環輸出一次)
                        if (item.index == 0x607A) {
                            static int readCount607A = 0;
                            if (++readCount607A % 100 == 0) {
                            }
                        }
                        
                                // If this item was recently written by the user, prefer the cached
                                // written value for a short grace period to avoid the snapshot
                                // immediately overwriting it when the device hasn't reflected the change yet.
                                bool usedRecent = false;
                                if (item.hasRecentWrite) {
                                    auto now = std::chrono::steady_clock::now();
                                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - item.lastWriteTime).count();
                                    const int GRACE_MS = 400; // configurable grace period
                                    if (ms < GRACE_MS) {
                                        // Use lastWrittenInt as authoritative display
                                        int32_t cached = item.lastWrittenInt;
                                        std::wstringstream ssItem;
                                        if (item.bitLength == 16) ssItem << cached << L" (0x" << std::hex << std::setw(4) << std::setfill(L'0') << (cached & 0xFFFF) << L")";
                                        else ssItem << cached << L" (0x" << std::hex << std::setw(8) << std::setfill(L'0') << (uint32_t)cached << L")";
                                        item.displayValue = ssItem.str();
                                        item.currentValue = std::to_wstring(cached);
                                        usedRecent = true;
                                    } else {
                                        // Grace period expired
                                        item.hasRecentWrite = false;
                                    }
                                }

                                if (!usedRecent) {
                                    // 更新顯示 from snapshot
                                    item.currentValue = std::to_wstring(value);
                                    std::wstringstream ssItem;
                                    if (item.bitLength == 16) {
                                        ssItem << value << L" (0x" << std::hex << std::setw(4) << std::setfill(L'0') << (value & 0xFFFF) << L")";
                                    } else {
                                        ssItem << value << L" (0x" << std::hex << std::setw(8) << std::setfill(L'0') << (uint32_t)value << L")";
                                    }
                                    item.displayValue = ssItem.str();
                                }

                                // 檢查 hValue 句柄是否有效
                                    if (item.hValue && IsWindow(item.hValue)) {
                                        SetWindowTextW(item.hValue, item.displayValue.c_str());
                                    } else {
                                        if (item.index == 0x607A || slaveChanged) {
                                            std::wstringstream errSS;
                                            errSS << L"[PDO UI錯誤] " << item.name << L" (0x" << std::hex << item.index 
                                                  << std::dec << L") hValue無效: 0x" << std::hex << (void*)item.hValue << L"\r\n";
                                            // non-intrusive debug log instead of visible status spam
                                            PostUTF8LogToUI(WideToUtf8(errSS.str()));
                                        }
                                    }
                    }
                }
            }
        }
    }

    // Watchdog: check WKC and ALstatus of slaves
    uint32_t wkc = etherCATMaster->GetWorkingCounter();
    auto slaves = etherCATMaster->GetSlaveInfo();
    bool anyWatchdog = false;
    for (auto &s : slaves) {
        if (s.state == 0x14 || wkc == 0) { // 0x14 = OP + error (Sync manager watchdog)
            anyWatchdog = true;
            break;
        }
    }

    if (anyWatchdog) {
        int hits = ++watchdogCounter;
        if (hits >= watchdogThreshold && !recoveryRunning.load()) {
            // start recovery thread
            recoveryRunning.store(true);
            recoveryThread = std::thread([this]() { StartWatchdogRecovery(); });
        }
    } else {
        watchdogCounter.store(0);
    }
}

void Win32Window::StartWatchdogRecovery() {
    // bump priority for recovery
    DWORD prevPriority = GetPriorityClass(GetCurrentProcess());
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    int prevThreadPrio = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    PostUTF8LogToUI(std::string("Watchdog triggered: starting automatic recovery...\n"));
    // Stop cyclic operation
    etherCATMaster->StopCyclicOperation();
    PostUTF8LogToUI(std::string("Stopped cyclic operation\n"));

    // Force runtime init for all slaves
    int count = etherCATMaster->GetSlaveCount();
    for (int i = 1; i <= count; ++i) {
        std::string slog;
        PostUTF8LogToUI(std::string("Recovery: ForceSlaveOperational for slave ") + std::to_string(i) + "\n");
        etherCATMaster->ForceSlaveOperational(i, slog);
        PostUTF8LogToUI(slog);
    }

    // Re-configure and restart cyclic
    PostUTF8LogToUI(std::string("Recovery: retry ConfigureSlaves()\n"));
    bool ok = etherCATMaster->ConfigureSlaves();
    if (!ok) PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() failed\n"));
    else PostUTF8LogToUI(std::string("Recovery: ConfigureSlaves() succeeded\n"));

    PostUTF8LogToUI(std::string("Recovery: restarting cyclic operation\n"));
    if (!etherCATMaster->StartCyclicOperation()) {
        PostUTF8LogToUI(std::string("Recovery: StartCyclicOperation failed: ") + etherCATMaster->GetLastError() + "\n");
    } else {
        PostUTF8LogToUI(std::string("Recovery: StartCyclicOperation succeeded\n"));
    }

    watchdogCounter.store(0);
    recoveryRunning.store(false);
    // restore priorities
    SetThreadPriority(GetCurrentThread(), prevThreadPrio);
    SetPriorityClass(GetCurrentProcess(), prevPriority);
}

// Post a UTF-8 encoded string from a worker thread to the UI thread.
// The string is copied to heap and freed by the UI thread handler.
void Win32Window::PostUTF8LogToUI(const std::string &utf8) {
    std::string *p = new std::string(utf8);
    PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)p);
}

void Win32Window::Show(int nCmdShow) {
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
}

int Win32Window::MessageLoop() {
    MSG msg = { 0 };
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

// Scan for ESI files in exe\esi and current working directory and fill hComboESI
void Win32Window::PopulateESIFiles()
{
    // ESI combo removed; do nothing. If you want to auto-load a default
    // ESI file, call PopulateObjectsFromESI with a hardcoded path here.
    (void)0;
}

void Win32Window::PopulateObjectsFromESI(const std::wstring& esiPathW)
{
    if (esiPathW.empty()) {
        if (hBtnImportParams) EnableWindow(hBtnImportParams, TRUE);
        return;
    }

    // wide -> utf8
    std::string pathUtf8 = WideToUtf8(esiPathW);

    std::string err;
    currentObjects.clear();
    currentPdos.clear();
    if (!EsiParser::Load(pathUtf8, currentObjects, currentPdos, err)) {
    std::wstring errW(err.begin(), err.end());
    PrependStatus(errW.c_str());
    PrependStatus(L"\r\n");
        if (hBtnImportParams) EnableWindow(hBtnImportParams, TRUE);
        return;
    }

    // Fill SDO parameter ListView
    if (hListSDOParams) {
        ListView_DeleteAllItems(hListSDOParams);
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            LVITEMW item = {0};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = (int)i;
            // Index column
            wchar_t idxbuf[32]; swprintf_s(idxbuf, L"0x%04X", currentObjects[i].index);
            item.pszText = idxbuf;
            item.lParam = (LPARAM)i;
            ListView_InsertItem(hListSDOParams, &item);
            // Sub
            wchar_t subbuf[16]; swprintf_s(subbuf, L"0x%02X", currentObjects[i].subindex);
            ListView_SetItemText(hListSDOParams, (int)i, 1, subbuf);
            // Name
            std::wstring nameW = Utf8ToWide(currentObjects[i].name);
            ListView_SetItemText(hListSDOParams, (int)i, 2, const_cast<LPWSTR>(nameW.c_str()));
            // Access (read/write/ro/wo) now at column 4 - prefer ESI-provided access string
            std::wstring accessW;
            if (!currentObjects[i].access.empty()) {
                std::string a = currentObjects[i].access;
                // normalize to lower for detection
                for (auto &c : a) c = (char)tolower((unsigned char)c);
                bool hasR = (a.find('r') != std::string::npos);
                bool hasW = (a.find('w') != std::string::npos);
                if (hasR && hasW) accessW = L"RW";
                else if (hasR) accessW = L"RO";
                else if (hasW) accessW = L"WO";
                else {
                    // unknown token: show uppercase as-is
                    std::string up = currentObjects[i].access;
                    for (auto &c : up) c = (char)toupper((unsigned char)c);
                    accessW = std::wstring(up.begin(), up.end());
                }
            } else {
                // Default to RO for objects without explicit access in ESI
                accessW = L"RO";
            }
            ListView_SetItemText(hListSDOParams, (int)i, 4, const_cast<LPWSTR>(accessW.c_str()));

            // DataType (new column at index 3): prefer explicit dataType text from ESI, else show bitlen
            std::wstring esiTypeW;
            if (!currentObjects[i].dataType.empty()) {
                esiTypeW = std::wstring(currentObjects[i].dataType.begin(), currentObjects[i].dataType.end());
            } else if (currentObjects[i].bitlen > 0) {
                wchar_t esiTypeBuf[64] = {0};
                swprintf_s(esiTypeBuf, L"%d-bit", currentObjects[i].bitlen);
                esiTypeW = esiTypeBuf;
            } else {
                esiTypeW.clear();
            }
            ListView_SetItemText(hListSDOParams, (int)i, 3, const_cast<LPWSTR>(esiTypeW.c_str()));

            // Value placeholder now at column 5, Hex at column 6
            ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(L""));
            ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(L""));
            // If we have cached values for the currently confirmed slave, restore them
            if (confirmedSlave > 0) {
                auto it = savedSDOValues.find(confirmedSlave);
                if (it != savedSDOValues.end()) {
                    const std::vector<std::wstring>& vals = it->second;
                    if ((int)vals.size() > (int)i) {
                        // Restore even empty string so cleared values are preserved
                        ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(vals[i].c_str()));
                    }
                }
            }
        }
        // Select first item if any
        if (!currentObjects.empty()) {
            ListView_SetItemState(hListSDOParams, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            // Enable SDO buttons only when both an ESI is loaded and we're connected
            bool isConnected = (etherCATMaster && etherCATMaster->IsInitialized());
            bool hasESI = !currentObjects.empty();
            EnableWindow(hBtnReadSDO, (hasESI && isConnected) ? TRUE : FALSE);
            EnableWindow(hBtnWriteSDO, (hasESI && isConnected) ? TRUE : FALSE);
            EnableWindow(hBtnReadAllSDO, (hasESI && isConnected) ? TRUE : FALSE);
            // Ensure Import Params button is enabled after ESI import if we have ESI
            if (hBtnImportParams) EnableWindow(hBtnImportParams, (hasESI ? TRUE : FALSE));
        } else {
            EnableWindow(hBtnReadSDO, FALSE);
            EnableWindow(hBtnWriteSDO, FALSE);
            EnableWindow(hBtnReadAllSDO, FALSE);
        }
    }

    // ESI-imported IOmap/PDO table population removed.
}

void Win32Window::OnBrowseESI()
{
    // Open file dialog
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"ESI Files\0*.xml\0All Files\0*.*\0";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        // Combobox removed: directly load chosen ESI file
        PopulateObjectsFromESI(szFile);
    }
    // Dialog closed (selection or cancel) - ensure Import Params button is enabled
    if (hBtnImportParams) EnableWindow(hBtnImportParams, TRUE);
}

void Win32Window::OnDumpESIForSelectedSlave()
{
    if (!etherCATMaster) return;

    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Request dump from master
    std::string dump;
    if (!etherCATMaster->DumpESI(slave, dump)) {
        std::string err = etherCATMaster->GetLastError();
        std::wstring errW(err.begin(), err.end());
        PrependStatus(errW.c_str());
        PrependStatus(L"\r\n");
        return;
    }

    // Show in status box in chunks
    const size_t CHUNK = 800;
    for (size_t pos = 0; pos < dump.size(); pos += CHUNK) {
        std::string part = dump.substr(pos, CHUNK);
        std::wstring wpart = Utf8ToWide(part);
        PrependStatus(wpart.c_str());
    }
}

// WideToUtf8 and EscapeXml moved to Win32Utils.cpp/h

void Win32Window::OnSaveParams()
{
    if (confirmedSlave <= 0) {
        MessageBoxW(hwnd, L"請先選擇從站再進行存檔。", L"未選擇從站", MB_OK | MB_ICONWARNING);
        return;
    }
    if (currentObjects.empty()) {
        MessageBoxW(hwnd, L"目前沒有載入任何 ESI 物件，無法存檔。", L"無 ESI", MB_OK | MB_ICONWARNING);
        return;
    }

    // Determine device name (try reading 0x1008:00 like other code)
    std::wstring deviceNameW;
    if (etherCATMaster) {
        char deviceName[256] = {0};
        int bufSz = (int)sizeof(deviceName);
        if (etherCATMaster->ReadSDO(confirmedSlave, 0x1008, 0x00, deviceName, bufSz)) {
            deviceNameW = Utf8ToWide(std::string(deviceName));
        }
    }
    // Fallback to slave name from master
    if (deviceNameW.empty()) {
        auto slaves = (etherCATMaster ? etherCATMaster->GetSlaveInfo() : std::vector<SlaveInfo>());
        if (confirmedSlave >= 1 && confirmedSlave <= (int)slaves.size()) {
            const auto &s = slaves[confirmedSlave - 1];
            deviceNameW = std::wstring(s.name.begin(), s.name.end());
        }
    }
    if (deviceNameW.empty()) deviceNameW = L"device";

    // Sanitize filename (replace invalid path chars)
    std::wstring safeName = deviceNameW;
    for (wchar_t &ch : safeName) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || iswspace(ch)) ch = L'_';
    }

    // Build output directory under exe path: <exe_dir>\Par
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring parDir = dir + L"\\Par";
    CreateDirectoryW(parDir.c_str(), NULL);

    // Compose filename: <Par>\<safeName>-<slave>.xml (e.g. D1_DS_P100E-7.xml)
    wchar_t fnameW[MAX_PATH];
    swprintf_s(fnameW, MAX_PATH, L"%s\\%s-%d.xml", parDir.c_str(), safeName.c_str(), confirmedSlave);

    // Build XML content (UTF-8) via helper so encoding/escaping is centralized
    std::vector<std::wstring> emptyHexVec;
    auto itHexs = savedSDOHexValues.find(confirmedSlave);
    const std::vector<std::wstring> &hexVec = (itHexs != savedSDOHexValues.end()) ? itHexs->second : emptyHexVec;
    std::string content = BuildESIDeviceXml(deviceNameW, currentObjects, hexVec);

    // If file already exists, ask user to confirm overwrite (Yes/No)
    DWORD fa = GetFileAttributesW(fnameW);
    if (fa != INVALID_FILE_ATTRIBUTES) {
        int resp = MessageBoxW(hwnd, L"檔案已存在，是否要覆蓋？", L"確認覆蓋", MB_YESNO | MB_ICONQUESTION);
        if (resp != IDYES) {
            PrependStatus(L"存檔已取消（使用者拒絕覆蓋）");
            return;
        }
    }

    // Write UTF-8 content to file
    FILE *f = NULL;
    errno_t er = _wfopen_s(&f, fnameW, L"wb");
    if (er != 0 || !f) {
        std::wstring msg = L"無法建立檔案: "; msg += fnameW;
        MessageBoxW(hwnd, msg.c_str(), L"寫入失敗", MB_OK | MB_ICONERROR);
        return;
    }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);

    std::wstring doneMsg = L"已存檔: ";
    // show in status as wide string
    doneMsg += fnameW;
    PrependStatus(doneMsg.c_str());
}

void Win32Window::OnImportParams()
{
    // Allow importing parameters even when no slave is selected (confirmedSlave may be 0)
    // Open file dialog for XML
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    wchar_t szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"XML files\0*.xml\0All files\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"選擇參數 XML 檔案";
    // Default initial directory: <exe_dir>\Par (same folder used by OnSaveParams)
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    static std::wstring initialParDir; // static to ensure lifetime for ofn.lpstrInitialDir
    initialParDir = dir + L"\\Par";
    ofn.lpstrInitialDir = initialParDir.c_str();
    if (!GetOpenFileNameW(&ofn)) {
        // user cancelled
        return;
    }

    // Parse XML with tinyxml2
    tinyxml2::XMLDocument doc;
    // tinyxml2 expects a narrow (UTF-8) file path; use shared utility
    std::string pathUtf8 = WideToUtf8(szFile);
    tinyxml2::XMLError err = doc.LoadFile(pathUtf8.empty() ? nullptr : pathUtf8.c_str());
    if (err != tinyxml2::XML_SUCCESS) {
        std::wstring msg = L"無法讀取 XML 檔案: "; msg += szFile;
        MessageBoxW(hwnd, msg.c_str(), L"讀取失敗", MB_OK | MB_ICONERROR);
        return;
    }

    tinyxml2::XMLElement *root = doc.FirstChildElement("Device");
    if (!root) {
        MessageBoxW(hwnd, L"XML 格式錯誤：找不到 Device 根節點。", L"格式錯誤", MB_OK | MB_ICONERROR);
        return;
    }

    // Prepare hex vector sized to currentObjects
    std::vector<std::wstring> hexVec(currentObjects.size());

    for (tinyxml2::XMLElement *obj = root->FirstChildElement("Object"); obj; obj = obj->NextSiblingElement("Object")) {
        const char *idxAttr = obj->Attribute("index");
        const char *subAttr = obj->Attribute("subindex");
        const char *hexAttr = obj->Attribute("hex");
        if (!idxAttr || !subAttr) continue;
        // parse index/subindex which are formatted like 0x1234
        unsigned int idx = 0, sub = 0;
        if (sscanf_s(idxAttr, "%i", &idx) != 1) {
            // try hex
            if (sscanf_s(idxAttr, "0x%x", &idx) != 1) continue;
        }
        if (sscanf_s(subAttr, "%i", &sub) != 1) {
            if (sscanf_s(subAttr, "0x%x", &sub) != 1) continue;
        }

        // find matching currentObjects entry
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if ((uint16_t)idx == currentObjects[i].index && (uint8_t)sub == currentObjects[i].subindex) {
                if (hexAttr && hexAttr[0] != '\0') {
                    // convert to wide
                    std::wstring whex = Utf8ToWide(hexAttr);
                    hexVec[i] = whex;
                }
                break;
            }
        }
    }

    // Store into savedSDOHexValues for the confirmed slave and update ListView
    savedSDOHexValues[confirmedSlave] = hexVec;

    // Ensure savedSDOValues has proper size for the confirmed slave
    auto &valVec = savedSDOValues[confirmedSlave];
    if ((int)valVec.size() < (int)currentObjects.size()) valVec.resize(currentObjects.size());

    if (hListSDOParams && IsWindow(hListSDOParams)) {
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if (!hexVec[i].empty()) {
                // Update Hex column
                ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(hexVec[i].c_str()));

                // Parse hex tokens (BE) into bytes
                std::vector<uint8_t> bytesBE;
                bool anyHexTok = false;
                ParseTokensToBytes(hexVec[i], bytesBE, anyHexTok);

                // Determine expected byte count from ESI bitlen for this object
                int expectedBytes = 0;
                int matchedBitlen = currentObjects[i].bitlen;
                if (matchedBitlen > 0) expectedBytes = (matchedBitlen + 7) / 8;
                else expectedBytes = (int)bytesBE.size();
                if (expectedBytes < 1) expectedBytes = 1;

                // Pad or truncate to expectedBytes (BE order)
                std::vector<uint8_t> paddedBE(expectedBytes, 0);
                if ((int)bytesBE.size() >= expectedBytes) {
                    int startIdx = (int)bytesBE.size() - expectedBytes;
                    for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                } else {
                    int pad = expectedBytes - (int)bytesBE.size();
                    for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                }

                // Compute LE integer value
                unsigned long long u = 0; int maxb = expectedBytes; if (maxb > 8) maxb = 8;
                for (int bi = 0; bi < maxb; ++bi) {
                    int idxFromEnd = expectedBytes - 1 - bi;
                    if (idxFromEnd >= 0) u |= (unsigned long long)paddedBE[idxFromEnd] << (8 * bi);
                }

                // Format according to dataType/bitlen similar to WM_USER_SDO_ITEM_READ
                std::wstring valueDisplay;
                const EsiObjectEntry &obj = currentObjects[i];
                std::string dtype = obj.dataType; std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });

                if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
                    valueDisplay = (u ? std::wstring(L"1") : std::wstring(L"0"));
                } else if (du.find("REAL") != std::string::npos || du.find("FLOAT") != std::string::npos) {
                    if (obj.bitlen == 32 && expectedBytes >= 4) {
                        uint32_t le = 0; for (int b = 0; b < 4; ++b) le |= (uint32_t)paddedBE[b] << (8*b);
                        float f; memcpy(&f, &le, sizeof(f)); std::wstringstream ss; ss << std::fixed << std::setprecision(6) << f; valueDisplay = ss.str();
                    } else if (obj.bitlen == 64 && expectedBytes >= 8) {
                        uint64_t le = 0; for (int b = 0; b < 8; ++b) le |= (uint64_t)paddedBE[b] << (8*b);
                        double d; memcpy(&d, &le, sizeof(d)); std::wstringstream ss; ss << std::fixed << std::setprecision(9) << d; valueDisplay = ss.str();
                    } else {
                        valueDisplay = std::to_wstring(u);
                    }
                } else if (du.find("INT") != std::string::npos || du.find("SINT") != std::string::npos || du.find("SIGNED") != std::string::npos) {
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
                } else {
                    valueDisplay = std::to_wstring(u);
                }

                // Update Value column and savedSDOValues
                ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(valueDisplay.c_str()));
                valVec[i] = valueDisplay;
            }
        }
    }

    std::wstring done = L"已從 " ; done += szFile; done += L" 匯入參數。";
    PrependStatus(done.c_str());
}

// BEGIN: PDODisplay.cpp merged content
// PDODisplay declarations moved into Win32Window.h; no separate include
// Ensure Windows macros `min`/`max` are undefined so std::min/std::max compile correctly
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <sstream>
#include <iomanip>

// Subclass procedure for group box to forward WM_COMMAND/WM_NOTIFY to its parent
static LRESULT CALLBACK GroupBoxSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    HWND hParent = (HWND)dwRefData;
    switch (uMsg) {
    case WM_COMMAND:
    case WM_NOTIFY:
        if (hParent && IsWindow(hParent)) {
            return SendMessageW(hParent, uMsg, wParam, lParam);
        }
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, GroupBoxSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

PDODisplayManager::PDODisplayManager()
    : hParent(NULL), hGroupBox(NULL), hOutputHeader(NULL), hInputHeader(NULL),
      currentY(0), isInitialized(false), selectedSlave(-1)
{
}

PDODisplayManager::~PDODisplayManager() {
    Clear();
}

void PDODisplayManager::Initialize(HWND parent, HINSTANCE hInstance, HFONT hFont, 
                                  int startY, int leftMargin, int width, int height) {
    if (isInitialized) return;
    
    hParent = parent;
    currentY = startY;
    groupLeftMargin = leftMargin;
    groupWidth = width;
    groupHeight = height;
    
    // Create group box
    hGroupBox = CreateWindowExW(0, L"BUTTON", L"PDO Items",
                               WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                               leftMargin, startY, width, height,
                               parent, NULL, hInstance, NULL);
    if (hGroupBox) {
        SendMessageW(hGroupBox, WM_SETFONT, (WPARAM)hFont, TRUE);
        SetWindowSubclass(hGroupBox, GroupBoxSubclassProc, 0, (DWORD_PTR)parent);
    }
    
    // Create headers
    hOutputHeader = CreateWindowExW(0, L"STATIC", L"Output (RxPDO)",
                                   WS_CHILD | WS_VISIBLE | SS_CENTER,
                                   leftMargin + 15, startY + GROUPBOX_INNER_TOP, 120, 20,
                                   parent, NULL, hInstance, NULL);
    if (hOutputHeader) {
        SendMessageW(hOutputHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    
    hInputHeader = CreateWindowExW(0, L"STATIC", L"Input (TxPDO)",
                                  WS_CHILD | WS_VISIBLE | SS_CENTER,
                                  leftMargin + width - 135, startY + GROUPBOX_INNER_TOP, 120, 20,
                                  parent, NULL, hInstance, NULL);
    if (hInputHeader) {
        SendMessageW(hInputHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    
    isInitialized = true;
}

void PDODisplayManager::RebuildPDOControls() {
    // Destroy existing controls then recreate according to pdoItems vector
    if (!isInitialized || !hParent) return;
    // Destroy previous item windows
    for (auto &it : pdoItems) {
        if (it.hLabel && IsWindow(it.hLabel)) { DestroyWindow(it.hLabel); it.hLabel = NULL; }
        if (it.hValue && IsWindow(it.hValue)) { DestroyWindow(it.hValue); it.hValue = NULL; }
        if (it.hEdit && IsWindow(it.hEdit)) { DestroyWindow(it.hEdit); it.hEdit = NULL; }
    if (it.hButton && IsWindow(it.hButton)) { DestroyWindow(it.hButton); it.hButton = NULL; }
    if (it.hAddress && IsWindow(it.hAddress)) { DestroyWindow(it.hAddress); it.hAddress = NULL; }
    if (it.hDesc && IsWindow(it.hDesc)) { DestroyWindow(it.hDesc); it.hDesc = NULL; }
    }

    HINSTANCE hInst = GetModuleHandle(NULL);
    HFONT hFont = (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0);
    if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    // Create new controls for each item. Y positioning will be handled in ResizeWidth.
    int editableCounter = 0;
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        std::wstring init = !pdoItems[i].currentValue.empty() ? pdoItems[i].currentValue : pdoItems[i].defaultValue;
        int idIndex = pdoItems[i].isEditable ? editableCounter : (int)i;
        CreatePDOItem(hParent, hInst, hFont, pdoItems[i], idIndex, GROUPBOX_INNER_TOP + 8, groupLeftMargin, groupWidth, init);
        if (pdoItems[i].isEditable) ++editableCounter;
    }

    // Apply font and recalc layout
    SetFont(hFont);
    ResizeWidth(groupWidth);
}

void PDODisplayManager::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    if (hGroupBox) ShowWindow(hGroupBox, cmd);
    if (hOutputHeader) ShowWindow(hOutputHeader, cmd);
    if (hInputHeader) ShowWindow(hInputHeader, cmd);
    for (auto &item : pdoItems) {
        if (item.hLabel) ShowWindow(item.hLabel, cmd);
        if (item.hValue) ShowWindow(item.hValue, cmd);
        if (item.hEdit) ShowWindow(item.hEdit, cmd);
        if (item.hButton) ShowWindow(item.hButton, cmd);
    }
}

void PDODisplayManager::SetTop(int top) {
    currentY = top;
    if (hGroupBox) {
        RECT r;
        GetWindowRect(hGroupBox, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hGroupBox, NULL, p.x, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (hOutputHeader) {
        RECT r;
        GetWindowRect(hOutputHeader, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hOutputHeader, NULL, p.x, top + GROUPBOX_INNER_TOP, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (hInputHeader) {
        RECT r;
        GetWindowRect(hInputHeader, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hInputHeader, NULL, p.x, top + GROUPBOX_INNER_TOP, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

int PDODisplayManager::GetHeight() const {
    return groupHeight;
}

void PDODisplayManager::ResizeHeight(int height) {
    groupHeight = height;
    if (hGroupBox) {
        RECT r;
        GetWindowRect(hGroupBox, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hGroupBox, NULL, p.x, p.y, groupWidth, height, SWP_NOZORDER);
    }
}

// Implementation of the added member functions
void PDODisplayManager::UpdateItemValue(size_t index, const std::wstring &newValue) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowTextW(item.hValue, newValue.c_str());
    }
}

void PDODisplayManager::SetItemEditable(size_t index, bool editable) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hEdit && IsWindow(item.hEdit)) {
        EnableWindow(item.hEdit, editable);
    }
}

void PDODisplayManager::SetItemButtonText(size_t index, const std::wstring &btnText) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowTextW(item.hButton, btnText.c_str());
    }
}

void PDODisplayManager::SelectSlave(int slaveIndex) {
    if (slaveIndex < 0 || slaveIndex >= (int)pdoItems.size()) return;
    selectedSlave = slaveIndex;
    // Update all items: selected slave gets white text on blue background
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        auto &item = pdoItems[i];
        if (i == slaveIndex) {
            // Selected slave: white text, blue background
            SetBkColor(item.hLabel ? GetDC(item.hLabel) : NULL, RGB(0, 0, 255));
            SetTextColor(item.hLabel ? GetDC(item.hLabel) : NULL, RGB(255, 255, 255));
        } else {
            // Deselect: default colors
            SetBkColor(item.hLabel ? GetDC(item.hLabel) : NULL, RGB(240, 240, 240));
            SetTextColor(item.hLabel ? GetDC(item.hLabel) : NULL, RGB(0, 0, 0));
        }
    }
}

void PDODisplayManager::BuildPDOItemsFromSDO(const EtherCATPDOConfig &pdoConfig) {
    Clear(); // Clear existing items
    // Create new PDO items based on the provided SDO configuration
    for (const auto &entry : pdoConfig.entries) {
        PDOItem item;
        item.isOutput = entry.isOutput;
        item.isEditable = entry.isEditable;
        item.name = entry.name;
        item.dataType = entry.dataType;
        item.unit = entry.unit;
        item.minValue = entry.minValue;
        item.maxValue = entry.maxValue;
        item.defaultValue = entry.defaultValue;
        item.currentValue = entry.currentValue;
        // Optionally initialize item.hLabel, item.hValue, etc. here
        pdoItems.push_back(item);
    }
    // Re-initialize the display manager to create controls for the new items
    // Use existing font and parent window handle
    if (isInitialized) {
        Initialize(hParent, GetModuleHandle(NULL), (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0),
                   currentY, groupLeftMargin, groupWidth, groupHeight);
    }
}

void PDODisplayManager::RefreshItemValues(const std::vector<std::wstring> &newValues) {
    if (newValues.size() != pdoItems.size()) return;
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        pdoItems[i].currentValue = newValues[i];
        UpdateItemValue(i, newValues[i]);
    }
}

void PDODisplayManager::SetItemUnit(size_t index, const std::wstring &unit) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.unit = unit;
    // Optionally update the display control for this item
}

void PDODisplayManager::SetItemMinMax(size_t index, const std::wstring &minVal, const std::wstring &maxVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.minValue = minVal;
    item.maxValue = maxVal;
    // Optionally update the display control for this item
}

void PDODisplayManager::SetItemDefault(size_t index, const std::wstring &defVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.defaultValue = defVal;
    // Optionally update the display control for this item
}

void PDODisplayManager::SetItemDataType(size_t index, const std::wstring &dataType) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.dataType = dataType;
    // Optionally update the display control for this item
}

void PDODisplayManager::EnableItem(size_t index, bool enable) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        EnableWindow(item.hLabel, enable);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        EnableWindow(item.hValue, enable);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        EnableWindow(item.hEdit, enable);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        EnableWindow(item.hButton, enable);
    }
}

void PDODisplayManager::DisableAllItems() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        EnableItem(i, false);
    }
}

void PDODisplayManager::EnableAllItems() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        EnableItem(i, true);
    }
}

void PDODisplayManager::SetItemColor(size_t index, COLORREF color) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        HDC hdc = GetDC(item.hLabel);
        SetTextColor(hdc, color);
        ReleaseDC(item.hLabel, hdc);
    }
}

void PDODisplayManager::SetItemBackgroundColor(size_t index, COLORREF bgColor) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        HDC hdc = GetDC(item.hLabel);
        SetBkColor(hdc, bgColor);
        ReleaseDC(item.hLabel, hdc);
    }
}

void PDODisplayManager::ResetItemColors() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        SetItemColor(i, RGB(0, 0, 0)); // Reset to black text
        SetItemBackgroundColor(i, RGB(240, 240, 240)); // Reset to default background
    }
}

void PDODisplayManager::UpdateItemDisplay(size_t index) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    // Update the display for this item based on its current value, editable state, etc.
    // This may involve showing/hiding controls, enabling/disabling edits, updating text, etc.
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowTextW(item.hValue, item.currentValue.c_str());
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        EnableWindow(item.hEdit, item.isEditable);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        // Update button text or functionality if needed
    }
}

void PDODisplayManager::RefreshDisplay() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        UpdateItemDisplay(i);
    }
}

void PDODisplayManager::SetParent(HWND parent) {
    hParent = parent;
    if (hGroupBox) {
        ::SetParent(hGroupBox, parent);
    }
    for (auto &item : pdoItems) {
        if (item.hLabel) ::SetParent(item.hLabel, parent);
        if (item.hValue) ::SetParent(item.hValue, parent);
        if (item.hEdit) ::SetParent(item.hEdit, parent);
        if (item.hButton) ::SetParent(item.hButton, parent);
    }
}

void PDODisplayManager::SetFont(HFONT hFont) {
    if (hGroupBox) {
        SendMessageW(hGroupBox, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
    for (auto &item : pdoItems) {
        if (item.hLabel) SendMessageW(item.hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (item.hValue) SendMessageW(item.hValue, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (item.hEdit) SendMessageW(item.hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
        if (item.hButton) SendMessageW(item.hButton, WM_SETFONT, (WPARAM)hFont, TRUE);
    }
}

void PDODisplayManager::SetGroupBoxText(const std::wstring &text) {
    if (hGroupBox) {
        SetWindowTextW(hGroupBox, text.c_str());
    }
}

void PDODisplayManager::GetItemRect(size_t index, RECT &rect) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        GetWindowRect(item.hLabel, &rect);
        ScreenToClient(hGroupBox, (POINT*)&rect);
    }
}

void PDODisplayManager::GetGroupBoxRect(RECT &rect) {
    if (hGroupBox) {
        GetWindowRect(hGroupBox, &rect);
        ScreenToClient(hParent, (POINT*)&rect);
    }
}

void PDODisplayManager::SetItemPosition(size_t index, int x, int y) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        SetWindowPos(item.hLabel, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowPos(item.hValue, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetWindowPos(item.hEdit, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowPos(item.hButton, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void PDODisplayManager::MoveItem(size_t index, int dx, int dy) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        RECT rc; GetWindowRect(item.hLabel, &rc);
        SetWindowPos(item.hLabel, NULL, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        RECT rc; GetWindowRect(item.hValue, &rc);
        SetWindowPos(item.hValue, NULL, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        RECT rc; GetWindowRect(item.hEdit, &rc);
        SetWindowPos(item.hEdit, NULL, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        RECT rc; GetWindowRect(item.hButton, &rc);
        SetWindowPos(item.hButton, NULL, rc.left + dx, rc.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void PDODisplayManager::ResizeItem(size_t index, int newWidth, int newHeight) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        SetWindowPos(item.hLabel, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowPos(item.hValue, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetWindowPos(item.hEdit, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowPos(item.hButton, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
}

void PDODisplayManager::SetItemRange(size_t index, const std::wstring &minVal, const std::wstring &maxVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.minValue = minVal;
    item.maxValue = maxVal;
    // Optionally update the display control for this item
}

void PDODisplayManager::SetItemValue(size_t index, const std::wstring &value) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.currentValue = value;
    // Update the display control for this item
    UpdateItemValue(index, value);
}

void PDODisplayManager::SetItemName(size_t index, const std::wstring &name) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.name = name;
    // Optionally update the display control for this item
}

// Placeholder implementations for other required methods
bool PDODisplayManager::RecalculateOffsetsFromSDO(class EtherCATMaster* master, int slaveIndex) {
    // Recalculate byte offsets for existing pdoItems using master's IO base offsets.
    // The project does not expose a GetPDOConfig API on EtherCATMaster, so
    // we rely on previously populated pdoItems (usually built from ESI) and
    // compute their byte offsets relative to the slave's IO base.
    if (!master || slaveIndex <= 0) return false;
    if (pdoItems.empty()) return false; // nothing to map

    int outBase = 0, inBase = 0;
    if (!master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
        // could not obtain IO bases
        return false;
    }

    // Walk items and assign byte offsets sequentially. This is a best-effort
    // mapping: it uses each item's bitLength (if set) to allocate bytes.
    int curOut = outBase;
    int curIn = inBase;
    for (auto &it : pdoItems) {
        int bytes = 1;
        if (it.bitLength > 0) bytes = (it.bitLength + 7) / 8;
        else bytes = 4; // fallback to 4 bytes if unknown

        if (it.isOutput) {
            it.byteOffset = curOut - outBase; // store relative to IO base
            curOut += bytes;
        } else {
            it.byteOffset = curIn - inBase; // relative to input base
            curIn += bytes;
        }
    }

    // Recreate controls to reflect new offsets (if UI already initialized)
    if (isInitialized) RebuildPDOControls();
    return true;
}

bool PDODisplayManager::WriteToIOMap(class EtherCATMaster* master, int slaveIndex, const std::wstring& itemName, int32_t value) {
    if (!master || slaveIndex <= 0) return false;

    // Convert name to UTF-8 for name-based API
    std::string nameUtf8(itemName.begin(), itemName.end());

    // Helper to post simple UTF-8 log back to UI thread (non-critical)
    auto PostUtf8Log = [&](const std::wstring &wmsg) {
        if (!hParent) return;
        std::string *s = new std::string(WideToUtf8(wmsg));
        SendMessageW(hParent, WM_USER_POST_UTF8_LOG, 0, (LPARAM)s);
    };

    // If no explicit item given, log available items and return
    if (itemName.empty()) {
        std::wstringstream dump;
        dump << L"[PDO] itemName empty; available items:\r\n";
        for (size_t i = 0; i < pdoItems.size(); ++i) {
            const auto &it = pdoItems[i];
            dump << L"  [" << i << L"] " << it.name << L" addr=" << it.address << L" offset=" << it.byteOffset << L" bits=" << (int)it.bitLength << L"\r\n";
        }
        PostUtf8Log(dump.str());
        return false;
    }

    int outBase = 0, inBase = 0;
    if (master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
        for (const auto &it : pdoItems) {
            if (it.name != itemName) continue;
            int bytes = (it.bitLength > 0) ? ((it.bitLength + 7) / 8) : 4;
            std::vector<uint8_t> buf(bytes);
            for (int b = 0; b < bytes; ++b) buf[b] = (uint8_t)((value >> (8 * b)) & 0xFF);
            int absOffset = (it.isOutput ? outBase : inBase) + it.byteOffset;
            std::wstringstream wlog; wlog << L"[PDO] StageOutputBytes absOffset=" << absOffset << L" len=" << bytes << L"\r\n";
            PostUtf8Log(wlog.str());

            // Attempt to stage outputs via master's API only. Do NOT write directly to IOmap here.
            bool staged = master->StageOutputBytes(absOffset, buf.data(), bytes);
            if (staged) {
                // update local UI cached display value
                for (size_t i = 0; i < pdoItems.size(); ++i) {
                    if (pdoItems[i].name == itemName) {
                        pdoItems[i].currentValue = FormatValue(value, pdoItems[i].bitLength);
                        pdoItems[i].lastWriteTime = std::chrono::steady_clock::now();
                        pdoItems[i].lastWrittenInt = value;
                        pdoItems[i].hasRecentWrite = true;
                        UpdateItemValue((int)i, pdoItems[i].currentValue);
                        break;
                    }
                }
                return true;
            } else {
                PostUtf8Log(L"[PDO] StageOutputBytes failed\r\n");
                // Do NOT fallback to WritePDOData or SDO. Return failure to caller.
                return false;
            }
        }
    }

    // Could not map item or GetSlaveIOBaseBytes failed: do not fallback to SDO or other paths.
    PostUtf8Log(L"[PDO] Could not map item or get IO base; WriteToIOMap failed\r\n");
    return false;
}

std::vector<std::wstring> PDODisplayManager::GetEditableItems() const {
    std::vector<std::wstring> res;
    for (const auto &it : pdoItems) if (it.isEditable) res.push_back(it.name);
    return res;
}

void PDODisplayManager::CreatePDOItem(HWND parent, HINSTANCE hInst, HFONT font,
                                     PDOItem& item, int itemIndex, int y, int leftMargin, int width, 
                                     const std::wstring& initialValue) {
    // Control ID ranges: edits start at 5000, buttons at 6000
    const int baseEdit = 5000;
    const int baseBtn = 6000;
    (void)width; // width is reserved for ResizeWidth; silence unused-parameter warning

    // Label (name only) - create with a minimal width; ResizeWidth will size it later
    item.hLabel = CreateWindowExW(0, L"STATIC", item.name.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  leftMargin + 10, y, 10, 20, parent, NULL, hInst, NULL);
    if (item.hLabel) SendMessageW(item.hLabel, WM_SETFONT, (WPARAM)font, TRUE);

    // Value display (right side)
    // create lightweight; exact width will be set by ResizeWidth
    item.hValue = CreateWindowExW(0, L"STATIC", initialValue.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   leftMargin + 140, y, 10, 22, parent, NULL, hInst, NULL);
    if (item.hValue) SendMessageW(item.hValue, WM_SETFONT, (WPARAM)font, TRUE);

    // Address field (small, left of label)
    std::wstring addr = item.address.empty() ? L"" : item.address;
    // minimal width; will be resized by ResizeWidth
    item.hAddress = CreateWindowExW(0, L"STATIC", addr.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     leftMargin + 2, y, 10, 20, parent, NULL, hInst, NULL);
    if (item.hAddress) SendMessageW(item.hAddress, WM_SETFONT, (WPARAM)font, TRUE);

    // Description field (small, to the right of name)
    item.hDesc = NULL;
    if (!item.description.empty()) {
        // create with minimal width; ResizeWidth will set appropriate size
        item.hDesc = CreateWindowExW(0, L"STATIC", item.description.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      leftMargin + 140, y, 10, 20, parent, NULL, hInst, NULL);
        if (item.hDesc) SendMessageW(item.hDesc, WM_SETFONT, (WPARAM)font, TRUE);
    }
    // Tooltip for description (if available)
    item.hTooltip = NULL;
    if (!item.description.empty()) {
        HWND hTip = CreateWindowExW(0, TOOLTIPS_CLASS, NULL,
                                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    parent, NULL, hInst, NULL);
        if (hTip) {
            TOOLINFOW ti = {0};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = parent;
            ti.uId = (UINT_PTR)item.hLabel;
            // lpszText must be non-const for TOOLINFO, cast is safe for our lifetime
            ti.lpszText = const_cast<LPWSTR>(item.description.c_str());
            SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            item.hTooltip = hTip;
        }
    }
    if (item.isEditable) {
        int eid = baseEdit + itemIndex;
        int bid = baseBtn + itemIndex;
    // create edit/button with minimal widths; ResizeWidth will compute final sizes
    item.hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initialValue.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                     leftMargin + 320, y, 10, 22, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(eid)), hInst, NULL);
        if (item.hEdit) SendMessageW(item.hEdit, WM_SETFONT, (WPARAM)font, TRUE);
    item.hButton = CreateWindowW(L"BUTTON", L"套用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                     leftMargin + 426, y, 10, 22, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(bid)), hInst, NULL);
        if (item.hButton) SendMessageW(item.hButton, WM_SETFONT, (WPARAM)font, TRUE);
    } else {
        item.hEdit = NULL;
        item.hButton = NULL;
    }
}

std::wstring PDODisplayManager::FormatValue(int32_t value, uint8_t bitLength) {
    (void)bitLength; // parameter may be unused for certain PDO types - silence C4100
    std::wstringstream ss; ss << value; return ss.str();
}

void PDODisplayManager::ResizeWidth(int width)
{
    if (!hParent || !hGroupBox) return;
    if (pdoItems.empty()) return;
    
    (void)width; // width is used indirectly via ResizeWidth consumers; silence unused parameter warning if not referenced
    groupWidth = width;
    
    // Resize group box itself
    RECT r;
    GetWindowRect(hGroupBox, &r);
    POINT p = { r.left, r.top };
    ScreenToClient(hParent, &p);
    SetWindowPos(hGroupBox, NULL, p.x, p.y, groupWidth, groupHeight, SWP_NOZORDER);
    
    // Calculate layout parameters
    const int spacing = 6;
    // Reduce inner left margin so content can use more horizontal space
    const int innerLeft = 10;
    // Reduce inner margin to allow wider innerWidth
    const int innerMargin = 20;
    int innerWidth = groupWidth - innerMargin;
    if (innerWidth < 200) innerWidth = 200;
    const int minColWidth = 140;
    const int colGap = 10;
    bool singleColumn = (innerWidth <= (minColWidth * 2 + colGap));
    int colWidth = singleColumn ? innerWidth : (innerWidth - colGap) / 2;
    int leftX = innerLeft;
    int rightX = singleColumn ? leftX : (innerLeft + colWidth + colGap);
    
    // Compute top baseline
    HFONT hFont = (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0);
    if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int headerHeight;
    {
        HDC hdc = GetDC(hGroupBox);
        HFONT oldf = (HFONT)SelectObject(hdc, hFont);
        TEXTMETRIC tm;
        GetTextMetrics(hdc, &tm);
        headerHeight = tm.tmHeight;
        SelectObject(hdc, oldf);
        ReleaseDC(hGroupBox, hdc);
    }
    // Start baseline: place first row after group inner top + header + a small gap
    int baseY = GROUPBOX_INNER_TOP + headerHeight + ROW_SPACING;
    int yLeft = baseY;
    int yRight = baseY;
    int yStack = baseY;
    
    // Lambda to layout a single PDO item (coordinates are relative to hParent, must add currentY + groupbox inner offset)
    auto layoutItem = [&](PDOItem& item, int colX, int colY, int col_W, int& nextY) {
        if (!item.hLabel) return;
        // Convert relative coordinates to absolute parent coordinates
        int absoluteX = groupLeftMargin + colX;
        int absoluteY = currentY + colY;
        int cur = absoluteX;
        const int sp = spacing;
        // Reserve left column for address and a description column after name
        // Make these adaptive so they don't overflow the column width
    int addressW = std::min(80, std::max(40, col_W / 10));
    int descW = std::min(160, std::max(60, col_W / 5));
        // available width after address and desc (and spacing)
        int availW = col_W - addressW - descW - sp*2;
        // Ensure a sensible minimum available area for label+controls
        const int minAvailW = 120;
        if (availW < minAvailW) {
            int need = minAvailW - availW;
            // try shrinking desc first
            int shrinkDesc = std::min(need, std::max(0, descW - 40));
            descW -= shrinkDesc; need -= shrinkDesc; availW += shrinkDesc;
            // then shrink address if still needed
            if (need > 0) {
                int shrinkAddr = std::min(need, std::max(0, addressW - 30));
                addressW -= shrinkAddr; need -= shrinkAddr; availW += shrinkAddr;
            }
            // if still insufficient, clamp availW to a small positive number to avoid negative widths
            if (availW < 40) availW = 40;
        }
        // New adaptive layout: evenly distribute available column width into logical sub-columns.
        // We'll compute a set of target widths for [address, name, desc, edit, button, value] depending on
        // whether the item is editable. All widths are at least a small minimum to remain usable.
        const int MIN_ADDRESS = 30;
        const int MIN_NAME = 60;
        const int MIN_DESC = 40;
        const int MIN_EDIT = 50;
        const int MIN_BTN = 40;
        const int MIN_VALUE = 40;

        if (item.isEditable) {
            // target logical columns: address, name, desc (optional), edit, button, value
            int logicalCols = 5; // address, name+desc, edit, button, value (we'll fold desc into name area when small)
            int baseW = col_W / logicalCols;
            int addressW2 = std::max(MIN_ADDRESS, baseW / 3);
            int editW2 = std::max(MIN_EDIT, baseW);
            int btnW2 = std::max(MIN_BTN, baseW / 2);
            int valueW2 = std::max(MIN_VALUE, baseW);
            // remaining width allocated to name/desc area
            int used = addressW2 + editW2 + btnW2 + valueW2 + sp * 4;
            int nameArea = col_W - used;
            if (nameArea < MIN_NAME + MIN_DESC) {
                // if too small, shrink edit/value first
                int deficit = (MIN_NAME + MIN_DESC) - nameArea;
                int shrinkFrom = (editW2 - MIN_EDIT) + (valueW2 - MIN_VALUE);
                int shrink = std::min(deficit, shrinkFrom);
                if (shrink > 0) {
                    int take = std::min(shrink, editW2 - MIN_EDIT);
                    editW2 -= take; shrink -= take;
                    int take2 = std::min(shrink, valueW2 - MIN_VALUE);
                    valueW2 -= take2; shrink -= take2;
                }
                // recompute used and nameArea
                used = addressW2 + editW2 + btnW2 + valueW2 + sp * 4;
                nameArea = col_W - used;
                if (nameArea < (MIN_NAME)) nameArea = MIN_NAME;
            }

            int nameW2 = nameArea; // includes desc region if present

            // vertical centering
            int labelH = 20; int ctrlH = 22;
            int labelTop = absoluteY + (ROW_HEIGHT - labelH) / 2;
            int ctrlTop  = absoluteY + (ROW_HEIGHT - ctrlH) / 2;

            // place address
            if (item.hAddress) SetWindowPos(item.hAddress, NULL, absoluteX, labelTop, addressW2, labelH, SWP_NOZORDER);
            cur += addressW2 + sp;

            // place name label in nameW2 area; if desc exists, reserve ~30% for desc
            int descReserve = (item.hDesc ? std::min((int)(nameW2/3), 160) : 0);
            int nameOnlyW = nameW2 - descReserve - sp;
            if (nameOnlyW < MIN_NAME) { nameOnlyW = MIN_NAME; descReserve = nameW2 - nameOnlyW - sp; if (descReserve < 0) descReserve = 0; }
            if (item.hLabel) SetWindowPos(item.hLabel, NULL, cur, labelTop, nameOnlyW, labelH, SWP_NOZORDER);
            cur += nameOnlyW + sp;

            if (item.hDesc && descReserve > 0) {
                SetWindowPos(item.hDesc, NULL, cur, labelTop, descReserve, labelH, SWP_NOZORDER);
                cur += descReserve + sp;
            }

            // place edit
            if (item.hEdit)  SetWindowPos(item.hEdit,  NULL, cur, ctrlTop, editW2, ctrlH, SWP_NOZORDER);
            cur += editW2 + sp;

            // place apply button
            if (item.hButton) SetWindowPos(item.hButton, NULL, cur, ctrlTop, btnW2, ctrlH, SWP_NOZORDER);
            cur += btnW2 + sp;

            // place value on the right
            if (item.hValue) SetWindowPos(item.hValue, NULL, cur, ctrlTop, valueW2, ctrlH, SWP_NOZORDER);

        } else {
            // non-editable: address | name+desc | value
            int logicalCols = 3;
            int baseW = col_W / logicalCols;
            int addressW2 = std::max(MIN_ADDRESS, baseW / 3);
            int valueW2 = std::max(MIN_VALUE, baseW);
            int used = addressW2 + valueW2 + sp * 2;
            int nameArea = col_W - used;
            if (nameArea < MIN_NAME) nameArea = MIN_NAME;
            int descReserve = (item.hDesc ? std::min(nameArea/3, 160) : 0);
            int nameOnlyW = nameArea - descReserve - sp;

            int labelH = 20; int ctrlH = 22;
            int labelTop = absoluteY + (ROW_HEIGHT - labelH) / 2;
            int ctrlTop  = absoluteY + (ROW_HEIGHT - ctrlH) / 2;

            if (item.hAddress) SetWindowPos(item.hAddress, NULL, absoluteX, labelTop, addressW2, labelH, SWP_NOZORDER);
            cur += addressW2 + sp;
            if (item.hLabel) SetWindowPos(item.hLabel, NULL, cur, labelTop, nameOnlyW, labelH, SWP_NOZORDER);
            cur += nameOnlyW + sp;
            if (item.hDesc && descReserve > 0) {
                SetWindowPos(item.hDesc, NULL, cur, labelTop, descReserve, labelH, SWP_NOZORDER);
                cur += descReserve + sp;
            }
            if (item.hValue) SetWindowPos(item.hValue, NULL, cur, ctrlTop, valueW2, ctrlH, SWP_NOZORDER);
        }
        nextY = colY + ROW_HEIGHT + ROW_SPACING;
    };

    // Layout items in two columns or stacked single column
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        bool isOut = pdoItems[i].isOutput;
        int x = singleColumn ? leftX : (isOut ? leftX : rightX);
        int w = colWidth;
        int &yRef = singleColumn ? yStack : (isOut ? yLeft : yRight);
        int nextY = yRef;
        layoutItem(pdoItems[i], x, yRef, w, nextY);
        // layoutItem sets nextY to the next available Y; adopt it directly
        yRef = nextY;
    }

    // Update group box height after resizing/relayout
    {
        int maxBottomClient = 0;
        for (const auto &it : pdoItems) {
            if (it.hLabel && IsWindow(it.hLabel)) {
                RECT rc; GetWindowRect(it.hLabel, &rc);
                POINT pt = { rc.left, rc.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hValue && IsWindow(it.hValue)) {
                RECT rc; GetWindowRect(it.hValue, &rc);
                POINT pt = { rc.left, rc.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hEdit && IsWindow(it.hEdit)) {
                RECT rc; GetWindowRect(it.hEdit, &rc);
                POINT pt = { rc.left, rc.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hButton && IsWindow(it.hButton)) {
                RECT rc; GetWindowRect(it.hButton, &rc);
                POINT pt = { rc.left, rc.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
        }

        int desiredClientH = maxBottomClient + bottomPadding;
        if (desiredClientH < minGroupHeight) desiredClientH = minGroupHeight;

        // Convert desired client size to total window size (account for frame)
        RECT adj = { 0, 0, groupWidth, desiredClientH };
        DWORD style = (DWORD)GetWindowLongPtr(hGroupBox, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtr(hGroupBox, GWL_EXSTYLE);
        AdjustWindowRectEx(&adj, style, FALSE, exstyle);
        int totalH = adj.bottom - adj.top;

        if (totalH != groupHeight) {
            groupHeight = totalH;
            RECT rr2; GetWindowRect(hGroupBox, &rr2);
            POINT pp2 = { rr2.left, rr2.top }; ScreenToClient(hParent, &pp2);
            SetWindowPos(hGroupBox, NULL, pp2.x, pp2.y, groupWidth, groupHeight, SWP_NOZORDER);
        }
    }
}

void PDODisplayManager::Clear() {
    // Destroy all item controls
    for (auto &item : pdoItems) {
        if (item.hLabel) { DestroyWindow(item.hLabel); item.hLabel = NULL; }
        if (item.hValue) { DestroyWindow(item.hValue); item.hValue = NULL; }
        if (item.hEdit) { DestroyWindow(item.hEdit); item.hEdit = NULL; }
        if (item.hButton) { DestroyWindow(item.hButton); item.hButton = NULL; }
    if (item.hAddress) { DestroyWindow(item.hAddress); item.hAddress = NULL; }
    }
    pdoItems.clear();
}

// Helper: read a PDOItem's numeric value from a per-slave snapshot (outputs first layout)
// Returns true on success and fills outValue (LE parsed, capped to 32 bits)
// Use free function ReadPDOItemValueFromSnapshot in include/PDOHelpers.h

// END: PDODisplay.cpp merged content

// =====================================================================
// Fault Reset button subclass and handler
// =====================================================================

// Subclass procedure to capture press/release on the fault reset button
LRESULT CALLBACK FaultResetButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* wnd = reinterpret_cast<Win32Window*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        // Start fault reset assert (set bit7 = 1)
        wnd->OnFaultReset(true);
        break;
    case WM_LBUTTONUP:
        // Release fault reset (set bit7 = 0)
        wnd->OnFaultReset(false);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            wnd->OnFaultReset(true);
        }
        break;
    case WM_KEYUP:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            wnd->OnFaultReset(false);
        }
        break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Instance method: initiate fault reset (async)
void Win32Window::OnFaultReset(bool assert)
{
    // Capture selected slave index from UI (combo box ID 2001)
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
    if (!etherCATMaster) {
        PrependStatus(L"[FaultReset] EtherCAT master not available\r\n");
        return;
    }

    // Dispatch background worker to avoid blocking UI; delegate core work to MotionController
    if (motionController) {
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->FaultResetAsync(slaveIdx, assert);
            // Post UTF8 log to UI
            std::string *msg = new std::string(WideToUtf8(r.message));
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)msg);
        }).detach();
    } else {
        PrependStatus(L"[FaultReset] MotionController not initialized\r\n");
    }
}

// Async implementations moved to MotionController; keep a small stub to avoid link errors
void Win32Window::OnFaultResetAsync(int slaveIdx, bool assert)
{
    // Intentionally left as stub. Core implementation now lives in MotionController::FaultResetAsync.
    (void)slaveIdx; (void)assert;
}

// =====================================================================
// Immediate Stop (Quick Stop) button subclass and handler
// =====================================================================

LRESULT CALLBACK ImmediateStopButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* wnd = reinterpret_cast<Win32Window*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        wnd->OnImmediateStop(true);
        break;
    case WM_LBUTTONUP:
        wnd->OnImmediateStop(false);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnImmediateStop(true);
        break;
    case WM_KEYUP:
        if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnImmediateStop(false);
        break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void Win32Window::OnImmediateStop(bool assert)
{
    // Per request: only act on press (assert==true). Ignore release (no clear).
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[ImmediateStop] No slave selected\r\n"); return; }
    if (!etherCATMaster) { PrependStatus(L"[ImmediateStop] EtherCAT master not available\r\n"); return; }

    if (motionController) {
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->ImmediateStopAsync(slaveIdx, assert);
            std::string *msg = new std::string(WideToUtf8(r.message));
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)msg);
        }).detach();
    } else {
        PrependStatus(L"[ImmediateStop] MotionController not initialized\r\n");
    }
}

void Win32Window::OnImmediateStopAsync(int slaveIdx, bool assert)
{
    (void)slaveIdx; (void)assert; // stubbed - logic moved to MotionController::ImmediateStopAsync
}

// =====================================================================
// Power Off button subclass and handler
// =====================================================================

// Subclass procedure to capture press on the Power Off button
LRESULT CALLBACK PowerOffButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* wnd = reinterpret_cast<Win32Window*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        wnd->OnPowerOff(true);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnPowerOff(true);
        break;
    // Release is a no-op for PowerOff (only act on press)
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void Win32Window::OnPowerOff(bool assert)
{
    // Only act on press; ignore release
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[PowerOff] No slave selected\r\n"); return; }
    if (!etherCATMaster) { PrependStatus(L"[PowerOff] EtherCAT master not available\r\n"); return; }

    if (motionController) {
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->PowerOffAsync(slaveIdx, assert);
            std::string *msg = new std::string(WideToUtf8(r.message));
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)msg);
        }).detach();
    } else {
        PrependStatus(L"[PowerOff] MotionController not initialized\r\n");
    }
}

void Win32Window::OnPowerOffAsync(int slaveIdx, bool assert)
{
    (void)slaveIdx; (void)assert; // stub - logic moved to MotionController::PowerOffAsync
}

// =====================================================================
// Power On button subclass and handler
// =====================================================================

// Subclass procedure to capture press on the Power On button
LRESULT CALLBACK PowerOnButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* wnd = reinterpret_cast<Win32Window*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        wnd->OnPowerOn(true);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnPowerOn(true);
        break;
    // Per request: release is a no-op for PowerOn (only act on press)
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void Win32Window::OnPowerOn(bool assert)
{
    // Only act on press; ignore release
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[PowerOn] No slave selected\r\n"); return; }
    if (!etherCATMaster) { PrependStatus(L"[PowerOn] EtherCAT master not available\r\n"); return; }

    if (motionController) {
        std::thread([this, slaveIdx, assert]() {
            MotionResult r = this->motionController->PowerOnAsync(slaveIdx, assert);
            std::string *msg = new std::string(WideToUtf8(r.message));
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)msg);
        }).detach();
    } else {
        PrependStatus(L"[PowerOn] MotionController not initialized\r\n");
    }
}

// Subclass procedure to capture press on the Home button
LRESULT CALLBACK HomeButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    Win32Window* wnd = reinterpret_cast<Win32Window*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        // log a quick non-intrusive debug message and call handler on press
        {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            PostMessage(top, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("Home button pressed\n"));
        }
        wnd->OnHome(true);
        return 0; // consume the mouse down so WM_COMMAND on release does not re-trigger
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            PostMessage(top, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("Home key invoked\n"));
            wnd->OnHome(true);
            return 0; // consume keydown to avoid duplicate WM_COMMAND
        }
        break;
    }

    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Home (Homing) button handler
void Win32Window::OnHome(bool assert)
{
    // Only act on press
    if (!assert) return;
    int slaveIdx = -1;
    HWND hCombo = GetDlgItem(hContentWnd, 2001);
    if (hCombo) {
        LRESULT sel = SendMessageW(hCombo, CB_GETCURSEL, 0, 0);
        if (sel != CB_ERR) slaveIdx = (int)(sel + 1);
    }

    if (slaveIdx <= 0) { PrependStatus(L"[Home] No slave selected\r\n"); return; }
    if (!etherCATMaster) { PrependStatus(L"[Home] EtherCAT master not available\r\n"); return; }

    // Immediate UI-thread debug: post a non-intrusive UTF-8 log to confirm handler reached
    {
        std::string dbg = std::string("OnHome invoked slave=") + std::to_string(slaveIdx) + "\n";
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string(dbg));
    }

    // Prevent re-entrancy: set homingBusy atomically on UI thread and disable the button.
    bool expected = false;
    if (!homingBusy.compare_exchange_strong(expected, true)) {
        // already busy
        PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("Homing already in progress\n"));
        return;
    }
    if (hBtnHome) EnableWindow(hBtnHome, FALSE);

    // Launch background worker delegating to MotionController. When finished, clear homingBusy and re-enable button.
    if (motionController) {
        std::thread([this, slaveIdx]() {
            MotionResult r = this->motionController->HomeAsync(slaveIdx, true);
            // Post the result message
            std::string *msg = new std::string(WideToUtf8(r.message));
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)msg);
            // clear busy and request UI to re-enable
            this->homingBusy.store(false);
            PostMessage(this->hwnd, WM_USER_POST_UTF8_LOG, 0, (LPARAM)new std::string("ENABLE_HOME\n"));
        }).detach();
    } else {
        PrependStatus(L"[Home] MotionController not initialized\r\n");
        this->homingBusy.store(false);
        if (hBtnHome) EnableWindow(hBtnHome, TRUE);
    }
}

void Win32Window::OnHomeAsync(int slaveIdx, bool assert)
{
    (void)slaveIdx; (void)assert; // stub - homing flow moved to MotionController::HomeAsync
}

void Win32Window::OnPowerOnAsync(int slaveIdx, bool assert)
{
    (void)slaveIdx; (void)assert; // stub - logic moved to MotionController::PowerOnAsync
}
