# AGENTS.md — WuBigCat 專案指南（給 AI 編碼代理）

> 本文件假設讀者對本專案一無所知。內容依實際專案內容撰寫；更細節的規則另見 `.github/copilot-instructions.md`（Copilot）、`CLAUDE.md` 與 `.claude/`（Claude Code）、`.github/project-memory/repo-facts.md`（共用記憶）、`docs/` 與 `scripts/README.md`。

## 專案概述

- **名稱**：BigCat — SOEM EtherCAT Master for Windows（專案根目錄名稱為 WuBigCat，CMake project 名稱為 `BigCat`）。
- **用途**：6 自由度（6-DoF）Stewart 平台運動控制系統。以 SOEM（Simple Open EtherCAT Master）在 Windows 上實作 EtherCAT 主站，控制 CiA402/DS402 伺服驅動器（DVS、X1S 等）。
- **作者**：wuyenhung（巫彥鴻）／立洋科技。**專有授權，非開源軟體**（見 `README_TW.md` 授權條款）。
- **主要功能**：
  - EtherCAT PDO（CSP/CSV 週期性資料交換）、SDO 讀寫、Mailbox、EMCY 診斷。
  - Win32 GUI（從站列表、運動控制面板、SDO 編輯、系統列最小化）。
  - 嵌入式 HTTP/SSE 伺服器（起始 port 8080；被其他程式占用時自動往上跳號至 8089，實際埠見 `GetLocalHttpServerPort()`／選單網址；開關由 `BigCat.ini [Server] EnableLocalHttpServer` 控制，預設關閉），提供 REST API 與 Web UI（`resources/web/`）。Web UI 為多語系（繁中＝來源語系、英文可即時切換，採「中文原文作 key」的靜態目錄方案，新增語系只需 `resources/web/locales/<code>.json` ＋ `locales/index.json` 一行，詳見 `docs/WEB_I18N.md`）。
  - Stewart 平台逆/正運動學（`StewartIK`）、PDOFilter（速度/加速度/硬限位保護）、Soft-Start、Jog。
  - MMF（Memory-Mapped File）行程間通訊，接收外部程序（如 FlyPT Mover）的 6 軸姿態輸入。
  - TPM/硬體指紋授權驗證、單例保護（命名 Mutex）、CrashHandler 自動產生 minidump。
  - 關機防護（`CoreHelpers::ShutdownGuard`）：程式執行期間封鎖 Windows 關機/重新啟動/登出
    （`ShutdownBlockReasonCreate` + 主視窗 `WM_QUERYENDSESSION` 回 `FALSE`）並防止閒置睡眠；
    由 `BigCat.ini [Safety]` 控制，詳見 `docs/SHUTDOWN_GUARD.md`。
  - 模擬模式（Simulation）：無實體硬體時以虛擬軸跑完整運動邏輯，大量 smoke 測試依賴此模式。

## 技術棧與運行架構

- **語言/標準**：C++17，MSVC 工具鏈（Windows-only，大量使用 Win32 API）。CMake ≥ 3.25。
- **第三方相依**（皆在 `lib/` 或 FetchContent）：
  - `lib/SOEM`：EtherCAT 主站函式庫，以 `add_subdirectory` 建成 `soem` target（注意：本 repo 的 SOEM 含本地修補，例如 `ec_main.c` 的 mailbox NULL guard，勿直接覆蓋為上游版本）。修補以 `git -C lib/SOEM diff HEAD` 匯出為 `lib/SOEM-local-changes.patch` 隨主 repo 保存；`.gitmodules` 指向上游 SOEM（gitlink 釘在上游 SHA），新環境 `git submodule update --init` 後以 `git -C lib/SOEM apply ../SOEM-local-changes.patch` 還原。**改動 lib/SOEM 原始碼後，必須在同一 patch 重新匯出該 patch 檔**（`lib/SOEM` 屬他人擁有的 checkout，非管理員執行 git 時需 `GIT_CONFIG_*` 或 `safe.directory` 例外）。
  - `lib/npcap-sdk-1.16`：封包擷取（wpcap/Packet），可用 `-DNPCAP_SDK_DIR` 或環境變數覆寫路徑。
  - tinyxml2：預設 FetchContent 抓取 9.0.0；`-DUSE_LOCAL_TINYXML2=ON` 改用 `lib/tinyxml2`（`default` preset 已設 ON）。
