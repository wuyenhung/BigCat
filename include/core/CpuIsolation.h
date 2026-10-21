#pragma once
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <windows.h>

namespace CoreHelpers {

    // CPU Isolation Manager
    // Hardware-level core isolation: reserves a physical CPU core (and its
    // HT/SMT sibling logical cores) exclusively for the SOEM cyclic thread.
    // Non-RT threads and other processes are barred from the RT core set via
    // per-thread affinity plus the software repel monitor.
    //
    // NOTE: the process affinity mask is deliberately kept at the FULL system
    // mask — narrowing it would make the cyclic thread's SetThreadAffinityMask
    // fail silently (thread affinity must be a subset of process affinity).

    class CpuIsolation {
    public:
        // ── OS-level hard core isolation ──
        // Call once at startup (before thread creation). Detects CPU topology,
        // identifies which logical cores belong to the target physical core,
        // raises the process priority class, and pins the CALLING thread to the
        // non-RT mask. The process affinity mask itself is NOT narrowed.
        // rtPhysicalCoreIndex: 0-based physical core index (main.cpp auto-picks
        //                      from the highest physical core downwards, skipping
        //                      core 0; BigCat.ini [EtherCAT] RtCoreIndex=N overrides)
        // logOut: diagnostic log
        // Returns true if hard isolation was successfully applied
        static bool InitializeHardIsolation(int rtPhysicalCoreIndex, std::string &logOut);

        // Pin the calling thread to the RT physical core ONLY (single logical core).
        // The HT/SMT sibling is intentionally left idle to keep L1/L2 cache clean.
        // Call from the cyclic thread entry point before entering the PDO loop.
        static bool PinCurrentThreadToRtCores();

        // Pin the calling thread AWAY from the RT physical core (ALL its logical
        // cores including HT/SMT siblings). Call from every non-cyclic worker
        // thread entry point to prevent cache-line bouncing and scheduling
        // pressure on the RT physical core.
        static bool ExcludeCurrentThreadFromRtCores();

        // Get the RT core affinity mask — all logical cores of the RT physical core.
        // Used as the monitor's guard mask, the baseline for VerifyRtCoreIsolation(),
        // and the strip source for IRQ/USB re-routing. NOT used to narrow the
        // process affinity mask (see class note above).
        static DWORD_PTR GetRtCoreMask();

        // Get the RT thread affinity mask — only the first (physical) logical core.
        // The HT sibling is intentionally excluded for clean L1/L2 cache.
        static DWORD_PTR GetRtThreadMask();

        // Get the non-RT core affinity mask — the RT-adjacent physical core
        // (rtPhysicalCore - 1) including all of its logical cores, i.e. the home
        // of every non-RT thread. This is NOT "all cores except the RT set".
        static DWORD_PTR GetNonRtCoreMask();

        // Get the 0-based physical core index being isolated
        static int GetRtPhysicalCoreIndex();

        // Returns true if hardware isolation is active
        static bool IsHardIsolationActive();

        // ── Verification ──
        // Enumerate all processes and report any that still have the RT core
        // in their affinity mask. Call after InitializeHardIsolation to
        // confirm the RT core is actually clear of foreign processes.
        // Returns a multi-line diagnostic string (UTF-8).
        static std::string VerifyRtCoreIsolation();

        // ── Runtime diagnostics ──
        // Comprehensive RT core health check: CPU topology, affinity masks,
        // per-core DPC/interrupt counters, cyclic thread binding confirmation.
        // Uses NtQuerySystemInformation(SystemProcessorPerformanceInformation)
        // to measure actual DPC and interrupt load on the RT physical core.
        // Returns a multi-line diagnostic string (UTF-8).
        static std::string DiagnoseRtCoreState();

