#include "MotionController.h"
#include "EtherCATMaster.h"
#include "PDOProxy.h"
#include "PDOHelpers.h" // ReadPDOItemValueFromSnapshot or helpers
#include "MotionLogic.h"
#include <sstream>
#include <windows.h>
// <ctime> previously used for simple timestamping in lambdas; replaced by TimestampNowW()

// Debug output control - set to 0 for production builds to reduce logging overhead
#ifndef MOTIONCTRL_DEBUG_LOG
#define MOTIONCTRL_DEBUG_LOG 0
#endif

// Helper: produce a wide timestamp prefix like "[HH:MM:SS.mmm] " for source logs
static std::wstring TimestampNowW() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t buf[64];
    swprintf_s(buf, sizeof(buf)/sizeof(buf[0]), L"[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::wstring(buf);
}

static bool StageZeroVelocityCommand(EtherCATMaster* master, PDOProxy* pdo, int slaveIdx)
{
    // This function stages zero to the velocity command PDO (0x60FF).
    // In CSP (position) mode, the velocity PDO is not the active command;
    // return success immediately to avoid overwriting the target position.
    if (master && master->IsCspMode()) {
        return true;
    }
    return MotionLogic::StagePDOByIndex(
        master,
        pdo,
        slaveIdx,
        0x60FF,  // Always target velocity PDO, not the mode-dependent default
        0u);
}

MotionController::MotionController(EtherCATMaster* master, PDOProxy* pdo)
    : etherCATMaster(master), pdoDisplay(pdo) {}

MotionController::~MotionController() {
    // 停止寸動緩啟動執行緒
    // 先設置 etherCATMaster/pdoDisplay 為 nullptr，讓執行中的線程能安全檢測到
    etherCATMaster = nullptr;
    pdoDisplay = nullptr;
    
    // 停止並 join 所有 per-slave 寸動執行緒（安全）
    std::lock_guard<std::mutex> lk(jogStatesMutex_);
    for (auto &kv : jogStates_) {
        auto st = kv.second;
        if (!st) continue;
        
        // 設置停止標記
        st->stop.store(true);
        
        // join 執行緒
        // 執行中的線程會看到 etherCATMaster/pdoDisplay == nullptr，應該會安全地退出
        if (st->thr.joinable()) {
            st->thr.join();
        }
    }
}

void MotionController::SetMaster(EtherCATMaster* master) {
    etherCATMaster = master;
}

MotionResult MotionController::FaultResetAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[FaultReset] invalid args or master missing");
        return res;
    }
    if (!pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[FaultReset] PDO display manager missing");
        return res;
    }

    bool staged = false;
    const PDOItem* cw = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (cw) {
        int32_t newv = assert ? 0x0080 : 0x0007;
        if (assert) {
            // Read 0x603F from per-slave PDO cache (updated every cycle, lock-free)
            uint16_t ec = 0;
            bool gotEc = etherCATMaster->GetPdo603F(static_cast<uint16_t>(slaveIdx), ec);
            if (!gotEc) {
                res.message = TimestampNowW() + std::wstring(L"[FaultReset] failed to read PDO cache 0x603F");
            } else if (ec == 0) {
                std::wstringstream ss; ss << TimestampNowW() << L"[FaultReset] 0x603F == 0; no error present; assert skipped slave=" << slaveIdx << L"\n";
                res.success = true;
                res.message = ss.str();
            } else {
                if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cw, newv)) {
                    std::wstringstream ss; ss << TimestampNowW() << L"[FaultReset] PDO staged ASSERT(0x0080) slave=" << slaveIdx << L" (0x6040=0x" << std::hex << std::uppercase << newv << std::dec << L")\n";
                    res.success = true; res.message = ss.str(); staged = true;
                } else res.message = L"[FaultReset] PDO staging failed";
            }
        } else {
            // Release: only perform staging to 0x0007 if current controlword snapshot shows 0x0080
            int32_t curv = 0;
            if (MotionLogic::ReadControlwordFromSnapshot(etherCATMaster, cw, slaveIdx, curv)) {
                if (curv == 0x0080) {
                    if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cw, newv)) {
                        std::wstringstream ss; ss << TimestampNowW() << L"[FaultReset] PDO staged RELEASE(0x0007) slave=" << slaveIdx << L" (0x6040=0x" << std::hex << std::uppercase << newv << std::dec << L")\n";
                        res.success = true; res.message = ss.str(); staged = true;
                    } else res.message = TimestampNowW() + std::wstring(L"[FaultReset] PDO staging failed");
                } else {
                    std::wstringstream ss; ss << TimestampNowW() << L"[FaultReset] release skipped; controlword!=0x0080 (cur=0x" << std::hex << std::uppercase << curv << std::dec << L")\n";
                    res.success = true;
                    res.message = ss.str();
                }
            } else {
                res.message = TimestampNowW() + std::wstring(L"[FaultReset] snapshot parse failed; cannot verify controlword before release");
            }
        }
    }
    // per policy: no SDO fallback
    if (!staged && !res.success) {
        if (res.message.empty()) res.message = TimestampNowW() + std::wstring(L"[FaultReset] PDO staging failed; no SDO fallback performed");
        res.success = false;
    }
    return res;
}

MotionResult MotionController::FaultResetAll() {
    MotionResult res{false, L""};
    if (!etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[FaultResetAll] EtherCAT master missing\n");
        return res;
    }
    int slaveCount = etherCATMaster->GetSlaveCount();
    if (slaveCount <= 0) {
        res.message = TimestampNowW() + std::wstring(L"[FaultResetAll] no slaves present\n");
        return res;
    }

    bool anySuccess = false;
    std::wstringstream allMsg;
    allMsg << TimestampNowW() << L"[FaultResetAll] starting assert phase for " << slaveCount << L" slaves\n";

    // 1) Assert fault-reset on all slaves
    for (int s = 1; s <= slaveCount; ++s) {
        MotionResult r = FaultResetAsync(s, true);
        allMsg << r.message;
        anySuccess = anySuccess || r.success;
    }

    // allow devices to process asserted reset
    Sleep(20);
    allMsg << TimestampNowW() << L"[FaultResetAll] starting release phase for " << slaveCount << L" slaves\n";

    // 2) Release fault-reset on all slaves
    for (int s = 1; s <= slaveCount; ++s) {
        MotionResult r = FaultResetAsync(s, false);
        allMsg << r.message;
        anySuccess = anySuccess || r.success;
    }

    res.success = anySuccess;
    res.message = allMsg.str();
    return res;
}

MotionResult MotionController::ImmediateStopAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] invalid args or master missing");
        return res;
    }
    if (!assert) { res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] release ignored"); return res; }

    // Use filter-based hold-position for deceleration-limited stop.
    // HoldCurrentPosition() restores full configured deceleration limits
    // and sets the profile target to current position.
    try {
        etherCATMaster->HoldPDOFilterPosition(slaveIdx);
        res.success = true;
        std::wstringstream ss;
        ss << TimestampNowW() << L"[ImmediateStop] filter hold-position set for slave=" << slaveIdx
           << L" (deceleration-limited stop via PDOFilter)\n";
        res.message = ss.str();
    } catch (const std::exception &ex) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] HoldPDOFilterPosition exception: ")
                      + std::wstring(ex.what(), ex.what() + strlen(ex.what()));
    } catch (...) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] HoldPDOFilterPosition unknown exception");
    }
    return res;
}

MotionResult MotionController::ImmediateStopAll() {
    MotionResult res{false, L""};
    if (!etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStopAll] EtherCAT master missing\n");
        return res;
    }
    int slaveCount = etherCATMaster->GetSlaveCount();
    if (slaveCount <= 0) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStopAll] no slaves present\n");
        return res;
    }

    std::wstringstream allMsg;
    allMsg << TimestampNowW() << L"[ImmediateStopAll] invoking deceleration-limited stop for " << slaveCount << L" slaves\n";

    // Use filter-based hold-position for deceleration-limited stop.
    // HoldCurrentPosition() restores full configured deceleration limits
    // and sets the profile target to current position.
    try {
        etherCATMaster->HoldAllPDOFiltersPosition();
        allMsg << TimestampNowW() << L"[ImmediateStopAll] all PDO filters set to hold current position\n";
        res.success = true;
    } catch (const std::exception &ex) {
        allMsg << TimestampNowW() << L"[ImmediateStopAll] HoldAllPDOFiltersPosition exception: " << ex.what() << L"\n";
    } catch (...) {
        allMsg << TimestampNowW() << L"[ImmediateStopAll] HoldAllPDOFiltersPosition unknown exception\n";
    }

    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        allMsg << TimestampNowW() << L"[ImmediateStopAll] process data sent\n";
    }
    res.message = allMsg.str();
    return res;
}

