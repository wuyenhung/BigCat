#include "../../include/core/CoreHelpers.h"
#include "../../include/core/CpuIsolation.h"
#include "../../include/core/CpuTopology.h"
#include "../../include/EtherCATMaster.h"
#include "../../include/Win32WindowCore.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <cwctype>
#include <cstdlib>
#include <vector>
#include <set>
#include <algorithm>
#include <cctype>

#include <iphlpapi.h>
#include <netioapi.h>
#include <pcap.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <powrprof.h>

// ── Global IRQ write gate ──
// Set to false to disable ALL MSI/MSI-X IRQ registry writes and RSS PowerShell calls.
// Reads and display (GetSystemIrqAffinitySummary etc.) still work.
static bool g_irqWriteEnabled = true;  // IRQ affinity writes enabled (registry DevicePolicy + AssignmentSetOverride)

namespace {

bool IsGenericAdapterFriendlyName(const std::string& friendlyName)
{
    if (friendlyName.empty()) {
        return false;
    }

    std::wstring wideName = CoreHelpers::Utf8ToWide(friendlyName);
    if (wideName.empty()) {
        return false;
    }

    std::transform(wideName.begin(), wideName.end(), wideName.begin(), ::towlower);

    const wchar_t* genericPrefixes[] = {
        L"ethernet",
        L"local area connection",
        L"乙太網路",
        L"區域連線",
        L"本機連線",
    };

    for (const wchar_t* prefix : genericPrefixes) {
        const std::wstring prefixText(prefix);
        if (wideName.rfind(prefixText, 0) != 0) {
            continue;
        }

        const std::wstring suffix = wideName.substr(prefixText.size());
        const bool suffixIsGeneric = std::all_of(
            suffix.begin(),
            suffix.end(),
            [](wchar_t ch) {
                return iswspace(ch) || iswdigit(ch) || ch == L'#' || ch == L'-' || ch == L'_';
            });
        if (suffixIsGeneric) {
            return true;
        }
    }

    return false;
}

std::string ResolveHumanReadableAdapterName(const AdapterEntry& entry)
{
    if (!entry.friendlyName.empty() &&
        (entry.description.empty() || !IsGenericAdapterFriendlyName(entry.friendlyName))) {
        return entry.friendlyName;
    }

    const std::string& description = entry.description;
    if (!description.empty()) {
        static const std::string prefix = "Network adapter '";
        static const std::string suffix = "' on local host";
        if (description.rfind(prefix, 0) == 0 &&
            description.size() > prefix.size() + suffix.size() &&
            description.compare(description.size() - suffix.size(), suffix.size(), suffix) == 0) {
            return description.substr(prefix.size(), description.size() - prefix.size() - suffix.size());
        }
        return description;
    }

    if (!entry.friendlyName.empty()) {
        return entry.friendlyName;
    }

    return entry.pcapName;
}

bool IsSimulationAdapterEntry(const AdapterEntry& entry)
{
    return EtherCATMaster::IsSimulationAdapterSelection(entry.pcapName) ||
           EtherCATMaster::IsSimulationAdapterSelection(entry.adapterName) ||
           EtherCATMaster::IsSimulationAdapterSelection(entry.friendlyName) ||
           EtherCATMaster::IsSimulationAdapterSelection(entry.description);
}

std::string ResolveAdapterJsonDisplay(const AdapterEntry& entry)
{
    if (IsSimulationAdapterEntry(entry)) {
        return EtherCATMaster::GetSimulationAdapterDisplayName();
    }
    return CoreHelpers::MakeAdapterDisplayString(entry);
}

AdapterEntry BuildSimulationAdapterEntry()
{
    AdapterEntry entry;
    entry.pcapName = EtherCATMaster::GetSimulationInterfaceName();
    entry.description = EtherCATMaster::GetSimulationAdapterDisplayName();
    entry.friendlyName = EtherCATMaster::GetSimulationAdapterDisplayName();
    entry.adapterName = EtherCATMaster::GetSimulationInterfaceName();
    entry.linkUp = true;
    return entry;
}

void AppendAdapterJsonEntry(std::ostringstream& js, const AdapterEntry& entry, bool& first)
{
    std::string display = ResolveAdapterJsonDisplay(entry);
    if (display.empty()) {
        return;
    }

    auto toLowerStr = [](const std::string& s) {
        std::string r = s;
        std::transform(r.begin(), r.end(), r.begin(), ::tolower);
        return r;
    };

    std::string entryLow = toLowerStr(display);
    if (entryLow.find("bluetooth") != std::string::npos ||
        entryLow.find("personal area network") != std::string::npos ||
        entryLow.find("wireless") != std::string::npos) {
        return;
    }

    if (!first) {
        js << ",";
    }
    first = false;

    const std::string dispEsc = CoreHelpers::EscapeJsonString(display);
    const std::string friendly = CoreHelpers::EscapeJsonString(entry.friendlyName);
    js << "{";
    js << "\"pcapName\":\"" << CoreHelpers::EscapeJsonString(entry.pcapName) << "\",";
    js << "\"display\":\"" << dispEsc << "\",";
    js << "\"friendly\":\"" << friendly << "\",";
    js << "\"adapterName\":\"" << CoreHelpers::EscapeJsonString(entry.adapterName) << "\",";
    js << "\"linkUp\":" << (entry.linkUp ? "true" : "false");
    js << "}";
}

// Strip trailing " [XXXXXXXX]" suffix added by FindAdapterDisplayName.
// Example: "Intel(R) I210 Gigabit Network Connection #2 [5652EACB]"
//       → "Intel(R) I210 Gigabit Network Connection #2"
// The bracket-enclosed GUID suffix breaks PowerShell -like matching because
// [...] is treated as a wildcard character class.
static std::string StripAdapterGuidSuffix(const std::string& name) {
    if (name.empty()) return name;
    // Pattern: " [XXXXXXXX]" at end where X is hex digit (0-9, A-F)
    size_t len = name.size();
    if (len < 12) return name;  // minimum " [XXXXXXXX]" = 12 chars
    if (name[len - 1] != ']') return name;
    size_t bracketOpen = name.rfind(" [");
    if (bracketOpen == std::string::npos) return name;
    // Verify the bracket content is 8 hex chars
    size_t contentLen = len - bracketOpen - 3;  // -3 for " [", "]"
    if (contentLen != 8) return name;
    for (size_t i = bracketOpen + 2; i < len - 1; ++i) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return name;
    }
    return name.substr(0, bracketOpen);
}

} // namespace

namespace CoreHelpers {

    // Convert wide (UTF-16) string to UTF-8 encoded std::string
    std::string WideToUtf8(const std::wstring &w) {
        if (w.empty()) return std::string();
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
        if (len <= 0) return std::string();
        std::string out(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &out[0], len, NULL, NULL);
        if (!out.empty() && out.back() == '\0') out.pop_back();
        return out;
    }

