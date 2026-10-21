#include "StewartIK.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool NearlyEqual(long double left, long double right, long double epsilon = 1e-9L) {
    return std::fabsl(left - right) <= epsilon;
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    using namespace WuSOEM;

    // --- Test 1: Default limits with valid pose ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(result.valid, "Test 1: Home pose should be valid") ||
            !Check(result.violationKind == Pose6DViolationKind::None,
                   "Test 1: No violation kind for valid pose")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 2: X limit violation (positive) ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 200.0L;  // exceeds maxX=150
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 2: X=200 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::X,
                   "Test 2: Violation kind should be X") ||
            !Check(NearlyEqual(result.limitValue, 150.0L),
                   "Test 2: Limit value should be 150") ||
            !Check(NearlyEqual(result.actualValue, 200.0L),
                   "Test 2: Actual value should be 200") ||
            !Check(!result.message.empty(), "Test 2: Should have error message") ||
            !Check(!result.messageW.empty(), "Test 2: Should have wide error message")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 3: X limit violation (negative) ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = -160.0L;  // exceeds maxX=150 (abs)
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 3: X=-160 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::X,
                   "Test 3: Violation kind should be X for negative")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 4: Y limit violation ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 151.0L;  // exceeds maxY=150
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 4: Y=151 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::Y,
                   "Test 4: Violation kind should be Y")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 5: Z limit violation (relative to initialHeight) ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 620.0L;  // dz = 620-450 = 170 > 150
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 5: Z=620 (dz=170) should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::Z,
                   "Test 5: Violation kind should be Z")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 6: Z at exactly the limit should be valid ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 600.0L;  // dz = 600-450 = 150 == maxZ
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(result.valid, "Test 6: Z=600 (dz=150=limit) should be valid (boundary)")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 7: Roll limit violation ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 30.0L;  // exceeds maxRoll=25
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 7: Roll=30 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::Roll,
                   "Test 7: Violation kind should be Roll")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 8: Pitch limit violation ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = -26.0L;  // exceeds maxPitch=25
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 8: Pitch=-26 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::Pitch,
                   "Test 8: Violation kind should be Pitch")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 9: Yaw limit violation ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 0.0L;
        pose.y = 0.0L;
        pose.z = 450.0L;
        pose.roll = 0.0L;
        pose.pitch = 0.0L;
        pose.yaw = 11.0L;  // exceeds maxYaw=10

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid, "Test 9: Yaw=11 should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::Yaw,
                   "Test 9: Violation kind should be Yaw")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 10: Multiple violations (first detected wins) ---
    {
        Pose6DLimits limits;
        Pose6D pose;
        pose.x = 200.0L;  // X violation
        pose.y = 200.0L;  // Y violation (but X is checked first)
        pose.z = 450.0L;
        pose.roll = 30.0L;  // Roll violation
        pose.pitch = 0.0L;
        pose.yaw = 0.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(!result.valid,
                   "Test 10: Multiple violations should be invalid") ||
            !Check(result.violationKind == Pose6DViolationKind::X,
                   "Test 10: First violation (X) should be reported")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 11: Custom limits ---
    {
        Pose6DLimits limits;
        limits.maxX = 200.0L;
        limits.maxY = 200.0L;
        limits.maxZ = 200.0L;
        limits.maxRoll = 35.0L;
        limits.maxPitch = 35.0L;
        limits.maxYaw = 20.0L;

        Pose6D pose;
        pose.x = 180.0L;
        pose.y = 180.0L;
        pose.z = 650.0L;  // dz = 200 == limit
        pose.roll = 30.0L;
        pose.pitch = -30.0L;
        pose.yaw = 15.0L;

        const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
        if (!Check(result.valid, "Test 11: Custom limits with in-range pose should be valid")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 12: Auto-recovery pattern simulation ---
    // Simulate: violation → hold → recovery
    {
        Pose6DLimits limits;
        
        // Step 1: Violation
        Pose6D badPose;
        badPose.x = 200.0L;
        badPose.y = 0.0L;
        badPose.z = 450.0L;
        badPose.roll = 0.0L;
        badPose.pitch = 0.0L;
        badPose.yaw = 0.0L;
        auto result1 = StewartIK::ValidatePose6D(badPose, 450.0L, limits);
        if (!Check(!result1.valid, "Test 12a: Violation should be detected")) {
            return EXIT_FAILURE;
        }

        // Step 2: Recovery (return to valid pose)
        Pose6D goodPose;
        goodPose.x = 100.0L;
        goodPose.y = 0.0L;
        goodPose.z = 500.0L;  // dz = 50 < 150
        goodPose.roll = 10.0L;
        goodPose.pitch = 0.0L;
        goodPose.yaw = 0.0L;
        auto result2 = StewartIK::ValidatePose6D(goodPose, 450.0L, limits);
        if (!Check(result2.valid, "Test 12b: Recovery should be detected as valid") ||
            !Check(result2.violationKind == Pose6DViolationKind::None,
                   "Test 12b: Recovery should have no violation kind")) {
            return EXIT_FAILURE;
        }
    }

    // --- Test 13: Exactly at limit ---
    {
        Pose6DLimits limits;
        for (const auto& [pose, violateKind, desc] : {
            // At limits (should be valid)
            std::make_tuple(Pose6D{150.0L, 0.0L, 450.0L, 0.0L, 0.0L, 0.0L}, false, "X at +max"),
            std::make_tuple(Pose6D{-150.0L, 0.0L, 450.0L, 0.0L, 0.0L, 0.0L}, false, "X at -max"),
            std::make_tuple(Pose6D{0.0L, 150.0L, 450.0L, 0.0L, 0.0L, 0.0L}, false, "Y at +max"),
            std::make_tuple(Pose6D{0.0L, -150.0L, 450.0L, 0.0L, 0.0L, 0.0L}, false, "Y at -max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 600.0L, 0.0L, 0.0L, 0.0L}, false, "Z at +max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 300.0L, 0.0L, 0.0L, 0.0L}, false, "Z at -max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, 25.0L, 0.0L, 0.0L}, false, "Roll at +max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, -25.0L, 0.0L, 0.0L}, false, "Roll at -max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, 0.0L, 25.0L, 0.0L}, false, "Pitch at +max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, 0.0L, -25.0L, 0.0L}, false, "Pitch at -max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, 0.0L, 0.0L, 10.0L}, false, "Yaw at +max"),
            std::make_tuple(Pose6D{0.0L, 0.0L, 450.0L, 0.0L, 0.0L, -10.0L}, false, "Yaw at -max"),
        }) {
            const auto result = StewartIK::ValidatePose6D(pose, 450.0L, limits);
            if (!Check(result.valid == !violateKind,
                       (std::string("Test 13: ") + desc + " should be " +
                        (violateKind ? "invalid" : "valid")).c_str())) {
                return EXIT_FAILURE;
            }
        }
    }

    std::cout << "All Pose6D limit validation tests passed.\n";
    return EXIT_SUCCESS;
}
