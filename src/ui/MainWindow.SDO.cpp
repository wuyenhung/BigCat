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
void MainWindow::OnReadSDO() {
    if (!IsMasterInitialized()) return;
    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Get selected parameter from ListView
    if (!hListSDOParams) {
        PrependStatus(L"SDO 參數表未建立\r\n");
        return;
    }
    int selItem = ListView_GetNextItem(hListSDOParams, -1, LVNI_SELECTED);
    if (selItem < 0 || selItem >= (int)currentObjects.size()) {
        PrependStatus(L"請先在參數表中選擇一個參數\r\n");
        return;
    }
    uint16_t index = currentObjects[selItem].index;
    uint8_t subindex = currentObjects[selItem].subindex;

    // Prepare textual index/subindex for debug output
    wchar_t indexW[32] = {0};
    wchar_t subindexW[32] = {0};
    swprintf_s(indexW, L"0x%04X", index);
    swprintf_s(subindexW, L"0x%02X", subindex);

    // Read SDO
    uint8_t buf[256] = {0};
    int size = sizeof(buf);
    if (MasterReadSDO(slave, index, subindex, buf, size)) {
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 讀取成功 - Index: 0x"
           << std::hex << std::uppercase << index
           << L" Subindex: 0x" << (int)subindex
           << std::dec << L" 長度: " << size << L" bytes\r\n";
        // 顯示位元組（LE 與 BE 表示）
        ss << L"  LE 位元組: " << CoreHelpers::BytesToHexDisplay(buf, size, false, true, true, true) << L"\r\n";
        ss << L"  BE 位元組: " << CoreHelpers::BytesToHexDisplay(buf, size, true, true, true, true) << L"\r\n";
        // 顯示整數（LE 解析）
        ss << L"  整數(LE 解析): ";
        // 以 64-bit 為主輸出（若 size>8，僅截取低 64 位）
        unsigned long long v64 = 0;
        int maxBytes = (size < 8 ? size : 8);
        for (int i = 0; i < maxBytes; ++i) v64 |= (unsigned long long)buf[i] << (8 * i);
        ss << std::dec << v64 << L" (0x" << std::hex << std::uppercase << v64 << L")\r\n";
        PrependStatus(ss.str().c_str());

        // Format display and hex using centralized helper to keep UI thin
        {
            const EsiObjectEntry* pobj = nullptr;
            if (selItem >= 0 && selItem < (int)currentObjects.size()) pobj = &currentObjects[selItem];
            auto formatted = FormatSDOValue(buf, size, pobj);
            const std::wstring &valueDisplay = formatted.first;
            const std::wstring &hexDisplay = formatted.second;

            // Update ListView columns
            if (hListSDOParams && IsWindow(hListSDOParams)) {
                ListView_SetItemText(hListSDOParams, selItem, 5, const_cast<LPWSTR>(valueDisplay.c_str()));
                ListView_SetItemText(hListSDOParams, selItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
            }

            // Update caches
            {
                auto &vec = savedSDOValues[slave];
                if ((int)vec.size() < (int)currentObjects.size()) vec.resize(currentObjects.size());
                vec[selItem] = valueDisplay;
                auto &hexvec = savedSDOHexValues[slave];
                if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                hexvec[selItem] = hexDisplay;
            }
        }
    } else {
    std::string error = MasterGetLastError();
        std::wstring errorW(error.begin(), error.end());
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 讀取失敗 - " << errorW << L"\r\n";
        PrependStatus(ss.str().c_str());
    }
}


void MainWindow::OnReadAllSDO()
{
    if (!IsMasterInitialized()) return;
    if (readAllRunning.load()) {
        PrependStatus(L"全部讀取已在執行中，請稍候...");
        return;
    }
    // Ensure a slave is selected
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) { PrependStatus(L"請先選擇一個從站\r\n"); return; }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Mark running and disable button to avoid re-entry
    readAllRunning.store(true);
    SetControlEnabled(hBtnReadAllSDO, false);

    // Clear UI value column immediately to reflect fresh read-all operation
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        for (int ii = 0; ii < (int)currentObjects.size(); ++ii) {
            ListView_SetItemText(hListSDOParams, ii, 5, const_cast<LPWSTR>(L""));
            ListView_SetItemText(hListSDOParams, ii, 6, const_cast<LPWSTR>(L""));
        }
    }

    
    // Clear any cached saved values for this slave so previous values don't reappear
    savedSDOValues[slave].clear();

    // Disable most UI controls to avoid concurrent commands while read-all is running
    if (hBtnReadSDO) SetControlEnabled(hBtnReadSDO, false);
    if (hBtnWriteSDO) SetControlEnabled(hBtnWriteSDO, false);
    // Also disable the "全部寫入" button while a read-all operation is in progress
    if (hBtnWriteAllSDO) SetControlEnabled(hBtnWriteAllSDO, false);
    // Temporarily disable Import/Save Params while read-all is running
    if (hBtnImportParams) SetControlEnabled(hBtnImportParams, false);
    if (hBtnSaveParams) SetControlEnabled(hBtnSaveParams, false);
    if (hBtnSendMailbox) SetControlEnabled(hBtnSendMailbox, false);
    if (hBtnBrowseESI) SetControlEnabled(hBtnBrowseESI, false);
    if (hBtnClearStatus) {
        HWND btn = (GetParent(hBtnClearStatus) == hContentWnd) ? hBtnClearStatus : (hChildStatusWnd ? GetDlgItem(hChildStatusWnd, 1032) : NULL);
        if (btn) SetControlEnabled(btn, false);
    }
    if (hBtnConnect) SetControlEnabled(hBtnConnect, false);
    if (hBtnDisconnect) SetControlEnabled(hBtnDisconnect, false);
    if (hButtonPDOStart) SetControlEnabled(hButtonPDOStart, false);
    if (hButtonPDOStop) SetControlEnabled(hButtonPDOStop, false);
    if (hBtnPDOSentStatus) SetControlEnabled(hBtnPDOSentStatus, false);
    // Disable motion control buttons while read-all is running
    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, false);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, false);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, false);
    if (hBtnHome) SetControlEnabled(hBtnHome, false);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, false);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, false);
    if (hBtnFaultReset) {
    if (!IsMasterInitialized() || disconnecting.load()) {
            SetControlEnabled(hBtnFaultReset, false);
        }
    }
    if (hBtnImmediateStop) SetControlEnabled(hBtnImmediateStop, false);
    if (hBtnStopAutoMode) SetControlEnabled(hBtnStopAutoMode, false);
    if (hButtonStartAutoMode) SetControlEnabled(hButtonStartAutoMode, false);
    // Also disable ESI and slave combos to prevent changing context mid-read
    if (hComboSlaves) SetControlEnabled(hComboSlaves, false);

    // Launch background reader thread
    std::thread([this, slave]() {
        // Lower thread priority so it doesn't preempt real-time PDO threads
        int prevPrio = GetThreadPriority(GetCurrentThread());
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

        // Batch messages to the UI to reduce PostMessage overhead.
        std::string batch;
        int batchCount = 0;
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if (!readAllRunning.load() || disconnecting.load()) break;
            uint16_t idx = currentObjects[i].index;
            uint8_t sub = currentObjects[i].subindex;
            uint8_t buf[256] = {0}; int size = sizeof(buf);
            bool ok = MasterReadSDO((int)slave, idx, sub, buf, size);
            if (ok) {
                // format BE hex string (human friendly)
                std::ostringstream oss;
                oss << std::hex << std::uppercase;
                for (int b = size - 1; b >= 0; --b) {
                    oss << "0x" << std::setw(2) << std::setfill('0') << (int)buf[b];
                    if (b > 0) oss << ' ';
                }
                std::string s = std::string("ITEM:") + std::to_string(slave) + ":" + std::to_string(idx) + ":" + std::to_string(sub) + ":" + oss.str();
                if (!batch.empty()) batch.push_back('|');
                batch += s;
                ++batchCount;
            } else {
                std::string err = MasterGetLastError();
                std::string s = std::string("ITEM_ERR:") + std::to_string(slave) + ":" + std::to_string(idx) + ":" + std::to_string(sub) + ":" + err;
                if (!batch.empty()) batch.push_back('|');
                batch += s;
                ++batchCount;
            }
            // Flush batch when it grows to avoid huge messages or long delays
            if (batchCount >= 32 || batch.size() > 8192) {
                std::string out = std::string("BATCH:") + batch;
                MainWindow::PostSDOItemToHWND(hwnd, std::move(out));
                batch.clear(); batchCount = 0;
            }
            // Small sleep to yield to PDO and other threads; tuned low to keep reasonable speed
            Sleep(10);
        }
        // Flush any remaining batched items
        if (!batch.empty()) {
            std::string out = std::string("BATCH:") + batch;
            MainWindow::PostSDOItemToHWND(hwnd, std::move(out));
            batch.clear(); batchCount = 0;
        }
        // Publish JSON cache for web UI
        try {
            std::string json = "[";
            bool firstItem = true;
            auto itVals = savedSDOValues.find((int)slave);
            auto itHexs = savedSDOHexValues.find((int)slave);
            for (size_t i = 0; i < currentObjects.size(); ++i) {
                const auto &obj = currentObjects[i];
                std::string name = obj.name;
                std::string dtype = obj.dataType;
                int bitlen = obj.bitlen;
                // Use original ESI token for JSON/UI where available; otherwise use normalized token
                std::string access = !obj.access.empty() ? obj.access : obj.accessNormalized;
                std::string val = "";
                std::string hex = "";
                if (itVals != savedSDOValues.end() && i < itVals->second.size()) {
                    val = CoreHelpers::WideToUtf8(itVals->second[i]);
                }
                if (itHexs != savedSDOHexValues.end() && i < itHexs->second.size()) {
                    hex = CoreHelpers::WideToUtf8(itHexs->second[i]);
                }
                if (!firstItem) json += ","; firstItem = false;
                json += "{";
                json += "\"index\":" + std::to_string((int)obj.index) + ",";
                json += "\"subindex\":" + std::to_string((int)obj.subindex) + ",";
                json += "\"name\":\"" + CoreHelpers::EscapeJsonString(name) + "\",";
                json += "\"dataType\":\"" + CoreHelpers::EscapeJsonString(dtype) + "\",";
                json += "\"bitlen\":" + std::to_string(bitlen) + ",";
                json += "\"access\":\"" + CoreHelpers::EscapeJsonString(access) + "\",";
                json += "\"value\":\"" + CoreHelpers::EscapeJsonString(val) + "\",";
                json += "\"hex\":\"" + CoreHelpers::EscapeJsonString(hex) + "\"";
                json += "}";
            }
            json += "]";
            SetSlaveSdoCacheJson((int)slave, json);
        } catch(...) {
            // non-fatal
        }

        SetThreadPriority(GetCurrentThread(), prevPrio);
    PostUTF8LogToUI(std::string("ENABLE_CONTROLS_NOREFLOW\n"));
        readAllRunning.store(false);
        return;
    }).detach();
}


