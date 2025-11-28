# 飽和攻擊防護測試指南

本文件的登入 lockout 與速率限制測試對齊目前 `src/server/HttpServer.Auth.cpp` / `HttpServer.API.cpp` 實作。SSE 專用連線上限、請求 body/header 大小限制與慢速攻擊防護仍屬未完成加強項，請不要把相關範例解讀成已上線行為。

## 🧪 已實現的防護機制測試方法

### 1. 登入嘗試限制 (Login Attempt Limiting)

**規則**:
- 每個 IP 最多允許 5 次失敗的登入嘗試
- 超過限制後，該 IP 將被鎖定 300 秒（5 分鐘）
- 成功登入後，計數器重置

#### 測試方法 1: PowerShell 腳本（自動）

```powershell
# 測試登入嘗試限制
$apiUrl = "http://localhost:8080/login"
$wrongPassword = "wrong_password_12345"
$correctPassword = "your_actual_password"

Write-Host "=== 測試登入嘗試限制 ===" -ForegroundColor Cyan

# 嘗試 6 次錯誤登入（應該在第 5 次後被鎖定）
for ($i = 1; $i -le 6; $i++) {
    Write-Host "嘗試 $i..." -NoNewline
    $response = curl -s -w "\n%{http_code}" -X POST "$apiUrl" `
        -H "X-API-Key: $wrongPassword" 2>&1
    
    $statusCode = $response[-1]
    $body = $response[0..$($response.Length-2)] -join "`n"
    
    Write-Host " HTTP $statusCode" -ForegroundColor $(if ($statusCode -eq "429") { "Red" } else { "Green" })
    
    if ($statusCode -eq "429") {
        Write-Host "  ✓ 登入已被鎖定 (429 Too Many Requests)" -ForegroundColor Red
        break
    }
}

Write-Host "`n等待 10 秒後重試（應該仍然被鎖定）..."
Start-Sleep -Seconds 10

$response = curl -s -w "\n%{http_code}" -X POST "$apiUrl" `
    -H "X-API-Key: $wrongPassword" 2>&1
$statusCode = $response[-1]
if ($statusCode -eq "429") {
    Write-Host "✓ 仍然被鎖定（正確）" -ForegroundColor Green
}

Write-Host "`n等待 300 秒後（模擬，實際等待 5 分鐘）..."
Write-Host "或執行正確登入來重置計數器..."

# 測試成功登入是否清除計數
Write-Host "`n嘗試正確登入..." -NoNewline
$response = curl -s -w "\n%{http_code}" -X POST "$apiUrl" `
    -H "X-API-Key: $correctPassword" 2>&1
$statusCode = $response[-1]
Write-Host " HTTP $statusCode" -ForegroundColor $(if ($statusCode -eq "200") { "Green" } else { "Red" })

if ($statusCode -eq "200") {
    Write-Host "✓ 正確登入成功，計數器已重置" -ForegroundColor Green
}
```

#### 測試方法 2: 手動測試 (curl 命令)

```bash
# 連續嘗試 6 次錯誤登入
for i in {1..6}; do
    echo "嘗試 $i:"
    curl -i -X POST http://localhost:8080/login \
         -H "X-API-Key: wrong_password" 2>&1 | grep "HTTP"
    sleep 1
done

# 預期結果:
# 嘗試 1: HTTP/1.1 401 Unauthorized
# 嘗試 2: HTTP/1.1 401 Unauthorized
# 嘗試 3: HTTP/1.1 401 Unauthorized
# 嘗試 4: HTTP/1.1 401 Unauthorized
# 嘗試 5: HTTP/1.1 401 Unauthorized
# 嘗試 6: HTTP/1.1 429 Too Many Requests ← 被鎖定
```

---

### 2. 速率限制 (Rate Limiting)

**規則**:
- 每個 IP 每秒最多允許 10 個請求
- 超過限制的請求返回 429 Too Many Requests
- 每秒時間窗口重置

