#include "EtherCATMaster.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kMmfDegMappingName[] = L"OUTPUT_MMF_DEG";
constexpr int kAxisCount = 6;
constexpr double kDsP100E7Scale = 0.0000274658203125;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool SnapshotI32(EtherCATMaster& master, int offset, int32_t& outValue) {
    outValue = 0;
    return master.SnapshotIOMap(offset, reinterpret_cast<uint8_t*>(&outValue), static_cast<int>(sizeof(outValue))) == sizeof(outValue);
}

std::vector<PDOChannelDescriptor> CreateStandardCyclicDescriptors() {
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

struct MmfDegMappingGuard {
    HANDLE mapping = nullptr;
    float* view = nullptr;

    explicit MmfDegMappingGuard(const std::array<float, kAxisCount>& values) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                     nullptr,
                                     PAGE_READWRITE,
                                     0,
                                     static_cast<DWORD>(sizeof(float) * kAxisCount),
                                     kMmfDegMappingName);
        if (!mapping) {
            return;
        }

        view = static_cast<float*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(float) * kAxisCount));
        if (!view) {
            CloseHandle(mapping);
            mapping = nullptr;
            return;
        }

        std::memcpy(view, values.data(), sizeof(float) * kAxisCount);
    }

    ~MmfDegMappingGuard() {
        if (view) {
            UnmapViewOfFile(view);
            view = nullptr;
        }
        if (mapping) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    bool IsReady() const {
        return mapping != nullptr && view != nullptr;
    }
};

}  // namespace

int main() {
    const std::array<float, kAxisCount> mmfTargets = {190.0f, 190.0f, 190.0f, 190.0f, 190.0f, 190.0f};
    MmfDegMappingGuard mmfGuard(mmfTargets);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_DEG should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    const auto descriptors = CreateStandardCyclicDescriptors();
    const int32_t initialActualRaw = static_cast<int32_t>(std::llround(179.0 / kDsP100E7Scale));
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 180.0, 600.0, 600.0, 2500.0);
        master.SetPDOFilterLimits(slave, -1000.0, 1000.0);

        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&initialActualRaw, static_cast<int>(sizeof(initialActualRaw))),
                   "simulation should allow setup of near-wrap actual position")) {
            return EXIT_FAILURE;
        }
    }

    if (!Check(master.StartMMF1In(),
               "MMF1 input reader should start successfully") ||
        !Check(master.StartCyclicOperation(),
               "simulation cyclic operation should start successfully")) {
        return EXIT_FAILURE;
    }

    const uint16_t enableOperation = 0x000F;
    const uint8_t cyclicMode = static_cast<uint8_t>(CoreHelpers::kDefaultCyclicOperationMode);
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6060, 0x00, (void*)&cyclicMode, 1),
                   "simulation slave should accept default cyclic mode") ||
            !Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6040, 0x00, (void*)&enableOperation, static_cast<int>(sizeof(enableOperation))),
                   "simulation slave should accept enable-operation controlword")) {
            return EXIT_FAILURE;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    master.EnablePDOFiltering(true);
    if (!Check(master.IsPDOFilteringEnabled(),
               "PDO filtering should enable successfully once simulation is operational")) {
        return EXIT_FAILURE;
    }
    master.TriggerAllPDOFiltersSoftStart();

    int outBase = -1;
    int inBase = -1;
    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave should expose IO base offsets")) {
        return EXIT_FAILURE;
    }

    bool sawCrossingRegion = false;
    bool havePreviousTarget = false;
    double previousTargetDeg = 0.0;
    double maxBackwardStepDeg = 0.0;
    double maxVelocityDegPerSec = 0.0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < deadline) {
        int32_t targetPosRaw = 0;
        int32_t targetVelRaw = 0;
        if (!Check(SnapshotI32(master, outBase + 2, targetPosRaw),
                   "simulation should expose logical target position bytes") ||
            !Check(SnapshotI32(master, outBase + 6, targetVelRaw),
                   "simulation should expose CSV target velocity bytes")) {
            return EXIT_FAILURE;
        }

        const double targetDeg = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
        const double velocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
        maxVelocityDegPerSec = (std::max)(maxVelocityDegPerSec, std::abs(velocityDegPerSec));
        if (targetDeg > 180.0) {
            sawCrossingRegion = true;
        }
        if (havePreviousTarget) {
            maxBackwardStepDeg = (std::max)(maxBackwardStepDeg, previousTargetDeg - targetDeg);
        }
        previousTargetDeg = targetDeg;
        havePreviousTarget = true;

        PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
        bool isError = false;
        bool isTrackingFatal = false;
        if (!Check(master.GetPDOFilterState(1, violationType, isError, isTrackingFatal),
                   "GetPDOFilterState should succeed during wrap continuity simulation") ||
            !Check(!isError, "wrap continuity soft-start should not latch PDOFilter error") ||
            !Check(!isTrackingFatal, "wrap continuity soft-start should not latch fatal tracking error")) {
            return EXIT_FAILURE;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(sawCrossingRegion,
               "wrap continuity regression should drive the logical target past +180 degrees") ||
        !Check(maxBackwardStepDeg < 20.0,
               "CSV soft-start logical target must not wrap from +180 degrees to -180 degrees") ||
        !Check(maxVelocityDegPerSec <= 35.0,
               "CSV soft-start velocity command should stay near the configured soft-start velocity limit")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
