#pragma once

#include <cstdint>

namespace CoreHelpers {

constexpr int8_t kCia402OperationModeProfileVelocity = 3;
constexpr int8_t kCia402OperationModeHoming = 6;
constexpr int8_t kCia402OperationModeCyclicSyncPosition = 8;
constexpr int8_t kCia402OperationModeCyclicSyncVelocity = 9;

// Default cyclic operation mode is CSP (Cyclic Synchronous Position).
// The existing CSV path is fully preserved and switchable via INI [Motion] OperationMode.
constexpr int8_t kDefaultCyclicOperationMode = kCia402OperationModeCyclicSyncPosition;
constexpr uint16_t kDefaultCyclicCommandPdoIndex = 0x607A;

inline constexpr bool IsVelocityDrivenOperationMode(int8_t mode)
{
    return mode == kCia402OperationModeProfileVelocity ||
           mode == kCia402OperationModeCyclicSyncVelocity;
}

inline constexpr bool IsPositionDrivenOperationMode(int8_t mode)
{
    return mode == kCia402OperationModeCyclicSyncPosition;
}

// Returns the active cyclic command PDO index for the given mode.
// CSP → 0x607A (Target Position), CSV → 0x60FF (Target Velocity).
inline constexpr uint16_t GetCyclicCommandPdoIndex(int8_t mode)
{
    return IsVelocityDrivenOperationMode(mode) ? 0x60FF : 0x607A;
}

inline constexpr const char* DescribeOperationModeShortName(int8_t mode)
{
    switch (mode) {
    case kCia402OperationModeProfileVelocity:
        return "PV";
    case kCia402OperationModeHoming:
        return "Homing";
    case kCia402OperationModeCyclicSyncPosition:
        return "CSP";
    case kCia402OperationModeCyclicSyncVelocity:
        return "CSV";
    default:
        return "Unknown";
    }
}

inline constexpr const char* DescribeOperationModeLongName(int8_t mode)
{
    switch (mode) {
    case kCia402OperationModeProfileVelocity:
        return "Profile Velocity";
    case kCia402OperationModeHoming:
        return "Homing";
    case kCia402OperationModeCyclicSyncPosition:
        return "Cyclic Synchronous Position";
    case kCia402OperationModeCyclicSyncVelocity:
        return "Cyclic Synchronous Velocity";
    default:
        return "Unknown";
    }
}

}  // namespace CoreHelpers