// StatusPane.cpp
// Moved out from MainWindow.cpp to separate compilation unit.

#include "MainWindow.h"
// no Http server header needed here
#include <windows.h>
#include <richedit.h>
#include <deque>
#include <vector>
#include <unordered_set>
#include "../include/core/CoreHelpers.h"

static constexpr LPCWSTR CHILD_STATUS_BG_COLOR = L"ChildStatusBgColor";
// Status viewer paging configuration
static constexpr size_t STATUS_FILE_PAGE_BYTES = 64 * 1024; // 64KB pages from file
static constexpr size_t STATUS_VIEW_MAX_WCHARS = 2 * 1024 * 1024; // 2M wide chars max held in view

// internal state for the status viewer (single child window assumed)
static std::deque<size_t> g_pageCharCounts; // char counts of prepended older pages (front=oldest)
static size_t g_totalViewChars = 0; // total char count in the RICHEDIT
static size_t g_fileLoadedBytesFromEnd = 0; // how many bytes from file end have been loaded (older pages appended)
static bool g_viewerInitialized = false;
// Keep track of how many characters at the top of the view were inserted from the
// in-memory status buffer so we can replace that region instead of inserting again
// (avoids doubled display when the file tail contains overlapping lines).
static size_t g_lastMemInsertChars = 0;

// custom message to request trimming of viewer after inserts
#define WM_USER_TRIM_STATUS_VIEW (WM_USER + 300)
// request to reset/clear viewer state (e.g., after truncate)
#define WM_USER_RESET_STATUS_VIEW (WM_USER + 301)

// Helper: compute logs file path (same as StatusLogger)
static std::wstring ResolveStatusLogPathLocal() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring logDir = dir + L"\\logs";
    return logDir + L"\\status.log";
}

// NOTE: previous helper to read file range (ReadStatusFileRangeWide) was removed because
// it was never referenced; file I/O is handled inline in the viewer functions above.

// Helper: trim viewer (remove oldest appended pages at the bottom) until under limit
static void TrimViewerIfNeeded(HWND hEdit) {
    if (!hEdit || !IsWindow(hEdit)) return;
    // Recalculate total chars if unknown
    g_totalViewChars = (size_t)GetWindowTextLengthW(hEdit);
    while (g_totalViewChars > STATUS_VIEW_MAX_WCHARS && !g_pageCharCounts.empty()) {
        size_t removeChars = g_pageCharCounts.front();
        long endPos = (long)removeChars;
        SendMessageW(hEdit, EM_SETSEL, 0, (LPARAM)endPos);
        SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
        g_totalViewChars = (size_t)GetWindowTextLengthW(hEdit);
        g_pageCharCounts.pop_front();
    }
}

