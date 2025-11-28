# 飽和攻擊防護指南

## 現有防護機制

### 1. **連線限制 (Connection Limiting)**
- **位置**: `src/server/HttpServer.Core.cpp`
- **機制**: 
    - 預設最多允許 32 個並發連線 (`g_maxConnections = 32`)
    - 可用環境變數 `WUSOEM_HTTP_MAX_CONNS` 覆寫
  - 每個新連線都被追蹤在 `g_activeConnections`
  - 超過限制的連線會被拒絕
    - 非 SSE 的同一 IP 新連線會取代舊連線，並保留 1500ms replacement grace window

```cpp
static std::atomic<int> g_activeConnections{0};
int g_maxConnections = 32;
const long long kPerIpReplacementGraceMs = 1500;
```

### 2. **請求節流與速率限制 (Throttling / Rate Limiting)**
- **位置**: `src/server/HttpServer.Core.cpp`, `src/server/HttpServer.Auth.cpp`, `src/server/HttpServer.API.cpp`
- **機制**:
  - HTTP 連線狀態更新最多每 300ms 發送一次 (`kHttpConnThrottleMs = 300`)
  - 被拒絕的請求有 500ms 的節流窗口 (`kRejectedThrottleMs = 500`)
    - 每個 IP 每 1000ms 最多允許 10 個受保護請求 (`kMaxRequestsPerSecond = 10`)
    - `/login`、主要 POST 命令端點與部分 API 路徑超過速率時回傳 429
  - 防止 UI 被狀態更新淹沒

```cpp
const long long kHttpConnThrottleMs = 300;
const long long kRejectedThrottleMs = 500;
constexpr int kMaxRequestsPerSecond = 10;
constexpr int kRateLimitWindowMs = 1000;
```

### 3. **會話超時 (Session Timeout)**
- **位置**: `src/server/HttpServer.Core.cpp`, `src/server/HttpServer.Auth.cpp`
- **機制**:
    - 會話 TTL: 24 小時
    - 不活動超時: 24 小時
  - 自動清理過期會話

```cpp
int g_sessionTtlSeconds = 24 * 60 * 60;
int g_sessionInactivityTimeoutSeconds = 24 * 60 * 60;
```

### 4. **會話 IP 綁定 (IP-Based Sessions)**
- **位置**: `src/server/HttpServer.API.cpp`, `src/server/HttpServer.Auth.cpp`
- **機制**:
  - 每個會話綁定到客戶端 IP
  - 不同 IP 無法共享會話
  - 防止會話盜用

```cpp
g_sessions[clientIP] = { expiry, lastActivity };
```

### 5. **強制登出機制 (Force Logout)**
- **位置**: `src/server/HttpServer.Core.cpp`, `src/server/HttpServer.API.cpp`
- **機制**:
  - 當 PC 開啟瀏覽器時設置強制登出旗標
  - 防止 web 客戶端保持舊會話

### 6. **檔案大小限制 (File Size Limit)**
- **位置**: `src/server/HttpServer.Static.cpp`
- **機制**:
  - 檔案讀取最大限制: 64MB
  - 防止大檔案 DoS 攻擊

```cpp
if (size <= 0 || size > 64 * 1024 * 1024) return std::string();
```

---

## 已實作但可調整的防護機制

### 1. **登入嘗試限制 (Login Attempt Limiting)** ⭐ 優先級: 高
**目前狀態**: ✅ 已實作於 `src/server/HttpServer.Auth.cpp`

**威脅**: 暴力破解密碼攻擊 (Brute Force)

**目前行為**:
```cpp
constexpr int kMaxLoginAttempts = 5;
constexpr int kLoginLockoutSeconds = 300;

if (IsLoginLockedOut(clientIP)) {
    // return 429 Too Many Requests
}
```

### 2. **速率限制 (Rate Limiting)** ⭐ 優先級: 高
**目前狀態**: ✅ 已實作於 `src/server/HttpServer.Auth.cpp`，並由 `/login`、主要 POST 命令端點與部分 API 路徑呼叫

**威脅**: 指令洪水 (Command Flooding)

**目前行為**:
```cpp
constexpr int kMaxRequestsPerSecond = 10;
constexpr int kRateLimitWindowMs = 1000;

if (!CheckRateLimit(clientIP)) {
    // return 429 Too Many Requests
}
```

---

## 仍建議加強的防護機制

### 3. **SSE 訂閱限制 (SSE Subscription Limit)** ⭐ 優先級: 中
**目前狀態**: ⚠️ 部分實現。SSE 連線納入全域 active connection 計數，但目前沒有獨立的 SSE 連線上限；同一 IP replacement 邏輯會略過 SSE socket。

**威脅**: SSE 連線洪水 (占滿帶寬)

**建議實現**:
```cpp
static std::atomic<int> g_activeSseConnections{0};
const int MAX_SSE_CONNECTIONS = 3;  // 最多 3 個 SSE 連線

if (g_activeSseConnections.load() >= MAX_SSE_CONNECTIONS) {
    return "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n";
}
```

### 4. **請求大小限制 (Request Size Limit)** ⭐ 優先級: 中
**目前狀態**: ⚠️ 部分實現 (檔案讀取有限制)

**威脅**: 超大請求洪水 (Request Body Flooding)

