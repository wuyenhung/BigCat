#include "EtherCATMaster.h"
#include "MmfPose6DOrder.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
#include <intrin.h>
#include <limits>
#include <mmsystem.h>
#include <soem/ec_print.h>
#include <fstream>
#include "SafeMem.h"
static std::wstring FormatPDOValueString(int32_t value, uint8_t bitLength) {
    wchar_t buf[64] = {0};
    if (bitLength == 16) {
        swprintf_s(buf, L"%d (0x%04X)", value, static_cast<unsigned>(value & 0xFFFF));
    } else {
        swprintf_s(buf, L"%d (0x%08X)", value, static_cast<unsigned>(static_cast<uint32_t>(value)));
    }
    return std::wstring(buf);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
using CreateWaitableTimerExWFn = HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

static HANDLE CreateHighResolutionTimerHandle()
{
    static CreateWaitableTimerExWFn createExFn = reinterpret_cast<CreateWaitableTimerExWFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW"));
    if (createExFn) {
        HANDLE h = createExFn(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (h) return h;
    }
    return CreateWaitableTimer(nullptr, FALSE, nullptr);
}

static bool TryGetIOmapOffset(const uint8_t* ptr, const uint8_t* base, size_t mapSize, int& outOffset)
{
    if (!ptr || !base || mapSize == 0) return false;
    const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    if (p < b || p >= (b + mapSize)) return false;
    outOffset = static_cast<int>(p - b);
    return true;
}

namespace {
    bool TryFindDescriptorByIndex(const std::vector<PDOChannelDescriptor>& descriptors, uint16_t index, uint8_t subindex, PDOChannelDescriptor& outDesc) {
        for (const auto &desc : descriptors) {
            if (desc.index == index && desc.subindex == subindex) {
                outDesc = desc;
                return true;
            }
        }
        return false;
    }

    constexpr int DefaultCyclicCommandFallbackOffset()
    {
        return (CoreHelpers::kDefaultCyclicCommandPdoIndex == 0x60FF) ? 6 : 2;
    }

    constexpr double kSoftStartCsvCorrectionMargin = 1.35;
    constexpr double kCsvCorrectionFilterTauSec = 0.025;
    constexpr double kCsvPostSoftStartCorrectionFilterTauSec = 0.120;
    constexpr double kCsvActualVelocityFilterTauSec = 0.040;
    constexpr double kCsvCorrectionAccelRatio = 0.5;
    constexpr double kCsvPostSoftStartCorrectionAccelRatio = 0.08;
    constexpr double kCsvPositionCorrectionGainRatio = 0.18;
    constexpr double kCsvSoftStartPullInPositionCorrectionGainRatio = 0.35;
    constexpr double kCsvHoldPositionCorrectionGainRatio = 0.04;
    constexpr double kCsvCorrectionVelocityDampingRatio = 0.16;
    constexpr double kCsvPostSoftStartHoldVelocityDampingRatio = 0.06;
    constexpr double kCsvSoftStartCorrectionMaxVelocityRatio = 0.12;
    constexpr double kCsvCorrectionMaxVelocityRatio = 0.02;
    constexpr double kCsvSoftStartPullInCorrectionMaxVelocityRatio = 0.40;
    constexpr double kCsvHoldCorrectionMaxVelocityRatio = 0.025;
    constexpr double kCsvSoftStartRecoveryHoldCorrectionMaxVelocityRatio = 0.020;
    constexpr double kCsvSoftStartCorrectionFeedforwardRatio = 0.30;
    constexpr double kCsvCorrectionFeedforwardRatio = 0.10;
    constexpr double kSimulationCsvPositionGainScale = 0.30;

    struct CsvVelocityCommandState {
        bool initialized{false};
        double smoothedPositionError{0.0};
        double smoothedActualVelocity{0.0};
        double correctionVelocity{0.0};
        double commandVelocity{0.0};
        int trackingClampIntentDir{0};

        void Reset()
        {
            initialized = false;
            smoothedPositionError = 0.0;
            smoothedActualVelocity = 0.0;
            correctionVelocity = 0.0;
            commandVelocity = 0.0;
            trackingClampIntentDir = 0;
        }
    };

    bool ReadDescriptorValue(const ecx_contextt& context, int slaveIndex, const std::vector<uint8_t>& snapshot,
                             const PDOChannelDescriptor& desc, int32_t &outValue)
    {
        if (slaveIndex <= 0) return false;
        outValue = 0;
        int outputSize = 0;
        if (slaveIndex <= context.slavecount) {
            outputSize = context.slavelist[slaveIndex].Obytes;
        }
        if (outputSize == 0) {
            outputSize = (int)snapshot.size();
        }

        int bytes = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : 4;
        if (bytes > 4) bytes = 4;
        int offset = desc.isOutput ? desc.byteOffset : (outputSize + desc.byteOffset);
        if (offset < 0 || offset + bytes > (int)snapshot.size()) return false;
        uint32_t raw = 0;
        for (int b = 0; b < bytes; ++b) {
            raw |= (uint32_t)snapshot[offset + b] << (8 * b);
        }
        outValue = (int32_t)raw;
        return true;
    }

    std::wstring FormatMechanicalValueString(double value) {
        wchar_t buf[64] = {0};
        swprintf_s(buf, L"%.4f", value);
        return std::wstring(buf);
    }

    bool IsMechanicalPositionObject(uint16_t index) {
        return index == 0x6064 || index == 0x607A;
    }

    double NormalizeMechanicalPositionDegrees(double value) {
        if (!std::isfinite(value)) return value;
        double wrapped = std::fmod(value, 360.0);
        if (wrapped > 180.0) wrapped -= 360.0;
        if (wrapped < -180.0) wrapped += 360.0;
        if (std::abs(wrapped) < 1e-12) return 0.0;
        return wrapped;
    }

    double ScalePDOToMechanicalContinuous(int32_t raw, double scale, bool invertSign) {
        const double sign = invertSign ? -1.0 : 1.0;
        return static_cast<double>(raw) * scale * sign;
    }

    double ScalePDOToMechanical(int32_t raw, double scale, bool invertSign, uint16_t index) {
        double value = ScalePDOToMechanicalContinuous(raw, scale, invertSign);
        if (IsMechanicalPositionObject(index)) {
            value = NormalizeMechanicalPositionDegrees(value);
        }
        return value;
    }

    double UnwrapMechanicalPositionNear(double normalizedPosition, double referencePosition) {
        if (!std::isfinite(normalizedPosition) || !std::isfinite(referencePosition)) {
            return normalizedPosition;
        }
        const double turns = std::round((referencePosition - normalizedPosition) / 360.0);
        return normalizedPosition + turns * 360.0;
    }

    std::pair<double, double> GetCsvMotionLimits(const PDOFilter& filter) {
        auto [motionMinLimit, motionMaxLimit] = filter.GetPDOLimits();
        const double margin = PDOFilter::POST_SOFT_START_LIMIT_PREVIEW_MARGIN_DEG;
        if (!filter.IsSoftStartActive() && !filter.IsHoldingPosition() &&
            std::isfinite(motionMinLimit) && std::isfinite(motionMaxLimit) &&
            (motionMaxLimit - motionMinLimit) > (2.0 * margin)) {
            motionMinLimit += margin;
            motionMaxLimit -= margin;
        }
        return {motionMinLimit, motionMaxLimit};
    }

    double ComputeCsvVelocityCommand(const PDOFilter& filter,
                                     double filteredOutput,
                                     double actualPosition,
                                     double actualVelocity,
                                     CsvVelocityCommandState& state,
                                     double cycleSeconds,
                                     double positionGainScale = 1.0) {
        // Use the guarded output velocity during soft-start so 0x60FF stays
        // within the tightened startup envelope. Once online, use the smooth
        // planned filter velocity instead of clamp-disturbed output deltas.
        double feedforwardVelocity = filter.IsSoftStartActive()
            ? filter.GetCurrentVelocity()
            : filter.GetInternalVelocity();
        if (!std::isfinite(feedforwardVelocity)) {
            feedforwardVelocity = 0.0;
        }

        if (!std::isfinite(filteredOutput) || !std::isfinite(actualPosition)) {
            state.Reset();
            return feedforwardVelocity;
        }

        const double safeCycleSeconds = (std::isfinite(cycleSeconds) && cycleSeconds > 0.0)
            ? cycleSeconds
            : 0.002;
        const bool actualVelocityAvailable = std::isfinite(actualVelocity);

        double maxVelocity = (std::max)(0.0, filter.GetMaxVelocity());
        if (filter.IsSoftStartActive()) {
            maxVelocity = (std::min)(
                maxVelocity,
                (std::max)(0.0, filter.GetSoftStartMaxVelocity() * kSoftStartCsvCorrectionMargin));
        }
        if (maxVelocity <= 0.0) {
            state.Reset();
            return 0.0;
        }

        double positionError = filteredOutput - actualPosition;
        const double deadband = filter.GetPositionDeadband();
        if (std::abs(positionError) <= deadband) {
            positionError = 0.0;
        }
        const double feedforwardActiveEps = (std::max)(deadband / safeCycleSeconds, 1e-6);
        const double leadErrorEps = (std::max)(deadband, maxVelocity * safeCycleSeconds * 0.05);
        const int feedbackLeadOutwardDir = (positionError < -leadErrorEps)
            ? +1
            : ((positionError > leadErrorEps) ? -1 : 0);
        const bool feedbackLeadAgainstMovingFeedforward =
            (feedbackLeadOutwardDir > 0 && feedforwardVelocity > feedforwardActiveEps) ||
            (feedbackLeadOutwardDir < 0 && feedforwardVelocity < -feedforwardActiveEps);
        const bool feedbackLeadLowSpeedInhibit =
            feedbackLeadOutwardDir != 0 && !feedbackLeadAgainstMovingFeedforward;
        if (feedbackLeadLowSpeedInhibit &&
            ((feedbackLeadOutwardDir > 0 && feedforwardVelocity > 0.0) ||
             (feedbackLeadOutwardDir < 0 && feedforwardVelocity < 0.0))) {
            feedforwardVelocity = 0.0;
        }

        const bool postSoftStartRecoveryRamp =
            !filter.IsSoftStartActive() &&
            !filter.HasLocalViolation() &&
            filter.GetConfiguredMaxVelocity() > 0.0 &&
            filter.GetMaxVelocity() < filter.GetConfiguredMaxVelocity() - 1e-6;
        const double holdCorrectionMaxVelocityRatio = postSoftStartRecoveryRamp
            ? kCsvSoftStartRecoveryHoldCorrectionMaxVelocityRatio
            : kCsvHoldCorrectionMaxVelocityRatio;

        const int clampDir = filter.GetTrackingClampDirection();

        const auto [pdoMinLimit, pdoMaxLimit] = GetCsvMotionLimits(filter);
        int hardClampDir = 0;
        double hardRemainingDistance = std::numeric_limits<double>::infinity();
        bool actualAtOrPastHardLimit = false;
        bool targetAtHardLimit = false;
        const double hardEps = (std::max)(deadband, 1e-6);
        // Match the braking envelope to the command ramp that is active now.
        // During soft-start this is lower than the configured machine decel,
        // so using the configured value would enter hard-limit braking too late.
        double hardDeceleration = std::abs(filter.GetMaxDeceleration());
        const double configuredHardDeceleration = std::abs(filter.GetConfiguredMaxDeceleration());
        if (configuredHardDeceleration > 0.0 &&
            (hardDeceleration <= 0.0 || configuredHardDeceleration < hardDeceleration)) {
            hardDeceleration = configuredHardDeceleration;
        }
        hardDeceleration = (std::max)(hardDeceleration, 1e-9);
        const double hardStopMargin = (std::max)(hardEps, maxVelocity * safeCycleSeconds);
        const double upperOutwardVelocityEstimate = (std::max)(
            0.0,
            (std::max)(
                (std::max)(feedforwardVelocity, state.commandVelocity),
                actualVelocityAvailable ? actualVelocity : 0.0));
        const double lowerOutwardVelocityEstimate = (std::max)(
            0.0,
            (std::max)(
                (std::max)(-feedforwardVelocity, -state.commandVelocity),
                actualVelocityAvailable ? -actualVelocity : 0.0));
        const double upperHardEntryMargin = hardStopMargin +
            (upperOutwardVelocityEstimate * upperOutwardVelocityEstimate) / (2.0 * hardDeceleration);
        const double lowerHardEntryMargin = hardStopMargin +
            (lowerOutwardVelocityEstimate * lowerOutwardVelocityEstimate) / (2.0 * hardDeceleration);
        if (std::isfinite(pdoMaxLimit) &&
            (filteredOutput >= pdoMaxLimit - upperHardEntryMargin || actualPosition >= pdoMaxLimit - upperHardEntryMargin)) {
            hardClampDir = +1;
            hardRemainingDistance = (std::max)(0.0, pdoMaxLimit - actualPosition);
            targetAtHardLimit = filteredOutput >= pdoMaxLimit - hardEps;
            actualAtOrPastHardLimit = actualPosition >= pdoMaxLimit - hardEps;
        } else if (std::isfinite(pdoMinLimit) &&
                   (filteredOutput <= pdoMinLimit + lowerHardEntryMargin || actualPosition <= pdoMinLimit + lowerHardEntryMargin)) {
            hardClampDir = -1;
            hardRemainingDistance = (std::max)(0.0, actualPosition - pdoMinLimit);
            targetAtHardLimit = filteredOutput <= pdoMinLimit + hardEps;
            actualAtOrPastHardLimit = actualPosition <= pdoMinLimit + hardEps;
        }

        double hardOutwardVelocityLimit = maxVelocity;
        if (hardClampDir != 0) {
            const bool clearOutwardImmediately = targetAtHardLimit || actualAtOrPastHardLimit ||
                hardRemainingDistance <= (hardStopMargin * 4.0);
            const double hardBrakingDistance = clearOutwardImmediately
                ? 0.0
                : (std::max)(0.0, hardRemainingDistance - hardStopMargin);
            hardOutwardVelocityLimit = (hardBrakingDistance <= hardEps)
                ? 0.0
                : std::sqrt(2.0 * hardDeceleration * hardBrakingDistance);
            hardOutwardVelocityLimit = (std::clamp)(hardOutwardVelocityLimit, 0.0, maxVelocity);
            const double actualOutwardVelocity = actualVelocityAvailable
                ? (hardClampDir > 0 ? (std::max)(0.0, actualVelocity) : (std::max)(0.0, -actualVelocity))
                : 0.0;
            if (actualOutwardVelocity > hardOutwardVelocityLimit) {
                hardOutwardVelocityLimit = 0.0;
            }
        }

        auto clampHardOutwardVelocity = [&](double velocity) {
            if (hardClampDir > 0 && velocity > hardOutwardVelocityLimit) {
                return hardOutwardVelocityLimit;
            }
            if (hardClampDir < 0 && velocity < -hardOutwardVelocityLimit) {
                return -hardOutwardVelocityLimit;
            }
            return velocity;
        };
        feedforwardVelocity = clampHardOutwardVelocity(feedforwardVelocity);

        // Directional CSV-correction inhibit while an explicit safety state
        // is clamped on the OUTWARD side. positionError = filteredOutput
        // - actualPosition; when the filter pins its output at iActValue ±
        // trackingErrorWindow as a safety boundary, the resulting positionError
        // IS that boundary, not normal servo lag. Letting Kp*positionError feed
        // 0x60FF in the same outward direction would push actualPosition
        // outward, the tracking window would slide outward with it, and the
        // filter would run away after a velocity-violation/recovery flips the
        // input direction faster than the filter can decelerate.
        //
        // BUT: as soon as the input direction reverses (e.g. user input drops
        // back through the actual position), the filter normally accelerates
        // its output back inward. Once the output leaves the outward boundary
        // (clampDir == 0) or positionError reverses sign (no longer pushing
        // outward), the closed-loop correction must resume so actualPosition
        // can chase the filter output and the axis keeps tracking. An
        // unconditional inhibit while HasTrackingErrorViolation() is true
        // (including the latched fatal state) would freeze the axis at the
        // event-time position even after the input direction reversed.
        const bool safetyTrackingClamp =
            (clampDir != 0) &&
            (filter.HasAccelerationViolation() || filter.HasDecelerationViolation() ||
             filter.HasTrackingErrorViolation() ||
             filter.IsTrackingErrorFatal()) &&
            !filter.HasHardLimitViolation();

        const double clampIntentVelocityEps = (std::max)(deadband / safeCycleSeconds, 1e-6);
        const bool feedforwardTowardClamp =
            (clampDir > 0 && feedforwardVelocity > clampIntentVelocityEps) ||
            (clampDir < 0 && feedforwardVelocity < -clampIntentVelocityEps);
        const bool feedforwardAwayFromClamp =
            (clampDir > 0 && feedforwardVelocity < -clampIntentVelocityEps) ||
            (clampDir < 0 && feedforwardVelocity > clampIntentVelocityEps);
        if (!safetyTrackingClamp || clampDir == 0 || feedforwardAwayFromClamp) {
            state.trackingClampIntentDir = 0;
        } else if (feedforwardTowardClamp) {
            state.trackingClampIntentDir = clampDir;
        }
        const bool commandIntentTowardClamp = state.trackingClampIntentDir == clampDir;
        const bool inhibitClampOutward = safetyTrackingClamp && !commandIntentTowardClamp;
        if (inhibitClampOutward &&
            ((clampDir > 0 && feedforwardVelocity > 0.0) ||
             (clampDir < 0 && feedforwardVelocity < 0.0))) {
            feedforwardVelocity = 0.0;
        }
        const bool inhibitCorrection =
            inhibitClampOutward &&
            (positionError != 0.0) &&
            ((clampDir > 0 && positionError > 0.0) ||
             (clampDir < 0 && positionError < 0.0));
        if (inhibitCorrection) {
            positionError = 0.0;
        }

        if (!state.initialized) {
            state.initialized = true;
            state.smoothedPositionError = positionError;
            state.smoothedActualVelocity = actualVelocityAvailable ? actualVelocity : 0.0;
            state.correctionVelocity = 0.0;
        } else {
            const double correctionFilterTau = filter.IsSoftStartActive()
                ? kCsvCorrectionFilterTauSec
                : kCsvPostSoftStartCorrectionFilterTauSec;
            const double alpha = safeCycleSeconds / (correctionFilterTau + safeCycleSeconds);
            state.smoothedPositionError += (positionError - state.smoothedPositionError) * alpha;
            if (actualVelocityAvailable) {
                const double velocityAlpha = safeCycleSeconds / (kCsvActualVelocityFilterTauSec + safeCycleSeconds);
                state.smoothedActualVelocity += (actualVelocity - state.smoothedActualVelocity) * velocityAlpha;
            }
            if (std::abs(positionError) <= deadband && std::abs(state.smoothedPositionError) <= deadband) {
                state.smoothedPositionError = 0.0;
            }
            if (inhibitCorrection) {
                // Bleed the smoothed error toward zero in the inhibited
                // outward direction so a previously accumulated correction
                // does not keep driving 0x60FF after the safety boundary
                // engages.
                if ((clampDir > 0 && state.smoothedPositionError > 0.0) ||
                    (clampDir < 0 && state.smoothedPositionError < 0.0)) {
                    state.smoothedPositionError = 0.0;
                }
                if ((clampDir > 0 && state.correctionVelocity > 0.0) ||
                    (clampDir < 0 && state.correctionVelocity < 0.0)) {
                    state.correctionVelocity = 0.0;
                }
                if (!safetyTrackingClamp && !filter.HasHardLimitViolation() &&
                    ((clampDir > 0 && state.commandVelocity > 0.0) ||
                     (clampDir < 0 && state.commandVelocity < 0.0))) {
                    state.commandVelocity = 0.0;
                }
            }
        }
        if (feedbackLeadOutwardDir != 0) {
            if ((feedbackLeadOutwardDir > 0 && state.smoothedPositionError > 0.0) ||
                (feedbackLeadOutwardDir < 0 && state.smoothedPositionError < 0.0)) {
                state.smoothedPositionError = 0.0;
            }
            if ((feedbackLeadOutwardDir > 0 && state.correctionVelocity > 0.0) ||
                (feedbackLeadOutwardDir < 0 && state.correctionVelocity < 0.0)) {
                state.correctionVelocity = 0.0;
            }
        }
        if (hardClampDir != 0) {
            state.correctionVelocity = clampHardOutwardVelocity(state.correctionVelocity);
            state.commandVelocity = clampHardOutwardVelocity(state.commandVelocity);
            if (actualAtOrPastHardLimit &&
                ((hardClampDir > 0 && state.commandVelocity > 0.0) ||
                 (hardClampDir < 0 && state.commandVelocity < 0.0))) {
                state.commandVelocity = 0.0;
            }
        }

        double desiredCorrectionVelocity = 0.0;
        const bool feedforwardActive = std::abs(feedforwardVelocity) > feedforwardActiveEps;
        const bool softStartPullIn = filter.IsSoftStartActive() && !feedforwardActive;
        const double activeCorrectionMaxVelocityRatio = filter.IsSoftStartActive()
            ? kCsvSoftStartCorrectionMaxVelocityRatio
            : kCsvCorrectionMaxVelocityRatio;
        const double activeCorrectionFeedforwardRatio = filter.IsSoftStartActive()
            ? kCsvSoftStartCorrectionFeedforwardRatio
            : kCsvCorrectionFeedforwardRatio;
        const double feedforwardCoupledLimit = std::abs(feedforwardVelocity) * activeCorrectionFeedforwardRatio +
            maxVelocity * holdCorrectionMaxVelocityRatio;
        double correctionVelocityLimit = softStartPullIn
            ? (maxVelocity * kCsvSoftStartPullInCorrectionMaxVelocityRatio)
            : (feedforwardActive
            ? (std::min)(maxVelocity * activeCorrectionMaxVelocityRatio, feedforwardCoupledLimit)
            : (maxVelocity * holdCorrectionMaxVelocityRatio));
        const double smoothedError = state.smoothedPositionError;
        if (std::abs(smoothedError) > deadband) {
            double gain = filter.GetPositionGain() * (softStartPullIn
                ? kCsvSoftStartPullInPositionCorrectionGainRatio
                : (feedforwardActive ? kCsvPositionCorrectionGainRatio : kCsvHoldPositionCorrectionGainRatio));
            if (filter.IsSoftStartActive()) {
                gain *= 0.1;
            }
            if (std::isfinite(positionGainScale) && positionGainScale > 0.0) {
                gain *= positionGainScale;
            }
            desiredCorrectionVelocity = smoothedError * gain;

            const double maxDeceleration = (std::max)(std::abs(filter.GetMaxDeceleration()), 1e-9);
            const double brakingLimit = std::sqrt(2.0 * maxDeceleration * std::abs(smoothedError) * 1.1);
            correctionVelocityLimit = (std::min)(correctionVelocityLimit, brakingLimit);
        }
        const bool allowVelocityDamping = filter.IsSoftStartActive() || !feedforwardActive;
        if (actualVelocityAvailable && allowVelocityDamping) {
            const double relativeActualVelocity = state.smoothedActualVelocity - feedforwardVelocity;
            const double dampingRatio = filter.IsSoftStartActive()
                ? kCsvCorrectionVelocityDampingRatio
                : kCsvPostSoftStartHoldVelocityDampingRatio;
            const double rawDampingVelocity = dampingRatio * relativeActualVelocity;
            const double dampingLimit = correctionVelocityLimit * (filter.IsSoftStartActive() ? 0.75 : 0.25);
            desiredCorrectionVelocity -= (std::clamp)(rawDampingVelocity,
                                                      -dampingLimit,
                                                      dampingLimit);
        }
        desiredCorrectionVelocity = (std::clamp)(desiredCorrectionVelocity,
                                                 -correctionVelocityLimit,
                                                 correctionVelocityLimit);

        const double correctionAccelRatio = filter.IsSoftStartActive()
            ? kCsvCorrectionAccelRatio
            : kCsvPostSoftStartCorrectionAccelRatio;
        const double correctionAccelLimit = (std::max)(std::abs(filter.GetMaxAcceleration()),
                                                       std::abs(filter.GetMaxDeceleration())) *
                                            correctionAccelRatio;
        if (correctionAccelLimit > 0.0) {
            const double maxCorrectionStep = correctionAccelLimit * safeCycleSeconds;
            const double correctionDelta = desiredCorrectionVelocity - state.correctionVelocity;
            state.correctionVelocity += (std::clamp)(correctionDelta, -maxCorrectionStep, maxCorrectionStep);
        } else {
            state.correctionVelocity = desiredCorrectionVelocity;
        }

        double desiredCommandVelocity =
            (std::clamp)(feedforwardVelocity + state.correctionVelocity, -maxVelocity, maxVelocity);
        desiredCommandVelocity = clampHardOutwardVelocity(desiredCommandVelocity);
        if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir > 0 && desiredCommandVelocity > 0.0) {
            desiredCommandVelocity = 0.0;
        } else if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir < 0 && desiredCommandVelocity < 0.0) {
            desiredCommandVelocity = 0.0;
        }
        const double commandDelta = desiredCommandVelocity - state.commandVelocity;
        const double maxPositiveStep = (std::max)(std::abs(filter.GetMaxAcceleration()), 0.0) * safeCycleSeconds;
        const double maxNegativeStep = (std::max)(std::abs(filter.GetMaxDeceleration()), 0.0) * safeCycleSeconds;
        if (maxPositiveStep > 0.0 && maxNegativeStep > 0.0) {
            state.commandVelocity +=
                (std::clamp)(commandDelta, -maxNegativeStep, maxPositiveStep);
        } else {
            state.commandVelocity = desiredCommandVelocity;
        }
        state.commandVelocity = clampHardOutwardVelocity(state.commandVelocity);
        if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir > 0 && state.commandVelocity > 0.0) {
            state.commandVelocity = (std::max)(0.0, state.commandVelocity - maxNegativeStep);
        } else if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir < 0 && state.commandVelocity < 0.0) {
            state.commandVelocity = (std::min)(0.0, state.commandVelocity + maxPositiveStep);
        }
        if (actualAtOrPastHardLimit &&
            ((hardClampDir > 0 && state.commandVelocity > 0.0) ||
             (hardClampDir < 0 && state.commandVelocity < 0.0))) {
            state.commandVelocity = 0.0;
        }

        return (std::clamp)(state.commandVelocity, -maxVelocity, maxVelocity);
    }

    struct TscSpinCalibration {
        bool ready{false};
        long double cyclesPerNs{0.0L};
    };

    bool CpuHasInvariantTsc()
    {
#if defined(_M_IX86) || defined(_M_X64)
        int cpuInfo[4] = {0, 0, 0, 0};
        __cpuid(cpuInfo, 0x80000007);
        return (cpuInfo[3] & (1 << 8)) != 0;
#else
        return false;
#endif
    }

    bool TryCalibrateTscSpin(const LARGE_INTEGER& qpcFrequency, TscSpinCalibration& out)
    {
        out = {};
        if (!CpuHasInvariantTsc() || qpcFrequency.QuadPart <= 0) {
            return false;
        }

        LARGE_INTEGER qpcStart{};
        LARGE_INTEGER qpcNow{};
        unsigned int tscAux = 0;
        QueryPerformanceCounter(&qpcStart);
        const unsigned __int64 tscStart = __rdtscp(&tscAux);
        const LONGLONG calibrationTicks = (qpcFrequency.QuadPart / 200LL > 0)
            ? (qpcFrequency.QuadPart / 200LL)
            : 1LL;
        do {
            YieldProcessor();
            QueryPerformanceCounter(&qpcNow);
        } while ((qpcNow.QuadPart - qpcStart.QuadPart) < calibrationTicks);

        const unsigned __int64 tscEnd = __rdtscp(&tscAux);
        const LONGLONG qpcDelta = qpcNow.QuadPart - qpcStart.QuadPart;
        const unsigned __int64 tscDelta = tscEnd - tscStart;
        if (qpcDelta <= 0 || tscDelta == 0) {
            return false;
        }

        const long double nsDelta =
            (static_cast<long double>(qpcDelta) * 1000000000.0L) /
            static_cast<long double>(qpcFrequency.QuadPart);
        if (!(nsDelta > 0.0L)) {
            return false;
        }

        out.cyclesPerNs = static_cast<long double>(tscDelta) / nsDelta;
        out.ready = std::isfinite(static_cast<double>(out.cyclesPerNs)) && out.cyclesPerNs > 0.0L;
        return out.ready;
    }

    unsigned __int64 DurationToTscCycles(const TscSpinCalibration& calibration,
                                         std::chrono::steady_clock::duration duration)
    {
        if (!calibration.ready) {
            return 0;
        }

        const auto durationNs = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
        if (durationNs <= 0) {
            return 0;
        }

        const long double cycles = static_cast<long double>(durationNs) * calibration.cyclesPerNs;
        if (!(cycles > 0.0L)) {
            return 0;
        }

        const long double maxCycles = static_cast<long double>((std::numeric_limits<unsigned __int64>::max)());
        if (cycles >= maxCycles) {
            return (std::numeric_limits<unsigned __int64>::max)();
        }

        return static_cast<unsigned __int64>(cycles + 0.5L);
    }

    constexpr uint32_t kDcPiReferenceCycleUs = 2000;

    int64_t ClampDcAdjustmentNs(int64_t adjustNs, int64_t boundNs)
    {
        if (boundNs <= 0) return 0;
        if (adjustNs > boundNs) return boundNs;
        if (adjustNs < -boundNs) return -boundNs;
        return adjustNs;
    }

    int64_t ComputeDcAdjustmentBoundNs(uint32_t cycleMicros)
    {
        const uint32_t effectiveCycle = cycleMicros ? cycleMicros : kDcPiReferenceCycleUs;
        return (static_cast<int64_t>(effectiveCycle) * 1000LL) / 4LL;
    }

    int64_t ComputeDcPhaseErrorNs(int64_t dctimeNs, int64_t cycleNs)
    {
        if (cycleNs <= 0) return 0;
        int64_t mod = dctimeNs % cycleNs;
        if (mod < 0) mod += cycleNs;
        const int64_t half = cycleNs / 2;
        return (mod > half) ? (mod - cycleNs) : mod;
    }
}
DWORD WINAPI EtherCATMaster::CyclicThreadFunc(LPVOID param) {
    EtherCATMaster* master = static_cast<EtherCATMaster*>(param);
    master->CyclicTask();
    return 0;
}

