#include "EtherCATMaster.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
#include <limits>
#include <intrin.h>
#include <mmsystem.h>
#include <soem/ec_print.h>
#include <fstream>
#include "SafeMem.h"
#ifdef _WIN32
// SEH-only wrapper to avoid mixing __try with C++ objects in the caller.
static bool TriggerSoftStartWithSEH(PDOFilter& filter)
{
    __try {
        filter.TriggerSoftStart();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif
std::vector<SlaveInfo> EtherCATMaster::GetSlaveInfo() {
    std::vector<SlaveInfo> info;
    
    EnterCriticalSection(&cs);
    
    for (int i = 1; i <= slaveCount; i++) {
        SlaveInfo si;
        si.position = i;
        si.name = context.slavelist[i].name;
        si.aliasAddress = context.slavelist[i].aliasadr;
        si.vendorId = context.slavelist[i].eep_man;
        si.productCode = context.slavelist[i].eep_id;
    // EEPROM revision field (if available)
    si.revision = context.slavelist[i].eep_rev;
    // IP is not typically present in EtherCAT context; leave empty unless assigned elsewhere
    si.ip = std::string();
        si.state = context.slavelist[i].state;
        si.outputBits = context.slavelist[i].Obits;
        si.inputBits = context.slavelist[i].Ibits;
        info.push_back(si);
    }
    
    LeaveCriticalSection(&cs);
    return info;
}

bool EtherCATMaster::AllSlavesOperationalNoAlarm() {
    EnterCriticalSection(&cs);

    if (slaveCount <= 0) {
        LeaveCriticalSection(&cs);
        return false;
    }

    ecx_readstate(&context);

    bool allOk = true;
    for (int i = 1; i <= slaveCount; ++i) {
        uint16_t st = context.slavelist[i].state;
        uint16_t al = context.slavelist[i].ALstatuscode;
        if ((st & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL || al != 0) {
            allOk = false;
            break;
        }
    }

    LeaveCriticalSection(&cs);
    return allOk;
}

std::string EtherCATMaster::GetSlaveEsiFile(int slaveIndex) const
{
    if (slaveIndex < 1 || slaveIndex > (int)slaveEsiFiles_.size()) {
        return std::string();
    }
    return slaveEsiFiles_[slaveIndex];
}

std::string EtherCATMaster::GetSlaveEsiDeviceName(int slaveIndex) const
{
    if (slaveIndex < 1 || slaveIndex > (int)slaveEsiDeviceNames_.size()) {
        return std::string();
    }
    return slaveEsiDeviceNames_[slaveIndex];
}

void EtherCATMaster::SetError(const std::string& error) {
    EnterCriticalSection(&errorCS_);
    lastError = error;
    LeaveCriticalSection(&errorCS_);
}


void EtherCATMaster::SetAutoDisconnectOnNetworkFailure(bool enable, int threshold /*=5*/)
{
    if (threshold <= 0) threshold = 5;
    networkFailureThreshold = threshold;
    autoDisconnectOnNetworkFailure.store(enable);
}

// No-op: accessors forwarded inline from header. Implementation not required.


// SetPDOFilterErrorCallback: removed. PDOFilter no longer supports an error
// callback interface; persistent global error handling is managed by the
// master via state flags and ClearError()/TriggerSoftStart() on the filters.


void EtherCATMaster::SetPDOFilterLimits(int slaveIndex, double minPos, double maxPos)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("SetPDOFilterLimits: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        pdoFilters_[slaveIndex].SetPDOLimits(minPos, maxPos);
    } catch (...) {
        // swallow exceptions
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::SetPDOFilterViolationCallback(int slaveIndex, PDOFilter::ViolationCallback callback)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("SetPDOFilterViolationCallback: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        pdoFilters_[slaveIndex].SetViolationCallback(callback);
    } catch (...) {
        // swallow exceptions to avoid destabilizing master thread
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::SetPDOFilterInputAnomalyCallback(int slaveIndex, PDOFilter::InputAnomalyCallback callback)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("SetPDOFilterInputAnomalyCallback: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        pdoFilters_[slaveIndex].SetInputAnomalyCallback(callback);
    } catch (...) {
        // swallow exceptions to avoid destabilizing master thread
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::ConfigurePDOFilter(int slaveIndex, double maxVel, double maxAccel, double maxDecel, double jerk,
                                        double velocityViolationThresholdScale,
                                        double accelerationViolationThresholdScale,
                                        double velocityViolationDelaySec,
                                        double accelerationViolationDelaySec)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("ConfigurePDOFilter: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        // Configure with master cycle time in microseconds
        uint32_t cycleUs = desiredCycleTime ? desiredCycleTime : 2000;
        pdoFilters_[slaveIndex].Configure(maxVel, maxAccel, maxDecel, jerk, cycleUs);
        pdoFilters_[slaveIndex].SetDynamicViolationConfig(velocityViolationThresholdScale,
                                                          accelerationViolationThresholdScale,
                                                          velocityViolationDelaySec,
                                                          accelerationViolationDelaySec);
    } catch (...) {
        // swallow exceptions
    }
    LeaveCriticalSection(&pdoFilterCS_);
}

/* SetPDOFilterUseFeedback removed: feedback-fusion is not implemented.
   Previously this function toggled whether a slave's PDOFilter used
   actValue for feedback fusion. The API was removed to avoid exposing
   an unimplemented feature. If feedback fusion is reintroduced, add
   the declaration back to the header and restore the implementation. */


void EtherCATMaster::SetPDOFilterTargetDeceleration(int slaveIndex, double targetDecel)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("SetPDOFilterTargetDeceleration: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        // Preserve existing velocity/acceleration limits, only change deceleration target
        double curVel = pdoFilters_[slaveIndex].GetMaxVelocity();
        double curAcc = pdoFilters_[slaveIndex].GetMaxAcceleration();
        // Clamp targetDecel to a reasonable positive value
        if (!std::isfinite(targetDecel) || targetDecel <= 0.0) {
            // If invalid, leave current decel unchanged
            targetDecel = pdoFilters_[slaveIndex].GetMaxDeceleration();
        }
        pdoFilters_[slaveIndex].SetLimitsWithRamp(curVel, curAcc, targetDecel);
    } catch (...) {
        // swallow exceptions to avoid destabilizing master thread
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


bool EtherCATMaster::IsPDOFilterSoftStartActive(int slaveIndex) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return false;
    bool active = false;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    try {
        auto it = pdoFilters_.find(slaveIndex);
        if (it != pdoFilters_.end()) {
            active = it->second.IsSoftStartActive();
        }
    } catch (...) {
        active = false;
    }
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return active;
}


double EtherCATMaster::GetConfiguredPDOFilterMaxDeceleration(int slaveIndex) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return 0.0;
    double val = 0.0;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    try {
        auto it = pdoFilters_.find(slaveIndex);
        if (it != pdoFilters_.end()) {
            val = it->second.GetConfiguredMaxDeceleration();
        }
    } catch (...) {
        val = 0.0;
    }
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return val;
}


double EtherCATMaster::GetPDOFilterSoftStartAccelRatio() const
{
    // Return the soft-start accel ratio constant used by PDOFilter (do not access PDOFilter's private member).
    // Keep the literal in sync with PDOFilter's implementation (default 0.2).
    return 0.2;
}


bool EtherCATMaster::GetPDOFilterViolationThresholds(int slaveIndex, double &velThreshold, double &accelPosThreshold, double &accelNegThreshold) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return false;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    auto it = pdoFilters_.find(slaveIndex);
    if (it == pdoFilters_.end()) { LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_)); return false; }
    const PDOFilter &pf = it->second;
    velThreshold = pf.GetVelocityViolationThreshold();
    accelPosThreshold = pf.GetAccelViolationThresholdPos();
    accelNegThreshold = pf.GetAccelViolationThresholdNeg();
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return true;
}


bool EtherCATMaster::GetPDOFilterTrackingErrorLimit(int slaveIndex, double &trackingErrorLimit) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return false;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    auto it = pdoFilters_.find(slaveIndex);
    if (it == pdoFilters_.end()) { LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_)); return false; }
    trackingErrorLimit = it->second.GetTrackingErrorLimit();
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return true;
}


