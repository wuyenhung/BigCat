#include "StatusLogger.h"
#include <windows.h>
#include <fstream>
#include <vector>
// Use shared utilities for UTF-8 conversion
#include "Win32Utils.h"

StatusLogger::StatusLogger() {}
StatusLogger::~StatusLogger() {}

void StatusLogger::AppendToFile(const std::string &utf8) {
    // Determine log path under executable directory: <exe_dir>\logs\status.log
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring logDir = dir + L"\\logs";
    CreateDirectoryW(logDir.c_str(), NULL);
    std::wstring logPath = logDir + L"\\status.log";

    FILE* f = NULL;
    errno_t er = _wfopen_s(&f, logPath.c_str(), L"ab");
    if (er == 0 && f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

void StatusLogger::TruncateFile() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring logDir = dir + L"\\logs";
    std::wstring logPath = logDir + L"\\status.log";
    FILE* f = NULL;
    errno_t er = _wfopen_s(&f, logPath.c_str(), L"wb");
    if (er == 0 && f) fclose(f);
}

void StatusLogger::Prepend(const std::wstring &text, HWND hChildEdit, HWND hChildWnd, bool writeToFile) {
    if (text.empty()) return;
    // Trim and normalize same as previous logic
    auto is_space = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f';
    };
    size_t first = 0;
    while (first < text.size() && is_space(text[first])) ++first;
    size_t last = text.size();
    while (last > first && is_space(text[last - 1])) --last;
    if (first == last) return;
    std::wstring trimmed = text.substr(first, last - first);
    if (trimmed.size() >= 2 && trimmed.substr(trimmed.size() - 2) == L"\r\n") {
    } else if (!trimmed.empty() && trimmed.back() == L'\n') {
        trimmed += L'\r';
    } else {
        trimmed += L"\r\n";
    }

    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t timebuf[64];
    swprintf_s(timebuf, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    std::wstring newText = std::wstring(timebuf) + trimmed;

    {
        std::lock_guard<std::mutex> lk(mtx);
        buffer = newText + buffer; // newest-first
    }

    // Sync to child if requested and visible
    if (hChildEdit && IsWindow(hChildEdit) && hChildWnd && IsWindowVisible(hChildWnd)) {
        SetWindowTextW(hChildEdit, GetBuffer().c_str());
        SendMessageW(hChildEdit, EM_SETSEL, 0, 0);
        ShowScrollBar(hChildEdit, SB_VERT, TRUE);
    }

    if (writeToFile) {
        std::string u8 = WideToUtf8(newText);
        if (!u8.empty()) AppendToFile(u8);
    }
}

void StatusLogger::PrependNoFile(const std::wstring &text, HWND hChildEdit, HWND hChildWnd) {
    Prepend(text, hChildEdit, hChildWnd, false);
}

void StatusLogger::Clear(bool truncateFile, HWND hChildEdit, HWND hChildWnd) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        buffer.clear();
    }
    if (hChildEdit && IsWindow(hChildEdit)) {
        SetWindowTextW(hChildEdit, L"\r\n");
        ShowScrollBar(hChildEdit, SB_VERT, TRUE);
    }
    if (truncateFile) TruncateFile();
    Prepend(L"Status cleared and log truncated\r\n", hChildEdit, hChildWnd, true);
}

std::wstring StatusLogger::GetBuffer() {
    std::lock_guard<std::mutex> lk(mtx);
    return buffer;
}
