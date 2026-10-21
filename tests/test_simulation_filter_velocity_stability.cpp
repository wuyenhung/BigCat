#include "EtherCATMaster.h"
#include "MMFWriter.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kMmfDegMappingName[] = L"OUTPUT_MMF_DEG";
constexpr wchar_t kPdoMmfMappingName[] = L"Local\\WuEtherCAT_PDO_TEST_FILTER_VELOCITY";
constexpr int kAxisCount = 6;
constexpr double kDsP100E7Scale = 0.0000274658203125;

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
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

bool WriteActualPositionDeg(EtherCATMaster& master, double actualPositionDeg) {
    const int32_t actualRaw = static_cast<int32_t>(std::lround(actualPositionDeg / kDsP100E7Scale));
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        if (!master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                             (void*)&actualRaw, static_cast<int>(sizeof(actualRaw)))) {
            return false;
        }
    }
    return true;
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

    void Write(const std::array<float, kAxisCount>& values) const {
        if (view) {
            std::memcpy(view, values.data(), sizeof(float) * kAxisCount);
        }
    }

    bool IsReady() const {
        return mapping != nullptr && view != nullptr;
    }
};

struct PdoMmfViewGuard {
    HANDLE mapping = nullptr;
    MMFWriter::MMFData* view = nullptr;

