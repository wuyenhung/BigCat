#include "../include/MotionLogic.h"
#include "EtherCATMaster.h"
#include "PDOProxy.h"
#include "PDOHelpers.h"
#include <thread>
#include <chrono>

namespace MotionLogic {

const PDOItem* FindControlwordItem(const std::vector<PDOItem>& items) {
    for (const auto &it : items) if (it.index == 0x6040 && it.isOutput) return &it;
    return nullptr;
}

const PDOItem* FindStatuswordItem(const std::vector<PDOItem>& items) {
    for (const auto &it : items) if (it.index == 0x6041 && !it.isOutput) return &it;
    return nullptr;
}

const PDOItem* FindActualPositionItem(const std::vector<PDOItem>& items) {
    for (const auto &it : items) if (it.index == 0x6064 && !it.isOutput) return &it;
    return nullptr;
}

const PDOItem* FindTargetPositionItem(const std::vector<PDOItem>& items) {
    for (const auto &it : items) if (it.index == 0x607A && it.isOutput) return &it;
    return nullptr;
}

const PDOItem* FindPDOItemByIndex(const std::vector<PDOItem>& items, uint16_t index, bool filterByOutput, bool isOutput) {
    for (const auto &it : items) {
        if (it.index == index) {
            if (!filterByOutput || it.isOutput == isOutput) return &it;
        }
    }
    return nullptr;
}

bool StageControlword(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, const PDOItem* cwItem, int32_t value) {
    if (!master || !pdo || !cwItem) return false;
    bool wrote = pdo->WriteToIOMap(master, slaveIdx, cwItem->name, value);
    if (wrote && master->IsRunning()) master->SendProcessDataOnce();
    return wrote;
}

bool StageControlwordWithoutSend(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, const PDOItem* cwItem, int32_t value) {
    if (!master || !pdo || !cwItem) return false;
    return pdo->WriteToIOMap(master, slaveIdx, cwItem->name, value);
}

bool ReadControlwordFromSnapshot(EtherCATMaster* master, const PDOItem* cwItem, int slaveIdx, int32_t &outValue) {
    if (!master || !cwItem) return false;
    std::vector<uint8_t> snapshot;
    if (!master->GetPDODataSnapshot(slaveIdx, snapshot)) return false;
    return ReadPDOItemValueFromSnapshot(master, slaveIdx, snapshot, *cwItem, outValue);
}

bool VerifyPDOBitSet(EtherCATMaster* master, const PDOItem* cwItem, int slaveIdx, uint32_t bitMask, int retries, int intervalMs) {
    if (!master || !cwItem) return false;
    for (int attempt = 0; attempt < retries; ++attempt) {
        int32_t val = 0;
        if (ReadControlwordFromSnapshot(master, cwItem, slaveIdx, val)) {
            if ((static_cast<uint32_t>(val) & bitMask) != 0) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }
    return false;
}

bool StagePDOByIndex(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx, uint16_t index, uint32_t value) {
    if (!master || !pdo) return false;
    const PDOItem* target = nullptr;
    for (const auto &it : pdo->pdoItems) {
        if (it.index == index && it.isOutput) { target = &it; break; }
    }
    if (!target) return false;
    // Use existing WriteToIOMap helper via PDOProxy to stage the value
    // NOTE: Do NOT call SendProcessDataOnce() here - let the caller batch
    // multiple stages and send once at the end. This prevents race conditions
    // where intermediate states are sent to drives.
    bool wrote = pdo->WriteToIOMap(master, slaveIdx, target->name, (int32_t)value);
    // Removed: if (wrote && master->IsRunning()) master->SendProcessDataOnce();
    return wrote;
}

} // namespace MotionLogic
