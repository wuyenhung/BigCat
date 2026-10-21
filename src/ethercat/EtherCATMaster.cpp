// EtherCATMaster.cpp - 簡要說明:
// EtherCAT 主站核心實作，包含通訊初始化、即時執行緒、PDO/SDO 交換與狀態管理。

#include "EtherCATMaster.h"
#include "core/CpuIsolation.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
// for YieldProcessor
#include <intrin.h>
// timeBeginPeriod/timeEndPeriod
#include <mmsystem.h>
// for ALstatus code to string
#include <soem/ec_print.h>
#include <fstream>
#include "SafeMem.h"

// --- Core allocator: round-robin pick of physical-core masks to spread threads ---
#include <windows.h>
#include <vector>
#include <cstdlib>
#include <mutex>
#include <atomic>
#include "core/CpuTopology.h"

namespace {
    // Cached list of candidate logical-processor masks (one representative bit per physical core)
    std::vector<DWORD_PTR> g_coreCandidates;
    std::mutex g_coreCandidatesMutex;
    std::atomic<size_t> g_coreNextIndex(0);

    // Core reserved for the CyclicTask – dynamically resolved from CpuIsolation.
    // All other threads allocated via round-robin must avoid this core.
    DWORD_PTR GetReservedCoreMask() {
        if (CoreHelpers::CpuIsolation::IsHardIsolationActive()) {
            DWORD_PTR m = CoreHelpers::CpuIsolation::GetRtCoreMask();
            if (m) return m;
        }
        int rtCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
        if (rtCore >= 0 && rtCore < 64) return (DWORD_PTR)1 << rtCore;
        return 0;
    }
    std::atomic<bool> g_physicalEthercatAllowed{true};
    std::mutex g_physicalEthercatPolicyMutex;
    std::string g_physicalEthercatRestrictionReason;

    // Build the candidate list from the current system/process affinity info.
    void BuildCoreCandidates()
{
    std::lock_guard<std::mutex> lk(g_coreCandidatesMutex);
    if (!g_coreCandidates.empty()) return; // already built
    DWORD_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) return;
    DWORD_PTR available = processMask ? processMask : systemMask;
    if (available == 0) return;

    // Intersect with non-RT core mask so per-slave threads are confined to
    // the near-RT core (adjacent to RT core). This keeps cache locality with
    // the Cyclic thread and leaves middle cores clean for DCS/game processes.
    DWORD_PTR nonRtMask = CoreHelpers::CpuIsolation::GetNonRtCoreMask();
    if (nonRtMask != 0) available &= nonRtMask;
    if (available == 0) available = processMask ? processMask : systemMask;

    // Allow user override via environment variables:
    //  - WUSOEM_AFFINITY_MASK: hex or decimal mask to further restrict allowed LPs
    //  - WUSOEM_MAX_CORES: decimal max number of physical-core candidates to use
    // Use CpuTopology to query physical core groups and pick one LP per core.
    CoreHelpers::CpuTopology::EnsurePopulated();
    const auto& cores = CoreHelpers::CpuTopology::Cores();

    for (const auto& c : cores) {
        if (c.lps.empty()) continue;
        DWORD_PTR pick = ((DWORD_PTR)1 << c.lps[0]) & available;
        if (pick == 0) continue;
        // Skip the reserved EtherCAT core
        if (pick & GetReservedCoreMask()) continue;
        g_coreCandidates.push_back(pick);
    }

    // Fallback: if no topology data available, pick lowest bits from process mask
    if (g_coreCandidates.empty()) {
        DWORD_PTR m = available;
        while (m) {
            DWORD_PTR pick = m & (~m + 1);
            if (!(pick & GetReservedCoreMask()))
                g_coreCandidates.push_back(pick);
            m &= (m - 1); // clear lowest bit
        }
    }

    // Auto-limit number of candidates to avoid over-constraining when process affinity is small
    // Strategy: if more than 3 physical-core candidates, keep only first 3 to preserve spread for RT threads
    const size_t defaultMax = 3;
    if (g_coreCandidates.size() > defaultMax) {
        g_coreCandidates.resize(defaultMax);
    }
}

} // anonymous namespace

void EtherCATMaster::SetPhysicalEthercatAllowed(bool allowed, const std::string& reason)
{
    g_physicalEthercatAllowed.store(allowed, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_physicalEthercatPolicyMutex);
    g_physicalEthercatRestrictionReason = allowed ? std::string() : reason;
}

bool EtherCATMaster::IsPhysicalEthercatAllowed()
{
    return g_physicalEthercatAllowed.load(std::memory_order_relaxed);
}

