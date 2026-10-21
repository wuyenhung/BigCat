// ShutdownGuard.cpp — 程式執行期間封鎖 Windows 關機 / 重新啟動 / 閒置睡眠。
// 實作說明見 include/core/ShutdownGuard.h。

#include "core/ShutdownGuard.h"

#include <sstream>

namespace CoreHelpers {
namespace ShutdownGuard {
namespace {

// 顯示在 Windows「正在封鎖關機」畫面的原因文字（盡量精簡，讓使用者知道要做什麼）。
constexpr wchar_t kBlockReason[] =
    L"BigCat 正在執行運動控制（EtherCAT / 6-DoF Stewart 平台）。\n"
    L"請先關閉 BigCat（需先斷開 EtherCAT 連線）再關機或重新啟動。";

constexpr wchar_t kUpdatePolicyKey[] =
    L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate\\AU";
constexpr wchar_t kUpdatePolicyValue[] = L"NoAutoRebootWithLoggedOnUsers";

bool g_enabled = true;
bool g_keepAwake = true;

// 註冊封鎖原因（不產生 log 訊息）。hwnd 必須屬於呼叫執行緒。
bool InstallReason(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    return ShutdownBlockReasonCreate(hwnd, kBlockReason) != FALSE;
}

} // namespace

void SetEnabled(bool enabled) {
    g_enabled = enabled;
}

bool IsEnabled() {
    return g_enabled;
}

void SetKeepSystemAwake(bool enabled) {
    g_keepAwake = enabled;
    // ES_CONTINUOUS：狀態持續有效，直到再次呼叫或執行緒結束。
    // ES_SYSTEM_REQUIRED：阻止系統因閒置進入睡眠（不影響螢幕關閉）。
    SetThreadExecutionState(enabled ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS);
}

bool IsKeepSystemAwake() {
    return g_keepAwake;
}

bool Install(HWND hwnd, std::string& logOut) {
    logOut.clear();
    if (!g_enabled) {
        logOut = "[ShutdownGuard] 關機封鎖未啟用（INI [Safety] BlockSystemShutdown=0）";
        return false;
    }
    if (InstallReason(hwnd)) {
        logOut = "[ShutdownGuard] OK 已封鎖 Windows 關機/重新啟動（程式執行期間）";
        return true;
    }
    std::ostringstream ss;
    ss << "[ShutdownGuard] WARN ShutdownBlockReasonCreate 失敗 (err="
       << GetLastError() << ")，仍會在 WM_QUERYENDSESSION 攔截關機";
    logOut = ss.str();
    return false;
}

void Remove(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    ShutdownBlockReasonDestroy(hwnd);
}

bool HandleQueryEndSession(HWND hwnd, LPARAM lParam, std::string& logOut) {
    logOut.clear();

    // critical shutdown（UPS/電源事件等）依設計無法封鎖，僅記錄並放行。
    const bool critical = (lParam & ENDSESSION_CRITICAL) != 0;
    if (critical) {
        logOut = "[ShutdownGuard] WARN 系統 critical shutdown（無法封鎖）— session 即將結束";
        return false;
    }

    if (!g_enabled) {
        logOut = "[ShutdownGuard] 關機封鎖未啟用，允許關機/重新啟動";
        return false;
    }

    // 重新註冊封鎖原因：Windows 在關機畫面顯示此文字。
    // 即使註冊失敗（例如非預期視窗狀態）仍回傳封鎖，避免運動中的平台被關機。
    if (!InstallReason(hwnd)) {
        std::ostringstream ss;
        ss << "[ShutdownGuard] WARN ShutdownBlockReasonCreate 失敗 (err="
           << GetLastError() << ")，仍封鎖本次關機";
        logOut = ss.str();
    }
    if (logOut.empty()) {
        logOut = "[ShutdownGuard] 已攔截 Windows 關機/重新啟動（BigCat 執行中）";
    }
    return true;
}

void HandleEndSession(HWND hwnd, WPARAM ending, LPARAM lParam) {
    const bool critical = (lParam & ENDSESSION_CRITICAL) != 0;
    if (ending) {
        std::ostringstream ss;
        ss << "[ShutdownGuard] WM_ENDSESSION: session 即將結束 (critical="
           << (critical ? 1 : 0) << ")，釋放關機封鎖原因";
        OutputDebugStringA((ss.str() + "\n").c_str());
        Remove(hwnd);
        return;
    }
    // 關機被取消（其他程式或使用者）：重新註冊封鎖原因，保持防護。
    InstallReason(hwnd);
}

bool ApplyUpdateNoAutoRebootPolicy(bool enabled, std::string& logOut) {
    logOut.clear();
    if (!enabled) {
        return true;
    }

    HKEY hKey = nullptr;
    LONG result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUpdatePolicyKey, 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                  &hKey, nullptr);
    if (result != ERROR_SUCCESS) {
        std::ostringstream ss;
        ss << "[ShutdownGuard] WARN 無法建立 Windows Update 原則機碼 (err=" << result
           << ")，需以系統管理員執行";
        logOut = ss.str();
        return false;
    }

    const DWORD value = 1;
    result = RegSetValueExW(hKey, kUpdatePolicyValue, 0, REG_DWORD,
                            reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);

    std::ostringstream ss;
    if (result == ERROR_SUCCESS) {
        ss << "[ShutdownGuard] OK Windows Update 已設為不因更新自動重新啟動"
              "（NoAutoRebootWithLoggedOnUsers=1）";
        logOut = ss.str();
        return true;
    }
    ss << "[ShutdownGuard] WARN 無法寫入 Windows Update 原則 (err=" << result
       << ")，需以系統管理員執行";
    logOut = ss.str();
    return false;
}

} // namespace ShutdownGuard
} // namespace CoreHelpers
