# WuBigCat 專案目錄說明 / Project Structure Guide

本文件整理 WuBigCat 專案目前的主要目錄、子目錄與用途。腳本細節以 `scripts/README.md` 為準，避免在多份文件重複維護。
This document summarizes the major directories, subdirectories, and responsibilities in WuBigCat. Script-specific details are owned by `scripts/README.md` to avoid duplicated maintenance.

---

## 1. 頂層目錄 | Top-Level Directories

| 路徑 | 用途 | 說明 |
| --- | --- | --- |
| `.github/` | Copilot/Agent 設定 | 專案級 Copilot 指令、自訂 agents 與 prompt 模板。 |
| `.vscode/` | 工作區設定與暫存分析輸出 | VS Code 設定、tasks、MCP 設定，以及 crash 分析輸出檔。 |
| `build/` | 主要 CMake 建置輸出 | 預設 Release/Debug 產物、Ninja/CMake 快取與中間檔。 |
| `docs/` | 專案文件 | 架構、故障保護、IK、NIC 優化、Copilot/Agent 指南。 |
| `esi/` | EtherCAT ESI XML | 裝置 ESI 描述檔，用於從站物件與 PDO 映射參考。 |
| `icons/` | 圖示素材與產生工具 | `.ico`、`.png` 與 icon 產生腳本。 |
| `include/` | 對外標頭與核心介面 | 主要類別宣告、共用 helper、介面定義。 |
| `lib/` | 第三方函式庫 | SOEM、Npcap SDK、tinyxml2 等外部相依。 |
| `Par/` | 機台/運動參數檔 | `machine.xml`、`motion.xml` 與各軸參數檔。 |
| `resources/` | 執行階段資源 | Win32 `.rc` 與 Web UI 靜態檔。 |
| `scripts/` | 自動化與維運腳本 | 建置、crash 分析、AI 請求、HTTP 分享等 PowerShell 腳本。 |
| `src/` | C++ 實作程式碼 | 依功能切分的應用、EtherCAT、UI、server、motion 模組。 |
| `tests/` | 測試程式 | Stewart IK、PDO filter、preload、座標轉換等測試。 |
| `tools/` | 輔助工具程式 | 例如授權產生器等獨立工具。 |

---

## 2. 主要子目錄 | Key Subdirectories

### `.github/`

| 路徑 | 用途 |
| --- | --- |
| `.github/copilot-instructions.md` | 專案級 Copilot 行為、風格與安全規則。 |
| `.github/agents/` | 自訂專家代理，例如運動控制與規劃代理。 |
| `.github/prompts/` | 可重用 prompt 模板，例如 code review、debug、generate tests。 |

### `.vscode/`

| 路徑 | 用途 |
| --- | --- |
| `.vscode/tasks.json` | 本機工作流程，例如 crash automation、build。 |
| `.vscode/mcp.json` | MCP 工具與服務整合設定。 |
| `.vscode/last_crash_ai_request.txt` | crash 分析後要送給 AI 的 prompt。 |
| `.vscode/crash_ai_response.txt` | AI 回應內容與建議 patch。 |

### `include/`

| 路徑 | 用途 |
| --- | --- |
| `include/core/` | 核心抽象介面與跨模組 helper 宣告。 |
| `include/sys/` | 系統層 helper，例如時間相關封裝。 |
| `include/EtherCATMaster.h` | EtherCAT 主站控制主介面。 |
| `include/MotionController.h` | 運動控制協調器介面。 |
| `include/StewartIK.h` | 6-DoF Stewart 平台運動學介面。 |
| `include/HttpServer.h` | 內建 HTTP/SSE 伺服器介面。 |
| `include/MainWindow.h` | Win32 主視窗與 UI 協調介面。 |

### `src/`

| 路徑 | 用途 |
| --- | --- |
| `src/app/` | 應用程式生命週期、AppController 與啟動流程。 |
| `src/ethercat/` | EtherCAT 主站、PDO/SDO、ESI 解析、濾波器。 |
| `src/io/` | MMF 讀寫與狀態記錄。 |
| `src/motion/` | MotionController、MotionLogic、StewartIK、MotionConfigIO。 |
| `src/security/` | 授權檢查與安全邏輯。 |
| `src/server/` | HTTP API、SSE、驗證、靜態檔服務。 |
| `src/ui/` | Win32 主視窗、自動流程視窗與桌面 UI 協調。 |

### `scripts/`

| 路徑 | 用途 |
| --- | --- |
| `scripts/analyze_dump_and_prepare_ai_prompt.ps1` | 分析最新 dump，輸出可直接送給 AI 的 prompt。 |
| `scripts/send_ai_request.ps1` | 將 prompt 送到 OpenAI API；模型預設與覆寫方式見 `scripts/README.md`。 |
| `scripts/run_cmake.ps1` | CMake configure/build/workflow wrapper，供 workspace task 與命令列共用。 |
| `scripts/run_ctest.ps1` | CTest wrapper，會處理 configure/build/test-dir 與工具解析。 |
| `scripts/run_smoke_tests.ps1` | Smoke test wrapper，建置煙霧測試目標並執行 `ctest -L smoke`。 |
| `scripts/build_incremental-Crash.ps1` | 預設的增量建置與 crash-handler 驗證入口。 |
| `scripts/build_ForceFull.ps1` | 強制全量重建入口。 |
| `scripts/build_release_hardened.ps1` | Release/performance 導向建置入口。 |
| `scripts/Invoke-BuildArtifactCleanup.ps1` | 清理可丟棄的建置中間檔與測試暫存。 |
| `scripts/share_http.ps1` | 協助 HTTP 服務分享或網路設定。 |
| `scripts/.snapshots/` | AI 互動時的程式碼快照與上下文資料。 |

### `tests/`

| 路徑 | 用途 |
| --- | --- |
| `tests/test_stewart_ik.cpp` | Stewart IK 計算正確性測試。 |
| `tests/test_pdofilter_velocity_continuity.cpp` | PDO filter 速度連續性測試。 |
| `tests/test_coordinate_transform.cpp` | 座標轉換測試。 |
| `tests/test_preload.cpp` | preload/初始化相關測試。 |

---

## 3. 文件去重原則 | Documentation De-Duplication

- 腳本用途、AI 模型預設與範例命令集中維護於 `scripts/README.md`。
- 建置、測試與本機工具需求集中維護於 `docs/ENVIRONMENT_SETUP.md`。
- 架構方向保留於 `docs/ARCHITECTURE_OPTIMIZATION_PLAN.md`；已收斂的實際入口合約保留於 `docs/APP_CONTROLLER_INTERFACE_CONTRACT.md`。
- README 僅保留入口摘要與連結，不重複長篇操作細節。

---

## 4. 建議整理原則 | Suggested Organization Rules

1. 新增 C++ 檔案時，優先放入對應功能子目錄，不再回到單一巨大 `src/` 平面結構。
2. 與 VS Code / Copilot / prompt 相關設定，集中放在 `.github/` 與 `.vscode/`。
3. 與部署、分析、建置相關的 PowerShell 腳本，統一收斂於 `scripts/`。
4. 文件優先放在 `docs/`，README 保留入口與快速導覽，不承載過長細節。
5. 機台或運動參數檔維持在 `Par/`，避免與程式碼或 ESI 混放。
