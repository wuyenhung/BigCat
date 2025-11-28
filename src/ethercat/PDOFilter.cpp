#include "PDOFilter.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// Debug output control - set to 0 for production builds to reduce logging overhead
#ifndef PDOFILTER_DEBUG_LOG
#define PDOFILTER_DEBUG_LOG 0
#endif

#if PDOFILTER_DEBUG_LOG
#define PDOFILTER_DBG(fmt, ...) do { char _bf[384]; sprintf_s(_bf, sizeof(_bf), fmt, __VA_ARGS__); OutputDebugStringA(_bf); } while(0)
#else
#define PDOFILTER_DBG(fmt, ...) ((void)0)
#endif

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
    , ioErrorFlag_(false)
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
    , iPositionDeadband_(1e-5) // Tighter deadband for better precision
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
    ioTrackingErrorStartTime_ = 0.0;
    ioTrackingErrorReferenceAbs_ = 0.0;
    ioNoViolationStartTime_ = 0.0;
    ioViolationClearStartTime_ = 0.0;
    ioViolationActive_ = false;
    ioLimitingEnabled_ = false;
    ioTrackingErrorFatalActive_ = false;
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
    
    // PT2 filter removed

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
    ioErrorFlag_ = false;
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
    ioTrackingErrorStartTime_ = 0.0;
    ioTrackingErrorReferenceAbs_ = 0.0;
    ioNoViolationStartTime_ = 0.0;
    ioViolationClearStartTime_ = 0.0;
    ioViolationActive_ = false;
    ioLimitingEnabled_ = false;
    ioTrackingErrorFatalActive_ = false;
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
    // PT2 state cleared (filter removed)
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
    // PT2 removed: no coefficients to recompute
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
    ioSoftStartReady_ = false;
    ioSoftStartStartTime_ = std::chrono::steady_clock::now();

    // Reset settle-timer state so a fresh soft-start doesn't inherit stale
    // "position was close" from a previous (possibly aborted) run.
    ioPositionWasClose_ = false;
    ioPositionCloseStartTime_ = 0.0;

    // Ensure we start with a clean slate regarding violations
    ioLimitingEnabled_ = false;
    ioViolationActive_ = false;
    ioViolationDetectionSuspended_ = false;
    ioViolationStartTime_ = 0.0;
    ioVelocityViolationStartTime_ = 0.0;
    ioAccelViolationStartTime_ = 0.0;
    ioTrackingErrorStartTime_ = 0.0;
    ioTrackingErrorReferenceAbs_ = 0.0;
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
    ioTrackingErrorFatalActive_ = false;

    iMaxVelocity_ = iConfiguredMaxVelocity_ * SOFT_START_VEL_RATIO;
    iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
    iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
    // disable any in-progress blending (we're entering soft-start again)
    ioVelocityBlendActive_ = false;
    // Reset any previous recovery-ramp state so each new soft-start really
    // starts from soft-start limits (prevents reusing max accel/decel).
    ioAccelRampActive_ = false;
    ioAccelRampStartTime_ = std::chrono::steady_clock::time_point();
    ioAccelRampStartVel_ = 0.0;
    ioAccelRampStartAcc_ = 0.0;
    ioAccelRampStartDec_ = 0.0;
    ioSoftStartCompleteTime_ = std::chrono::steady_clock::time_point();
    
    // Save original PDO limits for restoration after soft-start completes
    ioOriginalMinPDO_ = iMinPDO_;
    ioOriginalMaxPDO_ = iMaxPDO_;
    ioSoftStartLimitsAdjusted_ = false;
    
    // Force re-initialization on next Filter() call to sync with actual feedback position.
    // This prevents jumps if the filter was previously initialized at a different position.
    ioInitialized_ = false;
}

void PDOFilter::ClearError() {
    ioErrorFlag_ = false;
    ioTrackingErrorFatalActive_ = false;
    ioTrackingErrorStartTime_ = 0.0;
    ioTrackingErrorReferenceAbs_ = 0.0;
}