std::string EtherCATMaster::GetPhysicalEthercatRestrictionReason()
{
    std::lock_guard<std::mutex> lk(g_physicalEthercatPolicyMutex);
    return g_physicalEthercatRestrictionReason;
}

// Class static wrapper exposed via header
DWORD_PTR EtherCATMaster::AllocateCoreMaskRoundRobin()
{
    BuildCoreCandidates();
    std::lock_guard<std::mutex> lk(g_coreCandidatesMutex);
    if (g_coreCandidates.empty()) return 0;
    size_t idx = g_coreNextIndex.fetch_add(1) % g_coreCandidates.size();
    return g_coreCandidates[idx];
}

// Allow external trigger to force rebuild of candidate list (for auto-tune)
void ForceRebuildCoreCandidates()
{
    std::lock_guard<std::mutex> lk(g_coreCandidatesMutex);
    g_coreCandidates.clear();
    g_coreNextIndex.store(0);
    // next AllocateCoreMaskRoundRobin() will rebuild
}

void EtherCATMaster::AutoTuneAffinityIfNeeded()
{
    uint32_t maxObs = maxObservedCommsTime.load(std::memory_order_relaxed);
    uint64_t now = GetTickCount64();
    if (!autoTuneAffinity) return;
    if (maxObs < autoTuneJitterThreshold) return;
    if (now - lastAffinityTuneMs < autoTuneIntervalMs) return;
    // Trigger rebuild and reshuffle candidate list
    ForceRebuildCoreCandidates();
    lastAffinityTuneMs = now;
    // Log tuning event
    char buf[96];
    snprintf(buf, sizeof(buf), "AutoTuneAffinity: triggered (maxObs=%luus)", (unsigned long)maxObs);
    SetError(buf);
}

EtherCATMaster::EtherCATMaster() 
    : slaveCount(0)
    , isRunning(false)
    , isInitialized(false)
    , cyclicThread(NULL)
    , stopEvent(NULL)
    , cycleTime(2000)
    , desiredCycleTime(2000)
    , workingCounter(0)
    , pdosSentCount(0)
    , operationStartTime(0)
    , pdoProcessingThread(NULL)
    , pdoStopEvent(NULL)
    , pdoThreadRunning(false)
{
    memset(&context, 0, sizeof(context));
    memset(ifname, 0, sizeof(ifname));
    memset(IOmap, 0, sizeof(IOmap));
    InitializeCriticalSection(&cs);
    InitializeCriticalSection(&pdoDataCS);
    InitializeCriticalSection(&pdoExchangeCS);
    // initialize SDO lock
    InitializeCriticalSection(&sdoCS);
    // Initialize PDO filter lock
    InitializeCriticalSection(&pdoFilterCS_);
    // Initialize MMF output lock
    InitializeCriticalSection(&mmfOutputCS_);
    InitializeCriticalSection(&poseMmfCS_);
    InitializeCriticalSection(&simulationCS_);
    InitializeCriticalSection(&errorCS_);
    InitializeCriticalSection(&emcyCS_);
    // Initialize high-resolution perf frequency for PDO timing
    LARGE_INTEGER freq; QueryPerformanceFrequency(&freq);
    perfFreqTicks = (uint64_t)freq.QuadPart;
    for (auto& value : lastPdoFilterInputAngles_) {
        value.store(0.0, std::memory_order_relaxed);
    }
    
    // ========================================================================
    // UI Performance Optimization: Initialize double-buffer for lock-free reads
    // ========================================================================
    uiCacheBufferA = std::make_unique<UIDisplayCache>();
    uiCacheBufferB = std::make_unique<UIDisplayCache>();
    uiCacheRead.store(uiCacheBufferA.get(), std::memory_order_release);
    uiCacheWrite.store(uiCacheBufferB.get(), std::memory_order_release);
    uiCacheVersion.store(0, std::memory_order_release);
}

EtherCATMaster::~EtherCATMaster() {
    Shutdown();
    DeleteCriticalSection(&cs);
    DeleteCriticalSection(&pdoDataCS);
    DeleteCriticalSection(&pdoExchangeCS);
    // delete SDO lock
    DeleteCriticalSection(&sdoCS);
    // delete PDO filter lock
    DeleteCriticalSection(&pdoFilterCS_);
    // delete MMF output lock
    DeleteCriticalSection(&mmfOutputCS_);
    DisablePoseMMFOutput();
    DeleteCriticalSection(&poseMmfCS_);
    DeleteCriticalSection(&simulationCS_);
    DeleteCriticalSection(&errorCS_);
    DeleteCriticalSection(&emcyCS_);
}

