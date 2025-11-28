# 智慧等待 (Smart Wait) — 即時迴圈 CPU 優化模式

> 適用於任何「固定頻率處理 + 事件驅動接收」的即時系統迴圈，
> 例如 UDP 插值器、遊戲引擎 tick、感測器融合、音訊處理管線等。

---

## 1. 問題描述

### 典型的低效迴圈

許多即時系統採用「固定短超時輪詢」的設計：

```
┌──────────────────────────────────────────────┐
│  while (running) {                           │
│      recv();          // 非阻塞接收          │
│      process();       // 處理 + 發送         │
│      wait(500μs);     // 固定超時 ← 問題根源 │
│  }                                           │
└──────────────────────────────────────────────┘
```

```cpp
// ❌ 典型的低效等待 — 固定 500μs 超時
if (!has_new_data) {
    std::unique_lock<std::mutex> lk(mtx);
    cv.wait_for(lk, std::chrono::microseconds(500), [&]() {
        return has_new_data.load() || !running.load();
    });
}
```

### 為什麼這樣不好？

| 問題 | 影響 |
|------|------|
| **超時太短** | 500μs = 每秒 2000 次喚醒，但實際只需 500Hz 發送 |
| **空閒時仍然高頻輪詢** | 即使沒連線、沒資料，仍以 2000Hz 空轉 |
| **超時不對齊發送時機** | 醒來時不一定剛好要發送，白白浪費一次 context switch |
| **mutex lock/unlock 開銷** | 條件變數每次喚醒都涉及 kernel 呼叫 |

**實測結果**: 此模式占整個程式 CPU 消耗的 **40-60%**。

---

## 2. 解法：智慧等待

核心思想：**每次等待的時長 = 距離下次需要工作的精確時間差**。

```
┌───────────────────────────────────────────────────────┐
│  while (running) {                                    │
│      recv();                                          │
│      process_and_send();                              │
│      smart_wait(next_send_time, has_data);  ← 精準   │
│  }                                                    │
└───────────────────────────────────────────────────────┘
```

### 三種狀態的等待策略

```
                    ┌─────────────────────┐
            ┌──────│  檢查系統狀態        │──────┐
            │      └─────────────────────┘      │
            ▼               ▼                   ▼
    ┌──────────────┐ ┌──────────────┐  ┌──────────────┐
    │ 活躍且提前   │ │ 完全空閒     │  │ 落後進度     │
    │ (now < next) │ │ (無資料)     │  │ (now >= next)│
    │              │ │              │  │              │
    │ 精準休眠     │ │ 長休眠       │  │ 立即執行     │
    │ next - now   │ │ 10ms         │  │ (不等待)     │
    └──────────────┘ └──────────────┘  └──────────────┘
```

---

## 3. 完整實作範例 (C++)

### Before（原始代碼）

```cpp
// ❌ 固定 500μs 超時 — CPU 空轉嚴重
while (true) {
    auto now = std::chrono::steady_clock::now();

    // 接收
    recv_packets();

    // 處理 + 發送 (500Hz)
    while (now >= nextSendT) {
        process_and_send();
        nextSendT += send_interval;  // 2ms for 500Hz
    }

    // 等待 — 固定超時，不管實際需要
    if (!has_new_data.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(frame_mutex);
        cv.wait_for(lk, std::chrono::microseconds(500), [&]() {
            return has_new_data.load(std::memory_order_acquire) ||
                   !running.load(std::memory_order_relaxed);
        });
        has_new_data.store(false, std::memory_order_release);
    }
}
```

### After（智慧等待）

```cpp
// ✅ 智慧等待 — 根據實際需求精準休眠
while (true) {
    auto now = std::chrono::steady_clock::now();

    // 接收
    recv_packets();

    // 處理 + 發送 (500Hz)
    while (now >= nextSendT) {
        process_and_send();
        nextSendT += send_interval;  // 2ms for 500Hz
    }

    // ===== 智慧等待開始 =====
    {
        auto now_wait = std::chrono::steady_clock::now();

        if (has_data.load(std::memory_order_acquire) && now_wait < nextSendT) {
            // 【狀態 A】活躍中，但離下次發送還有時間
            // → 精準休眠到 nextSendT，或被新資料喚醒
            auto sleep_dur = nextSendT - now_wait;
            if (sleep_dur > std::chrono::microseconds(100)) {
                std::unique_lock<std::mutex> lk(frame_mutex);
                cv.wait_for(lk, sleep_dur, [&]() {
                    return has_new_data.load(std::memory_order_acquire) ||
                           !running.load(std::memory_order_relaxed);
                });
                has_new_data.store(false, std::memory_order_release);
            }

        } else if (!has_data.load(std::memory_order_acquire)) {
            // 【狀態 B】完全空閒，尚無任何資料
            // → 長休眠 10ms，避免無意義的高頻輪詢
            std::unique_lock<std::mutex> lk(frame_mutex);
            cv.wait_for(lk, std::chrono::milliseconds(10), [&]() {
                return has_new_data.load(std::memory_order_acquire) ||
                       !running.load(std::memory_order_relaxed);
            });
            has_new_data.store(false, std::memory_order_release);
        }
        // 【狀態 C】落後進度 (now >= nextSendT)
        // → 不等待，立即回到迴圈頂部追趕
    }
    // ===== 智慧等待結束 =====
}
```

