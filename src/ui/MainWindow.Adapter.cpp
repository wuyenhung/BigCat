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
#include "../include/core/CpuIsolation.h"
#include "../include/core/CpuTopology.h"
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
    if (GetModuleFileNameW(NULL, buf, MAX_PATH) == 0) return std::wstring(L".\\BigCat.ini");
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
            ? this->currentIfName.Copy()
            : (!this->webSelectedIfName.empty() ? this->webSelectedIfName.Copy() : savedAdapterUtf8);
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

    // Bounded cross-thread query: this helper is reachable from HTTP/SSE worker
    // threads (GetSelectedAdapterUtf8 -> here). Never use an unbounded
    // SendMessageW: it would block indefinitely while the UI thread is not pumping
    // messages (e.g. during MainWindow::Create()), see repo-facts 2026-10-03.
    DWORD_PTR dataResult = 0;
    if (SendMessageTimeoutW(hComboAdapter, CB_GETITEMDATA, (WPARAM)selectionIndex, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 100, &dataResult) == 0) {
        return false;
    }
    LRESULT data = static_cast<LRESULT>(dataResult);
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

// True when the calling thread is the one that created (and therefore owns) the
// given window. Used to keep all native-control access on the UI thread.
static bool IsUiThreadOwningWindow(HWND h) {
    if (!h || !IsWindow(h)) return false;
    return GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId();
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

    // NOTE: currentIfName / webSelectedIfName are read by the UI thread, the
    // watchdog/timer and the HTTP/SSE workers (GetWebSelectedAdapterUtf8), so these
    // assignments are a known cross-thread std::string race. Fixing it requires a
    // single-owner, mutex-protected selection cache (tracked in repo-facts
    // 2026-10-03) — do not assume these members are thread-safe.
    this->currentIfName = canonicalIfname;
    // mirror to status for visibility
    if (!isWeb && !canonicalIfname.empty()) {
        std::wstring w = CoreHelpers::Utf8ToWide(std::string("Selected adapter: ") + canonicalIfname + "\n");
        PrependStatus(w.c_str());
    }

    // ── Native-UI part must only run on the UI thread ────────────────────────
    // Everything below sends messages to native controls (CB_*, EnableWindow,
    // SetWindowTextW) and performs registry/IRQ-affinity work. Web-driven
    // selections arrive on an HTTP worker thread: a cross-thread SendMessageW can
    // block forever while the UI thread is not pumping messages (tray-only
    // deadlock; see repo-facts 2026-10-03) and must not run on the request path.
    // Post instead and let the UI thread apply it.
    if (!IsUiThreadOwningWindow(hwnd)) {
        if (hwnd && IsWindow(hwnd)) {
            PostMessageW(hwnd, WM_USER_APPLY_SELECTED_ADAPTER_UI, 0, 0);
        }
        return;
    }

    ApplySelectedAdapterToNativeUi(canonicalIfname);
}

