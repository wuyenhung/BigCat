#include "MainWindow.h"
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <sstream>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <algorithm>
#include <utility>
#include <unordered_map>
#include <array>
#include <map>
#include <future>
#include "Win32Utils.h"
#include "HttpServer.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../include/EtherCATMaster.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
#include "../include/MotionLogic.h"
#include "../include/AutoWindow.h"
// Local INI helpers used by adapter selection/load.
static std::wstring GetAppIniPath()
{
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, buf, MAX_PATH) == 0) return std::wstring(L".\\WuBigCat.ini");
    wchar_t *p = wcsrchr(buf, L'\\');
    wchar_t *dot = wcsrchr(buf, L'.');
    if (dot && (!p || dot > p)) *dot = L'\0';
    std::wstring ini = std::wstring(buf) + L".ini";
    return ini;
}

static std::wstring ReadIniString(const wchar_t* section, const wchar_t* key, const wchar_t* defaultValue)
{
    std::wstring ini = GetAppIniPath();
    wchar_t buf[1024] = {0};
    GetPrivateProfileStringW(section, key, defaultValue, buf, 1024, ini.c_str());
    return std::wstring(buf);
}

static void WriteIniString(const wchar_t* section, const wchar_t* key, const std::wstring& value)
{
    std::wstring ini = GetAppIniPath();
    WritePrivateProfileStringW(section, key, value.c_str(), ini.c_str());
}
void MainWindow::OnAppGetStatus()
{
    // Provide status buffer to HTTP thread: collect UI buffer and publish to server cache
    std::wstring buf = statusLogger.GetBufferChronological();
    std::string utf8 = CoreHelpers::WideToUtf8(buf);
    SetLastStatusUtf8(utf8);
}

// Enumerate adapters and publish JSON/status; also set a preferred default selection

void MainWindow::OnEnumerateAdapters()
{
    std::string err;
    std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(err);
    if (!err.empty()) {
        std::wstring werr = CoreHelpers::Utf8ToWide(err);
        if (!werr.empty()) { PrependStatus(werr.c_str()); PrependStatus(L"\r\n"); }
    }
    // Publish JSON for front-end consumers
    std::string json = CoreHelpers::BuildAdaptersJson(adapters);
    if (!json.empty()) {
        if (json != lastAdapterJson) {
            PostUTF8LogToUI(std::string("ADAPTERS_JSON:") + json + "\n");
            lastAdapterJson = json;
        }
    }
    // Re-evaluate web-selected adapter (if any) to ensure server only enables
    // the web Connect button when the chosen adapter is valid (link up) and
    // PC is not already busy/connected.
        // Also monitor web-selected adapter (if any) and update server UI state
        // so web clients only see the Connect button enabled when the chosen
        // adapter is link-up and the PC is not busy/connected.
        try {
            std::string webIf = this->GetWebSelectedAdapterUtf8();
            static bool lastWebDisabled = true;
            if (!webIf.empty()) {
                bool linkUp = false;
                bool found = false;
                const bool simulationSelected = EtherCATMaster::IsSimulationAdapterSelection(webIf);
                if (simulationSelected) {
                    linkUp = true;
                    found = true;
                }
                for (const auto &adapter : adapters) {
                    if (adapter.pcapName == webIf) {
                        linkUp = adapter.linkUp;
                        found = true;
                        break;
                    }
                }
                if (!found) linkUp = CoreHelpers::CheckLinkStatus(webIf.c_str());
                bool disabled = true;
                if (linkUp && !this->IsWorkerRunning() && !this->IsMasterInitialized()) disabled = false;
                if (disabled != lastWebDisabled) {
                    lastWebDisabled = disabled;
                    SetUiDisabled("connect", disabled);
                    try {
                        try {
                            std::string ast = std::string(simulationSelected ? "simulation:" : (linkUp ? "link_up:" : "no_link:")) + webIf;
                            SetAdapterStatus(ast);
                        } catch(...) {}
                    } catch(...) {}
                }
            } else {
                if (!lastWebDisabled) {
                    lastWebDisabled = true;
                    SetUiDisabled("connect", true);
                }
            }
        } catch(...) {}
    try {
        std::string webIf = this->GetWebSelectedAdapterUtf8();
        if (!webIf.empty()) {
            bool linkUp = false;
            bool found = false;
            const bool simulationSelected = EtherCATMaster::IsSimulationAdapterSelection(webIf);
            if (simulationSelected) {
                linkUp = true;
                found = true;
            }
            for (const auto &adapter : adapters) {
                if (adapter.pcapName == webIf) {
                    linkUp = adapter.linkUp;
                    found = true;
                    break;
                }
            }
            if (!found) linkUp = CoreHelpers::CheckLinkStatus(webIf.c_str());
            bool disable = true;
            if (linkUp && !this->IsWorkerRunning() && !this->IsMasterInitialized()) disable = false;
            SetUiDisabled("connect", disable);
            // Also post a short adapter status so web UI can display it
                    try {
                        std::string ast = std::string(simulationSelected ? "simulation:" : (linkUp ? "link_up:" : "no_link:")) + webIf;
                        SetAdapterStatus(ast);
                    } catch(...) {}
        }
    } catch(...) {}
    // Do not auto-select or switch adapters here; selection must be explicit
    // (user choice in UI or /set_adapter). Auto-picking on enumerate caused
    // surprise re-selection (e.g., defaulting to Twincat) and could interrupt
    // an active session.
}

