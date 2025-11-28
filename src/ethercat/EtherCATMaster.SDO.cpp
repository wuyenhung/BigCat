#include "EtherCATMaster.h"
#include <iostream>
#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <eh.h>
#include <iomanip>
#include <bitset>
#include <chrono>
#include <cmath>
#include <intrin.h>
#include <mmsystem.h>
#include <soem/ec_print.h>
#include <fstream>
#include "SafeMem.h"

namespace {
constexpr int kSdoIdleMinAttemptUs = 250;
constexpr int kSdoIdleGuardUs = 150;

std::string FormatSdoFailureMessage(const char* action,
                                    uint16_t slave,
                                    uint16_t index,
                                    uint8_t subindex,
                                    const char* suffix = nullptr)
{
    std::ostringstream oss;
    oss << "Failed to " << action << " SDO "
        << (action[0] == 'r' ? "from" : "to") << " slave " << slave
        << ", Index: 0x" << std::hex << index
        << ", Subindex: 0x" << static_cast<int>(subindex);
    if (suffix && suffix[0] != '\0') {
        oss << " (" << suffix << ')';
    }
    return oss.str();
}
}

bool EtherCATMaster::WaitForSdoIdleWindow(const std::chrono::steady_clock::time_point& deadline,
                                          int minWindowUs,
                                          int guardUs,
                                          int& attemptTimeoutUs)
{
    attemptTimeoutUs = 0;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const long long remainingOverallUs64 =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        if (remainingOverallUs64 <= 0) {
            return false;
        }

        if (!IsRunning()) {
            attemptTimeoutUs = static_cast<int>(remainingOverallUs64);
            return attemptTimeoutUs > 0;
        }

        if (!pdoExchangeActive_.load(std::memory_order_acquire)) {
            const long long idleDeadlineUs = pdoIdleWindowDeadlineUs_.load(std::memory_order_acquire);
            const long long nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count();
            const long long remainingIdleUs64 = idleDeadlineUs - nowUs - guardUs;
            if (remainingIdleUs64 > 0) {
                // The idle window is only used as a safe start gate. Clipping the
                // whole SOEM mailbox timeout down to a single predicted idle gap made
                // post-OP SDO reads/writes fail persistently on real hardware.
                if (remainingIdleUs64 >= minWindowUs) {
                    attemptTimeoutUs = static_cast<int>(remainingOverallUs64);
                    return true;
                }
            }
        }

        if (remainingOverallUs64 > 1000) {
            Sleep(0);
        } else {
            YieldProcessor();
        }
    }
}

bool EtherCATMaster::ReadSDODirectLocked(uint16_t slave, uint16_t index, uint8_t subindex,
                                         void* data, int& size, int timeoutUs,
                                         bool reportFailure, std::string* errorMessage)
{
    if (!isInitialized || slave == 0 || slave > slaveCount || data == nullptr || size <= 0) {
        const std::string message = "Invalid slave or not initialized";
        if (errorMessage) {
            *errorMessage = message;
        }
        if (reportFailure) {
            SetError(message);
        }
        return false;
    }

    if (timeoutUs <= 0) {
        timeoutUs = EC_TIMEOUTRXM;
    }

    const int wkc = ecx_SDOread(&context, slave, index, subindex, FALSE, &size, data, timeoutUs);

    if (wkc <= 0) {
        const std::string message = FormatSdoFailureMessage("read", slave, index, subindex);
        if (errorMessage) {
            *errorMessage = message;
        }
        if (reportFailure) {
            SetError(message);
        }
        return false;
    }
    return true;
}

bool EtherCATMaster::WriteSDODirectLocked(uint16_t slave, uint16_t index, uint8_t subindex,
                                          void* data, int size, int timeoutUs,
                                          bool reportFailure, std::string* errorMessage)
{
    if (!isInitialized || slave == 0 || slave > slaveCount || data == nullptr || size <= 0) {
        const std::string message = "Invalid slave or not initialized";
        if (errorMessage) {
            *errorMessage = message;
        }
        if (reportFailure) {
            SetError(message);
        }
        return false;
    }

    if (timeoutUs <= 0) {
        timeoutUs = EC_TIMEOUTRXM;
    }

    const int wkc = ecx_SDOwrite(&context, slave, index, subindex, FALSE, size, data, timeoutUs);

    if (wkc <= 0) {
        const std::string message = FormatSdoFailureMessage("write", slave, index, subindex);
        if (errorMessage) {
            *errorMessage = message;
        }
        if (reportFailure) {
            SetError(message);
        }
        return false;
    }
    return true;
}

bool EtherCATMaster::ReadSDODirect(uint16_t slave, uint16_t index, uint8_t subindex,
                                   void* data, int& size, int timeoutUs,
                                   bool reportFailure, std::string* errorMessage)
{
    EnterCriticalSection(&sdoCS);
    const bool ok = ReadSDODirectLocked(slave, index, subindex, data, size,
                                        timeoutUs, reportFailure, errorMessage);
    LeaveCriticalSection(&sdoCS);
    return ok;
}

