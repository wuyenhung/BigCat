#include "EtherCATMaster.h"
#include "MmfPose6DOrder.h"
#include "../include/core/AppConfig.h"
#include "../include/core/CpuIsolation.h"
#include "../include/core/CpuTopology.h"
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

// ── OS detection (affects Sleep(1) fallback strategy) ──
// Win11: Sleep(1) granularity 1-3ms → only use when wait > full cycle period
// Win10: Sleep(1) reliably ~1ms   → safe for hybrid bulk-wait + TSC-spin tail
static bool g_isWin11 = false;
// Set when BigCat.ini [Compat] ForceWin10Mode=1 masked a real Win11 detection.
static bool g_win10CompatForced = false;
static void DetectOSVersion() {
    static bool done = false;
    if (done) return; done = true;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
        auto fn = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW info = { sizeof(info) };
            if (fn(&info) == 0) g_isWin11 = (info.dwBuildNumber >= 22000);
        }
    }

    // Manual override: BigCat.ini [Compat] ForceWin10Mode=1 forces the Windows 10
    // cyclic wait strategy (HR waitable timer + 200μs TSC margin) even on Win11,
    // i.e. every `!g_isWin11` guard below behaves as if running on Win10.
    // Read once here (CyclicTask init, outside the PDO cycle): changing the INI
    // requires a reconnect/restart to take effect.
    if (AppConfig::ReadForceWin10Compat()) {
        g_win10CompatForced = g_isWin11;
        g_isWin11 = false;
    }
}

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

// ---------------------------------------------------------------------------
// Staged-output iteration helpers
//
// Apply staged outputs to IOmap.  A corrupted allocator (from a prior data
// race) can leave a vector's internal data pointer as NULL even when size()
// reports a plausible value.  These helpers validate data() before use and
// erase suspicious entries to prevent cascading heap corruption.
// ---------------------------------------------------------------------------

static void ApplyStagedOutputsToIOMap(
    uint8_t* ioMap,
    size_t ioMapSize,
    std::unordered_map<int, std::vector<uint8_t>>& stagedOutputs,
    const char* /*callerName*/)
{
    if (stagedOutputs.empty()) return;

    constexpr int kMaxStagedWriteBytes = 64;
    for (auto it = stagedOutputs.begin(); it != stagedOutputs.end(); ) {
        const int absOffset = it->first;
        const std::vector<uint8_t>& data = it->second;

        if (absOffset < 0 || absOffset >= static_cast<int>(ioMapSize)) {
            it = stagedOutputs.erase(it);
            continue;
        }

        const int maxWrite = static_cast<int>(ioMapSize) - absOffset;
        const int dataSize = static_cast<int>(data.size());
        if (dataSize <= 0 || dataSize > kMaxStagedWriteBytes) {
            it = stagedOutputs.erase(it);
            continue;
        }

        // Defensive: verify the vector's internal data pointer is valid
        // before dereferencing.  A corrupted allocator can leave data()
        // as a wild or null pointer even when size() reports a plausible
        // value.
        const uint8_t* dataPtr = data.data();
        if (!dataPtr) {
            it = stagedOutputs.erase(it);
            continue;
        }

        int toWrite = (dataSize < maxWrite) ? dataSize : maxWrite;
        if (toWrite > kMaxStagedWriteBytes) toWrite = kMaxStagedWriteBytes;
        if (toWrite > 0) {
            (void)safe_memcpy(ioMap + absOffset, dataPtr, static_cast<size_t>(toWrite));
        }
        ++it;
    }
}

