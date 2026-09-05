// EsiParser.cpp - 簡要說明:
// 解析 EtherCAT ESI XML 檔，抽出 Object 與 PDO 定義供系統使用。

#include "EsiParser.h"
#include <tinyxml2.h>
#include <sstream>
#include <cctype>
#include <cstring>
#include <cstdlib>
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
            if (objAccEl && objAccEl->GetText()) {
                std::string raw = objAccEl->GetText();
                // Preserve original token as provided by ESI
                entry.access = raw;
                // Store a normalized token for internal logic checks
                entry.accessNormalized = EsiParser::NormalizeAccess(raw);
            }
            
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
                if (accEl && accEl->GetText()) {
                    std::string raw = accEl->GetText();
                    entry.access = raw;
                    entry.accessNormalized = EsiParser::NormalizeAccess(raw);
                }
            }
            // Some ESI use <Info><SubItem> to describe elements of a complex data type (e.g. DT6099)
            // If Subindex nodes are not present, try parsing Info/SubItem as sequential subindices.
            XMLElement* infoEl_alt = obj->FirstChildElement("Info");
            bool parsedSubItems = false;
            if (obj->FirstChildElement("Subindex") == nullptr && infoEl_alt) {
                XMLElement* subI = infoEl_alt->FirstChildElement("SubItem");
                if (subI) {
                    parsedSubItems = true;
                    // collect subitems locally first
                    std::vector<EsiObjectEntry> localSubs;
                    int sidx = 0; // start numbering at 0
                    for (XMLElement* si = subI; si; si = si->NextSiblingElement("SubItem")) {
                        EsiObjectEntry sentry = entry;
                        sentry.subindex = static_cast<uint8_t>(sidx);
                        // Name inside SubItem
                        XMLElement* nameEl = si->FirstChildElement("Name");
                        if (nameEl && nameEl->GetText()) sentry.name = nameEl->GetText();
                        // DefaultValue inside SubItem Info
                        XMLElement* infoSi = si->FirstChildElement("Info");
                        if (infoSi) {
                            XMLElement* dv = infoSi->FirstChildElement("DefaultValue");
                            if (!dv) dv = infoSi->FirstChildElement("Default");
                            if (dv && dv->GetText()) sentry.defaultValue = dv->GetText();
                        }
                        // Type inside SubItem
                        XMLElement* siType = si->FirstChildElement("Type");
                        if (!siType) siType = si->FirstChildElement("DataType");
                        if (siType && siType->GetText()) {
                            std::string dt = EsiParser::NormalizeDataType(siType->GetText());
                            sentry.dataType = dt;
                            if (dt == "BOOL") sentry.bitlen = 1;
                            else if (dt == "BYTE") sentry.bitlen = 8;
                            else if (dt == "WORD") sentry.bitlen = 16;
                            else if (dt == "DWORD") sentry.bitlen = 32;
                        }
                        if (sentry.bitlen == 0) {
                            XMLElement* bs2 = si->FirstChildElement("BitSize");
                            if (!bs2) bs2 = si->FirstChildElement("BitLength");
                            if (bs2 && bs2->GetText()) sentry.bitlen = atoi(bs2->GetText());
                        }
                        XMLElement* accSi = si->FirstChildElement("Access");
                        if (!accSi) {
                            XMLElement* flagsSi = si->FirstChildElement("Flags");
                            if (flagsSi) accSi = flagsSi->FirstChildElement("Access");
                        }
                        if (accSi && accSi->GetText()) {
                            std::string raw = accSi->GetText();
                            sentry.access = raw;
                            sentry.accessNormalized = EsiParser::NormalizeAccess(raw);
                        }
                        localSubs.push_back(sentry);
                        ++sidx;
                    }
                    // Determine count from first subitem default if present (Number of entries)
                    int keepCount = -1; // -1 means keep all
                    if (!localSubs.empty() && !localSubs[0].defaultValue.empty()) {
                        // parse numeric default; support #xHEX or decimal
                        keepCount = (int)ParseHexIndex(localSubs[0].defaultValue.c_str());
                    }
                    // If no explicit default, derive count as number of subitems - 1
                    if (keepCount < 0) {
                        if ((int)localSubs.size() >= 1) keepCount = (int)localSubs.size() - 1;
                        else keepCount = 0;
                    }
                    // Set the defaultValue of the first subitem to the count (as decimal)
                    if (!localSubs.empty()) {
                        localSubs[0].defaultValue = std::to_string(keepCount);
                        // If these were parsed as SubItem entries, ensure the subindex 0
                        // entry uses the parent object's Name (index name) as requested.
                        localSubs[0].name = entry.name;
                    }
                    // Append only the first (index 0) and the next 'keepCount' items (1..keepCount)
                    if (!localSubs.empty()) {
                        objects.push_back(localSubs[0]);
                        for (int i = 1; i <= keepCount && i < (int)localSubs.size(); ++i) {
                            objects.push_back(localSubs[i]);
                        }
                    }
                }
            }
            
            // Only add the main entry if there are no Subindex children, or if it represents subindex 0
            bool hasSubindexChildren = (obj->FirstChildElement("Subindex") != nullptr);
            // If we've parsed SubItem entries for this object, avoid adding the main
            // top-level entry as subindex 0 to prevent duplicate 0x6099/0x00 rows.
            if (!hasSubindexChildren && !parsedSubItems) {
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
                // If this subindex is 0, prefer the parent object's name instead
                // when the object defines subindices (so the 0x00 row shows the Index name).
                if (sentry.subindex == 0) {
                    sentry.name = entry.name;
                }
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
                if (accEl2 && accEl2->GetText()) {
                    std::string raw = accEl2->GetText();
                    sentry.access = raw;
                    sentry.accessNormalized = EsiParser::NormalizeAccess(raw);
                }
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
                        if (o.accessNormalized.empty()) o.accessNormalized = def.accessNormalized;
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
                        if (e.accessNormalized.empty()) e.accessNormalized = src.accessNormalized;
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

// ---------------------------------------------------------------------------
// Lightweight identity-only load (vendor / product / revision)
// ---------------------------------------------------------------------------

static uint32_t ParseHexU32(const char* text)
{
    if (!text) return 0;
    const char* s = text;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (*s == '#') ++s;
    if (*s == 'x' || *s == 'X') ++s;
    return static_cast<uint32_t>(strtoul(s, nullptr, 16));
}

bool EsiParser::LoadDeviceIdentity(const std::string& path, EsiDeviceIdentity& id, std::string& err)
{
    id = EsiDeviceIdentity{};
    err.clear();

    XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != XML_SUCCESS) {
        err = "Failed to load: ";
        err += doc.ErrorStr();
        return false;
    }

    XMLElement* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) {
        err = "Missing EtherCATInfo root";
        return false;
    }

    // Vendor ID
    XMLElement* vendor = root->FirstChildElement("Vendor");
    if (vendor) {
        XMLElement* vid = vendor->FirstChildElement("Id");
        if (vid && vid->GetText()) {
            id.vendorId = ParseHexU32(vid->GetText());
        }
    }

    // Device → Type (ProductCode + RevisionNo)
    XMLElement* desc = root->FirstChildElement("Descriptions");
    if (!desc) desc = root;
    XMLElement* devs = desc->FirstChildElement("Devices");
    if (devs) devs = devs->FirstChildElement("Device");
    if (devs) {
        XMLElement* typeEl = devs->FirstChildElement("Type");
        if (typeEl) {
            const char* pc = typeEl->Attribute("ProductCode");
            if (pc) id.productCode = ParseHexU32(pc);
            const char* rn = typeEl->Attribute("RevisionNo");
            if (rn) id.revision = ParseHexU32(rn);
        }
    }

    id.valid = (id.vendorId != 0 || id.productCode != 0);
    return id.valid;
}