void EtherCATMaster::ResetDcDriftCompensationState()
{
    dcPiIntegralNs = 0.0;
    dcToffNs.store(0LL, std::memory_order_relaxed);
}

void EtherCATMaster::UpdateDcDriftCompensation(int64_t dctimeNs, uint32_t cycleMicros)
{
    const int64_t cycleNs = static_cast<int64_t>(cycleMicros ? cycleMicros : kDcPiReferenceCycleUs) * 1000LL;
    const int64_t boundNs = ComputeDcAdjustmentBoundNs(cycleMicros);
    if (cycleNs <= 0 || boundNs <= 0) {
        ResetDcDriftCompensationState();
        return;
    }

    const int64_t phaseErrorNs = ComputeDcPhaseErrorNs(dctimeNs, cycleNs);
    if (!useDcPiController.load(std::memory_order_relaxed)) {
        const int64_t previousOffsetNs = dcToffNs.load(std::memory_order_relaxed);
        const int64_t smoothedOffsetNs = static_cast<int64_t>(
            std::llround(0.8 * static_cast<double>(previousOffsetNs) + 0.2 * static_cast<double>(phaseErrorNs)));
        dcPiIntegralNs = 0.0;
        dcToffNs.store(ClampDcAdjustmentNs(smoothedOffsetNs, boundNs), std::memory_order_relaxed);
        return;
    }

    const double proportionalTermNs = dcPiProportionalGain * static_cast<double>(phaseErrorNs);
    const double outputBeforeIntegrationNs = proportionalTermNs + dcPiIntegralNs;
    const bool saturatedHigh = outputBeforeIntegrationNs >= static_cast<double>(boundNs);
    const bool saturatedLow = outputBeforeIntegrationNs <= -static_cast<double>(boundNs);
    const bool allowIntegration = !((saturatedHigh && phaseErrorNs > 0) || (saturatedLow && phaseErrorNs < 0));

    if (allowIntegration) {
        const double cycleScale = static_cast<double>(cycleMicros ? cycleMicros : kDcPiReferenceCycleUs) /
                                  static_cast<double>(kDcPiReferenceCycleUs);
        dcPiIntegralNs += dcPiIntegralGain * cycleScale * static_cast<double>(phaseErrorNs);
        if (dcPiIntegralNs > static_cast<double>(boundNs)) dcPiIntegralNs = static_cast<double>(boundNs);
        else if (dcPiIntegralNs < -static_cast<double>(boundNs)) dcPiIntegralNs = -static_cast<double>(boundNs);
    }

    double outputNs = proportionalTermNs + dcPiIntegralNs;
    if (outputNs > static_cast<double>(boundNs)) outputNs = static_cast<double>(boundNs);
    else if (outputNs < -static_cast<double>(boundNs)) outputNs = -static_cast<double>(boundNs);
    dcToffNs.store(static_cast<long long>(std::llround(outputNs)), std::memory_order_relaxed);
}


