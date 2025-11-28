// MMFReader.cpp - 簡要說明:
// 讀取共享記憶體映射 (MMF) 的資料並提供輪詢式取得最新數值。

#include "MMFReader.h"
#include <windows.h>
#include <vector>
#include "SafeMem.h"

namespace {
constexpr double kMaxMmfVelocityDeltaSec = 0.5;
constexpr double kMmfVelocityHoldTimeoutSec = 0.05;
}

MMFReader::MMFReader() {}

MMFReader::~MMFReader() {
    Stop();
}

bool MMFReader::Start(const std::wstring &mappingName, size_t offsetBytes) {
    // Configure mapping and enable polling; do not start any internal thread.
    Stop();
    mapName = mappingName;
    baseOffset = offsetBytes;
    // Initialize perf counter frequency if not already done
    LARGE_INTEGER freq;
    if (QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        perfFreqTicks = static_cast<double>(freq.QuadPart);
    } else {
        perfFreqTicks = 1.0; // fallback
    }
    lastPollTicks = 0;
    lastUpdateHz.store(0.0, std::memory_order_relaxed);
    seq.fetch_add(1, std::memory_order_acq_rel);
    channelUpdateTicks.fill(0);
    latestVelocity.fill(0.0);
    seq.fetch_add(1, std::memory_order_release);
    // mark as enabled for external polling
    running.store(true);
    // Try to map now (best-effort). PollOnce() will also attempt mapping.
    TryMapNow();
    return true;
}

void MMFReader::Stop() {
    // Disable external polling and unmap resources. Keep counters available.
    running.store(false);
    Unmap();
}

std::array<float, 6> MMFReader::GetLatest() const {
    // seqlock read: retry if writer in progress
    std::array<float, 6> out{};
    while (true) {
        uint64_t s = seq.load(std::memory_order_acquire);
        if (s & 1) continue; // writer in progress
        // copy data
        for (int i = 0; i < 6; ++i) out[i] = latest[i];
        uint64_t s2 = seq.load(std::memory_order_acquire);
        if (s == s2) return out; // success
    }
}

void MMFReader::GetLatestWithUpdateCounts(std::array<float, 6>& outValues,
                                          std::array<uint64_t, 6>& outUpdateCounts) const {
    while (true) {
        uint64_t s = seq.load(std::memory_order_acquire);
        if (s & 1) continue;
        for (int i = 0; i < 6; ++i) {
            outValues[i] = latest[i];
            outUpdateCounts[i] = channelUpdateCounts[i];
        }
        uint64_t s2 = seq.load(std::memory_order_acquire);
        if (s == s2) return;
    }
}

void MMFReader::GetLatestWithUpdateCountsAndVelocities(std::array<float, 6>& outValues,
                                                       std::array<uint64_t, 6>& outUpdateCounts,
                                                       std::array<double, 6>& outVelocities) const {
    while (true) {
        uint64_t s = seq.load(std::memory_order_acquire);
        if (s & 1) continue;
        for (int channelIndex = 0; channelIndex < 6; ++channelIndex) {
            outValues[channelIndex] = latest[channelIndex];
            outUpdateCounts[channelIndex] = channelUpdateCounts[channelIndex];
            outVelocities[channelIndex] = latestVelocity[channelIndex];
        }
        uint64_t s2 = seq.load(std::memory_order_acquire);
        if (s == s2) return;
    }
}

std::array<double, 6> MMFReader::GetLatestVelocities() const {
    std::array<double, 6> out{};
    while (true) {
        uint64_t s = seq.load(std::memory_order_acquire);
        if (s & 1) continue;
        for (int channelIndex = 0; channelIndex < 6; ++channelIndex) {
            out[channelIndex] = latestVelocity[channelIndex];
        }
        uint64_t s2 = seq.load(std::memory_order_acquire);
        if (s == s2) return out;
    }
}

