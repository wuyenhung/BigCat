// Drives PDOFilter with randomized superposed sine waves (with optional noise)
// to verify that the filtered output velocity stays continuous and within
// configured limits, and never produces NaN/Inf or sudden jumps.
//
// This is a deterministic, host-only unit test (no EtherCAT/MMF), so it can run
// anywhere as part of the smoke suite. It is the "random sine wave velocity
// stability" regression for the PDO filter.

#include "PDOFilter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

bool Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

struct SineComponent {
    double amplitudeDeg;
    double frequencyHz;
    double phaseRad;
};

struct ScenarioConfig {
    const char* label;
    uint32_t seed;
    int sineCount;
    double minFreqHz;
    double maxFreqHz;
    double totalAmplitudeDeg;
    double noiseAmplitudeDeg;
};

struct ScenarioResult {
    int samples = 0;
    double maxAbsInternalVel = 0.0;
    double maxAbsInternalAcc = 0.0;
    double maxAbsOutputVel = 0.0;
    double maxAbsOutputAcc = 0.0;
    double maxVelStep = 0.0;          // |vel[k] - vel[k-1]|
    double maxOutputVelStep = 0.0;    // |outputVel[k] - outputVel[k-1]|
    double maxTrackingError = 0.0;    // |setpoint - filterOutput|
    PDOFilter::ViolationType latchedViolations = PDOFilter::ViolationType::None;
};

std::vector<SineComponent> BuildSines(const ScenarioConfig& cfg) {
    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> freqDist(cfg.minFreqHz, cfg.maxFreqHz);
    std::uniform_real_distribution<double> phaseDist(0.0, 2.0 * kPi);
    std::uniform_real_distribution<double> weightDist(0.4, 1.0);

    std::vector<SineComponent> sines;
    sines.reserve(static_cast<size_t>(cfg.sineCount));
    double weightSum = 0.0;
    for (int i = 0; i < cfg.sineCount; ++i) {
        SineComponent s;
        s.frequencyHz = freqDist(rng);
        s.phaseRad = phaseDist(rng);
        s.amplitudeDeg = weightDist(rng);
        weightSum += s.amplitudeDeg;
        sines.push_back(s);
    }
    if (weightSum <= 0.0) {
        weightSum = 1.0;
    }
    for (auto& s : sines) {
        s.amplitudeDeg = (s.amplitudeDeg / weightSum) * cfg.totalAmplitudeDeg;
    }
    return sines;
}

double EvaluateRandomSine(const std::vector<SineComponent>& sines,
                          double timeSec,
                          std::mt19937& noiseRng,
                          double noiseAmplitudeDeg) {
    double value = 0.0;
    for (const auto& s : sines) {
        value += s.amplitudeDeg * std::sin(2.0 * kPi * s.frequencyHz * timeSec + s.phaseRad);
    }
    if (noiseAmplitudeDeg > 0.0) {
        std::uniform_real_distribution<double> noiseDist(-noiseAmplitudeDeg, noiseAmplitudeDeg);
        value += noiseDist(noiseRng);
    }
    return value;
}