---

## 4. 關鍵設計原則

### 4.1 三態決策邏輯

```cpp
if (有資料 && 未到發送時間) {
    // 狀態 A: 精準休眠 = nextSendT - now
} else if (完全沒資料) {
    // 狀態 B: 長休眠 (10-50ms)
} else {
    // 狀態 C: 落後了，立即執行不等待
}
```

> [!IMPORTANT]
> **狀態 C（落後追趕）是關鍵**：如果處理耗時超過一個 tick，不要等待，
> 立即回到迴圈頂部追趕進度。這確保系統不會因為一次延遲而累積越來越多的滯後。

### 4.2 為什麼用 `wait_for` 而不是 `sleep`

| 方式 | 優點 | 缺點 |
|------|------|------|
| `Sleep(ms)` | 簡單 | 無法被新資料提前喚醒 |
| `cv.wait_for(dur)` | 可被 `notify_one()` 提前喚醒 | 需搭配 mutex |
| `WaitForSingleObject` | Win32 原生 | 需額外建立 Event |

**推薦 `cv.wait_for`**，因為：
1. 精準休眠到 `nextSendT`
2. 新封包到達時可透過 `cv.notify_one()` 立即喚醒
3. 關閉程式時可透過 `!running` 條件立即喚醒

### 4.3 最小休眠門檻

```cpp
if (sleep_dur > std::chrono::microseconds(100)) {
    // 才值得進入 kernel wait
}
```

> [!TIP]
> 設定一個最小門檻（建議 50-200μs），避免休眠時間極短時反而因為
> kernel 呼叫的 overhead 花費更多時間。小於門檻時直接 spin 或立即回到迴圈頂部。

### 4.4 空閒休眠時長的選擇

```cpp
// 空閒時休眠 10ms
cv.wait_for(lk, std::chrono::milliseconds(10), ...);
```

| 休眠時長 | 喚醒頻率 | CPU 影響 | 延遲影響 |
|---------|---------|---------|---------|
| 1ms | 1000/s | 中 | 極低 |
| **10ms** | **100/s** | **極低** | **可接受** |
| 50ms | 20/s | 幾乎為零 | 明顯 |
| 100ms | 10/s | 零 | 高 |

**建議 10ms**：首次資料到達時最多延遲 10ms，之後由 `notify_one()` 即時喚醒。

---

## 5. 搭配的接收端喚醒

智慧等待必須搭配接收端的 `notify_one()` 才能發揮最大效果：

```cpp
// 接收端（另一個位置或同一迴圈中接收到封包後）
void on_packet_received() {
    // 更新資料
    {
        std::lock_guard<std::mutex> lock(frame_mutex);
        update_frame_data();
        has_new_data.store(true, std::memory_order_release);
    }

    // 喚醒等待中的處理迴圈
    cv.notify_one();  // ← 關鍵：讓 wait_for 提前返回
}
```

> [!WARNING]
> 如果忘記在接收端呼叫 `cv.notify_one()`，智慧等待會退化為
> 「每 2ms 醒一次」（狀態 A），雖然仍比固定 500μs 好，
> 但無法在封包到達時立即回應。

---

## 6. Windows 特殊注意事項

### 6.1 `timeBeginPeriod(1)`

Windows 默認定時器精度為 ~15.6ms。對於需要 500Hz (2ms) 的系統，
必須在程式啟動時呼叫：

```cpp
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// 程式啟動時
timeBeginPeriod(1);  // 設定系統定時器精度為 1ms

// 程式結束時
timeEndPeriod(1);    // 恢復預設
```