    // Convert UTF-8 encoded std::string to wide (UTF-16) std::wstring
    std::wstring Utf8ToWide(const std::string &s) {
        if (s.empty()) return std::wstring();
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
        if (len <= 0) return std::wstring();
        std::wstring out(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], len);
        if (!out.empty() && out.back() == L'\0') out.pop_back();
        return out;
    }

    // Escape basic XML characters in a UTF-8 encoded string
    std::string EscapeXml(const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back((char)c); break;
            }
        }
        return out;
    }

    std::string EscapeJsonString(const std::string &s) {
        std::string out; out.reserve(s.size());
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7]; snprintf(buf, sizeof(buf), "\\u%04X", c);
                    out += buf;
                } else out.push_back((char)c);
                break;
            }
        }
        return out;
    }

    std::wstring BytesToHexDisplay(const uint8_t* data, size_t len, bool bigEndian, bool prefix0x, bool spaceSeparated, bool uppercase) {
        if (!data || len == 0) return std::wstring();
        std::wostringstream ss;
        ss << std::hex << (uppercase ? std::uppercase : std::nouppercase);
        if (bigEndian) {
            for (int i = (int)len - 1; i >= 0; --i) {
                if (i != (int)len - 1 && spaceSeparated) ss << L" ";
                if (prefix0x) ss << L"0x";
                ss << std::setw(2) << std::setfill(L'0') << (int)data[i];
            }
        } else {
            for (size_t i = 0; i < len; ++i) {
                if (i != 0 && spaceSeparated) ss << L" ";
                if (prefix0x) ss << L"0x";
                ss << std::setw(2) << std::setfill(L'0') << (int)data[i];
            }
        }
        return ss.str();
    }

    void ParseTokensToBytes(const std::wstring &tokenStr, std::vector<uint8_t> &outBytes, bool &anyHexToken) {
        outBytes.clear();
        anyHexToken = false;
        std::wstring s = tokenStr;
        for (wchar_t &c : s) {
            if (c == L',' || c == L';' || c == L':' || c == L'|') c = L' ';
        }

        std::wistringstream iss(s);
        std::wstring tok;
        while (iss >> tok) {
            if (tok.empty()) continue;
            unsigned long long v = 0;
            if (tok.size() >= 2 && (tok.rfind(L"0x", 0) == 0 || tok.rfind(L"0X", 0) == 0)) {
                v = wcstoull(tok.c_str() + 2, NULL, 16);
                anyHexToken = true;
            } else {
                bool allHex = true;
                bool hasAlpha = false;
                for (wchar_t ch : tok) {
                    if (!iswxdigit(ch)) { allHex = false; break; }
                    if (iswalpha(ch)) hasAlpha = true;
                }
                if (allHex && (hasAlpha || tok.size() <= 2)) {
                    v = wcstoull(tok.c_str(), NULL, 16);
                    anyHexToken = true;
                } else {
                    v = wcstoull(tok.c_str(), NULL, 0);
                }
            }
            outBytes.push_back((uint8_t)(v & 0xFF));
        }
    }

    // Adapter helpers
    std::vector<AdapterEntry> EnumerateAdapters(std::string &errOut) {
        errOut.clear();
        std::vector<AdapterEntry> result;

        ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
        ULONG family = AF_UNSPEC;
        ULONG outBufLen = 0;
        DWORD rc = GetAdaptersAddresses(family, flags, NULL, NULL, &outBufLen);
        if (rc != ERROR_BUFFER_OVERFLOW) {
            errOut = "GetAdaptersAddresses failed to get buffer size";
            return result;
        }

        std::vector<BYTE> buffer(outBufLen);
        IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(family, flags, NULL, adapters, &outBufLen);
        if (rc != NO_ERROR) {
            errOut = "GetAdaptersAddresses failed";
            return result;
        }

        struct AdapterInfo { IP_ADAPTER_ADDRESSES* aa; };
        std::vector<AdapterInfo> goodAdapters;
        for (IP_ADAPTER_ADDRESSES* aa = adapters; aa; aa = aa->Next) {
            if (aa->IfType == IF_TYPE_ETHERNET_CSMACD && aa->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                bool isVirtual = false;
                if (aa->FriendlyName) {
                    std::wstring friendlyName(aa->FriendlyName);
                    std::transform(friendlyName.begin(), friendlyName.end(), friendlyName.begin(), ::towlower);
                    if (friendlyName.find(L"network bridge") != std::wstring::npos ||
                        friendlyName.find(L"bridge") != std::wstring::npos ||
                        friendlyName.find(L"vpn") != std::wstring::npos ||
                        friendlyName.find(L"virtual") != std::wstring::npos ||
                        friendlyName.find(L"tap") != std::wstring::npos ||
                        friendlyName.find(L"tun") != std::wstring::npos ||
                        friendlyName.find(L"bluetooth") != std::wstring::npos ||
                        friendlyName.find(L"wireless") != std::wstring::npos) {
                        isVirtual = true;
                    }
                }
                if (!isVirtual) goodAdapters.push_back({ aa });
            }
        }

        pcap_if_t* alldevs = nullptr;
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        if (pcap_findalldevs_ex(PCAP_SRC_IF_STRING, NULL, &alldevs, errbuf) == -1) {
            errOut = std::string("pcap_findalldevs_ex failed: ") + (errbuf[0] ? errbuf : "");
            return result;
        }

        auto sockaddr_equal = [](sockaddr* a, sockaddr* b) -> bool {
            if (!a || !b) return false;
            if (a->sa_family != b->sa_family) return false;
            if (a->sa_family == AF_INET) {
                sockaddr_in* ia = reinterpret_cast<sockaddr_in*>(a);
                sockaddr_in* ib = reinterpret_cast<sockaddr_in*>(b);
                return ia->sin_addr.S_un.S_addr == ib->sin_addr.S_un.S_addr;
            } else if (a->sa_family == AF_INET6) {
                sockaddr_in6* ia6 = reinterpret_cast<sockaddr_in6*>(a);
                sockaddr_in6* ib6 = reinterpret_cast<sockaddr_in6*>(b);
                return memcmp(&ia6->sin6_addr, &ib6->sin6_addr, sizeof(in6_addr)) == 0;
            }
            return false;
        };

        for (pcap_if_t* d = alldevs; d; d = d->next) {
            bool matched = false;
            IP_ADAPTER_ADDRESSES* matchedAA = nullptr;
            for (pcap_addr_t* a = d->addresses; a && !matched; a = a->next) {
                sockaddr* ps = a->addr;
                if (!ps) continue;
                for (auto &ad : goodAdapters) {
                    for (IP_ADAPTER_UNICAST_ADDRESS* ua = ad.aa->FirstUnicastAddress; ua && !matched; ua = ua->Next) {
                        sockaddr* as = ua->Address.lpSockaddr;
                        if (sockaddr_equal(ps, as)) { matched = true; matchedAA = ad.aa; break; }
                    }
                    if (matched) break; // stop iterating goodAdapters once a match is found
                }
            }

            if (!matched) {
                std::string pname = d->name ? d->name : std::string();
                for (auto &ad : goodAdapters) {
                    if (!ad.aa->AdapterName) continue;
                    std::string an(ad.aa->AdapterName);
                    if (!an.empty() && an.front() == '{' && an.back() == '}') an = an.substr(1, an.size()-2);
                    auto toLower = [](std::string s){ std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; };
                    std::string pl = toLower(pname);
                    std::string al = toLower(an);
                    if (!al.empty() && pl.find(al) != std::string::npos) { matched = true; matchedAA = ad.aa; break; }
                    if (ad.aa->FriendlyName) {
                        std::string fname = WideToUtf8(std::wstring(ad.aa->FriendlyName));
                        if (!fname.empty()) {
                            std::string fl = toLower(fname);
                            std::string desc = d->description ? std::string(d->description) : std::string();
                            std::string dl = toLower(desc);
                            if (dl.find(fl) != std::string::npos) { matched = true; matchedAA = ad.aa; break; }
                        }
                    }
                    if (matched) break;
                }
            }

            // ── Npcap-level wireless/Bluetooth exclusion ──
            // On zh-TW systems, GetAdaptersAddresses FriendlyName may be "藍牙網路連線"
            // which evades the ASCII-keyword check above.  Also check Npcap flags and
            // description so matched wireless devices fall through to the else block.
            if (matched) {
                std::string mdesc = d->description ? d->description : std::string();
                std::string mdescLower(mdesc);
                std::transform(mdescLower.begin(), mdescLower.end(), mdescLower.begin(), ::tolower);
                if ((d->flags & 0x8) ||  // PCAP_IF_WIRELESS
                    mdescLower.find("bluetooth") != std::string::npos ||
                    mdescLower.find("\xe8\x97\x8d\xe7\x89\x99") != std::string::npos ||  // 藍牙
                    mdescLower.find("\xe8\x97\x8d\xe8\x8a\xbd") != std::string::npos ||  // 藍芽
                    mdescLower.find("wireless") != std::string::npos) {
                    matched = false;
                    matchedAA = nullptr;
                }
            }

            if (matched) {
                AdapterEntry e;
                e.pcapName = d->name ? d->name : std::string();
                e.description = d->description ? d->description : std::string();
                e.friendlyName = "";
                if (matchedAA && matchedAA->FriendlyName) {
                    e.friendlyName = WideToUtf8(std::wstring(matchedAA->FriendlyName));
                }
                // AdapterName is typically a GUID like "{...}". Use without braces for display.
                e.adapterName = "";
                if (matchedAA && matchedAA->AdapterName) {
                    std::string an(matchedAA->AdapterName);
                    if (!an.empty() && an.front() == '{' && an.back() == '}') an = an.substr(1, an.size()-2);
                    e.adapterName = an;
                }
                e.linkUp = matchedAA ? (matchedAA->OperStatus == IfOperStatusUp) : true;
                result.push_back(e);
            } else {
                // Npcap device not matched to any GetAdaptersAddresses entry.
                // Typical cause: TCP/IP unbound from this NIC.
                // Include it if it looks like a real physical Ethernet NIC (not virtual / wireless).
                std::string pname = d->name ? d->name : std::string();
                std::string desc = d->description ? d->description : std::string();

                // --- Stage 1: exclude via Npcap flags (authoritative) ---
                // PCAP_IF_LOOPBACK=0x1, PCAP_IF_WIRELESS=0x8
                constexpr uint32_t kExcludeFlags = 0x1 | 0x8;  // loopback | wireless
                if (d->flags & kExcludeFlags)
                    continue;
                // NOTE: Do NOT filter on PCAP_IF_CONNECTION_STATUS_NOT_APPLICABLE (0x30).
                // When TCP/IP is unbound, Npcap may report the real NIC as not-applicable,
                // and filtering it here would hide the correct adapter.
                // However, NICs without TCP/IP cause WKC instability on Win11 —
                // that case is handled at connection time by EnsureStaticIpOnEthercatNic.

                // --- Stage 2: keyword exclusion on name + description ---
                // Concatenate name|description and lowercase for single-pass scan.
                std::string haystack = pname + "|" + desc;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
                static const char* skipKeywords[] = {
                    "loopback", "virtual", "vpn", "tap", "tun",
                    "bluetooth", "\xe8\x97\x8d\xe7\x89\x99", "\xe8\x97\x8d\xe8\x8a\xbd",  // 藍牙, 藍芽 (UTF-8)
                    "wireless", "wifi", "wi-fi",
                    "bridge", "wan miniport", "null",
                    "hyper-v", "ndis", "miniport"
                };
                bool skip = false;
                for (const char* kw : skipKeywords) {
                    if (haystack.find(kw) != std::string::npos) { skip = true; break; }
                }
                if (skip) continue;

                // --- Stage 3: positive signal — must look like real hardware ---
                // Npcap prefixes local adapters with rpcap:// (e.g. rpcap://\Device\NPF_{GUID})
                bool isNpfDevice = (pname.find("\\Device\\NPF_") != std::string::npos);
                bool hasNicDesc = (haystack.find("ethernet") != std::string::npos ||
                                   haystack.find("gigabit") != std::string::npos ||
                                   haystack.find("pci") != std::string::npos ||
                                   haystack.find("intel") != std::string::npos ||
                                   haystack.find("realtek") != std::string::npos ||
                                   haystack.find("i210") != std::string::npos ||
                                   haystack.find("i219") != std::string::npos ||
                                   haystack.find("i225") != std::string::npos ||
                                   haystack.find("i226") != std::string::npos ||
                                   haystack.find("network") != std::string::npos ||
                                   haystack.find("controller") != std::string::npos ||
                                   haystack.find("nic") != std::string::npos ||
                                   haystack.find("connection") != std::string::npos ||
                                   haystack.find("gbe") != std::string::npos ||
                                   haystack.find("pro/1000") != std::string::npos);
                if (!isNpfDevice && !hasNicDesc)
                    continue;

                AdapterEntry e;
                e.pcapName = pname;
                e.description = desc;
                e.friendlyName = "";   // resolved from description by ResolveHumanReadableAdapterName

                // Extract GUID from \Device\NPF_{GUID} (or any \Device\XXX_{GUID})
                auto bracePos = pname.rfind('{');
                if (bracePos != std::string::npos && pname.back() == '}') {
                    e.adapterName = pname.substr(bracePos + 1);
                    if (!e.adapterName.empty() && e.adapterName.back() == '}')
                        e.adapterName.pop_back();
                } else {
                    e.adapterName = pname;
                }

                e.linkUp = CheckLinkStatus(pname.c_str());
                result.push_back(e);
            }
        }  // for each pcap_if_t

        if (alldevs) pcap_freealldevs(alldevs);
        return result;
    }

    std::string MakeAdapterDisplayString(const AdapterEntry &d) {
        return ResolveHumanReadableAdapterName(d);
    }

    // Check link status for a pcap interface name.
    // Uses Windows GetIfEntry2 (OperStatus) for reliable physical link detection.
    // Falls back to pcap open/activate when the name does not match the
    // expected Npcap \Device\NPF_{GUID} format.
    bool CheckLinkStatus(const char* ifname) {
        if (!ifname) return false;

        // ── Extract GUID from Npcap device name (\Device\NPF_{GUID}) ──
        std::string name(ifname);
        const std::string npfPrefix = "\\Device\\NPF_{";
        GUID adapterGuid = {};
        bool guidParsed = false;

        if (name.size() > npfPrefix.size() && name.rfind(npfPrefix, 0) == 0 && name.back() == '}') {
            // name = "\Device\NPF_{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
            std::string guidStr = "{" + name.substr(npfPrefix.size());
            // guidStr = "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
            // Parse GUID manually to avoid ole32/rpcrt4 link dependency
            int parsed = sscanf_s(guidStr.c_str(),
                "{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
                &adapterGuid.Data1,
                &adapterGuid.Data2,
                &adapterGuid.Data3,
                &adapterGuid.Data4[0], &adapterGuid.Data4[1],
                &adapterGuid.Data4[2], &adapterGuid.Data4[3],
                &adapterGuid.Data4[4], &adapterGuid.Data4[5],
                &adapterGuid.Data4[6], &adapterGuid.Data4[7]);
            if (parsed == 11) {
                guidParsed = true;
            }
        }

        if (guidParsed) {
            // ── Resolve GUID → LUID → interface index → query OperStatus ──
            NET_LUID luid = {};
            if (ConvertInterfaceGuidToLuid(&adapterGuid, &luid) == NO_ERROR) {
                NET_IFINDEX ifIndex = 0;
                if (ConvertInterfaceLuidToIndex(&luid, &ifIndex) == NO_ERROR) {
                    MIB_IF_ROW2 ifRow = {};
                    ifRow.InterfaceIndex = ifIndex;
                    if (GetIfEntry2(&ifRow) == NO_ERROR) {
                        return (ifRow.OperStatus == IfOperStatusUp);
                    }
                }
            }
            // NetIO query failed (e.g. adapter removed mid-check, or
            // GUID→LUID→index resolution broke).  Return false: we cannot
            // confirm the link is up, and the old pcap_open_live fallback
            // only checks Npcap binding, never the physical link status.
            // Returning false here makes the link monitor and shutdown
            // fast-path treat a missing/unresolvable adapter as link-down.
            return false;
        }

        // ── Name is not in \Device\NPF_{GUID} format (e.g. simulation,
        //     a future adapter naming scheme).  Fall back to the legacy
        //     pcap open/activate check as a best-effort connectivity test.
        //     This path only applies when the adapter name cannot be
        //     resolved to a Windows network interface GUID. ──
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        pcap_t* handle = NULL;
        pcap_t* ph = pcap_create(ifname, errbuf);
        if (ph) {
            pcap_set_snaplen(ph, 64);
            pcap_set_promisc(ph, 0);
            pcap_set_timeout(ph, 1);
            typedef int (*pcap_set_immediate_mode_t)(pcap_t*, int);
            typedef int (*pcap_setmintocopy_t)(pcap_t*, int);
            static pcap_set_immediate_mode_t dyn_pcap_set_immediate_mode = NULL;
            static pcap_setmintocopy_t dyn_pcap_setmintocopy = NULL;
            if (!dyn_pcap_set_immediate_mode || !dyn_pcap_setmintocopy) {
                HMODULE h = GetModuleHandleA("wpcap.dll");
                if (h == NULL) {
                    h = LoadLibraryA("wpcap.dll");
                }
                if (h) {
                    dyn_pcap_set_immediate_mode = (pcap_set_immediate_mode_t)GetProcAddress(h, "pcap_set_immediate_mode");
                    dyn_pcap_setmintocopy = (pcap_setmintocopy_t)GetProcAddress(h, "pcap_setmintocopy");
                }
            }
            if (dyn_pcap_set_immediate_mode) dyn_pcap_set_immediate_mode(ph, 1);
            if (dyn_pcap_setmintocopy) dyn_pcap_setmintocopy(ph, 1);
            if (pcap_activate(ph) == 0) {
                handle = ph;
            } else {
                pcap_close(ph);
                handle = NULL;
            }
        } else {
            handle = pcap_open_live(ifname, 64, 0, 1000, errbuf);
        }
        if (handle) {
            pcap_close(handle);
            return true;
        }
        return false;
    }

    // System theme helper: read HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize\AppsUseLightTheme
    // Returns true when system is in dark mode (AppsUseLightTheme == 0)
    bool IsSystemDarkMode() {
        DWORD useLight = 1;
        DWORD cb = sizeof(useLight);
        LONG rc = RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme",
            RRF_RT_REG_DWORD,
            NULL,
            &useLight,
            &cb);
        if (rc != ERROR_SUCCESS) {
            return false; // assume light
        }
        return (useLight == 0);
    }

    std::string BuildAdaptersJson(const std::vector<AdapterEntry> &list) {
        std::ostringstream js;
        js << "[";
        bool first = true;
        bool haveSimulationEntry = false;
        for (const auto &a : list) {
            haveSimulationEntry = haveSimulationEntry || IsSimulationAdapterEntry(a);
            AppendAdapterJsonEntry(js, a, first);
        }
        if (!haveSimulationEntry) {
            AppendAdapterJsonEntry(js, BuildSimulationAdapterEntry(), first);
        }
        js << "]";
        return js.str();
    }

    // Return JSON array of adapters (uses EnumerateAdapters from Win32Utils)
    std::string EnumerateAdaptersJson() {
        std::string err;
        std::vector<AdapterEntry> list = CoreHelpers::EnumerateAdapters(err);
        return BuildAdaptersJson(list);
    }

    std::vector<int> DistributeColumns(int totalWidth, int colCount) {
        std::vector<int> widths(colCount, 0);
        if (colCount <= 0) return widths;
        int base = totalWidth / colCount;
        for (int i = 0; i < colCount; ++i) widths[i] = base;
        int sum = base * colCount;
        if (sum < totalWidth) widths[colCount - 1] += (totalWidth - sum);
        return widths;
    }

    // Set process high priority and lock affinity to avoid Core 0
    // Ideally locks to Core 4 (mask 0x10) if available to isolate from system interrupts.
    // If fewer cores, picks the highest available core index > 0.
    void SetProcessHighPriorityAndCore4Affinity() {
        // Set Priority Class to REALTIME_PRIORITY_CLASS, fallback to HIGH_PRIORITY_CLASS
        // REALTIME requires Admin (SeIncreaseBasePriorityPrivilege)
        if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        }

        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;

        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            // Exclude Core 0 (handles system IRQs) from the default process mask.
            // IMPORTANT: Do NOT exclude the RT core from the process affinity mask.
            // Windows requires thread affinity masks to be a SUBSET of the process
            // affinity mask. If SetProcessAffinityMask excludes the RT core, then
            // SetThreadAffinityMask for the cyclic thread will silently fail,
            // causing the PDO thread to land on wrong cores.
            // See: ApplyProcessLevelIsolation in CpuIsolation.cpp for full rationale.
            DWORD_PTR desiredMask = systemMask & ~((DWORD_PTR)1); // All except Core 0

            if (desiredMask != 0) {
                SetProcessAffinityMask(GetCurrentProcess(), desiredMask);
            }
        }
    }

    // Optimize NIC IRQ affinity using PowerShell RSS commands
    // This implements the automatic optimization steps from docs/nic_irq_affinity_bilingual.md
    bool OptimizeNicIrqAffinity(const std::string &adapterName, std::string &logOut) {
        logOut.clear();
        if (!g_irqWriteEnabled) {
            logOut = "[NIC Optimization] IRQ write disabled, skipping RSS optimization\n";
            return true;
        }
        if (adapterName.empty()) {
            logOut = "[NIC Optimization] Adapter name is empty, skipping optimization\n";
            return true; // not an error, just skip
        }

        // Build log message
        std::ostringstream log;
        std::string cleanName = StripAdapterGuidSuffix(adapterName);
        log << "[NIC Optimization] Starting RSS optimization for adapter: " << cleanName << "\n";

        // Extract actual adapter name from various formats:
        // "Network adapter 'TwinCAT-Intel PCI Ethernet Adapter (Gigabit) #2' on local host"
        // "Intel(R) PRO/1000 MT Network Connection"
        // "Realtek PCIe GbE Family Controller"
        std::string friendlyName = cleanName;
        
        // Try to extract from "Network adapter 'NAME' on local host" format
        size_t quote1 = cleanName.find('\'');
        size_t quote2 = cleanName.rfind('\'');
        if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
            friendlyName = cleanName.substr(quote1 + 1, quote2 - quote1 - 1);
            log << "[NIC Optimization] Extracted adapter name: " << friendlyName << "\n";
        }
        
        // Trim leading/trailing spaces
        while (!friendlyName.empty() && friendlyName.front() == ' ') {
            friendlyName.erase(0, 1);
        }
        while (!friendlyName.empty() && friendlyName.back() == ' ') {
            friendlyName.pop_back();
        }

        if (friendlyName.empty()) {
            log << "[NIC Optimization] Failed to extract valid adapter name\n";
            logOut = log.str();
            return true; // not critical
        }

        // Quick check: Skip RSS optimization for TwinCAT adapters (they don't support RSS)
        std::string lowerName = friendlyName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find("twincat") != std::string::npos) {
            log << "[NIC Optimization] ⓘ Detected TwinCAT adapter - RSS not supported by design\n";
            log << "[NIC Optimization] TwinCAT manages real-time networking at kernel level\n";
            log << "[NIC Optimization] RSS optimization not applicable - this is normal and expected\n";
            logOut = log.str();
            return true;
        }

        // Create temp file for output
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"rss", 0, tempFile);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        // Step 2: Apply RSS optimization settings
        // Pin NIC RSS to a safe core away from the RT physical core.
        // Use topology to avoid SMT siblings of the RT core.
        int rtCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();
        DWORD_PTR rtCoreMask3 = CoreHelpers::CpuIsolation::GetRtCoreMask();
        if (rtCoreMask3 == 0) {
            rtCoreMask3 = CpuTopology::GetCoreMask(rtCore);
            if (rtCoreMask3 == 0 && rtCore >= 0 && rtCore < 64) {
                rtCoreMask3 = (DWORD_PTR)1 << rtCore;
            }
        }
        int rssBaseCpu = CpuTopology::FindBestNonRtLp(rtCoreMask3);
        log << "[NIC Optimization] RT core=" << rtCore << ", RSS base processor=" << rssBaseCpu << "\n";

        std::wstring adapterNameW = CoreHelpers::Utf8ToWide(friendlyName);
        
        // Escape single quotes in adapter name for PowerShell
        std::wstring escapedName;
        for (wchar_t c : adapterNameW) {
            if (c == L'\'') {
                escapedName += L"''"; // PowerShell escape: ' becomes ''
            } else {
                escapedName += c;
            }
        }
        
        std::wstring setCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        setCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        setCmd += L"$rssBase = " + std::to_wstring(rssBaseCpu) + L"; ";
        setCmd += L"$adapterName = '";
        setCmd += escapedName;
        setCmd += L"'; ";
        setCmd += L"$adapter = Get-NetAdapter | Where-Object { $_.Name -eq $adapterName -or $_.InterfaceDescription -like ('*' + $adapterName + '*') } | Select-Object -First 1; ";
        setCmd += L"if ($adapter) { ";
        setCmd += L"Write-Output ('Found adapter: ' + $adapter.Name + ' (' + $adapter.InterfaceDescription + ')'); ";
        setCmd += L"$rss = Get-NetAdapterRss -Name $adapter.Name -ErrorAction SilentlyContinue; ";
        setCmd += L"if ($rss) { ";
        setCmd += L"try { ";
        setCmd += L"Set-NetAdapterRss -Name $adapter.Name -BaseProcessorNumber $rssBase -MaxProcessorNumber $rssBase -NumberOfReceiveQueues 1 -ErrorAction Stop; ";
        setCmd += L"Write-Output ('SUCCESS: RSS optimized - BaseProcessor=' + $rssBase + ', MaxProcessor=' + $rssBase + ', Queues=1'); ";
        setCmd += L"} catch { ";
        setCmd += L"Write-Output ('ERROR: Set-NetAdapterRss failed - ' + $_.Exception.Message); ";
        setCmd += L"Write-Output ('This may require administrator privileges'); ";
        setCmd += L"} ";
        setCmd += L"} else { ";
        setCmd += L"Write-Output ('WARN: Adapter does not support RSS or RSS is disabled'); ";
        setCmd += L"} ";
        setCmd += L"} else { ";
        setCmd += L"Write-Output ('ERROR: No matching adapter found'); ";
        setCmd += L"Write-Output ('Available adapters:'); ";
        setCmd += L"Get-NetAdapter | Where-Object { $_.Status -eq 'Up' } | ForEach-Object { Write-Output ('  - ' + $_.Name + ' (' + $_.InterfaceDescription + ')') }; ";
        setCmd += L"}\"";

        log << "[NIC Optimization] Applying RSS settings (BaseProcessor=" << rssBaseCpu << ", Queues=1)...\n";
        log << "[NIC Optimization] (This may take a few seconds...)\n";

        wchar_t tempFile2[MAX_PATH];
        GetTempFileNameW(tempPath, L"rss", 0, tempFile2);

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cmdLine2 = L"cmd.exe /c \"chcp 65001 > nul & ";
        cmdLine2 += setCmd;
        cmdLine2 += L" > \"";
        cmdLine2 += tempFile2;
        cmdLine2 += L"\" 2>&1\"";

        DWORD waitResult = WAIT_FAILED;
        
        if (CreateProcessW(NULL, &cmdLine2[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            waitResult = WaitForSingleObject(pi.hProcess, 20000); // 20 second timeout
            
            if (waitResult == WAIT_TIMEOUT) {
                log << "[NIC Optimization] Warning: PowerShell command timed out after 20 seconds\n";
                TerminateProcess(pi.hProcess, 1);
            }
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log << "[NIC Optimization] Error: Failed to create PowerShell process\n";
        }

        // Read output from temp file
        std::string output;
        HANDLE hFile = CreateFileW(tempFile2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(hFile, NULL);
            log << "[NIC Optimization] Output file size: " << fileSize << " bytes\n";
            
            char buffer[8192];
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                output = buffer;
                // Clean up output (remove extra newlines)
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
                    output.pop_back();
                }
                if (!output.empty()) {
                    log << "[NIC Optimization] PowerShell Output:\n" << output << "\n";
                } else {
                    log << "[NIC Optimization] PowerShell Output: (empty)\n";
                }
            } else {
                log << "[NIC Optimization] Error: Failed to read output file\n";
            }
            CloseHandle(hFile);
        } else {
            log << "[NIC Optimization] Error: Failed to open output file\n";
        }

        // Cleanup temp files
        DeleteFileW(tempFile);
        DeleteFileW(tempFile2);

        // Check if optimization was successful by examining output
        bool actualSuccess = (output.find("SUCCESS:") != std::string::npos);
        bool foundAdapter = (output.find("Found adapter:") != std::string::npos);
        bool hasError = (output.find("ERROR:") != std::string::npos);
        bool hasWarn = (output.find("WARN:") != std::string::npos);
        bool noRssSupport = (output.find("does not support RSS") != std::string::npos || 
                            output.find("RSS is disabled") != std::string::npos);
        bool isEmpty = output.empty();

        if (actualSuccess) {
            log << "[NIC Optimization] ✓ RSS optimization completed successfully\n";
            log << "[NIC Optimization] Settings take effect immediately. Consider reboot/adapter restart for full effect.\n";
        } else if (foundAdapter && noRssSupport) {
            log << "[NIC Optimization] ⓘ Adapter does not support RSS (common for TwinCAT/real-time adapters)\n";
            log << "[NIC Optimization] This is normal - TwinCAT manages low-level interrupt handling directly\n";
            log << "[NIC Optimization] RSS optimization not applicable for this adapter type\n";
        } else if (foundAdapter && hasWarn) {
            log << "[NIC Optimization] ⚠ Adapter found but RSS configuration unavailable\n";
        } else if (hasError) {
            log << "[NIC Optimization] ✗ Optimization failed - see PowerShell output above\n";
            log << "[NIC Optimization] Common causes: Admin privileges required, or driver doesn't support RSS\n";
        } else if (isEmpty) {
            log << "[NIC Optimization] ⚠ PowerShell returned no output\n";
            log << "[NIC Optimization] Possible causes:\n";
            log << "[NIC Optimization]   - PowerShell execution policy restrictions\n";
            log << "[NIC Optimization]   - Command timeout (increase wait time if needed)\n";
            log << "[NIC Optimization]   - TwinCAT adapter may not support RSS (this is normal)\n";
            log << "[NIC Optimization] RSS optimization skipped - connection will proceed normally\n";
        } else {
            log << "[NIC Optimization] ? Unable to determine optimization status from output\n";
            log << "[NIC Optimization] Connection will proceed regardless\n";
        }

        logOut = log.str();
        return true; // Return true even if optimization failed (non-critical)
    }

    bool RebalanceWirelessIrqAffinityAwayFromRtCore(std::string &logOut) {
        logOut.clear();
        if (!g_irqWriteEnabled) {
            logOut = "[Wireless IRQ] IRQ write disabled, skipping wireless rebalance\n";
            return true;
        }

        int ethercatRtCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();

        std::ostringstream log;
        log << "[Wireless IRQ] Rebalancing active WiFi / Bluetooth network paths away from RT core "
            << ethercatRtCore << "...\n";

        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"wir", 0, tempFile);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        std::wstring setCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        setCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        setCmd += L"$rtCore = " + std::to_wstring(ethercatRtCore) + L"; ";
        setCmd += L"$cpu = Get-CimInstance Win32_Processor -ErrorAction SilentlyContinue | Select-Object -First 1; ";
        setCmd += L"$logicalCount = if ($cpu -and $cpu.NumberOfLogicalProcessors) { [int]$cpu.NumberOfLogicalProcessors } else { 1 }; ";
        setCmd += L"$wirelessBase = if ($logicalCount -gt 1) { 1 } else { 0 }; ";
        setCmd += L"$wirelessMax = if ($logicalCount -gt $rtCore) { [Math]::Max($wirelessBase, $rtCore - 1) } else { [Math]::Max($wirelessBase, $logicalCount - 1) }; ";
        setCmd += L"if ($wirelessMax -lt $wirelessBase) { $wirelessMax = $wirelessBase }; ";
        setCmd += L"Write-Output ('INFO: Wireless RSS target processors=' + $wirelessBase + '..' + $wirelessMax + ', RT core=' + $rtCore); ";
        setCmd += L"$wirelessAdapters = Get-NetAdapter -IncludeHidden -ErrorAction SilentlyContinue | Where-Object { $_.Status -eq 'Up' -and ($_.NdisPhysicalMedium -eq '802.11' -or $_.NdisPhysicalMedium -eq 9 -or $_.Name -match 'wi-?fi|wlan|wireless|bluetooth|pan' -or $_.InterfaceDescription -match 'wi-?fi|wlan|wireless|bluetooth|pan|802\\.11') }; ";
        setCmd += L"if ($wirelessAdapters) { ";
        setCmd += L"foreach ($adapter in $wirelessAdapters) { ";
        setCmd += L"$kind = if ($adapter.Name -match 'bluetooth|pan' -or $adapter.InterfaceDescription -match 'bluetooth|pan') { 'Bluetooth' } else { 'WiFi' }; ";
        setCmd += L"Write-Output ('Found ' + $kind + ' adapter: ' + $adapter.Name + ' (' + $adapter.InterfaceDescription + ')'); ";
        setCmd += L"$rss = Get-NetAdapterRss -Name $adapter.Name -ErrorAction SilentlyContinue; ";
        setCmd += L"if ($rss) { ";
        setCmd += L"try { ";
        setCmd += L"Set-NetAdapterRss -Name $adapter.Name -BaseProcessorNumber $wirelessBase -MaxProcessorNumber $wirelessMax -NumberOfReceiveQueues 1 -ErrorAction Stop; ";
        setCmd += L"Write-Output ('SUCCESS: ' + $kind + ' RSS moved off RT core - ' + $adapter.Name + ' BaseProcessor=' + $wirelessBase + ' MaxProcessor=' + $wirelessMax + ' Queues=1'); ";
        setCmd += L"} catch { ";
        setCmd += L"Write-Output ('WARN: ' + $kind + ' RSS rebalance skipped for ' + $adapter.Name + ' - ' + $_.Exception.Message); ";
        setCmd += L"} ";
        setCmd += L"} else { ";
        setCmd += L"Write-Output ('INFO: ' + $kind + ' adapter has no RSS control - ' + $adapter.Name); ";
        setCmd += L"} ";
        setCmd += L"} ";
        setCmd += L"} else { ";
        setCmd += L"Write-Output ('INFO: No active WiFi or Bluetooth network adapters found for RT-core rebalance'); ";
        setCmd += L"} ";
        setCmd += L"$btDevices = Get-PnpDevice -Class Bluetooth -Status OK -ErrorAction SilentlyContinue; ";
        setCmd += L"if ($btDevices) { ";
        setCmd += L"foreach ($bt in $btDevices) { ";
        setCmd += L"$affPath = 'HKLM:\\SYSTEM\\CurrentControlSet\\Enum\\' + ($bt.InstanceId -replace '#','\\') + '\\Device Parameters\\Interrupt Management\\Affinity Policy'; ";
        setCmd += L"if (Test-Path $affPath) { ";
        setCmd += L"Write-Output ('INFO: Bluetooth PnP device exposes affinity policy path (not auto-modified): ' + $bt.FriendlyName); ";
        setCmd += L"} else { ";
        setCmd += L"Write-Output ('INFO: Bluetooth PnP device has no exposed affinity policy path: ' + $bt.FriendlyName); ";
        setCmd += L"} ";
        setCmd += L"} ";
        setCmd += L"}\"";

        log << "[Wireless IRQ] Applying RSS rebalance for active wireless adapters and auditing Bluetooth devices...\n";

        wchar_t tempFile2[MAX_PATH];
        GetTempFileNameW(tempPath, L"wir", 0, tempFile2);

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & ";
        cmdLine += setCmd;
        cmdLine += L" > \"";
        cmdLine += tempFile2;
        cmdLine += L"\" 2>&1\"";

        DWORD waitResult = WAIT_FAILED;

        if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            waitResult = WaitForSingleObject(pi.hProcess, 20000);
            if (waitResult == WAIT_TIMEOUT) {
                log << "[Wireless IRQ] Warning: PowerShell command timed out after 20 seconds\n";
                TerminateProcess(pi.hProcess, 1);
            }

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log << "[Wireless IRQ] Error: Failed to create PowerShell process\n";
        }

        std::string output;
        HANDLE hFile = CreateFileW(tempFile2, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buffer[8192];
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                output = buffer;
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
                    output.pop_back();
                }
                if (!output.empty()) {
                    log << "[Wireless IRQ] PowerShell Output:\n" << output << "\n";
                } else {
                    log << "[Wireless IRQ] PowerShell Output: (empty)\n";
                }
            } else {
                log << "[Wireless IRQ] Error: Failed to read output file\n";
            }
            CloseHandle(hFile);
        } else {
            log << "[Wireless IRQ] Error: Failed to open output file\n";
        }

        DeleteFileW(tempFile);
        DeleteFileW(tempFile2);

        const bool wirelessSuccess = (output.find("SUCCESS: WiFi RSS moved off RT core") != std::string::npos) ||
                                     (output.find("SUCCESS: Bluetooth RSS moved off RT core") != std::string::npos);
        const bool foundWifi = (output.find("Found WiFi adapter:") != std::string::npos);
        const bool foundBluetoothAdapter = (output.find("Found Bluetooth adapter:") != std::string::npos);
        const bool noWireless = (output.find("No active WiFi or Bluetooth network adapters found for RT-core rebalance") != std::string::npos);
        const bool noRssControl = (output.find("WiFi adapter has no RSS control") != std::string::npos) ||
                                  (output.find("Bluetooth adapter has no RSS control") != std::string::npos);
        const bool hasWarn = (output.find("WARN: WiFi RSS rebalance skipped") != std::string::npos) ||
                             (output.find("WARN: Bluetooth RSS rebalance skipped") != std::string::npos);
        const bool bluetoothPnpNoPolicy = (output.find("Bluetooth PnP device has no exposed affinity policy path") != std::string::npos);
        const bool bluetoothPnpPolicyVisible = (output.find("Bluetooth PnP device exposes affinity policy path") != std::string::npos);
        const bool isEmpty = output.empty();

        if (wirelessSuccess) {
            log << "[Wireless IRQ] ✓ Active wireless RSS queues moved away from RT core\n";
        } else if ((foundWifi || foundBluetoothAdapter) && noRssControl) {
            log << "[Wireless IRQ] ⓘ Active wireless adapter detected, but RSS control is unavailable\n";
        } else if ((foundWifi || foundBluetoothAdapter) && hasWarn) {
            log << "[Wireless IRQ] ⚠ Active wireless adapter found, but RSS rebalance did not apply\n";
            log << "[Wireless IRQ] Common causes: admin privileges required or driver does not expose RSS controls\n";
        } else if (noWireless) {
            log << "[Wireless IRQ] ⓘ No active WiFi or Bluetooth network adapters found; nothing to rebalance\n";
        } else if (isEmpty) {
            log << "[Wireless IRQ] ⚠ PowerShell returned no output; wireless rebalance skipped\n";
        } else {
            log << "[Wireless IRQ] ? Unable to determine wireless rebalance status from output\n";
        }

        if (bluetoothPnpPolicyVisible) {
            log << "[Wireless IRQ] ⓘ Bluetooth PnP affinity-policy path is visible on this host, but automatic registry override is not applied here\n";
        } else if (bluetoothPnpNoPolicy) {
            log << "[Wireless IRQ] ⓘ Bluetooth controller path does not expose a standard interrupt-affinity policy; only Bluetooth PAN/RSS can be rebalanced here\n";
        }

        logOut = log.str();
        return true;
    }

    // Isolate an Ethernet adapter for EtherCAT exclusive use
    // This disables all TCP/IP protocol bindings to prevent other software from interfering
    // with EtherCAT communication. Only Npcap/WinPcap driver remains enabled.
    // 
    // adapterName: friendly name or description of the adapter to isolate
    // restore: if true, re-enable all bindings instead of disabling them
    // Returns: true if operation succeeded, false on error
    bool IsolateEtherCATAdapter(const std::string &adapterName, bool restore, std::string &logOut) {
        logOut.clear();
        if (adapterName.empty()) {
            logOut = "[Adapter Isolation] Adapter name is empty, skipping\n";
            return false;
        }

        std::string cleanName = StripAdapterGuidSuffix(adapterName);
        std::ostringstream log;
        log << "[Adapter Isolation] " << (restore ? "Restoring" : "Isolating") 
            << " adapter: " << cleanName << "\n";

        // Extract adapter name from various formats
        std::string friendlyName = cleanName;
        
        // Try to extract from "Network adapter 'NAME' on local host" format
        size_t quote1 = cleanName.find('\'');
        size_t quote2 = cleanName.rfind('\'');
        if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
            friendlyName = cleanName.substr(quote1 + 1, quote2 - quote1 - 1);
            log << "[Adapter Isolation] Extracted adapter name: " << friendlyName << "\n";
        }
        
        // Trim leading/trailing spaces
        while (!friendlyName.empty() && friendlyName.front() == ' ') {
            friendlyName.erase(0, 1);
        }
        while (!friendlyName.empty() && friendlyName.back() == ' ') {
            friendlyName.pop_back();
        }

        if (friendlyName.empty()) {
            log << "[Adapter Isolation] Failed to extract valid adapter name\n";
            logOut = log.str();
            return false;
        }

        // Quick check: Skip isolation for TwinCAT adapters
        // TwinCAT adapters use their own driver (TwinCAT-Intel PCI Ethernet Adapter)
        // which already bypasses Windows network stack. Disabling bindings via PowerShell
        // is redundant and takes ~10 seconds.
        std::string lowerName = friendlyName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find("twincat") != std::string::npos) {
            log << "[Adapter Isolation] ⓘ Detected TwinCAT adapter - isolation redundant by design\n";
            log << "[Adapter Isolation] TwinCAT manages real-time networking at kernel level\n";
            log << "[Adapter Isolation] Skipping PowerShell binding operations to speed up startup\n";
            logOut = log.str();
            return true;
        }

        // Create temp file for output
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"iso", 0, tempFile);

        // Convert adapter name to wide string and escape for PowerShell
        std::wstring adapterNameW = CoreHelpers::Utf8ToWide(friendlyName);
        std::wstring escapedName;
        for (wchar_t c : adapterNameW) {
            if (c == L'\'') {
                escapedName += L"''"; // PowerShell escape: ' becomes ''
            } else {
                escapedName += c;
            }
        }

        // Build PowerShell command to disable/enable network bindings
        // Components to unbind (prevents protocol-level interference with EtherCAT):
        // - ms_tcpip  (TCP/IPv4) — ARP/DHCP/background traffic can cause WKC jitter
        // - ms_tcpip6 (TCP/IPv6)
        // - ms_msclient (Client for Microsoft Networks)
        // - ms_server (File and Printer Sharing)
        // - ms_pacer (QoS Packet Scheduler)
        // - ms_lltdio (Link-Layer Topology Discovery Mapper I/O)
        // - ms_rspndr (Link-Layer Topology Discovery Responder)
        // Npcap keep-alive handle (NpcapKeepAliveOpen) prevents NIC power-saving idle
        // that would otherwise occur when all protocols are unbound.
        
        std::wstring action = restore ? L"Enable" : L"Disable";
        std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        psCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        psCmd += L"$adapterName = '";
        psCmd += escapedName;
        psCmd += L"'; ";
        psCmd += L"$adapter = Get-NetAdapter | Where-Object { $_.Name -eq $adapterName -or $_.InterfaceDescription -like ('*' + $adapterName + '*') } | Select-Object -First 1; ";
        psCmd += L"if ($adapter) { ";
        psCmd += L"$ifName = $adapter.Name; ";
        psCmd += L"Write-Output ('Found adapter: ' + $ifName + ' (' + $adapter.InterfaceDescription + ')'); ";
        psCmd += L"Write-Output 'Unbinding all protocols including TCP/IPv4 to prevent EtherCAT interference'; ";
        
        // Disable/Enable each binding (now includes ms_tcpip)
        const wchar_t* components[] = {
            L"ms_tcpip", L"ms_tcpip6", L"ms_msclient", L"ms_server", L"ms_pacer", L"ms_lltdio", L"ms_rspndr"
        };
        const wchar_t* componentNames[] = {
            L"TCP/IPv4", L"TCP/IPv6", L"Microsoft Network Client", L"File and Printer Sharing", 
            L"QoS Packet Scheduler", L"LLTD Mapper I/O", L"LLTD Responder"
        };
        
        for (int i = 0; i < 7; ++i) {
            psCmd += action;
            psCmd += L"-NetAdapterBinding -Name $ifName -ComponentID ";
            psCmd += components[i];
            psCmd += L" -ErrorAction SilentlyContinue; ";
            psCmd += L"Write-Output ('";
            psCmd += action;
            psCmd += L"d ";
            psCmd += componentNames[i];
            psCmd += L"'); ";
        }

        
        // Also disable NetBIOS over TCP/IP via registry
        if (!restore) {
            psCmd += L"$guid = $adapter.InterfaceGuid; ";
            psCmd += L"$regPath = 'HKLM:\\SYSTEM\\CurrentControlSet\\Services\\NetBT\\Parameters\\Interfaces\\Tcpip_' + $guid; ";
            psCmd += L"if (Test-Path $regPath) { ";
            psCmd += L"Set-ItemProperty -Path $regPath -Name 'NetbiosOptions' -Value 2 -Type DWord -ErrorAction SilentlyContinue; ";
            psCmd += L"Write-Output 'Disabled NetBIOS over TCP/IP'; ";
            psCmd += L"} ";
        }
        
        // Set network profile to Public for stricter firewall (only when isolating)
        if (!restore) {
            psCmd += L"Set-NetConnectionProfile -InterfaceIndex $adapter.ifIndex -NetworkCategory Public -ErrorAction SilentlyContinue; ";
            psCmd += L"Write-Output 'Set network profile to Public'; ";
        }
        
        // Verify Npcap binding status
        psCmd += L"$npcap = Get-NetAdapterBinding -Name $ifName -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -like '*Npcap*' -or $_.DisplayName -like '*WinPcap*' }; ";
        psCmd += L"if ($npcap) { ";
        psCmd += L"if (-not $npcap.Enabled) { ";
        psCmd += L"Enable-NetAdapterBinding -Name $ifName -ComponentID $npcap.ComponentID -ErrorAction SilentlyContinue; ";
        psCmd += L"Write-Output 'Re-enabled Npcap driver'; ";
        psCmd += L"} else { ";
        psCmd += L"Write-Output ('Npcap driver is enabled: ' + $npcap.DisplayName); ";
        psCmd += L"} ";
        psCmd += L"} else { ";
        psCmd += L"Write-Output 'WARNING: Npcap/WinPcap driver not found'; ";
        psCmd += L"} ";
        
        psCmd += L"Write-Output 'SUCCESS: Adapter isolation completed'; ";
        psCmd += L"} else { ";
        psCmd += L"Write-Output 'ERROR: No matching adapter found'; ";
        psCmd += L"Write-Output 'Available adapters:'; ";
        psCmd += L"Get-NetAdapter | Where-Object { $_.Status -eq 'Up' } | ForEach-Object { Write-Output ('  - ' + $_.Name + ' (' + $_.InterfaceDescription + ')') }; ";
        psCmd += L"}\"";

        log << "[Adapter Isolation] Executing PowerShell command...\n";

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        // Use cmd.exe /c to properly handle redirection
        // Also set output encoding to OEM for proper ANSI reading
        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & ";
        cmdLine += psCmd;
        cmdLine += L" > \"";
        cmdLine += tempFile;
        cmdLine += L"\" 2>&1\"";

        bool success = false;
        
        if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 30000); // 30 second timeout
            
            if (waitResult == WAIT_TIMEOUT) {
                log << "[Adapter Isolation] Warning: PowerShell command timed out\n";
                TerminateProcess(pi.hProcess, 1);
            }
            
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log << "[Adapter Isolation] Error: Failed to create PowerShell process (error " 
                << GetLastError() << ")\n";
            log << "[Adapter Isolation] Note: This operation requires administrator privileges\n";
            logOut = log.str();
            DeleteFileW(tempFile);
            return false;
        }

        // Read output from temp file
        std::string output;
        HANDLE hFile = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD fileSize = GetFileSize(hFile, NULL);
            log << "[Adapter Isolation] Output file size: " << fileSize << " bytes\n";
            
            char buffer[8192];
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                output = buffer;
                // Clean up output
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' ')) {
                    output.pop_back();
                }
                if (!output.empty()) {
                    log << "[Adapter Isolation] PowerShell Output:\n" << output << "\n";
                } else {
                    log << "[Adapter Isolation] PowerShell returned empty output\n";
                }
            } else {
                log << "[Adapter Isolation] Failed to read output file (error " << GetLastError() << ")\n";
            }
            CloseHandle(hFile);
        } else {
            log << "[Adapter Isolation] Failed to open output file (error " << GetLastError() << ")\n";
        }
        DeleteFileW(tempFile);

        // Check result - be more lenient: if we see "Found adapter" and any "Disabled" messages, consider it success
        bool hasSuccess = (output.find("SUCCESS:") != std::string::npos);
        bool hasFoundAdapter = (output.find("Found adapter:") != std::string::npos);
        bool hasDisabled = (output.find("Disabled") != std::string::npos);
        bool hasError = (output.find("ERROR:") != std::string::npos);
        bool npcapMissing = (output.find("WARNING: Npcap") != std::string::npos);
        bool npcapEnabled = (output.find("Npcap driver is enabled") != std::string::npos);
        
        // Consider success if we found adapter and either got SUCCESS or disabled something
        success = hasSuccess || (hasFoundAdapter && (hasDisabled || npcapEnabled));

        if (success) {
            log << "[Adapter Isolation] ✓ " << (restore ? "Restoration" : "Isolation") << " completed successfully\n";
            if (!restore) {
                log << "[Adapter Isolation] ✓ Network bindings disabled, preventing interference\n";
            }
        } else if (hasError) {
            log << "[Adapter Isolation] ✗ Operation failed - check PowerShell output above\n";
        } else if (output.empty()) {
            log << "[Adapter Isolation] ⚠ No output from PowerShell - command may have failed silently\n";
        } else {
            log << "[Adapter Isolation] ⚠ Unexpected output - isolation status unclear\n";
        }
        
        if (npcapMissing) {
            log << "[Adapter Isolation] ⚠ Warning: Npcap/WinPcap driver not detected\n";
            log << "[Adapter Isolation] EtherCAT communication requires Npcap to be installed\n";
        }

        logOut = log.str();
        return success;
    }

    // Disable NIC interrupt moderation and latency-inducing offloads for
    // EtherCAT real-time operation.  Interrupt moderation is the single
    // largest controllable source of NIC IRQ latency on Windows — it can
    // delay interrupt delivery by 50-200 us by batching packets.
    //
    // This function uses PowerShell Set-NetAdapterAdvancedProperty to set
    // driver-level advanced properties.  Property names vary by NIC vendor;
    // we try Intel I210/I225 names first, then generic names as fallback.
    //
    // Individual property failures are non-fatal — the function returns
    // true as long as the PowerShell pipeline executed.
    bool OptimizeNicForRealtime(const std::string &adapterName, std::string &logOut) {
        logOut.clear();
        if (adapterName.empty()) {
            logOut = "[NIC Realtime] Adapter name is empty, skipping\n";
            return true;
        }

        std::ostringstream log;
        log << "[NIC Realtime] Tuning advanced properties for adapter: " << adapterName << "\n";

        // Extract friendly name (same logic as IsolateEtherCATAdapter)
        std::string friendlyName = adapterName;
        size_t quote1 = adapterName.find('\'');
        size_t quote2 = adapterName.rfind('\'');
        if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
            friendlyName = adapterName.substr(quote1 + 1, quote2 - quote1 - 1);
            log << "[NIC Realtime] Extracted adapter name: " << friendlyName << "\n";
        }
        while (!friendlyName.empty() && friendlyName.front() == ' ') friendlyName.erase(0, 1);
        while (!friendlyName.empty() && friendlyName.back() == ' ') friendlyName.pop_back();

        // Strip trailing " [XXXXXXXX]" suffix appended by FindAdapterDisplayName
        // (e.g. "Intel(R) I210 ... #2 [12345678]" → "Intel(R) I210 ... #2")
        {
            auto bracketPos = friendlyName.rfind(" [");
            if (bracketPos != std::string::npos) {
                std::string tail = friendlyName.substr(bracketPos);
                if (tail.size() == 11 && tail.front() == ' ' && tail[1] == '[' && tail.back() == ']') {
                    friendlyName = friendlyName.substr(0, bracketPos);
                    log << "[NIC Realtime] Stripped adapter ID suffix: " << friendlyName << "\n";
                }
            }
        }

        if (friendlyName.empty()) {
            log << "[NIC Realtime] Failed to extract valid adapter name\n";
            logOut = log.str();
            return true;
        }

        // Skip TwinCAT adapters (kernel-level real-time, no user tuning)
        std::string lowerName = friendlyName;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        if (lowerName.find("twincat") != std::string::npos) {
            log << "[NIC Realtime] TwinCAT adapter — driver-level tuning not applicable\n";
            logOut = log.str();
            return true;
        }

        // Build PowerShell command to set NIC advanced properties.
        // We set both Intel-specific and generic property names so the
        // script works across NIC vendors (Intel I210/I225, Realtek, etc).
        std::wstring adapterNameW = CoreHelpers::Utf8ToWide(friendlyName);
        std::wstring escapedName;
        for (wchar_t c : adapterNameW) {
            if (c == L'\'') escapedName += L"''";
            else escapedName += c;
        }

        std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        psCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        psCmd += L"$name = '"; psCmd += escapedName; psCmd += L"'; ";
        psCmd += L"$adapter = Get-NetAdapter | Where-Object { $_.Name -eq $name -or $_.InterfaceDescription -like ('*' + $name + '*') } | Select-Object -First 1; ";
        psCmd += L"if (-not $adapter) { Write-Output 'ERROR: Adapter not found'; exit 1 }; ";
        psCmd += L"$n = $adapter.Name; ";
        psCmd += L"Write-Output ('Found: ' + $n + ' (' + $adapter.InterfaceDescription + ')'); ";

        // Helper: try DisplayName first, then RegistryKeyword (with & without *).
        // Many NIC drivers use RegistryKeyword without the NDIS "*" prefix
        // (e.g. "DMACoalescing" not "*DMACoalescing").  We try all variants.
        // When -DisplayValue fails due to locale (e.g. "Disabled" vs "停用"),
        // we probe the property's ValidDisplayValues and pick the off-value.
        psCmd += L"function Set-NicProp { param($PropNames, $Value); ";
        psCmd += L"  foreach ($pn in $PropNames) { ";
        psCmd += L"    $prop = $null; ";
        psCmd += L"    $prop = Get-NetAdapterAdvancedProperty -Name $n -DisplayName $pn -ErrorAction SilentlyContinue; ";
        psCmd += L"    if (-not $prop) { $prop = Get-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $pn -ErrorAction SilentlyContinue }; ";
        psCmd += L"    if (-not $prop -and $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $pn.Substring(1) -ErrorAction SilentlyContinue }; ";
        psCmd += L"    if (-not $prop -and -not $pn.StartsWith('*')) { $prop = Get-NetAdapterAdvancedProperty -Name $n -RegistryKeyword ('*' + $pn) -ErrorAction SilentlyContinue }; ";
        psCmd += L"    if ($prop) { ";
        psCmd += L"      $setOk = $false; ";
        // Try English DisplayValue first
        psCmd += L"      try { ";
        psCmd += L"        Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $prop.RegistryKeyword -DisplayValue $Value -ErrorAction Stop; ";
        psCmd += L"        Write-Output ('  SET: ''' + $pn + ''' = ''' + $Value + ''''); return ";
        psCmd += L"      } catch { ";
        // Locale fallback: pick the "off/disabled" value from ValidDisplayValues
        psCmd += L"        if ($prop.ValidDisplayValues) { ";
        psCmd += L"          $offVal = $prop.ValidDisplayValues | Where-Object { $_ -match 'Disab|停用|關閉|Off' } | Select-Object -First 1; ";
        psCmd += L"          if (-not $offVal) { $offVal = $prop.ValidDisplayValues[0] }; ";
        psCmd += L"          try { ";
        psCmd += L"            Set-NetAdapterAdvancedProperty -Name $n -RegistryKeyword $prop.RegistryKeyword -DisplayValue $offVal -ErrorAction Stop; ";
        psCmd += L"            Write-Output ('  SET: ''' + $pn + ''' = ''' + $offVal + ''' (localized)'); return ";
        psCmd += L"          } catch { Write-Output ('  FAIL: ''' + $pn + ''' = ''' + $Value + ''' - ' + $_.Exception.Message) }; ";
        psCmd += L"        } else { Write-Output ('  FAIL: ''' + $pn + ''' = ''' + $Value + ''' - ' + $_.Exception.Message) }; ";
        psCmd += L"      }; ";
        psCmd += L"    }; ";
        psCmd += L"  }; ";
        psCmd += L"  Write-Output ('  SKIP: no match for ''' + [string]::Join(''|'', $PropNames) + ''''); ";
        psCmd += L"}; ";

        // ── Interrupt Moderation: OFF (CRITICAL for EtherCAT) ──
        // Intel I210 has TWO separate properties:
        //   1. "Interrupt Moderation"     → Enabled/Disabled
        //   2. "Interrupt Moderation Rate" → Adaptive/Low/Medium/High/Off
        // Both must be turned off for minimum latency.
        // Realtek: "Interrupt Moderation"
        psCmd += L"Write-Output '--- Interrupt Moderation ---'; ";
        psCmd += L"Set-NicProp @('Interrupt Moderation','*InterruptModeration') 'Disabled'; ";
        psCmd += L"Set-NicProp @('Interrupt Moderation Rate','ITR') 'Off'; ";

        // ── Rx/Tx Buffers: MAX ──
        // Larger buffers reduce NIC internal descriptor starvation
        psCmd += L"Write-Output '--- Receive/Transmit Buffers ---'; ";
        psCmd += L"Set-NicProp @('Receive Buffers','*ReceiveBuffers') '2048'; ";
        psCmd += L"Set-NicProp @('Transmit Buffers','*TransmitBuffers') '2048'; ";

        // ── Flow Control: OFF ──
        // Flow control pause frames can delay Tx, bad for deterministic cycle
        psCmd += L"Write-Output '--- Flow Control ---'; ";
        psCmd += L"Set-NicProp @('Flow Control','*FlowControl') 'Disabled'; ";

        // ── Energy Efficient Ethernet: OFF ──
        // EEE enters low-power states that add wake-up latency
        psCmd += L"Write-Output '--- Energy Efficient Ethernet ---'; ";
        psCmd += L"Set-NicProp @('Energy Efficient Ethernet','EEELinkAdvertisement','*EEEControl') 'Disabled'; ";

        // ── Reduce Speed On Power Down: OFF ──
        // Prevents NIC from dropping link speed on system idle, which causes
        // link flap and WKC=0 during EtherCAT operation.
        psCmd += L"Write-Output '--- ReduceSpeedOnPowerDown ---'; ";
        psCmd += L"Set-NicProp @('ReduceSpeedOnPowerDown') 'Disabled'; ";

        // ── Offload features: OFF (each adds processing latency) ──
        psCmd += L"Write-Output '--- Offload Features ---'; ";
        psCmd += L"Set-NicProp @('Large Send Offload (IPv4)','*LsoV1IPv4','Large Send Offload V1 (IPv4)') 'Disabled'; ";
        psCmd += L"Set-NicProp @('Large Send Offload V2 (IPv4)','*LsoV2IPv4') 'Disabled'; ";
        psCmd += L"Set-NicProp @('Large Send Offload V2 (IPv6)','*LsoV2IPv6') 'Disabled'; ";
        psCmd += L"Set-NicProp @('TCP Checksum Offload (IPv4)','*TCPChecksumOffloadIPv4','*TCPUDPChecksumOffloadIPv4') 'Disabled'; ";
        psCmd += L"Set-NicProp @('TCP Checksum Offload (IPv6)','*TCPChecksumOffloadIPv6','*TCPUDPChecksumOffloadIPv6') 'Disabled'; ";
        psCmd += L"Set-NicProp @('UDP Checksum Offload (IPv4)','*UDPChecksumOffloadIPv4') 'Disabled'; ";
        psCmd += L"Set-NicProp @('UDP Checksum Offload (IPv6)','*UDPChecksumOffloadIPv6') 'Disabled'; ";

        // ── IPv4 Checksum Offload (Intel I210 dedicated property) ──
        // Separate from TCP/UDP checksum offload — full IP-layer checksum hw offload
        psCmd += L"Write-Output '--- IPv4 Checksum Offload ---'; ";
        psCmd += L"Set-NicProp @('IPv4 Checksum Offload','*IPChecksumOffloadIPv4') 'Disabled'; ";

        // ── ARP/NS Offload: OFF (not needed for raw EtherCAT) ──
        psCmd += L"Write-Output '--- ARP/NS Offload ---'; ";
        psCmd += L"Set-NicProp @('ARP Offload','*PMARPOffload') 'Disabled'; ";
        psCmd += L"Set-NicProp @('NS Offload','*PMNSOffload') 'Disabled'; ";

        // ── Gigabit Master/Slave: Force Master ──
        // Master mode avoids auto-negotiation latency on link
        psCmd += L"Write-Output '--- Gigabit Master Mode ---'; ";
        psCmd += L"Set-NicProp @('Gigabit Master Slave Mode','PrimarySecondary','*MasterSlave') 'Force Master Mode'; ";

        // ── Priority & VLAN: OFF (not needed for raw EtherCAT) ──
        // Intel I210: "Packet Priority & VLAN"
        // Realtek: "Priority & VLAN"
        psCmd += L"Write-Output '--- Priority & VLAN ---'; ";
        psCmd += L"Set-NicProp @('Packet Priority & VLAN','Priority & VLAN','*PriorityVLANTag') 'Packet Priority & VLAN Disabled'; ";

        // ── Wake on LAN: OFF (prevents NIC from entering low-power states) ──
        psCmd += L"Write-Output '--- Wake on LAN ---'; ";
        psCmd += L"Set-NicProp @('Wake on Magic Packet','*WakeOnMagicPacket') 'Disabled'; ";
        psCmd += L"Set-NicProp @('Wake on Pattern Match','*WakeOnPattern') 'Disabled'; ";

        // ── DMA Coalescing: OFF (batches DMA transfers, adds latency) ──
        psCmd += L"Write-Output '--- DMA Coalescing ---'; ";
        psCmd += L"Set-NicProp @('DMA Coalescing','DMACoalescing') 'Disabled'; ";

        // ── Log Link State Event: OFF (event logging overhead during operation) ──
        psCmd += L"Write-Output '--- Link State Event Logging ---'; ";
        psCmd += L"Set-NicProp @('Log Link State Event','LogLinkStateEvent') 'Disabled'; ";

        psCmd += L"Write-Output 'SUCCESS: NIC real-time tuning completed'; ";
        psCmd += L"\"";

        // Execute PowerShell
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"rtn", 0, tempFile);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & ";
        cmdLine += psCmd;
        cmdLine += L" > \"";
        cmdLine += tempFile;
        cmdLine += L"\" 2>&1\"";

        log << "[NIC Realtime] Executing PowerShell (timeout=10s)...\n";

        if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW,
                           NULL, NULL, &si, &pi)) {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 10000);
            if (waitResult == WAIT_TIMEOUT) {
                log << "[NIC Realtime] Warning: PowerShell timed out\n";
                TerminateProcess(pi.hProcess, 1);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log << "[NIC Realtime] Error: CreateProcess failed (err="
                << GetLastError() << ")\n";
            logOut = log.str();
            DeleteFileW(tempFile);
            return false;
        }

        // Read output
        std::string output;
        HANDLE hFile = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buffer[8192];
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                output = buffer;
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                    output.pop_back();
                if (!output.empty()) {
                    log << "[NIC Realtime] PowerShell Output:\n" << output << "\n";
                }
            }
            CloseHandle(hFile);
        }
        DeleteFileW(tempFile);

        // Determine success
        bool hasSuccess = (output.find("SUCCESS:") != std::string::npos);
        bool hasFound = (output.find("Found:") != std::string::npos);
        bool hasSet = (output.find("SET:") != std::string::npos);
        bool hasError = (output.find("ERROR:") != std::string::npos);

        if (hasSuccess || (hasFound && hasSet)) {
            log << "[NIC Realtime] ✓ Advanced properties tuned for real-time EtherCAT\n";
            log << "[NIC Realtime] ✓ Interrupt moderation OFF, buffers maximized, offloads disabled\n";
            log << "[NIC Realtime] Settings take effect immediately; reboot recommended for full effect\n";
        } else if (hasError) {
            log << "[NIC Realtime] ✗ Some settings may have failed — review output above\n";
        } else {
            log << "[NIC Realtime] ⚠ PowerShell returned no actionable output\n";
            log << "[NIC Realtime] (Admin privileges required; NIC may not expose these properties)\n";
        }

        logOut = log.str();
        return true;
    }

    // Apply Npcap immediate mode + mintocopy to an already-open pcap handle.
    // Npcap (and WinPcap) default to batch-mode delivery: the kernel driver
    // waits until a timer expires or the buffer fills before copying packets
    // to userspace.  For EtherCAT 500us cycles this adds ~1ms of unnecessary
    // latency.  pcap_set_immediate_mode(1) tells the driver to deliver each
    // packet immediately; pcap_setmintocopy(0) sets the minimum bytes to copy
    // to 1 (instead of the default 16KB).
    //
    // ── Disable RSS on a Windows network adapter ──
    // Walks the network class registry keys, matches by friendly name, and sets *RSS=0.
    bool DisableAdapterRss(const std::string &friendlyName, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[RSS] Disabling RSS on adapter: " << friendlyName << "\n";

        if (friendlyName.empty()) {
            log << "[RSS] ERROR: empty friendly name\n";
            logOut = log.str();
            return false;
        }

        // First, resolve friendly name → adapter GUID via GetAdaptersAddresses
        std::string adapterGuid;
        {
            ULONG outBufLen = 0;
            DWORD rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                             NULL, NULL, &outBufLen);
            if (rc != ERROR_BUFFER_OVERFLOW) {
                log << "[RSS] ERROR: GetAdaptersAddresses size query failed (rc=" << rc << ")\n";
                logOut = log.str();
                return false;
            }
            std::vector<BYTE> buffer(outBufLen);
            IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            rc = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_INTERFACES,
                                      NULL, adapters, &outBufLen);
            if (rc != NO_ERROR) {
                log << "[RSS] ERROR: GetAdaptersAddresses failed (rc=" << rc << ")\n";
                logOut = log.str();
                return false;
            }
            std::wstring wFriendly = Utf8ToWide(friendlyName);
            for (IP_ADAPTER_ADDRESSES* aa = adapters; aa; aa = aa->Next) {
                if (aa->FriendlyName && wFriendly == aa->FriendlyName) {
                    if (aa->AdapterName) {
                        adapterGuid = aa->AdapterName;
                    }
                    break;
                }
            }
        }

        if (adapterGuid.empty()) {
            log << "[RSS] WARNING: adapter GUID not found for friendly name, trying registry scan\n";
        }

        // Walk network adapter class registry keys: {4D36E972-E325-11CE-BFC1-08002BE10318}\NNNN
        static const wchar_t kNetClass[] = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
        HKEY hClassKey = nullptr;
        LSTATUS lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kNetClass, 0, KEY_READ, &hClassKey);
        if (lr != ERROR_SUCCESS) {
            log << "[RSS] ERROR: cannot open network class registry key (err=" << lr << ")\n";
            logOut = log.str();
            return false;
        }

        bool found = false;
        bool alreadyDisabled = false;
        DWORD idx = 0;
        wchar_t subKeyName[256];
        while (RegEnumKeyW(hClassKey, idx++, subKeyName, _countof(subKeyName)) == ERROR_SUCCESS) {
            HKEY hDevKey = nullptr;
            if (RegOpenKeyExW(hClassKey, subKeyName, 0, KEY_READ | KEY_WRITE, &hDevKey) != ERROR_SUCCESS) {
                continue;
            }

            // Try matching by GUID first, then by friendly name
            bool match = false;
            if (!adapterGuid.empty()) {
                wchar_t guidBuf[256] = {0};
                DWORD guidSize = sizeof(guidBuf);
                if (RegQueryValueExW(hDevKey, L"NetCfgInstanceId", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(guidBuf), &guidSize) == ERROR_SUCCESS) {
                    if (adapterGuid == WideToUtf8(std::wstring(guidBuf))) {
                        match = true;
                    }
                }
            } else {
                // Fallback: match by DriverDesc (friendly name)
                wchar_t descBuf[256] = {0};
                DWORD descSize = sizeof(descBuf);
                if (RegQueryValueExW(hDevKey, L"DriverDesc", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(descBuf), &descSize) == ERROR_SUCCESS) {
                    std::wstring wDesc(descBuf);
                    if (wDesc == Utf8ToWide(friendlyName)) {
                        match = true;
                    }
                }
            }

            if (match) {
                found = true;
                // Read current *RSS value
                wchar_t rssBuf[32] = {0};
                DWORD rssSize = sizeof(rssBuf);
                DWORD rssType = REG_SZ;
                bool currentRssEnabled = true;
                if (RegQueryValueExW(hDevKey, L"*RSS", nullptr, &rssType,
                                     reinterpret_cast<LPBYTE>(rssBuf), &rssSize) == ERROR_SUCCESS) {
                    std::wstring rssVal(rssBuf);
                    currentRssEnabled = (rssVal != L"0");
                }

                if (!currentRssEnabled) {
                    alreadyDisabled = true;
                    log << "[RSS] ✓ already disabled on " << friendlyName << " (key=" << WideToUtf8(std::wstring(subKeyName)) << ")\n";
                } else {
                    const wchar_t newVal[] = L"0";
                    lr = RegSetValueExW(hDevKey, L"*RSS", 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(newVal),
                                        static_cast<DWORD>(sizeof(newVal)));
                    if (lr == ERROR_SUCCESS) {
                        log << "[RSS] ✓ disabled *RSS on " << friendlyName << " (key=" << WideToUtf8(std::wstring(subKeyName)) << ")\n";
                    } else {
                        log << "[RSS] ERROR: RegSetValueEx(*RSS) failed (err=" << lr << ") on key " << WideToUtf8(std::wstring(subKeyName)) << "\n";
                    }
                }
                RegCloseKey(hDevKey);
                break;
            }
            RegCloseKey(hDevKey);
        }
        RegCloseKey(hClassKey);

        if (!found) {
            log << "[RSS] WARNING: adapter not found in registry — cannot verify RSS (not an error)\n";
            logOut = log.str();
            return false;
        }

        logOut = log.str();
        return true;
    }

    // ── Lock NIC hardware for real-time EtherCAT ──
    // Disables power management (D3 sleep), interrupt moderation, EEE, DMA
    // coalescing, and flow control via direct registry write.  Works even
    // when TCP/IP is unbound — resolves WKC instability caused by NIC hardware
    // reverting to power-saving defaults.
    bool LockNicHardwareRealtime(const std::string &ifname, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[NIC Lock] Locking hardware for: " << ifname << "\n";

        if (ifname.empty()) {
            log << "[NIC Lock] ERROR: empty ifname\n";
            logOut = log.str();
            return false;
        }

        // ── Extract GUID from rpcap://\Device\NPF_{GUID} or \Device\NPF_{GUID} ──
        std::string adapterGuid;
        // Strip optional rpcap:// prefix (Npcap remote/local capture prefix)
        std::string cleanName = ifname;
        const std::string rpcapPrefix = "rpcap://";
        if (cleanName.size() > rpcapPrefix.size() &&
            cleanName.compare(0, rpcapPrefix.size(), rpcapPrefix) == 0) {
            cleanName = cleanName.substr(rpcapPrefix.size());
        }
        const std::string npfPrefix = "\\Device\\NPF_{";
        if (cleanName.size() > npfPrefix.size() &&
            cleanName.compare(0, npfPrefix.size(), npfPrefix) == 0 &&
            cleanName.back() == '}') {
            // "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}"
            adapterGuid = cleanName.substr(npfPrefix.size());
            if (!adapterGuid.empty() && adapterGuid.back() == '}')
                adapterGuid.pop_back();
        }
        if (adapterGuid.empty()) {
            log << "[NIC Lock] ERROR: cannot extract GUID from ifname\n";
            logOut = log.str();
            return false;
        }
        log << "[NIC Lock] GUID=" << adapterGuid << "\n";

        // ── Resolve Npcap device description for DriverDesc fallback matching ──
        std::string npcapDesc;  // Npcap-level description, e.g. "Intel(R) I210 ... #2"
        {
            pcap_if_t* alldevs = nullptr;
            char errbuf[PCAP_ERRBUF_SIZE] = {0};
            if (pcap_findalldevs_ex(PCAP_SRC_IF_STRING, NULL, &alldevs, errbuf) != -1) {
                for (pcap_if_t* d = alldevs; d; d = d->next) {
                    std::string pname = d->name ? d->name : "";
                    if (pname == ifname) {
                        // Npcap description format: "Network adapter 'Intel(R) ... #2' on local host"
                        std::string raw = d->description ? d->description : "";
                        static const std::string prefix = "Network adapter '";
                        static const std::string suffix = "' on local host";
                        if (raw.size() > prefix.size() + suffix.size() &&
                            raw.compare(0, prefix.size(), prefix) == 0 &&
                            raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            npcapDesc = raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size());
                        } else {
                            npcapDesc = raw;
                        }
                        break;
                    }
                }
                pcap_freealldevs(alldevs);
            }
        }
        if (!npcapDesc.empty())
            log << "[NIC Lock] Npcap desc=" << npcapDesc << "\n";

        // ── Find adapter in network class registry ──
        static const wchar_t kNetClass[] =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}";
        HKEY hClassKey = nullptr;
        LSTATUS lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kNetClass, 0, KEY_READ, &hClassKey);
        if (lr != ERROR_SUCCESS) {
            log << "[NIC Lock] ERROR: cannot open network class key (err=" << lr << ")\n";
            logOut = log.str();
            return false;
        }

        bool found = false;
        DWORD idx = 0;
        wchar_t subKeyName[256];
        while (RegEnumKeyW(hClassKey, idx++, subKeyName, _countof(subKeyName)) == ERROR_SUCCESS) {
            HKEY hDevKey = nullptr;
            if (RegOpenKeyExW(hClassKey, subKeyName, 0, KEY_READ | KEY_WRITE, &hDevKey) != ERROR_SUCCESS)
                continue;

            // Match by NetCfgInstanceId (GUID) first, then by DriverDesc fallback
            wchar_t guidBuf[256] = {0};
            DWORD guidSize = sizeof(guidBuf);
            bool match = false;
            if (RegQueryValueExW(hDevKey, L"NetCfgInstanceId", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(guidBuf), &guidSize) == ERROR_SUCCESS) {
                std::string regGuid = WideToUtf8(std::wstring(guidBuf));
                if (_stricmp(regGuid.c_str(), adapterGuid.c_str()) == 0)
                    match = true;
            }
            // Fallback: match by DriverDesc against Npcap description
            if (!match && !npcapDesc.empty()) {
                wchar_t descBuf[256] = {0};
                DWORD descSize = sizeof(descBuf);
                if (RegQueryValueExW(hDevKey, L"DriverDesc", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(descBuf), &descSize) == ERROR_SUCCESS) {
                    std::wstring wDesc(descBuf);
                    std::string regDesc = WideToUtf8(wDesc);
                    // Case-insensitive partial match
                    std::string regLower(regDesc);
                    std::string npcapLower(npcapDesc);
                    std::transform(regLower.begin(), regLower.end(), regLower.begin(), ::tolower);
                    std::transform(npcapLower.begin(), npcapLower.end(), npcapLower.begin(), ::tolower);
                    if (regLower.find(npcapLower) != std::string::npos ||
                        npcapLower.find(regLower) != std::string::npos)
                        match = true;
                }
            }

            if (!match) { RegCloseKey(hDevKey); continue; }
            found = true;

            log << "[NIC Lock] Found adapter at: " << WideToUtf8(std::wstring(subKeyName)) << "\n";

            // ── Helper: set REG_DWORD value if different ──
            auto setDword = [&](const wchar_t* name, DWORD desired) {
                DWORD cur = 0, curSize = sizeof(cur);
                bool exists = (RegQueryValueExW(hDevKey, name, nullptr, nullptr,
                                                reinterpret_cast<LPBYTE>(&cur), &curSize) == ERROR_SUCCESS);
                if (!exists || cur != desired) {
                    lr = RegSetValueExW(hDevKey, name, 0, REG_DWORD,
                                        reinterpret_cast<const BYTE*>(&desired), sizeof(desired));
                    if (lr == ERROR_SUCCESS)
                        log << "[NIC Lock]   SET " << WideToUtf8(std::wstring(name))
                            << " = " << desired << (exists ? "" : " (created)") << "\n";
                    else
                        log << "[NIC Lock]   FAIL " << WideToUtf8(std::wstring(name)) << " (err=" << lr << ")\n";
                }
            };

            // ── Helper: set REG_SZ value (update if exists) ──
            auto setSz = [&](const wchar_t* name, const wchar_t* desired) {
                wchar_t cur[64] = {0};
                DWORD curSize = sizeof(cur);
                if (RegQueryValueExW(hDevKey, name, nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(cur), &curSize) == ERROR_SUCCESS) {
                    if (wcscmp(cur, desired) != 0) {
                        DWORD szBytes = static_cast<DWORD>((wcslen(desired) + 1) * sizeof(wchar_t));
                        lr = RegSetValueExW(hDevKey, name, 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(desired), szBytes);
                        if (lr == ERROR_SUCCESS)
                            log << "[NIC Lock]   SET " << WideToUtf8(std::wstring(name))
                                << " = " << WideToUtf8(std::wstring(desired)) << "\n";
                        else
                            log << "[NIC Lock]   FAIL " << WideToUtf8(std::wstring(name)) << " (err=" << lr << ")\n";
                    }
                }
            };

            // ── Helper: create-or-update REG_SZ value (creates if missing) ──
            auto setSzCreate = [&](const wchar_t* name, const wchar_t* desired) {
                wchar_t cur[64] = {0};
                DWORD curSize = sizeof(cur);
                bool exists = (RegQueryValueExW(hDevKey, name, nullptr, nullptr,
                                                reinterpret_cast<LPBYTE>(cur), &curSize) == ERROR_SUCCESS);
                if (!exists || wcscmp(cur, desired) != 0) {
                    DWORD szBytes = static_cast<DWORD>((wcslen(desired) + 1) * sizeof(wchar_t));
                    lr = RegSetValueExW(hDevKey, name, 0, REG_SZ,
                                        reinterpret_cast<const BYTE*>(desired), szBytes);
                    if (lr == ERROR_SUCCESS)
                        log << "[NIC Lock]   SET " << WideToUtf8(std::wstring(name))
                            << " = " << WideToUtf8(std::wstring(desired))
                            << (exists ? "" : " (created)") << "\n";
                    else
                        log << "[NIC Lock]   FAIL " << WideToUtf8(std::wstring(name)) << " (err=" << lr << ")\n";
                }
            };

            // ═══════════════════════════════════════════════════════
            // Power Management — prevent D3 sleep (critical for WKC)
            // ═══════════════════════════════════════════════════════
            setDword(L"PnPCapabilities", 0);
            setSz(L"EnablePME", L"0");

            // ═══════════════════════════════════════════════════════
            // Interrupt Moderation — disable IRQ batching (CRITICAL)
            // Intel I210: *InterruptModeration=REG_SZ "0", ITR=REG_SZ "0"
            // Use setSzCreate so values are written even if missing.
            // ═══════════════════════════════════════════════════════
            setSzCreate(L"*InterruptModeration", L"0");
            setSzCreate(L"ITR", L"0");

            // ═══════════════════════════════════════════════════════
            // EEE / Green Ethernet — disable link power saving
            // ═══════════════════════════════════════════════════════
            setSzCreate(L"EEELinkAdvertisement", L"0");
            setSzCreate(L"EnableGreenEthernet", L"0");

            // ═══════════════════════════════════════════════════════
            // DMA Coalescing, Flow Control — disable latency sources
            // ═══════════════════════════════════════════════════════
            setSzCreate(L"DMACoalescing", L"0");
            setSzCreate(L"*FlowControl", L"0");

            // ═══════════════════════════════════════════════════════
            // Miscellaneous power-saving / latency features
            // ═══════════════════════════════════════════════════════
            setSz(L"ReduceSpeedOnPowerDown", L"0");
            setSz(L"AutoDisableGigabit", L"0");
            setSz(L"WaitForLink", L"0");
            setSz(L"SelectiveSuspend", L"0");
            setSz(L"LogLinkStateEvent", L"0");

            RegCloseKey(hDevKey);
            break;
        }
        RegCloseKey(hClassKey);

        if (!found) {
            log << "[NIC Lock] WARNING: adapter GUID not found in registry\n";
            logOut = log.str();
            return false;
        }

        // Registry values are now set.  Intel I210 reads advanced properties
        // at driver load time, so changes will take effect on next reboot or
        // driver restart.  We intentionally do NOT trigger DICS_PROPCHANGE here
        // because restarting the NIC driver causes a brief link flap that can
        // disrupt slave discovery during ecx_config_init.

        log << "[NIC Lock] ✓ Hardware locked for real-time EtherCAT\n";
        logOut = log.str();
        return true;
    }


    // ═══════════════════════════════════════════════════════════════
    // Ensure TCP/IPv4 (ms_tcpip) is bound to the EtherCAT NIC.
    // On Win11, a NIC without TCP/IP binding causes NDIS miniport
    // idle → NIC micro-sleep → WKC instability.  This must run BEFORE
    // EnsureStaticIpOnEthercatNic so GetAdaptersAddresses can find it.
    // ═══════════════════════════════════════════════════════════════
    bool EnsureTcpIpv4BoundOnEthercatNic(const std::string &ifname, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[TCP/IP Bind] Ensuring TCP/IPv4 is bound for: " << ifname << "\n";

        if (ifname.empty()) {
            log << "[TCP/IP Bind] ERROR: empty ifname\n";
            logOut = log.str();
            return false;
        }

        // ── Resolve adapter description via Npcap ──
        std::string npcapDesc;
        {
            pcap_if_t* alldevs = nullptr;
            char errbuf[PCAP_ERRBUF_SIZE] = {0};
            if (pcap_findalldevs_ex(PCAP_SRC_IF_STRING, NULL, &alldevs, errbuf) != -1) {
                for (pcap_if_t* d = alldevs; d; d = d->next) {
                    std::string pname = d->name ? d->name : "";
                    // Match both raw and rpcap:// prefixed forms
                    if (pname == ifname) {
                        std::string raw = d->description ? d->description : "";
                        static const std::string prefix = "Network adapter '";
                        static const std::string suffix = "' on local host";
                        if (raw.size() > prefix.size() + suffix.size() &&
                            raw.compare(0, prefix.size(), prefix) == 0 &&
                            raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            npcapDesc = raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size());
                        } else {
                            npcapDesc = raw;
                        }
                        break;
                    }
                    // Also try stripping rpcap:// prefix
                    std::string cleanName = ifname;
                    const std::string rpcapPrefix = "rpcap://";
                    if (cleanName.size() > rpcapPrefix.size() &&
                        cleanName.compare(0, rpcapPrefix.size(), rpcapPrefix) == 0) {
                        cleanName = cleanName.substr(rpcapPrefix.size());
                    }
                    if (pname == cleanName) {
                        std::string raw = d->description ? d->description : "";
                        static const std::string prefix = "Network adapter '";
                        static const std::string suffix = "' on local host";
                        if (raw.size() > prefix.size() + suffix.size() &&
                            raw.compare(0, prefix.size(), prefix) == 0 &&
                            raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
                            npcapDesc = raw.substr(prefix.size(), raw.size() - prefix.size() - suffix.size());
                        } else {
                            npcapDesc = raw;
                        }
                        break;
                    }
                }
                pcap_freealldevs(alldevs);
            }
        }

        if (npcapDesc.empty()) {
            log << "[TCP/IP Bind] WARNING: cannot resolve Npcap description — using raw ifname\n";
            npcapDesc = ifname;
        }
        log << "[TCP/IP Bind] Adapter description: " << npcapDesc << "\n";

        // ── Escape for PowerShell ──
        std::wstring descW = Utf8ToWide(npcapDesc);
        std::wstring escaped;
        for (wchar_t c : descW) {
            if (c == L'\'') escaped += L"''";
            else escaped += c;
        }

        // ── PowerShell: find adapter and enable ms_tcpip ──
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"tcp", 0, tempFile);

        std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        psCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        psCmd += L"$desc = '" + escaped + L"'; ";
        // Find adapter by InterfaceDescription (Npcap friendly name), then by Name fallback
        psCmd += L"$adapter = Get-NetAdapter | Where-Object { ";
        psCmd += L"$_.InterfaceDescription -like ('*' + $desc + '*') -or ";
        psCmd += L"$_.Name -eq $desc ";
        psCmd += L"} | Select-Object -First 1; ";
        psCmd += L"if (-not $adapter) { ";
        psCmd += L"Write-Output 'ERROR: Adapter not found via Get-NetAdapter'; ";
        psCmd += L"} else { ";
        psCmd += L"Write-Output ('Found adapter: ' + $adapter.Name + ' (' + $adapter.InterfaceDescription + ')'); ";
        psCmd += L"$binding = Get-NetAdapterBinding -Name $adapter.Name -ComponentID ms_tcpip -ErrorAction SilentlyContinue; ";
        psCmd += L"if (-not $binding) { ";
        psCmd += L"Write-Output 'WARNING: ms_tcpip binding not found — may need driver reinstall'; ";
        psCmd += L"} elseif ($binding.Enabled) { ";
        psCmd += L"Write-Output 'SUCCESS: TCP/IPv4 already enabled'; ";
        psCmd += L"} else { ";
        psCmd += L"Enable-NetAdapterBinding -Name $adapter.Name -ComponentID ms_tcpip -ErrorAction Stop; ";
        psCmd += L"Write-Output 'SUCCESS: TCP/IPv4 binding enabled'; ";
        psCmd += L"} }";
        psCmd += L"\"";

        log << "[TCP/IP Bind] Executing PowerShell...\n";

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & ";
        cmdLine += psCmd;
        cmdLine += L" > \"";
        cmdLine += tempFile;
        cmdLine += L"\" 2>&1\"";

        bool success = false;
        if (CreateProcessW(NULL, &cmdLine[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            DWORD waitResult = WaitForSingleObject(pi.hProcess, 15000);
            if (waitResult == WAIT_TIMEOUT) {
                log << "[TCP/IP Bind] Warning: PowerShell timed out\n";
                TerminateProcess(pi.hProcess, 1);
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            log << "[TCP/IP Bind] ERROR: Cannot launch PowerShell (err=" << GetLastError() << ")\n";
            logOut = log.str();
            DeleteFileW(tempFile);
            return false;
        }

        // Read output
        std::string output;
        HANDLE hFile = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buffer[4096];
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
                buffer[bytesRead] = '\0';
                output = buffer;
                while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                    output.pop_back();
                if (!output.empty())
                    log << "[TCP/IP Bind] Output: " << output << "\n";
            }
            CloseHandle(hFile);
        }
        DeleteFileW(tempFile);

        success = (output.find("SUCCESS:") != std::string::npos);
        bool hasError = (output.find("ERROR:") != std::string::npos);

        if (success) {
            log << "[TCP/IP Bind] ✓ TCP/IPv4 binding confirmed\n";
        } else if (hasError) {
            log << "[TCP/IP Bind] ⚠ Could not enable TCP/IPv4 — continuing anyway\n";
            success = false;
        } else {
            log << "[TCP/IP Bind] ⚠ Unexpected output — status unclear\n";
        }

        logOut = log.str();
        return success;
    }


    // ═══════════════════════════════════════════════════════════════
    // Npcap persistent keep-alive: open a pcap handle that stays
    // open across NIC tuning → master init, keeping NDIS OID
    // processing active so the NIC hardware never enters idle.
    // Close with NpcapKeepAliveClose() after SOEM opens its handle.
    // ═══════════════════════════════════════════════════════════════
    static pcap_t* s_keepAliveHandle = nullptr;

    bool NpcapKeepAliveOpen(const std::string &ifname, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[Npcap KeepAlive] Opening persistent handle: " << ifname << "\n";

        // Close any previous stale handle
        if (s_keepAliveHandle) {
            pcap_close(s_keepAliveHandle);
            s_keepAliveHandle = nullptr;
            log << "[Npcap KeepAlive] Closed stale previous handle\n";
        }

        if (ifname.empty()) {
            log << "[Npcap KeepAlive] ERROR: empty ifname\n";
            logOut = log.str();
            return false;
        }

        if (EtherCATMaster::IsSimulationAdapterSelection(ifname)) {
            log << "[Npcap KeepAlive] Skipping — simulation adapter\n";
            logOut = log.str();
            return true;
        }

        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        // Open with PCAP_OPENFLAG_MAX_RESPONSIVENESS to avoid driver buffering.
        // Non-promiscuous — we only need the handle to keep NDIS OIDs flowing.
        s_keepAliveHandle = pcap_open(
            ifname.c_str(),
            65536,
            PCAP_OPENFLAG_MAX_RESPONSIVENESS,
            1,       // 1ms read timeout
            NULL,     // no remote auth
            errbuf);

        if (!s_keepAliveHandle) {
            log << "[Npcap KeepAlive] ERROR: pcap_open failed: "
                << (errbuf[0] ? errbuf : "unknown") << "\n";
            logOut = log.str();
            return false;
        }

        // Query stats to trigger immediate OID_GEN_STATISTICS
        struct pcap_stat stats;
        memset(&stats, 0, sizeof(stats));
        if (pcap_stats(s_keepAliveHandle, &stats) == 0) {
            log << "[Npcap KeepAlive] ✓ Handle open — NDIS OID active"
                << " (recv=" << stats.ps_recv << ")\n";
        }

        logOut = log.str();
        return true;
    }

    void NpcapKeepAliveClose(std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        if (s_keepAliveHandle) {
            pcap_close(s_keepAliveHandle);
            s_keepAliveHandle = nullptr;
            log << "[Npcap KeepAlive] ✓ Handle closed\n";
        } else {
            log << "[Npcap KeepAlive] ⓘ No handle to close\n";
        }
        logOut = log.str();
    }


    // ═══════════════════════════════════════════════════════════════
    // Ensure the EtherCAT NIC has a static IPv4 address.  Windows
    // keeps the NDIS miniport driver active only while a protocol
    // (TCP/IP) has an active IP binding — without it the NIC can
    // micro-sleep between PDO frames and cause WKC instability.
    //
    // Uses link-local range 169.254.x.x to avoid conflicts with any
    // corporate / home network.  AddIPAddress performs ARP probing
    // (RFC 5227) before assigning to prevent IP collisions.
    // ═══════════════════════════════════════════════════════════════
    bool EnsureStaticIpOnEthercatNic(const std::string &ifname, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[Static IP] Checking IP config for: " << ifname << "\n";

        if (ifname.empty()) {
            log << "[Static IP] ERROR: empty ifname\n";
            logOut = log.str();
            return false;
        }

        // ── 1. Extract Npcap GUID ──
        std::string npcapGuid;
        const std::string npfMarker = "\\Device\\NPF_{";
        auto markerPos = ifname.find(npfMarker);
        if (markerPos != std::string::npos) {
            std::string tail = ifname.substr(markerPos + npfMarker.size());
            if (!tail.empty() && tail.back() == '}') tail.pop_back();
            npcapGuid = tail;
        }
        if (npcapGuid.empty()) {
            log << "[Static IP] ERROR: cannot extract Npcap GUID\n";
            logOut = log.str();
            return false;
        }

        // ── 2. Find adapter via GetAdaptersAddresses, matching by Npcap GUID ──
        ULONG bufLen = 0;
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &bufLen)
            != ERROR_BUFFER_OVERFLOW) {
            log << "[Static IP] ERROR: GetAdaptersAddresses size query failed\n";
            logOut = log.str();
            return false;
        }
        std::vector<BYTE> buf(bufLen);
        auto* aaList = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, aaList, &bufLen)
            != NO_ERROR) {
            log << "[Static IP] ERROR: GetAdaptersAddresses failed\n";
            logOut = log.str();
            return false;
        }

        IP_ADAPTER_ADDRESSES* target = nullptr;
        for (auto* aa = aaList; aa; aa = aa->Next) {
            if (!aa->AdapterName) continue;
            std::string winGuid(aa->AdapterName);
            // Strip braces from Windows GUID
            if (!winGuid.empty() && winGuid.front() == '{' && winGuid.back() == '}')
                winGuid = winGuid.substr(1, winGuid.size() - 2);
            // The Npcap GUID and Windows GUID are different — match by the
            // Npcap name appearing IN the adapter's AdapterName field, or
            // fall back to checking if the Npcap GUID exists anywhere in the
            // registry's NetCfgInstanceId for this adapter (via separate scan).
            // For simplicity, we match by the last 8 chars of the Npcap GUID
            // appearing anywhere in the Windows GUID.
            std::string npcapShort = npcapGuid.size() >= 8
                ? npcapGuid.substr(npcapGuid.size() - 8) : npcapGuid;
            std::string winLower(winGuid);
            std::transform(winLower.begin(), winLower.end(), winLower.begin(), ::tolower);
            std::transform(npcapShort.begin(), npcapShort.end(), npcapShort.begin(), ::tolower);
            if (winLower.find(npcapShort) != std::string::npos) {
                target = aa;
                break;
            }
        }

        // Fallback: match by FriendlyName/Description containing I210/Gigabit
        if (!target) {
            for (auto* aa = aaList; aa; aa = aa->Next) {
                if (aa->IfType != IF_TYPE_ETHERNET_CSMACD) continue;
                std::wstring desc = aa->Description ? aa->Description : L"";
                std::wstring friendly = aa->FriendlyName ? aa->FriendlyName : L"";
                std::transform(desc.begin(), desc.end(), desc.begin(), ::towlower);
                std::transform(friendly.begin(), friendly.end(), friendly.begin(), ::towlower);
                if (desc.find(L"i210") != std::wstring::npos ||
                    desc.find(L"i225") != std::wstring::npos ||
                    desc.find(L"gigabit") != std::wstring::npos ||
                    friendly.find(L"i210") != std::wstring::npos) {
                    target = aa;
                    break;
                }
            }
        }

        if (!target) {
            log << "[Static IP] WARNING: EtherCAT NIC not found in GetAdaptersAddresses\n";
            logOut = log.str();
            return false;
        }

        // ── 3. Check current IP configuration ──
        // Even if a static IP exists, we still run netsh to ensure a
        // clean single-IP config (removes any leftover APIPA/DHCP addresses
        // that AddIPAddress may have left from a previous run).
        bool hasStaticIp = false;
        std::string existingStaticIp;
        for (auto* ua = target->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                char ipStr[16];
                inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));

                if (ua->DadState == IpDadStatePreferred && ua->PrefixOrigin == IpPrefixOriginManual) {
                    hasStaticIp = true;
                    existingStaticIp = ipStr;
                    log << "[Static IP] Existing static IP: " << ipStr << "\n";
                } else if (ua->PrefixOrigin == IpPrefixOriginDhcp) {
                    log << "[Static IP] DHCP IP present: " << ipStr << " — will clean up\n";
                } else {
                    log << "[Static IP] Non-static IP present: " << ipStr
                        << " (origin=" << ua->PrefixOrigin << ") — will clean up\n";
                }
            }
        }

        // ── 4. Determine the target static IP ──
        std::string targetIpStr;
        ULONG candidate = 0;

        if (hasStaticIp) {
            // Reuse the existing static IP — netsh will still run to clean
            // up any APIPA/DHCP addresses lingering on the interface.
            in_addr addr;
            inet_pton(AF_INET, existingStaticIp.c_str(), &addr);
            candidate = addr.S_un.S_addr;
            targetIpStr = existingStaticIp;
        } else {
            // Collect all existing IPv4 addresses to avoid collisions
            std::set<ULONG> usedIps;
            for (auto* aa = aaList; aa; aa = aa->Next) {
                for (auto* ua = aa->FirstUnicastAddress; ua; ua = ua->Next) {
                    if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                        auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                        usedIps.insert(sin->sin_addr.S_un.S_addr);
                    }
                }
            }

            // Derive a deterministic IP from the GUID: use bytes 6-7 to form host part
            UINT ipHost = 1;
            if (npcapGuid.size() >= 23) {
                auto hexVal = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return 0;
                };
                int b6 = hexVal(npcapGuid[21]) * 16 + hexVal(npcapGuid[22]);
                int b7 = hexVal(npcapGuid[24]) * 16 + hexVal(npcapGuid[25]);
                ipHost = ((b6 % 254) + 1) * 256 + ((b7 % 254) + 1);
            }

            candidate = htonl(0xA9FE0000 | (ipHost & 0xFFFF));
            int tries = 0;
            while (usedIps.count(candidate) && tries < 100) {
                candidate = htonl(0xA9FE0000 | (((ipHost + tries * 7) & 0xFFFF) + 1));
                ++tries;
            }
            if (usedIps.count(candidate)) {
                log << "[Static IP] ERROR: all 100 candidate IPs conflict\n";
                logOut = log.str();
                return false;
            }

            char ipStr[16];
            in_addr candidateAddr;
            candidateAddr.S_un.S_addr = candidate;
            inet_ntop(AF_INET, &candidateAddr, ipStr, sizeof(ipStr));
            targetIpStr = ipStr;
        }

        log << "[Static IP] Target: " << targetIpStr << " / 255.255.0.0\n";

        // ── 5. Set static IP via netsh (replaces any existing DHCP/APIPA config) ──
        // AddIPAddress only ADDS secondary IPs; netsh properly replaces the
        // primary IP configuration.  Fall back to AddIPAddress if netsh fails.
        std::string friendlyName;
        if (target->FriendlyName)
            friendlyName = WideToUtf8(std::wstring(target->FriendlyName));

        bool ok = false;
        if (!friendlyName.empty()) {
            std::wstring cmd = L"netsh interface ipv4 set address name=\""
                + Utf8ToWide(friendlyName)
                + L"\" static " + Utf8ToWide(targetIpStr)
                + L" 255.255.0.0 none";
            STARTUPINFOW si = {}; si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi = {};
            std::wstring cmdLine = L"cmd.exe /c \"" + cmd + L"\"";
            if (CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 5000);
                DWORD exitCode = 0;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                if (exitCode == 0) {
                    log << "[Static IP] ✓ netsh configured successfully\n";
                    ok = true;
                } else {
                    log << "[Static IP] netsh exited with code " << exitCode << "\n";
                }
            } else {
                log << "[Static IP] netsh CreateProcess failed (err=" << GetLastError() << ")\n";
            }
        }

        // ── 5b. Fallback: AddIPAddress (ARP-conflict-safe, but adds secondary IP) ──
        if (!ok) {
            ULONG ipCtx = 0, ipInst = 0;
            DWORD addRet = AddIPAddress(candidate, htonl(0xFFFF0000),  // 255.255.0.0
                                         target->IfIndex, &ipCtx, &ipInst);
            if (addRet == NO_ERROR || addRet == ERROR_OBJECT_ALREADY_EXISTS) {
                log << "[Static IP] ✓ AddIPAddress fallback succeeded (ctx="
                    << ipCtx << " inst=" << ipInst << ")\n";
                ok = true;
            } else {
                log << "[Static IP] AddIPAddress fallback failed (err=" << addRet << ")\n";
            }
        }

        if (ok) {
            logOut = log.str();
            return true;
        }

        log << "[Static IP] WARNING: could not set static IP automatically\n";
        log << "[Static IP] Please manually set: " << targetIpStr << " / 255.255.0.0 on the EtherCAT NIC\n";
        logOut = log.str();
        return false;
    }

    // ── NVIDIA GPU: Disable Dynamic P-State to suppress nvlddmkm DPC bursts ──
    bool NvidiaSetDisableDynamicPState(bool disable, std::string &prevValueOut, std::string &logOut) {
        logOut.clear();
        prevValueOut.clear();
        std::ostringstream log;
        log << "[NVIDIA DPS] " << (disable ? "Disabling" : "Restoring") << " Dynamic P-State\n";

        static const wchar_t kDisplayClass[] =
            L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E968-E325-11CE-BFC1-08002BE10318}";
        HKEY hClassKey = nullptr;
        LSTATUS lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDisplayClass, 0, KEY_READ, &hClassKey);
        if (lr != ERROR_SUCCESS) {
            log << "[NVIDIA DPS] ERROR: cannot open display class key (err=" << lr << ")\n";
            logOut = log.str();
            return false;
        }

        bool anyModified = false;
        DWORD idx = 0;
        wchar_t subKeyName[256];
        while (RegEnumKeyW(hClassKey, idx++, subKeyName, _countof(subKeyName)) == ERROR_SUCCESS) {
            HKEY hDevKey = nullptr;
            if (RegOpenKeyExW(hClassKey, subKeyName, 0, KEY_READ | KEY_WRITE, &hDevKey) != ERROR_SUCCESS) {
                continue;
            }

            // Check if this is NVIDIA by DriverDesc or MatchingDeviceId
            wchar_t descBuf[256] = {0};
            DWORD descSize = sizeof(descBuf);
            bool isNvidia = false;
            if (RegQueryValueExW(hDevKey, L"DriverDesc", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(descBuf), &descSize) == ERROR_SUCCESS) {
                std::wstring desc(descBuf);
                if (desc.find(L"NVIDIA") != std::wstring::npos ||
                    desc.find(L"nvidia") != std::wstring::npos) {
                    isNvidia = true;
                }
            }
            if (!isNvidia) {
                wchar_t matchBuf[256] = {0};
                DWORD matchSize = sizeof(matchBuf);
                if (RegQueryValueExW(hDevKey, L"MatchingDeviceId", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(matchBuf), &matchSize) == ERROR_SUCCESS) {
                    std::wstring match(matchBuf);
                    if (match.find(L"nvidia") != std::wstring::npos ||
                        match.find(L"ven_10de") != std::wstring::npos) {
                        isNvidia = true;
                    }
                }
            }

            if (isNvidia) {
                // Read current value
                wchar_t curBuf[32] = {0};
                DWORD curSize = sizeof(curBuf);
                std::string currentVal;
                if (RegQueryValueExW(hDevKey, L"DisableDynamicPstate", nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(curBuf), &curSize) == ERROR_SUCCESS) {
                    currentVal = WideToUtf8(std::wstring(curBuf));
                } else {
                    currentVal = "0"; // default: Dynamic P-State ON
                }

                if (prevValueOut.empty()) {
                    prevValueOut = currentVal;
                }

                if (disable) {
                    if (currentVal == "1") {
                        log << "[NVIDIA DPS]  already disabled on " << WideToUtf8(std::wstring(subKeyName))
                            << " (" << WideToUtf8(std::wstring(descBuf)) << ")\n";
                        anyModified = true;
                    } else {
                        const wchar_t newVal[] = L"1";
                        lr = RegSetValueExW(hDevKey, L"DisableDynamicPstate", 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(newVal),
                                            static_cast<DWORD>(sizeof(newVal)));
                        if (lr == ERROR_SUCCESS) {
                            log << "[NVIDIA DPS]  disabled on " << WideToUtf8(std::wstring(subKeyName))
                                << " (" << WideToUtf8(std::wstring(descBuf)) << ")\n";
                            anyModified = true;
                        } else {
                            log << "[NVIDIA DPS]  ERROR: RegSetValueEx failed on "
                                << WideToUtf8(std::wstring(subKeyName)) << " (err=" << lr << ")\n";
                        }
                    }
                } else {
                    if (!prevValueOut.empty() && currentVal != prevValueOut) {
                        std::wstring restoreVal = Utf8ToWide(prevValueOut);
                        lr = RegSetValueExW(hDevKey, L"DisableDynamicPstate", 0, REG_SZ,
                                            reinterpret_cast<const BYTE*>(restoreVal.c_str()),
                                            static_cast<DWORD>((restoreVal.size() + 1) * sizeof(wchar_t)));
                        if (lr == ERROR_SUCCESS) {
                            log << "[NVIDIA DPS]  restored to " << prevValueOut << " on "
                                << WideToUtf8(std::wstring(subKeyName))
                                << " (" << WideToUtf8(std::wstring(descBuf)) << ")\n";
                            anyModified = true;
                        } else {
                            log << "[NVIDIA DPS]  ERROR: restore failed on "
                                << WideToUtf8(std::wstring(subKeyName)) << " (err=" << lr << ")\n";
                        }
                    } else {
                        log << "[NVIDIA DPS]  no restore needed on "
                            << WideToUtf8(std::wstring(subKeyName))
                            << " (current=" << currentVal << ", previous=" << prevValueOut << ")\n";
                        anyModified = true;
                    }
                }
            }
            RegCloseKey(hDevKey);
        }
        RegCloseKey(hClassKey);

        if (!anyModified) {
            log << "[NVIDIA DPS] WARNING: no NVIDIA display adapter found\n";
        }
        logOut = log.str();
        return anyModified;
    }

    // ── TwinCAT service control for DPC reduction ──
    bool ControlTwinCatServices(bool stop, std::string &savedListOut, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[TwinCAT] " << (stop ? "Stopping" : "Restoring") << " Tc* services\n";

        // Build PowerShell command to manage TwinCAT services
        std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        psCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        psCmd += L"$svcs = Get-Service -Name 'Tc*' -ErrorAction SilentlyContinue; ";
        if (stop) {
            // Stop all Tc* services that are running, save their names
            psCmd += L"$running = @(); ";
            psCmd += L"foreach ($s in $svcs) { ";
            psCmd += L"  if ($s.Status -eq 'Running') { $running += $s.Name; Stop-Service $s.Name -Force }; ";
            psCmd += L"}; ";
            psCmd += L"if ($running.Count -gt 0) { Write-Output ('STOPPED:' + [string]::Join(';', $running)) } ";
            psCmd += L"else { Write-Output 'NONE_RUNNING' }";
        } else {
            // Restore services from saved list
            std::wstring savedW = Utf8ToWide(savedListOut);
            psCmd += L"$toStart = @('" + savedW + L"') -split ';' | Where-Object { $_ -ne '' }; ";
            psCmd += L"foreach ($name in $toStart) { ";
            psCmd += L"  Start-Service $name -ErrorAction SilentlyContinue; ";
            psCmd += L"  Write-Output ('STARTED:' + $name); ";
            psCmd += L"}";
        }
        psCmd += L"\"";

        // Execute
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = NULL;
        sa.bInheritHandle = TRUE;

        HANDLE hStdoutRd, hStdoutWr;
        CreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0);
        SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        si.wShowWindow = SW_HIDE;
        si.hStdOutput = hStdoutWr;
        si.hStdError = hStdoutWr;
        ZeroMemory(&pi, sizeof(pi));

        if (CreateProcessW(NULL, &psCmd[0], NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(hStdoutWr);
            WaitForSingleObject(pi.hProcess, 15000); // 15s timeout

            // Read output
            DWORD avail = 0;
            if (PeekNamedPipe(hStdoutRd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
                std::vector<char> buf(avail + 1);
                DWORD read = 0;
                ReadFile(hStdoutRd, buf.data(), avail, &read, NULL);
                buf[read] = '\0';
                std::string output(buf.data(), read);
                // Parse output
                std::istringstream iss(output);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    if (stop && line.rfind("STOPPED:", 0) == 0) {
                        savedListOut = line.substr(8);
                        log << "[TwinCAT] Stopped services: " << savedListOut << "\n";
                    } else if (!stop && line.rfind("STARTED:", 0) == 0) {
                        log << "[TwinCAT] Restored: " << line.substr(8) << "\n";
                    } else if (line == "NONE_RUNNING") {
                        log << "[TwinCAT] No Tc* services were running\n";
                    }
                }
            } else {
                log << "[TwinCAT] No output from PowerShell\n";
            }
            CloseHandle(hStdoutRd);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            CloseHandle(hStdoutRd);
            CloseHandle(hStdoutWr);
            log << "[TwinCAT] ERROR: CreateProcess failed (err=" << GetLastError() << ")\n";
        }

        logOut = log.str();
        return true;
    }

    // These are Npcap extension APIs resolved at runtime from wpcap.dll to
    // avoid compile-time SDK dependencies.  Safe to call on any pcap handle;
    // if the APIs are absent the function logs a warning and returns false.
    bool NpcapApplyImmediateMode(void *pcapHandle, std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[Npcap Immed] Applying immediate mode to pcap handle " << pcapHandle << "\n";

        if (!pcapHandle) {
            log << "[Npcap Immed] ERROR: null pcap handle\n";
            logOut = log.str();
            return false;
        }

        pcap_t *ph = static_cast<pcap_t *>(pcapHandle);

        // Dynamically resolve Npcap extension APIs
        typedef int (*pcap_set_immediate_mode_t)(pcap_t *, int);
        typedef int (*pcap_setmintocopy_t)(pcap_t *, int);

        HMODULE hWpcap = GetModuleHandleA("wpcap.dll");
        if (!hWpcap) {
            hWpcap = LoadLibraryA("wpcap.dll");
        }
        if (!hWpcap) {
            log << "[Npcap Immed] ERROR: wpcap.dll not loaded — Npcap may not be installed\n";
            logOut = log.str();
            return false;
        }

        auto dyn_set_immediate = reinterpret_cast<pcap_set_immediate_mode_t>(
            GetProcAddress(hWpcap, "pcap_set_immediate_mode"));
        auto dyn_setmintocopy = reinterpret_cast<pcap_setmintocopy_t>(
            GetProcAddress(hWpcap, "pcap_setmintocopy"));

        int applied = 0;

        if (dyn_set_immediate) {
            int rc = dyn_set_immediate(ph, 1);
            if (rc == 0) {
                log << "[Npcap Immed] ✓ pcap_set_immediate_mode(1) — packets delivered immediately\n";
                ++applied;
            } else {
                log << "[Npcap Immed] ⚠ pcap_set_immediate_mode(1) returned " << rc << "\n";
            }
        } else {
            log << "[Npcap Immed] ⚠ pcap_set_immediate_mode not found in wpcap.dll (Npcap >= 0.9983 required)\n";
        }

        if (dyn_setmintocopy) {
            int rc = dyn_setmintocopy(ph, 1);
            if (rc == 0) {
                log << "[Npcap Immed] ✓ pcap_setmintocopy(1) — min copy threshold = 1 byte\n";
                ++applied;
            } else {
                log << "[Npcap Immed] ⚠ pcap_setmintocopy(1) returned " << rc << "\n";
            }
        } else {
            log << "[Npcap Immed] ⚠ pcap_setmintocopy not found in wpcap.dll (Npcap >= 1.70 required)\n";
        }

        if (applied > 0) {
            log << "[Npcap Immed] ✓ " << applied << " Npcap optimizations applied — "
                << "kernel→userspace delivery latency reduced by ~1ms\n";
        } else {
            log << "[Npcap Immed] ⚠ No Npcap optimizations could be applied — upgrade Npcap to latest version\n";
        }

        logOut = log.str();
        return applied > 0;
    }


    // ── Registry-based hardware MSI/MSI-X interrupt affinity helpers ──
    // Pin device interrupts to a specific CPU core via the Windows registry
    // Interrupt Management\Affinity Policy interface.
    // Requires Administrator privileges; non-fatal when elevation is missing.

    namespace {

        // Known device-class GUIDs for targeting hardware interrupts.
        const GUID kGuidDisplay     = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        const GUID kGuidScsiAdapter = {0x4d36e97b, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        const GUID kGuidUsb         = {0x36fc9e60, 0xc465, 0x11cf, {0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
        const GUID kGuidNet         = {0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        const GUID kGuidHdc         = {0x4d36e96a, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
        const GUID kGuidMedia       = {0x4d36e96c, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

        // ── USB IRQ redirect global state (written by RedirectUsbIrqAffinityFromRtCore,
        //     read by GetSystemIrqAffinitySummary for main-window display) ──
        static bool g_usbIrqRedirectAttempted = false;
        static int  g_usbIrqTargetCpu         = -1;     // target CPU we tried to set
        static int  g_usbIrqActualCpu         = -1;     // verified CPU after write+restart (-1=unverified)
        static bool g_usbIrqNeedsAdmin        = false;  // true if registry write failed (likely no Admin)
        static bool g_usbIrqMsiXRestartOk     = false;  // true if MSI-X controller was restarted
        static int  g_usbIrqLineBasedCount    = 0;      // count of line-based (INTx) controllers
        static int  g_usbIrqTotalSet          = 0;      // USB controllers actually written this session

        // ── NIC (non-EtherCAT) IRQ redirect global state ──
        static bool g_nicIrqRedirectAttempted = false;
        static int  g_nicIrqTargetCpu         = -1;     // target CPU for non-EtherCAT NICs
        static int  g_nicIrqOnRtCoreCount     = 0;      // NICs still on RT core after redirect
        static int  g_nicIrqTotalSet          = 0;      // NICs actually written this session

        // Set a single device's MSI/MSI-X interrupt affinity to a specific CPU via registry.
        // Writes DevicePolicy=4 (IRQ_DEVICE_POLICY_SPECIFIED_PROCESSORS) and
        // AssignmentSetOverride=<KAFFINITY> to steer interrupt processing to targetCpu.
        //
        // NOTE: We do NOT write MSISupported=1 — Windows and the device driver
        // negotiate MSI mode automatically. Forcing it via registry can cause
        // driver load failures on some NICs (observed: Intel I210 TwinCAT adapter
        // failed with MSISupported=1 after reboot, recovered only after key deletion
        // and disable/re-enable cycle).
        //
        // deviceInstanceId: the device instance ID from SetupDi (e.g. "PCI\VEN_...")
        // targetCpu: logical CPU index (0-based) for the affinity mask
        // Convert a 0-based physical core index to the KAFFINITY mask for
        // its FIRST logical CPU (used for IRQ affinity AssignmentSetOverride).
        // Delegates to CpuTopology — handles interleaved/sequential SMT layouts.
        // Returns 0 if the physical core is not found.
        DWORD_PTR PhysicalCoreToKaAffinityMask(int physCoreIndex) {
            return CpuTopology::GetPrimaryLpMask(physCoreIndex);
        }

        // Convert a logical CPU index to its 0-based physical core index.
        // Delegates to CpuTopology. Returns -1 if not found.
        int LogicalCpuToPhysicalCoreIndex(int logicalCpu) {
            const CpuTopology::CoreInfo* ci = CpuTopology::FindCoreByLp(logicalCpu);
            return ci ? ci->physIdx : -1;
        }

        // Returns true if the registry keys were written successfully (i.e. actually changed).
        // Returns false if values already match target (no write needed) or on error.
        // targetCpu: 0-based LOGICAL CPU index (Task Manager numbering) — directly
        //   converted to KAFFINITY mask as 1 << targetCpu.  Use CpuTopology helpers
        //   if you need to convert from physical core to logical CPU first.
        bool SetDeviceIrqAffinityToCpu(const std::wstring& deviceInstanceId, int targetCpu, std::string& logOut) {
            if (!g_irqWriteEnabled) {
                logOut += "skipped (IRQ write disabled)\n";
                return false;
            }

            std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\";
            regPath += deviceInstanceId;
            regPath += L"\\Device Parameters\\Interrupt Management\\Affinity Policy";

            // Direct logical-CPU-to-mask conversion (Task Manager numbering)
            if (targetCpu < 0 || targetCpu > 63) {
                logOut += "skipped (invalid logical CPU " + std::to_string(targetCpu) + ")\n";
                return false;
            }
            ULONG64 targetMask = 1ULL << targetCpu;
            char maskHex[20];
            snprintf(maskHex, sizeof(maskHex), "0x%llX", targetMask);

            // ── Check current values first to avoid unnecessary writes ──
            HKEY hKey = nullptr;
            LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey);
            if (result == ERROR_SUCCESS) {
                DWORD existingPolicy = 0;
                DWORD policySize = sizeof(existingPolicy);
                ULONG64 existingMask = 0;
                DWORD maskSize = sizeof(existingMask);
                bool policyOk = (RegQueryValueExW(hKey, L"DevicePolicy", nullptr, nullptr,
                                                   reinterpret_cast<LPBYTE>(&existingPolicy),
                                                   &policySize) == ERROR_SUCCESS);
                bool maskOk = (RegQueryValueExW(hKey, L"AssignmentSetOverride", nullptr, nullptr,
                                                 reinterpret_cast<LPBYTE>(&existingMask),
                                                 &maskSize) == ERROR_SUCCESS);
                RegCloseKey(hKey);

                if (policyOk && maskOk && existingPolicy == 4 && existingMask == targetMask) {
                    logOut += "already set, no change needed\n";
                    return false;  // not an error, but not a new write either
                }
            }

            // ── Write new values ──
            DWORD disposition = 0;
            result = RegCreateKeyExW(
                HKEY_LOCAL_MACHINE, regPath.c_str(),
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_WRITE | KEY_READ, nullptr,
                &hKey, &disposition);

            if (result != ERROR_SUCCESS) {
                logOut += "Cannot open/create Affinity Policy key (err="
                       + std::to_string(result) + "), may need Admin\n";
                return false;
            }

            // DevicePolicy = 4  => IRQ_DEVICE_POLICY_SPECIFIED_PROCESSORS
            DWORD devicePolicy = 4;
            result = RegSetValueExW(hKey, L"DevicePolicy", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&devicePolicy), sizeof(devicePolicy));
            if (result != ERROR_SUCCESS) {
                logOut += "Cannot set DevicePolicy (err=" + std::to_string(result) + ")\n";
                RegCloseKey(hKey);
                return false;
            }

            // AssignmentSetOverride = KAFFINITY bitmask (1ULL << targetCpu)
            result = RegSetValueExW(hKey, L"AssignmentSetOverride", 0, REG_BINARY,
                                    reinterpret_cast<const BYTE*>(&targetMask), sizeof(targetMask));
            if (result != ERROR_SUCCESS) {
                logOut += "Cannot set AssignmentSetOverride (err=" + std::to_string(result) + ")\n";
                RegCloseKey(hKey);
                return false;
            }

            RegCloseKey(hKey);
            logOut += "written (DevicePolicy=4, Mask=" + std::string(maskHex) + ")\n";
            return true;
        }

        // ── SetDeviceIrqAffinityMask: write IRQ affinity with an explicit KAFFINITY mask ──
        // Unlike SetDeviceIrqAffinityToCpu (which takes a single logical CPU and builds
        // a single-bit mask from it), this takes a raw KAFFINITY mask directly,
        // supporting multi-core ranges.
        bool SetDeviceIrqAffinityMask(const std::wstring& deviceInstanceId, ULONG64 targetMask, std::string& logOut) {
            if (!g_irqWriteEnabled) {
                logOut += "skipped (IRQ write disabled)\n";
                return false;
            }
            if (targetMask == 0) {
                logOut += "skipped (zero mask)\n";
                return false;
            }

            std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\";
            regPath += deviceInstanceId;
            regPath += L"\\Device Parameters\\Interrupt Management\\Affinity Policy";

            char maskHex[20];
            snprintf(maskHex, sizeof(maskHex), "0x%llX", targetMask);

            HKEY hKey = nullptr;
            LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey);
            if (result == ERROR_SUCCESS) {
                DWORD existingPolicy = 0;
                DWORD policySize = sizeof(existingPolicy);
                ULONG64 existingMask = 0;
                DWORD maskSize = sizeof(existingMask);
                bool policyOk = (RegQueryValueExW(hKey, L"DevicePolicy", nullptr, nullptr,
                                                   reinterpret_cast<LPBYTE>(&existingPolicy),
                                                   &policySize) == ERROR_SUCCESS);
                bool maskOk = (RegQueryValueExW(hKey, L"AssignmentSetOverride", nullptr, nullptr,
                                                 reinterpret_cast<LPBYTE>(&existingMask),
                                                 &maskSize) == ERROR_SUCCESS);
                RegCloseKey(hKey);
                if (policyOk && maskOk && existingPolicy == 4 && existingMask == targetMask) {
                    logOut += "already set, no change needed\n";
                    return false;
                }
            }

            DWORD disposition = 0;
            result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, nullptr,
                                     REG_OPTION_NON_VOLATILE, KEY_WRITE | KEY_READ,
                                     nullptr, &hKey, &disposition);
            if (result != ERROR_SUCCESS) {
                logOut += "Cannot open/create key (err=" + std::to_string(result) + ")\n";
                return false;
            }

            DWORD devicePolicy = 4;
            result = RegSetValueExW(hKey, L"DevicePolicy", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&devicePolicy), sizeof(devicePolicy));
            if (result != ERROR_SUCCESS) {
                logOut += "Cannot set DevicePolicy (err=" + std::to_string(result) + ")\n";
                RegCloseKey(hKey);
                return false;
            }

            result = RegSetValueExW(hKey, L"AssignmentSetOverride", 0, REG_BINARY,
                                    reinterpret_cast<const BYTE*>(&targetMask), sizeof(targetMask));
            if (result != ERROR_SUCCESS) {
                logOut += "Cannot set AssignmentSetOverride (err=" + std::to_string(result) + ")\n";
                RegCloseKey(hKey);
                return false;
            }

            RegCloseKey(hKey);
            logOut += "written (DevicePolicy=4, Mask=" + std::string(maskHex) + ")\n";
            return true;
        }

        // Convenience wrapper: pin to CPU 0 (legacy, kept for backward compatibility).
        bool SetDeviceIrqAffinityToCpu0(const std::wstring& deviceInstanceId, std::string& logOut) {
            return SetDeviceIrqAffinityToCpu(deviceInstanceId, 0, logOut);
        }

        // Enumerate present devices of a given setup class GUID and return their
        // device instance IDs, optionally filtered by a case-insensitive substring
        // match against the device description (SPDRP_DEVICEDESC).
        std::vector<std::wstring> FindDeviceInstanceIdsByClass(
            const GUID& classGuid,
            const std::wstring& descSubstring,
            std::string& logOut) {

            std::vector<std::wstring> result;
            HDEVINFO devInfo = SetupDiGetClassDevsW(&classGuid, nullptr, nullptr, DIGCF_PRESENT);
            if (devInfo == INVALID_HANDLE_VALUE) {
                logOut += "  [DevEnum] SetupDiGetClassDevs failed (err="
                       + std::to_string(GetLastError()) + ")\n";
                return result;
            }

            SP_DEVINFO_DATA devInfoData;
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
                wchar_t descBuf[256] = {0};
                if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                        SPDRP_DEVICEDESC, nullptr,
                        reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr)) {
                    std::wstring desc(descBuf);
                    if (!descSubstring.empty()) {
                        std::wstring descLower = desc;
                        std::wstring subLower  = descSubstring;
                        std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);
                        std::transform(subLower.begin(),  subLower.end(),  subLower.begin(),  ::towlower);
                        if (descLower.find(subLower) == std::wstring::npos) {
                            continue;  // description filter did not match
                        }
                    }
                }

                wchar_t instanceId[MAX_PATH] = {0};
                if (SetupDiGetDeviceInstanceIdW(devInfo, &devInfoData,
                        instanceId, MAX_PATH, nullptr)) {
                    result.push_back(std::wstring(instanceId));
                }
            }

            SetupDiDestroyDeviceInfoList(devInfo);
            return result;
        }

        // Find the device instance ID for the network adapter whose friendly name,
        // device description, or PCI vendor/device ID matches the user-selected adapter.
        // Returns empty string if no match found.
        //
        // adapterDisplayName may come in several formats:
        //   - "Intel(R) I210 Gigabit Network Connection [1533]" (from FindAdapterDisplayName)
        //   - "Network adapter 'Intel(R) I210 ...' on local host" (from some APIs)
        //   - Raw pcap name like "\Device\NPF_{GUID}"
        //   - Canonical ifname
        std::wstring FindEthercatNicInstanceId(const std::string& adapterDisplayName, std::string& logOut) {
            if (adapterDisplayName.empty()) {
                logOut += "  [FindEcatNic] Empty adapter name, skipping\n";
                return {};
            }

            logOut += "  [FindEcatNic] Searching for: '" + adapterDisplayName + "'\n";

            // ── Extract search terms ──
            // Term 1: strip trailing [xxxx] suffix added by FindAdapterDisplayName
            std::string cleanName = adapterDisplayName;
            {
                auto bracketPos = cleanName.rfind(" [");
                if (bracketPos != std::string::npos) {
                    std::string after = cleanName.substr(bracketPos);
                    if (after.size() >= 3 && after.back() == ']') {
                        cleanName = cleanName.substr(0, bracketPos);
                    }
                }
            }

            // Term 2: extract from 'quoted' format
            std::string extractedName = cleanName;
            size_t quote1 = adapterDisplayName.find('\'');
            size_t quote2 = adapterDisplayName.rfind('\'');
            if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
                extractedName = adapterDisplayName.substr(quote1 + 1, quote2 - quote1 - 1);
            }

            // Trim spaces
            auto trim = [](std::string& s) {
                while (!s.empty() && s.front() == ' ') s.erase(0, 1);
                while (!s.empty() && s.back() == ' ') s.pop_back();
            };
            trim(cleanName);
            trim(extractedName);

            // Prepare lowercase search terms
            auto toLower = [](const std::string& s) -> std::wstring {
                std::wstring ws = CoreHelpers::Utf8ToWide(s);
                std::transform(ws.begin(), ws.end(), ws.begin(), ::towlower);
                return ws;
            };
            std::wstring searchClean = toLower(cleanName);
            std::wstring searchExtracted = toLower(extractedName);
            std::wstring searchRaw = toLower(adapterDisplayName);

            // PCI vendor/device IDs commonly used for EtherCAT (fallback)
            // I210: VEN_8086&DEV_1533  I225-LM: VEN_8086&DEV_15F2  I225-V: VEN_8086&DEV_15F3
            auto isEthercatPciId = [](const std::wstring& lowerId) -> bool {
                return (lowerId.find(L"ven_8086") != std::wstring::npos &&
                    (lowerId.find(L"dev_1533") != std::wstring::npos ||
                     lowerId.find(L"dev_15f2") != std::wstring::npos ||
                     lowerId.find(L"dev_15f3") != std::wstring::npos));
            };

            HDEVINFO devInfo = SetupDiGetClassDevsW(&kGuidNet, nullptr, nullptr, DIGCF_PRESENT);
            if (devInfo == INVALID_HANDLE_VALUE) {
                logOut += "  [FindEcatNic] SetupDiGetClassDevs failed (err="
                       + std::to_string(GetLastError()) + ")\n";
                return {};
            }

            SP_DEVINFO_DATA devInfoData;
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            std::wstring bestId;
            int bestScore = 0;  // 5=PCI ID EtherCAT, 4=exact friendly, 3=exact desc, 2=sub friendly, 1=sub desc

            for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
                wchar_t instanceId[MAX_PATH] = {0};
                if (!SetupDiGetDeviceInstanceIdW(devInfo, &devInfoData,
                        instanceId, MAX_PATH, nullptr)) {
                    continue;
                }
                std::wstring instId(instanceId);
                std::wstring instIdLower = instId;
                std::transform(instIdLower.begin(), instIdLower.end(), instIdLower.begin(), ::towlower);

                // ── Check 1: PCI vendor/device ID fallback (strongest signal) ──
                if (isEthercatPciId(instIdLower)) {
                    if (5 > bestScore) {
                        bestId = instId;
                        bestScore = 5;
                    }
                    continue;  // PCI match is definitive, but keep scanning for name matches too
                }

                // ── Check 2: friendly name match ──
                wchar_t friendlyBuf[256] = {0};
                bool hasFriendly = SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                    SPDRP_FRIENDLYNAME, nullptr,
                    reinterpret_cast<BYTE*>(friendlyBuf), sizeof(friendlyBuf), nullptr);

                int score = 0;
                if (hasFriendly) {
                    std::wstring fname(friendlyBuf);
                    std::wstring fnameLower = fname;
                    std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(), ::towlower);

                    // Exact match with clean name or extracted name
                    if (!searchClean.empty() && fnameLower == searchClean) {
                        score = 4;
                    } else if (!searchExtracted.empty() && fnameLower == searchExtracted) {
                        score = 4;
                    } else if (!searchRaw.empty() && fnameLower == searchRaw) {
                        score = 4;
                    }
                    // Substring match
                    else if (score < 2 && !searchClean.empty() && fnameLower.find(searchClean) != std::wstring::npos) {
                        score = 2;
                    } else if (score < 2 && !searchExtracted.empty() && fnameLower.find(searchExtracted) != std::wstring::npos) {
                        score = 2;
                    } else if (score < 2 && !searchRaw.empty() && fnameLower.find(searchRaw) != std::wstring::npos) {
                        score = 2;
                    }
                }

                // ── Check 3: device description match ──
                wchar_t descBuf[256] = {0};
                bool hasDesc = SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                    SPDRP_DEVICEDESC, nullptr,
                    reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr);

                if (hasDesc && score < 2) {
                    std::wstring desc(descBuf);
                    std::wstring descLower = desc;
                    std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);

                    if (!searchClean.empty() && descLower == searchClean) {
                        score = 3;
                    } else if (!searchExtracted.empty() && descLower == searchExtracted) {
                        score = 3;
                    } else if (score < 1 && !searchClean.empty() && descLower.find(searchClean) != std::wstring::npos) {
                        score = 1;
                    } else if (score < 1 && !searchExtracted.empty() && descLower.find(searchExtracted) != std::wstring::npos) {
                        score = 1;
                    }
                }

                if (score > bestScore) {
                    bestId = instId;
                    bestScore = score;
                }
            }

            SetupDiDestroyDeviceInfoList(devInfo);

            if (!bestId.empty()) {
                const char* matchType = (bestScore == 5) ? "PCI ID (Intel I210/I225 EtherCAT)"
                                      : (bestScore >= 4) ? "exact friendly name"
                                      : (bestScore >= 3) ? "exact desc"
                                      : (bestScore >= 2) ? "substring friendly"
                                      : "substring desc";
                logOut += std::string("  [FindEcatNic] ✓ Matched [") + matchType
                       + "] → instance [" + CoreHelpers::WideToUtf8(bestId)
                       + "] (score=" + std::to_string(bestScore) + ")\n";
            } else {
                logOut += std::string("  [FindEcatNic] ✗ No match found (name search='") + cleanName
                       + "' extracted='" + extractedName + "'); will use PCI ID fallback in Gaming NIC exclusion\n";
            }
            return bestId;
        }

        // Apply IRQ affinity to all devices matching a class GUID + description filter,
        // targeting a specific logical CPU.
        // Returns the count of devices that were successfully set.
        int ApplyDeviceClassIrqAffinityToCpu(
            const GUID& classGuid,
            const std::wstring& descFilter,
            const std::string& deviceLabel,
            int targetCpu,
            std::string& logOut) {

            std::string enumLog;
            auto instances = FindDeviceInstanceIdsByClass(classGuid, descFilter, enumLog);
            logOut += enumLog;

            if (instances.empty()) {
                logOut += "  [HW IRQ] No matching " + deviceLabel + " devices found\n";
                return 0;
            }

            int successCount = 0;
            for (const auto& id : instances) {
                std::string devLog;
                std::string utf8Id = CoreHelpers::WideToUtf8(id);
                if (SetDeviceIrqAffinityToCpu(id, targetCpu, devLog)) {
                    ++successCount;
                    logOut += "  [HW IRQ] " + deviceLabel + " [" + utf8Id + "] -> CPU "
                           + std::to_string(targetCpu) + ": " + devLog;
                } else {
                    logOut += "  [HW IRQ] " + deviceLabel + " [" + utf8Id + "] SKIPPED: " + devLog;
                }
            }

            return successCount;
        }

        // Legacy wrapper: pin to CPU 0.
        int ApplyDeviceClassIrqAffinityToCpu0(
            const GUID& classGuid,
            const std::wstring& descFilter,
            const std::string& deviceLabel,
            std::string& logOut) {
            return ApplyDeviceClassIrqAffinityToCpu(classGuid, descFilter, deviceLabel, 0, logOut);
        }

    } // anonymous namespace

    // Forward declarations for functions defined later in this file
    bool GetDeviceInstanceEffectiveIrqCpu(const std::wstring& instanceId, int &outCpu, std::string &logOut);

    // System-wide IRQ affinity orchestrator.
    // Strategy (revised 2026-06-10 based on ETW trace analysis, adjusted for DCS+VR coexistence):
    //
    //   Host workload layout (16-core system):
    //     CPU 0-3 : DCS game         ← NO IRQs here (game FPS must be stable)
    //     CPU 4   : SOEM cyclic thread ← NEVER IRQ (hard-isolated)
    //     CPU 5   : Quest3 VR         ← no IRQs (VR compositor sensitive)
    //     CPU 6   : EtherCAT NIC      ← dedicated, away from DCS/VR/RT
    //     CPU 7   : USB host controllers (USBXHCI — biggest DPC source)
    //     CPU 8   : NVIDIA GPU (nvlddmkm — second DPC source)
    //     CPU 9   : NVMe SSD
    //     CPU 10  : Gaming / other NIC
    //     CPU 11-15: free
    //
    //   Key principle: NIC DPC must NOT run on the cyclic-thread core (CPU 4)
    //   because DPC runs at DISPATCH_LEVEL which preempts ANY thread, including
    //   TIME_CRITICAL. Putting NIC IRQ on CPU 4 would cause direct PDO jitter.
    //
    //   Also: NIC DPC must NOT compete with DCS game threads (CPU 0-3) or
    //   VR compositor (CPU 5) — both are latency-sensitive workloads.
    //
    //   SMT topology (8C/16T, AMD-style: logical N + logical N+8 = same physical core):
    //     Physical 0 → CPU 0 (DCS)  + CPU 8  ← SMT sibling (avoid!)
    //     Physical 1 → CPU 1 (DCS)  + CPU 9  ← SMT sibling (avoid!)
    //     Physical 2 → CPU 2 (DCS)  + CPU 10 ← SMT sibling (avoid!)
    //     Physical 3 → CPU 3 (DCS)  + CPU 11 ← lighter DCS, usable
    //     Physical 4 → CPU 4 (RT)   + CPU 12 ← NEVER use SMT of RT core
    //     Physical 5 → CPU 5 (VR)   + CPU 13 ← VR is GPU-bound, acceptable
    //     Physical 6 → CPU 6 (Ecat) + CPU 14 ← NIC primary light, OK
    //     Physical 7 → CPU 7 (USB)  + CPU 15 ← USB primary light, OK
    //
    //   Key principle: SMT siblings share the SAME physical core's execution
    //   units and L1/L2 cache. IRQ on CPU 8 means DPC competes with DCS on
    //   CPU 0 for the same physical core → FPS drops + IRQ latency.
    //
    // Registry changes may require a reboot to take full effect.
    bool ApplySystemWideIrqAffinityToCpu0(const std::string& ethercatAdapterName, std::string& logOut) {
        logOut.clear();
        if (!g_irqWriteEnabled) {
            logOut = "[System IRQ Affinity] IRQ writes disabled — skipping system-wide IRQ affinity\n";
            return true;
        }
        std::ostringstream log;
        log << "[System IRQ Affinity] === Spreading hardware interrupts across PHYSICAL cores only ===\n";

        int totalSet = 0;
        int totalSkipped = 0;

        // ── Auto-adaptive IRQ target assignment ──
        // Use CpuTopology to build masks from physical core indices.
        // Targets are assigned relative to the RT core, keeping it clean.
        CpuTopology::EnsurePopulated();
        int physCoreCount = CpuTopology::PhysicalCoreCount();

        int rtCore = CpuIsolation::GetRtPhysicalCoreIndex();
        DWORD_PTR rtFullMask = CpuIsolation::GetRtCoreMask();
        if (rtFullMask == 0 && rtCore >= 0 && rtCore < 64) {
            // Fallback: use topology to get proper mask from physical core index
            rtFullMask = CpuTopology::GetCoreMask(rtCore);
        }

        // ── Helper: build multi-core KAFFINITY mask from physical core range ──
        // Uses primary LPs only (never SMT siblings) for clean IRQ distribution.
        auto buildRangeMask = [](int fromPhys, int toPhys) -> ULONG64 {
            ULONG64 m = 0;
            for (int p = fromPhys; p <= toPhys; ++p) {
                m |= static_cast<ULONG64>(CpuTopology::GetPrimaryLpMask(p));
            }
            return m;
        };

        // ── IRQ target masks (topology-aware physical core ranges) ──
        //   Design principles (per community research, 2026-07):
        //   • NVIDIA GPU MSI-X vectors are extremely few (1-8, effective 1-4),
        //     so 1 dedicated physical core is sufficient — no need for 4 cores.
        //   • All non- EtherCAT I/O + storage (USB, NIC, Audio, NVMe, SATA)
        //     merged onto 1 shared core — they are not performance-critical for
        //     real-time motion control and do not need isolation from each other.
        //   • Middle cores (2 through N-3) are intentionally left free of IRQ
        //     affinity — these are "clean" cores for DCS World and other game
        //     processes that should not share cores with interrupt handlers.
        //   • EtherCAT isolated on its own core (N-2).
        //   • RT core (N-1) stripped from all masks by stripRt().
        //
        //   ≥8 cores: GPU→0, I/O+Storage→1, DCS→2..N-3, EtherCAT→N-2
        //   7 cores:  GPU→0, I/O+Storage→1, DCS→2..3,  EtherCAT→5
        //   6 cores:  GPU→0, I/O+Storage→1, DCS→2,     EtherCAT→4
        //   5 cores:  GPU→0, I/O+Storage→1,             EtherCAT→3
        //   4 cores:  GPU→0, I/O+Storage→1,             EtherCAT→2
        //   2-3 cores: All except EtherCAT→0,  EtherCAT→1
        ULONG64 kGpuMask, kUsbMask, kLanMask, kNvmeMask, kEcatMask;

        if (physCoreCount >= 8) {
            // GPU: 1 dedicated core (NVIDIA MSI-X vectors are very few)
            kGpuMask  = buildRangeMask(0, 0);
            // All I/O + storage merged on 1 core
            kUsbMask  = buildRangeMask(1, 1);
            kLanMask  = buildRangeMask(1, 1);
            kNvmeMask = buildRangeMask(1, 1);
            // EtherCAT: core N-2 (leaves N-1 for RT)
            kEcatMask = buildRangeMask(physCoreCount - 2, physCoreCount - 2);
        } else if (physCoreCount >= 4) {
            // 4-7 cores: GPU→0, I/O+Storage→1, EtherCAT→N-2
            int ecatCore;
            switch (physCoreCount) {
                case 7:  ecatCore = 5; break;
                case 6:  ecatCore = 4; break;
                case 5:  ecatCore = 3; break;
                case 4:
                default: ecatCore = 2; break;
            }
            kGpuMask  = buildRangeMask(0, 0);
            kUsbMask  = buildRangeMask(1, 1);
            kLanMask  = buildRangeMask(1, 1);
            kNvmeMask = buildRangeMask(1, 1);
            kEcatMask = buildRangeMask(ecatCore, ecatCore);
        } else {
            // 2-3 cores: all non-EtherCAT share core 0, EtherCAT on core 1
            kGpuMask  = buildRangeMask(0, 0);
            kUsbMask  = buildRangeMask(0, 0);
            kLanMask  = buildRangeMask(0, 0);
            kNvmeMask = buildRangeMask(0, 0);
            kEcatMask = buildRangeMask(1, 1);
        }

        // Strip RT physical core bits from ALL masks
        auto stripRt = [&](ULONG64& m) {
            ULONG64 before = m;
            m &= ~rtFullMask;
            if (m == 0 && before != 0) {
                m = buildRangeMask(0, 0) & ~rtFullMask;
                if (m == 0) m = before;
            }
        };
        stripRt(kGpuMask);
        stripRt(kUsbMask);
        stripRt(kLanMask);
        stripRt(kNvmeMask);
        stripRt(kEcatMask);

        log << "[System IRQ Affinity] System has " << physCoreCount
            << " physical cores; RT core = " << rtCore << " (mask 0x"
            << std::hex << rtFullMask << std::dec << ")\n";
        log << "[System IRQ Affinity] Targets: GPU=0x" << std::hex << kGpuMask
            << " USB=0x" << kUsbMask << " LAN=0x" << kLanMask
            << " NVMe=0x" << kNvmeMask << " EtherCAT=0x" << kEcatMask
            << std::dec << "\n";
        log << "[System IRQ Affinity] All IRQs pinned to physical cores only — NEVER logical/SMT siblings\n";

        auto checkMaskRt = [&](const char* label, ULONG64 m) {
            if ((m & rtFullMask) != 0) {
                log << "[System IRQ Affinity] ❌ " << label
                    << " mask 0x" << std::hex << m << " overlaps RT mask 0x" << rtFullMask << std::dec << "\n";
            }
        };
        checkMaskRt("GPU",      kGpuMask);
        checkMaskRt("USB",      kUsbMask);
        checkMaskRt("LAN",      kLanMask);
        checkMaskRt("NVMe",     kNvmeMask);
        checkMaskRt("EtherCAT", kEcatMask);

        // Resolve the user-selected EtherCAT NIC to device instance ID
        std::string resolveLog;
        std::wstring ecatNicInstanceId = FindEthercatNicInstanceId(ethercatAdapterName, resolveLog);
        log << resolveLog;
        std::string ecatNicInstanceIdUtf8 = CoreHelpers::WideToUtf8(ecatNicInstanceId);
        if (!ecatNicInstanceId.empty()) {
            log << "[System IRQ Affinity] EtherCAT NIC instance: " << ecatNicInstanceIdUtf8 << "\n";
        }

        // ── Helper: apply IRQ mask to a device class ──
        auto applyMaskToClass = [&](const GUID& classGuid, const std::wstring& descFilter,
                                     const std::string& label, ULONG64 mask) -> int {
            std::string enumLog;
            auto instances = FindDeviceInstanceIdsByClass(classGuid, descFilter, enumLog);
            log << enumLog;
            if (instances.empty()) {
                log << "[System IRQ Affinity] " << label << ": no matching devices found\n";
                return 0;
            }
            int ok = 0;
            for (const auto& id : instances) {
                std::string devLog;
                if (SetDeviceIrqAffinityMask(id, mask, devLog)) {
                    ++ok;
                    log << "[System IRQ Affinity] " << label << " [" << CoreHelpers::WideToUtf8(id)
                        << "] -> mask 0x" << std::hex << mask << std::dec << ": " << devLog;
                } else {
                    log << "[System IRQ Affinity] " << label << " [" << CoreHelpers::WideToUtf8(id)
                        << "] SKIPPED: " << devLog;
                }
            }
            return ok;
        };

        // 1. GPU (Display class, NVIDIA)
        {
            log << "[System IRQ Affinity] --- GPU (NVIDIA) → mask 0x" << std::hex << kGpuMask << std::dec << " ---\n";
            int set = applyMaskToClass(kGuidDisplay, L"nvidia", "GPU", kGpuMask);
            if (set > 0) { totalSet += set; } else { ++totalSkipped; }
        }

        // 2. NVMe SSD (SCSIAdapter class)
        {
            log << "[System IRQ Affinity] --- NVMe → mask 0x" << std::hex << kNvmeMask << std::dec << " ---\n";
            int set = applyMaskToClass(kGuidScsiAdapter, L"nvme", "NVMe", kNvmeMask);
            if (set == 0) set = applyMaskToClass(kGuidScsiAdapter, L"nvm express", "NVMe", kNvmeMask);
            if (set > 0) { totalSet += set; } else { ++totalSkipped; }
        }

        // 3. SATA AHCI controllers (HDC class, shares NVMe core — both storage)
        {
            log << "[System IRQ Affinity] --- SATA AHCI → mask 0x" << std::hex << kNvmeMask << std::dec << " ---\n";
            int set = applyMaskToClass(kGuidHdc, L"ahci", "SATA", kNvmeMask);
            if (set == 0) set = applyMaskToClass(kGuidHdc, L"sata", "SATA", kNvmeMask);
            if (set > 0) { totalSet += set; } else { ++totalSkipped; }
        }

        // 4. USB host controllers
        {
            log << "[System IRQ Affinity] --- USB host controllers → mask 0x" << std::hex << kUsbMask << std::dec << " ---\n";
            int set = applyMaskToClass(kGuidUsb, L"host controller", "USB-HC", kUsbMask);
            if (set == 0) set = applyMaskToClass(kGuidUsb, L"xhci", "USB-xHCI", kUsbMask);
            if (set == 0) set = applyMaskToClass(kGuidUsb, L"ehci", "USB-EHCI", kUsbMask);
            if (set > 0) { totalSet += set; } else { ++totalSkipped; }
        }

        // 5. Audio devices (Media class, shares USB core — both I/O)
        {
            log << "[System IRQ Affinity] --- Audio → mask 0x" << std::hex << kUsbMask << std::dec << " ---\n";
            int set = applyMaskToClass(kGuidMedia, L"high definition audio", "Audio", kUsbMask);
            if (set == 0) set = applyMaskToClass(kGuidMedia, L"audio", "Audio", kUsbMask);
            if (set > 0) { totalSet += set; } else { ++totalSkipped; }
        }

        // 6. Gaming / other NIC (Net class, excluding EtherCAT adapter)
        {
            log << "[System IRQ Affinity] --- Gaming NIC → mask 0x" << std::hex << kLanMask << std::dec << " ---\n";
            const GUID kGuidNet = {0x4d36e972, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
            std::string enumLog;
            auto nics = FindDeviceInstanceIdsByClass(kGuidNet, L"", enumLog);
            log << enumLog;
            int nicSet = 0;
            for (const auto& id : nics) {
                if (!ecatNicInstanceId.empty() && _wcsicmp(id.c_str(), ecatNicInstanceId.c_str()) == 0)
                    continue;
                std::string devLog;
                if (SetDeviceIrqAffinityMask(id, kLanMask, devLog)) {
                    ++nicSet;
                    log << "[System IRQ Affinity] LAN [" << CoreHelpers::WideToUtf8(id)
                        << "] -> mask 0x" << std::hex << kLanMask << std::dec << ": " << devLog;
                }
            }
            if (nicSet > 0) { totalSet += nicSet; } else { ++totalSkipped; }
        }

        // 7. EtherCAT NIC
        {
            log << "[System IRQ Affinity] --- EtherCAT NIC → mask 0x" << std::hex << kEcatMask << std::dec << " ---\n";
            if (ecatNicInstanceId.empty()) {
                log << "[System IRQ Affinity] EtherCAT NIC: could not resolve adapter, skipping IRQ affinity\n";
                ++totalSkipped;
            } else {
                std::string devLog;
                if (SetDeviceIrqAffinityMask(ecatNicInstanceId, kEcatMask, devLog)) {
                    log << "[System IRQ Affinity] EtherCAT NIC [" << ecatNicInstanceIdUtf8
                        << "] -> mask 0x" << std::hex << kEcatMask << std::dec << ": " << devLog;
                    ++totalSet;
                } else {
                    log << "[System IRQ Affinity] EtherCAT NIC [" << ecatNicInstanceIdUtf8 << "] SKIPPED: " << devLog;
                    ++totalSkipped;
                }
            }
        }

        log << "[System IRQ Affinity] === Summary: " << totalSet << " devices with IRQ affinity on physical cores only";
        if (totalSkipped > 0) log << ", " << totalSkipped << " categories had no matching device";
        log << " ===\n";
        if (totalSet > 0) {
            log << "[System IRQ Affinity] ⚠ REBOOT REQUIRED — " << totalSet << " device IRQ affinity keys written to registry\n";
            log << "[System IRQ Affinity] ⚠ Current IRQ routing is still using OLD configuration until next reboot\n";
            log << "[System IRQ Affinity] ⚠ 需重新啟動電腦，新的 IRQ 分散配置才會生效！\n";
        } else {
            log << "[System IRQ Affinity] No devices were set — ensure the program runs as Administrator\n";
        }

        logOut = log.str();
        return true;  // always return true; individual failures are non-fatal
    }

    // ── USB-specific IRQ redirect (called at startup, before cyclic thread) ──
    // Redirects USB host controller (xHCI/EHCI/OHCI/UHCI) interrupts away from
    // the RT physical core.  Uses a two-phase approach:
    //   1. Write registry MSI/MSI-X Interrupt Affinity Policy (DevicePolicy=4 +
    //      AssignmentSetOverride).
    //   2. For MSI-X capable controllers, programmatically restart the device
    //      (disable + re-enable) so the new affinity takes effect immediately
    //      without requiring a reboot.  Line-based (INTx) controllers are
    //      registry-only and still need a reboot.
    //
    // Device restart will briefly disconnect all USB devices — this is safe
    // during startup before any EtherCAT motion begins.
    bool RedirectUsbIrqAffinityFromRtCore(std::string &logOut) {
        logOut.clear();
        if (!g_irqWriteEnabled) {
            logOut = "[USB IRQ] IRQ writes disabled — skipping USB redirect\n";
            return true;
        }
        std::ostringstream log;

        int rtCore = CpuIsolation::GetRtPhysicalCoreIndex();
        log << "[USB IRQ] Redirecting USB host controller interrupts away from RT core CPU "
            << rtCore << "...\n";

        // ── Determine safe target CPU for USB ──
        // Uses CpuTopology for core count and best non-RT LP selection.
        CpuTopology::EnsurePopulated();
        int physCoreCount = CpuTopology::PhysicalCoreCount();

        // Use CpuTopology to find best non-RT logical CPU (avoids SMT siblings, prefers primary LPs)
        DWORD_PTR rtCoreMask = CpuIsolation::GetRtCoreMask();
        if (rtCoreMask == 0) {
            // Fallback: if hard isolation not active, get proper mask from topology
            rtCoreMask = CpuTopology::GetCoreMask(rtCore);
            if (rtCoreMask == 0 && rtCore >= 0 && rtCore < 64) {
                rtCoreMask = (DWORD_PTR)1 << rtCore;
            }
        }
        int usbTargetCpu = CpuTopology::FindBestNonRtLp(rtCoreMask);

        log << "[USB IRQ] System has " << physCoreCount << " physical cores; "
            << "USB target = CPU" << usbTargetCpu << " (logical), RT mask = 0x"
            << std::hex << rtCoreMask << std::dec << "\n";

        // Check if target overlaps RT core (compare logical CPU masks)
        if (usbTargetCpu >= 0 && usbTargetCpu < 64 && (rtCoreMask & ((DWORD_PTR)1 << usbTargetCpu))) {
            log << "[USB IRQ] ❌ Cannot find safe USB target — RT core is the only option\n";
            logOut = log.str();
            return false;
        }

        // ── Phase 1: Collect USB host controller instance IDs ──
        // Use SetupDi to enumerate USB class devices with MSI-X support.
        // We collect them first so Phase 2 can restart each one after registry write.
        struct UsbControllerInfo {
            std::wstring instanceId;
            std::string  utf8Name;       // for logging
            bool        msiSupported;
        };
        std::vector<UsbControllerInfo> usbDevices;

        {
            HDEVINFO devInfo = SetupDiGetClassDevsW(&kGuidUsb, nullptr, nullptr,
                DIGCF_PRESENT);
            if (devInfo != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA devInfoData;
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
                    wchar_t descBuf[256] = {0};
                    SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                        SPDRP_DEVICEDESC, nullptr,
                        reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr);
                    std::wstring desc(descBuf);

                    // Only target host controllers
                    std::wstring descLower = desc;
                    std::transform(descLower.begin(), descLower.end(),
                                   descLower.begin(), ::towlower);
                    bool isHostCtrl = (descLower.find(L"host controller") != std::wstring::npos) ||
                                      (descLower.find(L"xhci") != std::wstring::npos) ||
                                      (descLower.find(L"ehci") != std::wstring::npos) ||
                                      (descLower.find(L"ohci") != std::wstring::npos) ||
                                      (descLower.find(L"uhci") != std::wstring::npos);
                    if (!isHostCtrl) continue;

                    wchar_t instanceId[MAX_PATH] = {0};
                    if (!SetupDiGetDeviceInstanceIdW(devInfo, &devInfoData,
                            instanceId, MAX_PATH, nullptr)) continue;

                    // Check MSI support
                    std::wstring msiPath = L"SYSTEM\\CurrentControlSet\\Enum\\";
                    msiPath += instanceId;
                    msiPath += L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
                    bool msiOk = false;
                    HKEY hMsiKey = nullptr;
                    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, msiPath.c_str(), 0,
                                      KEY_READ, &hMsiKey) == ERROR_SUCCESS) {
                        DWORD msiSupported = 0;
                        DWORD sz = sizeof(msiSupported);
                        if (RegQueryValueExW(hMsiKey, L"MSISupported", nullptr, nullptr,
                                             reinterpret_cast<LPBYTE>(&msiSupported),
                                             &sz) == ERROR_SUCCESS) {
                            msiOk = (msiSupported == 1);
                        }
                        RegCloseKey(hMsiKey);
                    }

                    std::string utf8Name = CoreHelpers::WideToUtf8(desc);
                    usbDevices.push_back({std::wstring(instanceId), utf8Name, msiOk});

                    log << "[USB IRQ] Found " << (msiOk ? "MSI-X" : "line-based")
                        << " USB controller: " << utf8Name
                        << " [" << CoreHelpers::WideToUtf8(instanceId) << "]\n";
                }
                SetupDiDestroyDeviceInfoList(devInfo);
            }
        }

        if (usbDevices.empty()) {
            log << "[USB IRQ] ⓘ No USB host controllers found\n";
            logOut = log.str();
            return true;
        }

        // ── Phase 2: Write registry affinity for each controller ──
        int totalSet = 0;
        std::vector<size_t> msiXIndices;  // indices into usbDevices for MSI-X controllers

        for (size_t idx = 0; idx < usbDevices.size(); ++idx) {
            const auto& dev = usbDevices[idx];

            // Only redirect USB controllers currently on the RT core.
            // Controllers already on non-RT cores do not need to be touched
            // and should not trigger a "需重開機" prompt.
            int beforeCpu = -1;
            std::string beforeLog;
            bool onRt = (GetDeviceInstanceEffectiveIrqCpu(dev.instanceId, beforeCpu, beforeLog) && beforeCpu == rtCore);
            if (!onRt) {
                log << "[USB IRQ] " << dev.utf8Name << ": already on non-RT CPU, skip\n";
                continue;
            }

            std::string devLog;
            bool written = SetDeviceIrqAffinityToCpu(dev.instanceId, usbTargetCpu, devLog);
            log << "[USB IRQ] " << dev.utf8Name << ": " << devLog;
            if (written) {
                ++totalSet;
                if (dev.msiSupported) {
                    msiXIndices.push_back(idx);
                }
            }
        }

        // ── Phase 3: Post-write verification ──
        // Registry writes take effect after reboot.
        // We do NOT auto-restart USB controllers because that would
        // disconnect all USB devices (keyboard, mouse, etc.) — disruptive.
        // Read back the registry to confirm the write succeeded.
        int verifiedCpu = -1;
        {
            HDEVINFO devInfo2 = SetupDiGetClassDevsW(&kGuidUsb, nullptr, nullptr, DIGCF_PRESENT);
            if (devInfo2 != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA dd2;
                dd2.cbSize = sizeof(SP_DEVINFO_DATA);
                for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo2, i, &dd2); ++i) {
                    wchar_t descBuf[256] = {0};
                    SetupDiGetDeviceRegistryPropertyW(devInfo2, &dd2,
                        SPDRP_DEVICEDESC, nullptr,
                        reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr);
                    std::wstring desc(descBuf);
                    std::wstring descLower = desc;
                    std::transform(descLower.begin(), descLower.end(), descLower.begin(), ::towlower);
                    bool isHC = (descLower.find(L"host controller") != std::wstring::npos) ||
                                (descLower.find(L"xhci") != std::wstring::npos) ||
                                (descLower.find(L"ehci") != std::wstring::npos);
                    if (!isHC) continue;

                    wchar_t instId[MAX_PATH] = {0};
                    if (!SetupDiGetDeviceInstanceIdW(devInfo2, &dd2, instId, MAX_PATH, nullptr)) continue;

                    int cpu = -1;
                    std::string vlog;
                    if (GetDeviceInstanceEffectiveIrqCpu(std::wstring(instId), cpu, vlog)) {
                        verifiedCpu = cpu;
                        log << "[USB IRQ] ✓ Verified: " << CoreHelpers::WideToUtf8(desc)
                            << " → CPU " << cpu << "\n";
                        break;
                    }
                }
                SetupDiDestroyDeviceInfoList(devInfo2);
            }
        }

        // ── Update global state for main-window display ──
        g_usbIrqRedirectAttempted = true;
        g_usbIrqTargetCpu         = usbTargetCpu;
        g_usbIrqActualCpu         = verifiedCpu;
        g_usbIrqNeedsAdmin        = (totalSet == 0 && !usbDevices.empty());
        g_usbIrqMsiXRestartOk     = false;  // no auto-restart to avoid USB disconnect
        g_usbIrqLineBasedCount    = static_cast<int>(usbDevices.size()) - static_cast<int>(msiXIndices.size());
        g_usbIrqTotalSet          = totalSet;

        if (verifiedCpu >= 0 && verifiedCpu != usbTargetCpu) {
            log << "[USB IRQ] ⚠ MISMATCH: registry shows CPU " << verifiedCpu
                << " but target was CPU " << usbTargetCpu << " — check Admin privileges\n";
        }

        // ── Summary ──
        log << "[USB IRQ] === Summary ===\n";
        log << "[USB IRQ] Registry: " << totalSet << " controller(s) wrote IRQ affinity → CPU "
            << usbTargetCpu << "\n";

        int lineBasedCount = static_cast<int>(usbDevices.size()) - static_cast<int>(msiXIndices.size());
        if (totalSet > 0) {
            if (verifiedCpu == usbTargetCpu) {
                log << "[USB IRQ] ✓ Registry confirmed: USB → CPU " << usbTargetCpu << "\n";
                log << "[USB IRQ] ⚠ 需重新啟動電腦，USB 中斷親和性才會生效！\n";
            } else if (verifiedCpu >= 0) {
                log << "[USB IRQ] ⚠ Registry mismatch: wrote CPU " << usbTargetCpu
                    << " but reads CPU " << verifiedCpu << " — check Admin privileges\n";
            } else {
                log << "[USB IRQ] ⓘ Registry written but could not verify (may need Admin or reboot)\n";
            }
            if (lineBasedCount > 0) {
                log << "[USB IRQ] " << lineBasedCount
                    << " line-based (INTx) controller(s) — cannot be changed at runtime, reboot required\n";
            }
        } else if (!usbDevices.empty()) {
            log << "[USB IRQ] ⚠ No registry writes performed — 需管理員權限！\n";
        } else {
            log << "[USB IRQ] ⓘ No USB host controllers found\n";
        }

        logOut = log.str();
        return true;
    }

    // ── USB IRQ redirect status query ──
    // Returns the result of the last RedirectUsbIrqAffinityFromRtCore call
    // for display in the main window's IRQ summary area.
    bool GetUsbIrqRedirectStatus(int &outTargetCpu, int &outActualCpu,
                                 bool &outNeedsAdmin, bool &outMsiXRestartOk,
                                 int &outLineBasedCount) {
        if (!g_usbIrqRedirectAttempted) return false;
        outTargetCpu      = g_usbIrqTargetCpu;
        outActualCpu      = g_usbIrqActualCpu;
        outNeedsAdmin     = g_usbIrqNeedsAdmin;
        outMsiXRestartOk  = g_usbIrqMsiXRestartOk;
        outLineBasedCount = g_usbIrqLineBasedCount;
        return true;
    }

    // ── NIC IRQ redirect status query ──
    bool GetNicIrqRedirectStatus(int &outTargetCpu, int &outOnRtCoreCount) {
        if (!g_nicIrqRedirectAttempted) return false;
        outTargetCpu      = g_nicIrqTargetCpu;
        outOnRtCoreCount  = g_nicIrqOnRtCoreCount;
        return true;
    }

    // ── Non-EtherCAT NIC IRQ redirect (called at startup, before cyclic thread) ──
    // Enumerates all Net-class devices except the EtherCAT NIC and writes
    // registry IRQ affinity away from the RT core.  Targets CPU 7 on ≥8-core
    // systems (the documented "Gaming NIC" slot).  Like USB redirect, registry
    // changes require a reboot — no device restart is attempted.
    bool RedirectNicIrqFromRtCore(std::string &logOut) {
        logOut.clear();
        if (!g_irqWriteEnabled) {
            logOut = "[NIC IRQ] IRQ writes disabled — skipping NIC redirect\n";
            return true;
        }
        std::ostringstream log;
        int rtCore = CpuIsolation::GetRtPhysicalCoreIndex();

        log << "[NIC IRQ] Redirecting non-EtherCAT NIC interrupts away from RT core CPU "
            << rtCore << "...\n";

        // ── Pick target CPU ──
        // Matches ApplySystemWideIrqAffinityToCpu0 topology: non-EtherCAT NICs
        // belong on the I/O+Storage core (core 1 for ≥4-core systems).
        // Use CpuTopology for core count and primary LP extraction.
        CpuTopology::EnsurePopulated();
        int physCoreCount = CpuTopology::PhysicalCoreCount();

        // Use CpuTopology to find best non-RT logical CPU
        DWORD_PTR rtCoreMask2 = CpuIsolation::GetRtCoreMask();
        if (rtCoreMask2 == 0) {
            // Fallback: use topology to get proper mask
            rtCoreMask2 = CpuTopology::GetCoreMask(rtCore);
            if (rtCoreMask2 == 0 && rtCore >= 0 && rtCore < 64) {
                rtCoreMask2 = (DWORD_PTR)1 << rtCore;
            }
        }
        // NIC target: I/O+Storage core (physical core 1 on ≥4-core, core 0 on 2-3 core)
        int nicTargetPhys = (physCoreCount >= 4) ? 1 : 0;
        const auto* nicTargetCore = CpuTopology::FindCoreByPhysIdx(nicTargetPhys);
        int nicTargetCpu = (nicTargetCore && !nicTargetCore->lps.empty()) ? nicTargetCore->lps[0]
                         : CpuTopology::FindBestNonRtLp(rtCoreMask2);

        log << "[NIC IRQ] System has " << physCoreCount << " physical cores; "
            << "NIC target = CPU" << nicTargetCpu << " (logical), RT mask = 0x"
            << std::hex << rtCoreMask2 << std::dec << "\n";

        // ── Enumerate all Net-class devices ──
        std::vector<std::wstring> allNicIds;
        {
            std::string enumLog;
            allNicIds = FindDeviceInstanceIdsByClass(kGuidNet, L"", enumLog);
            log << enumLog;
        }

        if (allNicIds.empty()) {
            log << "[NIC IRQ] ⓘ No network adapters found\n";
            logOut = log.str();
            return true;
        }

        // ── Exclude EtherCAT NIC by PCI vendor/device ID ──
        // Intel I210 (VEN_8086&DEV_1533), I225-LM (DEV_15F2), I225-V (DEV_15F3)
        auto isEthercatNic = [](const std::wstring& id) -> bool {
            std::wstring lower = id;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            return (lower.find(L"ven_8086") != std::wstring::npos &&
                    (lower.find(L"dev_1533") != std::wstring::npos ||
                     lower.find(L"dev_15f2") != std::wstring::npos ||
                     lower.find(L"dev_15f3") != std::wstring::npos));
        };

        int totalSet = 0;
        int ethercatSkipped = 0;
        int onRtBefore = 0;

        for (const auto& id : allNicIds) {
            std::string utf8Id = CoreHelpers::WideToUtf8(id);

            if (isEthercatNic(id)) {
                ++ethercatSkipped;
                log << "[NIC IRQ]   Skipping EtherCAT NIC: " << utf8Id << "\n";
                continue;
            }

            // Check current IRQ CPU before writing — only redirect NICs
            // that are currently on the RT core.  NICs already on non-RT cores
            // do not need to be touched and should not trigger a "需重開機" prompt.
            int beforeCpu = -1;
            std::string beforeLog;
            bool onRt = (GetDeviceInstanceEffectiveIrqCpu(id, beforeCpu, beforeLog) && beforeCpu == rtCore);
            if (onRt) ++onRtBefore;

            if (!onRt) {
                log << "[NIC IRQ]   " << utf8Id << " already on non-RT CPU, skip\n";
                continue;
            }

            std::string devLog;
            if (SetDeviceIrqAffinityToCpu(id, nicTargetCpu, devLog)) {
                ++totalSet;
                log << "[NIC IRQ]   " << utf8Id << " → CPU " << nicTargetCpu << "\n";
            } else {
                log << "[NIC IRQ]   " << utf8Id << " SKIPPED: " << devLog;
            }
        }

        // ── Post-write verification ──
        int onRtAfter = 0;
        for (const auto& id : allNicIds) {
            if (isEthercatNic(id)) continue;
            int cpu = -1;
            std::string vlog;
            if (GetDeviceInstanceEffectiveIrqCpu(id, cpu, vlog) && cpu == rtCore) {
                ++onRtAfter;
            }
        }

        // ── Update global state ──
        g_nicIrqRedirectAttempted = true;
        g_nicIrqTargetCpu         = nicTargetCpu;
        g_nicIrqOnRtCoreCount     = onRtAfter;
        g_nicIrqTotalSet          = totalSet;

        // ── Summary ──
        log << "[NIC IRQ] === Summary ===\n";
        log << "[NIC IRQ] " << (allNicIds.size() - ethercatSkipped) << " non-EtherCAT NIC(s), "
            << totalSet << " wrote registry → CPU " << nicTargetCpu << "\n";
        log << "[NIC IRQ] NICs on RT core: " << onRtBefore << " before, " << onRtAfter << " after\n";
        if (onRtAfter > 0) {
            log << "[NIC IRQ] ⚠ " << onRtAfter << " NIC(s) still on RT CPU " << rtCore
                << " — run as Admin and reboot to apply\n";
        }
        if (totalSet > 0 && onRtAfter == 0) {
            log << "[NIC IRQ] ✓ All NICs redirected away from RT core — reboot required\n";
        }
        if (totalSet == 0 && (allNicIds.size() - ethercatSkipped) > 0) {
            log << "[NIC IRQ] ⚠ No registry writes — 需管理員權限！\n";
        }

        logOut = log.str();
        return true;
    }


    // ── USB xHCI Interrupt Moderation (IMOD) — set to 500 Hz ──
    // Reduces USB host controller interrupt frequency from the default
    // (potentially >1000 Hz) to 500 Hz (2 ms interval).  Lower USB interrupt
    // rate means fewer DPC/ISR preemptions on the PDO cycle thread.
    //
    // Strategy: write InterruptModerationInterval registry value under each
    // xHCI USB 3.0 controller's hardware key.  The Windows USB xHCI driver
    // (usbxhci.sys) reads this during initialization to program the IMOD
    // register.  Value = 16 corresponds to 16 × 125 µs ≈ 2 ms ≈ 500 Hz.
    //
    // This is a best-effort, per-controller registry write.  Some
    // controllers or drivers may ignore the setting; the function logs
    // each write and the final registry state.
    //
    // Requires Administrator privileges for registry writes; non-fatal
    // when elevation is missing.
    bool SetUsbInterruptRate500Hz(std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[USB 500Hz] Setting USB xHCI interrupt moderation to 500 Hz...\n";

        int totalSet = 0;
        int totalFound = 0;

        // ── Enumerate USB host controllers ──
        HDEVINFO devInfo = SetupDiGetClassDevsW(&kGuidUsb, nullptr, nullptr,
            DIGCF_PRESENT);
        if (devInfo == INVALID_HANDLE_VALUE) {
            log << "[USB 500Hz] SetupDiGetClassDevsW failed (err="
                << GetLastError() << ")\n";
            logOut = log.str();
            return false;
        }

        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); ++i) {
            wchar_t descBuf[256] = {0};
            SetupDiGetDeviceRegistryPropertyW(devInfo, &devInfoData,
                SPDRP_DEVICEDESC, nullptr,
                reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr);
            std::wstring desc(descBuf);

            // Only target xHCI (USB 3.0) host controllers — these have
            // a programmable IMOD register in the runtime register space.
            std::wstring descLower = desc;
            std::transform(descLower.begin(), descLower.end(),
                           descLower.begin(), ::towlower);
            bool isXhci = (descLower.find(L"xhci") != std::wstring::npos) ||
                          (descLower.find(L"xchi") != std::wstring::npos) ||
                          (descLower.find(L"usb 3") != std::wstring::npos) ||
                          (descLower.find(L"usb3") != std::wstring::npos) ||
                          (descLower.find(L"third generation") != std::wstring::npos);
            if (!isXhci) continue;

            wchar_t instanceId[MAX_PATH] = {0};
            if (!SetupDiGetDeviceInstanceIdW(devInfo, &devInfoData,
                    instanceId, MAX_PATH, nullptr)) continue;

            ++totalFound;
            std::string utf8Desc = WideToUtf8(desc);

            // ── Write InterruptModerationInterval → Device Parameters ──
            std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\";
            regPath += instanceId;
            regPath += L"\\Device Parameters";

            HKEY hKey = nullptr;
            DWORD disposition = 0;
            LONG result = RegCreateKeyExW(
                HKEY_LOCAL_MACHINE, regPath.c_str(),
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_WRITE | KEY_READ, nullptr,
                &hKey, &disposition);

            if (result != ERROR_SUCCESS) {
                log << "[USB 500Hz]   " << utf8Desc
                    << ": cannot open Device Parameters (err=" << result
                    << "), may need Admin\n";
                continue;
            }

            // IMOD interval value: 16 (125 µs increments × 16 = 2000 µs ≈ 2 ms ≈ 500 Hz)
            // xHCI 1.x uses 250 ns increments, but Windows usbxhci.sys
            // exposes the registry value in 125 µs units for backward
            // compatibility with xHCI 0.96.
            const DWORD kImodTarget = 16;
            DWORD existingValue = 0;
            DWORD existingSize = sizeof(existingValue);
            bool alreadySet = false;

            if (RegQueryValueExW(hKey, L"InterruptModerationInterval", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&existingValue),
                                 &existingSize) == ERROR_SUCCESS) {
                alreadySet = (existingValue == kImodTarget);
            }

            if (alreadySet) {
                log << "[USB 500Hz]   " << utf8Desc
                    << ": already 500 Hz (value=" << existingValue << ")\n";
                RegCloseKey(hKey);
                ++totalSet;
                continue;
            }

            result = RegSetValueExW(hKey, L"InterruptModerationInterval", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&kImodTarget),
                                    sizeof(kImodTarget));
            if (result == ERROR_SUCCESS) {
                ++totalSet;
                DWORD afterValue = 0;
                DWORD afterSize = sizeof(afterValue);
                RegQueryValueExW(hKey, L"InterruptModerationInterval", nullptr, nullptr,
                                 reinterpret_cast<LPBYTE>(&afterValue), &afterSize);
                log << "[USB 500Hz]   " << utf8Desc
                    << ": wrote InterruptModerationInterval=" << kImodTarget
                    << " (was " << existingValue << ", now reads "
                    << afterValue << ") ";
                // Determine equivalent Hz for display
                // 125 µs per increment: interval_ms = value * 0.125
                double intervalMs = static_cast<double>(afterValue) * 0.125;
                double freqHz = (intervalMs > 0) ? (1000.0 / intervalMs) : 0.0;
                log << "≈ " << static_cast<int>(freqHz + 0.5) << " Hz\n";
            } else {
                log << "[USB 500Hz]   " << utf8Desc
                    << ": failed to set InterruptModerationInterval (err="
                    << result << ")\n";
            }

            RegCloseKey(hKey);
        }

        SetupDiDestroyDeviceInfoList(devInfo);

        // ── Also try the global driver parameters key ──
        {
            const wchar_t* globalPath = L"SYSTEM\\CurrentControlSet\\Services\\USBXHCI\\Parameters";
            HKEY hGlobal = nullptr;
            DWORD disposition = 0;
            LONG result = RegCreateKeyExW(
                HKEY_LOCAL_MACHINE, globalPath,
                0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_WRITE | KEY_READ, nullptr,
                &hGlobal, &disposition);
            if (result == ERROR_SUCCESS) {
                DWORD existing = 0;
                DWORD sz = sizeof(existing);
                DWORD kImodGlobal = 16;
                if (RegQueryValueExW(hGlobal, L"InterruptModerationInterval", nullptr,
                                     nullptr, reinterpret_cast<LPBYTE>(&existing),
                                     &sz) != ERROR_SUCCESS || existing != kImodGlobal) {
                    result = RegSetValueExW(hGlobal, L"InterruptModerationInterval", 0,
                                            REG_DWORD,
                                            reinterpret_cast<const BYTE*>(&kImodGlobal),
                                            sizeof(kImodGlobal));
                    if (result == ERROR_SUCCESS) {
                        log << "[USB 500Hz] ✓ Global USBXHCI Parameters set to "
                            << kImodGlobal << "\n";
                    } else {
                        log << "[USB 500Hz] Global USBXHCI Parameters write failed (err="
                            << result << ")\n";
                    }
                } else {
                    log << "[USB 500Hz] ✓ Global USBXHCI Parameters already set\n";
                }
                RegCloseKey(hGlobal);
            } else {
                log << "[USB 500Hz] Global USBXHCI Parameters key open failed (err="
                    << result << "), may need Admin\n";
            }
        }

        // ── Summary ──
        if (totalFound == 0) {
            log << "[USB 500Hz] ⓘ No xHCI USB 3.0 host controllers found\n";
        } else {
            log << "[USB 500Hz] ✓ " << totalSet << "/" << totalFound
                << " xHCI controller(s) set to 500 Hz\n";
            if (totalSet < totalFound) {
                log << "[USB 500Hz] ⚠ Some controllers could not be configured"
                    << " — run as Admin or check driver support\n";
            }
            log << "[USB 500Hz] ⚠ 需重新啟動電腦，USB 中斷頻率設定才會生效！\n";
        }

        logOut = log.str();
        return totalSet > 0;
    }


    // ── HID polling rate — set to 125 Hz (8 ms) ──
    // Reduces USB HID (human interface device) interrupt transfer polling
    // frequency.  HID devices such as mice, keyboards, joysticks, TrackIR,
    // and game controllers use interrupt transfers to report data; the host
    // controller polls the device at the interval specified in the endpoint
    // descriptor.  This registry setting overrides the per-device polling
    // interval for all HidUsb.sys-attached devices.
    //
    // Default polling interval varies by device (typically 1–8 ms, i.e.
    // 1000–125 Hz).  Setting PollDeviceInterval = 8 ms lowers the rate to
    // 125 Hz, reducing USB interrupt load by 50–90 % vs high-rate gaming
    // devices (e.g. 1000 Hz mice).  125 Hz is still well within the
    // responsiveness range required for manual joystick / TrackIR control
    // in a motion platform UI.
    //
    // The setting takes effect globally for all HidUsb devices after reboot.
    // Per-device override values (if set in the device's hardware key) take
    // precedence over the global default.
    bool SetHidPollingRate(std::string &logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[HID Poll] Setting USB HID polling rate to 125 Hz (8 ms)...\n";

        const wchar_t* keyPath = L"SYSTEM\\CurrentControlSet\\Services\\HidUsb\\Parameters";
        const wchar_t* valueName = L"PollDeviceInterval";
        const DWORD kTargetMs = 8;  // 8 ms → 125 Hz

        HKEY hKey = nullptr;
        DWORD disposition = 0;
        LONG result = RegCreateKeyExW(
            HKEY_LOCAL_MACHINE, keyPath,
            0, nullptr, REG_OPTION_NON_VOLATILE,
            KEY_WRITE | KEY_READ, nullptr,
            &hKey, &disposition);

        if (result != ERROR_SUCCESS) {
            log << "[HID Poll] Cannot open/create HidUsb Parameters key (err="
                << result << "), may need Admin\n";
            logOut = log.str();
            return false;
        }

        DWORD existingValue = 0;
        DWORD dataSize = sizeof(existingValue);
        bool alreadySet = false;
        if (RegQueryValueExW(hKey, valueName, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&existingValue),
                             &dataSize) == ERROR_SUCCESS) {
            alreadySet = (existingValue == kTargetMs);
        }

        if (alreadySet) {
            log << "[HID Poll] ✓ Already set to " << existingValue
                << " ms (" << (1000 / existingValue) << " Hz)\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return true;
        }

        result = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&kTargetMs),
                                sizeof(kTargetMs));
        if (result == ERROR_SUCCESS) {
            DWORD afterValue = 0;
            DWORD afterSize = sizeof(afterValue);
            RegQueryValueExW(hKey, valueName, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&afterValue), &afterSize);
            int freqHz = (afterValue > 0) ? (1000 / afterValue) : 0;
            log << "[HID Poll] ✓ Wrote PollDeviceInterval=" << kTargetMs
                << " ms (was " << existingValue << " ms, now reads "
                << afterValue << " ms → " << freqHz << " Hz)\n";
        } else {
            log << "[HID Poll] Failed to write PollDeviceInterval (err="
                << result << ")\n";
        }

        RegCloseKey(hKey);

        // ── Also try per-device HID registry overrides ──
        // Enumerate all HID-class devices and write PollIntervalOverride
        // to their Device Parameters keys where not already set.
        // This ensures individual high-rate HID devices are capped.
        const GUID kGuidHid = {0x745a17a0, 0x74d3, 0x11d0,
                               {0xb6, 0xfe, 0x00, 0xa0, 0xc9, 0x0f, 0x57, 0xda}};
        int perDeviceSet = 0;
        int perDeviceFound = 0;

        HDEVINFO hDevInfo = SetupDiGetClassDevsW(&kGuidHid, nullptr, nullptr,
            DIGCF_PRESENT);
        if (hDevInfo != INVALID_HANDLE_VALUE) {
            SP_DEVINFO_DATA devInfoData;
            devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
            for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData); ++i) {
                wchar_t descBuf[256] = {0};
                SetupDiGetDeviceRegistryPropertyW(hDevInfo, &devInfoData,
                    SPDRP_DEVICEDESC, nullptr,
                    reinterpret_cast<BYTE*>(descBuf), sizeof(descBuf), nullptr);
                std::wstring desc(descBuf);

                wchar_t instanceId[MAX_PATH] = {0};
                if (!SetupDiGetDeviceInstanceIdW(hDevInfo, &devInfoData,
                        instanceId, MAX_PATH, nullptr)) continue;

                ++perDeviceFound;
                std::string utf8Desc = WideToUtf8(desc);

                // Build path: HKLM\SYSTEM\CurrentControlSet\Enum\<instance>\Device Parameters
                std::wstring devRegPath = L"SYSTEM\\CurrentControlSet\\Enum\\";
                devRegPath += instanceId;
                devRegPath += L"\\Device Parameters";

                HKEY hDevKey = nullptr;
                result = RegCreateKeyExW(
                    HKEY_LOCAL_MACHINE, devRegPath.c_str(),
                    0, nullptr, REG_OPTION_NON_VOLATILE,
                    KEY_WRITE | KEY_READ, nullptr,
                    &hDevKey, &disposition);
                if (result != ERROR_SUCCESS) continue;

                // Write PollIntervalOverride (ms) as the per-device cap.
                // HidUsb.sys respects this value over the global default.
                DWORD existVal = 0;
                DWORD existSz = sizeof(existVal);
                bool devAlready = (RegQueryValueExW(hDevKey, L"PollIntervalOverride",
                    nullptr, nullptr,
                    reinterpret_cast<LPBYTE>(&existVal),
                    &existSz) == ERROR_SUCCESS && existVal == kTargetMs);

                if (!devAlready) {
                    result = RegSetValueExW(hDevKey, L"PollIntervalOverride", 0,
                                            REG_DWORD,
                                            reinterpret_cast<const BYTE*>(&kTargetMs),
                                            sizeof(kTargetMs));
                    if (result == ERROR_SUCCESS) {
                        ++perDeviceSet;
                        log << "[HID Poll]   " << utf8Desc
                            << ": wrote PollIntervalOverride=" << kTargetMs
                            << " ms\n";
                    }
                } else {
                    log << "[HID Poll]   " << utf8Desc
                        << ": already " << existVal << " ms, skip\n";
                }
                RegCloseKey(hDevKey);
            }
            SetupDiDestroyDeviceInfoList(hDevInfo);
        }

        // ── Summary ──
        log << "[HID Poll] === Summary ===\n";
        if (result == ERROR_SUCCESS) {
            log << "[HID Poll] ✓ Global HidUsb\\Parameters: PollDeviceInterval="
                << kTargetMs << " ms (125 Hz)\n";
        }
        if (perDeviceFound > 0) {
            log << "[HID Poll] ✓ Per-device: " << perDeviceSet << "/"
                << perDeviceFound << " HID device(s) set to " << kTargetMs
                << " ms\n";
        }
        log << "[HID Poll] ⚠ 需重新啟動電腦，HID polling rate 設定才會生效！\n";

        logOut = log.str();
        return result == ERROR_SUCCESS || perDeviceSet > 0;
    }


    // Query the effective IRQ CPU for the given adapter by reading the
    // Device Parameters\Interrupt Management\Affinity Policy\AssignmentSetOverride
    // registry value for the resolved device instance ID.
    bool GetAdapterEffectiveIrqCpu(const std::string &adapterName, int &outCpu, std::string &logOut) {
        outCpu = -1;
        logOut.clear();
        std::ostringstream log;
        if (adapterName.empty()) {
            log << "[Query IRQ] Empty adapter name\n";
            logOut = log.str();
            return false;
        }

        std::string resolveLog;
        std::wstring instanceId = FindEthercatNicInstanceId(adapterName, resolveLog);
        log << resolveLog;
        if (instanceId.empty()) {
            log << "[Query IRQ] Could not resolve device instance ID for adapter\n";
            logOut = log.str();
            return false;
        }

        std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey);
        if (res != ERROR_SUCCESS) {
            log << "[Query IRQ] Cannot open registry key (err=" << res << ")\n";
            logOut = log.str();
            return false;
        }

        // Read AssignmentSetOverride (binary, 8 bytes -> KAFFINITY)
        BYTE buf[16] = {0};
        DWORD bufSize = sizeof(buf);
        DWORD type = 0;
        res = RegQueryValueExW(hKey, L"AssignmentSetOverride", nullptr, &type, buf, &bufSize);
        if (res != ERROR_SUCCESS || type != REG_BINARY || bufSize < sizeof(ULONG64)) {
            log << "[Query IRQ] AssignmentSetOverride not present or unreadable\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        // Interpret as 64-bit mask
        uint64_t mask = 0;
        memcpy(&mask, buf, sizeof(mask));
        if (mask == 0) {
            log << "[Query IRQ] AssignmentSetOverride present but mask == 0\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        // Find least-significant set bit -> logical CPU
        int cpu = -1;
        for (int i = 0; i < 64; ++i) {
            if (mask & (1ULL << i)) { cpu = i; break; }
        }
        if (cpu < 0) {
            log << "[Query IRQ] No CPU bit found in mask\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        outCpu = cpu;  // logical CPU (Task Manager numbering)
        log << "[Query IRQ] Adapter instance " << CoreHelpers::WideToUtf8(instanceId)
            << " -> logical CPU " << cpu << " (mask=0x";
        char tmp[32]; snprintf(tmp, sizeof(tmp), "%llX", (long long)mask);
        log << tmp << ")\n";
        RegCloseKey(hKey);
        logOut = log.str();
        return true;
    }

    bool GetDeviceInstanceEffectiveIrqCpu(const std::wstring& instanceId, int &outCpu, std::string &logOut) {
        outCpu = -1;
        logOut.clear();
        std::ostringstream log;
        if (instanceId.empty()) {
            log << "[Query IRQ] Empty device instance ID\n";
            logOut = log.str();
            return false;
        }

        std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId + L"\\Device Parameters\\Interrupt Management\\Affinity Policy";
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey);
        if (res != ERROR_SUCCESS) {
            log << "[Query IRQ] Cannot open registry key (err=" << res << ") for " << CoreHelpers::WideToUtf8(instanceId) << "\n";
            logOut = log.str();
            return false;
        }

        BYTE buf[16] = {0};
        DWORD bufSize = sizeof(buf);
        DWORD type = 0;
        res = RegQueryValueExW(hKey, L"AssignmentSetOverride", nullptr, &type, buf, &bufSize);
        if (res != ERROR_SUCCESS || type != REG_BINARY || bufSize < sizeof(ULONG64)) {
            log << "[Query IRQ] AssignmentSetOverride not present or unreadable for " << CoreHelpers::WideToUtf8(instanceId) << "\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        uint64_t mask = 0;
        memcpy(&mask, buf, sizeof(mask));
        if (mask == 0) {
            log << "[Query IRQ] AssignmentSetOverride present but mask == 0 for " << CoreHelpers::WideToUtf8(instanceId) << "\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        int cpu = -1;
        for (int i = 0; i < 64; ++i) {
            if (mask & (1ULL << i)) { cpu = i; break; }
        }
        if (cpu < 0) {
            log << "[Query IRQ] No CPU bit found in mask for " << CoreHelpers::WideToUtf8(instanceId) << "\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return false;
        }

        outCpu = cpu;  // logical CPU (Task Manager numbering)
        log << "[Query IRQ] Device instance " << CoreHelpers::WideToUtf8(instanceId) << " -> LP" << cpu << " (mask=0x";
        char tmp2[32]; snprintf(tmp2, sizeof(tmp2), "%llX", (long long)mask);
        log << tmp2 << ")\n";
        RegCloseKey(hKey);
        logOut = log.str();
        return true;
    }

    bool GetAdapterInterruptType(const std::string &adapterName,
                                 AdapterInterruptType &outType,
                                 std::string &outDescription,
                                 std::string &logOut) {
        outType = AdapterInterruptType::Unknown;
        outDescription = "Unknown";
        logOut.clear();
        std::ostringstream log;

        if (adapterName.empty()) {
            log << "[IRQ Type] Empty adapter name\n";
            logOut = log.str();
            return false;
        }

        std::string resolveLog;
        std::wstring instanceId = FindEthercatNicInstanceId(adapterName, resolveLog);
        log << resolveLog;
        if (instanceId.empty()) {
            log << "[IRQ Type] Could not resolve device instance ID\n";
            logOut = log.str();
            return false;
        }

        // Check MSISupported in MessageSignaledInterruptProperties
        std::wstring regPath = L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId
            + L"\\Device Parameters\\Interrupt Management\\MessageSignaledInterruptProperties";
        HKEY hKey = nullptr;
        LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, regPath.c_str(), 0, KEY_READ, &hKey);
        if (res != ERROR_SUCCESS) {
            // Key does not exist → Legacy INTx (no MSI properties exposed)
            outType = AdapterInterruptType::LegacyIntx;
            outDescription = "Legacy INTx";
            log << "[IRQ Type] No MSI properties key — NIC uses Legacy INTx (IRQ affinity NOT controllable via registry)\n";
            logOut = log.str();
            return true;
        }

        DWORD msiSupported = 0;
        DWORD bufSize = sizeof(msiSupported);
        res = RegQueryValueExW(hKey, L"MSISupported", nullptr, nullptr,
                               reinterpret_cast<BYTE*>(&msiSupported), &bufSize);
        RegCloseKey(hKey);

        if (res == ERROR_SUCCESS && msiSupported != 0) {
            outType = AdapterInterruptType::MsiX;
            outDescription = "MSI-X";
            log << "[IRQ Type] MSISupported=" << msiSupported
                << " — NIC uses MSI/MSI-X (IRQ affinity CAN be controlled via registry)\n";
        } else {
            outType = AdapterInterruptType::LegacyIntx;
            outDescription = "Legacy INTx";
            log << "[IRQ Type] MSISupported=" << msiSupported
                << " — NIC uses Legacy INTx (IRQ affinity NOT controllable via registry)\n";
        }

        logOut = log.str();
        return true;
    }

    bool CheckEthercatIrqAffinityReady(const std::string &adapterName,
                                       bool &outReady,
                                       std::string &outMessage,
                                       std::string &outLog) {
        outReady = false;
        outMessage.clear();
        outLog.clear();
        std::ostringstream log;

        // Compute expected EtherCAT NIC CPU using CpuTopology (mirrors ApplySystemWideIrqAffinityToCpu0)
        CpuTopology::EnsurePopulated();
        int physCoreCount = CpuTopology::PhysicalCoreCount();
        int rtCore = CpuIsolation::GetRtPhysicalCoreIndex();
        int kExpectedEcatCpu;
        if (physCoreCount >= 8) {
            // EtherCAT on core N-2 (matches ApplySystemWideIrqAffinityToCpu0 formula)
            int ecatPhys = physCoreCount - 2;
            const auto* ci = CpuTopology::FindCoreByPhysIdx(ecatPhys);
            kExpectedEcatCpu = (ci && !ci->lps.empty()) ? ci->lps[0] : ecatPhys;
        } else if (physCoreCount >= 4) {
            // Sync with ApplySystemWideIrqAffinityToCpu0 switch mapping
            int ecatPhys;
            switch (physCoreCount) {
                case 7:  ecatPhys = 5; break;
                case 6:  ecatPhys = 4; break;
                case 5:  ecatPhys = 3; break;
                case 4:
                default: ecatPhys = 2; break;
            }
            const auto* ci = CpuTopology::FindCoreByPhysIdx(ecatPhys);
            kExpectedEcatCpu = (ci && !ci->lps.empty()) ? ci->lps[0] : ecatPhys;
        } else {
            const auto* ci1 = CpuTopology::FindCoreByPhysIdx(1);
            kExpectedEcatCpu = (ci1 && !ci1->lps.empty()) ? ci1->lps[0] : 1;
        }
        // Ensure expected CPU doesn't overlap with RT core
        {
            DWORD_PTR rtMask = CpuIsolation::GetRtCoreMask();
            if (rtMask == 0) rtMask = CpuTopology::GetCoreMask(rtCore);
            if (rtMask == 0 && rtCore >= 0 && rtCore < 64) rtMask = (DWORD_PTR)1 << rtCore;
            if (rtMask && kExpectedEcatCpu >= 0 && kExpectedEcatCpu < 64 && (rtMask & ((DWORD_PTR)1 << kExpectedEcatCpu))) {
                // Pick the nearest non-RT logical CPU
                int alt = CpuTopology::FindBestNonRtLp(rtMask);
                if (alt >= 0) kExpectedEcatCpu = alt;
            }
        }

        // 1) Check interrupt type
        AdapterInterruptType intType = AdapterInterruptType::Unknown;
        std::string intTypeDesc;
        std::string intTypeLog;
        GetAdapterInterruptType(adapterName, intType, intTypeDesc, intTypeLog);
        log << intTypeLog;

        if (intType == AdapterInterruptType::LegacyIntx) {
            // Legacy INTx: IRQ routing is controlled by I/O APIC, not registry.
            // We cannot verify or enforce CPU affinity for this NIC type.
            outReady = false;
            outMessage = "網卡使用 Legacy INTx 中斷，無法透過軟體綁定 CPU。\n"
                         "EtherCAT 通訊可能受到中斷干擾。\n"
                         "建議在 BIOS 中設定或更換為支援 MSI-X 的驅動程式。";
            log << "[IRQ Check] Legacy INTx — cannot verify or control IRQ affinity\n";
            outLog = log.str();
            return true;
        }

        // 2) Read actual (registry) IRQ CPU
        int actualCpu = -1;
        std::string cpuLog;
        bool hasCpu = GetAdapterEffectiveIrqCpu(adapterName, actualCpu, cpuLog);
        log << cpuLog;

        if (!hasCpu || actualCpu < 0) {
            // No affinity policy set at all — IRQ is using default routing.
            outReady = false;
            outMessage = "網卡 IRQ affinity 尚未設定。\n"
                         "請先選擇網卡讓程式寫入 IRQ 配置，\n"
                         "然後【重新啟動電腦】使設定生效。";
            log << "[IRQ Check] No IRQ affinity policy found — reboot needed after first configuration\n";
            outLog = log.str();
            return true;
        }

        // 3) Compare actual vs expected
        if (actualCpu == kExpectedEcatCpu) {
            outReady = true;
            outMessage = "網卡 IRQ 已正確綁定到 CPU " + std::to_string(kExpectedEcatCpu) + "。";
            log << "[IRQ Check] ✓ Actual CPU " << actualCpu << " matches expected CPU " << kExpectedEcatCpu << "\n";
        } else {
            outReady = false;
            outMessage = "網卡 IRQ 目前綁定在 CPU " + std::to_string(actualCpu)
                       + "，但設定目標為 CPU " + std::to_string(kExpectedEcatCpu) + "。\n"
                       + "IRQ affinity 設定已寫入登錄檔，但【需要重新啟動電腦】才會生效！\n"
                       + "在重啟之前執行 EtherCAT 通訊可能受中斷干擾導致不穩定。";
            log << "[IRQ Check] ✗ Actual CPU " << actualCpu << " ≠ expected CPU " << kExpectedEcatCpu << " — REBOOT REQUIRED\n";
        }

        outLog = log.str();
        return true;
    }

    bool GetAdapterActualIrqNumber(const std::string &adapterName,
                                   unsigned long long &outIrqNumber,
                                   std::string &outDeviceName,
                                   std::string &logOut) {
        outIrqNumber = 0;
        outDeviceName.clear();
        logOut.clear();
        std::ostringstream log;

        if (adapterName.empty()) {
            log << "[Actual IRQ] Empty adapter name\n";
            logOut = log.str();
            return false;
        }

        // Extract clean adapter names for matching.
        // The adapter display may be "Intel(R) I210 ... #2 [5652EACB]" or
        // "Network adapter 'TwinCAT-Intel...' on local host".
        // WMI Win32_IRQResource.Name is the driver-provided device description
        // which usually matches the friendly name without brackets.
        std::string nameClean;
        {
            std::string s = adapterName;
            // Strip trailing " [XXXX]" suffix
            auto bracket = s.rfind(" [");
            if (bracket != std::string::npos && bracket + 3 < s.size()) {
                // Only strip if it looks like a short hex/guid suffix
                auto close = s.find(']', bracket);
                if (close != std::string::npos && close - bracket <= 14)
                    s = s.substr(0, bracket);
            }
            // Strip "Network adapter '...' on local host" wrapper
            auto q1 = s.find('\'');
            if (q1 != std::string::npos) {
                auto q2 = s.rfind('\'');
                if (q2 != std::string::npos && q2 > q1)
                    s = s.substr(q1 + 1, q2 - q1 - 1);
            }
            // Trim whitespace
            while (!s.empty() && s.front() == ' ') s.erase(0, 1);
            while (!s.empty() && s.back() == ' ') s.pop_back();
            nameClean = s;
        }

        // Build PowerShell: dump ALL IRQ resources, we'll match in C++
        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"irq", 0, tempFile);

        std::wstring psCmd = L"powershell.exe -NoProfile -NoLogo -ExecutionPolicy Bypass -Command \"";
        psCmd += L"Get-CimInstance Win32_IRQResource -ErrorAction SilentlyContinue | "
                 L"ForEach-Object { Write-Output ('IRQ=' + [string][uint32]$_.IRQNumber + '|NAME=' + $_.Name) }\"";

        log << "[Actual IRQ] Querying WMI for IRQ of '" << nameClean << "'...\n";

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        // Use cmd.exe /c wrapper so output redirection (>) works correctly.
        // Direct CreateProcessW with PowerShell does not handle shell redirects.
        // chcp 65001 ensures UTF-8 output for Chinese adapter names.
        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & " + psCmd + L" > \"" + tempFile + L"\" 2>&1\"";

        if (CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 15000); // 15s timeout for WMI
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            DWORD err = GetLastError();
            log << "[Actual IRQ] CreateProcessW failed (err=" << err << ")\n";
        }

        // Read ALL output and match in C++ (more robust than PowerShell -like)
        bool found = false;
        HANDLE hFile = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buf[8192] = {};
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
                buf[bytesRead] = '\0';
                std::string output(buf);

                // Build variants of the adapter name for matching
                auto toLower = [](std::string s) {
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    return s;
                };
                std::string searchLower = toLower(nameClean);

                // Try matching: check each "IRQ=...|NAME=..." line
                size_t pos = 0;
                while (pos < output.size()) {
                    auto eqPos = output.find("IRQ=", pos);
                    if (eqPos == std::string::npos) break;
                    auto pipePos = output.find('|', eqPos);
                    if (pipePos == std::string::npos) break;
                    auto nameEq = output.find("NAME=", pipePos);
                    if (nameEq == std::string::npos) break;
                    auto nlPos = output.find('\n', nameEq);

                    std::string irqStr = output.substr(eqPos + 4, pipePos - eqPos - 4);
                    std::string wmiName = output.substr(nameEq + 5,
                        (nlPos != std::string::npos) ? nlPos - nameEq - 5 : std::string::npos);
                    // Trim trailing \r\n
                    while (!wmiName.empty() && (wmiName.back() == '\r' || wmiName.back() == '\n' || wmiName.back() == ' '))
                        wmiName.pop_back();

                    std::string wmiLower = toLower(wmiName);

                    // Match: WMI name contains the search term, or vice versa
                    bool matched = false;
                    if (wmiLower.find(searchLower) != std::string::npos) {
                        matched = true;
                    } else if (searchLower.find(wmiLower) != std::string::npos && wmiLower.size() > 5) {
                        matched = true;
                    } else {
                        // Try matching first significant word (e.g. "Intel" or "TwinCAT")
                        auto firstSpace = searchLower.find(' ');
                        if (firstSpace != std::string::npos && firstSpace > 2) {
                            std::string firstWord = searchLower.substr(0, firstSpace);
                            if (wmiLower.find(firstWord) != std::string::npos)
                                matched = true;
                        }
                    }

                    if (matched) {
                        try {
                            outIrqNumber = std::stoull(irqStr);
                        } catch (...) { outIrqNumber = 0; }
                        outDeviceName = wmiName;
                        const char* kind = (outIrqNumber > 0xFFFF0000ULL) ? "MSI-X" : "Legacy INTx";
                        log << "[Actual IRQ] Found: IRQ " << outIrqNumber
                            << " (" << kind << ") — " << wmiName << "\n";
                        found = true;
                        break;  // first match wins
                    }

                    pos = (nlPos != std::string::npos) ? nlPos + 1 : nameEq + 1;
                }
            }
            CloseHandle(hFile);
        } else {
            log << "[Actual IRQ] Failed to read PowerShell output\n";
        }

        DeleteFileW(tempFile);
        logOut = log.str();
        return found;
    }

    bool GetSystemIrqAffinitySummary(const std::string &ethercatAdapterName, std::string &summary, std::string &logOut) {
        summary.clear();
        logOut.clear();
        std::ostringstream log;
        std::ostringstream summaryStream;

        std::string resolveLog;
        std::wstring ethercatInstanceId = FindEthercatNicInstanceId(ethercatAdapterName, resolveLog);
        log << resolveLog;
        if (!ethercatInstanceId.empty()) {
            log << "[Other IRQs] Resolved EtherCAT adapter instance: " << CoreHelpers::WideToUtf8(ethercatInstanceId) << "\n";
        } else {
            log << "[Other IRQs] WARNING: Could not resolve EtherCAT adapter instance\n";
        }

        bool first = true;
        auto appendCategory = [&](const std::wstring &categoryName, const std::vector<std::wstring> &instanceIds) {
            if (!first) {
                summaryStream << " | ";
            }
            first = false;

            if (instanceIds.empty()) {
                summaryStream << CoreHelpers::WideToUtf8(categoryName) << ": none";
                return;
            }

            bool anyAssigned = false;
            for (const auto &id : instanceIds) {
                int cpu = -1;
                std::string deviceLog;
                if (GetDeviceInstanceEffectiveIrqCpu(id, cpu, deviceLog)) {
                    summaryStream << CoreHelpers::WideToUtf8(categoryName) << ": CPU" << cpu;
                    anyAssigned = true;
                    break;
                }
            }

            if (!anyAssigned) {
                summaryStream << CoreHelpers::WideToUtf8(categoryName) << ": N/A";
            }
        };

        // GPU (NVIDIA / display class)
        {
            std::string enumLog;
            auto gpus = FindDeviceInstanceIdsByClass(kGuidDisplay, L"nvidia", enumLog);
            log << enumLog;
            appendCategory(L"GPU", gpus);
        }

        // NVMe controllers
        {
            std::string enumLog;
            auto nvmes = FindDeviceInstanceIdsByClass(kGuidScsiAdapter, L"nvme", enumLog);
            log << enumLog;
            if (nvmes.empty()) {
                std::string altLog;
                auto nvmeAlt = FindDeviceInstanceIdsByClass(kGuidScsiAdapter, L"nvm express", altLog);
                log << altLog;
                nvmes = std::move(nvmeAlt);
            }
            appendCategory(L"NVMe", nvmes);
        }

        // SATA AHCI controllers (HDC class, shares NVMe storage core)
        {
            std::string enumLog;
            auto satas = FindDeviceInstanceIdsByClass(kGuidHdc, L"ahci", enumLog);
            log << enumLog;
            if (satas.empty()) {
                std::string altLog;
                auto sataAlt = FindDeviceInstanceIdsByClass(kGuidHdc, L"sata", altLog);
                log << altLog;
                satas = std::move(sataAlt);
            }
            appendCategory(L"SATA", satas);
        }

        // USB host controllers — with redirect status annotation
        {
            std::string enumLog;
            auto usb = FindDeviceInstanceIdsByClass(kGuidUsb, L"host controller", enumLog);
            log << enumLog;
            if (usb.empty()) {
                std::string xhciLog;
                auto usbXhci = FindDeviceInstanceIdsByClass(kGuidUsb, L"xhci", xhciLog);
                log << xhciLog;
                usb = std::move(usbXhci);
            }
            if (usb.empty()) {
                std::string ehciLog;
                auto usbEhci = FindDeviceInstanceIdsByClass(kGuidUsb, L"ehci", ehciLog);
                log << ehciLog;
                usb = std::move(usbEhci);
            }

            // Standard registry read
            appendCategory(L"USB", usb);

            // Annotate with redirect status if available
            if (g_usbIrqRedirectAttempted) {
                if (g_usbIrqNeedsAdmin) {
                    summaryStream << " [需管理員權限]";
                } else if (g_usbIrqActualCpu >= 0 && g_usbIrqActualCpu == g_usbIrqTargetCpu) {
                    summaryStream << " → CPU" << g_usbIrqTargetCpu << " ✓";
                    if (g_usbIrqTotalSet > 0) {
                        summaryStream << " 需重開機";
                    }
                } else if (g_usbIrqActualCpu >= 0 && g_usbIrqActualCpu != g_usbIrqTargetCpu) {
                    summaryStream << " [目標CPU" << g_usbIrqTargetCpu
                                  << ", 實際CPU" << g_usbIrqActualCpu << "]";
                } else if (g_usbIrqLineBasedCount > 0) {
                    summaryStream << " [INTx, 需重開機]";
                } else if (g_usbIrqTotalSet > 0) {
                    summaryStream << " [需重開機]";
                }
            }
        }

        // Audio devices (Media class, shares USB I/O core)
        {
            std::string enumLog;
            auto audios = FindDeviceInstanceIdsByClass(kGuidMedia, L"high definition audio", enumLog);
            log << enumLog;
            if (audios.empty()) {
                std::string altLog;
                auto audioAlt = FindDeviceInstanceIdsByClass(kGuidMedia, L"audio", altLog);
                log << altLog;
                audios = std::move(audioAlt);
            }
            appendCategory(L"Audio", audios);
        }

        // Other network adapters (Gaming NICs) excluding selected EtherCAT NIC
        {
            std::string enumLog;
            auto nics = FindDeviceInstanceIdsByClass(kGuidNet, L"", enumLog);
            log << enumLog;

            int rtCore = CpuIsolation::GetRtPhysicalCoreIndex();
            DWORD_PTR rtMask = CpuIsolation::GetRtCoreMask();
            if (rtMask == 0 && rtCore >= 0 && rtCore < 64) {
                rtMask = (DWORD_PTR)1 << rtCore;
            }
            std::vector<std::wstring> otherNics;
            bool anyOnRt = false;
            for (const auto &id : nics) {
                if (!ethercatInstanceId.empty() && id == ethercatInstanceId) continue;
                // Skip EtherCAT NIC by PCI ID
                std::wstring lower = id;
                std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
                if (lower.find(L"ven_8086") != std::wstring::npos &&
                    (lower.find(L"dev_1533") != std::wstring::npos ||
                     lower.find(L"dev_15f2") != std::wstring::npos ||
                     lower.find(L"dev_15f3") != std::wstring::npos)) continue;

                int cpu = -1;
                std::string devLog;
                if (GetDeviceInstanceEffectiveIrqCpu(id, cpu, devLog) && cpu >= 0 && cpu < 64) {
                    if (rtMask & ((DWORD_PTR)1 << cpu)) anyOnRt = true;
                }
                otherNics.push_back(id);
            }

            if (anyOnRt) {
                log << "[Other IRQs] ⚠ NIC(s) on RT core mask 0x"
                    << std::hex << rtMask << std::dec
                    << " — reboot after Admin IRQ affinity write\n";
            }

            appendCategory(L"其他 NIC", otherNics);

            // Annotate from startup redirect status
            if (g_nicIrqRedirectAttempted) {
                if (anyOnRt) {
                    // NIC(s) still showing RT core in registry — redirect incomplete
                    summaryStream << " ⚠ 佔用RT核心 需重開機";
                } else if (g_nicIrqTargetCpu >= 0) {
                    // All NICs away from RT core — show target CPU
                    summaryStream << " → CPU" << g_nicIrqTargetCpu << " ✓";
                    // Only show "需重開機" if registry was actually written this session
                    if (g_nicIrqTotalSet > 0) {
                        summaryStream << " 需重開機";
                    }
                }
            } else if (anyOnRt) {
                summaryStream << " ⚠ 佔用RT CPU" << rtCore;
            }
        }

        summary = summaryStream.str();
        logOut = log.str();
        return true;
    }

    // ── Real-time Windows power policy ──
    // Activates Ultimate Performance (or High Performance fallback) power scheme,
    // disables core parking, USB selective suspend, and disk timeout.
    // Uses powrprof.h Power* APIs (C++ only, no PowerShell).
    bool ApplyRealtimePowerPolicy(std::string& logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[Power Policy] === Applying real-time Windows power configuration ===\n";

        // ── 1. Activate Ultimate Performance power scheme ──
        // GUID: {e9a42b02-d5df-448d-aa00-03f14749eb61}
        // Fallback: High Performance {8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c}
        {
            GUID ultimateGuid = {0xe9a42b02, 0xd5df, 0x448d,
                {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};
            GUID highPerfGuid = {0x8c5e7fda, 0xe8bf, 0x4a96,
                {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};

            DWORD result = PowerSetActiveScheme(NULL, &ultimateGuid);
            if (result == ERROR_SUCCESS) {
                log << "[Power Policy] ✓ Activated 'Ultimate Performance' power scheme\n";
            } else {
                log << "[Power Policy] Ultimate Performance not available (err="
                    << result << "), trying High Performance...\n";
                result = PowerSetActiveScheme(NULL, &highPerfGuid);
                if (result == ERROR_SUCCESS) {
                    log << "[Power Policy] ✓ Activated 'High Performance' power scheme\n";
                } else {
                    log << "[Power Policy] ⚠ Cannot set power scheme (err=" << result
                        << "), may need Admin; continuing with current scheme\n";
                }
            }
        }

        // ── 2. Disable core parking (0% parking) ──
        // Subgroup: Processor power management {54533251-82be-4824-96c1-47b60b740d00}
        // Setting:  Processor performance core parking min cores {0cc5b647-c74e-417b-9229-73e8462a09c4}
        {
            GUID *pActiveScheme = nullptr;
            DWORD result = PowerGetActiveScheme(NULL, &pActiveScheme);
            if (result != ERROR_SUCCESS || !pActiveScheme) {
                log << "[Power Policy] ⚠ Cannot query active scheme for core parking (err="
                    << result << ")\n";
            } else {
                GUID subProcessorGuid = {0x54533251, 0x82be, 0x4824,
                    {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
                GUID setParkingGuid  = {0x0cc5b647, 0xc74e, 0x417b,
                    {0x92, 0x29, 0x73, 0xe8, 0x46, 0x2a, 0x09, 0xc4}};

                DWORD parkingValue = 0;  // 0% = no parking
                result = PowerWriteACValueIndex(NULL, pActiveScheme,
                    &subProcessorGuid, &setParkingGuid, parkingValue);
                if (result == ERROR_SUCCESS) {
                    result = PowerSetActiveScheme(NULL, pActiveScheme);
                    log << "[Power Policy] ✓ Core parking disabled (min cores = 0%)\n";
                } else {
                    log << "[Power Policy] ⚠ Cannot write core parking setting (err="
                        << result << "), may need Admin\n";
                }

                LocalFree(pActiveScheme);
            }
        }

        // ── 3. Disable USB selective suspend ──
        // Subgroup: USB settings {2a737441-1930-4402-8d77-b2bebba308a3}
        // Setting:  USB selective suspend {48e6b7a6-50f5-4782-a5d4-53bb8f07e226}
        {
            GUID *pActiveScheme = nullptr;
            DWORD result = PowerGetActiveScheme(NULL, &pActiveScheme);
            if (result != ERROR_SUCCESS || !pActiveScheme) {
                log << "[Power Policy] ⚠ Cannot query active scheme for USB suspend (err="
                    << result << ")\n";
            } else {
                GUID subUsbGuid   = {0x2a737441, 0x1930, 0x4402,
                    {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};
                GUID setUsbSuspend = {0x48e6b7a6, 0x50f5, 0x4782,
                    {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}};

                DWORD usbValue = 0;  // 0 = disabled
                result = PowerWriteACValueIndex(NULL, pActiveScheme,
                    &subUsbGuid, &setUsbSuspend, usbValue);
                if (result == ERROR_SUCCESS) {
                    result = PowerSetActiveScheme(NULL, pActiveScheme);
                    log << "[Power Policy] ✓ USB selective suspend disabled\n";
                } else {
                    log << "[Power Policy] ⚠ Cannot write USB suspend setting (err="
                        << result << "), may need Admin\n";
                }

                LocalFree(pActiveScheme);
            }
        }

        // ── 4. Disable disk timeout (never turn off hard disk on AC) ──
        // Subgroup: Hard disk {0012ee47-9041-4b5d-9b77-535fba8b1442}
        // Setting:  Turn off hard disk after {6738e2c4-e8a5-4a42-b16a-e040e769756e}
        {
            GUID *pActiveScheme = nullptr;
            DWORD result = PowerGetActiveScheme(NULL, &pActiveScheme);
            if (result != ERROR_SUCCESS || !pActiveScheme) {
                log << "[Power Policy] ⚠ Cannot query active scheme for disk timeout (err="
                    << result << ")\n";
            } else {
                GUID subDiskGuid   = {0x0012ee47, 0x9041, 0x4b5d,
                    {0x9b, 0x77, 0x53, 0x5f, 0xba, 0x8b, 0x14, 0x42}};
                GUID setDiskTimeout = {0x6738e2c4, 0xe8a5, 0x4a42,
                    {0xb1, 0x6a, 0xe0, 0x40, 0xe7, 0x69, 0x75, 0x6e}};

                DWORD diskValue = 0;  // 0 seconds = never
                result = PowerWriteACValueIndex(NULL, pActiveScheme,
                    &subDiskGuid, &setDiskTimeout, diskValue);
                if (result == ERROR_SUCCESS) {
                    result = PowerSetActiveScheme(NULL, pActiveScheme);
                    log << "[Power Policy] ✓ Disk timeout disabled (never turn off)\n";
                } else {
                    log << "[Power Policy] ⚠ Cannot write disk timeout setting (err="
                        << result << "), may need Admin\n";
                }

                LocalFree(pActiveScheme);
            }
        }

        log << "[Power Policy] === Power configuration complete ===\n";
        logOut = log.str();
        return true;
    }

    bool SetProcessorIdleDisableForRtCore(std::string& logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[IdleDisable] Setting PROCESSOR_IDLE_DISABLE for RT core LPs...\n";

        // Get the RT core's full mask (all logical processors of the RT physical core)
        DWORD_PTR rtCoreMask = CoreHelpers::CpuIsolation::GetRtCoreMask();
        if (rtCoreMask == 0) {
            log << "[IdleDisable] RT core mask is zero — CpuIsolation not initialized?\n";
            logOut = log.str();
            return false;
        }

        // Extract logical processor indices from the mask
        std::vector<int> rtLps;
        DWORD_PTR m = rtCoreMask;
        int idx = 0;
        while (m != 0) {
            if (m & 1) rtLps.push_back(idx);
            m >>= 1;
            ++idx;
        }

        if (rtLps.empty()) {
            log << "[IdleDisable] No RT core LPs found in mask 0x"
                << std::hex << rtCoreMask << std::dec << "\n";
            logOut = log.str();
            return false;
        }

        log << "[IdleDisable] RT core logical processors:";
        for (int lp : rtLps) log << " " << lp;
        log << "\n";

        // For each RT LP, create/write HKLM\...\Control\Processor\{LP}\PROCESSOR_IDLE_DISABLE=1
        // This registry key is read by the kernel power manager at boot time.
        int successCount = 0;
        int failCount = 0;
        for (int lp : rtLps) {
            std::wstring keyPath = L"SYSTEM\\CurrentControlSet\\Control\\Processor\\"
                                   + std::to_wstring(lp);

            HKEY hKey = nullptr;
            LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(),
                                          0, nullptr, REG_OPTION_NON_VOLATILE,
                                          KEY_SET_VALUE | KEY_QUERY_VALUE,
                                          nullptr, &hKey, nullptr);
            if (result != ERROR_SUCCESS) {
                log << "[IdleDisable]  Cannot create/open key for LP " << lp
                    << " (err=" << result << ")\n";
                ++failCount;
                continue;
            }

            // Read current value first
            DWORD currentValue = 0;
            DWORD dataSize = sizeof(DWORD);
            DWORD type = REG_DWORD;
            LONG queryResult = RegQueryValueExW(hKey, L"PROCESSOR_IDLE_DISABLE",
                                                 nullptr, &type,
                                                 reinterpret_cast<BYTE*>(&currentValue),
                                                 &dataSize);

            if (queryResult == ERROR_SUCCESS && currentValue == 1) {
                log << "[IdleDisable]  LP " << lp
                    << " — already set to 1 (no change needed)\n";
                ++successCount;
                RegCloseKey(hKey);
                continue;
            }

            // Write PROCESSOR_IDLE_DISABLE = 1
            DWORD newValue = 1;
            result = RegSetValueExW(hKey, L"PROCESSOR_IDLE_DISABLE", 0, REG_DWORD,
                                    reinterpret_cast<const BYTE*>(&newValue),
                                    sizeof(DWORD));
            if (result == ERROR_SUCCESS) {
                log << "[IdleDisable]  LP " << lp
                    << " — PROCESSOR_IDLE_DISABLE set to 1"
                    << (queryResult == ERROR_SUCCESS
                        ? " (was " + std::to_string(currentValue) + ")"
                        : " (created)")
                    << "\n";
                ++successCount;
            } else {
                log << "[IdleDisable]  LP " << lp
                    << " — failed to write (err=" << result << ")\n";
                ++failCount;
            }

            RegCloseKey(hKey);
        }

        if (failCount > 0) {
            log << "[IdleDisable] " << successCount << " OK, " << failCount
                << " failed (non-fatal)\n";
        } else {
            log << "[IdleDisable] All " << successCount << " LP(s) configured OK\n";
        }
        log << "[IdleDisable] Note: changes require reboot to take effect\n";

        logOut = log.str();
        return (failCount == 0);
    }

    bool AddDefenderExclusionForSelf(std::string& logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[Defender] Adding Windows Defender exclusion for BigCat...\n";

        // 1. Get own executable path
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
            log << "[Defender] GetModuleFileNameW failed (err=" << GetLastError() << ")\n";
            logOut = log.str();
            return false;
        }

        // 2. Extract directory (strip filename)
        std::wstring exeDir = exePath;
        auto pos = exeDir.rfind(L'\\');
        if (pos != std::wstring::npos) {
            exeDir = exeDir.substr(0, pos);
        }
        log << "[Defender] Executable directory: " << CoreHelpers::WideToUtf8(exeDir) << "\n";

        // 3. Build PowerShell command: check if already excluded, add if not
        // Escape single quotes in path for PowerShell
        std::wstring escapedDir;
        for (wchar_t c : exeDir) {
            if (c == L'\'') escapedDir += L"''";
            else escapedDir += c;
        }
        std::wstring psCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        psCmd += L"$path = '"; psCmd += escapedDir; psCmd += L"'; ";
        psCmd += L"try { ";
        psCmd += L"  $pref = Get-MpPreference -ErrorAction Stop; ";
        psCmd += L"  $existing = $pref.ExclusionPath; ";
        psCmd += L"  if ($existing -contains $path) { ";
        psCmd += L"    Write-Output 'ALREADY_EXCLUDED'; ";
        psCmd += L"  } else { ";
        psCmd += L"    Add-MpPreference -ExclusionPath $path -ErrorAction Stop; ";
        psCmd += L"    Write-Output 'ADDED'; ";
        psCmd += L"  } ";
        psCmd += L"} catch { ";
        psCmd += L"  Write-Output ('ERROR: ' + $_.Exception.Message); ";
        psCmd += L"}";
        psCmd += L"\"";

        // 4. Execute via cmd.exe /c wrapper (same pattern as other PS calls in this file)
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        wchar_t tempFile[MAX_PATH] = {};
        GetTempFileNameW(tempDir, L"DEF", 0, tempFile);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};

        std::wstring cmdLine = L"cmd.exe /c \"chcp 65001 > nul & " + psCmd +
                               L" > \"" + tempFile + L"\" 2>&1\"";

        if (CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 30000); // 30s timeout for Defender WMI
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        } else {
            DWORD err = GetLastError();
            log << "[Defender] CreateProcessW failed (err=" << err << ")\n";
            DeleteFileW(tempFile);
            logOut = log.str();
            return false;
        }

        // 5. Read output
        bool success = false;
        HANDLE hFile = CreateFileW(tempFile, GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (hFile != INVALID_HANDLE_VALUE) {
            char buf[4096] = {};
            DWORD bytesRead = 0;
            if (ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr)) {
                buf[bytesRead] = '\0';
                std::string output(buf);
                // Trim trailing whitespace
                while (!output.empty() && (output.back() == '\r' || output.back() == '\n' || output.back() == ' '))
                    output.pop_back();

                if (output == "ALREADY_EXCLUDED") {
                    log << "[Defender] BigCat directory already in Defender exclusion list\n";
                    success = true;
                } else if (output == "ADDED") {
                    log << "[Defender] Successfully added BigCat directory to Defender exclusion list\n";
                    success = true;
                } else {
                    log << "[Defender] Unexpected PowerShell output: " << output << "\n";
                }
            }
            CloseHandle(hFile);
        }
        DeleteFileW(tempFile);

        if (!success) {
            log << "[Defender] Failed to configure Defender exclusion (non-fatal)\n";
        }

        logOut = log.str();
        return success;
    }

    bool SetNetworkThrottlingForRealtime(std::string& logOut) {
        logOut.clear();
        std::ostringstream log;
        log << "[NetThrottle] Setting NetworkThrottlingIndex to 0xFFFFFFFF...\n";

        const wchar_t* keyPath = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile";
        const wchar_t* valueName = L"NetworkThrottlingIndex";

        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey);
        if (result != ERROR_SUCCESS) {
            log << "[NetThrottle] Cannot open registry key (err=" << result
                << "), may need Admin\n";
            logOut = log.str();
            return false;
        }

        DWORD currentValue = 0;
        DWORD dataSize = sizeof(DWORD);
        DWORD type = REG_DWORD;
        RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&currentValue), &dataSize);

        DWORD newValue = 0xFFFFFFFF;
        if (currentValue == newValue) {
            log << "[NetThrottle] ✓ Already set to 0xFFFFFFFF (no change needed)\n";
            RegCloseKey(hKey);
            logOut = log.str();
            return true;
        }

        result = RegSetValueExW(hKey, valueName, 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&newValue), sizeof(DWORD));
        if (result == ERROR_SUCCESS) {
            log << "[NetThrottle] ✓ Set NetworkThrottlingIndex = 0xFFFFFFFF "
                << "(was 0x" << std::hex << currentValue << std::dec << ")\n";
        } else {
            log << "[NetThrottle] Failed to set value (err=" << result << ")\n";
        }

        RegCloseKey(hKey);
        logOut = log.str();
        return result == ERROR_SUCCESS;
    }

}
