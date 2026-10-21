#include "EtherCATMaster.h"
#include <sstream>
#include <iomanip>
#include <chrono>
#include <soem/ec_print.h>

// SOEM internal functions with external linkage but not declared in headers.
// These are safe to declare here since they are non-static in ec_main.c.
extern "C" {
    boolean ecx_poperror(ecx_contextt *context, ec_errort *Ec);
    int     ecx_mbxinhandler(ecx_contextt *context, uint8 group, int limit);
}

// ──────────────────────────────────────────────────────────────────────────
// SetEmcyCallback — register per-slave EMCY callback from UI thread
// ──────────────────────────────────────────────────────────────────────────
void EtherCATMaster::SetEmcyCallback(int slaveIndex, EmcyCallback callback)
{
    if (slaveIndex < 0 || slaveIndex > slaveCount) return;

    EnterCriticalSection(&emcyCS_);
    // Ensure vector is large enough (index 0 unused)
    if (static_cast<int>(emcyCallbacks_.size()) <= slaveIndex) {
        emcyCallbacks_.resize(slaveIndex + 1);
    }
    emcyCallbacks_[slaveIndex] = std::move(callback);
    LeaveCriticalSection(&emcyCS_);
}

// ──────────────────────────────────────────────────────────────────────────
// SetGlobalEmcyCallback — register callback for ALL slaves
// ──────────────────────────────────────────────────────────────────────────
void EtherCATMaster::SetGlobalEmcyCallback(EmcyCallback callback)
{
    EnterCriticalSection(&emcyCS_);
    globalEmcyCallback_ = std::move(callback);
    LeaveCriticalSection(&emcyCS_);
}

// ──────────────────────────────────────────────────────────────────────────
// GetLatestEmcyInfo — lock-free read of latest EMCY data for a slave
// ──────────────────────────────────────────────────────────────────────────
bool EtherCATMaster::GetLatestEmcyInfo(int slaveIndex, EmcyInfo& outInfo, bool consume)
{
    if (!emcyCache_ || slaveIndex < 1 || slaveIndex > slaveCount) return false;

    auto& slot = emcyCache_[slaveIndex];
    if (!slot.hasNewData.load(std::memory_order_acquire)) return false;

    outInfo.slave         = static_cast<uint16_t>(slaveIndex);
    outInfo.errorCode     = slot.errorCode.load(std::memory_order_relaxed);
    outInfo.errorRegister = slot.errorRegister.load(std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) {
        outInfo.mfgField[i] = slot.mfgField[i].load(std::memory_order_relaxed);
    }
    outInfo.timestamp     = slot.timestamp.load(std::memory_order_relaxed);

    if (consume) {
        slot.hasNewData.store(false, std::memory_order_release);
    }
    return true;
}

// ──────────────────────────────────────────────────────────────────────────
// ServiceEmcyMailbox — poll slave mailbox hardware for incoming EMCY messages
//
// SOEM's ecx_receive_processdata() only processes PDO datagrams (LRD/LRW).
// Mailbox data (CoE Emergency, SDO responses) arrives via ESC mailbox buffers
// (SM0/SM1) and must be explicitly read by calling ecx_mbxinhandler().
//
// Called from CyclicTask with throttling to limit RT overhead:
//   - Every cycle when WKC is abnormal (urgent: may indicate slave fault)
//   - Every ~10 cycles normally (background mailbox polling)
// ──────────────────────────────────────────────────────────────────────────
void EtherCATMaster::ServiceEmcyMailbox()
{
    if (slaveCount <= 0) return;
    if (simulationMode_.load(std::memory_order_relaxed)) return;

    // Throttle: only poll mailbox every N cycles, or every cycle if WKC abnormal
    static int pollCounter = 0;
    constexpr int kNormalPollInterval = 10;  // poll every 10 PDO cycles (~20ms @ 2kHz)

    int32_t wkc = workingCounter.load(std::memory_order_relaxed);
    bool urgent = (wkc <= 0) || (wkc == EC_NOFRAME);

    ++pollCounter;
    if (!urgent && (pollCounter % kNormalPollInterval) != 0) return;

    // ecx_mbxinhandler reads the ESC mailbox status register for each slave.
    // When a slave has pending mailbox data (bit 3 of mbxstatus), it issues
    // an FPRD to read the mailbox and dispatches CoE/SoE/EoE/FoE data.
    //
    // limit=2: process at most 2 mailbox reads per call to bound RT latency.
    // The function internally iterates slaves round-robin via lastmbxpos.
    ecx_mbxinhandler(&context, 0, 2);
}

