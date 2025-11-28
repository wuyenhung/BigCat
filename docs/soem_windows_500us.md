# SOEM on Windows - 500us Stable Cycle Architecture

## Overview

This document describes the current Windows near-RT baseline in this repo for
\~500 μs to 2 ms EtherCAT cycle time using SOEM, focusing on deterministic
PDO scheduling, DC phase alignment, and jitter reduction on a non-RT host.
When a section is a standalone SOEM sketch or tuning idea, treat it as a
conceptual reference; repo-specific status and remaining gaps are tracked in
`soem_windows_500us_execution_priority.md`.

------------------------------------------------------------------------

## System Architecture

    [Isolated Core 4]
     ├─ EtherCAT Thread (TIME_CRITICAL, receive->send polling)
     │   ├─ ec_receive_processdata(..., 0)
     │   ├─ staged output apply
     │   ├─ ec_send_processdata()
     │   ├─ DC PI sync correction
     │   └─ QPC coarse wait + TSC fine spin
     │
     └─ NIC (Intel i210)

    [Other Cores]
     ├─ UI / Logging / Non-RT tasks

------------------------------------------------------------------------

## Hardware & OS Setup

### NIC

-   Intel i210 / i225
-   Disable:
    -   Interrupt Moderation
    -   Energy Efficient Ethernet
    -   Flow Control

### CPU

-   Disable C-States / SpeedStep
-   Fixed frequency

### Windows

-   High Performance mode
-   Disable background tasks (update, defender during test)

------------------------------------------------------------------------

## Thread Strategy

-   REALTIME_PRIORITY_CLASS or HIGH_PRIORITY_CLASS (game coexistence mode)
-   THREAD_PRIORITY_TIME_CRITICAL
-   SetThreadAffinityMask
-   Receive-before-send polling loop (no EC_TIMEOUTRET blocking)
-   High-resolution timer + short TSC spin window

------------------------------------------------------------------------

## Timing Method

-   QueryPerformanceCounter (QPC) for coarse schedule and measurement
-   High-resolution waitable timer for longer waits
-   Invariant TSC spin for the final sub-200 μs window
-   No blocking receive in the cyclic thread; poll previous PDO frame with timeout 0

------------------------------------------------------------------------

## Minimal C++ Example

This example is a compact SOEM timing sketch, not the exact WuBigCat cyclic
loop. The current repo cyclic path polls the previous process-data frame with
timeout 0 before sending the next frame, applies staged outputs, then waits with
the QPC / high-resolution waitable timer / TSC-spin policy described above.

``` cpp
#include <windows.h>
#include <thread>
#include "ethercat.h"

#define CYCLE_TIME_NS 500000

LARGE_INTEGER freq;

inline int64_t get_time_ns()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (t.QuadPart * 1000000000LL) / freq.QuadPart;
}

void ec_loop()
{
    SetThreadAffinityMask(GetCurrentThread(), 1 << 2);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    int64_t next_time = get_time_ns();

    while (true)
    {
        next_time += CYCLE_TIME_NS;

        ec_send_processdata();
        ec_receive_processdata(EC_TIMEOUTRET);

        while (get_time_ns() < next_time)
        {
            _mm_pause();
        }
    }
}

int main()
{
    QueryPerformanceFrequency(&freq);

    if (ec_init("Ethernet0"))
    {
        if (ec_config_init(FALSE) > 0)
        {
            ec_config_map(&IOmap);
            ec_configdc();

            ec_statecheck(0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);

            ec_slave[0].state = EC_STATE_OPERATIONAL;
            ec_writestate(0);
            ec_statecheck(0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);

            std::thread t(ec_loop);
            t.join();
        }
    }
}
```

------------------------------------------------------------------------

## DC Synchronization

Use Distributed Clock:

    ec_configdc();

Current implementation:

-   Bounded PI controller for DC drift correction
-   Phase adjustment stored as `dcToffNs` and applied to the next wait window
-   Fallback smoothing path kept for A/B comparison

------------------------------------------------------------------------

## Jitter Reduction Techniques

-   Receive-before-send polling pipeline
-   Waitable timer + TSC spin hybrid
-   VirtualLock memory
-   CPU core isolation
-   NIC driver tuning and audit where the adapter exposes safe controls

------------------------------------------------------------------------

## Expected Performance

  Mode        Cycle    Jitter
  ----------- -------- -------------
  Default     1 ms     ±5\~20 ms
  Optimized   500 μs   ±50\~150 μs

------------------------------------------------------------------------

## Suggested AI Optimization Tasks

1.  ETW / DPC latency profiling on Windows
2.  NIC advanced-property audit beyond RSS and moderation
3.  Lock-free logging buffer for deeper cycle traces
4.  Cache alignment on hot PDO structures
5.  Interrupt latency measurement under mixed UI / HTTP load
6.  Prefetch optimization on IOmap and descriptor walks
7.  NUMA-aware memory placement
8.  Long-run 500 μs soak tests with jitter histogram export

------------------------------------------------------------------------

## Conclusion

Achieving 500 μs on Windows is about determinism, not raw performance.