bool PDOFilter::IsError() const {
    return ioErrorFlag_;
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
    
    // Restore original PDO limits if they were adjusted during soft-start
    if (ioSoftStartLimitsAdjusted_) {
        iMinPDO_ = ioOriginalMinPDO_;
        iMaxPDO_ = ioOriginalMaxPDO_;
        ioSoftStartLimitsAdjusted_ = false;
        PDOFILTER_DBG("PDOFilter: Restored original limits min=%.3f max=%.3f\n", iMinPDO_, iMaxPDO_);
    }
    
    // Start or confirm the accel/decel ramp to restore full limits.
    // If we haven't started the ramp yet (e.g. didn't hit the 5 deg threshold), start it now.
    if (!ioAccelRampActive_) {
        ioAccelRampActive_ = true;
        ioAccelRampStartTime_ = std::chrono::steady_clock::now();
        ioAccelRampStartVel_ = iMaxVelocity_;
        ioAccelRampStartAcc_ = iMaxAcceleration_;
        ioAccelRampStartDec_ = iMaxDeceleration_;
    }
    // Note: iMaxVelocity_ etc. will be interpolated in Filter()
    ioAccelCheckSkipCount_ = 5;
    // Ensure previous filtered output matches current state (PT2 removed)
    ioPrevFilteredOutput_ = oFilteredOutput_;
    // Begin a short blend period to smoothly transition the velocity source
    // from the value used during soft-start (raw) to the smoothed velocity.
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
    // PT2 filter removed, so position is simply the filtered output.
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
    // Verbose debug hook: log periodically for diagnostic runs (disabled in production)
#if PDOFILTER_DEBUG_LOG
    if ((++ioDebugLogCounter_ % 50) == 0) {
        PDOFILTER_DBG("PDOFilterDbg: pos=%.6f targ=%.6f dist=%.6f ff=%.6f desVel=%.6f curVelUsed=%.6f measuredFlag=%d desAcc=%.6f src=preFilt blend=0\n",
                  position, targetPos, distance, feedforwardVel, desiredVel, currentVelForControl, ioMeasuredVelocityProvided_?1:0, desiredAccel);
    }
#endif
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

    const bool latchedErrorAtEntry = ioErrorFlag_;
    if (latchedErrorAtEntry) {
        iSetValue = std::clamp(iActValue, iMinPDO_, iMaxPDO_);
        originalIncomingSet = iSetValue;
    }

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
        ioTrackingErrorStartTime_ = 0.0;
        ioTrackingErrorReferenceAbs_ = 0.0;
        ioNoViolationStartTime_ = 0.0;
        ioViolationClearStartTime_ = 0.0;
        ioViolationActive_ = false;
        ioLimitingEnabled_ = false;
        ioTrackingErrorFatalActive_ = false;
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
    bool trackingErrorFatalTriggered = latchedErrorAtEntry;
    bool forceViolationCallback = latchedErrorAtEntry;
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
    const bool anomalyGuardEnabled = !latchedErrorAtEntry &&
                                     (!inputStateKnown || allowInputPrediction) &&
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
            
            // Emit detailed anomaly diagnostics when debug logging is enabled.
#if PDOFILTER_DEBUG_LOG
            char bf_anomaly[256];
            sprintf_s(bf_anomaly, sizeof(bf_anomaly), 
                     "PDOFilter: Input anomaly detected! Original=%.6f, Change=%.6f, MaxAllowed=%.6f, Replaced=%.6f\n",
                     originalIncomingSet, inputChange, maxAllowedChange, replacementValue);
            OutputDebugStringA(bf_anomaly);
            printf("%s", bf_anomaly);

            // Include internal state so unexpected replacements can be diagnosed.
            char bf_state[384];
            sprintf_s(bf_state, sizeof(bf_state),
                      "PDOFilter: AnomalyState: prevSet=%.6f prevSmoothVel=%.6f extrapolVel=%.6f cycle=%.6f maxAccelLimit=%.6f cfgMaxAcc=%.6f cfgMaxDec=%.6f\n",
                      ioPrevSetValue_, ioPrevSmoothedVelocity_, extrapolationVel, iCycleTime_, maxAccelLimit, iConfiguredMaxAcceleration_, iConfiguredMaxDeceleration_);
            OutputDebugStringA(bf_state);
            printf("%s", bf_state);
#endif

#if PDOFILTER_DEBUG_LOG
            char bf_anomaly[512];
            sprintf_s(bf_anomaly, sizeof(bf_anomaly), 
                     "PDOFilter: Input anomaly detected! Original=%.6f, Change=%.6f, MaxAllowed=%.6f, Replaced=%.6f, Reason=%s\n",
                     originalIncomingSet, inputChange, maxAllowedChange, replacementValue, PDOFilter::InputAnomalyReasonToString(anomalyReason));
            OutputDebugStringA(bf_anomaly);
            printf("%s", bf_anomaly);
#endif

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
        trackingErrorTimerActive = directTrackingViolation || softStartSaturatedTracking || ioTrackingErrorFatalActive_;

        if (directTrackingViolation) {
            trackingErrorViolation = true;
            if (!ioSoftStartActive_) {
                if (ioTrackingErrorStartTime_ <= 0.0) {
                    ioTrackingErrorStartTime_ = currentTime;
                    ioTrackingErrorReferenceAbs_ = trackingErrorAbs;
                } else if (!ioTrackingErrorFatalActive_ &&
                           (currentTime - ioTrackingErrorStartTime_) >= TRACKING_ERROR_FATAL_DELAY_SEC) {
                    ioTrackingErrorFatalActive_ = true;
                    ioErrorFlag_ = true;
                    trackingErrorFatalTriggered = true;
                    forceViolationCallback = true;
                }
            } else {
                ioTrackingErrorStartTime_ = 0.0;
                ioTrackingErrorReferenceAbs_ = 0.0;
            }
            const double guardedSetpoint = std::clamp(originalIncomingSet,
                                                      iActValue - trackingErrorWindow,
                                                      iActValue + trackingErrorWindow);
            applyInputGuard(PDOFilter::InputAnomalyReason::MeasurementMismatch,
                            trackingErrorAbs,
                            trackingErrorWindow,
                            guardedSetpoint);

#if PDOFILTER_DEBUG_LOG
            char bf_mismatch[320];
            sprintf_s(bf_mismatch, sizeof(bf_mismatch),
                      "PDOFilter: Tracking mismatch guarded. cmd=%.6f act=%.6f err=%.6f win=%.6f guarded=%.6f\n",
                      issuedCommand, iActValue, trackingErrorAbs, trackingErrorWindow, guardedSetpoint);
            OutputDebugStringA(bf_mismatch);
            printf("%s", bf_mismatch);
#endif
        } else if (softStartSaturatedTracking) {
            trackingErrorViolation = false;
            ioTrackingErrorStartTime_ = 0.0;
            ioTrackingErrorReferenceAbs_ = 0.0;

#if PDOFILTER_DEBUG_LOG
            char bf_soft_start_sat[384];
            sprintf_s(bf_soft_start_sat, sizeof(bf_soft_start_sat),
                      "PDOFilter: Soft-start saturation tracking. cmd=%.6f eff=%.6f act=%.6f upErr=%.6f ref=%.6f win=%.6f\n",
                      issuedCommand, issuedEffectiveSetpoint, iActValue, upstreamTrackingErrorAbs, ioTrackingErrorReferenceAbs_, trackingErrorWindow);
            OutputDebugStringA(bf_soft_start_sat);
            printf("%s", bf_soft_start_sat);
#endif
        } else if (!ioTrackingErrorFatalActive_) {
            ioTrackingErrorStartTime_ = 0.0;
            ioTrackingErrorReferenceAbs_ = 0.0;
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
        double positionError = std::abs(iSetValue - iActValue);
        bool isPositionClose = (positionError < SOFT_START_POSITION_TOLERANCE);
        if (isPositionClose) {
            if (!ioPositionWasClose_) {
                ioPositionCloseStartTime_ = ioSoftStartElapsedSec_;
                ioPositionWasClose_ = true;
            } else if ((ioSoftStartElapsedSec_ - ioPositionCloseStartTime_) >= SOFT_START_SETTLE_TIME) {
                ioSoftStartReady_ = true;
            }
        } else { ioPositionWasClose_ = false; }

        // New trigger: if we are within 1.5 degrees, start recovery ramp early 
        // to avoid waiting for the final 0.1 degree settle.
        if (!ioAccelRampActive_ && positionError < 1.5) {
            ioAccelRampActive_ = true;
            ioAccelRampStartTime_ = now;
            ioAccelRampStartVel_ = iMaxVelocity_;
            ioAccelRampStartAcc_ = iMaxAcceleration_;
            ioAccelRampStartDec_ = iMaxDeceleration_;
            PDOFILTER_DBG("PDOFilter: Early ramp triggered (err=%.3f)\n", positionError);
        }
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
                    PDOFILTER_DBG("PDOFilter: Soft-start adjusted iMinPDO to actual position %.3f (was %.3f)\n", 
                             iActValue, ioOriginalMinPDO_);
                }
                if (needAdjustMax) {
                    iMaxPDO_ = iActValue;
                    PDOFILTER_DBG("PDOFilter: Soft-start adjusted iMaxPDO to actual position %.3f (was %.3f)\n", 
                             iActValue, ioOriginalMaxPDO_);
                }
                ioSoftStartLimitsAdjusted_ = true;
            }
            
            // Initialize the effective setpoint to the actual position to prevent immediate velocity spike
            ioPrevEffectiveSetpoint_ = iActValue;
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
        // PT2 removed: track previous filtered output directly
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
    bool isInputStale = inputStateKnown ? !inputUpdated : !valueChanged;

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

    // Calculate smoothed input
    double smoothedInput = 0.0;
    for (size_t i = 0; i < ioInputBufferCount_; ++i) smoothedInput += ioInputBuffer_[i];
    smoothedInput /= static_cast<double>(ioInputBufferCount_);

    // Smoothing Strategy: 
    // In CSP mode, we always use a small amount of smoothing to filter out discretization noise
    // from the MMF layer, unless we are in soft-start or predicting.
    // If we are predicting, we MUST bypass smoothing because the smoothed value will lag
    // behind the predicted value (which is moving), creating a negative feedback loop 
    // on velocity (braking) which leads to oscillation.
    double effectiveInput;
    const bool bridgeFromPrediction = previousCyclePredicted && !ioPredictedThisCycle_ && !isInputStale &&
                                      ioInputBufferCount_ >= INPUT_MA_SIZE;
    if (ioSoftStartActive_ || ioPredictedThisCycle_) {
        effectiveInput = iSetValue;
    } else if (ioVelocityBlendActive_) {
        double elapsed = std::chrono::duration<double>(now - ioSoftStartCompleteTime_).count();
        if (elapsed >= SOFT_START_VEL_BLEND_TIME) {
            ioVelocityBlendActive_ = false;
            effectiveInput = smoothedInput;
        } else {
            // Smoothly blend from Raw (iSetValue) to Smoothed (smoothedInput)
            // alpha goes from 1.0 (Raw) down to 0.0 (Smoothed)
            double alpha = 1.0 - (elapsed / SOFT_START_VEL_BLEND_TIME);
            effectiveInput = alpha * iSetValue + (1.0 - alpha) * smoothedInput;
        }
    } else if (bridgeFromPrediction) {
        // Just transitioned from prediction back to normal mode:
        // We must avoid jumping from the 'future' predicted value back to the 'lagging' smoothed value.
        // For one cycle, we use a hybrid to bridge the gap.
        effectiveInput = (iSetValue + smoothedInput) * 0.5;
    } else {
        effectiveInput = smoothedInput;
    }

    if (!ioSoftStartActive_ && !ioPredictedThisCycle_ && !ioVelocityBlendActive_ && !bridgeFromPrediction) {
        constexpr double INPUT_MA_LEAD_RATIO = 0.75;
        const double compensatedInput = smoothedInput + (iSetValue - smoothedInput) * INPUT_MA_LEAD_RATIO;
        const double envelopeMin = std::min(smoothedInput, iSetValue);
        const double envelopeMax = std::max(smoothedInput, iSetValue);
        effectiveInput = std::clamp(compensatedInput, envelopeMin, envelopeMax);
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
            ioSoftStartCommandVelocity_ = std::clamp(commandInputVelocity, -iMaxVelocity_, iMaxVelocity_);
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
        PDOFILTER_DBG("PDOFilter: Suspended detection (vel=%d acc=%d)\n", velocityViolation?1:0, candidateAccelViolation?1:0);
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

    // Diagnostic logging for acceleration check (throttled)
#if PDOFILTER_DEBUG_LOG
    if ((++ioDebugLogCounter_ % 50) == 0 || candidateAccelViolation || ioLimitingEnabled_) {
        char bf[256];
        sprintf_s(bf, sizeof(bf), "AccChk: t=%.3f rawA=%.3f smoothA=%.3f buf=%zu cand=%d lim=%d skip=%d atol=%.3f vtol=%.3f\n",
              currentTime, ioLastRawAcceleration_, smoothedAcceleration, ioVelocityBufferCount_, candidateAccelViolation?1:0, ioLimitingEnabled_?1:0, ioAccelCheckSkipCount_, ioAccelThresholdScale_, ioVelocityThresholdScale_);
        OutputDebugStringA(bf);
        // Also print to console to surface diagnostics during tests
        if (candidateAccelViolation || ioLimitingEnabled_) {
            printf("%s", bf);
        }
    }
#endif

    bool hasAccelViolation = candidateAccelViolation;
    
    double violationDuration = 0.0;
    if ((hasHardLimitMin || hasHardLimitMax) && !ioSoftStartActive_) {
        // Hard position limits are safety boundaries; keep the stricter crawl envelope here.
        ioLimitingEnabled_ = true; ioViolationActive_ = true; ioNoViolationStartTime_ = 0.0;
        iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
        iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
        iMaxVelocity_ = iConfiguredMaxVelocity_ * SOFT_START_VEL_RATIO;
        if (ioViolationStartTime_ <= 0.0) ioViolationStartTime_ = currentTime;
        violationDuration = currentTime - ioViolationStartTime_;
    } else if (velocityViolation || hasAccelViolation) {
        ioNoViolationStartTime_ = 0.0; ioViolationActive_ = true;
        if (velocityViolation) {
            if (ioVelocityViolationStartTime_ <= 0.0) {
                ioVelocityViolationStartTime_ = currentTime;
                // Surface candidate detection immediately to logs so it's visible in non-debug runs
                PDOFILTER_DBG("PDOFilter: Velocity candidate detected rawVel=%.3f thresh=%.3f t=%.3f\n", rawVelocity, velThreshold, currentTime);
            }
            if ((currentTime - ioVelocityViolationStartTime_) >= ioVelocityViolationDelaySec_) {
                const bool wasLimitingEnabled = ioLimitingEnabled_;
                if (!ioSoftStartActive_) {
                    ioLimitingEnabled_ = true;
                    // Preserve configured accel/decel for this axis' own
                    // dynamic violation so slowed/reversed MMF can brake old
                    // motion in either sign. Global-only limiting remains
                    // softer in SetGlobalLimitingEnabled().
                    iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                    iMaxDeceleration_ = iConfiguredMaxDeceleration_;
                    iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
                    // Suspend further violation detection until this violation clears
                    ioViolationDetectionSuspended_ = true;
                    // Log the limiting action so it's observable
                    if (!wasLimitingEnabled) {
                        PDOFILTER_DBG("PDOFilter: Velocity limiting engaged iMaxVel=%.3f iMaxAcc=%.3f iMaxDec=%.3f\n",
                                  iMaxVelocity_, iMaxAcceleration_, iMaxDeceleration_);
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
                    // Transition into limiting due to sustained accel violation:
                    // Preserve configured accel/decel for this axis' own
                    // dynamic violation so changed MMF commands can brake
                    // away from the old direction in either sign.
                    ioLimitingEnabled_ = true;
                    iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                    iMaxDeceleration_ = iConfiguredMaxDeceleration_;
                    iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
                    // Suspend further violation detection until this violation clears
                    ioViolationDetectionSuspended_ = true;
                    // Log the distinct limiting action for clarity
                    if (!wasLimitingEnabled) {
                        PDOFILTER_DBG("PDOFilter: Accel limiting engaged iMaxAcc=%.3f iMaxDec=%.3f iMaxVel=%.3f\n",
                                  iMaxAcceleration_, iMaxDeceleration_, iMaxVelocity_);
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
            PDOFILTER_DBG("PDOFilter: Stale/Anomaly limiting engaged (stale=%d anomaly=%d) iMaxVel=%.3f\n",
                      staleViolation?1:0, anomalyViolation?1:0, iMaxVelocity_);
        }
        if (ioViolationStartTime_ <= 0.0) ioViolationStartTime_ = currentTime;
        violationDuration = currentTime - ioViolationStartTime_;
    } else if (trackingErrorTimerActive || ioTrackingErrorFatalActive_) {
        ioNoViolationStartTime_ = 0.0;
        ioViolationActive_ = true;
        if (ioTrackingErrorStartTime_ <= 0.0) ioTrackingErrorStartTime_ = currentTime;
        violationDuration = currentTime - ioTrackingErrorStartTime_;
    } else {
        ioViolationActive_ = false; ioViolationStartTime_ = ioVelocityViolationStartTime_ = ioAccelViolationStartTime_ = 0.0;
        ioStaleViolationStartTime_ = 0.0; ioAnomalyViolationStartTime_ = 0.0;
        if (!ioTrackingErrorFatalActive_) {
            ioTrackingErrorStartTime_ = 0.0;
            ioTrackingErrorReferenceAbs_ = 0.0;
        }
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
            ioTrackingErrorFatalActive_ || (trackingErrorViolation && !ioSoftStartActive_);
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
            !ioTrackingErrorFatalActive_ &&
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
        } else if (vType != ioPrevViolationType_ || forceViolationCallback) {
            // Notify the UI with smoothed values when the violation TYPE changes.
            ioViolationCallback_(vType, smoothedVelocity, smoothedAcceleration, ioLimitingEnabled_, callbackViolationDuration);
        }

        ioPrevViolationType_ = vType;
    }
    ioPrevLimitingEnabled_ = ioLimitingEnabled_;
    
    if (!ioLimitingEnabled_) {
        if (ioAccelRampActive_) {
            // Interpolate velocity/accel/decel from their starting values to configured over ramp duration
            double rampElapsed = std::chrono::duration<double>(now - ioAccelRampStartTime_).count();
            double alpha = (ioAccelRampDurationSec_ > 0.0) ? (rampElapsed / ioAccelRampDurationSec_) : 1.0;
            if (alpha >= 1.0) {
                // Ramp complete: ensure all at configured values
                iMaxVelocity_ = iConfiguredMaxVelocity_;
                iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                iMaxDeceleration_ = iConfiguredMaxDeceleration_;
                ioAccelRampActive_ = false;
            } else {
                // linear interpolation
                iMaxVelocity_ = (1.0 - alpha) * ioAccelRampStartVel_ + alpha * iConfiguredMaxVelocity_;
                iMaxAcceleration_ = (1.0 - alpha) * ioAccelRampStartAcc_ + alpha * iConfiguredMaxAcceleration_;
                iMaxDeceleration_ = (1.0 - alpha) * ioAccelRampStartDec_ + alpha * iConfiguredMaxDeceleration_;
            }
        } else if (!ioSoftStartActive_) {
            // No ramp active and not in soft start: ensure values are at configured max
            iMaxVelocity_ = iConfiguredMaxVelocity_;
            iMaxAcceleration_ = iConfiguredMaxAcceleration_;
            iMaxDeceleration_ = iConfiguredMaxDeceleration_;
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
        double trackingClampedSetpoint = std::clamp(effectiveSetpoint,
                                                    iActValue - trackingErrorWindow,
                                                    iActValue + trackingErrorWindow);
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
        
        auto [desiredVel, targetAcc] = ComputeOnlineTargetVelAccel(effectiveSetpoint, feedforwardVel);
        if (ioSoftStartActive_ && std::isfinite(originalIncomingSet)) {
            const double brakeTarget = std::clamp(originalIncomingSet, iMinPDO_, iMaxPDO_);
            const double distanceToCommandTarget = brakeTarget - stepStartOutput;
            const double targetTolerance = std::max(SOFT_START_POSITION_TOLERANCE, iPositionDeadband_);
            const double maxSoftStartDecel = std::max(std::abs(iMaxDeceleration_), 1e-9);
            const double remainingDistance = std::max(0.0, std::abs(distanceToCommandTarget) - targetTolerance);
            const double targetBrakeVelocity = std::sqrt(2.0 * maxSoftStartDecel * remainingDistance);
            // Enforce a minimum creep speed floor so the axis can always converge
            // when very close to the target. Without this, the braking curve drives
            // velocity toward zero as remainingDistance -> 0, causing either
            // oscillation (feedback fights the tightening ceiling) or asymptotic
            // stall (can never cross the final tolerance boundary).
            const double minCreepVel = iMaxVelocity_ * 0.08;
            const double effectiveBrakeVelocity = std::max(targetBrakeVelocity, minCreepVel);
            
            const double rawTargetInputVel = ioSoftStartCommandVelocity_;
            const double targetInputVel = std::clamp(rawTargetInputVel, -iMaxVelocity_, iMaxVelocity_);
            
            if (distanceToCommandTarget > targetTolerance) {
                desiredVel = std::clamp(desiredVel, -iMaxVelocity_, targetInputVel + effectiveBrakeVelocity);
            } else if (distanceToCommandTarget < -targetTolerance) {
                desiredVel = std::clamp(desiredVel, targetInputVel - effectiveBrakeVelocity, iMaxVelocity_);
            } else {
                desiredVel = targetInputVel;
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
#if PDOFILTER_DEBUG_LOG
            char bf_os[256];
            sprintf_s(bf_os, sizeof(bf_os), "PDOFilter: OVERSHOOT DETECTED (+)! out=%.3f target=%.3f overshoot=%.3f vel=%.3f -> IMMEDIATE STOP\n",
                      oFilteredOutput_, effectiveSetpoint, overshoot, oCurrentVelocity_);
            OutputDebugStringA(bf_os);
            printf("%s", bf_os);
#endif
        } else if (overshoot < -OVERSHOOT_THRESHOLD && oCurrentVelocity_ < 0.0) {
            // Output is below target and still moving negative: brake now.
            isOvershooting = true;
#if PDOFILTER_DEBUG_LOG
            char bf_os[256];
            sprintf_s(bf_os, sizeof(bf_os), "PDOFilter: OVERSHOOT DETECTED (-)! out=%.3f target=%.3f overshoot=%.3f vel=%.3f -> IMMEDIATE STOP\n",
                      oFilteredOutput_, effectiveSetpoint, overshoot, oCurrentVelocity_);
            OutputDebugStringA(bf_os);
            printf("%s", bf_os);
#endif
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

            PDOFILTER_DBG("PDOFilter: OVERSHOOT DETECTED -> BRAKING with max decel=%.3f out=%.3f target=%.3f overshoot=%.3f vel=%.3f acc=%.3f\n",
                      iMaxDeceleration_, oFilteredOutput_, effectiveSetpoint, overshoot, oCurrentVelocity_, oCurrentAcceleration_);
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

        // Handle physical deceleration lead:
        // Do not force command to push outward when feedback already leads/overshoots the command in the direction of motion.
        // Doing so would feed positive feedback into the velocity output, destroying deceleration convergence and causing runaway oscillation.
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

    if (trackingErrorFatalTriggered) {
        finalResult = std::clamp(iActValue, iMinPDO_, iMaxPDO_);
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
    if (trackingErrorFatalTriggered) {
        oCurrentVelocity_ = 0.0;
        oCurrentAcceleration_ = 0.0;
    }
    ioPreviousCyclePredicted_ = ioPredictedThisCycle_;

    if ((trackingClampDirection != 0 && publishedOutputVelocity != committedOutputVelocity) ||
        trackingErrorFatalTriggered ||
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
    // (PT2 removed: finalResult == filtered output)
#if PDOFILTER_DEBUG_LOG
    if ((++ioDebugLogCounter_ % 200) == 0 || ioLimitingEnabled_) {
        char bf[512];
        sprintf_s(bf, sizeof(bf), "PDOFilter: t=%.3f set=%.6f act=%.6f preFilt=%.6f final=%.6f intVel=%.6f outVel=%.6f rawVel=%.6f smoothVel=%.6f lim=%d pred=%d\n",
                  currentTime, originalIncomingSet, iActValue, oFilteredOutput_, finalResult, oCurrentVelocity_, oOutputVelocity_, rawVelocity, smoothedVelocity, ioLimitingEnabled_?1:0, ioPredictedThisCycle_?1:0);
        OutputDebugStringA(bf);
    }
#endif

    // Track previous final output for the next cycle
    ioPrevFilteredOutput_ = finalResult;
    return finalResult;
}

// PT2 filter implementation removed.