MotionResult MotionController::SmoothStopAll(double rampSec) {
    (void)rampSec;

    MotionResult res{false, L""};
    if (!etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[SmoothStopAll] EtherCAT master missing\n");
        return res;
    }
    if (!pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[SmoothStopAll] PDO display manager missing\n");
        return res;
    }
    int slaveCount = etherCATMaster->GetSlaveCount();
    if (slaveCount <= 0) {
        res.message = TimestampNowW() + std::wstring(L"[SmoothStopAll] no slaves present\n");
        return res;
    }

    std::wstringstream allMsg;
    // Note: do not stop MMF input here; rely on HoldAllPDOFiltersPosition()
    // which was already called above to make filters ignore MMF inputs.
    allMsg << TimestampNowW() << L"[SmoothStopAll] commanding zero velocity / hold-position for " << slaveCount << L" slaves\n";
    bool anySuccess = false;

    // CRITICAL: Tell all PDO filters to hold their current position.
    // This prevents CyclicTask from continuing to track MMF setpoint after stop is requested.
    // Without this, filters would decelerate briefly then resume motion towards MMF target.
    try {
        etherCATMaster->HoldAllPDOFiltersPosition();
        allMsg << TimestampNowW() << L"[SmoothStopAll] PDO filters set to hold current position\n";
        anySuccess = true;
    } catch (...) {
        allMsg << TimestampNowW() << L"[SmoothStopAll] warning: failed to set hold-position mode\n";
    }

    // CSP mode: after HoldAllPDOFiltersPosition, sync 0x607A (Target Position)
    // to the actual position (0x6064) for each slave.  In CSP mode 0x607A is
    // the active position command; if a stale filter-output position is left
    // in IOmap, the next servo enable (e.g. after Fault Reset) will see a
    // position != actual and produce a sudden jump.  Copying 0x6064->0x607A
    // ensures the servo target equals its current position.
    //
    // Must clear staged outputs FIRST so that SendProcessDataOnce() below
    // does not re-apply an old 0x607A entry from stagedOutputs and overwrite
    // the corrected IOmap value.
    if (etherCATMaster->IsCspMode()) {
        etherCATMaster->ClearStagedOutputsForCyclicMotionCommands();
        const PDOItem* posActItem = MotionLogic::FindActualPositionItem(pdoDisplay->pdoItems);
        const PDOItem* posTgtItem = nullptr;
        for (const auto& it : pdoDisplay->pdoItems) {
            if (it.index == 0x607A && it.isOutput) { posTgtItem = &it; break; }
        }
        if (posActItem && posTgtItem) {
            for (int s = 1; s <= slaveCount; ++s) {
                int outBase = 0, inBase = 0;
                if (!etherCATMaster->GetSlaveIOBaseBytes(s, outBase, inBase)) continue;
                const int actOff = inBase + posActItem->byteOffset;
                const int tgtOff = outBase + posTgtItem->byteOffset;
                if (actOff >= 0 && tgtOff >= 0 &&
                    actOff + 4 <= 4096 &&
                    tgtOff + 4 <= 4096) {
                    int32_t actRaw = 0;
                    uint8_t* ioMap = etherCATMaster->GetIOMap();
                    std::memcpy(&actRaw, ioMap + actOff, sizeof(actRaw));
                    std::memcpy(ioMap + tgtOff, &actRaw, sizeof(actRaw));
                    std::wstringstream ss;
                    ss << TimestampNowW() << L"[SmoothStopAll] CSP sync 0x607A<-0x6064 slave=" << s
                       << L" raw=" << actRaw << L"\n";
                    allMsg << ss.str();
                }
            }
        }
    }

    // PDO filters are now holding current position with full configured
    // deceleration limits restored. The CSV controller will naturally
    // decelerate each axis toward zero velocity using the filter's
    // deceleration limit. We intentionally do NOT stage 0x60FF=0 here
    // because that would bypass the filter's deceleration ramp and cause
    // an abrupt drive-level quick stop.

    // Send one processdata to make the hold-position effective immediately
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        allMsg << TimestampNowW() << L"[SmoothStopAll] process data sent (deceleration-limited stop via PDOFilter)\n";
    }

    res.success = anySuccess;
    res.message = allMsg.str();
    return res;
}

MotionResult MotionController::PowerOffAsync(int slaveIdx, bool assert) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = TimestampNowW() + std::wstring(L"[PowerOff] invalid args or master missing"); return res; }
    if (!assert) { res.message = TimestampNowW() + std::wstring(L"[PowerOff] release ignored"); return res; }

    if (!pdoDisplay) { res.message = TimestampNowW() + std::wstring(L"[PowerOff] PDO display manager missing"); return res; }

    const uint32_t POWER_OFF_VALUE = 0x0006;
    const PDOItem* cwItem = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (!cwItem) { res.message = TimestampNowW() + std::wstring(L"[PowerOff] controlword item not found"); return res; }
    if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cwItem, (int32_t)POWER_OFF_VALUE)) {
    std::wstringstream ss; ss << TimestampNowW() << L"[PowerOff] PDO staged POWER_OFF slave=" << slaveIdx << L" (0x6040=0x" << std::hex << std::uppercase << POWER_OFF_VALUE << std::dec << L")\n";
        res.success = true; res.message = ss.str();
    } else res.message = TimestampNowW() + std::wstring(L"[PowerOff] PDO staging failed for POWER_OFF");
    return res;
}

MotionResult MotionController::PowerOnAsync(int slaveIdx, bool assert, bool haveParams) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = TimestampNowW() + std::wstring(L"[PowerOn] invalid args or master missing"); return res; }
    if (!assert) { res.message = TimestampNowW() + std::wstring(L"[PowerOn] release ignored"); return res; }

    if (!pdoDisplay) { res.message = TimestampNowW() + std::wstring(L"[PowerOn] PDO display manager missing"); return res; }

    // The following parameter is intentionally unused because automatic writes
    // of 0x60E0/0x60E1 were removed per UI request. Silence unused-parameter warning.
    (void)haveParams;

    // Prepare an ordered step log and an immediate logger for visibility (Debug + stdout)
    std::wstringstream stepLog;
    auto LogPowerOn = [&](const std::string &msg) {
        // use controller source timestamp helper so all controller logs are stamped from the same source
        std::wstring tsw = TimestampNowW();
        std::string ts(tsw.begin(), tsw.end());
        std::string full = ts + msg;
        // immediate outputs for developer visibility (timestamp included)
#if MOTIONCTRL_DEBUG_LOG
        OutputDebugStringA((full + "\n").c_str());
        printf("%s\n", full.c_str());
#endif
        // append to step summary WITH source timestamp so UI log reflects source time
        std::wstring wfull(full.begin(), full.end());
        stepLog << wfull << L"\n";
    };

    // CRITICAL FIX: Force a process data exchange BEFORE reading actual position.
    // After Homing or mode changes, IOmap may contain stale data. By sending/receiving
    // process data here, we ensure IOmap has the latest actual position from the drive.
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        // Small delay to ensure data is settled
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        etherCATMaster->SendProcessDataOnce();
    }

    // Read the configured cyclic operation mode from the master.
    const uint8_t mode = static_cast<uint8_t>(etherCATMaster->GetCyclicOperationMode());
    const bool isCSP = (mode == CoreHelpers::kCia402OperationModeCyclicSyncPosition);

    // === CSP initialization: write actual PDO position to PDO command position ===
    // Before switching to CSP mode, the target position (0x607A) must match
    // the actual position (0x6064) so the drive does not jump on mode entry.
    if (isCSP) {
        // Read actual position from PDO 0x6064 via IOmap.
        // Use the same StagePDOByIndex path for reliability — it finds the
        // correct PDO item in the registered descriptors and writes to IOmap.
        int32_t actualPosition = 0;
        bool gotActualPos = false;
        {
            // Find the 0x6064 item and read its value directly from IOmap
            // via the PDO snapshot mechanism.
            std::vector<uint8_t> snapshot;
            if (etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot) && !snapshot.empty()) {
                // Directly parse 0x6064 from the snapshot at standard TxPDO offset.
                // Snapshot layout: [Obytes output] + [0x6064 at input offset 0 (Startup.csv order)]
                int slaveObytes = 0;
                const auto* ctx = etherCATMaster->GetContext();
                if (ctx && slaveIdx <= ctx->slavecount) {
                    slaveObytes = ctx->slavelist[slaveIdx].Obytes;
                } else {
                    // Fallback: assume 17-byte output (7-entry RxPDO)
                    slaveObytes = EtherCATMaster::kPdoRxByteSize;
                }
                const int offset6064 = slaveObytes + EtherCATMaster::kPdoInOff_ActualPos;
                if (offset6064 + 4 <= static_cast<int>(snapshot.size())) {
                    uint32_t raw = 0;
                    for (int b = 0; b < 4; ++b) {
                        raw |= static_cast<uint32_t>(snapshot[offset6064 + b]) << (8 * b);
                    }
                    actualPosition = static_cast<int32_t>(raw);
                    gotActualPos = true;
                }
            }
        }

        if (gotActualPos) {
            // Use StagePDOByIndex to write actual position to 0x607A reliably.
            if (MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx,
                                             0x607A, static_cast<uint32_t>(actualPosition))) {
                char bf[128]; sprintf_s(bf, sizeof(bf),
                    "[PowerOn] CSP init: staged 0x607A=0x6064=%d", static_cast<int>(actualPosition));
                LogPowerOn(std::string(bf));

                // Verify 0x607A has been updated to match actual position before proceeding.
                // Without this wait, the drive may see stale 0x607A and jump on enable.
                const int kVerifyMaxRetries = 10;
                const int kVerifyIntervalMs = 5;
                bool verified = false;
                for (int retry = 0; retry < kVerifyMaxRetries; ++retry) {
                    if (etherCATMaster->IsRunning()) {
                        etherCATMaster->SendProcessDataOnce();
                        std::this_thread::sleep_for(std::chrono::milliseconds(kVerifyIntervalMs));
                    }
                    std::vector<uint8_t> verifySnap;
                    if (etherCATMaster->GetPDODataSnapshot(slaveIdx, verifySnap) && !verifySnap.empty()) {
                        const PDOItem* tposItem = MotionLogic::FindTargetPositionItem(pdoDisplay->pdoItems);
                        int32_t stagedPos = 0;
                        if (tposItem && ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, verifySnap, *tposItem, stagedPos)) {
                            if (stagedPos == actualPosition) {
                                verified = true;
                                char bf2[128]; sprintf_s(bf2, sizeof(bf2),
                                    "[PowerOn] CSP verify: 0x607A=%d matches after %d retries", static_cast<int>(stagedPos), retry + 1);
                                LogPowerOn(std::string(bf2));
                                break;
                            }
                            char bf2[128]; sprintf_s(bf2, sizeof(bf2),
                                "[PowerOn] CSP verify retry %d: 0x607A=%d != expected %d", retry + 1, static_cast<int>(stagedPos), static_cast<int>(actualPosition));
                            LogPowerOn(std::string(bf2));
                        }
                    }
                }
                if (!verified) {
                    char bf2[128]; sprintf_s(bf2, sizeof(bf2),
                        "[PowerOn] CSP WARNING: 0x607A not verified after %d retries, proceeding anyway", kVerifyMaxRetries);
                    LogPowerOn(std::string(bf2));
                }
            } else {
                char bf[128]; sprintf_s(bf, sizeof(bf),
                    "[PowerOn] CSP init: StagePDOByIndex(0x607A) returned false");
                LogPowerOn(std::string(bf));
            }
        } else {
            char bf[128]; sprintf_s(bf, sizeof(bf),
                "[PowerOn] CSP init: could not read 0x6064 from IOmap snapshot");
            LogPowerOn(std::string(bf));
        }
    }

    // 1) Stage cyclic command PDO to zero before enabling operation.
    //    For CSV: 0x60FF = 0.  For CSP: the command PDO 0x607A was already
    //    staged above with the actual position, so this is a no-op for CSP.
    if (!isCSP) {
        if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, slaveIdx)) {
            res.message = TimestampNowW() + std::wstring(L"[PowerOn] failed to stage PDO 0x60FF=0"); return res; }
        {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOn] staged PDO 0x60FF=0");
            LogPowerOn(std::string(bf));
        }
    }

    // 2) Stage controlword 0x6040 = 0x0F (write command)
    const PDOItem* cwItem = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (!cwItem) { res.message = TimestampNowW() + std::wstring(L"[PowerOn] controlword PDO (0x6040) not found"); return res; }
    int32_t cwVal = 0x000F;
    if (!MotionLogic::StageControlwordWithoutSend(etherCATMaster, pdoDisplay, slaveIdx, cwItem, cwVal)) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOn] failed to stage controlword 0x6040=0x0F"); return res; }
    {
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOn] staged controlword 0x6040=0x%04X", (unsigned)cwVal);
        LogPowerOn(std::string(bf));
    }

    // 3) Write 0x6060 = cyclic operation mode (PDO if extended, SDO if basic)
    if (!etherCATMaster->WriteModeOfOperation((uint16_t)slaveIdx, (uint8_t)mode)) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOn] failed to write 0x6060 cyclic mode"); return res;
    }
    {
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOn] wrote 0x6060=%d (%s)",
                                (int)mode,
                                CoreHelpers::DescribeOperationModeShortName(mode));
        LogPowerOn(std::string(bf));
    }

    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    std::wstringstream ss; ss << TimestampNowW() << L"[PowerOn] final: staged "
                              << (isCSP ? L"0x607A=actual and " : L"0x60FF=0 and ")
                              << L"0x6040=0x0F in "
                              << CoreHelpers::DescribeOperationModeShortName(mode) << L"\n";
    // Merge stepLog (all per-operation logs) with final summary so UI sees full sequence
    std::wstring finalMsg = stepLog.str() + ss.str();
    res.success = true; res.message = finalMsg;
    return res;
}

