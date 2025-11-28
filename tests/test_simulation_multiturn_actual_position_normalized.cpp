#include "EtherCATMaster.h"

#include <windows.h>

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

bool ParseMetric(const std::wstring& text, double& outValue) {
    outValue = 0.0;
    if (text.empty() || text == L"N/A") {
        return false;
    }
    wchar_t* end = nullptr;
    const double value = std::wcstod(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(value)) {
        return false;
    }
    outValue = value;
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
    const std::array<float, kAxisCount> mmfTargets = {30.0f, 30.0f, 30.0f, 30.0f, 30.0f, 30.0f};
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
    std::array<std::atomic<bool>, kAxisCount> fatalTrackingSeen{};
    for (auto& value : fatalTrackingSeen) {
        value.store(false, std::memory_order_relaxed);
    }

    const int32_t multiTurnActualRaw = static_cast<int32_t>(std::llround(-35736.3245 / kDsP100E7Scale));
    for (int slave = 1; slave <= kAxisCount; ++slave) {
        master.RegisterPDODescriptors(slave, descriptors);
        master.SetMechanicalScalingFactors(slave, kDsP100E7Scale, false);
        master.ConfigurePDOFilter(slave, 180.0, 200.0, 200.0, 1000.0);
        master.SetPDOFilterLimits(slave, -1000.0, 1000.0);
        master.SetPDOFilterViolationCallback(
            slave,
            [&fatalTrackingSeen, slave](PDOFilter::ViolationType type, double, double, bool, double duration) {
                if (duration >= 0.5 && PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) {
                    fatalTrackingSeen[slave - 1].store(true, std::memory_order_relaxed);
                }
            });

        if (!Check(master.WriteSDO(static_cast<uint16_t>(slave), 0x6064, 0x00,
                                   (void*)&multiTurnActualRaw, static_cast<int>(sizeof(multiTurnActualRaw))),
                   "simulation should allow test setup of multi-turn actual position")) {
            return EXIT_FAILURE;
        }
    }

    if (!Check(master.StartMMF1In(),
               "MMF1 input reader should start successfully") ||
        !Check(master.StartCyclicOperation(),
               "simulation cyclic operation should start successfully")) {
        return EXIT_FAILURE;
    }

    bool sawNormalizedMetric = false;
    const auto metricDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < metricDeadline && !sawNormalizedMetric) {
        sawNormalizedMetric = true;
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            MechanicalUnitMetrics metrics;
            double actualPosition = 0.0;
            if (!master.GetMechanicalUnitMetrics(slave, metrics) ||
                !metrics.valid ||
                !ParseMetric(metrics.actualPosition, actualPosition) ||
                std::abs(actualPosition) > 180.0) {
                sawNormalizedMetric = false;
                break;
            }
        }
        if (!sawNormalizedMetric) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (!Check(sawNormalizedMetric,
               "mechanical actual position metrics should normalize multi-turn raw feedback to a bounded single-turn angle")) {
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

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(650);
    while (std::chrono::steady_clock::now() < deadline) {
        for (int slave = 1; slave <= kAxisCount; ++slave) {
            PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
            bool isError = false;
            bool isTrackingFatal = false;
            if (!Check(master.GetPDOFilterState(slave, violationType, isError, isTrackingFatal),
                       "GetPDOFilterState should succeed during multi-turn simulation soft-start")) {
                return EXIT_FAILURE;
            }
            if (!Check(!isError, "multi-turn normalized soft-start should not latch PDOFilter error state") ||
                !Check(!isTrackingFatal, "multi-turn normalized soft-start should not latch fatal tracking error") ||
                !Check(!fatalTrackingSeen[slave - 1].load(std::memory_order_relaxed),
                       "multi-turn normalized soft-start should not report fatal tracking while feedback follows CSV command")) {
                return EXIT_FAILURE;
            }

            MechanicalUnitMetrics metrics;
            double actualPosition = 0.0;
            if (!Check(master.GetMechanicalUnitMetrics(slave, metrics) && metrics.valid && ParseMetric(metrics.actualPosition, actualPosition),
                       "multi-turn simulation soft-start should keep actual metrics available") ||
                !Check(std::abs(actualPosition) <= 180.0,
                       "multi-turn simulation soft-start actual metric should remain bounded to single-turn angle")) {
                return EXIT_FAILURE;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return EXIT_SUCCESS;
}