**建議實現**:
```cpp
const int MAX_REQUEST_BODY_SIZE = 1024 * 1024;  // 1MB 限制
const int MAX_HEADER_SIZE = 8 * 1024;          // 8KB 限制

if (bodySize > MAX_REQUEST_BODY_SIZE) {
    return "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\n\r\n";
}
```

### 5. **慢速攻擊防護 (Slow Attack Prevention)** ⭐ 優先級: 低
**目前狀態**: ❌ 未實現

**威脅**: 慢速讀寫攻擊 (Slowloris/Slow POST)

**建議實現**:
```cpp
struct ClientState {
    std::chrono::steady_clock::time_point lastActivityTime;
    int bytesReceived = 0;
};

const int INACTIVITY_TIMEOUT_SECONDS = 15;  // 15 秒無活動中斷
const int MIN_BYTES_PER_SECOND = 100;       // 至少 100 B/s

// 定期檢查並關閉緩慢客戶端
```

---

## 客戶端防護 (Web UI)

### 已實現:
✅ **登入後自動重新整理** - 清除舊狀態
✅ **登出後自動重新整理** - 清除敏感資料
✅ **快取禁用** (`cache: 'no-store'`) - 防止快取洩露
✅ **按鈕狀態控制** - 防止未授權操作
✅ **頁面返回自動重新整理** - 清除 bfcache

### 建議加強:
- ⚠️ **限制登入嘗試 UI 提示** - 在客戶端側實現視覺鎖定

```javascript
// auth.js 中添加
const MAX_LOGIN_ATTEMPTS = 5;
let loginAttemptCount = 0;
let loginLocked = false;
let lockoutEndTime = null;

async function pinOk() {
    if (loginLocked) {
        const remainingSeconds = Math.ceil((lockoutEndTime - Date.now()) / 1000);
        msg.textContent = `登入已鎖定 ${remainingSeconds} 秒後重試`;
        return;
    }
    
    // ... 原有邏輯 ...
    
    if (!r.ok) {
        loginAttemptCount++;
        if (loginAttemptCount >= MAX_LOGIN_ATTEMPTS) {
            loginLocked = true;
            lockoutEndTime = Date.now() + 300000;  // 5 分鐘鎖定
            msg.textContent = '登入失敗次數過多，已鎖定 5 分鐘';
        }
    }
}
```

---

## 部署檢查清單

- [ ] 確認連線限制已啟用 (`g_maxConnections = 32`，或用 `WUSOEM_HTTP_MAX_CONNS` 覆寫)
- [ ] 確認會話超時已設置 (目前 TTL 與不活動超時皆為 24 小時)
- [x] 登入嘗試限制已實作 (5 次失敗後鎖定 5 分鐘)
- [x] 速率限制已實作 (每 IP 每秒最多 10 個受保護請求)
- [ ] 實現 SSE 訂閱限制 (最多 3 個連線)
- [ ] 實現請求大小限制 (1MB 限制)
- [ ] 新增監控日誌記錄攻擊嘗試
- [ ] 配置防火牆規則 (選擇性 IP 白名單)
- [ ] 定期審查會話和連線日誌

---

## 監控和日誌記錄

### 建議記錄的事件:
1. ❌ 登入失敗 (失敗次數過多)
2. ❌ 連線拒絕 (超過限制)
3. ❌ 速率限制觸發
4. ❌ 異常請求大小
5. ❌ 會話超時

### 日誌範例:
```
[2025-12-09 10:30:45] LOGIN_FAILED: IP=192.168.1.100, attempts=3
[2025-12-09 10:31:12] CONNECTION_REJECTED: IP=192.168.1.101, active=33/32
[2025-12-09 10:32:00] RATE_LIMIT: IP=192.168.1.102, requests=25/10
[2025-12-09 10:33:15] SESSION_TIMEOUT: IP=192.168.1.100
```

---

## 參考資源

- OWASP: https://owasp.org/www-community/attacks/DoS_attacks
- 速率限制最佳實踐: https://cloud.google.com/architecture/rate-limiting-strategies-techniques
- HTTP 安全頭: https://owasp.org/www-project-secure-headers/

---

## 後續加強順序

1. **已完成基礎項**
  - 登入嘗試限制
  - 每 IP 速率限制
  - 全域連線上限與同 IP 非 SSE 連線取代

2. **中優先級**
   - SSE 訂閱限制
   - 請求大小限制
   - 日誌記錄

3. **低優先級**
   - 慢速攻擊防護
   - 客戶端 UI 鎖定提示

---

## 測試方法

### 測試連線限制:
```powershell
# 嘗試開啟 10 個並發連線
for ($i = 1; $i -le 10; $i++) {
    Start-Job {
        curl -s http://localhost:8080/api/pdo_data -H "X-API-Key: test" > $null
    }
}
```

### 測試登入限制:
```bash
# 嘗試 10 次錯誤登入
for i in {1..10}; do
    curl -X POST http://localhost:8080/login \
         -H "X-API-Key: wrong_password" \
         -w "\n"
done
```

### 測試速率限制:
```bash
# 在 1 秒內發送 20 個請求
ab -n 20 -c 20 http://localhost:8080/api/pdo_data
```

---

**最後更新**: 2026-05-18
**版本**: 1.0