void MainWindow::OnWriteSDO() {
    if (!IsMasterInitialized()) return;

    // 取得選中的從站 (從下拉選單)
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        PrependStatus(L"請先選擇一個從站\r\n");
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // 取得選中的參數（從 SDO 參數表）並使用其 Index/Subindex
    wchar_t indexW[32] = {0}, subindexW[32] = {0}, dataW[256] = {0};
    if (!hListSDOParams) {
        PrependStatus(L"SDO 參數表未建立\r\n");
        return;
    }
    int selItem = ListView_GetNextItem(hListSDOParams, -1, LVNI_SELECTED);
    if (selItem < 0 || selItem >= (int)currentObjects.size()) {
        PrependStatus(L"請先在參數表中選擇一個參數\r\n");
        return;
    }
    uint16_t index = currentObjects[selItem].index;
    uint8_t subindex = currentObjects[selItem].subindex;
    swprintf_s(indexW, L"0x%04X", index);
    swprintf_s(subindexW, L"0x%02X", subindex);
    // Read existing Value column text (if user previously read/edited it)
    ListView_GetItemText(hListSDOParams, selItem, 5, dataW, (int)_countof(dataW));
    // 解析輸入的資料，可接受 0x.. 0x.. 串列或十進制
    uint8_t wbuf[256] = {0};
    int wlen = 0;
    {
        // Basic DataType validation: ensure input roughly matches ESI dataType
        std::string dtype = currentObjects[selItem].dataType;
        std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
        // If BOOL, accept only 0/1 or true/false
        if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
            std::wstring sdata(dataW);
            std::wstring trimmed = sdata;
            // trim
            auto is_space = [](wchar_t c){ return iswspace(c); };
            while (!trimmed.empty() && is_space(trimmed.front())) trimmed.erase(trimmed.begin());
            while (!trimmed.empty() && is_space(trimmed.back())) trimmed.pop_back();
            if (trimmed.empty()) { PrependStatus(L"BOOL 欄位不能為空，請輸入 0 或 1\r\n"); return; }
            if (!(trimmed == L"0" || trimmed == L"1" || _wcsicmp(trimmed.c_str(), L"TRUE") == 0 || _wcsicmp(trimmed.c_str(), L"FALSE") == 0)) {
                PrependStatus(L"輸入錯誤: 布林欄位僅接受 0/1 或 true/false\r\n"); return;
            }
        }
        // 先嘗試以空白分隔的 hex bytes（視為 BE，高位在前），內部轉為 LE 寫入
        std::wstring s(dataW);
        std::wstringstream ss(s);
        std::wstring tok;
        bool anyHex = false;
        std::vector<uint8_t> typedBE;
        while (ss >> tok) {
            if (tok.rfind(L"0x",0)==0 || tok.rfind(L"0X",0)==0) {
                unsigned v = 0; swscanf_s(tok.c_str(), L"0x%x", &v);
                typedBE.push_back((uint8_t)(v & 0xFF));
                anyHex = true;
            }
        }
        if (anyHex) {
            // BE -> LE
            size_t beCount = typedBE.size();
            if (beCount > sizeof(wbuf)) beCount = sizeof(wbuf);
            wlen = (int)beCount;
            for (int i = 0; i < wlen; ++i) {
                wbuf[i] = typedBE[(int)beCount - 1 - i];
            }
        }
        if (!anyHex) {
            // 依據 ESI 的 bitlen 決定位元組數，預設 4 bytes（小端）
            int expectedBytes = 4;
            for (const auto &obj : currentObjects) {
                if (obj.index == index && obj.subindex == subindex && obj.bitlen > 0) {
                    expectedBytes = (obj.bitlen + 7) / 8;
                    break;
                }
            }
            if (expectedBytes < 1) expectedBytes = 1; if (expectedBytes > 8) expectedBytes = 8;
            uint64_t v = (uint64_t)wcstoull(dataW, NULL, 0);
            // Enforce ESI bitlen capacity and truncate if necessary
            uint64_t maxValW = 0ULL;
            if (expectedBytes >= 8) {
                maxValW = 0xFFFFFFFFFFFFFFFFULL;
            } else {
                maxValW = ((1ULL << (8 * expectedBytes)) - 1ULL);
            }
            if (v > maxValW) {
                std::wstringstream warn; warn << L"警告: 輸入值超出 ESI 定義的 " << expectedBytes << L" bytes，將以低位截斷。\r\n";
                PrependStatus(warn.str().c_str());
                v &= maxValW;
            }
            for (int i = 0; i < expectedBytes; ++i) {
                wbuf[i] = (uint8_t)((v >> (8*i)) & 0xFF);
            }
            wlen = expectedBytes;
        }
    }

    // Write SDO
    if (MasterWriteSDO(slave, index, subindex, wbuf, wlen)) {
        // 在狀態欄顯示寫入成功信息
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 寫入成功 - Index: 0x"
           << std::hex << std::uppercase << index
           << L" Subindex: 0x" << (int)subindex
           << L" 長度: " << std::dec << wlen << L" bytes  實際送出(LE): ";
        ss << std::hex << std::uppercase;
        for (int i = 0; i < wlen; ++i) {
            ss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)wbuf[i];
            if (i + 1 < wlen) ss << L" ";
        }
        // 顯示 LE 解析數值
        unsigned long long v64w = 0; for (int i = 0; i < (wlen < 8 ? wlen : 8); ++i) v64w |= (unsigned long long)wbuf[i] << (8*i);
        ss << L"  | 整數(LE 解析)= " << std::dec << v64w << L" (0x" << std::hex << std::uppercase << v64w << L")\r\n";
        PrependStatus(ss.str().c_str());

        // 立刻讀回確認
        Sleep(15);
        uint8_t rbuf[256] = {0};
        int rlen = sizeof(rbuf);
    if (MasterReadSDO(slave, index, subindex, rbuf, rlen)) {
            std::wstringstream rss;
            rss << L"[從站 " << slave << L"] 寫入後讀回 - Index: 0x"
                << std::hex << std::uppercase << index
                << L" Subindex: 0x" << (int)subindex
                << std::dec << L" 長度: " << rlen << L" bytes\r\n";
            // LE bytes
            rss << L"  LE 位元組: ";
            rss << std::hex << std::uppercase;
            for (int i = 0; i < rlen; ++i) {
                rss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                if (i + 1 < rlen) rss << L" ";
            }
            rss << L"\r\n";
            // BE bytes
            rss << L"  BE 位元組: ";
            for (int i = rlen - 1; i >= 0; --i) {
                rss << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                if (i > 0) rss << L" ";
            }
            rss << L"\r\n";
            // 整數解析（LE）
            unsigned long long v64r = 0; for (int i = 0; i < (rlen < 8 ? rlen : 8); ++i) v64r |= (unsigned long long)rbuf[i] << (8*i);
            rss << L"  整數(LE 解析): " << std::dec << v64r << L" (0x" << std::hex << std::uppercase << v64r << L")\r\n";

            // 比對（以 LE 位元組 compare，長度取 min）
            int cmpLen = (wlen < rlen ? wlen : rlen);
            bool eq = (cmpLen > 0 && memcmp(wbuf, rbuf, cmpLen) == 0 && wlen == rlen);
            rss << L"  比對: " << (eq ? L"相同" : L"不同") << L"（以 LE 位元組逐位比較" << (wlen == rlen ? L"、長度相同" : L"、長度不同") << L"）\r\n";
            if (!eq) {
                rss << L"  提示: 若為 0x60FF 等馬達目標值，需確認 0x6060 模式與 0x6040 狀態允許；部分裝置 SDO 回讀不反映 PDO 目標。\r\n";
                // 進一步讀取常見 CiA402 狀態以協助診斷
                auto read_u32 = [&](uint16_t idx, uint8_t sub, unsigned long long &out, int &lenOut) -> bool {
                    uint8_t tmp[8] = {0}; int L = sizeof(tmp);
                    bool ok = MasterReadSDO(slave, idx, sub, tmp, L);
                    if (!ok) return false; lenOut = L; out = 0ULL; int mx = (L < 8 ? L : 8);
                    for (int i = 0; i < mx; ++i) out |= (unsigned long long)tmp[i] << (8*i);
                    return true;
                };
                unsigned long long mo = 0, md = 0, sw = 0, ec = 0, vact = 0; int lmo=0,lmd=0,lsw=0,lec=0,lv=0;
                read_u32(0x6060, 0x00, mo, lmo); // Mode of Operation (commanded)
                read_u32(0x6061, 0x00, md, lmd); // Mode Display (actual)
                read_u32(0x6041, 0x00, sw, lsw); // Statusword
                read_u32(0x603F, 0x00, ec, lec); // Error Code
                read_u32(0x606C, 0x00, vact, lv); // Velocity Actual Value
                rss << L"  診斷: 0x6060(MOO)=" << mo << L" 0x6061(MOD)=" << md
                    << L" 0x6041(SW)=0x" << std::hex << std::uppercase << sw << std::dec
                    << L"  0x603F(ErrorCode)=0x" << std::hex << std::uppercase << ec << std::dec
                    << L"  0x606C(VelAct)=" << vact << L"\r\n";
            }
            PrependStatus(rss.str().c_str());
            // Update caches for Hex only; do NOT overwrite the user's Value input nor savedSDOValues here
            {
                // Build BE hex display from readback buffer (rbuf)
                std::wstringstream s2; s2 << std::hex << std::uppercase;
                for (int i = rlen - 1; i >= 0; --i) {
                    s2 << L"0x" << std::setw(2) << std::setfill(L'0') << (int)rbuf[i];
                    if (i > 0) s2 << L" ";
                }
                std::wstring hexDisplay = s2.str();

                auto &hexvec = savedSDOHexValues[slave];
                if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                hexvec[selItem] = hexDisplay;

                // Update only Hex column in the ListView, keep Value column as user-entered
                if (hListSDOParams && IsWindow(hListSDOParams)) {
                    ListView_SetItemText(hListSDOParams, selItem, 6, const_cast<LPWSTR>(hexDisplay.c_str()));
                }
            }
        } else {
            std::string err = MasterGetLastError();
            std::wstring errW(err.begin(), err.end());
            std::wstringstream rss;
            rss << L"[從站 " << slave << L"] 寫入後讀回失敗 - " << errW << L"\r\n";
            PrependStatus(rss.str().c_str());
        }
    } else {
    std::string error = MasterGetLastError();
        std::wstring errorW(error.begin(), error.end());
        std::wstringstream ss;
        ss << L"[從站 " << slave << L"] 寫入失敗 - " << errorW << L"\r\n";
        PrependStatus(ss.str().c_str());
    }
}


