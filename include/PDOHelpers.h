#ifndef PDO_HELPERS_H
#define PDO_HELPERS_H

#include <vector>
#include <cstdint>

// Forward declarations to avoid circular includes
class EtherCATMaster;
struct PDOItem;

// Read a PDOItem numeric value from a per-slave snapshot (outputs-first layout).
// master: EtherCATMaster used to obtain slave context if needed (may be nullptr).
// slaveIdx: 1-based slave index
// snapshot: per-slave snapshot produced by EtherCAT master (outputs then inputs)
// item: PDOItem describing index/offset/bitlen/isOutput
// outValue: filled with LE-parsed 32-bit value on success
// Returns true on success.
bool ReadPDOItemValueFromSnapshot(EtherCATMaster* master, int slaveIdx, const std::vector<uint8_t>& snapshot, const PDOItem& item, int32_t &outValue);

#endif // PDO_HELPERS_H
