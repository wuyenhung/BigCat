#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct EsiObjectEntry {
    uint16_t index; // 0x1234
    uint8_t subindex; // 0x01
    std::string name; // "StatusWord"
    std::string description;
    int bitlen; // size in bits
    std::string dataType; // textual DataType from ESI (eg. "DWORD", "BYTE")
    std::string access; // access string from ESI (eg. "rw", "ro", "wo")
    std::string accessNormalized; // normalized access token used for internal checks (eg. "rw", "ro", "wo")
    std::string defaultValue; // default value text from ESI (if any)
};

struct EsiPdoEntry {
    std::string pdoType; // "RxPdo" or "TxPdo"
    std::string name; // PDO name
    uint16_t index; // PDO index
    std::vector<EsiObjectEntry> entries; // member entries
};

// ---------------------------------------------------------------------------
// ESI-driven PDO / SM configuration structures
// ---------------------------------------------------------------------------

/// Device identity extracted from ESI header
struct EsiDeviceIdentity {
    uint32_t vendorId = 0;
    uint32_t productCode = 0;
    uint32_t revision = 0;
    bool valid = false;
};

/// Single PDO mapping entry (index, subindex, bitlen) for SM-level PDO
struct EsiSmPdoEntry {
    uint16_t index = 0;
    uint8_t  subindex = 0;
    int      bitlen = 0;
};

/// One RxPdo or TxPdo definition from the <Sm> section
struct EsiSmPdoDef {
    uint16_t index = 0;           // e.g. 0x1600
    std::string pdoType;          // "RxPdo" or "TxPdo"
    bool fixed = false;           // Fixed="1" or "0"
    int  smIndex = -1;            // Sm="2" / "3", -1 if not specified
    std::vector<EsiSmPdoEntry> entries;
};

/// SyncManager info from <Sm> element
struct EsiSmInfo {
    int  index = -1;              // SM number (0-based)
    uint16_t defaultSize = 0;
    uint16_t startAddress = 0;
    uint8_t  controlByte = 0;
    bool     enable = false;
};

/// Complete SM + PDO descriptor loaded from an ESI's <Sm> section
struct EsiSmDescriptor {
    EsiDeviceIdentity identity;
    std::string deviceName;           // <Name LcId="1033"> from ESI Device
    std::vector<EsiSmInfo> smList;
    std::vector<EsiSmPdoDef> rxPdos;
    std::vector<EsiSmPdoDef> txPdos;
};

class EsiParser {
public:
    // Load ESI file. Returns true on success.
    static bool Load(const std::string& path,
                     std::vector<EsiObjectEntry>& objects,
                     std::vector<EsiPdoEntry>& pdos,
                     std::string& err);

    // Quick-load only the device identity (vendor / product / revision) from ESI header.
    static bool LoadDeviceIdentity(const std::string& path, EsiDeviceIdentity& id, std::string& err);

    // Load SM sizes and PDO definitions from the <Sm> section of an ESI file.
    // This reads the actual hardware PDOs used by SOEM for process-data mapping.
    static bool LoadSmDescriptor(const std::string& path, EsiSmDescriptor& desc, std::string& err);

    // Normalize a raw DataType string from an ESI file to a canonical form
    // Examples: "DWORD" -> "DWORD", "uint32" -> "DWORD", "boolean" -> "BOOL"
    static std::string NormalizeDataType(const std::string& raw);
    // Normalize access flags (e.g. "Read, Write" -> "rw", "ReadOnly" -> "ro")
    static std::string NormalizeAccess(const std::string& raw);
};