> [!CAUTION]
> 不呼叫 `timeBeginPeriod(1)` 的話，`Sleep`/`wait_for` 的最小精度為 ~15ms，
> 會導致 500Hz 發送完全無法實現。

### 6.2 執行緒優先權

```cpp
// 對於即時性要求高的迴圈
SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
// 或更高（需謹慎使用）
SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
```

---

## 7. 效能對比

以 500Hz 發送迴圈 + ~50Hz 接收 為例：

| 指標 | 固定 500μs 超時 | 智慧等待 |
|------|:---------------:|:--------:|
| 喚醒次數/秒 (活躍) | ~2000 | ~550 |
| 喚醒次數/秒 (空閒) | ~2000 | ~100 |
| mutex lock/秒 | ~2000 | ~550 |
| CPU 使用率 (活躍) | 8-15% | 2-5% |
| CPU 使用率 (空閒) | 5-10% | **< 0.5%** |
| 資料到達響應延遲 | 0-500μs | **0μs** (notify) |
| 發送時機精度 | ±500μs 抖動 | **±100μs** |

> [!NOTE]
> 智慧等待**不僅降低 CPU**，還**提升發送時機精度**。
> 因為固定 500μs 超時會引入 0-500μs 的隨機抖動，
> 而智慧等待直接對齊 nextSendT。

---

## 8. 泛化模板

以下是可直接複製到其他專案的模板：

```cpp
/// 智慧等待模板
/// @param next_work_time  下次需要執行工作的時間點
/// @param has_data        是否已有初始資料（atomic<bool>）
/// @param has_new_event   是否有新事件需要處理（atomic<bool>）
/// @param mtx             與條件變數配對的 mutex
/// @param cv              條件變數
/// @param running         程式是否仍在運行（atomic<bool>）
/// @param min_spin_us     最小不進入 kernel wait 的 spin 門檻（微秒）
/// @param idle_sleep_ms   完全空閒時的休眠毫秒數
template<typename Clock>
void smart_wait(
    typename Clock::time_point next_work_time,
    std::atomic<bool>& has_data,
    std::atomic<bool>& has_new_event,
    std::mutex& mtx,
    std::condition_variable& cv,
    std::atomic<bool>& running,
    int min_spin_us = 100,
    int idle_sleep_ms = 10)
{
    auto now = Clock::now();

    if (has_data.load(std::memory_order_acquire) && now < next_work_time) {
        auto sleep_dur = next_work_time - now;
        if (sleep_dur > std::chrono::microseconds(min_spin_us)) {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, sleep_dur, [&]() {
                return has_new_event.load(std::memory_order_acquire) ||
                       !running.load(std::memory_order_relaxed);
            });
            has_new_event.store(false, std::memory_order_release);
        }
    } else if (!has_data.load(std::memory_order_acquire)) {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(idle_sleep_ms), [&]() {
            return has_new_event.load(std::memory_order_acquire) ||
                   !running.load(std::memory_order_relaxed);
        });
        has_new_event.store(false, std::memory_order_release);
    }
    // else: behind schedule → return immediately to catch up
}
```

**使用方式：**

```cpp
while (running.load()) {
    recv_and_process();

    while (now >= nextSendT) {
        send_tick();
        nextSendT += interval;
    }

    smart_wait<std::chrono::steady_clock>(
        nextSendT, has_data, has_new_data,
        frame_mutex, frame_cv, running);
}
```

---

## 9. 適用場景清單

| 場景 | 固定頻率 | 事件來源 | 適用性 |
|------|---------|---------|--------|
| UDP 插值器 / 動感平台 | 500Hz | 網路封包 | ✅ 完美適用 |
| 遊戲伺服器 tick loop | 30-128Hz | 玩家輸入 | ✅ 適用 |
| 音訊處理管線 | 44.1-48kHz | 音訊 buffer | ⚠️ 需更小的 spin 門檻 |
| 感測器融合 (IMU + GPS) | 100-1000Hz | 感測器中斷 | ✅ 適用 |
| GUI 渲染迴圈 | 60Hz | 使用者輸入 | ✅ 適用（idle_sleep 可更長） |
| 機器人控制迴圈 | 100-500Hz | 感測回授 | ✅ 適用 |

---

## 10. 總結

```
固定超時:  wake → 沒事做 → wake → 沒事做 → wake → 終於要發送了
智慧等待:  sleep 到精確發送時間 → 醒來就做事 → sleep → 醒來就做事
```

**一句話**: 不要用固定超時輪詢，改用「算好要睡多久再睡」。