MotionResult MotionController::PowerOnAllAsync(int slaveCount) {
    MotionResult res{false, L""};
    if (slaveCount <= 0 || !etherCATMaster || !pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] invalid args or master/PDO missing");
        return res;
    }

    // CRITICAL: Set powerOnInProgress flag FIRST to block CyclicTask from writing cyclic motion commands.
    // This must happen before any PDO operations to prevent race conditions
    etherCATMaster->SetPowerOnInProgress(true);
    
    // RAII guard to ensure powerOnInProgress is cleared even on early returns
    struct PowerOnGuard {
        EtherCATMaster* master;
        bool* successPtr;
        ~PowerOnGuard() { 
            // Note: We do NOT clear the flag on success - it will be cleared 
            // when soft-start completes (Auto Connect enables PDO filtering after this).
            // Only clear on failure to allow CyclicTask to resume normal operation.
            if (master && successPtr && !(*successPtr)) {
                master->SetPowerOnInProgress(false);
            }
        }
    };
    bool success = false;
    PowerOnGuard guard{etherCATMaster, &success};

#if MOTIONCTRL_DEBUG_LOG
    constexpr bool kPowerOnAllVerbose = true;
#else
    constexpr bool kPowerOnAllVerbose = false;
#endif
    
    std::wstringstream stepLog;
    auto LogAll = [&](const std::string &msg) {
        if (!kPowerOnAllVerbose) return;
        std::wstring tsw = TimestampNowW();
        std::string ts(tsw.begin(), tsw.end());
        std::string full = ts + msg;
#if MOTIONCTRL_DEBUG_LOG
        OutputDebugStringA((full + "\n").c_str());
        printf("%s\n", full.c_str());
#endif
        std::wstring wfull(full.begin(), full.end());
        stepLog << wfull << L"\n";
    };

    LogAll("[PowerOnAll] Started - CyclicTask command writes blocked");

    // Note: We do NOT trigger soft-start here. PDO filtering is not yet enabled
    // during PowerOnAllAsync, so CyclicTask won't write cyclic commands anyway.
    // Auto Connect will call TriggerAllPDOFiltersSoftStart() after this function
    // completes and before enabling PDO filtering.

    // PHASE 1: Send/receive process data to get latest actual positions from all drives
    LogAll("[PowerOnAll] Phase 1: Refreshing IOmap with current actual positions");
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        etherCATMaster->SendProcessDataOnce();
    }

    // Find 0x6064 (Position Actual Value) early - we need it for stabilization check
    const PDOItem* posItem = MotionLogic::FindActualPositionItem(pdoDisplay->pdoItems);
    if (!posItem) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] PDO 0x6064 not found");
        return res;
    }

    // Reuse snapshot buffers per slave to reduce allocations during PowerOnAll
    std::vector<std::vector<uint8_t>> snapshotCache(slaveCount + 1);

    // PHASE 1.5: Wait for all axes to stabilize (stop moving)
    // Use 40ms windows instead of 100ms for faster adaptive stabilization.
    LogAll("[PowerOnAll] Phase 1.5: Waiting for axes to stabilize (adaptive)...");
    
    const int MAX_STABILIZE_WAIT_MS = 2000;  // Max 2 seconds
    const int STABILIZE_WINDOW_MS = 40;      // Measure change over 40ms window
    const int POLL_INTERVAL_MS = 10;         // Poll every 10ms  
    const int POLLS_PER_WINDOW = STABILIZE_WINDOW_MS / POLL_INTERVAL_MS;
    const int32_t STABLE_THRESHOLD = 20;     // Position change < 20 counts
    const int STABLE_WINDOWS_REQUIRED = 2;   // Need 2 consecutive stable windows (80ms total)
    
    // Try to find velocity item for faster "already stopped" detection
    const PDOItem* velItem = MotionLogic::FindPDOItemByIndex(pdoDisplay->pdoItems, 0x606C, true, false);
    if (velItem) LogAll("[PowerOnAll] Phase 1.5: Velocity-aware stabilization enabled (0x606C found)");

    // Track position at start of each measurement window
    std::vector<int32_t> windowStartPositions(slaveCount + 1, 0);
    std::vector<int> stableWindowCount(slaveCount + 1, 0);
    
    auto stabilizeStart = std::chrono::steady_clock::now();
    bool allStable = false;
    int totalPolls = 0;
    int pollsInCurrentWindow = 0;
    int windowNumber = 0;
    
    // Read initial positions for the first window
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }
    for (int si = 1; si <= slaveCount; ++si) {
        auto &snap = snapshotCache[si];
        if (etherCATMaster->GetPDODataSnapshot(si, snap)) {
            ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *posItem, windowStartPositions[si]);
        }
    }
    
    while (!allStable) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stabilizeStart).count();
        if (elapsed > MAX_STABILIZE_WAIT_MS) {
            LogAll("[PowerOnAll] Phase 1.5: TIMEOUT waiting for axes to stabilize, proceeding anyway");
            break;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
        totalPolls++;
        pollsInCurrentWindow++;
        
        if (etherCATMaster->IsRunning()) {
            etherCATMaster->SendProcessDataOnce();
        }

        // Logic Check: If velocity-aware, we can see if all axes are ALREADY stopped (V=0)
        // This allows skipping windows if the machine is truly stationary.
        if (velItem) {
            bool currentSnapshotStopped = true;
            for (int si = 1; si <= slaveCount; ++si) {
                auto &snap = snapshotCache[si];
                int32_t curVel = 0;
                if (!etherCATMaster->GetPDODataSnapshot(si, snap) || 
                    !ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *velItem, curVel) ||
                    std::abs(curVel) > 5) { // 5 internal units tolerance
                    currentSnapshotStopped = false;
                    break;
                }
            }
            if (currentSnapshotStopped && elapsed > 50) { // Still need a tiny minimum wait for safety
                LogAll("[PowerOnAll] Phase 1.5: Instant stabilization (Zero Velocity detected for all axes)");
                allStable = true;
                break;
            }
        }
        
        // Evaluate stability at the end of each window
        if (pollsInCurrentWindow >= POLLS_PER_WINDOW) {
            pollsInCurrentWindow = 0;
            windowNumber++;
            allStable = true;
            
            char windowHeader[128];
            sprintf_s(windowHeader, sizeof(windowHeader), "[PowerOnAll] Phase 1.5: === Window %d evaluation (elapsed %lldms) ===", 
                      windowNumber, elapsed);
            LogAll(windowHeader);
            
            for (int si = 1; si <= slaveCount; ++si) {
                auto &snap = snapshotCache[si];
                if (!etherCATMaster->GetPDODataSnapshot(si, snap)) {
                    allStable = false;
                    stableWindowCount[si] = 0;
                    char bf[128]; 
                    sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 1.5: slave %d FAILED GetPDODataSnapshot", si);
                    LogAll(bf);
                    continue;
                }
                
                int32_t curPos = 0;
                if (!ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *posItem, curPos)) {
                    allStable = false;
                    stableWindowCount[si] = 0;
                    char bf[128]; 
                    sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 1.5: slave %d FAILED ReadPDOItemValue", si);
                    LogAll(bf);
                    continue;
                }
                
                int32_t windowDiff = std::abs(curPos - windowStartPositions[si]);
                int32_t prevPos = windowStartPositions[si];
                
                if (windowDiff < STABLE_THRESHOLD) {
                    stableWindowCount[si]++;
                    char bf[192]; 
                    sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 1.5: slave %d STABLE (diff=%d < %d) stableCount=%d/%d  pos: 0x%08X -> 0x%08X", 
                              si, windowDiff, STABLE_THRESHOLD, stableWindowCount[si], STABLE_WINDOWS_REQUIRED,
                              (uint32_t)prevPos, (uint32_t)curPos);
                    LogAll(bf);
                    if (stableWindowCount[si] < STABLE_WINDOWS_REQUIRED) {
                        allStable = false;
                    }
                } else {
                    // Still moving - reset stable counter
                    stableWindowCount[si] = 0;
                    allStable = false;
                    char bf[192]; 
                    sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 1.5: slave %d MOVING (diff=%d >= %d) stableCount=0  pos: 0x%08X -> 0x%08X", 
                              si, windowDiff, STABLE_THRESHOLD, 
                              (uint32_t)prevPos, (uint32_t)curPos);
                    LogAll(bf);
                }
                
                // Start new window from current position
                windowStartPositions[si] = curPos;
            }
        }
    }
    
    {
        int totalTimeMs = totalPolls * POLL_INTERVAL_MS;
        char bf[128]; 
        sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 1.5: Stabilization complete after %d windows (%dms total)", 
                  windowNumber, totalTimeMs);
        LogAll(bf);
    }

    // PHASE 2: Stage the cyclic command PDO for all axes.
    //   CSP: write actual position (0x6064) to target position (0x607A)
    //   CSV: stage 0x60FF = 0
    const uint8_t batchMode = static_cast<uint8_t>(etherCATMaster->GetCyclicOperationMode());
    const bool batchIsCSP = (batchMode == CoreHelpers::kCia402OperationModeCyclicSyncPosition);
    LogAll(batchIsCSP ? "[PowerOnAll] Phase 2: Staging 0x607A=0x6064 for all axes (CSP)"
                      : "[PowerOnAll] Phase 2: Staging 0x60FF=0 for all axes (CSV)");
    for (int si = 1; si <= slaveCount; ++si) {
        if (batchIsCSP) {
            // CSP: read actual position from 0x6064 and stage to 0x607A via StagePDOByIndex.
            int32_t actualPos = 0;
            auto &snap = snapshotCache[si];
            if (etherCATMaster->GetPDODataSnapshot(si, snap)) {
                ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *posItem, actualPos);
            }
            // Use StagePDOByIndex for reliable direct-to-IOmap write (same path as CSV's StageZeroVelocityCommand).
            MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, si,
                                         0x607A, static_cast<uint32_t>(actualPos));
            char bf[128]; sprintf_s(bf, sizeof(bf),
                "[PowerOnAll] slave %d: staged 0x607A=0x6064=%d (CSP init)", si, static_cast<int>(actualPos));
            LogAll(bf);
            // Store expected position for verification
            snapshotCache[si].resize(sizeof(int32_t));
            memcpy(snapshotCache[si].data(), &actualPos, sizeof(actualPos));
        } else {
            if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, si)) {
                char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 2: failed to stage 0x60FF=0 for slave %d", si);
                LogAll(bf);
                res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to stage PDO 0x60FF=0");
                return res;
            }
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: staged 0x60FF=0", si);
            LogAll(bf);
        }
    }
    
    // Phase 2.5: Send the staged commands before the mode change so the drives
    // see the correct PDO values (CSP: 0x607A=actual; CSV: 0x60FF=0) when
    // the new mode is acknowledged.
    LogAll(batchIsCSP
        ? "[PowerOnAll] Phase 2.5: Sending staged CSP position commands to IOmap"
        : "[PowerOnAll] Phase 2.5: Sending staged zero-velocity commands to IOmap");
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // Phase 2.6 (CSP only): Verify 0x607A has been updated to match actual position
    // for all axes. The drive must see the correct command position before mode switch
    // to prevent position jumps on enable.
    if (batchIsCSP) {
        LogAll("[PowerOnAll] Phase 2.6: Verifying 0x607A matches actual position for all axes");
        const PDOItem* tposItem = MotionLogic::FindTargetPositionItem(pdoDisplay->pdoItems);
        const int kVerifyMaxRetries = 10;
        const int kVerifyIntervalMs = 5;
        for (int retry = 0; retry < kVerifyMaxRetries; ++retry) {
            if (etherCATMaster->IsRunning()) {
                etherCATMaster->SendProcessDataOnce();
                std::this_thread::sleep_for(std::chrono::milliseconds(kVerifyIntervalMs));
            }
            bool allVerified = true;
            for (int si = 1; si <= slaveCount; ++si) {
                std::vector<uint8_t> verifySnap;
                if (!etherCATMaster->GetPDODataSnapshot(si, verifySnap) || verifySnap.empty()) {
                    allVerified = false; continue;
                }
                int32_t stagedPos = 0;
                if (!tposItem || !ReadPDOItemValueFromSnapshot(etherCATMaster, si, verifySnap, *tposItem, stagedPos)) {
                    allVerified = false; continue;
                }
                int32_t expectedPos = 0;
                if (snapshotCache[si].size() >= sizeof(int32_t))
                    memcpy(&expectedPos, snapshotCache[si].data(), sizeof(expectedPos));
                if (stagedPos != expectedPos) {
                    allVerified = false;
                    char bf2[128]; sprintf_s(bf2, sizeof(bf2),
                        "[PowerOnAll] Phase 2.6: slave %d retry %d: 0x607A=%d != expected %d",
                        si, retry + 1, static_cast<int>(stagedPos), static_cast<int>(expectedPos));
                    LogAll(bf2);
                }
            }
            if (allVerified) {
                char bf2[128]; sprintf_s(bf2, sizeof(bf2),
                    "[PowerOnAll] Phase 2.6: All axes verified after %d retries", retry + 1);
                LogAll(bf2);
                break;
            }
            if (retry == kVerifyMaxRetries - 1) {
                LogAll("[PowerOnAll] Phase 2.6: WARNING - not all axes verified, proceeding anyway");
            }
        }
    }

    // PHASE 2.7 (CSP only): Clear the persistent staged 0x607A entries
    // created in Phase 2.  Leaving them in place would cause CyclicTask to
    // overwrite IOmap[0x607A] with the snapshot value every cycle, defeating
    // the ongoing CSP sync in PDOProcessingTask.  ClearStagedOutputsForCyclic-
    // MotionCommands also CSP-syncs 0x607A←0x6064 as a final safety net.
    if (batchIsCSP) {
        LogAll("[PowerOnAll] Phase 2.7: Clearing staged 0x607A entries, letting CSP sync take over");
        etherCATMaster->ClearStagedOutputsForCyclicMotionCommands();
    }

    // PHASE 3: Write 0x6060 for ALL axes (PDO if extended, SDO if basic)
    LogAll("[PowerOnAll] Phase 3: Writing operation mode for all axes");
    for (int si = 1; si <= slaveCount; ++si) {
        if (!etherCATMaster->WriteModeOfOperation((uint16_t)si, (uint8_t)batchMode)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] failed to write 0x6060=%d for slave %d", (int)batchMode, si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to write 0x6060 cyclic mode");
            return res;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: wrote 0x6060=%d (%s)",
                                si, (int)batchMode, CoreHelpers::DescribeOperationModeShortName(batchMode));
        LogAll(bf);
    }

    // PHASE 4: Wait for axes to FULLY STOP after the mode change.
    LogAll("[PowerOnAll] Phase 4: Waiting for axes to stop after mode change...");
    {
        const int MAX_WAIT_MS = 1000;
        const int POLL_INTERVAL_MS = 10;
        const int32_t STOP_THRESHOLD = 10;
        const int32_t STOP_VELOCITY_THRESHOLD = 5;
        const int STABLE_COUNT_REQUIRED = 3;  // Reduced from 5 (30ms total)
        
        std::vector<int32_t> lastPositions(slaveCount + 1, 0);
        std::vector<int> stableCounts(slaveCount + 1, 0);
        
        // Read initial positions for fallback position-based settle detection.
        if (etherCATMaster->IsRunning()) {
            etherCATMaster->SendProcessDataOnce();
        }
        for (int si = 1; si <= slaveCount; ++si) {
            auto &snap = snapshotCache[si];
            if (etherCATMaster->GetPDODataSnapshot(si, snap)) {
                ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *posItem, lastPositions[si]);
            }
        }
        
        auto startWait = std::chrono::steady_clock::now();
        bool allStopped = false;
        int pollCount = 0;
        
        while (!allStopped) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startWait).count();
            if (elapsed > MAX_WAIT_MS) {
                LogAll("[PowerOnAll] Phase 4: Timeout waiting for axes to stop, proceeding anyway");
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
            pollCount++;

            if (etherCATMaster->IsRunning()) {
                etherCATMaster->SendProcessDataOnce();
            }
            
            allStopped = true;
            for (int si = 1; si <= slaveCount; ++si) {
                auto &snap = snapshotCache[si];
                if (!etherCATMaster->GetPDODataSnapshot(si, snap)) {
                    allStopped = false;
                    stableCounts[si] = 0;
                    continue;
                }
                
                int32_t curPos = 0;
                if (!ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *posItem, curPos)) {
                    allStopped = false;
                    stableCounts[si] = 0;
                    continue;
                }

                if (velItem) {
                    int32_t curVel = 0;
                    if (!ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *velItem, curVel)) {
                        allStopped = false;
                        stableCounts[si] = 0;
                        continue;
                    }

                    if (std::abs(curVel) <= STOP_VELOCITY_THRESHOLD) {
                        stableCounts[si]++;
                        if (stableCounts[si] < STABLE_COUNT_REQUIRED) {
                            allStopped = false;
                        }
                    } else {
                        stableCounts[si] = 0;
                        allStopped = false;
                        if (pollCount % 10 == 0) {
                            char bf[128];
                            sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 4: slave %d velocity still non-zero (vel=%d)", si, curVel);
                            LogAll(bf);
                        }
                    }
                    continue;
                }
                
                int32_t diff = std::abs(curPos - lastPositions[si]);
                lastPositions[si] = curPos;
                
                if (diff < STOP_THRESHOLD) {
                    stableCounts[si]++;
                    if (stableCounts[si] < STABLE_COUNT_REQUIRED) {
                        allStopped = false;
                    }
                } else {
                    stableCounts[si] = 0;
                    allStopped = false;
                    if (pollCount % 10 == 0) {  // Log every 100ms
                        char bf[128]; 
                        sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 4: slave %d still moving (diff=%d)", si, diff);
                        LogAll(bf);
                    }
                }
            }
        }
        
        char bf[128]; 
        sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 4: Axes stopped after %dms", pollCount * POLL_INTERVAL_MS);
        LogAll(bf);
    }

    // PHASE 5: Restage zero velocity right before enabling operation.
    LogAll("[PowerOnAll] Phase 5: Restaging 0x60FF=0 before enabling operation");
    for (int si = 1; si <= slaveCount; ++si) {
        if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, si)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 5: failed to stage 0x60FF=0 for slave %d", si);
            LogAll(bf);
            continue;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: final 0x60FF=0", si);
        LogAll(bf);
    }

    // PHASE 6: CiA402 state-machine sequence for all axes.
    // Some drives (e.g. strict CiA402 implementations on Axis 5/7) reject the
    // direct Ready-to-Switch-On → Operation-Enabled shortcut (0x000F) and
    // require the intermediate Switch-On step (0x0007 → Switched On) first.
    // We stage 0x0007, send, wait for the drives to acknowledge Switched On,
    // then allow extra settling cycles before staging 0x000F in Phase 7.
    LogAll("[PowerOnAll] Phase 6: Staging controlword 0x6040=0x0007 (Switch On) for all axes");
    const PDOItem* cwItem = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (!cwItem) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] controlword PDO (0x6040) not found");
        return res;
    }

    for (int si = 1; si <= slaveCount; ++si) {
        if (!MotionLogic::StageControlwordWithoutSend(etherCATMaster, pdoDisplay, si, cwItem, 0x0007)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] failed to stage 0x6040=0x0007 for slave %d", si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to stage controlword 0x6040=0x0007");
            return res;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: staged 0x6040=0x0007", si);
        LogAll(bf);
    }

    // Send the staged 0x0007 and wait for drives to reach Switched On (0x0233).
    LogAll("[PowerOnAll] Phase 6: Sending 0x0007 and waiting for Switched On state");
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }
    {
        // Poll statusword for up to 500ms; all axes must report 0x0233 (Switched On)
        // or already 0x1637 (Operation Enabled, from a prior enable).
        const int kSwitchOnTimeoutMs = 500;
        const int kSwitchOnPollMs = 10;
        const PDOItem* swItem = MotionLogic::FindStatuswordItem(pdoDisplay->pdoItems);
        int waited = 0;
        bool allSwitchedOn = false;
        while (waited < kSwitchOnTimeoutMs) {
            if (etherCATMaster->IsRunning()) {
                etherCATMaster->SendProcessDataOnce();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kSwitchOnPollMs));
            waited += kSwitchOnPollMs;

            allSwitchedOn = true;
            for (int si = 1; si <= slaveCount; ++si) {
                if (!swItem) { allSwitchedOn = false; break; }
                std::vector<uint8_t> snap;
                if (!etherCATMaster->GetPDODataSnapshot(si, snap)) { allSwitchedOn = false; continue; }
                int32_t sw = 0;
                if (!ReadPDOItemValueFromSnapshot(etherCATMaster, si, snap, *swItem, sw)) { allSwitchedOn = false; continue; }
                uint32_t usw = static_cast<uint32_t>(sw);
                // Accept Switched On (0x0233) or already Operation Enabled (0x1637/0x0237)
                const uint32_t kSwitchedOnMask = 0x006F;  // bits 0-3,5-6
                const uint32_t kSwitchedOnVal  = 0x0023;  // SO=1,RTSO=1,VE=1
                if ((usw & kSwitchedOnMask) != kSwitchedOnVal) {
                    // Also accept if already at Operation Enabled
                    const uint32_t kOpEnabledVal = 0x0027;
                    if ((usw & kSwitchedOnMask) != kOpEnabledVal) {
                        allSwitchedOn = false;
                        if (waited == kSwitchOnPollMs) {
                            char bf[128];
                            sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 6: slave %d waiting for Switched On (sw=0x%04X)", si, (unsigned)usw);
                            LogAll(bf);
                        }
                    }
                }
            }
            if (allSwitchedOn) break;
        }
        if (!allSwitchedOn) {
            LogAll("[PowerOnAll] Phase 6: Timeout waiting for Switched On, proceeding with 0x000F anyway");
        } else {
            char bf[128];
            sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 6: All axes reached Switched On after %dms", waited);
            LogAll(bf);

            // Extra settling: send 2 more frames of 0x0007 and wait 30ms before
            // transitioning to 0x000F.  Strict drives (e.g. Axis 5/7) may reject
            // a back-to-back 0x0007→0x000F transition without an intermediate
            // stabilisation period in Switched On.
            LogAll("[PowerOnAll] Phase 6: Extra settling — 2 more frames of 0x0007");
            for (int settle = 0; settle < 2; ++settle) {
                if (etherCATMaster->IsRunning()) {
                    etherCATMaster->SendProcessDataOnce();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            LogAll("[PowerOnAll] Phase 6: Settling complete, proceeding to 0x000F");
        }
    }

    // PHASE 7: Stage controlword 0x6040=0x000F (Enable Operation) for ALL axes.
    LogAll("[PowerOnAll] Phase 7: Staging controlword (0x6040=0x000F) for all axes");

    for (int si = 1; si <= slaveCount; ++si) {
        if (!MotionLogic::StageControlwordWithoutSend(etherCATMaster, pdoDisplay, si, cwItem, 0x000F)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] failed to stage 0x6040=0x000F for slave %d", si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to stage controlword 0x6040=0x000F");
            return res;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: staged 0x6040=0x000F", si);
        LogAll(bf);
    }

    // Send the staged 0x000F immediately.
    LogAll("[PowerOnAll] Phase 7: Sending staged PDO data to ALL axes simultaneously");
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    LogAll("[PowerOnAll] Complete: All axes powered on simultaneously");
    success = true;  // Mark success so guard doesn't clear powerOnInProgress
    res.success = true;
    if (kPowerOnAllVerbose) {
        res.message = stepLog.str();
    } else {
        res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] complete");
    }
    return res;
}

