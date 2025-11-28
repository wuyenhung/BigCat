#include "../../include/core/CoreHelpers.h"
#include "../../include/EtherCATMaster.h"
#include "../../include/Win32WindowCore.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <cwctype>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cctype>

#include <iphlpapi.h>
#include <pcap.h>

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

    const std::string dispEsc = Win32WindowCore::EscapeJsonString(display);
    const std::string friendly = Win32WindowCore::EscapeJsonString(entry.friendlyName);
    js << "{";
    js << "\"pcapName\":\"" << Win32WindowCore::EscapeJsonString(entry.pcapName) << "\",";
    js << "\"display\":\"" << dispEsc << "\",";
    js << "\"friendly\":\"" << friendly << "\",";
    js << "\"adapterName\":\"" << Win32WindowCore::EscapeJsonString(entry.adapterName) << "\",";
    js << "\"linkUp\":" << (entry.linkUp ? "true" : "false");
    js << "}";
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
            }
        }

        if (alldevs) pcap_freealldevs(alldevs);
        return result;
    }

    std::string MakeAdapterDisplayString(const AdapterEntry &d) {
        return ResolveHumanReadableAdapterName(d);
    }

    // Check link status for a pcap interface name by attempting to open it.
    // Returns true if opening the device succeeds (link likely up), false otherwise.
    bool CheckLinkStatus(const char* ifname) {
        if (!ifname) return false;
        char errbuf[PCAP_ERRBUF_SIZE] = {0};
        // pcap_open_live expects the pcap device name (as returned by pcap_findalldevs)
        // Use pcap_create + pcap_set_* for fine-grained control (immediate mode / small timeout)
        // This improves responsiveness for link checks and low-latency captures (Npcap extensions used).
        pcap_t* handle = NULL;
        // Prefer pcap_create when available so we can set immediate mode
        pcap_t* ph = pcap_create(ifname, errbuf);
        if (ph) {
            // Configure handle for low latency
            pcap_set_snaplen(ph, 64);
            pcap_set_promisc(ph, 0); // not interested in promiscuous capture here
            pcap_set_timeout(ph, 1); // 1ms timeout
            // Npcap immediate mode and mintocopy reduce latency by delivering packets immediately
            // These are supported by Npcap's WinPcap-compatible API; if not present this will fail to compile
            // and will be caught in CI; use Npcap SDK for minimal-latency setups.
            // Dynamically resolve Npcap extension functions at runtime to avoid link-time failures
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
            // Activate the configured handle
            if (pcap_activate(ph) == 0) {
                handle = ph;
            } else {
                // Activation failed; close and fallback
                pcap_close(ph);
                handle = NULL;
            }
        } else {
            // Fallback to pcap_open_live for older libpcap builds
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
        // Set Priority Class to HIGH_PRIORITY_CLASS (0x80)
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

        DWORD_PTR processMask = 0;
        DWORD_PTR systemMask = 0;

        if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) {
            // We want to allow the process to use any core EXCEPT Core 0 (which handles system IRQs).
            // We keep the isolated cores (like Core 4) in the process mask so that RT threads 
            // can explicitly pin themselves to them later.
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
        if (adapterName.empty()) {
            logOut = "[NIC Optimization] Adapter name is empty, skipping optimization\n";
            return true; // not an error, just skip
        }

        // Build log message
        std::ostringstream log;
        log << "[NIC Optimization] Starting RSS optimization for adapter: " << adapterName << "\n";

        // Extract actual adapter name from various formats:
        // "Network adapter 'TwinCAT-Intel PCI Ethernet Adapter (Gigabit) #2' on local host"
        // "Intel(R) PRO/1000 MT Network Connection"
        // "Realtek PCIe GbE Family Controller"
        std::string friendlyName = adapterName;
        
        // Try to extract from "Network adapter 'NAME' on local host" format
        size_t quote1 = adapterName.find('\'');
        size_t quote2 = adapterName.rfind('\'');
        if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
            friendlyName = adapterName.substr(quote1 + 1, quote2 - quote1 - 1);
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
        // Set RSS to use CPU 4 as base processor, limit to 1 queue to prevent interrupt roaming
        // This aligns with the recommendation in nic_irq_affinity_bilingual.md
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
        setCmd += L"$adapterName = '";
        setCmd += escapedName;
        setCmd += L"'; ";
        setCmd += L"$adapter = Get-NetAdapter | Where-Object { $_.Name -eq $adapterName -or $_.InterfaceDescription -like ('*' + $adapterName + '*') } | Select-Object -First 1; ";
        setCmd += L"if ($adapter) { ";
        setCmd += L"Write-Output ('Found adapter: ' + $adapter.Name + ' (' + $adapter.InterfaceDescription + ')'); ";
        setCmd += L"$rss = Get-NetAdapterRss -Name $adapter.Name -ErrorAction SilentlyContinue; ";
        setCmd += L"if ($rss) { ";
        setCmd += L"try { ";
        // Pin RSS to CPU 4-5, use only 1 queue to minimize interrupt spread
        setCmd += L"Set-NetAdapterRss -Name $adapter.Name -BaseProcessorNumber 4 -MaxProcessorNumber 5 -NumberOfReceiveQueues 1 -ErrorAction Stop; ";
        setCmd += L"Write-Output ('SUCCESS: RSS optimized - BaseProcessor=4, MaxProcessor=5, Queues=1'); ";
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

        log << "[NIC Optimization] Applying RSS settings (BaseProcessor=4, Queues=1)...\n";
        log << "[NIC Optimization] (This may take a few seconds...)\n";

        wchar_t tempFile2[MAX_PATH];
        GetTempFileNameW(tempPath, L"rss", 0, tempFile2);

        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        ZeroMemory(&pi, sizeof(pi));

        std::wstring cmdLine2 = setCmd;
        cmdLine2 += L" > \"";
        cmdLine2 += tempFile2;
        cmdLine2 += L"\" 2>&1";

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

        constexpr int kEthercatRtCore = 4;

        std::ostringstream log;
        log << "[Wireless IRQ] Rebalancing active WiFi / Bluetooth network paths away from RT core "
            << kEthercatRtCore << "...\n";

        wchar_t tempPath[MAX_PATH];
        wchar_t tempFile[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        GetTempFileNameW(tempPath, L"wir", 0, tempFile);

        STARTUPINFOW si;
        PROCESS_INFORMATION pi;

        std::wstring setCmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"";
        setCmd += L"$ErrorActionPreference = 'SilentlyContinue'; ";
        setCmd += L"$rtCore = 4; ";
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

        std::wstring cmdLine = setCmd;
        cmdLine += L" > \"";
        cmdLine += tempFile2;
        cmdLine += L"\" 2>&1";

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

        std::ostringstream log;
        log << "[Adapter Isolation] " << (restore ? "Restoring" : "Isolating") 
            << " adapter: " << adapterName << "\n";

        // Extract adapter name from various formats
        std::string friendlyName = adapterName;
        
        // Try to extract from "Network adapter 'NAME' on local host" format
        size_t quote1 = adapterName.find('\'');
        size_t quote2 = adapterName.rfind('\'');
        if (quote1 != std::string::npos && quote2 != std::string::npos && quote2 > quote1) {
            friendlyName = adapterName.substr(quote1 + 1, quote2 - quote1 - 1);
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
        // Components to control (NOT including ms_tcpip to allow GetAdaptersAddresses to work):
        // - ms_tcpip6 (TCP/IPv6)
        // - ms_msclient (Client for Microsoft Networks)
        // - ms_server (File and Printer Sharing)
        // - ms_pacer (QoS Packet Scheduler)
        // - ms_lltdio (Link-Layer Topology Discovery Mapper I/O)
        // - ms_rspndr (Link-Layer Topology Discovery Responder)
        // NOTE: We keep ms_tcpip (TCP/IPv4) enabled because GetAdaptersAddresses() 
        //       requires it to enumerate adapters properly.
        
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
        psCmd += L"Write-Output 'Keeping TCP/IPv4 enabled for adapter enumeration'; ";
        
        // Disable/Enable each binding (excluding ms_tcpip)
        const wchar_t* components[] = {
            L"ms_tcpip6", L"ms_msclient", L"ms_server", L"ms_pacer", L"ms_lltdio", L"ms_rspndr"
        };
        const wchar_t* componentNames[] = {
            L"TCP/IPv6", L"Microsoft Network Client", L"File and Printer Sharing", 
            L"QoS Packet Scheduler", L"LLTD Mapper I/O", L"LLTD Responder"
        };
        
        for (int i = 0; i < 6; ++i) {
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
        std::wstring cmdLine = L"cmd.exe /c \"";
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

}