// ---------------------------------------------------------------------------
// SM + PDO descriptor load from the <Sm> section at Device level
// ---------------------------------------------------------------------------

static uint8_t ParseHexU8(const char* text)
{
    if (!text) return 0;
    const char* s = text;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (*s == '#') ++s;
    if (*s == 'x' || *s == 'X') ++s;
    return static_cast<uint8_t>(strtoul(s, nullptr, 16));
}

static uint16_t ParseHexU16(const char* text)
{
    if (!text) return 0;
    const char* s = text;
    while (*s && isspace((unsigned char)*s)) ++s;
    if (*s == '#') ++s;
    if (*s == 'x' || *s == 'X') ++s;
    return static_cast<uint16_t>(strtoul(s, nullptr, 16));
}

bool EsiParser::LoadSmDescriptor(const std::string& path, EsiSmDescriptor& desc, std::string& err)
{
    desc = EsiSmDescriptor{};
    err.clear();

    // First load identity
    if (!LoadDeviceIdentity(path, desc.identity, err)) {
        return false;
    }

    XMLDocument doc;
    if (doc.LoadFile(path.c_str()) != XML_SUCCESS) {
        err = "Failed to load: ";
        err += doc.ErrorStr();
        return false;
    }

    XMLElement* root = doc.FirstChildElement("EtherCATInfo");
    if (!root) {
        err = "Missing EtherCATInfo root";
        return false;
    }

    // Navigate to <Device> under <Descriptions><Devices>
    XMLElement* descEl = root->FirstChildElement("Descriptions");
    if (!descEl) descEl = root;
    XMLElement* devsEl = descEl->FirstChildElement("Devices");
    if (devsEl) devsEl = devsEl->FirstChildElement("Device");
    if (!devsEl) {
        // No Device section; descriptor stays empty (valid for simple slaves)
        return true;
    }

    // Extract device name: prefer English (LcId="1033"), fallback to first <Name>
    for (XMLElement* nameEl = devsEl->FirstChildElement("Name"); nameEl;
         nameEl = nameEl->NextSiblingElement("Name")) {
        const char* lcid = nameEl->Attribute("LcId");
        if (lcid && strcmp(lcid, "1033") == 0) {
            const char* n = nameEl->GetText();
            if (n) desc.deviceName = n;
            break;
        }
    }
    if (desc.deviceName.empty()) {
        XMLElement* firstName = devsEl->FirstChildElement("Name");
        if (firstName && firstName->GetText()) {
            desc.deviceName = firstName->GetText();
        }
    }

    // Parse <Sm> elements
    int smIdx = 0;
    for (XMLElement* smEl = devsEl->FirstChildElement("Sm"); smEl; smEl = smEl->NextSiblingElement("Sm")) {
        EsiSmInfo sm;
        sm.index = smIdx++;
        const char* ds = smEl->Attribute("DefaultSize");
        if (ds) sm.defaultSize = ParseHexU16(ds);
        const char* sa = smEl->Attribute("StartAddress");
        if (sa) sm.startAddress = ParseHexU16(sa);
        const char* cb = smEl->Attribute("ControlByte");
        if (cb) sm.controlByte = ParseHexU8(cb);
        const char* en = smEl->Attribute("Enable");
        sm.enable = (en && strcmp(en, "1") == 0);
        desc.smList.push_back(sm);
    }

    // Parse <RxPdo> and <TxPdo> siblings of <Sm>
    auto parsePdoDef = [](XMLElement* pdoEl, const char* typeName) -> EsiSmPdoDef {
        EsiSmPdoDef def;
        def.pdoType = typeName;
        const char* idx = pdoEl->FirstChildElement("Index") ?
                          pdoEl->FirstChildElement("Index")->GetText() : nullptr;
        if (idx) def.index = ParseHexIndex(idx);
        const char* fixed = pdoEl->Attribute("Fixed");
        def.fixed = (fixed && strcmp(fixed, "1") == 0);
        const char* sm = pdoEl->Attribute("Sm");
        def.smIndex = sm ? atoi(sm) : -1;

        for (XMLElement* entry = pdoEl->FirstChildElement("Entry"); entry;
             entry = entry->NextSiblingElement("Entry")) {
            EsiSmPdoEntry e;
            const char* ei = entry->FirstChildElement("Index") ?
                             entry->FirstChildElement("Index")->GetText() : nullptr;
            if (ei) e.index = ParseHexIndex(ei);
            const char* si = entry->FirstChildElement("SubIndex") ?
                             entry->FirstChildElement("SubIndex")->GetText() : nullptr;
            if (si) e.subindex = static_cast<uint8_t>(strtoul(si, nullptr, 0));
            const char* bl = entry->FirstChildElement("BitLen") ?
                             entry->FirstChildElement("BitLen")->GetText() : nullptr;
            if (bl) e.bitlen = atoi(bl);
            def.entries.push_back(e);
        }
        return def;
    };

    for (XMLElement* child = devsEl->FirstChildElement(); child;
         child = child->NextSiblingElement()) {
        std::string name = child->Name();
        if (name == "RxPdo") {
            desc.rxPdos.push_back(parsePdoDef(child, "RxPdo"));
        } else if (name == "TxPdo") {
            desc.txPdos.push_back(parsePdoDef(child, "TxPdo"));
        }
    }

    return true;
}