// Native-UI half of SetSelectedAdapter. MUST run on the UI thread that owns hwnd.
void MainWindow::ApplySelectedAdapterToNativeUi(const std::string &canonicalIfname)
{
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
            LRESULT count = CoreHelpers::SendUiMessageBounded(hComboAdapter, CB_GETCOUNT, 0, 0);
            for (int i = 0; i < (int)count; ++i) {
                wchar_t buf[1024];
                LRESULT len = CoreHelpers::SendUiMessageBounded(hComboAdapter, CB_GETLBTEXT, (WPARAM)i, (LPARAM)buf);
                if (len > 0) {
                    std::string itemText = CoreHelpers::WideToUtf8(std::wstring(buf, buf + len));
                    if (itemText == desiredDisplay) {
                        CoreHelpers::SendUiMessageBounded(hComboAdapter, CB_SETCURSEL, (WPARAM)i, 0);
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

    // Update RT core display (always — not dependent on adapter selection)
    if (hStaticRtCore && IsWindow(hStaticRtCore)) {
        bool active = CoreHelpers::CpuIsolation::IsHardIsolationActive();
        if (active) {
            int physCore = CoreHelpers::CpuIsolation::GetRtPhysicalCoreIndex();

            // Resolve logical CPUs (Task Manager numbering) from topology
            CoreHelpers::CpuTopology::EnsurePopulated();
            const auto* ci = CoreHelpers::CpuTopology::FindCoreByPhysIdx(physCore);
            wchar_t rtbuf[128] = {0};
            if (ci && !ci->lps.empty()) {
                int primaryLp = ci->lps[0];
                if (ci->lps.size() >= 2) {
                    // Show primary + SMT sibling(s)
                    swprintf_s(rtbuf, L"CPU %d (實體核心 #%d, SMT:",
                               primaryLp, physCore);
                    for (size_t k = 1; k < ci->lps.size(); ++k)
                        swprintf_s(rtbuf + wcslen(rtbuf), 20, L" CPU%d", ci->lps[k]);
                    wcscat_s(rtbuf, L" 閒置)");
                } else {
                    swprintf_s(rtbuf, L"CPU %d (實體核心 #%d, 無 HT/SMT)",
                               primaryLp, physCore);
                }
            } else {
                swprintf_s(rtbuf, L"實體核心 #%d (無拓撲資訊)",
                           physCore);
            }
            SetWindowTextW(hStaticRtCore, rtbuf);
        } else {
            SetWindowTextW(hStaticRtCore, L"隔離未啟用");
        }
    }

    // Update IRQ display fields based on adapter type.
    if (!canonicalIfname.empty() && EtherCATMaster::IsSimulationAdapterSelection(canonicalIfname)) {
        // Simulation mode: no real NIC, clear IRQ fields
        if (hStaticAdapterIrqCpu && IsWindow(hStaticAdapterIrqCpu))
            SetWindowTextW(hStaticAdapterIrqCpu, L"Sim");
        if (hStaticOtherIrqAssignments && IsWindow(hStaticOtherIrqAssignments))
            SetWindowTextW(hStaticOtherIrqAssignments, L"Sim");
    } else if (!canonicalIfname.empty()) {
        // Real adapter: configure IRQ affinity and query actual values
        try {
            std::string displayName = canonicalIfname;
            // Build a display name for logging (same format as connect-time)
            std::string err;
            auto adapters = CoreHelpers::EnumerateAdapters(err);
            for (const auto& ad : adapters) {
                if (ad.pcapName == canonicalIfname) {
                    displayName = CoreHelpers::MakeAdapterDisplayString(ad);
                    break;
                }
            }
            std::string irqLog;
            bool irqDevicesSet = false;
            CoreHelpers::ApplySystemWideIrqAffinityToCpu0(displayName, irqLog);
            if (!irqLog.empty()) {
                // Post summary lines to status window (same filter as connect-time)
                std::istringstream iss(irqLog);
                std::string line;
                while (std::getline(iss, line)) {
                    if (line.empty()) continue;
                    if (line.find("=== Summary") != std::string::npos ||
                        line.find("CPU ") != std::string::npos ||
                        line.find("WARNING") != std::string::npos ||
                        line.find("Resolved EtherCAT") != std::string::npos ||
                        line.find("REBOOT") != std::string::npos ||
                        line.find("需重新啟動") != std::string::npos ||
                        line.find("NOTE:") != std::string::npos) {
                        PostUTF8LogToUI(line + "\n");
                    }
                    if (line.find("=== Summary") != std::string::npos) {
                        irqDevicesSet = (line.find(" 0 devices") == std::string::npos);
                    }
                }
            }
            if (irqDevicesSet) {
                PostUTF8LogToUI(std::string("\n"));
                PostUTF8LogToUI(std::string("⚠ ================================================================ ⚠\n"));
                PostUTF8LogToUI(std::string("⚠  IRQ 中斷分散設定已寫入登錄檔，需要【重新啟動電腦】才會生效！\n"));
                PostUTF8LogToUI(std::string("⚠  目前的 IRQ 配置仍是舊的，重開機前 EtherCAT 可能仍受干擾。\n"));
                PostUTF8LogToUI(std::string("⚠ ================================================================ ⚠\n"));
            }

            // ── Query ACTUAL (runtime) IRQ affinity and display on main screen ──
            // This reads the registry keys we just wrote (or pre-existing ones)
            // and shows the CURRENT effective assignment, so the user can see
            // whether a reboot is still needed (target ≠ actual until reboot).
            try {
                // 1) Selected adapter: effective IRQ CPU + interrupt type
                if (hStaticAdapterIrqCpu && IsWindow(hStaticAdapterIrqCpu)) {
                    int irqCpu = -1;
                    std::string irqQueryLog;
                    bool queryOk = CoreHelpers::GetAdapterEffectiveIrqCpu(displayName, irqCpu, irqQueryLog);

                    // Detect MSI-X vs Legacy INTx from registry
                    CoreHelpers::AdapterInterruptType intType = CoreHelpers::AdapterInterruptType::Unknown;
                    std::string intTypeDesc;
                    std::string intTypeLog;
                    CoreHelpers::GetAdapterInterruptType(displayName, intType, intTypeDesc, intTypeLog);

                    // Query actual IRQ number from WMI (same value as msinfo32)
                    unsigned long long actualIrq = 0;
                    std::string actualIrqName;
                    std::string actualIrqLog;
                    bool irqOk = CoreHelpers::GetAdapterActualIrqNumber(displayName, actualIrq, actualIrqName, actualIrqLog);

                    std::wstring adapterW = CoreHelpers::Utf8ToWide(displayName);
                    std::wstring txt = adapterW + L" → ";
                    if (irqOk && actualIrq > 0) {
                        txt += L"IRQ " + std::to_wstring(actualIrq);
                        if (queryOk && irqCpu >= 0) {
                            txt += L" (CPU" + std::to_wstring(irqCpu) + L")";
                        }
                    } else if (queryOk && irqCpu >= 0) {
                        txt += L"CPU" + std::to_wstring(irqCpu);
                    } else {
                        txt += L"N/A";
                    }
                    // Append interrupt type tag
                    if (intType == CoreHelpers::AdapterInterruptType::MsiX) {
                        txt += L" [MSI-X]";
                    } else if (intType == CoreHelpers::AdapterInterruptType::LegacyIntx) {
                        txt += L" [Legacy INTx ⚠]";
                    }
                    SetWindowTextW(hStaticAdapterIrqCpu, txt.c_str());

                    if (!irqQueryLog.empty())
                        PostUTF8LogToUI(std::string("[網卡 IRQ 查詢] ") + irqQueryLog);
                    if (!intTypeLog.empty())
                        PostUTF8LogToUI(std::string("[中斷類型] ") + intTypeLog);
                    if (!actualIrqLog.empty())
                        PostUTF8LogToUI(std::string("[實際 IRQ] ") + actualIrqLog);
                    if (intType == CoreHelpers::AdapterInterruptType::LegacyIntx) {
                        PostUTF8LogToUI(std::string("⚠ Legacy INTx 中斷無法透過 registry 綁定 CPU，IRQ affinity 設定對此網卡無效\n"));
                    }
                }

                // 2) Other IRQ assignments (GPU, NVMe, USB, Gaming NIC)
                if (hStaticOtherIrqAssignments && IsWindow(hStaticOtherIrqAssignments)) {
                    std::string otherIrqSummary;
                    std::string otherIrqLog;
                    if (CoreHelpers::GetSystemIrqAffinitySummary(displayName, otherIrqSummary, otherIrqLog) && !otherIrqSummary.empty()) {
                        SetWindowTextW(hStaticOtherIrqAssignments,
                            CoreHelpers::Utf8ToWide(otherIrqSummary).c_str());
                    } else {
                        SetWindowTextW(hStaticOtherIrqAssignments, L"未設定 (需重啟)");
                    }
                    if (!otherIrqLog.empty()) {
                        PostUTF8LogToUI(std::string("[其他 IRQ 查詢] ") + otherIrqLog);
                    }
                }
            } catch (...) {
                // Non-fatal: IRQ query display failure shouldn't block adapter selection
            }
        } catch (const std::exception &ex) {
            PostUTF8LogToUI(std::string("[Adapter IRQ] 設定失敗: ") + ex.what() + "\n");
        } catch (...) {
            PostUTF8LogToUI(std::string("[Adapter IRQ] 設定時發生未知異常\n"));
        }
    }
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
        // This getter is also called from HTTP/SSE worker threads
        // (HttpServer.API.cpp GetSelectedAdapterUiState). Use the bounded helper so a
        // cross-thread query can never block while the UI thread is not pumping
        // messages; on timeout the caller falls back to its cache.
        // Resolve the current combo selection against the cached combo-to-pcap mapping.
        bool selTimedOut = false;
        LRESULT sel = CoreHelpers::SendUiMessageBounded(hComboAdapter, CB_GETCURSEL, 0, 0, 100, &selTimedOut);
        if (selTimedOut) {
            return std::string();
        }
        std::string ifname;
        if (TryGetAdapterPcapNameFromComboSelection((int)sel, ifname)) {
            return ifname;
        }

        // Fallback to visible text if we couldn't map back to pcapName
        wchar_t buf[512] = {0};
        bool textTimedOut = false;
        (void)CoreHelpers::SendUiMessageBounded(hComboAdapter, WM_GETTEXT, (WPARAM)_countof(buf), (LPARAM)buf, 100, &textTimedOut);
        if (textTimedOut) {
            return std::string();
        }
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