// Initialize viewer with latest tail page
static void InitializeStatusViewer(HWND hEdit) {
    if (!hEdit || !IsWindow(hEdit)) return;
    // Clear any state
    g_pageCharCounts.clear(); g_totalViewChars = 0; g_fileLoadedBytesFromEnd = 0; g_viewerInitialized = false;
    // Read tail of file
    std::wstring path = ResolveStatusLogPathLocal();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return; // no file
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long filesize = ftell(f);
    if (filesize <= 0) { fclose(f); return; }
    size_t readBytes = (size_t)std::min<long long>((long long)STATUS_FILE_PAGE_BYTES, (long long)filesize);
    size_t offset = (size_t)filesize - readBytes;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return; }
    std::string buf; buf.resize(readBytes);
    size_t r = fread(&buf[0], 1, readBytes, f);
    fclose(f);
    if (r == 0) return;
    buf.resize(r);
    std::wstring w = CoreHelpers::Utf8ToWide(buf);
    std::vector<std::wstring> lines; size_t p = 0;
    while (p < w.size()) {
        size_t nl = w.find(L"\n", p);
        std::wstring line;
        if (nl == std::wstring::npos) { line = w.substr(p); p = w.size(); }
        else { line = w.substr(p, nl - p); p = nl + 1; if (!line.empty() && line.back() == L'\r') line.pop_back(); }
        lines.push_back(line + L"\r\n");
    }
    std::wstring pageText;
    // If a MainWindow instance exists (owner), we can avoid duplicating
    // lines that already exist in the in-memory buffer by removing file
    // tail lines that are present in the in-memory buffer.
    HWND owner = GetParent(hEdit);
    MainWindow* self = nullptr;
    if (owner && IsWindow(owner)) self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(owner, GWLP_USERDATA));
    std::wstring memBuf = self ? self->GetStatusBuffer() : std::wstring();
    std::unordered_set<std::wstring> memLines;
    if (!memBuf.empty()) {
        size_t p2 = 0;
        while (p2 < memBuf.size()) {
            size_t nl2 = memBuf.find(L"\n", p2);
            std::wstring line;
            if (nl2 == std::wstring::npos) { line = memBuf.substr(p2); p2 = memBuf.size(); }
            else { line = memBuf.substr(p2, nl2 - p2); p2 = nl2 + 1; if (!line.empty() && line.back() == L'\r') line.pop_back(); }
            memLines.insert(line);
        }
    }
    for (const auto& originalLine : lines) {
        std::wstring ln = originalLine;
        // strip trailing CRLF for comparison
        if (!ln.empty() && ln.back() == L'\n') ln.pop_back();
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (memLines.find(ln) == memLines.end()) pageText += originalLine;
    }
    // Set editor to this page
    SetWindowTextW(hEdit, pageText.c_str());
    g_pageCharCounts.clear();
    g_pageCharCounts.push_back((size_t)pageText.size());
    g_totalViewChars = (size_t)GetWindowTextLengthW(hEdit);
    g_fileLoadedBytesFromEnd = r; // we've loaded 'r' bytes from the tail
    g_viewerInitialized = true;
}

// Append the next older page from file to the bottom of the editor
static void AppendOlderPage(HWND hEdit) {
    if (!hEdit || !IsWindow(hEdit)) return;
    std::wstring path = ResolveStatusLogPathLocal();
    FILE* f = nullptr; if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return; if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long filesize = ftell(f);
    if ((long)g_fileLoadedBytesFromEnd >= filesize) { fclose(f); return; }
    size_t remaining = (size_t)filesize - g_fileLoadedBytesFromEnd;
    size_t toRead = std::min(STATUS_FILE_PAGE_BYTES, remaining);
    size_t offset = (size_t)filesize - g_fileLoadedBytesFromEnd - toRead;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return; }
    std::string buf; buf.resize(toRead);
    size_t r = fread(&buf[0], 1, toRead, f);
    fclose(f);
    if (r == 0) return;
    buf.resize(r);
    std::wstring w = CoreHelpers::Utf8ToWide(buf);
    std::vector<std::wstring> lines; size_t p = 0;
    while (p < w.size()) {
        size_t nl = w.find(L"\n", p);
        std::wstring line;
        if (nl == std::wstring::npos) { line = w.substr(p); p = w.size(); }
        else { line = w.substr(p, nl - p); p = nl + 1; if (!line.empty() && line.back() == L'\r') line.pop_back(); }
        lines.push_back(line + L"\r\n");
    }
    std::wstring pageText;
    // Apply same deduping when reading a page during append: avoid adding
    // lines that are already shown in the in-memory buffer.
    HWND owner = GetParent(hEdit);
    MainWindow* self = nullptr;
    if (owner && IsWindow(owner)) self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(owner, GWLP_USERDATA));
    std::wstring memBuf = self ? self->GetStatusBuffer() : std::wstring();
    std::unordered_set<std::wstring> memLines;
    if (!memBuf.empty()) {
        size_t p2 = 0;
        while (p2 < memBuf.size()) {
            size_t nl2 = memBuf.find(L"\n", p2);
            std::wstring line;
            if (nl2 == std::wstring::npos) { line = memBuf.substr(p2); p2 = memBuf.size(); }
            else { line = memBuf.substr(p2, nl2 - p2); p2 = nl2 + 1; if (!line.empty() && line.back() == L'\r') line.pop_back(); }
            memLines.insert(line);
        }
    }
    for (const auto& originalLine : lines) {
        std::wstring ln = originalLine;
        if (!ln.empty() && ln.back() == L'\n') ln.pop_back();
        if (!ln.empty() && ln.back() == L'\r') ln.pop_back();
        if (memLines.find(ln) == memLines.end()) pageText += originalLine;
    }
    // prepend older content to the beginning so the viewer stays chronological
    SendMessageW(hEdit, EM_SETSEL, 0, 0);
    SendMessageW(hEdit, EM_REPLACESEL, TRUE, (LPARAM)pageText.c_str());
    g_pageCharCounts.push_front((size_t)pageText.size());
    g_totalViewChars = (size_t)GetWindowTextLengthW(hEdit);
    g_fileLoadedBytesFromEnd += r;
    // Trim if needed
    TrimViewerIfNeeded(hEdit);
}

