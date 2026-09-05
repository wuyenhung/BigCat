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
#include "../app/AppController.h"
#include "../include/EtherCATMaster.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/PDOModel.h"
#include "../lib/tinyxml2/tinyxml2.h"
#include "../include/MotionConfigIO.h"
// MotionLogic provides helpers to stage controlword values into PDO output map
#include "../include/MotionLogic.h"
// AutoWindow helper (Create/Destroy)
#include "../include/AutoWindow.h"

// =============================================================================
// Static utility functions to avoid code duplication
// =============================================================================

// Generate UI timestamp string [HH:MM:SS.mmm]
static std::string UITimestampString() {
    SYSTEMTIME st; GetLocalTime(&st);
    char tbuf[64]; sprintf_s(tbuf, sizeof(tbuf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(tbuf);
}

// Check if a string is a command-like token that should not be timestamped
static bool IsCommandLikeString(const std::string &s) {
    // Known command-style tokens posted to WM_USER_POST_UTF8_LOG that the UI
    // expects verbatim (no timestamp prefix). Match by prefix.
    static const char* prefixes[] = {
        "ENABLE_CONTROLS",
        "ENABLE_CONTROLS_NOREFLOW",
        "ENABLE_HOME",
        "UPDATE_SLAVE_LIST",
        "SHUTDOWN_COMPLETE",
        "STOP_TIMER",
        "START_TIMER",
        "HTTP_LOCAL:",
        "HTTP_CMD:",
        "ADAPTERS_JSON:",
        "HTTP_LOCAL:STATUS:",
    };
    for (const char* p : prefixes) {
        size_t len = strlen(p);
        if (s.size() >= len && s.rfind(p, 0) == 0) return true;
    }
    return false;
}

// Check if string already has a timestamp prefix like "[HH:MM:SS"
static bool HasTimestampPrefix(const std::string &utf8) {
    return utf8.size() >= 4 && utf8[0] == '[' && 
           std::isdigit((unsigned char)utf8[1]) && 
           std::isdigit((unsigned char)utf8[2]) && 
           utf8[3] == ':';
}

static constexpr const char* kSimulationMessagePrefixUtf8 = u8"[模擬測試] ";
static constexpr const wchar_t* kSimulationMessagePrefixWide = L"[模擬測試] ";
static constexpr uint16_t kErrorCodeCommunicationFailureSentinel = 0xFFFF;

static std::string FormatHex16(uint16_t value)
{
    std::ostringstream oss;
    oss << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << value;
    return oss.str();
}

static std::wstring FormatErrorCode603FWide(bool valid, uint16_t value)
{
    if (!valid) {
        return L"N/A";
    }

    wchar_t buffer[16];
    swprintf_s(buffer, L"0x%04X", static_cast<unsigned>(value));
    return buffer;
}

static void AppendPdoViolationName(std::ostringstream& oss, PDOFilter::ViolationType type)
{
    bool first = true;
    auto append = [&](const char* text) {
        if (!first) oss << "|";
        oss << text;
        first = false;
    };

    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Velocity)) append("速度過大");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Acceleration)) append("加速度過大");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::Deceleration)) append("減速度過大");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMin)) append("超出下限");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::HardLimitMax)) append("超出上限");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::InputAnomaly)) append("MMF輸入異常");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::StaleInput)) append("MMF輸入停滯");
    if (PDOFilter::HasFlag(type, PDOFilter::ViolationType::TrackingError)) append("偏差值過大");
    if (first) oss << "錯誤旗標";
}

static size_t TimestampPayloadOffsetUtf8(const std::string& line)
{
    if (!HasTimestampPrefix(line)) return 0;
    size_t close = line.find(']');
    if (close == std::string::npos) return 0;
    size_t offset = close + 1;
    if (offset < line.size() && line[offset] == ' ') ++offset;
    return offset;
}

static size_t TimestampPayloadOffsetWide(const std::wstring& line)
{
    if (line.size() < 4 || line[0] != L'[' || line[1] < L'0' || line[1] > L'9' ||
        line[2] < L'0' || line[2] > L'9' || line[3] != L':') {
        return 0;
    }
    size_t close = line.find(L']');
    if (close == std::wstring::npos) return 0;
    size_t offset = close + 1;
    if (offset < line.size() && line[offset] == L' ') ++offset;
    return offset;
}

static bool IsBlankLineWide(const std::wstring& line)
{
    for (wchar_t ch : line) {
        if (ch != L' ' && ch != L'\t' && ch != L'\r' && ch != L'\n') return false;
    }
    return true;
}

static bool IsBlankLineUtf8(const std::string& line)
{
    for (unsigned char ch : line) {
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') return false;
    }
    return true;
}

static std::wstring AddSimulationPrefixToLine(std::wstring line)
{
    if (line.empty() || IsBlankLineWide(line)) return line;
    const size_t insertAt = TimestampPayloadOffsetWide(line);
    const size_t prefixLen = wcslen(kSimulationMessagePrefixWide);
    if (line.compare(insertAt, prefixLen, kSimulationMessagePrefixWide) == 0) return line;
    line.insert(insertAt, kSimulationMessagePrefixWide);
    return line;
}

static std::string AddSimulationPrefixToLine(std::string line)
{
    if (line.empty() || IsBlankLineUtf8(line)) return line;
    const size_t insertAt = TimestampPayloadOffsetUtf8(line);
    const size_t prefixLen = strlen(kSimulationMessagePrefixUtf8);
    if (line.compare(insertAt, prefixLen, kSimulationMessagePrefixUtf8) == 0) return line;
    line.insert(insertAt, kSimulationMessagePrefixUtf8);
    return line;
}

static std::wstring AddSimulationPrefixToStatusText(const std::wstring& text)
{
    std::wstring out;
    out.reserve(text.size() + wcslen(kSimulationMessagePrefixWide));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t lineEnd = pos;
        while (lineEnd < text.size() && text[lineEnd] != L'\r' && text[lineEnd] != L'\n') ++lineEnd;
        out += AddSimulationPrefixToLine(text.substr(pos, lineEnd - pos));
        if (lineEnd < text.size()) {
            out += text[lineEnd++];
            if (lineEnd < text.size() && text[lineEnd - 1] == L'\r' && text[lineEnd] == L'\n') {
                out += text[lineEnd++];
            }
        }
        pos = lineEnd;
    }
    return out;
}

