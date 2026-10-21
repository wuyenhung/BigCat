# Scripts — 獨立運行說明

這個資料夾包含專案共用的 PowerShell 腳本（*.ps1）。建置、CTest 與 smoke wrapper 現在會以 `$PSScriptRoot` 推導 repo root；一般建議仍從 repo root 或 VS Code workspace task 啟動。

主要腳本用途

- `analyze_dump_and_prepare_ai_prompt.ps1`：分析最新 dump，產生可直接送給 AI 的 prompt。
- `prepare_rotor_glb.ps1`：拆分 ka-52.glb 主旋翼 mesh（依三角形 centroid Y 把合併的 6 片槳葉+6 槳尖拆成上下各 3 片），重建為 `MainRotorUpper`/`MainRotorLower` 兩節點供 Web 檢視器做上下正反轉動畫；只追加 BIN 資料、原幾何不動。
- `prepare_ah64_rotors.ps1`：把 AH-64.glb 主旋翼（`MainRotor`，繞本地 Z）與尾旋翼（`TailRotor`，繞本地 X）的幾何位置原地平移重定原點，使碟心回到節點原點（節點 translation 補償、世界位置不變），供 Web 檢視器做旋轉動畫；只重寫位置位元組、不變大小。
- 注意：上述腳本內可執行字串為純 ASCII（PS 5.1 對無 BOM UTF-8 的中文會誤讀成 ANSI）。
- `send_ai_request.ps1`：送出 AI 請求，預設使用 `gpt-5.4`，可用 `-Model` 或 `OPENAI_MODEL` 覆寫。
- `run_cmake.ps1`：CMake configure/build/workflow wrapper。
- `run_ctest.ps1`：CTest wrapper，會處理 configure/build/test-dir 與工具解析。
- `run_smoke_tests.ps1`：建置 smoke 目標並執行 `ctest -L smoke`，同時輸出 `build/smoke_test_latest.log`。
- `i18n_report.ps1`：Web UI 多語系目錄覆蓋率報告。掃描 `resources/web` 的 HTML/JS，抽出含中日韓字元的 UI 原文（＝翻譯目錄的 key），比對 `resources/web/locales/*.json`，回報各語系覆蓋率與缺譯清單；`-Detailed` 附來源檔案、`-OutFile` 另存報告、`-FailOnMissing` 缺譯時 exit 1（可接 CI）。機制說明見 `docs/WEB_I18N.md`。
- `Invoke-BuildArtifactCleanup.ps1`：清理可丟棄的建置中間檔與測試暫存。
- `share_http.ps1`：協助 HTTP 存取或分享設定。
- `measure_baseline.ps1`：P0 量測基線腳本，透過 HTTP API 收集 EtherCAT 週期穩定性指標（commsTime / actualCycleTime / maxObservedCycleTime），產生固定格式 JSON 基線報告，支援回歸比較。
- `capture_etw_trace.ps1`：P2 ETW/WPR 輔助腳本，自動化 WPR 核心追蹤擷取（CPU/DPCISR profiles），覆蓋 connect/idle/motion 三場景，內建 WPA 分析指引。
- `audit_nic_settings.ps1`：P3 NIC driver 設定稽核腳本，唯讀檢查 Interrupt Moderation、EEE、Flow Control、RSS 等進階屬性，支援 PASS/FAIL/WARN 標記與 `-Apply` opt-in 套用。
- `set_browser_affinity.ps1`：對手動開啟的瀏覽器 process 即時套用 CPU affinity 規則。需系統管理員權限。
- `install_browser_affinity_permanent.ps1`：透過 Windows Image File Execution Options (IFEO) 寫入 `CpuAffinityMask`，讓瀏覽器每次啟動時自動套用 CPU 親和性規則。設定一次後永久有效，不需背景程序或手動執行。

主要原則

- 新增或維護腳本時，應讓腳本能從任何工作目錄運作。
- 腳本應以 `-ProjectRoot` 或基於 `$PSScriptRoot` 的相對路徑來尋找專案根目錄與建置目錄。
- 不要假設 VS Code 環境變數存在；如要在 VS Code 裡執行，請使用執行參數而非依賴 workspace 變數。

GPT-5.4 使用方式

```powershell
# 使用預設 GPT-5.4 模型送出 crash prompt
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\send_ai_request.ps1

# 明確指定模型
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\send_ai_request.ps1 -Model gpt-5.4

# 或使用環境變數設定預設模型
$env:OPENAI_MODEL = 'gpt-5.4'
```

常用驗證範例請從專案根目錄執行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_cmake.ps1 -Action configure -Preset default
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_cmake.ps1 -Action build -Preset default
.\scripts\build_incremental-Crash.ps1 -NonInteractive
.\scripts\run_smoke_tests.ps1 -BuildDir .\build -Config Release
```

專用建置入口：

```powershell
.\scripts\build_ForceFull.ps1
.\scripts\build_incremental-Crash.ps1 -NonInteractive
.\scripts\build_release_hardened.ps1
```

CI 建議

- 在 CI runner 上安裝必要工具 (cmake、clang/llvm 或 strip 等) 以便 `-StripBinary` 可運作。
- 若使用 Windows Hosted runner，請確保 CMake 和 build 工具 (Visual Studio / MSBuild) 已安裝並在 PATH。

AI 自動化輸出位置

- `.vscode/last_crash_ai_request.txt`：送出前的 prompt。
- `.vscode/crash_ai_response.txt`：模型回覆與 patch 建議。
