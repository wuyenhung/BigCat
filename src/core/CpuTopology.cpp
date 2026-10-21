// ============================================================================
// 立洋科技  CPU Topology implementation
// ============================================================================
#include "../../include/core/CpuTopology.h"
#include <mutex>
#include <sstream>
#include <algorithm>

namespace CoreHelpers {

// ── Static members ──
std::vector<CpuTopology::CoreInfo> CpuTopology::s_cores;
bool CpuTopology::s_populated = false;
int  CpuTopology::s_totalLps  = 0;

static std::mutex s_topoMutex;

// ── EnsurePopulated ──
void CpuTopology::EnsurePopulated()
{
    if (s_populated) return;
    std::lock_guard<std::mutex> lk(s_topoMutex);
    if (s_populated) return;
    Populate();
}

// ── Populate — calls GetLogicalProcessorInformationEx once ──
void CpuTopology::Populate()
{
    s_cores.clear();
    s_totalLps = 0;

    DWORD bufSize = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufSize);
    if (bufSize == 0) {
        s_populated = true;
        return;
    }

    std::vector<BYTE> buffer(bufSize);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bufSize)) {
        s_populated = true;
        return;
    }

    int physIdx = 0;
    BYTE* ptr = buffer.data();
    BYTE* end = ptr + bufSize;

    while (ptr < end) {
        auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
        if (entry->Size == 0) break;

        if (entry->Relationship == RelationProcessorCore) {
            CoreInfo ci;
            ci.physIdx = physIdx;
            ci.efficiencyClass = entry->Processor.EfficiencyClass;

            DWORD_PTR fullMask = 0;
            for (WORD g = 0; g < entry->Processor.GroupCount; ++g)
                fullMask |= entry->Processor.GroupMask[g].Mask;

            // Extract logical CPU indices from mask
            DWORD_PTR m = fullMask;
            int bit = 0;
            while (m) {
                if (m & 1) ci.lps.push_back(bit);
                m >>= 1;
                ++bit;
            }
            s_totalLps += static_cast<int>(ci.lps.size());
            s_cores.push_back(std::move(ci));
            ++physIdx;
        }
        ptr += entry->Size;
    }

    s_populated = true;
}

// ── Simple queries ──
int CpuTopology::PhysicalCoreCount()    { EnsurePopulated(); return static_cast<int>(s_cores.size()); }
int CpuTopology::LogicalProcessorCount(){ EnsurePopulated(); return s_totalLps; }

bool CpuTopology::IsSmtEnabled()
{
    EnsurePopulated();
    for (auto& c : s_cores) if (c.lps.size() >= 2) return true;
    return false;
}

const std::vector<CpuTopology::CoreInfo>& CpuTopology::Cores()
{
    EnsurePopulated();
    return s_cores;
}

// ── Lookup: logical CPU → CoreInfo ──
const CpuTopology::CoreInfo* CpuTopology::FindCoreByLp(int logicalCpu)
{
    EnsurePopulated();
    for (auto& c : s_cores) {
        for (int lp : c.lps) {
            if (lp == logicalCpu) return &c;
        }
    }
    return nullptr;
}

// ── Lookup: physical index → CoreInfo ──
const CpuTopology::CoreInfo* CpuTopology::FindCoreByPhysIdx(int physIdx)
{
    EnsurePopulated();
    for (auto& c : s_cores) {
        if (c.physIdx == physIdx) return &c;
    }
    return nullptr;
}

// ── All primary LPs (first LP of each core) ──
std::vector<int> CpuTopology::GetAllPrimaryLps()
{
    EnsurePopulated();
    std::vector<int> result;
    for (auto& c : s_cores) {
        if (!c.lps.empty()) result.push_back(c.lps[0]);
    }
    return result;
}

// ── Single-bit KAFFINITY mask for the primary LP of a physical core ──
DWORD_PTR CpuTopology::GetPrimaryLpMask(int physIdx)
{
    EnsurePopulated();
    const CoreInfo* ci = FindCoreByPhysIdx(physIdx);
    if (!ci || ci->lps.empty()) return 0;
    return (DWORD_PTR)1 << ci->lps[0];
}

