#include "PDOFilter.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Evaluate trapezoidal profile position at time t.
static double eval_trapezoid_pos(double t, double tacc, double tflat, double tdec, double vp, double dir, double startPos) {
    // return position (scalar) relative to start (signed by dir)
    if (t <= 0.0) return startPos;
    double pos = 0.0;
    if (t < tacc) {
        // accel phase: 0.5 * a * t^2, where a = vp / tacc
        double a = vp / tacc;
        pos = 0.5 * a * t * t;
    } else if (t < (tacc + tflat)) {
        // accel full then constant vel
        double a = vp / tacc;
        double distAcc = 0.5 * a * tacc * tacc;
        double dt = t - tacc;
        pos = distAcc + vp * dt;
    } else if (t < (tacc + tflat + tdec)) {
        double a = vp / tacc;
        double distAcc = 0.5 * a * tacc * tacc;
        double distFlat = vp * tflat;
        double dt = t - tacc - tflat;
        // decel phase: vp*dt - 0.5*a*dt^2
        pos = distAcc + distFlat + (vp * dt - 0.5 * a * dt * dt);
    } else {
        // end
        double a = vp / tacc;
        double distAcc = 0.5 * a * tacc * tacc;
        double distFlat = vp * tflat;
        double distDec = 0.5 * a * tdec * tdec; // should equal distAcc
        pos = distAcc + distFlat + distDec;
    }
    return startPos + dir * pos;
}

static double sanitize_positive_limit(double value) {
    return (std::isfinite(value) && value > 0.0) ? std::abs(value) : 0.0;
}

static double sanitize_positive_limit_or(double value, double fallback) {
    double sanitized = sanitize_positive_limit(value);
    return (sanitized > 0.0) ? sanitized : sanitize_positive_limit(fallback);
}

static bool has_valid_motion_limits(double maxVel, double maxAccel, double maxDecel, double cycleTime) {
    return std::isfinite(maxVel) && maxVel > 0.0 &&
           std::isfinite(maxAccel) && maxAccel > 0.0 &&
           std::isfinite(maxDecel) && maxDecel > 0.0 &&
           std::isfinite(cycleTime) && cycleTime > 0.0;
}

PDOFilter::PDOFilter()
    : ioHoldingPosition_(false)
    , iMaxVelocity_(0.0)
    , iMaxAcceleration_(0.0)
    , iMaxDeceleration_(0.0)
    , iJerk_(0.0)
    , iCycleTime_(0.002)
    , oFilteredOutput_(0.0)
    , oCurrentVelocity_(0.0)
    , oCurrentAcceleration_(0.0)
    , oOutputVelocity_(0.0)
    , oOutputAcceleration_(0.0)
    , ioPrevFilteredOutput_(0.0)
    , ioPrevOutputVelocity_(0.0)
    
    , ioInitialized_(false)
    , ioPrevActValue_(0.0)
    , iMinPDO_(-std::numeric_limits<double>::max())
    , iMaxPDO_(std::numeric_limits<double>::max())
    , ioOriginalMinPDO_(-std::numeric_limits<double>::max())
    , ioOriginalMaxPDO_(std::numeric_limits<double>::max())
    , ioSoftStartLimitsAdjusted_(false)
    , ioSoftStartElapsedSec_(0.0)
    , ioSoftStartActive_(false)
    , ioPositionCloseStartTime_(0.0)
    , ioPositionWasClose_(false)
    , ioSoftStartReady_(false)
    , ioSoftStartCompleteTime_(std::chrono::steady_clock::time_point())
    , ioVelocityBlendActive_(false)
    , ioAccelRampActive_(false)
    , ioAccelRampStartTime_(std::chrono::steady_clock::time_point())
    , ioAccelRampDurationSec_(3.0)
    , ioAccelRampStartVel_(0.0)
    , ioAccelRampStartAcc_(0.0)
    , ioAccelRampStartDec_(0.0)
    , iPositionGain_(70.0) // Increase Gain to reduce tracking lag (stiffer response)
    , iPositionDeadband_(3e-5) // >= 1 PDO count for Delta (kDsP100E7Scale ≈ 2.75e-5); prevents quantization limit cycle in CSP mode
    , iFeedforwardGain_(0.7)  // Reduce feedforward to avoid overshoot
    , ioPrevSetValue_(0.0)
    , ioPrevEffectiveSetpoint_(0.0)
    , ioSoftStartCommandVelocity_(0.0)
    , ioLastPredictedSetpoint_(0.0)
    , ioPredictedThisCycle_(false)
    , ioDebugLogCounter_(0)
    , ioStaleCounter_(0)
    , ioStaleViolationLatched_(false)
    , ioInputAnomalyCallback_(nullptr)
    , ioInputAnomalyActive_(false)
    , ioLastInputAnomalyReason_(InputAnomalyReason::Unknown)
    , ioStaleViolationStartTime_(0.0)
    , ioAnomalyViolationStartTime_(0.0)
{
    // initialize flags not covered by initializer list
    ioMeasuredVelocityProvided_ = false;
    ioPreviousCyclePredicted_ = false;
    ioGlobalLimitingForced_ = false;
    ioTrackingClampDirection_ = 0;
    // Initialize trapezoid profile state.
    profileTargetPos_ = 0.0;
    profileActive_ = false;
    profileStartTime_ = std::chrono::steady_clock::time_point();
    profileStartPos_ = 0.0;
    profilePeakVel_ = 0.0;
    profileTacc_ = 0.0;
    profileTflat_ = 0.0;
    profileTdec_ = 0.0;
    profileDuration_ = 0.0;
    profileDir_ = 1;
    
    // Cache configured limits for later restoration.
    iConfiguredMaxVelocity_ = iMaxVelocity_;
    iConfiguredMaxAcceleration_ = iMaxAcceleration_;
    iConfiguredMaxDeceleration_ = iMaxDeceleration_;
    
    // Initialize violation-tracking state.
    ioViolationStartTime_ = 0.0;
    ioAccelViolationStartTime_ = 0.0;
    ioVelocityViolationStartTime_ = 0.0;
    ioNoViolationStartTime_ = 0.0;
    ioViolationClearStartTime_ = 0.0;
    ioViolationActive_ = false;
    ioLimitingEnabled_ = false;
    ioFilterStartTimeInitialized_ = false;
    
    ioCurrentViolationType_ = ViolationType::None;
    ioPrevViolationType_ = ViolationType::None;
    ioLastRawVelocity_ = 0.0;
    ioLastRawAcceleration_ = 0.0;
    ioPrevLimitingEnabled_ = false;
    ioAccelCheckSkipCount_ = 0;
    ioViolationDetectionSuspended_ = false;
    ioViolationCallback_ = nullptr;
    
    ioVelocityBufferIndex_ = 0;
    ioVelocityBufferCount_ = 0;
    ioPrevSmoothedVelocity_ = 0.0;
    
    ioExtrapolationVelocityBuffer_.fill(0.0);
    ioExtrapolationVelocityIndex_ = 0;
    ioExtrapolationVelocityCount_ = 0;
    
    ioInputBuffer_.fill(0.0);
    ioInputBufferIndex_ = 0;
    ioInputBufferCount_ = 0;
    
    

    // initialize violation threshold scales
    ioAccelThresholdScale_ = PDOFilter::DEFAULT_ACCEL_VIOLATION_THRESHOLD_SCALE;
    ioVelocityThresholdScale_ = PDOFilter::DEFAULT_VELOCITY_VIOLATION_THRESHOLD_SCALE;
    ioAccelViolationDelaySec_ = PDOFilter::DEFAULT_ACCEL_VIOLATION_DELAY_SEC;
    ioVelocityViolationDelaySec_ = PDOFilter::DEFAULT_VELOCITY_VIOLATION_DELAY_SEC;
}

void PDOFilter::Reset() {
    oFilteredOutput_ = 0.0;
    oCurrentVelocity_ = 0.0;
    oCurrentAcceleration_ = 0.0;
    oOutputVelocity_ = 0.0;
    oOutputAcceleration_ = 0.0;
    ioPrevFilteredOutput_ = 0.0;
    ioPrevOutputVelocity_ = 0.0;
    ioInitialized_ = false;
    ioSoftStartElapsedSec_ = 0.0;
    ioSoftStartActive_ = false;
    ioSoftStartEverTriggered_ = false;
    ioSoftStartReady_ = false;
    ioSoftStartCompleteTime_ = std::chrono::steady_clock::time_point();
    ioPositionWasClose_ = false;
    ioPositionCloseStartTime_ = 0.0;
    ioVelocityBlendActive_ = false;
    ioSoftStartStartTime_ = std::chrono::steady_clock::time_point();
    profileActive_ = false;
    profileTargetPos_ = 0.0;
    ioPrevSetValue_ = 0.0;
    ioPrevEffectiveSetpoint_ = 0.0;
    ioSoftStartCommandVelocity_ = 0.0;
    ioHoldingPosition_ = false;
    ioViolationStartTime_ = 0.0;
    ioAccelViolationStartTime_ = 0.0;
    ioVelocityViolationStartTime_ = 0.0;
    ioNoViolationStartTime_ = 0.0;
    ioViolationClearStartTime_ = 0.0;
    ioViolationActive_ = false;
    ioLimitingEnabled_ = false;
    ioFilterStartTimeInitialized_ = false;
    ioViolationDetectionSuspended_ = false;
    ioCurrentViolationType_ = ViolationType::None;
    ioPrevViolationType_ = ViolationType::None;
    ioLastRawVelocity_ = 0.0;
    ioLastRawAcceleration_ = 0.0;
    ioPrevLimitingEnabled_ = false;
    ioAccelCheckSkipCount_ = 0;
    ioVelocityBuffer_.fill(0.0);
    ioVelocityBufferIndex_ = 0;
    ioVelocityBufferCount_ = 0;
    ioPrevSmoothedVelocity_ = 0.0;
    ioInputBuffer_.fill(0.0);
    ioInputBufferIndex_ = 0;
    ioInputBufferCount_ = 0;
    ioExtrapolationVelocityIndex_ = 0;
    ioExtrapolationVelocityCount_ = 0;
    ioLastPredictedSetpoint_ = 0.0;
    ioPredictedThisCycle_ = false;
    
    ioDebugLogCounter_ = 0;
    ioStaleCounter_ = 0;
    ioStaleViolationLatched_ = false;
    ioInputAnomalyActive_ = false;
    ioLastInputAnomalyReason_ = InputAnomalyReason::Unknown;
    ioStaleViolationStartTime_ = 0.0;
    ioAnomalyViolationStartTime_ = 0.0;
    ioMeasuredVelocityProvided_ = false;
    ioPreviousCyclePredicted_ = false;
    ioGlobalLimitingForced_ = false;
    ioTrackingClampDirection_ = 0;
    ioAccelRampActive_ = false;
    ioAccelRampStartTime_ = std::chrono::steady_clock::time_point();
    ioAccelRampStartVel_ = 0.0;
    ioAccelRampStartAcc_ = 0.0;
    ioAccelRampStartDec_ = 0.0;
}

void PDOFilter::Configure(double iMaxVelocity, double iMaxAcceleration, 
                         double iMaxDeceleration, double iJerk,
                         uint32_t iCycleTimeUs) {
    iMaxVelocity_ = sanitize_positive_limit(iMaxVelocity);
    iMaxAcceleration_ = sanitize_positive_limit(iMaxAcceleration);
    iMaxDeceleration_ = sanitize_positive_limit(iMaxDeceleration);
    iJerk_ = sanitize_positive_limit(iJerk);
    iConfiguredMaxVelocity_ = iMaxVelocity_;
    iConfiguredMaxAcceleration_ = iMaxAcceleration_;
    iConfiguredMaxDeceleration_ = iMaxDeceleration_;
    if (iCycleTimeUs > 0) {
        iCycleTime_ = iCycleTimeUs / 1000000.0;
    } else {
        iCycleTime_ = 0.002; 
    }
    
}

void PDOFilter::SetPDOLimits(double iMin, double iMax) {
    iMinPDO_ = iMin; iMaxPDO_ = iMax;
}

