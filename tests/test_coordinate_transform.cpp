#include "StewartIK.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool NearlyEqual(long double left, long double right, long double epsilon = 1e-9L) {
    return std::fabsl(left - right) <= epsilon;
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    WuSOEM::Pose6D pose;
    pose.yaw = 90.0L;

    const std::array<long double, 3> localDelta{{1.0L, 0.0L, 0.0L}};
    const auto worldDelta = WuSOEM::StewartIK::TransformPlatformToWorldDelta(localDelta, pose);
    if (!Check(NearlyEqual(worldDelta[0], 0.0L, 1e-8L), "yaw=90 should rotate local X to world Y (x component)") ||
        !Check(NearlyEqual(worldDelta[1], 1.0L, 1e-8L), "yaw=90 should rotate local X to world Y (y component)") ||
        !Check(NearlyEqual(worldDelta[2], 0.0L, 1e-8L), "yaw=90 should preserve zero z delta")) {
        return EXIT_FAILURE;
    }

    const auto recoveredLocalDelta = WuSOEM::StewartIK::TransformWorldToPlatformDelta(worldDelta, pose);
    if (!Check(NearlyEqual(recoveredLocalDelta[0], localDelta[0], 1e-8L), "inverse transform should recover local x") ||
        !Check(NearlyEqual(recoveredLocalDelta[1], localDelta[1], 1e-8L), "inverse transform should recover local y") ||
        !Check(NearlyEqual(recoveredLocalDelta[2], localDelta[2], 1e-8L), "inverse transform should recover local z")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}