void MainWindow::OnWriteAllSDO()
{
    if (!IsMasterInitialized()) return;
    // Ensure a slave is selected
    LRESULT sel = SendMessageW(hComboSlaves, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        MessageBoxW(hwnd, L"請先選擇一個從站\r\n", L"未選擇從站", MB_OK | MB_ICONWARNING);
        return;
    }
    LRESULT itemData = SendMessageW(hComboSlaves, CB_GETITEMDATA, sel, 0);
    uint16_t slave = (uint16_t)(itemData > 0 ? itemData : (sel + 1));

    // Disable UI controls to avoid concurrent operations
    if (hBtnReadSDO) SetControlEnabled(hBtnReadSDO, false);
    if (hBtnWriteSDO) SetControlEnabled(hBtnWriteSDO, false);
    if (hBtnReadAllSDO) SetControlEnabled(hBtnReadAllSDO, false);
    // Temporarily disable Import/Save Params while write-all is running
    if (hBtnImportParams) SetControlEnabled(hBtnImportParams, false);
    if (hBtnSaveParams) SetControlEnabled(hBtnSaveParams, false);
    if (hBtnWriteAllSDO) SetControlEnabled(hBtnWriteAllSDO, false);
    // Disable ESI browse/import while a full write is in progress
    if (hBtnBrowseESI) SetControlEnabled(hBtnBrowseESI, false);
    // Also disable motion control buttons while performing a full write
    if (hBtnFollowMode) SetControlEnabled(hBtnFollowMode, false);
    if (hBtnPowerOn) SetControlEnabled(hBtnPowerOn, false);
    if (hBtnPowerOff) SetControlEnabled(hBtnPowerOff, false);
    if (hBtnHome) SetControlEnabled(hBtnHome, false);
    if (hBtnJogCW) SetControlEnabled(hBtnJogCW, false);
    if (hBtnJogCCW) SetControlEnabled(hBtnJogCCW, false);
    if (hBtnFaultReset) {
    if (!IsMasterInitialized() || disconnecting.load()) {
            SetControlEnabled(hBtnFaultReset, false);
        }
    }
    if (hBtnImmediateStop) SetControlEnabled(hBtnImmediateStop, false);
    if (hBtnStopAutoMode) SetControlEnabled(hBtnStopAutoMode, false);
    if (hButtonStartAutoMode) SetControlEnabled(hButtonStartAutoMode, false);

    // Disable slave selection combobox to prevent changing target slave mid-operation
    if (hComboSlaves) SetControlEnabled(hComboSlaves, false);

    // Launch background thread to perform writes
    std::thread([this, slave]() {
        try {
            PostUTF8LogToUI(std::string("開始全部寫入 (RW) 參數...\n"));
            for (size_t i = 0; i < currentObjects.size(); ++i) {
                if (!IsMasterInitialized()) break;
                const auto &obj = currentObjects[i];
                // Determine if object is writable (access contains 'w' or 'W')
                const std::string &access =
                    obj.accessNormalized.empty() ? obj.access : obj.accessNormalized;
                if (access.find('w') == std::string::npos &&
                    access.find('W') == std::string::npos) {
                    continue;
                }

                // Prepare buffer to write: prefer savedSDOHexValues if present, otherwise try savedSDOValues
                std::vector<uint8_t> writeLE;
                bool haveData = false;
                auto itHexs = savedSDOHexValues.find(slave);
                if (itHexs != savedSDOHexValues.end() && i < itHexs->second.size() && !itHexs->second[i].empty()) {
                    std::vector<uint8_t> bytesBE; bool any = false;
                    CoreHelpers::ParseTokensToBytes(itHexs->second[i], bytesBE, any);
                    // Convert BE -> LE for write buffer
                    int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : (int)bytesBE.size();
                    if (expectedBytes < 1) expectedBytes = (int)bytesBE.size();
                    if (expectedBytes < 1) expectedBytes = 1;
                    // pad left (BE) then convert to LE
                    std::vector<uint8_t> paddedBE(expectedBytes, 0);
                    if ((int)bytesBE.size() >= expectedBytes) {
                        int startIdx = (int)bytesBE.size() - expectedBytes;
                        for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                    } else {
                        int pad = expectedBytes - (int)bytesBE.size();
                        for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                    }
                    writeLE.resize(expectedBytes);
                    for (int b = 0; b < expectedBytes; ++b) writeLE[b] = paddedBE[expectedBytes - 1 - b];
                    haveData = true;
                }

                if (!haveData) {
                    // try savedSDOValues decimal string
                    auto itVals = savedSDOValues.find(slave);
                    if (itVals != savedSDOValues.end() && i < itVals->second.size() && !itVals->second[i].empty()) {
                        // parse as either hex tokens or decimal according to ESI
                        const std::wstring &s = itVals->second[i];
                        std::vector<uint8_t> bytesBE; bool anyHex=false;
                        // reuse ParseTokensToBytes via wide->string conversion
                        CoreHelpers::ParseTokensToBytes(s, bytesBE, anyHex);
                        if (anyHex && !bytesBE.empty()) {
                            int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : (int)bytesBE.size();
                            if (expectedBytes < 1) expectedBytes = (int)bytesBE.size();
                            std::vector<uint8_t> paddedBE(expectedBytes, 0);
                            if ((int)bytesBE.size() >= expectedBytes) {
                                int startIdx = (int)bytesBE.size() - expectedBytes;
                                for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                            } else {
                                int pad = expectedBytes - (int)bytesBE.size();
                                for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                            }
                            writeLE.resize(expectedBytes);
                            for (int b = 0; b < expectedBytes; ++b) writeLE[b] = paddedBE[expectedBytes - 1 - b];
                            haveData = true;
                        } else {
                            // try decimal parsing into LE bytes
                            unsigned long long v = 0;
                            try { v = std::stoull(std::string(s.begin(), s.end())); } catch(...) { v = 0; }
                            int expectedBytes = (obj.bitlen > 0) ? ((obj.bitlen + 7) / 8) : 4;
                            if (expectedBytes < 1) expectedBytes = 1; if (expectedBytes > 8) expectedBytes = 8;
                            writeLE.resize(expectedBytes);
                            for (int b = 0; b < expectedBytes; ++b) writeLE[b] = (uint8_t)((v >> (8*b)) & 0xFF);
                            haveData = true;
                        }
                    }
                }

                if (!haveData) {
                    // nothing to write for this object
                    continue;
                }

                // Perform SDO write (LE buffer)
                bool ok = MasterWriteSDO((int)slave, obj.index, obj.subindex, writeLE.data(), (int)writeLE.size());
                if (ok) {
                    std::wstringstream ss; ss << L"寫入成功: 0x" << std::hex << std::uppercase << obj.index << L":" << (int)obj.subindex << L"\r\n";
                    PostUTF8LogToUI(CoreHelpers::WideToUtf8(ss.str()));
                    // Update cache and UI for this slave
                    auto &hexvec = savedSDOHexValues[slave]; if ((int)hexvec.size() < (int)currentObjects.size()) hexvec.resize(currentObjects.size());
                    std::wstringstream shex; shex << std::hex << std::uppercase;
                    for (int b = (int)writeLE.size()-1; b >= 0; --b) { if (b < (int)writeLE.size()-1) shex << L" "; shex << L"0x" << std::setw(2) << std::setfill(L'0') << (int)writeLE[b]; }
                    hexvec[i] = shex.str();
                    auto &valvec = savedSDOValues[slave]; if ((int)valvec.size() < (int)currentObjects.size()) valvec.resize(currentObjects.size());
                    unsigned long long u = 0; for (int b = 0; b < (int)writeLE.size() && b < 8; ++b) u |= (unsigned long long)writeLE[b] << (8*b);
                    valvec[i] = std::to_wstring(u);
                    if (confirmedSlave == slave && hListSDOParams && IsWindow(hListSDOParams)) {
                        ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(hexvec[i].c_str()));
                        ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(valvec[i].c_str()));
                    }
                } else {
                    std::string err = MasterGetLastError();
                    PostUTF8LogToUI(std::string("寫入失敗: ") + err + "\n");
                }

                // small delay to avoid hammering the network
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            PostUTF8LogToUI(std::string("全部寫入完成\n"));
            // Re-enable controls on UI thread
            MainWindow::PostUTF8LogToHWND(hwnd, std::string("ENABLE_CONTROLS\n"));
        } catch (const std::exception &ex) {
            std::string msg = std::string("全部寫入發生例外: ") + ex.what() + "\n";
            PostUTF8LogToUI(msg);
            MainWindow::PostUTF8LogToHWND(hwnd, std::string("ENABLE_CONTROLS\n"));
        } catch (...) {
            PostUTF8LogToUI(std::string("全部寫入發生未知例外\n"));
            MainWindow::PostUTF8LogToHWND(hwnd, std::string("ENABLE_CONTROLS\n"));
        }
        return;
    }).detach();
}


