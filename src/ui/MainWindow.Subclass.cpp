#include "MainWindow.h"
#include "../app/AppController.h"
#include "../include/core/CoreHelpers.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <cerrno>
#include <cctype>

LRESULT CALLBACK InplaceEditSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass; (void)dwRefData;
    (void)uIdSubclass;
    MainWindow* w = reinterpret_cast<MainWindow*>(dwRefData);
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
            // If the in-place edit was created for Motion list and is the Value subitem (3), force float-mode behavior
            bool motionFloatMode = (w->inplaceEditIsMotion == true && w->inplaceEditSubitem == 3);

            auto isSeparator = [&](wchar_t c){ return c == L',' || c == L';' || c == L':' || c == L'|'; };

            bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos);
            bool isBool = (du.find("BOOL") != std::string::npos) || (du.find("BOOLEAN") != std::string::npos);
            bool isUnsigned = (du.find("U") != std::string::npos && du.find("INT") != std::string::npos) || (du.find("BYTE") != std::string::npos) || (du.find("WORD") != std::string::npos) || (du.find("DWORD") != std::string::npos) || (du.find("LWORD") != std::string::npos) || (du.find("USINT") != std::string::npos) || (du.find("UINT") != std::string::npos) || (du.find("UDINT") != std::string::npos) || (du.find("ULINT") != std::string::npos);
            bool isSigned = (du.find("SINT") != std::string::npos) || (du.find("DINT") != std::string::npos) || (du.find("LINT") != std::string::npos) || ((du.find("INT") != std::string::npos) && !isUnsigned);

            if (ch < 0x20) break; // control chars allowed

            if (isFloat || motionFloatMode) {
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
            bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos) || (w->inplaceEditIsMotion == true && w->inplaceEditSubitem == 3);
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