bool EtherCATMaster::WriteSDODirect(uint16_t slave, uint16_t index, uint8_t subindex,
                                    void* data, int size, int timeoutUs,
                                    bool reportFailure, std::string* errorMessage)
{
    EnterCriticalSection(&sdoCS);
    const bool ok = WriteSDODirectLocked(slave, index, subindex, data, size,
                                         timeoutUs, reportFailure, errorMessage);
    LeaveCriticalSection(&sdoCS);
    return ok;
}

bool EtherCATMaster::ReadSDODuringIdleWindow(uint16_t slave, uint16_t index, uint8_t subindex,
                                             void* data, int& size, int timeoutUs,
                                             bool reportFailure, std::string* errorMessage)
{
    const int overallTimeoutUs = (timeoutUs > 0) ? timeoutUs : EC_TIMEOUTRXM;
    const bool onPdoThread = (pdoProcessingThread != nullptr) &&
        (GetCurrentThreadId() == GetThreadId(pdoProcessingThread));
    if (!IsRunning() || onPdoThread) {
        return ReadSDODirect(slave, index, subindex, data, size,
                             overallTimeoutUs, reportFailure, errorMessage);
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(overallTimeoutUs);
    std::string lastAttemptError;
    bool success = false;

    EnterCriticalSection(&sdoCS);
    while (true) {
        int attemptTimeoutUs = 0;
        if (!WaitForSdoIdleWindow(deadline, kSdoIdleMinAttemptUs, kSdoIdleGuardUs, attemptTimeoutUs)) {
            if (lastAttemptError.empty()) {
                lastAttemptError = "Timed out waiting for PDO idle gap for SDO read";
            }
            break;
        }

        int readSize = size;
        std::string attemptError;
        success = ReadSDODirectLocked(slave, index, subindex, data, readSize,
                                      attemptTimeoutUs, false, &attemptError);
        if (success) {
            size = readSize;
            break;
        }

        if (!attemptError.empty()) {
            lastAttemptError = attemptError;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }
    LeaveCriticalSection(&sdoCS);

    if (!success) {
        if (errorMessage) {
            *errorMessage = lastAttemptError;
        }
        if (reportFailure) {
            SetError(lastAttemptError.empty()
                         ? FormatSdoFailureMessage("read", slave, index, subindex, "idle-gap timeout")
                         : lastAttemptError);
        }
    }
    return success;
}

bool EtherCATMaster::WriteSDODuringIdleWindow(uint16_t slave, uint16_t index, uint8_t subindex,
                                              void* data, int size, int timeoutUs,
                                              bool reportFailure, std::string* errorMessage)
{
    const int overallTimeoutUs = (timeoutUs > 0) ? timeoutUs : EC_TIMEOUTRXM;
    const bool onPdoThread = (pdoProcessingThread != nullptr) &&
        (GetCurrentThreadId() == GetThreadId(pdoProcessingThread));
    if (!IsRunning() || onPdoThread) {
        return WriteSDODirect(slave, index, subindex, data, size,
                              overallTimeoutUs, reportFailure, errorMessage);
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(overallTimeoutUs);
    std::string lastAttemptError;
    bool success = false;

    EnterCriticalSection(&sdoCS);
    while (true) {
        int attemptTimeoutUs = 0;
        if (!WaitForSdoIdleWindow(deadline, kSdoIdleMinAttemptUs, kSdoIdleGuardUs, attemptTimeoutUs)) {
            if (lastAttemptError.empty()) {
                lastAttemptError = "Timed out waiting for PDO idle gap for SDO write";
            }
            break;
        }

        std::string attemptError;
        success = WriteSDODirectLocked(slave, index, subindex, data, size,
                                       attemptTimeoutUs, false, &attemptError);
        if (success) {
            break;
        }

        if (!attemptError.empty()) {
            lastAttemptError = attemptError;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            break;
        }
    }
    LeaveCriticalSection(&sdoCS);

    if (!success) {
        if (errorMessage) {
            *errorMessage = lastAttemptError;
        }
        if (reportFailure) {
            SetError(lastAttemptError.empty()
                         ? FormatSdoFailureMessage("write", slave, index, subindex, "idle-gap timeout")
                         : lastAttemptError);
        }
    }
    return success;
}

bool EtherCATMaster::ReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                             void* data, int &size, int timeoutUs) {
    if (simulationMode_.load(std::memory_order_relaxed)) {
        return ReadSimulationSdo(slave, index, subindex, data, size);
    }

    if (!isInitialized || slave == 0 || slave > slaveCount || data == nullptr || size <= 0) {
        SetError("Invalid slave or not initialized");
        return false;
    }

    return ReadSDODuringIdleWindow(slave, index, subindex, data, size,
                                   timeoutUs, true, nullptr);
}


bool EtherCATMaster::WriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                              void* data, int size) {
    if (simulationMode_.load(std::memory_order_relaxed)) {
        return WriteSimulationSdo(slave, index, subindex, data, size);
    }

    if (!isInitialized || slave == 0 || slave > slaveCount || data == nullptr || size <= 0) {
        SetError("Invalid slave or not initialized");
        return false;
    }

    return WriteSDODuringIdleWindow(slave, index, subindex, data, size,
                                    EC_TIMEOUTRXM, true, nullptr);
}

// Write SDO and read back to verify bytes (LE). Retries up to maxRetries.

bool EtherCATMaster::WriteAndVerifySDO(uint16_t slave, uint16_t index, uint8_t subindex,
                                      const void* data, int size, int maxRetries, int timeoutUs)
{
    if (!data || size <= 0) return false;
    // simplified SDO log: route entries to program message channels (no file IO)
    auto AppendSDOTrace = [&](const std::string &line){
        // Prefix for clarity
        std::string out = std::string("SDO: ") + line;
        // Store last error/message so UI can pick it up via existing SetError
        SetError(out);
        // Emit to debugger output and stdout for visibility
        OutputDebugStringA((out + "\n").c_str());
        printf("%s\n", out.c_str());
    };
    // make a local copy of bytes to compare against
    const uint8_t* wbytes = reinterpret_cast<const uint8_t*>(data);
    // log write request summary
    {
        char bf[256]; sprintf_s(bf, sizeof(bf), "slave=%u index=0x%04X sub=0x%02X action=write size=%d retries=%d", (unsigned)slave, (unsigned)index, (unsigned)subindex, size, maxRetries);
        AppendSDOTrace(std::string(bf));
    }
    // 僅使用 sdoCS 串行化 mailbox，避免長時間持有全域 cs 造成 PDO 抖動
    for (int attempt = 0; attempt < maxRetries; ++attempt) {
    char atb[128]; sprintf_s(atb, sizeof(atb), "slave=%u index=0x%04X sub=0x%02X attempt=%d", (unsigned)slave, (unsigned)index, (unsigned)subindex, attempt); AppendSDOTrace(std::string(atb));
        std::string writeError;
        bool writeOk = WriteSDODuringIdleWindow(slave, index, subindex, const_cast<void*>(data),
                                                size, timeoutUs, false, &writeError);
        if (!writeOk) {
            if (!writeError.empty()) {
                AppendSDOTrace(std::string("action=write error=") + writeError);
            }
            AppendSDOTrace("action=write result=failed");
            Sleep(10);
            continue;
        }

        int readSz = size;
        std::vector<uint8_t> rbuf(size);
        std::string readError;
        if (ReadSDODuringIdleWindow(slave, index, subindex, rbuf.data(), readSz,
                                    timeoutUs, false, &readError)) {
            char bf2[256]; sprintf_s(bf2, sizeof(bf2), "slave=%u index=0x%04X sub=0x%02X action=read readSz=%d", (unsigned)slave, (unsigned)index, (unsigned)subindex, readSz); AppendSDOTrace(std::string(bf2));

            if (readSz == size) {
                bool ok = true;
                for (int i = 0; i < size; ++i) if (rbuf[i] != wbytes[i]) { ok = false; break; }
                if (ok) {
                    AppendSDOTrace("action=verify result=ok");
                    return true;
                } else {
                    AppendSDOTrace("action=verify result=mismatch");
                }
            } else {
                AppendSDOTrace("action=read result=size_mismatch");
            }
        } else {
            if (!readError.empty()) {
                AppendSDOTrace(std::string("action=read error=") + readError);
            }
            AppendSDOTrace("action=read result=failed");
        }

        Sleep(10);
    }
    AppendSDOTrace("WriteAndVerifySDO: all attempts failed\n");
    return false;
}


bool EtherCATMaster::SendMailbox(uint16_t slave, uint8_t* data, int size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 使用 CoE (CAN over EtherCAT) mailbox
    // 注意：完整的 mailbox 實作需要使用 ec_mbxbuft 結構
    // 標記參數為已使用以避免未使用參數警告
    (void)data;
    (void)size;
    // 這裡暫時返回未實作
    SetError("Mailbox send requires ec_mbxbuft structure - use SDO instead");
    LeaveCriticalSection(&cs);
    return false;
}


bool EtherCATMaster::ReceiveMailbox(uint16_t slave, uint8_t* data, int* size) {
    EnterCriticalSection(&cs);
    
    if (!isInitialized || slave > slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    // 注意：完整的 mailbox 實作需要使用 ec_mbxbuft 結構
    // 標記參數為已使用以避免未使用參數警告
    (void)data;
    (void)size;
    // 這裡暫時返回未實作
    SetError("Mailbox receive requires ec_mbxbuft structure - use SDO instead");
    LeaveCriticalSection(&cs);
    return false;
}


bool EtherCATMaster::DumpESI(uint16_t slave, std::string &outDump)
{
    EnterCriticalSection(&cs);
    outDump.clear();
    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        SetError("Invalid slave or not initialized");
        LeaveCriticalSection(&cs);
        return false;
    }

    if (simulationMode_.load(std::memory_order_relaxed)) {
        std::ostringstream oss;
        oss << "Simulation device description for slave " << slave << "\n";
        oss << "Name: " << simulationAxes_[slave].name << "\n";
        oss << "RxPDO: 0x1600 -> 0x6040, 0x607A, 0x60FF, 0x6071\n";
        oss << "TxPDO: 0x1A00 -> 0x6041, 0x6064, 0x606C, 0x6077\n";
        oss << "Supported SDOs: 0x1008, 0x1C12, 0x1C13, 0x1600, 0x1A00, 0x603F, 0x6040, 0x6041, 0x6060, 0x6061, 0x607A, 0x6064, 0x606C, 0x60FF, 0x607C, 0x6098, 0x60E0, 0x60E1, 0x60FD\n";
        outDump = oss.str();
        LeaveCriticalSection(&cs);
        return true;
    }

    // Allocate buffer for EEPROM dump (size EC_MAXEEPBUF from SOEM)
    const int BUFSZ = EC_MAXEEPBUF;
    std::vector<uint8_t> buf(BUFSZ, 0);

    // Use SOEM helper to dump eeprom into buffer
    ecx_esidump(&context, slave, buf.data());

    // Convert binary dump to hex lines
    std::ostringstream oss;
    oss << "ESI/EEPROM dump for slave " << slave << "\n";
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < BUFSZ; i += 16) {
        oss << std::setw(4) << i << ": ";
        for (int j = 0; j < 16; ++j) {
            if (i + j < BUFSZ) oss << std::setw(2) << (int)buf[i + j] << ' ';
            else oss << "   ";
        }
        oss << " ";
        for (int j = 0; j < 16; ++j) {
            char c = (char)buf[i + j];
            if (i + j < BUFSZ && isprint((unsigned char)c)) oss << c;
            else oss << '.';
        }
        oss << "\n";
    }

    // Also read a few EEPROM registers via ecx_readeeprom to provide summary
    uint32_t manuf = ecx_readeeprom(&context, slave, ECT_SII_MANUF, EC_TIMEOUTEEP);
    uint32_t id = ecx_readeeprom(&context, slave, ECT_SII_ID, EC_TIMEOUTEEP);
    uint32_t rev = ecx_readeeprom(&context, slave, ECT_SII_REV, EC_TIMEOUTEEP);
    oss << "Manufacturer: 0x" << std::hex << manuf << " Product ID: 0x" << id << " Revision: 0x" << rev << "\n";

    outDump = oss.str();
    LeaveCriticalSection(&cs);
    return true;
}