void EtherCATMaster::CyclicTask() {
    if (simulationMode_.load(std::memory_order_relaxed)) {
        SimulationCyclicTask();
        return;
    }

    // ========================================================================
    // REAL-TIME THREAD PRIORITY OPTIMIZATION
    // ========================================================================
    // Set thread priority to TIME_CRITICAL to ensure EtherCAT cyclic task
    // is not preempted by lower-priority tasks (e.g., disk I/O, UI updates).
    // This is critical for maintaining deterministic cycle times.
    
    HANDLE currentThread = GetCurrentThread();
    
    // Set thread priority to highest real-time level
    if (SetThreadPriority(currentThread, THREAD_PRIORITY_TIME_CRITICAL)) {
        std::stringstream ss;
        ss << "RT-Optimization: Cyclic thread priority set to TIME_CRITICAL (level 15)";
        SetError(ss.str());
    } else {
        DWORD prioError = ::GetLastError();
        std::stringstream ss;
        ss << "Warning: Failed to set TIME_CRITICAL priority (error: " << prioError 
           << ") - may experience scheduling delays";
        SetError(ss.str());
        // Non-fatal: continue with default priority
    }
    
    // Optional: Boost the entire process priority class for maximum real-time performance
    // Note: This affects all threads in the process, use with caution
    DWORD currentPriorityClass = GetPriorityClass(GetCurrentProcess());
    if (currentPriorityClass < HIGH_PRIORITY_CLASS) {
        if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS)) {
            std::stringstream ss;
            ss << "RT-Optimization: Process priority class elevated to HIGH_PRIORITY";
            SetError(ss.str());
        } else {
            DWORD classError = ::GetLastError();
            std::stringstream ss;
            ss << "Info: Process priority class not elevated (error: " << classError 
               << ") - requires elevated privileges for REALTIME_PRIORITY";
            SetError(ss.str());
            // Non-fatal: HIGH_PRIORITY_CLASS requires admin rights on some systems
        }
    }
    
    // ========================================================================
    // CYCLIC TASK CPU AFFINITY BINDING
    // ========================================================================
    // Bind the main CyclicTask to Core 4 (logical processor 4, mask 0x10) to
    // match CpuIsolation's TARGET_ETHERCAT_CORE = 4.  CpuIsolation guards
    // Core 4 by repelling other processes; the CyclicTask must actually run
    // there for the protection to be effective.  Fall back to round-robin
    // only when Core 4 is not available on the host.
    {
        constexpr int    kEtherCATTargetCore = 4;
        constexpr DWORD_PTR kEtherCATCoreMask = (DWORD_PTR)1 << kEtherCATTargetCore;

        DWORD_PTR processMask2 = 0, systemMask2 = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processMask2, &systemMask2);
        DWORD_PTR available = processMask2 ? processMask2 : systemMask2;

        DWORD_PTR pick = 0;
        if (available & kEtherCATCoreMask) {
            pick = kEtherCATCoreMask;                       // preferred: Core 4
        } else {
            pick = AllocateCoreMaskRoundRobin();             // fallback
            if (pick) pick &= available;
        }

        if (pick) {
            DWORD_PTR prev = SetThreadAffinityMask(currentThread, pick);
            if (prev != 0) {
                cyclicThreadCoreMask_.store(pick, std::memory_order_relaxed);
                std::stringstream ss;
                ss << "RT-Optimization: CyclicTask affinity set to core mask 0x" << std::hex << pick;
                SetError(ss.str());
            } else {
                std::stringstream ss;
                ss << "Warning: Failed to set CyclicTask affinity (error: " << ::GetLastError() << ")";
                SetError(ss.str());
            }
        }
    }

    // ========================================================================
    // CYCLIC TASK INITIALIZATION
    // ========================================================================
    
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);

    unsigned long sampleCounter = 0;
    const unsigned long sampleInterval = 500;
    unsigned long lastSampleTid = 0;
    unsigned long long lastSampleTotalUs = 0ULL;

    HANDLE waitTimer = CreateHighResolutionTimerHandle();
    bool waitTimerValid = (waitTimer != nullptr);

    using steady_clock = std::chrono::steady_clock;
    auto targetTime = steady_clock::now();
    uint32_t lastCycleSnapshot = desiredCycleTime ? desiredCycleTime : 2000;
    if (lastCycleSnapshot == 0) lastCycleSnapshot = 2000;
    auto period = std::chrono::microseconds(lastCycleSnapshot);
    TscSpinCalibration tscSpinCalibration;
    TryCalibrateTscSpin(frequency, tscSpinCalibration);
    ResetDcDriftCompensationState();

    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);

        // Shorten lock scope: hold pdoDataCS only for the SOEM send/receive operations.
        // Move measurement/logging outside the critical section to reduce contention with other IOmap operations.
        LARGE_INTEGER lockEnterTick, lockLeaveTick;
        QueryPerformanceCounter(&lockEnterTick);
        pdoExchangeActive_.store(true, std::memory_order_release);
        EnterCriticalSection(&pdoDataCS);

        // Poll the previously transmitted PDO frame without blocking the RT core.
        // SOEM's timeout=0 path matches the Win32/non-RT polling model used by
        // the upstream inttime sample and avoids stalling inside EC_TIMEOUTRET.
        workingCounter = ecx_receive_processdata(&context, 0);
        
        // CRITICAL FIX: Apply staged output bytes to IOmap BEFORE sending process data.
        // This ensures that PowerOn staged motion commands and controlwords are written
        // to IOmap immediately and sent to slaves.
        if (!stagedOutputs.empty()) {
            constexpr int kMaxStagedWriteBytes = 64;
            for (auto it = stagedOutputs.begin(); it != stagedOutputs.end(); ) {
                const int absOffset = it->first;
                const std::vector<uint8_t> &data = it->second;

                if (absOffset < 0 || absOffset >= (int)sizeof(IOmap)) {
                    std::stringstream ss; ss << "CyclicTask: stagedOutputs absOffset out of range, erasing. absOffset=" << absOffset;
                    SetError(ss.str());
                    it = stagedOutputs.erase(it);
                    continue;
                }

                const int maxWrite = (int)sizeof(IOmap) - absOffset;
                const int dataSize = (int)data.size();
                if (dataSize <= 0 || dataSize > kMaxStagedWriteBytes) {
                    std::stringstream ss; ss << "CyclicTask: stagedOutputs entry size suspicious, erasing. absOffset=" << absOffset << " size=" << dataSize;
                    SetError(ss.str());
                    it = stagedOutputs.erase(it);
                    continue;
                }

                int toWrite = (dataSize < maxWrite) ? dataSize : maxWrite;
                if (toWrite > kMaxStagedWriteBytes) toWrite = kMaxStagedWriteBytes;
                if (toWrite > 0) {
                    if (!safe_memcpy(IOmap + absOffset, data.data(), (size_t)toWrite)) {
                        std::stringstream ss; ss << "CyclicTask: safe_memcpy failed absOffset=" << absOffset << " toWrite=" << toWrite;
                        SetError(ss.str());
                    }
                }
                ++it;
            }
        }
        
        ecx_send_processdata(&context);
        LeaveCriticalSection(&pdoDataCS);
        pdoExchangeActive_.store(false, std::memory_order_release);
        QueryPerformanceCounter(&lockLeaveTick);

        // ================================================================
        // COMMS TIME: Measure ONLY the SOEM send/receive portion (lock acquire
        // through lock release). Previously this was measured at the very end
        // of the loop iteration, which incorrectly included CheckSlaveStates,
        // MMF poll, cyclicCallback, and verbose logging – inflating both the
        // instantaneous and max values significantly.
        // ================================================================
        uint32_t commsMicros = 0;
        if (perfFreqTicks > 0) {
            uint64_t commsTicks = (uint64_t)(lockLeaveTick.QuadPart - start.QuadPart);
            commsMicros = (uint32_t)((commsTicks * 1000000ULL) / perfFreqTicks);
        }
        commsTime_.store(commsMicros, std::memory_order_relaxed);
        {
            uint32_t prevMax = maxObservedCommsTime.load(std::memory_order_relaxed);
            while (commsMicros > prevMax &&
                   !maxObservedCommsTime.compare_exchange_weak(prevMax, commsMicros, std::memory_order_relaxed)) {
            }
        }

        // Instrument lock hold time; warn if above threshold (1ms default).
        if (perfFreqTicks > 0) {
            uint64_t holdTicks = (uint64_t)(lockLeaveTick.QuadPart - lockEnterTick.QuadPart);
            uint64_t holdUs = (holdTicks * 1000000ULL) / perfFreqTicks;
            if (holdUs > 1000) { // if > 1000us, log a warning
                std::stringstream ss; ss << "[WARN] pdoDataCS held for " << holdUs << "us";
                SetError(ss.str());
            }
        }

        // ================================================================
        // OBSERVED CYCLE PERIOD: Measure actual cycle-to-cycle interval using
        // high-resolution perf counter delta between consecutive PDO sends.
        // This is the TRUE "actual cycle time" (e.g. ~2000us for a 2ms cycle).
        // Previously observedCycleMicros_ was declared but NEVER written.
        // ================================================================
        LARGE_INTEGER nowTick;
        QueryPerformanceCounter(&nowTick);
        uint64_t nowTicks = (uint64_t)nowTick.QuadPart;
        uint64_t prev = lastPdoPerfTicks.load(std::memory_order_relaxed);
        prevPdoPerfTicks.store(prev);
        lastPdoPerfTicks.store(nowTicks);
        pdosSentCount.fetch_add(1, std::memory_order_relaxed);
        if (prev != 0 && perfFreqTicks > 0) {
            uint64_t deltaTicks = (nowTicks > prev) ? (nowTicks - prev) : 1;
            double hz = static_cast<double>(perfFreqTicks) / static_cast<double>(deltaTicks);
            lastPdoHz.store(hz, std::memory_order_relaxed);

            // Compute true cycle-to-cycle period in microseconds
            uint32_t cyclePeriodUs = (uint32_t)((deltaTicks * 1000000ULL) / perfFreqTicks);
            observedCycleMicros_.store(cyclePeriodUs, std::memory_order_relaxed);

            // Track max cycle-to-cycle period (high-water mark for jitter detection)
            uint32_t prevCycleMax = maxObservedCycleMicros_.load(std::memory_order_relaxed);
            while (cyclePeriodUs > prevCycleMax &&
                   !maxObservedCycleMicros_.compare_exchange_weak(prevCycleMax, cyclePeriodUs, std::memory_order_relaxed)) {
            }
        }

        if (verboseCyclicLog && pdosSentCount.load(std::memory_order_relaxed) == 1) {
            SetError("[PDO啟動] 首次PDO發送成功，開始連續發送模式");
        }

        if (verboseCyclicLog && (pdosSentCount.load(std::memory_order_relaxed) % 50 == 0)) {
            uint64_t runningTime = GetTickCount64() - operationStartTime;
            char uiMsg[256];
                snprintf(uiMsg, sizeof(uiMsg),
                "[PDO統計] #%llu WKC=%u 通訊=%uμs 運行=%llums",
                (unsigned long long)pdosSentCount.load(std::memory_order_relaxed), workingCounter,
                commsMicros, runningTime);

            std::string tempError = lastError;
            SetError(uiMsg);
            if (tempError.empty()) {
                lastError.clear();
            }
        }

        // Throttle slave state checking to every ~100 cycles (~200ms at 2ms cycle).
        // ecx_readstate is an expensive mailbox operation that adds jitter if called every cycle.
        {
            static unsigned long checkStatesCounter = 0;
            if (++checkStatesCounter >= 100) {
                checkStatesCounter = 0;
                CheckSlaveStates();
            }
        }

        // If MMF1_IN reader is enabled, perform one poll from the master cyclic thread.
        if (mmf1ReaderRunning.load()) {
            // PollOnce returns true if a mapping existed and a read was attempted
            (void)mmf1Reader.PollOnce();
        }

        if (cyclicCallback) {
            cyclicCallback();
        }

        uint32_t configuredCycle = desiredCycleTime ? desiredCycleTime : 2000;
        if (configuredCycle == 0) configuredCycle = 2000;
        auto nowSteady = steady_clock::now();
        if (configuredCycle != lastCycleSnapshot) {
            lastCycleSnapshot = configuredCycle;
            period = std::chrono::microseconds(lastCycleSnapshot);
            targetTime = nowSteady;
        }
        targetTime += period;
        auto preWait = steady_clock::now();
        if (targetTime <= preWait) {
            targetTime = preWait + period;
        }

        auto waitDuration = targetTime - preWait;

        const bool dcCompEnabled = enableDcDriftComp.load(std::memory_order_relaxed);
        const bool hasDc = context.slavelist[0].hasdc;
        if (!(dcCompEnabled && hasDc) &&
            (dcToffNs.load(std::memory_order_relaxed) != 0LL || std::abs(dcPiIntegralNs) > 0.0)) {
            ResetDcDriftCompensationState();
        }

        if (dcCompEnabled && hasDc) {
            const int64_t boundNs = ComputeDcAdjustmentBoundNs(lastCycleSnapshot);
            int64_t adjustNs = ClampDcAdjustmentNs(dcToffNs.load(std::memory_order_relaxed), boundNs);
            if (adjustNs > 0) {
                auto adj = std::chrono::nanoseconds(adjustNs);
                if (adj < waitDuration) {
                    waitDuration -= adj;
                } else {
                    waitDuration = std::chrono::nanoseconds(0);
                }
            } else if (adjustNs < 0) {
                waitDuration += std::chrono::nanoseconds(-adjustNs);
            }
        }

        pdoIdleWindowDeadlineUs_.store(
            std::chrono::duration_cast<std::chrono::microseconds>(
                targetTime.time_since_epoch()).count(),
            std::memory_order_release);

        bool stopRequested = false;
        bool timerScheduled = false;
        if (waitTimerValid && waitDuration > std::chrono::microseconds(50)) {
            auto waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(waitDuration).count();
            LONGLONG hundredNs = waitNs / 100;
            if (hundredNs <= 0) hundredNs = 1;
            LARGE_INTEGER due;
            due.QuadPart = -hundredNs;
            if (SetWaitableTimer(waitTimer, &due, 0, NULL, NULL, FALSE)) {
                HANDLE handles[2] = { stopEvent, waitTimer };
                DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                timerScheduled = true;
                if (waitRes == WAIT_OBJECT_0) {
                    stopRequested = true;
                }
            } else {
                CancelWaitableTimer(waitTimer);
                CloseHandle(waitTimer);
                waitTimer = nullptr;
                waitTimerValid = false;
            }
        }

        if (!stopRequested && !timerScheduled && waitDuration > std::chrono::microseconds(200)) {
            auto sleepMs = std::chrono::duration_cast<std::chrono::milliseconds>(waitDuration);
            if (sleepMs.count() > 0) {
                Sleep((DWORD)sleepMs.count());
            }
        }

        while (!stopRequested) {
            auto now = std::chrono::steady_clock::now();
            if (now >= targetTime) {
                break;
            }
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                stopRequested = true;
                break;
            }
            auto remaining = targetTime - now;
            if (remaining > std::chrono::microseconds(200)) {
                // Yield the timeslice to reduce CPU while preserving timing accuracy
                Sleep(0);
            } else {
                const unsigned __int64 spinCycles = DurationToTscCycles(tscSpinCalibration, remaining);
                if (spinCycles > 0) {
                    unsigned int tscAux = 0;
                    const unsigned __int64 spinDeadline = __rdtscp(&tscAux) + spinCycles;
                    unsigned int stopPollCounter = 0;
                    while (!stopRequested) {
                        if ((++stopPollCounter & 0xFFu) == 0u &&
                            WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                            stopRequested = true;
                            break;
                        }
                        if (__rdtscp(&tscAux) >= spinDeadline) {
                            break;
                        }
                        YieldProcessor();
                    }
                    continue;
                }
                YieldProcessor();
            }
        }

        if (stopRequested) {
            break;
        }

        LARGE_INTEGER loopEnd;
        QueryPerformanceCounter(&loopEnd);
        cycleTime = (uint32_t)(((loopEnd.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart);

        if (dcCompEnabled && hasDc) {
            UpdateDcDriftCompensation((int64_t)context.DCtime, lastCycleSnapshot);
        }

        sampleCounter++;
        if (sampleCounter >= sampleInterval) {
            sampleCounter = 0;
            DWORD tid = GetCurrentThreadId();
            FILETIME ftCreate, ftExit, ftKernel, ftUser;
            if (GetThreadTimes(GetCurrentThread(), &ftCreate, &ftExit, &ftKernel, &ftUser)) {
                ULARGE_INTEGER k, u;
                k.LowPart = ftKernel.dwLowDateTime; k.HighPart = ftKernel.dwHighDateTime;
                u.LowPart = ftUser.dwLowDateTime; u.HighPart = ftUser.dwHighDateTime;
                unsigned long long total100ns = k.QuadPart + u.QuadPart;
                unsigned long long totalUs = total100ns / 10ULL;

                if (lastSampleTid == tid && totalUs > lastSampleTotalUs) {
                    (void)(totalUs - lastSampleTotalUs);
                }
                lastSampleTid = tid;
                lastSampleTotalUs = totalUs;
            } else {
                (void)::GetLastError();
            }
        }

        AutoTuneAffinityIfNeeded();
    }

    if (waitTimerValid) {
        CancelWaitableTimer(waitTimer);
        CloseHandle(waitTimer);
    }
    pdoExchangeActive_.store(false, std::memory_order_release);
    pdoIdleWindowDeadlineUs_.store(0, std::memory_order_release);
    ResetDcDriftCompensationState();
}


