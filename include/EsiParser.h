#pragma once

#include <string>
#include <vector>

struct EsiObjectEntry {
    uint16_t index; // 0x1234
    uint8_t subindex; // 0x01
    std::string name; // "StatusWord"
    std::string description;
    int bitlen; // size in bits
    std::string dataType; // textual DataType from ESI (eg. "DWORD", "BYTE")
    std::string access; // access string from ESI (eg. "rw", "ro", "wo")
};

struct EsiPdoEntry {
    std::string pdoType; // "RxPdo" or "TxPdo"
    std::string name; // PDO name
    uint16_t index; // PDO index
    std::vector<EsiObjectEntry> entries; // member entries
};

class EsiParser {
public:
    // Load ESI file. Returns true on success.
    static bool Load(const std::string& path,
                     std::vector<EsiObjectEntry>& objects,
                     std::vector<EsiPdoEntry>& pdos,
                     std::string& err);
    // Normalize a raw DataType string from an ESI file to a canonical form
    // Examples: "DWORD" -> "DWORD", "uint32" -> "DWORD", "boolean" -> "BOOL"
    static std::string NormalizeDataType(const std::string& raw);
    // Normalize access flags (e.g. "Read, Write" -> "rw", "ReadOnly" -> "ro")
    static std::string NormalizeAccess(const std::string& raw);
};
