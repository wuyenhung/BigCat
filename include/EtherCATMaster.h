#ifndef ETHERCAT_MASTER_H
#define ETHERCAT_MASTER_H

// 必須按照特定順序引入標頭檔，避免衝突
#define WIN32_LEAN_AND_MEAN
// HAVE_REMOTE and WPCAP are defined via CMake compile_definitions; avoid redefining them here
#define BPF_PROGRAM_DEFINED  // 防止 Packet32.h 重複定義 BPF 結構
#define BPF_INSN_DEFINED
#include <winsock2.h>
#include <windows.h>

#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <unordered_map>

// 在 Windows 標頭檔之後引入 SOEM
#include "soem/soem.h"

// EtherCAT 從站資訊結構
struct SlaveInfo {
    int position;
    std::string name;
    uint16_t aliasAddress;
    uint32_t vendorId;
    uint32_t productCode;
    uint16_t state;
    int outputBits;
    int inputBits;
};

// EtherCAT 主站類別
class EtherCATMaster {
public:
    EtherCATMaster();
    ~EtherCATMaster();

    // 初始化和關閉
    bool Initialize(const char* ifname);
    void Shutdown();
    void Shutdown(std::function<void(const std::string&)> logCallback);
    
    // 狀態管理
    bool ConfigureSlaves();
    bool StartCyclicOperation();
    void StopCyclicOperation();
    
    // PDO 操作
    bool ReadProcessData();
    bool WriteProcessData();
    
    // SDO 讀寫
    bool ReadSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                 void* data, int &size);
    bool WriteSDO(uint16_t slave, uint16_t index, uint8_t subindex, 
                  void* data, int size);
    
    // Mailbox 操作
    bool SendMailbox(uint16_t slave, uint8_t* data, int size);
    bool ReceiveMailbox(uint16_t slave, uint8_t* data, int* size);
    
    // 狀態查詢
    int GetSlaveCount() const { return slaveCount; }
    bool IsRunning() const { return isRunning; }
    bool IsInitialized() const { return isInitialized; }
    bool HasActiveSlaveThreads() const;
    std::vector<SlaveInfo> GetSlaveInfo();
    std::string GetLastError() const { return lastError; }
    
    // 週期任務
    void SetCyclicCallback(std::function<void()> callback) { 
        cyclicCallback = callback; 
    }
    
    // 設定週期時間
    void SetCycleTime(uint32_t microseconds) { 
        desiredCycleTime = microseconds; 
    }
    
    // 診斷
    uint32_t GetCycleTime() const { return desiredCycleTime; }  // 返回期望的週期時間
    uint32_t GetActualCycleTime() const { return cycleTime; }   // 返回實際測量的週期時間
    uint32_t GetWorkingCounter() const { return workingCounter; }
    uint32_t GetCommsTime() const { return commsTime; }        // 返回通訊工作時間
    uint64_t GetPDOSentCount() const { return pdosSentCount; }
    
    // PDO數據訪問
    uint8_t* GetIOMap() { return IOmap; }
    const ecx_contextt* GetContext() const { return &context; }
    // Thread-safe helper to obtain byte-aligned IO base offsets for a slave
    // Returns true if offsets were filled; outBase/inBase are in bytes
    bool GetSlaveIOBaseBytes(int slaveIndex, int &outBase, int &inBase);
        // Thread-safe snapshot of IOmap (copies requested range) - returns number of bytes copied
        int SnapshotIOMap(int offset, uint8_t* outBuf, int len);
        // Thread-safe write to output bytes within IOmap. Returns true if queued/written successfully.
        bool WriteOutputBytes(int absOffset, const uint8_t* data, int len);

    // PDO 即時資料處理 (核心執行緒)
    void StartPDOProcessingThread();
    void StopPDOProcessingThread();
    bool GetPDODataSnapshot(int slaveIndex, std::vector<uint8_t>& snapshot);
    bool WritePDOData(int slaveIndex, const std::string& itemName, int32_t value);
    // Overwrite any existing pending PDO output for the given item name.
    bool WritePDODataOverwrite(int slaveIndex, const std::string& itemName, int32_t value);
    // Stage output bytes into a persistent buffer that the PDO processing thread
    // will copy into IOmap every cycle. This avoids direct IOmap writes from UI threads.
    bool StageOutputBytes(int absOffset, const uint8_t* data, int len);
    // Stage output bytes only if there is not already a staged entry at absOffset.
    // Returns true if the bytes were staged, false if an entry already existed.
    bool StageOutputIfNotExists(int absOffset, const uint8_t* data, int len);

    // Check whether a name-based pending PDO output exists (enqueued via WritePDOData)
    bool HasPendingPDOOutput(const std::string& itemName);
    // Dump ESI / EEPROM for a specific slave. Returns dump as UTF-8 string.
    bool DumpESI(uint16_t slave, std::string &outDump);
    // Try runtime initialization for a slave (SDO/runtime steps) to reach OP without writing EEPROM.
    // Returns true if slave reached OP.
    bool ForceSlaveOperational(uint16_t slave, std::string &outLog);
    // Configure slave runtime PDO but don't transition to OP (stays in SAFE_OP)
    bool ConfigureSlaveRuntime(uint16_t slave, std::string &outLog);
    // Safely transition all slaves from OP to INIT before disconnecting
    void StopAllSlaves();
    // Send a single processdata roundtrip (SOEM standard practice before requesting OP)
    void SendProcessDataOnce();

