#pragma once

#include <windows.h>
#include <string>
#include <atomic>
#include <mutex>
#include <cstring>

// MMFWriter: 輕量級 Memory-Mapped File 寫入器
// 用於 EtherCAT 即時執行緒將 PDO 資料輸出給外部可視化軟體
// 設計目標：零鎖、零動態記憶體配置、最小延遲
class MMFWriter {
public:
    // 每個從軸的 PDO 資料（6個 double）
    // 每個從軸的 PDO 資料（擴充以匹配 PDO.log）
    struct SlaveData {
        double targetPosition;   // 目標位置 (角度或機械單位)
        double targetVelocity;   // 目標速度；CSV 模式為 0x60FF 命令速度
        double actualPosition;   // 實際位置
        double actualVelocity;   // 實際速度
        double pdoFilterOutput;  // PDO 濾波器輸出
        double pdoFilterVel;     // PDO 濾波器速度
        double pdoFilterAcc;     // PDO 濾波器加速度
        float mmfIn;             // MMF 輸入值
        uint16_t controlWord;    // 控制字
        uint16_t statusWord;     // 狀態字
        // 保留原有的加速度欄位（雖然 PDO.log 無此欄位，但可視化可能需要）
        double targetAcceleration; 
        double actualAcceleration; 
    };

    // 完整資料結構：最多支援 6 軸 + 標頭
    struct MMFData {
        uint32_t magic;          // 魔術數字 0x57554D46 ("WUMF") 用於驗證
        uint32_t version;        // 版本號 (更新為 2)
        uint64_t sequenceNumber; // 序列號，每次寫入遞增
        uint64_t timestamp;      // 時間戳記 (QueryPerformanceCounter)
        uint64_t epochMs;        // 系統時間 (Epoch MS) - 新增以匹配 log
        uint32_t slaveCount;     // 有效從軸數量 (1-6)
        uint32_t reserved;       // 保留位元組對齊
        SlaveData slaves[6];     // 6 個從軸資料
    };

    static constexpr uint32_t MAGIC = 0x57554D46; // "WUMF"
    static constexpr uint32_t VERSION = 2;
    static constexpr size_t MMF_SIZE = sizeof(MMFData);

    MMFWriter();
    ~MMFWriter();

    // 建立或開啟 MMF（mappingName: 例如 L"Local\\WuEtherCAT_PDO"）
    // 返回 true 表示成功
    bool Create(const std::wstring& mappingName);
    
    // 關閉 MMF
    void Close();
    
    // 寫入單個從軸資料（slaveIndex: 1-based，範圍 1-6）
    // 現在使用短期鎖以防止在 Close() 與 Create() 之間產生競態
    void WriteSlave(int slaveIndex, double targetPos, double targetVel, 
                    double actualPos, double actualVel,
                    double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc,
                    float mmfIn,
                    uint16_t controlWord, uint16_t statusWord,
                    double targetAcc = 0.0, double actualAcc = 0.0);
    
    // 批次寫入所有從軸資料（更高效）
    // slaveCount: 有效從軸數量，slaveData: 陣列指標
    void WriteAll(int slaveCount, const SlaveData* slaveData);
    
    // 更新標頭（序列號、時間戳）- 在每次 cyclic task 結束時呼叫
    void UpdateHeader(uint64_t epochMs = 0);
    
    // 檢查是否已建立
    bool IsCreated() const { return view_ != nullptr; }

private:
    HANDLE hMapping_;
    MMFData* view_;  // mapped view 指標
    std::atomic<uint64_t> sequence_;
    LARGE_INTEGER perfFreq_;
    // Protects create/close and writers from racing with unmap
    std::mutex mtx_;
};
