#include "EsiParser.h"
#include <tinyxml2.h>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <unordered_map>

using namespace tinyxml2;

static uint16_t ParseHexIndex(const char* text) {
    if (!text) return 0;
    // Accept formats like "0x1000", "#x1000", "#X1000", or decimal.
    const char* s = text;
    // skip whitespace
    while (*s && isspace((unsigned char)*s)) ++s;
    // skip leading '#'
    if (*s == '#') ++s;
    // if starts with x or X, skip it and parse as hex
    if (*s == 'x' || *s == 'X') {
        ++s;
        unsigned long v = strtoul(s, nullptr, 16);
        return static_cast<uint16_t>(v & 0xFFFF);
    }
    // try base 0 (handles 0x...), if result is 0 try explicit hex
    unsigned long v = strtoul(s, nullptr, 0);
    if (v == 0) {
        // try hex fallback
        v = strtoul(s, nullptr, 16);
    }
    return static_cast<uint16_t>(v & 0xFFFF);
}

bool EsiParser::Load(const std::string& path,
                     std::vector<EsiObjectEntry>& objects,
                     std::vector<EsiPdoEntry>& pdos,
                     std::string& err)
{
    objects.clear();
    pdos.clear();
    err.clear();

    XMLDocument doc;
    XMLError r = doc.LoadFile(path.c_str());
    if (r != XML_SUCCESS) {
        err = "Failed to load ESI: ";
        err += doc.ErrorStr();
        return false;
    }

    XMLElement* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) {
        err = "Not an EtherCAT ESI file: missing EtherCATInfo";
        return false;
    }

    // find Devices/Device/Profile/Dictionary/Objects
    XMLElement* el = root->FirstChildElement("Descriptions");
    if (!el) el = root;
    el = el->FirstChildElement("Devices");
    if (el) el = el->FirstChildElement("Device");
    if (el) el = el->FirstChildElement("Profile");
    if (el) el = el->FirstChildElement("Dictionary");
    if (el) el = el->FirstChildElement("Objects");

    if (el) {
        for (XMLElement* obj = el->FirstChildElement("Object"); obj; obj = obj->NextSiblingElement("Object")) {
            EsiObjectEntry entry;
            // Index may be provided as a child element <Index>0x1000</Index>
            // or sometimes as an attribute (Index="0x1000" or index="0x1000").
            const char* idxText = nullptr;
            XMLElement* idxEl = obj->FirstChildElement("Index");
            if (idxEl && idxEl->GetText()) idxText = idxEl->GetText();
            if (!idxText) {
                // try attributes
                const char* a = obj->Attribute("Index");
                if (!a) a = obj->Attribute("index");
                if (!a) a = obj->Attribute("value");
                idxText = a;
            }
            if (!idxText) continue;
            entry.index = ParseHexIndex(idxText);
            const char* name = obj->FirstChildElement("Name") ? obj->FirstChildElement("Name")->GetText() : nullptr;
            const char* desc = obj->FirstChildElement("Description") ? obj->FirstChildElement("Description")->GetText() : nullptr;
            entry.subindex = 0; // default; subindices are in "Subindex" children normally
            entry.name = name ? name : "";
            entry.description = desc ? desc : "";
            entry.dataType = "";
            entry.access = "";
            
            // Read DataType/Type and Access from Object level (for objects without Subindex)
            XMLElement* objTypeEl = obj->FirstChildElement("Type");
            if (!objTypeEl) objTypeEl = obj->FirstChildElement("DataType");
            if (objTypeEl && objTypeEl->GetText()) {
                std::string dt = EsiParser::NormalizeDataType(objTypeEl->GetText());
                entry.dataType = dt;
                // attempt to deduce bitlen from normalized token
                if (dt == "BOOL") entry.bitlen = 1;
                else if (dt == "BYTE") entry.bitlen = 8;
                else if (dt == "WORD") entry.bitlen = 16;
                else if (dt == "DWORD") entry.bitlen = 32;
                else entry.bitlen = 0;
            }
            // If bitlen still unknown, try BitSize / BitLength at Object level
            if (entry.bitlen == 0) {
                XMLElement* bs = obj->FirstChildElement("BitSize");
                if (!bs) bs = obj->FirstChildElement("BitLength");
                if (bs && bs->GetText()) entry.bitlen = atoi(bs->GetText());
            }
            // try to read access at Object level: either direct <Access> or under <Flags><Access>
            XMLElement* objAccEl = obj->FirstChildElement("Access");
            if (!objAccEl) {
                XMLElement* objFlags = obj->FirstChildElement("Flags");
                if (objFlags) objAccEl = objFlags->FirstChildElement("Access");
            }
            if (objAccEl && objAccEl->GetText()) entry.access = EsiParser::NormalizeAccess(objAccEl->GetText());
            
            // Try to find default subindices definitions
            XMLElement* subEl = obj->FirstChildElement("Subindex");
            if (subEl) {
                const char* subIdx = nullptr;
                XMLElement* subIdxEl = subEl->FirstChildElement("Index");
                if (subIdxEl && subIdxEl->GetText()) subIdx = subIdxEl->GetText();
                if (!subIdx) {
                    const char* a = subEl->Attribute("Index");
                    if (!a) a = subEl->Attribute("index");
                    if (!a) a = subEl->Attribute("value");
                    subIdx = a;
                }
                if (subIdx) entry.subindex = static_cast<uint8_t>(strtoul(subIdx, nullptr, 0));
                // Data type may be specified as <DataType> or <Type>; bit width may be under <BitSize> or <BitLength>
                XMLElement* dataTypeEl = subEl->FirstChildElement("DataType");
                if (!dataTypeEl) dataTypeEl = subEl->FirstChildElement("Type");
                if (dataTypeEl && dataTypeEl->GetText()) {
                    std::string dt = EsiParser::NormalizeDataType(dataTypeEl->GetText());
                    entry.dataType = dt;
                    // attempt to deduce bitlen from normalized token or from BitSize/BitLength children
                    if (dt == "BOOL") entry.bitlen = 1;
                    else if (dt == "BYTE") entry.bitlen = 8;
                    else if (dt == "WORD") entry.bitlen = 16;
                    else if (dt == "DWORD") entry.bitlen = 32;
                    else entry.bitlen = 0;
                }
                // If bitlen still unknown, try BitSize / BitLength elements
                if (entry.bitlen == 0) {
                    XMLElement* bs = subEl->FirstChildElement("BitSize");
                    if (!bs) bs = subEl->FirstChildElement("BitLength");
                    if (bs && bs->GetText()) entry.bitlen = atoi(bs->GetText());
                }
                // try to read access: either a direct <Access> child or under <Flags><Access>
                XMLElement* accEl = subEl->FirstChildElement("Access");
                if (!accEl) {
                    XMLElement* flags = subEl->FirstChildElement("Flags");
                    if (flags) accEl = flags->FirstChildElement("Access");
                }
                if (accEl && accEl->GetText()) entry.access = EsiParser::NormalizeAccess(accEl->GetText());
            }
            
            // Only add the main entry if there are no Subindex children, or if it represents subindex 0
            bool hasSubindexChildren = (obj->FirstChildElement("Subindex") != nullptr);
            if (!hasSubindexChildren) {
                objects.push_back(entry);
            }

            // parse Subindexes if multiple
            for (XMLElement* s = obj->FirstChildElement("Subindex"); s; s = s->NextSiblingElement("Subindex")) {
                EsiObjectEntry sentry = entry;
                const char* subIdx = nullptr;
                XMLElement* subIdxEl2 = s->FirstChildElement("Index");
                if (subIdxEl2 && subIdxEl2->GetText()) subIdx = subIdxEl2->GetText();
                if (!subIdx) {
                    const char* a = s->Attribute("Index");
                    if (!a) a = s->Attribute("index");
                    if (!a) a = s->Attribute("value");
                    subIdx = a;
                }
                if (subIdx) sentry.subindex = static_cast<uint8_t>(strtoul(subIdx, nullptr, 0));
                const char* sname = s->FirstChildElement("Name") ? s->FirstChildElement("Name")->GetText() : nullptr;
                if (sname) sentry.name = sname;
                XMLElement* dType = s->FirstChildElement("DataType");
                if (!dType) dType = s->FirstChildElement("Type");
                if (dType && dType->GetText()) {
                    std::string dt = EsiParser::NormalizeDataType(dType->GetText());
                    sentry.dataType = dt;
                    if (dt == "BOOL") sentry.bitlen = 1;
                    else if (dt == "BYTE") sentry.bitlen = 8;
                    else if (dt == "WORD") sentry.bitlen = 16;
                    else if (dt == "DWORD") sentry.bitlen = 32;
                }
                if (sentry.bitlen == 0) {
                    XMLElement* bs2 = s->FirstChildElement("BitSize");
                    if (!bs2) bs2 = s->FirstChildElement("BitLength");
                    if (bs2 && bs2->GetText()) sentry.bitlen = atoi(bs2->GetText());
                }
                // Access may be present as <Access> or under <Flags>
                XMLElement* accEl2 = s->FirstChildElement("Access");
                if (!accEl2) {
                    XMLElement* flags2 = s->FirstChildElement("Flags");
                    if (flags2) accEl2 = flags2->FirstChildElement("Access");
                }
                if (accEl2 && accEl2->GetText()) sentry.access = EsiParser::NormalizeAccess(accEl2->GetText());
                objects.push_back(sentry);
            }
        }

        // Build a lookup map of index/subindex -> object properties so that other
        // parts of the code (PDO entries, SDO table viewers) can be filled using
        // the ESI-provided types and access flags when they were not present
        // directly on the element.
        {
            std::unordered_map<uint32_t, EsiObjectEntry> lookup;
            for (const auto &o : objects) {
                uint32_t key = (static_cast<uint32_t>(o.index) << 16) | static_cast<uint32_t>(o.subindex);
                // prefer first occurrence
                if (lookup.find(key) == lookup.end()) lookup.emplace(key, o);
                // also store default (subindex 0) for this index if present
                if (o.subindex == 0) {
                    uint32_t defkey = (static_cast<uint32_t>(o.index) << 16) | 0u;
                    lookup[defkey] = o;
                }
            }
            // Fill missing fields on objects from default index entry if available
            for (auto &o : objects) {
                if (o.dataType.empty() || o.access.empty() || o.bitlen == 0) {
                    uint32_t defkey = (static_cast<uint32_t>(o.index) << 16) | 0u;
                    auto it = lookup.find(defkey);
                    if (it != lookup.end()) {
                        const EsiObjectEntry &def = it->second;
                        if (o.dataType.empty()) o.dataType = def.dataType;
                        if (o.access.empty()) o.access = def.access;
                        if (o.bitlen == 0) o.bitlen = def.bitlen;
                    }
                }
            }
        }
    }

    // Parse PDOs: search for RxPdo/TxPdo under Profile/TxPdoList or similar
    XMLElement* prof = root->FirstChildElement("Descriptions");
    if (prof) prof = prof->FirstChildElement("Devices");
    if (prof) prof = prof->FirstChildElement("Device");
    if (prof) prof = prof->FirstChildElement("Profile");
    XMLElement* pdoList = nullptr;
    if (prof) pdoList = prof->FirstChildElement("RxPdoList");
    if (!pdoList && prof) pdoList = prof->FirstChildElement("TxPdoList");

    // Helper lambda to parse PDO lists
    auto parsePdoList = [&](XMLElement* listEl, const char* typeName){
        if (!listEl) return;
        for (XMLElement* pdo = listEl->FirstChildElement("Pdo"); pdo; pdo = pdo->NextSiblingElement("Pdo")) {
            EsiPdoEntry pdoe;
            pdoe.pdoType = typeName ? typeName : "Pdo";
            const char* pname = pdo->FirstChildElement("Name") ? pdo->FirstChildElement("Name")->GetText() : nullptr;
            pdoe.name = pname ? pname : "";
            const char* pidx = pdo->FirstChildElement("Index") ? pdo->FirstChildElement("Index")->GetText() : nullptr;
            if (pidx) pdoe.index = ParseHexIndex(pidx);

            // Entries inside PDO
            for (XMLElement* entry = pdo->FirstChildElement("Entry"); entry; entry = entry->NextSiblingElement("Entry")) {
                EsiObjectEntry oe;
                const char* idx = entry->FirstChildElement("Index") ? entry->FirstChildElement("Index")->GetText() : nullptr;
                if (idx) oe.index = ParseHexIndex(idx);
                const char* subidx = entry->FirstChildElement("Subindex") ? entry->FirstChildElement("Subindex")->GetText() : nullptr;
                if (subidx) oe.subindex = static_cast<uint8_t>(strtoul(subidx, nullptr, 0));
                const char* ename = entry->FirstChildElement("Name") ? entry->FirstChildElement("Name")->GetText() : nullptr;
                if (ename) oe.name = ename;
                XMLElement* bitlenEl = entry->FirstChildElement("BitLength");
                if (bitlenEl && bitlenEl->GetText()) oe.bitlen = atoi(bitlenEl->GetText());
                pdoe.entries.push_back(oe);
            }
            // After collecting entries, try to fill missing info from the object dictionary
            std::unordered_map<uint32_t, EsiObjectEntry> lookup;
            for (const auto &o : objects) {
                uint32_t key = (static_cast<uint32_t>(o.index) << 16) | static_cast<uint32_t>(o.subindex);
                if (lookup.find(key) == lookup.end()) lookup.emplace(key, o);
                if (o.subindex == 0) {
                    uint32_t defkey = (static_cast<uint32_t>(o.index) << 16) | 0u;
                    lookup[defkey] = o;
                }
            }
            for (auto &e : pdoe.entries) {
                if (e.dataType.empty() || e.access.empty() || e.bitlen == 0) {
                    uint32_t key = (static_cast<uint32_t>(e.index) << 16) | static_cast<uint32_t>(e.subindex);
                    auto it = lookup.find(key);
                    if (it == lookup.end()) {
                        // try default subindex 0
                        uint32_t defkey = (static_cast<uint32_t>(e.index) << 16) | 0u;
                        it = lookup.find(defkey);
                    }
                    if (it != lookup.end()) {
                        const EsiObjectEntry &src = it->second;
                        if (e.dataType.empty()) e.dataType = src.dataType;
                        if (e.access.empty()) e.access = src.access;
                        if (e.bitlen == 0) e.bitlen = src.bitlen;
                    }
                }
            }
            pdos.push_back(pdoe);
        }
    };

    // Try RxPdoList and TxPdoList
    XMLElement* rx = prof ? prof->FirstChildElement("RxPdoList") : nullptr;
    XMLElement* tx = prof ? prof->FirstChildElement("TxPdoList") : nullptr;
    parsePdoList(rx, "RxPdo");
    parsePdoList(tx, "TxPdo");

    return true;
}

