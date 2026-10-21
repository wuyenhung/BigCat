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
constexpr double kUpperLimitDeg = 65.0;
constexpr double kTargetDeg = 64.5;
constexpr double kInitialActualDeg = 45.0;
constexpr double kConfiguredMaxDecelDeg = 3600.0;
constexpr double kSoftStartAccelRatio = 0.2;

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

int32_t MechanicalToRaw(double mechanicalValue) {
    return static_cast<int32_t>(std::llround(mechanicalValue / kDsP100E7Scale));
}

double RawToMechanical(int32_t rawValue) {
    return static_cast<double>(rawValue) * kDsP100E7Scale;
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

struct MasterShutdownGuard {
    EtherCATMaster& master;

    ~MasterShutdownGuard() {
        master.Shutdown();
    }
};

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
    mmfTargets.fill(static_cast<float>(kTargetDeg));
    MmfDegMappingGuard mmfGuard(mmfTargets);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_DEG should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    MasterShutdownGuard shutdownGuard{master};
    master.SetMmf1InputMode(Mmf1InputMode::ServoAngle);

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    const auto descriptors = CreateStandardCyclicDescriptors();
    std::array<std::atomic<bool>, kAxisCount> hardLimitSeen{};
    for (auto& value : hardLimitSeen) {
        value.store(false, std::memory_order_relaxed);
    }

    const int32_t initialActualRaw = MechanicalToRaw(kInitialActualDeg);
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 720.0, 3600.0, kConfiguredMaxDecelDeg, 8000.0);
        master.SetPDOFilterLimits(slave, -kUpperLimitDeg, kUpperLimitDeg);
        master.SetPDOFilterViolationCallback(
            slave,
            [&hardLimitSeen, slave](PDOFilter::ViolationType type, double, double, bool, double) {
                if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax) ||
                    PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) {
                    hardLimitSeen[slave - 1].store(true, std::memory_order_relaxed);
                }
            });

        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&initialActualRaw, static_cast<int>(sizeof(initialActualRaw))),
                   "simulation should allow setup of initial actual position")) {
            return EXIT_FAILURE;
        }
    }

    if (!Check(master.StartMMF1In(),
               "MMF1 input reader should start successfully") ||
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

    if (!Check(mmfMapped, "soft-start hard-limit prebrake test should consume the MMF mapping") ||
        !Check(mmfReads > 0, "soft-start hard-limit prebrake test should observe MMF reads")) {
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

    std::array<int, kAxisCount + 1> outBases{};
    std::array<int, kAxisCount + 1> inBases{};
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.GetSlaveIOBaseBytes(slave, outBases[slave], inBases[slave]),
                   "simulation slave should expose IO base offsets during soft-start")) {
            return EXIT_FAILURE;
        }
    }

    constexpr double activeSoftStartDecel = kConfiguredMaxDecelDeg * kSoftStartAccelRatio;
    bool sawPrebrakeWindow = false;
    double maxActualDeg = kInitialActualDeg;
    double maxVelocityNearEdgeDegPerSec = 0.0;
    double maxEnvelopeExcessDegPerSec = 0.0;
    int envelopeViolations = 0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
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

            const double actualDeg = RawToMechanical(actualPosRaw);
            const double velocityDegPerSec = RawToMechanical(targetVelRaw);
            maxActualDeg = (std::max)(maxActualDeg, actualDeg);

            if (actualDeg >= 58.0 && actualDeg < kUpperLimitDeg) {
                sawPrebrakeWindow = true;
                const double remaining = (std::max)(0.0, kUpperLimitDeg - actualDeg);
                const double activeEnvelopeVelocity = std::sqrt(2.0 * activeSoftStartDecel * remaining);
                const double excess = velocityDegPerSec - activeEnvelopeVelocity;
                maxEnvelopeExcessDegPerSec = (std::max)(maxEnvelopeExcessDegPerSec, excess);
                if (excess > 12.0) {
                    ++envelopeViolations;
                }
            }
            if (actualDeg >= 64.5) {
                maxVelocityNearEdgeDegPerSec = (std::max)(maxVelocityNearEdgeDegPerSec, velocityDegPerSec);
            }
        }

        bool anyHardLimitSeen = false;
        for (const auto& seen : hardLimitSeen) {
            anyHardLimitSeen = anyHardLimitSeen || seen.load(std::memory_order_relaxed);
        }
        if (anyHardLimitSeen) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    bool anyHardLimitSeen = false;
    for (const auto& seen : hardLimitSeen) {
        anyHardLimitSeen = anyHardLimitSeen || seen.load(std::memory_order_relaxed);
    }

    if (!sawPrebrakeWindow || anyHardLimitSeen || maxActualDeg > 65.05 ||
        envelopeViolations > 0 || maxVelocityNearEdgeDegPerSec > 45.0) {
        std::cerr << "soft-start hard-limit prebrake state:"
                  << " sawPrebrakeWindow=" << (sawPrebrakeWindow ? 1 : 0)
                  << " hardLimitSeen=" << (anyHardLimitSeen ? 1 : 0)
                  << " maxActual=" << maxActualDeg
                  << " maxVelocityNearEdge=" << maxVelocityNearEdgeDegPerSec
                  << " maxEnvelopeExcess=" << maxEnvelopeExcessDegPerSec
                  << " envelopeViolations=" << envelopeViolations << '\n';
    }

    if (!Check(sawPrebrakeWindow,
               "soft-start should pass through the hard-limit prebrake window") ||
        !Check(!anyHardLimitSeen,
               "soft-start must not report a PDO hard-limit violation while approaching a legal upper target") ||
        !Check(maxActualDeg <= 65.05,
               "soft-start actual position must not overshoot the upper PDO limit") ||
        !Check(envelopeViolations == 0,
               "CSV target velocity must stay inside the active soft-start braking envelope near the hard limit") ||
        !Check(maxVelocityNearEdgeDegPerSec <= 45.0,
               "CSV target velocity should be mostly bled off before the upper hard limit")) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