// -----------------------------------------------------------------------------
// UTF-8 log posting helper (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------

void MainWindow::PopulateAdapters() {
    // Publish JSON for front-end consumers (avoid calling OnEnumerateAdapters here
    // to prevent duplicate ADAPTERS_JSON messages when PopulateAdapters is called
    // from window initialization and elsewhere).
    std::string err;
    std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(err);
    adapterComboPcapNames.clear();
    std::string json = CoreHelpers::BuildAdaptersJson(adapters);
    if (!json.empty()) {
        if (json != lastAdapterJson) {
            PostUTF8LogToUI(std::string("ADAPTERS_JSON:") + json + "\n");
            lastAdapterJson = json;
        }
    }
    // Then, optionally fill native combobox for desktop UI users
    if (hComboAdapter && IsWindow(hComboAdapter)) {
        SendMessageW(hComboAdapter, CB_RESETCONTENT, 0, 0);
        size_t preferredIndex = 0;
        bool foundPreferred = false;
        std::wstring savedAdapter = ReadIniString(L"Network", L"AutoSelectedAdapter", L"");
        std::string savedAdapterUtf8 = CoreHelpers::WideToUtf8(savedAdapter);
        std::string preferredAdapterUtf8 = !this->currentIfName.empty()
            ? this->currentIfName
            : (!this->webSelectedIfName.empty() ? this->webSelectedIfName : savedAdapterUtf8);
        if (!preferredAdapterUtf8.empty()) {
            TryResolveAdapterPcapName(preferredAdapterUtf8);
        }
        const bool preferredSimulation = EtherCATMaster::IsSimulationAdapterSelection(preferredAdapterUtf8);
        if (!preferredAdapterUtf8.empty() && !preferredSimulation) {
            for (size_t i = 0; i < adapters.size(); ++i) {
                if (adapters[i].pcapName == preferredAdapterUtf8 || adapters[i].adapterName == preferredAdapterUtf8 || CoreHelpers::MakeAdapterDisplayString(adapters[i]) == preferredAdapterUtf8) {
                    preferredIndex = i;
                    foundPreferred = true;
                    break;
                }
            }
        }
        if (!foundPreferred && !preferredSimulation) {
            for (size_t i = 0; i < adapters.size(); ++i) { if (adapters[i].linkUp) { preferredIndex = i; break; } }
        }
        int lastAddedIndex = -1;
        // Measure text width so we can expand dropdown to show full content
        HDC hdc = GetDC(hComboAdapter);
        HFONT hComboFont = (HFONT)SendMessageW(hComboAdapter, WM_GETFONT, 0, 0);
        HFONT hOldFont = NULL;
        if (hdc && hComboFont) hOldFont = (HFONT)SelectObject(hdc, hComboFont);
        int maxTextWidth = 0;
        for (size_t i = 0; i < adapters.size(); ++i) {
            const AdapterEntry &ae = adapters[i];
            std::string visible = CoreHelpers::MakeAdapterDisplayString(ae);
            if (visible.empty()) visible = std::string("Adapter ") + std::to_string((int)i+1);
            if (!ae.adapterName.empty()) {
                // Append a short adapter id to disambiguate identical friendly names
                std::string shortId = ae.adapterName.size() > 8 ? ae.adapterName.substr(ae.adapterName.size() - 8) : ae.adapterName;
                visible += " [" + shortId + "]";
            }
            std::wstring wvis = CoreHelpers::Utf8ToWide(visible);
            int added = (int)SendMessageW(hComboAdapter, CB_ADDSTRING, 0, (LPARAM)wvis.c_str());
            if (added == CB_ERR || added == CB_ERRSPACE) {
                continue;
            }
            const size_t comboDataIndex = adapterComboPcapNames.size();
            adapterComboPcapNames.push_back(ae.pcapName);
            SendMessageW(hComboAdapter, CB_SETITEMDATA, (WPARAM)added, (LPARAM)comboDataIndex);
            // measure this item's width
            if (hdc && !wvis.empty()) {
                SIZE sz = {0,0};
                if (GetTextExtentPoint32W(hdc, wvis.c_str(), (int)wvis.size(), &sz)) {
                    if (sz.cx > maxTextWidth) maxTextWidth = sz.cx;
                }
            }
            lastAddedIndex = added;
            if (!preferredSimulation && i == preferredIndex) {
                SendMessageW(hComboAdapter, CB_SETCURSEL, (WPARAM)added, 0);
                if (foundPreferred && !adapters[preferredIndex].pcapName.empty()) {
                    SetSelectedAdapter(adapters[preferredIndex].pcapName);
                }
                // Do not auto-set adapter; wait for explicit user/web selection
            }
        }
        {
            const std::wstring wSimulation = CoreHelpers::Utf8ToWide(EtherCATMaster::GetSimulationAdapterDisplayName());
            int added = (int)SendMessageW(hComboAdapter, CB_ADDSTRING, 0, (LPARAM)wSimulation.c_str());
            if (added != CB_ERR && added != CB_ERRSPACE) {
                const size_t comboDataIndex = adapterComboPcapNames.size();
                adapterComboPcapNames.push_back(EtherCATMaster::GetSimulationInterfaceName());
                SendMessageW(hComboAdapter, CB_SETITEMDATA, (WPARAM)added, (LPARAM)comboDataIndex);
                lastAddedIndex = added;
                if (preferredSimulation) {
                    SendMessageW(hComboAdapter, CB_SETCURSEL, (WPARAM)added, 0);
                    SetSelectedAdapter(EtherCATMaster::GetSimulationInterfaceName());
                }
                if (hdc && !wSimulation.empty()) {
                    SIZE sz = {0,0};
                    if (GetTextExtentPoint32W(hdc, wSimulation.c_str(), (int)wSimulation.size(), &sz)) {
                        if (sz.cx > maxTextWidth) maxTextWidth = sz.cx;
                    }
                }
            }
        }
        // restore font selection / release HDC
        if (hdc) {
            if (hOldFont) SelectObject(hdc, hOldFont);
            ReleaseDC(hComboAdapter, hdc);
        }
        // If any item is wider than the control, expand the drop-down width and set horizontal extent
        if (maxTextWidth > 0) {
            const int padding = 24; // a little breathing room for icons/checkboxes
            RECT rc; GetWindowRect(hComboAdapter, &rc);
            int ctrlW = rc.right - rc.left;
            int desiredDropW = maxTextWidth + padding;
            int dropW = desiredDropW > ctrlW ? desiredDropW : ctrlW;
            SendMessageW(hComboAdapter, CB_SETDROPPEDWIDTH, (WPARAM)dropW, 0);
            SendMessageW(hComboAdapter, CB_SETHORIZONTALEXTENT, (WPARAM)(maxTextWidth + padding), 0);
        }
        if (lastAddedIndex >= 0) {
            LRESULT cur = SendMessageW(hComboAdapter, CB_GETCURSEL, 0, 0);
            if (cur == CB_ERR) {
                SendMessageW(hComboAdapter, CB_SETCURSEL, (WPARAM)0, 0);
                // No implicit selection; keep state untouched until user picks
            }
        }
    }
    // If we have an explicitly stored PC selection, update connect button state accordingly
    try {
        std::string cur = this->GetSelectedAdapterUtf8();
        if (!cur.empty()) UpdateConnectButtonStateForAdapter(cur);
    } catch(...) {}
}

