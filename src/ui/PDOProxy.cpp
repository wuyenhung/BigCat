// PDOProxy implementation (migrated from PDODisplayManager.cpp)
#include "EtherCATMaster.h"
#include "../include/PDOProxy.h"
#include "MainWindow.h"
#include "Win32Utils.h"
#include "../include/core/CoreHelpers.h"
#include "../include/PDOHelpers.h"
#include "../include/MotionLogic.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <sstream>
#include <iomanip>

// Implementation of PDOProxy methods (moved out of MainWindow.cpp)

PDOProxy::PDOProxy()
    : hParent(NULL), hGroupBox(NULL), hOutputHeader(NULL), hInputHeader(NULL),
      currentY(0), isInitialized(false), selectedSlave(-1)
{
}

PDOProxy::~PDOProxy() {
    Clear();
}

void PDOProxy::Initialize(HWND parent, HINSTANCE hInstance, HFONT hFont, 
                                  int startY, int leftMargin, int width, int height) {
    if (isInitialized) return;
    
    hParent = parent;
    currentY = startY;
    groupLeftMargin = leftMargin;
    groupWidth = width;
    groupHeight = height;
    
    // Create group box (no visible caption)
    hGroupBox = CreateChildControlEx(0, parent, hInstance, L"BUTTON", L"",
                               WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                               leftMargin, startY, width, height,
                               0, hFont);
    if (hGroupBox) {
        SetControlFont(hGroupBox, hFont, TRUE);
    }
    
    hOutputHeader = NULL;
    hInputHeader = NULL;
    
    isInitialized = true;
    if (pdoItems.empty()) {
        for (int i = 0; i < 4; ++i) {
            PDOItem po;
            po.isOutput = true;
            po.isEditable = false;
            po.name = L"(未載入)";
            po.dataType = L"";
            po.unit = L"";
            po.defaultValue = L"0";
            po.currentValue = L"";
            pdoItems.push_back(po);
        }
        for (int i = 0; i < 4; ++i) {
            PDOItem pi;
            pi.isOutput = false;
            pi.isEditable = false;
            pi.name = L"(未載入)";
            pi.dataType = L"";
            pi.unit = L"";
            pi.defaultValue = L"0";
            pi.currentValue = L"";
            pdoItems.push_back(pi);
        }
    }

    RebuildPDOControls();
}

void PDOProxy::RebuildPDOControls() {
    if (!isInitialized || !hParent) return;
    // Prevent re-entry: Rebuild can be triggered from other code paths; avoid overlapping rebuilds
    if (isRebuilding) return;
    isRebuilding = true;
    // RAII guard to ensure the flag is reset if we exit early or throw
    struct RebuildFlagGuard { bool &f; RebuildFlagGuard(bool &x) : f(x) {} ~RebuildFlagGuard() { f = false; } } guard(isRebuilding);
    // Suspend painting on the parent to avoid intermediate flicker/artifacts while we
    // destroy/create child windows and compute final positions. Use RAII guard
    // to ensure redraw and visibility are restored on all exit paths.
    struct PaintSuspend {
        HWND parent;
        HWND gb;
        PaintSuspend(HWND p, HWND g) : parent(p), gb(g) {
            if (parent && IsWindow(parent)) SendMessageW(parent, WM_SETREDRAW, FALSE, 0);
            if (gb && IsWindow(gb)) ShowWindow(gb, SW_HIDE);
        }
        ~PaintSuspend() {
            if (parent && IsWindow(parent)) {
                SendMessageW(parent, WM_SETREDRAW, TRUE, 0);
                RedrawWindow(parent, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            if (gb && IsWindow(gb)) ShowWindow(gb, SW_SHOW);
        }
    } paintGuard(hParent, hGroupBox);
    for (auto &it : pdoItems) {
        if (it.hLabel && IsWindow(it.hLabel)) { DestroyWindow(it.hLabel); it.hLabel = NULL; }
        if (it.hValue && IsWindow(it.hValue)) { DestroyWindow(it.hValue); it.hValue = NULL; }
        if (it.hEdit && IsWindow(it.hEdit)) { DestroyWindow(it.hEdit); it.hEdit = NULL; }
        if (it.hButton && IsWindow(it.hButton)) { DestroyWindow(it.hButton); it.hButton = NULL; }
        if (it.hAddress && IsWindow(it.hAddress)) { DestroyWindow(it.hAddress); it.hAddress = NULL; }
        if (it.hDesc && IsWindow(it.hDesc)) { DestroyWindow(it.hDesc); it.hDesc = NULL; }
    }

    HINSTANCE hInst = GetModuleHandle(NULL);
    HFONT hFont = (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0);
    if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    int editableCounter = 0;
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        // For editable inputs, do NOT use currentValue (which may have been set by controller staging).
        // Editable controls must reflect user input only; prefer defaultValue or empty if none.
        std::wstring init;
        if (pdoItems[i].isEditable) {
            // Try defaultValue first; leave empty if none to force user entry
            init = !pdoItems[i].defaultValue.empty() ? pdoItems[i].defaultValue : std::wstring(L"");
        } else {
            // Non-editable/display items use currentValue if available, else default
            init = !pdoItems[i].currentValue.empty() ? pdoItems[i].currentValue : pdoItems[i].defaultValue;
        }
        int idIndex = pdoItems[i].isEditable ? editableCounter : (int)i;
        CreatePDOItem(hParent, hInst, hFont, pdoItems[i], idIndex, GROUPBOX_INNER_TOP + 8, groupLeftMargin, groupWidth, init);
        if (pdoItems[i].isEditable) ++editableCounter;
    }

    SetFont(hFont);
    ResizeWidth(groupWidth);
}

void PDOProxy::Show(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    if (hGroupBox) ShowWindow(hGroupBox, cmd);
    if (hOutputHeader) ShowWindow(hOutputHeader, cmd);
    if (hInputHeader) ShowWindow(hInputHeader, cmd);
    for (auto &item : pdoItems) {
        if (item.hLabel) ShowWindow(item.hLabel, cmd);
        if (item.hValue) ShowWindow(item.hValue, cmd);
        if (item.hEdit) ShowWindow(item.hEdit, cmd);
        if (item.hButton) ShowWindow(item.hButton, cmd);
    }
}

void PDOProxy::SetTop(int top) {
    currentY = top;
    if (hGroupBox) {
        RECT r;
        GetWindowRect(hGroupBox, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hGroupBox, NULL, p.x, top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

int PDOProxy::GetHeight() const {
    return groupHeight;
}

void PDOProxy::ResizeHeight(int height) {
    groupHeight = height;
    if (hGroupBox) {
        RECT r;
        GetWindowRect(hGroupBox, &r);
        POINT p = { r.left, r.top };
        ScreenToClient(hParent, &p);
        SetWindowPos(hGroupBox, NULL, p.x, p.y, groupWidth, height, SWP_NOZORDER);
    }
}

void PDOProxy::UpdateItemValue(size_t index, const std::wstring &newValue) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowTextW(item.hValue, newValue.c_str());
        InvalidateRect(item.hValue, NULL, TRUE);
        UpdateWindow(item.hValue);
    }
}

void PDOProxy::SetItemEditable(size_t index, bool editable) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetControlEnabled(item.hEdit, editable);
    }
}