void EtherCATMaster::CheckSlaveStates() {
    // 檢查是否有從站不在 OP 狀態
    if (context.slavelist[0].state != EC_STATE_OPERATIONAL) {
        ecx_readstate(&context);
        for (int i = 1; i <= slaveCount; i++) {
            if (context.slavelist[i].state != EC_STATE_OPERATIONAL) {
                // 嘗試恢復
                context.slavelist[i].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context, i);
            }
        }
    }
}


void EtherCATMaster::InitializePDOData() {
    // 初始化 PDO 輸出數據，為所有伺服驅動器設置安全的初始值
    // 這可以防止從站在轉換到 OP 模式時因無效數據觸發看門狗錯誤
    
    printf("InitializePDOData: 為 %d 個從站設置安全初始值...\n", slaveCount);
    
    // 遍歷所有從站並設置初始輸出值
    for (int i = 1; i <= slaveCount; i++) {
        ec_slavet* slave = &context.slavelist[i];
        
        // 取得此從站的輸出數據起始位置（字節偏移）
        int outputOffset = -1;
        if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(slave->outputs),
                               reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outputOffset)) {
            if (slave->Ostartbit > 0) {
                outputOffset = slave->Ostartbit / 8;
            }
        }
        if (outputOffset < 0 && slave->Ostartbit > 0) {
            outputOffset = slave->Ostartbit / 8;
        }
        int outputBytes = (slave->Obits + 7) / 8;   // 輸出字節數

        const int ioMapSize = (int)sizeof(IOmap);
        const bool offsetValid = (outputOffset >= 0) && (outputOffset < ioMapSize);
        const bool rangeValid = offsetValid && (outputBytes > 0) && (outputOffset + outputBytes <= ioMapSize);
        if (rangeValid) {
            printf("  從站 %d (%s): 輸出偏移=%d, 字節數=%d\n", 
                   i, slave->name, outputOffset, outputBytes);
            
            // 為標準 cyclic RxPDO 佈局設置安全的初始值
            // Controlword(0x6040, 2 bytes) + logical target position(0x607A, 4 bytes) +
            // active cyclic command PDO(0x60FF for CSV, 4 bytes) + Target Torque(0x6071, 2 bytes)
            
            if (outputBytes >= 2) {
                // 設置 Controlword (0x6040)
                // 初始值: 0x0006 = "Shutdown" 狀態，準備啟用
                uint16_t controlword = 0x0006;
                safe_memcpy(IOmap + outputOffset, &controlword, sizeof(controlword));
                printf("    設置 Controlword = 0x%04X\n", controlword);
            }
            
            if (outputBytes >= 6) {
                // 設置 Target Position (0x607A) = 0 (起始位置)
                int32_t targetPosition = 0;
                safe_memcpy(IOmap + outputOffset + 2, &targetPosition, sizeof(targetPosition));
                printf("    設置 Target Position = %d\n", targetPosition);
            }
            
            if (outputBytes >= 10) {
                // 設置 active cyclic command PDO (0x60FF for CSV) = 0
                int32_t targetVelocity = 0;
                safe_memcpy(IOmap + outputOffset + 6, &targetVelocity, sizeof(targetVelocity));
                printf("    設置 Target Velocity = %d\n", targetVelocity);
            }
            
            // 如果有更多字節，設置為 0（例如 0x6071 Target Torque）
            if (outputBytes > 10) {
                memset(IOmap + outputOffset + 10, 0, (size_t)(outputBytes - 10));
            }
        } else {
            printf("  從站 %d (%s): 無輸出數據或偏移無效\n", i, slave->name);
        }
    }
    
    printf("InitializePDOData: 完成\n");
}


int EtherCATMaster::SnapshotIOMap(int offset, uint8_t* outBuf, int len) {
    if (!outBuf || offset < 0 || len <= 0) return 0;
    EnterCriticalSection(&cs);
    int maxCopy = (int)sizeof(IOmap) - offset;
    if (maxCopy <= 0) { LeaveCriticalSection(&cs); return 0; }
    int toCopy = (len < maxCopy) ? len : maxCopy;
    if (!safe_memcpy(outBuf, IOmap + offset, (size_t)toCopy)) { LeaveCriticalSection(&cs); return 0; }
    LeaveCriticalSection(&cs);
    return toCopy;
}


bool EtherCATMaster::WriteOutputBytes(int absOffset, const uint8_t* data, int len) {
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to WriteOutputBytes");
        return false;
    }
    
    // File-based write debug logging disabled (pdo_write_debug.log)
    // #if 0
    // CreateDirectoryA("logs", NULL);
    // FILE* debugFile = NULL;
    // fopen_s(&debugFile, "logs\\pdo_write_debug.log", "a");
    // if (debugFile) {
    //     auto now = std::chrono::system_clock::now();
    //     auto time_t = std::chrono::system_clock::to_time_t(now);
    //     auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    //     struct tm timeinfo; localtime_s(&timeinfo, &time_t);
    //     fprintf(debugFile, "[%02d:%02d:%02d.%03d] WriteOutputBytes: 偏移%d, 長度%d, 資料: ", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, (int)ms.count(), absOffset, len);
    //     for (int i = 0; i < len; i++) fprintf(debugFile, "%02X ", data[i]);
    //     if (len >= 2) { uint16_t value16 = data[0] | (data[1] << 8); fprintf(debugFile, " (16位值: %u)", value16); }
    //     fprintf(debugFile, "\n"); fclose(debugFile);
    // }
    // #endif
    
    EnterCriticalSection(&cs);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) { 
        LeaveCriticalSection(&cs); 
    // 記錄錯誤到 internal lastError
    std::stringstream ss_err; ss_err << "WriteOutputBytes: error maxWrite<=0 absOffset=" << absOffset << " IOmap_size=" << sizeof(IOmap);
    SetError(ss_err.str());
        return false; 
    }
    int toWrite = (len < maxWrite) ? len : maxWrite;
    if (!safe_memcpy(IOmap + absOffset, data, (size_t)toWrite)) {
        LeaveCriticalSection(&cs);
        std::stringstream ss_err; ss_err << "WriteOutputBytes: safe_memcpy failed absOffset=" << absOffset << " toWrite=" << toWrite;
        SetError(ss_err.str());
        return false;
    }
    LeaveCriticalSection(&cs);
    
    // Confirmation logging disabled (pdo_write_debug.log)
    // #if 0
    // fopen_s(&debugFile, "logs\\pdo_write_debug.log", "a");
    // if (debugFile) { fprintf(debugFile, "  -> 成功寫入 %d 字節到IOmap[%d..%d]: ", toWrite, absOffset, absOffset + toWrite - 1); for (int i = 0; i < toWrite; i++) fprintf(debugFile, "%02X ", IOmap[absOffset + i]); fprintf(debugFile, "\n"); fclose(debugFile); }
    // #endif
    
    return true;
}

// Debug logging to staged_write_trace.log removed



bool EtherCATMaster::StageOutputBytes(int absOffset, const uint8_t* data, int len,
                                      const char* callerFile, int callerLine, const char* callerFunc) {
    (void)callerFile; (void)callerLine; (void)callerFunc; // debug trace removed
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to StageOutputBytes");
        return false;
    }
    EnterCriticalSection(&pdoDataCS);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) {
        LeaveCriticalSection(&pdoDataCS);
        SetError("StageOutputBytes: absOffset out of range");
        return false;
    }
    constexpr int kMaxStagedWriteBytes = 64;
    int toWrite = (len < maxWrite) ? len : maxWrite;
    if (toWrite > kMaxStagedWriteBytes) {
        std::stringstream ss;
        ss << "StageOutputBytes: refusing oversized staged write (toWrite=" << toWrite << ") absOffset=" << absOffset;
        LeaveCriticalSection(&pdoDataCS);
        SetError(ss.str());
        return false;
    }
    
    // CRITICAL FIX: Write DIRECTLY to IOmap as well as staging.
    // This ensures that PowerOn values are immediately in IOmap when SendProcessDataOnce
    // is called, rather than waiting for CyclicTask to apply them (which may race).
    // Defensive copy: ensure we never call memcpy/memmove with a null source.
    if (!safe_memcpy(IOmap + absOffset, data, (size_t)toWrite)) {
        LeaveCriticalSection(&pdoDataCS);
        std::stringstream ss; ss << "StageOutputBytes: safe_memcpy failed absOffset=" << absOffset << " toWrite=" << toWrite;
        SetError(ss.str());
        return false;
    }

    std::vector<uint8_t> v(data, data + toWrite);
    // Store/replace the staged output at the absolute offset key
    stagedOutputs[absOffset] = std::move(v);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}


bool EtherCATMaster::StageOutputIfNotExists(int absOffset, const uint8_t* data, int len,
                                            const char* callerFile, int callerLine, const char* callerFunc) {
    (void)callerFile; (void)callerLine; (void)callerFunc; // debug trace removed
    if (!data || absOffset < 0 || len <= 0) {
        SetError("Invalid parameters to StageOutputIfNotExists");
        return false;
    }
    EnterCriticalSection(&pdoDataCS);
    int maxWrite = (int)sizeof(IOmap) - absOffset;
    if (maxWrite <= 0) {
        LeaveCriticalSection(&pdoDataCS);
        SetError("StageOutputIfNotExists: absOffset out of range");
        return false;
    }
    if (stagedOutputs.find(absOffset) != stagedOutputs.end()) {
        // already staged; do not overwrite
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }
    constexpr int kMaxStagedWriteBytes = 64;
    int toWrite = (len < maxWrite) ? len : maxWrite;
    if (toWrite > kMaxStagedWriteBytes) {
        std::stringstream ss;
        ss << "StageOutputIfNotExists: refusing oversized staged write (toWrite=" << toWrite << ") absOffset=" << absOffset;
        LeaveCriticalSection(&pdoDataCS);
        SetError(ss.str());
        return false;
    }
    
    // CRITICAL FIX: Write DIRECTLY to IOmap as well as staging.
    if (!safe_memcpy(IOmap + absOffset, data, (size_t)toWrite)) {
        LeaveCriticalSection(&pdoDataCS);
        SetError("StageOutputIfNotExists: safe_memcpy failed");
        return false;
    }
    
    std::vector<uint8_t> v;
    if (toWrite > 0) {
        v.assign(data, data + toWrite);
    }
    stagedOutputs[absOffset] = std::move(v);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}


