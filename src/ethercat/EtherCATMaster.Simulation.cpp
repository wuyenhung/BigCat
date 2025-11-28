#include "EtherCATMaster.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mmsystem.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "SafeMem.h"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

namespace {

constexpr int kSimulationSlaveCount = 6;
constexpr int kSimulationOutputBytesPerSlave = 12;
constexpr int kSimulationInputBytesPerSlave = 12;
constexpr int kSimulationTotalOutputBytes = kSimulationSlaveCount * kSimulationOutputBytesPerSlave;
constexpr uint32_t kSimulationVendorId = 0x5755534Fu; // "WUSO"
constexpr uint32_t kSimulationProductCode = 0x00060001u;
constexpr uint32_t kSimulationRevision = 0x00010000u;
constexpr double kSimulationServoMaxVelocityCountsPerSec = 12000000.0;
constexpr double kSimulationServoMaxAccelerationCountsPerSec2 = 95000000.0;
constexpr double kSimulationServoQuickStopAccelerationCountsPerSec2 = 140000000.0;
constexpr double kSimulationHomingMaxVelocityCountsPerSec = 2500000.0;
constexpr double kSimulationPositionLoopKp = 35.0;
constexpr double kSimulationPositionLoopKi = 12.0;
constexpr double kSimulationPositionLoopKd = 0.55;
constexpr double kSimulationVelocityLoopKp = 110.0;
constexpr double kSimulationVelocityLoopKi = 900.0;
constexpr double kSimulationHomingMinActiveSec = 0.08;
constexpr double kSimulationHomingMaxActiveSec = 0.60;
constexpr double kSimulationHomingCompleteWindowCounts = 25000.0;
constexpr double kSimulationHomingCompleteVelocityCountsPerSec = 250000.0;
constexpr double kSimulationCsvSettleWindowCounts = 10000.0;
constexpr double kSimulationCsvSettleVelocityCountsPerSec = 50000.0;
constexpr double kSimulationCsvSettleTargetDeltaCounts = 100.0;
constexpr double kSimulationCsvSettleStableSec = 0.25;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using CreateWaitableTimerExWFn = HANDLE (WINAPI *)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

HANDLE CreateSimulationWaitTimer()
{
    static CreateWaitableTimerExWFn createExFn = reinterpret_cast<CreateWaitableTimerExWFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW"));
    if (createExFn) {
        HANDLE timer = createExFn(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer) {
            return timer;
        }
    }
    return CreateWaitableTimer(nullptr, FALSE, nullptr);
}

template <typename T>
bool CopySimulationValue(const T& value, void* data, int& size)
{
    if (!data || size <= 0) {
        return false;
    }

    const int copyLen = std::min(size, static_cast<int>(sizeof(T)));
    std::memset(data, 0, static_cast<size_t>(size));
    std::memcpy(data, &value, static_cast<size_t>(copyLen));
    size = copyLen;
    return true;
}

bool CopySimulationBytes(const void* src, int srcLen, void* dst, int& dstLen)
{
    if (!src || srcLen <= 0 || !dst || dstLen <= 0) {
        return false;
    }

    const int copyLen = std::min(srcLen, dstLen);
    std::memset(dst, 0, static_cast<size_t>(dstLen));
    std::memcpy(dst, src, static_cast<size_t>(copyLen));
    dstLen = copyLen;
    return true;
}

uint16_t ReadLe16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

int32_t ReadLe32(const uint8_t* bytes)
{
    uint32_t value = 0;
    value |= static_cast<uint32_t>(bytes[0]);
    value |= static_cast<uint32_t>(bytes[1]) << 8;
    value |= static_cast<uint32_t>(bytes[2]) << 16;
    value |= static_cast<uint32_t>(bytes[3]) << 24;
    return static_cast<int32_t>(value);
}

void WriteLe16(uint8_t* bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value & 0xFFu);
    bytes[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

void WriteLe32(uint8_t* bytes, int32_t value)
{
    const uint32_t raw = static_cast<uint32_t>(value);
    bytes[0] = static_cast<uint8_t>(raw & 0xFFu);
    bytes[1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
    bytes[2] = static_cast<uint8_t>((raw >> 16) & 0xFFu);
    bytes[3] = static_cast<uint8_t>((raw >> 24) & 0xFFu);
}

int32_t RoundSimulationCount(double value)
{
    if (!std::isfinite(value)) {
        return 0;
    }

    const double minValue = static_cast<double>((std::numeric_limits<int32_t>::min)());
    const double maxValue = static_cast<double>((std::numeric_limits<int32_t>::max)());
    value = (std::clamp)(value, minValue, maxValue);
    return static_cast<int32_t>(std::llround(value));
}

void ApplyStagedSimulationOutputs(uint8_t* ioMap,
                                  size_t ioMapSize,
                                  const std::unordered_map<int, std::vector<uint8_t>>& stagedOutputs)
{
    for (const auto& entry : stagedOutputs) {
        const int absOffset = entry.first;
        const auto& staged = entry.second;
        if (absOffset < 0 || absOffset >= static_cast<int>(ioMapSize) || staged.empty()) {
            continue;
        }

        const int maxWrite = std::min(static_cast<int>(staged.size()), static_cast<int>(ioMapSize) - absOffset);
        if (maxWrite > 0) {
            safe_memcpy(ioMap + absOffset, staged.data(), static_cast<size_t>(maxWrite));
        }
    }
}

} // namespace

bool EtherCATMaster::InitializeSimulationLocked(const char* ifnameParam)
{
    if (isRunning) {
        StopCyclicOperation();
    }
    StopPDOProcessingThread();
    try {
        StopMMF1In();
        DisableMMFOutput();
    } catch (...) {
    }

    std::memset(&context, 0, sizeof(context));
    std::memset(IOmap, 0, sizeof(IOmap));
    std::memset(ifname, 0, sizeof(ifname));
    strncpy_s(ifname,
              ifnameParam ? ifnameParam : GetSimulationInterfaceName(),
              sizeof(ifname) - 1);

    simulationMode_.store(true, std::memory_order_relaxed);
    slaveCount = kSimulationSlaveCount;
    context.slavecount = slaveCount;
    workingCounter = 0;
    pdosSentCount.store(0, std::memory_order_relaxed);
    maxObservedCommsTime.store(0, std::memory_order_relaxed);
    observedCycleMicros_.store(0, std::memory_order_relaxed);
    maxObservedCycleMicros_.store(0, std::memory_order_relaxed);
    pdoExchangeActive_.store(false, std::memory_order_relaxed);
    pdoIdleWindowDeadlineUs_.store(0, std::memory_order_relaxed);

    EnterCriticalSection(&simulationCS_);
    simulationAxes_.clear();
    simulationAxes_.resize(slaveCount + 1);

    for (int slave = 1; slave <= slaveCount; ++slave) {
        auto& axis = simulationAxes_[slave];
        axis = SimulationAxisState{};
        axis.name = std::string("Sim Axis ") + std::to_string(slave);

        ec_slavet& simSlave = context.slavelist[slave];
        std::memset(&simSlave, 0, sizeof(simSlave));
        strncpy_s(simSlave.name, axis.name.c_str(), sizeof(simSlave.name) - 1);
        simSlave.state = EC_STATE_PRE_OP;
        simSlave.aliasadr = static_cast<uint16>(slave);
        simSlave.configadr = static_cast<uint16>(0x1000 + slave);
        simSlave.eep_man = kSimulationVendorId;
        simSlave.eep_id = kSimulationProductCode + static_cast<uint32_t>(slave - 1);
        simSlave.eep_rev = kSimulationRevision;
        simSlave.Obytes = kSimulationOutputBytesPerSlave;
        simSlave.Ibytes = kSimulationInputBytesPerSlave;
        simSlave.Obits = kSimulationOutputBytesPerSlave * 8;
        simSlave.Ibits = kSimulationInputBytesPerSlave * 8;

        const int outBase = (slave - 1) * kSimulationOutputBytesPerSlave;
        const int inBase = kSimulationTotalOutputBytes + (slave - 1) * kSimulationInputBytesPerSlave;
        simSlave.outputs = IOmap + outBase;
        simSlave.inputs = IOmap + inBase;
        simSlave.Ostartbit = static_cast<uint16>(outBase * 8);
        simSlave.Istartbit = static_cast<uint16>(inBase * 8);
    }
    LeaveCriticalSection(&simulationCS_);

    context.slavelist[0].state = EC_STATE_PRE_OP;

    try {
        softStartReadyCache_.reset();
        softStartReadyCache_.reset(new std::atomic<char>[slaveCount + 1]);
        softStartReadyCacheSize_ = slaveCount + 1;
        for (int i = 0; i < softStartReadyCacheSize_; ++i) {
            softStartReadyCache_[i].store(0, std::memory_order_relaxed);
        }
    } catch (...) {
        softStartReadyCacheSize_ = 0;
    }

    isInitialized = true;
    isRunning = false;
    SetError("Simulation master initialized with 6 virtual EtherCAT axes");
    return true;
}

bool EtherCATMaster::ConfigureSimulationSlavesLocked()
{
    if (!simulationMode_.load(std::memory_order_relaxed) || !isInitialized) {
        SetError("Simulation master not initialized");
        return false;
    }

    std::memset(IOmap, 0, sizeof(IOmap));

    EnterCriticalSection(&simulationCS_);
    for (int slave = 1; slave <= slaveCount; ++slave) {
        auto& axis = simulationAxes_[slave];
        axis.controlword = 0x0006;
        axis.lastControlword = 0x0006;
        axis.operationMode = CoreHelpers::kDefaultCyclicOperationMode;
        axis.modeDisplay = CoreHelpers::kDefaultCyclicOperationMode;
        axis.errorCode = 0;
        axis.targetPosition = 0;
        axis.targetVelocity = 0;
        axis.actualPositionCounts = 0.0;
        axis.actualVelocityCounts = 0.0;
        axis.positionErrorIntegralCountsSec = 0.0;
        axis.velocityErrorIntegralCounts = 0.0;
        axis.servoAccelerationCounts = 0.0;
        axis.positionVelocityDemandCounts = 0.0;
        axis.homingActive = false;
        axis.homingRemainingSec = 0.0;
        axis.homingElapsedSec = 0.0;
        axis.csvSettleStableSec = 0.0;
        axis.haveLastCsvTargetPosition = false;
        axis.lastCsvTargetPosition = 0;
        context.slavelist[slave].state = EC_STATE_SAFE_OP;
    }
    LeaveCriticalSection(&simulationCS_);

    context.slavelist[0].state = EC_STATE_SAFE_OP;
    InitializePDOData();
    AdvanceSimulation(0.0);
    workingCounter = slaveCount * 2;
    SetError("Simulation slaves configured with virtual PDO mapping");
    return true;
}

bool EtherCATMaster::StartSimulationCyclicOperationLocked()
{
    if (!isInitialized) {
        SetError("Not initialized");
        return false;
    }
    if (isRunning) {
        SetError("Already running");
        return false;
    }

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (stopEvent == NULL) {
        SetError("Failed to create simulation stop event");
        return false;
    }

    timePeriodActive = false;
    if (timeBeginPeriod(1) == TIMERR_NOERROR) {
        timePeriodActive = true;
    }

    isRunning = true;
    operationStartTime = GetTickCount64();
    cyclicThread = CreateThread(NULL, 0, CyclicThreadFunc, this, 0, NULL);
    if (cyclicThread == NULL) {
        isRunning = false;
        CloseHandle(stopEvent);
        stopEvent = NULL;
        if (timePeriodActive) {
            timeEndPeriod(1);
            timePeriodActive = false;
        }
        SetError("Failed to create simulation cyclic thread");
        return false;
    }

    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    for (int slave = 1; slave <= slaveCount; ++slave) {
        context.slavelist[slave].state = EC_STATE_OPERATIONAL;
    }
    workingCounter = slaveCount * 2;
    observedCycleMicros_.store(desiredCycleTime ? desiredCycleTime : 2000, std::memory_order_relaxed);
    StartPDOProcessingThread();
    SetError("Simulation cyclic operation started");
    return true;
}

void EtherCATMaster::ShutdownSimulationLocked(const std::function<void(const std::string&)>& logCallback)
{
    if (logCallback) {
        logCallback("[Simulation] stopping virtual EtherCAT session\n");
    }

    StopPDOProcessingThread();
    StopCyclicOperation();
    try {
        StopMMF1In();
        DisableMMFOutput();
    } catch (...) {
    }

    EnterCriticalSection(&cs);
    EnterCriticalSection(&simulationCS_);
    simulationAxes_.clear();
    LeaveCriticalSection(&simulationCS_);

    std::memset(IOmap, 0, sizeof(IOmap));
    std::memset(&context, 0, sizeof(context));
    workingCounter = 0;
    pdosSentCount.store(0, std::memory_order_relaxed);
    maxObservedCommsTime.store(0, std::memory_order_relaxed);
    observedCycleMicros_.store(0, std::memory_order_relaxed);
    maxObservedCycleMicros_.store(0, std::memory_order_relaxed);
    softStartReadyCache_.reset();
    softStartReadyCacheSize_ = 0;
    isInitialized = false;
    slaveCount = 0;
    simulationMode_.store(false, std::memory_order_relaxed);
    LeaveCriticalSection(&cs);
    SetError("Simulation shutdown complete");
}

bool EtherCATMaster::ReadSimulationSdo(uint16_t slave, uint16_t index, uint8_t subindex, void* data, int& size)
{
    if (!isInitialized || slave == 0 || slave > slaveCount || !data || size <= 0) {
        SetError("Invalid simulation SDO read request");
        return false;
    }

    bool ok = false;
    EnterCriticalSection(&simulationCS_);
    const auto& axis = simulationAxes_[slave];
    const uint16_t statusword = ComputeSimulationStatusword(axis);
    const int32_t actualPosition = RoundSimulationCount(axis.actualPositionCounts);
    const int32_t actualVelocity = RoundSimulationCount(axis.actualVelocityCounts);

    switch (index) {
    case 0x1008: {
        std::string name = axis.name;
        name.push_back('\0');
        ok = CopySimulationBytes(name.data(), static_cast<int>(name.size()), data, size);
        break;
    }
    case 0x1C12:
        if (subindex == 0x00) {
            const uint8_t count = 1;
            ok = CopySimulationValue(count, data, size);
        } else if (subindex == 0x01) {
            const uint16_t assignment = 0x1600;
            ok = CopySimulationValue(assignment, data, size);
        }
        break;
    case 0x1C13:
        if (subindex == 0x00) {
            const uint8_t count = 1;
            ok = CopySimulationValue(count, data, size);
        } else if (subindex == 0x01) {
            const uint16_t assignment = 0x1A00;
            ok = CopySimulationValue(assignment, data, size);
        }
        break;
    case 0x1600: {
        static const uint32_t mappings[] = {0x60400010u, 0x607A0020u, 0x60FF0020u, 0x60710010u};
        if (subindex == 0x00) {
            const uint8_t count = static_cast<uint8_t>(_countof(mappings));
            ok = CopySimulationValue(count, data, size);
        } else if (subindex >= 0x01 && subindex <= _countof(mappings)) {
            ok = CopySimulationValue(mappings[subindex - 1], data, size);
        }
        break;
    }
    case 0x1A00: {
        static const uint32_t mappings[] = {0x60410010u, 0x60640020u, 0x606C0020u, 0x60770010u};
        if (subindex == 0x00) {
            const uint8_t count = static_cast<uint8_t>(_countof(mappings));
            ok = CopySimulationValue(count, data, size);
        } else if (subindex >= 0x01 && subindex <= _countof(mappings)) {
            ok = CopySimulationValue(mappings[subindex - 1], data, size);
        }
        break;
    }
    case 0x603F:
        ok = CopySimulationValue(axis.errorCode, data, size);
        break;
    case 0x6040:
        ok = CopySimulationValue(axis.controlword, data, size);
        break;
    case 0x6041:
        ok = CopySimulationValue(statusword, data, size);
        break;
    case 0x6060:
        ok = CopySimulationValue(axis.operationMode, data, size);
        break;
    case 0x6061:
        ok = CopySimulationValue(axis.modeDisplay, data, size);
        break;
    case 0x6064:
        ok = CopySimulationValue(actualPosition, data, size);
        break;
    case 0x606C:
        ok = CopySimulationValue(actualVelocity, data, size);
        break;
    case 0x607A:
        ok = CopySimulationValue(axis.targetPosition, data, size);
        break;
    case 0x607C:
        ok = CopySimulationValue(axis.homeOffset, data, size);
        break;
    case 0x6098:
        ok = CopySimulationValue(axis.homingMethod, data, size);
        break;
    case 0x60E0:
        ok = CopySimulationValue(axis.softwareLimitMin, data, size);
        break;
    case 0x60E1:
        ok = CopySimulationValue(axis.softwareLimitMax, data, size);
        break;
    case 0x60FD:
        ok = CopySimulationBytes(axis.digitalInputs.data(), static_cast<int>(axis.digitalInputs.size()), data, size);
        break;
    case 0x60FF:
        ok = CopySimulationValue(axis.targetVelocity, data, size);
        break;
    default:
        break;
    }

    LeaveCriticalSection(&simulationCS_);

    if (!ok) {
        std::ostringstream oss;
        oss << "Unsupported simulation SDO read 0x" << std::hex << index << ":" << static_cast<int>(subindex);
        SetError(oss.str());
    }
    return ok;
}

bool EtherCATMaster::WriteSimulationSdo(uint16_t slave, uint16_t index, uint8_t subindex, const void* data, int size)
{
    (void)subindex;
    if (!isInitialized || slave == 0 || slave > slaveCount || !data || size <= 0) {
        SetError("Invalid simulation SDO write request");
        return false;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);

    EnterCriticalSection(&simulationCS_);
    auto& axis = simulationAxes_[slave];

    switch (index) {
    case 0x603F:
        axis.errorCode = (size >= 2) ? ReadLe16(bytes) : 0;
        break;
    case 0x6040:
        axis.controlword = (size >= 2) ? ReadLe16(bytes) : bytes[0];
        if ((axis.controlword & 0x0080u) != 0u) {
            axis.errorCode = 0;
        }
        EnterCriticalSection(&pdoDataCS);
        WriteLe16(IOmap + (slave - 1) * kSimulationOutputBytesPerSlave, axis.controlword);
        LeaveCriticalSection(&pdoDataCS);
        break;
    case 0x6060:
        axis.operationMode = static_cast<int8_t>(bytes[0]);
        axis.modeDisplay = axis.operationMode;
        if (axis.operationMode != 6) {
            axis.homingActive = false;
            axis.homingRemainingSec = 0.0;
            axis.homingElapsedSec = 0.0;
        }
        break;
    case 0x6064: {
        axis.actualPositionCounts = static_cast<double>((size >= 4) ? ReadLe32(bytes) : 0);
        axis.actualVelocityCounts = 0.0;
        axis.positionErrorIntegralCountsSec = 0.0;
        axis.velocityErrorIntegralCounts = 0.0;
        axis.servoAccelerationCounts = 0.0;
        axis.positionVelocityDemandCounts = 0.0;
        axis.homingElapsedSec = 0.0;
        axis.csvSettleStableSec = 0.0;
        axis.haveLastCsvTargetPosition = false;
        const int inBase = kSimulationTotalOutputBytes + (slave - 1) * kSimulationInputBytesPerSlave;
        EnterCriticalSection(&pdoDataCS);
        WriteLe32(IOmap + inBase + 2, RoundSimulationCount(axis.actualPositionCounts));
        WriteLe32(IOmap + inBase + 6, 0);
        LeaveCriticalSection(&pdoDataCS);
        break;
    }
    case 0x607A:
        axis.targetPosition = (size >= 4) ? ReadLe32(bytes) : 0;
        EnterCriticalSection(&pdoDataCS);
        WriteLe32(IOmap + (slave - 1) * kSimulationOutputBytesPerSlave + 2, axis.targetPosition);
        LeaveCriticalSection(&pdoDataCS);
        break;
    case 0x607C:
        axis.homeOffset = (size >= 4) ? ReadLe32(bytes) : 0;
        break;
    case 0x6098:
        axis.homingMethod = static_cast<int8_t>(bytes[0]);
        break;
    case 0x60E0:
        axis.softwareLimitMin = (size >= 4) ? static_cast<uint32_t>(ReadLe32(bytes)) : 0u;
        break;
    case 0x60E1:
        axis.softwareLimitMax = (size >= 4) ? static_cast<uint32_t>(ReadLe32(bytes)) : 0u;
        break;
    case 0x60FF:
        axis.targetVelocity = (size >= 4) ? ReadLe32(bytes) : 0;
        EnterCriticalSection(&pdoDataCS);
        WriteLe32(IOmap + (slave - 1) * kSimulationOutputBytesPerSlave + 6, axis.targetVelocity);
        LeaveCriticalSection(&pdoDataCS);
        break;
    default:
        // Allow unrelated configuration writes so the standard connect flow can continue.
        break;
    }

    LeaveCriticalSection(&simulationCS_);
    return true;
}

void EtherCATMaster::AdvanceSimulation(double dtSeconds)
{
    if (!simulationMode_.load(std::memory_order_relaxed) || !isInitialized) {
        return;
    }

    const uint32_t configuredCycleUs = desiredCycleTime ? desiredCycleTime : 2000;
    const double nominalDtSeconds = std::max(0.0005, static_cast<double>(configuredCycleUs) / 1000000.0);
    if (dtSeconds <= 0.0) {
        dtSeconds = nominalDtSeconds;
    } else {
        const double maxIntegrationDtSeconds = std::max(0.005, nominalDtSeconds * 5.0);
        dtSeconds = std::min(dtSeconds, maxIntegrationDtSeconds);
    }

    EnterCriticalSection(&simulationCS_);
    EnterCriticalSection(&pdoDataCS);

    const auto resetServoState = [](SimulationAxisState& axis) {
        axis.positionErrorIntegralCountsSec = 0.0;
        axis.velocityErrorIntegralCounts = 0.0;
        axis.servoAccelerationCounts = 0.0;
        axis.positionVelocityDemandCounts = 0.0;
    };

    const auto stepVelocityLoop = [dtSeconds](SimulationAxisState& axis,
                                              double demandedVelocity,
                                              double maxAcceleration) {
        if (!std::isfinite(demandedVelocity)) {
            demandedVelocity = 0.0;
        }
        demandedVelocity = (std::clamp)(demandedVelocity,
                                        -kSimulationServoMaxVelocityCountsPerSec,
                                        kSimulationServoMaxVelocityCountsPerSec);

        const double previousVelocity = axis.actualVelocityCounts;
        const double maxVelocityStep = (std::max)(0.0, maxAcceleration) * dtSeconds;
        const double velocityDelta = demandedVelocity - previousVelocity;
        if (maxVelocityStep > 0.0) {
            axis.actualVelocityCounts += (std::clamp)(velocityDelta, -maxVelocityStep, maxVelocityStep);
        } else {
            axis.actualVelocityCounts = demandedVelocity;
        }

        if (std::fabs(demandedVelocity) < 1.0 && std::fabs(axis.actualVelocityCounts) < 2.0) {
            axis.actualVelocityCounts = 0.0;
        }
        axis.velocityErrorIntegralCounts = 0.0;
        axis.servoAccelerationCounts = (dtSeconds > 0.0)
            ? ((axis.actualVelocityCounts - previousVelocity) / dtSeconds)
            : 0.0;

        axis.actualPositionCounts += axis.actualVelocityCounts * dtSeconds;
    };

    const auto stepPositionLoop = [dtSeconds, &stepVelocityLoop](SimulationAxisState& axis,
                                                                 double targetPosition,
                                                                 double maxVelocity,
                                                                 double maxAcceleration) {
        if (!std::isfinite(targetPosition)) {
            targetPosition = axis.actualPositionCounts;
        }

        const double positionError = targetPosition - axis.actualPositionCounts;
        const double positionIntegralLimit = kSimulationServoMaxVelocityCountsPerSec /
                                             kSimulationPositionLoopKi;
        axis.positionErrorIntegralCountsSec += positionError * dtSeconds;
        axis.positionErrorIntegralCountsSec = (std::clamp)(axis.positionErrorIntegralCountsSec,
                                                          -positionIntegralLimit,
                                                          positionIntegralLimit);

        double velocityDemand = kSimulationPositionLoopKp * positionError +
                                kSimulationPositionLoopKi * axis.positionErrorIntegralCountsSec -
                                kSimulationPositionLoopKd * axis.actualVelocityCounts;
        const double brakingVelocity = std::sqrt((std::max)(0.0, 2.0 * maxAcceleration * std::fabs(positionError)));
        const double velocityLimit = (std::min)(maxVelocity, brakingVelocity);
        velocityDemand = (std::clamp)(velocityDemand, -velocityLimit, velocityLimit);
        axis.positionVelocityDemandCounts = velocityDemand;

        stepVelocityLoop(axis, velocityDemand, maxAcceleration);
        const double settledError = targetPosition - axis.actualPositionCounts;
        if (std::fabs(settledError) < 2.0 && std::fabs(axis.actualVelocityCounts) < 8.0) {
            axis.actualPositionCounts = targetPosition;
            axis.actualVelocityCounts = 0.0;
            axis.positionErrorIntegralCountsSec = 0.0;
            axis.velocityErrorIntegralCounts = 0.0;
            axis.servoAccelerationCounts = 0.0;
            axis.positionVelocityDemandCounts = 0.0;
        }
    };

    for (int slave = 1; slave <= slaveCount; ++slave) {
        auto& axis = simulationAxes_[slave];
        const int outBase = (slave - 1) * kSimulationOutputBytesPerSlave;
        const int inBase = kSimulationTotalOutputBytes + (slave - 1) * kSimulationInputBytesPerSlave;

        axis.controlword = ReadLe16(IOmap + outBase);
        axis.targetPosition = ReadLe32(IOmap + outBase + 2);
        axis.targetVelocity = ReadLe32(IOmap + outBase + 6);

        if ((axis.controlword & 0x0080u) != 0u) {
            axis.errorCode = 0;
        }

        const bool homingMode = axis.operationMode == CoreHelpers::kCia402OperationModeHoming;
        const bool homingCommand = homingMode && ((axis.controlword & 0x0010u) != 0u);
        const bool homingRisingEdge = homingCommand && ((axis.lastControlword & 0x0010u) == 0u);
        if (homingRisingEdge) {
            axis.homingActive = true;
            axis.homingRemainingSec = kSimulationHomingMinActiveSec;
            axis.homingElapsedSec = 0.0;
            axis.positionErrorIntegralCountsSec = 0.0;
            axis.velocityErrorIntegralCounts = 0.0;
            axis.positionVelocityDemandCounts = 0.0;
        }

        const bool operationEnabled =
            ((axis.controlword & 0x000Fu) == 0x000Fu) || ((axis.controlword & 0x001Fu) == 0x001Fu);

        if (!homingMode) {
            axis.homingActive = false;
            axis.homingRemainingSec = 0.0;
            axis.homingElapsedSec = 0.0;
            axis.csvSettleStableSec = 0.0;
            axis.haveLastCsvTargetPosition = false;
        }

        if (axis.homingActive && homingCommand && operationEnabled) {
            const double target = static_cast<double>(axis.homeOffset);
            stepPositionLoop(axis,
                             target,
                             kSimulationHomingMaxVelocityCountsPerSec,
                             kSimulationServoMaxAccelerationCountsPerSec2);
            axis.homingElapsedSec += dtSeconds;
            axis.homingRemainingSec = std::max(0.0, kSimulationHomingMinActiveSec - axis.homingElapsedSec);
            const double delta = target - axis.actualPositionCounts;
            const bool minActiveElapsed = axis.homingElapsedSec >= kSimulationHomingMinActiveSec;
            const bool closeEnough = std::fabs(delta) <= kSimulationHomingCompleteWindowCounts &&
                                     std::fabs(axis.actualVelocityCounts) <= kSimulationHomingCompleteVelocityCountsPerSec;
            const bool maxActiveElapsed = axis.homingElapsedSec >= kSimulationHomingMaxActiveSec;
            if (minActiveElapsed && (closeEnough || maxActiveElapsed)) {
                axis.actualPositionCounts = target;
                axis.actualVelocityCounts = 0.0;
                axis.homingActive = false;
                axis.homingRemainingSec = 0.0;
                axis.homingElapsedSec = 0.0;
                axis.positionErrorIntegralCountsSec = 0.0;
                axis.velocityErrorIntegralCounts = 0.0;
                axis.servoAccelerationCounts = 0.0;
                axis.positionVelocityDemandCounts = 0.0;
                axis.csvSettleStableSec = 0.0;
                axis.haveLastCsvTargetPosition = false;
            }
        } else if (!operationEnabled) {
            axis.homingActive = false;
            axis.homingRemainingSec = 0.0;
            axis.homingElapsedSec = 0.0;
            axis.csvSettleStableSec = 0.0;
            axis.haveLastCsvTargetPosition = false;
            resetServoState(axis);
            stepVelocityLoop(axis, 0.0, kSimulationServoQuickStopAccelerationCountsPerSec2);
        } else if (homingMode) {
            axis.homingActive = false;
            axis.homingRemainingSec = 0.0;
            axis.homingElapsedSec = 0.0;
            axis.csvSettleStableSec = 0.0;
            axis.haveLastCsvTargetPosition = false;
            resetServoState(axis);
            stepVelocityLoop(axis, 0.0, kSimulationServoQuickStopAccelerationCountsPerSec2);
        } else if (CoreHelpers::IsVelocityDrivenOperationMode(axis.operationMode)) {
            const double requestedTargetVelocity = static_cast<double>(axis.targetVelocity);
            double targetVelocity = requestedTargetVelocity;
            if ((targetVelocity > 0.0 && axis.actualPositionCounts >= static_cast<double>(axis.targetPosition)) ||
                (targetVelocity < 0.0 && axis.actualPositionCounts <= static_cast<double>(axis.targetPosition))) {
                targetVelocity = 0.0;
                axis.targetVelocity = 0;
                WriteLe32(IOmap + outBase + 6, 0);
            }
            const double targetPosition = static_cast<double>(axis.targetPosition);
            const double targetDelta = axis.haveLastCsvTargetPosition
                ? std::fabs(targetPosition - static_cast<double>(axis.lastCsvTargetPosition))
                : std::numeric_limits<double>::infinity();
            const double positionError = targetPosition - axis.actualPositionCounts;
            const bool csvHoldCandidate =
                targetDelta <= kSimulationCsvSettleTargetDeltaCounts &&
                std::fabs(targetVelocity) <= kSimulationCsvSettleVelocityCountsPerSec &&
                std::fabs(axis.actualVelocityCounts) <= kSimulationCsvSettleVelocityCountsPerSec &&
                std::fabs(positionError) <= kSimulationCsvSettleWindowCounts;
            const double velocityBeforeStep = axis.actualVelocityCounts;
            if (csvHoldCandidate) {
                axis.csvSettleStableSec = (std::min)(axis.csvSettleStableSec + dtSeconds,
                                                     kSimulationCsvSettleStableSec);
                stepPositionLoop(axis,
                                 targetPosition,
                                 kSimulationCsvSettleVelocityCountsPerSec,
                                 kSimulationServoMaxAccelerationCountsPerSec2);
            } else {
                axis.positionErrorIntegralCountsSec = 0.0;
                axis.positionVelocityDemandCounts = targetVelocity;
                stepVelocityLoop(axis, targetVelocity, kSimulationServoMaxAccelerationCountsPerSec2);
                axis.csvSettleStableSec = 0.0;
            }
            if (((requestedTargetVelocity > 0.0 || velocityBeforeStep > 0.0) && axis.actualPositionCounts >= targetPosition) ||
                ((requestedTargetVelocity < 0.0 || velocityBeforeStep < 0.0) && axis.actualPositionCounts <= targetPosition)) {
                axis.actualPositionCounts = targetPosition;
                axis.actualVelocityCounts = 0.0;
                axis.velocityErrorIntegralCounts = 0.0;
                axis.servoAccelerationCounts = 0.0;
                axis.positionVelocityDemandCounts = 0.0;
                axis.targetVelocity = 0;
                WriteLe32(IOmap + outBase + 6, 0);
            }
            axis.lastCsvTargetPosition = axis.targetPosition;
            axis.haveLastCsvTargetPosition = true;
        } else {
            axis.csvSettleStableSec = 0.0;
            axis.haveLastCsvTargetPosition = false;
            const double target = static_cast<double>(axis.targetPosition);
            stepPositionLoop(axis,
                             target,
                             kSimulationServoMaxVelocityCountsPerSec,
                             kSimulationServoMaxAccelerationCountsPerSec2);
        }

        axis.modeDisplay = axis.operationMode;
        const uint16_t statusword = ComputeSimulationStatusword(axis);
        WriteLe16(IOmap + inBase, statusword);
        WriteLe32(IOmap + inBase + 2, RoundSimulationCount(axis.actualPositionCounts));
        WriteLe32(IOmap + inBase + 6, RoundSimulationCount(axis.actualVelocityCounts));
        WriteLe16(IOmap + inBase + 10, 0u);
        axis.lastControlword = axis.controlword;
    }

    if (!stagedOutputs.empty()) {
        ApplyStagedSimulationOutputs(IOmap, sizeof(IOmap), stagedOutputs);
    }

    LeaveCriticalSection(&pdoDataCS);
    LeaveCriticalSection(&simulationCS_);
}

void EtherCATMaster::SimulationCyclicTask()
{
    LARGE_INTEGER frequency = {};
    LARGE_INTEGER previousTick = {};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&previousTick);

    using steady_clock = std::chrono::steady_clock;
    auto nextWake = steady_clock::now();
    HANDLE waitTimer = CreateSimulationWaitTimer();

    auto waitForNextWake = [&](steady_clock::time_point deadline) {
        while (true) {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) {
                return false;
            }

            const auto now = steady_clock::now();
            if (now >= deadline) {
                return true;
            }

            const auto remaining = deadline - now;
            if (waitTimer) {
                const auto waitNs = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count();
                LONGLONG hundredNs = waitNs / 100;
                if (hundredNs <= 0) {
                    hundredNs = 1;
                }

                LARGE_INTEGER dueTime = {};
                dueTime.QuadPart = -hundredNs;
                if (SetWaitableTimer(waitTimer, &dueTime, 0, nullptr, nullptr, FALSE)) {
                    HANDLE handles[2] = { stopEvent, waitTimer };
                    const DWORD waitResult = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                    if (waitResult == WAIT_OBJECT_0) {
                        return false;
                    }
                    if (waitResult == WAIT_OBJECT_0 + 1) {
                        return true;
                    }
                } else {
                    CancelWaitableTimer(waitTimer);
                    CloseHandle(waitTimer);
                    waitTimer = nullptr;
                }
            }

            const auto remainingUs = std::chrono::duration_cast<std::chrono::microseconds>(remaining).count();
            DWORD waitMs = static_cast<DWORD>((std::max<long long>)(1, (remainingUs + 999) / 1000));
            if (WaitForSingleObject(stopEvent, waitMs) == WAIT_OBJECT_0) {
                return false;
            }
        }
    };