bool MMFReader::EnsureMapped() {
    if (hMap && view) return true;
    // Backoff if mapping is not yet available to avoid repeated OpenFileMapping calls
    const ULONGLONG nowTick = GetTickCount64();
    ULONGLONG lastFail = lastMapFailTick.load(std::memory_order_relaxed);
    if (lastFail != 0 && (nowTick - lastFail) < mapRetryIntervalMs) {
        return false;
    }
    // Try to open existing mapping (read-only)
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, mapName.c_str());
    if (!h) {
        lastMapFailTick.store(nowTick, std::memory_order_relaxed);
        return false;
    }
    // Map only the required range: baseOffset + mappedSize (e.g., 24 bytes for 6 floats)
    SIZE_T viewSize = baseOffset + mappedSize;
    if (viewSize == 0) viewSize = mappedSize;
    const void* v = MapViewOfFile(h, FILE_MAP_READ, 0, 0, viewSize);
    if (!v) {
        CloseHandle(h);
        lastMapFailTick.store(nowTick, std::memory_order_relaxed);
        return false;
    }
    hMap = h;
    view = static_cast<const uint8_t*>(v);
    everMapped.store(true);
    lastMapFailTick.store(0, std::memory_order_relaxed);
    // Try to detect a companion named Event: common convention mappingName + "_EVT"
    try {
        std::wstring tryName = mapName + L"_EVT";
        HANDLE he = OpenEventW(SYNCHRONIZE, FALSE, tryName.c_str());
        if (he) {
            // Event exists; consumer can use it
            CloseHandle(he);
            eventName = tryName;
            hasEvent.store(true);
        }
    } catch(...) {}
    return true;
}

void MMFReader::Unmap() {
    if (view) { UnmapViewOfFile(view); view = nullptr; }
    if (hMap) { CloseHandle(hMap); hMap = NULL; }
    // close event handle if opened
    if (!eventName.empty()) {
        // attempt to open and close; we don't store event handle per-thread in this design
        // actual OpenEvent used only to detect existence; nothing to close here
    }
}