void PDOProxy::SetItemButtonText(size_t index, const std::wstring &btnText) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowTextW(item.hButton, btnText.c_str());
    }
}

void PDOProxy::SelectSlave(int slaveIndex) {
    if (slaveIndex < 0 || (size_t)slaveIndex >= pdoItems.size()) return;
    selectedSlave = slaveIndex;
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        auto &item = pdoItems[i];
    // Do not force colors here. Let the system or parent WM_CTLCOLOR handlers decide
    // the visual appearance. Just request a redraw so the control repaints with
    // the current system/default colors.
    if (item.hLabel && IsWindow(item.hLabel)) InvalidateRect(item.hLabel, NULL, TRUE);
    if (item.hValue && IsWindow(item.hValue)) InvalidateRect(item.hValue, NULL, TRUE);
    }
}

void PDOProxy::BuildPDOItemsFromSDO(const EtherCATPDOConfig &pdoConfig) {
    Clear();
    for (const auto &entry : pdoConfig.entries) {
        PDOItem item;
        item.isOutput = entry.isOutput;
        item.isEditable = entry.isEditable;
        item.name = entry.name;
        item.dataType = entry.dataType;
        item.unit = entry.unit;
        item.minValue = entry.minValue;
        item.maxValue = entry.maxValue;
        item.defaultValue = entry.defaultValue;
        item.currentValue = entry.currentValue;
        pdoItems.push_back(item);
    }
    if (isInitialized) {
        Initialize(hParent, GetModuleHandle(NULL), (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0),
                   currentY, groupLeftMargin, groupWidth, groupHeight);
    }
}

void PDOProxy::RefreshItemValues(const std::vector<std::wstring> &newValues) {
    if (newValues.size() != pdoItems.size()) return;
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        pdoItems[i].currentValue = newValues[i];
        UpdateItemValue(i, newValues[i]);
    }
}

void PDOProxy::SetItemUnit(size_t index, const std::wstring &unit) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.unit = unit;
}

void PDOProxy::SetItemMinMax(size_t index, const std::wstring &minVal, const std::wstring &maxVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.minValue = minVal;
    item.maxValue = maxVal;
}

void PDOProxy::SetItemDefault(size_t index, const std::wstring &defVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.defaultValue = defVal;
}

void PDOProxy::SetItemDataType(size_t index, const std::wstring &dataType) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.dataType = dataType;
}

void PDOProxy::EnableItem(size_t index, bool enable) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        SetControlEnabled(item.hLabel, enable);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        SetControlEnabled(item.hValue, enable);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetControlEnabled(item.hEdit, enable);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        SetControlEnabled(item.hButton, enable);
    }
}

void PDOProxy::DisableAllItems() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        EnableItem(i, false);
    }
}

void PDOProxy::EnableAllItems() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        EnableItem(i, true);
    }
}

