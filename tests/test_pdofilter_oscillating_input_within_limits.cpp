// Regression: when MMF input oscillates with large amplitude but stays within
// PDO position limits, the filter output must (a) never exceed the configured
// PDO position limits and (b) not exhibit growing-amplitude resonance that
// produces large excursions ("跳位置") well beyond the input envelope.

#define _USE_MATH_DEFINES
#include "PDOFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

namespace {

bool Check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
}

struct RunStats {
    double maxOutput = -std::numeric_limits<double>::infinity();
    double minOutput = std::numeric_limits<double>::infinity();
    double maxInput = -std::numeric_limits<double>::infinity();
    double minInput = std::numeric_limits<double>::infinity();
    double maxAbsCycleStep = 0.0;
    int    samples = 0;
};

// Drive the filter with an oscillating MMF input. The actual feedback follows
// the previous filter output through a first-order lag (servo time constant
// `tauSec`). Real CSV servos do not perfectly match the command; the lag
// expands the (iActValue ± trackingErrorWindow) clamp window enough to let
// the filter integrate beyond a static safety bound, which is the regime
// where the resonance / over-limit symptoms have been reported on hardware.
RunStats RunOscillation(PDOFilter& filter,
                        double inputAmplitudeDeg,
                        double inputCenterDeg,
                        double inputFrequencyHz,
                        double durationSec,
                        double cycleTimeSec,
                        double tauSec,
                        bool   squareWave = false) {
    RunStats stats;
    double act = inputCenterDeg;
    double prevAct = inputCenterDeg;
    double prevOut = inputCenterDeg;
    bool prevOutValid = false;
    const int totalCycles = static_cast<int>(std::round(durationSec / cycleTimeSec));
    const double alpha = (tauSec > 0.0)
        ? (cycleTimeSec / (tauSec + cycleTimeSec))
        : 1.0;
    for (int i = 0; i < totalCycles; ++i) {
        double t = i * cycleTimeSec;
        double phase = std::sin(2.0 * M_PI * inputFrequencyHz * t);
        if (squareWave) {
            phase = (phase >= 0.0) ? 1.0 : -1.0;
        }
        double mmf = inputCenterDeg + inputAmplitudeDeg * phase;
        double measuredVel = (act - prevAct) / std::max(cycleTimeSec, 1e-9);
        double out = filter.Filter(mmf, act, measuredVel);
        // First-order servo: act drifts toward last command with time constant tau.
        prevAct = act;
        act = act + (out - act) * alpha;

        stats.maxOutput = std::max(stats.maxOutput, out);
        stats.minOutput = std::min(stats.minOutput, out);
        stats.maxInput = std::max(stats.maxInput, mmf);
        stats.minInput = std::min(stats.minInput, mmf);
        if (prevOutValid) {
            stats.maxAbsCycleStep = std::max(stats.maxAbsCycleStep,
                                             std::abs(out - prevOut));
        }
        prevOut = out;
        prevOutValid = true;
        ++stats.samples;
    }
    return stats;
}

}  // namespace

int main() {
    constexpr double kCycle = 0.002;            // 2 ms (500 Hz)
    constexpr double kPdoMin = -60.0;
    constexpr double kPdoMax = 60.0;
    constexpr double kDuration = 12.0;

    bool ok = true;

    struct Scenario {
        const char* name;
        double freq;
        double tau;
        bool   square;
        double amplitude;
        double center;
    };
    const Scenario scenarios[] = {
        // Centered oscillation
        {"sine-perfect",     0.5, 0.0,   false, 50.0, 0.0},
        {"sine-tight",       0.5, 0.005, false, 50.0, 0.0},
        {"sine-loose",       0.5, 0.05,  false, 50.0, 0.0},
        {"sine-medium",      1.0, 0.05,  false, 50.0, 0.0},
        {"sine-fast",        2.0, 0.05,  false, 50.0, 0.0},
        {"square-slow",      0.5, 0.05,  true,  50.0, 0.0},
        {"square-medium",    1.0, 0.05,  true,  50.0, 0.0},
        {"square-perfect",   0.5, 0.0,   true,  50.0, 0.0},
        // Off-centre oscillation that approaches the upper PDO limit
        {"sine-near-max",    0.5, 0.05,  false, 25.0, 30.0},
        {"sine-touching-max",0.5, 0.05,  false, 30.0, 30.0},
        {"sine-near-min",    0.5, 0.05,  false, 25.0, -30.0},
    };

    for (const auto& sc : scenarios) {
        PDOFilter filter;
        filter.Configure(100.0, 200.0, 200.0, 1000.0, 2000);
        filter.SetPDOLimits(kPdoMin, kPdoMax);
        filter.TriggerSoftStart();
        for (int i = 0; i < 200; ++i) filter.Filter(sc.center, sc.center, 0.0);
        filter.CompleteSoftStart();

        RunStats stats = RunOscillation(filter,
                                        sc.amplitude,
                                        sc.center,
                                        sc.freq,
                                        kDuration,
                                        kCycle,
                                        sc.tau,
                                        sc.square);

        std::cout << "[" << sc.name << "]"
                  << " amp=" << sc.amplitude << " ctr=" << sc.center
                  << " freq=" << sc.freq << "Hz tau=" << sc.tau << "s sq=" << sc.square
                  << " in=[" << stats.minInput << "," << stats.maxInput << "]"
                  << " out=[" << stats.minOutput << "," << stats.maxOutput << "]"
                  << " maxStep=" << stats.maxAbsCycleStep << "\n";

        const double absInputMax = std::abs(sc.center) + std::abs(sc.amplitude);

        // The hard-limit clamp is non-negotiable.
        ok &= Check(stats.maxOutput <= kPdoMax + 1e-6,
                    "filter output exceeded configured iMaxPDO_");
        ok &= Check(stats.minOutput >= kPdoMin - 1e-6,
                    "filter output exceeded configured iMinPDO_");

        // Resonance check: the smoothing filter must not amplify the input
        // amplitude. Allow a moderate overshoot tolerance for transient
        // response and tracking lag at higher frequencies.
        constexpr double kOvershootToleranceDeg = 8.0;
        ok &= Check(stats.maxOutput <= absInputMax + kOvershootToleranceDeg,
                    "filter output peak exceeded input amplitude (resonance)");
        ok &= Check(stats.minOutput >= -absInputMax - kOvershootToleranceDeg,
                    "filter output trough exceeded input amplitude (resonance)");

        // Per-cycle position step must respect velocity limit (with a 2.5x
        // safety factor for the overshoot-braking 2-cycle recompute path).
        const double maxAllowedStep = 100.0 * kCycle * 2.5;
        ok &= Check(stats.maxAbsCycleStep <= maxAllowedStep,
                    "filter produced a per-cycle step beyond velocity limit (jump)");
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