void MainWindow::OnSaveParams()
{
    bool hasSlave_local = (confirmedSlave > 0);
    if (currentObjects.empty()) {
        MessageBoxW(hwnd, L"目前沒有載入任何 ESI 物件，無法存檔。", L"無 ESI", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring deviceNameW;
    {
        std::string devUtf8;
        if (ReadDeviceNameUtf8(devUtf8)) {
            deviceNameW = CoreHelpers::Utf8ToWide(devUtf8);
        }
    }
    // Fallback to default if device name couldn't be read via SDO
    if (deviceNameW.empty()) deviceNameW = L"device";

    std::wstring safeName = deviceNameW;
    for (wchar_t &ch : safeName) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || iswspace(ch)) ch = L'_';
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    std::wstring parDir = dir + L"\\Par";
    CreateDirectoryW(parDir.c_str(), NULL);

    // Prepare content to save
    std::vector<std::wstring> emptyHexVec;
    std::vector<std::wstring> emptyValVec;
    auto itHexs = savedSDOHexValues.find(confirmedSlave);
    auto itVals = savedSDOValues.find(confirmedSlave);
    const std::vector<std::wstring> &hexVec = (itHexs != savedSDOHexValues.end()) ? itHexs->second : emptyHexVec;
    const std::vector<std::wstring> &valVec = (itVals != savedSDOValues.end()) ? itVals->second : emptyValVec;
    std::string content = BuildESIDeviceXml(deviceNameW, currentObjects, hexVec, valVec);

    // Present Save As dialog allowing user to choose path; prefill filename per convention
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    wchar_t szFile[MAX_PATH] = {0};
    if (hasSlave_local) {
        swprintf_s(szFile, MAX_PATH, L"%s\\%s-%d.xml", parDir.c_str(), safeName.c_str(), confirmedSlave);
    } else {
        swprintf_s(szFile, MAX_PATH, L"%s\\%s.xml", parDir.c_str(), safeName.c_str());
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"XML files\0*.xml\0All files\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = parDir.c_str();
    ofn.lpstrTitle = L"存檔為: 參數 XML 檔案";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_HIDEREADONLY;

    if (!GetSaveFileNameW(&ofn)) {
        // user cancelled
        PrependStatus(L"存檔已取消（使用者取消）");
        return;
    }

    // ofn.lpstrFile contains chosen path
    std::wstring chosenPath = ofn.lpstrFile;

    FILE *f = NULL;
    errno_t er = _wfopen_s(&f, chosenPath.c_str(), L"wb");
    if (er != 0 || !f) {
        std::wstring msg = L"無法建立檔案: "; msg += chosenPath;
        MessageBoxW(hwnd, msg.c_str(), L"寫入失敗", MB_OK | MB_ICONERROR);
        return;
    }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);

    std::wstring doneMsg = L"已存檔: ";
    doneMsg += chosenPath;
    PrependStatus(doneMsg.c_str());
}


void MainWindow::OnImportParams()
{
    OPENFILENAMEW ofn; memset(&ofn, 0, sizeof(ofn));
    wchar_t szFile[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"XML files\0*.xml\0All files\0*.*\0";
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrTitle = L"選擇參數 XML 檔案";

    // Build default filename same as OnSaveParams: Par\<safeDeviceName>[-<slave>].xml
    std::wstring deviceNameW;
    {
        std::string devUtf8;
        if (ReadDeviceNameUtf8(devUtf8)) {
            deviceNameW = CoreHelpers::Utf8ToWide(devUtf8);
        }
    }
    if (deviceNameW.empty()) deviceNameW = L"device";
    std::wstring safeName = deviceNameW;
    for (wchar_t &ch : safeName) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|' || iswspace(ch)) ch = L'_';
    }

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir = dir.substr(0, pos);
    static std::wstring initialParDir;
    initialParDir = dir + L"\\Par";
    CreateDirectoryW(initialParDir.c_str(), NULL);

    // Compose default full path
    if (confirmedSlave > 0) {
        swprintf_s(szFile, MAX_PATH, L"%s\\%s-%d.xml", initialParDir.c_str(), safeName.c_str(), confirmedSlave);
    } else {
        swprintf_s(szFile, MAX_PATH, L"%s\\%s.xml", initialParDir.c_str(), safeName.c_str());
    }

    ofn.lpstrInitialDir = initialParDir.c_str();
    // If user changes filter or filename, allow editing
    ofn.Flags |= OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) {
        return;
    }

    tinyxml2::XMLDocument doc;
    std::string pathUtf8 = CoreHelpers::WideToUtf8(szFile);
    tinyxml2::XMLError err = doc.LoadFile(pathUtf8.empty() ? nullptr : pathUtf8.c_str());
    if (err != tinyxml2::XML_SUCCESS) {
        std::wstring msg = L"無法讀取 XML 檔案: "; msg += szFile;
        MessageBoxW(hwnd, msg.c_str(), L"讀取失敗", MB_OK | MB_ICONERROR);
        return;
    }

    tinyxml2::XMLElement *root = doc.FirstChildElement("Device");
    if (!root) {
        MessageBoxW(hwnd, L"XML 格式錯誤：找不到 Device 根節點。", L"格式錯誤", MB_OK | MB_ICONERROR);
        return;
    }

    // 任何情況下都從 XML 完整重建物件表
    std::vector<EsiObjectEntry> xmlObjects;
    std::vector<std::wstring> hexVec;
    std::vector<std::wstring> valueFromFile;
    int importedCount = 0;

    for (tinyxml2::XMLElement *obj = root->FirstChildElement("Object"); obj; obj = obj->NextSiblingElement("Object")) {
        const char *idxAttr = obj->Attribute("index");
        const char *subAttr = obj->Attribute("subindex");
        const char *nameAttr = obj->Attribute("name");
        const char *dataTypeAttr = obj->Attribute("dataType");
        const char *accessAttr = obj->Attribute("access");
        const char *bitlenAttr = obj->Attribute("bitlen");
        const char *hexAttr = obj->Attribute("hex");
        const char *valueAttr = obj->Attribute("value");

        if (!idxAttr || !subAttr) continue;
        
        unsigned int idx = 0, sub = 0;
        if (sscanf_s(idxAttr, "%i", &idx) != 1) {
            if (sscanf_s(idxAttr, "0x%x", &idx) != 1) continue;
        }
        if (sscanf_s(subAttr, "%i", &sub) != 1) {
            if (sscanf_s(subAttr, "0x%x", &sub) != 1) continue;
        }

        EsiObjectEntry entry;
        entry.index = (uint16_t)idx;
        entry.subindex = (uint8_t)sub;
        entry.name = nameAttr ? nameAttr : "";
        entry.dataType = dataTypeAttr ? dataTypeAttr : "UNKNOWN";
        if (accessAttr) {
            entry.access = accessAttr;
            entry.accessNormalized = EsiParser::NormalizeAccess(entry.access);
        } else {
            entry.access.clear();
            entry.accessNormalized.clear();
        }
        
        if (bitlenAttr) {
            if (sscanf_s(bitlenAttr, "%d", &entry.bitlen) != 1) {
                entry.bitlen = 0;
            }
        } else {
            entry.bitlen = 0;
        }

        xmlObjects.push_back(entry);
        
        std::wstring hexW = (hexAttr && hexAttr[0] != '\0') ? CoreHelpers::Utf8ToWide(hexAttr) : L"";
        std::wstring valW = (valueAttr && valueAttr[0] != '\0') ? CoreHelpers::Utf8ToWide(valueAttr) : L"";
        
        hexVec.push_back(hexW);
        valueFromFile.push_back(valW);
        
        if (!hexW.empty() || !valW.empty()) {
            importedCount++;
        }
    }
    
    // 更新 currentObjects
    currentObjects = xmlObjects;

    savedSDOHexValues[confirmedSlave] = hexVec;

    auto &cachedValues = savedSDOValues[confirmedSlave];
    if ((int)cachedValues.size() < (int)currentObjects.size()) cachedValues.resize(currentObjects.size());

    // 完整重建 ListView
    if (hListSDOParams && IsWindow(hListSDOParams)) {
        ListView_DeleteAllItems(hListSDOParams);
        
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            const auto &obj = currentObjects[i];
            
            LVITEMW lvi; memset(&lvi, 0, sizeof(lvi));
            lvi.mask = LVIF_TEXT;
            lvi.iItem = (int)i;
            lvi.iSubItem = 0;
            
            wchar_t idxBuf[32];
            swprintf_s(idxBuf, 32, L"0x%04X", obj.index);
            lvi.pszText = idxBuf;
            int idx = ListView_InsertItem(hListSDOParams, &lvi);
            
            wchar_t subBuf[32];
            swprintf_s(subBuf, 32, L"0x%02X", obj.subindex);
            ListView_SetItemText(hListSDOParams, idx, 1, subBuf);
            
            std::wstring nameW = CoreHelpers::Utf8ToWide(obj.name);
            ListView_SetItemText(hListSDOParams, idx, 2, const_cast<LPWSTR>(nameW.c_str()));
            
            std::wstring dtypeW = CoreHelpers::Utf8ToWide(obj.dataType);
            ListView_SetItemText(hListSDOParams, idx, 3, const_cast<LPWSTR>(dtypeW.c_str()));
            
            std::wstring accessW = CoreHelpers::Utf8ToWide(obj.access);
            ListView_SetItemText(hListSDOParams, idx, 4, const_cast<LPWSTR>(accessW.c_str()));
        }
    }

    if (hListSDOParams && IsWindow(hListSDOParams)) {
        for (size_t i = 0; i < currentObjects.size(); ++i) {
            if (!hexVec[i].empty()) {
                ListView_SetItemText(hListSDOParams, (int)i, 6, const_cast<LPWSTR>(hexVec[i].c_str()));

                std::vector<uint8_t> bytesBE;
                bool anyHexTok = false;
                CoreHelpers::ParseTokensToBytes(hexVec[i], bytesBE, anyHexTok);

                int expectedBytes = 0;
                int matchedBitlen = currentObjects[i].bitlen;
                if (matchedBitlen > 0) expectedBytes = (matchedBitlen + 7) / 8;
                else expectedBytes = (int)bytesBE.size();
                if (expectedBytes < 1) expectedBytes = 1;

                std::vector<uint8_t> paddedBE(expectedBytes, 0);
                if ((int)bytesBE.size() >= expectedBytes) {
                    int startIdx = (int)bytesBE.size() - expectedBytes;
                    for (int b = 0; b < expectedBytes; ++b) paddedBE[b] = bytesBE[startIdx + b];
                } else {
                    int pad = expectedBytes - (int)bytesBE.size();
                    for (size_t b = 0; b < bytesBE.size(); ++b) paddedBE[pad + b] = bytesBE[b];
                }

                unsigned long long u = 0; int maxb = expectedBytes; if (maxb > 8) maxb = 8;
                for (int bi = 0; bi < maxb; ++bi) {
                    int idxFromEnd = expectedBytes - 1 - bi;
                    if (idxFromEnd >= 0) u |= (unsigned long long)paddedBE[idxFromEnd] << (8 * bi);
                }

                std::wstring valueDisplay;
                if (!valueFromFile[i].empty()) {
                    valueDisplay = valueFromFile[i];
                } else {
                    const EsiObjectEntry &obj = currentObjects[i];
                    std::string dtype = obj.dataType; std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });

                    if (du.find("BOOL") != std::string::npos || du.find("BOOLEAN") != std::string::npos) {
                        valueDisplay = (u ? std::wstring(L"1") : std::wstring(L"0"));
                    } else if (du.find("REAL") != std::string::npos || du.find("FLOAT") != std::string::npos) {
                        if (obj.bitlen == 32 && expectedBytes >= 4) {
                            uint32_t le = 0; for (int b = 0; b < 4; ++b) le |= (uint32_t)paddedBE[b] << (8*b);
                            float f; memcpy(&f, &le, sizeof(f)); std::wstringstream ss; ss << std::fixed << std::setprecision(6) << f; valueDisplay = ss.str();
                        } else if (obj.bitlen == 64 && expectedBytes >= 8) {
                            uint64_t le = 0; for (int b = 0; b < 8; ++b) le |= (uint64_t)paddedBE[b] << (8*b);
                            double d; memcpy(&d, &le, sizeof(d)); std::wstringstream ss; ss << std::fixed << std::setprecision(9) << d; valueDisplay = ss.str();
                        } else {
                            valueDisplay = std::to_wstring(u);
                        }
                    } else if (du.find("INT") != std::string::npos || du.find("SINT") != std::string::npos || du.find("SIGNED") != std::string::npos) {
                        int bits = (obj.bitlen > 0 ? obj.bitlen : (expectedBytes * 8));
                        long long sval = 0;
                        if (bits <= 64) {
                            if (bits < 64) {
                                unsigned long long mask = 1ULL << (bits - 1);
                                if (u & mask) {
                                    unsigned long long signmask = (~0ULL) << bits;
                                    sval = (long long)(u | signmask);
                                } else sval = (long long)u;
                            } else sval = (long long)u;
                        }
                        valueDisplay = std::to_wstring(sval);
                    } else {
                        valueDisplay = std::to_wstring(u);
                    }
                }

                ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(valueDisplay.c_str()));
                cachedValues[i] = valueDisplay;
            } else if (!valueFromFile[i].empty()) {
                if (hListSDOParams && IsWindow(hListSDOParams)) {
                    ListView_SetItemText(hListSDOParams, (int)i, 5, const_cast<LPWSTR>(valueFromFile[i].c_str()));
                }
                cachedValues[i] = valueFromFile[i];
            }
        }
    }

    // Log a summary of imported values (if any) for diagnostics
    if (importedCount > 0) {
        try { PostUTF8LogToUI(std::string("IMPORTED_SDO_VALUES: count=") + std::to_string(importedCount) + "\n"); } catch(...) {}
    }

    // 顯示匯入完整度資訊
    std::wstring done = L"已從 "; 
    done += szFile; 
    done += L" 完整載入參數表：";
    done += std::to_wstring((int)currentObjects.size());
    done += L" 個參數項目";
    
    PrependStatus(done.c_str());
    if (hBtnSaveParams && IsWindow(hBtnSaveParams)) {
        SetControlEnabled(hBtnSaveParams, true);
    }
    // If we have an ESI loaded and the EtherCAT master is initialized (connected),
    // enable SDO-related buttons so the operator can immediately perform SDO actions.
    bool hasESI_local = !currentObjects.empty();
    bool isConnected_local = IsMasterInitialized();
    if (hBtnReadSDO) SetControlEnabled(hBtnReadSDO, (hasESI_local && isConnected_local) ? true : false);
    if (hBtnWriteSDO) SetControlEnabled(hBtnWriteSDO, (hasESI_local && isConnected_local) ? true : false);
    if (hBtnReadAllSDO) SetControlEnabled(hBtnReadAllSDO, (hasESI_local && isConnected_local) ? true : false);
    bool hasSlave_local = (confirmedSlave > 0);
    if (hBtnWriteAllSDO) SetControlEnabled(hBtnWriteAllSDO, (hasESI_local && isConnected_local && hasSlave_local) ? true : false);
}