bool EtherCATMaster::ForceSlaveOperational(uint16_t slave, std::string &outLog)
{
    if (simulationMode_.load(std::memory_order_relaxed)) {
        std::ostringstream oss;
        if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
            oss << "Invalid slave or not initialized\n";
            outLog = oss.str();
            return false;
        }
        context.slavelist[slave].state = EC_STATE_OPERATIONAL;
        context.slavelist[0].state = EC_STATE_OPERATIONAL;
        oss << "Simulation slave " << slave << " already uses the virtual OP state\n";
        outLog = oss.str();
        return true;
    }

    // Use timeout to prevent infinite blocking
    DWORD waitResult = TryEnterCriticalSection(&cs) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (waitResult == WAIT_TIMEOUT) {
        // Try for up to 5 seconds
        for (int retry = 0; retry < 50; ++retry) {
            Sleep(100);
            if (TryEnterCriticalSection(&cs)) {
                waitResult = WAIT_OBJECT_0;
                break;
            }
        }
    }
    
    if (waitResult == WAIT_TIMEOUT) {
        outLog = "ERROR: Timeout acquiring lock (possible deadlock)\n";
        return false;
    }
    
    outLog.clear();
    std::ostringstream oss;

    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        oss << "Invalid slave or not initialized\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return false;
    }

    // Read current state and AL status
    ecx_readstate(&context);
    int st = context.slavelist[slave].state;
    oss << "Slave " << slave << " current state=0x" << std::hex << st << std::dec << "\n";
    const char* alstr = ec_ALstatuscode2string(context.slavelist[slave].ALstatuscode);
    oss << "ALstatuscode=0x" << std::hex << context.slavelist[slave].ALstatuscode << std::dec;
    if (alstr) oss << " (" << alstr << ")";
    oss << "\n";

    // If slave already OP, done
    if ((st & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL) {
        oss << "Already OP\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return true;
    }

    // Try to map/processdata again for this slave (re-run state transitions)
    // First request SAFE_OP then OP
    context.slavelist[slave].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE);
    oss << "Requested SAFE_OP, new state=0x" << std::hex << context.slavelist[slave].state << std::dec << "\n";

    // If IO bits are zero, report and attempt to re-config map for this slave
    if (context.slavelist[slave].Obits == 0 && context.slavelist[slave].Ibits == 0) {
        oss << "Note: Outputs/Inputs bits are zero. Attempting esidump/read eeprom for diagnostics...\n";
        // perform esidump into buffer (non-destructive)
        const int BUFSZ = EC_MAXEEPBUF;
        std::vector<uint8_t> buf(BUFSZ);
        ecx_esidump(&context, slave, buf.data());
        // Release lock for heavy string formatting
        LeaveCriticalSection(&cs);
        std::ostringstream head;
        head << "EEPROM head: ";
        for (int i = 0; i < 32 && i < BUFSZ; ++i) {
            head << std::hex << std::setw(2) << std::setfill('0') << (int)buf[i] << " ";
        }
        head << std::dec << "\n";
        EnterCriticalSection(&cs);
        oss << head.str();
    }

    // Try to set OP at group level
    context.slavelist[0].state = EC_STATE_OPERATIONAL;
    ecx_send_processdata(&context);
    ecx_receive_processdata(&context, EC_TIMEOUTRET);
    ecx_writestate(&context, 0);

    // Wait briefly and check
    ecx_statecheck(&context, slave, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
    ecx_readstate(&context);
    int st2 = context.slavelist[slave].state;
    oss << "After OP request, slave state=0x" << std::hex << st2 << std::dec << "\n";

    bool ok = ((st2 & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL);
    if (!ok) {
        // Slave failed initial transition - attempt runtime PDO configuration
        oss << "Attempting runtime PDO configuration...\n";
        
        // Step 1: Reset to INIT to clear any error state
        context.slavelist[slave].state = EC_STATE_INIT;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE);
        
        // Step 2: Return to PRE-OP for SDO configuration
        context.slavelist[slave].state = EC_STATE_PRE_OP;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 5);
        
        // Step 3: Configure PDO mappings for cyclic motion mode
        uint8_t zero = 0, one = 1, pdoCount = 4;
        
        // Disable assignments temporarily
        WriteSDO(slave, 0x1C12, 0x00, &zero, sizeof(zero));
        WriteSDO(slave, 0x1C13, 0x00, &zero, sizeof(zero));
        WriteSDO(slave, 0x1600, 0x00, &zero, sizeof(zero));
        uint32_t rxMappings[] = {0x60400010, 0x607A0020, 0x60FF0020, 0x60710010};
        WriteSDO(slave, 0x1600, 0x00, &zero, sizeof(zero));
        for (int i = 0; i < pdoCount; ++i) {
            WriteSDO(slave, 0x1600, i+1, &rxMappings[i], sizeof(uint32_t));
        }
        WriteSDO(slave, 0x1600, 0x00, &pdoCount, sizeof(pdoCount));
        
        // Configure TxPDO 0x1A00 (Slave->Master)
        uint32_t txMappings[] = {0x60410010, 0x60640020, 0x606C0020, 0x60770010};
        WriteSDO(slave, 0x1A00, 0x00, &zero, sizeof(zero));
        for (int i = 0; i < pdoCount; ++i) {
            WriteSDO(slave, 0x1A00, i+1, &txMappings[i], sizeof(uint32_t));
        }
        WriteSDO(slave, 0x1A00, 0x00, &pdoCount, sizeof(pdoCount));
        
        // Assign PDOs to Sync Managers
        uint16_t rxPdoAssign = 0x1600, txPdoAssign = 0x1A00;
    WriteSDO(slave, 0x1C12, 0x01, &rxPdoAssign, sizeof(rxPdoAssign));
    WriteSDO(slave, 0x1C12, 0x00, &one, sizeof(one));
    WriteSDO(slave, 0x1C13, 0x01, &txPdoAssign, sizeof(txPdoAssign));
    WriteSDO(slave, 0x1C13, 0x00, &one, sizeof(one));
        
        // Step 4: Set operation mode to the default cyclic mode
        int8_t opMode = CoreHelpers::kDefaultCyclicOperationMode;
    WriteSDO(slave, 0x6060, 0x00, &opMode, sizeof(opMode));
        
        // Step 5: Configure EtherCAT cycle time (0x60C2:01 - Interpolation time period value)
        // Convert from microseconds to units expected by device (typically 0.1ms units)
        // For 2000us (2ms), we need 20 units of 0.1ms
        uint8_t cycleUnits = (uint8_t)((desiredCycleTime + 50) / 100); // Round to 0.1ms units
        if (cycleUnits == 0) cycleUnits = 20; // Default 2ms = 20 * 0.1ms
        int8_t cycleIndex = -3; // Unit: 10^-3 seconds (1ms)
        oss << "Configuring cycle time: " << (int)cycleUnits << " * 0.1ms = " 
            << (cycleUnits * 0.1) << "ms\n";
    WriteSDO(slave, 0x60C2, 0x01, &cycleUnits, sizeof(cycleUnits));
    WriteSDO(slave, 0x60C2, 0x02, &cycleIndex, sizeof(cycleIndex));
        
        oss << "PDO configuration complete. Retrying state transitions...\n";
        
        // Step 5: Attempt state transitions with new configuration
        context.slavelist[slave].state = EC_STATE_SAFE_OP;
        ecx_writestate(&context, slave);
        ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 3);
        
        context.slavelist[0].state = EC_STATE_OPERATIONAL;
        ecx_send_processdata(&context);
        ecx_receive_processdata(&context, EC_TIMEOUTRET);
        ecx_writestate(&context, 0);
        ecx_statecheck(&context, slave, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE);
        
        ecx_readstate(&context);
        st2 = context.slavelist[slave].state;
        ok = ((st2 & EC_STATE_OPERATIONAL) == EC_STATE_OPERATIONAL);
        
        if (ok) {
            oss << "Runtime configuration successful - slave reached OP\n";
            outLog = oss.str();
            LeaveCriticalSection(&cs);
            return ok;
        }
        
        oss << "Runtime configuration failed. Running diagnostics...\n";
        uint32_t vendor = 0;
        int sz = sizeof(vendor);
    int wkc = 0;
    int tmpSz = (int)sz;
    if (!ReadSDO(slave, 0x1018, 0x00, &vendor, tmpSz)) wkc = -1; else wkc = 1;
        if (wkc > 0) {
            oss << "Read SDO 0x1018:vendor=0x" << std::hex << vendor << std::dec << "\n";
        } else {
            oss << "SDO 0x1018 read failed or not present\n";
        }
        
        // Read PDO assignment 0x1C12 (RxPDO) and 0x1C13 (TxPDO) for additional diagnostics
        oss << "Reading PDO assignment (0x1C12 RxPDO, 0x1C13 TxPDO)...\n";
        // helper lambda to read assignment
        auto readAssignment = [&](uint16_t idx, const char* name){
            uint8_t count8 = 0;
            int szz = sizeof(count8);
            int w = 0;
            int tmpSzz = szz;
            if (!ReadSDO(slave, idx, 0x00, &count8, tmpSzz)) w = -1; else w = 1;
            if (w > 0) {
                oss << name << " count=" << (int)count8 << "\n";
                for (uint8_t i = 1; i <= count8; ++i) {
                    uint32_t entry = 0;
                    int szz2 = sizeof(entry);
                    int w2 = 0;
                    int tmpSzz2 = szz2;
                    if (!ReadSDO(slave, idx, i, &entry, tmpSzz2)) w2 = -1; else w2 = 1;
                    if (w2 > 0) {
                        oss << "  " << name << "[" << (int)i << "]=0x" << std::hex << entry << std::dec << "\n";
                    } else {
                        oss << "  " << name << "[" << (int)i << "]=<read fail>\n";
                    }
                }
            } else {
                oss << name << " not readable or absent\n";
            }
        };

        readAssignment(0x1C12, "RxPDO(0x1C12)");
        readAssignment(0x1C13, "TxPDO(0x1C13)");

        // Exhaustive read: iterate 0x1600..0x160F (RxPDO areas) and 0x1A00..0x1A0F (TxPDO areas)
        auto readFullPDORange = [&](uint16_t baseIdx, const char* nameBase){
            for (uint16_t off = 0; off < 0x10; ++off) {
                uint16_t idx = baseIdx + off;
                char nm[32];
                sprintf_s(nm, sizeof(nm), "%s(0x%04X)", nameBase, idx);
                uint8_t cnt = 0;
                int szz = sizeof(cnt);
                int w = 0;
                int tmpSzz = szz;
                if (!ReadSDO(slave, idx, 0x00, &cnt, tmpSzz)) w = -1; else w = 1;
                if (w > 0 && cnt > 0) {
                    oss << nm << " count=" << (int)cnt << "\n";
                    for (uint8_t si = 1; si <= cnt; ++si) {
                        uint32_t map = 0;
                        int szz2 = sizeof(map);
                        int w2 = 0;
                        int tmpSzz2 = szz2;
                        if (!ReadSDO(slave, idx, si, &map, tmpSzz2)) w2 = -1; else w2 = 1;
                        if (w2 > 0) {
                            oss << "  [" << (int)si << "]=0x" << std::hex << map << std::dec << "\n";
                        } else {
                            oss << "  [" << (int)si << "]=<read fail>\n";
                        }
                    }
                } else if (w > 0) {
                    oss << nm << " count=0\n";
                } else {
                    // not present or not readable
                    // Do not spam if absent; show once per index
                    oss << nm << " not readable/absent\n";
                }
            }
        };

        readFullPDORange(0x1600, "RxPDO");
        readFullPDORange(0x1A00, "TxPDO");
    }

    outLog = oss.str();
    LeaveCriticalSection(&cs);
    return ok;
}


