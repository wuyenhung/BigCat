# CLAUDE.md — WuBigCat（給 Claude Code）

> 本專案由 GitHub Copilot 與 Claude Code **混合開發**。完整專案指南在 [AGENTS.md](AGENTS.md)（兩工具共用、單一事實來源）；本檔只寫 Claude Code 專屬的載入順序、記憶與 slash command 用法，其餘事項一律以 AGENTS.md 為準。

## 專案一句話

BigCat — Windows 上的 SOEM EtherCAT Master，6-DoF Stewart 平台運動控制（C++17 / Win32 / CiA402）。詳見 AGENTS.md「專案概述」。

## 給 Claude Code 的載入順序（必讀）

1. **先讀 [AGENTS.md](AGENTS.md)** — 專案、架構、建置/測試、EtherCAT 規則、編碼慣例（UTF-8 no BOM）全部寫在那裡，本檔不重複。
2. **動工前讀 `.github/project-memory/repo-facts.md`**（可只讀開頭＋觸及子系統段落）— repo 級耐久記憶的**唯一事實來源**，Copilot 與 Claude Code 共用同一份。
3. Copilot 專屬規則檔案（`.github/copilot-instructions.md`、`.github/instructions/*.instructions.md`、`.github/agents/*.agent.md`、`.github/prompts/*.prompt.md`）**是同一套專案政策的另一種格式**，主要給 Copilot 的 rules/agent 用。Claude Code 已由 `.claude/` 對應檔案承接其精神；若 `.claude/` 內容與 `.github/` 有出入，以 `.github/`（較完整、更新較勤）為準並回報差異。

## 建置與測試（速查）

```powershell
cmake --preset default && cmake --build --preset default      # configure + build
ctest --preset smoke                                          # smoke 回歸
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build_incremental-Crash.ps1   # 慣用增量驗證
.\scripts\run_smoke_tests.ps1 -BuildDir .\build -Config Release   # smoke 回歸 wrapper
```

- 日常驗證：`scripts/build_incremental-Crash.ps1`（預設 Release + crash handler + PDB）。
- smoke 是回歸入口（`ctest -L smoke`）；IntelliSense 診斷常誤報，**以增量建置 + smoke 為準**。
- 更多細節（preset、wrapper 差異、何時用 ForceFull/hardened）見 AGENTS.md「建置與測試命令」。

## 記憶策略（Claude Code 專屬）

- repo 級知識一律寫進 `.github/project-memory/repo-facts.md`（與 Copilot 共用，需 commit 才跨機器）。新增條目：日期 + 動詞標籤 + 事實/根因 + 受影響檔案 + Validation。
- Claude Code 原生記憶目錄 `~/.claude/projects/.../memory/` **只放**：使用者個人偏好、跨 repo 的工作方式（如「修改期間極簡輸出」）與本 repo 規則的捷徑指標，**不放** repo 事實（避免兩份拷貝分歧）。
- 行為/工作流程/建置政策變更時，同一個 patch 更新 AGENTS.md、repo-facts.md 與相關 docs。

## Slash Commands

| Command | 對應 Copilot 產物 | 用途 |
| --- | --- | --- |
| `/code-review` | `.github/prompts/code-review.prompt.md` | 運動安全/CiA402/race 風險審查 |
| `/debug` | `.github/prompts/debug.prompt.md` | EtherCAT/運動除錯（root cause → 證據 → 修復 → 驗證） |
| `/crash-triage` | `.github/prompts/crash-triage.prompt.md` | 最新 crash dump 分析 |
| `/generate-tests` | `.github/prompts/generate-tests.prompt.md` | 產生聚焦測試 |

另有 `.claude/agents/`（`motion-expert`、`planner`）承接 `.github/agents/` 的專家角色。

## 寫碼注意（Claude Code 特別容易被唸的）

- 檔案**一律 UTF-8 without BOM**（含中文檔，勿轉 ANSI/Big5/BOM）；MSVC 用 `/utf-8`。
- 近即時 PDO 週期是第一級約束：cyclic 路徑禁止 blocking call / heap churn / console / 長 mutex；勿削弱既有 affinity/priority/timer 調校。
- 小而精的 root-cause 修改；UI/HTTP 動作走 AppController 路由；保留大檔拆分結構。
- 驗證跑完（含被中斷）後**清理可丟棄的 log 與暫存檔**（repo-facts.md 第 5 條慣例）。
- 修改會動到真實運動平台：從小振幅測起、軟體限位強制、急停第一等。授權/TPM 保護不得繞過。