bool EtherCATMaster::GetPDOFilterState(int slaveIndex, PDOFilter::ViolationType &violationType, bool &isError, bool &isTrackingErrorFatal) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return false;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    auto it = pdoFilters_.find(slaveIndex);
    if (it == pdoFilters_.end()) {
        LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
        return false;
    }
    const PDOFilter &pf = it->second;
    violationType = pf.GetCurrentViolationType();
    isError = false;
    isTrackingErrorFatal = false;
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return true;
}


bool EtherCATMaster::GetPDOLimits(int slaveIndex, double &minPos, double &maxPos) const
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return false;
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    auto it = pdoFilters_.find(slaveIndex);
    if (it == pdoFilters_.end()) { LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_)); return false; }
    const PDOFilter &pf = it->second;
    auto limits = pf.GetPDOLimits();
    minPos = limits.first;
    maxPos = limits.second;
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return true;
}


bool EtherCATMaster::WaitForPDOFiltersToSettle(double velThreshold, double timeoutSec)
{
    if (timeoutSec <= 0.0) timeoutSec = 0.0;
    const int pollMs = 10;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        bool allBelow = true;
        EnterCriticalSection(&pdoFilterCS_);
        try {
            for (const auto &kv : pdoFilters_) {
                const PDOFilter &f = kv.second;
                double v = std::abs(f.GetCurrentVelocity());
                if (v > velThreshold) { allBelow = false; break; }
            }
        } catch (...) {
            // on any exception, break and treat as not settled
            allBelow = false;
        }
        LeaveCriticalSection(&pdoFilterCS_);

        if (allBelow) return true;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(now - start).count();
        if (elapsed >= timeoutSec) return false;
        // Sleep a short while and try again
        Sleep(pollMs);
    }
    return false;
}

