// StatusLogger: manages in-memory status buffer and on-disk status.log
#pragma once
#include <windows.h>
#include <string>
#include <mutex>

class StatusLogger {
public:
    StatusLogger();
    ~StatusLogger();

    // Prepend text to the in-memory buffer and optionally append to on-disk log.
    // If hChildEdit/hChildWnd are provided and visible, the child edit control
    // will be synchronized with the new buffer (newest-first) like previous behavior.
    void Prepend(const std::wstring &text, HWND hChildEdit = NULL, HWND hChildWnd = NULL, bool writeToFile = true);

    // UI-only prepend: same as Prepend but does not write to disk
    void PrependNoFile(const std::wstring &text, HWND hChildEdit = NULL, HWND hChildWnd = NULL);

    // Clear in-memory buffer and optionally truncate on-disk log
    void Clear(bool truncateFile = true, HWND hChildEdit = NULL, HWND hChildWnd = NULL);

    // Return a copy of the current buffer (thread-safe)
    std::wstring GetBuffer();

private:
    std::wstring buffer;
    std::mutex mtx;
    // helper: append UTF-8 to file (append/ truncate as needed)
    void AppendToFile(const std::string &utf8);
    void TruncateFile();
};
