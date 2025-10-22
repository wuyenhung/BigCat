#include "MotionController.h"
#include "EtherCATMaster.h"
#include "PDODisplayManager.h"
#include "PDOHelpers.h" // ReadPDOItemValueFromSnapshot or helpers
#include <sstream>
#include <thread>
#include <chrono>

MotionController::MotionController(EtherCATMaster* master, PDODisplayManager* pdo)
    : etherCATMaster(master), pdoDisplay(pdo) {}

MotionController::~MotionController() {}

void MotionController::SetMaster(EtherCATMaster* master) {
    etherCATMaster = master;
}

static std::wstring ToW(const std::string &s) {
    std::wstring ws;
    ws.assign(s.begin(), s.end());
    return ws;
}

MotionResult MotionController::FaultResetAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) {
        res.message = L"[FaultReset] invalid args or master missing";
        return res;
    }

    bool staged = false;
    for (const auto &it : pdoDisplay->pdoItems) {
        if (it.index == 0x6040 && it.isOutput) {
            std::vector<uint8_t> snapshot;
            if (etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot)) {
                int32_t curv = 0;
                if (ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snapshot, it, curv)) {
                    int32_t newv = curv;
                    if (assert) newv |= 0x0080; else newv &= ~0x0080;
                    bool wrote = pdoDisplay->WriteToIOMap(etherCATMaster, slaveIdx, it.name, (int32_t)newv);
                    if (wrote) {
                        std::wstringstream ss; ss << L"[FaultReset] PDO staged " << (assert ? L"ASSERT" : L"RELEASE") << L" slave=" << slaveIdx << L" (0x6040=0x" << std::hex << newv << std::dec << L")\n";
                        res.success = true; res.message = ss.str();
                        if (etherCATMaster->IsRunning()) etherCATMaster->SendProcessDataOnce();
                        staged = true;
                        break;
                    } else {
                        res.message = L"[FaultReset] PDO staging failed";
                    }
                } else {
                    res.message = L"[FaultReset] snapshot parse failed";
                }
            } else {
                res.message = L"[FaultReset] could not obtain PDO snapshot";
            }
        }
        if (staged) break;
    }
    if (!staged) {
        // per policy: no SDO fallback
        if (res.message.empty()) res.message = L"[FaultReset] PDO staging failed; no SDO fallback performed";
        res.success = false;
    }
    return res;
}

MotionResult MotionController::ImmediateStopAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) {
        res.message = L"[ImmediateStop] invalid args or master missing";
        return res;
    }
    if (!assert) { res.message = L"[ImmediateStop] release ignored"; return res; }

    const uint32_t QS_MASK = 0x0004;
    bool staged = false;
    for (const auto &it : pdoDisplay->pdoItems) {
        if (it.index == 0x6040 && it.isOutput) {
            std::vector<uint8_t> snapshot;
            if (etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot)) {
                int32_t curv = 0;
                if (ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snapshot, it, curv)) {
                    int32_t newv = curv;
                    newv |= QS_MASK;
                    bool wrote = pdoDisplay->WriteToIOMap(etherCATMaster, slaveIdx, it.name, (int32_t)newv);
                    if (wrote) {
                        std::wstringstream ss; ss << L"[ImmediateStop] PDO staged QS=1 slave=" << slaveIdx << L" (0x6040=0x" << std::hex << newv << std::dec << L")\n";
                        res.success = true; res.message = ss.str();
                        if (etherCATMaster->IsRunning()) etherCATMaster->SendProcessDataOnce();
                        staged = true; break;
                    } else res.message = L"[ImmediateStop] PDO staging failed";
                } else res.message = L"[ImmediateStop] snapshot parse failed";
            } else res.message = L"[ImmediateStop] could not obtain PDO snapshot";
        }
        if (staged) break;
    }
    if (!staged) { if (res.message.empty()) res.message = L"[ImmediateStop] PDO staging failed; no SDO fallback"; }
    return res;
}