void PDOFilter::SetLimitsWithRamp(double maxVel, double maxAccel, double maxDecel) {
    iMaxVelocity_ = sanitize_positive_limit_or(maxVel, iMaxVelocity_);
    iMaxAcceleration_ = sanitize_positive_limit_or(maxAccel, iMaxAcceleration_);
    iMaxDeceleration_ = sanitize_positive_limit_or(maxDecel, iMaxDeceleration_);
}

void PDOFilter::TriggerSoftStart() {
    ioSoftStartElapsedSec_ = 0.0;
    ioSoftStartActive_ = true;
    ioSoftStartEverTriggered_ = true;
    ioSoftStartReady_ = false;
    ioSoftStartStartTime_ = std::chrono::steady_clock::now();

    ioPositionWasClose_ = false;
    ioPositionCloseStartTime_ = 0.0;

    // Reset all violation / tracking state for a clean start
    ioLimitingEnabled_ = false;
    ioViolationActive_ = false;
    ioViolationDetectionSuspended_ = false;
    ioViolationStartTime_ = 0.0;
    ioVelocityViolationStartTime_ = 0.0;
    ioAccelViolationStartTime_ = 0.0;
    ioStaleViolationStartTime_ = 0.0;
    ioAnomalyViolationStartTime_ = 0.0;
    ioStaleCounter_ = 0;
    ioStaleViolationLatched_ = false;
    ioInputAnomalyActive_ = false;
    ioPredictedThisCycle_ = false;
    ioPreviousCyclePredicted_ = false;
    ioSoftStartCommandVelocity_ = 0.0;
    ioTrackingClampDirection_ = 0;
    ioCurrentViolationType_ = ViolationType::None;
    ioPrevViolationType_ = ViolationType::None;
    ioPrevLimitingEnabled_ = false;

    iMaxVelocity_ = iConfiguredMaxVelocity_ * SOFT_START_VEL_RATIO;
    iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
    iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
    ioVelocityBlendActive_ = false;
    ioAccelRampActive_ = false;
    ioAccelRampStartTime_ = std::chrono::steady_clock::time_point();
    ioAccelRampStartVel_ = 0.0;
    ioAccelRampStartAcc_ = 0.0;
    ioAccelRampStartDec_ = 0.0;
    ioSoftStartCompleteTime_ = std::chrono::steady_clock::time_point();
    
    // If the previous soft-start was abandoned without
    // CompleteSoftStart() (e.g. due to ImmediateStop), the PDO
    // limits may still be expanded from that run.  Restore the
    // true configured limits before saving a fresh snapshot,
    // otherwise the expanded limits become the new "original"
    // and the error accumulates across stop/restart cycles.
    if (ioSoftStartLimitsAdjusted_) {
        iMinPDO_ = ioOriginalMinPDO_;
        iMaxPDO_ = ioOriginalMaxPDO_;
    }
    ioOriginalMinPDO_ = iMinPDO_;
    ioOriginalMaxPDO_ = iMaxPDO_;
    ioSoftStartLimitsAdjusted_ = false;
    
    // Force re-init on next Filter() to sync with actual feedback
    ioInitialized_ = false;

    // Exit hold-position mode if it was active (e.g. after Immediate Stop).
    // A new soft-start must track the target; holding the previous position
    // is incompatible and causes the filter to stay frozen while the CSV
    // correction layer tries to move the axis — producing a brief vibration.
    ioHoldingPosition_ = false;
    profileActive_ = false;

    
}

bool PDOFilter::IsSoftStartReady() const {
    return ioSoftStartReady_;
}

void PDOFilter::CompleteSoftStart() {
    ioSoftStartActive_ = false;
    ioSoftStartReady_ = false;
    ioSoftStartElapsedSec_ = 0.0;
    ioPositionWasClose_ = false;
    ioPositionCloseStartTime_ = 0.0;

    if (ioSoftStartLimitsAdjusted_) {
        iMinPDO_ = ioOriginalMinPDO_;
        iMaxPDO_ = ioOriginalMaxPDO_;
        ioSoftStartLimitsAdjusted_ = false;
    }

    // Clear any violation / limiting state that may have been
    // triggered by soft-start oscillation.  If limiting was
    // active (ioLimitingEnabled_=true), the recovery ramp and
    // normal full-limit restoration are skipped, leaving the
    // filter stuck with reduced limits — this produces the
    // "slow convergence after oscillating soft-start" symptom.
    ioLimitingEnabled_ = false;
    ioViolationActive_ = false;
    ioViolationDetectionSuspended_ = false;
    ioViolationStartTime_ = 0.0;
    ioVelocityViolationStartTime_ = 0.0;
    ioAccelViolationStartTime_ = 0.0;
    ioCurrentViolationType_ = ViolationType::None;
    ioPrevViolationType_ = ViolationType::None;
    ioPrevLimitingEnabled_ = false;

    // Start recovery ramp from soft-start limits back to configured full limits
    if (!ioAccelRampActive_) {
        ioAccelRampActive_ = true;
        ioAccelRampStartTime_ = std::chrono::steady_clock::now();
        ioAccelRampStartVel_ = iMaxVelocity_;
        ioAccelRampStartAcc_ = iMaxAcceleration_;
        ioAccelRampStartDec_ = iMaxDeceleration_;
    }
    ioAccelCheckSkipCount_ = 5;
    ioPrevFilteredOutput_ = oFilteredOutput_;
    ioSoftStartCompleteTime_ = std::chrono::steady_clock::now();
    ioVelocityBlendActive_ = true;

    
}

void PDOFilter::ResetStaleInputMonitoring() {
    ioStaleCounter_ = 0;
    ioStaleViolationStartTime_ = 0.0;
    ioStaleViolationLatched_ = false;
    ioAnomalyViolationStartTime_ = 0.0;
    ioInputAnomalyActive_ = false;
}

void PDOFilter::HoldCurrentPosition() {
    // Always restore full configured limits so emergency stop has full braking power.
    // Previously this only ran during soft-start; when violation limiting had reduced
    // iMaxDeceleration_, the axis could creep at very low speed and fail to stop.
    iMaxVelocity_ = iConfiguredMaxVelocity_;
    iMaxAcceleration_ = iConfiguredMaxAcceleration_;
    iMaxDeceleration_ = iConfiguredMaxDeceleration_;
    ioSoftStartActive_ = false;
    ioSoftStartReady_ = false;
    // Disable any in-progress limiting so the accel ramp logic doesn't hold reduced values.
    ioLimitingEnabled_ = false;
    ioAccelRampActive_ = false;
    ioViolationDetectionSuspended_ = false;
    profileActive_ = true;
    ioHoldingPosition_ = true;
    double currentOutput = oFilteredOutput_;
    double nowPos = (ioInitialized_) ? currentOutput : ioPrevActValue_;
    profileTargetPos_ = nowPos;
    oFilteredOutput_ = nowPos;
    profileStartPos_ = nowPos;
    profileStartTime_ = std::chrono::steady_clock::now();
    profileDuration_ = 0.0;
    profileTacc_ = 0.0;
    profileTflat_ = 0.0;
    profileTdec_ = 0.0;
    // previous filtered output matches held position
    ioPrevFilteredOutput_ = nowPos;
    ioInputBuffer_.fill(nowPos);
    ioInputBufferIndex_ = 0;
    ioInputBufferCount_ = INPUT_MA_SIZE;
    ioPrevSetValue_ = nowPos;
    ioPrevActValue_ = nowPos;
    ioPrevEffectiveSetpoint_ = nowPos;
    ioPredictedThisCycle_ = false;
    ioPreviousCyclePredicted_ = false;
    ioTrackingClampDirection_ = 0;
}

void PDOFilter::ReleaseHoldPosition() {
    ioHoldingPosition_ = false;
    profileActive_ = false;
}

double PDOFilter::ComputeDynamicLimitMaxVelocity() const {
    const double configured = sanitize_positive_limit(iConfiguredMaxVelocity_);
    if (configured <= 0.0) {
        return 0.0;
    }

    const double threshold = sanitize_positive_limit(GetVelocityViolationThreshold());
    const double softStartCap = sanitize_positive_limit(GetSoftStartMaxVelocity());
    const double lowerBound = (softStartCap > 0.0) ? softStartCap : configured;
    const double requested = (threshold > 0.0) ? threshold : configured;
    return std::clamp(requested, lowerBound, configured);
}

// Estimate extrapolation velocity from recent velocity history.
// Newer samples get more weight so prediction follows the latest trend.
double PDOFilter::ComputeExtrapolationVelocity() const {
    if (ioExtrapolationVelocityCount_ == 0) {
        // No history yet: fall back to the previous smoothed velocity.
        return ioPrevSmoothedVelocity_;
    }
    
    // Use an exponentially weighted average over the ring buffer.
    double totalWeight = 0.0;
    double weightedSum = 0.0;
    
    // Start with weight 1.0 and decay older samples geometrically.
    double weight = 1.0;
    const double decay = 0.7;  // Higher value keeps more history in the estimate.
    
    for (size_t i = 0; i < ioExtrapolationVelocityCount_; ++i) {
        totalWeight += weight;
        weight *= decay;
    }
    
    // Walk backward from the newest sample in the ring buffer.
    weight = 1.0;
    for (size_t i = 0; i < ioExtrapolationVelocityCount_; ++i) {
        // Newest sample is index-1; older samples follow in reverse order.
        size_t idx = (ioExtrapolationVelocityIndex_ + EXTRAPOLATION_HISTORY_SIZE - 1 - i) % EXTRAPOLATION_HISTORY_SIZE;
        weightedSum += ioExtrapolationVelocityBuffer_[idx] * weight;
        weight *= decay;
    }
    
    return weightedSum / totalWeight;
}

double PDOFilter::ComputeTrackingErrorWindow() const {
    return GetTrackingErrorLimit();
}

void PDOFilter::UpdateOutputVelocityAndAcceleration() {
    if (iCycleTime_ > 0.0) {
        double newOutputVel = (oFilteredOutput_ - ioPrevFilteredOutput_) / iCycleTime_;
        oOutputAcceleration_ = (newOutputVel - ioPrevOutputVelocity_) / iCycleTime_;
        oOutputVelocity_ = newOutputVel;
        ioPrevFilteredOutput_ = oFilteredOutput_;
        ioPrevOutputVelocity_ = oOutputVelocity_;
    }
}

void PDOFilter::UpdateOutputVelocityAndAcceleration(double currentFinalOutput, double prevFinalOutput) {
    if (iCycleTime_ > 0.0) {
        double newOutputVel = (currentFinalOutput - prevFinalOutput) / iCycleTime_;
        oOutputAcceleration_ = (newOutputVel - ioPrevOutputVelocity_) / iCycleTime_;
        oOutputVelocity_ = newOutputVel;
        // note: do not overwrite ioPrevFilteredOutput_ here (it tracks the previous final output)
        ioPrevOutputVelocity_ = oOutputVelocity_;
    }
}