#### 測試方法 1: PowerShell 腳本（自動）

```powershell
# 測試速率限制
Write-Host "=== 測試速率限制 ===" -ForegroundColor Cyan

$apiUrl = "http://localhost:8080/api/pdo_data"
$apiKey = "your_actual_password"
$requestCount = 15

Write-Host "在 1 秒內發送 $requestCount 個請求..."
$responses = @()

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()

for ($i = 1; $i -le $requestCount; $i++) {
    $response = curl -s -o /dev/null -w "%{http_code}" -X GET "$apiUrl" `
        -H "X-API-Key: $apiKey" 2>&1
    $responses += $response
    Write-Host "  請求 $i: HTTP $response" -ForegroundColor $(if ($response -eq "429") { "Red" } else { "Green" })
}

$stopwatch.Stop()
$elapsed = $stopwatch.ElapsedMilliseconds

Write-Host "`n結果:"
$count429 = @($responses | Where-Object { $_ -eq "429" }).Count
$count200 = @($responses | Where-Object { $_ -eq "200" }).Count
Write-Host "  200 (成功): $count200 個"
Write-Host "  429 (限制): $count429 個"
Write-Host "  耗時: ${elapsed}ms"

if ($count429 -gt 0) {
    Write-Host "`n✓ 速率限制正常運作（超過 10 個請求時開始返回 429）" -ForegroundColor Green
}
```

#### 測試方法 2: Apache Bench 工具

```bash
# 使用 Apache Bench 進行負載測試
ab -n 20 -c 5 \
   -H "X-API-Key: your_actual_password" \
   http://localhost:8080/api/pdo_data

# 預期結果:
# 應該看到大部分請求返回 200，
# 但一些請求返回 429 Too Many Requests
```

#### 測試方法 3: 串列連續請求

```bash
# 快速發送 15 個請求
for i in {1..15}; do
    echo "請求 $i:"
    curl -s -o /dev/null -w "HTTP %{http_code}\n" \
         -X GET http://localhost:8080/api/pdo_data \
         -H "X-API-Key: your_actual_password" &
done
wait

# 預期結果:
# 前 10 個: HTTP 200
# 後 5 個: HTTP 429
```

---

### 3. 命令端點速率限制

**端點**:
- `/connect`, `/disconnect`, `/start_auto`, `/stop_auto`
- `/auto/immediate_stop`, `/auto/fault_reset`, `/auto/down_to_origin`

**測試方法**:

```powershell
# 快速發送多個 /start_auto 命令
$apiKey = "your_actual_password"
$endpoint = "http://localhost:8080/start_auto"

Write-Host "=== 測試命令端點速率限制 ===" -ForegroundColor Cyan

