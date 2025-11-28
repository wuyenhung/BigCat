// StatusLogger: manages in-memory status buffer and on-disk status.log
#pragma once
#include <windows.h>
#include "UIConstants.h"
#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

class StatusLogger {
public:
    StatusLogger();
    ~StatusLogger();

    // Prepend text to the in-memory buffer and optionally append to on-disk log.
    // If hChildEdit/hChildWnd are provided and visible, the child edit control
    // will be synchronized with the new buffer (newest-first) like previous behavior.
    void Prepend(const std::wstring &text, HWND hChildEdit = NULL, HWND hChildWnd = NULL, bool writeToFile = true, bool userScrolled = false);

    // UI-only prepend: same as Prepend but does not write to disk
    void PrependNoFile(const std::wstring &text, HWND hChildEdit = NULL, HWND hChildWnd = NULL, bool userScrolled = false);

    // Clear in-memory buffer and optionally truncate on-disk log.
    // writeClearNoticeToFile controls whether the synthetic "Status cleared..."
    // entry is appended back to disk after truncation.
    // emitClearNotice controls whether the UI/in-memory buffer receives that line.
    void Clear(bool truncateFile = true,
               HWND hChildEdit = NULL,
               HWND hChildWnd = NULL,
               bool writeClearNoticeToFile = true,
               bool emitClearNotice = true);

    // Return a copy of the current buffer (thread-safe)
    std::wstring GetBuffer();
    // Return a chronological copy of the current buffer (oldest-first).
    std::wstring GetBufferChronological();

    // WM_USER_STATUS_UPDATE_BATCH is defined in UIConstants.h.

private:
    // In-memory UI buffer (newest-first)
    std::wstring buffer;
    std::mutex mtx;
    // Flag indicating a batched UI update message is pending (posted to UI)
    std::atomic<bool> uiUpdatePending{false};

    // Deduplication helpers: keep track of the last emitted (content-only)
    // line and its timestamp to suppress exact duplicates that occur
    // within the same millisecond. When duplicates are suppressed we
    // increment lastAddedRepeatCount and update the top-of-buffer text
    // to show the repeat count (e.g., "(x3)"). This helps avoid noisy
    // duplicate logs when multiple subsystems post the same message.
    // lastAddedContent stores the last line content (without timestamp) to compare
    // These fields are protected by a dedicated mutex so they can be queried
    // and updated without holding the main 'mtx' that guards the text buffer.
    std::wstring lastAddedContent;   // protected by lastAddedMtx
    // lastAddedTimeMs now stores absolute epoch milliseconds (system_clock)
    // to allow deduplication across a short time window (DEDUPE_WINDOW_MS)
    uint64_t lastAddedTimeMs = 0;    // protected by lastAddedMtx (epoch ms)
    int lastAddedRepeatCount = 0;    // protected by lastAddedMtx
    std::mutex lastAddedMtx;
    static constexpr uint64_t DEDUPE_WINDOW_MS = 1000; // suppress duplicates within 1s

    // Buffered file appends handled by a background writer thread to avoid
    // performing disk I/O on the UI or real-time threads.
    std::string fileBuffer;                // UTF-8 data pending to be flushed
    uint64_t fileBufferEpoch = 1;
    std::mutex fileMtx;
    std::condition_variable fileCv;
    std::thread writerThread;
    std::atomic<bool> writerRunning{false};
    std::atomic<uint64_t> fileEpoch{1};

    // Writer thresholds / intervals
    static constexpr size_t FLUSH_THRESHOLD = 8 * 1024; // flush when >= 8KB
    static constexpr int FLUSH_INTERVAL_MS = 200;      // flush at least every 200 ms
    static constexpr size_t ROTATE_SIZE = 10 * 1024 * 1024; // rotate after 10MB (simple truncate)
    // In-memory buffer retention policy: keep only recent bytes to bound memory.
    // Buffer semantics are newest-first; older content beyond this will be dropped
    // from memory but remain on-disk in rotated files.
    static constexpr size_t INMEM_MAX_BYTES = 512 * 1024; // keep ~512KB in memory

public:
    // Read a byte range from the single status.log file under the logs directory.
    // Returns raw UTF-8 bytes read (may be less than len if EOF reached).
    std::string ReadStatusFileRangeUtf8(size_t offset, size_t len);

    // helper: perform immediate append of utf8 to disk (called by writer thread)
    void FlushToDisk(const std::string &utf8);
    void TruncateFile();

    // writer thread loop
    void WriterLoop();
    // helper: ensure writer thread is running
    void EnsureWriterRunning();
    // Mark that a pending UI batch update has been processed by the status window.
    // This clears the internal pending flag so future Prepend() calls will post
    // another batched UI update. Intended to be called from the UI thread.
    void MarkUiUpdateProcessed();
};