static std::string AddSimulationPrefixToStatusText(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + strlen(kSimulationMessagePrefixUtf8));
    size_t pos = 0;
    while (pos < text.size()) {
        size_t lineEnd = pos;
        while (lineEnd < text.size() && text[lineEnd] != '\r' && text[lineEnd] != '\n') ++lineEnd;
        out += AddSimulationPrefixToLine(text.substr(pos, lineEnd - pos));
        if (lineEnd < text.size()) {
            out += text[lineEnd++];
            if (lineEnd < text.size() && text[lineEnd - 1] == '\r' && text[lineEnd] == '\n') {
                out += text[lineEnd++];
            }
        }
        pos = lineEnd;
    }
    return out;
}

// --- INI helpers for persistent UI options ---------------------------------
static std::wstring GetAppIniPath()
{
    wchar_t buf[MAX_PATH] = {0};
    if (GetModuleFileNameW(NULL, buf, MAX_PATH) == 0) return std::wstring(L".\\WuBigCat.ini");
    // strip extension
    wchar_t *p = wcsrchr(buf, L'\\');
    wchar_t *dot = wcsrchr(buf, L'.');
    if (dot && (!p || dot > p)) *dot = L'\0';
    std::wstring ini = std::wstring(buf) + L".ini";
    return ini;
}

static void WriteIniBool(const wchar_t* section, const wchar_t* key, bool value)
{
    std::wstring ini = GetAppIniPath();
    WritePrivateProfileStringW(section, key, value ? L"1" : L"0", ini.c_str());
}

// Implementations of non-UI methods originally in MainWindow.cpp
// Keep MainWindow.cpp focused on UI creation/layout and lightweight handlers.

// -----------------------------------------------------------------------------
// App/HTTP bridging helpers
// -----------------------------------------------------------------------------

void MainWindow::PostUTF8LogToUI(std::string utf8) {
    // NOTE: Command-like messages (SHUTDOWN_COMPLETE, START_TIMER, STOP_TIMER,
    // HTTP_CMD: etc.) must be delivered to the UI even when the window is
    // minimized/trayed so internal state transitions complete correctly.
    // Non-command log lines may be safely suppressed while minimized to avoid
    // activating the window.
    
    // NOTE: We intentionally do NOT suppress UI log updates for auto-down-to-origin
    // here any longer. Previously we blocked posting while autoDownToOriginRunning
    // to avoid heavy redraw; that suppression prevented any UI-visible logs and
    // made debugging the workflow hard. Instead, rely on StatusLogger's batching
    // (WM_USER_STATUS_UPDATE_BATCH) and removed SetFocus/WM_SETREDRAW side-effects
    // so the UI will not be unexpectedly activated while still receiving logs.
    
    // Allocate on heap and post to UI thread; UI thread deletes the pointer
    // Use static helper functions to avoid code duplication
    bool hasTs = HasTimestampPrefix(utf8);
    bool isCmd = IsCommandLikeString(utf8);

    // When minimized, still allow command-like messages through so that important
    // state transitions (eg. SHUTDOWN_COMPLETE) are not missed and leave the
    // application/UI in a consistent state. Non-command logs are still
    // suppressed when the window is minimized/trayed.
    if ((minimizedToTray || isMinimizedState) && !isCmd && !this->forceRunWhenMinimized) {
        return;
    }

    std::string out = (hasTs || isCmd) ? std::move(utf8) : UITimestampString() + utf8;
    if (!isCmd && IsSimulationModeSelectedForMessages()) {
        out = AddSimulationPrefixToStatusText(out);
    }
    if (!isCmd) {
        AppController::GetInstance().PublishLogEvent("MainWindow", out);
    }
    PostedUtf8UiMessage* message =
        new PostedUtf8UiMessage{std::move(out),
                                isCmd ? 0 : statusLogEpoch_.load(std::memory_order_acquire),
                                isCmd};
    PostMessage(hwnd, WM_USER_POST_UTF8_LOG, 0, reinterpret_cast<LPARAM>(message));
}

// Public static helper implementation. Use same timestamp prefixing rules
// as PostUTF8LogToUI but post to the provided HWND. This lets non-instance
// callbacks (server threads, subclass procs via top HWND, etc.) post
// UTF-8 log lines while ensuring consistent timestamps and ownership.
void MainWindow::PostUTF8LogToHWND(HWND target, std::string utf8) {
    if (!target) return;
    // Use static helper functions to avoid code duplication
    bool hasTs = HasTimestampPrefix(utf8);
    bool isCmd = IsCommandLikeString(utf8);
    std::string out = (hasTs || isCmd) ? std::move(utf8) : UITimestampString() + utf8;
    MainWindow* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(target, GWLP_USERDATA));
    if (!isCmd && self && self->IsSimulationModeSelectedForMessages()) {
        out = AddSimulationPrefixToStatusText(out);
    }
    if (!isCmd) {
        AppController::GetInstance().PublishLogEvent("MainWindow.Static", out);
    }
    const uint64_t epoch = (!isCmd && self) ? self->statusLogEpoch_.load(std::memory_order_acquire) : 0;
    PostedUtf8UiMessage* message = new PostedUtf8UiMessage{std::move(out), epoch, isCmd};
    PostMessage(target, WM_USER_POST_UTF8_LOG, 0, reinterpret_cast<LPARAM>(message));
}

// Post SDO item / batch payload to UI thread using the dedicated message id.
// The existing WM_USER_SDO_ITEM_READ handler expects a heap-allocated
// std::string* in lParam and will delete it after processing, so keep that
// ownership contract here.
void MainWindow::PostSDOItemToHWND(HWND target, std::string utf8) {
    if (!target) return;
    std::string* s = new std::string(std::move(utf8));
    PostMessage(target, WM_USER_SDO_ITEM_READ, 0, reinterpret_cast<LPARAM>(s));
}

// -----------------------------------------------------------------------------
// Connection lifecycle (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------

void MainWindow::OnSendMailbox() {
    if (!IsMasterInitialized()) return;

    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // 取得 Mailbox 資料
    wchar_t mailboxDataW[512];
    GetWindowTextW(hEditMailbox, mailboxDataW, 512);

    // 轉換 hex 字串為 bytes（支援以空白分隔的十六進位 token）
    uint8_t data[256];
    int dataSize = 0;
    wchar_t* context = NULL;
    for (wchar_t* token = wcstok_s(mailboxDataW, L" \t,;", &context);
         token != NULL && dataSize < 256;
         token = wcstok_s(NULL, L" \t,;", &context)) {
        data[dataSize++] = (uint8_t)wcstoul(token, NULL, 16);
    }

    if (dataSize <= 0) return;

    if (MasterSendMailbox(slave, data, dataSize)) {
        PrependStatus(L"Mailbox sent successfully\r\n");
    } else {
        std::wstring errW = CoreHelpers::Utf8ToWide(MasterGetLastError());
        PrependStatus(errW.c_str());
        PrependStatus(L"\r\n");
    }
}