bool EtherCATMaster::ConfigureSlaveRuntime(uint16_t slave, std::string &outLog)
{
    if (simulationMode_.load(std::memory_order_relaxed)) {
        std::ostringstream oss;
        if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
            oss << "Invalid slave or not initialized\n";
            outLog = oss.str();
            return false;
        }
        context.slavelist[slave].state = EC_STATE_SAFE_OP;
        context.slavelist[0].state = EC_STATE_SAFE_OP;
        oss << "Simulation slave " << slave << " runtime PDO already configured\n";
        outLog = oss.str();
        return true;
    }

    // Configure slave runtime PDO but stay in SAFE_OP (don't transition to OP yet)
    // This allows all slaves to be configured first, then switched to OP together
    
    DWORD waitResult = TryEnterCriticalSection(&cs) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (waitResult == WAIT_TIMEOUT) {
        for (int retry = 0; retry < 50; ++retry) {
            Sleep(100);
            if (TryEnterCriticalSection(&cs)) {
                waitResult = WAIT_OBJECT_0;
                break;
            }
        }
    }
    
    if (waitResult == WAIT_TIMEOUT) {
        outLog = "ERROR: Timeout acquiring lock\n";
        return false;
    }
    
    outLog.clear();
    std::ostringstream oss;

    if (!isInitialized || slave == 0 || slave > (uint16_t)slaveCount) {
        oss << "Invalid slave or not initialized\n";
        outLog = oss.str();
        LeaveCriticalSection(&cs);
        return false;
    }

    // Read current state
    ecx_readstate(&context);
    int st = context.slavelist[slave].state;
    oss << "Slave " << slave << " current state=0x" << std::hex << st << std::dec << "\n";
    
    // If slave has error flag (bit 4), acknowledge and clear it
    if (st & 0x10) {
        oss << "Slave has error flag set - reading AL Status Code...\n";
        uint16_t alStatusCode = 0;
        // Read AL Status Code (0x0134) to get error details
        ecx_FPRD(&context.port, context.slavelist[slave].configadr, 
                 ECT_REG_ALSTATCODE, sizeof(alStatusCode), &alStatusCode, EC_TIMEOUTRET);
        oss << "AL Status Code: 0x" << std::hex << alStatusCode << std::dec << "\n";
        
        // Clear error by writing to AL Control register with error acknowledge bit
        uint16_t alControl = 0x0011;  // Error ACK (bit 4) + INIT state (0x01)
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 ECT_REG_ALCTL, sizeof(alControl), &alControl, EC_TIMEOUTRET);
        oss << "Sent error acknowledge to AL Control\n";
        
    // Wait a bit for error to clear (release lock to avoid long hold)
    LeaveCriticalSection(&cs);
    osal_usleep(10000);
    EnterCriticalSection(&cs);
    }

    // Reset to INIT to clear any error state
    context.slavelist[slave].state = EC_STATE_INIT;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE * 2);
    
    // Verify INIT state was reached
    ecx_readstate(&context);
    if ((context.slavelist[slave].state & 0x0F) != EC_STATE_INIT) {
        oss << "WARNING: Failed to reach INIT state, current=0x" 
            << std::hex << context.slavelist[slave].state << std::dec << "\n";
        // If still in error, try one more time with explicit error clear
        if (context.slavelist[slave].state & 0x10) {
            uint16_t alControl = 0x0011;
            ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                     ECT_REG_ALCTL, sizeof(alControl), &alControl, EC_TIMEOUTRET);
            // Wait for error clear to propagate (release lock during wait)
            LeaveCriticalSection(&cs);
            osal_usleep(20000);
            EnterCriticalSection(&cs);
            context.slavelist[slave].state = EC_STATE_INIT;
            ecx_writestate(&context, slave);
            ecx_statecheck(&context, slave, EC_STATE_INIT, EC_TIMEOUTSTATE * 3);
        }
    }
    
    // Clear all FMMU and SM configurations to ensure clean state
    oss << "Clearing FMMU and SM configurations...\n";
    uint8_t zeroBlock[16] = {0};
    // Clear FMMU 0-3 (each FMMU is 16 bytes at 0x0600, 0x0610, 0x0620, 0x0630)
    for (int i = 0; i < 4; i++) {
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 0x0600 + (i * 0x10), 16, zeroBlock, EC_TIMEOUTRET);
    }
    // Clear SM 0-7 (each SM is 8 bytes at 0x0800, 0x0808, etc.)
    for (int i = 0; i < 8; i++) {
        ecx_FPWR(&context.port, context.slavelist[slave].configadr,
                 0x0800 + (i * 0x08), 8, zeroBlock, EC_TIMEOUTRET);
    }
    // Give slave time to settle after clearing FMMU/SM (release lock during wait)
    LeaveCriticalSection(&cs);
    osal_usleep(10000);
    EnterCriticalSection(&cs);
    
    // Move to PRE-OP (SOEM will use slave's EEPROM configuration)
    oss << "Moving to PRE-OP (using EEPROM defaults)...\n";
    context.slavelist[slave].state = EC_STATE_PRE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_PRE_OP, EC_TIMEOUTSTATE * 3);
    
    // Only set operation mode to the default cyclic mode - don't modify PDO mappings
    int8_t opMode = CoreHelpers::kDefaultCyclicOperationMode;
    bool sdoOk = WriteSDO(slave, 0x6060, 0x00, &opMode, sizeof(opMode));
    if (sdoOk) {
        oss << "Set operation mode to "
            << CoreHelpers::DescribeOperationModeShortName(opMode)
            << " (" << static_cast<int>(opMode) << ")\n";
    } else {
        oss << "WARNING: Failed to set operation mode\n";
    }
    
    // Move to SAFE_OP (but don't go to OP yet)
    context.slavelist[slave].state = EC_STATE_SAFE_OP;
    ecx_writestate(&context, slave);
    ecx_statecheck(&context, slave, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 3);
    
    ecx_readstate(&context);
    int finalState = context.slavelist[slave].state;
    bool ok = ((finalState & EC_STATE_SAFE_OP) == EC_STATE_SAFE_OP);
    
    if (ok) {
        oss << "Runtime configuration OK - slave at SAFE_OP (state=0x" << std::hex << finalState << std::dec << ")\n";
    } else {
        oss << "Configuration failed - final state=0x" << std::hex << finalState << std::dec << "\n";
    }
    
    outLog = oss.str();
    LeaveCriticalSection(&cs);
    return ok;
}