// ── Full KAFFINITY mask for a physical core (all LPs including SMT) ──
DWORD_PTR CpuTopology::GetCoreMask(int physIdx)
{
    EnsurePopulated();
    const CoreInfo* ci = FindCoreByPhysIdx(physIdx);
    if (!ci) return 0;
    DWORD_PTR m = 0;
    for (int lp : ci->lps) m |= ((DWORD_PTR)1 << lp);
    return m;
}

// ── Multi-core KAFFINITY mask from physical core indices ──
DWORD_PTR CpuTopology::BuildMultiCoreMask(const std::vector<int>& physIndices)
{
    EnsurePopulated();
    DWORD_PTR m = 0;
    for (int p : physIndices) {
        const CoreInfo* ci = FindCoreByPhysIdx(p);
        if (ci && !ci->lps.empty()) m |= ((DWORD_PTR)1 << ci->lps[0]);
    }
    return m;
}

// ── SMT siblings of a logical CPU ──
std::vector<int> CpuTopology::GetSmtSiblings(int logicalCpu)
{
    EnsurePopulated();
    const CoreInfo* ci = FindCoreByLp(logicalCpu);
    if (!ci || ci->lps.size() < 2) return {};
    std::vector<int> result;
    for (int lp : ci->lps) {
        if (lp != logicalCpu) result.push_back(lp);
    }
    return result;
}

// ── Find best non-RT LP for IRQ offload ──
int CpuTopology::FindBestNonRtLp(DWORD_PTR rtCoreMask)
{
    EnsurePopulated();
    if (s_cores.empty()) return 0;

    // Collect all LPs that are NOT in the RT core mask
    std::vector<int> candidatesPrimary;
    std::vector<int> candidatesSmt;

    for (auto& c : s_cores) {
        bool coreHasRt = false;
        for (int lp : c.lps) {
            if (rtCoreMask & ((DWORD_PTR)1 << lp)) {
                coreHasRt = true;
                break;
            }
        }
        if (coreHasRt) continue; // skip entire physical core

        for (size_t k = 0; k < c.lps.size(); ++k) {
            int lp = c.lps[k];
            if (k == 0)
                candidatesPrimary.push_back(lp);
            else
                candidatesSmt.push_back(lp);
        }
    }

    // Prefer primary LPs on non-RT cores
    if (!candidatesPrimary.empty())
        return candidatesPrimary[0];

    // Fallback: non-primary SMT threads
    if (!candidatesSmt.empty())
        return candidatesSmt[0];

    // Last resort: logical CPU 0 (system default)
    return 0;
}

// ── Check if two LPs share a physical core ──
bool CpuTopology::SharePhysicalCore(int lpA, int lpB)
{
    if (lpA == lpB) return true;
    const CoreInfo* ciA = FindCoreByLp(lpA);
    const CoreInfo* ciB = FindCoreByLp(lpB);
    return ciA && ciB && (ciA == ciB);
}

// ── Dump table for debugging ──
std::string CpuTopology::DumpTable()
{
    EnsurePopulated();
    std::ostringstream ss;
    ss << "=== CPU Topology ===\n";
    ss << "Physical cores: " << s_cores.size()
       << "  Logical processors: " << s_totalLps << "\n";
    for (auto& c : s_cores) {
        ss << "  Core " << c.physIdx;
        if (c.efficiencyClass == 1)      ss << " [E-core]";
        else if (c.efficiencyClass >= 2) ss << " [P-core]";
        else if (c.lps.size() >= 2)      ss << " [SMT]";
        ss << " :";
        for (size_t k = 0; k < c.lps.size(); ++k) {
            ss << " CPU" << c.lps[k];
            if (k == 0 && c.lps.size() >= 2 && c.efficiencyClass == 0)
                ss << "*";
        }
        ss << "\n";
    }
    ss << "Legend: * = primary thread (Task Manager CPU numbering)\n";
    return ss.str();
}

} // namespace CoreHelpers