// -----------------------------------------------------------------------------
// Timer-driven periodic updates (moved from MainWindow.cpp)
// -----------------------------------------------------------------------------
void MainWindow::UpdateSlaveList() {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master) return;
    if ((minimizedToTray || isMinimizedState) && !this->forceRunWhenMinimized) return; // skip heavy UI updates only when minimized and not forced
    const auto* ethercatService = AppController::GetInstance().GetEtherCATService();
    const bool isMasterRunning = ethercatService ? ethercatService->IsRunning() : master->IsRunning();
    const bool isMasterInitialized = ethercatService ? ethercatService->IsInitialized() : master->IsInitialized();
    // Diagnostic: if we are running UpdateSlaveList while minimized (forced), log it for visibility
    if ((minimizedToTray || isMinimizedState) && this->forceRunWhenMinimized) {
        try { MainWindow::PostUTF8LogToHWND(hwnd, std::string("HTTP_LOCAL:[UpdateSlaveList] running while minimized\n")); } catch(...) {}
    }

    ListView_DeleteAllItems(hListSlaves);
    
    // 同時更新從站下拉選單
    // NOTE: From 2025-11-16 the application enforces that the slave list is
    // only updated as part of the connection lifecycle (OnConnect) — manual
    // updates via clicks are disabled to avoid unnecessary SDO reads.
    SendMessageW(hComboSlaves, CB_RESETCONTENT, 0, 0);

    auto slaves = master->GetSlaveInfo();
    
    // If no slaves found, keep the ListView and combobox empty (no placeholder entries)
    if (slaves.empty()) {
        // Ensure combobox has no selection
        SendMessageW(hComboSlaves, CB_SETCURSEL, (WPARAM)CB_ERR, 0);
        return;
    }

    std::array<uint16_t, 6> errorCodes603F = {0, 0, 0, 0, 0, 0};
    std::array<bool, 6> errorCodeValid603F = {false, false, false, false, false, false};
    GetLastReportedErrorCodes(errorCodes603F, errorCodeValid603F);

    for (const auto& slave : slaves) {
        LVITEM lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = slave.position - 1;

        // 位置
        wchar_t posBuf[32];
        swprintf_s(posBuf, L"%d", slave.position);
        lvi.pszText = posBuf;
        lvi.iSubItem = 0;
        ListView_InsertItem(hListSlaves, &lvi);

        // 名稱 - 嘗試讀取實際設備名稱
        std::wstring nameW;
        if (isMasterRunning) {
            char deviceName[128] = {0};
            int nameSize = sizeof(deviceName);
            if (master->ReadSDO(slave.position, 0x1008, 0x00, deviceName, nameSize)) {
                // 成功讀取設備名稱，轉換為寬字元
                nameW = CoreHelpers::Utf8ToWide(std::string(deviceName));
            }
        }
        // 如果沒有讀到設備名稱，使用 SOEM 識別的名稱
        if (nameW.empty()) {
            nameW = std::wstring(slave.name.begin(), slave.name.end());
        }
        ListView_SetItemText(hListSlaves, lvi.iItem, 1, (LPWSTR)nameW.c_str());

        // 0x603F Error Code
        const int errorIndex = slave.position - 1;
        std::wstring errorCodeText = (errorIndex >= 0 && errorIndex < 6)
            ? FormatErrorCode603FWide(errorCodeValid603F[errorIndex], errorCodes603F[errorIndex])
            : L"N/A";
        ListView_SetItemText(hListSlaves, lvi.iItem, 2, (LPWSTR)errorCodeText.c_str());

        // Vendor ID
        wchar_t buf[64];
        swprintf_s(buf, L"0x%08X", (unsigned int)slave.vendorId);
        ListView_SetItemText(hListSlaves, lvi.iItem, 3, buf);

        // Product Code
        swprintf_s(buf, L"0x%08X", (unsigned int)slave.productCode);
        ListView_SetItemText(hListSlaves, lvi.iItem, 4, buf);

        // 狀態
        swprintf_s(buf, L"0x%02X", (unsigned int)slave.state);
        ListView_SetItemText(hListSlaves, lvi.iItem, 5, buf);

        // Outputs
        swprintf_s(buf, L"%d", (int)slave.outputBits);
        ListView_SetItemText(hListSlaves, lvi.iItem, 6, buf);

        // Inputs
        swprintf_s(buf, L"%d", (int)slave.inputBits);
        ListView_SetItemText(hListSlaves, lvi.iItem, 7, buf);
    }

    // Populate slave combobox and select first
    // 嘗試讀取每個從站的實際設備名稱
    for (const auto& s : slaves) {
        std::wstring displayName;
        
        // 嘗試從從站讀取設備名稱 (0x1008:00)
        if (isMasterRunning) {
            char deviceName[128] = {0};
            int nameSize = sizeof(deviceName);
            if (master->ReadSDO(s.position, 0x1008, 0x00, deviceName, nameSize)) {
                // 成功讀取設備名稱，轉換為寬字元
                displayName = CoreHelpers::Utf8ToWide(std::string(deviceName));
            }
        }
        
        // 如果沒有讀到設備名稱，使用 SOEM 識別的名稱
        if (displayName.empty()) {
            displayName = std::wstring(s.name.begin(), s.name.end());
        }
        
        // 格式化顯示：站號: 設備名稱
        std::wstringstream ss;
        ss << s.position << L": " << displayName;
        int addIdx = (int)SendMessageW(hComboSlaves, CB_ADDSTRING, 0, (LPARAM)ss.str().c_str());
        // Bind the real slave position to the combobox item data so selection -> position mapping is robust
        SendMessageW(hComboSlaves, CB_SETITEMDATA, (WPARAM)addIdx, (LPARAM)s.position);
    }
    if (!slaves.empty()) {
        SendMessageW(hComboSlaves, CB_SETCURSEL, 0, 0);
        // 初始選擇第一個從站並完整更新顯示（包括 SDO 和 PDO）
        if (isMasterRunning) {
            LRESULT cur = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
            int pos = (int)SendMessageW(hComboSlaves, CB_GETITEMDATA, cur, 0);
            if (pos <= 0) pos = 1; // fallback
            
            // Set confirmed slave to trigger full display update
            confirmedSlave = pos;

            // Auto-load associated motion config for this slave if present
            auto itCfg = savedMotionConfigForSlave.find(confirmedSlave);
            if (isMasterInitialized && itCfg != savedMotionConfigForSlave.end()) {
                std::string loadedUtf82;
                MotionConfigIO::LoadMotionParamsFromFileAtPath(hListMotionParams, itCfg->second, loadedUtf82);
            } else if (!isMasterInitialized && !savedMotionConfigGlobal.empty()) {
                // When not connected, keep global config applied
                std::string loadedUtf83;
                MotionConfigIO::LoadMotionParamsFromFileAtPath(hListMotionParams, savedMotionConfigGlobal, loadedUtf83);
            }
            
            // Restore saved SDO values if any, or clear the value column
            auto savedIt = savedSDOValues.find(confirmedSlave);
                if (savedIt != savedSDOValues.end() && hListSDOParams && IsWindow(hListSDOParams)) {
                const std::vector<std::wstring>& vals = savedIt->second;
                for (int ii = 0; ii < (int)vals.size() && ii < (int)currentObjects.size(); ++ii) {
                    // Value column moved to 5; restore decimal values there
                    ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(vals[ii].c_str()));
                    // also restore hex column if available (now column 6)
                    auto ithex4 = savedSDOHexValues.find(confirmedSlave);
                    if (ithex4 != savedSDOHexValues.end() && (int)ithex4->second.size() > ii) {
                        ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(ithex4->second[ii].c_str()));
                    }
                }
            } else if (hListSDOParams && IsWindow(hListSDOParams)) {
                // No saved values - clear the value and hex columns
                for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
                    ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(L""));
                    ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
                }
            }
            
            // Build PDO items dynamically from parsed ESI `currentPdos` and
            // only show/rebuild the PDO area after successful connection.
            if (isMasterInitialized) {
                // If no ESI was loaded by the user, attempt to request the ESI
                // from the slave via master->DumpESI and parse it. This allows
                // dynamic population of PDOs immediately after connect.
                if (currentPdos.empty()) {
                    if (master) {
                        // Attempt to discover PDOs directly via SDOs when no ESI is loaded.
                        // Read RxPDO assignment (0x1C12) and TxPDO assignment (0x1C13)
                        currentPdos.clear();
                        // Helper lambda to read assignment and mappings
                        auto readPdoAssignment = [&](uint16_t assignIndex, const char* pdoTypePrefix) {
                            // read number of PDOs assigned (subindex 0)
                                    uint8_t countBuf = 0;
                                    int countBufLen = (int)sizeof(countBuf);
                                    if (!master->ReadSDO(confirmedSlave, assignIndex, 0x00, (char*)&countBuf, countBufLen)) return;
                                    int count = (int)countBuf;
                            for (int p = 1; p <= count; ++p) {
                                uint32_t pdoIdx = 0;
                                int readSz = (int)sizeof(pdoIdx);
                                if (!master->ReadSDO(confirmedSlave, assignIndex, (uint8_t)p, (char*)&pdoIdx, readSz)) continue;
                                // pdoIdx may be little-endian on wire; ensure host order
                                // Interpret as little-endian 32-bit
                                // On little-endian hosts this is a no-op
                                // Extract index (low 16 bits)
                                uint16_t pdoIndex = (uint16_t)(pdoIdx & 0xFFFF);

                                // Build a Pdo entry by reading its mapping count and entries
                                EsiPdoEntry pdoe;
                                pdoe.pdoType = std::string(pdoTypePrefix);
                                // create a simple name from index
                                {
                                    char buf[32]; sprintf_s(buf, sizeof(buf), "%s_0x%04X", pdoTypePrefix, pdoIndex);
                                    pdoe.name = std::string(buf);
                                }

                                // read mapping count at subindex 0 of the PDO index
                                uint8_t mapCount = 0;
                                int mapCountLen = (int)sizeof(mapCount);
                                if (!master->ReadSDO(confirmedSlave, pdoIndex, 0x00, (char*)&mapCount, mapCountLen)) {
                                    // fallback: try reading as 16-bit count
                                    uint16_t mapCount16 = 0;
                                    int mapCount16Len = (int)sizeof(mapCount16);
                                    if (master->ReadSDO(confirmedSlave, pdoIndex, 0x00, (char*)&mapCount16, mapCount16Len)) mapCount = (uint8_t)mapCount16;
                                }

                                for (int me = 1; me <= (int)mapCount; ++me) {
                                    uint32_t mapping = 0;
                                    int mappingLen = (int)sizeof(mapping);
                                    if (!master->ReadSDO(confirmedSlave, pdoIndex, (uint8_t)me, (char*)&mapping, mappingLen)) continue;
                                    // mapping format: 0xIIIISSBB where IIII=index, SS=subindex, BB=bitlength
                                    uint16_t objIdx = (uint16_t)((mapping >> 16) & 0xFFFF);
                                    uint8_t objSub = (uint8_t)((mapping >> 8) & 0xFF);
                                    uint8_t bitlen = (uint8_t)(mapping & 0xFF);
                                    EsiObjectEntry oe;
                                    oe.index = objIdx;
                                    oe.subindex = objSub;
                                    oe.bitlen = bitlen;
                                    // name unknown without ESI; leave empty
                                    oe.name = "";
                                    pdoe.entries.push_back(oe);
                                }

                                currentPdos.push_back(pdoe);
                            }
                        };

                        // Read RxPDOs (master->slave outputs)
                        readPdoAssignment(0x1C12, "RxPdo");
                        // Read TxPDOs (slave->master inputs)
                        readPdoAssignment(0x1C13, "TxPdo");

                        // If still empty, fall back to DumpESI attempt (legacy behaviour)
                        if (currentPdos.empty()) {
                            std::string dump;
                            if (master->DumpESI(confirmedSlave, dump)) {
                                // DumpESI prints EEPROM/ESI hex dump, not XML. Keep for diagnostics only.
                                std::wstring w = CoreHelpers::Utf8ToWide(dump);
                                PrependStatus(w.c_str());
                            }
                        }
                    }
                }
                // Clear any previous items
                pdoDisplay.Clear();
                // Populate pdoDisplay.pdoItems from currentPdos (ESI parsed)
                for (const auto &p : currentPdos) {
                    for (const auto &e : p.entries) {
                        PDOItem itm;
                                // Determine item name: prefer ESI-provided name; if absent try currentObjects lookup; otherwise use address
                                std::wstring name;
                                if (!e.name.empty()) {
                                    name = std::wstring(e.name.begin(), e.name.end());
                                } else {
                                    // try to find matching object entry parsed earlier
                                    for (const auto &o : currentObjects) {
                                        if (o.index == (uint16_t)e.index && o.subindex == (uint8_t)e.subindex) {
                                            name = std::wstring(o.name.begin(), o.name.end());
                                            break;
                                        }
                                    }
                                }
                                itm.name = name; // may be empty for now
                                itm.index = (uint16_t)e.index;
                                itm.subindex = (uint8_t)e.subindex;
                        itm.bitLength = (uint8_t)e.bitlen;
                        itm.isOutput = (p.pdoType.find("Rx") != std::string::npos); // RxPDO -> outputs
                        // editable if access contains 'w' (rw or wo)
                        std::string acc = e.accessNormalized.empty() ? e.access : e.accessNormalized;
                        for (auto &c : acc) c = (char)tolower((unsigned char)c);
                        itm.isEditable = (acc.find('w') != std::string::npos);
                        // Additionally, allow outputs (RxPDO) to be editable so user can write outputs
                        if (itm.isOutput) itm.isEditable = true;
                        itm.byteOffset = 0; // will be set by RecalculateOffsetsFromSDO
                        itm.currentValue = L"";
                        itm.displayValue = L"";
                        itm.hLabel = itm.hValue = itm.hEdit = itm.hButton = NULL;
                        itm.dataType = std::wstring(e.dataType.begin(), e.dataType.end());
                        itm.unit = L""; itm.minValue = L""; itm.maxValue = L""; itm.defaultValue = L"";
                            // format address and description from ESI entry
                            {
                                wchar_t buf[32];
                                swprintf_s(buf, L"0x%04X:%02X", itm.index, itm.subindex);
                                itm.address = buf;
                                // if name still empty, use address as visible name
                                if (itm.name.empty()) itm.name = buf;
                            }
                            itm.description = std::wstring(e.description.begin(), e.description.end());
                        pdoDisplay.pdoItems.push_back(itm);
                    }
                }

                // Recalculate offsets using master-provided IO base and then rebuild UI
                if (pdoDisplay.RecalculateOffsetsFromSDO(master, confirmedSlave)) {
                    pdoDisplay.RebuildPDOControls();
                    // Restore any cached edit texts by name
                    RestorePDOInputsFromGlobal();
                } else {
                    // still rebuild controls so UI shows items even without absolute offsets
                    pdoDisplay.RebuildPDOControls();
                    RestorePDOInputsFromGlobal();
                }

                // IMPORTANT: ensure PDO descriptors are registered for ALL slaves
                // so the EtherCAT master can update mechanical-unit metrics for every
                // slave continuously — not only the currently-selected one.
                if (master && master->IsInitialized()) {
                    for (const auto &s2 : slaves) {
                        // read PDO assignment for this slave (RxPDO 0x1C12, TxPDO 0x1C13)
                        std::vector<PDOChannelDescriptor> descriptors;
                        descriptors.reserve(32);
                        auto readForSlave = [&](uint16_t assignIndex, bool isOutput) {
                            uint8_t cnt = 0; int cntSz = (int)sizeof(cnt);
                            if (!master->ReadSDO(s2.position, assignIndex, 0x00, (char*)&cnt, cntSz)) return;
                            int count = (int)cnt;
                            for (int p = 1; p <= count; ++p) {
                                uint32_t pdoIdx = 0; int idxSz = (int)sizeof(pdoIdx);
                                if (!master->ReadSDO(s2.position, assignIndex, (uint8_t)p, (char*)&pdoIdx, idxSz)) continue;
                                uint16_t pdoIndex = (uint16_t)(pdoIdx & 0xFFFF);
                                // mapping count
                                uint8_t mapCount = 0; int mapCountLen = (int)sizeof(mapCount);
                                if (!master->ReadSDO(s2.position, pdoIndex, 0x00, (char*)&mapCount, mapCountLen)) {
                                    uint16_t mapCount16 = 0; int mlen = (int)sizeof(mapCount16);
                                    if (!master->ReadSDO(s2.position, pdoIndex, 0x00, (char*)&mapCount16, mlen)) continue;
                                    mapCount = (uint8_t)mapCount16;
                                }
                                for (int me = 1; me <= (int)mapCount; ++me) {
                                    uint32_t mapping = 0; int mappingLen = (int)sizeof(mapping);
                                    if (!master->ReadSDO(s2.position, pdoIndex, (uint8_t)me, (char*)&mapping, mappingLen)) continue;
                                    uint16_t objIdx = (uint16_t)((mapping >> 16) & 0xFFFF);
                                    uint8_t objSub = (uint8_t)((mapping >> 8) & 0xFF);
                                    uint8_t bitlen = (uint8_t)(mapping & 0xFF);
                                    PDOChannelDescriptor desc;
                                    desc.name = L"";
                                    desc.index = objIdx; desc.subindex = objSub; desc.bitLength = bitlen; desc.isOutput = isOutput;
                                    descriptors.push_back(std::move(desc));
                                }
                            }
                        };
                        readForSlave(0x1C12, true);
                        readForSlave(0x1C13, false);

                        // compute byte offsets (out/in) using master IO base
                        int outBase = 0, inBase = 0; if (master->GetSlaveIOBaseBytes(s2.position, outBase, inBase)) {
                            int curOut = outBase; int curIn = inBase;
                            for (auto &d : descriptors) {
                                int bytes = (d.bitLength > 0) ? ((d.bitLength + 7) / 8) : 4;
                                if (d.isOutput) { d.byteOffset = curOut - outBase; curOut += bytes; }
                                else { d.byteOffset = curIn - inBase; curIn += bytes; }
                            }
                            // register with master
                            if (!descriptors.empty()) master->RegisterPDODescriptors(s2.position, descriptors);
                        }
                    }
                }
            }

            // Ensure PDO area visible after building
            pdoDisplay.Show(true);
            // Apply statusword gating to PDO controls via centralized helper so
            // newly created controls or later rebuilds won't bypass the gating logic.
            if (confirmedSlave > 0 && isMasterInitialized) {
                ApplyStatuswordDisableToPDOControls(confirmedSlave);
            }
            UpdateSelectedSlaveStatus(pos);
        }
    }
    // Update thread-safe JSON cache for /slaves endpoint
    try {
        std::ostringstream ss;
        // Helper: sanitize narrow string: trim, remove embedded control chars, and limit length
        auto Sanitize = [](const std::string &in)->std::string {
            // trim
            size_t s = 0; while (s < in.size() && isspace((unsigned char)in[s])) ++s;
            size_t e = in.size(); while (e > s && isspace((unsigned char)in[e-1])) --e;
            std::string tmp = (s==0 && e==in.size()) ? in : in.substr(s, e-s);
            // remove control chars except tab(0x09) and space
            std::string out; out.reserve(tmp.size());
            for (unsigned char c : tmp) {
                if (c >= 0x20 || c == 0x09) out.push_back((char)c);
            }
            // truncate to reasonable max length
            const size_t MAXN = 128;
            if (out.size() > MAXN) out.resize(MAXN);
            return out;
        };

        // Helper: JSON-escape a narrow string (escape control chars, quote, backslash)
        auto JsonEscape = [](const std::string &in)->std::string {
            std::ostringstream out;
            for (unsigned char c : in) {
                switch (c) {
                    case '"': out << "\\\""; break;
                    case '\\': out << "\\\\"; break;
                    case '\b': out << "\\b"; break;
                    case '\f': out << "\\f"; break;
                    case '\n': out << "\\n"; break;
                    case '\r': out << "\\r"; break;
                    case '\t': out << "\\t"; break;
                    default:
                            if (c < 0x20) {
                            char buf[8]; sprintf_s(buf, "\\u%04X", (unsigned)c);
                            out << buf;
                        } else {
                            out << c;
                        }
                }
            }
            return out.str();
        };
        ss << "[";
        bool first = true;
        for (const auto &s : slaves) {
            if (!first) ss << ",";
            first = false;
            // 名稱 — 嘗試讀取 0x1008 設備名稱 (與 PC UI 一致)
            std::string nameForWeb = s.name;  // fallback: SOEM name
            if (isMasterRunning) {
                char deviceName[128] = {0};
                int nameSize = sizeof(deviceName);
                if (master->ReadSDO(s.position, 0x1008, 0x00, deviceName, nameSize)) {
                    nameForWeb = std::string(deviceName);
                }
            }
            std::string nameSan = Sanitize(nameForWeb);
            std::string ipSan = Sanitize(s.ip);
            std::string nameEsc = JsonEscape(nameSan);
            std::string ipEsc = JsonEscape(ipSan);
            const int errorIndex = s.position - 1;
            const bool errorCodeValid = errorIndex >= 0 && errorIndex < 6 && errorCodeValid603F[errorIndex];
            const uint16_t errorCode603F = (errorIndex >= 0 && errorIndex < 6) ? errorCodes603F[errorIndex] : 0;
            // include revision, ip and I/O summary fields for frontend to mirror Win32 UI
            ss << "{\"position\":" << s.position << ",\"name\":\"" << nameEsc << "\",\"errorCode603F\":";
            if (errorCodeValid) {
                ss << errorCode603F;
            } else {
                ss << "null";
            }
            ss << ",\"errorCode603FValid\":" << (errorCodeValid ? "true" : "false")
               << ",\"vendorId\":" << s.vendorId << ",\"productCode\":" << s.productCode << ",\"revision\":" << s.revision << ",\"ip\":\"" << ipEsc << "\",\"state\":" << (int)s.state << ",\"outputs\":" << s.outputBits << ",\"inputs\":" << s.inputBits << "}";
        }
        ss << "]";
    // publish cache via HttpServer helper
    SetSlaveCacheJson(ss.str());
    } catch(...) {
        SetSlaveCacheJson("");
    }
}

