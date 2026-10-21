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

constexpr wchar_t kMmf6DofMappingName[] = L"OUTPUT_MMF_6DOF";
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

struct MasterShutdownGuard {
    EtherCATMaster& master;

    ~MasterShutdownGuard() {
        master.Shutdown();
    }
};

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

struct Mmf6DofMappingGuard {
    HANDLE mapping = nullptr;
    float* view = nullptr;

    explicit Mmf6DofMappingGuard(const std::array<float, kAxisCount>& values) {
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                     nullptr,
                                     PAGE_READWRITE,
                                     0,
                                     static_cast<DWORD>(sizeof(float) * kAxisCount),
                                     kMmf6DofMappingName);
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

    ~Mmf6DofMappingGuard() {
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
    const std::array<float, kAxisCount> poseTarget = {0.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f};
    Mmf6DofMappingGuard mmfGuard(poseTarget);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_6DOF should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    WuSOEM::StewartParams params;
    params.servoAngleLimit = 65.0L;
    params.fisheyeAngleLimit = 35.0L;
    master.SetStewartParams(params);
    master.SetMmf1InputMode(Mmf1InputMode::Pose6D);
    master.SetMmf1PoseCoordinateSystem(Mmf1PoseCoordinateSystem::Platform);

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
    MasterShutdownGuard masterShutdownGuard{master};

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
               "MMF1 Pose6D input reader should start successfully") ||
        !Check(master.StartCyclicOperation(),
               "simulation cyclic operation should start successfully")) {
        return EXIT_FAILURE;
    }

    const uint16_t enableOperation = 0x000F;
    const uint8_t cyclicMode = static_cast<uint8_t>(master.GetCyclicOperationMode());
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

    std::array<int, kAxisCount + 1> outBases{};
    std::array<int, kAxisCount + 1> inBases{};
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.GetSlaveIOBaseBytes(slave, outBases[slave], inBases[slave]),
                   "simulation slave should expose IO base offsets during Pose6D soft-start")) {
            return EXIT_FAILURE;
        }
    }

    bool allReady = false;
    double maxAbsFilterOutputDeg = 0.0;
    double maxAbsVelocityDegPerSec = 0.0;
    std::array<double, kAxisCount + 1> finalTargetDeg{};
    std::array<double, kAxisCount + 1> finalActualDeg{};
    std::array<double, kAxisCount + 1> finalVelocityDegPerSec{};
    std::array<bool, kAxisCount + 1> finalReady{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        allReady = master.AreAllPDOFiltersSoftStartReady_Cached();

        for (int slave = 1; slave <= kAxisCount; ++slave) {
            PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
            bool isError = false;
            bool isTrackingFatal = false;
            if (!Check(master.GetPDOFilterState(slave, violationType, isError, isTrackingFatal),
                       "GetPDOFilterState should succeed during Pose6D simulation soft-start") ||
                !Check(!isError, "Pose6D simulation soft-start should not latch PDOFilter error state") ||
                !Check(!isTrackingFatal, "Pose6D simulation soft-start should not latch fatal tracking error") ||
                !Check(!fatalTrackingSeen[slave - 1].load(std::memory_order_relaxed),
                       "Pose6D simulation soft-start should not report fatal tracking while feedback follows CSV command")) {
                return EXIT_FAILURE;
            }

            int32_t targetPosRaw = 0;
            int32_t targetVelRaw = 0;
            if (!Check(SnapshotI32(master, outBases[slave] + EtherCATMaster::kPdoOutOff_TargetPos, targetPosRaw),
                       "Pose6D simulation should expose target position bytes") ||
                !Check(SnapshotI32(master, outBases[slave] + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                       "Pose6D simulation should expose CSV velocity bytes")) {
                return EXIT_FAILURE;
            }

            maxAbsFilterOutputDeg = (std::max)(maxAbsFilterOutputDeg, std::abs(static_cast<double>(targetPosRaw) * kDsP100E7Scale));
            maxAbsVelocityDegPerSec = (std::max)(maxAbsVelocityDegPerSec, std::abs(static_cast<double>(targetVelRaw) * kDsP100E7Scale));
            finalTargetDeg[slave] = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
            finalVelocityDegPerSec[slave] = static_cast<double>(targetVelRaw) * kDsP100E7Scale;

            int32_t actualPosRaw = 0;
            if (!Check(SnapshotI32(master, inBases[slave] + EtherCATMaster::kPdoInOff_ActualPos, actualPosRaw),
                       "Pose6D simulation should expose actual position bytes")) {
                return EXIT_FAILURE;
            }
            finalActualDeg[slave] = static_cast<double>(actualPosRaw) * kDsP100E7Scale;
            finalReady[slave] = master.IsPDOFilterSoftStartReady_Cached(slave);
        }

        if (allReady) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!allReady || maxAbsVelocityDegPerSec > 35.0) {
        std::cerr << "Pose6D soft-start final state:";
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            std::cerr << " S" << slave
                      << " ready=" << (finalReady[slave] ? 1 : 0)
                      << " target=" << finalTargetDeg[slave]
                      << " actual=" << finalActualDeg[slave]
                      << " vel=" << finalVelocityDegPerSec[slave];
        }
        std::cerr << " maxFilter=" << maxAbsFilterOutputDeg
                  << " maxVel=" << maxAbsVelocityDegPerSec << '\n';
    }

    if (!Check(allReady,
               "Pose6D simulation soft-start should complete while MMF input is present") ||
        !Check(maxAbsFilterOutputDeg <= 80.0,
               "Pose6D simulation soft-start should keep filter output in servo-angle units, not raw counts or runaway turns") ||
        !Check(maxAbsVelocityDegPerSec <= 35.0,
               "Pose6D simulation soft-start velocity command should stay near the configured soft-start velocity limit")) {
        return EXIT_FAILURE;
    }

    master.CompleteAllPDOFiltersSoftStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    double maxAbsPostCompleteVelocityDegPerSec = 0.0;
    double maxPostCompleteVelocityJumpDegPerSec = 0.0;
    std::array<double, kAxisCount + 1> previousTargetVelocityDeg{};
    std::array<bool, kAxisCount + 1> havePreviousTargetVelocity{};
    int postCompleteSamples = 0;
    const auto holdDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    while (std::chrono::steady_clock::now() < holdDeadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            int32_t targetVelRaw = 0;
            if (!Check(SnapshotI32(master, outBases[slave] + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                       "Pose6D simulation should expose post-soft-start CSV velocity bytes")) {
                return EXIT_FAILURE;
            }

            const double velocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
            maxAbsPostCompleteVelocityDegPerSec =
                (std::max)(maxAbsPostCompleteVelocityDegPerSec, std::abs(velocityDegPerSec));
            if (havePreviousTargetVelocity[slave]) {
                maxPostCompleteVelocityJumpDegPerSec =
                    (std::max)(maxPostCompleteVelocityJumpDegPerSec,
                               std::abs(velocityDegPerSec - previousTargetVelocityDeg[slave]));
            }
            previousTargetVelocityDeg[slave] = velocityDegPerSec;
            havePreviousTargetVelocity[slave] = true;
            ++postCompleteSamples;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Check(postCompleteSamples >= 600,
               "Pose6D post-soft-start hold should collect enough CSV velocity samples") ||
        !Check(maxAbsPostCompleteVelocityDegPerSec <= 8.0,
               "Pose6D post-soft-start held target should not keep issuing oscillatory CSV velocity") ||
        !Check(maxPostCompleteVelocityJumpDegPerSec <= 4.0,
               "Pose6D post-soft-start held target should not produce CSV velocity jumps")) {
        std::cerr << "Pose6D post-soft-start hold state:"
                  << " samples=" << postCompleteSamples
                  << " maxPostCompleteVel=" << maxAbsPostCompleteVelocityDegPerSec
                  << " maxPostCompleteVelJump=" << maxPostCompleteVelocityJumpDegPerSec << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