MotionResult MotionController::PowerOffAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = L"[PowerOff] invalid args or master missing"; return res; }
    if (!assert) { res.message = L"[PowerOff] release ignored"; return res; }

    const uint32_t POWER_OFF_VALUE = 0x0006;
    const PDOItem* cwItem = nullptr;
    for (const auto &it : pdoDisplay->pdoItems) if (it.index == 0x6040 && it.isOutput) { cwItem = &it; break; }
    if (!cwItem) { res.message = L"[PowerOff] controlword item not found"; return res; }
    bool wrote = pdoDisplay->WriteToIOMap(etherCATMaster, slaveIdx, cwItem->name, (int32_t)POWER_OFF_VALUE);
    if (wrote) {
        std::wstringstream ss; ss << L"[PowerOff] PDO staged POWER_OFF slave=" << slaveIdx << L" (0x6040=0x" << std::hex << POWER_OFF_VALUE << std::dec << L")\n";
        if (etherCATMaster->IsRunning()) etherCATMaster->SendProcessDataOnce();
        res.success = true; res.message = ss.str();
    } else {
        res.message = L"[PowerOff] PDO staging failed for POWER_OFF";
    }
    return res;
}

MotionResult MotionController::PowerOnAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = L"[PowerOn] invalid args or master missing"; return res; }
    if (!assert) { res.message = L"[PowerOn] release ignored"; return res; }

    // Power on should set bits in controlword; strategy: stage controlword bits 0..3 via PDO staging
    const PDOItem* cwItem = nullptr;
    for (const auto &it : pdoDisplay->pdoItems) if (it.index == 0x6040 && it.isOutput) { cwItem = &it; break; }
    if (!cwItem) { res.message = L"[PowerOn] controlword item not found"; return res; }

    // read snapshot then set bits 0..3 (0x000F)
    std::vector<uint8_t> snapshot;
    if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot)) { res.message = L"[PowerOn] could not obtain PDO snapshot"; return res; }
    int32_t curv = 0;
    if (!ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snapshot, *cwItem, curv)) { res.message = L"[PowerOn] snapshot parse failed"; return res; }
    int32_t newv = curv | 0x000F;
    bool wrote = pdoDisplay->WriteToIOMap(etherCATMaster, slaveIdx, cwItem->name, (int32_t)newv);
    if (wrote) {
        std::wstringstream ss; ss << L"[PowerOn] PDO staged POWER_ON slave=" << slaveIdx << L" (0x6040=0x" << std::hex << newv << std::dec << L")\n";
        if (etherCATMaster->IsRunning()) etherCATMaster->SendProcessDataOnce();
        res.success = true; res.message = ss.str();
    } else res.message = L"[PowerOn] PDO staging failed";
    return res;
}

