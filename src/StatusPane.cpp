// StatusPane.cpp
// Moved out from Win32Window.cpp to separate compilation unit.

#include "Win32Window.h"
#include <windows.h>

// Child/status window proc (modeless)
static LRESULT CALLBACK ChildStatusWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CLOSE:
        // Hide instead of destroy to preserve content
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_SIZE:
        {
            // Resize child edit to fill
            HWND hEdit = FindWindowEx(hwnd, NULL, L"Edit", NULL);
            if (hEdit) {
                RECT rc; GetClientRect(hwnd, &rc);
                SetWindowPos(hEdit, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER);
            }
        }
        break;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

void Win32Window::ToggleChildStatusWindow()
{
    if (!hChildStatusWnd || !IsWindow(hChildStatusWnd)) {
        // Register class if necessary
        WNDCLASSEX wc = {0};
        wc.cbSize = sizeof(WNDCLASSEX);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ChildStatusWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ChildStatusWindowClass";
        RegisterClassEx(&wc);

        // Create modeless window owned by main hwnd
        hChildStatusWnd = CreateWindowEx(WS_EX_TOOLWINDOW, L"ChildStatusWindowClass", L"狀態視窗", WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 600, 300, hwnd, NULL, hInstance, NULL);
        if (!hChildStatusWnd) return;
        // Create read-only multiline edit with vertical scrollbar
        hChildStatusEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 600, 300, hChildStatusWnd, NULL, hInstance, NULL);
        SendMessageW(hChildStatusEdit, WM_SETFONT, (WPARAM)hGuiFont, MAKELPARAM(TRUE,0));
        // Fill edit with existing buffer (newest-first) so it's up-to-date when opened
        if (hChildStatusEdit && IsWindow(hChildStatusEdit)) {
            SetWindowTextW(hChildStatusEdit, statusLogger.GetBuffer().c_str());
            SendMessageW(hChildStatusEdit, EM_SETSEL, 0, 0);
            ShowScrollBar(hChildStatusEdit, SB_VERT, TRUE);
        }
        ShowWindow(hChildStatusWnd, SW_SHOW);
    } else {
        if (IsWindowVisible(hChildStatusWnd)) {
            ShowWindow(hChildStatusWnd, SW_HIDE);
        } else {
            // update content before showing
            if (hChildStatusEdit && IsWindow(hChildStatusEdit)) {
                SetWindowTextW(hChildStatusEdit, statusLogger.GetBuffer().c_str());
                SendMessageW(hChildStatusEdit, EM_SETSEL, 0, 0);
                ShowScrollBar(hChildStatusEdit, SB_VERT, TRUE);
            }
            ShowWindow(hChildStatusWnd, SW_SHOW);
        }
    }
}

void Win32Window::OnClearStatus()
{
    // Use statusLogger to clear in-memory buffer and truncate file, and
    // sync child status edit window if present.
    statusUserScrolled = false;
    statusLogger.Clear(true, hChildStatusEdit, hChildStatusWnd);
}

void Win32Window::MarkStatusScrolled() {
    statusUserScrolled = true;
}