bool MainWindow::TryGetAdapterPcapNameFromComboSelection(int selectionIndex, std::string &ifname) const
{
    ifname.clear();
    if (selectionIndex == CB_ERR || !hComboAdapter || !IsWindow(hComboAdapter)) {
        return false;
    }

    LRESULT data = SendMessageW(hComboAdapter, CB_GETITEMDATA, (WPARAM)selectionIndex, 0);
    if (data == CB_ERR || data < 0) {
        return false;
    }

    const size_t comboDataIndex = static_cast<size_t>(data);
    if (comboDataIndex >= adapterComboPcapNames.size()) {
        return false;
    }

    ifname = adapterComboPcapNames[comboDataIndex];
    return !ifname.empty();
}

void MainWindow::SetSelectedAdapter(const std::string &ifname, bool isWeb)
{
    std::string canonicalIfname = ifname;
    if (!canonicalIfname.empty()) {
        try {
            TryResolveAdapterPcapName(canonicalIfname);
        } catch (...) {
        }
        WriteIniString(L"Network", L"AutoSelectedAdapter", CoreHelpers::Utf8ToWide(canonicalIfname));
    }

    // If called from web clients, keep the web-specific log message, but do not
    // early-return. Web and desktop adapter selections must stay synchronized.
    if (isWeb) {
        // Only update and log when the selection actually changes to avoid
        // noisy duplicate logs when the frontend reposts the same value
        // (for example on page reloads).
        if (this->webSelectedIfName != canonicalIfname) {
            this->webSelectedIfName = canonicalIfname;
            if (!canonicalIfname.empty()) {
                std::wstring w = CoreHelpers::Utf8ToWide(std::string("Selected adapter (web): ") + canonicalIfname + "\n");
                PrependStatus(w.c_str());
                // Also post a concise UTF-8 marker so HTTP server / logs can pick it up
                PostUTF8LogToUI(std::string("HTTP_LOCAL:SELECTED_ADAPTER:") + canonicalIfname + "\n");
            }
        }
    }

    // Keep desktop selection as the canonical adapter choice for web flows too,
    // so web clients reflect native combobox changes instead of holding a stale item.
    this->webSelectedIfName = canonicalIfname;

    // store atomically (string assignment is safe here as caller is UI/http thread)
    this->currentIfName = canonicalIfname;
    // mirror to status for visibility
    if (!isWeb && !canonicalIfname.empty()) {
        std::wstring w = CoreHelpers::Utf8ToWide(std::string("Selected adapter: ") + canonicalIfname + "\n");
        PrependStatus(w.c_str());
    }
    // Update native read-only edit control if present
    if (hComboAdapter && IsWindow(hComboAdapter)) {
        // Try to find a matching combobox entry corresponding to this pcapName.
        std::string err;
        std::vector<AdapterEntry> adapters = CoreHelpers::EnumerateAdapters(err);
        std::string desiredDisplay;
        if (EtherCATMaster::IsSimulationAdapterSelection(canonicalIfname)) {
            desiredDisplay = EtherCATMaster::GetSimulationAdapterDisplayName();
        }
        for (size_t i = 0; i < adapters.size(); ++i) {
            if (adapters[i].pcapName == canonicalIfname) {
                desiredDisplay = CoreHelpers::MakeAdapterDisplayString(adapters[i]);
                if (!adapters[i].adapterName.empty()) {
                    std::string shortId = adapters[i].adapterName.size() > 8 ? adapters[i].adapterName.substr(adapters[i].adapterName.size() - 8) : adapters[i].adapterName;
                    desiredDisplay += " [" + shortId + "]";
                }
                break;
            }
        }
        bool applied = false;
        if (!desiredDisplay.empty()) {
            // Iterate combobox items and match by visible text
            LRESULT count = SendMessageW(hComboAdapter, CB_GETCOUNT, 0, 0);
            for (int i = 0; i < (int)count; ++i) {
                wchar_t buf[1024];
                LRESULT len = SendMessageW(hComboAdapter, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf);
                if (len > 0) {
                    std::string itemText = CoreHelpers::WideToUtf8(std::wstring(buf, buf + len));
                    if (itemText == desiredDisplay) {
                        SendMessageW(hComboAdapter, CB_SETCURSEL, (WPARAM)i, 0);
                        applied = true;
                        break;
                    }
                }
            }
        }
        if (!applied) {
            // Fallback: set control text to the raw ifname so user can see canonical value
            std::wstring w = CoreHelpers::Utf8ToWide(canonicalIfname);
            SetWindowTextW(hComboAdapter, w.c_str());
        }
    }
    // After applying PC selection, update Connect button state based on link
    try {
        UpdateConnectButtonStateForAdapter(canonicalIfname);
    } catch(...) {}
}