bool MainWindow::ValidateSDOValueInput(int itemIndex, const std::wstring &input, std::wstring &outError) {
    outError.clear();
    if (itemIndex < 0 || itemIndex >= (int)currentObjects.size()) return true; // nothing to validate
    const EsiObjectEntry &obj = currentObjects[itemIndex];
    std::string dtype = obj.dataType;
    std::string du = dtype; std::transform(du.begin(), du.end(), du.begin(), [](unsigned char c){ return (char)std::toupper(c); });
    std::wstring s = input;
    // trim
    auto is_space = [](wchar_t c){ return iswspace(c); };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    if (s.empty()) {
        outError = L"輸入不能為空";
        return false;
    }

    bool isFloat = (du.find("REAL") != std::string::npos) || (du.find("FLOAT") != std::string::npos) || (du.find("DOUBLE") != std::string::npos) || (du.find("LREAL") != std::string::npos);
    bool isBool = (du.find("BOOL") != std::string::npos) || (du.find("BOOLEAN") != std::string::npos);
    bool isUnsigned = (du.find("U") != std::string::npos && du.find("INT") != std::string::npos) || (du.find("BYTE") != std::string::npos) || (du.find("WORD") != std::string::npos) || (du.find("DWORD") != std::string::npos) || (du.find("LWORD") != std::string::npos) || (du.find("USINT") != std::string::npos) || (du.find("UINT") != std::string::npos) || (du.find("UDINT") != std::string::npos) || (du.find("ULINT") != std::string::npos);
    bool isSigned = (du.find("SINT") != std::string::npos) || (du.find("DINT") != std::string::npos) || (du.find("LINT") != std::string::npos) || ((du.find("INT") != std::string::npos) && !isUnsigned);

    // If input looks like hex token list (0x..), accept and skip numeric validation
    bool anyHexTok = false;
    std::vector<uint8_t> typedBE;
    // Use shared utility to parse tokenized hex/decimal sequences
    CoreHelpers::ParseTokensToBytes(s, typedBE, anyHexTok);
    if (anyHexTok && !typedBE.empty()) return true; // allow hex token input

    if (isBool) {
        if (!(s == L"0" || s == L"1" || _wcsicmp(s.c_str(), L"TRUE") == 0 || _wcsicmp(s.c_str(), L"FALSE") == 0)) {
            outError = L"布林欄位僅接受 0/1 或 true/false";
            return false;
        }
        return true;
    }

    if (isFloat) {
        // try to parse as double
        wchar_t *endp = nullptr; errno = 0;
        double v = wcstod(s.c_str(), &endp);
        if (endp == s.c_str() || *endp != L'\0') {
            outError = L"浮點數格式不正確";
            return false;
        }
        (void)v; return true;
    }

    // Signed/Unsigned integer validation
    if (isUnsigned || isSigned) {
        // remove separators and whitespace
        std::wstring t = s;
        for (auto it = t.begin(); it != t.end();) {
            if (iswspace(*it) || *it == L',' || *it == L';' || *it == L':' || *it == L'|') it = t.erase(it); else ++it;
        }

        // Hex form (0x...) accepted for both signed and unsigned (we allow user hex tokens)
        if (t.size() >= 2 && (t[0]==L'0') && (t[1]==L'x' || t[1]==L'X')) {
            for (size_t i = 2; i < t.size(); ++i) if (!iswxdigit(t[i])) { outError = L"十六進位格式錯誤"; return false; }
            return true;
        }

        if (isSigned) {
            // parse as signed decimal (allow leading + / -)
            wchar_t *endp = nullptr;
            errno = 0;
            long long sv = wcstoll(t.c_str(), &endp, 10);
            if (endp == t.c_str() || *endp != L'\0') {
                outError = L"整數格式不正確";
                return false;
            }
            // If bitlen provided, enforce signed range
            if (obj.bitlen > 0 && obj.bitlen <= 64) {
                int bits = obj.bitlen;
                if (bits >= 1 && bits <= 64) {
                    // compute signed min/max safely
                    if (bits == 64) {
                        // range covers full signed 64-bit
                    } else {
                        long long minVal = -(1LL << (bits - 1));
                        long long maxVal = ((1LL << (bits - 1)) - 1LL);
                        if (sv < minVal || sv > maxVal) {
                            outError = L"輸入值超出 ESI 定義的帶符號位元長度範圍";
                            return false;
                        }
                    }
                }
            }
            (void)sv; return true;
        } else {
            // unsigned: do not accept leading '-'
            if (!t.empty() && t[0] == L'-') { outError = L"無符號整數欄位不接受負值"; return false; }
            // parse as unsigned decimal
            wchar_t *endp = nullptr; errno = 0;
            unsigned long long v = wcstoull(t.c_str(), &endp, 10);
            if (endp == t.c_str() || *endp != L'\0') { outError = L"整數格式不正確"; return false; }
            if (obj.bitlen > 0 && obj.bitlen <= 64) {
                int expectedBytes = (obj.bitlen + 7) / 8;
                if (expectedBytes >= 1 && expectedBytes <= 8) {
                    unsigned long long maxVal = (expectedBytes >= 8) ? 0xFFFFFFFFFFFFFFFFULL : ((1ULL << (8*expectedBytes)) - 1ULL);
                    if (v > maxVal) { outError = L"輸入值超出 ESI 定義的位元長度"; return false; }
                }
            }
            (void)v; return true;
        }
    }

    // Default: accept any non-empty input
    return true;
}

// Child/status window proc moved to StatusPane.cpp

// Subclass proc for in-place edit control to capture Enter/Esc and forward commit




