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
void MainWindow::OnDumpESI(uint16_t slave)
{
    if (!IsMasterInitialized() || slave == 0) {
        PrependStatus(L"尚未連線或從站未選擇\r\n");
        return;
    }
    std::string dump;
    if (!MasterDumpESI(slave, dump)) {
        std::string err = MasterGetLastError();
        std::wstring errW = CoreHelpers::Utf8ToWide(err);
        if (!errW.empty()) { PrependStatus(errW.c_str()); PrependStatus(L"\r\n"); }
        return;
    }
    const size_t CHUNK = 800;
    for (size_t pos = 0; pos < dump.size(); pos += CHUNK) {
        std::string part = dump.substr(pos, CHUNK);
        std::wstring wpart = CoreHelpers::Utf8ToWide(part);
        PrependStatus(wpart.c_str());
    }
}

// -----------------------------------------------------------------------------
// 0x603F Error Code 循環檢查機制 (每50ms檢查一個從站,避免阻塞主執行緒)
// -----------------------------------------------------------------------------

void MainWindow::PopulateObjectsFromESI(const std::wstring& esiPathW)
{
    if (esiPathW.empty()) {
        // No ESI selected: keep Import Params enabled so operator can still open files
        return;
    }

    // wide -> utf8
    std::string pathUtf8 = CoreHelpers::WideToUtf8(esiPathW);

    std::string err;
    currentObjects.clear();
    currentPdos.clear();
    if (!EsiParser::Load(pathUtf8, currentObjects, currentPdos, err)) {
    std::wstring errW(err.begin(), err.end());
    PrependStatus(errW.c_str());
    PrependStatus(L"\r\n");
        // Parsing failed but keep Import Params available for retry
        return;
    }

    // Fill SDO parameter ListView
    if (hListSDOParams) {
        ListView_DeleteAllItems(hListSDOParams);
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = (int)i;
            // Index column
            wchar_t idxbuf[32]; swprintf_s(idxbuf, L"0x%04X", currentObjects[i].index);
            item.pszText = idxbuf;
            item.lParam = (LPARAM)i;
            ListView_InsertItem(hListSDOParams, &item);
            // Sub
            wchar_t subbuf[16]; swprintf_s(subbuf, L"0x%02X", currentObjects[i].subindex);
            ListView_SetItemText(hListSDOParams, (int)i, 1, subbuf);
            // Name
            std::wstring nameW = CoreHelpers::Utf8ToWide(currentObjects[i].name);
            ListView_SetItemText(hListSDOParams, (int)i, 2, const_cast<LPWSTR>(nameW.c_str()));
            // Access (read/write/ro/wo) now at column 4 - prefer ESI-provided access string
            std::wstring accessW;
            if (!currentObjects[i].access.empty()) {
                // Show the original token from ESI as-is (do not change case)
                accessW = CoreHelpers::Utf8ToWide(currentObjects[i].access);
            } else if (!currentObjects[i].accessNormalized.empty()) {
                // Fallback to normalized token if the original access string was not present
                accessW = CoreHelpers::Utf8ToWide(currentObjects[i].accessNormalized);
            } else {
                // Default to 'ro' (lower-case) when absent
                accessW = L"ro";
            }
            ListView_SetItemText(hListSDOParams, (int)i, 4, const_cast<LPWSTR>(accessW.c_str()));

            // DataType (new column at index 3): prefer explicit dataType text from ESI, else show bitlen
            std::wstring esiTypeW;
            if (!currentObjects[i].dataType.empty()) {
                esiTypeW = std::wstring(currentObjects[i].dataType.begin(), currentObjects[i].dataType.end());
            } else if (currentObjects[i].bitlen > 0) {
                wchar_t esiTypeBuf[64] = {0};
                swprintf_s(esiTypeBuf, L"%d-bit", currentObjects[i].bitlen);
                esiTypeW = esiTypeBuf;
            } else {
                esiTypeW.clear();
            }
            ListView_SetItemText(hListSDOParams, (int)i, 3, const_cast<LPWSTR>(esiTypeW.c_str()));

            // Value placeholder now at column 5, Hex at column 6
            // Do NOT prefill with ESI defaultValue. Leave blank unless a saved value exists.
            ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(L""));
            ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(L""));
            // If we have cached values for the currently confirmed slave, restore them
            if (confirmedSlave > 0) {
                auto it = savedSDOValues.find(confirmedSlave);
                if (it != savedSDOValues.end()) {
                    const std::vector<std::wstring>& vals = it->second;
                    if ((int)vals.size() > (int)i) {
                        // Restore even empty string so cleared values are preserved
                        ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(vals[i].c_str()));
                    }
                }
            }
        }
        // Select first item if any
        if (!currentObjects.empty()) {
            ListView_SetItemState(hListSDOParams, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            // Enable SDO buttons only when both an ESI is loaded and we're connected
            bool isConnected_local = IsMasterInitialized();
            bool hasESI_local = !currentObjects.empty();
            SetControlEnabled(hBtnReadSDO, (hasESI_local && isConnected_local) ? true : false);
            SetControlEnabled(hBtnWriteSDO, (hasESI_local && isConnected_local) ? true : false);
            SetControlEnabled(hBtnReadAllSDO, (hasESI_local && isConnected_local) ? true : false);
            // Ensure Import Params button is enabled after ESI import if we have ESI
            if (hBtnImportParams) SetControlEnabled(hBtnImportParams, true);
            // Enable Save Params only when an ESI is loaded (hasESI) and a slave is selected
            if (hBtnSaveParams && IsWindow(hBtnSaveParams)) {
                SetControlEnabled(hBtnSaveParams, true);
            }
        } else {
            SetControlEnabled(hBtnReadSDO, false);
            SetControlEnabled(hBtnWriteSDO, false);
            SetControlEnabled(hBtnReadAllSDO, false);
        }
    }

    
}

