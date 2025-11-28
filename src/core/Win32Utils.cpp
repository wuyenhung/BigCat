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
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cctype>
#include "LayoutHelper.h"

#include "../include/core/CoreHelpers.h"

// MeasureTextHeight is implemented in src/Win32Measure.cpp to avoid
// pulling heavy networking headers into this translation unit.

std::vector<AdapterEntry> EnumerateAdapters(std::string &errOut) {
    // Thin wrapper delegating to core implementation
    return CoreHelpers::EnumerateAdapters(errOut);
}
// Build ESI Device XML (UTF-8) from device name and ESI object list.
// hexVec contains per-object BE hex display strings (wide strings converted by caller).
std::string BuildESIDeviceXml(const std::wstring &deviceNameW, const std::vector<struct EsiObjectEntry> &objects, const std::vector<std::wstring> &hexVec, const std::vector<std::wstring> &valueVec) {
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

        std::string valueUtf8;
        if (i < valueVec.size() && !valueVec[i].empty()) {
            valueUtf8 = EscapeXml(WideToUtf8(valueVec[i]));
        }

        ss << "  <Object index=\"" << idxbuf << "\" subindex=\"" << subbuf << "\" name=\"" << nameUtf8 << "\"";
        if (!dtypeUtf8.empty()) ss << " dataType=\"" << dtypeUtf8 << "\"";
        if (!accessUtf8.empty()) ss << " access=\"" << accessUtf8 << "\"";
        if (bitlen > 0) ss << " bitlen=\"" << bitlen << "\"";
        if (!hexUtf8.empty()) ss << " hex=\"" << hexUtf8 << "\"";
        if (!valueUtf8.empty()) ss << " value=\"" << valueUtf8 << "\"";
        if (!descUtf8.empty()) {
            ss << " >\n    <Description>" << descUtf8 << "</Description>\n  </Object>\n";
        } else {
            ss << " />\n";
        }
    }

    ss << "</Device>\n";
    return ss.str();
}

// ApplyDistributedColumns implementation uses the DistributeColumns helper
// from Win32WindowCore to compute widths and then applies them to the ListView.
#include "../include/Win32WindowCore.h"
void ApplyDistributedColumns(HWND hList, int colCount) {
    if (!hList || !IsWindow(hList) || colCount <= 0) return;
    RECT rcClient = {};
    GetClientRect(hList, &rcClient);
    int lvClientW = rcClient.right - rcClient.left;
    int vsw = GetSystemMetrics(SM_CXVSCROLL);
    BOOL hasVScroll = (GetWindowLong(hList, GWL_STYLE) & WS_VSCROLL) != 0;
    if (hasVScroll) lvClientW -= vsw;
    if (lvClientW < 100) lvClientW = 100;
    std::vector<int> widths = CoreHelpers::DistributeColumns(lvClientW, colCount);
    for (int i = 0; i < (int)widths.size(); ++i) ListView_SetColumnWidth(hList, i, widths[i]);
}

void ShrinkColumnWidthWithRedistribute(HWND hList, int targetColumn, double factor, int growColumn, int minWidth) {
    if (!hList || !IsWindow(hList)) return;
    if (targetColumn < 0) return;
    if (factor <= 0.0) return;
    if (minWidth < 10) minWidth = 10;

    int currentWidth = ListView_GetColumnWidth(hList, targetColumn);
    if (currentWidth <= 0) return;

    int newWidth = static_cast<int>(currentWidth * factor);
    if (newWidth < minWidth) newWidth = minWidth;
    int freedPixels = currentWidth - newWidth;
    if (freedPixels == 0) return;

    ListView_SetColumnWidth(hList, targetColumn, newWidth);

    if (growColumn >= 0 && growColumn != targetColumn) {
        int growWidth = ListView_GetColumnWidth(hList, growColumn);
        if (growWidth < 0) growWidth = 0;
        ListView_SetColumnWidth(hList, growColumn, growWidth + freedPixels);
    }
}

void ShrinkColumnsWithRedistribute(HWND hList, const std::vector<int> &columns, double factor, int growColumn, int minWidth) {
    if (!hList || !IsWindow(hList)) return;
    for (int col : columns) {
        ShrinkColumnWidthWithRedistribute(hList, col, factor, growColumn, minWidth);
    }
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

    // Build continuous hex display (big-endian numeric) from LE buffer.
    // Show as single 0xHEX string, zero-padded to the number of significant
    // nibbles derived from object bitlen (if available) or by byte width.
    int useBytes = expectedBytes;
    if (useBytes > 8) useBytes = 8; // cap to 64 bits for display
    unsigned long long u = 0;
    for (int bi = 0; bi < useBytes; ++bi) u |= (unsigned long long)buf[bi] << (8 * bi);
    // Produce minimal continuous hex (no zero-padding). Example: 0xA3, 0x0 -> 0x0
    std::wostringstream shex;
    shex << std::hex << std::uppercase;
    if (u == 0) {
        shex << L"0x0";
    } else {
        shex << L"0x" << u;
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

// --- Migration wrappers and small helpers (delegate to CoreHelpers) ---
bool CheckLinkStatus(const char* ifname) {
    return CoreHelpers::CheckLinkStatus(ifname);
}

bool IsSystemDarkMode() {
    return CoreHelpers::IsSystemDarkMode();
}

std::string WideToUtf8(const std::wstring &w) {
    return CoreHelpers::WideToUtf8(w);
}

std::wstring Utf8ToWide(const std::string &s) {
    return CoreHelpers::Utf8ToWide(s);
}

std::string EscapeXml(const std::string &s) {
    return CoreHelpers::EscapeXml(s);
}

std::wstring BytesToHexDisplay(const uint8_t* data, size_t len, bool bigEndian, bool prefix0x, bool spaceSeparated, bool uppercase) {
    return CoreHelpers::BytesToHexDisplay(data, len, bigEndian, prefix0x, spaceSeparated, uppercase);
}

void ParseTokensToBytes(const std::wstring &tokenStr, std::vector<uint8_t> &outBytes, bool &anyHexToken) {
    CoreHelpers::ParseTokensToBytes(tokenStr, outBytes, anyHexToken);
}

std::string MakeAdapterDisplayString(const AdapterEntry &d) {
    return CoreHelpers::MakeAdapterDisplayString(d);
}

std::string EnumerateAdaptersJson() {
    return CoreHelpers::EnumerateAdaptersJson();
}