        // ── Repel interval control ──
        // Set the interval (ms) between foreign-process repel scans.
        // MonitorThreadLoop() clamps the stored value to 5..100 ms (a 2 ms
        // request is therefore honored as 5 ms).
        // NOTE: no caller exists today, so s_repelIntervalMs stays at its 10 ms
        // default. Kept as the intended hook for PDO cycle-time coupling.
        static void SetRepelIntervalMs(int ms) { s_repelIntervalMs.store(ms, std::memory_order_relaxed); }
        static int  GetRepelIntervalMs() { return s_repelIntervalMs.load(std::memory_order_relaxed); }

        // ── Software-level isolation monitor (existing behavior) ──
        // Run one immediate round of repelling foreign processes from the RT core.
        // Useful at startup to clear the RT core before the periodic monitor begins.
        static void RepelOnce();
        static void StartIsolationMonitor();
        static void StopIsolationMonitor();

        // Signal the monitor thread to perform one repel cycle.
        // Called from CyclicTask once per PDO cycle (SetEvent on
        // s_repelTriggerEvent).
        // NOTE: not wired up — MonitorThreadLoop() waits only on monitorStopEvent
        // with timer-based slices, so the signal is currently never consumed (the
        // auto-reset event simply stays signaled). Wiring it up would require
        // WaitForMultipleObjects plus a minimum-interval throttle.
        static void TriggerRepel();

    private:
        // CPU topology detection helpers
        static bool DetectCpuTopology(int rtPhysicalCoreIndex, std::string &logOut);
        static void ApplyProcessLevelIsolation(std::string &logOut);

        // ── Win11+ real-time thread / process boost (safe no-op on older OS) ──
        // Thread-level: lower IO priority (reduce Npcap callback preemption) +
        //               register as Pro Audio for favourable scheduler quantum.
        static void ApplyRealtimeThreadBoost(std::string &logOut);
        // Process-level: disable CPU power throttling (Win11 throttles all
        //                 processes by default; this keeps cyclic core at full speed).
        static void ApplyProcessPowerThrottling(std::string &logOut);

        // Monitor thread
        static void MonitorThreadLoop();
        static void RepelOtherProcesses(DWORD_PTR rtCoreMask);
        // Fast (affinity-only) repel scan — one process snapshot, one
        // OpenProcess per process, write only for overlapping processes.
        // forceThreadMigrate = true additionally force-migrates the threads of
        // every process whose mask was just changed, so they leave the RT core
        // immediately instead of waiting for the next quantum boundary.
        static void RepelOtherProcessesFast(DWORD_PTR rtCoreMask,
                                            bool forceThreadMigrate = false);

        // Thread-level repel: force-migrate all threads of a single process
        // away from the RT core. Called immediately after SetProcessAffinityMask
        // to ensure threads don't linger on the RT core until quantum expiry.
        static void RepelProcessThreadsFromRtCore(DWORD pid, DWORD_PTR rtCoreMask);

        // State
        static std::atomic<bool> monitorRunning;
        static std::thread monitorThread;
        static HANDLE monitorStopEvent;  // wakes Sleep in MonitorThreadLoop for fast stop

        // Hard isolation state
        static bool   hardIsolationActive_;
        static int    rtPhysicalCore_;
        static DWORD_PTR rtCoreMask_;      // all logical cores of the RT physical core (monitor guard mask)
        static DWORD_PTR rtThreadMask_;    // first logical core only (for cyclic thread — clean L1/L2)
        static DWORD_PTR nonRtCoreMask_;   // RT-adjacent physical core (rtPhysicalCore - 1), all its logical cores

        // Repel trigger event — pulsed by CyclicTask each PDO cycle.
        // Currently not consumed by the monitor (see TriggerRepel note).
        static HANDLE s_repelTriggerEvent;

        // Dynamic repel interval (ms). Never written today (no caller of
        // SetRepelIntervalMs); MonitorThreadLoop() clamps it to 5..100 ms.
        static std::atomic<int> s_repelIntervalMs;

        // Legacy name list (browsers / VR / Game Bar processes). Not referenced
        // by any repel path today — the monitor repels by affinity overlap, not
        // by name. Kept only as a candidate list for targeted repelling.
        static const std::vector<std::wstring> targetProcessNames;
    };

}