void EtherCATMaster::ClearStagedOutputsForCyclicMotionCommands() {
    // This function clears staged outputs for the cyclic motion command PDOs across all slaves.
    // Called when PDO filtering is disabled to prevent old filter values from
    // overwriting manual inputs.
    EnterCriticalSection(&pdoDataCS);

    struct OutputRange {
        int offset;
        int bytes;
    };

    // We need to find both the logical target-position channel (0x607A) and the
    // active cyclic command PDO (0x60FF for CSV), remove them from stagedOutputs,
    // and zero their current IOmap bytes so the last CSV command is not re-used.
    std::vector<OutputRange> rangesToClear;

    auto addRangeToClear = [&](int offset, int bytes) {
        if (bytes <= 0) return;
        if (offset < 0 || offset >= (int)sizeof(IOmap)) return;

        const int clampedBytes = (std::min)(bytes, (int)sizeof(IOmap) - offset);
        if (clampedBytes <= 0) return;

        for (auto &range : rangesToClear) {
            if (range.offset == offset) {
                if (clampedBytes > range.bytes) {
                    range.bytes = clampedBytes;
                }
                return;
            }
        }

        rangesToClear.push_back(OutputRange{offset, clampedBytes});
    };
    
    EnterCriticalSection(&cs);
    for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
        // Get output base for this slave
        int outBase = -1;
        if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIdx].outputs),
                               reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outBase)) {
            outBase = context.slavelist[slaveIdx].Ostartbit / 8;
        }
        
        // Fallback when outBase is zero and not the first slave
        if (outBase == 0 && slaveIdx > 1) {
            int accOutBits = 0;
            for (int i = 1; i < slaveIdx; ++i) {
                accOutBits += context.slavelist[i].Obits;
            }
            int fallbackOut = accOutBits / 8;
            if (fallbackOut > 0) {
                outBase = fallbackOut;
            }
        }
        if (outBase < 0) {
            continue;
        }
        
        // Find staged motion-command descriptors for this slave
        auto descIt = pdoDisplayDescriptors.find(slaveIdx);
        if (descIt != pdoDisplayDescriptors.end()) {
            for (const auto& desc : descIt->second) {
                if (desc.subindex != 0) {
                    continue;
                }
                if (desc.index == 0x607A || desc.index == CoreHelpers::kDefaultCyclicCommandPdoIndex) {
                    int bytes = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : (int)sizeof(int32_t);
                    if (bytes <= 0) {
                        bytes = (int)sizeof(int32_t);
                    }
                    addRangeToClear(outBase + desc.byteOffset, bytes);
                }
            }
        } else {
            // Fallback to the standard RxPDO layout when descriptor metadata is unavailable.
            addRangeToClear(outBase + 2, (int)sizeof(int32_t));
            addRangeToClear(outBase + DefaultCyclicCommandFallbackOffset(), (int)sizeof(int32_t));
        }
    }
    LeaveCriticalSection(&cs);
    
    // Remove the staged motion-command outputs and clear the currently latched IOmap bytes.
    for (const auto& range : rangesToClear) {
        auto it = stagedOutputs.find(range.offset);
        if (it != stagedOutputs.end()) {
            stagedOutputs.erase(it);
        }
        memset(IOmap + range.offset, 0, (size_t)range.bytes);
    }
    
    LeaveCriticalSection(&pdoDataCS);
}


void EtherCATMaster::RegisterPDODescriptors(int slaveIndex, const std::vector<PDOChannelDescriptor>& descriptors) {
    if (slaveIndex <= 0) return;
    EnterCriticalSection(&pdoDataCS);
    if (descriptors.empty()) {
        pdoDisplayDescriptors.erase(slaveIndex);
        pdoDisplayValues.erase(slaveIndex);
    } else {
        pdoDisplayDescriptors[slaveIndex] = descriptors;
        // Drop any stale values so UI will wait for next cycle-produced data
        pdoDisplayValues.erase(slaveIndex);
    }
    LeaveCriticalSection(&pdoDataCS);
}


bool EtherCATMaster::GetPDOChannelValues(int slaveIndex, std::vector<PDOChannelValue>& outValues) {
    outValues.clear();
    if (slaveIndex <= 0) return false;
    EnterCriticalSection(&pdoDataCS);
    auto it = pdoDisplayValues.find(slaveIndex);
    if (it != pdoDisplayValues.end()) {
        outValues = it->second;
        LeaveCriticalSection(&pdoDataCS);
        return !outValues.empty();
    }
    LeaveCriticalSection(&pdoDataCS);
    return false;
}

// ============================================================================
// UI Performance Optimization: Lock-free accessors using double-buffered cache
// ============================================================================

bool EtherCATMaster::TryGetPDOChannelValuesLockFree(int slaveIndex, std::vector<PDOChannelValue>& outValues, uint64_t* outVersion) {
    outValues.clear();
    if (slaveIndex <= 0) return false;
    
    // Read from the current read buffer (set by RT thread via atomic swap)
    UIDisplayCache* cache = uiCacheRead.load(std::memory_order_acquire);
    if (!cache) return false;
    
    auto it = cache->pdoValues.find(slaveIndex);
    if (it != cache->pdoValues.end() && !it->second.empty()) {
        outValues = it->second;
        if (outVersion) *outVersion = uiCacheVersion.load(std::memory_order_acquire);
        return true;
    }
    return false;
}


bool EtherCATMaster::TryGetMechanicalUnitMetricsLockFree(int slaveIndex, MechanicalUnitMetrics& outMetrics, uint64_t* outVersion) {
    if (slaveIndex <= 0) return false;
    
    // Read from the current read buffer (set by RT thread via atomic swap)
    UIDisplayCache* cache = uiCacheRead.load(std::memory_order_acquire);
    if (!cache) return false;
    
    auto it = cache->mechMetrics.find(slaveIndex);
    if (it != cache->mechMetrics.end()) {
        outMetrics = it->second;
        if (outVersion) *outVersion = uiCacheVersion.load(std::memory_order_acquire);
        return true;
    }
    return false;
}


void EtherCATMaster::SetMechanicalScalingFactors(int slaveIndex, double p1Scale, bool invertSign) {
    if (slaveIndex <= 0) return;
    EnterCriticalSection(&pdoDataCS);
    MotionScaling &scaleEntry = motionScalingFactors[slaveIndex];
    scaleEntry.scale = p1Scale;
    scaleEntry.invertSign = invertSign;
    LeaveCriticalSection(&pdoDataCS);
}


bool EtherCATMaster::GetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& outMetrics) {
    if (slaveIndex <= 0) return false;
    EnterCriticalSection(&pdoDataCS);
    auto it = mechanicalUnitValues.find(slaveIndex);
    if (it != mechanicalUnitValues.end()) {
        outMetrics = it->second;
        LeaveCriticalSection(&pdoDataCS);
        return true;
    }
    LeaveCriticalSection(&pdoDataCS);
    return false;
}


void EtherCATMaster::UpdateMechanicalUnitsForSlave(int slaveIndex, const std::vector<uint8_t>& snapshot,
                                                  const std::vector<PDOChannelDescriptor>& descriptors,
                                                  const MotionScaling& scaling)
{
    MechanicalUnitMetrics metrics;
    metrics.targetPosition = L"N/A";
    metrics.actualPosition = L"N/A";
    metrics.targetSpeed = L"N/A";
    metrics.actualSpeed = L"N/A";

    auto applyScaling = [&](uint16_t index, const uint8_t subidx, std::wstring &outValue) {
        PDOChannelDescriptor desc;
        if (!TryFindDescriptorByIndex(descriptors, index, subidx, desc)) return;
        int32_t raw = 0;
        if (!ReadDescriptorValue(context, slaveIndex, snapshot, desc, raw)) return;
        double scaled = ScalePDOToMechanical(raw, scaling.scale, scaling.invertSign, index);
        outValue = FormatMechanicalValueString(scaled);
    };

    applyScaling(0x607A, 0, metrics.targetPosition);
    applyScaling(0x6064, 0, metrics.actualPosition);
    applyScaling(CoreHelpers::kDefaultCyclicCommandPdoIndex, 0, metrics.targetSpeed);
    applyScaling(0x606C, 0, metrics.actualSpeed);

    metrics.valid = (metrics.targetPosition != L"N/A") ||
                    (metrics.actualPosition != L"N/A") ||
                    (metrics.targetSpeed != L"N/A") ||
                    (metrics.actualSpeed != L"N/A");

    EnterCriticalSection(&pdoDataCS);
    mechanicalUnitValues[slaveIndex] = metrics;
    LeaveCriticalSection(&pdoDataCS);
}


bool EtherCATMaster::HasPendingPDOOutput(const std::string& itemName) {
    EnterCriticalSection(&pdoDataCS);
    bool found = (pendingPDOOutputs.find(itemName) != pendingPDOOutputs.end());
    LeaveCriticalSection(&pdoDataCS);
    return found;
}


bool EtherCATMaster::GetSlaveIOBaseBytes(int slaveIndex, int &outBase, int &inBase) {
    outBase = -1; inBase = -1;
    EnterCriticalSection(&cs);
    if (!isInitialized || slaveIndex <= 0 || slaveIndex > slaveCount) {
        LeaveCriticalSection(&cs);
        return false;
    }
    // Prefer exact byte offsets from SOEM-provided IOmap pointers when available
    if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].outputs),
                           reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outBase)) {
        outBase = context.slavelist[slaveIndex].Ostartbit / 8;
    }
    if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].inputs),
                           reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), inBase)) {
        inBase  = context.slavelist[slaveIndex].Istartbit / 8;
    }

    // Fallback: if startbits are zero for this slave (and it's not the first),
    // try to compute bases by accumulating previous slaves' bit lengths.
    if ((outBase == 0 && inBase == 0) && slaveIndex > 1) {
        int accOutBits = 0;
        int accInBits = 0;
        for (int i = 1; i < slaveIndex; ++i) {
            accOutBits += context.slavelist[i].Obits;
            accInBits  += context.slavelist[i].Ibits;
        }
        // convert bits -> bytes (round down)
        int fallbackOut = accOutBits / 8;
        int fallbackIn  = accInBits / 8;
        // Only use fallback if it yields non-zero values
        if (fallbackOut > 0 || fallbackIn > 0) {
            outBase = fallbackOut;
            inBase = fallbackIn;
        }
    }

    const int ioMapSize = (int)sizeof(IOmap);
    const bool outNeeded = (context.slavelist[slaveIndex].Obytes > 0);
    const bool inNeeded  = (context.slavelist[slaveIndex].Ibytes > 0);
    const bool outValid = (outBase >= 0 && outBase < ioMapSize);
    const bool inValid  = (inBase >= 0 && inBase < ioMapSize);
    if ((outNeeded && !outValid) || (inNeeded && !inValid)) {
        LeaveCriticalSection(&cs);
        return false;
    }

    LeaveCriticalSection(&cs);
    return true;
}


void EtherCATMaster::SendProcessDataOnce() {
    if (simulationMode_.load(std::memory_order_relaxed)) {
        AdvanceSimulation(0.0);
        workingCounter = (slaveCount > 0) ? slaveCount * 2 : 0;
        pdosSentCount.fetch_add(1, std::memory_order_relaxed);
        observedCycleMicros_.store(desiredCycleTime ? desiredCycleTime : 2000, std::memory_order_relaxed);
        return;
    }

    // Apply any staged output bytes to IOmap before sending
    EnterCriticalSection(&pdoDataCS);
    if (!stagedOutputs.empty()) {
        constexpr int kMaxStagedWriteBytes = 64;
        for (auto it = stagedOutputs.begin(); it != stagedOutputs.end(); ) {
            const int absOffset = it->first;
            const std::vector<uint8_t> &data = it->second;

            if (absOffset < 0 || absOffset >= (int)sizeof(IOmap)) {
                std::stringstream ss; ss << "SendProcessDataOnce: stagedOutputs absOffset out of range, erasing. absOffset=" << absOffset;
                SetError(ss.str());
                it = stagedOutputs.erase(it);
                continue;
            }

            const int maxWrite = (int)sizeof(IOmap) - absOffset;
            const int dataSize = (int)data.size();
            if (dataSize <= 0 || dataSize > kMaxStagedWriteBytes) {
                std::stringstream ss; ss << "SendProcessDataOnce: stagedOutputs entry size suspicious, erasing. absOffset=" << absOffset << " size=" << dataSize;
                SetError(ss.str());
                it = stagedOutputs.erase(it);
                continue;
            }

            int toWrite = (dataSize < maxWrite) ? dataSize : maxWrite;
            if (toWrite > kMaxStagedWriteBytes) toWrite = kMaxStagedWriteBytes;
            if (toWrite > 0) {
                if (!safe_memcpy(IOmap + absOffset, data.data(), (size_t)toWrite)) {
                    std::stringstream ss; ss << "SendProcessDataOnce: safe_memcpy failed absOffset=" << absOffset << " toWrite=" << toWrite;
                    SetError(ss.str());
                }
            }
            ++it;
        }
    }
    LeaveCriticalSection(&pdoDataCS);
    
    // Send one processdata roundtrip (standard SOEM practice before OP request)
    ecx_send_processdata(&context);
    ecx_receive_processdata(&context, EC_TIMEOUTRET);
}

// Per-slave cyclic thread implementation

DWORD WINAPI EtherCATMaster::PerSlaveCyclicThreadFunc(LPVOID param) {
    SlaveThreadData* data = (SlaveThreadData*)param;
    data->master->PerSlaveCyclicTask(data->slaveIndex, data->stopEvent);
    return 0;
}