MotionResult MotionController::HomeAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = L"[Home] invalid args or master missing"; return res; }
    if (!assert) { res.message = L"[Home] release ignored"; return res; }
    // Implementation follows provided pseudocode:
    // - UI and implementation separated: MotionController will NOT touch UI BUSY flag
    // - Use snapshot to read PDO and staging (WriteToIOMap) to write PDO
    // Steps:
    // 1) SDO 0x6060 = 6 (homing mode)
    // 2) SDO 0x6098 = 35 (use current position as Home)
    // 3) SDO 0x607C = 0 (homing parameter)
    // 4) snapshot read 0x6040, stage same value but with bit4 set (command)
    // 5) poll 0x6041 via snapshot: success when bit12=1 && bit10=1 && bit13=0; error when bit13=1
    // 6) timeout = 3s -> failure

    // find controlword (0x6040) PDO item
    const PDOItem* cwItem = nullptr;
    for (const auto &it : pdoDisplay->pdoItems) if (it.index == 0x6040 && it.isOutput) { cwItem = &it; break; }
    if (!cwItem) { res.message = L"[Home] controlword item not found"; return res; }

    // 1) write 0x6060 = 6
    uint8_t buf6060_mode6[1] = { (uint8_t)6 };
    if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)buf6060_mode6, 1)) {
        res.message = L"[Home] failed to write SDO 0x6060=6"; return res;
    }

    // 2) write 0x6098 = 35 (32-bit)
    {
        uint32_t v6098 = 35;
        if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6098, (uint8_t)0x00, (void*)&v6098, (int)sizeof(v6098))) {
            res.message = L"[Home] failed to write SDO 0x6098=35"; return res;
        }
    }

    // 3) write 0x607C = 0 (32-bit)
    {
        uint32_t v607c = 0;
        if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x607C, (uint8_t)0x00, (void*)&v607c, (int)sizeof(v607c))) {
            res.message = L"[Home] failed to write SDO 0x607C=0"; return res;
        }
    }

    // 4) snapshot read and stage 0x6040 with bit4 set
    std::vector<uint8_t> snapshot;
    if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot)) {
        res.message = L"[Home] could not obtain PDO snapshot"; return res;
    }

    int32_t curv = 0;
    if (!ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snapshot, *cwItem, curv)) {
        res.message = L"[Home] snapshot parse failed (controlword)"; return res;
    }

    const int32_t CMD_BIT4 = 0x0010; // bit4
    int32_t newcw = curv | CMD_BIT4;
    bool staged = pdoDisplay->WriteToIOMap(etherCATMaster, slaveIdx, cwItem->name, newcw);
    if (staged && etherCATMaster->IsRunning()) etherCATMaster->SendProcessDataOnce();
    if (!staged) { res.message = L"[Home] PDO staging failed (set bit4)"; return res; }

    // 5) poll 0x6041 via snapshot for up to 3s
    const int timeoutMs = 3000;
    const int pollIntervalMs = 50;
    int elapsed = 0;
    uint32_t lastStatus = 0;
    const PDOItem* statusItem = nullptr;
    for (const auto &it : pdoDisplay->pdoItems) if (it.index == 0x6041 && !it.isOutput) { statusItem = &it; break; }

    bool finished = false;
    while (elapsed < timeoutMs) {
        std::vector<uint8_t> snap2;
        if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snap2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs)); elapsed += pollIntervalMs; continue;
        }

        bool have = false;
        if (statusItem) {
            int32_t sval = 0;
            if (ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snap2, *statusItem, sval)) {
                lastStatus = (uint32_t)sval; have = true;
            }
        } else {
            // fallback to SDO read of 0x6041
            uint8_t sbuf[8] = {0}; int slen = (int)sizeof(sbuf);
            if (etherCATMaster->ReadSDO(slaveIdx, 0x6041, 0x00, (char*)sbuf, slen)) {
                uint32_t st = 0; int use = (slen < 4 ? slen : 4);
                for (int i = 0; i < use; ++i) st |= (uint32_t)((uint8_t)sbuf[i]) << (8 * i);
                lastStatus = st; have = true;
            }
        }

        if (have) {
            bool b12 = (lastStatus & 0x1000U) != 0;
            bool b10 = (lastStatus & 0x0400U) != 0;
            bool b13 = (lastStatus & 0x2000U) != 0;
            if (b13) {
                // error
                std::wstringstream ss; ss << L"[Home] homing error (status bit13) slave=" << slaveIdx << L" status=0x" << std::hex << lastStatus << std::dec << L"\n";
                res.success = false; res.message = ss.str(); finished = true; break;
            }
            if (b12 && b10 && !b13) {
                std::wstringstream ss; ss << L"[Home] homing completed slave=" << slaveIdx << L" status=0x" << std::hex << lastStatus << std::dec << L"\n";
                res.success = true; res.message = ss.str(); finished = true; break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs)); elapsed += pollIntervalMs;
    }

    if (!finished) {
        std::wstringstream ss; ss << L"[Home] TIMEOUT after 3s waiting for homing completion slave=" << slaveIdx << L" status=0x" << std::hex << lastStatus << std::dec << L"\n";
        res.success = false; res.message = ss.str();
    }

    // NOTE: per "UI 與實作需分離" the UI is responsible for BUSY flag (set/clear). MotionController only performs SDO/PDO work.
    return res;
}
