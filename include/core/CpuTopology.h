// ============================================================================
// 立洋科技  CPU Topology — unified abstraction over GetLogicalProcessorInformationEx
//
// Provides a single cache of logical CPU ↔ physical core ↔ SMT thread mappings.
// All IRQ affinity, thread affinity, and CPU isolation decisions should query
// through this layer instead of hardcoding core numbers.
//
// Dynamic — adapts to AMD / Intel / hybrid / HT-disabled without recompilation.
// ============================================================================
#pragma once

#include <windows.h>
#include <vector>
#include <string>
#include <cstdint>

namespace CoreHelpers {

struct CpuTopology {
    // ── Per-physical-core record ──
    struct CoreInfo {
        int  physIdx;               // 0-based physical core index (OS enumeration order)
        int  efficiencyClass;       // 0=uniform, 1=E-core, 2+=P-core (or other)
        std::vector<int> lps;       // logical CPU indices (0-based, Task Manager numbering)
        bool isHybrid() const { return efficiencyClass != 0; }
        int  primaryLp() const { return lps.empty() ? -1 : lps[0]; }
        bool hasSmt()    const { return lps.size() >= 2; }
    };

    // ── Ensure topology is populated (idempotent, thread-safe) ──
    static void EnsurePopulated();

    // ── Queries ──
    static int  PhysicalCoreCount();
    static int  LogicalProcessorCount();
    static bool IsSmtEnabled();          // any core has ≥2 LPs

    // Look up which physical core owns a given logical CPU
    static const CoreInfo* FindCoreByLp(int logicalCpu);

    // Look up a physical core by its enumeration index
    static const CoreInfo* FindCoreByPhysIdx(int physIdx);

    // Get all primary logical processors (one per physical core)
    static std::vector<int> GetAllPrimaryLps();

    // Get single-bit KAFFINITY mask for the primary LP of a physical core.
    // Returns 0 if physIdx not found.
    static DWORD_PTR GetPrimaryLpMask(int physIdx);

    // Get full KAFFINITY mask for a physical core (all LPs including SMT siblings).
    // Returns 0 if physIdx not found.
    static DWORD_PTR GetCoreMask(int physIdx);

    // Build multi-core KAFFINITY mask from a list of physical core indices.
    // Uses primary LP of each core only (never SMT siblings).
    static DWORD_PTR BuildMultiCoreMask(const std::vector<int>& physIndices);

    // Get SMT siblings of a logical CPU (excluding itself); empty if no SMT
    static std::vector<int> GetSmtSiblings(int logicalCpu);

    // Find a non-RT logical CPU suitable for IRQ offload:
    //   - Not on the RT physical core (including HT siblings)
    //   - Prefers primary LPs over HT siblings
    //   - Prefers non-hybrid cores if available
    //   - Falls back to logical CPU 0 if no alternative
    static int  FindBestNonRtLp(DWORD_PTR rtCoreMask);

    // Check whether two logical CPUs share a physical core
    static bool SharePhysicalCore(int lpA, int lpB);

    // ── Display (for debugging/logging) ──
    static std::string DumpTable();

    // ── Raw cached data ──
    static const std::vector<CoreInfo>& Cores();

private:
    static void Populate();
    static std::vector<CoreInfo> s_cores;
    static bool s_populated;
    static int  s_totalLps;
};

} // namespace CoreHelpers
