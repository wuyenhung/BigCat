#pragma once

#include <cstdint>
#include <vector>
#include "../PDOModel.h"

class IEtherCATService {
public:
    virtual ~IEtherCATService() = default;

    virtual bool IsInitialized() const = 0;
    virtual bool IsRunning() const = 0;
    virtual int GetSlaveCount() const = 0;
    virtual bool GetLastSdo60FD(std::vector<uint8_t>& out) = 0;
    virtual bool GetLastSdo60FDLowByte(uint8_t& outLowByte) = 0;
    virtual bool TryGetPDOChannelValuesLockFree(int slaveIndex, std::vector<PDOChannelValue>& outValues, uint64_t* outVersion) = 0;
    virtual bool GetPDOChannelValues(int slaveIndex, std::vector<PDOChannelValue>& outValues) = 0;
    virtual void SetMechanicalScalingFactors(int slaveIndex, double p1Scale, bool invertSign) = 0;
    virtual bool TryGetMechanicalUnitMetricsLockFree(int slaveIndex, MechanicalUnitMetrics& outMetrics) = 0;
    virtual bool GetMechanicalUnitMetrics(int slaveIndex, MechanicalUnitMetrics& outMetrics) = 0;
    virtual void ClearGlobalError() = 0;
    virtual void ResetPDOFilter(int slaveIndex) = 0;
    virtual void ResetMaxObservedCommsTime() = 0;
    virtual void ResetMaxObservedCycleMicros() = 0;
};
