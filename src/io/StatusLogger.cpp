#include "StatusLogger.h"
#include <windows.h>
#include <fstream>
#include <chrono>
// Use shared utilities for UTF-8 conversion
#include "Win32Utils.h"
#include "../include/core/CoreHelpers.h"
// Allow writing the in-memory status buffer into the HTTP server cache
#include "HttpServer.h"

namespace {
std::wstring ReverseLineOrder(const std::wstring& text) {
    if (text.empty()) return std::wstring();

    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        std::wstring line;
        if (nl == std::wstring::npos) {
            line = text.substr(pos);
            pos = text.size();
        } else {
            line = text.substr(pos, nl - pos + 1);
            pos = nl + 1;
        }
        lines.push_back(std::move(line));
    }

    std::wstring out;
    out.reserve(text.size());
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        out += *it;
    }
    return out;
}
} // namespace

StatusLogger::StatusLogger() {
    writerRunning.store(true);
    writerThread = std::thread(&StatusLogger::WriterLoop, this);
}

void StatusLogger::MarkUiUpdateProcessed() {
    uiUpdatePending.store(false);
}

StatusLogger::~StatusLogger() {
    // Stop writer thread and flush remaining data
    writerRunning.store(false);
    fileCv.notify_all();
    if (writerThread.joinable()) writerThread.join();
    // flush any remaining data synchronously
    std::string remaining;
    {
        std::lock_guard<std::mutex> lk(fileMtx);
        remaining.swap(fileBuffer);
    }
    if (!remaining.empty()) FlushToDisk(remaining);
}

// Helper: path resolution for status.log under <exe-dir>\logs\status.log
static std::wstring ResolveStatusLogPath() {
    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring logDir = dir + L"\\logs";
    CreateDirectoryW(logDir.c_str(), NULL);
    std::wstring logPath = logDir + L"\\status.log";
    return logPath;
}

// Resolve logs directory path (ensure exists) and return it (without trailing slash)
static std::wstring ResolveStatusLogDir() {
    std::wstring logPath = ResolveStatusLogPath();
    size_t pos = logPath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return logPath;
    return logPath.substr(0, pos);
}

void StatusLogger::FlushToDisk(const std::string &utf8) {
    if (utf8.empty()) return;
    std::wstring logPath = ResolveStatusLogPath();
    FILE* f = NULL;
    errno_t er = _wfopen_s(&f, logPath.c_str(), L"ab");
    if (er == 0 && f) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    }
}

void StatusLogger::TruncateFile() {
    std::wstring logPath = ResolveStatusLogPath();
    // Truncate the single status.log file to zero-length. We intentionally
    // keep only one log file (no rotation) per user request.
    FILE* f = NULL;
    errno_t er = _wfopen_s(&f, logPath.c_str(), L"wb");
    if (er == 0 && f) fclose(f);
}

void StatusLogger::WriterLoop() {
    using namespace std::chrono_literals;
    while (writerRunning.load()) {
        std::unique_lock<std::mutex> lk(fileMtx);
        if (fileCv.wait_for(lk, std::chrono::milliseconds(FLUSH_INTERVAL_MS), [this]{ return !fileBuffer.empty() || !writerRunning.load(); })) {
            // woke because fileBuffer has data or writerStopping
        }
        // move buffer out
        std::string toWrite;
        const uint64_t batchEpoch = fileBufferEpoch;
        toWrite.swap(fileBuffer);
        lk.unlock();

        if (!toWrite.empty()) {
            if (batchEpoch == fileEpoch.load(std::memory_order_acquire)) {
                FlushToDisk(toWrite);
            }
            // simple rotation: if file exceeds ROTATE_SIZE, truncate
            // We check file size occasionally (cost low compared to frequent writes)
            std::wstring logPath = ResolveStatusLogPath();
            // No automatic rotation: keep single status.log file. Any truncation
            // is performed explicitly by Clear(true).
        }
    }
}

std::string StatusLogger::ReadStatusFileRangeUtf8(size_t offset, size_t len) {
    std::wstring path = ResolveStatusLogPath();
    FILE* f = nullptr;
    errno_t er = _wfopen_s(&f, path.c_str(), L"rb");
    if (er != 0 || !f) return std::string();
    // Seek to offset (offset is bytes)
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return std::string(); }
    long filesize = ftell(f);
    if ((long)offset >= filesize) { fclose(f); return std::string(); }
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return std::string(); }
    size_t toRead = len;
    if ((long)offset + (long)len > filesize) toRead = (size_t)(filesize - (long)offset);
    std::string buf; buf.resize(toRead);
    size_t r = fread(&buf[0], 1, toRead, f);
    fclose(f);
    if (r < toRead) buf.resize(r);
    return buf;
}

