// test_mmf_sine_writer.cpp
// 模擬 FlyPT Mover：以穩定 1ms 週期寫入 sin 波形到 MMF，供 bigcat.exe 讀取
// 用法: test_mmf_sine_writer.exe [amplitude] [period_sec] [duration_sec] [fade_in_sec] [fade_out_sec]
//   預設: amplitude=50, period=5s, duration=0 (永久), fade_in=3s, fade_out=3s
//   duration=0 為永久執行，按 Ctrl+C 優雅漸出退出
//
// MMF 名稱: Local\OUTPUT_MMF_6DOF (Pose6D 模式)
// 6 通道: Sway, Surge, Heave, Pitch, Roll, Yaw
// sin 波只驅動 Heave (ch2)，其餘為 0

#include <windows.h>
#include <mmsystem.h>
#include <avrt.h>
#include <intrin.h>     // _mm_pause
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

static constexpr const wchar_t* kMmfName = L"Local\\OUTPUT_MMF_6DOF";
static constexpr size_t kChannelCount = 6;
static constexpr double kDefaultAmplitude = 50.0;      // mm or deg
static constexpr double kDefaultPeriodSec = 5.0;        // sin 波週期
static constexpr double kDefaultDurationSec = 0.0;      // 0 = 永久執行 (Ctrl+C 退出)
static constexpr double kDefaultFadeInSec = 3.0;        // 漸入時間
static constexpr double kDefaultFadeOutSec = 3.0;       // 漸出時間
static constexpr double kCycleMs = 1.0;                 // 目標週期

// 背景輸出執行緒：避免 printf 在終端機選取時阻塞主循環定時
// 主循環只更新 atomic 狀態，輸出執行緒每 500ms 列印一次
struct CycleStatus {
    double elapsedSec;
    size_t cycles;
    float sinVal;
    double gain;
    size_t overruns;
};
static volatile LONG g_statusLock = 0;
static CycleStatus g_status = {};
static bool g_running = true;

static void status_update(const CycleStatus& s) {
    // 簡單 spin-lock（只有數十 cycles，遠小於 1ms 週期）
    while (InterlockedExchange(&g_statusLock, 1)) _mm_pause();
    g_status = s;
    InterlockedExchange(&g_statusLock, 0);
}

static bool status_read(CycleStatus& out) {
    while (InterlockedExchange(&g_statusLock, 1)) _mm_pause();
    out = g_status;
    InterlockedExchange(&g_statusLock, 0);
    return g_running;
}

DWORD WINAPI OutputThread(LPVOID) {
    // 設為低優先級，不影響主循環
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (true) {
        Sleep(500); // 每 500ms 更新一次畫面
        CycleStatus s;
        if (!status_read(s)) break;
        if (s.cycles > 0) {
            printf("  t=%.1fs  cycles=%zu  sin=%.2f  gain=%.2f  overruns=%zu\r",
                   s.elapsedSec, s.cycles, s.sinVal, s.gain, s.overruns);
            fflush(stdout);
        }
    }
    return 0;
}

static volatile bool g_exitRequested = false;

static BOOL WINAPI CtrlHandler(DWORD dwCtrlType) {
    if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
        g_exitRequested = true;
        return TRUE;
    }
    return FALSE;
}

// 平滑漸變增益：用 cosine 曲線做出平滑的 ramp
static inline double fade_gain(double t, double fadeDur, bool isFadeIn) {
    if (fadeDur <= 0.0) return 1.0;
    double ratio = t / fadeDur;
    if (ratio >= 1.0) return 1.0;
    // cosine easing: fade-in 0→1, fade-out 1→0
    double cosVal = std::cos(ratio * 3.141592653589793);
    if (isFadeIn)
        return 0.5 * (1.0 - cosVal);   // 0→1
    else
        return 0.5 * (1.0 + cosVal);   // 1→0
}

struct JitterStats {
    double minMs = 1e9;
    double maxMs = 0.0;
    double sumMs = 0.0;
    size_t count = 0;
    size_t overruns = 0;  // cycles > 2.5ms
    void record(double ms) {
        if (ms < minMs) minMs = ms;
        if (ms > maxMs) maxMs = ms;
        sumMs += ms;
        count++;
        if (ms > 2.5) overruns++;
    }
};

