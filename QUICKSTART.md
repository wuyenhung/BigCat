# SOEM EtherCAT 快速開始指南

## 步驟 1: 安裝 SOEM 函式庫

在專案根目錄執行：

```powershell
.\setup_soem.ps1
```

如果您已經有 Git，腳本會自動下載 SOEM。
如果沒有 Git，腳本會引導您手動下載。

## 步驟 2: 編譯專案

```powershell
.\build.ps1
```

選擇編譯方式：
- **選項 1**: 自動使用 CMake 編譯（推薦）
- **選項 2**: 在 Visual Studio 中打開專案手動編譯

## 步驟 3: 查找網路介面卡

### 方法 1: 使用 PowerShell

```powershell
Get-NetAdapter | Format-Table Name, InterfaceDescription, ifIndex
```

記錄您要使用的網路介面卡名稱。

### 方法 2: 使用 Npcap

如果已安裝 Npcap，可以使用其提供的工具查看介面列表。

### 方法 3: 使用 SOEM 範例

如果 SOEM 已編譯，可以使用其測試程式：

```powershell
cd lib\SOEM\test\win32\slaveinfo
# 編譯後執行
.\slaveinfo.exe
```

## 步驟 4: 轉換網路介面卡名稱

Windows 中 SOEM 使用的介面名稱格式為：

```
\Device\NPF_{GUID}
```

其中 GUID 是網路介面卡的唯一識別碼。

### 如何獲取完整的裝置名稱？

使用此 PowerShell 命令：

```powershell
Get-NetAdapter | ForEach-Object {
    $guid = (Get-NetAdapterHardwareInfo -Name $_.Name).PnpDeviceID
    if ($guid -match '\{([^}]+)\}') {
        Write-Host "$($_.Name): \Device\NPF_$($matches[1])"
    }
}
```

## 步驟 5: 執行程式

1. **以系統管理員權限執行** `BigCat.exe`

   在 PowerShell 中：
   ```powershell
    Start-Process .\build\Release\BigCat.exe -Verb RunAs
   ```

2. 在程式的「網路介面卡」欄位輸入步驟 4 獲取的裝置名稱

3. 點擊「連接」按鈕

4. 如果一切正常，您應該會看到：
   - 從站列表顯示偵測到的 EtherCAT 裝置
   - 狀態框顯示 "EtherCAT 已啟動!"
   - 週期時間和 WKC 開始更新

## 常見網路介面卡設定

### Intel 網路卡

對於 Intel 乙太網路控制器，建議調整以下設定（裝置管理員 → 網路介面卡 → 內容 → 進階）：

- **Speed & Duplex**: 100 Mbps Full Duplex 或 1.0 Gbps Full Duplex
- **Interrupt Moderation**: Disabled
- **Receive Buffers**: 增加到最大值
- **Transmit Buffers**: 增加到最大值
- **Flow Control**: Disabled
- **Energy Efficient Ethernet**: Disabled

### Realtek 網路卡

- **Speed & Duplex**: 100 Mbps Full Duplex
- **Green Ethernet**: Disabled
- **Offload**: 建議全部停用

## 測試 SDO 讀寫

大多數 EtherCAT 從站都支援以下標準物件：

| Index  | Subindex | 描述                | 存取 |
|--------|----------|---------------------|------|
| 0x1000 | 0x00     | Device Type         | RO   |
| 0x1008 | 0x00     | Device Name         | RO   |
| 0x1009 | 0x00     | Hardware Version    | RO   |
| 0x100A | 0x00     | Software Version    | RO   |
| 0x1018 | 0x00     | Identity Object     | RO   |

### 範例：讀取裝置類型

1. 選擇從站（通常是位置 1）
2. 輸入 Index: `0x1000`
3. 輸入 Subindex: `0x00`
4. 點擊「讀取 SDO」

## 測試 Mailbox

Mailbox 通訊用於非週期性的資料交換。範例：

### 發送簡單的 CoE 訊息

在 Mailbox 欄位輸入（16 進制，空格分隔）：
```
03 10 00 10 00 00 00 00
```

這是一個讀取 0x1000 索引的 CoE 請求。

## 故障排除

### 問題：找不到從站

**檢查項目：**
- [ ] EtherCAT 裝置是否已上電？
- [ ] 網路線是否正確連接？
- [ ] 是否以系統管理員權限執行？
- [ ] Npcap/WinPcap 是否已安裝？
- [ ] 網路介面卡名稱是否正確？

### 問題：週期時間過長

**可能原因：**
- 系統負載過高
- 網路介面卡設定不當
- 需要啟用即時優先權

**解決方式：**
- 關閉不必要的程式
- 調整網路介面卡設定（見上方）
- 在 Task Manager 中提升程式優先權

### 問題：Working Counter 異常

**檢查項目：**
- [ ] 從站狀態是否為 OPERATIONAL？
- [ ] PDO 映射是否正確？
- [ ] 網路品質是否良好？

## 進階主題

### 修改週期時間

在 `EtherCATMaster.cpp` 的 `CyclicTask()` 中修改：

```cpp
// 修改這一行來調整週期時間
Sleep(1);  // 1ms，可改為其他值
```

### 新增自訂 PDO 處理

在 `main.cpp` 中新增回調函數：

```cpp
g_etherCATMaster.SetCyclicCallback([]() {
    // 讀取輸入
    uint8_t input = IOmap[0];  // 假設輸入從偏移 0 開始
    
    // 處理邏輯
    // ...
    
    // 寫入輸出
    IOmap[100] = output_value;  // 假設輸出從偏移 100 開始
});
```

## 更多資源

- SOEM 文件：https://github.com/OpenEtherCATsociety/SOEM
- EtherCAT 技術規範：https://www.ethercat.org/
- Beckhoff 資訊系統：https://infosys.beckhoff.com/

## 需要幫助？

如果您遇到問題，請：

1. 查看狀態框中的錯誤訊息
2. 參考 README.md 中的常見問題
3. 在 GitHub Issues 中搜尋類似問題
4. 提交新的 Issue 並附上詳細資訊

---

祝您使用順利！
