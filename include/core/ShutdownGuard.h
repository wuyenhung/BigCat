#pragma once

// ShutdownGuard — 程式執行期間封鎖 Windows 關機 / 重新啟動 / 閒置睡眠。
//
// 機制：
//   1. ShutdownBlockReasonCreate()：向系統註冊「封鎖原因」，關機畫面會顯示給使用者。
//   2. 主視窗 WM_QUERYENDSESSION 回傳 FALSE：系統取消本次關機 / 重新啟動 / 登出。
//   3. SetThreadExecutionState(ES_SYSTEM_REQUIRED)：避免系統閒置進入睡眠（螢幕仍可關閉）。
//
// 限制（Windows 設計如此，無法繞過）：
//   - 使用者在「正在封鎖關機」畫面仍可選擇「仍要關機」強制關機。
//   - 系統 critical shutdown（lParam 含 ENDSESSION_CRITICAL，例如 UPS/電源事件）
//     無法封鎖，本模組僅記錄並放行。
//   - 使用者主動選擇「睡眠 / 休眠」不會被 ES_SYSTEM_REQUIRED 阻止。
//
// 開關由 BigCat.ini [Safety] 控制（見 AppConfig），預設：封鎖關機 = 開。

#include <string>
#include <windows.h>

namespace CoreHelpers {
namespace ShutdownGuard {

// 總開關（INI [Safety] BlockSystemShutdown）。須在 Install() 前設定。
void SetEnabled(bool enabled);
bool IsEnabled();

// 防止系統閒置睡眠（螢幕仍可關閉）。狀態綁定呼叫執行緒，於主執行緒呼叫一次即可；
// 執行緒結束（程式退出）時自動失效。
void SetKeepSystemAwake(bool enabled);
bool IsKeepSystemAwake();

// 對 hwnd 註冊關機封鎖原因。hwnd 必須屬於呼叫執行緒（UI 執行緒）。
// 成功時 logOut 填入說明訊息；失敗（權限/視窗無效）不影響後續封鎖判斷。
bool Install(HWND hwnd, std::string& logOut);
void Remove(HWND hwnd);

// WM_QUERYENDSESSION 處理。回傳 true 表示本次關機 / 重新啟動應被封鎖
// （window proc 需回傳 FALSE）。
bool HandleQueryEndSession(HWND hwnd, LPARAM lParam, std::string& logOut);

// WM_ENDSESSION 處理：ending != FALSE 表示 session 真的結束（含無法封鎖的
// critical shutdown）；ending == FALSE 表示關機被取消，重新註冊封鎖原因。
void HandleEndSession(HWND hwnd, WPARAM ending, LPARAM lParam);

// Windows Update「使用者登入時不自動重新啟動」原則
// （HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU\
//   NoAutoRebootWithLoggedOnUsers = 1）。需系統管理員權限。
// enabled == false 時完全不碰登錄檔（不會刪除既有原則）。
bool ApplyUpdateNoAutoRebootPolicy(bool enabled, std::string& logOut);

} // namespace ShutdownGuard
} // namespace CoreHelpers