    while (WaitForSingleObject(stopEvent, 0) != WAIT_OBJECT_0) {
        LARGE_INTEGER currentTick = {};
        QueryPerformanceCounter(&currentTick);

        uint32_t cycleUs = desiredCycleTime ? desiredCycleTime : 2000;
        if (cycleUs == 0) {
            cycleUs = 2000;
        }

        double dtSeconds = static_cast<double>(cycleUs) / 1000000.0;
        if (frequency.QuadPart > 0 && previousTick.QuadPart != 0) {
            const double deltaTicks = static_cast<double>(currentTick.QuadPart - previousTick.QuadPart);
            if (deltaTicks > 0.0) {
                dtSeconds = deltaTicks / static_cast<double>(frequency.QuadPart);
                const uint32_t observedUs = static_cast<uint32_t>((deltaTicks * 1000000.0) / static_cast<double>(frequency.QuadPart));
                observedCycleMicros_.store(observedUs, std::memory_order_relaxed);
                uint32_t prevMax = maxObservedCycleMicros_.load(std::memory_order_relaxed);
                while (observedUs > prevMax &&
                       !maxObservedCycleMicros_.compare_exchange_weak(prevMax, observedUs, std::memory_order_relaxed)) {
                }
            }
        }
        previousTick = currentTick;

        // MMFReader is externally polled by the master cyclic thread. Mirror the
        // real EtherCAT path here so simulation mode consumes fresh MMF1_IN data.
        if (mmf1ReaderRunning.load(std::memory_order_relaxed)) {
            (void)mmf1Reader.PollOnce();
        }

        AdvanceSimulation(dtSeconds);
        workingCounter = slaveCount * 2;
        pdosSentCount.fetch_add(1, std::memory_order_relaxed);
        commsTime_.store(0, std::memory_order_relaxed);
        maxObservedCommsTime.store(0, std::memory_order_relaxed);

        const auto period = std::chrono::microseconds(cycleUs);
        nextWake += period;
        const auto afterCycle = steady_clock::now();
        if (nextWake <= afterCycle) {
            nextWake = afterCycle + period;
        }
        const auto idleDeadline = std::chrono::duration_cast<std::chrono::microseconds>(nextWake.time_since_epoch()).count();
        pdoIdleWindowDeadlineUs_.store(idleDeadline, std::memory_order_release);

        if (!waitForNextWake(nextWake)) {
            break;
        }
    }