std::string MainWindow::GetWebSelectedAdapterUtf8() const
{
    return this->webSelectedIfName;
}


void MainWindow::UpdateConnectButtonStateForAdapter(const std::string &ifname)
{
    bool enabled = false;
    const bool simulationSelected = EtherCATMaster::IsSimulationAdapterSelection(ifname);
    if (!ifname.empty()) {
        bool linkUp = false;
        try {
            linkUp = simulationSelected ? true : CoreHelpers::CheckLinkStatus(ifname.c_str());
        } catch(...) { linkUp = false; }
        if (linkUp) {
            // Ensure global UI/work state allows new connect attempts
            const auto snapshot = GetSystemStateSnapshot();
            if (!snapshot.busy && !IsMasterInitialized()) enabled = true;
        }
    }
    // Apply to PC UI controls
    if (hBtnConnect && IsWindow(hBtnConnect)) SetControlEnabled(hBtnConnect, enabled);
    // Mirror to server UI state so web clients see consistent enabled/disabled
    SetUiDisabled("connect", !enabled);
    // Also set adapter status for SSE consumers
    try {
        std::string ast = std::string(simulationSelected ? "simulation:" : (enabled ? "link_up:" : "no_link:")) + ifname;
        SetAdapterStatus(ast);
    } catch(...) {}
}