void EtherCATMaster::PerSlaveCyclicTask(uint16_t slaveIndex, HANDLE slaveStopEvent) {
    // Continuously send/receive processdata for this specific slave until stop signal
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    
    // Cache cycle time to avoid repeated access
    uint32_t cycleMicros = desiredCycleTime;
    if (cycleMicros == 0) cycleMicros = 2000; // default 2ms if not set
    
    while (WaitForSingleObject(slaveStopEvent, 0) == WAIT_TIMEOUT) {
        QueryPerformanceCounter(&start);
        
        // Try to acquire lock without blocking to avoid deadlock
        // If lock is held by another operation (e.g., ForceSlaveOperational), skip this cycle
        if (TryEnterCriticalSection(&cs)) {
            // Send/receive processdata for all slaves (SOEM requires group-level operation)
            ecx_send_processdata(&context);
            ecx_receive_processdata(&context, EC_TIMEOUTRET);
            
            // Quick check: if slave lost OP, mark for recovery but don't block here
            if ((context.slavelist[slaveIndex].state & EC_STATE_OPERATIONAL) != EC_STATE_OPERATIONAL) {
                context.slavelist[slaveIndex].state = EC_STATE_OPERATIONAL;
                ecx_writestate(&context, slaveIndex);
            }
            
            LeaveCriticalSection(&cs);
        }
        // If couldn't get lock, just sleep and try next cycle
        
        // Sleep for cycle time (use UI-configured value)
        QueryPerformanceCounter(&end);
        LONGLONG elapsed_us = ((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart;
        LONGLONG sleep_us = (LONGLONG)cycleMicros - elapsed_us;
        if (sleep_us > 0) {
            // 如果剩餘時間 >= 1ms，Sleep 那麼多毫秒；若小於 1ms，用 Sleep(0) 讓出時片以避免忙等
            DWORD sleepMs = (DWORD)(sleep_us / 1000);
            if (sleepMs > 1) {
                Sleep(sleepMs);
            } else {
                // 0 或 1 ms 的情況下使用 Sleep(0) 讓出處理器
                Sleep(0);
            }
        }
    }
}


bool EtherCATMaster::StartSlaveThread(uint16_t slaveIndex) {
    EnterCriticalSection(&cs);
    
    // Check if thread already exists for this slave
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex) {
            if (st.isRunning) {
                LeaveCriticalSection(&cs);
                return true; // Already running
            }
            // Thread exists but not running - clean up first
            if (st.thread) CloseHandle(st.thread);
            if (st.stopEvent) CloseHandle(st.stopEvent);
            st.thread = NULL;
            st.stopEvent = NULL;
        }
    }
    
    // Create new thread data
    SlaveThreadData data;
    data.slaveIndex = slaveIndex;
    data.master = this;
    data.stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    data.isRunning = false;
    data.thread = NULL;
    
    if (!data.stopEvent) {
        LeaveCriticalSection(&cs);
        return false;
    }
    
    // Store in vector first to get stable address
    bool found = false;
    SlaveThreadData* dataPtr = nullptr;
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex) {
            st = data;
            dataPtr = &st;
            found = true;
            break;
        }
    }
    if (!found) {
        slaveThreads.push_back(data);
        dataPtr = &slaveThreads.back();
    }
    
    // Now create thread with stable pointer from vector
    dataPtr->thread = CreateThread(NULL, 0, PerSlaveCyclicThreadFunc, dataPtr, 0, NULL);
    if (!dataPtr->thread) {
        CloseHandle(dataPtr->stopEvent);
        dataPtr->stopEvent = NULL;
        LeaveCriticalSection(&cs);
        return false;
    }
    
    dataPtr->isRunning = true;

    // Raise per-slave cyclic thread priority to improve PDO real-time responsiveness
    if (!SetThreadPriority(dataPtr->thread, THREAD_PRIORITY_HIGHEST)) {
            (void)::GetLastError();
            std::stringstream ss; ss << "SetThreadPriority for slave thread failed";
            std::string msg = ss.str();
            printf("%s\n", msg.c_str());
        }
    // 停用自動優先權提升
    SetThreadPriorityBoost(dataPtr->thread, TRUE);

    // Bind cyclic thread to one available processor to avoid costly thread migration
    {
        DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            if (processMask != 0) {
                // Use centralized round-robin allocator to pick a logical processor representing a physical core
                DWORD_PTR pick = AllocateCoreMaskRoundRobin();
                if (pick) {
                    DWORD_PTR processMask2 = 0, systemMask2 = 0;
                    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask2, &systemMask2)) {
                        DWORD_PTR available2 = processMask2 ? processMask2 : systemMask2;
                        pick &= available2;
                    }
                    if (pick) {
                        DWORD_PTR prev = SetThreadAffinityMask(dataPtr->thread, pick);
                        if (prev == 0) {
                            (void)::GetLastError();
                            std::stringstream ss; ss << "SetThreadAffinityMask failed pick=0x" << std::hex << pick;
                            std::string msg = ss.str();
                            printf("%s\n", msg.c_str());
                        } else {
                            std::stringstream ss; ss << "Cyclic thread affinity set pick=0x" << std::hex << pick << " prev=0x" << prev;
                            std::string msg = ss.str();
                            printf("%s\n", msg.c_str());
                        }
                    }
                }
            } else {
                printf("GetProcessAffinityMask returned empty process mask\n");
            }
        } else {
            (void)::GetLastError();
            std::stringstream ss; ss << "GetProcessAffinityMask failed";
            std::string msg = ss.str();
            printf("%s\n", msg.c_str());
        }
    }
    
    LeaveCriticalSection(&cs);
    return true;
}


void EtherCATMaster::StopSlaveThread(uint16_t slaveIndex) {
    EnterCriticalSection(&cs);
    
    for (auto& st : slaveThreads) {
        if (st.slaveIndex == slaveIndex && st.isRunning) {
            SetEvent(st.stopEvent);
            LeaveCriticalSection(&cs);
            
            WaitForSingleObject(st.thread, 5000);
            
            EnterCriticalSection(&cs);
            CloseHandle(st.thread);
            CloseHandle(st.stopEvent);
            st.thread = NULL;
            st.stopEvent = NULL;
            st.isRunning = false;
            LeaveCriticalSection(&cs);
            return;
        }
    }
    
    LeaveCriticalSection(&cs);
}


bool EtherCATMaster::HasActiveSlaveThreads() const {
    // Check if global cyclic thread is running
    return isRunning;
}


void EtherCATMaster::StartPDOProcessingThread() {
    if (pdoThreadRunning) return;

    pdoStopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (pdoStopEvent == NULL) {
        SetError("Failed to create PDO stop event");
        return;
    }

    pdoProcessingThread = CreateThread(NULL, 0, PDOProcessingThreadFunc, this, 0, NULL);
    if (pdoProcessingThread == NULL) {
        SetError("Failed to create PDO processing thread");
        CloseHandle(pdoStopEvent);
        pdoStopEvent = NULL;
        return;
    }

    // Set high priority for PDO processing thread. Increase conservatively to HIGHEST
    // (avoid TIME_CRITICAL here to leave the most aggressive level for the cyclic timer thread).
        if (!SetThreadPriority(pdoProcessingThread, THREAD_PRIORITY_HIGHEST)) {
            (void)::GetLastError();
            std::stringstream ss; ss << "SetThreadPriority(PDO) failed";
            printf("%s\n", ss.str().c_str());
        }

    // ========================================================================
    // ========================================================================
    // CPU 親和性設定
    // ========================================================================
    // Process 層級的 Affinity 已設定，PDO Processing Thread 自動繼承。
    // 如果需要可在此微調，目前使用 Process 預設親和性即可。
    // ========================================================================
    // CPU 親和性設定
    // ========================================================================
    // Process 層級的 Affinity 已設定，PDO Processing Thread 自動繼承。
    // 目前使用 Process 預設親和性即可。
    {
        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
    }
    
    // Fallback: Use round-robin allocator if explicit core binding is desired (legacy logic preserved)
    {
        // printf("[Info] Using round-robin allocator for PDO thread\n");
            
        DWORD_PTR processMask = 0, systemMask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            if (processMask != 0) {
                DWORD_PTR pick = AllocateCoreMaskRoundRobin();
                if (pick) {
                    DWORD_PTR processMask2 = 0, systemMask2 = 0;
                    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask2, &systemMask2)) {
                        DWORD_PTR available2 = processMask2 ? processMask2 : systemMask2;
                        pick &= available2;
                    }
                    if (pick) {
                        DWORD_PTR prev = SetThreadAffinityMask(pdoProcessingThread, pick);
                        if (prev == 0) {
                            (void)::GetLastError();
                            // std::stringstream ss; ss << "SetThreadAffinityMask(PDO) failed pick=0x" << std::hex << pick;
                            // printf("%s\n", ss.str().c_str());
                        } else {
                            // std::stringstream ss; ss << "PDO thread affinity set pick=0x" << std::hex << pick << " prev=0x" << prev;
                            // printf("%s\n", ss.str().c_str());
                        }
                    }
                }
            }
        }
    }


    // 記錄PDO操作開始時間
    operationStartTime = GetTickCount64();
    
    pdoThreadRunning = true;
}


void EtherCATMaster::StopPDOProcessingThread() {
    if (pdoThreadRunning) {
        SetEvent(pdoStopEvent);

        if (pdoProcessingThread != NULL) {
            WaitForSingleObject(pdoProcessingThread, INFINITE);
            CloseHandle(pdoProcessingThread);
            pdoProcessingThread = NULL;
        }

        if (pdoStopEvent != NULL) {
            CloseHandle(pdoStopEvent);
            pdoStopEvent = NULL;
        }

        pdoThreadRunning = false;
    }

    // Clear cached PDO/runtime data even if the thread was already stopped.
    // This prevents stale staged CSV/CSP commands from being replayed after a reconnect.
    EnterCriticalSection(&pdoDataCS);
    pdoDataSnapshots.clear();
    pendingPDOOutputs.clear();
    pdoDisplayDescriptors.clear();
    pdoDisplayValues.clear();
    mechanicalUnitValues.clear();
    stagedOutputs.clear();
    LeaveCriticalSection(&pdoDataCS);
}


DWORD WINAPI EtherCATMaster::PDOProcessingThreadFunc(LPVOID param) {
    EtherCATMaster* master = static_cast<EtherCATMaster*>(param);
    master->PDOProcessingTask();
    return 0;
}