bool EtherCATMaster::StartSdoReader(unsigned int periodMs, const std::function<void(const std::string&)> &logCb)
{
    if (sdoReaderRunning.load()) return true;
    // Enforce configured fixed period (force to 100ms for product constraints)
    const unsigned int FIXED_SDO_PERIOD_MS = 100;
    if (periodMs != FIXED_SDO_PERIOD_MS) {
        if (logCb) {
            std::ostringstream oss; oss << "SDO reader period forced to " << FIXED_SDO_PERIOD_MS << "ms (requested " << periodMs << "ms)";
            logCb(oss.str());
        }
    }
    periodMs = FIXED_SDO_PERIOD_MS;
    sdoLogCb = logCb;
    sdoReaderPeriodMs.store(periodMs);
    sdoReaderStop.store(false);
    sdoReaderRunning.store(true);
    // Force first read to be treated as changed so initial state is logged
    {
        std::lock_guard<std::mutex> lk(sdoReaderMutex);
        sdoLastVal.clear();
    }
    sdoReaderLastRunMs.store(0);
    // Note: we no longer spawn a dedicated SDO reader thread here. Instead,
    // a caller may invoke SdoReaderTick() periodically (e.g. from a shared
    // error-check thread) so SDO reads can be consolidated with other checks.
    return true;
}