void PDOProxy::SetItemColor(size_t index, COLORREF color) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    (void)color; // parameter intentionally unused
    // Intentionally do not force text color; let the control use system/default colors.
    // Trigger a repaint in case the visual state needs updating.
    if (item.hLabel && IsWindow(item.hLabel)) InvalidateRect(item.hLabel, NULL, TRUE);
}

void PDOProxy::SetItemBackgroundColor(size_t index, COLORREF bgColor) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    (void)bgColor; // parameter intentionally unused
    // Intentionally do not force background color; let the control use system/default colors.
    if (item.hLabel && IsWindow(item.hLabel)) InvalidateRect(item.hLabel, NULL, TRUE);
}

void PDOProxy::ResetItemColors() {
    // Remove explicit reset behavior. Instead, invalidate all item controls so they
    // repaint using the current system/default colors or parent-provided brushes.
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        auto &item = pdoItems[i];
        if (item.hLabel && IsWindow(item.hLabel)) InvalidateRect(item.hLabel, NULL, TRUE);
        if (item.hValue && IsWindow(item.hValue)) InvalidateRect(item.hValue, NULL, TRUE);
        if (item.hEdit && IsWindow(item.hEdit)) InvalidateRect(item.hEdit, NULL, TRUE);
        if (item.hButton && IsWindow(item.hButton)) InvalidateRect(item.hButton, NULL, TRUE);
    }
}

void PDOProxy::SetApplyButtonsEnabled(bool enabled) {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        auto &item = pdoItems[i];
        if (item.isEditable && item.hEdit && IsWindow(item.hEdit)) {
            SetControlEnabled(item.hEdit, enabled);
        }
        if (item.hButton && IsWindow(item.hButton)) {
            SetControlEnabled(item.hButton, enabled);
        }
    }
}

void PDOProxy::UpdateItemDisplay(size_t index) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowTextW(item.hValue, item.currentValue.c_str());
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetControlEnabled(item.hEdit, item.isEditable);
    }
}

void PDOProxy::RefreshDisplay() {
    for (size_t i = 0; i < pdoItems.size(); ++i) {
        UpdateItemDisplay(i);
    }
}

void PDOProxy::SetParent(HWND parent) {
    hParent = parent;
    if (hGroupBox) {
        ::SetParent(hGroupBox, parent);
    }
    for (auto &item : pdoItems) {
        if (item.hLabel) ::SetParent(item.hLabel, parent);
        if (item.hValue) ::SetParent(item.hValue, parent);
        if (item.hEdit) ::SetParent(item.hEdit, parent);
        if (item.hButton) ::SetParent(item.hButton, parent);
    }
}

void PDOProxy::SetFont(HFONT hFont) {
    if (hGroupBox) {
        SetControlFont(hGroupBox, hFont, TRUE);
    }
    for (auto &item : pdoItems) {
        if (item.hLabel) SetControlFont(item.hLabel, hFont, TRUE);
        if (item.hValue) SetControlFont(item.hValue, hFont, TRUE);
        if (item.hEdit) SetControlFont(item.hEdit, hFont, TRUE);
        if (item.hButton) SetControlFont(item.hButton, hFont, TRUE);
    }
}

void PDOProxy::SetGroupBoxText(const std::wstring &text) {
    if (hGroupBox) {
        SetWindowTextW(hGroupBox, text.c_str());
    }
}

void PDOProxy::GetItemRect(size_t index, RECT &rect) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        GetWindowRect(item.hLabel, &rect);
        ScreenToClient(hGroupBox, (POINT*)&rect);
    }
}

void PDOProxy::GetGroupBoxRect(RECT &rect) {
    if (hGroupBox) {
        GetWindowRect(hGroupBox, &rect);
        ScreenToClient(hParent, (POINT*)&rect);
    }
}