- **執行需求**：Windows 10/11 x64、Npcap 驅動、**系統管理員權限**（封包注入與 IRQ/affinity 設定）。EtherCAT 網卡介面格式為 `\Device\NPF_{GUID}`。
- **近即時（near-real-time）設計**：Windows 非 RTOS，PDO 週期穩定性是第一級約束：
  - EtherCAT cyclic thread 與 UI、HTTP、logging、I/O 執行緒分離；cyclic thread 綁定到隔離的 RT 實體核心（`src/core/CpuIsolation.cpp`、`CpuTopology.cpp`），並使用 MMCSS Pro Audio 提升優先權。
  - PDO 週期路徑禁止 blocking call、heap 配置、console 輸出與長時間 mutex；緩衝區全部預先配置。
  - 非 RT 執行緒（HTTP、UI、Monitor、SSE）被限制到 RT 相鄰核心；IRQ affinity 有全系統性調校（見 `docs/windows-realtime-optimization.md`）。
  - **任何修改都應先評估對 PDO 同步/週期抖動的影響**，除非任務明確要求，不得削弱現有 timing、affinity、priority 調校。
- **輸出**：主執行檔 `BigCat.exe`（Win32 GUI 子系統），輸出於 `build/Release/bin/main/`；建置後自動把 `Par/`（參數檔）與 `esi/`（ESI XML）複製到執行檔旁。

## 目錄與模組劃分

CMake 把程式碼切成多個 static library，再由主程式組裝（見 `CMakeLists.txt`）：

| 目錄 / target | 職責 |
| --- | --- |
| `src/core`（`bigcat_core`） | AppConfig（INI）、CPU 拓撲/隔離、Win32 工具、即時電源策略。 |
| `src/io`（`bigcat_io`） | MMFReader/MMFWriter、StatusLogger、MachineConfigIO（machine.xml）。 |
| `src/security`（`bigcat_security`） | LicenseChecker（TPM/硬體指紋 SHA-256 授權）。 |
| `src/ethercat`（`bigcat_ethercat`） | EtherCATMaster（拆成 `.Init/.Cyclic/.SDO/.Simulation/.MMF/.EMCY/.Diagnostics` 多檔）、PDOFilter、PDOHelpers、EsiParser。 |
| `src/motion`（`bigcat_motion`） | MotionController（CiA402 狀態機、上電/回原點/急停）、MotionLogic、MotionConfigIO、StewartIK。 |
| `src/server`（`bigcat_server` + `bigcat_server_support`） | HttpServer（`.Core/.API/.SSE/.Auth/.Static` 拆檔）、API/Static helpers、MainWindow.HTTP。 |
| `src/app`（`bigcat_app`） | **AppController — 生命週期、HTTP、自動流程的單一協調入口**；另有 `main.cpp`、CrashHandler。 |
| `src/ui`（`bigcat_ui`） | MainWindow（拆成 `.Command/.Connect/.Motion/.SDO/.Timer/.Watchdog/.ESI/.Adapter/.Master/...` 多檔）、AutoWindow、StatusPane、PDOProxy、Win32WindowCore。 |
| `include/` | 對外標頭；`include/core/` 放介面（IEtherCATService、IMotionService、EventBus、SystemState 等）。 |
| `tests/` | 測試程式（見「測試策略」）。 |
| `tools/` | 獨立工具：`cpu_burn_test`、`mmf_sine_writer`、`mmf_scope`；`generate_license`（授權產生）為**作者私有**：來源與 exe 不入版控、不在 public repo，建置時缺來源自動略過 target（見 CMakeLists 與 `repo-facts.md`）。 |
| `resources/web/` | Web UI 靜態檔（auto.html、status.html、sdo 頁面、auth.js、sidebar-menu.js 等）；多語系引擎 `js/i18n.js` 與語系目錄 `locales/*.json`（見 `docs/WEB_I18N.md`）。 |
| `Par/` | 運動/機械參數（`motion.xml`、`machine.xml`、`BigCat.ini`、各軸 XML）。 |
| `esi/` | EtherCAT 從站 ESI 描述檔。 |
| `scripts/` | PowerShell 自動化（建置/測試 wrapper、crash 分析、NIC/ETW 診斷）。 |
| `docs/` | 架構與保護機制文件（PROJECT_STRUCTURE_GUIDE、APP_CONTROLLER_INTERFACE_CONTRACT、SATURATION_ATTACK_PROTECTION、StewartIK_Manual 等）。 |
| `.github/` | Copilot 指令（`copilot-instructions.md`、`instructions/`）、自訂 agents（`agents/`）、prompt 模板（`prompts/`）、`project-memory/repo-facts.md`（repo 級 AI 記憶，Copilot 與 Claude Code 共用）。 |
| `CLAUDE.md`、`.claude/` | Claude Code 入口與配置：`CLAUDE.md` 承接 AGENTS.md（載入順序/記憶/命令速查）、`.claude/agents/` 與 `.claude/commands/` 承接 `.github/agents/` 與 `.github/prompts/` 的對應角色。 |

