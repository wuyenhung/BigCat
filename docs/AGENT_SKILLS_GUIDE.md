# WuSOEM_C++ Agent Skills 使用手冊
# WuSOEM_C++ Agent Skills User Guide

本文件只保留 Copilot 客製化的入口說明，不重複專案規範、EtherCAT/CiA402 細節或 agent/prompt 內容。
This document is an index for Copilot customization only. It does not repeat project rules, EtherCAT/CiA402 details, or full agent/prompt content.

---

## 單一事實來源 | Single Sources of Truth

| 主題 | 主要文件 | 用途 |
|------|----------|------|
| 專案級 AI 規範 | `.github/copilot-instructions.md` | 專案背景、程式碼原則、EtherCAT/CiA402 規範、PDO 執行緒穩定化、prompt 撰寫原則 |
| 專案級 AI 記憶 | `.github/project-memory/repo-facts.md` | 已驗證的 repo 事實、build/test 入口、持續維護的 AI 專案知識 |
| 自訂 agent 角色 | `.github/agents/*.agent.md` | 定義角色邊界、適用情境與輸出格式 |
| 任務 prompt | `.github/prompts/*.prompt.md` | 定義任務範圍與額外要求 |
| 專案目錄索引 | `docs/PROJECT_STRUCTURE_GUIDE.md` | 專案結構與主要檔案位置 |

原則：新增或修改文件時，先判斷是否已有主文件承載同一資訊，避免在多個檔案重複維護。

---

## 何時用什麼 | What To Use When

| 需求 | 建議 |
|------|------|
| 想定義全專案 AI 行為 | 改 `.github/copilot-instructions.md` |
| 想新增可重用任務 | 在 `.github/prompts/` 新增或修改 prompt |
| 想新增專家角色 | 在 `.github/agents/` 新增或修改 agent |
| 想查專案目錄與腳本位置 | 讀 `docs/PROJECT_STRUCTURE_GUIDE.md` |

目前可用項目：
- `/code-review`
- `/debug`
- `/generate-tests`
- `@motion-expert`
- `@planner`

---

## 記憶同步 | Memory Sync

- 這個 repo 沒有可在 VS Code 開啟瞬間執行的 Copilot 啟動 hook；這裡的「自動同步」是指第一次實質 AI 對話時執行記憶檢查與遷移。
- 第一次實質對話時，先檢查 `/memories/repo/`，把有長期價值且已驗證的 repo 事實搬到 `.github/project-memory/repo-facts.md`。
- 任務過程中若產生新的 repo 級已驗證知識，需在同一 patch 更新 `.github/project-memory/repo-facts.md`。
- 若候選記憶有疑問、與既有 repo 記憶衝突，或看起來像是未驗證的更佳做法，先問使用者，再決定是否寫入 `.github/project-memory/repo-facts.md`。
- 想跨電腦保留記憶，仍需把 `.github/project-memory/` 與相關 `.github` 規則檔納入 git commit/push。

---

## 維護規則 | Maintenance Rules

1. `.github/copilot-instructions.md` 保留全域規範，不放任務範例或長篇教學。
2. `.github/project-memory/repo-facts.md` 保留可演進的 repo 事實；若與 `/memories/repo/` 重疊，以這份為準。
3. prompt 只寫任務範圍、輸出格式與必要 delta，不重貼專案背景。
4. agent 只寫角色邊界、適用情境與輸出格式，不重貼全域規範。
5. docs 下的 AI 使用手冊只做索引與導覽，不複製 `.github/` 內容。
6. 若某份文件只是「摘要」另一份文件，優先考慮合併或刪除摘要。

---

## 快速使用 | Quick Use

在 Chat 中直接輸入：
- `/code-review #selection`
- `/debug #selection`
- `/generate-tests #selection`
- `@motion-expert 分析 PDO 週期抖動來源`
- `@planner 規劃 EtherCAT 模組拆分步驟`

---

## 最小維護清單 | Minimal Maintenance Checklist

- 新增規範前，先確認是否屬於全域規範；若是，只改 `.github/copilot-instructions.md`。
- 在新電腦或新環境的第一次 AI 互動時，先檢查 `/memories/repo/`，把可重用且已驗證的事實同步到 `.github/project-memory/repo-facts.md`。
- 新增或修正 repo 事實前，先改 `.github/project-memory/repo-facts.md`，不要在 `/memories/repo/` 與文件中重複維護。
- 任務若新增了已驗證的 repo 知識，結束前把 `.github/project-memory/repo-facts.md` 一起更新並納入版控。
- 若候選記憶有疑問、邏輯衝突或存在未驗證的替代方案，先詢問使用者，不要直接寫入專案記憶。
- 新增 prompt 或 agent 前，先確認能否用現有項目解決，避免持續膨脹。
- 文件更新後若內容只是重述其他檔案，直接改成連結或索引說明。
