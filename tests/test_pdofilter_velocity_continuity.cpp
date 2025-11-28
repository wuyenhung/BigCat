#include "PDOFilter.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <limits>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

void LogProgress(const char* label, int step, int total) {
    std::cout << "[progress] " << label << ' ' << step << '/' << total << '\n';
}

bool CheckAccelEnvelope(double output,
                        double previousOutput,
                        double previousVelocity,
                        double maxVelocity,
                        double maxAcceleration,
                        double cycleTime,
                        double pdoMin,
                        double pdoMax,
                        const char* message) {
    double vLow = previousVelocity - maxAcceleration * cycleTime;
    double vHigh = previousVelocity + maxAcceleration * cycleTime;
    vLow = std::max(vLow, -std::abs(maxVelocity));
    vHigh = std::min(vHigh, std::abs(maxVelocity));

    const double pLow = std::max(previousOutput + vLow * cycleTime, pdoMin);
    const double pHigh = std::min(previousOutput + vHigh * cycleTime, pdoMax);
    const bool insideEnvelope = (pLow <= pHigh)
        ? (output >= pLow - 1e-6 && output <= pHigh + 1e-6)
        : (output >= pdoMin - 1e-6 && output <= pdoMax + 1e-6);

    if (!insideEnvelope) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6)
            << message
            << " prevOut=" << previousOutput
            << " prevVel=" << previousVelocity
            << " output=" << output
            << " allowed=[" << pLow << ", " << pHigh << "]"
            << " maxV=" << maxVelocity
            << " maxA=" << maxAcceleration
            << " dt=" << cycleTime;
        return Check(false, oss.str().c_str());
    }

    return true;
}

}  // namespace

