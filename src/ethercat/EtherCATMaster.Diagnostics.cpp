#include "EtherCATMaster.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
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
    lastError = error;
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
    isError = pf.IsError();
    isTrackingErrorFatal = pf.IsTrackingErrorFatal();
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
    ClearStagedOutputsForCyclicMotionCommands();
    // On real hardware the cyclic task handles PDO exchange; calling
    // ecx_send_processdata from this (auto-connect) thread races with
    // the cyclic task and can cause WKC=0 / frame corruption.
    // In simulation mode SendProcessDataOnce is needed to advance the
    // simulation engine, and there is no concurrent cyclic thread.
    if (IsRunning() && simulationMode_.load(std::memory_order_relaxed)) {
        SendProcessDataOnce();
    }

    EnterCriticalSection(&pdoFilterCS_);

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
    LeaveCriticalSection(&pdoFilterCS_);

    // Clear powerOnInProgress flag now that soft-start is active.
    // The soft-start's IsSoftStartActive() will continue to block CyclicTask writes.
    powerOnInProgress_.store(false, std::memory_order_release);
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
        // Ensure filter error flag is cleared and state is reset to avoid
        // persistent error conditions after a successful fault-reset.
        pdoFilters_[slaveIndex].ClearError();
        // Also reset the main state so next Filter() initializes from actual position
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
    // 在 critical section 內呼叫此函式
    // 使用 PDOFilter 的恢復延遲常數
    static constexpr double RECOVERY_DELAY_SEC = 3.0;  // 與 PDOFilter 一致
    
    // 檢查是否有任何從站正在違規
    bool anyViolating = IsAnySlaveViolating();
    auto now = std::chrono::steady_clock::now();
    
    if (anyViolating) {
        // 有從站違規：設定全局違規狀態，重置無違規計時器
        // NOTE: If the only violations present are HardLimitMin/HardLimitMax,
        // do NOT force global limiting onto all non-violating slaves. Only
        // apply global limiting when there is a dynamic violation (velocity/
        // acceleration/deceleration), which indicates a system-wide safety risk.
        globalViolationActive_.store(true, std::memory_order_release);
        globalNoViolationTimerActive_ = false;  // 停止計時器

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
            // Dynamic violation: keep previous behaviour and force-limiting on all slaves
            globalLimitingEnabled_.store(true, std::memory_order_release);
            for (auto &pair : pdoFilters_) {
                pair.second.SetGlobalLimitingEnabled(true);
            }
            // Debug log removed for production
        } else {
            // Only hard-limit violations: enable limiting only for the violating slaves
            globalLimitingEnabled_.store(false, std::memory_order_release);
            for (int idx : violatingSlaves) {
                try {
                    pdoFilters_.at(idx).SetGlobalLimitingEnabled(true);
                } catch (...) {}
            }
            // Debug log removed for production
        }
    } else {
        // 所有從站都沒有違規
        if (globalViolationActive_.load(std::memory_order_acquire)) {
            // The global violation latch remains debounced below, but the
            // physical per-axis limiting must start recovering as soon as no
            // active violation remains. Holding the 20% accel/decel reduction
            // for the full debounce window makes otherwise-clean axes unable
            // to track CSV targets, which can create a second wave of
            // TrackingError violations before the global latch clears.
            // 之前有全局違規，開始計時恢復
            if (!globalNoViolationTimerActive_) {
                globalLimitingEnabled_.store(false, std::memory_order_release);
                for (auto& pair : pdoFilters_) {
                    pair.second.SetGlobalLimitingEnabled(false);
                }

                // 首次進入無違規狀態，記錄時間
                globalNoViolationStartTime_ = now;
                globalNoViolationTimerActive_ = true;
            } else {
                // 檢查是否已經無違規超過 RECOVERY_DELAY_SEC
                auto elapsed = std::chrono::duration<double>(now - globalNoViolationStartTime_).count();
                if (elapsed >= RECOVERY_DELAY_SEC) {
                    // 恢復正常：清除全局違規狀態
                    globalViolationActive_.store(false, std::memory_order_release);
                    globalNoViolationTimerActive_ = false;
                }
            }
        }
    }
}



