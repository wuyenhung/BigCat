// ShutdownGuard 決策邏輯驗證（不需要真實視窗）。
// 驗證重點：
//   - 啟用時：一般 WM_QUERYENDSESSION 必須封鎖（回傳 true）— 即使視窗尚未註冊。
//   - critical shutdown（ENDSESSION_CRITICAL）：依 Windows 設計無法封鎖，必須放行。
//   - 停用（INI [Safety] BlockSystemShutdown=0）：放行。
//   - Windows Update 原則未啟用時不得觸碰登錄檔。

#include "core/ShutdownGuard.h"

#include <string>

int main()
{
    using namespace CoreHelpers::ShutdownGuard;
    std::string log;

    // 預設：啟用封鎖。
    SetEnabled(true);
    if (!IsEnabled()) return 1;

    // 一般關機/重新啟動 → 封鎖（無有效視窗時仍須封鎖，只是無法顯示原因）。
    if (!HandleQueryEndSession(nullptr, 0, log)) return 2;

    // 系統 critical shutdown → 無法封鎖。
    if (HandleQueryEndSession(nullptr, ENDSESSION_CRITICAL, log)) return 3;

    // 停用 → 放行。
    SetEnabled(false);
    if (HandleQueryEndSession(nullptr, 0, log)) return 4;

    // 停用時 Install 失敗並回報 INI 開關名稱。
    log.clear();
    if (Install(nullptr, log)) return 5;
    if (log.find("BlockSystemShutdown") == std::string::npos) return 6;

    // 閒置睡眠防護旗標。
    SetKeepSystemAwake(false);
    if (IsKeepSystemAwake()) return 7;
    SetKeepSystemAwake(true);
    if (!IsKeepSystemAwake()) return 8;

    // Windows Update 原則：未啟用時不寫登錄檔，僅回報。
    log.clear();
    if (!ApplyUpdateNoAutoRebootPolicy(false, log)) return 9;
    if (!log.empty()) return 10;

    // 還原預設值，避免影響同行程後續檢查。
    SetEnabled(true);
    return 0;
}
