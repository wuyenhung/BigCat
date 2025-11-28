#include "StewartIK.h"

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
    WuSOEM::StewartParams params;

    const auto points = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(600.0L, 100.0L, 0.0L);

    long double sumX = 0.0L;
    long double sumY = 0.0L;
    for (const auto& point : points) {
        sumX += point[0];
        sumY += point[1];
    }

    if (!Check(NearlyEqual(sumX, 0.0L, 1e-6L), "hexagon points should be centered on X") ||
        !Check(NearlyEqual(sumY, 0.0L, 1e-6L), "hexagon points should be centered on Y")) {
        return EXIT_FAILURE;
    }

    const auto identity = WuSOEM::StewartIK::GetRotationMatrix(0.0L, 0.0L, 0.0L);
    if (!Check(NearlyEqual(identity[0][0], 1.0L), "identity rotation should preserve x axis") ||
        !Check(NearlyEqual(identity[1][1], 1.0L), "identity rotation should preserve y axis") ||
        !Check(NearlyEqual(identity[2][2], 1.0L), "identity rotation should preserve z axis") ||
        !Check(NearlyEqual(identity[0][1], 0.0L), "identity rotation should not skew x/y") ||
        !Check(NearlyEqual(identity[0][2], 0.0L), "identity rotation should not skew x/z")) {
        return EXIT_FAILURE;
    }

    if (!Check(WuSOEM::StewartIK::NormalizeAngleDeg(540.0L) == 180.0L, "540 degrees should normalize to 180") ||
        !Check(WuSOEM::StewartIK::NormalizeAngleDeg(-540.0L) == -180.0L, "-540 degrees should normalize to -180")) {
        return EXIT_FAILURE;
    }

    const long double initialHeight = WuSOEM::StewartIK::CalculateInitialHeight(params, 15.0L);
    if (!Check(initialHeight > 0.0L, "initial height should stay positive") ||
        !Check(NearlyEqual(initialHeight, 472.777156002373L, 1e-6L), "initial height should match the shared C++ geometry calculation")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}