// Toggle the modeless Auto Window (create if not present, destroy if present)

void MainWindow::OnBrowseESI()
{
    // Open file dialog
    // Inform HTTP server that browse is in progress so web UI can disable the button
    SetUiDisabled("browse_esi", true);
    OPENFILENAMEW ofn;
    wchar_t szFile[MAX_PATH] = {0};
    wchar_t szInitialDir[MAX_PATH] = {0};
    
    // Get the directory of the executable and build the path to \esi
    wchar_t szExePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, szExePath, MAX_PATH);
    
    // Remove filename to get directory
    wchar_t* pLastBackslash = wcsrchr(szExePath, L'\\');
    if (pLastBackslash) {
        *pLastBackslash = L'\0';
    }
    
    // Construct the esi directory path
    swprintf_s(szInitialDir, MAX_PATH, L"%s\\esi", szExePath);
    
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = szInitialDir;
    ofn.lpstrFilter = L"ESI Files\0*.xml\0All Files\0*.*\0";
    ofn.nFilterIndex = 1; // Default to ESI Files filter
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        
        PopulateObjectsFromESI(szFile);
    }
    // Dialog closed (selection or cancel).
    // Clear server-side busy flag so web UI can re-enable the browse button immediately
    SetUiDisabled("browse_esi", false);
    // Do NOT enable Import Params here unconditionally; PopulateObjectsFromESI()
    // will enable it only if the ESI file was successfully loaded.
}


void MainWindow::OnDumpESIForSelectedSlave()
{
    if (!IsMasterInitialized()) return;
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) { PrependStatus(L"請先選擇一個從站\r\n"); return; }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));
    // Delegate to logic-layer implementation
    OnDumpESI(slave);
}

// WideToUtf8 and EscapeXml moved to Win32Utils.cpp/h

// moved to MainWindow.SDO.cpp: MainWindow::OnSaveParams()

// moved to MainWindow.SDO.cpp: MainWindow::OnImportParams()

// BEGIN: PDODisplay.cpp merged content
// PDO display implementation moved to src/PDOProxy.cpp (renamed from PDODisplayManager)
// See src/PDOProxy.cpp for the full implementation. For other
// translation units prefer the `PDOProxy` header (include/PDOProxy.h)
// to reference the display manager type without depending on the full definition.
// Small MainWindow helpers that reference pdoDisplay live here