void MainWindow::UpdateStatusText() {
    // If child status window is visible, scroll it to top (newest first)
    if (hChildStatusEdit && IsWindow(hChildStatusEdit) && hChildStatusWnd && IsWindowVisible(hChildStatusWnd)) {
        // place caret at start so newest message is visible
        SendMessageW(hChildStatusEdit, EM_SETSEL, 0, 0);
    }
}

void MainWindow::UpdateSelectedSlaveStatus(int slaveIdx) {
    auto* master = AppController::GetInstance().GetMaster();
    if (!master || !IsMasterInitialized()) return;
    
    // 獲取從站信息
    auto slaves = master->GetSlaveInfo();
    if (slaveIdx < 1 || slaveIdx > (int)slaves.size()) return;
    
    const auto& slave = slaves[slaveIdx - 1];
    
    // 嘗試讀取設備名稱 (0x1008:00 - Device Name)
    char deviceName[128] = {0};
    std::wstring deviceNameW;
    int nameSize = sizeof(deviceName);
    
        if (master->ReadSDO(slaveIdx, 0x1008, 0x00, deviceName, nameSize)) {
        // 轉換為寬字元
    deviceNameW = CoreHelpers::Utf8ToWide(std::string(deviceName));
    }
    
    // 如果沒有讀到設備名稱，使用預設名稱
    if (deviceNameW.empty()) {
        deviceNameW = std::wstring(slave.name.begin(), slave.name.end());
    }
    
    // Status strings are not output to the UI
    
    // Do not print detailed slave summary to status window here; only track lastReportedSelectedSlave.
    lastReportedSelectedSlave = slaveIdx;

    // PDO 更新現在由 OnTimer 自動處理,這裡只需確保顯示區域可見
    if (master && master->IsRunning()) {
        pdoDisplay.Show(true); // ensure PDO area is visible when a slave is selected
    }
}