std::pair<double, double> PDOFilter::ComputeOnlineTargetVelAccel(double targetPos, double feedforwardVel) {
    // Default position used for control is the internal integrated output.
    // PT2 removed; position is the filtered output.
    double position = oFilteredOutput_;
    double distance = targetPos - position;
    double distAbs = std::abs(distance);
    double effMaxVel = iMaxVelocity_;
    if (distAbs <= 1e-9 && std::abs(feedforwardVel) <= 1e-9) return { 0.0, 0.0 };
    // Apply a deadband to small position errors to avoid large correction from high Kp.
    double effectiveDeadband = iPositionDeadband_;
    double maxStopVel = effMaxVel; 
    double correctionVel = 0.0;
    if (distAbs >= effectiveDeadband) {
        // During soft-start, use a much lower position gain to prevent aggressive correction
        // from large initial position errors (e.g., when starting outside limits).
        // This allows the soft-start velocity/acceleration limits to control the ramp smoothly.
        double effectiveGain = iPositionGain_;
        if (ioSoftStartActive_) {
            effectiveGain = iPositionGain_ * 0.1;
            // The tracking-error clamp window (5°) creates a constant position
            // error during approach, causing the filter to cruise at
            // v ≈ gain × window.  If the deceleration ceiling cannot stop
            // that cruise speed within the window, the filter overshoots.
            // Condition for no overshoot:  gain² ≤ 2·maxDecel / window
            //   (from stopping distance: v²/(2·a) ≤ window,
            //    with v = gain·window → gain²·window/(2·a) ≤ 1).
            // Clamp effectiveGain dynamically so the approach speed stays
            // within the soft-start deceleration capability.
            static constexpr double kTrackingWindow = TRACKING_ERROR_SOFT_START_MAX_DEVIATION;
            double maxDecel = std::max(std::abs(iMaxDeceleration_), 1e-9);
            double maxSafeGain = std::sqrt(2.0 * maxDecel / std::max(kTrackingWindow, 0.1));
            if (effectiveGain > maxSafeGain) {
                effectiveGain = maxSafeGain;
            }
        }
        correctionVel = distance * effectiveGain;
        
        // Safe Braking Curve:
        // Limit correction velocity to preventing overshoot.
        // v_max = sqrt(2 * a * d). 
        // We use a 0.95 factor (close to 1.0) to minimize lag while still respecting the deceleration limit.
        // Limit correction velocity (P-term) to prevent overshoot.
        // v_max = sqrt(2 * a * d). 
        // We use the CURRENT active deceleration limit (iMaxDeceleration_) to ensure consistency
        // with the clamping logic below. This prevents overshoots during soft-start/limiting phases.
        // A factor of 1.1 is used to be slightly more permissive, avoiding "dips" near the target.
        maxStopVel = std::sqrt(2.0 * iMaxDeceleration_ * distAbs * 1.1);
        correctionVel = std::clamp(correctionVel, -maxStopVel, maxStopVel);
    }
    double desiredVel = feedforwardVel + correctionVel;
    
    // Total Velocity is NOT clamped by the static braking curve, because the target is moving.
    // We trust that the Feedforward profile itself is physically realizable (or close to it).
    // The Correction clamp above handles the safety of the 'catch-up' component.
    
    desiredVel = std::clamp(desiredVel, -effMaxVel, effMaxVel);
    // Use the latest raw velocity (measured when available, otherwise
    // estimated from input difference) as the primary feedback for the
    // control loop. This reduces sensitivity to internal integrator drift
    // and helps cancel estimation-induced oscillations.
    // Prefer an externally measured velocity when available. Otherwise
    // use internal state to avoid chasing the raw setpoint derivative which
    // can cause oscillation.
    // Use the internal velocity state as the primary feedback for the control loop.
    // This ensures stability by closing the loop around the internal integrator,
    // avoiding the phase lag of the PT2 filter or the noise of input differentiation.
    double currentVelForControl = oCurrentVelocity_;
    
    // Note: We deliberately ignore external post-filtered velocity feedback
    // for the control loop feedback to guarantee stability of the command generation profile.
    // Closing the loop on filtered outputs previously caused instability.
    double velDiff = desiredVel - currentVelForControl;
    double desiredAccel = velDiff / std::max(iCycleTime_, 1e-9);
    double a_limit = (velDiff >= 0) ? iMaxAcceleration_ : iMaxDeceleration_;
    desiredAccel = std::clamp(desiredAccel, -std::abs(a_limit), std::abs(a_limit));
    
    return { desiredVel, desiredAccel };
}

double PDOFilter::Filter(double iSetValue, double iActValue, double iMeasuredVel) {
    return FilterWithInputState(iSetValue,
                                iActValue,
                                iMeasuredVel,
                                false,
                                true,
                                std::numeric_limits<double>::quiet_NaN(),
                                true);
}