int main(int argc, char* argv[]) {
    double amplitude = kDefaultAmplitude;
    double periodSec = kDefaultPeriodSec;
    double durationSec = kDefaultDurationSec;
    double fadeInSec = kDefaultFadeInSec;
    double fadeOutSec = kDefaultFadeOutSec;

    if (argc >= 2) { double v = atof(argv[1]); if (v > 0.0) amplitude = v; }
    if (argc >= 3) { double v = atof(argv[2]); if (v > 0.0) periodSec = v; }
    if (argc >= 4) { double v = atof(argv[3]); if (v >= 0.0) durationSec = v; }
    if (argc >= 5) { double v = atof(argv[4]); if (v >= 0.0) fadeInSec = v; }
    if (argc >= 6) { double v = atof(argv[5]); if (v >= 0.0) fadeOutSec = v; }

    const bool infinite = (durationSec <= 0.0);
    if (infinite) {
        // 永久模式：設一個極大的擬似時長，實際靠 Ctrl+C 中斷
        durationSec = 1e9;
    } else if (fadeInSec + fadeOutSec > durationSec) {
        // 防呆：漸入+漸出不能超過總時長
        double total = fadeInSec + fadeOutSec;
        fadeInSec = fadeInSec * durationSec / total;
        fadeOutSec = fadeOutSec * durationSec / total;
        printf("WARNING: fade_in+fade_out > duration, scaled to in=%.1fs out=%.1fs\n",
               fadeInSec, fadeOutSec);
    }

    // Ctrl+C 處理：用於永久模式優雅漸出退出
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // === 即時優化：MMCSS + 核心綁定 + 高優先級 ===
    // MMCSS Pro Audio: Windows 多媒體排程器保護，降低被其他執行緒搶佔
    DWORD mmcssTaskIndex = 0;
    HANDLE hMmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTaskIndex);
    if (!hMmcss) {
        printf("WARNING: AvSetMmThreadCharacteristics failed (err=%lu), continuing without MMCSS\n",
               GetLastError());
    }

    // 提高 process 優先級（REALTIME 需 Admin，失敗降級 HIGH）
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
        printf("WARNING: REALTIME_PRIORITY_CLASS requires Admin (err=%lu), falling back to HIGH\n",
               GetLastError());
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 綁定到 CPU 3（根據 IRQ 策略表，此核心無 IRQ/DPC 競爭）
    DWORD_PTR targetCpu = 0x08;  // CPU 3
    DWORD_PTR oldAffinity = SetThreadAffinityMask(GetCurrentThread(), targetCpu);
    if (!oldAffinity) {
        printf("WARNING: SetThreadAffinityMask failed (err=%lu)\n", GetLastError());
    }

    // 鎖定記憶體防止 page fault 中斷
    {
        HANDLE hProc = GetCurrentProcess();
        SetProcessWorkingSetSize(hProc, SIZE_T(-1), SIZE_T(-1));
    }

    // 系統 timer 解析度降到 1ms
    timeBeginPeriod(1);

    // 建立 MMF（若已存在則開啟，否則新建）
    const DWORD mmfSize = static_cast<DWORD>(sizeof(float) * kChannelCount);
    HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, kMmfName);
    if (!hMap) {
        hMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, mmfSize, kMmfName);
    }
    if (!hMap) {
        fprintf(stderr, "ERROR: Cannot open or create MMF '%ls' (err=%lu)\n",
                kMmfName, GetLastError());
        return 1;
    }

    float* view = static_cast<float*>(MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, mmfSize));
    if (!view) {
        fprintf(stderr, "ERROR: MapViewOfFile failed (err=%lu)\n", GetLastError());
        CloseHandle(hMap);
        return 1;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const LONGLONG cycleTicks = static_cast<LONGLONG>(kCycleMs / 1000.0 * freq.QuadPart);
    const double freqInv = 1.0 / static_cast<double>(freq.QuadPart);

    printf("MMF Sine Writer: %ls\n", kMmfName);
    printf("  Realtime: MMCSS=%s, CPU=%d, Priority=REALTIME+CRITICAL\n",
           hMmcss ? "Pro Audio" : "OFF", (int)(targetCpu ? 3 : -1));
    if (infinite) {
        printf("  Amplitude: %.1f, Period: %.1fs, Duration: INFINITE (Ctrl+C)\n",
               amplitude, periodSec);
    } else {
        printf("  Amplitude: %.1f, Period: %.1fs, Duration: %.0fs\n",
               amplitude, periodSec, durationSec);
    }
    printf("  Fade-in: %.1fs, Fade-out: %.1fs\n", fadeInSec, fadeOutSec);
    printf("  Target cycle: %.1fms (%lld QPC ticks)\n", kCycleMs, cycleTicks);
    printf("  Sine on Heave (ch2), others=0\n");
    printf("  Console output on background thread (click terminal freely)\n");
    printf("  Press Ctrl+C to stop (graceful fade-out)\n\n");
    fflush(stdout);

    // 啟動背景輸出執行緒
    HANDLE hOutputThread = CreateThread(nullptr, 0, OutputThread, nullptr, 0, nullptr);
    if (!hOutputThread) {
        printf("WARNING: OutputThread creation failed (err=%lu), continuing without\n",
               GetLastError());
    }

    JitterStats stats;
    LARGE_INTEGER t0, nextWake;
    QueryPerformanceCounter(&t0);
    nextWake = t0;
    double elapsedSec = 0.0;

    // 漸出起始時間：finite 模式為結束前 fadeOutSec 秒；
    // infinite 模式由 Ctrl+C 觸發時動態設定
    double fadeOutStart = infinite ? 1e9 : durationSec - fadeOutSec;
    bool fadeOutActive = false;
    size_t cycles = 0;

    while (elapsedSec < durationSec) {
        // 計算 sin 值
        double phase = elapsedSec / periodSec * 2.0 * 3.141592653589793;
        float sinVal = static_cast<float>(amplitude * std::sin(phase));

        // 漸入/漸出增益
        double gain = 1.0;

        // 漸入（只在 startup 階段）
        if (elapsedSec < fadeInSec) {
            gain = fade_gain(elapsedSec, fadeInSec, true);
        }

        // Ctrl+C 觸發漸出（infinite 模式）
        if (infinite && g_exitRequested && !fadeOutActive) {
            fadeOutStart = elapsedSec;
            fadeOutActive = true;
            printf("\n  *** Ctrl+C caught, fading out... ***\n");
            fflush(stdout);
        }

        // 漸出
        if (elapsedSec >= fadeOutStart) {
            double tOut = elapsedSec - fadeOutStart;
            if (tOut >= fadeOutSec) {
                // 漸出完成，最後一次寫 0 後退出
                for (size_t i = 0; i < kChannelCount; ++i) view[i] = 0.0f;
                elapsedSec = (elapsedSec > fadeOutStart + fadeOutSec)
                             ? fadeOutStart + fadeOutSec : elapsedSec;
                break;
            }
            gain = fade_gain(tOut, fadeOutSec, false);
        }

        sinVal = static_cast<float>(sinVal * gain);

        // 寫入 MMF: Heave (ch2) = sin, 其餘 = 0
        view[0] = 0.0f;           // Sway
        view[1] = 0.0f;           // Surge
        view[2] = sinVal;         // Heave ← sin 波
        view[3] = 0.0f;           // Pitch
        view[4] = 0.0f;           // Roll
        view[5] = 0.0f;           // Yaw

        cycles++;

        // 更新背景輸出狀態（無阻塞，無論 terminal 是否被選取）
        CycleStatus stat;
        stat.elapsedSec = elapsedSec;
        stat.cycles = cycles;
        stat.sinVal = view[2];
        stat.gain = gain;
        stat.overruns = stats.overruns;
        status_update(stat);

        // 精確等待下一個週期
        nextWake.QuadPart += cycleTicks;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        // 記錄 jitter
        LONGLONG delta = now.QuadPart - t0.QuadPart;
        double actualMs = delta * freqInv * 1000.0;
        double expectedMs = cycles * kCycleMs;
        double jitterMs = actualMs - expectedMs;
        stats.record(kCycleMs + jitterMs);  // actual cycle time

        // Spin-wait 到目標時間（1ms 策略：不用 Sleep(1)，避免超時）
        if (nextWake.QuadPart > now.QuadPart) {
            const LONGLONG kSpinPause = cycleTicks / 20;      // ~50us 以下純 spin
            const LONGLONG kSleep0Threshold = cycleTicks / 2; // >500us 才 Sleep(0)
            while (true) {
                QueryPerformanceCounter(&now);
                if (now.QuadPart >= nextWake.QuadPart) break;
                LONGLONG remaining = nextWake.QuadPart - now.QuadPart;
                if (remaining > kSleep0Threshold) {
                    Sleep(0);   // 遠離目標：快速讓出時間片
                } else if (remaining > kSpinPause) {
                    _mm_pause(); // 中距離：pause 指令降低功耗
                }
                // 最後 ~50us：純 spin，不 pause 確保精確
            }
        } else {
            // 超時：直接設下個目標
            nextWake = now;
        }

        elapsedSec = (now.QuadPart - t0.QuadPart) * freqInv;
    }

    // 通知背景輸出執行緒結束
    g_running = false;
    if (hOutputThread) {
        WaitForSingleObject(hOutputThread, 2000);
        CloseHandle(hOutputThread);
    }

    printf("\n");  // 換行，結束進度列

    // 最後寫入 0 避免 bigcat 停留在偏移位置
    for (size_t i = 0; i < kChannelCount; ++i) view[i] = 0.0f;

    UnmapViewOfFile(view);
    CloseHandle(hMap);
    timeEndPeriod(1);
    if (hMmcss) AvRevertMmThreadCharacteristics(hMmcss);
    if (oldAffinity) SetThreadAffinityMask(GetCurrentThread(), oldAffinity);

    printf("\n\n========== MMF Sine Writer Summary ==========\n");
    printf("Total cycles:     %zu\n", cycles);
    printf("Duration:         %.3f s\n", elapsedSec);
    printf("Avg cycle:        %.3f ms (%.1f Hz)\n",
           stats.sumMs / stats.count, 1000.0 * stats.count / stats.sumMs);
    printf("Min cycle:        %.3f ms\n", stats.minMs);
    printf("Max cycle:        %.3f ms\n", stats.maxMs);
    printf("Jitter:           %.3f ms\n", stats.maxMs - stats.minMs);
    printf("Overruns (>2.5ms): %zu / %zu\n", stats.overruns, stats.count);
    if (stats.overruns > 0)
        printf("  ** WARNING: %zu cycles exceeded 2.5ms! **\n", stats.overruns);
    else
        printf("  ** All cycles within 2.5ms ✓ **\n");

    return 0;
}
