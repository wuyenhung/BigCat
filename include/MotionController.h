#pragma once
#include <string>
#include <vector>

class EtherCATMaster;
class PDODisplayManager;

// Simple result struct returned from motion operations
struct MotionResult {
    bool success;
    std::wstring message; // human readable UTF-16 message for UI
};

class MotionController {
public:
    MotionController(EtherCATMaster* master, PDODisplayManager* pdo);
    ~MotionController();

    // Allow attaching or updating the EtherCAT master after construction
    void SetMaster(EtherCATMaster* master);

    // Non-UI, thread-safe entry points. Implementations perform core
    // EtherCAT / PDO staging work and return diagnostic text for the UI.
    MotionResult FaultResetAsync(int slaveIdx, bool assert);
    MotionResult ImmediateStopAsync(int slaveIdx, bool assert);
    MotionResult PowerOffAsync(int slaveIdx, bool assert);
    MotionResult PowerOnAsync(int slaveIdx, bool assert);
    MotionResult HomeAsync(int slaveIdx, bool assert);

private:
    EtherCATMaster* etherCATMaster;
    PDODisplayManager* pdoDisplay;
};