void PDOProxy::SetItemPosition(size_t index, int x, int y) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        SetWindowPos(item.hLabel, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowPos(item.hValue, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetWindowPos(item.hEdit, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowPos(item.hButton, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void PDOProxy::MoveItem(size_t index, int dx, int dy) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        RECT rcLabel; GetWindowRect(item.hLabel, &rcLabel);
        SetWindowPos(item.hLabel, NULL, rcLabel.left + dx, rcLabel.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        RECT rcValue; GetWindowRect(item.hValue, &rcValue);
        SetWindowPos(item.hValue, NULL, rcValue.left + dx, rcValue.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        RECT rcEdit; GetWindowRect(item.hEdit, &rcEdit);
        SetWindowPos(item.hEdit, NULL, rcEdit.left + dx, rcEdit.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        RECT rcButton; GetWindowRect(item.hButton, &rcButton);
        SetWindowPos(item.hButton, NULL, rcButton.left + dx, rcButton.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

void PDOProxy::ResizeItem(size_t index, int newWidth, int newHeight) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    if (item.hLabel && IsWindow(item.hLabel)) {
        SetWindowPos(item.hLabel, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hValue && IsWindow(item.hValue)) {
        SetWindowPos(item.hValue, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hEdit && IsWindow(item.hEdit)) {
        SetWindowPos(item.hEdit, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (item.hButton && IsWindow(item.hButton)) {
        SetWindowPos(item.hButton, NULL, 0, 0, newWidth, newHeight, SWP_NOMOVE | SWP_NOZORDER);
    }
}

void PDOProxy::SetItemRange(size_t index, const std::wstring &minVal, const std::wstring &maxVal) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.minValue = minVal;
    item.maxValue = maxVal;
}

void PDOProxy::SetItemValue(size_t index, const std::wstring &value) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.currentValue = value;
    UpdateItemValue(index, value);
}

void PDOProxy::SetItemName(size_t index, const std::wstring &name) {
    if (index >= pdoItems.size()) return;
    auto &item = pdoItems[index];
    item.name = name;
}

bool PDOProxy::RecalculateOffsetsFromSDO(EtherCATMaster* master, int slaveIndex) {
    if (!master || slaveIndex <= 0) return false;
    if (pdoItems.empty()) return false;

    int outBase = 0, inBase = 0;
    if (!master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
        return false;
    }

    int curOut = outBase;
    int curIn = inBase;
    for (auto &it : pdoItems) {
        int bytes = 1;
        if (it.bitLength > 0) bytes = (it.bitLength + 7) / 8;
        else bytes = 4;

        if (it.isOutput) {
            it.byteOffset = curOut - outBase;
            curOut += bytes;
        } else {
            it.byteOffset = curIn - inBase;
            curIn += bytes;
        }
    }

    if (master) {
        std::vector<PDOChannelDescriptor> descriptors;
        descriptors.reserve(pdoItems.size());
        for (const auto &it : pdoItems) {
            PDOChannelDescriptor desc;
            desc.name = it.name;
            desc.index = it.index;
            desc.subindex = it.subindex;
            desc.bitLength = it.bitLength;
            desc.isOutput = it.isOutput;
            desc.byteOffset = it.byteOffset;
            descriptors.push_back(std::move(desc));
        }
        master->RegisterPDODescriptors(slaveIndex, descriptors);
    }

    if (isInitialized) RebuildPDOControls();
    return true;
}

bool PDOProxy::WriteToIOMap(EtherCATMaster* master, int slaveIndex, const std::wstring& itemName, int32_t value) {
    if (!master || slaveIndex <= 0) return false;
    std::string nameUtf8(itemName.begin(), itemName.end());

    auto PostUtf8Log = [&](const std::wstring &wmsg) {
        if (!hParent) return;
        std::string utf = CoreHelpers::WideToUtf8(wmsg);
        MainWindow::PostUTF8LogToHWND(hParent, utf);
    };

    if (itemName.empty()) {
        std::wstringstream dump;
        dump << L"[PDO] itemName empty; available items:\r\n";
        for (size_t i = 0; i < pdoItems.size(); ++i) {
            const auto &it = pdoItems[i];
            dump << L"  [" << i << L"] " << it.name << L" addr=" << it.address << L" offset=" << it.byteOffset << L" bits=" << (int)it.bitLength << L"\r\n";
        }
        PostUtf8Log(dump.str());
        return false;
    }

    int outBase = 0, inBase = 0;
    if (master->GetSlaveIOBaseBytes(slaveIndex, outBase, inBase)) {
        for (const auto &it : pdoItems) {
            if (it.name != itemName) continue;
            int bytes = (it.bitLength > 0) ? ((it.bitLength + 7) / 8) : 4;
            std::vector<uint8_t> buf(bytes);
            for (int b = 0; b < bytes; ++b) buf[b] = (uint8_t)((value >> (8 * b)) & 0xFF);
            int absOffset = (it.isOutput ? outBase : inBase) + it.byteOffset;
            std::wstringstream wlog; wlog << L"[PDO] StageOutputBytes absOffset=" << absOffset << L" len=" << bytes << L"\r\n";
            PostUtf8Log(wlog.str());

            // Validate computed addresses and sizes before staging.
            // Provide detailed diagnostic logs to help track incorrect offsets.
            int64_t absEnd = (int64_t)absOffset + (int64_t)bytes - 1;
            std::wstringstream diag;
            diag << L"[PDO DEBUG] name=" << itemName << L" addr=" << it.address << L" isOutput=" << (it.isOutput?L"out":L"in")
                 << L" byteOffset=" << it.byteOffset << L" outBase=" << outBase << L" inBase=" << inBase
                 << L" absOffset=" << absOffset << L" bytes=" << bytes << L" absEnd=" << absEnd << L"\r\n";
            PostUtf8Log(diag.str());

            // Basic bounds check: ensure non-negative offset and reasonable end.
            // IOmap size in EtherCATMaster is 4096 bytes; guard against overflow.
            const int IOMAP_SIZE = 4096; // keep in sync with EtherCATMaster::IOmap
            if (absOffset < 0 || bytes <= 0 || absEnd < 0 || absEnd >= IOMAP_SIZE) {
                std::wstringstream bad;
                bad << L"[PDO ERROR] computed absOffset/size out of range: absOffset=" << absOffset << L" bytes=" << bytes << L" IOMAP_SIZE=" << IOMAP_SIZE << L"\r\n";
                PostUtf8Log(bad.str());
                return false;
            }

            // SECURITY: prevent direct writes to Target Position (0x607A) while PDO filtering is active.
            // Writing 0x607A directly into IOmap bypasses the PDOFilter and can result in full-speed jumps
            // despite the UI showing a Soft Start. Block these writes and log a warning so callers
            // (e.g. MotionController/UI) must use the PDOFilter path instead.
            if (it.index == 0x607A && master && master->IsPDOFilteringEnabled()) {
                PostUtf8Log(std::wstring(L"[PDO] Blocked write to 0x607A while PDO filtering is enabled\r\n"));
                return false;
            }

            // Stage: prefer non-overwrite then overwrite (to minimize stomping)
            bool staged = master->StageOutputIfNotExists(absOffset, buf.data(), bytes, __FILE__, __LINE__, __func__);
            if (!staged) staged = master->StageOutputBytes(absOffset, buf.data(), bytes, __FILE__, __LINE__, __func__);
            if (staged) {
                std::wstringstream ok; ok << L"[PDO] staged absOffset=" << absOffset << L" len=" << bytes << L"\r\n";
                PostUtf8Log(ok.str());
                // If operator requested PDO 0x607A recording, emit a concise trace line
                try {
                    if (it.index == 0x607A && master && master->IsPDO607ARecordingEnabled()) {
                        std::vector<uint8_t> snap;
                        if (master->GetPDODataSnapshot(slaveIndex, snap)) {
                            const PDOItem* actItem = MotionLogic::FindActualPositionItem(pdoItems);
                            int32_t currentRaw = 0;
                            if (actItem) {
                                if (ReadPDOItemValueFromSnapshot(master, slaveIndex, snap, *actItem, currentRaw)) {
                                    printf("[PDO607A_REC] S%d: target=%d (0x%08X) curr=%d (0x%08X)\n",
                                           slaveIndex, value, (uint32_t)value, currentRaw, (uint32_t)currentRaw);
                                } else {
                                    printf("[PDO607A_REC] S%d: target=%d (0x%08X) curr=N/A\n",
                                           slaveIndex, value, (uint32_t)value);
                                }
                            } else {
                                // no descriptor for actual pos; still print target
                                printf("[PDO607A_REC] S%d: target=%d (0x%08X) curr=desc_missing\n",
                                       slaveIndex, value, (uint32_t)value);
                            }
                        }
                    }
                } catch(...) {}
                return true;
            } else {
                PostUtf8Log(L"[PDO] StageOutputBytes failed\r\n");
                return false;
            }
        }
    }

    PostUtf8Log(L"[PDO] Could not map item or get IO base; WriteToIOMap failed\r\n");
    return false;
}

std::vector<std::wstring> PDOProxy::GetEditableItems() const {
    std::vector<std::wstring> res;
    for (const auto &it : pdoItems) if (it.isEditable) res.push_back(it.name);
    return res;
}

void PDOProxy::CreatePDOItem(HWND parent, HINSTANCE hInst, HFONT font,
                                     PDOItem& item, int itemIndex, int y, int leftMargin, int width, 
                                     const std::wstring& initialValue) {
    const int baseEdit = 5000;
    const int baseBtn = 6000;
    (void)width;

    item.hLabel = CreateChildControlEx(0, parent, hInst, L"STATIC", item.name.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  leftMargin + 10, y, 10, 20, 0, font);
    if (item.hLabel) SetControlFont(item.hLabel, font, TRUE);

    item.hValue = CreateChildControlEx(WS_EX_CLIENTEDGE, parent, hInst, L"STATIC", initialValue.c_str(), 
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | WS_BORDER,
                                   leftMargin + 140, y, 10, 22, 0, font);
    if (item.hValue) SetControlFont(item.hValue, font, TRUE);

    std::wstring addr = item.address.empty() ? L"" : item.address;
    item.hAddress = CreateChildControlEx(0, parent, hInst, L"STATIC", addr.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     leftMargin + 2, y, 10, 20, 0, font);
    if (item.hAddress) SetControlFont(item.hAddress, font, TRUE);

    item.hDesc = NULL;
    if (!item.description.empty()) {
        item.hDesc = CreateChildControlEx(0, parent, hInst, L"STATIC", item.description.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      leftMargin + 140, y, 10, 20, 0, font);
    if (item.hDesc) SetControlFont(item.hDesc, font, TRUE);
    }

    item.hTooltip = NULL;
    if (!item.description.empty()) {
        HWND hTip = CreateChildControlEx(0, parent, hInst, TOOLTIPS_CLASS, NULL,
                                    WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    0, NULL);
        if (hTip) {
            TOOLINFOW ti; ZeroMemory(&ti, sizeof(ti));
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = parent;
            ti.uId = (UINT_PTR)item.hLabel;
            ti.lpszText = const_cast<LPWSTR>(item.description.c_str());
            SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            item.hTooltip = hTip;
        }
    }
    if (item.isEditable) {
        int eid = baseEdit + itemIndex;
        int bid = baseBtn + itemIndex;
        item.hEdit = CreateChildControlEx(WS_EX_CLIENTEDGE, parent, hInst, L"EDIT", initialValue.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                         leftMargin + 320, y, 10, 22, eid, font);
    if (item.hEdit) SetControlFont(item.hEdit, font, TRUE);
        item.hButton = CreateChildControl(parent, hInst, L"BUTTON", L"套用", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         leftMargin + 426, y, 10, 22, bid, font);
    if (item.hButton) SetControlFont(item.hButton, font, TRUE);
    } else {
        item.hEdit = NULL;
        item.hButton = NULL;
    }
}

std::wstring PDOProxy::FormatValue(int32_t value, uint8_t bitLength) {
    // Produce combined decimal and hex representation: "<dec> (0x<HEX>)"
    // Hex representation uses the raw bits (unsigned) and is zero-padded according
    // to the bitLength when supplied, otherwise defaults to 8 bytes (32-bit).
    int hexBytes = 4; // default bytes
    if (bitLength > 0) {
        hexBytes = (bitLength + 7) / 8;
        if (hexBytes < 1) hexBytes = 1;
    }
    int hexDigits = hexBytes * 2;

    std::wstringstream ss;
    // decimal (signed)
    ss << std::dec << value;
    // space then hex in parentheses
    ss << L" (0x";
    ss << std::hex << std::uppercase << std::setw(hexDigits) << std::setfill(L'0') << (uint32_t)value;
    ss << L")";
    return ss.str();
}

void PDOProxy::ResizeWidth(int width)
{
    if (!hParent || !hGroupBox) return;
    if (pdoItems.empty()) return;
    
    // Safety: verify parent and groupbox are still valid windows
    if (!IsWindow(hParent) || !IsWindow(hGroupBox)) return;
    
    // Suspend repaint while we compute and set layout positions to avoid
    // intermediate overlapping visuals. Use local guard RAII to ensure restore.
    struct PaintSuspendLocal {
        HWND parent; HWND gb; PaintSuspendLocal(HWND p, HWND g) : parent(p), gb(g) { if (parent && IsWindow(parent)) SendMessageW(parent, WM_SETREDRAW, FALSE, 0); if (gb && IsWindow(gb)) ShowWindow(gb, SW_HIDE); }
        ~PaintSuspendLocal() { if (parent && IsWindow(parent)) { SendMessageW(parent, WM_SETREDRAW, TRUE, 0); RedrawWindow(parent, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN); } if (gb && IsWindow(gb)) ShowWindow(gb, SW_SHOW); }
    } paintL(hParent, hGroupBox);

    (void)width;
    groupWidth = width;
    
    RECT r;
    GetWindowRect(hGroupBox, &r);
    POINT p = { r.left, r.top };
    ScreenToClient(hParent, &p);
    SetWindowPos(hGroupBox, NULL, p.x, p.y, groupWidth, groupHeight, SWP_NOZORDER);
    
    const int spacing = 6;
    const int innerLeftDefault = 10; // default left inset
    const int innerMargin = 20;

    // Map groupbox client origin to parent and get client width so centering
    // uses the inner client area (excluding window frame/border).
    POINT groupClientOrigin = {0,0};
    int groupClientWidth = groupWidth;
    if (hGroupBox) {
        MapWindowPoints(hGroupBox, hParent, &groupClientOrigin, 1);
        RECT rcClient; GetClientRect(hGroupBox, &rcClient);
        groupClientWidth = rcClient.right - rcClient.left;
    } else {
        groupClientOrigin.x = groupLeftMargin;
        groupClientOrigin.y = currentY;
    }

    int innerWidth = groupClientWidth - innerMargin;
    if (innerWidth < 200) innerWidth = 200;
    const int minColWidth = 140;
    const int colGap = 10;
    bool singleColumn = (innerWidth <= (minColWidth * 2 + colGap));
    int colWidth = singleColumn ? innerWidth : (innerWidth - colGap) / 2;

    // Compute total content width and center it within the group box
    int contentTotalWidth = singleColumn ? colWidth : (colWidth * 2 + colGap);
    int centeredInnerLeft = innerLeftDefault;
    if (groupWidth > contentTotalWidth) {
        centeredInnerLeft = (groupWidth - contentTotalWidth) / 2;
        if (centeredInnerLeft < innerLeftDefault) centeredInnerLeft = innerLeftDefault; // respect minimal inset
    }

    int leftX = centeredInnerLeft;
    int rightX = singleColumn ? leftX : (centeredInnerLeft + colWidth + colGap);

    HFONT hFont = (HFONT)SendMessageW(hGroupBox, WM_GETFONT, 0, 0);
    if (!hFont) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int headerHeight;
    {
        headerHeight = MeasureTextHeight(hFont);
    }
    int baseY = GROUPBOX_INNER_TOP + headerHeight + ROW_SPACING;
    int yLeft = baseY;
    int yRight = baseY;
    int yStack = baseY;

    auto layoutItem = [&](PDOItem& item, int colX, int colY, int col_W, int& nextY) {
        if (!item.hLabel) return;
        int absoluteX = groupClientOrigin.x + colX;
        int absoluteY = groupClientOrigin.y + colY;
        int cur = absoluteX;
        const int sp = spacing;
    // Increase address column width to make addresses more readable.
    int addressW = (std::min)(200, (std::max)(100, col_W / 5));
    int descW = (std::min)(180, (std::max)(60, col_W / 6));
        int availW = col_W - addressW - descW - sp*2;
        if (availW < 120) {
            int need = 120 - availW;
            int shrinkDesc = (std::min)(need, (std::max)(0, descW - 40));
            descW -= shrinkDesc; need -= shrinkDesc; availW += shrinkDesc;
            if (need > 0) {
                int shrinkAddr = (std::min)(need, (std::max)(0, addressW - 30));
                addressW -= shrinkAddr; need -= shrinkAddr; availW += shrinkAddr;
            }
            if (availW < 40) availW = 40;
        }
        // Constants removed as they are no longer used in the optimized layout logic


        if (item.isEditable) {
            // Layout for editable items (Outputs): Address | Name | Edit | Btn | Value
            // Priority: Value (120px min) > Edit (60px min) > Button (40px) > Address/Name
            const int btnW2 = 40; // Fixed button width
            int editW2 = 70;
            int valueW2 = 130; // Larger default for value display
            
            // Scale based on available width
            if (col_W > 600) { 
                editW2 = 90; 
                valueW2 = 150; 
            } else if (col_W > 500) {
                editW2 = 80;
                valueW2 = 140;
            } else if (col_W < 400) { 
                editW2 = 60; 
                valueW2 = 120; // Absolute minimum for value
            }

            // Reserve space for critical controls: Edit + Button + Value
            int criticalWidth = editW2 + btnW2 + valueW2 + (sp * 4); // 4 spaces total
            int remaining = col_W - criticalWidth;
            
            // Ensure we have minimum space
            if (remaining < 80) {
                remaining = 80;
                // Shrink edit/value slightly if needed
                if (col_W < criticalWidth + 80) {
                    int deficit = (criticalWidth + 80) - col_W;
                    if (editW2 > 60) {
                        int shrink = (std::min)(deficit, editW2 - 60);
                        editW2 -= shrink; deficit -= shrink;
                    }
                    if (deficit > 0 && valueW2 > 120) {
                        int shrink = (std::min)(deficit, valueW2 - 120);
                        valueW2 -= shrink; deficit -= shrink;
                    }
                    remaining = col_W - (editW2 + btnW2 + valueW2 + (sp * 4));
                    if (remaining < 60) remaining = 60;
                }
            }
            
            // Split remaining between Address (30%) and Name (70%)
            int addressW2 = (int)(remaining * 0.30);
            if (addressW2 < 40) addressW2 = 40;
            int nameW2 = remaining - addressW2;
            if (nameW2 < 40) nameW2 = 40;

            int labelH = 20; int ctrlH = 22;
            int labelTop = absoluteY + (ROW_HEIGHT - labelH) / 2;
            int ctrlTop  = absoluteY + (ROW_HEIGHT - ctrlH) / 2;

            if (item.hAddress && IsWindow(item.hAddress)) SetWindowPos(item.hAddress, NULL, absoluteX, labelTop, addressW2, labelH, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur += addressW2 + sp;

            // Name uses all remaining space (skip Desc to save space for Value)
            if (item.hLabel && IsWindow(item.hLabel)) SetWindowPos(item.hLabel, NULL, cur, labelTop, nameW2, labelH, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur += nameW2 + sp;
            
            // Hide Desc in this layout to maximize space for Value
            if (item.hDesc && IsWindow(item.hDesc)) {
                ShowWindow(item.hDesc, SW_HIDE);
            }

            if (item.hEdit && IsWindow(item.hEdit))  SetWindowPos(item.hEdit,  NULL, cur, ctrlTop, editW2, ctrlH, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur += editW2 + sp;

            if (item.hButton && IsWindow(item.hButton)) SetWindowPos(item.hButton, NULL, cur, ctrlTop, btnW2, ctrlH, SWP_NOZORDER);
            cur += btnW2 + sp;

            if (item.hValue && IsWindow(item.hValue)) SetWindowPos(item.hValue, NULL, cur, ctrlTop, valueW2, ctrlH, SWP_NOZORDER | SWP_SHOWWINDOW);

        } else {
            // Layout for read-only items (Inputs): Address | Name | Value
            // Priority: Value (120px min) > Address/Name
            int valueW2 = 130;
            if (col_W > 500) valueW2 = 150;
            else if (col_W > 400) valueW2 = 140;
            else if (col_W < 350) valueW2 = 120; // Absolute minimum

            // Reserve space for value
            int remaining = col_W - valueW2 - (sp * 3); // 3 spaces total
            if (remaining < 80) {
                remaining = 80;
                if (col_W < valueW2 + 80 + (sp * 3)) {
                    int deficit = (valueW2 + 80 + (sp * 3)) - col_W;
                    if (valueW2 > 120) {
                        int shrink = (std::min)(deficit, valueW2 - 120);
                        valueW2 -= shrink; deficit -= shrink;
                    }
                    remaining = col_W - valueW2 - (sp * 3);
                    if (remaining < 60) remaining = 60;
                }
            }
            
            // Split remaining: Address (30%) | Name (70%)
            int addressW2 = (int)(remaining * 0.30);
            if (addressW2 < 40) addressW2 = 40;
            int nameW2 = remaining - addressW2;
            if (nameW2 < 40) nameW2 = 40;

            int labelH = 20; int ctrlH = 22;
            int labelTop = absoluteY + (ROW_HEIGHT - labelH) / 2;
            int ctrlTop  = absoluteY + (ROW_HEIGHT - ctrlH) / 2;

            if (item.hAddress && IsWindow(item.hAddress)) SetWindowPos(item.hAddress, NULL, absoluteX, labelTop, addressW2, labelH, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur += addressW2 + sp;
            
            // Name uses all remaining space (hide Desc to save space)
            if (item.hLabel && IsWindow(item.hLabel)) SetWindowPos(item.hLabel, NULL, cur, labelTop, nameW2, labelH, SWP_NOZORDER | SWP_SHOWWINDOW);
            cur += nameW2 + sp;
            
            // Hide Desc to maximize Value space
            if (item.hDesc && IsWindow(item.hDesc)) {
                ShowWindow(item.hDesc, SW_HIDE);
            }
            if (item.hValue && IsWindow(item.hValue)) {
                SetWindowPos(item.hValue, NULL, cur, ctrlTop, valueW2, ctrlH, SWP_NOZORDER | SWP_SHOWWINDOW);
                // Force text refresh after resize
                SetWindowTextW(item.hValue, item.currentValue.c_str());
                InvalidateRect(item.hValue, NULL, TRUE);
            }
        }
        nextY = colY + ROW_HEIGHT + ROW_SPACING;
    };

    for (size_t i = 0; i < pdoItems.size(); ++i) {
        bool isOut = pdoItems[i].isOutput;
        int x = singleColumn ? leftX : (isOut ? leftX : rightX);
        int w = colWidth;
        int &yRef = singleColumn ? yStack : (isOut ? yLeft : yRight);
        int nextY = yRef;
        layoutItem(pdoItems[i], x, yRef, w, nextY);
        yRef = nextY;
    }

    {
        int maxBottomClient = 0;
        for (const auto &it : pdoItems) {
            if (it.hLabel && IsWindow(it.hLabel)) {
                RECT rcLabel2; GetWindowRect(it.hLabel, &rcLabel2);
                POINT pt = { rcLabel2.left, rcLabel2.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hValue && IsWindow(it.hValue)) {
                RECT rcValue2; GetWindowRect(it.hValue, &rcValue2);
                POINT pt = { rcValue2.left, rcValue2.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hEdit && IsWindow(it.hEdit)) {
                RECT rcEdit2; GetWindowRect(it.hEdit, &rcEdit2);
                POINT pt = { rcEdit2.left, rcEdit2.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
            if (it.hButton && IsWindow(it.hButton)) {
                RECT rcButton2; GetWindowRect(it.hButton, &rcButton2);
                POINT pt = { rcButton2.left, rcButton2.bottom };
                ScreenToClient(hGroupBox, &pt);
                if (pt.y > maxBottomClient) maxBottomClient = pt.y;
            }
        }

        int desiredClientH = maxBottomClient + bottomPadding;
        if (desiredClientH < minGroupHeight) desiredClientH = minGroupHeight;

        RECT adj = { 0, 0, groupWidth, desiredClientH };
        DWORD style = (DWORD)GetWindowLongPtr(hGroupBox, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongPtr(hGroupBox, GWL_EXSTYLE);
        AdjustWindowRectEx(&adj, style, FALSE, exstyle);
        int totalH = adj.bottom - adj.top;

        if (totalH != groupHeight) {
            groupHeight = totalH;
            RECT rr2; GetWindowRect(hGroupBox, &rr2);
            POINT pp2 = { rr2.left, rr2.top }; ScreenToClient(hParent, &pp2);
            SetWindowPos(hGroupBox, NULL, pp2.x, pp2.y, groupWidth, groupHeight, SWP_NOZORDER);
        }
        // Force parent and children to repaint immediately to avoid transient
        // drawing artifacts when controls are destroyed/created and moved.
        if (hParent && IsWindow(hParent)) {
            RedrawWindow(hParent, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
    }
    // Re-enable painting and show groupbox; force an immediate redraw of all children
    if (hParent && IsWindow(hParent)) SendMessageW(hParent, WM_SETREDRAW, TRUE, 0);
    if (hGroupBox && IsWindow(hGroupBox)) ShowWindow(hGroupBox, SW_SHOW);
    if (hParent && IsWindow(hParent)) RedrawWindow(hParent, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    isRebuilding = false;
}

void PDOProxy::Clear() {
    for (auto &item : pdoItems) {
        if (item.hLabel) { DestroyWindow(item.hLabel); item.hLabel = NULL; }
        if (item.hValue) { DestroyWindow(item.hValue); item.hValue = NULL; }
        if (item.hEdit) { DestroyWindow(item.hEdit); item.hEdit = NULL; }
        if (item.hButton) { DestroyWindow(item.hButton); item.hButton = NULL; }
        if (item.hAddress) { DestroyWindow(item.hAddress); item.hAddress = NULL; }
    }
    pdoItems.clear();
}
