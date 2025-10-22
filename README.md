# SOEM EtherCAT Master - Windows（中文說明）

本專案示範如何在 Windows 平台使用 C++ 與 SOEM（Simple Open EtherCAT Master）實作 EtherCAT 通訊，並提供一個簡單的 Win32 GUI，支援 PDO、SDO 與 Mailbox 的基本操作。

## 主要功能

- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案結構（重點）

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/
└─ src/
```

主要檔案：

- `src/main.cpp`：程式進入點
- `include/EtherCATMaster.h`, `src/EtherCATMaster.cpp`：SOEM 封裝（初始化、PDO/SDO、週期作業）
- `include/Win32Window.h`, `src/Win32Window.cpp`：Win32 GUI

## 前置需求

- Visual Studio 2019 或更新版本（含 C++ 桌面工作負載）
- CMake 3.15+
- Npcap（建議）或 WinPcap

參考資源：

- SOEM（官方）：[OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap（官方）：[npcap.com](https://npcap.com/)
- EtherCAT：[ethercat.org](https://www.ethercat.org/)

## 快速建置（PowerShell）

```powershell
# 在專案根目錄執行
mkdir build; cd build
cmake ..
cmake --build . --config Release
```

使用 Visual Studio 生成器（範例）：

```powershell
cmake .. -G "Visual Studio 17 2022" -A x64
```

執行（建議以系統管理員權限）：

```powershell
.\build\Release\BigCat.exe
```

## 使用說明（重點）

1. 以系統管理員身份執行編譯後的執行檔。
2. 在 GUI 中選擇或輸入網路介面（例如 `\\Device\\NPF_{...}`）。
3. 點擊「連接」進行從站偵測與配置。

## 範例流程

### SDO 範例

1. 選擇目標從站
2. 輸入 SDO Index（例如 `0x1000`）與 Subindex（例如 `0x00`）
3. 點擊「讀取 SDO」或「寫入 SDO」

### Mailbox 範例

1. 選擇目標從站
2. 在 Mailbox 欄位輸入以空格分隔的十六進位資料（例如 `01 02 03 04`）
3. 點擊「發送 Mailbox」

## 清理與整理建議

建議將臨時或範例檔移至 `archive/`（例如）：

- `src/EtherCATExample_Advanced.cpp`
- `lib/npcap-sdk/Examples-remote/...`
- `COMPILATION_SUCCESS_REPORT.md`, `BUILD_STATUS.md`, `TEST_REPORT.md`
- `.snapshots/`

## 自動整理紀錄

2025-10-03：執行非破壞性的整理作業，將部分暫存或重複檔案移至 `archive/`（保留可還原備份）。

若要移除 build 產物或其他大型檔案，請先提供保留清單。

## 參考資料

- SOEM（官方）：[OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap（官方）：[npcap.com](https://npcap.com/)
- EtherCAT：[ethercat.org](https://www.ethercat.org/)

***

請注意：此專案為示範用途，與實際 EtherCAT 裝置互動前請在安全環境下測試並遵循裝置手冊。

## 最後更新

- 2025年10月20日

## 版本

- 1.0.0

## 主要功能
- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案摘要
目錄結構（重要檔案）：

```text
SOEM_C++/
├─ CMakeLists.txt
├─ README.md
# SOEM EtherCAT Master - Windows 10

此專案在 Windows 平台使用 C++ 與 SOEM (Simple Open EtherCAT Master) 實作 EtherCAT 通訊，並包含一個簡單的 Win32 GUI，支援 PDO、SDO 與 Mailbox 操作。

## 主要功能

- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案摘要

重要檔案與目錄：

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/
├─ src/
# SOEM EtherCAT Master - Windows (中文說明)

本專案使用 C++ 與 SOEM（Simple Open EtherCAT Master）在 Windows 平台上示範 EtherCAT 通訊，並提供一個簡單的 Win32 GUI，支援基本的 PDO、SDO 與 Mailbox 操作。

## 主要功能

- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案結構（重點）

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/
└─ src/
```

主要檔案：

- `src/main.cpp`：程式進入點
- `include/EtherCATMaster.h`, `src/EtherCATMaster.cpp`：SOEM 封裝（初始化、PDO/SDO、週期作業）
- `include/Win32Window.h`, `src/Win32Window.cpp`：Win32 GUI

## 前置需求

- Visual Studio 2019 或更新版本（含 C++ 桌面工作負載）
- CMake 3.15+
- Npcap（建議）或 WinPcap

參考資源：

- SOEM: [OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap: [npcap.com](https://npcap.com/)
- EtherCAT: [ethercat.org](https://www.ethercat.org/)

## 快速建置（PowerShell）

```powershell
# 在專案根目錄執行
# SOEM EtherCAT Master - Windows (中文說明)

本專案使用 C++ 與 SOEM（Simple Open EtherCAT Master）在 Windows 平台上示範 EtherCAT 通訊，並提供一個簡單的 Win32 GUI，涵蓋 PDO、SDO 與 Mailbox 的基本操作。