bool MotionController::HomePrepareSDO(int slaveIdx, bool haveP13, uint32_t p13) {
    if (slaveIdx <= 0 || !etherCATMaster) return false;

    // 1) Write 0x6060 = 6 (homing mode; PDO if extended, SDO if basic)
    if (!etherCATMaster->WriteModeOfOperation((uint16_t)slaveIdx, 6)) {
        return false;
    }

    // 2) Write 0x6098 via SDO: select homing method per detected drive type
    {
        int8_t method = EtherCATMaster::kHomingMethodDvs; // default DVS
        const ecx_contextt* cx = etherCATMaster->GetContext();
        if (cx && slaveIdx <= cx->slavecount) {
            method = EtherCATMaster::GetHomingMethod(cx->slavelist[slaveIdx].eep_man);
        }
        int probeSz = 4;
        uint8_t probeBuf[4] = {0};
        bool wrote = false;
        if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6098, (uint8_t)0x00, (void*)probeBuf, probeSz)) {
            if (probeSz == 1) {
                uint8_t v8 = (uint8_t)method;
                wrote = etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v8, 1);
            } else if (probeSz == 2) {
                uint16_t v16 = (uint16_t)method;
                wrote = etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v16, 2);
            } else {
                uint32_t v32 = (uint32_t)method;
                wrote = etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v32, 4);
            }
        }
        if (!wrote) {
            uint32_t v6098 = (uint32_t)method;
            wrote = etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6098, (uint8_t)0x00, (void*)&v6098, (int)sizeof(v6098));
        }
        if (!wrote) return false;
    }

    // 3) Write 0x607C via SDO
    {
        uint32_t v607c = haveP13 ? p13 : 0;
        if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x607C, (uint8_t)0x00, (void*)&v607c, (int)sizeof(v607c))) {
            return false;
        }
    }

    return true;
}