架構規則（來自 `.github/copilot-instructions.md`）：
- UI/HTTP 命令應經由 `AppController` 路由，不要在新呼叫點重組流程。
- 保持模組邊界：bus/PDO/SDO → `src/ethercat`；CiA402 與運動學 → `src/motion`；HTTP/auth/static → `src/server`；Win32 編排 → `src/ui`。
- 保留大檔拆分的結構（如 `MainWindow.*.cpp`、`EtherCATMaster.*.cpp`）；新增邏輯優先抽成具名 helper，不要併回單一巨檔。

## 建置與測試命令

主要使用 CMake presets（`CMakePresets.json`）或 repo 自帶 PowerShell wrapper：

```powershell
# Configure + Build（Release, Ninja, 預設 preset）
cmake --preset default
cmake --build --preset default

# 一鍵 configure+build+test
cmake --workflow --preset default

# 完整測試 / 僅 smoke 測試
ctest --preset default
ctest --preset smoke

# Repo wrapper（推薦給 AI 代理使用）
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_cmake.ps1 -Action build -Preset default
.\scripts\build_incremental-Crash.ps1 -NonInteractive      # 預設增量建置驗證
.\scripts\run_smoke_tests.ps1 -BuildDir .\build -Config Release   # smoke 回歸
```

- 日常驗證用 `scripts/build_incremental-Crash.ps1`；`build_ForceFull.ps1` 僅用於快取腐壞/疑難建置失敗；`build_release_hardened.ps1` 僅用於 release/效能任務。
- VS Code 內建 tasks：`CMake: Configure`、`CMake: Build Release`、`Build: Incremental Crash`、`CTest: Run Smoke`、`CTest: Run All` 等。在 VS Code 整合終端機容易卡住，AI 代理應優先使用這些 task 或以一次性 `cmd.exe /d /s /c` 包裝呼叫 repo 腳本。
- 另有 `vs2022` preset（Visual Studio 17 2022 generator，輸出到 `build-vs2022/`）作為備援。
- 常用 CMake 選項：`ENABLE_CRASH_HANDLER`（預設 ON）、`GENERATE_PDB_FOR_RELEASE`（預設 ON，Release 產生 PDB 並停用 /GL /LTCG）、`USE_LOCAL_TINYXML2`、`BIGCAT_ENABLE_RELEASE_AVX2`（預設 OFF，保留舊 CPU 相容性）、`BIGCAT_ENABLE_UNITY_BUILD`（預設 OFF）、`BIGCAT_TEST_TIMEOUT_SECONDS`（預設 300）。
- VS Code 的 IntelliSense 診斷在此 repo 常誤報 include-path/不完整型別錯誤；**以增量建置 + smoke 測試結果為準**。

## 測試策略

