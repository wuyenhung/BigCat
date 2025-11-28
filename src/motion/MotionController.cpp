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
    return MotionLogic::StagePDOByIndex(
        master,
        pdo,
        slaveIdx,
        CoreHelpers::kDefaultCyclicCommandPdoIndex,
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
            // Before asserting fault-reset (0x0080) require SDO 0x603F != 0
            uint32_t ec = 0; int readSz = (int)sizeof(ec);
            if (!etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x603F, (uint8_t)0x00, (void*)&ec, readSz)) {
                res.message = TimestampNowW() + std::wstring(L"[FaultReset] failed to read SDO 0x603F");
            } else if (ec == 0) {
                std::wstringstream ss; ss << TimestampNowW() << L"[FaultReset] SDO 0x603F == 0; no error present; assert skipped slave=" << slaveIdx << L"\n";
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
    if (!pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] PDO display manager missing");
        return res;
    }
    if (!assert) { res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] release ignored"); return res; }

    const uint32_t QS_MASK = 0x0004;
    bool staged = false;
    const PDOItem* cw = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (cw) {
        // Step A: Read current controlword snapshot and stage QS (quick stop)
        int32_t curv = 0;
        if (MotionLogic::ReadControlwordFromSnapshot(etherCATMaster, cw, slaveIdx, curv)) {
            int32_t newv = curv | QS_MASK;
            if (MotionLogic::StageControlword(etherCATMaster, pdoDisplay, slaveIdx, cw, newv)) {
                if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, slaveIdx)) {
                    res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] failed to stage PDO 0x60FF=0 after QS staging");
                    return res;
                }

                // Optionally, ensure processdata is sent so staging takes effect immediately
                if (etherCATMaster->IsRunning()) {
                    etherCATMaster->SendProcessDataOnce();
                }

                std::wstringstream ss; ss << TimestampNowW() << L"[ImmediateStop] staged 0x60FF=0 and PDO staged QS=1 slave=" << slaveIdx << L" (0x6040=0x" << std::hex << std::uppercase << newv << std::dec << L")\n";
                res.success = true; res.message = ss.str(); staged = true;
            } else res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] PDO staging of QS failed");
        } else res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] snapshot parse failed while reading controlword for QS");
    } else res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] controlword item not found");
    if (!staged) { if (res.message.empty()) res.message = TimestampNowW() + std::wstring(L"[ImmediateStop] PDO staging failed; no SDO fallback"); }
    return res;
}

