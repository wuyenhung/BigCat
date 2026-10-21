// test_mmf_monitor.cpp
// 獨立監控程式：讀取 Local\Wu6DoF_MMF，分析寫入穩定性
// 用法: test_mmf_monitor.exe [duration_sec] [csv_output_path]
//   無參數: 執行 10 秒，輸出摘要到 stdout
//   指定秒數: 執行指定秒數
//   指定 csv 路徑: 同時輸出逐筆 CSV

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

static constexpr const wchar_t* kMmfName = L"Local\\OUTPUT_MMF_6DOF";
static constexpr size_t kChannelCount = 6;
static constexpr double kDefaultDurationSec = 10.0;

struct Sample {
    LARGE_INTEGER qpc;
    float values[kChannelCount];
    bool anyChanged;
};

int main(int argc, char* argv[]) {
    double durationSec = kDefaultDurationSec;
    const char* csvPath = nullptr;

    if (argc >= 2) {
        double v = atof(argv[1]);
        if (v > 0.0) durationSec = v;
    }
    if (argc >= 3) {
        csvPath = argv[2];
    }

    // Open MMF
    HANDLE hMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kMmfName);
    if (!hMap) {
        fprintf(stderr, "ERROR: Cannot open MMF '%ls' (err=%lu). Is the MMF writer running?\n",
                kMmfName, GetLastError());
        return 1;
    }

    const void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(float) * kChannelCount);
    if (!view) {
        fprintf(stderr, "ERROR: MapViewOfFile failed (err=%lu)\n", GetLastError());
        CloseHandle(hMap);
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqInv = 1.0 / static_cast<double>(freq.QuadPart);

    printf("MMF Monitor: reading '%ls', duration=%.1fs\n", kMmfName, durationSec);
    printf("QPC frequency: %lld Hz\n", freq.QuadPart);
    fflush(stdout);

    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>(durationSec * 2000.0)); // ~2kHz

    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const LONGLONG t0Ticks = t0.QuadPart;
    const LONGLONG durationTicks = static_cast<LONGLONG>(durationSec * freq.QuadPart);

    float prevValues[kChannelCount] = {0};
    bool firstSample = true;

    FILE* csvFile = nullptr;
    if (csvPath) {
        csvFile = fopen(csvPath, "w");
        if (csvFile) {
            fprintf(csvFile, "elapsed_ms,v0,v1,v2,v3,v4,v5,changed\n");
        }
    }

    // Tight polling loop
    while (true) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        Sample s;
        s.qpc = now;
        memcpy(s.values, view, sizeof(float) * kChannelCount);

        s.anyChanged = false;
        if (firstSample) {
            firstSample = false;
            s.anyChanged = true;
        } else {
            for (size_t i = 0; i < kChannelCount; ++i) {
                if (s.values[i] != prevValues[i]) {
                    s.anyChanged = true;
                    break;
                }
            }
        }

        for (size_t i = 0; i < kChannelCount; ++i) prevValues[i] = s.values[i];
        samples.push_back(s);

        if (csvFile) {
            double elapsed = (now.QuadPart - t0Ticks) * freqInv * 1000.0;
            fprintf(csvFile, "%.3f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%d\n",
                    elapsed,
                    (double)s.values[0], (double)s.values[1],
                    (double)s.values[2], (double)s.values[3],
                    (double)s.values[4], (double)s.values[5],
                    s.anyChanged ? 1 : 0);
        }

        if ((now.QuadPart - t0Ticks) >= durationTicks) break;

        // Spin-wait ~0.5ms to avoid 100% CPU but keep high sample rate
        LARGE_INTEGER spinTarget = now;
        spinTarget.QuadPart += freq.QuadPart / 2000; // 0.5ms
        while (true) {
            LARGE_INTEGER cur;
            QueryPerformanceCounter(&cur);
            if (cur.QuadPart >= spinTarget.QuadPart) break;
        }
    }

    if (csvFile) fclose(csvFile);

    UnmapViewOfFile(view);
    CloseHandle(hMap);

    // Statistics
    size_t totalSamples = samples.size();
    size_t changedSamples = 0;
    double sumInterval = 0.0;
    double minInterval = 1e9;
    double maxInterval = 0.0;
    LONGLONG lastChangeTicks = 0;
    bool haveFirstChange = false;

    for (const auto& s : samples) {
        if (s.anyChanged) {
            changedSamples++;
            if (haveFirstChange) {
                double interval = (s.qpc.QuadPart - lastChangeTicks) * freqInv * 1000.0;
                sumInterval += interval;
                if (interval < minInterval) minInterval = interval;
                if (interval > maxInterval) maxInterval = interval;
            } else {
                haveFirstChange = true;
            }
            lastChangeTicks = s.qpc.QuadPart;
        }
    }

    double totalTimeSec = (samples.back().qpc.QuadPart - t0Ticks) * freqInv;
    double sampleRate = totalSamples / totalTimeSec;
    double changeRate = changedSamples / totalTimeSec;

    printf("\n========== MMF Monitor Summary ==========\n");
    printf("Duration:        %.3f s\n", totalTimeSec);
    printf("Total samples:   %zu (%.1f Hz)\n", totalSamples, sampleRate);
    printf("Changed samples: %zu (%.1f Hz)\n", changedSamples, changeRate);
    printf("Unchanged ratio: %.1f%%\n", 100.0 * (totalSamples - changedSamples) / totalSamples);

    if (changedSamples >= 2) {
        size_t intervals = changedSamples - 1;
        double avgInterval = sumInterval / intervals;
        printf("Update intervals:\n");
        printf("  Min:  %.3f ms\n", minInterval);
        printf("  Max:  %.3f ms\n", maxInterval);
        printf("  Avg:  %.3f ms\n", avgInterval);
        printf("  Jitter (max-min): %.3f ms\n", maxInterval - minInterval);

        if (maxInterval > avgInterval * 2.0) {
            printf("  ** WARNING: Max interval > 2x average - writer is irregular! **\n");
        }
        if (minInterval < 0.5) {
            printf("  ** NOTE: Min interval < 0.5ms - possible duplicate write burst **\n");
        }
    } else {
        printf("  ** WARNING: Fewer than 2 changes detected - MMF writer may not be active **\n");
    }

    // Channel-specific: check which channels actually change
    printf("\nChannel activity:\n");
    for (size_t ch = 0; ch < kChannelCount; ++ch) {
        size_t chChanges = 0;
        float firstVal = samples[0].values[ch];
        float lastVal = samples.back().values[ch];
        for (size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].values[ch] != samples[i-1].values[ch]) chChanges++;
        }
        printf("  Ch%zu: %zu changes, first=%.4f last=%.4f range=%.4f\n",
               ch, chChanges, (double)firstVal, (double)lastVal,
               (double)(lastVal - firstVal));
    }

    if (csvPath) printf("\nCSV written to: %s\n", csvPath);
    return 0;
}