// Helper: check selected slave 0x6041 statusword bit3 and enable/disable PDO edits/buttons
void MainWindow::ApplyStatuswordDisableToPDOControls(int slaveIndex) {
    auto* master = AppController::GetInstance().GetMaster();
    if (slaveIndex <= 0 || !master || !IsMasterInitialized()) return;
    bool baseEnableMotion_local = (slaveIndex > 0) && IsMasterInitialized();
    bool motionAllowed_local = baseEnableMotion_local;
    const bool autoUiLockActive = IsAutoProcedureUiLockActive();
    bool statuswordFault_local = false;
    bool found6041 = false;

    if (baseEnableMotion_local && master) {
        int outBase = 0, inBase = 0;
        if (master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
            // Try to find explicit 0x6041 mapping first
            for (const auto &itm : pdoDisplay.pdoItems) {
                if ((int)itm.index == 0x6041) {
                    found6041 = true;
                    int absOff = inBase + itm.byteOffset;
                    uint8_t swBuf[2] = {0};
                    int copied = master->SnapshotIOMap(absOff, swBuf, 2);
                    if (copied >= 2) {
                        uint16_t sw = (uint16_t)swBuf[0] | ((uint16_t)swBuf[1] << 8);
                        if (sw & (1u << 3)) {
                            statuswordFault_local = true;
                        }
                    }
                    break; // found mapping for 0x6041
                }
            }
            // Fallback: if no explicit 0x6041 mapping found, attempt to read first two input bytes
            if (!found6041) {
                uint8_t fb[2] = {0};
                int copied = master->SnapshotIOMap(inBase, fb, 2);
                if (copied >= 2) {
                    uint16_t sw = (uint16_t)fb[0] | ((uint16_t)fb[1] << 8);
                    // if bit3 set -> fault
                    if (sw & (1u << 3)) statuswordFault_local = true;
                }
            }
        }
    }

    if (statuswordFault_local || autoUiLockActive) motionAllowed_local = false;

    // Use PDOProxy helpers to consistently enable/disable all item controls
    if (!motionAllowed_local) {
        pdoDisplay.DisableAllItems();
    } else {
        pdoDisplay.EnableAllItems();
    }

    const bool faultResetOnlyMode = IsManualResetRequired() ||
                                    this->anySlaveHasInternalError.load() ||
                                    master->HasGlobalError();
    const bool faultResetAllowed = baseEnableMotion_local &&
                                   !faultResetBusy.load() &&
                                   !immediateStopBusy.load() &&
                                   !powerOpBusy.load() &&
                                   (!autoUiLockActive || faultResetOnlyMode);
    // Ensure FaultReset remains available after a safety stop even if Auto UI lock remains active.
    if (hBtnFaultReset) SetControlEnabled(hBtnFaultReset, faultResetAllowed ? true : false);
    ApplyAutoProcedureUiLockState();
}