MotionResult MotionController::ImmediateStopAll() {
    MotionResult res{false, L""};
    if (!etherCATMaster) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStopAll] EtherCAT master missing\n");
        return res;
    }
    if (!pdoDisplay) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStopAll] PDO display manager missing\n");
        return res;
    }
    int slaveCount = etherCATMaster->GetSlaveCount();
    if (slaveCount <= 0) {
        res.message = TimestampNowW() + std::wstring(L"[ImmediateStopAll] no slaves present\n");
        return res;
    }

    std::wstringstream allMsg;
    allMsg << TimestampNowW() << L"[ImmediateStopAll] invoking immediate stop for " << slaveCount << L" slaves\n";
    bool anySuccess = false;
    const PDOItem* cwItem = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (!cwItem) {
        allMsg << TimestampNowW() << L"[ImmediateStopAll] controlword item missing\n";
        res.message = allMsg.str();
        return res;
    }
    const uint32_t QS_MASK = 0x0004;
    for (int s = 1; s <= slaveCount; ++s) {
        int32_t cur = 0;
        if (!MotionLogic::ReadControlwordFromSnapshot(etherCATMaster, cwItem, s, cur)) {
            allMsg << TimestampNowW() << L"[ImmediateStopAll] failed to read snapshot slave=" << s << L"\n";
            continue;
        }
        int32_t newVal = static_cast<int32_t>(static_cast<uint32_t>(cur) | QS_MASK);
        if (MotionLogic::StageControlwordWithoutSend(etherCATMaster, pdoDisplay, s, cwItem, newVal)) {
            std::wstringstream ss;
            ss << TimestampNowW() << L"[ImmediateStopAll] staged QS=1 slave=" << s << L" (0x6040=0x" << std::hex << std::uppercase << newVal << std::dec << L")\n";
            allMsg << ss.str();
            anySuccess = true;

            if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, s)) {
                allMsg << TimestampNowW() << L"[ImmediateStopAll] failed to stage PDO 0x60FF=0 for slave=" << s << L"\n";
            }
        } else {
            allMsg << TimestampNowW() << L"[ImmediateStopAll] failed to stage controlword slave=" << s << L"\n";
        }
    }

    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        allMsg << TimestampNowW() << L"[ImmediateStopAll] process data sent\n";
    }
    res.success = anySuccess;
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
    } catch (...) {
        allMsg << TimestampNowW() << L"[SmoothStopAll] warning: failed to set hold-position mode\n";
    }

    for (int s = 1; s <= slaveCount; ++s) {
        // NOTE: HoldAllPDOFiltersPosition() already:
        // 1. Restored normal deceleration limits (cleared soft-start)
        // 2. Calculated stopping distance using those limits
        // 3. Set the target position to where the motor will naturally stop
        //
        // We do NOT modify the deceleration here anymore because the stopping
        // distance calculation in HoldCurrentPosition() depends on the current
        // deceleration limit. Changing it here would cause a mismatch.

        if (StageZeroVelocityCommand(etherCATMaster, pdoDisplay, s)) {
            std::wstringstream ss; ss << TimestampNowW() << L"[SmoothStopAll] staged 0x60FF=0 slave=" << s << L"\n";
            allMsg << ss.str();
            anySuccess = true;
        } else {
            allMsg << TimestampNowW() << L"[SmoothStopAll] failed to stage 0x60FF=0 for slave=" << s << L"\n";
        }
    }

    // Send one processdata to make staged targets effective immediately
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
        allMsg << TimestampNowW() << L"[SmoothStopAll] process data sent\n";
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

    // 1) Stage cyclic command PDO 0x60FF = 0 before enabling operation.
    if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, slaveIdx)) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOn] failed to stage PDO 0x60FF=0"); return res; }
    {
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOn] staged PDO 0x60FF=0");
        LogPowerOn(std::string(bf));
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

    // 3) Write SDO 0x6060 = default cyclic mode first.
    uint8_t mode = CoreHelpers::kDefaultCyclicOperationMode;
    if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)&mode, 1)) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOn] failed to write/verify SDO 0x6060 default cyclic mode"); return res;
    }
    {
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOn] wrote SDO 0x6060=%d (%s)",
                                (int)mode,
                                CoreHelpers::DescribeOperationModeShortName(mode));
        LogPowerOn(std::string(bf));
    }

    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    std::wstringstream ss; ss << TimestampNowW() << L"[PowerOn] final: staged 0x60FF=0 and 0x6040=0x0F in "
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

    // PHASE 2: Stage the cyclic command PDO to zero velocity for all axes.
    LogAll("[PowerOnAll] Phase 2: Staging 0x60FF=0 for all axes");
    for (int si = 1; si <= slaveCount; ++si) {
        if (!StageZeroVelocityCommand(etherCATMaster, pdoDisplay, si)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] Phase 2: failed to stage 0x60FF=0 for slave %d", si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to stage PDO 0x60FF=0");
            return res;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: staged 0x60FF=0", si);
        LogAll(bf);
    }
    
    // Send the staged zero velocity commands before the mode change so the drives
    // see a benign command when CSV is acknowledged.
    LogAll("[PowerOnAll] Phase 2.5: Sending staged zero-velocity commands to IOmap");
    if (etherCATMaster->IsRunning()) {
        etherCATMaster->SendProcessDataOnce();
    }

    // PHASE 3: Write SDO 0x6060=default cyclic mode for ALL axes.
    LogAll("[PowerOnAll] Phase 3: Writing default cyclic mode for all axes");
    uint8_t mode = CoreHelpers::kDefaultCyclicOperationMode;
    
    for (int si = 1; si <= slaveCount; ++si) {
        if (!etherCATMaster->WriteSDO((uint16_t)si, (uint16_t)0x6060, (uint8_t)0x00, (void*)&mode, 1)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] failed to write SDO 0x6060=%d for slave %d", (int)mode, si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to write SDO 0x6060 default cyclic mode");
            return res;
        }

        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: wrote SDO 0x6060=%d (%s)",
                                si,
                                (int)mode,
                                CoreHelpers::DescribeOperationModeShortName(mode));
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

    // PHASE 6: Stage controlword 0x6040=0x0F for ALL axes
    // CRITICAL: This must be done AFTER final zero-velocity staging and RIGHT BEFORE send!
    // Do NOT stage this earlier - CyclicTask will send it immediately!
    LogAll("[PowerOnAll] Phase 6: Staging controlword (0x6040=0x0F) for all axes");
    const PDOItem* cwItem = MotionLogic::FindControlwordItem(pdoDisplay->pdoItems);
    if (!cwItem) {
        res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] controlword PDO (0x6040) not found");
        return res;
    }

    for (int si = 1; si <= slaveCount; ++si) {
        if (!MotionLogic::StageControlwordWithoutSend(etherCATMaster, pdoDisplay, si, cwItem, 0x000F)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] failed to stage 0x6040 for slave %d", si);
            LogAll(bf);
            res.message = TimestampNowW() + std::wstring(L"[PowerOnAll] failed to stage controlword 0x6040=0x0F");
            return res;
        }
        char bf[128]; sprintf_s(bf, sizeof(bf), "[PowerOnAll] slave %d: staged 0x6040=0x000F", si);
        LogAll(bf);
    }

    // PHASE 7: IMMEDIATELY send process data - no delay!
    // Both 0x60FF and 0x6040 are staged, send them together atomically.
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

