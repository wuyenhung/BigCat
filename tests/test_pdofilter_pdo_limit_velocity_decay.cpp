// Regression: when the filter command rams into the configured PDO position
// limit, the internal velocity (used as feedforward by CSV velocity command)
// must decay to zero so the slave does not retain a residual outward velocity
// (the "won't stop at limit" hazard the operator is concerned about).
//
// The test runs a deterministic closed-loop simulation (actual = previous
// filter output) with an aggressive constant-position target far beyond the
// upper PDO limit, then verifies:
//   1. Filter output saturates at the configured limit (no overshoot beyond
//      epsilon).
//   2. Once the command reaches a hard edge, GetInternalVelocity() must not
//      retain an outward feedforward component.
//   3. Away from the hard edge, velocity remains continuous
//      (|Δv| ≤ 1.5 · maxDecel · dt per cycle).
//   4. Symmetric behavior for the lower PDO limit.

#include "PDOFilter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>

namespace {

bool Check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

struct Result {
    bool ok = true;
    double maxOutput = 0.0;
    double minOutput = 0.0;
    double finalInternalVelocity = 0.0;
    double maxInternalVelocityStep = 0.0;
    int saturationCycle = -1;
};

enum class ActualMode {
    ClosedLoop,    // actual follows the filter output (ideal servo)
    StuckPastLimit // actual is pinned beyond the PDO limit (e.g. manually
                   // pushed, or limits were tightened mid-flight)
};

Result RunRamIntoLimit(double pdoMin, double pdoMax, double constantTarget,
                       int totalCycles, ActualMode mode,
                       double stuckActual = 0.0) {
    Result r{};
    PDOFilter filter;
    const double maxVel = 100.0;
    const double maxAcc = 200.0;
    const double maxDec = 200.0;
    const double jerk  = 1000.0;
    const int cycleUs  = 1000;
    filter.Configure(maxVel, maxAcc, maxDec, jerk, cycleUs);
    filter.SetPDOLimits(pdoMin, pdoMax);

    // Initialize and skip soft-start so the test focuses on the steady-state
    // PDO-limit braking behavior. Soft-start is exercised by other tests.
    const double initialActual =
        (mode == ActualMode::StuckPastLimit) ? stuckActual : 0.0;
    filter.Filter(initialActual, initialActual,
                  std::numeric_limits<double>::quiet_NaN());
    filter.CompleteSoftStart();

    const double dt = cycleUs / 1000000.0;
    const double maxVelStep = 1.5 * maxDec * dt; // continuity tolerance
    constexpr double kProtectedLimitMargin = 2.0;
    const double saturationEdge =
        (constantTarget > 0.0) ? (pdoMax - kProtectedLimitMargin) : (pdoMin + kProtectedLimitMargin);

    double actual = initialActual;
    double prevInternalVelocity = filter.GetInternalVelocity();
    // Initialize tracked extrema from the first in-loop output, not the
    // pre-loop init snapshot (which may temporarily reflect a soft-start
    // limit-adjust state). The cyclic thread only writes PDO output after
    // each Filter() call inside the control loop, so the in-loop values are
    // what the slave actually sees.
    bool extremaInitialized = false;

    for (int cycle = 1; cycle <= totalCycles; ++cycle) {
        const double output = filter.Filter(constantTarget, actual, 0.0);
        if (mode == ActualMode::ClosedLoop) {
            actual = output; // closed-loop: servo perfectly tracks the command
        }
        // StuckPastLimit: actual stays pinned at stuckActual (set above)

        const double internalVel = filter.GetInternalVelocity();
        const double velStep = std::abs(internalVel - prevInternalVelocity);

        if (!extremaInitialized) {
            r.maxOutput = output;
            r.minOutput = output;
            extremaInitialized = true;
        } else {
            r.maxOutput = std::max(r.maxOutput, output);
            r.minOutput = std::min(r.minOutput, output);
        }
        r.maxInternalVelocityStep = std::max(r.maxInternalVelocityStep, velStep);

        if (r.saturationCycle < 0) {
            const bool atLimit = (constantTarget > 0.0)
                ? (output >= saturationEdge - 1e-3)
                : (output <= saturationEdge + 1e-3);
            if (atLimit) r.saturationCycle = cycle;
        }

        const bool atUpperLimit = (constantTarget > 0.0) && (output >= saturationEdge - 1e-6);
        const bool atLowerLimit = (constantTarget < 0.0) && (output <= saturationEdge + 1e-6);
        const bool clearedOutwardFeedforwardAtLimit =
            (atUpperLimit && prevInternalVelocity > 0.0 && std::abs(internalVel) <= 1e-6) ||
            (atLowerLimit && prevInternalVelocity < 0.0 && std::abs(internalVel) <= 1e-6);

        if (atUpperLimit && internalVel > 1e-6) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6)
                << "internal velocity must not keep feeding outward at upper protected edge"
                << " cycle=" << cycle
                << " output=" << output
                << " internalVel=" << internalVel;
            r.ok = Check(false, oss.str()) && r.ok;
        }
        if (atLowerLimit && internalVel < -1e-6) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6)
                << "internal velocity must not keep feeding outward at lower protected edge"
                << " cycle=" << cycle
                << " output=" << output
                << " internalVel=" << internalVel;
            r.ok = Check(false, oss.str()) && r.ok;
        }

        if (!clearedOutwardFeedforwardAtLimit && velStep > maxVelStep + 1e-6) {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(6)
                << "internal velocity step exceeded max-decel limit"
                << " cycle=" << cycle
                << " prevV=" << prevInternalVelocity
                << " curV=" << internalVel
                << " step=" << velStep
                << " allowed=" << maxVelStep;
            r.ok = Check(false, oss.str()) && r.ok;
        }

        prevInternalVelocity = internalVel;
        r.finalInternalVelocity = internalVel;
    }

    return r;
}

}  // namespace

