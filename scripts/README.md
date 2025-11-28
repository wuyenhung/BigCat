# Scripts — 獨立運行說明

這個資料夾包含專案共用的 PowerShell 腳本（*.ps1）。建置、CTest 與 smoke wrapper 現在會以 `$PSScriptRoot` 推導 repo root；一般建議仍從 repo root 或 VS Code workspace task 啟動。

主要腳本用途

- `analyze_dump_and_prepare_ai_prompt.ps1`：分析最新 dump，產生可直接送給 AI 的 prompt。
- `send_ai_request.ps1`：送出 AI 請求，預設使用 `gpt-5.4`，可用 `-Model` 或 `OPENAI_MODEL` 覆寫。
- `run_cmake.ps1`：CMake configure/build/workflow wrapper。
- `run_ctest.ps1`：CTest wrapper，會處理 configure/build/test-dir 與工具解析。
- `run_smoke_tests.ps1`：建置 smoke 目標並執行 `ctest -L smoke`，同時輸出 `build/smoke_test_latest.log`。
- `Invoke-BuildArtifactCleanup.ps1`：清理可丟棄的建置中間檔與測試暫存。
- `share_http.ps1`：協助 HTTP 存取或分享設定。

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
