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

bool CheckEnvelope(double output,
                   double previousOutput,
                   double previousVelocity,
                   double maxVelocity,
                   double maxAcceleration,
                   double cycleTime,
                   double pdoMin,
                   double pdoMax,
                   const char* context) {
    double vLow = previousVelocity - maxAcceleration * cycleTime;
    double vHigh = previousVelocity + maxAcceleration * cycleTime;
    vLow = std::max(vLow, -std::abs(maxVelocity));
    vHigh = std::min(vHigh, std::abs(maxVelocity));

    const double pLow = std::max(previousOutput + vLow * cycleTime, pdoMin);
    const double pHigh = std::min(previousOutput + vHigh * cycleTime, pdoMax);
    const bool ok = (pLow <= pHigh)
        ? (output >= pLow - 1e-6 && output <= pHigh + 1e-6)
        : (output >= pdoMin - 1e-6 && output <= pdoMax + 1e-6);
    if (ok) {
        return true;
    }

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6)
        << context
        << " prevOut=" << previousOutput
        << " prevVel=" << previousVelocity
        << " output=" << output
        << " allowed=[" << pLow << ", " << pHigh << "]";
    return Check(false, oss.str());
}

} // namespace

int main() {
    constexpr double kPdoMin = -65.0;
    constexpr double kPdoMax = 65.0;
    constexpr double kProtectedPdoMin = kPdoMin + 2.0;
    constexpr double kProtectedPdoMax = kPdoMax - 2.0;
    constexpr double kCycleSec = 0.001;
    constexpr double kMaxVelocity = 100.0;
    constexpr double kMaxAcceleration = 200.0;

    PDOFilter filter;
    filter.Configure(kMaxVelocity, kMaxAcceleration, kMaxAcceleration, 1000.0, 1000);
    filter.SetPDOLimits(kPdoMin, kPdoMax);
    filter.SetViolationCallback([](PDOFilter::ViolationType, double, double, bool, double) {});

    double actual = 30.0;
    double output = filter.Filter(30.0, actual, std::numeric_limits<double>::quiet_NaN());
    filter.CompleteSoftStart();

    if (!Check(std::abs(output - actual) < 1e-9, "filter should initialize at the actual position")) {
        return EXIT_FAILURE;
    }

    // Build a realistic negative command velocity while the axis is still
    // inside the PDO envelope. This is the precondition for the field trace:
    // the filter is already moving down when the violation/fault condition
    // exposes a feedback position outside the hard lower limit.
    for (int i = 0; i < 900; ++i) {
        output = filter.Filter(-120.0, actual, 0.0);
        actual += std::clamp(output - actual, -kMaxVelocity * kCycleSec, kMaxVelocity * kCycleSec);
    }

    if (!Check(output > kPdoMin + 1.0 && output < kPdoMax,
               "setup should still be inside the PDO envelope before the feedback fault") ||
        !Check(filter.GetInternalVelocity() < -1.0,
               "setup should build a negative internal velocity before the feedback fault")) {
        std::cerr << "setup output=" << output
                  << " internalVelocity=" << filter.GetInternalVelocity()
                  << " actual=" << actual << '\n';
        return EXIT_FAILURE;
    }

    // Feedback now reports a position beyond the lower hard limit, while the
    // upstream command also asks for more negative motion. The smoke contract
    // is the operator requirement: no one-cycle position jump, no output past
    // the hard limit, and no retained outward feedforward once the hard edge
    // is reached.
    const double faultedActual = -95.0;
    bool reachedLowerEdge = false;
    double maxStep = 0.0;
    double minOutput = output;
    for (int i = 0; i < 8000; ++i) {
        const double previousOutput = output;
        const double previousOutputVelocity = filter.GetCurrentVelocity();
        output = filter.Filter(-120.0, faultedActual, 0.0);
        maxStep = std::max(maxStep, std::abs(output - previousOutput));
        minOutput = std::min(minOutput, output);

        if (!Check(std::isfinite(output), "faulted-lower-limit output must stay finite") ||
            !Check(output >= kPdoMin - 1e-6 && output <= kPdoMax + 1e-6,
                   "faulted-lower-limit output must never exceed the PDO hard limits") ||
            !CheckEnvelope(output,
                           previousOutput,
                           previousOutputVelocity,
                           kMaxVelocity,
                           kMaxAcceleration,
                           kCycleSec,
                           kProtectedPdoMin,
                           kProtectedPdoMax,
                           "faulted-lower-limit output must not jump outside the acceleration envelope")) {
            return EXIT_FAILURE;
        }

        if (output <= kProtectedPdoMin + 1e-6) {
            reachedLowerEdge = true;
            if (!Check(filter.GetInternalVelocity() >= -1e-6,
                       "internal feedforward must not keep driving outward after the lower protected edge is reached")) {
                std::cerr << "cycle=" << i
                          << " output=" << output
                          << " internalVelocity=" << filter.GetInternalVelocity()
                          << " currentVelocity=" << filter.GetCurrentVelocity() << '\n';
                return EXIT_FAILURE;
            }
        }
    }

    if (!Check(reachedLowerEdge, "faulted-lower-limit case should converge to the clamped lower protected target") ||
        !Check(minOutput >= kProtectedPdoMin - 1e-6, "minimum output should remain inside the lower protected PDO edge")) {
        std::cerr << "reachedLowerEdge=" << reachedLowerEdge
                  << " minOutput=" << minOutput
                  << " maxStep=" << maxStep
                  << " finalInternalVelocity=" << filter.GetInternalVelocity() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[violation-no-runaway] minOutput=" << minOutput
              << " maxStep=" << maxStep
              << " finalInternalVelocity=" << filter.GetInternalVelocity() << '\n';
    return EXIT_SUCCESS;
}