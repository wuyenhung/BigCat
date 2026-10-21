#include "EtherCATMaster.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
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

    add(L"0x6040", 0x6040, 16, true, EtherCATMaster::kPdoOutOff_Controlword);
    add(L"0x607A", 0x607A, 32, true, EtherCATMaster::kPdoOutOff_TargetPos);
    add(L"0x60FF", 0x60FF, 32, true, EtherCATMaster::kPdoOutOff_TargetVel);
    add(L"0x6071", 0x6071, 16, true, EtherCATMaster::kPdoOutOff_TargetTorque);
    add(L"0x6041", 0x6041, 16, false, EtherCATMaster::kPdoInOff_Statusword);
    add(L"0x6064", 0x6064, 32, false, EtherCATMaster::kPdoInOff_ActualPos);
    add(L"0x606C", 0x606C, 32, false, EtherCATMaster::kPdoInOff_ActualVel);
    add(L"0x6077", 0x6077, 16, false, EtherCATMaster::kPdoInOff_ActualTorque);
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
    const std::array<float, kAxisCount> mmfTargets = {10.0f, 10.0f, 10.0f, 10.0f, 10.0f, 10.0f};
    MmfDegMappingGuard mmfGuard(mmfTargets);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_DEG should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    master.SetCyclicOperationMode(CoreHelpers::kCia402OperationModeCyclicSyncVelocity);

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    const auto descriptors = CreateStandardCyclicDescriptors();
    std::array<std::atomic<bool>, kAxisCount> fatalTrackingSeen{};
    for (auto& value : fatalTrackingSeen) {
        value.store(false, std::memory_order_relaxed);
    }

    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 180.0, 600.0, 600.0, 2500.0);
        master.SetPDOFilterLimits(slave, -65.0, 65.0);
        master.SetPDOFilterViolationCallback(
            slave,
            [&fatalTrackingSeen, slave](PDOFilter::ViolationType type, double, double, bool, double duration) {
                if (duration >= 0.5 && PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                    fatalTrackingSeen[slave - 1].store(true, std::memory_order_relaxed);
                }
            });
    }

    if (!Check(master.StartMMF1In(),
               "MMF1 input reader should start successfully for simulation soft-start regression") ||
        !Check(master.StartCyclicOperation(),
               "simulation cyclic operation should start successfully")) {
        return EXIT_FAILURE;
    }

    bool mmfMapped = false;
    uint64_t mmfReads = 0;
    for (int i = 0; i < 40; ++i) {
        mmfMapped = mmfMapped || master.MMF1IsMapped();
        mmfReads += master.MMF1GetAndResetReadCount();
        if (mmfMapped && mmfReads > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!Check(mmfMapped, "simulation soft-start regression should consume the MMF mapping") ||
        !Check(mmfReads > 0, "simulation soft-start regression should observe at least one MMF read")) {
        return EXIT_FAILURE;
    }

    const uint16_t enableOperation = 0x000F;
    const uint8_t cyclicMode = static_cast<uint8_t>(master.GetCyclicOperationMode());
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6060, 0x00, (void*)&cyclicMode, 1),
                   "simulation slave should accept default cyclic mode before soft-start") ||
            !Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6040, 0x00, (void*)&enableOperation, static_cast<int>(sizeof(enableOperation))),
                   "simulation slave should accept enable-operation controlword before soft-start")) {
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    double maxAbsActualPositionMechanical = 0.0;
    std::array<int, kAxisCount + 1> outBases{};
    std::array<int, kAxisCount + 1> inBases{};
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.GetSlaveIOBaseBytes(slave, outBases[slave], inBases[slave]),
                   "simulation slave should expose IO base offsets during soft-start")) {
            return EXIT_FAILURE;
        }
    }

    while (std::chrono::steady_clock::now() < deadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
            bool isError = false;
            bool isTrackingFatal = false;
            if (!Check(master.GetPDOFilterState(slave, violationType, isError, isTrackingFatal),
                       "GetPDOFilterState should succeed during simulation soft-start")) {
                return EXIT_FAILURE;
            }
            if (!Check(!isError, "field-like simulation soft-start should not latch PDOFilter error state during the first 2 seconds") ||
                !Check(!isTrackingFatal, "field-like simulation soft-start should not latch fatal tracking error during the first 2 seconds") ||
                !Check(!fatalTrackingSeen[slave - 1].load(std::memory_order_relaxed),
                       "field-like simulation soft-start should not report 0.5 seconds of fatal tracking while feedback follows CSV command")) {
                return EXIT_FAILURE;
            }

            int32_t actualPosition = 0;
            if (!Check(SnapshotI32(master, inBases[slave] + EtherCATMaster::kPdoInOff_ActualPos, actualPosition),
                       "simulation soft-start should expose actual position bytes")) {
                return EXIT_FAILURE;
            }
            maxAbsActualPositionMechanical = (std::max)(
                maxAbsActualPositionMechanical,
                std::abs(static_cast<double>(actualPosition) * kDsP100E7Scale));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!Check(maxAbsActualPositionMechanical <= 15.0,
               "field-like simulation soft-start should keep actual positions bounded instead of running away to huge values")) {
        return EXIT_FAILURE;
    }

    bool allReady = false;
    const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < readyDeadline) {
        allReady = master.AreAllPDOFiltersSoftStartReady_Cached();
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
            bool isError = false;
            bool isTrackingFatal = false;
            if (!Check(master.GetPDOFilterState(slave, violationType, isError, isTrackingFatal),
                       "GetPDOFilterState should succeed while waiting for soft-start ready") ||
                !Check(!isError, "simulation soft-start should not latch PDOFilter error while settling") ||
                !Check(!isTrackingFatal, "simulation soft-start should not latch fatal tracking while settling")) {
                return EXIT_FAILURE;
            }
        }
        if (allReady) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::array<double, kAxisCount + 1> minActualDeg{};
    std::array<double, kAxisCount + 1> maxActualDeg{};
    minActualDeg.fill(1.0e9);
    maxActualDeg.fill(-1.0e9);
    double maxPostReadyTargetErrorDeg = 0.0;
    const auto postReadyDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < postReadyDeadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            int32_t targetPosition = 0;
            int32_t actualPosition = 0;
            if (!Check(SnapshotI32(master, outBases[slave] + EtherCATMaster::kPdoOutOff_TargetPos, targetPosition),
                       "simulation post-soft-start should expose target position bytes") ||
                !Check(SnapshotI32(master, inBases[slave] + EtherCATMaster::kPdoInOff_ActualPos, actualPosition),
                       "simulation post-soft-start should expose actual position bytes")) {
                return EXIT_FAILURE;
            }

            const double targetDeg = static_cast<double>(targetPosition) * kDsP100E7Scale;
            const double actualDeg = static_cast<double>(actualPosition) * kDsP100E7Scale;
            minActualDeg[slave] = (std::min)(minActualDeg[slave], actualDeg);
            maxActualDeg[slave] = (std::max)(maxActualDeg[slave], actualDeg);
            maxPostReadyTargetErrorDeg = (std::max)(maxPostReadyTargetErrorDeg, std::abs(actualDeg - targetDeg));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    double maxPostReadyActualRangeDeg = 0.0;
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        maxPostReadyActualRangeDeg = (std::max)(maxPostReadyActualRangeDeg,
                                               maxActualDeg[slave] - minActualDeg[slave]);
    }
    if (!allReady || maxPostReadyTargetErrorDeg > 1.0 || maxPostReadyActualRangeDeg > 0.75) {
        std::cerr << "post-soft-start simulation actual-position stability:"
                  << " allReady=" << (allReady ? 1 : 0)
                  << " maxError=" << maxPostReadyTargetErrorDeg
                  << " maxRange=" << maxPostReadyActualRangeDeg << '\n';
    }
    if (!Check(allReady,
               "simulation soft-start should reach ready for a constant 10-degree command") ||
        !Check(maxPostReadyTargetErrorDeg <= 1.0,
               "simulation actual position should stay close to filter output after soft-start") ||
        !Check(maxPostReadyActualRangeDeg <= 0.75,
               "simulation actual position should not oscillate after soft-start")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}