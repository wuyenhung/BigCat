#include "MmfPose6DOrder.h"

#include <cstdlib>
#include <iostream>

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    const std::array<float, 6> mmfValues{{10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f}};
    const WuSOEM::Pose6D pose = WuSOEM::MmfPose6DOrder::BuildPoseFromMmfInput(mmfValues);

    if (!Check(pose.x == 10.0L, "MMF Pose6D value 1 should map to sway/X") ||
        !Check(pose.y == 20.0L, "MMF Pose6D value 2 should map to surge/Y") ||
        !Check(pose.z == 30.0L, "MMF Pose6D value 3 should map to heave/Z") ||
        !Check(pose.roll == 40.0L, "MMF Pose6D value 4 should map to the pitch UI axis") ||
        !Check(pose.pitch == 50.0L, "MMF Pose6D value 5 should map to the roll UI axis") ||
        !Check(pose.yaw == 60.0L, "MMF Pose6D value 6 should map to FYaw")) {
        return EXIT_FAILURE;
    }

    const std::array<float, 6> wrappedAngles{{0.0f, 0.0f, 0.0f, 190.0f, -190.0f, 540.0f}};
    const WuSOEM::Pose6D wrappedPose = WuSOEM::MmfPose6DOrder::BuildPoseFromMmfInput(wrappedAngles);
    if (!Check(wrappedPose.roll == -170.0L, "MMF pitch slot should normalize into pose.roll [-180, 180]") ||
        !Check(wrappedPose.pitch == 170.0L, "MMF roll slot should normalize into pose.pitch [-180, 180]") ||
        !Check(wrappedPose.yaw == 180.0L, "FYaw should be normalized into [-180, 180]")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}