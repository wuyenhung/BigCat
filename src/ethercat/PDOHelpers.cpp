#include "../include/PDOHelpers.h"
#include "EtherCATMaster.h"
#include "MainWindow.h" // for PDOItem definition
#include <unordered_map>

std::wstring GetPdoAddressDescription(uint16_t index, uint8_t subindex)
{
    // Well-known CiA402 / DS402 object dictionary entries with Chinese descriptions.
    // Subindex 0 is the most common; other subindices are less common but are included where meaningful.
    static const std::unordered_map<uint32_t, const wchar_t*> map = {
        // key = (index << 8) | subindex; subindex 0 is the default
        { 0x100000, L"裝置類型" },
        { 0x100100, L"錯誤暫存器" },
        { 0x100800, L"裝置名稱" },
        { 0x100900, L"硬體版本" },
        { 0x100A00, L"軟體版本" },
        { 0x101800, L"身分識別" },

        { 0x603F00, L"錯誤碼" },
        { 0x604000, L"控制字" },
        { 0x604100, L"狀態字" },
        { 0x605A00, L"快速停止選項" },
        { 0x605B00, L"關機選項" },
        { 0x605D00, L"停止選項" },
        { 0x606000, L"操作模式" },
        { 0x606100, L"操作模式顯示" },
        { 0x606200, L"位置需求值" },
        { 0x606300, L"實際位置(編碼器)" },
        { 0x606400, L"實際位置" },
        { 0x606500, L"跟隨誤差範圍" },
        { 0x606700, L"位置範圍" },
        { 0x606800, L"位置視窗" },
        { 0x606B00, L"速度需求值" },
        { 0x606C00, L"實際速度" },
        { 0x606D00, L"速度範圍" },
        { 0x607100, L"目標轉矩" },
        { 0x607200, L"最大轉矩" },
        { 0x607400, L"轉矩需求值" },
        { 0x607700, L"實際轉矩" },
        { 0x607800, L"實際電流" },
        { 0x607900, L"DC 鏈電壓" },
        { 0x607A00, L"目標位置" },
        { 0x607C00, L"原點偏移" },
        { 0x607D00, L"軟體位置下限" },
        { 0x607E00, L"軟體位置上限" },
        { 0x608100, L"輪廓速度" },
        { 0x608300, L"輪廓加速度" },
        { 0x608400, L"輪廓減速度" },
        { 0x608500, L"快速停止減速" },
        { 0x608600, L"運動輪廓類型" },
        { 0x609800, L"歸原模式" },
        { 0x609900, L"歸原速度(高速)" },
        { 0x609A00, L"歸原速度(低速)" },
        { 0x60B000, L"位置偏移" },
        { 0x60B100, L"速度偏移" },
        { 0x60B200, L"轉矩偏移" },
        { 0x60C100, L"內插資料紀錄" },
        { 0x60C200, L"內插時間週期" },
        { 0x60E000, L"正向轉矩限制" },
        { 0x60E100, L"負向轉矩限制" },
        { 0x60F400, L"跟隨誤差實際值" },
        { 0x60FC00, L"位置需求值(內部)" },
        { 0x60FD00, L"數位輸入" },
        { 0x60FE00, L"數位輸出" },
        { 0x60FF00, L"目標速度" },
        { 0x650200, L"支援驅動模式" },
    };

    uint32_t key = (static_cast<uint32_t>(index) << 8) | subindex;
    auto it = map.find(key);
    if (it != map.end()) return it->second;

    // If subindex != 0 not found, try subindex 0 as fallback
    if (subindex != 0) {
        key = (static_cast<uint32_t>(index) << 8) | 0;
        it = map.find(key);
        if (it != map.end()) return it->second;
    }

    return L"";
}

bool ReadPDOItemValueFromSnapshot(EtherCATMaster* master, int slaveIdx, const std::vector<uint8_t>& snapshot, const PDOItem& item, int32_t &outValue)
{
    outValue = 0;
    if (slaveIdx <= 0) return false;
    // Determine outputSize (number of output bytes) so inputs' offsets can be computed
    int outputSize = 0;
    if (master) {
        const ecx_contextt* ctx = master->GetContext();
        if (ctx && slaveIdx > 0 && slaveIdx <= ctx->slavecount) {
            outputSize = ctx->slavelist[slaveIdx].Obytes;
        }
    }
    if (outputSize == 0) {
        outputSize = (int)snapshot.size();
    }

    int bytes = (item.bitLength > 0) ? ((item.bitLength + 7) / 8) : 4;
    if (bytes > 4) bytes = 4; // cap to 32-bit reads

    int idxInSnap = item.isOutput ? item.byteOffset : (outputSize + item.byteOffset);
    if (idxInSnap < 0) return false;
    if (idxInSnap + bytes > (int)snapshot.size()) return false;

    uint32_t val = 0;
    for (int b = 0; b < bytes; ++b) val |= (uint32_t)snapshot[idxInSnap + b] << (8 * b);
    // Perform sign-extension for reads smaller than 32-bit so that
    // signed values (e.g., 16-bit, 24-bit) are interpreted correctly.
    if (bytes < 4) {
        const int bits = bytes * 8;
        const uint32_t sign_bit = 1u << (bits - 1);
        if (val & sign_bit) {
            // set upper bits to 1
            uint32_t mask = ~((1u << bits) - 1u);
            val |= mask;
        }
    }
    outValue = (int32_t)val;
    return true;
}