MotionResult MotionController::HomeAsync(int slaveIdx, bool assert, bool haveP13, uint32_t p13, bool skipSdo) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] invalid args or master missing"); return res; }
    if (!assert) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] release ignored"); return res; }
    if (!pdoDisplay) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] PDO display manager missing"); return res; }
    // Origin-setting logger: timestamped, tagged messages routed to UI via SetError and to stdout/debugger
    auto LogHome = [&](const std::string &msg){
        // use controller source timestamp helper for accurate milliseconds
        std::wstring tsw = TimestampNowW();
        std::string ts(tsw.begin(), tsw.end());
        std::string out = ts + std::string("[OriginSet] ") + msg;
#if MOTIONCTRL_DEBUG_LOG
        OutputDebugStringA((out + "\n").c_str());
        printf("%s\n", out.c_str());
#else
        (void)out; // suppress unused variable warning
#endif
    };
    auto AppendFileMC = [&](const char* txt){ LogHome(std::string(txt)); };
    // Accumulate a UI-visible summary of steps performed so OnHome can post it
    std::wstringstream stepLog;
    // find PDO items: controlword(0x6040 output) and status(0x6041 input)
    const PDOItem* cwItem = nullptr;
    const PDOItem* statusItem = nullptr;
    for (const auto &it : pdoDisplay->pdoItems) {
        if (it.index == 0x6040 && it.isOutput) cwItem = &it;
        else if (it.index == 0x6041 && !it.isOutput) statusItem = &it;
    }
    {
        AppendFileMC("HomeAsync: after pdoItems scan\n");
    if (!cwItem) AppendFileMC("HomeAsync: cwItem MISSING\n"); else AppendFileMC("HomeAsync: cwItem OK\n");
    if (!statusItem) AppendFileMC("HomeAsync: statusItem MISSING\n"); else AppendFileMC("HomeAsync: statusItem OK\n");
    AppendFileMC("HomeAsync: mode display (0x6061) from PDO cache\n");
    }
    if (!cwItem) {
        std::wstringstream ss; ss << TimestampNowW() << L"[OriginSet] controlword PDO (0x6040) not found\nAvailable PDO items:\n";
        for (const auto &it : pdoDisplay->pdoItems) {
            ss << L"  index=0x" << std::hex << std::uppercase << it.index << std::dec << L" sub=0x" << (int)it.subindex << L" isOutput=" << (it.isOutput ? L"Y" : L"N") << L" name=\"" << it.name << L"\"\n";
        }
        res.message = ss.str();
        return res;
    }
    if (!statusItem) {
        std::wstringstream ss; ss << TimestampNowW() << L"[OriginSet] status PDO (0x6041) not found\nAvailable PDO items:\n";
        for (const auto &it : pdoDisplay->pdoItems) {
            ss << L"  index=0x" << std::hex << std::uppercase << it.index << std::dec << L" sub=0x" << (int)it.subindex << L" isOutput=" << (it.isOutput ? L"Y" : L"N") << L" name=\"" << it.name << L"\"\n";
        }
        res.message = ss.str();
        return res;
    }
    // Read current mode display from PDO cache (lock-free, no snapshot needed)
    {
        uint8_t tmp = 0;
        if (etherCATMaster->GetPdo6061(static_cast<uint16_t>(slaveIdx), tmp)) {
            char bf[64]; sprintf_s(bf, sizeof(bf), "HomeAsync: PDO 0x6061=%u\n", tmp); AppendFileMC(bf);
        }
    }

    // Read current 6040 from snapshot and require bit4 == 0 before starting
    std::vector<uint8_t> snap;
    AppendFileMC("HomeAsync: before GetPDODataSnapshot initial\n");
    if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snap)) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to get PDO snapshot"); return res; }
    AppendFileMC("HomeAsync: after GetPDODataSnapshot initial\n");
    int32_t curCW = 0;
    if (!ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snap, *cwItem, curCW)) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to read controlword from snapshot"); return res; }
    const uint32_t CMD_BIT4 = 0x0010;
    if ((static_cast<uint32_t>(curCW) & CMD_BIT4) != 0) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] controlword bit4 already set; abort"); return res; }

    // NOTE: BUSY flag must be managed by UI. MotionController will only perform SDO/PDO operations.

    // 1-3) SDO preamble: 0x6060=6, 0x6098=35, 0x607C — skip if caller already ran HomePrepareSDO
    if (!skipSdo) {
        if (!HomePrepareSDO(slaveIdx, haveP13, p13)) {
            AppendFileMC("HomeAsync: HomePrepareSDO FAILED\n");
            res.message = TimestampNowW() + std::wstring(L"[OriginSet] SDO preamble failed"); return res;
        }
        AppendFileMC("HomeAsync: SDO preamble OK\n");
        stepLog << TimestampNowW() << L"[OriginSet] SDO preamble complete\n";
    } else {
        AppendFileMC("HomeAsync: skipping SDO preamble (already prepared)\n");
    }

    // 4) Stage PDO controlword = 0x0F (write command) via staging
    AppendFileMC("HomeAsync: before StageControlword -> 0x0F\n");
    int32_t st0 = 0x0F;
    if (!MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cwItem, st0)) {
        AppendFileMC("HomeAsync: StageControlword 0x0F FAILED\n");
        res.message = TimestampNowW() + std::wstring(L"[OriginSet] PDO staging failed (write 0x0F)"); return res;
    }
    AppendFileMC("HomeAsync: after StageControlword -> 0x0F (OK)\n");
    stepLog << TimestampNowW() << L"[OriginSet] staged controlword 0x6040=0x0F\n";

    // CRITICAL: Wait at least one PDO cycle for the drive to process 0x0F
    // before proceeding to 0x1F. Without this wait, the drive may see 0x1F
    // before acknowledging 0x0F, causing the homing sequence to fail.
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        etherCATMaster->SendProcessDataOnce();
    }
    AppendFileMC("HomeAsync: waited for 0x0F to settle before 0x1F\n");

    // Mode verification removed: 0x6061 is now cached every PDO cycle and
    // the drive accepts mode changes within 1-2 cycles. The statusword polling
    // below will catch any actual failure (e.g. mode not accepted).

    // Capture the pre-start statusword so we can reject stale "already complete"
    // states such as 0x1637 that may still be visible when origin-setting has not yet run.
    uint32_t preStartStatus = 0;
    bool havePreStartStatus = false;
    {
        std::vector<uint8_t> preStartSnap;
        if (etherCATMaster->GetPDODataSnapshot(slaveIdx, preStartSnap)) {
            int32_t preStartVal = 0;
            if (ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, preStartSnap, *statusItem, preStartVal)) {
                preStartStatus = static_cast<uint32_t>(preStartVal);
                havePreStartStatus = true;
                char bf[128];
                sprintf_s(bf, sizeof(bf), "HomeAsync: pre-start status=0x%08X\n", (unsigned)preStartStatus);
                AppendFileMC(bf);
            }
        }
    }

    // 5) Stage PDO controlword = 0x1F (start origin-setting sequence)
    AppendFileMC("HomeAsync: before StageControlword -> 0x1F\n");
    int32_t st1 = 0x1F;
    if (!MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cwItem, st1)) {
        AppendFileMC("HomeAsync: StageControlword 0x1F FAILED\n");
        res.message = TimestampNowW() + std::wstring(L"[OriginSet] PDO staging failed (write 0x1F)"); return res;
    }
    AppendFileMC("HomeAsync: after StageControlword -> 0x1F (OK)\n");
    stepLog << TimestampNowW() << L"[OriginSet] staged controlword 0x6040=0x1F\n";
    // Verify bit4 set (part of 0x1F)
    AppendFileMC("HomeAsync: before VerifyPDOBitSet (bit4)\n");
    if (!MotionLogic::VerifyPDOBitSet(etherCATMaster, cwItem, slaveIdx, CMD_BIT4, 5, 10)) {
        AppendFileMC("HomeAsync: VerifyPDOBitSet FAILED\n");
        res.message = TimestampNowW() + std::wstring(L"[OriginSet] PDO verify failed (bit4 not set after 0x1F)"); return res; }
    AppendFileMC("HomeAsync: after VerifyPDOBitSet (OK)\n");

    // 6) poll 0x6041 via snapshot for up to 3s
    const int timeoutMs = 3000;
    const int pollIntervalMs = 10;
    int elapsed = 0;
    uint32_t lastStatus = 0;
    bool finished = false;
    bool sawStatusChangeAfterStart = false;
    bool sawNonCompleteAfterStart = false;
    bool loggedIgnoredStaleComplete = false;
    std::vector<uint8_t> snap3;
    while (elapsed < timeoutMs) {
        if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snap3)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "HomeAsync: GetPDODataSnapshot failed at elapsed=%d\n", elapsed); AppendFileMC(bf);
            Sleep(pollIntervalMs); elapsed += pollIntervalMs; continue; }

        int32_t sval = 0;
        if (ReadPDOItemValueFromSnapshot(etherCATMaster, slaveIdx, snap3, *statusItem, sval)) {
            lastStatus = (uint32_t)sval;
            char bf[128]; sprintf_s(bf, sizeof(bf), "HomeAsync: poll elapsed=%d status=0x%08X\n", elapsed, (unsigned)lastStatus); AppendFileMC(bf);
            bool b12 = (lastStatus & 0x1000U) != 0;
            bool b10 = (lastStatus & 0x0400U) != 0;
            bool b13 = (lastStatus & 0x2000U) != 0;
            bool complete = b12 && b10 && !b13;
            if (!havePreStartStatus || lastStatus != preStartStatus) {
                sawStatusChangeAfterStart = true;
            }
            if (!complete) {
                sawNonCompleteAfterStart = true;
            }
                if (b13) {
                std::wstringstream ss; ss << TimestampNowW() << L"[OriginSet] homing error (status bit13) slave=" << slaveIdx << L" status=0x" << std::hex << std::uppercase << lastStatus << std::dec << L"\n";
                AppendFileMC("HomeAsync: homing error detected (bit13)\n");
                res.success = false; res.message = ss.str(); finished = true; break;
            }
            if (complete) {
                    const bool preStartComplete = havePreStartStatus &&
                        ((preStartStatus & 0x1000U) != 0) &&
                        ((preStartStatus & 0x0400U) != 0) &&
                        ((preStartStatus & 0x2000U) == 0);
                    if (!sawStatusChangeAfterStart || (preStartComplete && !sawNonCompleteAfterStart)) {
                        if (!loggedIgnoredStaleComplete) {
                            char bfIgnore[160];
                            sprintf_s(bfIgnore, sizeof(bfIgnore),
                                "HomeAsync: ignoring stale complete status=0x%08X preStart=0x%08X changed=%d sawNonComplete=%d\n",
                                (unsigned)lastStatus,
                                (unsigned)preStartStatus,
                                sawStatusChangeAfterStart ? 1 : 0,
                                sawNonCompleteAfterStart ? 1 : 0);
                            AppendFileMC(bfIgnore);
                            loggedIgnoredStaleComplete = true;
                        }
                        Sleep(pollIntervalMs); elapsed += pollIntervalMs; continue;
                    }
                    // Log final origin-setting completion message with status
                    {
                        char bf2[128]; sprintf_s(bf2, sizeof(bf2), "homing completed slave=%d status=0x%04X", slaveIdx, (unsigned)lastStatus);
                        AppendFileMC(bf2);
                    }
                    AppendFileMC("HomeAsync: homing completed detected (b12&&b10)\n");
                    {
                        bool havePostHomeTarget = false;
                        int32_t expectedHomePosition = 0;
                        // Before returning success: read SDO 0x607C and stage it into PDO 0x607A
                        uint32_t v607c = 0; int sz607c = (int)sizeof(v607c);
                        if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x607C, (uint8_t)0x00, (void*)&v607c, sz607c)) {
                            havePostHomeTarget = true;
                            expectedHomePosition = static_cast<int32_t>(v607c);
                            char bf3[128]; sprintf_s(bf3, sizeof(bf3), "HomeAsync: read SDO 0x607C val=0x%08X\n", v607c); AppendFileMC(bf3);
                            if (MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x607A, (uint32_t)v607c)) {
                                std::wstringstream ssx; ssx << L"[OriginSet] staged PDO 0x607A=0x" << std::hex << std::uppercase << v607c << std::dec << L"\n";
                                stepLog << ssx.str();
                                AppendFileMC("HomeAsync: staged PDO 0x607A OK\n");
                            } else {
                                AppendFileMC("HomeAsync: failed to stage PDO 0x607A\n");
                                stepLog << TimestampNowW() << L"[OriginSet] FAILED to stage PDO 0x607A\n";
                            }
                        } else {
                            AppendFileMC("HomeAsync: failed to read SDO 0x607C\n");
                            stepLog << TimestampNowW() << L"[OriginSet] FAILED to read SDO 0x607C\n";
                        }

                        // Stage controlword 0x6040 = 0x0F so UI can clear BUSY
                        int32_t postFinal = 0x0F;
                            if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cwItem, postFinal)) {
                            AppendFileMC("HomeAsync: staged controlword 0x0F after completion\n");
                            stepLog << TimestampNowW() << L"[OriginSet] staged controlword 0x6040=0x0F (post-complete)\n";
                        } else {
                            AppendFileMC("HomeAsync: failed to stage controlword 0x0F after completion\n");
                            stepLog << TimestampNowW() << L"[OriginSet] FAILED to stage controlword 0x6040=0x0F (post-complete)\n";
                        }

                        if (etherCATMaster->IsSimulationMode() && havePostHomeTarget) {
                            constexpr int32_t kSimulationHomeSettleToleranceCounts = 4;
                            constexpr int kSimulationHomeSettleTimeoutMs = 1500;
                            constexpr int kSimulationHomeSettlePollMs = 20;
                            auto absDiffCounts = [](int32_t lhs, int32_t rhs) -> int64_t {
                                const int64_t delta = static_cast<int64_t>(lhs) - static_cast<int64_t>(rhs);
                                return (delta < 0) ? -delta : delta;
                            };

                            bool settled = false;
                            int32_t settledTarget = 0;
                            int32_t settledActual = 0;
                            for (int settleElapsed = 0;
                                 settleElapsed <= kSimulationHomeSettleTimeoutMs;
                                 settleElapsed += kSimulationHomeSettlePollMs) {
                                int targetSize = (int)sizeof(settledTarget);
                                int actualSize = (int)sizeof(settledActual);
                                const bool gotTarget = etherCATMaster->ReadSDO((uint16_t)slaveIdx,
                                                                                (uint16_t)0x607A,
                                                                                (uint8_t)0x00,
                                                                                (void*)&settledTarget,
                                                                                targetSize);
                                const bool gotActual = etherCATMaster->ReadSDO((uint16_t)slaveIdx,
                                                                                (uint16_t)0x6064,
                                                                                (uint8_t)0x00,
                                                                                (void*)&settledActual,
                                                                                actualSize);
                                if (gotTarget && gotActual) {
                                    const bool targetApplied =
                                        absDiffCounts(settledTarget, expectedHomePosition) <= kSimulationHomeSettleToleranceCounts;
                                    const bool actualApplied =
                                        absDiffCounts(settledActual, expectedHomePosition) <= kSimulationHomeSettleToleranceCounts;
                                    const bool settledDelta =
                                        absDiffCounts(settledTarget, settledActual) <= kSimulationHomeSettleToleranceCounts;
                                    if (targetApplied && actualApplied && settledDelta) {
                                        settled = true;
                                        break;
                                    }
                                }
                                if (settleElapsed < kSimulationHomeSettleTimeoutMs) {
                                    Sleep(kSimulationHomeSettlePollMs);
                                }
                            }

                            if (!settled) {
                                std::wstringstream ssSettle;
                                ssSettle << TimestampNowW()
                                         << L"[OriginSet] simulation target/actual settle timeout slave=" << slaveIdx
                                         << L" expected=" << expectedHomePosition
                                         << L" target=" << settledTarget
                                         << L" actual=" << settledActual << L"\n";
                                std::wstring full = stepLog.str() + ssSettle.str();
                                res.success = false;
                                res.message = full;
                                finished = true;
                                break;
                            }

                            std::wstringstream ssSettleOk;
                            ssSettleOk << TimestampNowW()
                                       << L"[OriginSet] simulation target/actual settled expected=" << expectedHomePosition
                                       << L" target=" << settledTarget
                                       << L" actual=" << settledActual << L"\n";
                            stepLog << ssSettleOk.str();
                        }

                        const uint8_t restoreMode = static_cast<uint8_t>(etherCATMaster->GetCyclicOperationMode());
                        if (!etherCATMaster->WriteModeOfOperation((uint16_t)slaveIdx, restoreMode)) {
                            std::wstringstream ssRestore;
                            ssRestore << TimestampNowW()
                                      << L"[OriginSet] failed to restore SDO 0x6060=" << (int)restoreMode
                                      << L" (" << CoreHelpers::DescribeOperationModeShortName(restoreMode) << L")\n";
                            std::wstring full = stepLog.str() + ssRestore.str();
                            res.success = false;
                            res.message = full;
                            finished = true;
                            break;
                        }
                        stepLog << TimestampNowW()
                                << L"[OriginSet] restored SDO 0x6060=" << (int)restoreMode
                                << L" (" << CoreHelpers::DescribeOperationModeShortName(restoreMode) << L")\n";

                        // No read-back verify: PDO cache updates every cycle; drive accepts
                        // mode changes within 1-2 PDO cycles. The operation sequence below
                        // (FaultReset + PowerOn) will catch any actual mode-rejection fault.

                        // CSP init after homing: write actual position to 0x607A
                        // so the drive does not jump when entering CSP mode.
                        if (restoreMode == CoreHelpers::kCia402OperationModeCyclicSyncPosition) {
                            stepLog << TimestampNowW()
                                    << L"[OriginSet] CSP restore: syncing 0x607A <- 0x6064 before SendProcessData\n";
                            // Send one roundtrip to get fresh actual position
                            if (etherCATMaster->IsRunning()) {
                                etherCATMaster->SendProcessDataOnce();
                            }
                            // Read actual position from 0x6064 via IOmap snapshot
                            int32_t actualPos = 0;
                            bool gotPos = false;
                            {
                                std::vector<uint8_t> snapshot;
                                if (etherCATMaster->GetPDODataSnapshot(slaveIdx, snapshot) && !snapshot.empty()) {
                                    int slaveObytes = 0;
                                    const auto* ctx = etherCATMaster->GetContext();
                                    if (ctx && slaveIdx <= ctx->slavecount) {
                                        slaveObytes = ctx->slavelist[slaveIdx].Obytes;
                                    } else {
                                        slaveObytes = EtherCATMaster::kPdoRxByteSize;
                                    }
                                    const int offset6064 = slaveObytes + EtherCATMaster::kPdoInOff_ActualPos;
                                    if (offset6064 + 4 <= static_cast<int>(snapshot.size())) {
                                        uint32_t raw = 0;
                                        for (int b = 0; b < 4; ++b) {
                                            raw |= static_cast<uint32_t>(snapshot[offset6064 + b]) << (8 * b);
                                        }
                                        actualPos = static_cast<int32_t>(raw);
                                        gotPos = true;
                                    }
                                }
                            }
                            if (gotPos) {
                                MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx,
                                                             0x607A, static_cast<uint32_t>(actualPos));
                                wchar_t bf[128];
                                swprintf_s(bf, L"[OriginSet] CSP: staged 0x607A=0x6064=%d\n",
                                           static_cast<int>(actualPos));
                                stepLog << TimestampNowW() << bf;
                            } else {
                                stepLog << TimestampNowW()
                                        << L"[OriginSet] CSP: could not read 0x6064, skip sync\n";
                            }
                        }

                        if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, slaveIdx)) {
                            std::wstring full = stepLog.str() +
                                TimestampNowW() + std::wstring(L"[OriginSet] failed to stage PDO 0x60FF=0 after mode restore\n");
                            res.success = false;
                            res.message = full;
                            finished = true;
                            break;
                        }
                        stepLog << TimestampNowW() << L"[OriginSet] staged PDO 0x60FF=0 after mode restore\n";

                        if (etherCATMaster->IsRunning()) {
                            etherCATMaster->SendProcessDataOnce();
                        }

                        std::wstringstream ss2;
                        ss2 << TimestampNowW() << L"[OriginSet] completed slave=" << slaveIdx << L" status=0x" << std::hex << std::uppercase << lastStatus << std::dec << L"\n";
                        // Prepend step summary so UI sees the performed SDO/PDO actions
                        std::wstring full = stepLog.str() + ss2.str();
                        res.success = true; res.message = full; finished = true; break;
                    }
            }
        } else {
            AppendFileMC("HomeAsync: ReadPDOItemValueFromSnapshot failed during poll\n");
        }
        Sleep(pollIntervalMs); elapsed += pollIntervalMs;
    }

    // On finish/error/timeout: best-effort stage controlword = 0x0F so UI can clear BUSY
    // Per contract: UI owns BUSY; controller will only perform SDO/PDO operations
    AppendFileMC("HomeAsync: attempting final StageControlword -> 0x0F (best-effort)\n");
    int32_t finalSt = 0x0F;
    if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cwItem, finalSt)) {
        AppendFileMC("HomeAsync: final StageControlword 0x0F OK\n");
    stepLog << TimestampNowW() << L"[OriginSet] final staged controlword 0x6040=0x0F\n";
    } else {
        AppendFileMC("HomeAsync: final StageControlword 0x0F FAILED\n");
    }

    // Home success restores the default cyclic mode before returning so later
    // CSV/CSP-neutral motion paths do not inherit Homing mode 6.

    if (!finished) {
        std::wstringstream ss; ss << TimestampNowW() << L"[OriginSet] TIMEOUT after 3s waiting for completion slave=" << slaveIdx << L" status=0x" << std::hex << lastStatus << std::dec << L"\n";
        // include performed steps so UI can inspect actions taken before timeout
        std::wstring full = stepLog.str() + ss.str();
        res.success = false; res.message = full;
    }

    return res;
}

