# BigCat — SOEM EtherCAT Master for Windows

**BigCat** 是一套基於 [SOEM（Simple Open EtherCAT Master）](https://github.com/OpenEtherCATsociety/SOEM) 的 Windows EtherCAT 主站應用程式，專為 **6 自由度（6-DoF）運動平台控制** 設計。本專案提供完整的 Win32 GUI、嵌入式 HTTP/SSE 伺服器、Stewart 平台逆運動學、PDO 濾波器及授權管理等功能。

> **專案作者：wuyenhung 巫彥鴻** · 📧 wuyenhung@gmail.com · 🏢 立洋科技

---

## 目錄

- [主要功能](#主要功能)
- [系統需求](#系統需求)
- [專案結構](#專案結構)
- [快速建置](#快速建置)
- [執行與啟動模式](#執行與啟動模式)
- [Web UI](#web-ui)
- [授權機制](#授權機制)
- [進階功能](#進階功能)
- [常用操作與提示](#常用操作與提示)
- [CMake 選項](#cmake-選項)
- [參考資料](#參考資料)
- [版本歷程](#版本歷程)

---

## 主要功能

### EtherCAT 通訊
- **PDO（Process Data Object）**：週期性過程資料交換，支援 CSP（Cyclic Synchronous Position）模式
- **SDO（Service Data Object）**：服務資料對象讀寫與驗證
- **Mailbox**：郵箱發送/接收

### Win32 GUI
- 從站列表、狀態監控、SDO 編輯
- 運動控制面板：上電、回原點、緩停、急停、寸動（Jog）
- 狀態日誌與歷史記錄瀏覽
- 系統列（Tray）最小化

### 運動控制
- **MotionController**：批次上電 / 回原點 / 急停 / 平滑停止
- **PDOFilter**：速度、加速度、減速度、硬限位濾波保護
- **緩啟動（Soft-Start）**：漸進啟動避免衝擊
- **寸動（Jog）**：正向/反向手動移動

### Stewart 平台運動學
- **StewartIK**：6-DoF Stewart 平台逆運動學計算
- **ForwardKinematics**：正運動學姿態求解
- 支援對稱六邊形平台與底座參數配置

### 行程間通訊
- **MMFReader / MMFWriter**：Memory-Mapped File 讀寫，支援外部程序 6 軸浮點數據輸入

### HTTP 伺服器
- 嵌入式 HTTP 伺服器（預設 port 8080）
- SSE（Server-Sent Events）即時狀態推送
- RESTful API：從站資訊、SDO 讀寫、狀態查詢
- 登入/PIN 驗證與 Session 管理

### 安全與授權
- TPM / 硬體指紋授權驗證
- 自動授權檔 `license.txt` 支援
- 單例保護：防止多重執行個體

### 崩潰處理
- **CrashHandler**：自動產生 minidump 檔案
- 自動化崩潰分析腳本

---

## 系統需求

完整工具安裝與開發環境建議請先閱讀 [docs/ENVIRONMENT_SETUP.md](docs/ENVIRONMENT_SETUP.md)。缺少 Node.js、CMake、Git 等常用工具會降低 Web UI 驗證、建置與 smoke test 效率。

| 項目 | 需求 |
|------|------|
| 作業系統 | Windows 10/11（64-bit） |
| 編譯器 | Visual Studio 2022+（含 C++ 桌面工作負載） |
| CMake | 3.25+（此工作區已驗證 4.1.0） |
| 網路驅動 | Npcap（建議）或 WinPcap |
| 權限 | 系統管理員權限（封包擷取/注入） |

---

## 專案結構

WuBigCat 的主要目錄已依功能切分為核心程式碼、Web UI、文件、腳本、測試與參數檔。若需要逐一查看每個資料夾與子資料夾的用途，請直接參考 [docs/PROJECT_STRUCTURE_GUIDE.md](docs/PROJECT_STRUCTURE_GUIDE.md)。

重點目錄摘要：
- `include/`、`src/`：核心介面與實作。
- `resources/web/`：Web UI 靜態資源與前端頁面。
- `scripts/`：建置/測試 wrapper、crash 分析、AI 請求與 HTTP 分享輔助腳本。
- `docs/`：架構、保護機制、Agent/Copilot 使用文件。
- `tests/`：單元與行為測試。
- `Par/`、`esi/`：運動參數與 EtherCAT ESI 定義檔。
- `.github/`、`.vscode/`：Copilot/Agent 設定、工作區 task 與 crash 自動化輸出。

AI 自動化模型預設、參數覆寫與 legacy 腳本限制以 [scripts/README.md](scripts/README.md) 為準。

專案結構示意（核心檔案不逐一列舉，僅給出結構視角）

WuBigCat/
- CMakeLists.txt
- .github/
- .vscode/
- include/
- src/
- resources/
- lib/
- scripts/
- tests/
- docs/
- esi/
- Par/

---

## 快速建置

### 使用 PowerShell（推薦，本地開發機建置示意）

1) 進入專案目錄並建立建置目錄
   - PowerShell
     cd D:\Yenhung\WORK\Wu6Dof\PROG\WuBigCat
     New-Item -ItemType Directory -Path .\build -Force
2) 產生 Visual Studio 專案與編譯選項
   - 在 PowerShell（或 Visual Studio 開發指令對應環境）中執行：
     cmake --preset default
3) 編譯 Release 版本
     cmake --build --preset default

### 輸出位置
- 具體可執行檔與工具的輸出位置取決於 CMake 專案設定，通常位於 build\\Release\\bin\\ 目錄下，您可以在該目錄中找到：
  - BigCat.exe（主執行檔）
  - 其他工具與單元測試可執行檔（如 license_generate 等），依專案設定而定

---

## 執行與啟動模式

### 系統管理員權限執行

```powershell
Start-Process .\build\Release\bin\main\BigCat.exe -Verb RunAs
```

### 啟動模式

| 模式 | 命令列參數 | 行為 |
|------|------------|------|
| **正常模式** | （無） | 主視窗縮至系統列；嵌入式 HTTP 伺服器是否啟動由 `BigCat.ini` 中的 `[Server] EnableLocalHttpServer` 控制（預設：關閉）。 |
| **Debug 模式** | `Debug` | 主視窗正常顯示，方便開發除錯 |

### 單例保護

程式使用命名 Mutex 防止多重執行個體。若已有執行中的 BigCat，新啟動的執行個體會自動退出。

## 配置（INI） — 建議設定

BigCat 會在執行檔同目錄讀取同名 `.ini`（例如 `BigCat.ini`）。常用的 INI 選項已同步到 UI（選項勾選會寫入 INI），可於安裝或自動部署時預先設定。

常用鍵（section / key / 說明 / 預設）：
- [Server] EnableLocalHttpServer — 是否啟動內建 HTTP 伺服器（0/1） — 預設：0（關）
- [Startup] ShowMainWindow — 啟動時是否顯示主視窗（0/1） — 預設：0（關）
- [Startup] AutoTrigger — 啟動時是否自動觸發自動序列（0/1） — 預設：1（開）
- [Startup] TriggerTestCrash — 啟動時是否觸發測試當機（0/1） — 預設：0（關）
- [Compat] ForceWin10Mode — 強制使用 Windows 10 的週期等待策略（0/1） — 預設：0（關，依作業系統自動判斷）
- [Logging] EnableStatusLog — 是否將狀態訊息寫入 `logs/status.log`（0/1） — 預設：1（開）

範例 `BigCat.ini`（可另存為 `BigCat.ini` 放在執行檔同目錄）：
```ini
[Server]
; 是否啟動本機 HTTP 伺服器（SSE / REST）
EnableLocalHttpServer=0

[Startup]
; 啟動時顯示主視窗（否則最小化到系統列）
ShowMainWindow=0
; 啟動時是否自動觸發自動序列（Auto）
AutoTrigger=1
; 測試用：啟動時觸發測試當機
TriggerTestCrash=0

[Logging]
; 是否寫入 logs/status.log（UI: 狀態視窗 -> 儲存 status.log）
EnableStatusLog=1
```

---

## Web UI

### 存取方式

正常模式啟動後，HTTP 伺服器會在 port 8080 啟動。請手動在瀏覽器開啟：
- `http://localhost:8080/auto.html` - 自動操作主頁

### 功能頁面

| 頁面 | 說明 |
|------|------|
| `auto.html` | 自動操作主頁 |
| `status.html` | 狀態監控 |
| `sdo.html` | SDO 讀寫 |

### 登入驗證

- 所有頁面使用共用側選單（`sidebar-menu.js`）與登入模組（`auth.js`）
- 未登入時，互動控制會被禁用，僅保留登入按鈕
- 登入狀態儲存於 `localStorage`（key: `wusoem_logged_in_v1`）

### 環境變數

| 變數 | 說明 | 預設值 |
|------|------|--------|
| `WUSOEM_HTTP_MAX_CONNS` | 同時連線上限 | 2 |

---

## 授權機制

### 授權驗證

BigCat 使用硬體指紋（含 TPM/EK）計算 SHA-256 授權金鑰。

### 授權方式

1. **授權檔**：將 64 字元 hex 金鑰存入 `license.txt`，放置於 `BigCat.exe` 同目錄
2. **環境變數**：設定 `BIGCAT_LICENSE_KEY`
3. **命令列**：直接傳入 64 字元 hex 金鑰

### 開發者繞過

- 僅 Debug 建置可設定 `BIGCAT_DISABLE_LICENSE_CHECK=1` 繞過驗證
- Release 建置會忽略此設定

### 授權工具

```powershell
# 產生本機授權
.\build\Release\bin\main\generate_license.exe -l -o license.txt

# 從指紋字串產生
.\build\Release\bin\main\generate_license.exe -f "<fingerprint>" -o license.txt
```

GUI 模式：直接執行 `generate_license.exe`（無參數），可視覺化產生授權。

### 驗證失敗

驗證失敗時會顯示對話框，包含：
- 裝置指紋
- 預期的授權金鑰（自動複製到剪貼簿）

---

## 進階功能

### PDO 濾波器

`PDOFilter` 提供即時運動保護：

- **速度限制**：防止超速
- **加速度/減速度限制**：平滑運動曲線
- **硬限位保護**：防止超出行程範圍
- **違規回調**：即時通知違規狀態

### Stewart 運動學

```cpp
#include "StewartIK.h"

WuSOEM::StewartParams params;
WuSOEM::Pose6D pose{0, 0, 450, 0, 0, 0}; // x, y, z, roll, pitch, yaw

auto result = WuSOEM::StewartIK::CalculateInverse(params, pose);
if (result.valid) {
    // 使用 result.servoAngles[0..5]
}
```

### 崩潰處理

- 自動產生 minidump 至 `dumps/` 目錄
- 使用 `scripts/analyze_dump_and_prepare_ai_prompt.ps1` 分析
- 整合 AI 輔助診斷流程

### 狀態日誌

- 日誌儲存於 `logs/status.log`（可由 UI 或 INI 開關控制；INI: `[Logging] EnableStatusLog=1`，預設：啟用）
- 自動時間戳記輪替
- 記憶體限制：僅保留最近約 512KB
- 支援分頁讀取歷史

---

## 常用操作與提示

### SDO 讀寫範例

1. 選擇目標從站
2. 輸入 Index（如 `0x1000`）與 Subindex（如 `0x00`）
3. 點擊「讀取 SDO」或「寫入 SDO」

### Mailbox 範例

1. 選擇目標從站
2. 輸入 hex 資料（以空格分隔，如 `01 02 03 04`）
3. 點擊「發送 Mailbox」

### 網路介面

Windows 下 SOEM 使用的介面格式：
```
\Device\NPF_{GUID}
```

查詢可用介面：
```powershell
Get-NetAdapter | Format-Table Name, InterfaceDescription, ifIndex
```

### 常見問題

| 問題 | 解決方案 |
|------|----------|
| CMake 找不到 Windows SDK | 使用 VS Developer PowerShell |
| 連結錯誤 wpcap/Packet | 安裝 Npcap SDK 或設定 `-DNPCAP_SDK_DIR` |
| 無法存取網路介面 | 以系統管理員權限執行 |

---

## CMake 選項

| 選項 | 說明 | 預設 |
|------|------|------|
| `-DNPCAP_SDK_DIR=<path>` | Npcap SDK 路徑 | 自動偵測 |
| `-DUSE_LOCAL_TINYXML2=ON` | 使用本機 tinyxml2 | OFF |
| `-DGENERATE_PDB_FOR_RELEASE=ON` | Release 產生 PDB | ON |

---

## 參考資料

- [SOEM - Simple Open EtherCAT Master](https://github.com/OpenEtherCATsociety/SOEM)
- [Npcap](https://npcap.com/)
- [EtherCAT Technology Group](https://www.ethercat.org/)
- [tinyxml2](https://github.com/leethomason/tinyxml2)

---

## 版本歷程

### v1.0.0（最新）

**2026-01-24 — CPU 效能優化**
- EtherCAT 週期執行緒採用混合等待策略（Sleep(0) + YieldProcessor）
- UI Timer 間隔從 100ms 調整為 200ms
- HTTP URL 刷新與鏈路狀態檢查節流至 1 秒間隔
- MMF 映射失敗時增加 200ms 退避重試
- SSE 廣播器在無客戶端時增加 200ms 閒置退避

**2025-12-14 — 安全停止改進**
- 修正：緩啟動期間按下「立即停止」可能造成爆衝問題
- 改進：立即停止流程將持位目標設為當前實際位置
- 新增 `pdo_filter_stop_test` 單元測試驗證 Hold 行為

**2025-10-28 — 日誌系統改進**
- 狀態日誌改用時間戳記輪替
- 狀態視窗分頁讀取（預設 64KB/頁）
- 記憶體上限約 2M wchars

**2025-10-20 — 基礎功能**
- Win32 GUI 完整實作
- EtherCAT PDO/SDO/Mailbox 支援
- Stewart 平台運動學
- HTTP/SSE 伺服器
- 授權管理

---

## 版權與授權

### 著作權聲明

Copyright © 2025-2026 **巫彥鴻 (wuyenhung)**. All rights reserved.

本專案（BigCat）之所有原始碼、文件、資源檔案、建置腳本及相關設計，均為作者 **巫彥鴻 (wuyenhung)** 之智慧財產，受中華民國著作權法及國際著作權公約保護。

### 專案授權條款（專有授權）

⚠️ **本專案並非開源軟體，不適用 MIT、GPL、Apache 或其他任何開源授權條款。**

未經作者 **巫彥鴻 (wuyenhung)** 事先書面同意，任何個人或組織：

1. **不得使用** — 不得以任何目的（商業或非商業）執行、部署或使用本專案之任何部分
2. **不得修改** — 不得修改、改作、衍生、反編譯或逆向工程本專案之原始碼或二進位檔案
3. **不得散布** — 不得複製、散布、轉授權、出租、出借或以其他方式提供本專案予第三方
4. **不得移除保護** — 不得移除、規避、破解或以其他方式繞過本專案之 TPM（Trusted Platform Module）硬體綁定驗證、授權檢查或其他技術保護措施

> **試用模式例外**：本軟體內建免序號試用功能，使用者無需取得授權金鑰即可執行本軟體進行評估與測試。試用模式不受上述第 1 項「不得使用」之限制，但仍受第 2、3、4 項及下方「試用模式安全聲明」之全部約束。

### ⚠️ 試用模式安全聲明

本軟體控制之運動平台涉及機械運動，具有造成人員傷亡及設備損壞之潛在風險。就試用（免序號）模式之使用，特此聲明：

| 項目 | 說明 |
|------|------|
| 🚫 **不保證人身安全** | 作者不對試用模式下之任何人身傷害、死亡或財產損失承擔任何責任 |
| 👤 **使用者自行負責** | 使用者應自行確保操作區域淨空、設備參數正確、緊急停止裝置可用、人員受過適當訓練 |
| 📋 **不在保固範圍內** | 試用模式下之一切使用行為，完全不在作者或立洋科技之保固、技術支援或售後服務範圍內 |
| ⚖️ **風險自負** | 一旦執行試用模式，即視為已閱讀、理解並同意上述全部條款，自願承擔一切後果 |

#### 違反條款之法律效果

- 違反上述任一條款者，授權即刻終止，且作者保留依法追究民事賠償及刑事責任之權利
- 擅自破解或規避 TPM 授權驗證機制者，除違反本授權條款外，可能另觸犯中華民國《刑法》第 358-362 條（妨害電腦使用罪章）及《著作權法》第 80-2 條（防盜拷措施之保護）之相關規定

#### 取得授權

如需使用本專案，請聯繫作者取得書面授權：

- 📧 Email：wuyenhung@gmail.com
- 🏢 公司：立洋科技

### 第三方程式庫授權

本專案包含以下第三方程式庫，各自適用其原始授權條款：

| 程式庫 | 授權類型 | 說明 |
|--------|---------|------|
| [SOEM](https://github.com/OpenEtherCATsociety/SOEM) | GPLv2 / Commercial | Simple Open EtherCAT Master |
| [tinyxml2](https://github.com/leethomason/tinyxml2) | zlib License | XML 解析器 |
| [Npcap SDK](https://npcap.com/) | Npcap License | 網路封包擷取 SDK |

上述第三方程式庫之使用與散布，請依各程式庫之原始授權條款辦理。本專案對第三方程式庫之授權條款不做任何修改或附加限制。

---

**⚠️ 注意：此專案為開發與測試用途，與實際 EtherCAT 裝置互動前請在安全環境下測試並遵循裝置手冊。**

**⚠️ 警告：未經授權使用本專案或破解 TPM 保護機制屬違法行為，作者保留全部法律追訴權利。**

**⚠️ 安全警告：試用模式不保證人身安全，使用者需自行注意安全並承擔一切風險，不在保固範圍內。**