// Normalize DataType strings found in ESI files to canonical tokens used by the rest of the code.
std::string EsiParser::NormalizeDataType(const std::string& raw) {
    if (raw.empty()) return std::string();
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back((char)toupper((unsigned char)c));
    // Trim whitespace
    auto start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::string();
    auto end = s.find_last_not_of(" \t\n\r");
    s = s.substr(start, end - start + 1);

    // map common variants to canonical types
    if (s == "BOOL" || s == "BOOLEAN") return "BOOL";
    if (s == "BYTE" || s == "UINT8" || s == "USINT" || s == "U8") return "BYTE";
    // Don't normalize the ambiguous token "UINT" to WORD here; keep it as-is
    // so that the UI shows the original ESI token (e.g. "UINT"). Specific
    // sized variants map to canonical tokens below.
    if (s == "WORD" || s == "UINT16" || s == "UINT16_T" || s == "UINT_16") return "WORD";
    if (s == "DWORD" || s == "UINT32" || s == "UDINT" || s == "ULONG" || s == "U32") return "DWORD";
    // Some ESI files may include bitlengths like "UINT8[8]" or similar; try to detect numeric width
    if (s.find("UINT") != std::string::npos) {
        if (s.find("8") != std::string::npos) return "BYTE";
        if (s.find("16") != std::string::npos) return "WORD";
        if (s.find("32") != std::string::npos) return "DWORD";
    }
    // Fallback: return uppercased token
    return s;
}

