// EtherCATMaster.cpp - 簡要說明:
// EtherCAT 主站核心實作，包含通訊初始化、即時執行緒、PDO/SDO 交換與狀態管理。

#include "EtherCATMaster.h"
// #include "core/CpuIsolation.h" - removed as unused
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

namespace {
    // Cached list of candidate logical-processor masks (one representative bit per physical core)
    std::vector<DWORD_PTR> g_coreCandidates;
    std::mutex g_coreCandidatesMutex;
    std::atomic<size_t> g_coreNextIndex(0);

    // Core reserved for the CyclicTask – must match CpuIsolation TARGET_ETHERCAT_CORE.
    // All other threads allocated via round-robin must avoid this core.
    constexpr int    kReservedCore     = 4;
    constexpr DWORD_PTR kReservedCoreMask = (DWORD_PTR)1 << kReservedCore;
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

    // Allow user override via environment variables:
    //  - WUSOEM_AFFINITY_MASK: hex or decimal mask to further restrict allowed logical processors (e.g. 0x3)
    //  - WUSOEM_MAX_CORES: decimal max number of physical-core candidates to use (e.g. 2)
    // Query physical core groups and pick a single logical processor per core
    DWORD len = 0;
    BOOL ok = GetLogicalProcessorInformation(NULL, &len);
    if (!ok && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
        PSYSTEM_LOGICAL_PROCESSOR_INFORMATION info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
        if (info) {
            if (GetLogicalProcessorInformation(info, &len)) {
                DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
                for (DWORD i = 0; i < count; ++i) {
                    if (info[i].Relationship == RelationProcessorCore) {
                        DWORD_PTR mask = info[i].ProcessorMask & available;
                        if (mask) {
                            // pick lowest set bit within this core mask
                            DWORD_PTR pick = mask & (~mask + 1);
                            // Skip the reserved EtherCAT core – it is exclusively
                            // used by the CyclicTask and protected by CpuIsolation.
                            if (pick == kReservedCoreMask) continue;
                            g_coreCandidates.push_back(pick);
                        }
                    }
                }
            }
            free(info);
        }
    }
    // No external env overrides: allocator will auto-limit candidates below based on available cores
    // Fallback: if no info or none matched, pick lowest bits from available
    if (g_coreCandidates.empty()) {
        DWORD_PTR m = available;
        while (m) {
            DWORD_PTR pick = m & (~m + 1);
            if (pick != kReservedCoreMask)
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
    std::stringstream ss; ss << "AutoTuneAffinity: triggered (maxObs=" << maxObs << "us)";
    SetError(ss.str());
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
    // initialize SDO lock
    InitializeCriticalSection(&sdoCS);
    // Initialize PDO filter lock
    InitializeCriticalSection(&pdoFilterCS_);
    // Initialize MMF output lock
    InitializeCriticalSection(&mmfOutputCS_);
    InitializeCriticalSection(&simulationCS_);
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
    // delete SDO lock
    DeleteCriticalSection(&sdoCS);
    // delete PDO filter lock
    DeleteCriticalSection(&pdoFilterCS_);
    // delete MMF output lock
    DeleteCriticalSection(&mmfOutputCS_);
    DeleteCriticalSection(&simulationCS_);
}

