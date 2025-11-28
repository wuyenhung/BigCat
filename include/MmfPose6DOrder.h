#pragma once

#include "StewartIK.h"

#include <array>

namespace WuSOEM {

namespace MmfPose6DOrder {

constexpr std::array<const char*, 6> kInputNames{{
    "Sway",
    "Surge",
    "Heave",
    "Pitch",
    "Roll",
    "FYaw",
}};

// MMF1 Pose6D public input order:
//   1=Sway, 2=Surge, 3=Heave, 4=Pitch, 5=Roll, 6=FYaw.
// These values are mapped into the existing Pose6D/UI fields so they match the
// physical machine behavior validated in the field:
//   Sway  -> pose.x   (UI: 左右移動)
//   Surge -> pose.y   (UI: 前後移動)
//   Pitch -> pose.roll  (UI field label: 前後傾斜角)
//   Roll  -> pose.pitch (UI field label: 左右傾斜角)
inline Pose6D BuildPoseFromMmfInput(const std::array<float, 6>& values) {
    Pose6D pose;
    pose.x = static_cast<long double>(values[0]);
    pose.y = static_cast<long double>(values[1]);
    pose.z = static_cast<long double>(values[2]);
    pose.roll = StewartIK::NormalizeAngleDeg(static_cast<long double>(values[3]));
    pose.pitch = StewartIK::NormalizeAngleDeg(static_cast<long double>(values[4]));
    pose.yaw = StewartIK::NormalizeAngleDeg(static_cast<long double>(values[5]));
    return pose;
}

}  // namespace MmfPose6DOrder

}  // namespace WuSOEM