MotionResult MotionController::JogPositiveStart(int slaveIdx, uint32_t stagedVal) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { 
        res.message = TimestampNowW() + std::wstring(L"[JogStart] invalid args or master missing"); 
        return res; 
    }
    
    // 1) Zero the velocity command PDO (0x60FF) and flush to hardware BEFORE
    //    switching the drive to CSV mode.  This prevents an initial velocity
    //    burst from any stale non-zero value that may be latched in IOmap.
    MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, 0u);
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // 2) Write 0x6060 = CSV (9) (PDO if extended, SDO if basic)
    const uint8_t jogMode = CoreHelpers::kCia402OperationModeCyclicSyncVelocity;
    if (!etherCATMaster->WriteModeOfOperation((uint16_t)slaveIdx, jogMode)) {
        std::wstringstream ss;
          ss << TimestampNowW() << L"[JogStart] failed to write 0x6060="
              << (int)jogMode << L" ("
              << CoreHelpers::DescribeOperationModeShortName(jogMode)
              << L")";
        res.message = ss.str();
        return res;
    }

    // 3) After mode switch, let the drive settle into CSV mode.
    //    Send several PDO frames with 0x60FF=0 to ensure the drive
    //    sees zero velocity when it begins tracking the velocity PDO.
    for (int flushCycle = 0; flushCycle < 5; ++flushCycle) {
        MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, 0u);
        if (etherCATMaster->IsRunning()) {
            etherCATMaster->SendProcessDataOnce();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // 4) 安全停止並重啟執行緒
    std::shared_ptr<JogState> state;
    std::thread oldThread; // 暫存舊執行緒以便在 mutex 外 join
    
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        auto it = jogStates_.find(slaveIdx);
        if (it == jogStates_.end()) {
            state = std::make_shared<JogState>();
            jogStates_[slaveIdx] = state;
        } else {
            state = it->second;
        }

        // 無論執行緒是否活躍，都要先清理舊執行緒
        // （執行緒完成後仍是 joinable 但 active=false，必須 join 才能安全重用）
        if (state->active.load()) {
            state->stop.store(true);
        }
        
        // 總是檢查並移出舊執行緒，即使它已經完成
        if (state->thr.joinable()) {
            oldThread = std::move(state->thr);
        }
    }
    
    // 在 mutex 外等待舊執行緒結束（避免阻塞其他軸的操作）
    if (oldThread.joinable()) {
        // 簡單的 join（線程應該會快速完成或檢測到停止標記）
        // 如果線程卡住，這可能會導致應用掛起，但至少不會崩潰
        oldThread.join();
    }
    
    // 重新獲取鎖並啟動新執行緒
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        
        // 重置狀態並啟動新執行緒
        state->stop.store(false);
        state->active.store(true);
        state->targetVel = stagedVal;
        
        // 創建新執行緒（保持 joinable 以便解構函數能正確清理）
        state->thr = std::thread([this, slaveIdx, state]() {
            RunJogSoftStartWorker(state, slaveIdx);
        });
        
