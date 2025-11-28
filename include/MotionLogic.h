#pragma once
#include <cstdint>
#include <vector>
#include "PDOProxy.h"

class EtherCATMaster;
struct PDOItem;

namespace MotionLogic {
    // Find the first controlword PDO item (0x6040 output) or nullptr
    const PDOItem* FindControlwordItem(const std::vector<PDOItem>& items);

    // Find the first statusword PDO item (0x6041 input) or nullptr
    const PDOItem* FindStatuswordItem(const std::vector<PDOItem>& items);

    // Find the first actual position PDO item (0x6064 input) or nullptr
    const PDOItem* FindActualPositionItem(const std::vector<PDOItem>& items);

    // Find the first target position PDO item (0x607A output) or nullptr
    const PDOItem* FindTargetPositionItem(const std::vector<PDOItem>& items);

    // Generic PDO item finder by index (and optionally isOutput flag)
    const PDOItem* FindPDOItemByIndex(const std::vector<PDOItem>& items, uint16_t index, bool filterByOutput = false, bool isOutput = false);

    // Stage a controlword value via PDOProxy and send process data once
    bool StageControlword(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, const PDOItem* cwItem, int32_t value);
    bool StageControlwordWithoutSend(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, const PDOItem* cwItem, int32_t value);

    // Verify that a given bit(s) are set in the controlword via PDO snapshot
    bool VerifyPDOBitSet(EtherCATMaster* master, const PDOItem* cwItem, int slaveIdx, uint32_t bitMask, int retries = 5, int intervalMs = 10);

    // Read current controlword value from snapshot (returns false on error)
    bool ReadControlwordFromSnapshot(EtherCATMaster* master, const PDOItem* cwItem, int slaveIdx, int32_t &outValue);
    // Stage arbitrary PDO by object index (0x60FF etc.) using PDOProxy lookup by index
    bool StagePDOByIndex(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, uint16_t index, uint32_t value);
}
