# Slash Command 索引（承接 .github/prompts/）

此目錄讓 Claude Code 的 `/` slash command 對應既有 Copilot prompt 模板。完整規則在 `.github/prompts/` 的對應檔案（同一專案政策），此檔只列名稱與用途。

- `/code-review` — 審查 C++ 程式碼的運動控制風險與回歸。承接 `.github/prompts/code-review.prompt.md`。以 #selection 或 codebase 為範圍；優先：motion safety regressions、CiA402 misuse、PDO data consistency、thread/race/jitter risks、resource lifetime。回傳 file+line、impact、smallest safe fix。
- `/debug` — 除錯 EtherCAT、CiA402 與運動控制問題。承接 `.github/prompts/debug.prompt.md`。診斷：#selection 或相關程式碼。檢查 slave discovery/AL state/PDO mapping、CiA402 transitions/fault recovery、motion validation/limits/feedback、thread scheduling/PDO jitter。先讀 repo-facts.md 再選驗證命令。
- `/crash-triage` — 分析最新 crash dump。承接 `.github/prompts/crash-triage.prompt.md`。檢查 `.vscode/last_crash_ai_request.txt`、`.vscode/crash_ai_response.txt`、最新 dump-analysis 輸出；root cause → 證據 → 修補安全性評估 → 最小替代方案。
- `/generate-tests` — 為運動控制程式碼產生聚焦測試。承接 `.github/prompts/generate-tests.prompt.md`。少量高價值案例：boundary/invalid、CiA402 transitions、PDO/motion math、safety。放 `tests/`，沿用既有無框架風格。