int main() {
    bool ok = true;

    // ---- Case A: input rams into UPPER PDO limit (closed loop) ----
    {
        const double pdoMin = -50.0;
        const double pdoMax = 50.0;
        const double target = 200.0; // far beyond +pdoMax
        const Result r = RunRamIntoLimit(pdoMin, pdoMax, target, 5000,
                                         ActualMode::ClosedLoop);

        ok = Check(r.ok, "upper-limit closed-loop reported a velocity discontinuity") && ok;
        ok = Check(r.maxOutput <= pdoMax + 1e-6,
                   "filter output must not exceed upper PDO limit (closed loop)") && ok;
        ok = Check(r.saturationCycle > 0,
                   "filter output should saturate at upper PDO limit within run (closed loop)") && ok;
        ok = Check(std::abs(r.finalInternalVelocity) <= 0.5,
                   "internal velocity must decay near zero once pinned at upper limit "
                   "(closed loop, else CSV will retain residual outward velocity)") && ok;

        std::cout << "[upper-closed] saturationCycle=" << r.saturationCycle
                  << " maxOutput=" << r.maxOutput
                  << " finalInternalVel=" << r.finalInternalVelocity
                  << " maxStep=" << r.maxInternalVelocityStep << '\n';
    }

    // ---- Case B: input rams into LOWER PDO limit (closed loop) ----
    {
        const double pdoMin = -50.0;
        const double pdoMax = 50.0;
        const double target = -200.0; // far beyond -pdoMin
        const Result r = RunRamIntoLimit(pdoMin, pdoMax, target, 5000,
                                         ActualMode::ClosedLoop);

        ok = Check(r.ok, "lower-limit closed-loop reported a velocity discontinuity") && ok;
        ok = Check(r.minOutput >= pdoMin - 1e-6,
                   "filter output must not exceed lower PDO limit (closed loop)") && ok;
        ok = Check(r.saturationCycle > 0,
                   "filter output should saturate at lower PDO limit within run (closed loop)") && ok;
        ok = Check(std::abs(r.finalInternalVelocity) <= 0.5,
                   "internal velocity must decay near zero once pinned at lower limit "
                   "(closed loop, else CSV will retain residual outward velocity)") && ok;

        std::cout << "[lower-closed] saturationCycle=" << r.saturationCycle
                  << " minOutput=" << r.minOutput
                  << " finalInternalVel=" << r.finalInternalVelocity
                  << " maxStep=" << r.maxInternalVelocityStep << '\n';
    }

    // ---- Case C: actual stuck PAST upper PDO limit (servo over-pushed or
    //              limits tightened mid-flight). The filter must NOT keep
    //              feeding outward velocity to CSV in this state. ----
    {
        const double pdoMin = -50.0;
        const double pdoMax = 50.0;
        const double target = 200.0;
        const double stuckActual = 100.0; // beyond +pdoMax
        const Result r = RunRamIntoLimit(pdoMin, pdoMax, target, 5000,
                                         ActualMode::StuckPastLimit, stuckActual);

        ok = Check(r.ok, "upper-limit stuck-actual reported a velocity discontinuity") && ok;
        ok = Check(r.maxOutput <= pdoMax + 1e-6,
                   "filter output must not exceed upper PDO limit even when actual is past it") && ok;
        ok = Check(std::abs(r.finalInternalVelocity) <= 0.5,
                   "internal velocity must decay near zero when output is pinned at upper limit "
                   "with actual already beyond it (else CSV writes residual outward velocity that "
                   "drives the motor further past the limit)") && ok;

        std::cout << "[upper-stuck] saturationCycle=" << r.saturationCycle
                  << " maxOutput=" << r.maxOutput
                  << " finalInternalVel=" << r.finalInternalVelocity
                  << " maxStep=" << r.maxInternalVelocityStep << '\n';
    }

    // ---- Case D: symmetric stuck-past-lower-limit ----
    {
        const double pdoMin = -50.0;
        const double pdoMax = 50.0;
        const double target = -200.0;
        const double stuckActual = -100.0; // beyond -pdoMin
        const Result r = RunRamIntoLimit(pdoMin, pdoMax, target, 5000,
                                         ActualMode::StuckPastLimit, stuckActual);

        ok = Check(r.ok, "lower-limit stuck-actual reported a velocity discontinuity") && ok;
        ok = Check(r.minOutput >= pdoMin - 1e-6,
                   "filter output must not exceed lower PDO limit even when actual is past it") && ok;
        ok = Check(std::abs(r.finalInternalVelocity) <= 0.5,
                   "internal velocity must decay near zero when output is pinned at lower limit "
                   "with actual already beyond it") && ok;

        std::cout << "[lower-stuck] saturationCycle=" << r.saturationCycle
                  << " minOutput=" << r.minOutput
                  << " finalInternalVel=" << r.finalInternalVelocity
                  << " maxStep=" << r.maxInternalVelocityStep << '\n';
    }

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