## 主要功能

- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案結構（重點）

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/
└─ src/
```

主要檔案：

- `src/main.cpp`：程式進入點
- `include/EtherCATMaster.h`, `src/EtherCATMaster.cpp`：SOEM 封裝（初始化、PDO/SDO、週期作業）
- `include/Win32Window.h`, `src/Win32Window.cpp`：Win32 GUI

## 前置需求

- Visual Studio 2019 或更新版本（含 C++ 桌面工作負載）
- CMake 3.15+
- Npcap（建議）或 WinPcap

參考資源：

- SOEM（官方）：[OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap（官方）：[npcap.com](https://npcap.com/)
- EtherCAT：[ethercat.org](https://www.ethercat.org/)

## 快速建置（PowerShell）

```powershell
# 在專案根目錄執行
mkdir build; cd build
cmake ..
cmake --build . --config Release
```

使用 Visual Studio 生成器（範例）：

```powershell
cmake .. -G "Visual Studio 17 2022" -A x64
```

執行（建議以系統管理員權限）：

```powershell
.\build\Release\BigCat.exe
```

## 使用說明（重點）

1. 以系統管理員身份執行編譯後的執行檔。
2. 在 GUI 中選擇或輸入網路介面（例如 `\\Device\\NPF_{...}`）。
3. 點擊「連接」進行從站偵測與配置。

## 範例流程

### SDO 範例

1. 選擇目標從站
2. 輸入 SDO Index（例如 `0x1000`）與 Subindex（例如 `0x00`）
3. 點擊「讀取 SDO」或「寫入 SDO」
# SOEM EtherCAT Master - Windows (中文說明)

本專案使用 C++ 與 SOEM（Simple Open EtherCAT Master）在 Windows 平台上示範 EtherCAT 通訊，並提供一個簡單的 Win32 GUI，涵蓋 PDO、SDO 與 Mailbox 的基本操作。

## 主要功能

- Win32 GUI：從站列表、SDO 編輯、Mailbox 發送/接收、狀態顯示
- PDO：週期性過程資料交換
- SDO：服務資料對象的讀寫

## 專案結構（重點）

```text
.
├─ CMakeLists.txt
├─ README.md
├─ include/
└─ src/
```

主要檔案：

- `src/main.cpp`：程式進入點
- `include/EtherCATMaster.h`, `src/EtherCATMaster.cpp`：SOEM 封裝（初始化、PDO/SDO、週期作業）
- `include/Win32Window.h`, `src/Win32Window.cpp`：Win32 GUI

## 前置需求

- Visual Studio 2019 或更新版本（含 C++ 桌面工作負載）
- CMake 3.15+
- Npcap（建議）或 WinPcap

參考資源：

- SOEM（官方）：[OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap（官方）：[npcap.com](https://npcap.com/)
- EtherCAT：[ethercat.org](https://www.ethercat.org/)

## 快速建置（PowerShell）

```powershell
# 在專案根目錄執行
mkdir build; cd build
cmake ..
cmake --build . --config Release
```

使用 Visual Studio 生成器（範例）：

```powershell
cmake .. -G "Visual Studio 17 2022" -A x64
```

執行（建議以系統管理員權限）：

```powershell
.\build\Release\BigCat.exe
```

## 使用說明（重點）

1. 以系統管理員身份執行編譯後的執行檔。
2. 在 GUI 中選擇或輸入網路介面（例如 `\\Device\\NPF_{...}`）。
3. 點擊「連接」進行從站偵測與配置。

## 範例流程

### SDO 範例

1. 選擇目標從站
2. 輸入 SDO Index（例如 `0x1000`）與 Subindex（例如 `0x00`）
3. 點擊「讀取 SDO」或「寫入 SDO」

### Mailbox 範例

1. 選擇目標從站
2. 在 Mailbox 欄位輸入以空格分隔的十六進位資料（例如 `01 02 03 04`）
3. 點擊「發送 Mailbox」

## 清理與整理建議

建議將臨時或範例檔移至 `archive/`（例如）：

- `src/EtherCATExample_Advanced.cpp`
- `lib/npcap-sdk/Examples-remote/...`
- `COMPILATION_SUCCESS_REPORT.md`, `BUILD_STATUS.md`, `TEST_REPORT.md`
- `.snapshots/`

## 自動整理紀錄

2025-10-03：執行非破壞性的整理作業，將部分暫存或重複檔案移至 `archive/`（保留可還原備份）。

若要移除 build 產物或其他大型檔案，請先提供保留清單。

## 參考資料

- SOEM（官方）：[OpenEtherCATsociety/SOEM](https://github.com/OpenEtherCATsociety/SOEM)
- Npcap（官方）：[npcap.com](https://npcap.com/)
- EtherCAT：[ethercat.org](https://www.ethercat.org/)

***

請注意：此專案為示範用途，與實際 EtherCAT 裝置互動前請在安全環境下測試並遵循裝置手冊。

## 最後更新

- 2025年10月20日

## 版本

- 1.0.0

