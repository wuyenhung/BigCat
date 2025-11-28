// PDOProxy: renamed from PDODisplayManager
// This header contains the full declaration of the display manager class
// under the new name `PDOProxy` so the repository can move to the new
// identifier in a single coordinated change.
#pragma once

#include <vector>
#include <string>
#include <cstdint>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <chrono>
#include <unordered_map>

// Layout constants used by PDOProxy
const int GROUPBOX_INNER_TOP = 20;
const int ROW_HEIGHT = 25;
const int ROW_SPACING = 5;
const int bottomPadding = 10;
const int minGroupHeight = 100;

// PDO item structure - single PDO entry representation used by UI and logic
struct PDOItem {
	std::wstring name;
	uint16_t index = 0;
	uint8_t subindex = 0;
	uint8_t bitLength = 0;
	bool isOutput = false;
	int byteOffset = 0;
	bool isEditable = false;

	std::wstring currentValue;
	std::wstring displayValue;

	HWND hLabel = NULL;
	HWND hValue = NULL;
	HWND hEdit = NULL;
	HWND hButton = NULL;
	HWND hAddress = NULL;
	HWND hTooltip = NULL;
	HWND hDesc = NULL;

	std::wstring dataType;
	std::wstring unit;
	std::wstring minValue;
	std::wstring maxValue;
	std::wstring defaultValue;
	std::wstring address;
	std::wstring description;

	// recent-write bookkeeping removed: UI is driven only by periodic snapshots
};

struct EtherCATPDOEntry {
	bool isOutput;
	bool isEditable;
	std::wstring name;
	std::wstring dataType;
	std::wstring unit;
	std::wstring minValue;
	std::wstring maxValue;
	std::wstring defaultValue;
	std::wstring currentValue;
};

struct EtherCATPDOConfig {
	std::vector<EtherCATPDOEntry> entries;
};

class EtherCATMaster; // forward

class PDOProxy {
public:
	PDOProxy();
	~PDOProxy();

	// Initialization & UI lifecycle
	void Initialize(HWND parent, HINSTANCE hInstance, HFONT hFont,
					int startY, int leftMargin, int width, int height = 260);
	void RebuildPDOControls();
	void Show(bool visible);
	void Clear();

	// Layout helpers
	void ResizeWidth(int width);
	void ResizeHeight(int height);
	void SetTop(int top);
	int GetHeight() const;

	// PDO mapping helpers
	bool RecalculateOffsetsFromSDO(EtherCATMaster* master, int slaveIndex);
	bool WriteToIOMap(EtherCATMaster* master, int slaveIndex, const std::wstring& itemName, int32_t value);

	// Accessors used by MainWindow and other modules
	std::vector<PDOItem> pdoItems;
	std::vector<std::wstring> GetEditableItems() const;
	// Inline accessor to avoid link-time missing symbol during incremental migration
	inline HWND GetGroupBox() const { return hGroupBox; }

	// Editing helpers
	void UpdateItemValue(size_t index, const std::wstring &newValue);
	void SetItemEditable(size_t index, bool editable);
	void SetItemButtonText(size_t index, const std::wstring &btnText);
	void SelectSlave(int slaveIndex);
	void BuildPDOItemsFromSDO(const EtherCATPDOConfig &pdoConfig);
	void RefreshItemValues(const std::vector<std::wstring> &newValues);
	void SetItemUnit(size_t index, const std::wstring &unit);
	void SetItemMinMax(size_t index, const std::wstring &minVal, const std::wstring &maxVal);
	void SetItemDefault(size_t index, const std::wstring &defVal);
	void SetItemDataType(size_t index, const std::wstring &dataType);
	void EnableItem(size_t index, bool enable);
	void DisableAllItems();
	void EnableAllItems();
	void SetItemColor(size_t index, COLORREF color);
	void SetItemBackgroundColor(size_t index, COLORREF bgColor);
	void ResetItemColors();
	void UpdateItemDisplay(size_t index);
	void RefreshDisplay();
	// Enable/disable all "Apply" buttons created for editable PDO items
	void SetApplyButtonsEnabled(bool enabled);
	void SetParent(HWND parent);
	void SetFont(HFONT hFont);
	void SetGroupBoxText(const std::wstring &text);
	void GetItemRect(size_t index, RECT &rect);
	void GetGroupBoxRect(RECT &rect);
	void SetItemPosition(size_t index, int x, int y);
	void MoveItem(size_t index, int dx, int dy);
	void ResizeItem(size_t index, int newWidth, int newHeight);
	void SetItemRange(size_t index, const std::wstring &minVal, const std::wstring &maxVal);
	void SetItemValue(size_t index, const std::wstring &value);
	void SetItemName(size_t index, const std::wstring &name);

	// Format helper: produce decimal + hex representation for UI display
	std::wstring FormatValue(int32_t value, uint8_t bitLength);

private:
	void CreatePDOItem(HWND parent, HINSTANCE hInst, HFONT font,
					  PDOItem& item, int itemIndex, int y, int leftMargin, int width,
					  const std::wstring& initialValue = L"0");

	HWND hParent = NULL;
	HWND hGroupBox = NULL;
	HWND hOutputHeader = NULL;
	HWND hInputHeader = NULL;
	int currentY = 0;
	bool isInitialized = false;
	int selectedSlave = -1;
	int lastMappedSlaveOffsets = -1;
	std::unordered_map<int, std::wstring> lastIODump;

	int groupLeftMargin = 0;
	int groupWidth = 0;
	int groupHeight = 0;
	bool isRebuilding = false;
};
