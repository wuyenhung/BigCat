#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <memory>
#include <mutex>

class EtherCATMaster;
#include "PDOProxy.h"

// Simple result struct returned from motion operations
struct MotionResult {
    bool success;
    std::wstring message; // human readable UTF-16 message for UI
};

class MotionController {
public:
    MotionController(EtherCATMaster* master, PDOProxy* pdo);
    ~MotionController();

    // Allow attaching or updating the EtherCAT master after construction
    void SetMaster(EtherCATMaster* master);

    // Non-UI, thread-safe entry points. Implementations perform core
    // EtherCAT / PDO staging work and return diagnostic text for the UI.
    MotionResult FaultResetAsync(int slaveIdx, bool assert);
    // Reset all slaves: perform assert on all slaves then release on all.
    // Returns a MotionResult whose message is a concatenation of per-slave messages
    // and success==true if any individual operation reported success.
    MotionResult FaultResetAll();
    MotionResult ImmediateStopAsync(int slaveIdx, bool assert);
    MotionResult ImmediateStopAll();
    // Attempt a smooth stop by asking PDOFilters to hold their current position
    // while also staging the active cyclic command PDO to zero velocity.
    // rampSec is reserved for future use.
    // Returns success==true if staging succeeded for at least one slave.
    MotionResult SmoothStopAll(double rampSec = 0.25);
    MotionResult PowerOffAsync(int slaveIdx, bool assert);
    // PowerOnAsync optionally accepts precomputed motion-parameter values used
    // to write SDOs 0x60E0/0x60E1 before executing the rest of the PowerOn
    // sequence. Callers that don't have these values may omit them (defaults
    // keep compatibility).
    MotionResult PowerOnAsync(int slaveIdx, bool assert, bool haveParams = false);
    // HomeAsync: origin-setting sequence. Optionally accept a P13 motion-parameter value (from
    // `savedMotionParamsForSlave` "P13" row, column '值'). If haveP13==true
    // the controller will write that value into SDO 0x607C instead of 0.
    // Set skipSdo=true when HomePrepareSDO already ran — skips SDO preamble and
    // jumps directly to PDO controlword staging + polling.
    MotionResult HomeAsync(int slaveIdx, bool assert, bool haveP13 = false, uint32_t p13 = 0, bool skipSdo = false);
    // HomePrepareSDO: execute only the SDO-heavy preamble (0x6060=6, 0x6098, 0x607C)
    // without staging any controlword. Call this for every slave first, then
    // call HomeAsync with skipSdo=true for all slaves so the PDO controlword
    // stages are batched into the same cycle — all axes start homing together.
    bool HomePrepareSDO(int slaveIdx, bool haveP13, uint32_t p13);
    // PowerOnAllAsync: Batch power-on for multiple slaves, ensuring all PDO data
    // is staged before any controlword is sent to prevent race conditions.
    // This avoids the issue where sequential PowerOnAsync calls would enable one
    // axis before others are ready, causing position error faults.
    MotionResult PowerOnAllAsync(int slaveCount);
    // Jog (寸動) operations: start/stop positive and negative direction
    MotionResult JogPositiveStart(int slaveIdx, uint32_t stagedVal = 20000u);
    MotionResult JogNegativeStart(int slaveIdx, uint32_t stagedVal = (uint32_t)-20000);
    // Batch jog: all axes zero 0x60FF, switch to CSV mode, wait for all mode
    // switches, then write target velocity simultaneously — eliminates the
    // per-axis serial delay that causes visible start-asynchrony.
    MotionResult JogNegativeStartBatch(const std::vector<int>& slaves, const std::vector<uint32_t>& velocities);
    MotionResult JogStop(int slaveIdx);

    // 設置寸動緩啟動時間（秒），預設 0.5 秒
    void SetJogSoftStartTime(double timeSec) { jogSoftStartTime_ = timeSec; }
    double GetJogSoftStartTime() const { return jogSoftStartTime_; }

    // PDO display proxy for motion-command staging (e.g. 0x60FF=0)
    PDOProxy* GetPDODisplay() const { return pdoDisplay; }

private:
    EtherCATMaster* etherCATMaster;
    PDOProxy* pdoDisplay;

    // 寸動緩啟動相關
    double jogSoftStartTime_ = 0.5;  // 預設 0.5 秒

    // Per-slave jog state holds the thread and atomic flags for that slave.
    struct JogState {
        std::atomic<bool> active{false};
        std::atomic<bool> stop{false};
        std::thread thr; // may be empty if not running
        uint32_t targetVel{0};
    };

    // Keep shared_ptr values so threads may hold onto state object safely.
    std::unordered_map<int, std::shared_ptr<JogState>> jogStates_;
    // Protects access to jogStates_
    std::mutex jogStatesMutex_;

    // 寸動緩啟動執行函數
    void RunJogSoftStart(int slaveIdx, uint32_t targetVel);
    // Internal worker that accepts shared state so thread can safely consult stop flag
    void RunJogSoftStartWorker(std::shared_ptr<JogState> state, int slaveIdx);
};