std::string MainWindow::GetSelectedAdapterUtf8() const
{
    // If we have an explicitly stored canonical interface name, return it
    if (!this->currentIfName.empty()) return this->currentIfName;

    if (hComboAdapter && IsWindow(hComboAdapter)) {
        // Resolve the current combo selection against the cached combo-to-pcap mapping.
        int sel = (int)SendMessageW(hComboAdapter, CB_GETCURSEL, 0, 0);
        std::string ifname;
        if (TryGetAdapterPcapNameFromComboSelection(sel, ifname)) {
            return ifname;
        }

        // Fallback to visible text if we couldn't map back to pcapName
        wchar_t buf[512] = {0};
        GetWindowTextW(hComboAdapter, buf, (int)_countof(buf));
        std::wstring ws(buf);
        return CoreHelpers::WideToUtf8(ws);
    }
    return std::string();
}


bool MainWindow::IsAdapterComboEnabled() const
{
    if (hComboAdapter && IsWindow(hComboAdapter)) {
        return IsWindowEnabled(hComboAdapter) != FALSE;
    }
    return false;
}


// CheckLinkStatus moved to Win32Utils.cpp

// OnDisconnect moved to MainWindow.Logic.cpp

// CompleteDisconnect moved to MainWindow.Logic.cpp