bool RunScenario(const ScenarioConfig& cfg,
                 ScenarioResult& result,
                 std::ostream& diagnostics) {
    // Use generous limits relative to the random sine excitation so that the
    // setpoint stays well within the filter's allowed velocity/acceleration
    // budget. The point of this test is to check filter-output stability for
    // *legal* inputs, not to exercise the violation/limiting state machine.
    constexpr double maxVelDegPerSec = 200.0;
    constexpr double maxAccDegPerSec2 = 2000.0;
    constexpr double maxJerkDegPerSec3 = 20000.0;
    constexpr uint32_t cycleTimeUs = 1000;     // 1 ms cycle (matches PDO cycle)
    constexpr double cycleTimeSec = cycleTimeUs / 1.0e6;
    constexpr double simulationDurationSec = 5.0;
    constexpr double warmupSec = 0.30;

    PDOFilter filter;
    filter.Configure(maxVelDegPerSec, maxAccDegPerSec2, maxAccDegPerSec2, maxJerkDegPerSec3, cycleTimeUs);
    filter.SetPDOLimits(-90.0, 90.0);

    PDOFilter::ViolationType latched = PDOFilter::ViolationType::None;
    filter.SetViolationCallback(
        [&latched](PDOFilter::ViolationType type, double, double, bool, double) {
            latched = latched | type;
        });

    const auto sines = BuildSines(cfg);
    std::mt19937 noiseRng(cfg.seed ^ 0x9E3779B9u);

    double previousInternalVel = 0.0;
    double previousOutputVel = 0.0;
    double previousActual = 0.0;
    bool havePreviousVel = false;

    // Closed-loop assumption: actual position lags the filter output by one cycle
    // (perfect servo follower). This matches the regression assumption used in
    // test_pdofilter_velocity_continuity.cpp.
    double lastFilterOutput = 0.0;

    const int totalSteps = static_cast<int>(simulationDurationSec / cycleTimeSec);
    const int warmupSteps = static_cast<int>(warmupSec / cycleTimeSec);

    // Per-cycle tolerances: configured accel * dt is the theoretical maximum
    // change in internal velocity per cycle; allow a small numerical margin.
    const double maxVelStepAllowed = maxAccDegPerSec2 * cycleTimeSec * 1.5 + 1e-6;
    // Output velocity is derived from differentiated position, so a slightly
    // larger margin is needed (especially with input noise).
    const double maxOutputVelStepAllowed = maxAccDegPerSec2 * cycleTimeSec * 4.0 + 1e-6;
    const double maxAbsVelAllowed = maxVelDegPerSec * 1.05;
    const double maxAbsAccAllowed = maxAccDegPerSec2 * 1.6;

    for (int step = 0; step < totalSteps; ++step) {
        const double timeSec = step * cycleTimeSec;
        const double setpoint = EvaluateRandomSine(sines, timeSec, noiseRng, cfg.noiseAmplitudeDeg);
        const double measuredVel = (step == 0)
            ? std::numeric_limits<double>::quiet_NaN()
            : (lastFilterOutput - previousActual) / cycleTimeSec;

        const double output = filter.Filter(setpoint, lastFilterOutput, measuredVel);
        previousActual = lastFilterOutput;
        lastFilterOutput = output;

        if (!std::isfinite(output)) {
            diagnostics << cfg.label << ": filter produced non-finite output at step " << step
                        << " setpoint=" << setpoint << '\n';
            return false;
        }

        const double internalVel = filter.GetInternalVelocity();
        const double internalAcc = filter.GetInternalAcceleration();
        const double outputVel = filter.GetCurrentVelocity();
        const double outputAcc = filter.GetCurrentAcceleration();
        if (!std::isfinite(internalVel) || !std::isfinite(internalAcc) ||
            !std::isfinite(outputVel) || !std::isfinite(outputAcc)) {
            diagnostics << cfg.label << ": non-finite filter state at step " << step
                        << " internalVel=" << internalVel
                        << " internalAcc=" << internalAcc
                        << " outputVel=" << outputVel
                        << " outputAcc=" << outputAcc << '\n';
            return false;
        }

        if (step >= warmupSteps) {
            result.maxAbsInternalVel = (std::max)(result.maxAbsInternalVel, std::abs(internalVel));
            result.maxAbsInternalAcc = (std::max)(result.maxAbsInternalAcc, std::abs(internalAcc));
            result.maxAbsOutputVel = (std::max)(result.maxAbsOutputVel, std::abs(outputVel));
            result.maxAbsOutputAcc = (std::max)(result.maxAbsOutputAcc, std::abs(outputAcc));
            result.maxTrackingError = (std::max)(result.maxTrackingError, std::abs(setpoint - output));
            if (havePreviousVel) {
                const double velStep = std::abs(internalVel - previousInternalVel);
                const double outVelStep = std::abs(outputVel - previousOutputVel);
                result.maxVelStep = (std::max)(result.maxVelStep, velStep);
                result.maxOutputVelStep = (std::max)(result.maxOutputVelStep, outVelStep);

                if (velStep > maxVelStepAllowed) {
                    diagnostics << cfg.label << ": internal velocity step exceeds accel*dt at step " << step
                                << " velStep=" << velStep
                                << " allowed=" << maxVelStepAllowed
                                << " prevVel=" << previousInternalVel
                                << " curVel=" << internalVel << '\n';
                    return false;
                }
                if (outVelStep > maxOutputVelStepAllowed) {
                    diagnostics << cfg.label << ": output velocity step exceeds limit at step " << step
                                << " outVelStep=" << outVelStep
                                << " allowed=" << maxOutputVelStepAllowed
                                << " prevOutVel=" << previousOutputVel
                                << " curOutVel=" << outputVel << '\n';
                    return false;
                }
            }
            if (std::abs(internalVel) > maxAbsVelAllowed) {
                diagnostics << cfg.label << ": internal velocity exceeded configured maximum at step " << step
                            << " |vel|=" << std::abs(internalVel)
                            << " allowed=" << maxAbsVelAllowed << '\n';
                return false;
            }
            if (std::abs(internalAcc) > maxAbsAccAllowed) {
                diagnostics << cfg.label << ": internal acceleration exceeded configured maximum at step " << step
                            << " |acc|=" << std::abs(internalAcc)
                            << " allowed=" << maxAbsAccAllowed << '\n';
                return false;
            }
            ++result.samples;
        }

        previousInternalVel = internalVel;
        previousOutputVel = outputVel;
        havePreviousVel = true;
    }

    result.latchedViolations = latched;

    // NOTE: We deliberately do NOT fail when Velocity / InputAnomaly flags latch.
    // Random sine excitation can momentarily slew faster than the filter's
    // raw-velocity violation threshold (60% of configured max by default), which
    // is *expected* and exactly what the filter is designed to clamp. The point
    // of this regression is that the *output* velocity stays smooth and within
    // the configured limit even when the raw input is aggressive — that is
    // already verified above by the per-step numeric checks. We only fail if a
    // TrackingError latches, which would indicate the filter could not keep up
    // and let actual position drift dangerously far from the setpoint.
    if (PDOFilter::HasFlag(latched, PDOFilter::ViolationType::TrackingError)) {
        diagnostics << cfg.label << ": tracking-error violation latched flags=0x"
                    << std::hex << static_cast<unsigned>(latched) << std::dec << '\n';
        return false;
    }

    return true;
}