// ──────────────────────────────────────────────────────────────────────────
// EmcyInfoFromError — populate EmcyInfo from SOEM ec_errort union
// ──────────────────────────────────────────────────────────────────────────
static void EmcyInfoFromError(const ec_errort& ecErr, EtherCATMaster::EmcyInfo& info)
{
    info.slave         = ecErr.Slave;
    info.errorCode     = ecErr.ErrorCode;
    info.errorRegister = ecErr.ErrorReg;
    info.mfgField[0]   = ecErr.b1;
    info.mfgField[1]   = static_cast<uint8_t>(ecErr.w1 & 0xFF);
    info.mfgField[2]   = static_cast<uint8_t>((ecErr.w1 >> 8) & 0xFF);
    info.mfgField[3]   = static_cast<uint8_t>(ecErr.w2 & 0xFF);
    info.mfgField[4]   = static_cast<uint8_t>((ecErr.w2 >> 8) & 0xFF);

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
}

// ──────────────────────────────────────────────────────────────────────────
// DispatchEmcy — update atomic cache + invoke callbacks for one EmcyInfo
// ──────────────────────────────────────────────────────────────────────────
void EtherCATMaster::DispatchEmcy(const EmcyInfo& info)
{
    const int slaveIdx = info.slave;
    if (slaveIdx < 1 || slaveIdx > slaveCount) return;

    // ── Debounce: skip if same error code already cached for this slave ──
    auto& slot = emcyCache_[slaveIdx];
    uint16_t prevCode = slot.errorCode.load(std::memory_order_relaxed);
    uint8_t  prevReg  = slot.errorRegister.load(std::memory_order_relaxed);
    if (prevCode == info.errorCode && prevReg == info.errorRegister && slot.hasNewData.load(std::memory_order_acquire)) {
        // Same error already dispatched — only update timestamp, no re-log
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        slot.timestamp.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_relaxed);
        SetGlobalError();
        return;
    }

    // ── Update lock-free atomic cache ──
    slot.errorCode.store(info.errorCode, std::memory_order_relaxed);
    slot.errorRegister.store(info.errorRegister, std::memory_order_relaxed);
    for (int i = 0; i < 5; ++i) {
        slot.mfgField[i].store(info.mfgField[i], std::memory_order_relaxed);
    }
    slot.timestamp.store(info.timestamp, std::memory_order_relaxed);
    slot.hasNewData.store(true, std::memory_order_release);

    // ── Log with source tag (stack buffer — no heap allocation in RT thread) ──
    {
        const char* sourceTag = "EMCY";
        if (info.errorCode == 0xFF01) {
            sourceTag = "EMCY-COMMLOSS";       // synthesized: slave FPRD failed
        } else if ((info.errorCode & 0xF000) == 0x1000) {
            sourceTag = "EMCY-NOT-OP";          // Layer E: slave left OP state
        } else if (info.errorRegister == 0x11 && info.errorCode != 0xFF01) {
            sourceTag = "EMCY-ALSTAT";          // AL status code read from slave register
        }
        char buf[112];
        snprintf(buf, sizeof(buf),
            "%s slave#%d ErrCode=0x%04X ErrReg=0x%02X state=0x%02X",
            sourceTag, slaveIdx, info.errorCode,
            static_cast<int>(info.errorRegister),
            static_cast<int>(info.mfgField[0]));
        SetError(std::string(buf));
    }

    // ── Callbacks ──
    EmcyCallback slaveCb;
    if (slaveIdx < static_cast<int>(emcyCallbacks_.size())) {
        slaveCb = emcyCallbacks_[slaveIdx];
    }
    if (slaveCb) slaveCb(info);
    if (globalEmcyCallback_) globalEmcyCallback_(info);

    // ── Latch global error for any EMCY event ──
    // All emergency events (real CoE Emergency, synthesized comm-loss,
    // or AL-status error) indicate a slave fault that requires motion
    // stop.  SetGlobalError() is an atomic store; the UI timer polls
    // HasGlobalError() asynchronously to trigger emergency stop.
    SetGlobalError();
}

