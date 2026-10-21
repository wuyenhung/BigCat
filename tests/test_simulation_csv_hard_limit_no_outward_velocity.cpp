#include "EtherCATMaster.h"

#include <windows.h>

#include <algorithm>
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
    mmfTargets.fill(0.0f);
    MmfDegMappingGuard mmfGuard(mmfTargets);
    if (!Check(mmfGuard.IsReady(), "test MMF mapping for OUTPUT_MMF_DEG should be created")) {
        return EXIT_FAILURE;
    }

    EtherCATMaster master;
    master.SetCyclicOperationMode(CoreHelpers::kCia402OperationModeCyclicSyncVelocity);
    master.SetMmf1InputMode(Mmf1InputMode::ServoAngle);

    if (!Check(master.Initialize(EtherCATMaster::GetSimulationInterfaceName()),
               "simulation initialize should succeed") ||
        !Check(master.ConfigureSlaves(),
               "simulation slave configuration should succeed")) {
        return EXIT_FAILURE;
    }

    const auto descriptors = CreateStandardCyclicDescriptors();
    const int32_t initialActualRaw = static_cast<int32_t>(std::llround(45.0 / kDsP100E7Scale));
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 720.0, 3600.0, 3600.0, 8000.0);
        master.SetPDOFilterLimits(slave, -65.0, 65.0);
        master.SetPDOFilterViolationCallback(slave, [](PDOFilter::ViolationType, double, double, bool, double) {});

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

    int outBase = -1;
    int inBase = -1;
    if (!Check(master.GetSlaveIOBaseBytes(1, outBase, inBase),
               "simulation slave should expose IO base offsets")) {
        return EXIT_FAILURE;
    }

    mmfGuard.Write(mmfTargets);

    bool sawTrackingBoundary = false;
    bool sawTrackingRecoveryVelocity = false;
    double minTrackingBoundaryVelocity = 0.0;
    double finalTrackingTargetDeg = 0.0;
    double finalTrackingVelocityDegPerSec = 0.0;

    const auto trackingDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(900);
    while (std::chrono::steady_clock::now() < trackingDeadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                       (void*)&initialActualRaw, static_cast<int>(sizeof(initialActualRaw))),
                       "simulation should allow holding actual position for tracking-clamp regression")) {
                return EXIT_FAILURE;
            }
        }

        int32_t targetPosRaw = 0;
        int32_t targetVelRaw = 0;
        if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetPos, targetPosRaw),
                   "tracking-clamp regression should expose logical target position bytes") ||
            !Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                   "tracking-clamp regression should expose CSV target velocity bytes")) {
            return EXIT_FAILURE;
        }

        finalTrackingTargetDeg = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
        finalTrackingVelocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
        if (finalTrackingTargetDeg >= 29.0 && finalTrackingTargetDeg <= 31.0) {
            sawTrackingBoundary = true;
            minTrackingBoundaryVelocity = (std::min)(minTrackingBoundaryVelocity, finalTrackingVelocityDegPerSec);
            if (finalTrackingVelocityDegPerSec < -1.0) {
                sawTrackingRecoveryVelocity = true;
            }
        }

        if (sawTrackingBoundary && sawTrackingRecoveryVelocity) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Check(sawTrackingBoundary,
               "tracking-clamp regression should drive the logical target to actual minus tracking window") ||
        !Check(sawTrackingRecoveryVelocity,
               "CSV command must keep pulling actual toward a commanded lower tracking boundary instead of zeroing 0x60FF")) {
        std::cerr << "tracking-clamp CSV state:"
                  << " sawTrackingBoundary=" << (sawTrackingBoundary ? 1 : 0)
                  << " minTrackingBoundaryVelocity=" << minTrackingBoundaryVelocity
                  << " finalTarget=" << finalTrackingTargetDeg
                  << " finalVelocity=" << finalTrackingVelocityDegPerSec << '\n';
        return EXIT_FAILURE;
    }

    master.EnablePDOFiltering(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.ResetPDOFilter(slave);
    }
    master.EnablePDOFiltering(true);
    if (!Check(master.IsPDOFilteringEnabled(),
               "PDO filtering should re-enable before the isolated lead-inhibit regression")) {
        return EXIT_FAILURE;
    }

    mmfTargets.fill(40.0f);
    mmfGuard.Write(mmfTargets);

    const int32_t zeroActualRaw = 0;
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&zeroActualRaw, static_cast<int>(sizeof(zeroActualRaw))),
                   "simulation should allow resetting actual position while seeding positive CSV command")) {
            return EXIT_FAILURE;
        }
    }

    bool seededPositiveCommand = false;
    double seededTargetDeg = 0.0;
    double seededVelocityDegPerSec = 0.0;
    double seededActualDeg = 0.0;
    const auto seedDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
    while (std::chrono::steady_clock::now() < seedDeadline) {
        int32_t targetPosRaw = 0;
        int32_t targetVelRaw = 0;
        int32_t actualPosRaw = 0;
        if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetPos, targetPosRaw),
                   "lead-inhibit regression should expose seeded target position bytes") ||
            !Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                   "lead-inhibit regression should expose seeded target velocity bytes") ||
            !Check(SnapshotI32(master, inBase + EtherCATMaster::kPdoInOff_ActualPos, actualPosRaw),
                   "lead-inhibit regression should expose seeded actual position bytes")) {
            return EXIT_FAILURE;
        }

        seededTargetDeg = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
        seededVelocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
        seededActualDeg = static_cast<double>(actualPosRaw) * kDsP100E7Scale;
        if (seededTargetDeg > 12.0 && seededVelocityDegPerSec > 20.0 && seededActualDeg > 1.0) {
            seededPositiveCommand = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Check(seededPositiveCommand,
               "lead-inhibit regression should first build a positive CSV command")) {
        std::cerr << "lead-inhibit seed state: target=" << seededTargetDeg
                  << " actual=" << seededActualDeg
                  << " velocity=" << seededVelocityDegPerSec << '\n';
        return EXIT_FAILURE;
    }

    mmfTargets.fill(0.0f);
    mmfGuard.Write(mmfTargets);

    bool sawLeadInhibit = false;
    bool sawActualLead = false;
    double maxPositiveLeadVelocity = 0.0;
    double finalLeadTargetDeg = seededTargetDeg;
    double finalLeadVelocityDegPerSec = seededVelocityDegPerSec;
    double finalLeadActualDeg = seededActualDeg;
    const auto leadDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    while (std::chrono::steady_clock::now() < leadDeadline) {
        int32_t targetPosRaw = 0;
        if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetPos, targetPosRaw),
                   "lead-inhibit regression should expose current target position bytes")) {
            return EXIT_FAILURE;
        }

        const double targetDeg = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
        int32_t targetVelRaw = 0;
        int32_t actualPosRaw = 0;
        if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                   "lead-inhibit regression should expose CSV target velocity bytes") ||
            !Check(SnapshotI32(master, inBase + EtherCATMaster::kPdoInOff_ActualPos, actualPosRaw),
                   "lead-inhibit regression should expose actual position bytes")) {
            return EXIT_FAILURE;
        }

        finalLeadTargetDeg = targetDeg;
        finalLeadVelocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
        finalLeadActualDeg = static_cast<double>(actualPosRaw) * kDsP100E7Scale;
        if (finalLeadActualDeg > finalLeadTargetDeg + 0.5) {
            sawActualLead = true;
            maxPositiveLeadVelocity = (std::max)(maxPositiveLeadVelocity, finalLeadVelocityDegPerSec);
        }
        if (sawActualLead && finalLeadActualDeg > finalLeadTargetDeg + 0.5 && finalLeadVelocityDegPerSec <= 0.5) {
            sawLeadInhibit = true;
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!Check(sawLeadInhibit,
               "CSV command must clear outward velocity when actual feedback leads the command target")) {
        std::cerr << "lead-inhibit CSV state:"
                << " sawActualLead=" << (sawActualLead ? 1 : 0)
                  << " seededTarget=" << seededTargetDeg
                << " seededActual=" << seededActualDeg
                  << " seededVelocity=" << seededVelocityDegPerSec
                  << " finalTarget=" << finalLeadTargetDeg
                  << " finalActual=" << finalLeadActualDeg
                  << " finalVelocity=" << finalLeadVelocityDegPerSec
                  << " maxPositiveLeadVelocity=" << maxPositiveLeadVelocity << '\n';
        return EXIT_FAILURE;
    }

    master.EnablePDOFiltering(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    mmfTargets.fill(0.0f);
    mmfGuard.Write(mmfTargets);
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.ResetPDOFilter(slave);
        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&zeroActualRaw, static_cast<int>(sizeof(zeroActualRaw))),
                   "simulation should allow resetting actual position before hard-limit regression")) {
            return EXIT_FAILURE;
        }
    }
    master.EnablePDOFiltering(true);
    if (!Check(master.IsPDOFilteringEnabled(),
               "PDO filtering should re-enable before the isolated hard-limit regression")) {
        return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    mmfTargets.fill(-120.0f);
    mmfGuard.Write(mmfTargets);

    constexpr double kLowerProtectedEdgeDeg = -63.0;
    bool sawLowerProtectedTarget = false;
    bool sawActualAtProtectedEdge = false;
    double minTargetDeg = 1000.0;
    double minActualDeg = 1000.0;
    double mostNegativeVelocityAtEdge = 0.0;
    double mostNegativeVelocityPastProtectedEdge = 0.0;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3500);
    while (std::chrono::steady_clock::now() < deadline) {
        int32_t targetPosRaw = 0;
        int32_t targetVelRaw = 0;
        int32_t actualPosRaw = 0;
        if (!Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetPos, targetPosRaw),
                   "simulation should expose logical target position bytes") ||
            !Check(SnapshotI32(master, outBase + EtherCATMaster::kPdoOutOff_TargetVel, targetVelRaw),
                   "simulation should expose CSV target velocity bytes") ||
            !Check(SnapshotI32(master, inBase + EtherCATMaster::kPdoInOff_ActualPos, actualPosRaw),
                   "simulation should expose actual position bytes")) {
            return EXIT_FAILURE;
        }

        const double targetDeg = static_cast<double>(targetPosRaw) * kDsP100E7Scale;
        const double velocityDegPerSec = static_cast<double>(targetVelRaw) * kDsP100E7Scale;
        const double actualDeg = static_cast<double>(actualPosRaw) * kDsP100E7Scale;

        minTargetDeg = (std::min)(minTargetDeg, targetDeg);
        minActualDeg = (std::min)(minActualDeg, actualDeg);

        if (targetDeg <= kLowerProtectedEdgeDeg + 0.1) {
            sawLowerProtectedTarget = true;
        }
        if (targetDeg <= kLowerProtectedEdgeDeg + 0.1 && actualDeg <= kLowerProtectedEdgeDeg + 0.1) {
            mostNegativeVelocityAtEdge = (std::min)(mostNegativeVelocityAtEdge, velocityDegPerSec);
        }
        if (actualDeg <= kLowerProtectedEdgeDeg) {
            sawActualAtProtectedEdge = true;
            mostNegativeVelocityPastProtectedEdge = (std::min)(mostNegativeVelocityPastProtectedEdge, velocityDegPerSec);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

        if (!Check(sawLowerProtectedTarget,
             "hard-limit regression should drive the logical target to the lower protected PDO edge") ||
         !Check(minTargetDeg >= kLowerProtectedEdgeDeg - 0.05,
             "logical target position must not pass the lower protected PDO edge") ||
        !Check(mostNegativeVelocityAtEdge >= -30.0,
             "CSV command must not retain a large outward velocity once the target is pinned at the lower protected edge") ||
         !Check(!sawActualAtProtectedEdge || mostNegativeVelocityPastProtectedEdge >= -1.0,
             "CSV command must clear outward velocity once actual position reaches or passes the lower protected edge") ||
         !Check(minActualDeg >= kLowerProtectedEdgeDeg - 0.5,
             "simulation actual position should not run away past the lower protected edge")) {
        const auto filterInputs = master.GetPDOFilterInputAngles();
        std::cerr << "hard-limit CSV state:"
                << " sawLowerProtectedTarget=" << (sawLowerProtectedTarget ? 1 : 0)
                << " sawActualAtProtectedEdge=" << (sawActualAtProtectedEdge ? 1 : 0)
                  << " minTarget=" << minTargetDeg
                  << " minActual=" << minActualDeg
                  << " mostNegativeVelocityAtEdge=" << mostNegativeVelocityAtEdge
                << " mostNegativeVelocityPastProtectedEdge=" << mostNegativeVelocityPastProtectedEdge
                  << " filterInput0=" << filterInputs[0] << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "[csv-hard-limit] minTarget=" << minTargetDeg
              << " minActual=" << minActualDeg
              << " edgeVelocity=" << mostNegativeVelocityAtEdge
              << " pastEdgeVelocity=" << mostNegativeVelocityPastProtectedEdge << '\n';
    return EXIT_SUCCESS;
}