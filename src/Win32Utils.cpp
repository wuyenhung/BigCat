#include "Win32Utils.h"

// Implementation notes:
// This file implements utility helpers used by UI and non-UI modules.
// Keep low-level encoding APIs (WideCharToMultiByte / MultiByteToWideChar)
// inside this module so callers can use Utf8ToWide / WideToUtf8 consistently.
#include <string>
// Win32Utils.cpp
// Implementation of small, pure helper functions used by the UI and other
// non-UI parts of the program. This file centralizes encoding conversions
// (UTF-8 <-> UTF-16), XML escaping, adapter enumeration helpers and ESI
// serialization helpers. It should not perform UI operations like dialog
// boxes or direct control manipulation.
#include "EsiParser.h"
#include <cwctype>
#include <sstream>
#include <iomanip>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <pcap.h>

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
                    if (sockaddr_equal(ps, as)) { matched = true; break; }
                }
                if (matched) matchedAA = ad.aa;
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
                if (!al.empty() && pl.find(al) != std::string::npos) { matched = true; break; }
                if (ad.aa->FriendlyName) {
                    std::string fname = WideToUtf8(std::wstring(ad.aa->FriendlyName));
                    if (!fname.empty()) {
                        std::string fl = toLower(fname);
                        std::string desc = d->description ? std::string(d->description) : std::string();
                        std::string dl = toLower(desc);
                        if (dl.find(fl) != std::string::npos) { matched = true; break; }
                    }
                }
                if (matched) matchedAA = ad.aa;
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
            // Determine link status via CheckLinkStatus using pcap interface name
            if (!e.pcapName.empty()) {
                e.linkUp = CheckLinkStatus(e.pcapName.c_str());
            } else {
                e.linkUp = true; // conservative fallback
            }
            result.push_back(e);
        }
    }

    if (alldevs) pcap_freealldevs(alldevs);
    return result;
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

std::string MakeAdapterDisplayString(const AdapterEntry &d) {
    if (!d.description.empty()) return d.description + " (" + d.pcapName + ")";
    return d.pcapName;
}

void ParseTokensToBytes(const std::wstring &tokenStr, std::vector<uint8_t> &outBytes, bool &anyHexToken)
{
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

bool IsSystemDarkMode() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwValue = 1;
        DWORD dwSize = sizeof(DWORD);
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&dwValue, &dwSize);
        RegCloseKey(hKey);
        return dwValue == 0;
    }
    return false;
}

bool CheckLinkStatus(const char* ifname) {
    ULONG flags = GAA_FLAG_INCLUDE_PREFIX;
    ULONG family = AF_UNSPEC;
    ULONG outBufLen = 0;
    DWORD rc = GetAdaptersAddresses(family, flags, NULL, NULL, &outBufLen);
    if (rc != ERROR_BUFFER_OVERFLOW) {
        return true; // conservative: assume up if we can't query
    }

    std::vector<BYTE> buffer(outBufLen);
    IP_ADAPTER_ADDRESSES* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    rc = GetAdaptersAddresses(family, flags, NULL, adapters, &outBufLen);
    if (rc != NO_ERROR) {
        return true;
    }

    for (IP_ADAPTER_ADDRESSES* aa = adapters; aa; aa = aa->Next) {
        bool matches = false;
        if (aa->AdapterName) {
            std::string adapterName(aa->AdapterName);
            std::string pcapName(ifname);
            if (!adapterName.empty() && adapterName.front() == '{' && adapterName.back() == '}') {
                adapterName = adapterName.substr(1, adapterName.size() - 2);
            }
            std::transform(adapterName.begin(), adapterName.end(), adapterName.begin(), ::tolower);
            std::transform(pcapName.begin(), pcapName.end(), pcapName.begin(), ::tolower);
            if (!adapterName.empty() && pcapName.find(adapterName) != std::string::npos) {
                matches = true;
            }
        }
        if (matches) {
            return (aa->OperStatus == IfOperStatusUp);
        }
    }

    return true;
}

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

// Build ESI Device XML (UTF-8) from device name and ESI object list.
// hexVec contains per-object BE hex display strings (wide strings converted by caller).
std::string BuildESIDeviceXml(const std::wstring &deviceNameW, const std::vector<struct EsiObjectEntry> &objects, const std::vector<std::wstring> &hexVec) {
    std::ostringstream ss;
    ss << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    std::string devUtf8 = EscapeXml(WideToUtf8(deviceNameW));
    ss << "<Device name=\"" << devUtf8 << "\">\n";

    for (size_t i = 0; i < objects.size(); ++i) {
        const auto &obj = objects[i];
        char idxbuf[32]; snprintf(idxbuf, sizeof(idxbuf), "0x%04X", obj.index);
        char subbuf[16]; snprintf(subbuf, sizeof(subbuf), "0x%02X", obj.subindex);
        std::string nameUtf8 = EscapeXml(obj.name);
        std::string descUtf8 = EscapeXml(obj.description);
        std::string dtypeUtf8 = EscapeXml(obj.dataType);
        std::string accessUtf8 = EscapeXml(obj.access);
        int bitlen = obj.bitlen;

        std::string hexUtf8;
        if (i < hexVec.size() && !hexVec[i].empty()) {
            hexUtf8 = EscapeXml(WideToUtf8(hexVec[i]));
        }

        ss << "  <Object index=\"" << idxbuf << "\" subindex=\"" << subbuf << "\" name=\"" << nameUtf8 << "\"";
        if (!dtypeUtf8.empty()) ss << " dataType=\"" << dtypeUtf8 << "\"";
        if (!accessUtf8.empty()) ss << " access=\"" << accessUtf8 << "\"";
        if (bitlen > 0) ss << " bitlen=\"" << bitlen << "\"";
        if (!hexUtf8.empty()) ss << " hex=\"" << hexUtf8 << "\"";
        if (!descUtf8.empty()) {
            ss << " >\n    <Description>" << descUtf8 << "</Description>\n  </Object>\n";
        } else {
            ss << " />\n";
        }
    }

    ss << "</Device>\n";
    return ss.str();
}