for ($i = 1; $i -le 15; $i++) {
    $response = curl -s -o /dev/null -w "%{http_code}" -X POST "$endpoint" `
        -H "X-API-Key: $apiKey" 2>&1
    Write-Host "  命令 $i: HTTP $response" -ForegroundColor $(if ($response -eq "429") { "Red" } else { "Green" })
}
```

---

## 📊 監控和日誌記錄

### 檢查伺服器日誌

目前登入嘗試與速率限制邏輯位於 `src/server/HttpServer.Auth.cpp`。若要新增安全事件記錄，應在該檔案的 `RecordFailedLoginAttempt()` / `CheckRateLimit()` 附近加入非阻塞、低頻率的紀錄；不要在高頻路徑加入同步磁碟 I/O。

```cpp
static void RecordFailedLoginAttempt(const std::string& clientIP)
{
    std::lock_guard<std::mutex> lk(g_loginAttemptMutex);
    auto& attempt = g_loginAttempts[clientIP];
    attempt.failedCount++;
    attempt.lastAttemptTime = std::chrono::steady_clock::now();
    
    // 添加日誌記錄
    std::cerr << "[SECURITY] Failed login from IP: " << clientIP 
              << ", attempts: " << attempt.failedCount << std::endl;
}
```

### 預期的日誌輸出

```
[SECURITY] Failed login from IP: 192.168.1.100, attempts: 1
[SECURITY] Failed login from IP: 192.168.1.100, attempts: 2
[SECURITY] Failed login from IP: 192.168.1.100, attempts: 3
[SECURITY] Failed login from IP: 192.168.1.100, attempts: 4
[SECURITY] Failed login from IP: 192.168.1.100, attempts: 5
[SECURITY] Rate limit or lockout for IP: 192.168.1.100
```

---

## 🔧 可配置參數

如需調整防護策略，目前需修改 split HTTP server 檔案中的常數；尚未提供 INI 或 Web UI 設定介面：

```cpp
// 登入嘗試限制
// src/server/HttpServer.Auth.cpp
constexpr int kMaxLoginAttempts = 5;               // 改為 3 或 10
constexpr int kLoginLockoutSeconds = 300;          // 改為 600 (10分鐘) 或 60 (1分鐘)

// 速率限制
// src/server/HttpServer.Auth.cpp
constexpr int kMaxRequestsPerSecond = 10;          // 改為 20 或 5
constexpr int kRateLimitWindowMs = 1000;           // 改為 2000 (2秒) 或 500 (0.5秒)

// src/server/HttpServer.Core.cpp
int g_maxConnections = 32;                         // 也可用 WUSOEM_HTTP_MAX_CONNS 覆寫
```

---

## ✅ 測試檢查清單

- [ ] 登入失敗 5 次後被鎖定
- [ ] 被鎖定後 5 分鐘內無法登入
- [ ] 成功登入後計數器重置
- [ ] 超過 10 個請求/秒時返回 429
- [ ] 等待 1 秒後速率限制重置
- [ ] 命令端點也受到速率限制保護
- [ ] SSE 連線仍然正常工作
- [ ] 日誌正確記錄攻擊嘗試

---

## 🚀 部署建議

### 生產環境配置

```cpp
// 更嚴格的設置
constexpr int kMaxLoginAttempts = 3;               // 更嚴格
constexpr int kLoginLockoutSeconds = 600;          // 10 分鐘鎖定

// 根據實際需求調整速率限制
constexpr int kMaxRequestsPerSecond = 20;          // 如果有更多合法用戶
```

### 監控建議

- 定期檢查被鎖定的 IP
- 設置告警當發現異常登入嘗試
- 記錄所有 429 響應以檢測攻擊模式
- 考慮添加 IP 黑名單機制

---

## 🔐 額外安全措施

### 1. 客戶端側限制 (部分實作)

目前 `resources/web/auth.js` 會辨識伺服器回傳的 429，並顯示「嘗試次數過多，請稍後再試」。下方本地計數/倒數鎖定是可選的前端加強範例，不是目前已上線的完整行為。

```javascript
// 建議在登入失敗時添加指數退避
const MAX_LOGIN_ATTEMPTS = 5;
let loginAttemptCount = 0;

async function pinOk() {
    if (loginAttemptCount >= MAX_LOGIN_ATTEMPTS) {
        const remainingSeconds = 300;  // 5 分鐘
        msg.textContent = `登入已鎖定，請在 ${remainingSeconds} 秒後重試`;
        return;
    }
    
    // ... 登入邏輯 ...
    
    if (!r.ok) {
        loginAttemptCount++;
    }
}
```

### 2. 防火牆規則 (可選)

```bash
# Windows Firewall (PowerShell 管理員)
Add-NetFirewallRule -DisplayName "HTTP Server Rate Limit" `
    -Direction Inbound -Action Block -Protocol TCP -LocalPort 8080 `
    -RemoteAddress "192.168.1.50" -Description "Block suspicious IP"
```

---

**最後更新**: 2026-05-18
**版本**: 1.0
