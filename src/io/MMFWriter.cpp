#include "MMFWriter.h"
#include <cstring>
#include "SafeMem.h"

MMFWriter::MMFWriter()
    : hMapping_(NULL)
    , view_(nullptr)
    , sequence_(0)
{
    QueryPerformanceFrequency(&perfFreq_);
}

MMFWriter::~MMFWriter() {
    Close();
}

bool MMFWriter::Create(const std::wstring& mappingName) {
    std::lock_guard<std::mutex> lk(mtx_);
    // Inline cleanup to avoid recursive lock on mtx_ (Close() also acquires mtx_).
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (hMapping_) {
        CloseHandle(hMapping_);
        hMapping_ = NULL;
    }

    // 建立或開啟 file mapping object
    hMapping_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,    // 使用系統分頁檔
        NULL,                     // 預設安全描述符
        PAGE_READWRITE,          // 讀寫權限
        0,                        // 高位元組大小
        static_cast<DWORD>(MMF_SIZE), // 低位元組大小
        mappingName.c_str()      // 命名
    );

    if (!hMapping_) {
        return false;
    }

    // 映射 view
    view_ = static_cast<MMFData*>(MapViewOfFile(
        hMapping_,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        MMF_SIZE
    ));

    if (!view_) {
        CloseHandle(hMapping_);
        hMapping_ = NULL;
        return false;
    }

    // 初始化標頭
    std::memset(view_, 0, MMF_SIZE);
    view_->magic = MAGIC;
    view_->version = VERSION;
    view_->sequenceNumber = 0;
    view_->slaveCount = 0;
    sequence_.store(0, std::memory_order_relaxed);

    return true;
}

void MMFWriter::Close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (view_) {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }
    if (hMapping_) {
        CloseHandle(hMapping_);
        hMapping_ = NULL;
    }
}

void MMFWriter::WriteSlave(int slaveIndex, double targetPos, double targetVel,
                            double actualPos, double actualVel,
                            double pdoFilterOutput, double pdoFilterVel, double pdoFilterAcc,
                            float mmfIn,
                            uint16_t controlWord, uint16_t statusWord,
                            double targetAcc, double actualAcc) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!view_) return;
    if (slaveIndex < 1 || slaveIndex > 6) return;

    int idx = slaveIndex - 1; // 轉為 0-based
    view_->slaves[idx].targetPosition = targetPos;
    view_->slaves[idx].targetVelocity = targetVel;
    view_->slaves[idx].actualPosition = actualPos;
    view_->slaves[idx].actualVelocity = actualVel;
    view_->slaves[idx].pdoFilterOutput = pdoFilterOutput;
    view_->slaves[idx].pdoFilterVel = pdoFilterVel;
    view_->slaves[idx].pdoFilterAcc = pdoFilterAcc;
    view_->slaves[idx].mmfIn = mmfIn;
    view_->slaves[idx].controlWord = controlWord;
    view_->slaves[idx].statusWord = statusWord;
    view_->slaves[idx].targetAcceleration = targetAcc;
    view_->slaves[idx].actualAcceleration = actualAcc;
    
    // 更新 slaveCount（如果需要擴展）
    if (static_cast<uint32_t>(slaveIndex) > view_->slaveCount) {
        view_->slaveCount = static_cast<uint32_t>(slaveIndex);
    }
}

void MMFWriter::WriteAll(int slaveCount, const SlaveData* slaveData) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!view_ || !slaveData) return;
    if (slaveCount < 0 || slaveCount > 6) return;

    // 批次複製（更高效）
    if (!safe_memcpy(view_->slaves, slaveData, sizeof(SlaveData) * slaveCount)) {
        return;
    }
    view_->slaveCount = static_cast<uint32_t>(slaveCount);
}

void MMFWriter::UpdateHeader(uint64_t epochMs) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!view_) return;

    // 遞增序列號
    uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    view_->sequenceNumber = seq;

    // 更新時間戳
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    view_->timestamp = static_cast<uint64_t>(now.QuadPart);
    view_->epochMs = epochMs;
}