// Helper: choose readable text color for a background color
static COLORREF ChooseTextColorForBackground(COLORREF bg) {
    BYTE r = GetRValue(bg);
    BYTE g = GetGValue(bg);
    BYTE b = GetBValue(bg);
    // Perceived luminance
    double lum = 0.299 * r + 0.587 * g + 0.114 * b;
    if (lum < 128.0) return RGB(230, 230, 230); // light text for dark bg
    return RGB(0, 0, 0); // dark text for light bg
}

// Child/status window proc (modeless)
static LRESULT CALLBACK ChildStatusWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_DESTROY:
        {
            // Clean up local bg brush if we created one
            HBRUSH hLocal = (HBRUSH)GetPropW(hwnd, L"ChildStatusBgBrush");
            if (hLocal) {
                RemovePropW(hwnd, L"ChildStatusBgBrush");
                DeleteObject(hLocal);
            }
            RemovePropW(hwnd, L"ChildStatusOwner");
            RemovePropW(hwnd, CHILD_STATUS_BG_COLOR);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 1032) {
            MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (self) { self->OnClearStatus(); return 0; }
        }
        if (HIWORD(wParam) == BN_CLICKED && LOWORD(wParam) == 1118) {
            MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (self) {
                LRESULT checked = SendMessageW(GetDlgItem(hwnd, 1118), BM_GETCHECK, 0, 0);
                bool enable = (checked == BST_CHECKED);
                self->SetStatusLogEnabled(enable);
                // show UI-only confirmation so change is visible even if file-writing now disabled
                self->PrependStatusNoFile(enable ? L"[設定] status.log 儲存已啟用\r\n" : L"[設定] status.log 儲存已停用\r\n");
            }
            return 0;
        }
        if (HIWORD(wParam) == EN_VSCROLL) {
            MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (self) { self->MarkStatusScrolled(); }
                // If user scrolled near the top, attempt to prepend older page.
            HWND hEdit = FindWindowExW(hwnd, NULL, L"RICHEDIT50W", NULL);
            if (hEdit && IsWindow(hEdit)) {
                int first = (int)SendMessageW(hEdit, EM_GETFIRSTVISIBLELINE, 0, 0);
                    if (first <= 2) {
                    AppendOlderPage(hEdit);
                }
            }
        }
        break;
    case WM_USER_TRIM_STATUS_VIEW:
        // request to ensure viewer is trimmed under memory cap
        {
            HWND hEdit = FindWindowExW(hwnd, NULL, L"RICHEDIT50W", NULL);
            if (hEdit && IsWindow(hEdit)) TrimViewerIfNeeded(hEdit);
        }
        return 0;

    case WM_USER_RESET_STATUS_VIEW:
        // reset viewer internal state
        g_pageCharCounts.clear(); g_totalViewChars = 0; g_fileLoadedBytesFromEnd = 0; g_viewerInitialized = false;
        g_lastMemInsertChars = 0;
        {
            HWND hEdit = FindWindowExW(hwnd, NULL, L"RICHEDIT50W", NULL);
            if (hEdit && IsWindow(hEdit)) {
                SetWindowTextW(hEdit, L"");
                ShowScrollBar(hEdit, SB_VERT, TRUE);
            }
        }
        return 0;
    case WM_USER_STATUS_UPDATE_BATCH:
        {
            HWND hEdit = FindWindowExW(hwnd, NULL, L"RICHEDIT50W", NULL);
            if (hEdit && IsWindow(hEdit)) {
                // Obtain the owning MainWindow instance stored in GWLP_USERDATA
                MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
                std::wstring buf;
                if (self) buf = self->GetStatusBufferChronological();
                else buf = L"";
                if (!buf.empty()) {
                    long curLen = (long)GetWindowTextLengthW(hEdit);
                    // Replace the region previously inserted as in-memory buffer at the end
                    if (g_lastMemInsertChars > 0) {
                        SendMessageW(hEdit, EM_SETSEL, (WPARAM)(curLen - (long)g_lastMemInsertChars), (LPARAM)curLen);
                    } else {
                        SendMessageW(hEdit, EM_SETSEL, (WPARAM)curLen, (LPARAM)curLen);
                    }
                    SendMessageW(hEdit, EM_REPLACESEL, TRUE, (LPARAM)buf.c_str());
                    g_lastMemInsertChars = buf.size();
                    g_totalViewChars = (size_t)GetWindowTextLengthW(hEdit);
                    if (self && !self->IsStatusUserScrolled()) {
                        long newLen = (long)GetWindowTextLengthW(hEdit);
                        SendMessageW(hEdit, EM_SETSEL, (WPARAM)newLen, (LPARAM)newLen);
                        SendMessageW(hEdit, EM_SCROLLCARET, 0, 0);
                    }
                }
                // Trim viewer if needed after the insert
                TrimViewerIfNeeded(hEdit);
            }
            // Notify the window that the status UI processed the batch so future posts are allowed
            MainWindow* owner = reinterpret_cast<MainWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
            if (owner) owner->MarkStatusUiProcessed();
        }
        return 0;
    case WM_SIZE:
        {
            // Find RichEdit control (class name is RICHEDIT50W)
            HWND hEdit = FindWindowExW(hwnd, NULL, L"RICHEDIT50W", NULL);
            RECT rc; GetClientRect(hwnd, &rc);
            const int btnW = 100, btnH = 22, margin = 8;
            const int chkW = 180; // width for the status.log checkbox
            HWND hBtn = GetDlgItem(hwnd, 1032);
            HWND hChk = GetDlgItem(hwnd, 1118);
            // no load-history button
            int editTop = 0;
            if (hChk && IsWindow(hChk)) {
                // place checkbox to the left of the Clear button
                SetWindowPos(hChk, NULL, rc.right - btnW - chkW - margin - 8, margin, chkW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (hBtn && IsWindow(hBtn)) {
                SetWindowPos(hBtn, NULL, rc.right - btnW - margin, margin, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
                editTop = margin + btnH + margin;
            }
            if (hEdit) {
                SetWindowPos(hEdit, NULL, 0, editTop, rc.right, rc.bottom - editTop, SWP_NOZORDER);
            }
        }
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void MainWindow::ToggleChildStatusWindow()
{
    if (!hChildStatusWnd || !IsWindow(hChildStatusWnd)) {
        // Register class if necessary
        WNDCLASSEX wc = {};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ChildStatusWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"ChildStatusWindowClass";
        RegisterClassEx(&wc);

        // Create modeless window
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        hChildStatusWnd = CreateWindowEx(WS_EX_TOOLWINDOW, L"ChildStatusWindowClass", L"狀態視窗", 
            WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 800, screenHeight, 
            NULL, NULL, hInstance, NULL);
        if (!hChildStatusWnd) return;

        SetWindowLongPtr(hChildStatusWnd, GWLP_USERDATA, (LONG_PTR)this);
        SetPropW(hChildStatusWnd, L"ChildStatusOwner", (HANDLE)hwnd);

        // Get background color from main window
        COLORREF bgCol = GetSysColor(COLOR_WINDOW);
        HWND hOwner = (HWND)GetPropW(hChildStatusWnd, L"ChildStatusOwner");
        if (hOwner && IsWindow(hOwner)) {
            HANDLE hCol = GetPropW(hOwner, L"BigCatBgColor");
            if (hCol) {
                bgCol = (COLORREF)(UINT_PTR)hCol;
                HBRUSH hNew = CreateSolidBrush(bgCol);
                SetPropW(hChildStatusWnd, L"ChildStatusBgBrush", (HANDLE)hNew);
                SetClassLongPtrW(hChildStatusWnd, GCLP_HBRBACKGROUND, (LONG_PTR)hNew);
            }
        }
        SetPropW(hChildStatusWnd, CHILD_STATUS_BG_COLOR, (HANDLE)(UINT_PTR)bgCol);

        // Load RichEdit library
        LoadLibraryW(L"msftedit.dll");

        // Create RichEdit control
        hChildStatusEdit = CreateWindowW(L"RICHEDIT50W", L"", 
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 800, screenHeight, hChildStatusWnd, NULL, hInstance, NULL);
// No OnLoadStatusFile - history-load removed by request
        if (hChildStatusEdit && IsWindow(hChildStatusEdit)) {
            // Set background color
            SendMessageW(hChildStatusEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)bgCol);
            
            // Apply font first
            if (hGuiFont) SendMessageW(hChildStatusEdit, WM_SETFONT, (WPARAM)hGuiFont, TRUE);

            // Initialize viewer with file tail and then inject current in-memory buffer at top
            InitializeStatusViewer(hChildStatusEdit);
            // Insert in-memory buffer at top (if any)
            std::wstring buf = statusLogger.GetBufferChronological();
            if (!buf.empty()) {
                long curLen = (long)GetWindowTextLengthW(hChildStatusEdit);
                if (g_lastMemInsertChars > 0) {
                    SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)(curLen - (long)g_lastMemInsertChars), (LPARAM)curLen);
                } else {
                    SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)curLen, (LPARAM)curLen);
                }
                SendMessageW(hChildStatusEdit, EM_REPLACESEL, TRUE, (LPARAM)buf.c_str());
                g_lastMemInsertChars = buf.size();
                g_totalViewChars = (size_t)GetWindowTextLengthW(hChildStatusEdit);
            }
            
            // Set text color AFTER setting content (SetWindowTextW resets formatting)
            COLORREF textCol = ChooseTextColorForBackground(bgCol);
            CHARFORMAT2W cf = {};
            cf.cbSize = sizeof(CHARFORMAT2W);
            cf.dwMask = CFM_COLOR;
            cf.crTextColor = textCol;
            SendMessageW(hChildStatusEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
            
            long endPos = (long)GetWindowTextLengthW(hChildStatusEdit);
            SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)endPos, (LPARAM)endPos);
            SendMessageW(hChildStatusEdit, EM_SCROLLCARET, 0, 0);
            ShowScrollBar(hChildStatusEdit, SB_VERT, TRUE);
        }

        ShowWindow(hChildStatusWnd, SW_SHOW);
        // Position child status window at the right side of the monitor that contains the owner window.
        // Respect work area (taskbar) and leave a small margin so it doesn't touch the screen edge.
        {
            const int margin = 8;
            RECT ownerRect; ownerRect.left = ownerRect.top = ownerRect.right = ownerRect.bottom = 0;
            HWND owner = (HWND)GetPropW(hChildStatusWnd, L"ChildStatusOwner");
            if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

            HMONITOR hMon = MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONULL);
            if (!hMon) hMon = MonitorFromWindow(hChildStatusWnd, MONITOR_DEFAULTTONEAREST);

            MONITORINFO mi; ZeroMemory(&mi, sizeof(mi)); mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(hMon ? hMon : MonitorFromWindow(hChildStatusWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                // fallback to primary screen work area
                mi.rcWork.left = 0; mi.rcWork.top = 0;
                mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
                mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
            }

            // Calculate size and position to stick to the right edge of work area
            int statusWidth = 800;
            int statusHeight = screenHeight;
            int workW = mi.rcWork.right - mi.rcWork.left;
            int workH = mi.rcWork.bottom - mi.rcWork.top;
            if (statusWidth > workW - 2 * margin) statusWidth = workW - 2 * margin;
            if (statusHeight > workH - 2 * margin) statusHeight = workH - 2 * margin;

            int x = mi.rcWork.right - statusWidth - margin;
            int y = mi.rcWork.top + margin;

            // Apply size/position while keeping window topmost
            SetWindowPos(hChildStatusWnd, HWND_TOPMOST, x, y, statusWidth, statusHeight, SWP_SHOWWINDOW);
            // Also ensure child rich edit control is resized on WM_SIZE handler if needed
        }

        // Create or reparent Clear Status button
            if (hBtnClearStatus && IsWindow(hBtnClearStatus)) {
            SetParent(hBtnClearStatus, hChildStatusWnd);
            LONG style = GetWindowLongPtrW(hBtnClearStatus, GWL_STYLE);
            style &= ~WS_POPUP; style |= WS_CHILD | WS_VISIBLE;
            SetWindowLongPtrW(hBtnClearStatus, GWL_STYLE, style);
            ShowWindow(hBtnClearStatus, SW_SHOW);
        } else {
            hBtnClearStatus = CreateWindowW(L"BUTTON", L"狀態清除", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0, 0, 100, 22, hChildStatusWnd, (HMENU)1032, hInstance, NULL);
            if (hBtnClearStatus && hGuiFont) SendMessageW(hBtnClearStatus, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
        }

        // Create or reparent "儲存 status.log" checkbox (persisted via INI)
        if (hChkStatusLog && IsWindow(hChkStatusLog)) {
            SetParent(hChkStatusLog, hChildStatusWnd);
            LONG style = GetWindowLongPtrW(hChkStatusLog, GWL_STYLE);
            style &= ~WS_POPUP; style |= WS_CHILD | WS_VISIBLE | WS_TABSTOP;
            SetWindowLongPtrW(hChkStatusLog, GWL_STYLE, style);
            ShowWindow(hChkStatusLog, SW_SHOW);
        } else {
            const int chkW = 180, chkH = 22;
            hChkStatusLog = CreateWindowW(L"BUTTON", L"儲存 status.log", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                0, 0, chkW, chkH, hChildStatusWnd, (HMENU)1118, hInstance, NULL);
            if (hChkStatusLog && hGuiFont) SendMessageW(hChkStatusLog, WM_SETFONT, (WPARAM)hGuiFont, TRUE);
            // initialize checkbox state from runtime flag
            SendMessageW(hChkStatusLog, BM_SETCHECK, IsStatusLogEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }

            // no load-history button in child window (single-file design, on-demand paging)

        SendMessageW(hChildStatusWnd, WM_SIZE, 0, 0);
    } else {
        if (IsWindowVisible(hChildStatusWnd)) {
            ShowWindow(hChildStatusWnd, SW_HIDE);
        } else {
            // Update content before showing
            if (hChildStatusEdit && IsWindow(hChildStatusEdit)) {
                // Reapply background color
                HANDLE hColProp = GetPropW(hChildStatusWnd, CHILD_STATUS_BG_COLOR);
                if (hColProp) {
                    COLORREF bgCol = (COLORREF)(UINT_PTR)hColProp;
                    SendMessageW(hChildStatusEdit, EM_SETBKGNDCOLOR, 0, (LPARAM)bgCol);
                }
                
                // Initialize viewer & inject current in-memory buffer on show
                if (!g_viewerInitialized) InitializeStatusViewer(hChildStatusEdit);
                std::wstring buf = statusLogger.GetBufferChronological();
                if (!buf.empty()) {
                    long curLen = (long)GetWindowTextLengthW(hChildStatusEdit);
                    if (g_lastMemInsertChars > 0) {
                        SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)(curLen - (long)g_lastMemInsertChars), (LPARAM)curLen);
                    } else {
                        SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)curLen, (LPARAM)curLen);
                    }
                    SendMessageW(hChildStatusEdit, EM_REPLACESEL, TRUE, (LPARAM)buf.c_str());
                    g_lastMemInsertChars = buf.size();
                    g_totalViewChars = (size_t)GetWindowTextLengthW(hChildStatusEdit);
                }
                
                // Then apply text color formatting (must be after SetWindowTextW)
                if (hColProp) {
                    COLORREF bgCol = (COLORREF)(UINT_PTR)hColProp;
                    COLORREF textCol = ChooseTextColorForBackground(bgCol);
                    CHARFORMAT2W cf = {};
                    cf.cbSize = sizeof(CHARFORMAT2W);
                    cf.dwMask = CFM_COLOR;
                    cf.crTextColor = textCol;
                    SendMessageW(hChildStatusEdit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
                }
                
                long endPos = (long)GetWindowTextLengthW(hChildStatusEdit);
                SendMessageW(hChildStatusEdit, EM_SETSEL, (WPARAM)endPos, (LPARAM)endPos);
                SendMessageW(hChildStatusEdit, EM_SCROLLCARET, 0, 0);
                ShowScrollBar(hChildStatusEdit, SB_VERT, TRUE);
            }
            // Each time we show, ensure child window is positioned at the right edge of the owner's monitor
            {
                const int margin = 8;
                HWND owner = (HWND)GetPropW(hChildStatusWnd, L"ChildStatusOwner");
                RECT ownerRect; ownerRect.left = ownerRect.top = ownerRect.right = ownerRect.bottom = 0;
                if (owner && IsWindow(owner)) GetWindowRect(owner, &ownerRect);

                HMONITOR hMon = MonitorFromRect(&ownerRect, MONITOR_DEFAULTTONULL);
                if (!hMon) hMon = MonitorFromWindow(hChildStatusWnd, MONITOR_DEFAULTTONEAREST);

                MONITORINFO mi; ZeroMemory(&mi, sizeof(mi)); mi.cbSize = sizeof(mi);
                if (!GetMonitorInfoW(hMon ? hMon : MonitorFromWindow(hChildStatusWnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    mi.rcWork.left = 0; mi.rcWork.top = 0;
                    mi.rcWork.right = GetSystemMetrics(SM_CXSCREEN);
                    mi.rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
                }

                int statusWidth = 800;
                int statusHeight = GetSystemMetrics(SM_CYSCREEN);
                int workW = mi.rcWork.right - mi.rcWork.left;
                int workH = mi.rcWork.bottom - mi.rcWork.top;
                if (statusWidth > workW - 2 * margin) statusWidth = workW - 2 * margin;
                if (statusHeight > workH - 2 * margin) statusHeight = workH - 2 * margin;

                int x = mi.rcWork.right - statusWidth - margin;
                int y = mi.rcWork.top + margin;
                SetWindowPos(hChildStatusWnd, HWND_TOPMOST, x, y, statusWidth, statusHeight, SWP_SHOWWINDOW);
            }
            ShowWindow(hChildStatusWnd, SW_SHOW);
        }
    }
}

void MainWindow::OnClearStatus()
{
    statusUserScrolled = false;
    statusLogEpoch_.fetch_add(1, std::memory_order_acq_rel);
    // Clear Status should always purge the persisted status.log so stale runs cannot
    // be reloaded into the viewer later. The checkbox only controls whether the
    // synthetic clear marker is written back after truncation.
    statusLogger.Clear(true, hChildStatusEdit, hChildStatusWnd, false, false);
    PrependStatus(L"Status cleared and log truncated\r\n");
}

// history-load feature removed: status pane now uses on-demand paging

void MainWindow::MarkStatusScrolled() {
    statusUserScrolled = true;
}