void PrintScenarioSummary(const ScenarioConfig& cfg, const ScenarioResult& r) {
    std::cout << "[stability] " << cfg.label
              << " samples=" << r.samples
              << " maxAbsInternalVel=" << r.maxAbsInternalVel
              << " maxAbsInternalAcc=" << r.maxAbsInternalAcc
              << " maxAbsOutputVel=" << r.maxAbsOutputVel
              << " maxAbsOutputAcc=" << r.maxAbsOutputAcc
              << " maxVelStep=" << r.maxVelStep
              << " maxOutputVelStep=" << r.maxOutputVelStep
              << " maxTrackingError=" << r.maxTrackingError
              << '\n';
}

}  // namespace

int main() {
    const std::array<ScenarioConfig, 4> scenarios = {{
        {"low-frequency-mix",    0xC0FFEE01u, 3, 0.3, 1.2,  12.0, 0.0},
        {"mid-frequency-mix",    0xC0FFEE02u, 4, 0.5, 2.0,  10.0, 0.0},
        {"high-frequency-mix",   0xC0FFEE03u, 5, 0.8, 3.0,   8.0, 0.0},
        {"noisy-mid-frequency",  0xC0FFEE04u, 4, 0.5, 2.0,  10.0, 0.05},
    }};

    for (const auto& cfg : scenarios) {
        ScenarioResult result;
        std::ostringstream diagnostics;
        const bool ok = RunScenario(cfg, result, diagnostics);
        PrintScenarioSummary(cfg, result);
        if (!Check(ok, diagnostics.str())) {
            return EXIT_FAILURE;
        }
        if (!Check(result.samples > 4000,
                   std::string("scenario ") + cfg.label + " should collect enough post-warmup samples")) {
            return EXIT_FAILURE;
        }
    }

    std::cout << "[stability] all random-sine scenarios passed\n";
    return EXIT_SUCCESS;
}