- 測試位於 `tests/`，是**無框架的獨立可執行檔**：`main()` 回傳 `EXIT_SUCCESS`/`EXIT_FAILURE`，用 `std::cerr` 印出失敗訊息（參考 `tests/test_stewart_ik.cpp` 的寫法）。由 CTest 驅動。
- 在 `CMakeLists.txt` 以 `bigcat_add_optional_test(<name> <source> "<labels>" <libs...>)` 註冊；來源檔不存在時自動跳過。新增測試需在此登記並加上合適 label（`smoke`、`server`、`math`、`pdofilter`、`ethercat;simulation` 等）。
- 測試 label 群組：`smoke`（回歸主線，含 motionconfig、math、ethercat simulation、pdofilter）、`server`（HTTP auth/static/api）、`utility;diagnostic` 等。`ctest -L smoke` 是標準回歸入口。
- 大量測試走 **Simulation 模式**（無硬體），涵蓋 soft-start、硬限位、CSV 換行連續性、多圈位置正規化、虛擬軸 PID 等行為。
- `tests/test_Ethercat_jog.cpp` 是獨立 target（直接連 SOEM，需要實體網卡），不在 smoke 群組內。
- 測試逾時預設 300 秒；CTest 以 Release 組態、4 並行執行。
- AI 觸發的建置/測試結束後（含中斷），應刪除可丟棄的 log、暫存與臨時驗證檔；失敗診斷需要的檔案除外。

## 程式碼風格與慣例

- 現代 C++17、RAII；**類別用 CamelCase，函式/變數用 snake_case**。
- 修根因不修症狀；保持公開行為穩定；聚焦小修改，不做大範圍重構；保留 crash handling 與診斷路徑。
- 優先更新已有測試覆蓋的 helper/seam，不要引入平行程式路徑。修改 HTTP 或自動流程前，先查 `docs/` 是否已有 contract 文件或既有 helper 擁有該政策。
- **編碼：所有專案文字檔（原始碼、腳本、文件、JSON/XML）一律 UTF-8 without BOM**。含中文的檔案尤其不可轉成 ANSI/Big5/UTF-8 BOM；MSVC 以 `/utf-8` 編譯。Win32 UI 字串邊界仍用 wide string / UTF-16 轉換 helper。發現 mojibake 應直接修正編碼或重寫註解。
- 註解與文件主要使用繁體中文（部分文件為英文或簡體中文）；新註解建議跟隨所在檔案既有語言。
- **Web UI 多語系**：`resources/web/` 的網頁以「中文原文作 key」的靜態目錄翻譯（`resources/web/js/i18n.js` + `resources/web/locales/*.json`，來源語系 `zh-Hant`）。新增 UI 文字照常寫中文，跑 `scripts/i18n_report.ps1` 補缺譯；**不要用顯示文字做邏輯判斷**（翻譯後 `textContent` 會變），需要判斷請用 `id`/`dataset`。細節與陷阱見 `docs/WEB_I18N.md`。版面必須容得下較長譯文：grid 欄位寫 `minmax(0, 1fr)`（勿用裸 `1fr`，其下限是 min-content，英文單字會撐爆欄位）、文字容器 `min-width: 0` + 可換行，改完要 zh/en 並排量測（只算「英文才有、中文沒有」的差異）。
- 編譯警示：MSVC `/W4`，並停用 4996/4819/4201/4244；定義 `UNICODE/_UNICODE`、`WIN32_LEAN_AND_MEAN` 等（見 `bigcat_common` interface target）。

## EtherCAT / CiA402 規則

- 啟用運動前先確認從站掃描、狀態機與 PDO 映射正確；PDO layout、縮放與 endian 假設要明確且一致。
- CiA402 狀態切換不可跳步：Enable 順序為 Fault Reset → Shutdown → Switch On → Enable Operation；停止先 Quick Stop / Disable Operation，確認安全後才移除轉矩。
- statusword、operation mode、target/actual position、fault code 視為一組；fault 時一併記錄 controlword/statusword/mode/alarm。
- Operation Enabled 且安全檢查通過前不得下運動命令；CSP/CSV/CST/homing 切換需確認 mode acknowledgement。
- 注意驅動器差異：DVS（vid=0x445653）與 X1S（vid=0x766）的轉矩限制索引、堵轉偵測方式不同（見 `repo-facts.md` 與 `EtherCATMaster::IsDvsDrive/IsX1sDrive`）。