// Helper: Update window text without triggering full redraw
// Uses WM_SETTEXT with WM_SETREDRAW to completely suppress repaints
inline void SetTextNoRedraw(HWND hwnd, const wchar_t* text) {
    if (!hwnd || !IsWindow(hwnd)) return;
    // Get current text to avoid redundant updates
    wchar_t currentText[512] = {0};
    GetWindowTextW(hwnd, currentText, 512);
    if (wcscmp(currentText, text) == 0) return; // No change, skip update
    
    // Temporarily disable redraw, update text, then re-enable without forcing redraw
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    SendMessageW(hwnd, WM_SETTEXT, 0, (LPARAM)text);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    // Note: We don't call InvalidateRect here, letting the control decide when to repaint
}

void MainWindow::PrependStatus(const wchar_t* text) {
    if (!text) return;
    bool writeToFile = this->statusLogEnabled_.load();
    std::wstring message(text);
    if (IsSimulationModeSelectedForMessages()) {
        message = AddSimulationPrefixToStatusText(message);
    }
    statusLogger.Prepend(message, hChildStatusEdit, hChildStatusWnd, writeToFile, statusUserScrolled);
}

bool MainWindow::IsSimulationModeSelectedForMessages() const
{
    auto* master = AppController::GetInstance().GetMaster();
    if (master && master->IsInitialized()) {
        return master->IsSimulationMode();
    }

    if (EtherCATMaster::IsSimulationAdapterSelection(currentIfName) ||
        EtherCATMaster::IsSimulationAdapterSelection(webSelectedIfName)) {
        return true;
    }

    return master && master->IsSimulationMode();
}