int main() {
    {
     PDOFilter configFilter;
     if (!Check(std::abs(configFilter.GetVelocityViolationThresholdScale() - 0.60) < 1e-12,
             "default velocity violation threshold scale should be 0.60") ||
         !Check(std::abs(configFilter.GetAccelViolationThresholdScale() - 1.20) < 1e-12,
             "default accel/decel violation threshold scale should be 1.20") ||
         !Check(std::abs(configFilter.GetVelocityViolationDelaySec() - 0.50) < 1e-12,
             "default velocity violation delay should be 0.50s") ||
         !Check(std::abs(configFilter.GetAccelViolationDelaySec() - 0.20) < 1e-12,
             "default accel/decel violation delay should be 0.20s")) {
         return EXIT_FAILURE;
     }

     configFilter.SetDynamicViolationConfig(0.45, 1.05, 0.25, 0.10);
     if (!Check(std::abs(configFilter.GetVelocityViolationThresholdScale() - 0.45) < 1e-12,
             "velocity violation threshold scale should be configurable") ||
         !Check(std::abs(configFilter.GetAccelViolationThresholdScale() - 1.05) < 1e-12,
             "accel/decel violation threshold scale should be configurable") ||
         !Check(std::abs(configFilter.GetVelocityViolationDelaySec() - 0.25) < 1e-12,
             "velocity violation delay should be configurable") ||
         !Check(std::abs(configFilter.GetAccelViolationDelaySec() - 0.10) < 1e-12,
             "accel/decel violation delay should be configurable")) {
         return EXIT_FAILURE;
     }

     configFilter.SetDynamicViolationConfig(-1.0,
                             std::numeric_limits<double>::quiet_NaN(),
                             -0.1,
                             std::numeric_limits<double>::infinity());
     if (!Check(std::abs(configFilter.GetVelocityViolationThresholdScale() - 0.60) < 1e-12,
             "invalid velocity violation threshold scale should fall back to default") ||
         !Check(std::abs(configFilter.GetAccelViolationThresholdScale() - 1.20) < 1e-12,
             "invalid accel/decel violation threshold scale should fall back to default") ||
         !Check(std::abs(configFilter.GetVelocityViolationDelaySec() - 0.50) < 1e-12,
             "invalid velocity violation delay should fall back to default") ||
         !Check(std::abs(configFilter.GetAccelViolationDelaySec() - 0.20) < 1e-12,
             "invalid accel/decel violation delay should fall back to default")) {
         return EXIT_FAILURE;
     }
    }

    PDOFilter filter;
    filter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    filter.SetPDOLimits(-1000.0, 1000.0);

    const double firstOutput = filter.Filter(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    if (!Check(filter.IsInitialized(), "filter should initialize on first sample") ||
        !Check(std::isfinite(firstOutput), "first output should be finite") ||
        !Check(std::abs(firstOutput) < 1e-9, "first output should start from actual position")) {
        return EXIT_FAILURE;
    }

    const double secondOutput = filter.Filter(1.0, 0.0, 0.0);
    const double thirdOutput = filter.Filter(1.0, secondOutput, 0.0);
    if (!Check(std::isfinite(secondOutput), "second output should be finite") ||
        !Check(std::isfinite(thirdOutput), "third output should be finite") ||
        !Check(secondOutput >= firstOutput, "output should move forward for a positive step") ||
        !Check(thirdOutput >= secondOutput, "output should continue forward without reversing") ||
        !Check(std::abs(filter.GetCurrentVelocity()) <= filter.GetVelocityViolationThreshold() + 1e-6, "reported velocity should remain within configured threshold for this tiny smoke step")) {
        return EXIT_FAILURE;
    }

    {
        PDOFilter responsivenessFilter;
        responsivenessFilter.Configure(200.0, 50000.0, 50000.0, 20000.0, 1000);
        responsivenessFilter.SetPDOLimits(-1000.0, 1000.0);

        double actual = 0.0;
        double output = responsivenessFilter.Filter(0.0, actual, std::numeric_limits<double>::quiet_NaN());
        actual = output;

        double maxSteadyRampLag = 0.0;
        for (int step = 1; step <= 500; ++step) {
            const double setpoint = 60.0 * static_cast<double>(step) * 0.001;
            output = responsivenessFilter.Filter(setpoint, actual, std::numeric_limits<double>::quiet_NaN());
            actual = output;

            if (step > 150) {
                maxSteadyRampLag = std::max(maxSteadyRampLag, setpoint - output);
            }
        }

        if (!Check(maxSteadyRampLag < 0.12,
                   "input moving-average compensation should keep legal ramp lag below 0.12 degrees")) {
            std::cerr << "maxSteadyRampLag=" << maxSteadyRampLag << '\n';
            return EXIT_FAILURE;
        }
    }

    PDOFilter lagGuardFilter;
    lagGuardFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    lagGuardFilter.SetPDOLimits(-1000.0, 1000.0);

    bool measurementMismatchSeen = false;
    lagGuardFilter.SetInputAnomalyCallback(
        [&](double, double, double, double, PDOFilter::InputAnomalyReason reason) {
            if (reason == PDOFilter::InputAnomalyReason::MeasurementMismatch) {
                measurementMismatchSeen = true;
            }
        });

    double guardedOutput = lagGuardFilter.Filter(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    if (!Check(std::isfinite(guardedOutput), "lag-guard filter should initialize cleanly")) {
        return EXIT_FAILURE;
    }

    double commandedPosition = 0.0;
    double previousVelocity = lagGuardFilter.GetCurrentVelocity();
    const double maxVelocityStep = std::max(lagGuardFilter.GetConfiguredMaxAcceleration(),
                                            lagGuardFilter.GetConfiguredMaxDeceleration()) * 0.01 + 1e-6;
    const double trackingLimit = lagGuardFilter.GetTrackingErrorLimit();
    for (int i = 0; i < 400; ++i) {
        commandedPosition += 0.2;
        guardedOutput = lagGuardFilter.Filter(commandedPosition, 0.0, 0.0);
        const double currentVelocity = lagGuardFilter.GetCurrentVelocity();

        if (!Check(std::isfinite(guardedOutput), "guarded output should remain finite during lag protection") ||
            !Check(std::isfinite(currentVelocity), "guarded velocity should remain finite during lag protection")) {
            return EXIT_FAILURE;
        }

        if (guardedOutput < trackingLimit - 1e-6 &&
            std::abs(currentVelocity - previousVelocity) > maxVelocityStep) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6)
                << "lag protection must not introduce velocity jumps"
                << " i=" << i
                << " out=" << guardedOutput
                << " prevVel=" << previousVelocity
                << " curVel=" << currentVelocity
                << " delta=" << std::abs(currentVelocity - previousVelocity)
                << " limit=" << maxVelocityStep;
            if (!Check(false, oss.str().c_str())) {
                return EXIT_FAILURE;
            }
        }

        previousVelocity = currentVelocity;
    }

    if (!Check(measurementMismatchSeen, "tracking mismatch protection should report MeasurementMismatch") ||
        !Check(guardedOutput < 40.0, "lag protection should keep the command from running far ahead of feedback")) {
        return EXIT_FAILURE;
    }

    // Regression: tracking-mismatch replacement must follow the current input
    // direction, not the stale/previous command direction. If the last issued
    // command is above actual but the new input is already back inside the
    // tracking window, replacing the input with actual+window makes the filter
    // keep driving upward even though the operator command has returned low.
    {
        PDOFilter guardSourceFilter;
        guardSourceFilter.Configure(100.0, 600.0, 600.0, 1000.0, 1000);
        guardSourceFilter.SetPDOLimits(-1000.0, 1000.0);
        double mismatchReplacement = std::numeric_limits<double>::quiet_NaN();
        guardSourceFilter.SetInputAnomalyCallback(
            [&](double, double replacement, double, double, PDOFilter::InputAnomalyReason reason) {
                if (reason == PDOFilter::InputAnomalyReason::MeasurementMismatch) {
                    mismatchReplacement = replacement;
                }
            });
        double actual = 0.0;
        double output = guardSourceFilter.Filter(0.0, actual, std::numeric_limits<double>::quiet_NaN());
        guardSourceFilter.CompleteSoftStart();
        double highTarget = 0.0;
        for (int i = 0; i < 900; ++i) {
            highTarget += 0.4;
            output = guardSourceFilter.Filter(highTarget, actual, 0.0);
            actual = output;
        }
        const double slippedActual = output - (guardSourceFilter.GetTrackingErrorLimit() + 5.0);
        output = guardSourceFilter.Filter(slippedActual, slippedActual, 0.0);
        if (!Check(std::isfinite(mismatchReplacement),
                   "tracking mismatch replacement should be reported when actual slips behind issued command") ||
            !Check(std::abs(mismatchReplacement - slippedActual) <= 1e-6,
                   "tracking mismatch replacement must preserve the safe current input instead of forcing actual+window")) {
            std::cerr << "mismatchReplacement=" << mismatchReplacement
                      << " slippedActual=" << slippedActual
                      << " output=" << output << '\n';
            return EXIT_FAILURE;
        }
    }

    // Regression: dynamic/global limiting may soften acceleration, but it must
    // keep the configured deceleration available. Otherwise when MMF input has
    // already slowed/reversed, a 70-90 deg/s residual old-direction velocity can
    // take hundreds of ms to decay, push the filter into the tracking-error
    // window, and leave Filter output pinned away from the new input.
    {
        PDOFilter safetyDecelFilter;
        safetyDecelFilter.Configure(100.0, 600.0, 600.0, 1000.0, 1000);
        safetyDecelFilter.SetPDOLimits(-1000.0, 1000.0);
        safetyDecelFilter.SetViolationCallback(
            [](PDOFilter::ViolationType, double, double, bool, double) {});
        double actual = 0.0;
        double output = safetyDecelFilter.Filter(0.0, actual, std::numeric_limits<double>::quiet_NaN());
        safetyDecelFilter.CompleteSoftStart();
        double highTarget = 0.0;
        for (int i = 0; i < 900; ++i) {
            highTarget += 0.4;
            output = safetyDecelFilter.Filter(highTarget, actual, 0.0);
            actual = output;
        }

        safetyDecelFilter.SetGlobalLimitingEnabled(true);
        if (!Check(safetyDecelFilter.GetMaxDeceleration() < safetyDecelFilter.GetConfiguredMaxDeceleration(),
                   "global-only limiting should still start with a softened deceleration envelope")) {
            return EXIT_FAILURE;
        }

        safetyDecelFilter.SetDynamicViolationConfig(0.60, 0.10, 0.0, 0.0);
        for (int i = 0; i < 24; ++i) {
            output = safetyDecelFilter.FilterWithInputState(
                highTarget,
                actual,
                -200.0,
                true,
                true,
                0.001,
                true);
            actual = output;
        }
         if (!Check(std::abs(safetyDecelFilter.GetMaxAcceleration() - safetyDecelFilter.GetConfiguredMaxAcceleration()) < 1e-9,
                 "a globally limited axis must restore configured acceleration once it has its own dynamic violation") ||
             !Check(std::abs(safetyDecelFilter.GetMaxDeceleration() - safetyDecelFilter.GetConfiguredMaxDeceleration()) < 1e-9,
                 "a globally limited axis must restore configured deceleration once it has its own dynamic violation")) {
            return EXIT_FAILURE;
        }

        const double beforeVelocity = safetyDecelFilter.GetInternalVelocity();
        const double laggedActual = output - (safetyDecelFilter.GetTrackingErrorLimit() + 5.0);
        const double lowTarget = laggedActual - 50.0;
        output = safetyDecelFilter.Filter(lowTarget, laggedActual, 0.0);
        const double afterVelocity = safetyDecelFilter.GetInternalVelocity();
        const double velocityBleed = beforeVelocity - afterVelocity;
        const double configuredDecelStep = safetyDecelFilter.GetConfiguredMaxDeceleration() * 0.001;
        if (!Check(beforeVelocity > 10.0, "test setup should build positive velocity before safety braking") ||
            !Check(velocityBleed >= configuredDecelStep * 0.75,
                   "tracking safety braking must use configured decel, not globally reduced decel")) {
            std::cerr << "beforeVelocity=" << beforeVelocity
                      << " afterVelocity=" << afterVelocity
                      << " velocityBleed=" << velocityBleed
                      << " configuredStep=" << configuredDecelStep
                      << " output=" << output << '\n';
            return EXIT_FAILURE;
        }
    }

    PDOFilter unconfiguredFilter;
    unconfiguredFilter.SetPDOLimits(-1000.0, 1000.0);

    const double unconfiguredOutput = unconfiguredFilter.Filter(120.0, 12.5, std::numeric_limits<double>::quiet_NaN());
    if (!Check(std::isfinite(unconfiguredOutput), "unconfigured filter should still return a finite output") ||
        !Check(std::abs(unconfiguredOutput - 12.5) < 1e-9, "unconfigured filter should stay synced to actual position") ||
        !Check(!unconfiguredFilter.IsInitialized(), "unconfigured filter should remain passive until limits are configured") ||
        !Check(std::abs(unconfiguredFilter.GetInternalVelocity()) < 1e-9, "unconfigured filter should not keep any residual velocity")) {
        return EXIT_FAILURE;
    }

    PDOFilter invalidConfigFilter;
    invalidConfigFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    invalidConfigFilter.SetPDOLimits(-1000.0, 1000.0);

    double commanded = 0.0;
    double movingActual = 0.0;
    double invalidConfigOutput = invalidConfigFilter.Filter(commanded, movingActual, std::numeric_limits<double>::quiet_NaN());
    for (int i = 0; i < 12; ++i) {
        commanded += 0.0002;
        invalidConfigOutput = invalidConfigFilter.Filter(commanded, movingActual, 0.0);
        movingActual = invalidConfigOutput;
    }

    if (!Check(std::isfinite(invalidConfigOutput), "precondition filter output should remain finite before invalid reconfigure") ||
        !Check(invalidConfigFilter.GetInternalVelocity() > 0.0, "precondition filter should build positive internal velocity before invalid reconfigure")) {
        return EXIT_FAILURE;
    }

    invalidConfigFilter.Configure(0.0, 0.0, 0.0, 0.0, 1000);
    const double passiveOutput = invalidConfigFilter.Filter(commanded, movingActual, 0.0);
    if (!Check(std::isfinite(passiveOutput), "invalidly configured filter should still return a finite output") ||
        !Check(std::abs(passiveOutput - movingActual) < 1e-9, "invalid configuration should clamp output back to actual position immediately") ||
        !Check(!invalidConfigFilter.IsInitialized(), "invalid configuration should keep the filter passive until valid limits return") ||
        !Check(std::abs(invalidConfigFilter.GetInternalVelocity()) < 1e-9, "invalid configuration should clear stale internal velocity to prevent jump-speed carryover")) {
        return EXIT_FAILURE;
    }

    invalidConfigFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    const double rearmedOutput = invalidConfigFilter.Filter(commanded, movingActual, 0.0);
    commanded += 0.0002;
    const double resumedOutput = invalidConfigFilter.Filter(commanded, movingActual, 0.0);
    if (!Check(std::isfinite(rearmedOutput), "filter should reinitialize cleanly after valid limits are restored") ||
        !Check(std::abs(rearmedOutput - movingActual) < 1e-9, "reinitialized filter should start from actual position after invalid config") ||
        !Check(invalidConfigFilter.IsInitialized(), "filter should leave passive mode after valid limits are restored") ||
        !Check(resumedOutput >= rearmedOutput, "filter should resume forward motion smoothly after valid limits are restored")) {
        return EXIT_FAILURE;
    }

    PDOFilter commandHardLimitFilter;
    commandHardLimitFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    commandHardLimitFilter.SetPDOLimits(-10.0, 10.0);

    bool commandHardLimitMaxSeen = false;
    bool commandHardLimitLimiting = false;
    commandHardLimitFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool limiting, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax)) {
                commandHardLimitMaxSeen = true;
                commandHardLimitLimiting = limiting;
            }
        });

    double hardLimitOutput = commandHardLimitFilter.Filter(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    hardLimitOutput = commandHardLimitFilter.Filter(12.0, 0.0, 0.0);
    if (!Check(std::isfinite(hardLimitOutput), "command hard-limit output should stay finite") ||
        !Check(commandHardLimitMaxSeen, "command above upper limit should report HardLimitMax") ||
        !Check(commandHardLimitLimiting, "command above upper limit should engage limiting") ||
        !Check(commandHardLimitFilter.HasHardLimitViolation(), "command above upper limit should latch hard-limit state")) {
        return EXIT_FAILURE;
    }

    PDOFilter actualHardLimitFilter;
    actualHardLimitFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    actualHardLimitFilter.SetPDOLimits(-10.0, 10.0);

    bool actualHardLimitMaxSeen = false;
    actualHardLimitFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax)) {
                actualHardLimitMaxSeen = true;
            }
        });

    hardLimitOutput = actualHardLimitFilter.Filter(5.0, 9.5, std::numeric_limits<double>::quiet_NaN());
    hardLimitOutput = actualHardLimitFilter.Filter(5.0, 10.5, 0.0);
    if (!Check(std::isfinite(hardLimitOutput), "actual hard-limit output should stay finite") ||
        !Check(actualHardLimitMaxSeen, "actual position above upper limit should report HardLimitMax") ||
        !Check(actualHardLimitFilter.HasHardLimitViolation(), "actual position above upper limit should latch hard-limit state")) {
        return EXIT_FAILURE;
    }

    PDOFilter softStartLowerEscapeFilter;
    softStartLowerEscapeFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    softStartLowerEscapeFilter.SetPDOLimits(-65.0, 65.0);
    softStartLowerEscapeFilter.TriggerSoftStart();

    bool softStartLowerEscapeHardLimitSeen = false;
    softStartLowerEscapeFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) {
                softStartLowerEscapeHardLimitSeen = true;
            }
        });

    double softStartLowerEscapeOutput = softStartLowerEscapeFilter.Filter(-64.0, -68.0, 0.0);
    softStartLowerEscapeOutput = softStartLowerEscapeFilter.Filter(-64.0, -68.0005, 0.0);
    if (!Check(std::isfinite(softStartLowerEscapeOutput), "soft-start lower-limit escape output should stay finite") ||
        !Check(!softStartLowerEscapeHardLimitSeen, "soft-start from below P9 toward the safe range must not report HardLimitMin") ||
        !Check(!softStartLowerEscapeFilter.HasHardLimitViolation(), "soft-start escaping the lower boundary must not latch hard-limit state")) {
        return EXIT_FAILURE;
    }

    PDOFilter softStartLowerOutwardFilter;
    softStartLowerOutwardFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
    softStartLowerOutwardFilter.SetPDOLimits(-65.0, 65.0);
    softStartLowerOutwardFilter.TriggerSoftStart();

    bool softStartLowerOutwardHardLimitSeen = false;
    softStartLowerOutwardFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) {
                softStartLowerOutwardHardLimitSeen = true;
            }
        });

    double softStartLowerOutwardOutput = softStartLowerOutwardFilter.Filter(-64.0, -68.0, 0.0);
    softStartLowerOutwardOutput = softStartLowerOutwardFilter.Filter(-69.0, -68.0, 0.0);
    if (!Check(std::isfinite(softStartLowerOutwardOutput), "soft-start lower-limit outward output should stay finite") ||
        !Check(softStartLowerOutwardHardLimitSeen, "soft-start command farther below the adjusted lower boundary must still report HardLimitMin") ||
        !Check(softStartLowerOutwardFilter.HasHardLimitViolation(), "soft-start outward lower command must still latch hard-limit state")) {
        return EXIT_FAILURE;
    }

    PDOFilter softStartGuardFilter;
    softStartGuardFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    softStartGuardFilter.SetPDOLimits(-1000.0, 1000.0);
    softStartGuardFilter.TriggerSoftStart();

    if (!Check(std::abs(softStartGuardFilter.GetMaxVelocity() - 14.0) < 1e-9,
               "soft-start max velocity should use the configured 0.14 ratio")) {
        return EXIT_FAILURE;
    }

    bool softStartTrackingViolationSeen = false;
    softStartGuardFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                softStartTrackingViolationSeen = true;
            }
        });

    const double softStartLimit = softStartGuardFilter.GetTrackingErrorLimit();
    if (!Check(std::abs(softStartLimit - 5.0) < 1e-9, "soft-start tracking-error limit should be tightened to 5 degrees")) {
        return EXIT_FAILURE;
    }

    double softStartOutput = softStartGuardFilter.Filter(30.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    softStartOutput = softStartGuardFilter.Filter(30.0, 0.0, 0.0);
    if (!Check(std::isfinite(softStartOutput), "soft-start guarded output should remain finite") ||
        !Check(!softStartTrackingViolationSeen, "soft-start must not flag tracking error before the filter output itself reaches the tightened limit")) {
        return EXIT_FAILURE;
    }

    for (int i = 0; i < 120; ++i) {
        softStartOutput = softStartGuardFilter.Filter(30.0, 0.0, 0.0);
    }

    if (!Check(!softStartTrackingViolationSeen, "soft-start lag should clamp internally without emitting TrackingError callbacks") ||
        !Check(!PDOFilter::HasFlag(softStartGuardFilter.GetCurrentViolationType(), PDOFilter::ViolationType::TrackingError),
               "soft-start tracking clamp must not become a formal reported violation") ||
        !Check(!softStartGuardFilter.IsError(), "soft-start tracking clamp must not latch the filter error flag") ||
        !Check(softStartOutput <= softStartLimit + 1e-6, "soft-start tracking guard should cap output within tightened limit")) {
        return EXIT_FAILURE;
    }

    {
        PDOFilter softStartTargetBrakeFilter;
        softStartTargetBrakeFilter.Configure(720.0, 600.0, 600.0, 4000.0, 1000);
        softStartTargetBrakeFilter.SetPDOLimits(-65.0, 65.0);
        softStartTargetBrakeFilter.TriggerSoftStart();

        const double fixedTarget = 36.79;
        double actual = -68.0;
        double output = softStartTargetBrakeFilter.Filter(fixedTarget, actual, 0.0);
        double maxOutput = output;
        double velocityWhenLeadIntroduced = std::numeric_limits<double>::quiet_NaN();
        double velocityAfterLead = std::numeric_limits<double>::quiet_NaN();
        bool leadIntroduced = false;
        int cyclesAfterLead = 0;

        for (int i = 0; i < 6500; ++i) {
            output = softStartTargetBrakeFilter.Filter(fixedTarget, actual, 0.0);
            maxOutput = std::max(maxOutput, output);
            const double velocity = softStartTargetBrakeFilter.GetInternalVelocity();

            if (leadIntroduced) {
                ++cyclesAfterLead;
                if (cyclesAfterLead == 80) {
                    velocityAfterLead = velocity;
                    break;
                }
            }

            if (output >= fixedTarget - 1.0) {
                if (!leadIntroduced) {
                    leadIntroduced = true;
                    velocityWhenLeadIntroduced = velocity;
                }
                actual = std::min(output + 22.0, 64.0);
            } else {
                actual = output;
            }
        }

        if (!Check(leadIntroduced,
                   "soft-start target-brake regression should exercise leading feedback near the fixed target") ||
            !Check(std::isfinite(velocityAfterLead),
                   "soft-start target-brake regression should collect post-lead velocity") ||
            !Check(velocityWhenLeadIntroduced > 10.0,
                   "soft-start target-brake setup should have positive velocity when leading feedback starts") ||
            !Check(velocityAfterLead < velocityWhenLeadIntroduced - 5.0,
                   "soft-start must brake when feedback leads outward near a fixed target") ||
            !Check(maxOutput <= fixedTarget + 0.5,
                   "soft-start fixed-target output must not overshoot the command target by a large margin")) {
            std::cerr << "soft-start target-brake state:"
                      << " maxOutput=" << maxOutput
                      << " velocityWhenLeadIntroduced=" << velocityWhenLeadIntroduced
                      << " velocityAfterLead=" << velocityAfterLead
                      << " leadIntroduced=" << (leadIntroduced ? 1 : 0)
                      << '\n';
            return EXIT_FAILURE;
        }
    }

    {
        PDOFilter postSoftStartLimitPreviewFilter;
        postSoftStartLimitPreviewFilter.Configure(720.0, 600.0, 600.0, 4000.0, 1000);
        postSoftStartLimitPreviewFilter.SetPDOLimits(-65.0, 65.0);
        postSoftStartLimitPreviewFilter.TriggerSoftStart();

        constexpr double kCycleSec = 0.001;
        double actual = 0.0;
        double output = postSoftStartLimitPreviewFilter.FilterWithInputState(
            0.0,
            actual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            true,
            kCycleSec,
            false);
        actual = output;
        postSoftStartLimitPreviewFilter.CompleteSoftStart();
        postSoftStartLimitPreviewFilter.SetLimitsWithRamp(720.0, 600.0, 600.0);

        double maxOutput = output;
        for (int i = 0; i < 3000; ++i) {
            output = postSoftStartLimitPreviewFilter.FilterWithInputState(
                64.0,
                actual,
                std::numeric_limits<double>::quiet_NaN(),
                true,
                true,
                kCycleSec,
                false);
            actual = output;
            maxOutput = std::max(maxOutput, output);
        }

         if (!Check(maxOutput <= 63.0 + 1e-6,
                 "post-soft-start limit preview should ignore commands inside the last 2 degrees before upper PDO limit") ||
            !Check(std::abs(postSoftStartLimitPreviewFilter.GetInternalVelocity()) <= 0.5,
                   "post-soft-start limit preview should bleed outward feedforward to zero at the protected edge")) {
            std::cerr << "post-soft-start limit-preview state:"
                      << " maxOutput=" << maxOutput
                      << " finalVel=" << postSoftStartLimitPreviewFilter.GetInternalVelocity()
                      << '\n';
            return EXIT_FAILURE;
        }
    }

    {
        PDOFilter softStartMeasuredVelocityBrakeFilter;
        softStartMeasuredVelocityBrakeFilter.Configure(720.0, 600.0, 600.0, 4000.0, 1000);
        softStartMeasuredVelocityBrakeFilter.SetPDOLimits(-65.0, 65.0);
        softStartMeasuredVelocityBrakeFilter.TriggerSoftStart();

        const double fixedTarget = 36.79;
        double actual = -68.0;
        double output = softStartMeasuredVelocityBrakeFilter.Filter(fixedTarget, actual, 0.0);
        double maxOutput = output;
        double velocityWhenLeadIntroduced = std::numeric_limits<double>::quiet_NaN();
        double velocityAfterLead = std::numeric_limits<double>::quiet_NaN();
        bool leadIntroduced = false;
        int cyclesAfterLead = 0;

        for (int i = 0; i < 6500; ++i) {
            const double measuredVelocity = (output >= fixedTarget - 1.0) ? 80.0 : 0.0;
            output = softStartMeasuredVelocityBrakeFilter.Filter(fixedTarget, actual, measuredVelocity);
            maxOutput = std::max(maxOutput, output);
            const double velocity = softStartMeasuredVelocityBrakeFilter.GetInternalVelocity();

            if (leadIntroduced) {
                ++cyclesAfterLead;
                if (cyclesAfterLead == 80) {
                    velocityAfterLead = velocity;
                    break;
                }
            }

            if (output >= fixedTarget - 1.0) {
                if (!leadIntroduced) {
                    leadIntroduced = true;
                    velocityWhenLeadIntroduced = velocity;
                }
                actual = std::min(output + 22.0, 64.0);
            } else {
                actual = output;
            }
        }

        if (!Check(leadIntroduced,
                   "soft-start measured-velocity brake regression should exercise leading feedback near the fixed target") ||
            !Check(std::isfinite(velocityAfterLead),
                   "soft-start measured-velocity brake regression should collect post-lead velocity") ||
            !Check(velocityWhenLeadIntroduced > 10.0,
                   "soft-start measured-velocity brake setup should have positive velocity when leading feedback starts") ||
            !Check(velocityAfterLead < velocityWhenLeadIntroduced - 5.0,
                   "soft-start must brake against the command target even when measured feedback velocity is still outward") ||
            !Check(maxOutput <= fixedTarget + 0.5,
                   "soft-start fixed-target output must not overshoot the command target by a large margin when measured feedback velocity is outward")) {
            std::cerr << "soft-start measured-velocity brake state:"
                      << " maxOutput=" << maxOutput
                      << " velocityWhenLeadIntroduced=" << velocityWhenLeadIntroduced
                      << " velocityAfterLead=" << velocityAfterLead
                      << " leadIntroduced=" << (leadIntroduced ? 1 : 0)
                      << '\n';
            return EXIT_FAILURE;
        }
    }

    PDOFilter softStartRestoreFilter;
    softStartRestoreFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    softStartRestoreFilter.SetPDOLimits(-1000.0, 1000.0);
    softStartRestoreFilter.TriggerSoftStart();

    double restoreSetpoint = 0.0;
    double restoreActual = 0.0;
    double restoreOutput = softStartRestoreFilter.Filter(restoreSetpoint, restoreActual, std::numeric_limits<double>::quiet_NaN());
    for (int i = 0; i < 8; ++i) {
        restoreSetpoint += 0.05;
        restoreOutput = softStartRestoreFilter.Filter(restoreSetpoint, restoreActual, std::numeric_limits<double>::quiet_NaN());
        restoreActual = restoreOutput;
    }

    softStartRestoreFilter.CompleteSoftStart();
    const auto recoveryDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3300);
    int recoveryProgressTick = 0;
    while (std::chrono::steady_clock::now() < recoveryDeadline) {
        restoreSetpoint = std::min(restoreSetpoint + 0.2, 80.0);
        restoreOutput = softStartRestoreFilter.Filter(restoreSetpoint, restoreActual, std::numeric_limits<double>::quiet_NaN());
        restoreActual = restoreOutput;
        ++recoveryProgressTick;
        if ((recoveryProgressTick % 30) == 0) {
            LogProgress("soft-start restore ramp", recoveryProgressTick, 165);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    restoreSetpoint = std::min(restoreSetpoint + 0.2, 80.0);
    restoreOutput = softStartRestoreFilter.Filter(restoreSetpoint, restoreActual, std::numeric_limits<double>::quiet_NaN());
    restoreActual = restoreOutput;

    const double restoredMaxVelocity = softStartRestoreFilter.GetMaxVelocity();
    if (std::abs(restoredMaxVelocity - 100.0) >= 1e-3) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6)
            << "soft-start completion should restore configured max velocity after the recovery ramp"
            << " restoredMaxVelocity=" << restoredMaxVelocity
            << " limitingEnabled=" << (softStartRestoreFilter.IsLimitingEnabled() ? 1 : 0)
            << " violationType=" << static_cast<int>(softStartRestoreFilter.GetCurrentViolationType());
        if (!Check(false, oss.str().c_str())) {
            return EXIT_FAILURE;
        }
    }

    PDOFilter softStartCompletionContinuityFilter;
    softStartCompletionContinuityFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    softStartCompletionContinuityFilter.SetPDOLimits(-1000.0, 1000.0);
    softStartCompletionContinuityFilter.TriggerSoftStart();

    double continuitySetpoint = 10.0;
    double continuityActual = 0.0;
    double continuityOutput = softStartCompletionContinuityFilter.Filter(
        continuitySetpoint,
        continuityActual,
        std::numeric_limits<double>::quiet_NaN());
    continuityActual = continuityOutput;

    bool completionReady = false;
    for (int i = 0; i < 300; ++i) {
        continuityOutput = softStartCompletionContinuityFilter.Filter(
            continuitySetpoint,
            continuityActual,
            std::numeric_limits<double>::quiet_NaN());
        continuityActual = continuityOutput;
        if (softStartCompletionContinuityFilter.IsSoftStartReady()) {
            completionReady = true;
            break;
        }
        if (((i + 1) % 60) == 0) {
            LogProgress("soft-start ready wait", i + 1, 300);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(completionReady, "soft-start continuity filter should become ready before completion handoff test")) {
        return EXIT_FAILURE;
    }

    const double completionReferenceOutput = continuityOutput;
    softStartCompletionContinuityFilter.CompleteSoftStart();

    double completionMinOutput = continuityOutput;
    for (int i = 0; i < 20; ++i) {
        continuityOutput = softStartCompletionContinuityFilter.Filter(
            continuitySetpoint,
            continuityActual,
            std::numeric_limits<double>::quiet_NaN());
        continuityActual = continuityOutput;
        completionMinOutput = std::min(completionMinOutput, continuityOutput);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(completionMinOutput >= (completionReferenceOutput - 1.0),
               "soft-start completion should not drop the output back toward zero during handoff")) {
        return EXIT_FAILURE;
    }

    PDOFilter softStartSaturationFilter;
    softStartSaturationFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    softStartSaturationFilter.SetPDOLimits(-1000.0, 1000.0);
    softStartSaturationFilter.TriggerSoftStart();

    bool softStartSaturationFatalReported = false;
    softStartSaturationFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double duration) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError) && duration >= 0.5) {
                softStartSaturationFatalReported = true;
            }
        });

    double softStartSaturationOutput = softStartSaturationFilter.Filter(30.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    if (!Check(std::isfinite(softStartSaturationOutput), "soft-start saturation filter should initialize cleanly")) {
        return EXIT_FAILURE;
    }

    for (int i = 0; i < 120; ++i) {
        softStartSaturationOutput = softStartSaturationFilter.Filter(30.0, 0.0, 0.0);
        if (((i + 1) % 40) == 0) {
            LogProgress("soft-start saturation guard", i + 1, 120);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(!softStartSaturationFatalReported,
               "soft-start saturation must not escalate to fatal tracking only because the upstream target is far") ||
        !Check(!softStartSaturationFilter.IsError(),
               "soft-start saturation must not latch PDOFilter error while output is guarded within the soft-start tracking window") ||
        !Check(softStartSaturationOutput <= softStartLimit + 1e-6,
               "soft-start saturation should keep output inside the tightened tracking window")) {
        return EXIT_FAILURE;
    }

    PDOFilter softStartFollowerFilter;
    softStartFollowerFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    softStartFollowerFilter.SetPDOLimits(-1000.0, 1000.0);
    softStartFollowerFilter.TriggerSoftStart();

    bool followerTrackingReported = false;
    softStartFollowerFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                followerTrackingReported = true;
            }
        });

    double followerActual = 0.0;
    double followerOutput = softStartFollowerFilter.Filter(30.0, followerActual, std::numeric_limits<double>::quiet_NaN());
    followerActual = followerOutput;
    for (int i = 0; i < 90; ++i) {
        followerOutput = softStartFollowerFilter.Filter(30.0, followerActual, std::numeric_limits<double>::quiet_NaN());
        followerActual = followerOutput;
        if (((i + 1) % 30) == 0) {
            LogProgress("soft-start follower", i + 1, 90);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(!followerTrackingReported,
               "soft-start must not report fatal tracking only because the upstream target is still far when feedback follows issued output") ||
        !Check(!softStartFollowerFilter.IsError(),
               "soft-start follower case must not latch PDOFilter error while feedback follows issued output")) {
        return EXIT_FAILURE;
    }

    PDOFilter fatalTrackingFilter;
    fatalTrackingFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    constexpr double kFatalPdoMin = -65.0;
    constexpr double kFatalPdoMax = 65.0;
    constexpr double kFatalCycleTimeSec = 0.010;
    fatalTrackingFilter.SetPDOLimits(kFatalPdoMin, kFatalPdoMax);

    bool fatalTrackingReported = false;
    double maxTrackingDuration = 0.0;
    bool fatalTrackingLatched = false;
    double fatalTrackingLatchTimeSec = -1.0;
    const auto fatalTrackingStart = std::chrono::steady_clock::now();
    fatalTrackingFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool, double duration) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                maxTrackingDuration = std::max(maxTrackingDuration, duration);
                if (duration >= 0.5) {
                    fatalTrackingReported = true;
                }
            }
        });

    double fatalOutput = fatalTrackingFilter.Filter(30.0, 0.0, std::numeric_limits<double>::quiet_NaN());
    if (!Check(std::isfinite(fatalOutput), "fatal-tracking filter should initialize cleanly")) {
        return EXIT_FAILURE;
    }

    constexpr int kFatalTrackingMaxCycles = 420;
    for (int i = 0; i < kFatalTrackingMaxCycles; ++i) {
        fatalOutput = fatalTrackingFilter.Filter(30.0, 0.0, 0.0);
        if (!fatalTrackingLatched && fatalTrackingFilter.IsError()) {
            fatalTrackingLatched = true;
            fatalTrackingLatchTimeSec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - fatalTrackingStart).count();
            break;
        }
        if (((i + 1) % 80) == 0) {
            LogProgress("fatal tracking wait", i + 1, kFatalTrackingMaxCycles);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!fatalTrackingLatched || fatalTrackingLatchTimeSec < 0.5) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6)
            << "persistent tracking error should latch after at least 0.5 seconds"
            << " latchTime=" << fatalTrackingLatchTimeSec
            << " maxDuration=" << maxTrackingDuration
            << " isError=" << (fatalTrackingFilter.IsError() ? 1 : 0)
            << " finalOut=" << fatalOutput;
        if (!Check(false, oss.str().c_str())) {
            return EXIT_FAILURE;
        }
    }

    if (!fatalTrackingReported && maxTrackingDuration > 0.0) {
        std::cerr << "warning: tracking-error callback duration remained below 0.5s while fatal latch succeeded"
                  << " maxDuration=" << maxTrackingDuration << '\n';
    }

    if (!Check(fatalTrackingFilter.IsError(), "persistent tracking error should latch PDOFilter error state")) {
        return EXIT_FAILURE;
    }

    // After fatal latches, the output must converge toward iActValue under
    // the FINAL acceleration limit (no instantaneous snap is allowed). The
    // prior output velocity may still need a bounded decel interval, so this
    // checks the physical envelope directly instead of requiring position to
    // be monotonic every single sample.
    double previousFatalOutput = fatalOutput;
    double previousFatalOutputVelocity = fatalTrackingFilter.GetCurrentVelocity();
    bool awayVelocityDecayed = false;
    int movingAwayCycles = 0;
    bool convergedToActual = std::abs(fatalOutput) < 1e-6;
    constexpr int kFatalConvergeMaxCycles = 4000;
    for (int j = 0; j < kFatalConvergeMaxCycles && !convergedToActual; ++j) {
        fatalOutput = fatalTrackingFilter.Filter(30.0, 0.0, 0.0);
        if (!CheckAccelEnvelope(fatalOutput,
                                previousFatalOutput,
                                previousFatalOutputVelocity,
                                std::max(fatalTrackingFilter.GetConfiguredMaxVelocity(), fatalTrackingFilter.GetMaxVelocity()),
                                std::max(fatalTrackingFilter.GetConfiguredMaxAcceleration(), fatalTrackingFilter.GetMaxAcceleration()),
                                kFatalCycleTimeSec,
                                kFatalPdoMin,
                                kFatalPdoMax,
                                "fatal-tracking latched error path must keep every cycle inside the acceleration envelope")) {
            return EXIT_FAILURE;
        }
        if (!Check(fatalOutput >= kFatalPdoMin - 1e-6 && fatalOutput <= kFatalPdoMax + 1e-6,
                   "fatal-tracking latched error output must stay inside PDO hard limits")) {
            return EXIT_FAILURE;
        }

        const double currentFatalOutputVelocity = fatalTrackingFilter.GetCurrentVelocity();
        const bool previousVelocityWasMovingAwayFromActual =
            std::abs(previousFatalOutput) > 1e-6 &&
            std::abs(previousFatalOutputVelocity) > 0.1 &&
            (previousFatalOutput * previousFatalOutputVelocity) > 0.0;
        if (previousVelocityWasMovingAwayFromActual) {
            ++movingAwayCycles;
            const bool currentVelocityStillMovingAwayFromActual =
                std::abs(fatalOutput) > 1e-6 &&
                std::abs(currentFatalOutputVelocity) > 0.1 &&
                (fatalOutput * currentFatalOutputVelocity) > 0.0;
            if (currentVelocityStillMovingAwayFromActual &&
                std::abs(currentFatalOutputVelocity) > std::abs(previousFatalOutputVelocity) + 1e-6) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6)
                    << "fatal-tracking recovery must decelerate old outward velocity instead of cruising away at constant speed"
                    << " prevOut=" << previousFatalOutput
                    << " out=" << fatalOutput
                    << " prevVel=" << previousFatalOutputVelocity
                    << " curVel=" << currentFatalOutputVelocity
                    << " cycle=" << j;
                if (!Check(false, oss.str().c_str())) {
                    return EXIT_FAILURE;
                }
            }
            if (!currentVelocityStillMovingAwayFromActual ||
                std::abs(currentFatalOutputVelocity) + 1e-6 < std::abs(previousFatalOutputVelocity)) {
                awayVelocityDecayed = true;
            }
        }

        const double absOut = std::abs(fatalOutput);
        previousFatalOutput = fatalOutput;
        previousFatalOutputVelocity = currentFatalOutputVelocity;
        if (absOut < 1e-3) {
            convergedToActual = true;
        }
    }

    if (!Check(convergedToActual,
               "fatal tracking error should converge output toward actual position within bounded cycles") ||
        !Check(movingAwayCycles == 0 || awayVelocityDecayed,
               "fatal tracking recovery must visibly decay any old outward velocity before convergence")) {
        return EXIT_FAILURE;
    }

    // Regression for the field shape: after the fatal/error latch is active,
    // the early error-return path must not snap the filter output to actual
    // feedback, and it must not mirror an actual position that is already
    // beyond the configured PDO hard limit. This guards the visible sequence:
    // acceleration/deceleration violation -> constant-speed outward drift ->
    // one-cycle position jump -> output/actual beyond the limit.
    previousFatalOutput = fatalOutput;
    previousFatalOutputVelocity = fatalTrackingFilter.GetCurrentVelocity();
    bool movedTowardLowerLimit = false;
    constexpr int kLatchedErrorLimitCycles = 220;
    for (int j = 0; j < kLatchedErrorLimitCycles; ++j) {
        fatalOutput = fatalTrackingFilter.Filter(-120.0, -95.0, 0.0);
        if (!Check(std::isfinite(fatalOutput), "latched-error output must stay finite when actual is past the PDO limit") ||
            !Check(fatalOutput >= kFatalPdoMin - 1e-6 && fatalOutput <= kFatalPdoMax + 1e-6,
                   "latched-error output must stay inside the hard PDO limits even when actual feedback is already beyond them") ||
            !CheckAccelEnvelope(fatalOutput,
                                previousFatalOutput,
                                previousFatalOutputVelocity,
                                std::max(fatalTrackingFilter.GetConfiguredMaxVelocity(), fatalTrackingFilter.GetMaxVelocity()),
                                std::max(fatalTrackingFilter.GetConfiguredMaxAcceleration(), fatalTrackingFilter.GetMaxAcceleration()),
                                kFatalCycleTimeSec,
                                kFatalPdoMin,
                                kFatalPdoMax,
                                "latched-error output must not jump while converging toward the clamped safety target")) {
            return EXIT_FAILURE;
        }
        if (fatalOutput < previousFatalOutput - 1e-6) {
            movedTowardLowerLimit = true;
        }
        previousFatalOutput = fatalOutput;
        previousFatalOutputVelocity = fatalTrackingFilter.GetCurrentVelocity();
    }

    if (!Check(movedTowardLowerLimit,
               "latched-error output should move toward the clamped lower safety target without snapping past it")) {
        return EXIT_FAILURE;
    }

    PDOFilter realMmfFilter;
    realMmfFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
    realMmfFilter.SetPDOLimits(-1000.0, 1000.0);

    int realMmfSubstitutionCount = 0;
    realMmfFilter.SetInputAnomalyCallback(
        [&](double, double, double, double, PDOFilter::InputAnomalyReason reason) {
            if (reason == PDOFilter::InputAnomalyReason::ExceededAccelLimit ||
                reason == PDOFilter::InputAnomalyReason::StaleRecovery) {
                ++realMmfSubstitutionCount;
            }
        });

    double realMmfActual = 0.0;
    double realMmfTarget = 0.0;
    double realMmfOutput = realMmfFilter.FilterWithInputState(
        realMmfTarget,
        realMmfActual,
        std::numeric_limits<double>::quiet_NaN(),
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        false);
    double previousInternalVelocity = realMmfFilter.GetInternalVelocity();
    double maxInternalVelocityStep = 0.0;
    constexpr int kCyclesPerMmfSample = 10;
    constexpr int kMmfSamples = 80;
    for (int sample = 1; sample <= kMmfSamples; ++sample) {
        realMmfTarget = 10.0 * std::sin(static_cast<double>(sample) * 0.03);
        for (int subcycle = 0; subcycle < kCyclesPerMmfSample; ++subcycle) {
            const bool sampleUpdated = (subcycle == 0);
            realMmfOutput = realMmfFilter.FilterWithInputState(
                realMmfTarget,
                realMmfActual,
                std::numeric_limits<double>::quiet_NaN(),
                true,
                sampleUpdated,
                sampleUpdated ? 0.010 : std::numeric_limits<double>::quiet_NaN(),
                false);
            realMmfActual = realMmfOutput;

            const double currentInternalVelocity = realMmfFilter.GetInternalVelocity();
            maxInternalVelocityStep = std::max(maxInternalVelocityStep,
                                               std::abs(currentInternalVelocity - previousInternalVelocity));
            previousInternalVelocity = currentInternalVelocity;

            if (!Check(std::isfinite(realMmfOutput), "real MMF output should stay finite") ||
                !Check(!realMmfFilter.WasPredictionUsed(), "real MMF input should not be replaced by prediction")) {
                return EXIT_FAILURE;
            }
        }
    }

    if (!Check(realMmfSubstitutionCount == 0, "real MMF samples should not trigger prediction-based substitution") ||
        !Check(maxInternalVelocityStep <= 0.25, "real MMF filtering should not introduce internal velocity jumps")) {
        std::cerr << "realMmfSubstitutionCount=" << realMmfSubstitutionCount
                  << " maxInternalVelocityStep=" << maxInternalVelocityStep
                  << " finalOutput=" << realMmfOutput << '\n';
        return EXIT_FAILURE;
    }

    PDOFilter guardedInputFilter;
    guardedInputFilter.Configure(10000.0, 100000.0, 100000.0, 2500.0, 1000);
    guardedInputFilter.SetPDOLimits(-1000.0, 1000.0);
    guardedInputFilter.SetViolationCallback([](PDOFilter::ViolationType, double, double, bool, double) {});

    int guardedInputSubstitutions = 0;
    guardedInputFilter.SetInputAnomalyCallback(
        [&](double, double, double, double, PDOFilter::InputAnomalyReason reason) {
            if (reason == PDOFilter::InputAnomalyReason::ExceededAccelLimit ||
                reason == PDOFilter::InputAnomalyReason::StaleRecovery) {
                ++guardedInputSubstitutions;
            }
        });

    double aggressiveGuardActual = 0.0;
    double aggressiveGuardOutput = guardedInputFilter.Filter(0.0, aggressiveGuardActual);
    aggressiveGuardActual = aggressiveGuardOutput;
    for (int i = 0; i < 800; ++i) {
        const double guardedTarget = (i % 2 == 0) ? 30.0 : -30.0;
        aggressiveGuardOutput = guardedInputFilter.Filter(guardedTarget, aggressiveGuardActual);
        aggressiveGuardActual = aggressiveGuardOutput;
        if (!Check(std::isfinite(aggressiveGuardOutput), "input-guard output should stay finite under aggressive command jumps") ||
            !Check(!guardedInputFilter.HasInputAnomalyViolation(), "input guard substitutions should not latch a formal InputAnomaly violation") ||
            !Check(!guardedInputFilter.IsError(), "input guard substitutions should not latch the PDOFilter error flag")) {
            return EXIT_FAILURE;
        }
    }

    if (!Check(guardedInputSubstitutions > 0, "aggressive command jumps should still be guarded and reported diagnostically")) {
        return EXIT_FAILURE;
    }

    PDOFilter accelRecoveryFilter;
    accelRecoveryFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
    accelRecoveryFilter.SetPDOLimits(-1000.0, 1000.0);
    accelRecoveryFilter.SetDynamicViolationConfig(0.60, 1.20, 0.0, 0.0);
    accelRecoveryFilter.SetViolationCallback([](PDOFilter::ViolationType, double, double, bool, double) {});

    double accelRecoveryActual = 0.0;
    double accelRecoveryOutput = accelRecoveryFilter.FilterWithInputState(
        0.0,
        accelRecoveryActual,
        std::numeric_limits<double>::quiet_NaN(),
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        true);
    accelRecoveryActual = accelRecoveryOutput;

    double jumpTarget = 0.0;
    for (int i = 0; i < 80; ++i) {
        jumpTarget += 1.0;
        accelRecoveryOutput = accelRecoveryFilter.FilterWithInputState(
            jumpTarget,
            accelRecoveryActual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            true,
            0.001,
            true);
        accelRecoveryActual = accelRecoveryOutput;
    }

    if (!Check(accelRecoveryFilter.IsLimitingEnabled(),
               "sustained over-acceleration input should engage dynamic limiting before recovery test")) {
        return EXIT_FAILURE;
    }

    const double recoveryStartOutput = accelRecoveryOutput;
    double recoveryCommand = recoveryStartOutput;
    for (int i = 0; i < 600; ++i) {
        recoveryCommand += 0.05;
        accelRecoveryOutput = accelRecoveryFilter.FilterWithInputState(
            recoveryCommand,
            accelRecoveryActual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            true,
            0.001,
            true);
        accelRecoveryActual = accelRecoveryOutput;
    }

    if (!Check(accelRecoveryOutput >= recoveryStartOutput + 2.0,
               "filter output must continue moving toward changed MMF input while dynamic limiting is active") ||
        !Check(std::isfinite(accelRecoveryOutput),
               "dynamic-limiting recovery output must remain finite")) {
        std::cerr << "recoveryStartOutput=" << recoveryStartOutput
                  << " finalOutput=" << accelRecoveryOutput
                  << " finalCommand=" << recoveryCommand
                  << " limiting=" << (accelRecoveryFilter.IsLimitingEnabled() ? 1 : 0)
                  << " currentVelocity=" << accelRecoveryFilter.GetCurrentVelocity()
                  << " internalVelocity=" << accelRecoveryFilter.GetInternalVelocity() << '\n';
        return EXIT_FAILURE;
    }

    PDOFilter measuredVelocityRecoveryFilter;
    measuredVelocityRecoveryFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
    measuredVelocityRecoveryFilter.SetPDOLimits(-1000.0, 1000.0);
    measuredVelocityRecoveryFilter.SetDynamicViolationConfig(0.60, 1.20, 0.0, 0.0);
    measuredVelocityRecoveryFilter.SetViolationCallback([](PDOFilter::ViolationType, double, double, bool, double) {});

    double measuredRecoveryActual = 0.0;
    double measuredRecoveryOutput = measuredVelocityRecoveryFilter.FilterWithInputState(
        0.0,
        measuredRecoveryActual,
        0.0,
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        true);
    measuredRecoveryActual = measuredRecoveryOutput;

    double measuredRecoveryCommand = 0.0;
    for (int i = 0; i < 80; ++i) {
        measuredRecoveryCommand += 1.0;
        measuredRecoveryOutput = measuredVelocityRecoveryFilter.FilterWithInputState(
            measuredRecoveryCommand,
            measuredRecoveryActual,
            120.0,
            true,
            true,
            0.001,
            true);
        measuredRecoveryActual = measuredRecoveryOutput;
    }

    if (!Check(measuredVelocityRecoveryFilter.IsLimitingEnabled(),
               "measured over-velocity input should engage limiting before reversal recovery test")) {
        return EXIT_FAILURE;
    }

    const double measuredRecoveryStartOutput = measuredRecoveryOutput;
    double measuredRecoveryPeakOutput = measuredRecoveryOutput;
    for (int i = 0; i < 800; ++i) {
        measuredRecoveryCommand -= 0.20;
        measuredRecoveryOutput = measuredVelocityRecoveryFilter.FilterWithInputState(
            measuredRecoveryCommand,
            measuredRecoveryActual,
            0.0,
            true,
            true,
            0.001,
            true);
        measuredRecoveryActual = measuredRecoveryOutput;
             measuredRecoveryPeakOutput = std::max(measuredRecoveryPeakOutput, measuredRecoveryOutput);
    }

            if (!Check(measuredVelocityRecoveryFilter.GetInternalVelocity() < -1.0,
                 "filter internal velocity must reverse toward changed MMF input after measured over-acceleration limiting") ||
             !Check(measuredRecoveryOutput <= measuredRecoveryPeakOutput - 1.0,
                 "filter output must come back from the old-direction peak after reversed MMF input") ||
        !Check(std::isfinite(measuredRecoveryOutput),
               "measured-velocity recovery output must remain finite")) {
        std::cerr << "measuredRecoveryStartOutput=" << measuredRecoveryStartOutput
                    << " peakOutput=" << measuredRecoveryPeakOutput
                  << " finalOutput=" << measuredRecoveryOutput
                  << " finalCommand=" << measuredRecoveryCommand
                  << " limiting=" << (measuredVelocityRecoveryFilter.IsLimitingEnabled() ? 1 : 0)
                  << " currentVelocity=" << measuredVelocityRecoveryFilter.GetCurrentVelocity()
                  << " internalVelocity=" << measuredVelocityRecoveryFilter.GetInternalVelocity() << '\n';
        return EXIT_FAILURE;
    }

    PDOFilter accelJitterFilter;
    accelJitterFilter.Configure(5000.0, 100.0, 100.0, 2500.0, 1000);
    accelJitterFilter.SetPDOLimits(-1000.0, 1000.0);

    int dynamicViolationCallbacks = 0;
    bool jitterLimitingSeen = false;
    accelJitterFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool limiting, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Velocity) ||
                PDOFilter::HasFlag(type, PDOFilter::ViolationType::Acceleration) ||
                PDOFilter::HasFlag(type, PDOFilter::ViolationType::Deceleration)) {
                ++dynamicViolationCallbacks;
            }
            jitterLimitingSeen = jitterLimitingSeen || limiting;
        });

    double jitterActual = 0.0;
    double jitterOutput = accelJitterFilter.FilterWithInputState(
        0.0,
        jitterActual,
        0.0,
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        false);
    jitterActual = jitterOutput;

    for (int i = 0; i < 24; ++i) {
        jitterOutput = accelJitterFilter.FilterWithInputState(
            0.0,
            jitterActual,
            0.0,
            true,
            false,
            std::numeric_limits<double>::quiet_NaN(),
            false);
        jitterActual = jitterOutput;
    }

    jitterOutput = accelJitterFilter.FilterWithInputState(
        0.0,
        jitterActual,
        10.0,
        true,
        false,
        std::numeric_limits<double>::quiet_NaN(),
        false);
    jitterActual = jitterOutput;

    for (int i = 0; i < 120; ++i) {
        jitterOutput = accelJitterFilter.FilterWithInputState(
            0.0,
            jitterActual,
            0.0,
            true,
            false,
            std::numeric_limits<double>::quiet_NaN(),
            false);
        jitterActual = jitterOutput;
    }

    if (!Check(dynamicViolationCallbacks == 0,
               "single-cycle measured-velocity jitter should not emit accel/decel violation callbacks") ||
        !Check(!jitterLimitingSeen,
               "single-cycle measured-velocity jitter should not engage limiting") ||
        !Check(accelJitterFilter.GetCurrentViolationType() == PDOFilter::ViolationType::None,
               "single-cycle measured-velocity jitter should not leave a dynamic violation latched")) {
        std::cerr << "dynamicViolationCallbacks=" << dynamicViolationCallbacks
                  << " limitingSeen=" << (jitterLimitingSeen ? 1 : 0)
                  << " currentViolation=" << static_cast<int>(accelJitterFilter.GetCurrentViolationType())
                  << " output=" << jitterOutput << '\n';
        return EXIT_FAILURE;
    }

    PDOFilter heldConstantFilter;
    heldConstantFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
    heldConstantFilter.SetPDOLimits(-1000.0, 1000.0);

    double heldConstantActual = 5.0;
    double heldConstantOutput = heldConstantFilter.FilterWithInputState(
        5.0,
        heldConstantActual,
        std::numeric_limits<double>::quiet_NaN(),
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        true);
    heldConstantActual = heldConstantOutput;

    for (int i = 0; i < 1500; ++i) {
        heldConstantOutput = heldConstantFilter.FilterWithInputState(
            5.0,
            heldConstantActual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            false,
            std::numeric_limits<double>::quiet_NaN(),
            true);
        heldConstantActual = heldConstantOutput;
        if (!Check(!heldConstantFilter.HasStaleInputViolation(), "held constant MMF setpoint should not become a stale-input violation") ||
            !Check(!heldConstantFilter.IsLimitingEnabled(), "held constant MMF setpoint should not engage limiting") ||
            !Check(!heldConstantFilter.IsError(), "held constant MMF setpoint should not latch filter error")) {
            return EXIT_FAILURE;
        }
    }

    PDOFilter heldMmfStepFilter;
    heldMmfStepFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
    heldMmfStepFilter.SetPDOLimits(-1000.0, 1000.0);

    double heldActual = 0.0;
    double heldOutput = heldMmfStepFilter.FilterWithInputState(
        0.0,
        heldActual,
        std::numeric_limits<double>::quiet_NaN(),
        true,
        true,
        std::numeric_limits<double>::quiet_NaN(),
        false);
    heldActual = heldOutput;

    for (int i = 0; i < 30; ++i) {
        heldOutput = heldMmfStepFilter.FilterWithInputState(
            0.0,
            heldActual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            false,
            std::numeric_limits<double>::quiet_NaN(),
            false);
        heldActual = heldOutput;
    }

    double previousHeldVelocity = heldMmfStepFilter.GetInternalVelocity();
    for (int subcycle = 0; subcycle < 10; ++subcycle) {
        heldOutput = heldMmfStepFilter.FilterWithInputState(
            20.0,
            heldActual,
            std::numeric_limits<double>::quiet_NaN(),
            true,
            subcycle == 0,
            subcycle == 0 ? 0.010 : std::numeric_limits<double>::quiet_NaN(),
            false);
        heldActual = heldOutput;

        const double currentHeldVelocity = heldMmfStepFilter.GetInternalVelocity();
        if (!Check(std::isfinite(heldOutput), "held MMF step output should stay finite") ||
            !Check(!heldMmfStepFilter.WasPredictionUsed(), "held MMF step should not use prediction") ||
            !Check(currentHeldVelocity + 1e-9 >= previousHeldVelocity,
                   "held MMF step velocity should not reverse after the first real sample")) {
            std::cerr << "subcycle=" << subcycle
                      << " output=" << heldOutput
                      << " previousVelocity=" << previousHeldVelocity
                      << " currentVelocity=" << currentHeldVelocity << '\n';
            return EXIT_FAILURE;
        }
        previousHeldVelocity = currentHeldVelocity;
    }

    PDOFilter globalForcedRecoveryFilter;
    globalForcedRecoveryFilter.Configure(100.0, 200.0, 200.0, 1000.0, 10000);
    globalForcedRecoveryFilter.SetPDOLimits(-1000.0, 1000.0);

    bool globalForcedViolationSeen = false;
    bool globalForcedClearSeen = false;
    bool globalForcedClearWhileLimiting = false;
    globalForcedRecoveryFilter.SetViolationCallback(
        [&](PDOFilter::ViolationType type, double, double, bool limiting, double) {
            if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Velocity)) {
                globalForcedViolationSeen = true;
            }
            if (type == PDOFilter::ViolationType::None) {
                globalForcedClearSeen = true;
                globalForcedClearWhileLimiting = globalForcedClearWhileLimiting || limiting;
            }
        });

    double globalForcedOutput = globalForcedRecoveryFilter.Filter(
        0.0,
        0.0,
        std::numeric_limits<double>::quiet_NaN());
    if (!Check(std::isfinite(globalForcedOutput), "global-forced recovery filter should initialize cleanly")) {
        return EXIT_FAILURE;
    }

    constexpr int kVelocityViolationCycles = 145;
    for (int i = 0; i < kVelocityViolationCycles; ++i) {
        globalForcedOutput = globalForcedRecoveryFilter.Filter(0.0, 0.0, 150.0);
        if (((i + 1) % 50) == 0) {
            LogProgress("global-forced violation setup", i + 1, kVelocityViolationCycles);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    globalForcedRecoveryFilter.SetGlobalLimitingEnabled(true);
    if (!Check(globalForcedViolationSeen, "global-forced recovery setup should report a velocity violation") ||
        !Check(globalForcedRecoveryFilter.IsLimitingEnabled(), "global-forced recovery setup should engage limiting") ||
         !Check(globalForcedRecoveryFilter.HasVelocityViolation(), "global-forced recovery setup should expose active velocity violation") ||
         !Check(std::abs(globalForcedRecoveryFilter.GetMaxVelocity() - globalForcedRecoveryFilter.GetVelocityViolationThreshold()) < 1e-9,
             "dynamic/global limiting should cap velocity at the violation threshold instead of soft-start crawl speed") ||
         !Check(globalForcedRecoveryFilter.GetMaxVelocity() > globalForcedRecoveryFilter.GetSoftStartMaxVelocity(),
             "dynamic/global limiting must keep enough velocity for the axis to continue moving")) {
        return EXIT_FAILURE;
    }

    constexpr int kCleanStoppedCycles = 75;
    for (int i = 0; i < kCleanStoppedCycles; ++i) {
        globalForcedOutput = globalForcedRecoveryFilter.Filter(0.0, 0.0, 0.0);
        if (((i + 1) % 25) == 0) {
            LogProgress("global-forced clean debounce", i + 1, kCleanStoppedCycles);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(globalForcedClearSeen, "stopped axis should emit violation clear while global limiting is still held") ||
        !Check(globalForcedClearWhileLimiting, "violation clear should be reported before forced limiting is released") ||
        !Check(globalForcedRecoveryFilter.GetCurrentViolationType() == PDOFilter::ViolationType::None,
               "stopped axis should not keep stale velocity violation while globally force-limited") ||
        !Check(globalForcedRecoveryFilter.IsLimitingEnabled(),
               "global-forced limiting should remain held until the master releases it")) {
        std::cerr << "globalForcedClearSeen=" << globalForcedClearSeen
                  << " clearWhileLimiting=" << globalForcedClearWhileLimiting
                  << " currentViolation=" << static_cast<int>(globalForcedRecoveryFilter.GetCurrentViolationType())
                  << " limiting=" << (globalForcedRecoveryFilter.IsLimitingEnabled() ? 1 : 0)
                  << " output=" << globalForcedOutput << '\n';
        return EXIT_FAILURE;
    }

    // === Regression: tracking-error window must bleed internal velocity ===
    // Reproduces the field scenario where a velocity violation forces the
    // input direction to flip during recovery (e.g. operator releases the
    // stick after a fast move). The filter has built up large outward
    // velocity, the new input is far in the opposite direction, and the
    // actual servo lags. The tracking-error window engages and clamps the
    // filter output to (iActValue ± window). Without bleeding the internal
    // velocity that is still pointing outward, GetInternalVelocity() (which
    // CSV uses as 0x60FF feedforward) keeps driving the servo outward, the
    // tracking window slides outward with iActValue, and the filter output
    // runs away in the wrong direction (the symptom: "filter output goes to
    // extreme value and cannot reconverge to track position").
    {
        PDOFilter trackingBleedFilter;
        trackingBleedFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
        trackingBleedFilter.SetPDOLimits(-1000.0, 1000.0);
        // Register a no-op violation callback so ioCurrentViolationType_ gets
        // updated each cycle (needed for HasTrackingErrorViolation()).
        bool trackingViolationFromCallback = false;
        trackingBleedFilter.SetViolationCallback(
            [&](PDOFilter::ViolationType type, double, double, bool, double) {
                if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                    trackingViolationFromCallback = true;
                }
            });
        // Initialize at zero, complete soft-start so the test exercises the
        // steady-state tracking guard (soft-start has its own envelope).
        trackingBleedFilter.Filter(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
        trackingBleedFilter.CompleteSoftStart();

        const double trackingWindow = trackingBleedFilter.GetTrackingErrorLimit();
        const double dt = 0.001;
        const double maxDecel = trackingBleedFilter.GetConfiguredMaxDeceleration();
        // Allow up to ~3x maxDecel*dt per cycle: in the tracking-window-pinned
        // scenario both the normal targetAcc step and the overshoot-brake step
        // can engage in the same cycle, on top of the new tracking-window
        // velocity bleed. The PDO-limit test uses 1.5x because overshoot brake
        // does not fire when effectiveSetpoint sits ahead of the output, but
        // here effectiveSetpoint is at the opposite tracking-window bound so
        // overshoot is large by construction.
        const double maxVelStep = 3.0 * maxDecel * dt;

        // Phase 1: closed-loop ramp positive to build up positive velocity.
        double actual = 0.0;
        double output = 0.0;
        double cmd = 0.0;
        for (int i = 0; i < 800; ++i) {
            cmd += 0.5; // 500 deg/s requested -> filter caps to maxVel
            output = trackingBleedFilter.Filter(cmd, actual, 0.0);
            actual = output; // closed loop
        }
        const double builtVelocity = trackingBleedFilter.GetInternalVelocity();
        if (!Check(builtVelocity > 50.0,
                   "phase 1 should build a substantial positive internal velocity before the input flips")) {
            std::cerr << "builtVelocity=" << builtVelocity << '\n';
            return EXIT_FAILURE;
        }

        // Phase 2: input flips far negative, but actual is now stuck (servo
        // physically cannot track the flip immediately because lower-level
        // limits, mechanical inertia, or a recent velocity violation have
        // pinned it). The filter command stays >= trackingWindow above
        // actual, so the tracking-window clamp engages on every cycle.
        const double pinnedActual = actual;
        const double flippedTarget = pinnedActual - 200.0; // far negative
        double prevInternalVelocity = builtVelocity;
        double prevPublishedVelocity = trackingBleedFilter.GetCurrentVelocity();
        double maxBleedVelocityStep = 0.0;
        bool trackingViolationSeen = false;
        bool internalVelocityBledToZero = false;
        bool publishedVelocityBledToZero = false;
        for (int i = 0; i < 4000; ++i) {
            output = trackingBleedFilter.Filter(flippedTarget, pinnedActual, 0.0);
            const double internalVel = trackingBleedFilter.GetInternalVelocity();
            const double publishedVel = trackingBleedFilter.GetCurrentVelocity();
            const double velStep = std::abs(internalVel - prevInternalVelocity);
            maxBleedVelocityStep = std::max(maxBleedVelocityStep, velStep);
            const bool atUpperTrackingBoundary = output >= pinnedActual + trackingWindow - 1e-6;
            const bool atLowerTrackingBoundary = output <= pinnedActual - trackingWindow + 1e-6;
            const bool clearedOutwardFeedforwardAtTrackingBoundary =
                (atUpperTrackingBoundary && prevInternalVelocity > 0.0 && std::abs(internalVel) <= 1e-6) ||
                (atLowerTrackingBoundary && prevInternalVelocity < 0.0 && std::abs(internalVel) <= 1e-6);

            if (trackingBleedFilter.HasTrackingErrorViolation()) {
                trackingViolationSeen = true;
            }
            if (!Check(std::isfinite(output), "tracking-window output must remain finite during bleed") ||
                !Check(std::isfinite(internalVel), "tracking-window internal velocity must remain finite during bleed") ||
                !Check(std::isfinite(publishedVel), "tracking-window published velocity must remain finite during bleed")) {
                return EXIT_FAILURE;
            }

            if (!clearedOutwardFeedforwardAtTrackingBoundary && velStep > maxVelStep + 1e-6) {
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(6)
                    << "tracking-window velocity bleed must respect max-decel continuity"
                    << " i=" << i
                    << " prevV=" << prevInternalVelocity
                    << " curV=" << internalVel
                    << " step=" << velStep
                    << " allowed=" << maxVelStep;
                if (!Check(false, oss.str().c_str())) {
                    return EXIT_FAILURE;
                }
            }

            // The fix should bring internal velocity to ~0 within a bounded
            // window once the tracking-window clamp engages. Without the
            // bleed, internal velocity stays near maxVel and the runaway
            // would continue indefinitely.
            if (i >= 1500 && std::abs(internalVel) <= 1.0) {
                internalVelocityBledToZero = true;
            }
                 if (i >= 1500 && std::abs(publishedVel) <= 1.0) {
                  publishedVelocityBledToZero = true;
                 }

            prevInternalVelocity = internalVel;
                 prevPublishedVelocity = publishedVel;
        }

        if (!Check(trackingViolationSeen || trackingViolationFromCallback,
                   "tracking-window flip scenario should report a TrackingError violation") ||
            !Check(internalVelocityBledToZero,
                   "internal velocity must decay to ~0 once the tracking-window pins the output "
                   "(else CSV feedforward keeps driving the servo outward and the filter cannot reconverge)") ||
                 !Check(publishedVelocityBledToZero,
                     "published output velocity must decay to ~0 once the tracking-window pins the output "
                     "(else soft-start 0x60FF keeps driving the servo outward)") ||
            !Check(std::abs(trackingBleedFilter.GetInternalVelocity()) <= 1.0,
                   "final internal velocity must be near zero so 0x60FF stops feeding outward velocity") ||
                 !Check(std::abs(trackingBleedFilter.GetCurrentVelocity()) <= 1.0,
                     "final published output velocity must be near zero so soft-start feedforward stops writing outward 0x60FF") ||
            !Check(output <= pinnedActual + trackingWindow + 1e-6,
                   "filter output must remain within iActValue + trackingErrorWindow when the upper guard engages") ||
            !Check(output >= pinnedActual - trackingWindow - 1e-6,
                   "filter output must remain within iActValue - trackingErrorWindow when the upper guard engages")) {
            std::cerr << "trackingViolationSeen=" << trackingViolationSeen
                      << " internalVelocityBledToZero=" << internalVelocityBledToZero
                     << " publishedVelocityBledToZero=" << publishedVelocityBledToZero
                      << " finalInternalVel=" << trackingBleedFilter.GetInternalVelocity()
                     << " finalPublishedVel=" << trackingBleedFilter.GetCurrentVelocity()
                      << " maxStep=" << maxBleedVelocityStep
                      << " output=" << output
                      << " pinnedActual=" << pinnedActual
                      << " window=" << trackingWindow << '\n';
            return EXIT_FAILURE;
        }
    }

    // Tracking-error recovery scenario:
    //
    // Reproduces the field chart where after a tracking-error event the input
    // direction reverses (e.g. user oscillating input drops back through the
    // actual position). The filter MUST continue tracking the new direction
    // instead of freezing the output at the event-time position. Asserts:
    //   - GetTrackingClampDirection() returns to 0 once the filter output
    //     leaves the outward boundary,
    //   - filter output moves substantially toward the new input target,
    //   - filter output stays within iActValue ± trackingErrorWindow at all
    //     times,
    //   - actual (closed-loop) follows the filter output downward.
    {
        PDOFilter recoveryFilter;
        recoveryFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
        recoveryFilter.SetPDOLimits(-1000.0, 1000.0);
        recoveryFilter.SetViolationCallback(
            [](PDOFilter::ViolationType, double, double, bool, double) {});
        recoveryFilter.Filter(0.0, 0.0, std::numeric_limits<double>::quiet_NaN());
        recoveryFilter.CompleteSoftStart();

        const double trackingWindow = recoveryFilter.GetTrackingErrorLimit();
        double actual = 0.0;
        double output = 0.0;

        // Phase 1: closed-loop ramp upward to a substantial positive output.
        double cmd = 0.0;
        for (int i = 0; i < 800; ++i) {
            cmd += 0.5;
            output = recoveryFilter.Filter(cmd, actual, 0.0);
            actual = output;
        }
        const double peakOutput = output;

        // Phase 2: simulate that actual abruptly slips well below the filter
        // output (servo cannot follow because of a velocity-violation lockout
        // or an external mechanical event); the tracking-window upper clamp
        // must engage. Hold actual far below output for ~50 ms while input
        // also drives further upward so the runaway condition would persist
        // without the directional inhibit.
        const double laggedActual = peakOutput - (trackingWindow + 5.0);
        double upwardCmd = peakOutput + 200.0;
        bool clampWentOutward = false;
        for (int i = 0; i < 50; ++i) {
            output = recoveryFilter.Filter(upwardCmd, laggedActual, 0.0);
            if (recoveryFilter.GetTrackingClampDirection() == +1) {
                clampWentOutward = true;
            }
        }
        if (!Check(clampWentOutward,
                   "phase 2 setup: upper tracking-window clamp must engage when actual lags far "
                   "below the filter output and input keeps pushing upward")) {
            std::cerr << "peakOutput=" << peakOutput
                      << " laggedActual=" << laggedActual
                      << " output=" << output
                      << " clampDir=" << recoveryFilter.GetTrackingClampDirection() << '\n';
            return EXIT_FAILURE;
        }

        // Phase 3: input REVERSES far negative, and actual now follows the
        // filter output via a simple closed-loop servo. The fix must let the
        // clamp release once filter output drops off the upper boundary, and
        // CSV-style closed-loop tracking (simulated here as actual chasing
        // output by a bounded per-cycle step) must drag actual downward.
        const double newTarget = -50.0;
        const double dt = 0.001;
        const double maxVel = recoveryFilter.GetConfiguredMaxVelocity();
        const double servoMaxStep = maxVel * dt;
        actual = laggedActual;
        bool clampReleased = false;
        bool actualMovedDownward = false;
        bool outputMovedDownward = false;
        const double startOutput = output;
        const double startActual = actual;
        // Tracking-window convergence is allowed to take multiple cycles
        // because the FINAL acceleration limit caps the per-cycle change of
        // filter output to what the configured max accel/velocity physically
        // permits ("any limiting state must not cause a position jump"). The
        // assertion is therefore: the absolute tracking error
        // |output - actual| must NOT grow from cycle to cycle, and must
        // eventually settle inside the tracking window.
        double prevTrackingErr = std::abs(output - actual);
        bool convergedIntoWindow = (prevTrackingErr <= trackingWindow + 1e-6);
        for (int i = 0; i < 4000; ++i) {
            output = recoveryFilter.Filter(newTarget, actual, 0.0);
            const double err = output - actual;
            const double step = std::clamp(err, -servoMaxStep, servoMaxStep);
            actual += step;
            if (recoveryFilter.GetTrackingClampDirection() == 0) {
                clampReleased = true;
            }
            if (actual <= startActual - 5.0) {
                actualMovedDownward = true;
            }
            if (output <= startOutput - 5.0) {
                outputMovedDownward = true;
            }
            const double trackingErr = std::abs(output - actual);
            // Tracking error may shrink each cycle, or stay essentially the
            // same once already inside the window; a small numerical slack
            // covers floating-point noise in the convergence ramp.
            if (!Check(trackingErr <= prevTrackingErr + 1e-6 ||
                       trackingErr <= trackingWindow + 1e-6,
                       "filter output tracking error must monotonically approach the trackingErrorWindow during recovery")) {
                std::cerr << "i=" << i << " output=" << output << " actual=" << actual
                          << " trackingErr=" << trackingErr
                          << " prevTrackingErr=" << prevTrackingErr
                          << " window=" << trackingWindow << '\n';
                return EXIT_FAILURE;
            }
            prevTrackingErr = trackingErr;
            if (trackingErr <= trackingWindow + 1e-6) {
                convergedIntoWindow = true;
            }
        }
        if (!Check(convergedIntoWindow,
                   "filter output must converge inside iActValue ± trackingErrorWindow during recovery")) {
            std::cerr << "finalOutput=" << output << " finalActual=" << actual
                      << " window=" << trackingWindow << '\n';
            return EXIT_FAILURE;
        }
        if (!Check(clampReleased,
                   "tracking-window clamp must release once filter output moves off the boundary") ||
            !Check(outputMovedDownward,
                   "filter output must move at least 5 deg toward the negative target during recovery") ||
            !Check(actualMovedDownward,
                   "actual position must follow the filter output downward during recovery "
                   "(if frozen, the field symptom 'cannot continue tracking' reproduces)")) {
            std::cerr << "clampReleased=" << clampReleased
                      << " outputMovedDownward=" << outputMovedDownward
                      << " actualMovedDownward=" << actualMovedDownward
                      << " startActual=" << startActual
                      << " finalActual=" << actual
                      << " startOutput=" << startOutput
                      << " finalOutput=" << output << '\n';
            return EXIT_FAILURE;
        }
    }

    // Regression for the field chart: if the actual feedback has already
    // been pulled outward by CSV velocity/inertia while the upstream input is
    // back inside/lower, the actual-centered tracking window must not drag
    // Filter output farther away from the input. It may decelerate old
    // velocity, but it must not accept the sliding window as a new outward
    // trajectory target.
    {
        PDOFilter slidingWindowFilter;
        slidingWindowFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
        slidingWindowFilter.SetPDOLimits(-1000.0, 1000.0);
        slidingWindowFilter.SetViolationCallback(
            [](PDOFilter::ViolationType, double, double, bool, double) {});
        double output = slidingWindowFilter.Filter(35.0, 35.0, std::numeric_limits<double>::quiet_NaN());
        slidingWindowFilter.CompleteSoftStart();

        const double startOutput = output;
        double actual = 55.0;
        const double input = 20.0;
        bool trackingClampSeen = false;
        double maxOutput = output;
        double maxInternalVelocity = slidingWindowFilter.GetInternalVelocity();
        for (int i = 0; i < 600; ++i) {
            const double previousOutput = output;
            output = slidingWindowFilter.Filter(input, actual, 0.0);
            maxOutput = std::max(maxOutput, output);
            maxInternalVelocity = std::max(maxInternalVelocity, slidingWindowFilter.GetInternalVelocity());
            if (slidingWindowFilter.GetTrackingClampDirection() < 0) {
                trackingClampSeen = true;
            }
            if (!Check(std::isfinite(output), "sliding-window guard output must stay finite") ||
                !Check(output <= previousOutput + 0.101,
                       "sliding tracking window must not pull Filter output upward away from a lower input")) {
                std::cerr << "i=" << i
                          << " input=" << input
                          << " actual=" << actual
                          << " previousOutput=" << previousOutput
                          << " output=" << output
                          << " internalVelocity=" << slidingWindowFilter.GetInternalVelocity()
                          << " clampDir=" << slidingWindowFilter.GetTrackingClampDirection() << '\n';
                return EXIT_FAILURE;
            }
            actual += 0.04;
        }

         if (!Check(maxOutput <= startOutput + 0.6,
                   "Filter output must not ride a sliding actual-position window toward the limit") ||
                     !Check(maxInternalVelocity <= 0.25,
                     "Filter internal velocity must not feed outward while actual drags the tracking window away from input")) {
            std::cerr << "trackingClampSeen=" << trackingClampSeen
                      << " startOutput=" << startOutput
                      << " maxOutput=" << maxOutput
                      << " finalOutput=" << output
                     << " maxInternalVelocity=" << maxInternalVelocity
                      << " finalInternalVelocity=" << slidingWindowFilter.GetInternalVelocity() << '\n';
            return EXIT_FAILURE;
        }
    }

    // Regression for physical EtherCAT drives with poor position following:
    // if actual feedback oscillates outside the tracking window while the
    // operator/MMF input is held constant, the safety clamp may move the
    // logical filter position to keep |output - actual| bounded. That
    // actual-driven clamp movement must NOT be republished as PDOFilter
    // internal velocity, because CSV uses GetInternalVelocity() as feedforward
    // and would otherwise inject the feedback oscillation back into 0x60FF.
    {
        PDOFilter laggedActualFilter;
        laggedActualFilter.Configure(80.0, 100.0, 100.0, 2500.0, 1000);
        laggedActualFilter.SetPDOLimits(-1000.0, 1000.0);
        laggedActualFilter.SetViolationCallback(
            [](PDOFilter::ViolationType, double, double, bool, double) {});

        constexpr double heldInput = -16.0;
        double output = laggedActualFilter.Filter(heldInput, heldInput, std::numeric_limits<double>::quiet_NaN());
        laggedActualFilter.CompleteSoftStart();

        const double trackingWindow = laggedActualFilter.GetTrackingErrorLimit();
        double maxAbsInternalVelocity = 0.0;
        double maxAbsOutputVelocity = 0.0;
        bool clampSeen = false;
        for (int i = 0; i < 1200; ++i) {
            const double t = static_cast<double>(i) * 0.001;
            const double amplitude = 0.75 + 3.0 * (static_cast<double>(i) / 1199.0);
            constexpr double oscillationHz = 6.0;
            const double actual = heldInput - trackingWindow - 0.8 +
                                  amplitude * std::sin(2.0 * 3.14159265358979323846 * oscillationHz * t);
            const double actualVelocity = amplitude * 2.0 * 3.14159265358979323846 * oscillationHz *
                                          std::cos(2.0 * 3.14159265358979323846 * oscillationHz * t);
            output = laggedActualFilter.Filter(heldInput, actual, actualVelocity);
            if (laggedActualFilter.GetTrackingClampDirection() != 0) {
                clampSeen = true;
            }
            maxAbsInternalVelocity = std::max(maxAbsInternalVelocity,
                                              std::abs(laggedActualFilter.GetInternalVelocity()));
            maxAbsOutputVelocity = std::max(maxAbsOutputVelocity,
                                            std::abs(laggedActualFilter.GetCurrentVelocity()));
            if (!Check(std::isfinite(output), "lagged-actual output must stay finite")) {
                return EXIT_FAILURE;
            }
        }

        if (!Check(clampSeen,
                   "lagged-actual regression should exercise the tracking clamp") ||
            !Check(maxAbsInternalVelocity <= 8.0,
                   "actual-driven tracking-window motion must not become CSV feedforward velocity")) {
            std::cerr << "clampSeen=" << clampSeen
                      << " maxAbsOutputVelocity=" << maxAbsOutputVelocity
                      << " maxAbsInternalVelocity=" << maxAbsInternalVelocity
                      << " finalOutput=" << output
                      << " finalInternalVelocity=" << laggedActualFilter.GetInternalVelocity() << '\n';
            return EXIT_FAILURE;
        }
    }

    // Regression for feedback leading command during deceleration (Feedback Lead Ignore-Clamp):
    // during a deceleration process, actual feedback position may lead/overshoot the command because of
    // physical inertia. The tracking clamp must not force the command to push outward to chase that overshoot,
    // which would cause positive feedback runaway speed.
    {
        PDOFilter decelerationLeadFilter;
        decelerationLeadFilter.Configure(100.0, 200.0, 200.0, 1000.0, 1000);
        decelerationLeadFilter.SetPDOLimits(-1000.0, 1000.0);
        decelerationLeadFilter.SetViolationCallback(
            [](PDOFilter::ViolationType, double, double, bool, double) {});

        // Start at 0, complete soft-start
        double output = decelerationLeadFilter.Filter(0.0, 0.0, 0.0);
        decelerationLeadFilter.CompleteSoftStart();

        // 1. Move toward a fixed positive target until the filter is near the command target.
        const double input = 5.0;
        double actual = 0.0;
        bool nearTarget = false;
        for (int i = 0; i < 1600; ++i) {
            actual = output;
            output = decelerationLeadFilter.Filter(input, actual, 0.0);
            if (output >= input - 0.4) {
                nearTarget = true;
                break;
            }
        }

        if (!Check(nearTarget,
                   "feedback-lead regression should bring the filter near the fixed command target before forcing overshoot")) {
            std::cerr << "input=" << input
                      << " output=" << output
                      << " intVel=" << decelerationLeadFilter.GetInternalVelocity() << '\n';
            return EXIT_FAILURE;
        }

        // 2. Continuous deceleration: input target remains constant, but actual feedback leads/overshoots the command target.
        double maxOutputAfterLead = output;
        double maxOvershootAfterLead = 0.0;
        for (int i = 0; i < 160; ++i) {
            actual = input + 0.3;
            double prevOutput = output;
            output = decelerationLeadFilter.Filter(input, actual, 0.0);
            maxOutputAfterLead = std::max(maxOutputAfterLead, output);
            maxOvershootAfterLead = std::max(maxOvershootAfterLead, output - input);

            if (!Check(output <= prevOutput + 0.02,
                       "filter output must not be pulled outward after actual feedback overshoots the fixed command target")) {
                std::cerr << "i=" << i
                          << " input=" << input
                          << " actual=" << actual
                          << " prevOutput=" << prevOutput
                          << " output=" << output
                          << " intVel=" << decelerationLeadFilter.GetInternalVelocity() << '\n';
                return EXIT_FAILURE;
            }
        }

        if (!Check(maxOutputAfterLead <= input + 0.30,
                 "feedback-lead regression must keep command overshoot bounded near the fixed target") ||
             !Check(decelerationLeadFilter.GetInternalVelocity() <= 1.0,
                   "command velocity must not be pushed outward to chase leading feedback")) {
            std::cerr << "input=" << input
                      << " maxOutputAfterLead=" << maxOutputAfterLead
                 << " maxOvershootAfterLead=" << maxOvershootAfterLead
                      << " finalOutput=" << output
                      << " finalInternalVelocity=" << decelerationLeadFilter.GetInternalVelocity() << '\n';
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}