// Subclass proc for Jog buttons to translate mouse down/up into start/stop
LRESULT CALLBACK JogButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass; (void)dwRefData;
    MainWindow* w = reinterpret_cast<MainWindow*>(dwRefData);
    if (!w) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        {
            // Determine which jog button this is and call direction-specific handlers
            if (w->IsJogCWHandle(hwnd)) {
                w->OnJogCWPressed();
            } else if (w->IsJogCCWHandle(hwnd)) {
                w->OnJogCCWPressed();
            } else {
                // Delegate to public UI wrapper which manages BUSY, timers and calls MotionController
                w->OnJogButtonPressed();
            }
            // Ensure we capture mouse so we receive the matching LBUTTONUP even
            // if the pointer leaves the button. We'll release on LBUTTONUP or
            // when WM_CAPTURECHANGED is received.
            SetCapture(hwnd);
        }
        return 0;
    case WM_MOUSEMOVE:
        {
            // Check if left button is still pressed and mouse has left button area
            if ((wParam & MK_LBUTTON) && GetCapture() == hwnd) {
                POINT pt;
                pt.x = GET_X_LPARAM(lParam);
                pt.y = GET_Y_LPARAM(lParam);
                RECT rc;
                GetClientRect(hwnd, &rc);
                // If mouse is outside button area, stop jog and release capture
                if (!PtInRect(&rc, pt)) {
                    if (w->IsJogCWHandle(hwnd)) {
                        w->OnJogCWReleased();
                    } else if (w->IsJogCCWHandle(hwnd)) {
                        w->OnJogCCWReleased();
                    } else {
                        w->OnJogButtonReleased();
                    }
                    ReleaseCapture();
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        {
            if (w->IsJogCWHandle(hwnd)) {
                w->OnJogCWReleased();
            } else if (w->IsJogCCWHandle(hwnd)) {
                w->OnJogCCWReleased();
            } else {
                // delegate to instance method which manages internal state and logs
                w->OnJogButtonReleased();
            }
            // Release capture explicitly to avoid leaving capture on this control
            // when returning to the application window.
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        {
            // If capture moved away from this button (for example user pressed
            // another window or a modal dialog grabbed capture), ensure jog is
            // stopped and internal state is cleared. OnJogButtonReleased is
            // idempotent and will safely ignore if no jog was active.
            w->OnJogButtonReleased();
            // Also attempt to release capture if somehow still owned
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass procedure for content pane: forward WM_COMMAND/WM_NOTIFY to the parent window
LRESULT CALLBACK ContentPaneSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    (void)uIdSubclass;
    // Retrieve parent (main window) from subclass data (dwRefData holds pointer to MainWindow)
    MainWindow* pThis = reinterpret_cast<MainWindow*>(dwRefData);
    if (!pThis) return DefSubclassProc(hwnd, uMsg, wParam, lParam);

    switch (uMsg) {
    case WM_COMMAND:
    case WM_NOTIFY:
        // Forward to main window procedure for centralized handling
        return SendMessageW(pThis->GetHwnd(), uMsg, wParam, lParam);
    // Forward control color messages so the main window can supply brushes
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        // Let the main window handle coloring and return its result
        return SendMessageW(pThis->GetHwnd(), uMsg, wParam, lParam);
    // Don't handle window move/size/hit test - let them go to parent (main window)
    case WM_NCHITTEST:
    case WM_SYSCOMMAND:
    case WM_SIZING:
    case WM_MOVING:
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    default:
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Subclass for SDO parameter ListView to detect mouse clicks on Value column
LRESULT CALLBACK SDOListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* w = reinterpret_cast<MainWindow*>(dwRefData);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    {
        // Compute hit-test at mouse point
        POINT pt; pt.x = (short)LOWORD(lParam); pt.y = (short)HIWORD(lParam);
    LVHITTESTINFO hti{};
        hti.pt = pt;
        int hit = ListView_SubItemHitTest(hwnd, &hti);
            if (hit >= 0) {
                if (w && hwnd == w->GetHListSlaves()) {
                    // Slave-list clicks are selection-only. Refresh happens in the
                    // connection lifecycle, so keep this path silent and skip SDO edit handling.
                } else {
                    // Only allow editing Value (5) or Hex (6) via mouse click (columns updated)
                    int editSub = hti.iSubItem;
                    if (editSub == 5 || editSub == 6) {
                        // Only trigger in-place editing for the SDO parameter ListView.
                        if (w && w->GetHwnd() && hwnd == w->GetHListSDOParams()) {
                            PostMessage(w->GetHwnd(), WM_USER_BEGIN_VALUE_EDIT, (WPARAM)hti.iItem, (LPARAM)editSub);
                        }
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

// Subclass for Motion parameter ListView: start in-place edit on any column click
LRESULT CALLBACK MotionListSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* w = reinterpret_cast<MainWindow*>(dwRefData);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN:
    {
        LVHITTESTINFO hti{};
        hti.pt.x = (short)LOWORD(lParam);
        hti.pt.y = (short)HIWORD(lParam);
        int hit = ListView_SubItemHitTest(hwnd, &hti);
        if (hit >= 0 && w && w->GetHwnd()) {
            int sub = hti.iSubItem;
            // If clicked the first column (index 0) do not start editing; allow default processing (selection)
            if (sub == 0) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
            if (sub < 0 || sub > 5) sub = 3; // default to Value column
            PostMessage(w->GetHwnd(), WM_USER_BEGIN_MOTION_VALUE_EDIT, (WPARAM)hti.iItem, (LPARAM)sub);
            // Swallow default processing to avoid selection highlight and double-processing when we start editing
            return 0;
        }
        // If not handled above, fall back to default
        return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    }
    case WM_LBUTTONDBLCLK:
        // Ignore double-click to disable editing by double-click
        return 0;
            // Ignore WM_NOTIFY for motion list to avoid duplicate handling; subclass manages single-click editing
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, MotionListSubclassProc, uIdSubclass);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Public: 由 ListView 子類別在滾動後呼叫，保持內嵌編輯與 Value 欄對齊，或在不可視時提交關閉


LRESULT CALLBACK FaultResetButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                              UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* wnd = reinterpret_cast<MainWindow*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    (void)uIdSubclass;

    switch (uMsg) {
    case WM_LBUTTONDOWN:
        AppController::GetInstance().FaultReset(true);
        break;
    case WM_LBUTTONUP:
        AppController::GetInstance().FaultReset(false);
        break;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) AppController::GetInstance().FaultReset(true);
        break;
    case WM_KEYUP:
        if (wParam == VK_SPACE || wParam == VK_RETURN) AppController::GetInstance().FaultReset(false);
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Instance method: initiate fault reset (async)
// moved to MainWindow.Motion.cpp: MainWindow::OnFaultReset()

// Async implementations moved to MotionController; stubs removed to reduce unused API surface

// =====================================================================
// Immediate Stop (Quick Stop) button subclass and handler
// =====================================================================

// UI-only subclass procedure for Immediate Stop
LRESULT CALLBACK ImmediateStopButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                                 UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* wnd = reinterpret_cast<MainWindow*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN: wnd->OnImmediateStop(true); break;
    case WM_LBUTTONUP:   wnd->OnImmediateStop(false); break;
    case WM_KEYDOWN:     if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnImmediateStop(true); break;
    case WM_KEYUP:       if (wParam == VK_SPACE || wParam == VK_RETURN) wnd->OnImmediateStop(false); break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// moved to MainWindow.Motion.cpp: MainWindow::OnImmediateStop()

// OnImmediateStopAsync stub removed (logic moved to MotionController::ImmediateStopAsync)

// =====================================================================
// Power Off button subclass and handler
// =====================================================================

// Subclass procedure to capture press on the Power Off button
LRESULT CALLBACK PowerOffButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* wnd = reinterpret_cast<MainWindow*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN: AppController::GetInstance().PowerOff(true); break;
    case WM_KEYDOWN:     if (wParam == VK_SPACE || wParam == VK_RETURN) AppController::GetInstance().PowerOff(true); break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// moved to MainWindow.Motion.cpp: MainWindow::OnPowerOff()

// OnPowerOffAsync stub removed (logic moved to MotionController::PowerOffAsync)

// =====================================================================
// Power On button subclass and handler
// =====================================================================

// Subclass procedure to capture press on the Power On button
LRESULT CALLBACK PowerOnButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* wnd = reinterpret_cast<MainWindow*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN: AppController::GetInstance().PowerOn(true); break;
    case WM_KEYDOWN:     if (wParam == VK_SPACE || wParam == VK_RETURN) AppController::GetInstance().PowerOn(true); break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// moved to MainWindow.Motion.cpp: MainWindow::OnPowerOn()

// Subclass procedure to capture press on the Home button (UI-only)
LRESULT CALLBACK HomeButtonSubclassProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    MainWindow* wnd = reinterpret_cast<MainWindow*>(dwRefData);
    if (!wnd) return DefSubclassProc(hwnd, uMsg, wParam, lParam);
    (void)uIdSubclass;
    switch (uMsg) {
    case WM_LBUTTONDOWN:
        {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            SYSTEMTIME st; GetLocalTime(&st);
            char tbuf[64]; sprintf_s(tbuf, sizeof(tbuf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            MainWindow::PostUTF8LogToHWND(top, std::string(tbuf) + std::string("Origin setting button pressed\n"));
        }
        AppController::GetInstance().Home(true);
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            HWND top = GetAncestor(hwnd, GA_ROOT);
            SYSTEMTIME st2; GetLocalTime(&st2);
            char tbuf2[64]; sprintf_s(tbuf2, sizeof(tbuf2), "[%02d:%02d:%02d.%03d] ", st2.wHour, st2.wMinute, st2.wSecond, st2.wMilliseconds);
            MainWindow::PostUTF8LogToHWND(top, std::string(tbuf2) + std::string("Origin setting key invoked\n"));
            AppController::GetInstance().Home(true);
            return 0;
        }
        break;
    }
    return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

// Origin setting button handler
// moved to MainWindow.Motion.cpp: MainWindow::OnHome()

// OnHomeAsync and OnPowerOnAsync stubs removed (logic moved to MotionController)