void EtherCATMaster::PDOProcessingTask() {
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    uint32_t cycleMicros = desiredCycleTime; // Use same cycle time as cyclic task for consistency
    if (cycleMicros == 0) cycleMicros = 2000; // Default 2ms if not set
    struct MechanicalWork {
        int slaveIndex = 0;
        std::vector<uint8_t> snapshot;
        std::vector<PDOChannelDescriptor> descriptors;
        MotionScaling scaling;
    };

    std::array<uint64_t, 6> lastMmfFilterUpdateCounts{};
    std::array<LONGLONG, 6> lastMmfFilterUpdateTicks{};
    std::array<CsvVelocityCommandState, 6> csvVelocityCommandStates{};
    const double cycleSeconds = (cycleMicros > 0) ? (static_cast<double>(cycleMicros) / 1000000.0) : 0.002;
    auto resetCsvVelocityCommandStates = [&]() {
        for (auto& state : csvVelocityCommandStates) {
            state.Reset();
        }
    };

    while (WaitForSingleObject(pdoStopEvent, 0) != WAIT_OBJECT_0) {
        QueryPerformanceCounter(&start);

        if (!isRunning || !isInitialized) {
            lastMmfFilterUpdateCounts.fill(0);
            lastMmfFilterUpdateTicks.fill(0);
            resetCsvVelocityCommandStates();
            Sleep(10);
            continue;
        }

        const bool filteringEnabledThisCycle = pdoFilteringEnabled_.load(std::memory_order_relaxed);
        if (!filteringEnabledThisCycle) {
            resetCsvVelocityCommandStates();
        }

        // NOTE: Redundant stagedOutputs application removed from PDOProcessingTask.
        // CyclicTask already applies stagedOutputs to IOmap before every ecx_send_processdata,
        // so duplicate application here was pure overhead adding unnecessary pdoDataCS contention.

        // Prefetch per-slave IO bases and sizes under cs (short critical section)
        struct IOMeta { int outBase; int inBase; int outBytes; int inBytes; };
        std::vector<IOMeta> metas;
        metas.resize(slaveCount + 1);
        EnterCriticalSection(&cs);
        for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
            IOMeta m{};
            if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIdx].outputs),
                                   reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), m.outBase)) {
                m.outBase = context.slavelist[slaveIdx].Ostartbit / 8;
            }
            if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIdx].inputs),
                                   reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), m.inBase)) {
                m.inBase = context.slavelist[slaveIdx].Istartbit / 8;
            }
            m.outBytes = context.slavelist[slaveIdx].Obytes;
            m.inBytes  = context.slavelist[slaveIdx].Ibytes;
            // Fallback when both bases are zero and not the first slave
            if (m.outBase == 0 && m.inBase == 0 && slaveIdx > 1) {
                int accOutBits = 0, accInBits = 0;
                for (int i = 1; i < slaveIdx; ++i) {
                    accOutBits += context.slavelist[i].Obits;
                    accInBits  += context.slavelist[i].Ibits;
                }
                int fallbackOut = accOutBits / 8;
                int fallbackIn  = accInBits / 8;
                if (fallbackOut > 0 || fallbackIn > 0) {
                    m.outBase = fallbackOut;
                    m.inBase  = fallbackIn;
                }
            }
            const int ioMapSize = (int)sizeof(IOmap);
            if (m.outBytes > 0 && (m.outBase < 0 || m.outBase >= ioMapSize)) {
                m.outBase = -1;
                m.outBytes = 0;
            }
            if (m.inBytes > 0 && (m.inBase < 0 || m.inBase >= ioMapSize)) {
                m.inBase = -1;
                m.inBytes = 0;
            }
            metas[slaveIdx] = m;
        }
        LeaveCriticalSection(&cs);

        // Pre-allocate outside pdoDataCS lock
        std::vector<MechanicalWork> mechanicalJobs;
        mechanicalJobs.reserve(slaveCount);

        // Process legacy name-based pending outputs by staging them into stagedOutputs.
        // CyclicTask applies stagedOutputs to IOmap before every ecx_send_processdata.
        EnterCriticalSection(&pdoDataCS);
        for (const auto& output : pendingPDOOutputs) {
            const std::string& itemName = output.first;
            int slaveIndex = output.second.first;
            int32_t value = output.second.second;
            uint16_t objectIndex = 0;
            if (itemName.find("0x6040") != std::string::npos) objectIndex = 0x6040;
            else if (itemName.find("0x607A") != std::string::npos) objectIndex = 0x607A;
            else if (itemName.find("0x6081") != std::string::npos) objectIndex = 0x6081;
            else if (itemName.find("0x60FF") != std::string::npos) objectIndex = 0x60FF;
            else if (itemName.find("0x6071") != std::string::npos) objectIndex = 0x6071;
            else if (itemName.find("0x6083") != std::string::npos) objectIndex = 0x6083;
            else if (itemName.find("0x6084") != std::string::npos) objectIndex = 0x6084;
            else if (itemName.find("0x6060") != std::string::npos) objectIndex = 0x6060;

            if (objectIndex != 0 && slaveIndex > 0 && slaveIndex <= slaveCount) {
                const IOMeta &m = metas[slaveIndex];
                int writeOffset = -1;
                int writeSize = 0;
                auto descIt = pdoDisplayDescriptors.find(slaveIndex);
                if (descIt != pdoDisplayDescriptors.end()) {
                    PDOChannelDescriptor desc;
                    if (m.outBase >= 0 && TryFindDescriptorByIndex(descIt->second, objectIndex, 0, desc) && desc.isOutput) {
                        writeOffset = m.outBase + desc.byteOffset;
                        writeSize = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : 4;
                        if (writeSize > 4) writeSize = 4;
                    }
                }

                if (writeOffset < 0 && m.outBase >= 0) {
                    switch (objectIndex) {
                    case 0x6040:
                        writeOffset = m.outBase;
                        writeSize = 2;
                        break;
                    case 0x607A:
                        writeOffset = m.outBase + 2;
                        writeSize = 4;
                        break;
                    case 0x60FF:
                        writeOffset = m.outBase + DefaultCyclicCommandFallbackOffset();
                        writeSize = 4;
                        break;
                    case 0x6071:
                        writeOffset = m.outBase + 10;
                        writeSize = 2;
                        break;
                    default:
                        break;
                    }
                }
                if (writeOffset >= 0 && writeSize > 0 && (size_t)writeOffset + (size_t)writeSize <= sizeof(IOmap)) {
                    // create little-endian byte vector
                    std::vector<uint8_t> bytes(writeSize);
                    if (writeSize == 2) {
                        int16_t val16 = (int16_t)value;
                        bytes[0] = (uint8_t)(val16 & 0xFF);
                        bytes[1] = (uint8_t)((val16 >> 8) & 0xFF);
                    } else if (writeSize == 1) {
                        bytes[0] = (uint8_t)(value & 0xFF);
                    } else {
                        int32_t val32 = value;
                        bytes[0] = (uint8_t)(val32 & 0xFF);
                        bytes[1] = (uint8_t)((val32 >> 8) & 0xFF);
                        bytes[2] = (uint8_t)((val32 >> 16) & 0xFF);
                        bytes[3] = (uint8_t)((val32 >> 24) & 0xFF);
                    }
                    // stage the bytes at the absolute IO offset
                    stagedOutputs[writeOffset] = std::move(bytes);
                }
            }
        }
        pendingPDOOutputs.clear();

        // Continue under same pdoDataCS lock for snapshot + display computation
        // (eliminates one Leave/Enter pair to reduce contention with CyclicTask)
        for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
            std::vector<uint8_t> snapshot;
            const IOMeta &m = metas[slaveIdx];
            int totalSize = m.outBytes + m.inBytes;
            if (totalSize > 0 && totalSize <= 1024) {
                snapshot.resize(totalSize);
                if (m.outBase >= 0 && (size_t)m.outBase + (size_t)m.outBytes <= sizeof(IOmap))
                    memcpy(snapshot.data(), IOmap + m.outBase, m.outBytes);
                if (m.inBase >= 0 && (size_t)m.inBase + (size_t)m.inBytes <= sizeof(IOmap))
                    memcpy(snapshot.data() + m.outBytes, IOmap + m.inBase, m.inBytes);
            }

            auto descIt = pdoDisplayDescriptors.find(slaveIdx);
            MechanicalWork job;
            job.slaveIndex = slaveIdx;
            job.snapshot = snapshot;
            if (descIt != pdoDisplayDescriptors.end()) {
                job.descriptors = descIt->second;
            }
            auto scaleIt = motionScalingFactors.find(slaveIdx);
            if (scaleIt != motionScalingFactors.end()) job.scaling = scaleIt->second;
            mechanicalJobs.push_back(std::move(job));
            if (descIt != pdoDisplayDescriptors.end()) {
                std::vector<PDOChannelValue> computed;
                computed.reserve(descIt->second.size());
                for (const auto &desc : descIt->second) {
                    PDOChannelValue val;
                    val.descriptor = desc;
                    val.valid = false;
                    int bytes = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : 4;
                    if (bytes > 4) bytes = 4;
                    if (bytes > 0 && !snapshot.empty()) {
                        int idxInSnapshot = desc.isOutput ? desc.byteOffset : (m.outBytes + desc.byteOffset);
                        if (idxInSnapshot >= 0 && idxInSnapshot + bytes <= (int)snapshot.size()) {
                            uint32_t raw = 0;
                            for (int b = 0; b < bytes; ++b) raw |= (uint32_t)snapshot[idxInSnapshot + b] << (8 * b);
                            val.rawValue = (int32_t)raw;
                            val.formattedValue = FormatPDOValueString(val.rawValue, desc.bitLength);
                            val.valid = true;
                        }
                    }
                    if (!val.valid) {
                        val.formattedValue = L"N/A";
                        val.rawValue = 0;
                    }
                    computed.push_back(std::move(val));
                }
                pdoDisplayValues[slaveIdx] = std::move(computed);
            } else {
                pdoDisplayValues.erase(slaveIdx);
            }

            pdoDataSnapshots[slaveIdx] = std::move(snapshot);
        }

        // ========================================================================
        // UI Performance Optimization: Update lock-free double-buffer for UI reads
        // (Merged into existing pdoDataCS scope to eliminate a separate Enter/Leave)
        // ========================================================================
        UIDisplayCache* writeCache = uiCacheWrite.load(std::memory_order_relaxed);
        if (writeCache) {
            writeCache->pdoValues = pdoDisplayValues;
            writeCache->mechMetrics = mechanicalUnitValues;
        }
        LeaveCriticalSection(&pdoDataCS);

        // Atomic pointer swap is lock-free; performed outside pdoDataCS
        if (writeCache) {
            writeCache->updateCounter++;
            UIDisplayCache* oldRead = uiCacheRead.exchange(writeCache, std::memory_order_acq_rel);
            uiCacheWrite.store(oldRead, std::memory_order_release);
            uiCacheVersion.fetch_add(1, std::memory_order_release);
        }

        // ========================================================================
        // PDO Recording: Write CSV entries for all slaves if recording is enabled
        // ========================================================================
        // ========================================================================
        // PDO Recording & MMF Output: Write CSV entries and update MMF
        // ========================================================================
        bool recordEnabled = pdoRecordingEnabled_.load(std::memory_order_relaxed);
        bool mmfEnabled = mmfOutputEnabled_.load(std::memory_order_relaxed);

        if (recordEnabled || mmfEnabled) {
            // Get epoch time (milliseconds since 1970-01-01 00:00:00 UTC)
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            uint64_t epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
            uint64_t uptimeMs = GetTickCount64();
            
            // Read MMF_IN values (6 floats for 6 slaves)
            std::array<float, 6> mmfInValues = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            if (mmf1ReaderRunning.load()) {
                std::array<uint64_t, 6> ignoredUpdateCounts{};
                mmf1Reader.GetLatestWithUpdateCounts(mmfInValues, ignoredUpdateCounts);
            }
            
            for (const auto &job : mechanicalJobs) {
                int slaveIdx = job.slaveIndex;
                const auto &snapshot = job.snapshot;
                const auto &descriptors = job.descriptors;
                const auto &scaling = job.scaling;
                
                if (snapshot.empty() || descriptors.empty()) continue;
                
                // Helper lambda to read PDO value by index/subindex
                auto ReadPDOValue = [&](uint16_t index, uint8_t subindex) -> int32_t {
                    PDOChannelDescriptor desc;
                    if (!TryFindDescriptorByIndex(descriptors, index, subindex, desc)) return 0;
                    int32_t value = 0;
                    ReadDescriptorValue(context, slaveIdx, snapshot, desc, value);
                    return value;
                };
                
                // Convert to mechanical units using scaling
                auto ToMechanical = [&](int32_t raw, uint16_t index) -> double {
                    return ScalePDOToMechanical(raw, scaling.scale, scaling.invertSign, index);
                };
                
                // Read standard CANopen DS402 PDO objects
                uint16_t controlWord = (uint16_t)ReadPDOValue(0x6040, 0);  // Controlword
                uint16_t statusWord  = (uint16_t)ReadPDOValue(0x6041, 0);  // Statusword
                
                // Read raw values
                int32_t targetPosRaw = ReadPDOValue(0x607A, 0);            // Target Position
                int32_t targetVelRaw = ReadPDOValue(CoreHelpers::kDefaultCyclicCommandPdoIndex, 0); // Active cyclic command PDO
                int32_t actualPosRaw = ReadPDOValue(0x6064, 0);            // Position Actual Value
                int32_t actualVelRaw = ReadPDOValue(0x606C, 0);            // Velocity Actual Value
                
                // Convert to mechanical units (mm, mm/s)
                double targetPos = ToMechanical(targetPosRaw, 0x607A);
                double targetVel = ToMechanical(targetVelRaw, CoreHelpers::kDefaultCyclicCommandPdoIndex);
                double actualPos = ToMechanical(actualPosRaw, 0x6064);
                double actualVel = ToMechanical(actualVelRaw, 0x606C);
                
                // Get MMF_IN value for this slave
                float mmfIn = (slaveIdx >= 1 && slaveIdx <= 6) ? mmfInValues[slaveIdx - 1] : 0.0f;
                
                // Expose a passive output while filtering is disabled or the
                // filter has not initialized from feedback yet. Without this,
                // log/MMF viewers see the constructor default 0.0 and render a
                // false startup jump before the first Filter() pass seeds the
                // internal output from actual position.
                const bool filteringEnabled = filteringEnabledThisCycle;
                double pdoFilterOutput = actualPos;
                double pdoFilterVel = actualVel;
                double pdoFilterAcc = 0.0;
                {
                    // Lock to safely access pdoFilters_
                    EnterCriticalSection(&pdoFilterCS_);
                    auto it = pdoFilters_.find(slaveIdx);
                    if (it != pdoFilters_.end()) {
                        if (filteringEnabled && it->second.IsInitialized()) {
                            pdoFilterOutput = it->second.GetFilteredOutput();
                            pdoFilterVel = it->second.GetInternalVelocity();
                            pdoFilterAcc = it->second.GetInternalAcceleration();
                        }
                    }
                    LeaveCriticalSection(&pdoFilterCS_);
                }
                
                // Write to circular buffer
                if (recordEnabled) {
                    WritePDOLogEntry(slaveIdx, epochMs, uptimeMs, 
                                     mmfIn,
                                     controlWord, statusWord, 
                                     targetPos, targetVel, 
                                     actualPos, actualVel,
                                     pdoFilterOutput,
                                     pdoFilterVel,
                                     pdoFilterAcc);
                }

                // Write to MMF
                if (mmfEnabled) {
                    // targetAcc/actualAcc not available in PDO log, passing 0.0
                    mmfWriter_.WriteSlave(slaveIdx, targetPos, targetVel, 
                                          actualPos, actualVel, 
                                          pdoFilterOutput, pdoFilterVel, pdoFilterAcc,
                                          mmfIn, 
                                          controlWord, statusWord,
                                          0.0, 0.0);
                }
            }
            
            if (mmfEnabled) {
                mmfWriter_.UpdateHeader(epochMs);
            }
        }

        // ========================================================================
        // PDO Filtering: Apply acceleration/deceleration/jerk limits to MMF input
        // ========================================================================
        if (filteringEnabledThisCycle) {
            // Read latest MMF1_IN values (6 floats for 6 slaves)
            std::array<float, 6> mmfInValues = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            std::array<uint64_t, 6> mmfUpdateCounts{};
            if (mmf1ReaderRunning.load()) {
                mmf1Reader.GetLatestWithUpdateCounts(mmfInValues, mmfUpdateCounts);
            }
            uint64_t pose6dUpdateCount = 0;
            for (uint64_t count : mmfUpdateCounts) {
                pose6dUpdateCount += count;
            }

            // ---- Pose6D → Servo Angle IK conversion (if enabled) ----
            if (mmf1InputMode_ == Mmf1InputMode::Pose6D) {
                auto snap = ReadStewartParamsSnapshot();
                WuSOEM::Pose6D pose = WuSOEM::MmfPose6DOrder::BuildPoseFromMmfInput(mmfInValues);

                const std::array<long double, 3> poseDelta{{
                    pose.x,
                    pose.y,
                    pose.z,
                }};

                if (mmf1PoseCoordinateSystem_ == Mmf1PoseCoordinateSystem::Platform) {
                    const auto worldDelta = WuSOEM::StewartIK::TransformPlatformToWorldDelta(poseDelta, pose);
                    pose.x = worldDelta[0];
                    pose.y = worldDelta[1];
                    pose.z = snap.initialHeight + worldDelta[2];
                } else {
                    pose.x = poseDelta[0];
                    pose.y = poseDelta[1];
                    pose.z = snap.initialHeight + poseDelta[2];
                }

                auto ikResult = WuSOEM::StewartIK::CalculateInverse(snap.params, pose);
                if (ikResult.valid) {
                    for (int i = 0; i < 6; ++i) {
                        mmfInValues[i] = static_cast<float>(ikResult.servoAngles[i]);
                    }
                    lastValidServoAngles_ = mmfInValues;
                } else {
                    // IK failed: use last valid servo angles
                    mmfInValues = lastValidServoAngles_;
                }

                // A Pose6D sample is transformed into all six servo-angle
                // setpoints. Treat any pose-channel update as a new sample for
                // every servo axis so the filter does not infer stale input
                // from unrelated source-channel indices.
                mmfUpdateCounts.fill(pose6dUpdateCount);
            }
            // ---- End Pose6D IK conversion ----

            // Process each slave with PDO filter
            EnterCriticalSection(&pdoFilterCS_);
            for (const auto &job : mechanicalJobs) {
                int slaveIdx = job.slaveIndex;
                if (slaveIdx < 1 || slaveIdx > 6) continue; // Only process slaves 1-6
                
                const auto &snapshot = job.snapshot;
                const auto &descriptors = job.descriptors;
                const auto &scaling = job.scaling;
                
                // Check if filter is configured for this slave
                auto filterIt = pdoFilters_.find(slaveIdx);
                if (filterIt == pdoFilters_.end()) continue;
                
                // Read actual position from PDO 0x6064 (Position Actual Value)
                int32_t actualPosRaw = 0;
                PDOChannelDescriptor desc;
                if (TryFindDescriptorByIndex(descriptors, 0x6064, 0, desc)) {
                    ReadDescriptorValue(context, slaveIdx, snapshot, desc, actualPosRaw);
                }

                // Convert actual position to mechanical units using scaling.
                // Display paths normalize position objects to one turn, but the
                // CSV filter must use a position unwrapped near its current
                // command/reference so crossing +/-180 degrees does not look
                // like a 360-degree feedback jump.
                double sign = scaling.invertSign ? -1.0 : 1.0;
                double actualPosMechanical = ScalePDOToMechanical(actualPosRaw, scaling.scale, scaling.invertSign, 0x6064);
                
                // Get MMF input value (mechanical units, e.g., degrees or mm)
                float mmfSetValue = mmfInValues[slaveIdx - 1];
                const int mmfAxisIndex = slaveIdx - 1;
                const uint64_t mmfUpdateCount = mmfUpdateCounts[mmfAxisIndex];
                const bool mmfInputStateKnown = mmf1ReaderRunning.load(std::memory_order_relaxed) && mmfUpdateCount > 0;
                const bool mmfInputUpdated = mmfInputStateKnown && mmfUpdateCount != lastMmfFilterUpdateCounts[mmfAxisIndex];
                double mmfInputElapsedSec = std::numeric_limits<double>::quiet_NaN();
                if (mmfInputUpdated && lastMmfFilterUpdateCounts[mmfAxisIndex] != 0 && lastMmfFilterUpdateTicks[mmfAxisIndex] > 0) {
                    const LONGLONG tickDelta = start.QuadPart - lastMmfFilterUpdateTicks[mmfAxisIndex];
                    if (tickDelta > 0 && frequency.QuadPart > 0) {
                        mmfInputElapsedSec = static_cast<double>(tickDelta) / static_cast<double>(frequency.QuadPart);
                    }
                }

                double unwrapReference = static_cast<double>(mmfSetValue);
                if (filterIt->second.IsInitialized()) {
                    unwrapReference = filterIt->second.GetFilteredOutput();
                }
                actualPosMechanical = UnwrapMechanicalPositionNear(actualPosMechanical, unwrapReference);

                // If MMF1_IN is not yet running and this axis is in soft-start,
                // use actual position as temporary setpoint to avoid the first-run
                // jump/stutter toward startup default MMF values.
                if (!mmf1ReaderRunning.load(std::memory_order_relaxed) &&
                    filterIt->second.IsSoftStartActive()) {
                    mmfSetValue = static_cast<float>(actualPosMechanical);
                }
                lastPdoFilterInputAngles_[mmfAxisIndex].store(static_cast<double>(mmfSetValue), std::memory_order_relaxed);
                
                // Check if filter needs initialization
                bool filterNeedsInit = !filterIt->second.IsInitialized();
                const int csvStateIndex = slaveIdx - 1;
                if (filterNeedsInit &&
                    csvStateIndex >= 0 &&
                    csvStateIndex < static_cast<int>(csvVelocityCommandStates.size())) {
                    csvVelocityCommandStates[csvStateIndex].Reset();
                }
                
                // During soft-start, the filter will naturally track the MMF setpoint
                // starting from the actual position. The filter's internal TriggerSoftStart()
                // sets ioInitialized_=false, so the first Filter() call will initialize
                // oFilteredOutput_ to iActValue. From there, the filter will smoothly
                // ramp towards the MMF setpoint using reduced velocity/acceleration limits.
                // 
                // NOTE: We do NOT force mmfSetValue = actualPos here. The filter needs
                // to see the real MMF setpoint to determine when it has "caught up" and
                // soft-start can complete. If we force mmfSetValue = actualPos, the filter
                // would immediately think it's at the target and soft-start would complete
                // instantly.
                
                // Read actual velocity from PDO 0x606C (Velocity Actual Value) if available
                int32_t actualVelRaw = 0;
                double actualVelMechanical = std::numeric_limits<double>::quiet_NaN(); // Default to NaN (not available)
                if (TryFindDescriptorByIndex(descriptors, 0x606C, 0, desc)) {
                    if (ReadDescriptorValue(context, slaveIdx, snapshot, desc, actualVelRaw)) {
                        actualVelMechanical = ScalePDOToMechanical(actualVelRaw, scaling.scale, scaling.invertSign, 0x606C);
                    }
                }
                
                // Apply PDO filter (with acceleration/deceleration/jerk limits)
                // On first call, Filter() will initialize to actualPosMechanical and return it
                double filteredOutput = 0.0;
                try {
                    filteredOutput = filterIt->second.FilterWithInputState(
                        static_cast<double>(mmfSetValue),  // iSetValue (mechanical units)
                        actualPosMechanical,                // iActValue (mechanical units)
                        actualVelMechanical,                // iMeasuredVel (mechanical units, or NaN)
                        mmfInputStateKnown,
                        mmfInputUpdated,
                        mmfInputElapsedSec,
                        !simulationMode_.load(std::memory_order_relaxed)
                    );
                } catch (...) {
                    // Filter error - skip this slave
                    continue;
                }
                if (mmfInputUpdated) {
                    lastMmfFilterUpdateCounts[mmfAxisIndex] = mmfUpdateCount;
                    lastMmfFilterUpdateTicks[mmfAxisIndex] = start.QuadPart;
                }
                // Update lock-free cache of soft-start ready flag for this slave
                try {
                    if (softStartReadyCache_ && softStartReadyCacheSize_ > slaveIdx) {
                        softStartReadyCache_[slaveIdx].store(filterIt->second.IsSoftStartReady() ? 1 : 0, std::memory_order_relaxed);
                    }
                } catch (...) {
                    // swallow - cache update best-effort
                }
                
                // Convert the filter's logical position target and issued velocity
                // back to encoder units. In CSV mode, 0x607A is retained as a
                // logical target/telemetry channel while 0x60FF is the actual motion command.
                if (scaling.scale > 1e-12) { // Avoid division by zero
                    double targetPosRaw = filteredOutput * sign / scaling.scale;
                    int32_t targetPosEncoded = static_cast<int32_t>(std::round(targetPosRaw));
                    
                    // Find offset for PDO 0x607A (logical target position)
                    const IOMeta &m = metas[slaveIdx];
                    int targetPosOffset = m.outBase;
                    int targetVelOffset = m.outBase + 6;
                    
                    // Typically 0x607A is at offset +2 (after 0x6040 controlword which is 2 bytes)
                    // and 0x60FF at offset +6, but prefer descriptor lookup for safety.
                    if (TryFindDescriptorByIndex(descriptors, 0x607A, 0, desc)) {
                        targetPosOffset = m.outBase + desc.byteOffset;
                    } else {
                        targetPosOffset = m.outBase + 2;
                    }
                    if (TryFindDescriptorByIndex(descriptors, 0x60FF, 0, desc)) {
                        targetVelOffset = m.outBase + desc.byteOffset;
                    }
                    
                    // CRITICAL: During PowerOnAllAsync, do NOT overwrite staged motion commands.
                    // 
                    // During soft-start, we DO want to write both the filter's logical position
                    // target (0x607A) and the CSV velocity command (0x60FF). The drive follows
                    // 0x60FF, while 0x607A remains available for logging/UI telemetry.
                    //
                    // Only skip writes when:
                    // 1. PowerOnAllAsync is in progress (powerOnInProgress_)
                    // 2. Filter is not yet initialized (filterNeedsInit) - wait for first Filter() call
                    bool skipWrite = filterNeedsInit || powerOnInProgress_.load(std::memory_order_acquire);
                    
                    if (!skipWrite) {
                        if (csvStateIndex < 0 ||
                            csvStateIndex >= static_cast<int>(csvVelocityCommandStates.size())) {
                            continue;
                        }
                        double csvVelocityCommand = ComputeCsvVelocityCommand(
                            filterIt->second,
                            filteredOutput,
                            actualPosMechanical,
                            actualVelMechanical,
                            csvVelocityCommandStates[csvStateIndex],
                            cycleSeconds,
                            simulationMode_.load(std::memory_order_relaxed) ? kSimulationCsvPositionGainScale : 1.0);
                        double targetVelRaw = csvVelocityCommand * sign / scaling.scale;
                        int32_t targetVelEncoded = static_cast<int32_t>(std::round(targetVelRaw));
                        bool clearHardOutwardCsvNow = false;
                        {
                            const auto [pdoMinLimit, pdoMaxLimit] = GetCsvMotionLimits(filterIt->second);
                            const double hardEps = (std::max)(filterIt->second.GetPositionDeadband(), 1e-6);
                            if (std::isfinite(pdoMaxLimit) &&
                                (actualPosMechanical >= pdoMaxLimit - hardEps || filteredOutput >= pdoMaxLimit - hardEps)) {
                                if (csvVelocityCommand > 0.0) {
                                    targetVelEncoded = 0;
                                }
                                clearHardOutwardCsvNow = true;
                            } else if (std::isfinite(pdoMinLimit) &&
                                       (actualPosMechanical <= pdoMinLimit + hardEps || filteredOutput <= pdoMinLimit + hardEps)) {
                                if (csvVelocityCommand < 0.0) {
                                    targetVelEncoded = 0;
                                }
                                clearHardOutwardCsvNow = true;
                            }
                        }

                        auto Stage32 = [&](int writeOffset, int32_t value) {
                            if (writeOffset < 0 || (size_t)writeOffset + 4 > sizeof(IOmap)) {
                                return;
                            }
                            std::vector<uint8_t> bytes(4);
                            bytes[0] = (uint8_t)(value & 0xFF);
                            bytes[1] = (uint8_t)((value >> 8) & 0xFF);
                            bytes[2] = (uint8_t)((value >> 16) & 0xFF);
                            bytes[3] = (uint8_t)((value >> 24) & 0xFF);
                            stagedOutputs[writeOffset] = std::move(bytes);
                        };

                        Stage32(targetPosOffset, targetPosEncoded);
                        Stage32(targetVelOffset, targetVelEncoded);
                        if (clearHardOutwardCsvNow && targetVelOffset >= 0 &&
                            static_cast<size_t>(targetVelOffset) + sizeof(targetVelEncoded) <= sizeof(IOmap)) {
                            EnterCriticalSection(&pdoDataCS);
                            safe_memcpy(IOmap + targetVelOffset, &targetVelEncoded, sizeof(targetVelEncoded));
                            LeaveCriticalSection(&pdoDataCS);
                        }
                    } else if (csvStateIndex >= 0 &&
                               csvStateIndex < static_cast<int>(csvVelocityCommandStates.size())) {
                        csvVelocityCommandStates[csvStateIndex].Reset();
                    }
                }
            }
            
            // 更新全局違規狀態（在所有從站處理完畢後）
            UpdateGlobalViolationState();
            
            LeaveCriticalSection(&pdoFilterCS_);
        }
        
        // Batch compute mechanical metrics (lock-free), then single-lock writeback.
        // Replaces per-slave UpdateMechanicalUnitsForSlave calls that each acquired
        // pdoDataCS individually, reducing N lock/unlock pairs to 1.
        {
            std::vector<std::pair<int, MechanicalUnitMetrics>> batchMetrics;
            batchMetrics.reserve(mechanicalJobs.size());
            for (auto &job : mechanicalJobs) {
                MechanicalUnitMetrics metrics;
                metrics.targetPosition = L"N/A";
                metrics.actualPosition = L"N/A";
                metrics.targetSpeed = L"N/A";
                metrics.actualSpeed = L"N/A";

                auto applyScaling = [&](uint16_t index, uint8_t subidx, std::wstring &outValue) {
                    PDOChannelDescriptor desc;
                    if (!TryFindDescriptorByIndex(job.descriptors, index, subidx, desc)) return;
                    int32_t raw = 0;
                    if (!ReadDescriptorValue(context, job.slaveIndex, job.snapshot, desc, raw)) return;
                    double scaled = ScalePDOToMechanical(raw, job.scaling.scale, job.scaling.invertSign, index);
                    outValue = FormatMechanicalValueString(scaled);
                };

                applyScaling(0x607A, 0, metrics.targetPosition);
                applyScaling(0x6064, 0, metrics.actualPosition);
                applyScaling(CoreHelpers::kDefaultCyclicCommandPdoIndex, 0, metrics.targetSpeed);
                applyScaling(0x606C, 0, metrics.actualSpeed);

                metrics.valid = (metrics.targetPosition != L"N/A") ||
                                (metrics.actualPosition != L"N/A") ||
                                (metrics.targetSpeed != L"N/A") ||
                                (metrics.actualSpeed != L"N/A");

                batchMetrics.emplace_back(job.slaveIndex, std::move(metrics));
            }
            // Single lock to write all mechanical metrics
            EnterCriticalSection(&pdoDataCS);
            for (auto &p : batchMetrics) {
                mechanicalUnitValues[p.first] = std::move(p.second);
            }
            LeaveCriticalSection(&pdoDataCS);
        }

        // High-precision timing to maintain consistent cycle time
        QueryPerformanceCounter(&end);
        LONGLONG elapsed_us = ((end.QuadPart - start.QuadPart) * 1000000) / frequency.QuadPart;
        LONGLONG remaining_us = (LONGLONG)cycleMicros - elapsed_us;
        if (remaining_us > 0) {
            // Windows high-resolution Waitable Timer solution (Win10 1803+)
            // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION provides microsecond precision with low CPU usage
            static HANDLE hTimer = NULL;
            static bool timerInitFailed = false;
            
            if (hTimer == NULL && !timerInitFailed) {
                // Create high-resolution timer (initialize once)
                hTimer = CreateWaitableTimerExW(
                    NULL,                                   // default security attributes
                    NULL,                                   // unnamed
                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,  // high-resolution flag
                    TIMER_ALL_ACCESS
                );
                
                if (hTimer == NULL) {
                    // High-resolution timer not supported (Windows 10 < 1803)
                    timerInitFailed = true;
                }
            }
            
            if (hTimer != NULL) {
                // DC 相位微調（us）
                int64_t adjUs = 0;
                if (enableDcDriftComp.load(std::memory_order_relaxed) && context.slavelist[0].hasdc) {
                    adjUs = (int64_t)(dcToffNs.load(std::memory_order_relaxed) / 1000LL);
                    int64_t bound = (int64_t)((cycleMicros > 0 ? cycleMicros : 2000) / 4);
                    if (adjUs > bound) adjUs = bound; else if (adjUs < -bound) adjUs = -bound;
                }
                // Use high-resolution timer to wait
                // Negative value indicates relative time, units of 100 nanoseconds
                LARGE_INTEGER dueTime;
                int64_t waitUs = remaining_us - adjUs;
                if (waitUs < 0) waitUs = 0;
                dueTime.QuadPart = -(waitUs * 10LL);  // convert microseconds to 100ns
                
                if (SetWaitableTimer(hTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                    // Wait for timer or stop event
                    HANDLE events[2] = { hTimer, pdoStopEvent };
                    DWORD result = WaitForMultipleObjects(2, events, FALSE, INFINITE);
                    
                    if (result == WAIT_OBJECT_0 + 1) {
                        // pdoStopEvent triggered
                        break;
                    }
                    // result == WAIT_OBJECT_0: timer expired normally
                }
            } else {
                // Fallback solution: Sleep(1) mixed with busy-wait
                // For systems that don't support high-resolution timers
                if (remaining_us > 1500) {
                    Sleep(1);  // coarse wait
                } else {
                    // Short busy-wait
                    QueryPerformanceCounter(&end);
                    long long target = start.QuadPart + ((int64_t)cycleMicros * frequency.QuadPart) / 1000000;
                    while (end.QuadPart < target) {
                        YieldProcessor();
                        QueryPerformanceCounter(&end);
                    }
                }
            }
        }
        // Service any pending request to complete all PDOFilter soft-starts
        if (completeSoftStartRequested_.load(std::memory_order_acquire)) {
            // Perform completion while holding pdoFilterCS_
            EnterCriticalSection(&pdoFilterCS_);
            try {
                for (auto &p : pdoFilters_) {
                    try { p.second.CompleteSoftStart(); } catch (...) {}
                }
            } catch (...) {}
            LeaveCriticalSection(&pdoFilterCS_);
            resetCsvVelocityCommandStates();
            // Clear the request flag
            completeSoftStartRequested_.store(false, std::memory_order_release);
        }
    }
}