private:
    // 內部函數
    static DWORD WINAPI CyclicThreadFunc(LPVOID param);
    void CyclicTask();
    void CheckSlaveStates();
    void SetError(const std::string& error);
    void InitializePDOData();  // 初始化 PDO 輸出數據
    
    // Per-slave cyclic thread support
    struct SlaveThreadData {
        uint16_t slaveIndex;  // 1-based slave index
        EtherCATMaster* master;
        HANDLE thread;
        HANDLE stopEvent;
        bool isRunning;
    };
    static DWORD WINAPI PerSlaveCyclicThreadFunc(LPVOID param);
    void PerSlaveCyclicTask(uint16_t slaveIndex, HANDLE slaveStopEvent);
    bool StartSlaveThread(uint16_t slaveIndex);
    void StopSlaveThread(uint16_t slaveIndex);

private:
    // SOEM Context (新版 API)
    ecx_contextt context;
    
    char ifname[256];
    int slaveCount;
    bool isRunning;
    bool isInitialized;
    
    HANDLE cyclicThread;  // Legacy global cyclic thread (deprecated)
    HANDLE stopEvent;     // Legacy global stop event (deprecated)
    
    // Per-slave cyclic threads (new architecture)
    std::vector<SlaveThreadData> slaveThreads;
    
    uint32_t cycleTime;      // 微秒
    uint32_t desiredCycleTime; // 期望的週期時間（微秒）
    uint32_t workingCounter;
    uint32_t commsTime; // 通訊實際工作時間 (microseconds): send/receive + checks
    uint64_t pdosSentCount; // PDO 發送計數
    uint64_t operationStartTime; // 操作開始時間 (GetTickCount64())
    bool timePeriodActive; // whether timeBeginPeriod was requested
    
    std::string lastError;
    std::function<void()> cyclicCallback;
    
    // IO 映射緩衝區
    uint8_t IOmap[4096];
    
    // 執行緒同步
    CRITICAL_SECTION cs;
    
    // PDO 即時處理執行緒 (核心執行緒)
    HANDLE pdoProcessingThread;
    HANDLE pdoStopEvent;
    bool pdoThreadRunning;
    static DWORD WINAPI PDOProcessingThreadFunc(LPVOID param);
    void PDOProcessingTask();

    // 執行緒安全的 PDO 資料緩衝區
    CRITICAL_SECTION pdoDataCS;
    std::unordered_map<int, std::vector<uint8_t>> pdoDataSnapshots; // slaveIndex -> IO data snapshot
    std::unordered_map<std::string, std::pair<int, int32_t>> pendingPDOOutputs; // itemName -> (slaveIndex, value)
    // stagedOutputs: absolute IOmap offset -> data bytes that should be continuously applied by PDO task
    std::unordered_map<int, std::vector<uint8_t>> stagedOutputs;
};

#endif // ETHERCAT_MASTER_H