// Format SDO value for UI: returns pair<valueDisplay, hexDisplay>
std::pair<std::wstring, std::wstring> FormatSDOValue(const uint8_t* buf, int size, const struct EsiObjectEntry* objPtr) {
    if (!buf || size <= 0) return { std::wstring(), std::wstring() };

    // Determine expected bytes
    int expectedBytes = size;
    bool hasObj = (objPtr != nullptr);
    if (hasObj && objPtr->bitlen > 0) expectedBytes = (objPtr->bitlen + 7) / 8;
    if (!hasObj) expectedBytes = (expectedBytes > 4 ? expectedBytes : 4);
    if (expectedBytes < 1) expectedBytes = 1;

    // Build BE hex display from LE buffer
    std::vector<uint8_t> lebuf(expectedBytes, 0);
    for (int i = 0; i < size && i < expectedBytes; ++i) lebuf[i] = buf[i];
    std::wostringstream shex; shex << std::hex << std::uppercase;
    for (int i = expectedBytes - 1; i >= 0; --i) {
        shex << L"0x" << std::setw(2) << std::setfill(L'0') << (int)lebuf[i];
        if (i > 0) shex << L" ";
    }
    std::wstring hexDisplay = shex.str();

    // Format value column depending on dataType
    std::wstring valueDisplay;
    if (hasObj) {
        std::string dtype = objPtr->dataType;
        std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        // BOOL/BOOLEAN
        if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
            unsigned long long v = 0; int maxb = (size < 8 ? size : 8);
            for (int bi = 0; bi < maxb; ++bi) v |= (unsigned long long)buf[bi] << (8 * bi);
            valueDisplay = (v ? std::wstring(L"1") : std::wstring(L"0"));
        }
        // FLOAT / REAL (IEEE754)
        else if (du.find("REAL") != std::string::npos || du.find("FLOAT") != std::string::npos) {
            if (objPtr->bitlen == 32 && size >= 4) {
                uint32_t raw = 0;
                for (int b = 0; b < 4; ++b) raw |= (uint32_t)buf[b] << (8*b);
                float f; memcpy(&f, &raw, sizeof(f));
                std::wostringstream ssFloat; ssFloat << std::fixed << std::setprecision(6) << f;
                valueDisplay = ssFloat.str();
            } else if (objPtr->bitlen == 64 && size >= 8) {
                uint64_t raw = 0;
                for (int b = 0; b < 8; ++b) raw |= (uint64_t)buf[b] << (8*b);
                double d; memcpy(&d, &raw, sizeof(d));
                std::wostringstream ssDouble; ssDouble << std::fixed << std::setprecision(9) << d;
                valueDisplay = ssDouble.str();
            } else {
                unsigned long long v = 0; int maxb = (size < 8 ? size : 8);
                for (int bi = 0; bi < maxb; ++bi) v |= (unsigned long long)buf[bi] << (8 * bi);
                valueDisplay = std::to_wstring(v);
            }
        }
        // Signed integer
        else if (du.find("INT") != std::string::npos || du.find("SINT") != std::string::npos || du.find("SIGNED") != std::string::npos) {
            int bits = (objPtr->bitlen > 0 ? objPtr->bitlen : (size * 8));
            if (bits <= 64) {
                unsigned long long u = 0; int maxb = (size < 8 ? size : 8);
                for (int bi = 0; bi < maxb; ++bi) u |= (unsigned long long)buf[bi] << (8 * bi);
                long long signedVal = 0;
                if (bits < 64) {
                    unsigned long long mask = 1ULL << (bits - 1);
                    if (u & mask) {
                        unsigned long long signmask = (~0ULL) << bits;
                        signedVal = (long long)(u | signmask);
                    } else signedVal = (long long)u;
                } else {
                    signedVal = (long long)u;
                }
                valueDisplay = std::to_wstring(signedVal);
            } else {
                valueDisplay = std::to_wstring(0ULL);
            }
        }
        // Unsigned integer or default
        else {
            unsigned long long u = 0; int maxb = (size < 8 ? size : 8);
            for (int bi = 0; bi < maxb; ++bi) u |= (unsigned long long)buf[bi] << (8 * bi);
            valueDisplay = std::to_wstring(u);
        }
    } else {
        unsigned long long u = 0; int maxb = (size < 8 ? size : 8);
        for (int bi = 0; bi < maxb; ++bi) u |= (unsigned long long)buf[bi] << (8 * bi);
        valueDisplay = std::to_wstring(u);
    }

    return { valueDisplay, hexDisplay };
}