void StatusLogger::Prepend(const std::wstring &text, HWND hChildEdit, HWND hChildWnd, bool writeToFile, bool userScrolled) {
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

    // If trimmed contains multiple lines, split and prefix each line with a timestamp
    // We also perform deduplication: if a new line's content (excluding timestamp)
    // exactly matches the previous added line and the timestamps are equal
    // (within the same millisecond), we suppress writing the duplicate to the
    // file and increment a repeat counter which is shown as "(xN)" on the
    // top-most in-memory line.
    std::wstring newText;
    size_t pos = 0;
    while (pos < trimmed.size()) {
        size_t nl = trimmed.find(L'\n', pos);
        std::wstring line;
        if (nl == std::wstring::npos) {
            line = trimmed.substr(pos);
            pos = trimmed.size();
        } else {
            line = trimmed.substr(pos, nl - pos);
            pos = nl + 1; // skip '\n'
            // If previous char was '\r', remove it from line end
            if (!line.empty() && line.back() == L'\r') line.pop_back();
        }
        // If the line already begins with a timestamp like [HH:MM:SS.mmm], preserve it
        bool hasTs = false;
        int hh=0, mm=0, ss=0, ms=0;
        if (line.size() >= 13 && line[0] == L'[') {
            if (swscanf_s(line.c_str(), L"[%d:%d:%d.%d]", &hh, &mm, &ss, &ms) == 4) {
                if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60 && ss >= 0 && ss < 60 && ms >= 0 && ms < 1000) hasTs = true;
            }
        }
        SYSTEMTIME st;
        // Use epoch ms for dedupe (see later)
        std::wstring contentOnly; // raw message content (without timestamp)
        std::wstring tsLine; // full timestamped line to be appended
        if (hasTs) {
            // preserve existing timestamp in the line; but we need raw content
            // so skip the parsed timestamp prefix if present
            // Find first space after ']'
            size_t br = line.find(L']');
            if (br != std::wstring::npos && br + 2 < line.size()) contentOnly = line.substr(br + 2); else contentOnly = line;
            tsLine = line + L"\r\n";
            // We ignore the parsed timestamp for dedupe; dedupe uses epoch ms recorded at time of insertion
        } else {
            GetLocalTime(&st);
            wchar_t timebuf[64];
            swprintf_s(timebuf, L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            contentOnly = line; // raw content (no timestamp yet)
            tsLine = std::wstring(timebuf) + std::wstring(L"[added] ") + line + L"\r\n";
            // Do not use parsed timestamp for dedupe; later we compute epoch ms for dedupe
        }

        // Deduplication check
        bool isDuplicate = false;
        {
            using namespace std::chrono;
            uint64_t nowMs = (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            std::lock_guard<std::mutex> lk(lastAddedMtx);
            if (!lastAddedContent.empty() && contentOnly == lastAddedContent) {
                // Treat as duplicate if the previous identical message was added within a short window
                if (nowMs - lastAddedTimeMs <= DEDUPE_WINDOW_MS) {
                    isDuplicate = true;
                    ++lastAddedRepeatCount;
                    // update lastAddedTimeMs so repeated calls within window keep refreshing timestamp
                    lastAddedTimeMs = nowMs;
                }
            }
        }

            if (isDuplicate) {
            // Update the in-memory buffer's top line to show the repeat count
            std::lock_guard<std::mutex> lk(mtx);
            // Find the first newline (top-most line) and replace the line suffix with (xN)
            size_t nlPos = buffer.find(L'\n');
            if (nlPos != std::wstring::npos) {
                std::wstring topLine = buffer.substr(0, nlPos + 1); // includes CRLF
                // Remove trailing CRLF then append (xN) then CRLF
                if (topLine.size() >= 2 && topLine.substr(topLine.size()-2) == L"\r\n") topLine = topLine.substr(0, topLine.size()-2);
                // strip existing (xN) if present
                size_t paren = topLine.find_last_of(L'(');
                if (paren != std::wstring::npos) {
                    // ensure it looks like "(xNN)"
                    size_t close = topLine.find_last_of(L')');
                    if (close != std::wstring::npos && close > paren) topLine = topLine.substr(0, paren-1);
                }
                wchar_t repbuf[32]; swprintf_s(repbuf, L" (x%d)", lastAddedRepeatCount);
                std::wstring newTop = topLine + std::wstring(repbuf) + std::wstring(L"\r\n");
                // Replace only the top line
                buffer = newTop + buffer.substr(nlPos + 1);
            }
            // If UI is visible, post a batch update so repeat count is refreshed
            if (hChildEdit && IsWindow(hChildEdit) && hChildWnd && IsWindowVisible(hChildWnd) && !IsIconic(hChildWnd)) {
                bool expected = false; if (uiUpdatePending.compare_exchange_strong(expected, true)) PostMessageW(hChildWnd, WM_USER_STATUS_UPDATE_BATCH, 0, 0);
            }
            // Don't append duplicate to fileBuffer to avoid duplicate file lines
            continue;
        }

        // New (non-duplicate) line: record as lastAdded and append as usual
        {
            using namespace std::chrono;
            uint64_t nowMs = (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
            std::lock_guard<std::mutex> lk(lastAddedMtx);
            lastAddedContent = contentOnly;
            lastAddedTimeMs = nowMs;
            lastAddedRepeatCount = 1;
        }

        // At this point we have tsLine already constructed; append that
        newText += tsLine;
    }

    {
        std::lock_guard<std::mutex> lk(mtx);
        buffer = newText + buffer; // newest-first
        // Trim in-memory buffer to keep memory bounded (cut older content)
        if (buffer.size() > INMEM_MAX_BYTES) {
            // Attempt to keep whole lines: find the last '\n' within allowed range
            size_t keep = INMEM_MAX_BYTES;
            // If keep falls in middle of multi-byte character we still cut by wchar units
            size_t pos = buffer.find_last_of(L'\n', keep);
            if (pos == std::wstring::npos) {
                // No newline found within limit — just truncate at keep
                buffer = buffer.substr(0, keep);
            } else {
                // Keep through that newline (inclusive)
                buffer = buffer.substr(0, pos + 1);
            }
        }
    }

    // Instead of synchronously sending EM_* messages every time we get a new
    // line (which can cause heavy UI thrash), post a single batched update
    // message to the status window and throttle further posts until the
    // UI processes the batch. The UI handler will call MarkUiUpdateProcessed()
    // when done so more updates may be posted.
    if (hChildEdit && IsWindow(hChildEdit) && hChildWnd && IsWindowVisible(hChildWnd) && !IsIconic(hChildWnd)) {
        // Ensure the vertical scrollbar is visible
        ShowScrollBar(hChildEdit, SB_VERT, TRUE);
        // Post a single batched update if one is not already pending.
        // We keep the pending flag as a member variable (uiUpdatePending),
        // but since this cpp doesn't currently declare that member in this
        // translation unit, we'll use the public MarkUiUpdateProcessed()/GetBuffer
        // API to coordinate with the UI. Here we'll use PostMessage unconditionally
        // but protect flooding by relying on an atomic flag 'uiUpdatePending'
        // implemented in the header/cpp pair (see updated header).
        // Post the batched update only if one is not already pending. The
        // UI handler will call MarkUiUpdateProcessed() when it finishes
        // processing the batch so we can post again.
        bool expected = false;
        if (uiUpdatePending.compare_exchange_strong(expected, true)) {
            PostMessageW(hChildWnd, WM_USER_STATUS_UPDATE_BATCH, 0, 0);
        }
    }

    // Enqueue for background file write
        if (writeToFile) {
        std::string u8 = CoreHelpers::WideToUtf8(newText);
        if (!u8.empty()) {
            {
                std::lock_guard<std::mutex> lk(fileMtx);
                fileBufferEpoch = fileEpoch.load(std::memory_order_acquire);
                fileBuffer += u8;
            }
            // If buffer size exceeds threshold, notify writer to flush earlier
            size_t bufSize = 0; { std::lock_guard<std::mutex> lk(fileMtx); bufSize = fileBuffer.size(); }
            if (bufSize >= FLUSH_THRESHOLD) fileCv.notify_one();
        }
    }

    // Mirror the in-memory status buffer (UTF-8) to the HTTP server cache
    try {
        std::wstring cur = GetBufferChronological();
        std::string curU8 = CoreHelpers::WideToUtf8(cur);
        if (!curU8.empty()) SetLastStatusUtf8(curU8);
    } catch (...) {
        // ignore failures
    }
}

void StatusLogger::PrependNoFile(const std::wstring &text, HWND hChildEdit, HWND hChildWnd, bool userScrolled) {
    Prepend(text, hChildEdit, hChildWnd, false, userScrolled);
}

void StatusLogger::Clear(bool truncateFile, HWND hChildEdit, HWND hChildWnd, bool writeClearNoticeToFile, bool emitClearNotice) {
    {
        std::lock_guard<std::mutex> lk(mtx);
        buffer.clear();
    }
    {
        std::lock_guard<std::mutex> lk(fileMtx);
        if (truncateFile) {
            fileEpoch.fetch_add(1, std::memory_order_acq_rel);
            fileBuffer.clear();
            fileBufferEpoch = fileEpoch.load(std::memory_order_acquire);
        }
    }
    if (hChildEdit && IsWindow(hChildEdit)) {
        SetWindowTextW(hChildEdit, L"");
        ShowScrollBar(hChildEdit, SB_VERT, TRUE);
    }
    if (truncateFile) {
        // Ask UI viewer to reset (clear any loaded pages) before truncating
        if (hChildWnd && IsWindow(hChildWnd)) PostMessageW(hChildWnd, WM_USER + 301, 0, 0);
        TruncateFile();
    }
    if (emitClearNotice) {
        // Only write the "Status cleared..." line back to disk when explicitly requested.
        // This lets the Clear button always purge stale persisted history even when the
        // runtime status.log checkbox is disabled.
        Prepend(L"Status cleared and log truncated\r\n", hChildEdit, hChildWnd, truncateFile && writeClearNoticeToFile, false);
    }
    // Reset dedupe state
    {
        std::lock_guard<std::mutex> lk(lastAddedMtx);
        lastAddedContent.clear(); lastAddedTimeMs = 0; lastAddedRepeatCount = 0;
    }
}

std::wstring StatusLogger::GetBuffer() {
    std::lock_guard<std::mutex> lk(mtx);
    return buffer;
}

std::wstring StatusLogger::GetBufferChronological() {
    std::lock_guard<std::mutex> lk(mtx);
    return ReverseLineOrder(buffer);
}