bool EtherCATMaster::GetPDODataSnapshot(int slaveIndex, std::vector<uint8_t>& snapshot) {
    // CRITICAL FIX: Read DIRECTLY from IOmap instead of using cached pdoDataSnapshots.
    // The cache may be stale after mode changes (e.g., after Homing).
    // This ensures PowerOn reads the actual current position from the drives.
    
    if (slaveIndex < 1 || slaveIndex > slaveCount) return false;
    
    EnterCriticalSection(&pdoDataCS);
    
    // Get slave IO bases
    int outBase = -1, inBase = -1;
    if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].outputs),
                           reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outBase)) {
        outBase = context.slavelist[slaveIndex].Ostartbit / 8;
    }
    if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].inputs),
                           reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), inBase)) {
        inBase = context.slavelist[slaveIndex].Istartbit / 8;
    }
    
    int outBytes = context.slavelist[slaveIndex].Obytes;
    int inBytes = context.slavelist[slaveIndex].Ibytes;
    
    // Fallback when both bases are zero and not the first slave
    if (outBase == 0 && inBase == 0 && slaveIndex > 1) {
        int accOutBits = 0, accInBits = 0;
        for (int i = 1; i < slaveIndex; ++i) {
            accOutBits += context.slavelist[i].Obits;
            accInBits += context.slavelist[i].Ibits;
        }
        int fallbackOut = accOutBits / 8;
        int fallbackIn = accInBits / 8;
        if (fallbackOut > 0 || fallbackIn > 0) {
            outBase = fallbackOut;
            inBase = fallbackIn;
        }
    }
    
    const int ioMapSize = (int)sizeof(IOmap);
    if ((outBytes > 0 && (outBase < 0 || outBase >= ioMapSize)) ||
        (inBytes > 0 && (inBase < 0 || inBase >= ioMapSize))) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }

    int totalSize = outBytes + inBytes;
    if (totalSize <= 0 || totalSize > 1024) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }
    
    snapshot.resize(totalSize);
    if (outBase >= 0 && (size_t)outBase + (size_t)outBytes <= sizeof(IOmap))
        memcpy(snapshot.data(), IOmap + outBase, outBytes);
    if (inBase >= 0 && (size_t)inBase + (size_t)inBytes <= sizeof(IOmap))
        memcpy(snapshot.data() + outBytes, IOmap + inBase, inBytes);
    
    LeaveCriticalSection(&pdoDataCS);
    return true;
}


bool EtherCATMaster::WritePDOData(int slaveIndex, const std::string& itemName, int32_t value) {
    EnterCriticalSection(&pdoDataCS);
    pendingPDOOutputs[itemName] = std::make_pair(slaveIndex, value);
    LeaveCriticalSection(&pdoDataCS);
    return true;
}



