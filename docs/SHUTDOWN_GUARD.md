# 關機防護（ShutdownGuard）

## 目的

BigCat 執行期間若 Windows 關機、重新啟動、登出或閒置睡眠，會直接中斷 EtherCAT PDO 週期：
伺服驅動器失去週期性命令、Stewart 平台可能停在任意姿態。因此程式在執行期間主動封鎖
這些系統事件，直到使用者正常關閉 BigCat（依既有規則，連接中必須先斷開 EtherCAT 才能關閉）。

實作：`include/core/ShutdownGuard.h`、`src/core/ShutdownGuard.cpp`（`bigcat_core`）。

## 機制

| 機制 | 作用 |
| --- | --- |
| `ShutdownBlockReasonCreate(hwnd, ...)` | 向系統註冊封鎖原因；關機畫面會顯示「BigCat 正在執行運動控制…」。於 `MainWindow::Create()` 註冊，`WM_CLOSE`（正常關閉）時移除。 |
| 主視窗 `WM_QUERYENDSESSION` 回傳 `FALSE` | 系統取消本次關機 / 重新啟動 / 登出（見 `MainWindow::HandleMessage`）。 |
| `WM_ENDSESSION` | `wParam == FALSE`（關機被取消）→ 重新註冊封鎖原因；`wParam != FALSE`（session 真的結束）→ 釋放封鎖原因。 |
| `SetThreadExecutionState(ES_CONTINUOUS \| ES_SYSTEM_REQUIRED)` | 避免系統因閒置進入睡眠（螢幕仍可關閉）。綁定主執行緒，程式結束即失效；由 `main.cpp` 啟動時設定。 |
| `HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU\NoAutoRebootWithLoggedOnUsers = 1` | 選用：Windows Update 不因更新在使用者登入時自動重新啟動（需系統管理員權限）。 |

`WM_QUERYENDSESSION` 由系統送往所有頂層視窗，任一視窗回傳 `FALSE` 即取消關機；
MainWindow 為主視窗且常駐（含最小化到系統匣時），因此可穩定攔截。

## INI 設定（`BigCat.ini`）

```ini
[Safety]
BlockSystemShutdown=1            ; 1=封鎖關機/重新啟動/登出（預設），0=不封鎖
KeepSystemAwake=1                ; 1=防止系統閒置睡眠（預設），0=不防止
BlockWindowsUpdateAutoReboot=0   ; 1=寫入 Windows Update 不自動重啟原則（預設 0，需管理員）
```

- 三者皆由 `AppConfig` 讀取，缺鍵時 `EnsureDefaultIniFile()` 會補上預設值。
- `BlockWindowsUpdateAutoReboot=0` 時**完全不碰登錄檔**（不會刪除既有原則）；
  若曾設為 1 而要還原：

  ```powershell
  reg delete "HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU" /v NoAutoRebootWithLoggedOnUsers /f
  ```

## 無法封鎖的情況（Windows 設計限制）

1. 使用者在「正在封鎖關機」畫面選擇「仍要關機」→ 強制關機。
2. 系統 critical shutdown（`WM_QUERYENDSESSION` 的 `lParam` 含 `ENDSESSION_CRITICAL`，
   例如 UPS/電源事件）：本模組記錄後放行。
3. 使用者主動選擇「睡眠 / 休眠」或直接斷電（`ES_SYSTEM_REQUIRED` 只擋閒置逾時）。
4. Windows Update 若已達強制安裝期限（deadline），仍可能強制重新啟動；
   `BlockWindowsUpdateAutoReboot=1` 可降低發生機率，但非絕對保證。

## 驗證

```powershell
ctest --preset smoke -R test_shutdown_guard     # 決策邏輯（不需視窗）
```

手動驗證：啟動 BigCat（`Startup\AutoTrigger=1` 進入模擬或實機連線）→ 開始功能表選「關機」
→ 應出現「BigCat 正在阻止關機」畫面；於 BigCat 按關閉（並先斷開連線）後再關機應可正常完成。
狀態列會顯示 `[Safety] ✓ 已封鎖 Windows 關機/重新啟動…`。