MotionResult MotionController::HomeAsync(int slaveIdx, bool assert, bool haveP13, uint32_t p13) {
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
    AppendFileMC("HomeAsync: mode display (0x6061) will be read via SDO\n");
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
    // Always use SDO to obtain mode display (0x6061)
    uint32_t tmp = 0; int readSz = (int)sizeof(tmp);
    if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6061, (uint8_t)0x00, (void*)&tmp, readSz)) {
        AppendFileMC("HomeAsync: SDO read 0x6061 succeeded\n");
    } else {
        AppendFileMC("HomeAsync: SDO read 0x6061 failed\n");
        res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to read SDO 0x6061");
        return res;
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

    // 1) write 0x6060 = 6
    AppendFileMC("HomeAsync: before SDO 0x6060 write\n");
    uint8_t buf6060_mode6[1] = { (uint8_t)6 };
    if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)buf6060_mode6, 1)) {
        AppendFileMC("HomeAsync: SDO 0x6060 write FAILED\n");
        res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to verify SDO 0x6060=6"); return res;
    }
    AppendFileMC("HomeAsync: after SDO 0x6060 write (OK)\n");
    stepLog << TimestampNowW() << L"[OriginSet] wrote SDO 0x6060=6\n";

    // Debug: immediately read back 0x6060 and 0x6061 to capture device response
    {
        AppendFileMC("HomeAsync: debug: readback 0x6060/0x6061\n");
        // read 0x6060 as 8-bit
        int readSz6060 = 1; uint8_t rb6060 = 0;
        if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)&rb6060, readSz6060)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "HomeAsync: debug: 0x6060 readSz=%d val=0x%02X\n", readSz6060, rb6060); AppendFileMC(bf);
        } else {
            AppendFileMC("HomeAsync: debug: 0x6060 read FAILED\n");
        }

        // read 0x6061 as up to 32-bit to capture display mode representation
        int readSz6061 = (int)sizeof(uint32_t); uint32_t rb6061 = 0;
        if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6061, (uint8_t)0x00, (void*)&rb6061, readSz6061)) {
            char bf2[128]; sprintf_s(bf2, sizeof(bf2), "HomeAsync: debug: 0x6061 readSz=%d val=0x%08X (dec=%d)\n", readSz6061, rb6061, (int32_t)rb6061); AppendFileMC(bf2);
        } else {
            AppendFileMC("HomeAsync: debug: 0x6061 read FAILED\n");
        }
    }

    // 2) write 0x6098 = 35 (32-bit)
    {
        AppendFileMC("HomeAsync: before SDO 0x6098 write (35)\n");
        // Probe device for the current SDO size (ReadSDO will update probeSz)
        int probeSz = 4;
        uint8_t probeBuf[4] = {0};
            if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6098, (uint8_t)0x00, (void*)probeBuf, probeSz)) {
            char bf[128]; sprintf_s(bf, sizeof(bf), "HomeAsync: SDO 0x6098 probe readSz=%d bytes=", probeSz); AppendFileMC(bf);
            for (int i = 0; i < probeSz; ++i) { char b2[8]; sprintf_s(b2, sizeof(b2), "%02X ", probeBuf[i]); AppendFileMC(b2); }
            AppendFileMC("\n");

            bool wrote = false;
                if (probeSz == 1) {
                uint8_t v8 = (uint8_t)35;
                if (etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v8, 1)) wrote = true;
            } else if (probeSz == 2) {
                uint16_t v16 = (uint16_t)35;
                if (etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v16, 2)) wrote = true;
            } else {
                uint32_t v32 = (uint32_t)35;
                if (etherCATMaster->WriteSDO((uint16_t)slaveIdx, 0x6098, 0x00, &v32, 4)) wrote = true;
            }

            if (!wrote) {
                AppendFileMC("HomeAsync: SDO 0x6098 write FAILED\n");
                res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to verify SDO 0x6098=35"); return res;
            }
            AppendFileMC("HomeAsync: after SDO 0x6098 write (OK)\n");
            stepLog << TimestampNowW() << L"[OriginSet] wrote SDO 0x6098=35\n";
        } else {
            AppendFileMC("HomeAsync: SDO 0x6098 probe failed - attempting 32-bit write\n");
            uint32_t v6098 = 35;
            if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6098, (uint8_t)0x00, (void*)&v6098, (int)sizeof(v6098))) {
                AppendFileMC("HomeAsync: SDO 0x6098 write FAILED\n");
                res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to verify SDO 0x6098=35"); return res;
            }
            AppendFileMC("HomeAsync: after SDO 0x6098 write (OK)\n");
        }
    }

    // 3) write 0x607C: use provided P13 if available, otherwise write 0
    {
        uint32_t v607c = 0;
        if (haveP13) v607c = p13;
        AppendFileMC("HomeAsync: before SDO 0x607C write\n");
        {
            char bf[128]; sprintf_s(bf, sizeof(bf), "HomeAsync: SDO 0x607C will be written with value=0x%08X\n", v607c); AppendFileMC(bf);
        }
        if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x607C, (uint8_t)0x00, (void*)&v607c, (int)sizeof(v607c))) {
            AppendFileMC("HomeAsync: SDO 0x607C write FAILED\n");
            res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to verify SDO 0x607C"); return res;
        }
        AppendFileMC("HomeAsync: after SDO 0x607C write (OK)\n");
        {
            std::wstringstream ssx; ssx << TimestampNowW() << L"[OriginSet] wrote SDO 0x607C=0x" << std::hex << std::uppercase << v607c << std::dec << L"\n";
            stepLog << ssx.str();
        }
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

    // Verify mode display (0x6061) == 6 using PDO snapshot or SDO fallback
    std::vector<uint8_t> snap2;
    if (!etherCATMaster->GetPDODataSnapshot(slaveIdx, snap2)) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] failed to get PDO snapshot after staging 0x0F"); return res; }
    AppendFileMC("HomeAsync: after GetPDODataSnapshot after 0x0F\n");
    // no persistent verifyMode required; we only need to confirm it becomes 6
    // verify mode display == 6, but allow up to 500ms for device to update before failing
    const int modeVerifyTimeoutMs = 500;
    const int modePollIntervalMs = 10;
    int modeElapsed = 0;
    bool modeOk = false;
    // Verify via SDO repeatedly until mode display == 6 or timeout
    while (modeElapsed <= modeVerifyTimeoutMs) {
        uint32_t v = 0; int sz = (int)sizeof(v);
        if (etherCATMaster->ReadSDO((uint16_t)slaveIdx, (uint16_t)0x6061, (uint8_t)0x00, (void*)&v, sz)) {
            char bf2[128]; sprintf_s(bf2, sizeof(bf2), "HomeAsync: SDO verifyMode poll elapsed=%d value=%u\n", modeElapsed, v); AppendFileMC(bf2);
            if ((int32_t)v == 6) { modeOk = true; break; }
        } else {
            AppendFileMC("HomeAsync: ReadSDO 0x6061 failed while verifying mode (retry)\n");
        }
        Sleep(modePollIntervalMs);
        modeElapsed += modePollIntervalMs;
    }
    if (!modeOk) { res.message = TimestampNowW() + std::wstring(L"[OriginSet] SDO verify failed: mode display (0x6061) != 6 after 500ms"); return res; }

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

                        const uint8_t restoreMode = CoreHelpers::kDefaultCyclicOperationMode;
                        if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx,
                                                      (uint16_t)0x6060,
                                                      (uint8_t)0x00,
                                                      (void*)&restoreMode,
                                                      1)) {
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

                        bool restoreModeOk = false;
                        for (int restoreElapsedMs = 0; restoreElapsedMs <= modeVerifyTimeoutMs; restoreElapsedMs += modePollIntervalMs) {
                            uint32_t restoreDisplay = 0;
                            int restoreSize = (int)sizeof(restoreDisplay);
                            if (etherCATMaster->ReadSDO((uint16_t)slaveIdx,
                                                        (uint16_t)0x6061,
                                                        (uint8_t)0x00,
                                                        (void*)&restoreDisplay,
                                                        restoreSize) &&
                                (int8_t)(restoreDisplay & 0xFF) == (int8_t)restoreMode) {
                                restoreModeOk = true;
                                break;
                            }
                            if (restoreElapsedMs < modeVerifyTimeoutMs) {
                                Sleep(modePollIntervalMs);
                            }
                        }
                        if (!restoreModeOk) {
                            std::wstringstream ssRestore;
                            ssRestore << TimestampNowW()
                                      << L"[OriginSet] SDO verify failed: mode display (0x6061) != "
                                      << (int)restoreMode << L" after homing restore\n";
                            std::wstring full = stepLog.str() + ssRestore.str();
                            res.success = false;
                            res.message = full;
                            finished = true;
                            break;
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
    
    // 1) Write SDO 0x6060 = default cyclic mode so jog follows the same
    // motion contract as the main CSV path.
    // This is done OUTSIDE the mutex to allow multiple axes to configure simultaneously
    uint8_t mode = CoreHelpers::kDefaultCyclicOperationMode;
    if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)&mode, 1)) {
        std::wstringstream ss;
          ss << TimestampNowW() << L"[JogStart] failed to write SDO 0x6060="
              << (int)mode << L" ("
              << CoreHelpers::DescribeOperationModeShortName(mode)
              << L")";
        res.message = ss.str();
        return res;
    }
    
    // 2) 安全停止並重啟執行緒
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
           << L"[JogStart] soft start initiated (SDO 6060=" << (int)mode
              << L" " << CoreHelpers::DescribeOperationModeShortName(mode)
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
    
    // 1) Write SDO 0x6060 = default cyclic mode so jog follows the same
    // motion contract as the main CSV path.
    // This is done OUTSIDE the mutex to allow multiple axes to configure simultaneously
    uint8_t mode = CoreHelpers::kDefaultCyclicOperationMode;
    if (!etherCATMaster->WriteSDO((uint16_t)slaveIdx, (uint16_t)0x6060, (uint8_t)0x00, (void*)&mode, 1)) {
        std::wstringstream ss;
          ss << TimestampNowW() << L"[JogNegStart] failed to write SDO 0x6060="
              << (int)mode << L" ("
              << CoreHelpers::DescribeOperationModeShortName(mode)
              << L")";
        res.message = ss.str();
        return res;
    }
    
    // 2) 安全停止並重啟執行緒
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
           << L"[JogNegStart] soft start initiated (SDO 6060=" << (int)mode
              << L" " << CoreHelpers::DescribeOperationModeShortName(mode)
           << L", target 60FF=" << (int32_t)stagedVal << L")";
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
