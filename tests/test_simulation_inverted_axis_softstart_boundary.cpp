#include "EtherCATMaster.h"

#include <windows.h>

#include <array>
#include <algorithm>
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

int32_t MechanicalToRaw(double mechanicalValue, bool invertSign) {
    const double sign = invertSign ? -1.0 : 1.0;
    return static_cast<int32_t>(std::llround(mechanicalValue * sign / kDsP100E7Scale));
}

double RawToMechanical(int32_t rawValue, bool invertSign) {
    const double sign = invertSign ? -1.0 : 1.0;
    return static_cast<double>(rawValue) * kDsP100E7Scale * sign;
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

        Write(values);
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

    void Write(const std::array<float, kAxisCount>& values) const {
        if (view) {
            std::memcpy(view, values.data(), sizeof(float) * kAxisCount);
            FlushViewOfFile(view, sizeof(float) * kAxisCount);
        }
    }
};

}  // namespace

int main() {
    std::array<float, kAxisCount> mmfTargets{};
    mmfTargets.fill(10.0f);
    MmfDegMappingGuard mmfGuard(mmfTargets);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_DEG should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    master.SetCyclicOperationMode(CoreHelpers::kCia402OperationModeCyclicSyncVelocity);
    MasterShutdownGuard masterShutdownGuard{master};
    master.SetMmf1InputMode(Mmf1InputMode::ServoAngle);

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    const auto descriptors = CreateStandardCyclicDescriptors();
    std::array<bool, kAxisCount + 1> invertAxis{};
    std::array<int, kAxisCount + 1> outBases{};
    std::array<int, kAxisCount + 1> inBases{};
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        invertAxis[slave] = (slave % 2) == 0;
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, invertAxis[slave]);
        master.ConfigurePDOFilter(slave, 180.0, 600.0, 600.0, 2500.0);
        master.SetPDOFilterLimits(slave, -65.0, 65.0);
        master.SetPDOFilterViolationCallback(slave, [](PDOFilter::ViolationType, double, double, bool, double) {});

        const int32_t initialActualRaw = MechanicalToRaw(-68.0, invertAxis[slave]);
        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&initialActualRaw, static_cast<int>(sizeof(initialActualRaw))),
                   "simulation should allow setup of initial actual position") ||
            !Check(master.GetSlaveIOBaseBytes(slave, outBases[slave], inBases[slave]),
                   "simulation slave should expose IO base offsets")) {
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
    mmfGuard.Write(mmfTargets);

    if (!Check(std::abs(master.GetConfiguredPDOFilterMaxDeceleration(1) - 600.0) < 1e-6,
               "configured PDO filter deceleration should report P5, not the soft-start active deceleration")) {
        return EXIT_FAILURE;
    }

    std::array<double, kAxisCount + 1> maxInwardVelocity{};
    std::array<double, kAxisCount + 1> maxActualAdvance{};
    std::array<int32_t, kAxisCount + 1> lastVelocityRaw{};
    std::array<double, kAxisCount + 1> lastActual{};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    while (std::chrono::steady_clock::now() < deadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            int32_t targetVelRaw = 0;
            int32_t actualPosRaw = 0;
            if (!Check(SnapshotI32(master, outBases[slave] + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                       "simulation should expose CSV target velocity bytes") ||
                !Check(SnapshotI32(master, inBases[slave] + EtherCATMaster::kPdoInOff_ActualPos, actualPosRaw),
                       "simulation should expose actual position bytes")) {
                return EXIT_FAILURE;
            }

            const double velocityMechanical = RawToMechanical(targetVelRaw, invertAxis[slave]);
            const double actualMechanical = RawToMechanical(actualPosRaw, invertAxis[slave]);
            maxInwardVelocity[slave] = (std::max)(maxInwardVelocity[slave], velocityMechanical);
            maxActualAdvance[slave] = (std::max)(maxActualAdvance[slave], actualMechanical - (-68.0));
            lastVelocityRaw[slave] = targetVelRaw;
            lastActual[slave] = actualMechanical;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool ok = true;
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        ok = Check(maxInwardVelocity[slave] > 0.5,
                   "soft-start should command inward velocity for every axis at the lower boundary") && ok;
        ok = Check(maxActualAdvance[slave] > 0.05,
                   "soft-start should move every axis inward from the lower boundary") && ok;
    }

    if (!ok) {
        std::cerr << "inverted soft-start state:";
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            std::cerr << " S" << slave
                      << " inv=" << (invertAxis[slave] ? 1 : 0)
                      << " maxInwardVel=" << maxInwardVelocity[slave]
                      << " maxAdvance=" << maxActualAdvance[slave]
                      << " lastRawVel=" << lastVelocityRaw[slave]
                      << " lastActual=" << lastActual[slave];
        }
        std::cerr << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[inverted-softstart]";
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        std::cout << " S" << slave
                  << " inv=" << (invertAxis[slave] ? 1 : 0)
                  << " maxInwardVel=" << maxInwardVelocity[slave]
                  << " maxAdvance=" << maxActualAdvance[slave];
    }
    std::cout << '\n';
    return EXIT_SUCCESS;
}