double PDOFilter::FilterWithInputState(double iSetValue,
                                       double iActValue,
                                       double iMeasuredVel,
                                       bool iInputUpdateKnown,
                                       bool iInputUpdated,
                                       double iInputElapsedSec,
                                       bool iAllowInputPrediction) {
    if (!ioFilterStartTimeInitialized_) {
        ioFilterStartTime_ = std::chrono::steady_clock::now();
        ioFilterStartTimeInitialized_ = true;
    }
    auto now = std::chrono::steady_clock::now();
    double currentTime = std::chrono::duration<double>(now - ioFilterStartTime_).count();

    // Use the raw feedback position for soft-start math; the final output is still clamped at the end.
    // This preserves the true starting offset even when the axis begins outside P9/P10, so soft-start
    // calculates from the actual location instead of the limit boundary.

    // Keep a copy of the original incoming setpoint (MMF input)
    double originalIncomingSet = iSetValue;
    const double SET_EQ_EPS = 1e-9;
    const bool inputStateKnown = iInputUpdateKnown;
    const bool inputUpdated = !inputStateKnown || iInputUpdated;
    const bool allowInputPrediction = iAllowInputPrediction;
    const double inputElapsedSec = (std::isfinite(iInputElapsedSec) && iInputElapsedSec > 0.0)
        ? iInputElapsedSec
        : std::max(iCycleTime_, 1e-9);
    const bool previousCyclePredicted = ioPreviousCyclePredicted_;

    if (!has_valid_motion_limits(iConfiguredMaxVelocity_,
                                 iConfiguredMaxAcceleration_,
                                 iConfiguredMaxDeceleration_,
                                 iCycleTime_)) {
        oFilteredOutput_ = iActValue;
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = 0.0;
        oOutputVelocity_ = 0.0;
        oOutputAcceleration_ = 0.0;
        ioPrevFilteredOutput_ = iActValue;
        ioPrevOutputVelocity_ = 0.0;
        ioPrevActValue_ = iActValue;
        ioPrevSetValue_ = originalIncomingSet;
        ioPrevEffectiveSetpoint_ = iActValue;
        ioLastPredictedSetpoint_ = iActValue;
        ioPredictedThisCycle_ = false;
        ioPrevSmoothedVelocity_ = 0.0;
        ioSoftStartCommandVelocity_ = 0.0;
        ioMeasuredVelocityProvided_ = false;
        ioLastRawVelocity_ = 0.0;
        ioLastRawAcceleration_ = 0.0;
        ioVelocityBuffer_.fill(0.0);
        ioVelocityBufferIndex_ = 0;
        ioVelocityBufferCount_ = 0;
        ioExtrapolationVelocityBuffer_.fill(0.0);
        ioExtrapolationVelocityIndex_ = 0;
        ioExtrapolationVelocityCount_ = 0;
        ioInputBuffer_.fill(iActValue);
        ioInputBufferIndex_ = 0;
        ioInputBufferCount_ = INPUT_MA_SIZE;
        ioStaleCounter_ = 0;
        ioStaleViolationLatched_ = false;
        ioInputAnomalyActive_ = false;
        ioLastInputAnomalyReason_ = InputAnomalyReason::Unknown;
        ioStaleViolationStartTime_ = 0.0;
        ioAnomalyViolationStartTime_ = 0.0;
        ioViolationStartTime_ = 0.0;
        ioAccelViolationStartTime_ = 0.0;
        ioVelocityViolationStartTime_ = 0.0;
        ioNoViolationStartTime_ = 0.0;
        ioViolationClearStartTime_ = 0.0;
        ioViolationActive_ = false;
        ioLimitingEnabled_ = false;
        ioViolationDetectionSuspended_ = false;
        ioCurrentViolationType_ = ViolationType::None;
        ioPrevViolationType_ = ViolationType::None;
        ioPrevLimitingEnabled_ = false;
        ioPreviousCyclePredicted_ = false;
        ioTrackingClampDirection_ = 0;
        ioInitialized_ = false;
        return oFilteredOutput_;
    }

    bool measuredVelocityProvided = std::isfinite(iMeasuredVel);
    // Remember whether an external measured velocity was supplied so downstream
    // routines (e.g., ComputeOnlineTargetVelAccel) can use it.
    ioMeasuredVelocityProvided_ = measuredVelocityProvided;
    if (measuredVelocityProvided) {
        ioLastRawVelocity_ = iMeasuredVel;
        ioLastRawAcceleration_ = (ioLastRawVelocity_ - ioPrevSmoothedVelocity_) / std::max(iCycleTime_, 1e-9);
    }

    // Stale->changed transition should be treated as a violation event
    // (limiting + RECOVERY_DELAY_SEC flow), not as a soft-start restart.
    const bool valueChanged = (std::abs(originalIncomingSet - ioPrevSetValue_) > SET_EQ_EPS);
    bool mmfInputChanged = inputStateKnown ? inputUpdated : valueChanged;
    double staleDurationBeforeSec = ioStaleCounter_ * std::max(iCycleTime_, 1e-9);
    bool staleRecoveryTransition =
        (!inputStateKnown && !ioSoftStartActive_ && !ioHoldingPosition_ &&
         mmfInputChanged && staleDurationBeforeSec >= STALE_VIOLATION_DELAY_SEC);

    bool inputAnomalyDetected = false;
    bool trackingErrorViolation = false;
    bool trackingErrorTimerActive = false;
    PDOFilter::InputAnomalyReason anomalyReason = PDOFilter::InputAnomalyReason::Unknown;
    double anomalyObservedValue = 0.0;
    double anomalyAllowedValue = 0.0;
    double anomalyReplacementValue = iSetValue;

    auto applyInputGuard = [&](PDOFilter::InputAnomalyReason reason,
                               double observedValue,
                               double allowedValue,
                               double replacementValue) {
        inputAnomalyDetected = true;
        anomalyReason = reason;
        anomalyObservedValue = observedValue;
        anomalyAllowedValue = allowedValue;
        anomalyReplacementValue = replacementValue;
        iSetValue = replacementValue;
    };

    // === Input anomaly detection ===
    // Reject setpoint jumps that exceed what configured accel/decel can explain in one cycle.
    // If a jump is implausible, continue with a predicted setpoint instead.
    const bool anomalyGuardEnabled = (!inputStateKnown || allowInputPrediction) &&
                                     (!inputStateKnown || inputUpdated);
    if (ioInitialized_ && !ioSoftStartActive_ && anomalyGuardEnabled) {
        // Absolute setpoint delta for this cycle.
        double inputChange = std::abs(iSetValue - ioPrevSetValue_);
        
        // Compute a conservative maximum per-cycle delta from configured limits.
        // The 1.5 factor gives a small buffer for real-world noise and discretization.
        double maxAccelLimit = std::max(iConfiguredMaxAcceleration_, iConfiguredMaxDeceleration_);
        double guardDt = inputStateKnown ? inputElapsedSec : std::max(iCycleTime_, 1e-9);
        double maxAllowedChange = maxAccelLimit * 1.5 * guardDt * guardDt;
        
        // Treat the jump as anomalous and continue with predicted motion instead.
        if (inputChange > maxAllowedChange) {
            PDOFilter::InputAnomalyReason detectedReason;
            if (ioStaleCounter_ > 0) {
                detectedReason = PDOFilter::InputAnomalyReason::StaleRecovery;
            } else if (originalIncomingSet < iMinPDO_ || originalIncomingSet > iMaxPDO_) {
                detectedReason = PDOFilter::InputAnomalyReason::PDOLimitJump;
            } else {
                detectedReason = PDOFilter::InputAnomalyReason::ExceededAccelLimit;
            }

            // Predict the next setpoint from recent velocity history.
            double extrapolationVel = ComputeExtrapolationVelocity();
            double predictionAnchor = previousCyclePredicted ? ioPrevEffectiveSetpoint_ : ioPrevSetValue_;
            double predictedValue = predictionAnchor + extrapolationVel * iCycleTime_;
            double replacementValue = predictedValue;
            if (detectedReason == PDOFilter::InputAnomalyReason::ExceededAccelLimit &&
                (ioLimitingEnabled_ || ioViolationDetectionSuspended_)) {
                const double recoveryVelocity = sanitize_positive_limit(ComputeDynamicLimitMaxVelocity());
                const double recoveryDt = std::max(guardDt, std::max(iCycleTime_, 1e-9));
                const double recoveryStep = std::max(maxAllowedChange, recoveryVelocity * recoveryDt);
                if (std::isfinite(recoveryStep) && recoveryStep > 0.0) {
                    const double targetDelta = originalIncomingSet - ioPrevEffectiveSetpoint_;
                    const double rawDelta = originalIncomingSet - ioPrevSetValue_;
                    const bool rawReversingTowardEffective =
                        std::abs(rawDelta) > maxAllowedChange &&
                        std::abs(targetDelta) > maxAllowedChange &&
                        ((rawDelta < 0.0 && targetDelta > 0.0) ||
                         (rawDelta > 0.0 && targetDelta < 0.0));
                    const double recoverySourceDelta = rawReversingTowardEffective ? rawDelta : targetDelta;
                    const double recoveryDelta = std::clamp(recoverySourceDelta,
                                                           -recoveryStep,
                                                           recoveryStep);
                    replacementValue = ioPrevEffectiveSetpoint_ + recoveryDelta;
                }
            }
            applyInputGuard(detectedReason,
                            inputChange,
                            maxAllowedChange,
                            replacementValue);
            
            

        }
    }

    if (ioInitialized_) {
        // Tracking error must be evaluated from the last command actually issued by
        // the filter, not from the upstream MMF target. This avoids false positives
        // on healthy axes during soft-start when the target is still far away.
        const double issuedCommand = ioPrevFilteredOutput_;
        const double issuedEffectiveSetpoint = ioPrevEffectiveSetpoint_;
        const double trackingError = issuedCommand - iActValue;
        const double trackingErrorAbs = std::abs(trackingError);
        const double trackingErrorWindow = ComputeTrackingErrorWindow();
        const double effectiveSetpointErrorAbs = std::abs(issuedEffectiveSetpoint - iActValue);
        const double upstreamTrackingErrorAbs = std::abs(originalIncomingSet - iActValue);
        const bool softStartSaturatedTracking =
            ioSoftStartActive_ &&
            upstreamTrackingErrorAbs > (trackingErrorWindow + 1e-3) &&
            effectiveSetpointErrorAbs >= (trackingErrorWindow - 1e-3);
        const bool directTrackingViolation = (trackingErrorAbs >= (trackingErrorWindow - 1e-3));
        trackingErrorTimerActive = directTrackingViolation || softStartSaturatedTracking;

        if (directTrackingViolation) {
            trackingErrorViolation = true;
            // Always preserve the original MMF target as the guarded
            // setpoint.  The tracking clamp at the end of Filter()
            // already constrains the output to [actual-window,
            // actual+window]; clamping the setpoint as well is
            // redundant and harmful — it can redirect the filter
            // away from the intended target when actual has
            // deviated beyond the window, causing the filter to
            // chase the wrong position (and in soft-start with
            // its 5° window this can produce large-amplitude
            // oscillation between PDO hard limits).
            const double guardedSetpoint = originalIncomingSet;
            applyInputGuard(PDOFilter::InputAnomalyReason::MeasurementMismatch,
                            trackingErrorAbs,
                            trackingErrorWindow,
                            guardedSetpoint);

            
        } else if (softStartSaturatedTracking) {
            trackingErrorViolation = false;

            
        } else {
            // tracking error within window; nothing to do
        }
    }

    if (inputAnomalyDetected) {
        if ((!ioInputAnomalyActive_ || anomalyReason != ioLastInputAnomalyReason_) && ioInputAnomalyCallback_) {
            try {
                ioInputAnomalyCallback_(originalIncomingSet,
                                        anomalyReplacementValue,
                                        anomalyObservedValue,
                                        anomalyAllowedValue,
                                        anomalyReason);
            } catch (...) {
                // swallow exceptions to avoid destabilizing the filter loop
            }
        }
        ioInputAnomalyActive_ = true;
        ioLastInputAnomalyReason_ = anomalyReason;
    } else {
        ioInputAnomalyActive_ = false;
        ioLastInputAnomalyReason_ = InputAnomalyReason::Unknown;
    }

    if (ioSoftStartActive_) {
        ioSoftStartElapsedSec_ = std::chrono::duration<double>(now - ioSoftStartStartTime_).count();
        // Readiness compares the filter's own output position against
        // the MMF target (originalIncomingSet).  Using iActValue
        // (actual position) was unsafe: when the servo overshoots
        // past the target the filter can still be far from the target,
        // causing premature soft-start completion, a sudden restoration
        // of full velocity/acceleration limits, and a violent
        // acceleration-deceleration oscillation across all six axes.
        double positionError = std::abs(originalIncomingSet - oFilteredOutput_);
        bool isPositionClose = (positionError < SOFT_START_READY_TOLERANCE);
        if (isPositionClose) {
            if (!ioPositionWasClose_) {
                ioPositionCloseStartTime_ = ioSoftStartElapsedSec_;
                ioPositionWasClose_ = true;
                
            } else if ((ioSoftStartElapsedSec_ - ioPositionCloseStartTime_) >= SOFT_START_SETTLE_TIME) {
                ioSoftStartReady_ = true;
                
            }
        } else {
            if (ioPositionWasClose_) {
                
            }
            ioPositionWasClose_ = false;
            // Reset ready flag when position leaves the ready tolerance zone.
            // Without this, a brief transient entry into the ready window can
            // latch ioSoftStartReady_ permanently, causing the auto-connect
            // monitor to declare soft-start complete while the axis is still
            // oscillating or overshooting.
            ioSoftStartReady_ = false;
        }

        // Periodic status: log position error every ~1s during soft-start
        {
            static int ssDbgCounter = 0;
            if ((++ssDbgCounter % 500) == 1) {  // ~1s at 2ms cycle
                
            }
        }

        // NOTE: Early recovery ramp removed.
        // It previously started ramping limits back to full when positionError < 1.5°,
        // but that raised iMaxDeceleration_ (and thus targetBrakeVelocity) and iMaxVelocity_
        // (and thus minCreepVel) while the filter was still approaching the target,
        // allowing the filter to accelerate near the target and overshoot.
        // CompleteSoftStart() already handles the post-soft-start recovery ramp correctly.
    }

    if (!ioInitialized_) {
        oFilteredOutput_ = iActValue;
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = 0.0;
        ioInitialized_ = true;
        ioPrevActValue_ = iActValue;
        ioPrevSetValue_ = iSetValue;
        
        // During soft-start, temporarily adjust PDO limits if actual position is outside the configured limits.
        // This prevents clamping from causing velocity spikes when P9 > P13 (actual position).
        // The original limits will be restored when soft-start completes.
        if (ioSoftStartActive_) {
            bool needAdjustMin = (iActValue < iMinPDO_);
            bool needAdjustMax = (iActValue > iMaxPDO_);
            
            if (needAdjustMin || needAdjustMax) {
                    if (needAdjustMin) {
                    iMinPDO_ = iActValue;
                    }
                if (needAdjustMax) {
                    iMaxPDO_ = iActValue;
                }
                ioSoftStartLimitsAdjusted_ = true;
            }
            
            // Initialize the previous effective setpoint to the
            // PDO-clamped MMF target — NOT to the actual position.
            // Setting it to actual causes a massive feedforward
            // velocity spike on the very next Filter() cycle when
            // effectiveSetpoint jumps to the (potentially far-away)
            // MMF target.  That spike destabilises the filter from
            // the first real cycle and is the root cause of the
            // intermittent soft-start oscillation: when actual
            // happens to be near the target the spike is harmless;
            // when actual is far away the spike triggers a
            // persistent limit cycle.
            const double initTarget = std::clamp(originalIncomingSet, iMinPDO_, iMaxPDO_);
            ioPrevEffectiveSetpoint_ = initTarget;
            ioInputBuffer_.fill(iActValue);
                 
        } else {
            ioPrevEffectiveSetpoint_ = iSetValue;
            ioInputBuffer_.fill(iSetValue);
        }
        ioAccelCheckSkipCount_ = 5; // Increased to allow more time for buffers to stabilize
        ioPrevOutputVelocity_ = 0.0;
        ioInputBufferIndex_ = 0;
        ioInputBufferCount_ = INPUT_MA_SIZE;
        ioVelocityBuffer_.fill(0.0);
        ioVelocityBufferIndex_ = 0;
        ioVelocityBufferCount_ = 0;
        ioPrevSmoothedVelocity_ = 0.0;
        // previous filtered output tracked directly
        ioPrevFilteredOutput_ = iActValue;
        ioPredictedThisCycle_ = false;
        ioPreviousCyclePredicted_ = false;
        ioTrackingClampDirection_ = 0;
        UpdateOutputVelocityAndAcceleration();
        return oFilteredOutput_;
    }

    // --- Prediction and Smoothing Logic ---
    // If the MMF input equals the previous sample, it's 'stale'. 
    // We predict forward using the last known output velocity to keep motion smooth.
    // Always use the actual input value comparison (valueChanged) for stale
    // detection.  In Pose6D mode, inputUpdated tracks raw-MMF channel counts
    // which are broadcast to all axes after IK; a per-axis IK output may not
    // have changed even when the raw count advanced.  Using valueChanged
    // ensures prediction activates whenever the filter's real input is static.
    bool isInputStale = !valueChanged;

    // Once soft-start is finished, stale-input detection stays active continuously
    // while online filtering is enabled. It is only paused during soft-start itself
    // (or explicit hold-position), and is reset when online mode turns off.
    const bool staleDetectionEnabled =
        (!ioSoftStartActive_ && !ioHoldingPosition_);

    if (staleDetectionEnabled) {
        if (isInputStale) {
            if (ioStaleCounter_ < 1000000) ioStaleCounter_++; // Prevent overflow
        } else {
            ioStaleCounter_ = 0;
        }
    } else {
        // During soft-start or hold-position, stale timing is ignored.
        ioStaleCounter_ = 0;
        ioStaleViolationStartTime_ = 0.0;
    }

    // Only predict if input is stale AND we haven't timed out (e.g. 50ms at 1kHz).
    // This allows smoothing slow updates (e.g. 30Hz) but stops "cruising" if input is truly stopped.
    const bool measuredStoppedHold = measuredVelocityProvided &&
                                     std::abs(iMeasuredVel) <= 1e-6 &&
                                     isInputStale;
    bool shouldPredict = allowInputPrediction && isInputStale && !measuredStoppedHold &&
                         (ioStaleCounter_ < 50) && staleDetectionEnabled;

    if (shouldPredict) {
        // Continuous prediction: extrapolate from the last effective setpoint.
        // We limit prediction to avoid runaway, but allow it for multiple cycles to handle 
        // slow MMF update rates (e.g., 30Hz or 60Hz UI inputs).
        // Use multi-point weighted average velocity for more stable extrapolation.
        // This reduces noise and provides better trajectory estimation than single-point velocity.
        double extrapolationVel = ComputeExtrapolationVelocity();
        double predicted = ioPrevEffectiveSetpoint_ + extrapolationVel * iCycleTime_;
        iSetValue = predicted;
        ioPredictedThisCycle_ = true;
        ioLastPredictedSetpoint_ = predicted;
    } else {
        ioPredictedThisCycle_ = false;
    }

    // prediction/MA remain active and are used to compute the effective input.

    // Update input history for smoothing
    ioInputBuffer_[ioInputBufferIndex_] = iSetValue;
    ioInputBufferIndex_ = (ioInputBufferIndex_ + 1) % INPUT_MA_SIZE;
    if (ioInputBufferCount_ < INPUT_MA_SIZE) ioInputBufferCount_++;

    // Calculate smoothed input (kept for blend/bridge transitions only; default uses raw iSetValue)
    double smoothedInput = 0.0;
    for (size_t i = 0; i < ioInputBufferCount_; ++i) smoothedInput += ioInputBuffer_[i];
    smoothedInput /= static_cast<double>(ioInputBufferCount_);

    // Smoothing disabled: EMA pre-filtering is done upstream (MMF pre-filter).
    // Default uses raw iSetValue directly to avoid double-filtering and reduce lag.
    double effectiveInput = iSetValue;
    const bool bridgeFromPrediction = previousCyclePredicted && !ioPredictedThisCycle_ && !isInputStale &&
                                      ioInputBufferCount_ >= INPUT_MA_SIZE;
    if (ioSoftStartActive_ || ioPredictedThisCycle_) {
        effectiveInput = iSetValue;
    } else if (ioVelocityBlendActive_) {
        double elapsed = std::chrono::duration<double>(now - ioSoftStartCompleteTime_).count();
        if (elapsed >= SOFT_START_VEL_BLEND_TIME) {
            ioVelocityBlendActive_ = false;
            effectiveInput = iSetValue;
        } else {
            double alpha = 1.0 - (elapsed / SOFT_START_VEL_BLEND_TIME);
            effectiveInput = alpha * iSetValue + (1.0 - alpha) * smoothedInput;
        }
    } else if (bridgeFromPrediction) {
        effectiveInput = (iSetValue + smoothedInput) * 0.5;
    }
    // Improve prediction quality by maintaining velocity estimate during stale cycles.
    // Previously, we used '0.0' (based on static MMF input) which caused the smoothed velocity 
    // to decay, leading to "braking" during prediction and violent "catch-up" jumps when 
    // new data arrived. Now we hold the velocity constant to ensure smooth tracking.
    // Runaway drift is prevented by the prediction timeout (ioStaleCounter_).
    double commandInputVelocity = 0.0;
    bool commandInputVelocityUpdated = false;
    if (inputStateKnown) {
        if (inputUpdated) {
            commandInputVelocity = (originalIncomingSet - ioPrevSetValue_) / inputElapsedSec;
            commandInputVelocityUpdated = true;
        }
    } else if (valueChanged) {
        commandInputVelocity = (originalIncomingSet - ioPrevSetValue_) / std::max(iCycleTime_, 1e-9);
        commandInputVelocityUpdated = true;
    }
    if (!std::isfinite(commandInputVelocity)) {
        commandInputVelocity = 0.0;
        commandInputVelocityUpdated = false;
    }

    if (ioSoftStartActive_) {
        if (commandInputVelocityUpdated) {
            // Low-pass filter the commanded input velocity to prevent
            // MMF target jitter / oscillation from propagating into
            // targetInputVel and destabilising the soft-start velocity
            // clamp.  Without this, a toggling MMF target produces a
            // toggling upper velocity bound which feeds through the
            // CSV command, the servo, the mechanical coupling, and
            // (via the external application) back to the MMF target
            // — a platform-level feedback loop that oscillates all
            // six axes simultaneously.
            constexpr double kTauSec = 0.1;  // 100 ms smoothing
            const double dt = std::max(iCycleTime_, 1e-9);
            const double alpha = dt / (kTauSec + dt);
            const double clampedInputVel = std::clamp(commandInputVelocity,
                                                      -iMaxVelocity_,
                                                      iMaxVelocity_);
            ioSoftStartCommandVelocity_ +=
                (clampedInputVel - ioSoftStartCommandVelocity_) * alpha;
        } else {
            const double commandVelocityBleed = std::abs(iMaxDeceleration_) * std::max(iCycleTime_, 1e-9);
            if (std::abs(ioSoftStartCommandVelocity_) <= commandVelocityBleed) {
                ioSoftStartCommandVelocity_ = 0.0;
            } else {
                ioSoftStartCommandVelocity_ -= std::copysign(commandVelocityBleed, ioSoftStartCommandVelocity_);
            }
        }
    } else {
        ioSoftStartCommandVelocity_ = 0.0;
    }

    double rawVelocity;
    if (ioPredictedThisCycle_) {
        // When predicting, hold previous smoothed velocity to avoid decay
        rawVelocity = ioPrevSmoothedVelocity_;
    } else if (measuredVelocityProvided) {
        // Prefer external measured velocity when available so the MA reflects
        // the actual motion rather than setpoint-derived deltas.
        rawVelocity = iMeasuredVel;
    } else if (inputStateKnown && inputUpdated) {
        rawVelocity = (originalIncomingSet - ioPrevSetValue_) / inputElapsedSec;
    } else if (inputStateKnown && !inputUpdated) {
        rawVelocity = 0.0;
    } else {
        // Fallback: estimate from setpoint difference. 
        // If we were previously stale, divide the total jump by the duration of the stale period
        // to get the true average velocity instead of a 1ms spike.
        double deltaPos = originalIncomingSet - ioPrevSetValue_;
        double deltaTime = (ioStaleCounter_ + 1) * std::max(iCycleTime_, 1e-9);
        rawVelocity = deltaPos / deltaTime;
    }

    ioVelocityBuffer_[ioVelocityBufferIndex_] = rawVelocity;
    ioVelocityBufferIndex_ = (ioVelocityBufferIndex_ + 1) % VELOCITY_MA_SIZE;
    if (ioVelocityBufferCount_ < VELOCITY_MA_SIZE) ioVelocityBufferCount_++;
    double smoothedVelocity = 0.0;
    for (size_t i = 0; i < ioVelocityBufferCount_; ++i) smoothedVelocity += ioVelocityBuffer_[i];
    smoothedVelocity /= static_cast<double>(ioVelocityBufferCount_);
    double smoothedAcceleration = (iCycleTime_ > 0.0 && ioVelocityBufferCount_ >= 2) ?
                                  (smoothedVelocity - ioPrevSmoothedVelocity_) / iCycleTime_ : 0.0;
    ioPrevSmoothedVelocity_ = smoothedVelocity;
    
    // Keep recent smoothed velocities for multi-sample extrapolation.
    ioExtrapolationVelocityBuffer_[ioExtrapolationVelocityIndex_] = smoothedVelocity;
    ioExtrapolationVelocityIndex_ = (ioExtrapolationVelocityIndex_ + 1) % EXTRAPOLATION_HISTORY_SIZE;
    if (ioExtrapolationVelocityCount_ < EXTRAPOLATION_HISTORY_SIZE) ioExtrapolationVelocityCount_++;
    
    if (ioAccelCheckSkipCount_ > 0) ioAccelCheckSkipCount_--;
    if (!measuredVelocityProvided) {
        ioLastRawVelocity_ = rawVelocity;
        ioLastRawAcceleration_ = smoothedAcceleration;
    }

    // Candidate detection for accel violation (separated to allow diagnostic logging)
    bool hasHardLimitMin = (originalIncomingSet < iMinPDO_) || (iSetValue < iMinPDO_) || (iActValue < iMinPDO_);
    bool hasHardLimitMax = (originalIncomingSet > iMaxPDO_) || (iSetValue > iMaxPDO_) || (iActValue > iMaxPDO_);
    if (ioSoftStartActive_ && ioSoftStartLimitsAdjusted_) {
        constexpr double kSoftStartEscapePositionEps = 1e-4;
        constexpr double kSoftStartEscapeVelocityEps = 1e-3;
        const bool lowerLimitExpanded = iMinPDO_ < ioOriginalMinPDO_;
        const bool upperLimitExpanded = iMaxPDO_ > ioOriginalMaxPDO_;
        auto feedbackMovingOutward = [&](bool lowerLimit) -> bool {
            if (measuredVelocityProvided) {
                return lowerLimit
                    ? (iMeasuredVel < -kSoftStartEscapeVelocityEps)
                    : (iMeasuredVel > kSoftStartEscapeVelocityEps);
            }
            return lowerLimit
                ? (iActValue < ioPrevActValue_ - kSoftStartEscapePositionEps)
                : (iActValue > ioPrevActValue_ + kSoftStartEscapePositionEps);
        };

        if (lowerLimitExpanded && hasHardLimitMin) {
            const bool commandEscapesLower =
                originalIncomingSet >= iActValue - kSoftStartEscapePositionEps &&
                iSetValue >= iActValue - kSoftStartEscapePositionEps &&
                effectiveInput >= iActValue - kSoftStartEscapePositionEps;
            if (commandEscapesLower && !feedbackMovingOutward(true)) {
                hasHardLimitMin = false;
            }
        }
        if (upperLimitExpanded && hasHardLimitMax) {
            const bool commandEscapesUpper =
                originalIncomingSet <= iActValue + kSoftStartEscapePositionEps &&
                iSetValue <= iActValue + kSoftStartEscapePositionEps &&
                effectiveInput <= iActValue + kSoftStartEscapePositionEps;
            if (commandEscapesUpper && !feedbackMovingOutward(false)) {
                hasHardLimitMax = false;
            }
        }
    }
    double velThreshold = iConfiguredMaxVelocity_ * ioVelocityThresholdScale_;
    bool velocityViolation = (!ioSoftStartActive_ && std::abs(rawVelocity) > velThreshold);

    double accelPosThresh = iConfiguredMaxAcceleration_ * ioAccelThresholdScale_;
    double accelNegThresh = -iConfiguredMaxDeceleration_ * ioAccelThresholdScale_;
    bool candidateAccelViolation = (!ioSoftStartActive_ && ioAccelCheckSkipCount_ <= 0 && ioVelocityBufferCount_ >= VELOCITY_MA_SIZE &&
                                    (smoothedAcceleration > accelPosThresh || smoothedAcceleration < accelNegThresh));
    if (measuredVelocityProvided && std::abs(iMeasuredVel) <= 1e-6 && std::abs(rawVelocity) <= 1e-6) {
        candidateAccelViolation = false;
    }

    // === Stale input and anomaly violations ===
    // Stale input is latched only after the setpoint stops changing for a sustained interval.
    bool staleViolation = false;
    bool anomalyViolation = false;
    if (staleDetectionEnabled) {
        // Stale input: treat as violation only after a sustained no-update interval.
        double staleDurationSec = ioStaleCounter_ * std::max(iCycleTime_, 1e-9);
        if (!inputStateKnown && isInputStale && !measuredStoppedHold &&
            staleDurationSec >= STALE_VIOLATION_DELAY_SEC) {
            staleViolation = true;
            ioStaleViolationLatched_ = true;
            if (ioStaleViolationStartTime_ <= 0.0) ioStaleViolationStartTime_ = currentTime;
        } else if (!isInputStale) {
            ioStaleViolationStartTime_ = 0.0;
        }

        if (staleRecoveryTransition) {
            ioStaleViolationStartTime_ = 0.0;
            ioStaleViolationLatched_ = false;
        }

        // Input anomaly replacement is a guard action, not an operator-reset fault.
        // Only a PDO-limit jump may become a formal violation; normal accel-limited
        // substitution and stale-recovery bridging must not latch alarms or stop CSV output.
        const bool inputAnomalyRequiresViolation =
            ioInputAnomalyActive_ && ioLastInputAnomalyReason_ == InputAnomalyReason::PDOLimitJump;
        if (inputAnomalyRequiresViolation) {
            if (ioAnomalyViolationStartTime_ <= 0.0) ioAnomalyViolationStartTime_ = currentTime;
            if ((currentTime - ioAnomalyViolationStartTime_) >= INPUT_ANOMALY_VIOLATION_DELAY_SEC) {
                anomalyViolation = true;
            }
        } else {
            ioAnomalyViolationStartTime_ = 0.0;
        }
    } else {
        ioStaleViolationStartTime_ = 0.0;
        ioAnomalyViolationStartTime_ = 0.0;
    }

    // Stale/anomaly violations are already time-debounced above, so they can
    // suspend detection immediately. Velocity/acceleration candidates are
    // handled by their own timers below; suspending on the first noisy sample
    // turns a harmless one-cycle derivative spike into a latched warning.
    if ((staleViolation || anomalyViolation) && !ioViolationDetectionSuspended_) {
        ioViolationDetectionSuspended_ = true;
        ioViolationClearStartTime_ = 0.0;
    }

    // If violation detection is suspended (we're currently handling a sustained
    // velocity/accel violation), then only watch for the condition clearing.
    if (ioViolationDetectionSuspended_) {
        // Check if raw input is clean (including stale/anomaly conditions)
        bool isInputClean = (!velocityViolation && !candidateAccelViolation && !staleViolation && !anomalyViolation);

        if (isInputClean) {
            // violation cleared in this frame
            if (ioViolationClearStartTime_ <= 0.0) {
                 ioViolationClearStartTime_ = currentTime;
            } 
            
            if ((currentTime - ioViolationClearStartTime_) >= 0.5) { // 0.5s debounce threshold
                // violation cleared stably: resume detection
                ioViolationDetectionSuspended_ = false;
                ioVelocityViolationStartTime_ = 0.0;
                ioAccelViolationStartTime_ = 0.0;
                ioStaleViolationStartTime_ = 0.0;
                ioAnomalyViolationStartTime_ = 0.0;
                ioViolationClearStartTime_ = 0.0;
            }
        } else {
            // still violating: reset debounce timer
            ioViolationClearStartTime_ = 0.0;
        }

        // If we are still suspended (either not clean, or debounce waiting),
        // we MUST hold the violation flags TRUE to prevent downstream logic
        // from resetting the violation timers (ioVelocityViolationStartTime_, etc).
        // This ensures that a flickering signal doesn't reset the "Time to Limit" counter.
        if (ioViolationDetectionSuspended_) {
             if (ioVelocityViolationStartTime_ > 0.0) velocityViolation = true;
             if (ioAccelViolationStartTime_ > 0.0) candidateAccelViolation = true;
             if (ioStaleViolationStartTime_ > 0.0) staleViolation = true;
             if (ioAnomalyViolationStartTime_ > 0.0) anomalyViolation = true;
             
             // Fallback: if we are suspended but timers are somehow 0 (e.g. initial frame race),
             // force Accel violation to keep system in "Active" state.
             if (!velocityViolation && !candidateAccelViolation && !staleViolation && !anomalyViolation) candidateAccelViolation = true;
        }
    }

    

    bool hasAccelViolation = candidateAccelViolation;
    
    double violationDuration = 0.0;
    if ((hasHardLimitMin || hasHardLimitMax) && !ioSoftStartActive_) {
        // Hard position limits are safety boundaries; keep the stricter crawl envelope here.
        ioLimitingEnabled_ = true; ioViolationActive_ = true; ioNoViolationStartTime_ = 0.0;
        iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
        iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
        iMaxVelocity_ = iConfiguredMaxVelocity_ * 0.12;
        if (ioViolationStartTime_ <= 0.0) ioViolationStartTime_ = currentTime;
        violationDuration = currentTime - ioViolationStartTime_;
    } else if (velocityViolation || hasAccelViolation) {
        ioNoViolationStartTime_ = 0.0; ioViolationActive_ = true;
        if (velocityViolation) {
                if (ioVelocityViolationStartTime_ <= 0.0) {
                ioVelocityViolationStartTime_ = currentTime;
            }
            if ((currentTime - ioVelocityViolationStartTime_) >= ioVelocityViolationDelaySec_) {
                const bool wasLimitingEnabled = ioLimitingEnabled_;
                if (!ioSoftStartActive_) {
                    ioLimitingEnabled_ = true;
                    if (ioGlobalLimitingForced_) {
                        // This axis was already globally force-limited by the master.
                        // Now that it has its own velocity violation, restore configured
                        // accel/decel so the axis retains full braking capability for
                        // safety recovery (e.g. MMF reversals can brake the old direction).
                        iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                        iMaxDeceleration_ = iConfiguredMaxDeceleration_;
                    } else {
                        iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
                        iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
                    }
                    iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
                    // Suspend further violation detection until this violation clears
                    ioViolationDetectionSuspended_ = true;
                    // Log the limiting action so it's observable
                        if (!wasLimitingEnabled) {
                        
                    }
                }
            }
        } else {
            ioVelocityViolationStartTime_ = 0.0;
        }
        if (hasAccelViolation) {
            if (ioAccelViolationStartTime_ <= 0.0) ioAccelViolationStartTime_ = currentTime;
            if ((currentTime - ioAccelViolationStartTime_) >= ioAccelViolationDelaySec_) {
                const bool wasLimitingEnabled = ioLimitingEnabled_;
                if (!ioSoftStartActive_) {
                    ioLimitingEnabled_ = true;
                    if (ioGlobalLimitingForced_) {
                        iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                        iMaxDeceleration_ = iConfiguredMaxDeceleration_;
                    } else {
                        iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
                        iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
                    }
                    iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
                    // Suspend further violation detection until this violation clears
                    ioViolationDetectionSuspended_ = true;
                    // Log the distinct limiting action for clarity
                        if (!wasLimitingEnabled) {
                        
                    }
                }
            }
        } else ioAccelViolationStartTime_ = 0.0;
        if (ioViolationStartTime_ <= 0.0) ioViolationStartTime_ = currentTime;
        violationDuration = currentTime - ioViolationStartTime_;
    } else if (staleViolation || anomalyViolation) {
        // Treat stale or anomalous MMF input like a safety violation and reduce limits.
        ioNoViolationStartTime_ = 0.0; ioViolationActive_ = true;
            if (!ioLimitingEnabled_) {
            ioLimitingEnabled_ = true;
            iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
            iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
            iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
            ioViolationDetectionSuspended_ = true;
        }
        if (ioViolationStartTime_ <= 0.0) ioViolationStartTime_ = currentTime;
        violationDuration = currentTime - ioViolationStartTime_;
    } else if (trackingErrorTimerActive) {
        ioNoViolationStartTime_ = 0.0;
        ioViolationActive_ = true;
        violationDuration = 0.0;
    } else {
        ioViolationActive_ = false; ioViolationStartTime_ = ioVelocityViolationStartTime_ = ioAccelViolationStartTime_ = 0.0;
        ioStaleViolationStartTime_ = 0.0; ioAnomalyViolationStartTime_ = 0.0;
        if (ioLimitingEnabled_ && !ioGlobalLimitingForced_) {
            if (ioNoViolationStartTime_ <= 0.0) {
                // first frame without violation: start recovery timer
        ioViolationDetectionSuspended_ = false;
                ioNoViolationStartTime_ = currentTime;
            } else if ((currentTime - ioNoViolationStartTime_) >= RECOVERY_DELAY_SEC) {
                // Recovery: instead of instantly restoring accel/decel limits,
                // start a smooth ramp back to configured values to avoid a
                // sudden 'catch-up' acceleration spike.
                ioLimitingEnabled_ = false;
                ioNoViolationStartTime_ = 0.0;
                ioAccelRampActive_ = true;
                ioAccelRampStartTime_ = std::chrono::steady_clock::now();
                ioAccelRampStartVel_ = iMaxVelocity_;
                ioAccelRampStartAcc_ = iMaxAcceleration_; // current (reduced) value
                ioAccelRampStartDec_ = iMaxDeceleration_;
            }
        }
    }

    if (ioViolationCallback_) {
        ViolationType vType = ViolationType::None;
        const bool reportDynamicViolation = ioLimitingEnabled_ || ioViolationDetectionSuspended_;
        if (reportDynamicViolation && velocityViolation) vType |= ViolationType::Velocity;
        if (reportDynamicViolation && hasAccelViolation) vType |= (smoothedAcceleration > 0 ? ViolationType::Acceleration : ViolationType::Deceleration);
        if (hasHardLimitMin) vType |= ViolationType::HardLimitMin;
        if (hasHardLimitMax) vType |= ViolationType::HardLimitMax;
        if (anomalyViolation) vType |= ViolationType::InputAnomaly;
        if (staleViolation) vType |= ViolationType::StaleInput;
        const bool reportTrackingErrorViolation =
            (trackingErrorViolation && !ioSoftStartActive_);
        if (reportTrackingErrorViolation) vType |= ViolationType::TrackingError;
        
        // When a violation is active (detection suspended) or limiting is engaged,
        // latch the violation type to the previous value. This prevents "chatter"
        // (e.g. Accel <-> Decel toggling) and repetitive logs, satisfying the requirement
        // to stop violation checking until the violation is fully released.
        if ((ioViolationDetectionSuspended_ || ioLimitingEnabled_) &&
            vType != ViolationType::None &&
            ioPrevViolationType_ != ViolationType::None) {
            vType = ioPrevViolationType_;
        }
        
        ioCurrentViolationType_ = vType;
        double callbackViolationDuration = violationDuration;
        if (ioSoftStartActive_ &&
            PDOFilter::HasFlag(vType, ViolationType::TrackingError)) {
            callbackViolationDuration = 0.0;
        }

        // Notify on limiting state transitions (existing behavior)
        if (!ioPrevLimitingEnabled_ && ioLimitingEnabled_) {
            // Report the smoothed velocity/acceleration values (the same
            // quantities used for violation detection) so the UI sees a
            // consistent and signed value rather than transient raw deltas.
            ioViolationCallback_(vType, smoothedVelocity, smoothedAcceleration, true, callbackViolationDuration);
        } else if (ioPrevLimitingEnabled_ && !ioLimitingEnabled_) {
            ioViolationCallback_(ViolationType::None, smoothedVelocity, smoothedAcceleration, false, 0.0);
        } else if (vType != ioPrevViolationType_) {
            // Notify the UI with smoothed values when the violation TYPE changes.
            ioViolationCallback_(vType, smoothedVelocity, smoothedAcceleration, ioLimitingEnabled_, callbackViolationDuration);
        }

        ioPrevViolationType_ = vType;
    }
    ioPrevLimitingEnabled_ = ioLimitingEnabled_;
    
    if (!ioLimitingEnabled_) {
        if (ioAccelRampActive_) {
            // Interpolate velocity/accel/decel from their starting values to configured over ramp duration.
            // When dynamic scaling is active, target is 0.5x configured instead of full.
            double rampElapsed = std::chrono::duration<double>(now - ioAccelRampStartTime_).count();
            double alpha = (ioAccelRampDurationSec_ > 0.0) ? (rampElapsed / ioAccelRampDurationSec_) : 1.0;
            const double dynFactor = ioDynamicScalingAccelActive_ ? 0.5 : 1.0;
            if (alpha >= 1.0) {
                iMaxVelocity_ = iConfiguredMaxVelocity_;
                iMaxAcceleration_ = iConfiguredMaxAcceleration_ * dynFactor;
                iMaxDeceleration_ = iConfiguredMaxDeceleration_ * dynFactor;
                ioAccelRampActive_ = false;
            } else {
                iMaxVelocity_ = (1.0 - alpha) * ioAccelRampStartVel_ + alpha * iConfiguredMaxVelocity_;
                iMaxAcceleration_ = (1.0 - alpha) * ioAccelRampStartAcc_ + alpha * iConfiguredMaxAcceleration_ * dynFactor;
                iMaxDeceleration_ = (1.0 - alpha) * ioAccelRampStartDec_ + alpha * iConfiguredMaxDeceleration_ * dynFactor;
            }
        } else if (!ioSoftStartActive_) {
            // Ensure values are at configured max, unless dynamic scaling is active (0.5x)
            iMaxVelocity_ = iConfiguredMaxVelocity_;
            if (!ioDynamicScalingAccelActive_) {
                iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                iMaxDeceleration_ = iConfiguredMaxDeceleration_;
            }
        }
    }

    constexpr double kPostSoftStartLimitPreviewMargin = PDOFilter::POST_SOFT_START_LIMIT_PREVIEW_MARGIN_DEG;
    double motionMinLimit = iMinPDO_;
    double motionMaxLimit = iMaxPDO_;
    const bool postSoftStartLimitPreviewEnabled =
        !ioSoftStartActive_ &&
        !ioHoldingPosition_ &&
        std::isfinite(iMinPDO_) &&
        std::isfinite(iMaxPDO_) &&
        (iMaxPDO_ - iMinPDO_) > (2.0 * kPostSoftStartLimitPreviewMargin);
    if (postSoftStartLimitPreviewEnabled) {
        motionMinLimit = iMinPDO_ + kPostSoftStartLimitPreviewMargin;
        motionMaxLimit = iMaxPDO_ - kPostSoftStartLimitPreviewMargin;
    }

    if (!ioSoftStartActive_) {
        if (hasHardLimitMin) {
            effectiveInput = std::max(effectiveInput, iMinPDO_);
        }
        if (hasHardLimitMax) {
            effectiveInput = std::min(effectiveInput, iMaxPDO_);
        }
        if (postSoftStartLimitPreviewEnabled) {
            effectiveInput = std::clamp(effectiveInput, motionMinLimit, motionMaxLimit);
        }
    }

    double effectiveSetpoint = effectiveInput;
    if (ioHoldingPosition_) {
        effectiveSetpoint = profileTargetPos_;
        profileActive_ = true;
    } else if (profileActive_) {
        double t = std::chrono::duration<double>(now - profileStartTime_).count();
        if (t >= profileDuration_) { effectiveSetpoint = profileTargetPos_; profileActive_ = false; }
        else { effectiveSetpoint = eval_trapezoid_pos(t, profileTacc_, profileTflat_, profileTdec_, profilePeakVel_, (double)profileDir_, profileStartPos_); }
    }
    if (!ioHoldingPosition_) {
        const double trackingErrorWindow = ComputeTrackingErrorWindow();
        const double requestedSetpoint = effectiveSetpoint;
        const double previousOutputForClamp = oFilteredOutput_;
        // Expand the clamp window so it always includes the original
        // MMF target, preventing the effectiveSetpoint from being
        // pushed AWAY from the intended target when actual deviates
        // beyond the tracking window.  Without this, a target of 10°
        // with actual at 40° would clamp the setpoint to 35° (actual-5),
        // redirecting the filter away from the target and producing
        // large-amplitude oscillation between PDO hard limits.
        const double lowerBound = std::min(originalIncomingSet,
                                           iActValue - trackingErrorWindow);
        const double upperBound = std::max(originalIncomingSet,
                                           iActValue + trackingErrorWindow);
        double trackingClampedSetpoint = std::clamp(effectiveSetpoint,
                                                    lowerBound,
                                                    upperBound);
        if (requestedSetpoint < previousOutputForClamp && trackingClampedSetpoint > previousOutputForClamp) {
            trackingClampedSetpoint = previousOutputForClamp;
        } else if (requestedSetpoint > previousOutputForClamp && trackingClampedSetpoint < previousOutputForClamp) {
            trackingClampedSetpoint = previousOutputForClamp;
        }
        effectiveSetpoint = std::clamp(trackingClampedSetpoint, motionMinLimit, motionMaxLimit);
    }

    const double stepStartOutput = oFilteredOutput_;
    const double stepStartVelocity = oCurrentVelocity_;

    if (ioHoldingPosition_) {
        double targetAcc = (iCycleTime_ > 0.0) ? (-oCurrentVelocity_ / iCycleTime_) : 0.0;
        targetAcc = std::clamp(targetAcc, -std::abs(iMaxDeceleration_), std::abs(iMaxDeceleration_));
        oCurrentVelocity_ += targetAcc * iCycleTime_;
        oFilteredOutput_ += oCurrentVelocity_ * iCycleTime_;
        oCurrentAcceleration_ = targetAcc;
    } else {
        // Feedforward is calculated from the effective setpoint dynamics.
        // This naturally includes the "Correction Kick" when the MMF input updates (Step minus Prediction),
        // allowing the filter to jump immediately to the new trajectory height without lag.
        double feedforwardVel = (iCycleTime_ > 0.0) ? (effectiveSetpoint - ioPrevEffectiveSetpoint_) / iCycleTime_ : 0.0;
        feedforwardVel *= iFeedforwardGain_;
        
        // During actual soft-start (not the landing phase), override
        // effectiveSetpoint to the PDO-clamped MMF target so the filter
        // always converges to the correct safe position.
        if (ioSoftStartActive_ && std::isfinite(originalIncomingSet)) {
            // Safety: override effectiveSetpoint to the PDO-clamped MMF
            // target.  Multiple clamp/window layers above can, in edge
            // cases on real hardware, redirect the setpoint away from
            // the intended target.  Forcing it here ensures the filter
            // always converges to the correct position.
            const double brakeTarget = std::clamp(originalIncomingSet, iMinPDO_, iMaxPDO_);
            effectiveSetpoint = brakeTarget;
            // Recompute feedforward with the corrected setpoint so the
            // velocity command is consistent.
            feedforwardVel = (iCycleTime_ > 0.0) ? (effectiveSetpoint - ioPrevEffectiveSetpoint_) / iCycleTime_ : 0.0;
            feedforwardVel *= iFeedforwardGain_;
        }
        
        auto [desiredVel, targetAcc] = ComputeOnlineTargetVelAccel(effectiveSetpoint, feedforwardVel);
        if (ioSoftStartActive_ && std::isfinite(originalIncomingSet)) {
            const double brakeTarget = std::clamp(originalIncomingSet, iMinPDO_, iMaxPDO_);
            const double distanceToCommandTarget = brakeTarget - stepStartOutput;
            const double maxSoftStartDecel = std::max(std::abs(iMaxDeceleration_), 1e-9);

            // Ramp the effective max velocity from 5 % to 100 % of the
            // soft-start limit over 1.0 s to prevent an initial jerk.
            constexpr double kSsVelRampSec = 1.0;
            const double ssVelRamp = (ioSoftStartElapsedSec_ < kSsVelRampSec)
                ? (0.05 + 0.95 * ioSoftStartElapsedSec_ / kSsVelRampSec)
                : 1.0;
            const double effMaxVel  = iMaxVelocity_ * ssVelRamp;

            // Enforce a minimum creep speed floor so the axis can always converge
            // when very close to the target. Without this, the braking curve drives
            // velocity toward zero as remainingDistance -> 0, causing either
            // oscillation (feedback fights the tightening ceiling) or asymptotic
            // stall (can never cross the final tolerance boundary).
            const double minCreepVel = effMaxVel * 0.08;
            // Ensure the tolerance deadband is at least the stopping distance
            // from minCreepVel so the filter has enough room to decelerate to
            // zero after crossing into tolerance.  Without this, a small
            // SOFT_START_POSITION_TOLERANCE combined with the minCreepVel floor
            // causes overshoot near the target.
            const double minStoppingDistFromCreep = (minCreepVel * minCreepVel) / (2.0 * maxSoftStartDecel);
            const double targetTolerance = std::max({ SOFT_START_POSITION_TOLERANCE, iPositionDeadband_, minStoppingDistFromCreep });
            const double remainingDistance = std::max(0.0, std::abs(distanceToCommandTarget) - targetTolerance);
            const double targetBrakeVelocity = std::sqrt(2.0 * maxSoftStartDecel * remainingDistance);
            const double effectiveBrakeVelocity = std::max(targetBrakeVelocity, minCreepVel);
            
            const double rawTargetInputVel = ioSoftStartCommandVelocity_;
            const double targetInputVel = std::clamp(rawTargetInputVel, -effMaxVel, effMaxVel);

            
            if (distanceToCommandTarget > targetTolerance) {
                desiredVel = std::clamp(desiredVel, -effMaxVel, targetInputVel + effectiveBrakeVelocity);
            } else if (distanceToCommandTarget < -targetTolerance) {
                // Command has passed the target.  Instead of relying
                // on the CSV layer to suppress outward feedforward
                // (which causes stop-go stutter), force the filter
                // itself to brake aggressively by setting the desired
                // velocity to zero when the current velocity is still
                // moving away from the target.  This makes the filter
                // decelerate at max rate, which in turn reduces the
                // CSV feedforward naturally without external inhibition.
                if ((distanceToCommandTarget < 0.0 && stepStartVelocity > 0.0) ||
                    (distanceToCommandTarget > 0.0 && stepStartVelocity < 0.0)) {
                    desiredVel = 0.0;
                } else {
                    desiredVel = std::clamp(desiredVel,
                                            targetInputVel - effectiveBrakeVelocity,
                                            targetInputVel + effectiveBrakeVelocity);
                }
            } else {
                // Within the braking deadband but not yet at the exact target.
                // Instead of stopping completely (desiredVel = targetInputVel = 0),
                // asymptotically inch toward the target at a speed proportional
                // to the remaining distance.  This prevents the filter from
                // parking 0.08° away during the soft-start ready phase, which
                // would cause a sudden velocity jump when CompleteSoftStart()
                // restores full limits.  The proportional gain is kept low
                // enough that the filter never overshoots the target.
                //
                // distanceToCommandTarget is bounded by ±targetTolerance
                // (≈ ±0.08°).  Using the soft-start effective position gain
                // (iPositionGain_ * 0.1 ≈ 7.0) keeps the approach speed
                // matched to the soft-start correction dynamics so the
                // velocity does not jump when CompleteSoftStart() removes the
                // soft-start limits.  A lower gain (e.g. 2.0) makes the
                // filter park too far from the target, causing a visible
                // acceleration transient after the transition.
                const double kSoftStartSettleApproachGain = iPositionGain_ * 0.1;
                desiredVel = distanceToCommandTarget * kSoftStartSettleApproachGain;
            }

            // When the actual feedback position has already passed the
            // target, limit velocity in the overshoot direction based on
            // the remaining distance to the PDO hard limit (not the
            // distance past the target).  Using the distance to the hard
            // limit ensures that as actual drifts toward the limit, the
            // allowed velocity shrinks toward zero, preventing the command
            // from being driven into the limit at speed.
            if (iActValue > brakeTarget) {
                const double distanceToHardLimit = std::max(0.0, iMaxPDO_ - iActValue);
                const double brakeToHardLimit = std::sqrt(2.0 * maxSoftStartDecel * distanceToHardLimit);
                if (desiredVel > 0.0) {
                    desiredVel = std::min(desiredVel, brakeToHardLimit);
                }
            } else if (iActValue < brakeTarget) {
                const double distanceToHardLimit = std::max(0.0, iActValue - iMinPDO_);
                const double brakeToHardLimit = std::sqrt(2.0 * maxSoftStartDecel * distanceToHardLimit);
                if (desiredVel < 0.0) {
                    desiredVel = std::max(desiredVel, -brakeToHardLimit);
                }
            }
        }
        const double trackingErrorWindow = ComputeTrackingErrorWindow();
        const double maxDecelMagnitude = std::max(std::abs(iMaxDeceleration_), 1e-9);
        if (postSoftStartLimitPreviewEnabled) {
            if (desiredVel > 0.0) {
                const double remainingDistance = std::max(0.0, motionMaxLimit - oFilteredOutput_);
                const double brakingLimitedVel = std::sqrt(2.0 * maxDecelMagnitude * remainingDistance);
                desiredVel = std::min(desiredVel, brakingLimitedVel);
            } else if (desiredVel < 0.0) {
                const double remainingDistance = std::max(0.0, oFilteredOutput_ - motionMinLimit);
                const double brakingLimitedVel = std::sqrt(2.0 * maxDecelMagnitude * remainingDistance);
                desiredVel = std::max(desiredVel, -brakingLimitedVel);
            }
        }
        if (desiredVel > 0.0) {
            const double remainingDistance = std::max(0.0, (iActValue + trackingErrorWindow) - oFilteredOutput_);
            const double brakingLimitedVel = std::sqrt(2.0 * maxDecelMagnitude * remainingDistance);
            desiredVel = std::min(desiredVel, brakingLimitedVel);
        } else if (desiredVel < 0.0) {
            const double remainingDistance = std::max(0.0, oFilteredOutput_ - (iActValue - trackingErrorWindow));
            const double brakingLimitedVel = std::sqrt(2.0 * maxDecelMagnitude * remainingDistance);
            desiredVel = std::max(desiredVel, -brakingLimitedVel);
        }
        const double desiredAcc = (iCycleTime_ > 0.0) ? (desiredVel - oCurrentVelocity_) / iCycleTime_ : 0.0;
        targetAcc = std::clamp(desiredAcc, -std::abs(iMaxDeceleration_), std::abs(iMaxAcceleration_));

        if (oCurrentVelocity_ > 0.0) {
            const double remainingDistance = std::max(0.0, (iActValue + trackingErrorWindow) - oFilteredOutput_);
            const double stoppingDistance = (oCurrentVelocity_ * oCurrentVelocity_) / (2.0 * maxDecelMagnitude);
            if (stoppingDistance >= remainingDistance) {
                targetAcc = -std::abs(iMaxDeceleration_);
            }
        } else if (oCurrentVelocity_ < 0.0) {
            const double remainingDistance = std::max(0.0, oFilteredOutput_ - (iActValue - trackingErrorWindow));
            const double stoppingDistance = (oCurrentVelocity_ * oCurrentVelocity_) / (2.0 * maxDecelMagnitude);
            if (stoppingDistance >= remainingDistance) {
                targetAcc = std::abs(iMaxDeceleration_);
            }
        }
        if (postSoftStartLimitPreviewEnabled) {
            const double stoppingDistance = (oCurrentVelocity_ * oCurrentVelocity_) / (2.0 * maxDecelMagnitude);
            if (oCurrentVelocity_ > 0.0) {
                const double remainingDistance = std::max(0.0, motionMaxLimit - oFilteredOutput_);
                if (stoppingDistance >= remainingDistance) {
                    targetAcc = -std::abs(iMaxDeceleration_);
                }
            } else if (oCurrentVelocity_ < 0.0) {
                const double remainingDistance = std::max(0.0, oFilteredOutput_ - motionMinLimit);
                if (stoppingDistance >= remainingDistance) {
                    targetAcc = std::abs(iMaxDeceleration_);
                }
            }
        }

        oCurrentVelocity_ += targetAcc * iCycleTime_;
        oFilteredOutput_ += oCurrentVelocity_ * iCycleTime_;
        oCurrentAcceleration_ = targetAcc;
        
        // === Overshoot braking ===
        // If output has passed the target and is still moving farther away, brake immediately.
        // This is a safety catch for large prediction or setpoint transitions.
        static constexpr double OVERSHOOT_THRESHOLD = 0.25; // Small tolerance to avoid chatter near target.
        
        double overshoot = oFilteredOutput_ - effectiveSetpoint;
        bool isOvershooting = false;
        
        // Only brake when output is beyond the target and still moving away from it.
        if (overshoot > OVERSHOOT_THRESHOLD && oCurrentVelocity_ > 0.0) {
            // Output is above target and still moving positive: brake now.
            isOvershooting = true;
        } else if (overshoot < -OVERSHOOT_THRESHOLD && oCurrentVelocity_ < 0.0) {
            // Output is below target and still moving negative: brake now.
            isOvershooting = true;
        }
        
        if (isOvershooting) {
            // Apply the braking effect in this cycle instead of snapping velocity to zero.
            // The old hard reset created a single-cycle discontinuity in output velocity.
            double prevVelocity = oCurrentVelocity_;
            double decelMag = std::abs(iMaxDeceleration_);
            if (decelMag > 0.0 && prevVelocity != 0.0) {
                double brakeStep = decelMag * iCycleTime_;
                double nextVelMag = std::max(0.0, std::abs(prevVelocity) - brakeStep);
                oCurrentVelocity_ = std::copysign(nextVelMag, prevVelocity);
                oCurrentAcceleration_ = (oCurrentVelocity_ - prevVelocity) / std::max(iCycleTime_, 1e-9);

                // Re-apply this cycle's position integration with the braked velocity.
                oFilteredOutput_ -= prevVelocity * iCycleTime_;
                oFilteredOutput_ += oCurrentVelocity_ * iCycleTime_;
            } else {
                oCurrentAcceleration_ = 0.0;
            }

            
        }

    }

    ioPrevEffectiveSetpoint_ = effectiveSetpoint;
    ioPrevActValue_ = iActValue;
    // Track original incoming setpoint (from MMF) for next-cycle comparison
    ioPrevSetValue_ = originalIncomingSet;
    const double plannedVelocityBeforeSafetyClamp = oCurrentVelocity_;

    double finalResult = oFilteredOutput_;
    int trackingClampDirection = 0;
    int leadSuppressedClampDirection = 0;
    bool trackingWindowActive = false;
    double trackingSafetyMin = 0.0;
    double trackingSafetyMax = 0.0;
    if (!ioHoldingPosition_) {
        const double trackingErrorWindow = ComputeTrackingErrorWindow();
        const double trackingMin = iActValue - trackingErrorWindow;
        const double trackingMax = iActValue + trackingErrorWindow;
        trackingWindowActive = true;
        trackingSafetyMin = trackingMin;
        trackingSafetyMax = trackingMax;
        double trackingTarget = finalResult;

        const bool ignoreMinClamp = (stepStartVelocity >= 0.0 && iActValue > stepStartOutput);
        const bool ignoreMaxClamp = (stepStartVelocity < 0.0 && iActValue < stepStartOutput);

        if (trackingTarget > trackingMax) {
            if (!ignoreMaxClamp) {
                trackingClampDirection = +1;
                trackingTarget = trackingMax;
            } else {
                leadSuppressedClampDirection = +1;
            }
        } else if (trackingTarget < trackingMin) {
            if (!ignoreMinClamp) {
                trackingClampDirection = -1;
                trackingTarget = trackingMin;
            } else {
                leadSuppressedClampDirection = -1;
            }
        }

        if (trackingClampDirection < 0 && originalIncomingSet < stepStartOutput && trackingTarget > stepStartOutput) {
            trackingTarget = stepStartOutput;
        } else if (trackingClampDirection > 0 && originalIncomingSet > stepStartOutput && trackingTarget < stepStartOutput) {
            trackingTarget = stepStartOutput;
        }
        finalResult = trackingTarget;
    }

    finalResult = std::clamp(finalResult, motionMinLimit, motionMaxLimit);

    const double dt = std::max(iCycleTime_, 1e-9);
    const double envelopeMaxVel = std::max(std::abs(iMaxVelocity_), std::abs(iConfiguredMaxVelocity_));
    const double envelopeMaxAcc = std::max(std::abs(iMaxAcceleration_), std::abs(iConfiguredMaxAcceleration_));
    const double envelopeMaxDec = std::max(std::abs(iMaxDeceleration_), std::abs(iConfiguredMaxDeceleration_));
    if (envelopeMaxVel > 0.0 && envelopeMaxAcc > 0.0 && envelopeMaxDec > 0.0) {
        double vLow = stepStartVelocity - envelopeMaxDec * dt;
        double vHigh = stepStartVelocity + envelopeMaxAcc * dt;
        vLow = std::max(vLow, -envelopeMaxVel);
        vHigh = std::min(vHigh, envelopeMaxVel);
        if (vLow <= vHigh) {
            const double pLow = std::max(stepStartOutput + vLow * dt, motionMinLimit);
            const double pHigh = std::min(stepStartOutput + vHigh * dt, motionMaxLimit);
            if (pLow <= pHigh) {
                finalResult = std::clamp(finalResult, pLow, pHigh);
            }
        }
    }

    const double safetyBleedStep = std::max(std::abs(iMaxDeceleration_),
                                           std::abs(iConfiguredMaxDeceleration_)) * dt;
    if (trackingClampDirection > 0 || trackingClampDirection < 0) {
        double committedVelocity = (finalResult - stepStartOutput) / dt;
        if (trackingClampDirection > 0 && committedVelocity > 0.0) {
            const double maxOutwardVelocity = std::max(0.0, stepStartVelocity - safetyBleedStep);
            committedVelocity = std::min(committedVelocity, maxOutwardVelocity);
            finalResult = std::clamp(stepStartOutput + committedVelocity * dt, motionMinLimit, motionMaxLimit);
        } else if (trackingClampDirection < 0 && committedVelocity < 0.0) {
            const double maxOutwardVelocity = std::min(0.0, stepStartVelocity + safetyBleedStep);
            committedVelocity = std::max(committedVelocity, maxOutwardVelocity);
            finalResult = std::clamp(stepStartOutput + committedVelocity * dt, motionMinLimit, motionMaxLimit);
        }
    }
    if (leadSuppressedClampDirection != 0) {
        double committedVelocity = (finalResult - stepStartOutput) / dt;
        const double intentSetpoint = std::isfinite(originalIncomingSet) ? originalIncomingSet : effectiveSetpoint;
        const double intentDelta = intentSetpoint - stepStartOutput;
        const bool commandIntentMatchesCommittedMotion =
            (intentDelta > iPositionDeadband_ && committedVelocity > 0.0) ||
            (intentDelta < -iPositionDeadband_ && committedVelocity < 0.0);
        if (!commandIntentMatchesCommittedMotion &&
            ((leadSuppressedClampDirection < 0 && committedVelocity > 0.0) ||
             (leadSuppressedClampDirection > 0 && committedVelocity < 0.0))) {
            committedVelocity = 0.0;
            finalResult = std::clamp(stepStartOutput + committedVelocity * dt, motionMinLimit, motionMaxLimit);
        }
    }

    if (trackingWindowActive) {
        const double trackingProjectionTolerance = std::max(0.1, std::abs(stepStartVelocity) * dt + safetyBleedStep * dt + 1e-6);
        const bool nearTrackingWindow =
            stepStartOutput >= (trackingSafetyMin - trackingProjectionTolerance) &&
            stepStartOutput <= (trackingSafetyMax + trackingProjectionTolerance);
        const bool finalNearTrackingWindow =
            finalResult >= (trackingSafetyMin - trackingProjectionTolerance) &&
            finalResult <= (trackingSafetyMax + trackingProjectionTolerance);
        if (nearTrackingWindow || finalNearTrackingWindow) {
            const double trackingProjectedTarget = std::clamp(finalResult, trackingSafetyMin, trackingSafetyMax);
            if (trackingProjectedTarget != finalResult) {
                const double maxTrackingVelocityStep = 3.0 * safetyBleedStep;
                const double desiredProjectionVelocity = (trackingProjectedTarget - stepStartOutput) / dt;
                const double boundedProjectionVelocity = std::clamp(desiredProjectionVelocity,
                                                                    stepStartVelocity - maxTrackingVelocityStep,
                                                                    stepStartVelocity + maxTrackingVelocityStep);
                finalResult = stepStartOutput + boundedProjectionVelocity * dt;
            }
        }
    }

    finalResult = std::clamp(finalResult, motionMinLimit, motionMaxLimit);
    oFilteredOutput_ = finalResult;

    const double committedOutputVelocity = (finalResult - stepStartOutput) / dt;
    double nextInternalVelocity = committedOutputVelocity;
    double publishedOutputVelocity = committedOutputVelocity;
    if (trackingClampDirection != 0) {
        const double intentDelta = effectiveSetpoint - stepStartOutput;
        const bool heldInputNearCurrentOutput = std::abs(intentDelta) <= std::max(iPositionDeadband_, 1e-6);
        const bool clampMotionOpposesInputIntent =
            (intentDelta > 0.0 && committedOutputVelocity < 0.0) ||
            (intentDelta < 0.0 && committedOutputVelocity > 0.0);
        if (heldInputNearCurrentOutput || clampMotionOpposesInputIntent) {
            nextInternalVelocity = heldInputNearCurrentOutput ? 0.0 : plannedVelocityBeforeSafetyClamp;
            if (trackingClampDirection > 0 && nextInternalVelocity > 0.0) {
                nextInternalVelocity = std::max(0.0, nextInternalVelocity - safetyBleedStep);
            } else if (trackingClampDirection < 0 && nextInternalVelocity < 0.0) {
                nextInternalVelocity = std::min(0.0, nextInternalVelocity + safetyBleedStep);
            }
            // During safety-clamp motion, publish commanded motion intent
            // instead of actual-driven clamp displacement. Soft-start CSV
            // feedforward consumes GetCurrentVelocity().
            publishedOutputVelocity = nextInternalVelocity;
        }
    }

    oCurrentVelocity_ = nextInternalVelocity;
    oCurrentAcceleration_ = (oCurrentVelocity_ - stepStartVelocity) / dt;

    const bool atUpperHardLimit = finalResult >= iMaxPDO_ - 1e-6;
    const bool atLowerHardLimit = finalResult <= iMinPDO_ + 1e-6;
    const bool atUpperMotionLimit = postSoftStartLimitPreviewEnabled && finalResult >= motionMaxLimit - 1e-6;
    const bool atLowerMotionLimit = postSoftStartLimitPreviewEnabled && finalResult <= motionMinLimit + 1e-6;
    if (atUpperHardLimit && (oCurrentVelocity_ > 0.0 || stepStartOutput > iMaxPDO_)) {
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = (oCurrentVelocity_ - stepStartVelocity) / dt;
    } else if (atLowerHardLimit && (oCurrentVelocity_ < 0.0 || stepStartOutput < iMinPDO_)) {
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = (oCurrentVelocity_ - stepStartVelocity) / dt;
    } else if (atUpperMotionLimit && (oCurrentVelocity_ > 0.0 || stepStartOutput > motionMaxLimit)) {
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = (oCurrentVelocity_ - stepStartVelocity) / dt;
    } else if (atLowerMotionLimit && (oCurrentVelocity_ < 0.0 || stepStartOutput < motionMinLimit)) {
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = (oCurrentVelocity_ - stepStartVelocity) / dt;
    }

    ioTrackingClampDirection_ = trackingClampDirection;
    ioPreviousCyclePredicted_ = ioPredictedThisCycle_;

    if ((trackingClampDirection != 0 && publishedOutputVelocity != committedOutputVelocity) ||
        (atUpperHardLimit && publishedOutputVelocity > 0.0) ||
        (atLowerHardLimit && publishedOutputVelocity < 0.0) ||
        (atUpperMotionLimit && publishedOutputVelocity > 0.0) ||
        (atLowerMotionLimit && publishedOutputVelocity < 0.0)) {
        publishedOutputVelocity = oCurrentVelocity_;
    }
    oOutputAcceleration_ = (publishedOutputVelocity - ioPrevOutputVelocity_) / dt;
    oOutputVelocity_ = publishedOutputVelocity;
    ioPrevOutputVelocity_ = oOutputVelocity_;

    // Compute displayed output position from the final result
    // Track previous final output for the next cycle
    ioPrevFilteredOutput_ = finalResult;
    return finalResult;
}