#if MOTIONCTRL_DEBUG_LOG
        {
            std::string dbg = std::string("[JogStart] started soft-start thread slave=") + std::to_string(slaveIdx);
            OutputDebugStringA((dbg + "\n").c_str());
            printf("%s\n", dbg.c_str());
        }
#endif
    }
    
    res.success = true;
    {
        std::wstringstream ss;
        ss << TimestampNowW()
           << L"[JogStart] soft start initiated (SDO 6060=" << (int)jogMode
              << L" " << CoreHelpers::DescribeOperationModeShortName(jogMode)
           << L", target 60FF=" << (int32_t)stagedVal << L")";
        res.message = ss.str();
    }
    return res;
}

MotionResult MotionController::JogStop(int slaveIdx) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { 
        res.message = TimestampNowW() + std::wstring(L"[JogStop] invalid args or master missing"); 
        return res; 
    }
    if (!pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[JogStop] PDO display manager missing");
        return res;
    }
    
    // 停止緩啟動執行緒（如果正在執行）
    std::shared_ptr<JogState> state;
    std::thread oldThread; // 暫存執行緒以便在 mutex 外 join
    
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        auto it = jogStates_.find(slaveIdx);
        if (it != jogStates_.end()) state = it->second;
        
        if (state) {
            // 如果執行緒還在運行，設置停止標記
            if (state->active.load()) {
                state->stop.store(true);
            }
            // 總是清理舊執行緒（即使已完成）
            if (state->thr.joinable()) {
                oldThread = std::move(state->thr);
            }
        }
    }
    
    // 在 mutex 外 join 執行緒
    // 執行緒應該會快速檢測到 stop 標記並退出
    if (oldThread.joinable()) {
        // 給執行緒一些時間來反應 stop 標記
        // 由於執行線程會在每個 Sleep(20ms) 後檢查 stop 標記，應該在 100ms 內完成
        oldThread.join();
#if MOTIONCTRL_DEBUG_LOG
        {
            std::string dbg = std::string("[JogStop] joined soft-start thread slave=") + std::to_string(slaveIdx);
            OutputDebugStringA((dbg + "\n").c_str());
            printf("%s\n", dbg.c_str());
        }
#endif
    }
    
    // Stage PDO 0x60FF = 0 to stop command
    if (etherCATMaster && !MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, 0u)) {
        res.message = TimestampNowW() + std::wstring(L"[JogStop] failed to stage PDO 0x60FF=0"); 
        return res;
    }
    
    res.success = true; 
    res.message = TimestampNowW() + std::wstring(L"[JogStop] staged 60FF=0");
    return res;
}

