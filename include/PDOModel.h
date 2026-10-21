#pragma once

#ifdef __cplusplus

#include <string>
#include <cstdint>

// Data-only descriptor of a PDO channel used to map raw snapshots.
struct PDOChannelDescriptor {
    std::wstring name;
    uint16_t index = 0;
    uint8_t subindex = 0;
    uint8_t bitLength = 0;
    bool isOutput = false;
    int byteOffset = 0;
};

// Computed value for a PDO channel produced by the EtherCAT master thread.
struct PDOChannelValue {
    PDOChannelDescriptor descriptor;
    int32_t rawValue = 0;
    std::wstring formattedValue;
    bool valid = false;
};

// Data computed by EtherCAT master and consumed by the UI mechanical unit table
struct MechanicalUnitMetrics {
    std::wstring targetPosition;
    std::wstring actualPosition;
    std::wstring targetSpeed;
    std::wstring actualSpeed;
    std::wstring actualTorque;
    bool valid = false;
};

#endif // __cplusplus
