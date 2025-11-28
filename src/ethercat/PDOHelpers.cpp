#include "../include/PDOHelpers.h"
#include "EtherCATMaster.h"
#include "MainWindow.h" // for PDOItem definition

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