    if (waitTimer) {
        CancelWaitableTimer(waitTimer);
        CloseHandle(waitTimer);
    }
}

uint16_t EtherCATMaster::ComputeSimulationStatusword(const SimulationAxisState& axis) const
{
    if (axis.errorCode != 0) {
        return 0x0008u;
    }
    if ((axis.controlword & 0x0080u) != 0u) {
        return 0x0040u;
    }

    const uint16_t basicState = static_cast<uint16_t>(axis.controlword & 0x001Fu);
    if (basicState == 0x000Fu || basicState == 0x001Fu) {
        if (axis.homingActive) {
            return 0x0237u;
        }
        if (axis.operationMode == CoreHelpers::kCia402OperationModeHoming) {
            return 0x1637u;
        }

        const double positionError = static_cast<double>(axis.targetPosition) - axis.actualPositionCounts;
        const bool targetReached =
            std::fabs(positionError) <= kSimulationCsvSettleWindowCounts &&
            std::fabs(static_cast<double>(axis.targetVelocity)) <= kSimulationCsvSettleVelocityCountsPerSec &&
            std::fabs(axis.actualVelocityCounts) <= kSimulationCsvSettleVelocityCountsPerSec;
        return targetReached ? 0x1637u : 0x1237u;
    }
    if ((axis.controlword & 0x0007u) == 0x0007u) {
        return 0x0023u;
    }
    if ((axis.controlword & 0x0007u) == 0x0006u) {
        return 0x0021u;
    }
    return 0x0040u;
}