// SetPDOFilterSoftStartTime removed — soft-start duration no longer configurable.


void EtherCATMaster::TriggerPDOFilterSoftStart(int slaveIndex)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("TriggerPDOFilterSoftStart: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        pdoFilters_[slaveIndex].TriggerSoftStart();
    } catch (...) {
        // swallow exceptions
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::TriggerAllPDOFiltersSoftStart()
{
    // Safer implementation: ensure entries exist for all configured slaves
    // and invoke TriggerSoftStart() per-slave under SEH/C++ guards. This
    // prevents iterator invalidation or unhandled structured exceptions
    // from crashing the process when GUI triggers a global soft-start.
    //
    // A new soft-start must not inherit a previous CSV velocity command.  The
    // first filter cycle intentionally skips writing while it initializes from
    // actual feedback; without clearing here, a stale staged 0x60FF can remain
    // active until the second cycle and can push an axis away from a zero target.
    // Moved inside pdoFilterCS_ below so the per-cycle CSP sync in the PDO task
    // cannot overwrite the freshly-synced 0x607A with wrong offsets before
    // filtering is enabled.
    // On real hardware the cyclic task handles PDO exchange; calling
    // ecx_send_processdata from this (auto-connect) thread races with
    // the cyclic task and can cause WKC=0 / frame corruption.
    // In simulation mode SendProcessDataOnce is needed to advance the
    // simulation engine, and there is no concurrent cyclic thread.
    if (IsRunning() && simulationMode_.load(std::memory_order_relaxed)) {
        SendProcessDataOnce();
    }

    EnterCriticalSection(&pdoFilterCS_);

    // Clear stale staged commands and sync 0x607A ← 0x6064 (CSP) / clear
    // 0x60FF (CSV) inside the CS so the PDOProcessingTask cannot run its
    // per-cycle CSP sync (which may use wrong offsets) between the clear
    // and the filter-enable below.
    ClearStagedOutputsForCyclicMotionCommands();

    // Enable PDO filtering atomically with soft-start so the
    // PDOProcessingTask sees a consistent state on the very next
    // cycle: filtering is on AND every filter has soft-start
    // active with ioInitialized_=false (will seed from actual
    // feedback).  Without this, the window between a separate
    // EnablePDOFiltering(true) and TriggerAllPDOFiltersSoftStart()
    // allows the cyclic task to process MMF targets at full
    // velocity/acceleration limits before soft-start protection is
    // in place.
    pdoFilteringEnabled_.store(true, std::memory_order_release);

    // Clear the lock-free ready cache BEFORE triggering soft-start on each
    // filter. This eliminates the race condition where the polling thread
    // reads stale "ready=true" values from a previous run before the
    // cyclic task has a chance to call Filter() and update the cache.
    if (softStartReadyCache_) {
        for (int i = 0; i < softStartReadyCacheSize_; ++i) {
            softStartReadyCache_[i].store(0, std::memory_order_relaxed);
        }
    }

    // Make sure a PDOFilter instance exists for every slave index
    for (int si = 1; si <= slaveCount; ++si) {
        if (pdoFilters_.find(si) == pdoFilters_.end()) {
            pdoFilters_[si] = PDOFilter();
        }
    }

    // Trigger soft-start per slave with separate guards so one failing
    // slave doesn't bring down the whole operation.
    // Use SEH wrapper to isolate faults per slave

    for (int si = 1; si <= slaveCount; ++si) {
#ifdef _WIN32
        try {
            if (!TriggerSoftStartWithSEH(pdoFilters_[si])) {
                std::ostringstream oss; oss << "SEH exception during TriggerSoftStart for slave " << si;
                SetError(oss.str());
            }
        } catch (const std::exception &ex) {
            std::ostringstream oss; oss << "Exception during TriggerSoftStart for slave " << si << ": " << ex.what();
            SetError(oss.str());
        } catch (...) {
            std::ostringstream oss; oss << "Unknown exception during TriggerSoftStart for slave " << si;
            SetError(oss.str());
        }
#else
        try {
            pdoFilters_[si].TriggerSoftStart();
        } catch (const std::exception &ex) {
            std::ostringstream oss; oss << "Exception during TriggerSoftStart for slave " << si << ": " << ex.what();
            SetError(oss.str());
        } catch (...) {
            std::ostringstream oss; oss << "Unknown exception during TriggerSoftStart for slave " << si;
            SetError(oss.str());
        }
#endif
    }
    // Pose6D 區塊使用此 lock-free flag 在 cyclic 路徑中停用動態縮放，
    // 避免進入 CS。在此立即設為 true，確保第一個 cycle 就能看到。
    pose6dAnySoftStartActive_.store(true, std::memory_order_relaxed);

    // Clear powerOnInProgress flag inside the critical section so the
    // PDOProcessingTask, which enters pdoFilterCS_ after seeing
    // pdoFilteringEnabled_==true, never observes powerOnInProgress_==true
    // on the first cycle.  If the flag is cleared outside the CS, the
    // first filter cycle skips writing CSV commands (skipWrite=true),
    // the filter advances its internal state while the servo stands still,
    // and the second cycle issues a catch-up velocity that causes a
    // perceptible "bump" at the start of soft-start.
    powerOnInProgress_.store(false, std::memory_order_release);

    LeaveCriticalSection(&pdoFilterCS_);
}


bool EtherCATMaster::IsPDOFilterSoftStartReady(int slaveIndex)
{
    if (slaveIndex <= 0) return false;
    EnterCriticalSection(&pdoFilterCS_);
    bool ready = false;
    auto it = pdoFilters_.find(slaveIndex);
    if (it != pdoFilters_.end()) {
        try {
            ready = it->second.IsSoftStartReady();
        } catch (...) {
            ready = false;
        }
    }
    LeaveCriticalSection(&pdoFilterCS_);
    return ready;
}


bool EtherCATMaster::AreAllPDOFiltersSoftStartReady()
{
    EnterCriticalSection(&pdoFilterCS_);
    bool allReady = true;
    try {
        for (auto &p : pdoFilters_) {
            if (!p.second.IsSoftStartReady()) { allReady = false; break; }
        }
    } catch (...) {
        allReady = false;
    }
    LeaveCriticalSection(&pdoFilterCS_);
    return allReady;
}


void EtherCATMaster::CompleteAllPDOFiltersSoftStart()
{
    EnterCriticalSection(&pdoFilterCS_);
    try {
        for (auto &p : pdoFilters_) {
            p.second.CompleteSoftStart();
        }
    } catch (...) {
        // swallow
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::RequestCompleteAllPDOFiltersSoftStart()
{
    completeSoftStartRequested_.store(true, std::memory_order_release);
}


void EtherCATMaster::HoldAllPDOFiltersPosition()
{
    EnterCriticalSection(&pdoFilterCS_);
    for (auto &pair : pdoFilters_) {
        pair.second.HoldCurrentPosition();
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


void EtherCATMaster::HoldPDOFilterPosition(int slaveIndex)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return;
    EnterCriticalSection(&pdoFilterCS_);
    auto it = pdoFilters_.find(slaveIndex);
    if (it != pdoFilters_.end()) {
        it->second.HoldCurrentPosition();
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


double EtherCATMaster::GetMaxFilterVelocity()
{
    double maxVel = 0.0;
    EnterCriticalSection(&pdoFilterCS_);
    for (const auto &kv : pdoFilters_) {
        double v = std::abs(kv.second.GetCurrentVelocity());
        if (v > maxVel) maxVel = v;
    }
    LeaveCriticalSection(&pdoFilterCS_);
    return maxVel;
}


double EtherCATMaster::GetMinConfiguredMaxDeceleration()
{
    double minDecel = (std::numeric_limits<double>::max)();
    bool found = false;
    EnterCriticalSection(&pdoFilterCS_);
    for (const auto &kv : pdoFilters_) {
        double d = kv.second.GetConfiguredMaxDeceleration();
        if (d > 0.0 && d < minDecel) {
            minDecel = d;
            found = true;
        }
    }
    LeaveCriticalSection(&pdoFilterCS_);
    return found ? minDecel : 200.0;  // fallback 200 deg/s²
}


void EtherCATMaster::ReleaseAllPDOFiltersHoldPosition()
{
    EnterCriticalSection(&pdoFilterCS_);
    for (auto &pair : pdoFilters_) {
        pair.second.ReleaseHoldPosition();
    }
    LeaveCriticalSection(&pdoFilterCS_);
}


bool EtherCATMaster::IsPDOFilterSoftStartReady_Cached(int slaveIndex) const
{
    if (slaveIndex <= 0) return false;
    if (!softStartReadyCache_) return false;
    if (softStartReadyCacheSize_ <= slaveIndex) return false;
    return softStartReadyCache_[slaveIndex].load(std::memory_order_relaxed) != 0;
}


bool EtherCATMaster::AreAllPDOFiltersSoftStartReady_Cached() const
{
    // If cache uninitialized, fall back to false
    if (!softStartReadyCache_) return false;
    for (int i = 1; i < softStartReadyCacheSize_; ++i) {
        if (softStartReadyCache_[i].load(std::memory_order_relaxed) == 0) return false;
    }
    return true;
}


void EtherCATMaster::ResetPDOFilter(int slaveIndex)
{
    if (slaveIndex <= 0 || slaveIndex > slaveCount) {
        SetError("ResetPDOFilter: Invalid slave index");
        return;
    }

    EnterCriticalSection(&pdoFilterCS_);
    if (pdoFilters_.find(slaveIndex) == pdoFilters_.end()) {
        pdoFilters_[slaveIndex] = PDOFilter();
    }
    try {
        pdoFilters_[slaveIndex].Reset();
    } catch(...) {
        // swallow exceptions to avoid destabilizing master thread
    }
    LeaveCriticalSection(&pdoFilterCS_);
}

// === 全局違規狀態管理 ===


bool EtherCATMaster::IsAnySlaveViolating() const
{
    // 檢查是否有任何從站正在違規
    // NOTE: previously this checked IsLimitingEnabled(), but that can be set
    //       by a global forcing call (SetGlobalLimitingEnabled) and thus
    //       create a self-sustaining state where the master never clears
    //       global limiting. Check the actual violation type instead.
    // 注意：此方法曾要求呼叫者持有 pdoFilterCS_ 鎖，但為了讓呼叫方（例如 UI）能安全使用，
    // 在函式內部自動加鎖以避免競態。
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    bool any = false;
    for (const auto& pair : pdoFilters_) {
        const PDOFilter &f = pair.second;
        if (f.GetCurrentViolationType() != PDOFilter::ViolationType::None) {
            any = true;
            break;
        }
    }
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&pdoFilterCS_));
    return any;
}


void EtherCATMaster::UpdateGlobalViolationState()
{
    bool anyViolating = IsAnySlaveViolating();

    if (anyViolating) {
        // Collect violating slaves and detect whether any violation is dynamic
        std::vector<int> violatingSlaves;
        bool anyDynamicViolation = false;
        for (const auto &kv : pdoFilters_) {
            int idx = kv.first;
            const PDOFilter &pf = kv.second;
            auto vType = pf.GetCurrentViolationType();
            if (vType != PDOFilter::ViolationType::None) {
                violatingSlaves.push_back(idx);
                if (PDOFilter::HasFlag(vType, PDOFilter::ViolationType::Velocity) ||
                    PDOFilter::HasFlag(vType, PDOFilter::ViolationType::Acceleration) ||
                    PDOFilter::HasFlag(vType, PDOFilter::ViolationType::Deceleration)) {
                    anyDynamicViolation = true;
                }
            }
        }

        if (anyDynamicViolation) {
            globalLimitingEnabled_.store(true, std::memory_order_release);
            for (auto &pair : pdoFilters_) {
                pair.second.SetGlobalLimitingEnabled(true);
            }
        } else {
            // Only hard-limit violations: enable limiting only for the violating slaves
            globalLimitingEnabled_.store(false, std::memory_order_release);
            for (int idx : violatingSlaves) {
                try {
                    pdoFilters_.at(idx).SetGlobalLimitingEnabled(true);
                } catch (...) {}
            }
        }
    } else {
        // No violations: clear limiting immediately
        if (globalLimitingEnabled_.load(std::memory_order_acquire)) {
            globalLimitingEnabled_.store(false, std::memory_order_release);
            for (auto& pair : pdoFilters_) {
                pair.second.SetGlobalLimitingEnabled(false);
            }
        }
    }
}



