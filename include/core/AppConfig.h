#pragma once

#include <string>
#include <cstdint>

namespace AppConfig {

struct PdoFilterDynamicViolationConfig {
    double velocityThresholdScale = 0.60;
    double accelerationThresholdScale = 1.20;
    double velocityViolationDelaySec = 0.50;
    double accelerationViolationDelaySec = 0.20;
};

std::wstring GetAppIniPath();
bool EnsureDefaultIniFile();

bool ReadBool(const wchar_t* section, const wchar_t* key, bool defaultValue);
double ReadDouble(const wchar_t* section, const wchar_t* key, double defaultValue);
std::wstring ReadServerPassword();
PdoFilterDynamicViolationConfig ReadPdoFilterDynamicViolationConfig();

// Read the MMF1 input mapping name override from [MMF] InputName.
// When the key is missing, empty, or still set to a built-in default
// (OUTPUT_MMF_6DOF / OUTPUT_MMF_DEG), returns fallback (the mode-dependent
// hardcoded name) so existing behavior is preserved.
std::wstring ReadMmf1InputName(const std::wstring& fallback);

// Read the configured cyclic operation mode from [Motion] OperationMode.
// Valid values: "CSP" (mode 8) or "CSV" (mode 9). Defaults to CSP.
// Returns the CiA402 mode constant (8 or 9).
int8_t ReadCyclicOperationMode();

// Read whether external E-Stop (0x60FD bit0) detection is enabled.
// [Motion] ExternalEstopEnabled = 1 (default) or 0.
bool ReadExternalEstopEnabled();

// Read the reverse-jog speed threshold for AutoDownToOrigin workflow.
// [Motion] ReverseJogSpeedThreshold = -2.5 (default, unit: deg/s).
// When the actual mechanical speed rises above this value (closer to zero),
// the axis is considered stalled/blocked and jog is stopped.
double ReadReverseJogSpeedThreshold();

// Read the reverse-jog torque threshold for AutoDownToOrigin workflow
// (used for X1S/X5_7SEtherCAT drives instead of speed threshold).
// [Motion] ReverseJogTorqueThreshold = 400 (default, unit: 0.1% rated torque).
// When actual torque (0x6077) exceeds this value, the axis is considered blocked.
double ReadReverseJogTorqueThreshold();

// [Safety] BlockSystemShutdown = 1 (default): 程式執行期間封鎖 Windows 關機/重新啟動
// （WM_QUERYENDSESSION 回 FALSE + ShutdownBlockReasonCreate）。
bool ReadBlockSystemShutdown();

// [Safety] KeepSystemAwake = 1 (default): 程式執行期間防止系統閒置進入睡眠
// （螢幕仍可關閉）。僅影響本行程，結束後自動失效。
bool ReadKeepSystemAwake();

// [Safety] BlockWindowsUpdateAutoReboot = 0 (default): 設定 Windows Update
// 「使用者登入時不自動重新啟動」原則（需系統管理員權限）。
bool ReadBlockWindowsUpdateAutoReboot();

// [Compat] ForceWin10Mode = 0 (預設，停用): 強制採用 Windows 10 的週期等待策略
// （high-resolution waitable timer + 200μs TSC spin margin），即使在 Win11 也一樣。
// =1 時 `g_isWin11` 會被覆寫為 false，等於把 `!g_isWin11` guard 全部視為成立。
// 用途：釐清平台差異、或在 Win11 的 waitable-timer 行為已修復時作為回退開關。
// 由 `EtherCATMaster.Cyclic.cpp::DetectOSVersion()` 在 CyclicTask 啟動時讀取一次，
// 因此修改後需重新啟動程式（或重新連線）才生效。
bool ReadForceWin10Compat();

} // namespace AppConfig