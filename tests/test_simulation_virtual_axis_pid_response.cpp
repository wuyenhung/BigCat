#include "EtherCATMaster.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr double kDsP100E7Scale = 0.0000274658203125;

bool Check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool SnapshotI32(EtherCATMaster& master, int offset, int32_t& outValue)
{
    outValue = 0;
    return master.SnapshotIOMap(offset,
                                reinterpret_cast<uint8_t*>(&outValue),
                                static_cast<int>(sizeof(outValue))) == sizeof(outValue);
}

bool ReadSdoU16(EtherCATMaster& master, uint16_t index, uint16_t& outValue)
{
    outValue = 0;
    int size = static_cast<int>(sizeof(outValue));
    return master.ReadSDO(1, index, 0x00, &outValue, size) && size == static_cast<int>(sizeof(outValue));
}

bool ReadSdoI32(EtherCATMaster& master, uint16_t index, int32_t& outValue)
{
    outValue = 0;
    int size = static_cast<int>(sizeof(outValue));
    return master.ReadSDO(1, index, 0x00, &outValue, size) && size == static_cast<int>(sizeof(outValue));
}

bool WriteSdoU8(EtherCATMaster& master, uint16_t index, uint8_t value)
{
    return master.WriteSDO(1, index, 0x00, &value, static_cast<int>(sizeof(value)));
}

bool WriteSdoU16(EtherCATMaster& master, uint16_t index, uint16_t value)
{
    return master.WriteSDO(1, index, 0x00, &value, static_cast<int>(sizeof(value)));
}

bool WriteSdoI32(EtherCATMaster& master, uint16_t index, int32_t value)
{
    return master.WriteSDO(1, index, 0x00, &value, static_cast<int>(sizeof(value)));
}

std::vector<PDOChannelDescriptor> CreateStandardCyclicDescriptors()
{
    std::vector<PDOChannelDescriptor> descriptors;
    descriptors.reserve(8);

    auto add = [&](const wchar_t* name, uint16_t index, uint8_t bitLength, bool isOutput, int byteOffset) {
        PDOChannelDescriptor desc;
        desc.name = name;
        desc.index = index;
        desc.subindex = 0;
        desc.bitLength = bitLength;
        desc.isOutput = isOutput;
        desc.byteOffset = byteOffset;
        descriptors.push_back(std::move(desc));
    };

    add(L"0x6040", 0x6040, 16, true, 0);
    add(L"0x607A", 0x607A, 32, true, 2);
    add(L"0x60FF", 0x60FF, 32, true, 6);
    add(L"0x6071", 0x6071, 16, true, 10);
    add(L"0x6041", 0x6041, 16, false, 0);
    add(L"0x6064", 0x6064, 32, false, 2);
    add(L"0x606C", 0x606C, 32, false, 6);
    add(L"0x6077", 0x6077, 16, false, 10);
    return descriptors;
}

}  // namespace

