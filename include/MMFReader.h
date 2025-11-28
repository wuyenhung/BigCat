#pragma once

#include <windows.h>
#include <array>
#include <string>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

// MMFReader: background reader that polls a named Memory-Mapped File every ~1ms
// and decodes 6 floats from a specified offset. Thread-safe snapshot retrieval.
class MMFReader {
public:
    MMFReader();
    ~MMFReader();

    // Start background reading. mappingName: e.g. L"Local\\Wu6DoF_MMF"
    // offsetBytes: start offset where 6 floats are laid out consecutively
    // Configure and enable the reader. This no longer spawns a background
    // thread — the caller (e.g. EtherCAT cyclic thread) must call PollOnce()
    // periodically to perform reads.
    bool Start(const std::wstring &mappingName, size_t offsetBytes = 0);
    void Stop();

    // Perform one read/update cycle. Intended to be called from an external
    // periodic thread (e.g. EtherCAT master cyclic thread). Returns true if
    // a mapping existed and a read was attempted.
    bool PollOnce();

    // Return latest snapshot of 6 floats (thread-safe copy)
    std::array<float, 6> GetLatest() const;
    void GetLatestWithUpdateCounts(std::array<float, 6>& outValues,
                                   std::array<uint64_t, 6>& outUpdateCounts) const;
    void GetLatestWithUpdateCountsAndVelocities(std::array<float, 6>& outValues,
                                                std::array<uint64_t, 6>& outUpdateCounts,
                                                std::array<double, 6>& outVelocities) const;
    std::array<double, 6> GetLatestVelocities() const;

    // Returns true if thread is running
    bool IsRunning() const { return running.load(); }
    // Returns true if we have successfully mapped the MMF at least once
    bool IsMapped() const { return everMapped.load(); }
    // Attempt to synchronously map the MMF once using the configured mapping name
    // Returns true if mapping succeeded.
    bool TryMapNow();
    // If mapped, copy mappedSize bytes starting at baseOffset into out (returns true)
    bool GetRawSnapshot(std::vector<uint8_t> &out) const;

private:
    // legacy ThreadLoop removed — polling is now driven externally
    bool EnsureMapped();
    void Unmap();

    std::wstring mapName;
    size_t baseOffset = 0;
    HANDLE hMap = NULL;
    const uint8_t* view = nullptr;
    // number of bytes we expect to map/read (default 6 floats)
    size_t mappedSize = sizeof(float) * 6;

    // No internal worker thread anymore. The reader exposes PollOnce().
    // running flag semantic preserved for compatibility but reflects whether
    // Start() has been called (enabled), not an internal thread.
    std::atomic<bool> running{false};
    // Count how many successful reads the background thread performed
    std::atomic<uint64_t> readCount{0};
    // Count how many reads produced non-zero payloads (meaningful values)
    std::atomic<uint64_t> nonZeroReadCount{0};
    // Sequence lock for lock-free snapshot publishing (even = stable, odd = writing)
    mutable std::atomic<uint64_t> seq{0};

    // previous design used mutex; keep latest but we'll publish via seq
    mutable std::array<float, 6> latest{ {0,0,0,0,0,0} };
    std::atomic<bool> everMapped{false};
    // Count how many times the snapshot changed compared to previous (producer updated)
    std::atomic<uint64_t> updateCount{0};
    // store previous snapshot to detect changes (updated by worker)
    mutable std::array<float, 6> prevSnapshot{ {0,0,0,0,0,0} };
    std::array<uint64_t, 6> channelUpdateCounts{ {0,0,0,0,0,0} };
    std::array<uint64_t, 6> channelUpdateTicks{ {0,0,0,0,0,0} };
    std::array<double, 6> latestVelocity{ {0,0,0,0,0,0} };
    // Optional: named event used by producer to signal updates (consumer will try OpenEvent)
    std::wstring eventName;
    std::atomic<bool> hasEvent{false};
    // Backoff for mapping attempts when the MMF is not yet available
    std::atomic<ULONGLONG> lastMapFailTick{0};
    uint32_t mapRetryIntervalMs = 200;
    // MMF update frequency tracking (Hz)
    std::atomic<double> lastUpdateHz{0.0};
    uint64_t lastPollTicks{0};
    double perfFreqTicks{0.0}; // QueryPerformanceFrequency result
public:
    // Return and reset the internal read counter (reads since last call)
    uint64_t GetAndResetReadCount();
    uint64_t GetAndResetNonZeroReadCount();
    uint64_t GetAndResetUpdateCount();
    // Return last computed MMF update frequency in Hz
    double GetLastUpdateFrequencyHz() const { return lastUpdateHz.load(std::memory_order_relaxed); }
    // If consumer successfully opened a named Event for the mapping, this returns true
    bool HasEvent() const { return hasEvent.load(std::memory_order_acquire); }
    // Get the event name being used (may be empty)
    const std::wstring &GetEventName() const { return eventName; }
};
