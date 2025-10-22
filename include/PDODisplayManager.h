#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Minimal forward declarations / type definitions extracted so other translation
// units (e.g., MotionController.cpp) can reference PDO structures without
// pulling the entire Win32Window.cpp implementation.

struct PDOItem {
    std::wstring name;
    uint16_t index = 0;
    uint8_t subindex = 0;
    bool isOutput = false; // true if mapped to outputs (Tx to slave)
    uint16_t bitLength = 0; // bit length
    uint32_t address = 0; // IO map address offset
    // ... other fields may exist in the full implementation but are not required
};

class EtherCATMaster; // forward

class PDODisplayManager {
public:
    PDODisplayManager();
    ~PDODisplayManager();

    // Public subset used by MotionController
    std::vector<PDOItem> pdoItems;

    bool WriteToIOMap(EtherCATMaster* master, int slaveIndex, const std::wstring& itemName, int32_t value);
    // Other methods exist in the full implementation but are not declared here.
};