    explicit PdoMmfViewGuard(const wchar_t* mappingName) {
        mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
        if (!mapping) {
            return;
        }
        view = static_cast<MMFWriter::MMFData*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(MMFWriter::MMFData)));
        if (!view) {
            CloseHandle(mapping);
            mapping = nullptr;
        }
    }

    ~PdoMmfViewGuard() {
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
    std::array<float, kAxisCount> mmfTargets{};
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
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 80.0, 100.0, 100.0, 2500.0);
        master.SetPDOFilterLimits(slave, -65.0, 65.0);
    }

    if (!Check(master.StartMMF1In(),
               "MMF1 input reader should start successfully") ||
        !Check(master.EnableMMFOutput(kPdoMmfMappingName),
               "PDO MMF output should start for telemetry sampling") ||
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

    master.EnablePDOFiltering(true);
    if (!Check(master.IsPDOFilteringEnabled(),
               "PDO filtering should enable successfully once simulation is operational")) {
        return EXIT_FAILURE;
    }
    master.TriggerAllPDOFiltersSoftStart();

    PdoMmfViewGuard pdoView(kPdoMmfMappingName);
    if (!Check(pdoView.IsReady(), "PDO MMF telemetry mapping should be readable by the test")) {
        return EXIT_FAILURE;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const auto softStartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < softStartDeadline &&
           !master.AreAllPDOFiltersSoftStartReady_Cached()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!Check(master.AreAllPDOFiltersSoftStartReady_Cached(),
               "simulation moving-sine regression should complete soft-start before telemetry sampling")) {
        return EXIT_FAILURE;
    }
    master.CompleteAllPDOFiltersSoftStart();
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    double previousVelocity = std::numeric_limits<double>::quiet_NaN();
    double previousTargetVelocity = std::numeric_limits<double>::quiet_NaN();
    double maxVelocityJump = 0.0;
    double maxTargetVelocityJump = 0.0;
    double maxTargetVelocityJumpPrev = 0.0;
    double maxTargetVelocityJumpCur = 0.0;
    double maxTargetVelocityJumpFilter = 0.0;
    double maxAbsTargetFilterVelocityError = 0.0;
    double maxInjectedLeadTargetFilterVelocityError = 0.0;
    double minInjectedLeadTargetFilterVelocityRatio = std::numeric_limits<double>::infinity();
    double maxAbsVelocity = 0.0;
    double maxAbsTargetVelocity = 0.0;
    double maxAbsAcceleration = 0.0;
    double maxAbsActualFilterError = 0.0;
    double maxAbsActualFilterRipple = 0.0;
    double maxAbsActualVelocity = 0.0;
    double smoothedActualFilterError = 0.0;
    bool haveSmoothedActualFilterError = false;
    int samples = 0;

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(2200);
    auto nextTargetUpdate = start;
    constexpr double amplitudeDeg = 20.0;
    constexpr double periodSec = 2.0;
    constexpr double omega = 2.0 * 3.14159265358979323846 / periodSec;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto now = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double>(now - start).count();
        if (now >= nextTargetUpdate) {
            const float target = static_cast<float>(amplitudeDeg * std::sin(omega * elapsedSec));
            mmfTargets.fill(target);
            mmfGuard.Write(mmfTargets);
            nextTargetUpdate = now + std::chrono::milliseconds(10);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (pdoView.view->magic != MMFWriter::MAGIC || pdoView.view->version != MMFWriter::VERSION) {
            continue;
        }

        const auto& slave = pdoView.view->slaves[0];
        const double targetVelocity = slave.targetVelocity;
        const double actualPosition = slave.actualPosition;
        const double actualVelocity = slave.actualVelocity;
        const double filterOutput = slave.pdoFilterOutput;
        const double filterVelocity = slave.pdoFilterVel;
        const double filterAcceleration = slave.pdoFilterAcc;
        if (!std::isfinite(targetVelocity) ||
            !std::isfinite(actualPosition) ||
            !std::isfinite(actualVelocity) ||
            !std::isfinite(filterOutput) ||
            !std::isfinite(filterVelocity) ||
            !std::isfinite(filterAcceleration)) {
            std::cerr << "non-finite velocity telemetry target=" << targetVelocity
                      << " actualPos=" << actualPosition
                      << " actualVel=" << actualVelocity
                      << " filterOutput=" << filterOutput
                      << " filter=" << filterVelocity
                      << " acceleration=" << filterAcceleration << '\n';
            return EXIT_FAILURE;
        }

        maxAbsTargetVelocity = (std::max)(maxAbsTargetVelocity, std::abs(targetVelocity));
        maxAbsActualVelocity = (std::max)(maxAbsActualVelocity, std::abs(actualVelocity));
        const double actualFilterError = actualPosition - filterOutput;
        maxAbsActualFilterError = (std::max)(maxAbsActualFilterError, std::abs(actualFilterError));
        if (!haveSmoothedActualFilterError) {
            smoothedActualFilterError = actualFilterError;
            haveSmoothedActualFilterError = true;
        } else {
            constexpr double trackingRippleAlpha = 0.03;
            smoothedActualFilterError += trackingRippleAlpha * (actualFilterError - smoothedActualFilterError);
            maxAbsActualFilterRipple =
                (std::max)(maxAbsActualFilterRipple, std::abs(actualFilterError - smoothedActualFilterError));
        }
        maxAbsVelocity = (std::max)(maxAbsVelocity, std::abs(filterVelocity));
        maxAbsTargetFilterVelocityError = (std::max)(
            maxAbsTargetFilterVelocityError,
            std::abs(targetVelocity - filterVelocity));
        const bool injectedLeadWindow = elapsedSec >= 0.65 && elapsedSec <= 1.15 && std::abs(filterVelocity) > 12.0;
        if (injectedLeadWindow) {
            const double injectedLeadActualDeg = filterOutput + std::copysign(0.8, filterVelocity);
            if (!Check(WriteActualPositionDeg(master, injectedLeadActualDeg),
                       "simulation should allow moving-feedback-lead injection")) {
                return EXIT_FAILURE;
            }
            maxInjectedLeadTargetFilterVelocityError = (std::max)(
                maxInjectedLeadTargetFilterVelocityError,
                std::abs(targetVelocity - filterVelocity));
            minInjectedLeadTargetFilterVelocityRatio = (std::min)(
                minInjectedLeadTargetFilterVelocityRatio,
                std::abs(targetVelocity) / (std::abs(filterVelocity) + 1e-9));
        }
        maxAbsAcceleration = (std::max)(maxAbsAcceleration, std::abs(filterAcceleration));
        if (std::isfinite(previousTargetVelocity)) {
            const double targetVelocityJump = std::abs(targetVelocity - previousTargetVelocity);
            if (targetVelocityJump > maxTargetVelocityJump) {
                maxTargetVelocityJump = targetVelocityJump;
                maxTargetVelocityJumpPrev = previousTargetVelocity;
                maxTargetVelocityJumpCur = targetVelocity;
                maxTargetVelocityJumpFilter = filterVelocity;
            }
        }
        if (std::isfinite(previousVelocity)) {
            maxVelocityJump = (std::max)(maxVelocityJump, std::abs(filterVelocity - previousVelocity));
        }
        previousTargetVelocity = targetVelocity;
        previousVelocity = filterVelocity;
        ++samples;
    }

    if (!Check(samples >= 800, "simulation filter velocity test should collect enough telemetry samples")) {
        return EXIT_FAILURE;
    }

    if (maxAbsTargetVelocity > 85.0 ||
        maxAbsActualVelocity > 95.0 ||
        maxAbsVelocity > 85.0 ||
        maxAbsAcceleration > 125.0 ||
        maxAbsActualFilterError > 12.5 ||
        maxAbsActualFilterRipple > 1.25 ||
        maxAbsTargetFilterVelocityError > 12.0 ||
        maxInjectedLeadTargetFilterVelocityError > 12.0 ||
        minInjectedLeadTargetFilterVelocityRatio < 0.75 ||
        maxTargetVelocityJump > 3.0 ||
        maxVelocityJump > 3.0) {
        std::cerr << "unstable filter velocity telemetry:"
                  << " samples=" << samples
                  << " maxAbsTargetVelocity=" << maxAbsTargetVelocity
                  << " maxAbsActualVelocity=" << maxAbsActualVelocity
                  << " maxAbsActualFilterError=" << maxAbsActualFilterError
                  << " maxAbsActualFilterRipple=" << maxAbsActualFilterRipple
                  << " maxAbsVelocity=" << maxAbsVelocity
                  << " maxAbsTargetFilterVelocityError=" << maxAbsTargetFilterVelocityError
                  << " maxInjectedLeadTargetFilterVelocityError=" << maxInjectedLeadTargetFilterVelocityError
                  << " minInjectedLeadTargetFilterVelocityRatio=" << minInjectedLeadTargetFilterVelocityRatio
                  << " maxAbsAcceleration=" << maxAbsAcceleration
                  << " maxTargetVelocityJump=" << maxTargetVelocityJump
                  << " targetJumpPrev=" << maxTargetVelocityJumpPrev
                  << " targetJumpCur=" << maxTargetVelocityJumpCur
                  << " targetJumpFilter=" << maxTargetVelocityJumpFilter
                  << " maxVelocityJump=" << maxVelocityJump << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