MotionResult MotionController::JogNegativeStart(int slaveIdx, uint32_t stagedVal) {
    MotionResult res{false, L""};
    if (slaveIdx <= 0 || !etherCATMaster) { 
        res.message = TimestampNowW() + std::wstring(L"[JogNegStart] invalid args or master missing"); 
        return res; 
    }
    
    // 1) Zero the velocity command PDO (0x60FF) and flush to hardware BEFORE
    //    switching the drive to CSV mode.  This prevents an initial velocity
    //    burst from any stale non-zero value that may be latched in IOmap.
    MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, 0u);
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // 2) Write 0x6060 = CSV (9) — jog is velocity-driven (PDO if extended, SDO if basic)
    const uint8_t jogMode = CoreHelpers::kCia402OperationModeCyclicSyncVelocity;
    if (!etherCATMaster->WriteModeOfOperation((uint16_t)slaveIdx, jogMode)) {
        std::wstringstream ss;
          ss << TimestampNowW() << L"[JogNegStart] failed to write 0x6060="
              << (int)jogMode << L" ("
              << CoreHelpers::DescribeOperationModeShortName(jogMode)
              << L")";
        res.message = ss.str();
        return res;
    }

    // 3) After mode switch, let the drive settle into CSV mode.
    //    Send several PDO frames with 0x60FF=0 to ensure the drive
    //    sees zero velocity when it begins tracking the velocity PDO.
    for (int flushCycle = 0; flushCycle < 5; ++flushCycle) {
        MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, 0u);
        if (etherCATMaster->IsRunning()) {
            etherCATMaster->SendProcessDataOnce();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    // 4) 安全停止並重啟執行緒
    std::shared_ptr<JogState> state;
    std::thread oldThread; // 暫存舊執行緒以便在 mutex 外 join
    
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        auto it = jogStates_.find(slaveIdx);
        if (it == jogStates_.end()) {
            state = std::make_shared<JogState>();
            jogStates_[slaveIdx] = state;
        } else {
            state = it->second;
        }

        // 無論執行緒是否活躍，都要先清理舊執行緒
        // （執行緒完成後仍是 joinable 但 active=false，必須 join 才能安全重用）
        if (state->active.load()) {
            state->stop.store(true);
        }
        
        // 總是檢查並移出舊執行緒，即使它已經完成
        if (state->thr.joinable()) {
            oldThread = std::move(state->thr);
        }
    }
    
    // 在 mutex 外等待舊執行緒結束
    if (oldThread.joinable()) {
        oldThread.join();
    }
    
    // 重新獲取鎖並啟動新執行緒
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        
        // 重置狀態並啟動新執行緒
        state->stop.store(false);
        state->active.store(true);
        state->targetVel = stagedVal;
        
        // 創建新執行緒（保持 joinable）
        state->thr = std::thread([this, slaveIdx, state]() {
            RunJogSoftStartWorker(state, slaveIdx);
        });
        
#if MOTIONCTRL_DEBUG_LOG
        {
            std::string dbg = std::string("[JogNegStart] started soft-start thread slave=") + std::to_string(slaveIdx);
            OutputDebugStringA((dbg + "\n").c_str());
            printf("%s\n", dbg.c_str());
        }
#endif
    }
    
    res.success = true;
    {
        std::wstringstream ss;
        ss << TimestampNowW()
           << L"[JogNegStart] soft start initiated (SDO 6060=" << (int)jogMode
              << L" " << CoreHelpers::DescribeOperationModeShortName(jogMode)
           << L", target 60FF=" << (int32_t)stagedVal << L")";
        res.message = ss.str();
    }
    return res;
}

MotionResult MotionController::JogNegativeStartBatch(const std::vector<int>& slaves, const std::vector<uint32_t>& velocities) {
    MotionResult res{false, L""};
    if (slaves.empty() || slaves.size() != velocities.size()) {
        res.message = TimestampNowW() + L"[JogBatch] slave/velocity count mismatch";
        return res;
    }
    if (!etherCATMaster || !pdoDisplay) {
        res.message = TimestampNowW() + L"[JogBatch] master or PDO display missing";
        return res;
    }

    const uint8_t jogMode = CoreHelpers::kCia402OperationModeCyclicSyncVelocity;
    const size_t count = slaves.size();

    // === Phase 1: Zero 0x60FF + switch to CSV mode for ALL axes ===
    // Stage all outputs before any PDO exchange — all drives see the change
    // in the same cycle.
    for (size_t i = 0; i < count; ++i) {
        int s = slaves[i];
        if (s <= 0) continue;
        MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, s, 0x60FF, 0u);
        if (!etherCATMaster->WriteModeOfOperation((uint16_t)s, jogMode)) {
            std::wstringstream ss;
            ss << TimestampNowW() << L"[JogBatch] failed to write 0x6060=" << (int)jogMode << L" for slave " << s;
            res.message = ss.str();
            return res;
        }
    }
    // Single PDO exchange — all axes get 0x60FF=0 and mode=9 simultaneously
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // === Phase 2: Wait for all axes to confirm CSV mode (0x6061 == 9) ===
    const int modeSwitchTimeoutMs = 200;
    const int pollIntervalMs = 4;
    int waited = 0;
    std::vector<bool> modeOk(count, false);
    size_t modeOkCount = 0;
    while (waited < modeSwitchTimeoutMs && modeOkCount < count) {
        // Stage 0x60FF=0 again while waiting to keep drives seeing zero
        for (size_t i = 0; i < count; ++i) {
            if (!modeOk[i]) {
                MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaves[i], 0x60FF, 0u);
            }
        }
        if (etherCATMaster->IsRunning()) {
            etherCATMaster->SendProcessDataOnce();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
        waited += pollIntervalMs;

        for (size_t i = 0; i < count; ++i) {
            if (modeOk[i]) continue;
            uint8_t mode = 0;
            if (etherCATMaster->GetPdo6061((uint16_t)slaves[i], mode) && mode == jogMode) {
                modeOk[i] = true;
                ++modeOkCount;
            }
        }
    }
    if (modeOkCount < count) {
        std::wstringstream ss;
        ss << TimestampNowW() << L"[JogBatch] mode switch timeout: " << modeOkCount << L"/" << count << L" axes in CSV mode";
        res.message = ss.str();
        return res;
    }

    // === Phase 3: Stage target velocity for ALL axes simultaneously ===
    for (size_t i = 0; i < count; ++i) {
        MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaves[i], 0x60FF, velocities[i]);
    }
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // === Phase 4: Start soft-start threads for all axes ===
    // Reuse existing JogNegativeStart thread-management logic (Phase 4 only)
    for (size_t i = 0; i < count; ++i) {
        int s = slaves[i];
        std::shared_ptr<JogState> state;
        std::thread oldThread;
        {
            std::lock_guard<std::mutex> lk(jogStatesMutex_);
            auto it = jogStates_.find(s);
            if (it == jogStates_.end()) {
                state = std::make_shared<JogState>();
                jogStates_[s] = state;
            } else {
                state = it->second;
            }
            if (state->active.load()) state->stop.store(true);
            if (state->thr.joinable()) oldThread = std::move(state->thr);
        }
        if (oldThread.joinable()) oldThread.join();

        {
            std::lock_guard<std::mutex> lk(jogStatesMutex_);
            state->stop.store(false);
            state->active.store(true);
            state->targetVel = velocities[i];
            state->thr = std::thread([this, s, state]() {
                RunJogSoftStartWorker(state, s);
            });
        }
    }

    res.success = true;
    {
        std::wstringstream ss;
        ss << TimestampNowW() << L"[JogBatch] " << count << L" axes started in CSV mode";
        res.message = ss.str();
    }
    return res;
}

// RunJogSoftStart just locates the per-slave JogState and delegates to the worker
void MotionController::RunJogSoftStart(int slaveIdx, uint32_t targetVel) {
    std::shared_ptr<JogState> state;
    {
        std::lock_guard<std::mutex> lk(jogStatesMutex_);
        auto it = jogStates_.find(slaveIdx);
        if (it != jogStates_.end()) state = it->second;
    }
    if (!state) return;

    // ensure the state's targetVel matches passed value (caller may pass it)
    state->targetVel = targetVel;
    RunJogSoftStartWorker(state, slaveIdx);
}

// Actual worker uses shared state to consult active/stop flags and update lifecycle safely
void MotionController::RunJogSoftStartWorker(std::shared_ptr<JogState> state, int slaveIdx) {
    if (!state) return;

    const double startTime = GetTickCount64() / 1000.0; // seconds
    const double duration = jogSoftStartTime_;
    const uint32_t updateIntervalMs = 20; // 20ms

    // compute sign/direction
    bool isNegative = (int32_t)state->targetVel < 0;
    uint32_t absTargetVel = isNegative ? (uint32_t)(-(int32_t)state->targetVel) : state->targetVel;

    while (!state->stop.load()) {
        // 在每次迭代檢查關鍵指針，以便在銷毀期間安全退出
        // 如果 etherCATMaster 或 pdoDisplay 已被設置為 nullptr（在析構函數中），立即退出
        if (!etherCATMaster || !pdoDisplay) {
#if MOTIONCTRL_DEBUG_LOG
            {
                std::string dbg = std::string("[JogSoftStartWorker] master or PDO became null, exiting slave=") + std::to_string(slaveIdx);
                OutputDebugStringA((dbg + "\n").c_str());
                printf("%s\n", dbg.c_str());
            }
#endif
            state->active.store(false);
            break;
        }

        double elapsed = (GetTickCount64() / 1000.0) - startTime;
        if (elapsed >= duration) {
            // final
            if (MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, state->targetVel)) {
                state->active.store(false);
            }
            break;
        }

        double ratio = elapsed / duration;
        if (ratio > 1.0) ratio = 1.0;
        if (ratio < 0.0) ratio = 0.0;

        uint32_t currentVel = static_cast<uint32_t>(absTargetVel * ratio);
        if (isNegative) currentVel = (uint32_t)(-(int32_t)currentVel);

        if (!MotionLogic::StagePDOByIndex(etherCATMaster, pdoDisplay, slaveIdx, 0x60FF, currentVel)) {
            // if stage fails, stop worker
            state->active.store(false);
            break;
        }

        Sleep(updateIntervalMs);
    }

    state->active.store(false);
}
