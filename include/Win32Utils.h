// Simple utilities shared by Win32 UI and non-UI code
//
// This header exposes a small collection of helpers used by the UI layer
// and other non-UI components. Keep only pure, side-effect-free logic
// here (encoding conversions, string escaping, adapter enumeration helpers,
// and ESI serialization). UI-specific tasks such as file dialogs, message
// boxes and control manipulation should remain in `MainWindow.cpp`.
#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <utility>
// Win32 types used by some utility functions
#include <windows.h>

// Win32Utils: utility helpers for the UI and non-UI code
// - Centralizes UTF-8 <-> UTF-16 conversion helpers
// - Provides pure-logic helpers (parsing, XML building, adapter display formatting)
// - UI code should use these helpers rather than calling Win32 encoding APIs directly

// Convert a byte buffer to a human-readable hex display string.
// Parameters allow controlling endianness, prefixing, spacing and letter-case.
std::wstring BytesToHexDisplay(const uint8_t* data, size_t len, bool bigEndian = true, bool prefix0x = true, bool spaceSeparated = true, bool uppercase = true);

// Parse a token string (space/comma/semicolon/colon/pipe separated) into bytes.
// Accepts tokens like "0x12", "12" (decimal), "FF" (hex without 0x) and sets anyHexToken
// when at least one token appears hex-like. Results are pushed as raw bytes (lowest 8 bits).
void ParseTokensToBytes(const std::wstring &tokenStr, std::vector<uint8_t> &outBytes, bool &anyHexToken);

// Query whether the system preference indicates dark theme for apps.
bool IsSystemDarkMode();

// Check whether a pcap interface name corresponds to an adapter that reports link up.
// Expects `ifname` to be a UTF-8 encoded pcap interface name (e.g. "\\Device\\..." or GUID form).
bool CheckLinkStatus(const char* ifname);

// Convert wide (UTF-16) string to UTF-8 encoded std::string
std::string WideToUtf8(const std::wstring &w);

// Convert UTF-8 encoded std::string to wide (UTF-16) std::wstring
std::wstring Utf8ToWide(const std::string &s);

// Escape basic XML characters in a UTF-8 encoded string
std::string EscapeXml(const std::string &s);

// Adapter enumeration helper
struct AdapterEntry {
	std::string pcapName;      // pcap device name (UTF-8)
	std::string description;   // description (UTF-8)
	std::string friendlyName;  // adapter friendly name (UTF-8, if known)
    std::string adapterName;   // AdapterName (GUID or device id string), useful to disambiguate friendly names
	bool linkUp;               // whether adapter reports link up
};

// Enumerate pcap-visible adapters filtered to likely physical Ethernet adapters.
// Returns a vector of AdapterEntry. On error, returns empty vector and optionally
// fills errOut with a UTF-8 error message.
std::vector<AdapterEntry> EnumerateAdapters(std::string &errOut);

// Return a JSON array (UTF-8) representing available adapters. Useful for HTTP server
// to avoid exposing internal AdapterEntry structure to other translation units.
std::string EnumerateAdaptersJson();

// Create a human-readable display string for an adapter entry. Typically
// returns "<description> (<pcapName>)" when description exists, otherwise
// just the pcapName.
std::string MakeAdapterDisplayString(const AdapterEntry &d);

// Build an ESI-style Device XML (UTF-8) from device name, object list and
// optional per-object hex values (wide strings already converted from source).
// Returns a UTF-8 encoded std::string containing the full XML document.
std::string BuildESIDeviceXml(const std::wstring &deviceNameW, const std::vector<struct EsiObjectEntry> &objects, const std::vector<std::wstring> &hexVec, const std::vector<std::wstring> &valueVec);

// Forward declaration for ESI object entry used by format helpers
struct EsiObjectEntry;

// Format an SDO value and its hex display for UI presentation.
// - buf/size: raw bytes returned by ReadSDO (LE byte order)
// - objPtr: optional pointer to an ESI object entry describing dataType/bitlen (may be nullptr)
// Returns: pair<valueDisplay (wstring), hexDisplay (wstring)>; hexDisplay is big-endian styled by convention.
std::pair<std::wstring, std::wstring> FormatSDOValue(const uint8_t* buf, int size, const struct EsiObjectEntry* objPtr);

// Measure the vertical height (pixels) of a single text row for the provided HFONT.
// Returns a sensible default (LayoutConfig::rowH) when measurement is not possible.
int MeasureTextHeight(HFONT hFont);

// Safely enable/disable a control. Checks for null HWND and IsWindow before calling EnableWindow.
inline void SetControlEnabled(HWND hCtrl, bool enable) {
	if (!hCtrl) return;
	if (!IsWindow(hCtrl)) return;
	const bool currentlyEnabled = (::IsWindowEnabled(hCtrl) != FALSE);
	if (currentlyEnabled == enable) return;
	::EnableWindow(hCtrl, enable ? TRUE : FALSE);
}

// Compute and apply evenly distributed column widths for a ListView control.
// Accounts for vertical scrollbar width and uses Win32WindowCore::DistributeColumns.
void ApplyDistributedColumns(HWND hList, int colCount);

// Reduce a column width by a multiplicative factor and give freed pixels to another column.
// Useful when certain identifier columns should remain narrow while others grow.
void ShrinkColumnWidthWithRedistribute(HWND hList, int targetColumn, double factor, int growColumn, int minWidth = 30);

// Convenience overload: shrink multiple columns sequentially, redistributing to the same grow column.
void ShrinkColumnsWithRedistribute(HWND hList, const std::vector<int> &columns, double factor, int growColumn, int minWidth = 30);

// Convenience helper: set font for a control if valid. redraw==TRUE will force a redraw.
inline void SetControlFont(HWND hCtrl, HFONT hFont, BOOL redraw = TRUE) {
	if (!hCtrl) return;
	if (!IsWindow(hCtrl)) return;
	SendMessageW(hCtrl, WM_SETFONT, (WPARAM)hFont, MAKELPARAM((int)redraw, 0));
}

// Inline helpers to simplify creating child controls and automatically applying font.
inline HWND CreateChildControl(HWND parent, HINSTANCE hInst, LPCWSTR className, LPCWSTR text, DWORD style,
	int x, int y, int width, int height, int id, HFONT hFont) {
	HMENU hMenu = (id != 0) ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : NULL;
	HWND hwnd = CreateWindowW(className, text, style, x, y, width, height, parent, hMenu, hInst, NULL);
	if (hwnd && hFont) SendMessageW(hwnd, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
	return hwnd;
}

inline HWND CreateChildControlEx(DWORD exStyle, HWND parent, HINSTANCE hInst, LPCWSTR className, LPCWSTR text, DWORD style,
	int x, int y, int width, int height, int id, HFONT hFont) {
	HMENU hMenu = (id != 0) ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : NULL;
	HWND hwnd = CreateWindowExW(exStyle, className, text, style, x, y, width, height, parent, hMenu, hInst, NULL);
	if (hwnd && hFont) SendMessageW(hwnd, WM_SETFONT, (WPARAM)hFont, MAKELPARAM(TRUE, 0));
	return hwnd;
}