int main()
{
    EtherCATMaster master;
    master.SetCycleTime(2000);

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    master.RegisterPDODescriptors(1, CreateStandardCyclicDescriptors());
    master.SetMechanicalScalingFactors(1, kDsP100E7Scale, false);

    if (!Check(master.StartCyclicOperation(),
               "simulation cyclic operation should start successfully")) {
        return EXIT_FAILURE;
    }

    const uint8_t cspMode = static_cast<uint8_t>(CoreHelpers::kCia402OperationModeCyclicSyncPosition);
    const uint16_t enableOperation = 0x000F;
    if (!Check(master.WriteSDO(1, 0x6060, 0x00, (void*)&cspMode, 1),
               "simulation slave should accept CSP mode") ||
        !Check(master.WriteSDO(1, 0x6040, 0x00, (void*)&enableOperation, static_cast<int>(sizeof(enableOperation))),
               "simulation slave should accept enable-operation controlword")) {
        return EXIT_FAILURE;
    }

    int outBase = -1;
    int inBase = -1;
    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave should expose IO base offsets")) {
        return EXIT_FAILURE;
    }

    const int32_t targetPositionRaw = static_cast<int32_t>(std::llround(10.0 / kDsP100E7Scale));
    if (!Check(master.WriteSDO(1, 0x607A, 0x00, (void*)&targetPositionRaw, static_cast<int>(sizeof(targetPositionRaw))),
               "simulation slave should accept CSP target position")) {
        return EXIT_FAILURE;
    }

    bool sawLaggedMotion = false;
    bool sawMovingStatusNotReached = false;
    double maxVelocityDuringFirstWindow = 0.0;
    const auto firstWindowDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(120);
    while (std::chrono::steady_clock::now() < firstWindowDeadline) {
        int32_t actualPositionRaw = 0;
        int32_t actualVelocityRaw = 0;
        uint16_t movingStatus = 0;
        if (!Check(SnapshotI32(master, inBase + 2, actualPositionRaw),
                   "simulation PID response should expose actual position bytes") ||
            !Check(SnapshotI32(master, inBase + 6, actualVelocityRaw),
                   "simulation PID response should expose actual velocity bytes") ||
            !Check(ReadSdoU16(master, 0x6041, movingStatus),
                   "simulation PID response should expose moving statusword")) {
            return EXIT_FAILURE;
        }

        const double actualPositionDeg = static_cast<double>(actualPositionRaw) * kDsP100E7Scale;
        const double actualVelocityDegPerSec = static_cast<double>(actualVelocityRaw) * kDsP100E7Scale;
        if (actualPositionDeg > 0.05 && actualPositionDeg < 9.0) {
            sawLaggedMotion = true;
            if ((movingStatus & 0x0400u) == 0u) {
                sawMovingStatusNotReached = true;
            }
        }
        maxVelocityDuringFirstWindow = (std::max)(maxVelocityDuringFirstWindow, std::abs(actualVelocityDegPerSec));
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    int32_t finalPositionRaw = 0;
    int32_t finalVelocityRaw = 0;
    uint16_t finalStatus = 0;
    const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1400);
    do {
        if (!Check(SnapshotI32(master, inBase + 2, finalPositionRaw),
                   "simulation PID response should keep actual position readable") ||
            !Check(SnapshotI32(master, inBase + 6, finalVelocityRaw),
                   "simulation PID response should keep actual velocity readable")) {
            return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < settleDeadline);

    const double finalPositionDeg = static_cast<double>(finalPositionRaw) * kDsP100E7Scale;
    const double finalVelocityDegPerSec = static_cast<double>(finalVelocityRaw) * kDsP100E7Scale;
        if (!Check(ReadSdoU16(master, 0x6041, finalStatus),
             "simulation PID response should expose final statusword")) {
         return EXIT_FAILURE;
        }
    if (maxVelocityDuringFirstWindow > 260.0 ||
        std::abs(finalPositionDeg - 10.0) > 0.35 ||
         std::abs(finalVelocityDegPerSec) > 12.0 ||
         !sawMovingStatusNotReached ||
         (finalStatus & 0x0400u) == 0u) {
        std::cerr << "virtual PID response final state:"
                  << " sawLag=" << (sawLaggedMotion ? 1 : 0)
                << " sawMovingNotReached=" << (sawMovingStatusNotReached ? 1 : 0)
                  << " maxVelocity=" << maxVelocityDuringFirstWindow
                  << " finalPosition=" << finalPositionDeg
                << " finalVelocity=" << finalVelocityDegPerSec
                << " finalStatus=0x" << std::hex << finalStatus << std::dec << '\n';
    }

    if (!Check(sawLaggedMotion,
               "simulation virtual axis should move through intermediate feedback instead of snapping to the target") ||
         !Check(sawMovingStatusNotReached,
             "simulation moving axis should not report target reached while feedback is still in transit") ||
        !Check(maxVelocityDuringFirstWindow <= 260.0,
               "simulation virtual axis should limit acceleration/velocity like a servo plant") ||
        !Check(std::abs(finalPositionDeg - 10.0) <= 0.35,
               "simulation virtual axis PID loop should settle near the CSP target") ||
        !Check(std::abs(finalVelocityDegPerSec) <= 12.0,
             "simulation virtual axis PID loop should settle with low residual velocity") ||
         !Check((finalStatus & 0x0400u) != 0u,
             "simulation settled axis should report target reached")) {
        return EXIT_FAILURE;
    }

        const int32_t compressionPositionRaw = static_cast<int32_t>(std::llround(-67.55 / kDsP100E7Scale));
        const int32_t homeOffsetRaw = static_cast<int32_t>(std::llround(-68.0 / kDsP100E7Scale));
        const uint8_t homingMode = static_cast<uint8_t>(CoreHelpers::kCia402OperationModeHoming);
        const uint16_t startHoming = 0x001F;
        const uint16_t keepEnabled = 0x000F;

        if (!Check(WriteSdoI32(master, 0x6064, compressionPositionRaw),
             "simulation homing scenario should allow setting compressed actual position") ||
         !Check(WriteSdoI32(master, 0x607C, homeOffsetRaw),
             "simulation homing scenario should accept home offset") ||
         !Check(WriteSdoU8(master, 0x6060, homingMode),
             "simulation homing scenario should enter homing mode") ||
         !Check(WriteSdoU16(master, 0x6040, keepEnabled),
             "simulation homing scenario should enable operation before start") ||
         !Check(WriteSdoU16(master, 0x6040, startHoming),
             "simulation homing scenario should accept homing start bit")) {
         return EXIT_FAILURE;
        }

        bool homingComplete = false;
        bool sawHomingActive = false;
        uint16_t homingStatus = 0;
        const auto homingDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
        while (std::chrono::steady_clock::now() < homingDeadline) {
         if (!Check(ReadSdoU16(master, 0x6041, homingStatus),
                 "simulation homing scenario should expose statusword")) {
             return EXIT_FAILURE;
         }
         const bool complete = (homingStatus & 0x1000u) != 0u &&
                      (homingStatus & 0x0400u) != 0u &&
                      (homingStatus & 0x2000u) == 0u;
        if (!complete) {
            sawHomingActive = true;
        }
        if (complete && sawHomingActive) {
             homingComplete = true;
             break;
         }
         std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        int32_t homedActualRaw = 0;
        if (!Check(ReadSdoI32(master, 0x6064, homedActualRaw),
             "simulation homing scenario should expose final actual position")) {
         return EXIT_FAILURE;
        }
        const double homedActualDeg = static_cast<double>(homedActualRaw) * kDsP100E7Scale;
        if (!homingComplete || std::abs(homedActualDeg + 68.0) > 0.05) {
         std::cerr << "simulation homing final state:"
                << " complete=" << (homingComplete ? 1 : 0)
                      << " sawActive=" << (sawHomingActive ? 1 : 0)
                << " status=0x" << std::hex << homingStatus << std::dec
                << " actual=" << homedActualDeg << '\n';
        }
        if (!Check(homingComplete,
             "simulation homing from compressed position should complete before MotionController timeout") ||
         !Check(std::abs(homedActualDeg + 68.0) <= 0.05,
             "simulation homing should settle at the configured home offset")) {
         return EXIT_FAILURE;
        }

        if (!Check(WriteSdoU16(master, 0x6040, keepEnabled),
             "simulation homing scenario should accept clearing homing start bit after completion")) {
         return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        int32_t heldAfterCompleteRaw = 0;
        if (!Check(ReadSdoI32(master, 0x6064, heldAfterCompleteRaw),
             "simulation homing scenario should expose held actual position after bit4 clear")) {
         return EXIT_FAILURE;
        }
        const double heldAfterCompleteDeg = static_cast<double>(heldAfterCompleteRaw) * kDsP100E7Scale;
        if (!Check(std::abs(heldAfterCompleteDeg + 68.0) <= 0.05,
             "simulation homing mode with bit4 clear should hold position instead of drifting toward 0x607A")) {
         std::cerr << "heldAfterCompleteDeg=" << heldAfterCompleteDeg << '\n';
         return EXIT_FAILURE;
        }

        if (!Check(WriteSdoI32(master, 0x6064, compressionPositionRaw),
             "simulation homing abort scenario should allow resetting compressed actual position") ||
         !Check(WriteSdoU8(master, 0x6060, homingMode),
             "simulation homing abort scenario should enter homing mode") ||
         !Check(WriteSdoU16(master, 0x6040, startHoming),
             "simulation homing abort scenario should start homing") ) {
         return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!Check(WriteSdoU16(master, 0x6040, keepEnabled),
             "simulation homing abort scenario should accept bit4 clear")) {
         return EXIT_FAILURE;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        int32_t heldAfterAbortRaw = 0;
        uint16_t abortStatus = 0;
        if (!Check(ReadSdoI32(master, 0x6064, heldAfterAbortRaw),
             "simulation homing abort scenario should expose held actual position") ||
         !Check(ReadSdoU16(master, 0x6041, abortStatus),
             "simulation homing abort scenario should expose statusword")) {
         return EXIT_FAILURE;
        }
        const double heldAfterAbortDeg = static_cast<double>(heldAfterAbortRaw) * kDsP100E7Scale;
        const bool homingStillActive = (abortStatus & 0x1000u) == 0u && (abortStatus & 0x0200u) != 0u;
        if (!Check(heldAfterAbortDeg < -60.0,
             "simulation homing abort should not drift slowly back toward zero") ||
         !Check(!homingStillActive,
             "simulation homing abort should clear active homing status after bit4 is released")) {
         std::cerr << "simulation homing abort state:"
                << " status=0x" << std::hex << abortStatus << std::dec
                << " actual=" << heldAfterAbortDeg << '\n';
         return EXIT_FAILURE;
        }

    return EXIT_SUCCESS;
}