void MainWindow::SetStatusLogEnabled(bool enabled)
{
    // persist selection and update runtime flag
    WriteIniBool(L"Logging", L"EnableStatusLog", enabled);
    statusLogEnabled_.store(enabled);
}

bool MainWindow::IsStatusLogEnabled() const {
    return statusLogEnabled_.load();
}

// UI-only status prepend: same behavior as PrependStatus for the UI control
// but does not append to on-disk status.log. Useful for frequent messages
// (like selected-slave summary) that would otherwise bloat the log file.
void MainWindow::PrependStatusNoFile(const wchar_t* text)
{
    if (!text) return;
    std::wstring message(text);
    if (IsSimulationModeSelectedForMessages()) {
        message = AddSimulationPrefixToStatusText(message);
    }
    statusLogger.PrependNoFile(message, hChildStatusEdit, hChildStatusWnd);
}

// Validate a user's input for SDO Value column according to ESI dataType/bitlen
void MainWindow::ToggleAutoWindow()
{
    auto syncAutoWindowProcedureState = [this]() {
        if (!hAutoWindow || !IsWindow(hAutoWindow)) {
            return;
        }

        SendMessageW(hAutoWindow,
                     IsAutoProcedureUiLockActive() ? WM_USER_AUTO_ENABLE_ALL : WM_USER_AUTO_DISABLE_ALL,
                     0,
                     0);
        PostMessageW(hAutoWindow, WM_USER_AUTO_STATUS_UPDATE, 0, 0);
    };

    // If no auto window exists, create one. Otherwise destroy it.
    if (!hAutoWindow || !IsWindow(hAutoWindow)) {
        // Previously we blocked creation unless connected and PDO running. Change behavior:
        // Always allow the Auto Window to be created from the main UI. If the system
        // is not connected or reports busy state, keep the informational status message
        // but do not prevent the window from opening.
        if (!MasterIsRunning()) {
            PrependStatus(L"[自動視窗] 尚未連線或主迴圈未啟動；已開啟自動視窗，但某些功能可能不可用。\r\n");
        } else {
            // Still report busy state as a hint but don't block the UI.
            const auto snapshot = GetSystemStateSnapshot();
            if (snapshot.busy) {
                PrependStatus(L"[自動視窗] 系統目前忙碌；自動視窗已開啟，但部分控制項可能受限。\r\n");
            }
        }
        HWND h = CreateAutoWindow(hwnd);
        if (h) {
            hAutoWindow = h;
            syncAutoWindowProcedureState();
        }
    } else if (IsIconic(hAutoWindow) || !IsWindowVisible(hAutoWindow)) {
        ShowWindow(hAutoWindow, IsIconic(hAutoWindow) ? SW_RESTORE : SW_SHOW);
        SetWindowPos(hAutoWindow,
                     HWND_TOPMOST,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(hAutoWindow);
        syncAutoWindowProcedureState();
    } else {
        DestroyAutoWindow(hAutoWindow);
        hAutoWindow = NULL;
    }
}

void MainWindow::PostAutoStatusUpdate() {
    // Notify the modeless AutoWindow (if present) to refresh its status lamps
    // Safe to call from worker/background threads because PostMessageW is thread-safe.
    if (hAutoWindow && IsWindow(hAutoWindow)) {
        PostMessageW(hAutoWindow, WM_USER_AUTO_STATUS_UPDATE, 0, 0);
    }
    AppController::GetInstance().PublishStatusEvent();
}

std::string MainWindow::BuildEthercatErrorReasonSummary() {
    {
        std::ostringstream oss;
        int extraReported = 0;
        bool foundReported = false;
        std::lock_guard<std::mutex> lock(errorCodeStateMutex);
        for (const auto& item : lastReportedErrorCode) {
            const int slaveIndex = item.first;
            const uint16_t errorCode = item.second;
            if (errorCode == 0) continue;
            if (foundReported) {
                ++extraReported;
                continue;
            }

            foundReported = true;
            if (errorCode == kErrorCodeCommunicationFailureSentinel) {
                int count = 0;
                const auto counterIt = errorCodeConsecutiveCounter.find(slaveIndex);
                if (counterIt != errorCodeConsecutiveCounter.end()) {
                    count = counterIt->second;
                }
                oss << "Slave " << slaveIndex << " 讀取 0x603F 失敗"
                    << " (count=" << count << "/" << errorCodeConsecutiveThreshold << ")";
            } else {
                oss << "Slave " << slaveIndex << " 0x603F=" << FormatHex16(errorCode);
            }
        }
        if (foundReported) {
            if (extraReported > 0) {
                oss << "; plus " << extraReported << " more slave(s)";
            }
            return oss.str();
        }
    }

    if (anySlaveHasError.load()) {
        std::ostringstream oss;
        const int count = globalErrorCounter.load();
        const int masterThreshold = MasterGetNetworkFailureThreshold();
        const int effectiveThreshold = (masterThreshold > 0)
            ? (std::max)(masterThreshold, globalErrorThreshold)
            : globalErrorThreshold;
        oss << "persistent communication error"
            << " (globalErrorCounter=" << count << "/" << effectiveThreshold << ")";
        return oss.str();
    }

    if (auto* master = AppController::GetInstance().GetMaster()) {
        const int slaveCount = master->GetSlaveCount();
        for (int slaveIndex = 1; slaveIndex <= slaveCount; ++slaveIndex) {
            PDOFilter::ViolationType violationType = PDOFilter::ViolationType::None;
            bool filterError = false;
            bool trackingFatal = false;
            if (!master->GetPDOFilterState(slaveIndex, violationType, filterError, trackingFatal)) {
                continue;
            }
            if (!filterError && !trackingFatal && violationType == PDOFilter::ViolationType::None) {
                continue;
            }

            std::ostringstream oss;
            oss << "Slave " << slaveIndex << " PDO ";
            AppendPdoViolationName(oss, violationType);

            double velocityThreshold = 0.0;
            double accelPosThreshold = 0.0;
            double accelNegThreshold = 0.0;
            double trackingErrorLimit = 0.0;
            double minPos = 0.0;
            double maxPos = 0.0;
            const bool gotThresholds = master->GetPDOFilterViolationThresholds(
                slaveIndex, velocityThreshold, accelPosThreshold, accelNegThreshold);
            const bool gotTrackingLimit = master->GetPDOFilterTrackingErrorLimit(slaveIndex, trackingErrorLimit);
            const bool gotLimits = master->GetPDOLimits(slaveIndex, minPos, maxPos);

            oss << std::fixed << std::setprecision(2);
            if (PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::TrackingError) && gotTrackingLimit) {
                oss << " (threshold=" << trackingErrorLimit << " deg)";
            } else if (PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::Velocity) && gotThresholds) {
                oss << " (threshold=" << velocityThreshold << " deg/s)";
            } else if (PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::Acceleration) && gotThresholds) {
                oss << " (threshold=" << accelPosThreshold << " deg/s^2)";
            } else if (PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::Deceleration) && gotThresholds) {
                oss << " (threshold=" << accelNegThreshold << " deg/s^2)";
            } else if ((PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::HardLimitMin) ||
                        PDOFilter::HasFlag(violationType, PDOFilter::ViolationType::HardLimitMax)) && gotLimits) {
                oss << " (limit=" << minPos << ".." << maxPos << " deg)";
            }
            if (trackingFatal) {
                oss << "; fatal tracking timeout";
            }
            return oss.str();
        }

        if (master->HasGlobalError()) {
            const std::string lastError = master->GetLastError();
            if (!lastError.empty()) {
                return std::string("master global error latch active; lastError=") + lastError;
            }
            return "master global error latch active";
        }
    }

    if (anySlaveHasInternalError.load()) {
        return "0x603F internal-error latch active; waiting for next error-code poll";
    }

    if (IsExternalImmediateStopActive()) {
        return "external immediate-stop input active (0x60FD bit0)";
    }
    if (IsExternalEStopActive()) {
        return "external emergency-stop input active (0x60FD bit5)";
    }
    if (IsManualResetRequired()) {
        return "manual reset required after safety stop";
    }

    return "unknown; check 0x603F/PDOFilter/watchdog logs";
}