// Normalize Access description into small tokens: "ro", "wo", "rw".
std::string EsiParser::NormalizeAccess(const std::string& raw) {
    if (raw.empty()) return std::string();
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back((char)toupper((unsigned char)c));
    // collapse common separators
    for (char &c : s) if (c == ',' || c == ';' || c == '/') c = ' ';

    bool canRead = false;
    bool canWrite = false;
    // check keywords (common words: READ, RD, WRITE, WR)
    if (s.find("READ") != std::string::npos || s.find("RD") != std::string::npos || s == "R") canRead = true;
    if (s.find("WRITE") != std::string::npos || s.find("WR") != std::string::npos || s == "W") canWrite = true;
    if (!canRead && !canWrite) {
        // some ESI uses "RO", "RW", "WO" directly
        if (s.find("RO") != std::string::npos) canRead = true;
        if (s.find("WO") != std::string::npos) canWrite = true;
        if (s.find("RW") != std::string::npos) { canRead = true; canWrite = true; }
    }
    if (canRead && canWrite) return "rw";
    if (canRead) return "ro";
    if (canWrite) return "wo";
    // fallback: lowercase trimmed
    auto start = raw.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return std::string();
    auto end = raw.find_last_not_of(" \t\n\r");
    std::string trimmed = raw.substr(start, end - start + 1);
    for (char &c : trimmed) c = (char)tolower((unsigned char)c);
    return trimmed;
}