// ──────────────────────────────────────────────────────────────────────────
// ProcessEmcyErrors — service mailbox HW, drain SOEM error ring buffer,
// and synthesize emergency from AL status when WKC is abnormal.
// Called from CyclicTask (RT thread) every PDO cycle.
// ──────────────────────────────────────────────────────────────────────────
void EtherCATMaster::ProcessEmcyErrors()
{
    if (slaveCount <= 0) return;
    if (simulationMode_.load(std::memory_order_relaxed)) return;

    // Lazy-allocate or re-allocate EMCY cache when slave count changes
    if (!emcyCache_ || pdoCacheSlaveCount_.load(std::memory_order_relaxed) != slaveCount) {
        emcyCache_ = std::make_unique<EmcySlot[]>(slaveCount + 1); // 1-indexed
    }

    // ── Step 1: Service mailbox hardware ──
    // This is the critical fix: ecx_receive_processdata does NOT process
    // mailbox data.  We must call ecx_mbxinhandler to actually read the
    // ESC mailbox buffers and dispatch CoE Emergency messages into SOEM's
    // error ring buffer (context.elist).
    ServiceEmcyMailbox();

    // ── Step 2: Drain SOEM error ring buffer ──
    ec_errort ecErr;
    while (ecx_poperror(&context, &ecErr)) {
        if (ecErr.Etype == EC_ERR_TYPE_EMERGENCY) {
            EmcyInfo info;
            EmcyInfoFromError(ecErr, info);
            DispatchEmcy(info);
        }
    }

    // ── Step 3: Multi-layer fallback diagnostics ──
    // Layer A: WKC deviation detection (every cycle)
    // Layer B: Cycle-time watchdog alert (every cycle)
    // Layer C: Periodic AL Status poll for ALL slaves (every ~500 cycles)
    // Layer D: Reactive AL Status read when WKC or cycle is bad (every ~100 cycles)
    //
    // Some slave firmwares do NOT generate a CoE Emergency for PDO
    // watchdog timeout.  These layers provide independent detection.
    static unsigned long fallbackCounter = 0;
    ++fallbackCounter;

    int32_t wkc = workingCounter.load(std::memory_order_relaxed);
    uint32_t cycleUs = observedCycleMicros_.load(std::memory_order_relaxed);

    // ── Expected WKC from SOEM group config (cached — stable after init) ──
    static uint32_t cachedExpectedWkc = 0;
    if (cachedExpectedWkc == 0 && slaveCount > 0) {
        EnterCriticalSection(&cs);
        cachedExpectedWkc = static_cast<uint32_t>(
            context.grouplist[0].outputsWKC +
            context.grouplist[0].inputsWKC);
        LeaveCriticalSection(&cs);
    }

    // ── Layer A: WKC deviation ──
    // EC_NOFRAME (-1) = NIC/driver failure, no frame returned
    // 0 = frame returned but no slave processed it
    // < expected = partial slave failure (some slaves not responding)
    bool wkcNoFrame  = (wkc == EC_NOFRAME);
    bool wkcZero     = (wkc == 0);
    bool wkcPartial  = (cachedExpectedWkc > 0 && wkc > 0 && static_cast<uint32_t>(wkc) < cachedExpectedWkc);

    // ── Layer B: Cycle-time watchdog ──
    uint32_t watchdogThresholdUs = desiredCycleTime * 3;
    if (watchdogThresholdUs < 3000) watchdogThresholdUs = 3000; // floor 3ms
    bool cycleSpike = (cycleUs > 0 && cycleUs >= watchdogThresholdUs);

    bool anyTrigger = wkcNoFrame || wkcZero || wkcPartial || cycleSpike;

    // ── WKC deviation → latch global error for UI/watchdog visibility ──
    // EC_NOFRAME, WKC=0, or partial WKC all indicate slave communication
    // loss.  SetGlobalError() is an atomic store safe from the RT cyclic
    // thread; the UI timer and watchdog poll HasGlobalError()
    // asynchronously to trigger emergency stop and status messages.
    //
    // Debounce: EC_NOFRAME and WKC=0 can occur transiently from NIC driver
    // scheduling jitter or DPC interference — a single bad frame should not
    // trigger emergency stop.  Require kNoframeThreshold consecutive bad
    // frames (default 5 ≈ 10ms @ 2kHz) before latching the error.
    // WKC partial (some slaves responding, some not) triggers immediately
    // because it indicates sustained slave loss, not a transient NIC glitch.
    static constexpr int kNoframeThreshold = 5;
    static int noframeCounter = 0;
    static bool wkcWasBad = false;

    if (wkcNoFrame || wkcZero) {
        ++noframeCounter;
        if (noframeCounter >= kNoframeThreshold) {
            SetGlobalError();
            if (!wkcWasBad) {
                wkcWasBad = true;
                char buf[128];
                if (wkcNoFrame) {
                    snprintf(buf, sizeof(buf),
                        "EMCY-CRIT: EC_NOFRAME NIC/driver failure x%d consecutive (WKC=%d)",
                        noframeCounter, wkc);
                } else {
                    snprintf(buf, sizeof(buf),
                        "EMCY-CRIT: WKC=0 no slave responded x%d consecutive (expected=%u)",
                        noframeCounter, cachedExpectedWkc);
                }
                SetError(std::string(buf));
            }
        }
    } else if (wkcPartial) {
        // Partial WKC: immediate trigger (sustained slave loss)
        noframeCounter = 0;
        SetGlobalError();
        if (!wkcWasBad) {
            wkcWasBad = true;
            char buf[128];
            snprintf(buf, sizeof(buf),
                "EMCY-CRIT: WKC=%d < expected=%u (~%u slaves unresponsive)",
                wkc, cachedExpectedWkc,
                cachedExpectedWkc - static_cast<uint32_t>(wkc));
            SetError(std::string(buf));
        }
    } else {
        // WKC normal — reset all counters
        noframeCounter = 0;
        wkcWasBad = false;
    }

    // ── Layer C: Unconditional periodic AL Status poll (every ~500 cycles ≈ 1s) ──
    // Detects slave faults that don't affect WKC (e.g., internal drive fault
    // while slave still responds to EtherCAT frames).
    // Also synthesizes communication-loss EMCY when a previously-known slave
    // no longer responds to register reads (rdWkc <= 0).
    //
    // Layer E (inline): reads ECT_REG_ALSTAT (0x0130) — the actual EtherCAT
    // state register — to detect slaves that left OP without setting an error
    // code (e.g. clean transition OP→SAFE_OP where ALSTATCODE=0 but PDO is
    // no longer exchanged).  A slave not in OP (state != 0x08) with non-zero
    // configadr is a PDO read/write failure.
    if ((fallbackCounter % 500) == 0) {
        for (int i = 1; i <= slaveCount; ++i) {
            ec_slavet& sl = context.slavelist[i];
            if (sl.state == 0) continue;

            uint16_t alStatusCode = 0;
            int rdWkc = ecx_FPRD(&context.port, sl.configadr,
                ECT_REG_ALSTATCODE, sizeof(alStatusCode), &alStatusCode, EC_TIMEOUTRET);

            if (rdWkc > 0 && alStatusCode != 0) {
                EmcyInfo info;
                info.slave = static_cast<uint16_t>(i);
                info.errorCode = alStatusCode;
                info.errorRegister = 0x11;  // bit0=generic, bit4=comms
                info.mfgField[0] = static_cast<uint8_t>(sl.state & 0xFF);
                info.mfgField[1] = static_cast<uint8_t>((sl.state >> 8) & 0xFF);
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                DispatchEmcy(info);
            } else if (rdWkc <= 0 && (wkcNoFrame || wkcZero || wkcPartial)) {
                // Slave not responding to register reads during WKC deviation —
                // synthesize a communication-loss emergency.
                EmcyInfo info;
                info.slave = static_cast<uint16_t>(i);
                info.errorCode = 0xFF01;  // manufacturer-specific: comm loss
                info.errorRegister = 0x11; // bit0=generic, bit4=comms
                info.mfgField[0] = static_cast<uint8_t>(sl.state & 0xFF);
                info.mfgField[1] = 0;
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                DispatchEmcy(info);
            }

            // ── Layer E: check actual EtherCAT state (0x0130) ──
            // A slave not in OP (state != 0x08) means PDO is not being
            // exchanged, even if WKC and ALSTATCODE appear normal.
            if (rdWkc > 0) {
                uint16_t alStatReg = 0;
                int rdWkc2 = ecx_FPRD(&context.port, sl.configadr,
                    ECT_REG_ALSTAT, sizeof(alStatReg), &alStatReg, EC_TIMEOUTRET);
                if (rdWkc2 > 0) {
                    uint8_t slaveState = static_cast<uint8_t>(alStatReg & 0x0F);
                    if (slaveState != 0x08 && slaveState != 0x00) {
                        // Slave not in OP — synthesize state-change EMCY
                        EmcyInfo info;
                        info.slave = static_cast<uint16_t>(i);
                        info.errorCode = (0x1000u | static_cast<uint16_t>(slaveState));
                        info.errorRegister = 0x11;
                        info.mfgField[0] = slaveState;
                        info.mfgField[1] = static_cast<uint8_t>((alStatReg >> 8) & 0xFF);
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                        DispatchEmcy(info);
                    }
                }
            }
        }
    }

    // ── Layer D: Reactive AL Status read when trigger is active ──
    // Faster polling (every ~100 cycles ≈ 200ms) when a problem is detected.
    // Synthesizes communication-loss EMCY for slaves that fail to respond
    // to register reads (rdWkc <= 0) during active WKC deviation.
    if (anyTrigger && (fallbackCounter % 100) == 0) {
        for (int i = 1; i <= slaveCount; ++i) {
            ec_slavet& sl = context.slavelist[i];
            if (sl.state == 0) continue;

            uint16_t alStatusCode = 0;
            int rdWkc = ecx_FPRD(&context.port, sl.configadr,
                ECT_REG_ALSTATCODE, sizeof(alStatusCode), &alStatusCode, EC_TIMEOUTRET);

            if (rdWkc > 0 && alStatusCode != 0) {
                EmcyInfo info;
                info.slave = static_cast<uint16_t>(i);
                info.errorCode = alStatusCode;
                info.errorRegister = 0x11;
                info.mfgField[0] = static_cast<uint8_t>(sl.state & 0xFF);
                info.mfgField[1] = static_cast<uint8_t>((sl.state >> 8) & 0xFF);
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                DispatchEmcy(info);
            } else if (rdWkc <= 0 && (wkcNoFrame || wkcZero || wkcPartial)) {
                // Slave not responding to register reads during WKC deviation —
                // synthesize a communication-loss emergency.
                EmcyInfo info;
                info.slave = static_cast<uint16_t>(i);
                info.errorCode = 0xFF01;  // manufacturer-specific: comm loss
                info.errorRegister = 0x11;
                info.mfgField[0] = static_cast<uint8_t>(sl.state & 0xFF);
                info.mfgField[1] = 0;
                LARGE_INTEGER qpc;
                QueryPerformanceCounter(&qpc);
                info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                DispatchEmcy(info);
            }

            // ── Layer E (reactive): check actual EtherCAT state (0x0130) ──
            if (rdWkc > 0) {
                uint16_t alStatReg = 0;
                int rdWkc2 = ecx_FPRD(&context.port, sl.configadr,
                    ECT_REG_ALSTAT, sizeof(alStatReg), &alStatReg, EC_TIMEOUTRET);
                if (rdWkc2 > 0) {
                    uint8_t slaveState = static_cast<uint8_t>(alStatReg & 0x0F);
                    if (slaveState != 0x08 && slaveState != 0x00) {
                        EmcyInfo info;
                        info.slave = static_cast<uint16_t>(i);
                        info.errorCode = (0x1000u | static_cast<uint16_t>(slaveState));
                        info.errorRegister = 0x11;
                        info.mfgField[0] = slaveState;
                        info.mfgField[1] = static_cast<uint8_t>((alStatReg >> 8) & 0xFF);
                        LARGE_INTEGER qpc;
                        QueryPerformanceCounter(&qpc);
                        info.timestamp = static_cast<uint64_t>(qpc.QuadPart);
                        DispatchEmcy(info);
                    }
                }
            }
        }
    }

    // ── Log warnings when triggers are active ──
    if (anyTrigger && (fallbackCounter % 100) == 0) {
        std::ostringstream oss;
        if (wkcNoFrame || wkcZero || wkcPartial) {
            oss << "EMCY-CRIT: ";
        } else {
            oss << "EMCY-WARN: ";
        }
        if (wkcNoFrame) {
            oss << "EC_NOFRAME (NIC/driver issue)";
        } else if (wkcZero) {
            oss << "WKC=0 (no slave responded)";
        } else if (wkcPartial) {
            oss << "WKC=" << wkc << " < expected=" << cachedExpectedWkc
                << " (partial slave failure, ~"
                << (cachedExpectedWkc - static_cast<uint32_t>(wkc))
                << " missing)";
        }
        if (cycleSpike) {
            if (wkcPartial || wkcZero || wkcNoFrame) oss << "; ";
            oss << "cycle spike " << cycleUs << "us >="
                << watchdogThresholdUs << "us (SM watchdog risk)";
        }
        oss << " — checking AL Status";
        SetError(oss.str());
    }
}