// PollOnce: perform one read and publish snapshot using seqlock.
// Intended to be called from an external periodic thread (e.g. EtherCAT master).
bool MMFReader::PollOnce() {
    if (!running.load()) return false; // not enabled
    if (!EnsureMapped()) return false;

    // Read 6 floats from baseOffset; use memcpy for safety
    std::array<float, 6> snap{};
    const uint8_t* localView = view;
    if (!localView) return false;
    const uint8_t* base = localView + baseOffset;
    if (!safe_memcpy(snap.data(), base, sizeof(float) * 6)) return false;
    LARGE_INTEGER now;
    const bool haveTimestamp = QueryPerformanceCounter(&now) != FALSE;
    const uint64_t nowTicks = haveTimestamp ? static_cast<uint64_t>(now.QuadPart) : 0;
    // record one successful read
    readCount.fetch_add(1, std::memory_order_relaxed);
    // check if any value is non-zero (meaningful data)
    bool anyNonZero = false;
    for (int i = 0; i < 6; ++i) { if (snap[i] != 0.0f) { anyNonZero = true; break; } }
    if (anyNonZero) nonZeroReadCount.fetch_add(1, std::memory_order_relaxed);
    std::array<uint64_t, 6> nextUpdateCounts = channelUpdateCounts;
    std::array<uint64_t, 6> nextUpdateTicks = channelUpdateTicks;
    std::array<double, 6> nextVelocities = latestVelocity;

    // compare to prevSnapshot to detect actual updates (prevSnapshot owned by poller)
    bool changed = false;
    for (int channelIndex = 0; channelIndex < 6; ++channelIndex) {
        if (prevSnapshot[channelIndex] != snap[channelIndex]) {
            changed = true;
            ++nextUpdateCounts[channelIndex];

            double velocity = 0.0;
            const uint64_t previousUpdateTicks = nextUpdateTicks[channelIndex];
            if (nowTicks != 0 && previousUpdateTicks != 0 && perfFreqTicks > 0.0) {
                const uint64_t deltaTicks = (nowTicks > previousUpdateTicks) ? (nowTicks - previousUpdateTicks) : 1;
                const double deltaSec = static_cast<double>(deltaTicks) / perfFreqTicks;
                if (deltaSec > 0.0 && deltaSec <= kMaxMmfVelocityDeltaSec) {
                    velocity = (static_cast<double>(snap[channelIndex]) - static_cast<double>(prevSnapshot[channelIndex])) / deltaSec;
                }
            }
            nextVelocities[channelIndex] = velocity;
            nextUpdateTicks[channelIndex] = nowTicks;
        } else if (nowTicks != 0 && nextUpdateTicks[channelIndex] != 0 && perfFreqTicks > 0.0) {
            const uint64_t ageTicks = (nowTicks > nextUpdateTicks[channelIndex])
                ? (nowTicks - nextUpdateTicks[channelIndex])
                : 0;
            const double ageSec = static_cast<double>(ageTicks) / perfFreqTicks;
            if (ageSec >= kMmfVelocityHoldTimeoutSec) {
                nextVelocities[channelIndex] = 0.0;
            }
        }
    }
    if (changed) {
        updateCount.fetch_add(1, std::memory_order_relaxed);
        prevSnapshot = snap;
        if (nowTicks != 0 && perfFreqTicks > 0.0) {
            if (lastPollTicks != 0) {
                const uint64_t deltaTicks = (nowTicks > lastPollTicks) ? (nowTicks - lastPollTicks) : 1;
                double hz = perfFreqTicks / static_cast<double>(deltaTicks);
                lastUpdateHz.store(hz, std::memory_order_relaxed);
            }
            lastPollTicks = nowTicks;
        }
    }

    // publish using seqlock: increment seq to odd, write, then increment to even
    seq.fetch_add(1, std::memory_order_acq_rel); // make odd
    for (int channelIndex = 0; channelIndex < 6; ++channelIndex) {
        latest[channelIndex] = snap[channelIndex];
        channelUpdateCounts[channelIndex] = nextUpdateCounts[channelIndex];
        channelUpdateTicks[channelIndex] = nextUpdateTicks[channelIndex];
        latestVelocity[channelIndex] = nextVelocities[channelIndex];
    }
    if (changed) {
        prevSnapshot = snap;
    }
    seq.fetch_add(1, std::memory_order_release); // make even
    
    return true;
}

bool MMFReader::TryMapNow() {
    // Attempt to open and map the file once synchronously. Do not start worker here.
    if (EnsureMapped()) return true;
    // EnsureMapped may have opened handles; clean up any partial state
    Unmap();
    return false;
}

bool MMFReader::GetRawSnapshot(std::vector<uint8_t> &out) const {
    if (!everMapped.load() || !view) return false;
    SIZE_T size = mappedSize;
    const uint8_t* localView = view;
    if (!localView) return false;
    const uint8_t* base = localView + baseOffset;
    out.resize(size);
    if (!safe_memcpy(out.data(), base, size)) return false;
    return true;
}

uint64_t MMFReader::GetAndResetReadCount() {
    // atomic exchange to zero and return previous count
    // readCount is atomic<uint64_t>
    uint64_t prev = readCount.load(std::memory_order_relaxed);
    // attempt to CAS loop to zero
    while (!readCount.compare_exchange_weak(prev, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        // prev updated with current value; loop until success
    }
    return prev;
}

uint64_t MMFReader::GetAndResetNonZeroReadCount() {
    uint64_t prev = nonZeroReadCount.load(std::memory_order_relaxed);
    while (!nonZeroReadCount.compare_exchange_weak(prev, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    return prev;
}

uint64_t MMFReader::GetAndResetUpdateCount() {
    uint64_t prev = updateCount.load(std::memory_order_relaxed);
    while (!updateCount.compare_exchange_weak(prev, 0, std::memory_order_acq_rel, std::memory_order_relaxed)) {}
    return prev;
}