static void ApplyDeferredStagedWrites(
    std::unordered_map<int, std::vector<uint8_t>>& stagedOutputs,
    const std::vector<std::pair<int, int32_t>>& deferredWrites)
{
    if (deferredWrites.empty()) return;

    for (const auto& sw : deferredWrites) {
        std::vector<uint8_t> bytes(4);
        bytes[0] = static_cast<uint8_t>(sw.second & 0xFF);
        bytes[1] = static_cast<uint8_t>((sw.second >> 8) & 0xFF);
        bytes[2] = static_cast<uint8_t>((sw.second >> 16) & 0xFF);
        bytes[3] = static_cast<uint8_t>((sw.second >> 24) & 0xFF);
        stagedOutputs[sw.first] = std::move(bytes);
    }
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
        // Startup.csv: 0x607A at offset 0, 0x60FF at offset 4.
        return (CoreHelpers::kDefaultCyclicCommandPdoIndex == 0x60FF) ? 4 : 0;
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
    // After soft-start, enable velocity damping when feedforward
    // speed is below ~15 % of max velocity.  Low-speed moves and
    // near-hold states otherwise run undamped because the
    // post-soft-start correction filter (tau=120 ms) is too slow
    // to stabilise the position-error loop without damping.
    constexpr double kCsvLowSpeedDampingThresholdRatio = 0.15;

    // When the PDOFilter tracking clamp is active (e.g. external force
    // pushes actual outside the tracking window), use a faster correction
    // filter tau so the CSV controller can respond to the disturbance
    // before the phase lag causes oscillation.  Once the clamp clears,
    // the normal slow tau resumes for noise immunity.
    constexpr double kCsvTrackingClampCorrectionFilterTauSec = 0.050;
    // Likewise, use a higher correction acceleration ratio during
    // tracking-clamp events so the correction velocity can change
    // direction quickly enough to damp the disturbance transient.
    constexpr double kCsvTrackingClampCorrectionAccelRatio = 0.20;

    // Integral gain ratio relative to the position P-gain.
    // Standard PIV servo tuning: Ki = Kp / Ti  with Ti ≈ 20–50 ms.
    // At 500 Hz this gives Ki / Kp ≈ dt / Ti = 0.002 / 0.035 ≈ 0.057.
    constexpr double kCsvIntegralGainRatio = 0.05;
    // Integral windup limit as a fraction of max velocity.
    constexpr double kCsvIntegralWindupRatio = 0.10;

    struct CsvVelocityCommandState {
        bool initialized{false};
        double smoothedPositionError{0.0};
        double smoothedActualVelocity{0.0};
        double correctionVelocity{0.0};
        double commandVelocity{0.0};
        double integralError{0.0};
        int trackingClampIntentDir{0};
        bool wasSoftStartActive{false};  // detect ss→normal transition
        int prevClampDir{0};             // detect tracking-clamp transitions

        void Reset()
        {
            initialized = false;
            smoothedPositionError = 0.0;
            smoothedActualVelocity = 0.0;
            correctionVelocity = 0.0;
            commandVelocity = 0.0;
            integralError = 0.0;
            trackingClampIntentDir = 0;
            wasSoftStartActive = false;
            prevClampDir = 0;
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

        // When soft-start completes, the CSV controller transitions
        // from soft-start parameters (reduced gains, soft-start
        // velocity limits) to full normal-mode parameters.  If the
        // soft-start oscillated, the accumulated CSV state
        // (correctionVelocity, commandVelocity, integralError,
        // smoothed errors) carries the oscillation into normal
        // mode.  Reset the state on the transition to give
        // normal mode a clean start.
        const bool softStartActiveNow = filter.IsSoftStartActive();
        if (state.wasSoftStartActive && !softStartActiveNow) {
            // Soft-start → normal transition: keep the command
            // velocity for continuity but reset correction and
            // integral state that may have accumulated oscillation.
            state.correctionVelocity = 0.0;
            state.integralError = 0.0;
            state.smoothedPositionError = 0.0;
            // Do NOT reset commandVelocity — it provides
            // continuity across the transition.
        }
        state.wasSoftStartActive = softStartActiveNow;

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
        // During soft-start, when the actual position leads the
        // command, reduce (but do not zero) the outward feedforward
        // component so the servo is not pushed further past the
        // target.  The reduction is capped at 50 % so the axis
        // never comes to a complete stop, preventing "stop-go"
        // stutter while still limiting overshoot.
        if (filter.IsSoftStartActive() && feedbackLeadOutwardDir != 0) {
            constexpr double kDeadbandDeg = 0.5;
            constexpr double kFullReduceDeg = 2.0;
            const double leadMagnitude = std::abs(positionError);
            if (leadMagnitude > kDeadbandDeg) {
                // alpha: 0 at deadband, 0.5 (max) at full-reduce
                const double alpha = (std::min)(0.5,
                    (leadMagnitude - kDeadbandDeg) /
                    (kFullReduceDeg - kDeadbandDeg) * 0.5);
                if (feedbackLeadOutwardDir > 0 && feedforwardVelocity > 0.0) {
                    feedforwardVelocity *= (1.0 - alpha);
                } else if (feedbackLeadOutwardDir < 0 && feedforwardVelocity < 0.0) {
                    feedforwardVelocity *= (1.0 - alpha);
                }
            }
        }

        // Post-soft-start recovery ramp: when the filter's limits
        // are still ramping back to full (GetMaxVelocity below
        // configured) AND the actual position already leads the
        // filter output, the feedforward pushes the actual even
        // further past the target.  Zero the feedforward in this
        // narrow window so P-correction alone pulls actual back.
        // Do NOT apply this in normal steady-state operation where
        // feedforward is essential for smooth tracking.
        {
            const bool inRecoveryRamp =
                !filter.IsSoftStartActive() &&
                filter.GetConfiguredMaxVelocity() > 0.0 &&
                filter.GetMaxVelocity() < filter.GetConfiguredMaxVelocity() - 1e-6;
            const bool postSoftStartActualLeads =
                inRecoveryRamp &&
                feedbackLeadOutwardDir != 0 &&
                ((feedbackLeadOutwardDir > 0 && feedforwardVelocity > 0.0) ||
                 (feedbackLeadOutwardDir < 0 && feedforwardVelocity < 0.0));
            if (postSoftStartActualLeads) {
                feedforwardVelocity = 0.0;
            }
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
             filter.HasTrackingErrorViolation()) &&
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
        // Allow position-error correction when the feedforward direction
        // has already reversed (away from clamp), so the axis can recover
        // toward the target instead of being frozen in both directions.
        const bool inhibitCorrection =
            inhibitClampOutward &&
            !feedforwardAwayFromClamp &&
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
            // When the PDOFilter tracking clamp is active (e.g. external
            // force pushes actual outside the tracking window), use a
            // faster correction filter tau (50 ms instead of 120 ms) so
            // the CSV controller can respond to the disturbance before
            // the phase lag causes oscillation.  Once the clamp clears,
            // the normal slow tau resumes for noise immunity.
            const bool trackingClampActive = clampDir != 0;
            const double correctionFilterTau = filter.IsSoftStartActive()
                ? kCsvCorrectionFilterTauSec
                : (trackingClampActive
                    ? kCsvTrackingClampCorrectionFilterTauSec
                    : kCsvPostSoftStartCorrectionFilterTauSec);
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
            state.integralError = 0.0;
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
                // During soft-start the position-correction gain is
                // normally reduced to 10 % so the CSV controller does
                // not fight the soft-start trajectory.  However, when
                // the actual position leads the filter output by more
                // than a small deadband the axis has overshot and the
                // standard reduction leaves too little authority to
                // pull it back (the filter is already at the target
                // and the feedforward is near zero).  Ramp the gain
                // toward 30 % as the lead grows.
                constexpr double kLeadSafeDeg = 0.2;   // below this: keep 0.1x
                constexpr double kLeadFullDeg = 0.8;   // above this: 0.3x
                const double leadMagnitude = (std::max)(0.0, -positionError);
                double softStartGainScale = 0.1;
                if (leadMagnitude > kLeadSafeDeg) {
                    const double t = (std::min)(1.0,
                        (leadMagnitude - kLeadSafeDeg) / (kLeadFullDeg - kLeadSafeDeg));
                    softStartGainScale = 0.1 + t * 0.2;  // 0.1 → 0.3
                }
                gain *= softStartGainScale;
            }
            if (std::isfinite(positionGainScale) && positionGainScale > 0.0) {
                gain *= positionGainScale;
            }
            desiredCorrectionVelocity = smoothedError * gain;

            const double maxDeceleration = (std::max)(std::abs(filter.GetMaxDeceleration()), 1e-9);
            const double brakingLimit = std::sqrt(2.0 * maxDeceleration * std::abs(smoothedError) * 1.1);
            correctionVelocityLimit = (std::min)(correctionVelocityLimit, brakingLimit);
        }
        // After soft-start, velocity damping is only enabled when
        // feedforward is inactive (near-hold).  At very low feedforward
        // speeds the correction loop can oscillate because the post-soft-
        // start filter (tau=120 ms) and accel limit (0.08 ratio) are
        // too slow to provide stable tracking without damping.
        // Enable damping below ~15 % of max velocity to suppress this
        // low-speed hunting while keeping full-speed tracking undamped.
        const bool lowSpeedDamping =
            !filter.IsSoftStartActive() && feedforwardActive &&
            maxVelocity > 0.0 &&
            std::abs(feedforwardVelocity) < maxVelocity * kCsvLowSpeedDampingThresholdRatio;
        const bool allowVelocityDamping =
            filter.IsSoftStartActive() || !feedforwardActive || lowSpeedDamping;
        if (actualVelocityAvailable && allowVelocityDamping) {
            const double relativeActualVelocity = state.smoothedActualVelocity - feedforwardVelocity;
            const double dampingRatio = filter.IsSoftStartActive()
                ? kCsvCorrectionVelocityDampingRatio
                : kCsvPostSoftStartHoldVelocityDampingRatio;
            const double rawDampingVelocity = dampingRatio * relativeActualVelocity;
            // At low speed (post-soft-start, feedforward active but
            // below threshold), allow a larger damping fraction so the
            // correction does not swing past the target.
            const double dampingLimit = correctionVelocityLimit *
                (filter.IsSoftStartActive() ? 0.75 : (lowSpeedDamping ? 0.50 : 0.25));
            desiredCorrectionVelocity -= (std::clamp)(rawDampingVelocity,
                                                      -dampingLimit,
                                                      dampingLimit);
        }

        // --- Integral (I) term with anti-windup ---
        // Accumulates position error to eliminate steady-state offset.
        // Standard PIV servo control: Ki = Kp / Ti (Ti ≈ 20-50 ms).
        // Integral is clamped to a fraction of max velocity and frozen
        // when the correction is already at its limit (anti-windup).
        {
            const double integralGain =
                filter.GetPositionGain() * kCsvIntegralGainRatio;
            const double integralWindupLimit =
                maxVelocity * kCsvIntegralWindupRatio;

            // Anti-windup: suspend integration when correction is
            // saturated or when the axis is at a hard limit.
            const bool correctionSaturated =
                std::abs(desiredCorrectionVelocity) >=
                    correctionVelocityLimit - 1e-9;
            const bool atHardLimit =
                hardClampDir != 0 &&
                ((hardClampDir > 0 && positionError > 0.0) ||
                 (hardClampDir < 0 && positionError < 0.0));

            if (!correctionSaturated && !atHardLimit) {
                // Use raw position error (not smoothed) for faster
                // integral response.  The dt is the cycle time.
                const double dt = safeCycleSeconds;
                state.integralError += positionError * integralGain * dt;

                // Clamp to windup limit
                state.integralError = (std::clamp)(state.integralError,
                    -integralWindupLimit, integralWindupLimit);
            }

            // Zero-crossing reset: clear integral when position
            // error changes sign (avoids overshoot from stale
            // accumulated error).
            if ((positionError > 0.0 && state.integralError < 0.0) ||
                (positionError < 0.0 && state.integralError > 0.0)) {
                state.integralError = 0.0;
            }
        }

        // Add the integral term to the correction
        desiredCorrectionVelocity += state.integralError;

        desiredCorrectionVelocity = (std::clamp)(desiredCorrectionVelocity,
                                                 -correctionVelocityLimit,
                                                 correctionVelocityLimit);

        // When the tracking clamp just disengaged (prevClampDir != 0 →
        // clampDir == 0), the filter's GetInternalVelocity() can jump
        // from the suppressed value (0) to the free-motion planned
        // velocity.  This jump feeds directly into CSV feedforward and
        // can cause a velocity transient that starts an oscillation.
        // Reset the correction state to prevent accumulated errors from
        // amplifying this transient.
        if (state.prevClampDir != 0 && clampDir == 0 && !filter.IsSoftStartActive()) {
            state.correctionVelocity = 0.0;
            state.smoothedPositionError = 0.0;
            state.integralError = 0.0;
        }
        state.prevClampDir = clampDir;

        // When the tracking clamp is active (disturbance such as
        // external force), use a higher correction acceleration ratio
        // (0.20 instead of 0.08) so the correction velocity can change
        // direction quickly enough to damp the disturbance transient.
        const bool trackingClampActiveForAccel = clampDir != 0;
        const double correctionAccelRatio = filter.IsSoftStartActive()
            ? kCsvCorrectionAccelRatio
            : (trackingClampActiveForAccel
                ? kCsvTrackingClampCorrectionAccelRatio
                : kCsvPostSoftStartCorrectionAccelRatio);
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
        // Soft-start: capped outward velocity reduction (max 50 %)
        // to prevent stop-go stutter.  Same formula as feedforward.
        if (filter.IsSoftStartActive() && feedbackLeadOutwardDir != 0) {
            constexpr double kDeadbandDeg = 0.5;
            constexpr double kFullReduceDeg = 2.0;
            const double leadMagnitude = std::abs(positionError);
            if (leadMagnitude > kDeadbandDeg) {
                const double alpha = (std::min)(0.5,
                    (leadMagnitude - kDeadbandDeg) /
                    (kFullReduceDeg - kDeadbandDeg) * 0.5);
                if (feedbackLeadOutwardDir > 0 && desiredCommandVelocity > 0.0) {
                    desiredCommandVelocity *= (1.0 - alpha);
                } else if (feedbackLeadOutwardDir < 0 && desiredCommandVelocity < 0.0) {
                    desiredCommandVelocity *= (1.0 - alpha);
                }
            }
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
        // The acceleration-limited step above (from desiredCommandVelocity=0
        // toward state.commandVelocity at maxNegativeStep/maxPositiveStep)
        // already bleeds the command velocity at the configured deceleration
        // rate.  An additional explicit bleed here doubles the deceleration
        // and creates a stop-start hunting pattern at low speed when the
        // actual position repeatedly crosses the lead/error boundary.
        // Soft-start mode still keeps the explicit bleed because the
        // soft-start decel is already intentionally low.
        if (filter.IsSoftStartActive()) {
            if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir > 0 && state.commandVelocity > 0.0) {
                state.commandVelocity = (std::max)(0.0, state.commandVelocity - maxNegativeStep);
            } else if (feedbackLeadLowSpeedInhibit && feedbackLeadOutwardDir < 0 && state.commandVelocity < 0.0) {
                state.commandVelocity = (std::min)(0.0, state.commandVelocity + maxPositiveStep);
            }
        }
        // Soft-start: capped outward velocity reduction (max 50 %)
        // to prevent stop-go stutter.  Same formula as feedforward.
        if (filter.IsSoftStartActive() && feedbackLeadOutwardDir != 0) {
            constexpr double kDeadbandDeg = 0.5;
            constexpr double kFullReduceDeg = 2.0;
            const double leadMagnitude = std::abs(positionError);
            if (leadMagnitude > kDeadbandDeg) {
                const double alpha = (std::min)(0.5,
                    (leadMagnitude - kDeadbandDeg) /
                    (kFullReduceDeg - kDeadbandDeg) * 0.5);
                if (feedbackLeadOutwardDir > 0 && state.commandVelocity > 0.0) {
                    state.commandVelocity *= (1.0 - alpha);
                } else if (feedbackLeadOutwardDir < 0 && state.commandVelocity < 0.0) {
                    state.commandVelocity *= (1.0 - alpha);
                }
            }
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

    // Verify actual priority and log the effective level
    {
        int actualPrio = GetThreadPriority(currentThread);
        DWORD procClass = GetPriorityClass(GetCurrentProcess());
        const char* className = "UNKNOWN";
        if (procClass == REALTIME_PRIORITY_CLASS) className = "REALTIME";
        else if (procClass == HIGH_PRIORITY_CLASS) className = "HIGH";
        else if (procClass == ABOVE_NORMAL_PRIORITY_CLASS) className = "ABOVE_NORMAL";
        else if (procClass == NORMAL_PRIORITY_CLASS) className = "NORMAL";
        // Calculate effective base priority: TIME_CRITICAL = class_base + 15 (sat=15)
        // REALTIME class base=16 → effective 31; HIGH class base=13 → effective 15
        int effectiveBase = 0;
        if (procClass == REALTIME_PRIORITY_CLASS) effectiveBase = 16;
        else if (procClass == HIGH_PRIORITY_CLASS) effectiveBase = 13;
        else if (procClass == ABOVE_NORMAL_PRIORITY_CLASS) effectiveBase = 10;
        else if (procClass == NORMAL_PRIORITY_CLASS) effectiveBase = 8;
        int effectivePrio = effectiveBase + (actualPrio >= 0 ? actualPrio : 0);
        std::stringstream ss;
        ss << "RT-Verify: Process class=" << className
           << " Thread priority=" << actualPrio
           << " Effective base priority=" << effectivePrio
           << " (max user-mode=31, system DPC/ISR always higher)";
        SetError(ss.str());
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
    // Bind the main CyclicTask to the RT physical core's logical core set.
    // If CpuIsolation::InitializeHardIsolation was called at startup, use
    // PinCurrentThreadToRtCores() for hardware-detected full-core isolation
    // (both HT/SMT siblings). Otherwise fall back to legacy Core 4 single-bit.

    if (CoreHelpers::CpuIsolation::IsHardIsolationActive()) {
        if (CoreHelpers::CpuIsolation::PinCurrentThreadToRtCores()) {
            DWORD_PTR mask = CoreHelpers::CpuIsolation::GetRtThreadMask();
            cyclicThreadCoreMask_.store(mask, std::memory_order_relaxed);
            int physCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
            CoreHelpers::CpuTopology::EnsurePopulated();
            const auto* ci = CoreHelpers::CpuTopology::FindCoreByPhysIdx(physCore);
            int primaryLp = (ci && !ci->lps.empty()) ? ci->lps[0] : physCore;
            std::stringstream ss;
            ss << "RT-Optimization: CyclicTask hard-isolated to CPU " << primaryLp
               << " (physical core " << physCore << ", mask 0x" << std::hex << mask
               << ", HT sibling idle, clean L1/L2)";
            SetError(ss.str());

            // Runtime verification: confirm the thread mask via non-destructive
            // re-assertion. SetThreadAffinityMask returns the PREVIOUS mask;
            // calling it with the same mask twice should return the same mask
            // both times if the pin truly succeeded.
            // IMPORTANT: Do NOT widen to sysAffinity and narrow back — that
            // gives the Win11 scheduler a window to migrate the thread off
            // the RT core, and the subsequent narrowing does not force an
            // immediate migration. The thread would stay on the wrong core.
            {
                HANDLE hCur = GetCurrentThread();
                DWORD_PTR reassert = SetThreadAffinityMask(hCur, mask);
                if (reassert != mask) {
                    std::stringstream warn;
                    warn << "RT-Verify: ⚠ AFFINITY MISMATCH — expected mask 0x"
                         << std::hex << mask << " but re-assert returned 0x"
                         << reassert << std::dec
                         << " — cyclic thread may not be on RT core!";
                    SetError(warn.str());
                } else {
                    std::stringstream ok;
                    ok << "RT-Verify: ✓ CyclicTask affinity confirmed at mask 0x"
                       << std::hex << mask << std::dec;
                    SetError(ok.str());
                }
            }
        } else {
            std::stringstream ss;
            ss << "Warning: PinCurrentThreadToRtCores failed — falling back to legacy affinity";
            SetError(ss.str());
        }
    }

    // Legacy / fallback affinity (only used if hard isolation is not active)
    if (!CoreHelpers::CpuIsolation::IsHardIsolationActive() ||
        cyclicThreadCoreMask_.load(std::memory_order_relaxed) == 0) {
        int ethercatTargetCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
        DWORD_PTR kEtherCATCoreMask = 0;
        if (ethercatTargetCore >= 0 && ethercatTargetCore < 64) {
            // Use CpuTopology to map physical core → logical CPU mask
            // instead of assuming physIdx == logical CPU bit position.
            CoreHelpers::CpuTopology::EnsurePopulated();
            kEtherCATCoreMask = CoreHelpers::CpuTopology::GetPrimaryLpMask(ethercatTargetCore);
        }
        if (kEtherCATCoreMask == 0) {
            // Last resort: treat physIdx as logical CPU (works without SMT)
            if (ethercatTargetCore >= 0 && ethercatTargetCore < 64) {
                kEtherCATCoreMask = (DWORD_PTR)1 << ethercatTargetCore;
            }
        }

        DWORD_PTR processMask2 = 0, systemMask2 = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &processMask2, &systemMask2);
        DWORD_PTR available = processMask2 ? processMask2 : systemMask2;

        DWORD_PTR pick = 0;
        if (available & kEtherCATCoreMask) {
            pick = kEtherCATCoreMask;                       // preferred: RT physical core
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

    DetectOSVersion();  // one-time Win10/Win11 detection for Sleep strategy
    if (g_win10CompatForced) {
        // Visible confirmation that [Compat] ForceWin10Mode=1 took effect
        // (this OS was detected as Win11 but the Win10 wait strategy is used).
        SetError("Compat: [Compat] ForceWin10Mode=1 - 已在 Win11 上強制採用 Win10 週期等待策略 (waitable timer + TSC margin)");
    }

    unsigned long sampleCounter = 0;
    const unsigned long sampleInterval = 500;
    unsigned long lastSampleTid = 0;
    unsigned long long lastSampleTotalUs = 0ULL;

    // Use HR timer for low-CPU wait on both Win10 and Win11.
    // (The earlier Win11 DC SYNC0 failure was caused by ecx_readstate
    //  polling in the OP wait loop, not by the HR timer itself.)
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

    // Reset cycle-timing state so the first observed-cycle after a restart
    // does not include the gap since the previous run (which could be
    // seconds, inflating maxObservedCycleMicros_ to 75 ms).
    lastPdoPerfTicks.store(0, std::memory_order_relaxed);
    observedCycleMicros_.store(0, std::memory_order_relaxed);

    constexpr int kPipelinePrimingCycles = 5;
    int pipelinePrimingRemaining = kPipelinePrimingCycles;
    pdoPipelinePrimed_.store(false, std::memory_order_release);

    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {

        LARGE_INTEGER start;
        QueryPerformanceCounter(&start);

        // ============================================================
        // PDO EXCHANGE — DIAG: add lockEnter/lockLeave QPC + comms tracking
        LARGE_INTEGER lockEnterTick;
        QueryPerformanceCounter(&lockEnterTick);
        pdoExchangeActive_.store(true, std::memory_order_release);
        EnterCriticalSection(&pdoDataCS);
        if (!stagedOutputs.empty()) {
            ApplyStagedOutputsToIOMap(IOmap, sizeof(IOmap), stagedOutputs, "CyclicTask");
        }
        // Release pdoDataCS BEFORE the network I/O.  pcap_sendpacket can block
        // for many seconds inside the NDIS driver when the link/slave is dead;
        // holding pdoDataCS across that would stall every StageOutputBytes /
        // SendProcessDataOnce caller (emergency-stop / disconnect paths) and
        // make the UI hang.  Only IOmap memory writes need pdoDataCS.
        LeaveCriticalSection(&pdoDataCS);
        // The SOEM port exchange is serialized by pdoExchangeCS so it can never
        // overlap an external exchange (connect-time ecx_writestate/readstate,
        // ForceSlaveOperational).  Held across the (possibly blocking) send/
        // receive, but no other thread competes for it while the cyclic task
        // is running, so the RT cycle is unaffected.
        EnterCriticalSection(&pdoExchangeCS);
        ecx_send_processdata(&context);
        {
            LARGE_INTEGER recvStart, recvEnd;
            QueryPerformanceCounter(&recvStart);
            workingCounter = ecx_receive_processdata(&context, EC_TIMEOUTRET);
            QueryPerformanceCounter(&recvEnd);
            uint64_t recvTicks = (uint64_t)(recvEnd.QuadPart - recvStart.QuadPart);
            uint32_t recvUs = (perfFreqTicks > 0) ? (uint32_t)((recvTicks * 1000000ULL) / perfFreqTicks) : 0;
            if (recvUs > maxRecvTimeUs_.load(std::memory_order_relaxed)) {
                maxRecvTimeUs_.store(recvUs, std::memory_order_relaxed);
            }
        }
        LeaveCriticalSection(&pdoExchangeCS);
        pdoExchangeActive_.store(false, std::memory_order_release);
        LARGE_INTEGER lockLeaveTick;
        QueryPerformanceCounter(&lockLeaveTick);

        {
            uint32_t commsMicros = 0;
            if (perfFreqTicks > 0) {
                uint64_t commsTicks = (uint64_t)(lockLeaveTick.QuadPart - start.QuadPart);
                commsMicros = (uint32_t)((commsTicks * 1000000ULL) / perfFreqTicks);
            }
            commsTime_.store(commsMicros, std::memory_order_relaxed);
            if (commsMicros > maxObservedCommsTime.load(std::memory_order_relaxed)) {
                maxObservedCommsTime.store(commsMicros, std::memory_order_relaxed);
            }
        }

        // ================================================================
        // OBSERVED CYCLE PERIOD + diagnostics (no post-PDO processing yet)
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
            uint32_t cyclePeriodUs = (uint32_t)((deltaTicks * 1000000ULL) / perfFreqTicks);
            observedCycleMicros_.store(cyclePeriodUs, std::memory_order_relaxed);
            if (cyclePeriodUs > maxObservedCycleMicros_.load(std::memory_order_relaxed)) {
                maxObservedCycleMicros_.store(cyclePeriodUs, std::memory_order_relaxed);
            }

            // ── Spike diagnostics: classify spike as comms or idle ──
            // A spike > 1.5× the configured cycle time (min 3000 μs) is
            // recorded with the dominant phase.  This catches spikes that
            // exceed the normal PDO timing budget (e.g. browser-induced
            // DPC/interrupt bursts) without false-triggering on benign
            // scheduler jitter.
            {
                const uint32_t base = (desiredCycleTime > 100 && desiredCycleTime < 50000)
                    ? desiredCycleTime : 2000;
                // 1.5× desired cycle, floor 3000 μs — sensitive enough to
                // catch a 4.78 ms spike on a 2 ms cycle.
                const uint32_t spikeThresholdUs = (base * 3 / 2 > 3000) ? (base * 3 / 2) : 3000;
                if (cyclePeriodUs >= spikeThresholdUs) {
                    const uint32_t comms = commsTime_.load(std::memory_order_relaxed);
                    // If comms time accounts for >50% of the spike, blame comms;
                    // otherwise the spike happened in the idle-wait (DPC/IPI).
                    const uint32_t phase = (comms * 2 > cyclePeriodUs) ? 1u : 2u;
                    lastSpikePhase_.store(phase, std::memory_order_relaxed);
                    lastSpikeCycleUs_.store(cyclePeriodUs, std::memory_order_relaxed);
                    lastSpikeCommsUs_.store(comms, std::memory_order_relaxed);
                    spikeCount_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if (verboseCyclicLog && pdosSentCount.load(std::memory_order_relaxed) == 1) {
            SetError("[PDO啟動] 首次PDO發送成功，開始連續發送模式");
        }
        if ((pdosSentCount.load(std::memory_order_relaxed) & 0x1FFu) == 0u) {
            pdoDiagCycleUs_.store(observedCycleMicros_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            pdoDiagMaxCycleUs_.store(maxObservedCycleMicros_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        UpdatePdoInputCaches();
        // Drain SOEM error ring buffer for CoE Emergency messages.
        // Emergencies arrive via mailbox (SM0/SM1) — independent of PDO.
        ProcessEmcyErrors();
        {
            static unsigned long checkStatesCounter = 0;
            if (++checkStatesCounter >= 100) {
                checkStatesCounter = 0;
                CheckSlaveStates();
            }
        }
        // MMF PollOnce is now handled by the dedicated MMF receiver thread
        // on a non-RT Worker core. The RT path reads the latest snapshot
        // lock-free via GetLatest()/GetLatestWithUpdateCounts().
        // Removing the duplicate call from CyclicTask prevents the two
        // callers from racing and skewing the MMF update-frequency metric.
        if (cyclicCallback) {
            cyclicCallback();
        }
        // Trigger one foreign-process repel cycle per PDO cycle.
        // The monitor thread runs on the near-RT core, so this
        // lightweight SetEvent call does not affect PDO timing.
        CoreHelpers::CpuIsolation::TriggerRepel();

        if (pipelinePrimingRemaining > 0) {
            --pipelinePrimingRemaining;
            if (pipelinePrimingRemaining == 0) {
                pdoPipelinePrimed_.store(true, std::memory_order_release);
            }
        }

        // Signal PDOProcessingTask: manual-reset event
        if (pdoCycleEvent) {
            SetEvent(pdoCycleEvent);
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

        // ── Cycle-deadline wait ──
        // Win10: HR timer with 200μs early-wake margin + TSC spin tail.
        //   Absorbs kernel timer DPC jitter on the RT core (tested working).
        // Win11: SwitchToThread per-iteration + TSC spin (no SwitchToThread).
        //   Win11 waitable timer is buggy — causes EtherCAT OP init failure
        //   (see WIN11_KNOWN_ISSUES.md).  The timer path is guarded by
        //   !g_isWin11 to keep the original SwitchToThread approach.
        //   SwitchToThread on isolated RT core burns ~95% CPU but is the
        //   only reliable wait for the cyclic thread on Win11.
        constexpr auto kTimerMargin = std::chrono::microseconds(200);
        if (!g_isWin11 && waitTimerValid &&
            waitDuration > kTimerMargin + std::chrono::microseconds(50)) {
            // timer = deadline - 200μs (early wake to absorb timer DPC jitter)
            auto adjustedWait = waitDuration - kTimerMargin;
            auto waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(adjustedWait).count();
            LONGLONG hundredNs = waitNs / 100;
            if (hundredNs <= 0) hundredNs = 1;
            LARGE_INTEGER due;
            due.QuadPart = -hundredNs;
            if (SetWaitableTimer(waitTimer, &due, 0, NULL, NULL, FALSE)) {
                HANDLE handles[2] = { stopEvent, waitTimer };
                DWORD waitRes = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (waitRes == WAIT_OBJECT_0) { stopRequested = true; }
                // Fall through to TSC spin for the remaining margin — even if
                // the timer DPC delays wake-up by tens of microseconds, the
                // spin catches up the difference without missing the deadline.
            } else {
                CancelWaitableTimer(waitTimer);
                CloseHandle(waitTimer);
                waitTimer = nullptr;
                waitTimerValid = false;
            }
        }

        // ── TSC spin tail (Win10 after timer wake; Win11 full idle-wait) ──
        // Win10: reaches here with ~200μs remaining after the timer wakes;
        //        pure TSC spin, no kernel entry.
        // Win11: timer path skipped (!g_isWin11 guard) → full ~1900μs idle
        //        handled here: >1ms → SwitchToThread per iteration (95% CPU,
        //        required for reliable OP init, see WIN11_KNOWN_ISSUES.md);
        //        ≤1ms → pure TSC spin, no SwitchToThread.
        while (!stopRequested) {
            auto now = std::chrono::steady_clock::now();
            if (now >= targetTime) break;
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                stopRequested = true; break;
            }
            auto remaining = targetTime - now;
            if (remaining > std::chrono::microseconds(1000)) {
                // Win11: all idle-wait (>1ms) uses SwitchToThread per
                // iteration — required for reliable EtherCAT OP init
                // despite 95% CPU (WIN11_KNOWN_ISSUES.md).
                // Win10: timer path handles the >1ms gap normally; this
                // branch is hit only if timer creation failed (rare).
                SwitchToThread();
                continue;
            }
            // Pure TSC spin for gaps ≤1ms.  No kernel entry at all.
            auto spinDuration = remaining;
            if (spinDuration > std::chrono::microseconds(500)) {
                spinDuration = std::chrono::microseconds(500);
            }
            const unsigned __int64 spinCycles = DurationToTscCycles(tscSpinCalibration, spinDuration);
            if (spinCycles > 0) {
                unsigned int tscAux = 0;
                const unsigned __int64 spinDeadline = __rdtscp(&tscAux) + spinCycles;
                unsigned int stopPollCounter = 0;
                while (!stopRequested) {
                    if ((++stopPollCounter & 0xFFu) == 0u) {
                        if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                            stopRequested = true; break;
                        }
                    }
                    if (__rdtscp(&tscAux) >= spinDeadline) break;
                    YieldProcessor();
                }
                continue;
            }
            YieldProcessor();
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
    pdoPipelinePrimed_.store(false, std::memory_order_release);
    ResetDcDriftCompensationState();
}


void EtherCATMaster::CheckSlaveStates() {
    // ── Read-only passive monitor ──
    // Intentionally does NOT call ecx_readstate or any SOEM port function.
    // Even with TryEnterCriticalSection(&pdoDataCS), the pdoDataCS window
    // between CyclicTask cycles is enough for this function to acquire the
    // lock, call ecx_readstate (which holds port->rx_mutex), and block the
    // CyclicTask's next ecx_receive_processdata — causing 30 ms comms spikes.
    //
    // The watchdog independently monitors slave states via GetSlaveInfo()
    // (reads context.slavelist[].state without SOEM port access) and will
    // trigger a full recovery (with StopCyclicOperation first) if needed.
    if (!pdoPipelinePrimed_.load(std::memory_order_acquire)) {
        return;
    }

    // Quick check using cached group state (no SOEM port access).
    // This state is maintained by the last successful ecx_readstate call
    // (from OP verification or watchdog recovery).
    if (context.slavelist[0].state != EC_STATE_OPERATIONAL) {
        char buf[96];
        snprintf(buf, sizeof(buf),
            "[StateWarn] group state=0x%x (not OP) — watchdog will recover if needed",
            context.slavelist[0].state);
        SetError(buf);
    }
}


void EtherCATMaster::ReadPdoOffsetsFromDriver(int slave) {
    // Read actual PDO mapping from driver via SDO to determine real byte offsets.
    // Populates pdoRuntimeOffsets_[slave] with {CiA402_index → {byteOffset, byteSize}}.
    if (slave <= 0 || slave > slaveCount) return;
    if ((int)pdoRuntimeOffsets_.size() <= slave) pdoRuntimeOffsets_.resize(slave + 1);
    auto& offsets = pdoRuntimeOffsets_[slave];
    offsets.clear();

    auto readPdoMapping = [&](uint16_t assignIndex, bool isOutput) {
        // Read PDO assignment count
        uint8_t count = 0; int sz = (int)sizeof(count);
        if (!ReadSDO((uint16_t)slave, assignIndex, 0x00, (char*)&count, sz) || count == 0) return;
        for (uint8_t p = 1; p <= count; ++p) {
            uint32_t pdoIdx = 0; int isz = (int)sizeof(pdoIdx);
            if (!ReadSDO((uint16_t)slave, assignIndex, p, (char*)&pdoIdx, isz)) continue;
            uint16_t pdoIndex = (uint16_t)(pdoIdx & 0xFFFF);

            // Read mapping entry count
            uint8_t mapCount = 0; int msz = (int)sizeof(mapCount);
            if (!ReadSDO((uint16_t)slave, pdoIndex, 0x00, (char*)&mapCount, msz)) {
                uint16_t mapCount16 = 0; msz = (int)sizeof(mapCount16);
                if (!ReadSDO((uint16_t)slave, pdoIndex, 0x00, (char*)&mapCount16, msz)) continue;
                mapCount = (uint8_t)mapCount16;
            }

            // Walk entries in driver order, accumulating byte offset
            int runningOffset = 0;
            for (uint8_t me = 1; me <= mapCount; ++me) {
                uint32_t mapping = 0; int mmsz = (int)sizeof(mapping);
                if (!ReadSDO((uint16_t)slave, pdoIndex, me, (char*)&mapping, mmsz)) continue;
                uint16_t objIdx = (uint16_t)((mapping >> 16) & 0xFFFF);
                uint8_t  objSub = (uint8_t)((mapping >> 8) & 0xFF);
                uint8_t  bitlen = (uint8_t)(mapping & 0xFF);
                int byteLen = (bitlen > 0) ? ((bitlen + 7) / 8) : 4;
                if (byteLen > 4) byteLen = 4;

                PdoEntryOffset eo;
                eo.byteOffset = runningOffset;
                eo.byteSize = byteLen;
                offsets[objIdx] = eo;
                runningOffset += byteLen;
            }
        }
    };

    readPdoMapping(0x1C12, true);   // RxPDO (outputs)
    readPdoMapping(0x1C13, false);  // TxPDO (inputs)
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

        // Read actual PDO byte offsets from driver via SDO
        ReadPdoOffsetsFromDriver(i);

        const int ioMapSize = (int)sizeof(IOmap);
        const bool offsetValid = (outputOffset >= 0) && (outputOffset < ioMapSize);
        const bool rangeValid = offsetValid && (outputBytes > 0) && (outputOffset + outputBytes <= ioMapSize);
        if (rangeValid) {
            printf("  從站 %d (%s): 輸出偏移=%d, 字節數=%d\n", 
                   i, slave->name, outputOffset, outputBytes);
            
            // Helper: get runtime offset for a CiA402 index, fall back to compile-time constant
            auto off = [&](uint16_t ciaIndex, int fallbackConst) -> int {
                if (i < (int)pdoRuntimeOffsets_.size()) {
                    auto it = pdoRuntimeOffsets_[i].find(ciaIndex);
                    if (it != pdoRuntimeOffsets_[i].end() && it->second.byteOffset >= 0)
                        return it->second.byteOffset;
                }
                return fallbackConst;
            };
            
            // Write initial safe values at driver-reported offsets
            const int tposOff = off(0x607A, kPdoOutOff_TargetPos);
            const int tvelOff = off(0x60FF, kPdoOutOff_TargetVel);
            const int ttorOff = off(0x6071, kPdoOutOff_TargetTorque);
            const int ctrlOff = off(0x6040, kPdoOutOff_Controlword);
            
            if (outputBytes >= tposOff + 4) {
                int32_t targetPosition = 0;
                safe_memcpy(IOmap + outputOffset + tposOff, &targetPosition, sizeof(targetPosition));
                printf("    設置 Target Position = %d (offset %d)\n", targetPosition, tposOff);
            }
            
            if (outputBytes >= tvelOff + 4) {
                int32_t targetVelocity = 0;
                safe_memcpy(IOmap + outputOffset + tvelOff, &targetVelocity, sizeof(targetVelocity));
                printf("    設置 Target Velocity = %d (offset %d)\n", targetVelocity, tvelOff);
            }
            
            if (outputBytes >= ttorOff + 2) {
                memset(IOmap + outputOffset + ttorOff, 0, 2);
            }
            
            if (outputBytes >= ctrlOff + 2) {
                uint16_t controlword = 0x0006;  // Shutdown
                safe_memcpy(IOmap + outputOffset + ctrlOff, &controlword, sizeof(controlword));
                printf("    設置 Controlword = 0x%04X (offset %d)\n", controlword, ctrlOff);
            }
            
            // Zero any remaining output bytes not covered by known objects
            const int lastKnownEnd = (std::max)({tposOff + 4, tvelOff + 4, ttorOff + 2, ctrlOff + 2});
            if (outputBytes > lastKnownEnd) {
                memset(IOmap + outputOffset + lastKnownEnd, 0, (size_t)(outputBytes - lastKnownEnd));
                printf("    清除剩餘 %d 位元組 (offset %d+)\n", outputBytes - lastKnownEnd, lastKnownEnd);
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


void EtherCATMaster::UpdatePdoInputCaches()
{
    // PDO cache for 0x60FD/0x603F/0x6061.
    // Uses runtime offsets when available (read from actual driver via SDO),
    // falls back to compile-time constants matching Par/Startup.csv.
    if (!isInitialized || pdoCacheSlaveCount_.load(std::memory_order_acquire) <= 0 || context.slavecount < 1) return;

    // 0x60FD: slave 1 only (used by AutoWindow external button sync)
    const uint8_t* s1in = reinterpret_cast<const uint8_t*>(context.slavelist[1].inputs);
    const int diOff = (1 < (int)pdoRuntimeOffsets_.size())
        ? ([](auto& m, uint16_t k)->int{ auto it=m.find(k); return it!=m.end()?it->second.byteOffset:kPdoInOff_DigitalInputs; })(pdoRuntimeOffsets_[1], 0x60FD)
        : kPdoInOff_DigitalInputs;
    if (s1in && context.slavelist[1].Ibytes >= diOff + 4) {
        const uint8_t* p = s1in + diOff;
        uint32_t fd = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        pdo60FDCache_.store(fd, std::memory_order_relaxed);
        pdo60FDValid_.store(true, std::memory_order_release);
    }

    // 0x603F + 0x6061: all slaves, one pass
    int cacheSlaveCount = pdoCacheSlaveCount_.load(std::memory_order_acquire);
    for (int si = 1; si <= cacheSlaveCount; ++si) {
        const uint8_t* in = reinterpret_cast<const uint8_t*>(context.slavelist[si].inputs);
        if (!in || context.slavelist[si].Ibytes < kPdoTxByteSize) continue;

        // Use runtime offsets if available, else constants
        const int modeOff = (si < (int)pdoRuntimeOffsets_.size())
            ? ([](auto& m, uint16_t k)->int{ auto it=m.find(k); return it!=m.end()?it->second.byteOffset:kPdoInOff_ModeDisplay; })(pdoRuntimeOffsets_[si], 0x6061)
            : kPdoInOff_ModeDisplay;
        const int errOff = (si < (int)pdoRuntimeOffsets_.size())
            ? ([](auto& m, uint16_t k)->int{ auto it=m.find(k); return it!=m.end()?it->second.byteOffset:kPdoInOff_ErrorCode; })(pdoRuntimeOffsets_[si], 0x603F)
            : kPdoInOff_ErrorCode;

        pdo6061Cache_[si].store(in[modeOff], std::memory_order_relaxed);
        uint16_t ec = (uint16_t)in[errOff] | ((uint16_t)in[errOff + 1] << 8);
        pdo603FCache_[si].store(ec, std::memory_order_relaxed);
    }
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
    char errBuf[96];
    snprintf(errBuf, sizeof(errBuf), "WriteOutputBytes: error maxWrite<=0 absOffset=%d IOmap_size=%zu", absOffset, sizeof(IOmap));
    SetError(errBuf);
        return false; 
    }
    int toWrite = (len < maxWrite) ? len : maxWrite;
    if (!safe_memcpy(IOmap + absOffset, data, (size_t)toWrite)) {
        LeaveCriticalSection(&cs);
        char errBuf[96];
        snprintf(errBuf, sizeof(errBuf), "WriteOutputBytes: safe_memcpy failed absOffset=%d toWrite=%d", absOffset, toWrite);
        SetError(errBuf);
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
        char errBuf[112];
        snprintf(errBuf, sizeof(errBuf), "StageOutputBytes: refusing oversized staged write (toWrite=%d) absOffset=%d", toWrite, absOffset);
        LeaveCriticalSection(&pdoDataCS);
        SetError(errBuf);
        return false;
    }
    
    // CRITICAL FIX: Write DIRECTLY to IOmap as well as staging.
    // This ensures that PowerOn values are immediately in IOmap when SendProcessDataOnce
    // is called, rather than waiting for CyclicTask to apply them (which may race).
    // Defensive copy: ensure we never call memcpy/memmove with a null source.
    if (!safe_memcpy(IOmap + absOffset, data, (size_t)toWrite)) {
        LeaveCriticalSection(&pdoDataCS);
        char errBuf[96];
        snprintf(errBuf, sizeof(errBuf), "StageOutputBytes: safe_memcpy failed absOffset=%d toWrite=%d", absOffset, toWrite);
        SetError(errBuf);
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
        char errBuf[128];
        snprintf(errBuf, sizeof(errBuf), "StageOutputIfNotExists: refusing oversized staged write (toWrite=%d) absOffset=%d", toWrite, absOffset);
        LeaveCriticalSection(&pdoDataCS);
        SetError(errBuf);
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
    // active cyclic command PDO (0x60FF for CSV, 0x607A for CSP), remove them
    // from stagedOutputs, and zero their current IOmap bytes so the last
    // command is not re-used.
    // IMPORTANT: In CSP mode, 0x607A IS the active position command; zeroing it
    // would command the drive to move to position 0 (causing a burst).
    // In CSP mode we only clear the staged output but preserve the IOmap value.
    const uint16_t activeCommandPdo = CoreHelpers::GetCyclicCommandPdoIndex(GetCyclicOperationMode());
    const bool cspMode = (activeCommandPdo == 0x607A);
    std::vector<OutputRange> rangesToClear;
    // Separate list: offsets that should only be removed from stagedOutputs
    // but NOT zeroed in IOmap (CSP position command).
    std::vector<int> preserveIomapOffsets;

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
    // CSP mode: collect per-slave offsets for syncing 0x607A to 0x6064.
    // We must capture these inside cs (context.slavelist access) but perform
    // the IOmap copy outside cs (to keep the critical section short).
    struct CspSyncEntry { int tgtOff; int actOff; };
    std::vector<CspSyncEntry> cspSyncEntries;
    if (cspMode) cspSyncEntries.resize(slaveCount + 1);

    for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
        // Get output base for this slave
        int outBase = -1;
        if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIdx].outputs),
                               reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outBase)) {
            outBase = context.slavelist[slaveIdx].Ostartbit / 8;
        }
        int inBase = -1;
        if (cspMode) {
            if (!TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIdx].inputs),
                                   reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), inBase)) {
                inBase = context.slavelist[slaveIdx].Istartbit / 8;
            }
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

        // CSP mode: look up 0x6064 (input) and 0x607A (output) offsets
        if (cspMode && inBase >= 0) {
            auto& entry = cspSyncEntries[slaveIdx];
            entry.tgtOff = -1;
            entry.actOff = -1;
            auto descIt = pdoDisplayDescriptors.find(slaveIdx);
            if (descIt != pdoDisplayDescriptors.end()) {
                for (const auto& desc : descIt->second) {
                    if (desc.subindex != 0) continue; // must match staged-clear logic
                    if (desc.index == 0x607A && desc.isOutput) entry.tgtOff = outBase + desc.byteOffset;
                    if (desc.index == 0x6064 && !desc.isOutput) entry.actOff = inBase + desc.byteOffset;
                }
            }
            // Fallback to standard startup.csv offsets
            if (entry.tgtOff < 0) entry.tgtOff = outBase + kPdoOutOff_TargetPos;
            if (entry.actOff < 0) entry.actOff = inBase + kPdoInOff_ActualPos;
        }
        
        // Find staged motion-command descriptors for this slave
        auto descIt = pdoDisplayDescriptors.find(slaveIdx);
        if (descIt != pdoDisplayDescriptors.end()) {
            for (const auto& desc : descIt->second) {
                if (desc.subindex != 0) {
                    continue;
                }
                // Match the active cyclic command PDO (0x607A for CSP, 0x60FF
                // for CSV).  Also always match 0x60FF so that CSP-mode
                // stagedOutputs and IOmap are cleared of any stale velocity
                // command — even though 0x60FF is not the active command in
                // CSP mode, a persisted non-zero staged value from a previous
                // CSV session or an edge-case code path would otherwise leak
                // through and cause a single-cycle velocity glitch.
                if (desc.index == 0x607A || desc.index == 0x60FF || desc.index == activeCommandPdo) {
                    int bytes = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : (int)sizeof(int32_t);
                    if (bytes <= 0) {
                        bytes = (int)sizeof(int32_t);
                    }
                    const int offset = outBase + desc.byteOffset;
                    // In CSP mode, 0x607A IS the active position command;
                    // preserve its IOmap value to avoid commanding the drive
                    // to move to position 0.
                    if (cspMode && desc.index == 0x607A) {
                        preserveIomapOffsets.push_back(offset);
                    }
                    addRangeToClear(offset, bytes);
                }
            }
        } else {
            // Fallback to the standard RxPDO layout when descriptor metadata is unavailable.
            // Startup.csv order: 0x607A at kPdoOutOff_TargetPos, 0x60FF at kPdoOutOff_TargetVel.
            if (cspMode) {
                preserveIomapOffsets.push_back(outBase + kPdoOutOff_TargetPos);
            }
            addRangeToClear(outBase + kPdoOutOff_TargetPos, (int)sizeof(int32_t));  // 0x607A
            addRangeToClear(outBase + kPdoOutOff_TargetVel, (int)sizeof(int32_t));  // 0x60FF
        }
    }
    LeaveCriticalSection(&cs);
    
    // Remove the staged motion-command outputs and clear the currently latched IOmap bytes.
    // For CSP mode, instead of just preserving the stale 0x607A IOmap value, copy the
    // actual position (0x6064) to 0x607A so the position command always reflects the
    // true servo position.  This prevents a jump when soft-start begins after the servo
    // has moved (e.g. during a fault-reset / power-on sequence) while 0x607A was static.
    for (const auto& range : rangesToClear) {
        auto it = stagedOutputs.find(range.offset);
        if (it != stagedOutputs.end()) {
            stagedOutputs.erase(it);
        }
        // Skip IOmap zeroing for offsets that must be preserved (CSP position command)
        bool preserve = false;
        for (int preservedOffset : preserveIomapOffsets) {
            if (range.offset == preservedOffset) {
                preserve = true;
                break;
            }
        }
        if (!preserve) {
            memset(IOmap + range.offset, 0, (size_t)range.bytes);
        }
    }

    // CSP mode: after clearing staged outputs, sync every slave's 0x607A
    // (Target Position) to its 0x6064 (Position Actual Value) so the
    // position command always reflects the true servo position.
    // Offsets were collected inside cs above to avoid accessing
    // context.slavelist outside the critical section.
    // NOTE: The per-cycle CSP sync (that tracked 0x607A←0x6064 while
    // filtering was off) is DISABLED; this one-time sync is the sole
    // correction before soft-start enables the filter.
    if (cspMode) {
        for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
            const auto& entry = cspSyncEntries[slaveIdx];
            if (entry.tgtOff >= 0 && entry.actOff >= 0 &&
                entry.tgtOff + 4 <= (int)sizeof(IOmap) &&
                entry.actOff + 4 <= (int)sizeof(IOmap)) {
                int32_t actRaw = 0;
                std::memcpy(&actRaw, IOmap + entry.actOff, sizeof(actRaw));
                std::memcpy(IOmap + entry.tgtOff, &actRaw, sizeof(actRaw));
            } else {
                // Diagnostic: CSP sync skipped due to invalid offsets
                std::ostringstream diag;
                diag << "[CSP-SYNC] S" << slaveIdx << " SKIP tgtOff=" << entry.tgtOff << " actOff=" << entry.actOff;
                SetError(diag.str());
            }
        }
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
        // Immediately compute display values from current IOmap so the UI
        // timer does not encounter an empty cache on the next tick.
    }
    LeaveCriticalSection(&pdoDataCS);
    if (!descriptors.empty()) {
        RefreshPDODisplayValues(slaveIndex);
    }
}

void EtherCATMaster::RefreshPDODisplayValues(int slaveIndex) {
    if (slaveIndex <= 0 || slaveIndex > slaveCount) return;
    if (!isInitialized) return;

    // Snapshot current IOmap for this slave
    std::vector<uint8_t> snapshot;
    EnterCriticalSection(&pdoDataCS);
    auto descIt = pdoDisplayDescriptors.find(slaveIndex);
    if (descIt == pdoDisplayDescriptors.end() || descIt->second.empty()) {
        // No descriptors → erase stale values
        pdoDisplayValues.erase(slaveIndex);
        LeaveCriticalSection(&pdoDataCS);
        return;
    }

    const auto& descriptors = descIt->second;
    int outBase = -1, inBase = -1;
    TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].outputs),
                      reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), outBase);
    if (outBase < 0) outBase = context.slavelist[slaveIndex].Ostartbit / 8;
    TryGetIOmapOffset(reinterpret_cast<const uint8_t*>(context.slavelist[slaveIndex].inputs),
                      reinterpret_cast<const uint8_t*>(IOmap), sizeof(IOmap), inBase);
    if (inBase < 0) inBase = context.slavelist[slaveIndex].Istartbit / 8;
    int outBytes = context.slavelist[slaveIndex].Obytes;
    int inBytes  = context.slavelist[slaveIndex].Ibytes;

    int totalSize = outBytes + inBytes;
    if (totalSize > 0 && totalSize <= 1024) {
        snapshot.resize(totalSize);
        if (outBase >= 0 && (size_t)outBase + (size_t)outBytes <= sizeof(IOmap))
            memcpy(snapshot.data(), IOmap + outBase, outBytes);
        if (inBase >= 0 && (size_t)inBase + (size_t)inBytes <= sizeof(IOmap))
            memcpy(snapshot.data() + outBytes, IOmap + inBase, inBytes);
    }

    std::vector<PDOChannelValue> computed;
    computed.reserve(descriptors.size());
    for (const auto& desc : descriptors) {
        PDOChannelValue val;
        val.descriptor = desc;
        val.valid = false;
        int bytes = (desc.bitLength > 0) ? ((desc.bitLength + 7) / 8) : 4;
        if (bytes > 4) bytes = 4;
        if (bytes > 0 && !snapshot.empty()) {
            int idxInSnapshot = desc.isOutput ? desc.byteOffset : (outBytes + desc.byteOffset);
            if (idxInSnapshot >= 0 && idxInSnapshot + bytes <= (int)snapshot.size()) {
                uint32_t raw = 0;
                for (int b = 0; b < bytes; ++b)
                    raw |= (uint32_t)snapshot[idxInSnapshot + b] << (8 * b);
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
    pdoDisplayValues[slaveIndex] = std::move(computed);
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
    metrics.actualTorque = L"N/A";

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

    // 0x6077 Actual Torque: store raw value (unit: 0.1% rated torque) without
    // position-scale conversion — P1 scaling only applies to position/velocity.
    {
        PDOChannelDescriptor torqueDesc;
        if (TryFindDescriptorByIndex(descriptors, 0x6077, 0, torqueDesc)) {
            int32_t rawTorque = 0;
            if (ReadDescriptorValue(context, slaveIndex, snapshot, torqueDesc, rawTorque)) {
                const double sign = scaling.invertSign ? -1.0 : 1.0;
                metrics.actualTorque = FormatMechanicalValueString(static_cast<double>(rawTorque) * sign);
            }
        }
    }

    metrics.valid = (metrics.targetPosition != L"N/A") ||
                    (metrics.actualPosition != L"N/A") ||
                    (metrics.targetSpeed != L"N/A") ||
                    (metrics.actualSpeed != L"N/A") ||
                    (metrics.actualTorque != L"N/A");

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
        UpdatePdoInputCaches();
        return;
    }

    // Memory-only flush — no SOEM port access.
    // Applies staged outputs to IOmap.  CyclicTask sends them on the next
    // PDO cycle (≤2 ms).  Callers must wait one cycle before reading back
    // updated values (e.g. statusword after controlword write).
    if (!stagedOutputs.empty()) {
        EnterCriticalSection(&pdoDataCS);
        ApplyStagedOutputsToIOMap(IOmap, sizeof(IOmap), stagedOutputs, "SendProcessDataOnce");
        LeaveCriticalSection(&pdoDataCS);
    }
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
            DWORD sleepMs = (DWORD)(sleep_us / 1000);
            if (sleepMs > 1) {
                Sleep(sleepMs);
            } else {
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
    pdoCycleEvent = CreateEvent(NULL, TRUE, FALSE, NULL);  // manual-reset
    if (pdoCycleEvent == NULL) {
        SetError("Failed to create PDO cycle event");
        CloseHandle(pdoStopEvent);
        pdoStopEvent = NULL;
        return;
    }

    pdoProcessingThread = CreateThread(NULL, 0, PDOProcessingThreadFunc, this, 0, NULL);
    if (pdoProcessingThread == NULL) {
        SetError("Failed to create PDO processing thread");
        CloseHandle(pdoStopEvent);
        pdoStopEvent = NULL;
        CloseHandle(pdoCycleEvent);
        pdoCycleEvent = NULL;
        return;
    }

    // Set TIME_CRITICAL priority for PDO processing thread.
    // CyclicTask also runs at TIME_CRITICAL (on the dedicated RT core).
    // Both need equal priority so that Critical Section handoff is fair —
    // otherwise the higher-priority CyclicTask always wins pdoDataCS and
    // PDOProcessingTask starves (MMF input sluggish, soft-start slow).
    // PDOProcessingTask is pinned to a non-RT core via round-robin, so it
    // does not compete with CyclicTask for the same CPU.
        if (!SetThreadPriority(pdoProcessingThread, THREAD_PRIORITY_TIME_CRITICAL)) {
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
        if (pdoCycleEvent != NULL) {
            CloseHandle(pdoCycleEvent);
            pdoCycleEvent = NULL;
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

    // Low-Latency MMF Path: pin this thread TO the RT core so the entire
    // MMF→IK→filter→staging pipeline runs on the same physical core as
    // the CyclicTask. This eliminates cross-core cache misses and
    // scheduler jitter for the latency-critical computation path.
    //
    // Priority set to HIGHEST (not TIME_CRITICAL): ensures CyclicTask
    // (TIME_CRITICAL) always preempts PDOProcessingTask, so IK/filter
    // computation NEVER delays the PDO exchange cycle.
    if (master->IsLowLatencyMmfPath()) {
        CoreHelpers::CpuIsolation::PinCurrentThreadToRtCores();
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        SetThreadPriorityBoost(GetCurrentThread(), TRUE);  // disable dynamic boost
    } else {
        // Default: exclude from RT core to keep L1/L2 clean for CyclicTask
        CoreHelpers::CpuIsolation::ExcludeCurrentThreadFromRtCores();
    }

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
        // ── Pre-OP guard: defer all processing until the CyclicTask has
        // established the PDO pipeline AND all slaves are confirmed OP.
        // During DC pre-stabilisation and OP transition, any pdoDataCS
        // contention or SOEM port access from this thread would interfere
        // with the critical initialisation sequence (AL=0x001A risk).
        if (!pdoPipelinePrimed_.load(std::memory_order_acquire) ||
            context.slavelist[0].state != EC_STATE_OPERATIONAL) {
            Sleep(10);
            continue;
        }

        // ── Event-driven cycle sync (no spin, no sleep, no timer) ──
        // CyclicTask calls SetEvent(pdoCycleEvent) after each PDO exchange.
        // We wait, process, and immediately wait again.
        {
            WaitForSingleObject(pdoCycleEvent, INFINITE);
            if (WaitForSingleObject(pdoStopEvent, 0) == WAIT_OBJECT_0) {
                goto pdo_processing_done;
            }
            ResetEvent(pdoCycleEvent);
        }

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

        // CSP mode: when PDO filtering is disabled, continuously sync
        // 0x607A (Target Position) <- 0x6064 (Position Actual Value) so
        // IOmap always reflects the true servo position.
        //
        // DISABLED — the per-cycle sync uses metas[] offsets which may be
        // incorrect for some slaves after a reconnect.  Writing to the wrong
        // IOmap offset actively corrupts other PDOs (torque limit,
        // controlword, etc.), causing large, visible servo transients.
        // ClearStagedOutputsForCyclicMotionCommands already performs a
        // correct one-time sync right before soft-start, making the
        // per-cycle sync unnecessary.
#if 0
        if (!filteringEnabledThisCycle &&
            !simulationMode_.load(std::memory_order_relaxed) &&
            GetCyclicOperationMode() == CoreHelpers::kCia402OperationModeCyclicSyncPosition) {
            for (int slaveIdx = 1; slaveIdx <= slaveCount; ++slaveIdx) {
                const IOMeta &m = metas[slaveIdx];
                if (m.outBase < 0 || m.inBase < 0) continue;
                int tgtOff = m.outBase + kPdoOutOff_TargetPos;
                int actOff = m.inBase  + kPdoInOff_ActualPos;
                auto descIt = pdoDisplayDescriptors.find(slaveIdx);
                if (descIt != pdoDisplayDescriptors.end()) {
                    for (const auto& desc : descIt->second) {
                        if (desc.subindex != 0) continue;
                        if (desc.index == 0x607A && desc.isOutput) tgtOff = m.outBase + desc.byteOffset;
                        if (desc.index == 0x6064 && !desc.isOutput) actOff = m.inBase + desc.byteOffset;
                    }
                }
                if (tgtOff < 0 || actOff < 0) continue;
                if ((size_t)tgtOff + 4 > sizeof(IOmap) || (size_t)actOff + 4 > sizeof(IOmap)) continue;
                int32_t actRaw = 0;
                std::memcpy(&actRaw, IOmap + actOff, sizeof(actRaw));
                std::memcpy(IOmap + tgtOff, &actRaw, sizeof(actRaw));
            }
        }
#endif // 0 — per-cycle CSP sync disabled (see comment above)

        LeaveCriticalSection(&cs);

        // Pre-allocate outside pdoDataCS lock
        std::vector<MechanicalWork> mechanicalJobs;
        mechanicalJobs.reserve(slaveCount);

        // Process legacy name-based pending outputs by staging them into stagedOutputs.
        // CyclicTask applies stagedOutputs to IOmap before every ecx_send_processdata.
        // Use TryEnterCriticalSection — if CyclicTask holds pdoDataCS, skip this
        // cycle instead of blocking.  The CyclicTask at TIME_CRITICAL TSC-spins
        // and re-acquires every 2 ms; blocking here would cause 70 ms cycle spikes.
        if (!TryEnterCriticalSection(&pdoDataCS)) {
            // Lock busy — CyclicTask is exchanging PDOs. Skip this cycle.
            continue;
        }
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
                        writeOffset = m.outBase + kPdoOutOff_Controlword;
                        writeSize = 2;
                        break;
                    case 0x607A:
                        writeOffset = m.outBase + kPdoOutOff_TargetPos;
                        writeSize = 4;
                        break;
                    case 0x60FF:
                        writeOffset = m.outBase + kPdoOutOff_TargetVel;
                        writeSize = 4;
                        break;
                    case 0x6071:
                        writeOffset = m.outBase + kPdoOutOff_TargetTorque;
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

        // Resolve the active cyclic command PDO index once per cycle:
        // CSP → 0x607A (position), CSV → 0x60FF (velocity).
        const uint16_t activeCmdPdo = CoreHelpers::GetCyclicCommandPdoIndex(GetCyclicOperationMode());

        if (recordEnabled || mmfEnabled) {
            // Get epoch time (milliseconds since 1970-01-01 00:00:00 UTC)
            auto now = std::chrono::system_clock::now();
            auto epoch = now.time_since_epoch();
            uint64_t epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(epoch).count();
            uint64_t uptimeMs = GetTickCount64();
            
            // Read MMF_IN values (6 floats for 6 slaves) — PollOnce is now
            // handled by the MMF receiver thread; here we just read the latest snapshot.
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
                // Read the active cyclic command PDO using the configured mode
                // (0x607A for CSP position, 0x60FF for CSV velocity).
                int32_t targetVelRaw = ReadPDOValue(activeCmdPdo, 0);      // Active cyclic command PDO
                int32_t actualPosRaw = ReadPDOValue(0x6064, 0);            // Position Actual Value
                int32_t actualVelRaw = ReadPDOValue(0x606C, 0);            // Velocity Actual Value
                
                // Convert to mechanical units (mm, mm/s)
                double targetPos = ToMechanical(targetPosRaw, 0x607A);
                double targetVel = ToMechanical(targetVelRaw, activeCmdPdo);
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
                double filterTarget = 0.0;
                int    softStartActive = 0;
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
                        softStartActive = it->second.IsSoftStartActive() ? 1 : 0;
                    }
                    LeaveCriticalSection(&pdoFilterCS_);
                }
                // Read the last filter target via the lock-free atomic
                if (slaveIdx >= 1 && slaveIdx <= 6) {
                    filterTarget = lastPdoFilterInputAngles_[slaveIdx - 1].load(std::memory_order_relaxed);
                }
                const double filterTrackingError = pdoFilterOutput - actualPos;
                
                // Write to circular buffer
                if (recordEnabled) {
                    WritePDOLogEntry(slaveIdx, epochMs, uptimeMs, 
                                     mmfIn,
                                     controlWord, statusWord, 
                                     targetPos, targetVel, 
                                     actualPos, actualVel,
                                     pdoFilterOutput,
                                     pdoFilterVel,
                                     pdoFilterAcc,
                                     filterTarget,
                                     softStartActive,
                                     filterTrackingError);
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
        // Forward Kinematics: PDO filter output → 6-DOF platform pose
        // Uses the PDO filter's smoothed output angle instead of raw PDO
        // target/actual.  Filter output sits between commanded and measured —
        // smoother than raw target (0x607A) and ahead of actual feedback
        // (0x6064).  This reduces jitter in the pose stream and perceived
        // latency at the motion-rig consumer without adding extra filtering.
        // ========================================================================
        {
            bool poseMmfActive = poseMmfEnabled_.load(std::memory_order_relaxed);
            if (slaveCount == 6 && poseMmfActive) {
                auto now = std::chrono::system_clock::now();
                uint64_t epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();

                std::array<long double, 6> filterAngles{};
                bool allReady = true;
                {
                    EnterCriticalSection(&pdoFilterCS_);
                    for (int i = 1; i <= 6; ++i) {
                        auto it = pdoFilters_.find(i);
                        if (it != pdoFilters_.end() && it->second.IsInitialized()) {
                            filterAngles[i - 1] = static_cast<long double>(it->second.GetFilteredOutput());
                        } else {
                            allReady = false;
                            break;
                        }
                    }
                    LeaveCriticalSection(&pdoFilterCS_);
                }

                if (allReady) {
                    auto snap = ReadStewartParamsSnapshot();
                    WuSOEM::Pose6D initialPose;
                    initialPose.z = snap.initialHeight;
                    auto fwdResult = WuSOEM::StewartIK::CalculateForward(
                        snap.params, filterAngles, initialPose);
                    forwardKinPose_ = fwdResult.pose;
                    forwardKinInitialHeight_ = snap.initialHeight;
                    forwardKinValid_.store(fwdResult.valid, std::memory_order_release);
                    WritePoseMMF(fwdResult.pose, fwdResult.valid, snap.initialHeight, epochMs);
                } else {
                    forwardKinValid_.store(false, std::memory_order_release);
                    WuSOEM::Pose6D zeroPose{};
                    WritePoseMMF(zeroPose, false, 0.0L, epochMs);
                }
            } else if (slaveCount == 6) {
                forwardKinValid_.store(false, std::memory_order_release);
            }
        }

        // ========================================================================
        // PDO Filtering: Apply acceleration/deceleration/jerk limits to MMF input
        // ========================================================================
        // PollOnce is now handled by the dedicated MMF receiver thread (non-RT
        // core). The RT thread reads the latest snapshot via GetLatestWithUpdateCounts
        // without touching the MMF mapping, eliminating MMF arrival jitter from
        // the EtherCAT control loop.

        if (filteringEnabledThisCycle) {
            // Read latest MMF1_IN values (6 floats for 6 slaves)
            std::array<float, 6> mmfInValues = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            std::array<uint64_t, 6> mmfUpdateCounts{};
            if (mmf1ReaderRunning.load()) {
                mmf1Reader.GetLatestWithUpdateCounts(mmfInValues, mmfUpdateCounts);
            }

            // ---- MMF 前置濾波: EMA + spike clamp ----
            // 在 IK 之前套用，避免 Pose6D 座標雜訊被 IK 非線性放大。
            {
                constexpr float kSpikeMaxDelta = 5.0f;   // 單次最大變化 (Pose6D 單位: mm or deg)
                constexpr float kEmaAlpha = 0.4f;         // EMA 平滑係數
                for (int i = 0; i < 6; ++i) {
                    if (!mmfPreFilterInit_[i]) {
                        mmfPreFilterPrevRaw_[i] = mmfInValues[i];
                        mmfPreFilterEma_[i] = mmfInValues[i];
                        mmfPreFilterInit_[i] = true;
                        continue;
                    }
                    // Spike clamp: 單通道變化超過閾值則箝位
                    float clamped = mmfInValues[i];
                    const float delta = clamped - mmfPreFilterPrevRaw_[i];
                    if (std::abs(delta) > kSpikeMaxDelta) {
                        clamped = mmfPreFilterPrevRaw_[i] + std::copysign(kSpikeMaxDelta, delta);
                    }
                    mmfPreFilterPrevRaw_[i] = mmfInValues[i]; // 儲存原始值供下週期比對
                    // EMA
                    mmfPreFilterEma_[i] = mmfPreFilterEma_[i] * (1.0f - kEmaAlpha) + clamped * kEmaAlpha;
                    mmfInValues[i] = mmfPreFilterEma_[i];
                }
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

                // ---- Pose6D 前置驗證層 (硬限制) ----
                const auto validationResult = WuSOEM::StewartIK::ValidatePose6D(
                    pose, snap.initialHeight, pose6dLimits_);
                if (!validationResult.valid) {
                    // 姿態超出限制：凍結運動，設定警告旗標
                    mmfInValues = lastValidServoAngles_;
                    if (!pose6dPrevCycleViolated_) {
                        // 邊緣觸發：只在首次違規時記錄訊息 / 請求警告音
                        {
                            std::lock_guard<std::mutex> lk(pose6dLimitWarningMsgMutex_);
                            pose6dLimitWarningMsg_ = validationResult.messageW;
                        }
                        pose6dLimitWarning_.store(true, std::memory_order_relaxed);
                        pose6dPrevCycleViolated_ = true;
                    }
                } else {
                    // 姿態在限制內：清除警告旗標
                    if (pose6dPrevCycleViolated_) {
                        pose6dLimitWarning_.store(false, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lk(pose6dLimitWarningMsgMutex_);
                            pose6dLimitWarningMsg_ = L"Pose6D 限制已解除，恢復正常操作";
                        }
                        pose6dPrevCycleViolated_ = false;
                    }
                }

                // ---- 動態行程縮放邊緣觸發警報 ----
                if (validationResult.dynamicScalingActive) {
                    if (!pose6dPrevDynamicScaling_) {
                        {
                            std::lock_guard<std::mutex> lk(pose6dDynamicScalingMsgMutex_);
                            pose6dDynamicScalingMsg_ = L"Z 達極限 50%，已啟動動態行程縮放 (X/Y/Roll/Pitch/Yaw)，加減速限制降為 0.5x";
                        }
                        pose6dDynamicScaling_.store(true, std::memory_order_relaxed);
                        pose6dPrevDynamicScaling_ = true;
                        // 將 6 軸加減速限制降為設定值的 0.5 倍
                        for (int i = 1; i <= 6; ++i) {
                            auto it = pdoFilters_.find(i);
                            if (it != pdoFilters_.end()) {
                                it->second.ApplyDynamicScalingAccelLimits(true);
                            }
                        }
                    }
                } else {
                    if (pose6dPrevDynamicScaling_) {
                        pose6dDynamicScaling_.store(false, std::memory_order_relaxed);
                        {
                            std::lock_guard<std::mutex> lk(pose6dDynamicScalingMsgMutex_);
                            pose6dDynamicScalingMsg_ = L"Z 已回到安全範圍，動態行程縮放已關閉，加減速限制已恢復";
                        }
                        pose6dPrevDynamicScaling_ = false;
                        // 恢復 6 軸正常加減速限制
                        for (int i = 1; i <= 6; ++i) {
                            auto it = pdoFilters_.find(i);
                            if (it != pdoFilters_.end()) {
                                it->second.ApplyDynamicScalingAccelLimits(false);
                            }
                        }
                    }
                }

                if (!validationResult.valid) {
                    // 已凍結，跳過 IK 計算
                    mmfUpdateCounts.fill(pose6dUpdateCount);
                } else {
                    // 使用動態縮放後的飽和姿態（無縮放時等同原始 pose）
                    auto ikResult = WuSOEM::StewartIK::CalculateInverse(
                        snap.params, validationResult.clampedPose);
                    if (ikResult.valid) {
                        for (int i = 0; i < 6; ++i) {
                            mmfInValues[i] = static_cast<float>(ikResult.servoAngles[i]);
                        }
                        lastValidServoAngles_ = mmfInValues;
                    } else {
                        // IK failed: use last valid servo angles
                        mmfInValues = lastValidServoAngles_;
                    }
                }

                // A Pose6D sample is transformed into all six servo-angle
                // setpoints. Treat any pose-channel update as a new sample for
                // every servo axis so the filter does not infer stale input
                // from unrelated source-channel indices.
                mmfUpdateCounts.fill(pose6dUpdateCount);
            }
            // ---- End Pose6D IK conversion ----

            // Collect deferred staged output writes so they are applied
            // under pdoDataCS (the same CS used by CyclicTask when reading
            // stagedOutputs).  Writing to stagedOutputs while holding only
            // pdoFilterCS_ creates a data race with CyclicTask's iteration
            // under pdoDataCS, which can corrupt the map's allocator and
            // cause a NULL-pointer crash in std::memmove during subsequent
            // heap operations.
            std::vector<std::pair<int, int32_t>> deferredStagedWrites;
            deferredStagedWrites.reserve(12); // 2 per slave × 6 axes

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

                // When the MMF reader is not yet running, do NOT
                // overwrite mmfSetValue with actualPosMechanical.
                // Doing so creates a per-axis positive-feedback loop:
                //   actual position → mmfSetValue → filter target →
                //   CSV command → servo motion → actual changes →
                //   mmfSetValue changes → repeat.
                // Through mechanical coupling on the Stewart platform
                // this loop synchronises across all six axes, producing
                // the intermittent full-platform oscillation observed
                // during soft-start.
                //
                // The filter already initialises its output to the
                // actual position (ioInitialized_=false → first
                // Filter() call sets oFilteredOutput_=iActValue), so
                // there is no "first-run jump" to protect against.
                // When the MMF reader starts, the soft-start velocity
                // limits handle the transition smoothly.
                lastPdoFilterInputAngles_[mmfAxisIndex].store(static_cast<double>(mmfSetValue), std::memory_order_relaxed);

                // Compute filter-input velocity using precise PDO-cycle QPC timing
                {
                    const double curAngle = static_cast<double>(mmfSetValue);
                    const LONGLONG curTick = start.QuadPart;
                    if (lastPdoFilterInputTicks_[mmfAxisIndex] != 0 && frequency.QuadPart > 0) {
                        const LONGLONG dtTicks = curTick - lastPdoFilterInputTicks_[mmfAxisIndex];
                        if (dtTicks > 0) {
                            const double dtSec = static_cast<double>(dtTicks) / static_cast<double>(frequency.QuadPart);
                            const double prevAngle = lastPdoFilterInputPrevAngles_[mmfAxisIndex];
                            const double vel = (curAngle - prevAngle) / dtSec;
                            lastPdoFilterInputVelocities_[mmfAxisIndex].store(vel, std::memory_order_relaxed);
                        }
                    }
                    lastPdoFilterInputPrevAngles_[mmfAxisIndex] = curAngle;
                    lastPdoFilterInputTicks_[mmfAxisIndex] = curTick;
                }
                
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
                    // On filter init cycle, immediately overwrite stale IOmap
                    // values for 0x607A/0x60FF before the CyclicTask sends them.
                    // ClearStagedOutputsForCyclicMotionCommands may leave stale
                    // values in IOmap; this direct write eliminates the one-cycle
                    // staging delay that causes a position jump (bump).
                    //
                    // PRE-WRITE SNAPSHOT: read IOmap BEFORE the direct write
                    // to determine what CyclicTask would have sent to the servo
                    // if it ran before PDOProcessingTask.
                    int32_t ioPreTgtEnc = 0, ioPreVelEnc = 0;
                    int tOff = 0, vOff = 0;
                    {
                        const IOMeta &mPre = metas[slaveIdx];
                        tOff = mPre.outBase + kPdoOutOff_TargetPos;
                        vOff = mPre.outBase + kPdoOutOff_TargetVel;
                        PDOChannelDescriptor dPre;
                        if (TryFindDescriptorByIndex(descriptors, 0x607A, 0, dPre)) tOff = mPre.outBase + dPre.byteOffset;
                        if (TryFindDescriptorByIndex(descriptors, 0x60FF, 0, dPre)) vOff = mPre.outBase + dPre.byteOffset;
                        if (tOff >= 0 && (size_t)tOff + 4 <= sizeof(IOmap))
                            std::memcpy(&ioPreTgtEnc, IOmap + tOff, sizeof(ioPreTgtEnc));
                        if (vOff >= 0 && (size_t)vOff + 4 <= sizeof(IOmap))
                            std::memcpy(&ioPreVelEnc, IOmap + vOff, sizeof(ioPreVelEnc));
                    }
                    if (filterNeedsInit && scaling.scale > 1e-12) {
                        double tRaw = filteredOutput * sign / scaling.scale;
                        int32_t tEnc = static_cast<int32_t>(std::round(tRaw));
                        int32_t zeroVel = 0;
                        EnterCriticalSection(&pdoDataCS);
                        if (tOff >= 0 && (size_t)tOff + 4 <= sizeof(IOmap))
                            std::memcpy(IOmap + tOff, &tEnc, sizeof(tEnc));
                        if (vOff >= 0 && (size_t)vOff + 4 <= sizeof(IOmap))
                            std::memcpy(IOmap + vOff, &zeroVel, sizeof(zeroVel));
                        LeaveCriticalSection(&pdoDataCS);
                    }
                    // During hold-position (e.g. after Immediate Stop), continuously sync
                    // 0x607A to actual position so the drive stays at current location.
                    // Without this, 0x607A freezes at the pre-stop value while the axis
                    // drifts - the accumulated position error causes a bump/vibration
                    // when soft-start snaps 0x607A to actualPos.
                    if (!filterNeedsInit && filterIt->second.IsHoldingPosition() && scaling.scale > 1e-12) {
                        int32_t actEnc = actualPosRaw;
                        int32_t zeroVel = 0;
                        EnterCriticalSection(&pdoDataCS);
                        if (tOff >= 0 && (size_t)tOff + 4 <= sizeof(IOmap))
                            std::memcpy(IOmap + tOff, &actEnc, sizeof(actEnc));
                        if (vOff >= 0 && (size_t)vOff + 4 <= sizeof(IOmap))
                            std::memcpy(IOmap + vOff, &zeroVel, sizeof(zeroVel));
                        LeaveCriticalSection(&pdoDataCS);
                    }
                } catch (...) {
                    // Filter error - reset CSV state and clear filter
                    // error flags to avoid propagating inconsistent state
                    // on the next cycle.
                    if (csvStateIndex >= 0 &&
                        csvStateIndex < static_cast<int>(csvVelocityCommandStates.size())) {
                        csvVelocityCommandStates[csvStateIndex].Reset();
                    }
                    try {
                        // ClearError removed: tracking-error fatal latch no longer exists.
                    } catch (...) {}
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
                    int targetPosOffset = m.outBase + kPdoOutOff_TargetPos;
                    int targetVelOffset = m.outBase + kPdoOutOff_TargetVel;
                    
                    // Prefer descriptor lookup for safety; fall back to startup.csv constants.
                    if (TryFindDescriptorByIndex(descriptors, 0x607A, 0, desc)) {
                        targetPosOffset = m.outBase + desc.byteOffset;
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
                    // filterNeedsInit is computed BEFORE FilterWithInputState() above.
                    // By the time we reach here, the filter has already been initialised
                    // (its first call seeds oFilteredOutput_ from iActValue, i.e. the
                    // actual position).  Including filterNeedsInit in skipWrite causes a
                    // one-cycle 0-glitch: ClearStagedOutputsForCyclicMotionCommands()
                    // zeroes IOmap, and the next cycle skips the write because
                    // filterNeedsInit is still true, so the servo receives 0x607A=0
                    // and 0x60FF=0 for one frame.
                    //
                    // Only skip writes when PowerOnAllAsync is in progress
                    // (powerOnInProgress_).  The filterNeedsInit path resets
                    // csvVelocityCommandStates above and is safe to write on the
                    // same cycle.
                    bool skipWrite = powerOnInProgress_.load(std::memory_order_acquire);
                    
                    if (!skipWrite) {
                        const bool cspMode = (GetCyclicOperationMode() == CoreHelpers::kCia402OperationModeCyclicSyncPosition);

                        if (cspMode) {
                            // === CSP mode: write filtered position to 0x607A ===
                            // The PDOFilter already produces a safe filtered position.
                            // Clamp to PDO hard limits for safety, then write directly.
                            const auto [pdoMinLimit, pdoMaxLimit] = GetCsvMotionLimits(filterIt->second);
                            double clampedOutput = filteredOutput;
                            if (std::isfinite(pdoMaxLimit)) {
                                clampedOutput = (std::min)(clampedOutput, pdoMaxLimit);
                            }
                            if (std::isfinite(pdoMinLimit)) {
                                clampedOutput = (std::max)(clampedOutput, pdoMinLimit);
                            }
                            double targetPosRaw = clampedOutput * sign / scaling.scale;
                            int32_t targetPosEncoded = static_cast<int32_t>(std::round(targetPosRaw));

                            // Defer staged-output writes so they happen under
                            // pdoDataCS (matching CyclicTask), not pdoFilterCS_.
                            if (targetPosOffset >= 0 && (size_t)targetPosOffset + 4 <= sizeof(IOmap)) {
                                deferredStagedWrites.emplace_back(targetPosOffset, targetPosEncoded);
                            }
                            // In CSP mode, also stage 0x60FF = 0 so the velocity PDO
                            // channel does not hold a stale CSV velocity command.
                            if (targetVelOffset >= 0 && (size_t)targetVelOffset + 4 <= sizeof(IOmap)) {
                                deferredStagedWrites.emplace_back(targetVelOffset, 0);
                            }
                            // Reset CSV state so it does not accumulate during CSP mode.
                            if (csvStateIndex >= 0 &&
                                csvStateIndex < static_cast<int>(csvVelocityCommandStates.size())) {
                                csvVelocityCommandStates[csvStateIndex].Reset();
                            }
                        } else {
                            // === CSV mode: compute velocity command ===
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
                        // When hard-limit limiting is active, the PDOFilter has already clamped
                        // its output velocity (typically 0 at the limit).  Override the CSV
                        // command so 0x60FF immediately reflects the filter's clamped velocity,
                        // preventing position-error correction from fighting the safety boundary.
                        if (filterIt->second.HasHardLimitViolation() && filterIt->second.IsLimitingEnabled()) {
                            csvVelocityCommand = filterIt->second.GetCurrentVelocity();
                        }
                        double targetVelRaw = csvVelocityCommand * sign / scaling.scale;
                        int32_t targetVelEncoded = static_cast<int32_t>(std::round(targetVelRaw));
                        {
                            const auto [pdoMinLimit, pdoMaxLimit] = GetCsvMotionLimits(filterIt->second);
                            const double hardEps = (std::max)(filterIt->second.GetPositionDeadband(), 1e-6);
                            if (std::isfinite(pdoMaxLimit) &&
                                (actualPosMechanical >= pdoMaxLimit - hardEps || filteredOutput >= pdoMaxLimit - hardEps)) {
                                if (csvVelocityCommand > 0.0) {
                                    targetVelEncoded = 0;
                                }
                            } else if (std::isfinite(pdoMinLimit) &&
                                       (actualPosMechanical <= pdoMinLimit + hardEps || filteredOutput <= pdoMinLimit + hardEps)) {
                                if (csvVelocityCommand < 0.0) {
                                    targetVelEncoded = 0;
                                }
                            }
                        }

                        // Defer staged-output writes so they happen under
                        // pdoDataCS (matching CyclicTask), not pdoFilterCS_.
                        // Both the logical position target (0x607A) and the CSV
                        // velocity command (0x60FF, already zeroed when
                        // clearHardOutwardCsvNow) are staged here; CyclicTask
                        // applies them on the next PDO send.  The one-cycle
                        // delay is safe because the hard-limit braking envelope
                        // already clamps outward velocity to zero in the same
                        // cycle via ComputeCsvVelocityCommand.
                        if (targetPosOffset >= 0 && (size_t)targetPosOffset + 4 <= sizeof(IOmap)) {
                            deferredStagedWrites.emplace_back(targetPosOffset, targetPosEncoded);
                        }
                        if (targetVelOffset >= 0 && (size_t)targetVelOffset + 4 <= sizeof(IOmap)) {
                            deferredStagedWrites.emplace_back(targetVelOffset, targetVelEncoded);
                        }
                        } // end CSV mode
                    } else if (csvStateIndex >= 0 &&
                               csvStateIndex < static_cast<int>(csvVelocityCommandStates.size())) {
                        csvVelocityCommandStates[csvStateIndex].Reset();
                    }
                }
            }
            
            // 更新全局違規狀態（在所有從站處理完畢後）
            UpdateGlobalViolationState();
            
            LeaveCriticalSection(&pdoFilterCS_);

            // Apply deferred staged output writes under pdoDataCS so they are
            // serialized with CyclicTask's stagedOutputs reads (which also use
            // pdoDataCS).  Writing to stagedOutputs under pdoFilterCS_ created
            // a data race that could corrupt the map's allocator.
            if (!deferredStagedWrites.empty()) {
                EnterCriticalSection(&pdoDataCS);
                ApplyDeferredStagedWrites(stagedOutputs, deferredStagedWrites);
                LeaveCriticalSection(&pdoDataCS);
            }
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
                metrics.actualTorque = L"N/A";

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
                applyScaling(activeCmdPdo, 0, metrics.targetSpeed);
                applyScaling(0x606C, 0, metrics.actualSpeed);

                // 0x6077 Actual Torque: store raw value (unit: 0.1% rated torque) without
                // position-scale conversion — P1 scaling only applies to position/velocity.
                {
                    PDOChannelDescriptor torqueDesc;
                    if (TryFindDescriptorByIndex(job.descriptors, 0x6077, 0, torqueDesc)) {
                        int32_t rawTorque = 0;
                        if (ReadDescriptorValue(context, job.slaveIndex, job.snapshot, torqueDesc, rawTorque)) {
                            const double sign = job.scaling.invertSign ? -1.0 : 1.0;
                            metrics.actualTorque = FormatMechanicalValueString(static_cast<double>(rawTorque) * sign);
                        }
                    }
                }

                metrics.valid = (metrics.targetPosition != L"N/A") ||
                                (metrics.actualPosition != L"N/A") ||
                                (metrics.targetSpeed != L"N/A") ||
                                (metrics.actualSpeed != L"N/A") ||
                                (metrics.actualTorque != L"N/A");

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
        pdoProcElapsedUs_.store(static_cast<uint32_t>(elapsed_us), std::memory_order_relaxed);

        // ── Cycle-skip detection: warn when processing overruns the period ──
        {
            static int pdoCycleSkipCount = 0;
            if (elapsed_us > static_cast<LONGLONG>(cycleMicros)) {
                ++pdoCycleSkipCount;
                if (pdoCycleSkipCount % 100 == 1) {
                    char msg[128];
                    snprintf(msg, sizeof(msg),
                        "[PDOProc] cycle overrun: elapsed=%lldus > period=%uus (skip #%d)",
                        elapsed_us, cycleMicros, pdoCycleSkipCount);
                    SetError(msg);
                }
            }
        }

        // Processing complete — loop back to WaitForSingleObject immediately.
        // No timing wait needed; CyclicTask's SetEvent provides the pace.
        
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
            completeSoftStartRequested_.store(false, std::memory_order_release);
        }

        // Diagnostic: count completed PDO processing cycles
        pdoProcCycleCount_.fetch_add(1, std::memory_order_relaxed);
    }
pdo_processing_done:
    ; // exit path when pdoStopEvent is signaled during cycle wait
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
    
    // Defensive: validate that outBytes/inBytes are individually sane before
    // using them for memcpy.  SOEM context corruption (from a NIC driver race
    // or interrupt storm) can produce wildly invalid I/O byte counts.
    if (outBytes < 0 || outBytes > 1024 || inBytes < 0 || inBytes > 1024) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }
    if (outBase < -4096 || outBase > (int)sizeof(IOmap) + 4096 ||
        inBase  < -4096 || inBase  > (int)sizeof(IOmap) + 4096) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }

    try {
        snapshot.resize(static_cast<size_t>(totalSize));
    } catch (...) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }

    // Double-check: resize could have thrown, but also verify the data pointer
    // is valid before memcpy — a corrupted allocator may return a null pointer.
    if (snapshot.size() != static_cast<size_t>(totalSize) || snapshot.data() == nullptr) {
        LeaveCriticalSection(&pdoDataCS);
        return false;
    }

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