void EtherCATMaster::StopSdoReader()
{
    if (!sdoReaderRunning.load()) return;
    sdoReaderStop.store(true);
    sdoReaderCv.notify_all();
    // If an internal thread was previously created, join it. In typical
    // consolidated-mode we don't spawn a dedicated thread so join is no-op.
    if (sdoReaderThread.joinable()) sdoReaderThread.join();
    sdoReaderRunning.store(false);
}

// Perform one periodic tick for SDO reader; will actually perform a read only
// when the configured period elapsed since last real read. Safe to call from
// another thread (e.g. error-check thread) at higher frequency.

void EtherCATMaster::SdoReaderTick()
{
    if (!sdoReaderRunning.load() || sdoReaderStop.load()) return;
    if (!sdoReaderEnabled.load()) return;

    using clock = std::chrono::steady_clock;
    uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
    uint64_t last = sdoReaderLastRunMs.load();
    unsigned int period = sdoReaderPeriodMs.load();
    if (period == 0) period = 100;
    if (last != 0 && nowMs - last < (uint64_t)period) return; // not yet time

    // Update last-run timestamp early to avoid concurrent reads
    sdoReaderLastRunMs.store(nowMs);
    sdoReaderBusy.store(true);
    auto t0 = clock::now();

    uint8_t buf[32] = {0}; int len = 8;
    bool ok = ReadSDO(1, 0x60FD, 0x00, buf, len);
    if (ok) {
        std::vector<uint8_t> cur(buf, buf + len);
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(sdoReaderMutex);
            if (sdoLastVal.empty() || sdoLastVal != cur) changed = true;
            if (changed) sdoLastVal = std::move(cur);
        }
        if (changed) {
            std::ostringstream oss; oss << std::hex << std::uppercase;
            for (int i = len - 1; i >= 0; --i) {
                if (i != len - 1) oss << ' ';
                oss << "0x" << std::setw(2) << std::setfill('0') << (int)buf[i];
            }
            std::string hexStr = oss.str();
            std::ostringstream bitsoss;
            for (int i = len - 1; i >= 0; --i) {
                if (i != len - 1) bitsoss << ' ';
                bitsoss << std::bitset<8>(buf[i]).to_string();
            }
            std::string bitsStr = bitsoss.str();
            if (sdoLogCb) sdoLogCb(std::string("0x60FD changed -> ") + hexStr + std::string(" ; bits -> ") + bitsStr);
        }
    } else {
        if (sdoLogCb) sdoLogCb(std::string("Read 0x60FD failed: ") + GetLastError());
    }

    auto t1 = clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    sdoLastObservedCycleMs.store(elapsedMs);
    sdoReaderBusy.store(false);
}

// Thread-safe accessor for last-read SDO 0x60FD value

bool EtherCATMaster::GetLastSdo60FD(std::vector<uint8_t>& out)
{
    // Return bytes in the same visual order as logs (MSB .. LSB)
    std::lock_guard<std::mutex> lk(sdoReaderMutex);
    if (sdoLastVal.empty()) return false;
    out.assign(sdoLastVal.rbegin(), sdoLastVal.rend());
    return true;
}

bool EtherCATMaster::GetLastSdo60FDLowByte(uint8_t& outLowByte)
{
    std::lock_guard<std::mutex> lk(sdoReaderMutex);
    if (sdoLastVal.empty()) return false;
    // sdoLastVal keeps the raw mailbox order; byte 0 is the low/control byte used by AutoWindow.
    outLowByte = sdoLastVal.front();
    return true;
}

// ------------------------------------------------------------
// MMF1_IN control (wraps MMFReader member)
// ------------------------------------------------------------