## 安全考量

- **人身安全**：本系統控制真實運動平台。測試運動一律從小振幅開始，先驗證通訊與 servo enable；軟體限位必須強制執行；急停是第一等路徑。試用模式無人身安全保證。
- **授權保護**：TPM/硬體指紋綁定，授權金鑰為 64 字元 hex，來源為 `license.txt`、`BIGCAT_LICENSE_KEY` 環境變數或命令列。`BIGCAT_DISABLE_LICENSE_CHECK=1` 僅 Debug 建置有效。**不得移除或繞過授權/TPM 保護機制**（法律明文禁止）。
- **HTTP 伺服器**：預設關閉；有登入/PIN 驗證、Session 管理、單一 IP 單一連線與飽和攻擊防護（見 `docs/SATURATION_ATTACK_PROTECTION.md`、`docs/SINGLE_CONNECTION_PER_IP.md`）。`WUSOEM_HTTP_MAX_CONNS` 控制同時連線上限（預設 2）。網路/auth 強化相關修改請先參考上述文件。
- **Crash 處理**：CrashHandler 會在 `dumps/` 產生 minidump；分析流程見 `scripts/analyze_dump_and_prepare_ai_prompt.ps1` 與 `tools/cdb/`。修改時不得破壞 crash 分析自動化。
- **腳本安全**：`scripts/` 內部分腳本會修改系統設定（NIC、IRQ affinity、IFEO），需管理員權限；執行前確認其用途。

## AI 代理工作流程（本 repo 既定慣例，適用 Copilot、Claude Code 與 DSH）

1. 進行實質修改前，先讀 `.github/project-memory/repo-facts.md`（可只讀開頭與相關子系統段落）與最近的擁有者文件；形成單一假設與一個低成本驗證方式後再動手。
2. 建置/測試/診斷優先使用 repo 內 PowerShell 腳本與 VS Code task，避免互動式終端操作。
3. 用精準搜尋與範圍式讀檔，避免把大檔/完整 log 載入對話。
4. 做能被驗證的最小根因修改，立即跑聚焦驗證再擴大範圍。
5. 行為、工作流程、建置/測試政策或耐久性 repo 知識有變更時，在同一個 patch 更新相關文件與 `.github/project-memory/repo-facts.md`。
6. 若使用者要求與既有 repo 記憶/規則衝突，先向使用者確認，不要靜默覆蓋。

### Copilot、Claude Code 與 DSH 混合使用

- **單一事實來源**：`AGENTS.md`（專案指南）、`repo-facts.md`（repo 記憶）。三個入口都讀同一份，禁止另立平行 repo 記憶；任一方向發現差異時以 `.github/` 為準並回報。
- **Copilot 層**：`.github/copilot-instructions.md`（global rules）、`.github/instructions/`（per-path rules）、`.github/agents/`（自訂 agents）、`.github/prompts/`（prompt 模板）、`.github/project-memory/`（repo 記憶）。
- **Claude Code 層**：`CLAUDE.md`（入口，承接 AGENTS.md）、`.claude/agents/`（`motion-expert`、`planner`，承接 `.github/agents/`）、`.claude/commands/skills.md`（`/code-review`、`/debug`、`/crash-triage`、`/generate-tests` 索引，承接 `.github/prompts/`）。
- **DSH（DeepSeek Harness）層**：DSH 會話自動載入專案根 `AGENTS.md` 與 `CLAUDE.md`（另含使用者層 `$DSH_HOME/AGENTS.md` 全域規則），動工前讀 `repo-facts.md` 的順序、驗證命令與清理慣例和上述兩工具相同；DSH 唯一的私有檔案是專案根 `local-model-stats.json`（本地模型派工驗收統計與 lessons，DSH 開工前必讀 `lessons`；**只放派工帳單，repo 事實仍只寫 `repo-facts.md`**）。
- 專案政策若只在其中一層更新，其餘各層下次動工時應同步補上，維持各層等價；新增大型政策段落時優先寫在共用檔（AGENTS.md / docs / repo-facts.md），不要只埋進單一工具的私有規則檔。