void MainWindow::NotifyEthercatError(bool active, const std::string& reasonOverride) {
    const bool wasActive = ethercatError.exchange(active);
    // Do NOT call PostAutoStatusUpdate() to avoid triggering AutoWindow updates
    // which could cause the main window to show/update unexpectedly
    // Play/stop a continuous system alert for ethercat errors
    if (active) {
        // Start error sound if not already playing. If another component (soft-start)
        // is already playing the same system sound, don't restart it.
        if (!this->errorSoundPlaying_.load()) {
            if (!this->softStartSoundPlaying_.load() && !this->violationSoundPlaying_.load()) {
                try { PlaySound(TEXT("SystemAsterisk"), NULL, SND_ALIAS | SND_ASYNC | SND_LOOP); } catch(...) {}
            }
            this->errorSoundPlaying_.store(true);
            const std::string reason = reasonOverride.empty()
                ? BuildEthercatErrorReasonSummary()
                : reasonOverride;
            PostUTF8LogToUI(std::string("[ERROR] EtherCAT error active - alert sound playing; reason: ") +
                            reason + "\n");
        }
    } else {
        // Clear error sound flag and stop system sound only when idle
        const bool stopInputActive = IsExternalImmediateStopActive() || IsExternalEStopActive() || IsManualResetRequired();
        if (!stopInputActive) {
            this->errorSoundPlaying_.store(false);
            StopSystemAlertSoundIfIdle();
        }
        if (wasActive) {
            PostUTF8LogToUI(std::string("[ERROR] EtherCAT error cleared\n"));
        }
    }
}

void MainWindow::CaptureCurrentPDOInputsByName()
{
    for (const auto &it : pdoDisplay.pdoItems) {
        if (!it.isEditable) continue;
        std::wstring name = it.name;
        std::wstring val;
        int ctrlId = (it.hEdit ? GetDlgCtrlID(it.hEdit) : -1);
        auto itc = pdoEditTextCache.find(ctrlId);
        if (itc != pdoEditTextCache.end()) {
            val = itc->second;
        } else if (it.hEdit && IsWindow(it.hEdit)) {
            wchar_t buf[64] = {0};
            GetWindowTextW(it.hEdit, buf, (int)_countof(buf));
            val = buf;
        } else {
            val = L"0";
        }
        globalPDOInputByName[name] = val;
    }
}

void MainWindow::RestorePDOInputsFromGlobal()
{
    for (auto &it : pdoDisplay.pdoItems) {
        if (!it.isEditable) continue;
        auto f = globalPDOInputByName.find(it.name);
        if (f != globalPDOInputByName.end()) {
            const std::wstring &val = f->second;
            if (it.hEdit && IsWindow(it.hEdit)) {
                SetWindowTextW(it.hEdit, val.c_str());
                int id = it.hEdit ? GetDlgCtrlID(it.hEdit) : -1;
                if (id >= 5000 && id < 6100) {
                    pdoEditTextCache[id] = val;
                }
            }
        }
    }
}
// END: PDODisplay.cpp merged content

// =====================================================================
// Fault Reset button subclass and handler
// =====================================================================

// Subclass procedure to capture press/release on the fault reset button (UI-only)
