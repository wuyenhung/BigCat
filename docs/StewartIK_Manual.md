# StewartIK 使用手冊

## Stewart 平台逆運動學與正運動學計算模組

**版本**: 2.0  
**更新日期**: 2026-01-30  
**命名空間**: `WuSOEM`

---

## 目錄

1. [概述](#概述)
2. [檔案結構](#檔案結構)
3. [資料結構](#資料結構)
4. [API 參考](#api-參考)
5. [使用範例](#使用範例)
6. [演算法說明](#演算法說明)
7. [座標系統定義](#座標系統定義)
8. [錯誤處理](#錯誤處理)
9. [效能優化](#效能優化)
10. [常見問題](#常見問題)

---

## 概述

`StewartIK` 是一個高精度的 6-DOF Stewart 平台運動學計算模組，提供：

- **逆運動學 (IK)**: 給定平台姿態 (X, Y, Z, Roll, Pitch, Yaw)，計算 6 支力臂的角度
- **正運動學 (FK)**: 給定 6 支力臂角度，反算平台姿態
- **座標轉換**: 平台本地座標系與世界座標系之間的轉換

### 特性

- 使用 `long double` 高精度浮點數運算
- 逆運動學精度達 1e-12 mm 容差
- 正運動學使用解析雅可比矩陣 + Levenberg-Marquardt 優化
- 完整的錯誤檢測與報告機制

---

## 檔案結構

```
include/
  └── StewartIK.h      # 頭文件：結構定義與函式宣告
src/
  └── StewartIK.cpp    # 實作：運動學計算核心
```

---

## 資料結構

### StewartParams - 機構參數

```cpp
struct StewartParams {
    long double platformEdgeA = 600.0L;   // 平台六邊形邊長 A (mm)
    long double platformEdgeB = 100.0L;   // 平台六邊形邊長 B (mm)
    long double baseEdgeA = 600.0L;       // 底座六邊形邊長 A (mm)
    long double baseEdgeB = 100.0L;       // 底座六邊形邊長 B (mm)
    long double rodLength = 500.0L;       // 連桿長度 (mm)
    long double servoArmLength = 300.0L;  // 力臂長度 (mm)
    long double servoAngleLimit = 75.0L;  // 力臂角度限制 (±度)
    long double fisheyeAngleLimit = 30.0L;// 魚眼軸承角度限制 (度)
    long double baseHeight = 0.0L;        // 底座高度 (mm)
    std::array<long double,6> servoAxisAngle{{0,300,120,60,240,180}}; // 各力臂軸向角度 (度)
};
```

#### 六邊形結構說明

平台和底座都是對稱六邊形，邊長交替排列 (A-B-A-B-A-B)：

```
        頂點2
       /     \
     B/       \A
     /         \
  頂點3       頂點1
    |           |
   A|           |A
    |           |
  頂點4       頂點0
     \         /
     B\       /A
       \     /
        頂點5
```

### Pose6D - 6 自由度姿態

```cpp
struct Pose6D {
    long double x = 0.0L;       // X 位移 (mm)
    long double y = 0.0L;       // Y 位移 (mm)
    long double z = 450.0L;     // Z 高度 (mm)
    long double roll = 0.0L;    // 繞 X 軸旋轉 (度)
    long double pitch = 0.0L;   // 繞 Y 軸旋轉 (度)
    long double yaw = 0.0L;     // 繞 Z 軸旋轉 (度)
};
```

#### 角度慣例

- **Roll**: 繞 X 軸旋轉（左右傾斜）
- **Pitch**: 繞 Y 軸旋轉（前後傾斜）
- **Yaw**: 繞 Z 軸旋轉（水平旋轉）
- 旋轉順序: Rz(yaw) × Ry(pitch) × Rx(roll)
- 正向: 右手定則

### IKResult - 逆運動學結果

```cpp
struct IKResult {
    std::array<long double,6> servoAngles;          // 6 支力臂角度 (度)
    std::array<std::array<long double,3>,6> armEndPoints;    // 力臂端點世界座標
    std::array<std::array<long double,3>,6> platformPoints;  // 平台連接點世界座標
    bool valid = true;                              // 計算是否有效
    std::vector<std::string> errors;                // 錯誤訊息列表
};
```

### ForwardResult - 正運動學結果

```cpp
struct ForwardResult {
    Pose6D pose;                      // 計算出的平台姿態
    bool valid = false;               // 計算是否有效
    std::vector<std::string> errors;  // 錯誤訊息列表
};
```

### Pose6DLimits - 姿態限制設定

```cpp
struct Pose6DLimits {
    // 硬限制（世界座標系）
    long double maxX = 150.0L;       // X 平移 (mm)
    long double maxY = 150.0L;       // Y 平移 (mm)
    long double maxZ = 150.0L;       // Z 平移 (mm)
    long double maxRoll = 25.0L;     // Roll 旋轉 (°)
    long double maxPitch = 25.0L;    // Pitch 旋轉 (°)
    long double maxYaw = 10.0L;      // Yaw 旋轉 (°)
    // 機構限制（供參考）
    long double fisheyeAngleLimit = 35.0L;
    long double servoAngleLimit = 65.0L;
    // 奇點門檻（供未來使用）
    long double singularityVerticalAngle = 18.0L;
    long double singularityHorizontalAngle = 87.0L;
    long double singularityExtendRatio = 95.0L;
    long double singularityContractRatio = 105.0L;
    // 動態 Z 觸發縮放參數（預設值集中定義於 include/StewartIK.h: Pose6DDynamicDefaults）
    long double dynamicLimitStartRatio = Pose6DDynamicDefaults::kStartRatio;   // 預設 0.50（50%）
    long double dynamicLimitMinScale   = Pose6DDynamicDefaults::kMinScale;     // 預設 0.50（50%）
};
```

**說明**: `Pose6DLimits` 可從 `Par/machine.xml` 的 `<Limits><MaxPoseLimits>` 區段載入。

### Pose6DValidationResult - 姿態驗證結果

```cpp
struct Pose6DValidationResult {
    bool valid = true;                        // 是否在硬限制內
    Pose6DViolationKind violationKind;        // 違規軸
    long double limitValue;                   // 限制值
    long double actualValue;                  // 實際值
    std::string message;                      // UTF-8 錯誤訊息
    std::wstring messageW;                    // UTF-16 錯誤訊息 (UI)
    bool dynamicScalingActive = false;        // 動態縮放是否啟用中
    Pose6D clampedPose;                       // 飽和後的姿態（供 IK 使用）
};
```

**重要**: `clampedPose` 是經過動態縮放飽和的安全姿態，應傳入 `CalculateInverse` 而非原始 `pose`。無縮放時 `clampedPose` 等同輸入。

---

## API 參考

### 主要函式

#### CalculateInverse - 逆運動學

```cpp
static IKResult CalculateInverse(const StewartParams &params, const Pose6D &pose);
```

**功能**: 給定平台姿態，計算 6 支力臂的角度。

**參數**:
- `params`: 機構參數
- `pose`: 目標姿態 (X, Y, Z, Roll, Pitch, Yaw)

**返回**: `IKResult` 包含力臂角度與驗證資訊

**範例**:
```cpp
StewartParams params;
Pose6D pose;
pose.z = 450.0L;
pose.yaw = 5.0L;

auto result = StewartIK::CalculateInverse(params, pose);
if (result.valid) {
    for (int i = 0; i < 6; ++i) {
        std::cout << "力臂" << (i+1) << ": " << result.servoAngles[i] << "°\n";
    }
}
```

---

#### CalculateForward - 正運動學

```cpp
static ForwardResult CalculateForward(
    const StewartParams &params, 
    const std::array<long double,6> &inputServoAngles, 
    const Pose6D &initialPose
);
```

**功能**: 給定 6 支力臂角度，反算平台姿態。

**參數**:
- `params`: 機構參數
- `inputServoAngles`: 6 支力臂角度 (度)
- `initialPose`: 初始猜測姿態（用於迭代求解）

**返回**: `ForwardResult` 包含計算出的姿態

**範例**:
```cpp
StewartParams params;
std::array<long double,6> angles = {5.13, 13.45, 5.13, 13.45, 5.13, 13.45};
Pose6D initialGuess;
initialGuess.z = 450.0L;

auto result = StewartIK::CalculateForward(params, angles, initialGuess);
if (result.valid) {
    std::cout << "Yaw = " << result.pose.yaw << "°\n";
}
```

---

### 輔助函式

#### GetRotationMatrix - 取得旋轉矩陣

```cpp
static std::array<std::array<long double,3>,3> GetRotationMatrix(
    long double roll, long double pitch, long double yaw
);
```

**功能**: 根據 Roll/Pitch/Yaw 角度計算 3×3 旋轉矩陣。

**參數**: 角度均為度數

**旋轉順序**: Rz(yaw) × Ry(pitch) × Rx(roll)

---

#### CalculateSymmetricHexagonPoints - 計算對稱六邊形頂點

```cpp
static std::array<std::array<long double,3>,6> CalculateSymmetricHexagonPoints(
    long double edgeA, long double edgeB, long double zHeight
);
```

**功能**: 計算以原點為中心的對稱六邊形 6 個頂點座標。

**參數**:
- `edgeA`, `edgeB`: 交替邊長 (A-B-A-B-A-B)
- `zHeight`: Z 軸高度

---

#### TransformPlatformToWorldDelta - 座標系轉換

```cpp
static std::array<long double,3> TransformPlatformToWorldDelta(
    const std::array<long double,3>& localDelta,
    const Pose6D& currentPose
);
```

**功能**: 將平台本地座標系的位移向量轉換到世界座標系。

**用途**: 當使用者在平台座標系下輸入移動量時，需要轉換到世界座標系。

**參數**:
- `localDelta`: 平台本地座標系下的位移向量 {dx, dy, dz}
- `currentPose`: 平台當前姿態（用於建立旋轉矩陣）

**返回**: 世界座標系下的位移向量

**數學原理**: worldDelta = R * platformDelta

---

#### TransformWorldToPlatformDelta - 逆座標系轉換

```cpp
static std::array<long double,3> TransformWorldToPlatformDelta(
    const std::array<long double,3>& worldDelta,
    const Pose6D& currentPose
);
```

**功能**: 將世界座標系的位移向量轉換到平台本地座標系（逆轉換）。

**用途**: 當需要在平台座標系下表示世界座標系的移動量時使用。

**參數**:
- `worldDelta`: 世界座標系下的位移向量 {dx, dy, dz}
- `currentPose`: 平台當前姿態（用於建立旋轉矩陣）

**返回**: 平台本地座標系下的位移向量

**數學原理**: platformDelta = R^T * worldDelta
（利用旋轉矩陣的正交性質：R^(-1) = R^T）

---

#### NormalizeAngleDeg - 角度正規化

```cpp
static long double NormalizeAngleDeg(long double a);
```

**功能**: 將角度正規化到 [-180°, 180°] 範圍。

---

#### ValidatePose6D - 姿態前置驗證

```cpp
static Pose6DValidationResult ValidatePose6D(
    const Pose6D& pose,
    long double initialHeight,
    const Pose6DLimits& limits
);
```

**功能**: 在 IK 計算前驗證姿態是否在安全範圍內，並進行動態行程縮放。

**雙層保護機制**:

| 層級 | 檢查對象 | 違規行為 |
|------|---------|---------|
| **硬限制** | 原始 `maxX/Y/Z/Roll/Pitch/Yaw` | **凍結運動**（valid=false） |
| **動態縮放** | Z 位置 ≥ 50% maxZ 時，septic S-curve 縮小 X/Y/Roll/Pitch/Yaw | **飽和 clamp**（不凍結） |

**動態縮放公式**（Z 觸發，septic S-curve / 7 階 smoothstep）:
```
zRatio = |pose.z - initialHeight| / maxZ
if zRatio >= kStartRatio (預設 0.50):
    t   = (zRatio - 0.50) / 0.50                     // 正規化 0→1
    st  = −20t⁷ + 70t⁶ − 84t⁵ + 35t⁴                  // septic smoothstep
    scale = 1.0 − st × 0.50                           // 輸出 1.0 → 0.50
    effMaxX = maxX × scale
    effMaxRoll = maxRoll × scale
    ... (Y, Pitch, Yaw 同理；Z 本身不縮放)
```

> **為何使用 septic S-curve？** 線性縮放在啟動／結束點有導數不連續，
> 造成縮放速度突變 → 瞬間加速度過大。cosine easing 僅保證一階導數連續；
> quintic 保證到二階（加速度連續）。septic (7 階) smoothstep
> `s(t)=−20t⁷+70t⁶−84t⁵+35t⁴` 的一階/二階/三階導數在 t=0 和 t=1 皆為零，
> 確保 jerk 在縮放轉折點也連續，S 曲線特性加倍。

**參數**:
- `pose`: 世界座標系姿態（z 為絕對值）
- `initialHeight`: 初始高度（home position Z）
- `limits`: 限制設定（從 machine.xml 載入）

**返回**: `Pose6DValidationResult`
- `valid`: 是否在硬限制內
- `dynamicScalingActive`: Z 是否在縮放區域
- `clampedPose`: 飽和後的安全姿態，應傳入 `CalculateInverse`

**範例**:
```cpp
Pose6DLimits limits;
Pose6D targetPose;
targetPose.z = 550.0L;  // Z 位移 = 100mm (67% of 150mm)

auto valid = StewartIK::ValidatePose6D(targetPose, 450.0L, limits);
if (!valid.valid) {
    // 超出硬限制：凍結
} else {
    // 使用 clampedPose 做 IK
    auto ik = StewartIK::CalculateInverse(params, valid.clampedPose);
}
```

---

## 使用範例

### 基本逆運動學計算

```cpp
#include "StewartIK.h"
using namespace WuSOEM;

int main() {
    // 設定機構參數
    StewartParams params;
    params.platformEdgeA = 600.0L;
    params.platformEdgeB = 100.0L;
    params.baseEdgeA = 600.0L;
    params.baseEdgeB = 100.0L;
    params.rodLength = 500.0L;
    params.servoArmLength = 300.0L;
    
    // 設定目標姿態
    Pose6D pose;
    pose.x = 0.0L;
    pose.y = 0.0L;
    pose.z = 450.0L;
    pose.roll = 0.0L;
    pose.pitch = 0.0L;
    pose.yaw = 5.0L;  // 旋轉 5 度
    
    // 計算逆運動學
    auto result = StewartIK::CalculateInverse(params, pose);
    
    if (result.valid) {
        std::cout << "計算成功！力臂角度:\n";
        for (int i = 0; i < 6; ++i) {
            std::cout << "  力臂" << (i+1) << ": " 
                      << result.servoAngles[i] << "°\n";
        }
    } else {
        std::cout << "計算失敗！錯誤:\n";
        for (const auto& err : result.errors) {
            std::cout << "  - " << err << "\n";
        }
    }
    
    return 0;
}
```

### 完整驗證流程 (IK → FK → 比對)

```cpp
#include "StewartIK.h"
#include <cmath>
using namespace WuSOEM;

bool verifyKinematics(const StewartParams& params, const Pose6D& targetPose) {
    // Step 1: 逆運動學
    auto ikResult = StewartIK::CalculateInverse(params, targetPose);
    if (!ikResult.valid) return false;
    
    // Step 2: 正運動學
    auto fkResult = StewartIK::CalculateForward(
        params, ikResult.servoAngles, targetPose
    );
    if (!fkResult.valid) return false;
    
    // Step 3: 比對
    const double tolerance = 1e-6;
    bool match = true;
    match &= std::abs(targetPose.x - fkResult.pose.x) < tolerance;
    match &= std::abs(targetPose.y - fkResult.pose.y) < tolerance;
    match &= std::abs(targetPose.z - fkResult.pose.z) < tolerance;
    match &= std::abs(targetPose.roll - fkResult.pose.roll) < tolerance;
    match &= std::abs(targetPose.pitch - fkResult.pose.pitch) < tolerance;
    match &= std::abs(targetPose.yaw - fkResult.pose.yaw) < tolerance;
    
    return match;
}
```

---

## 演算法說明

### 逆運動學演算法

1. **計算平台連接點世界座標**
   - 使用旋轉矩陣將平台本地點轉換到世界座標

2. **對每支力臂 (i = 0..5)**:
   - 計算基座點到平台點的目標向量
   - 檢查目標距離是否在可達範圍內
   - 使用餘弦定理計算初始力臂角度估計
   - **牛頓迭代優化** (最多 50 次迭代，容差 1e-12 mm)
     - 計算當前連桿長度誤差
     - 使用數值微分計算梯度
     - 更新力臂角度

3. **輸出優化後的角度**（關鍵修正點）

### 正運動學演算法

使用 **Newton-Raphson + Levenberg-Marquardt** 方法：

1. **初始猜測**: 使用提供的初始姿態或中立位置

2. **迭代求解** (最多 50 次迭代):
   - 計算當前姿態下的 6 支連桿長度
   - 計算誤差向量 (當前長度 - 設定長度)
   - **計算解析雅可比矩陣** ∂L/∂(x,y,z,roll,pitch,yaw)
   - 求解 LM 修正方程: (JᵀJ + λI)δ = Jᵀe
   - 更新姿態，調整阻尼係數 λ

3. **收斂判定**: 總誤差 < 1e-12 mm

### 解析雅可比矩陣

對於每支連桿 i，雅可比矩陣元素為：

```
∂Lᵢ/∂x = dxᵢ / Lᵢ
∂Lᵢ/∂y = dyᵢ / Lᵢ  
∂Lᵢ/∂z = dzᵢ / Lᵢ
∂Lᵢ/∂roll = (d · ∂P/∂roll) / Lᵢ
∂Lᵢ/∂pitch = (d · ∂P/∂pitch) / Lᵢ
∂Lᵢ/∂yaw = (d · ∂P/∂yaw) / Lᵢ
```

其中 d = 平台點 - 力臂端點，∂P/∂angle 為旋轉矩陣對角度的偏導數。

---

## 座標系統定義

### 世界座標系 (World Frame)

```
      Z ↑
        |
        |
        +-----→ X
       /
      ↙ Y
```

- **原點**: 底座中心
- **X 軸**: 水平向右
- **Y 軸**: 水平向前
- **Z 軸**: 垂直向上

### 平台座標系 (Platform Frame)

- **原點**: 平台中心
- 軸向與世界座標系初始對齊
- 隨平台旋轉

### 力臂軸向角度 (servoAxisAngle)

定義每支力臂在 XY 平面上的擺動方向：

| 力臂索引 | 預設角度 | 方向說明 |
|---------|---------|---------|
| 0 | 0° | 正 X 方向 |
| 1 | 300° | 東北方向 |
| 2 | 120° | 西北方向 |
| 3 | 60° | 東北方向 |
| 4 | 240° | 西南方向 |
| 5 | 180° | 負 X 方向 |

---

## 動態行程縮放

### 目的

Stewart 平台在中間位置時 6-DOF 行程最大，但上下到極限 Z 高度時因機構干涉不能移動大角度或大位移。動態縮放根據 Z 位置自動調整其他軸的允許範圍。

### 行為表（maxZ=150mm, initialHeight=450mm, cosine easing）

| Z 位置 | Z 比例 | 縮放係數 | X/Y 有效限制 | Roll 有效限制 | Yaw 有效限制 |
|--------|--------|---------|-------------|--------------|-------------|
| 450mm (±0mm) | 0% | 1.00 | ±150mm | ±25° | ±10° |
| 495mm (±45mm) | 30% | 1.00（未啟動） | ±150mm | ±25° | ±10° |
| 525mm (±75mm) | 50% | 1.00 → 開始縮放 | ±150mm | ±25° | ±10° |
| 540mm (±90mm) | 60% | 0.95 | ±143mm | ±23.8° | ±9.5° |
| 555mm (±105mm) | 70% | 0.83 | ±124mm | ±20.7° | ±8.3° |
| 570mm (±120mm) | 80% | 0.67 | ±101mm | ±16.8° | ±6.7° |
| 585mm (±135mm) | 90% | 0.55 | ±82mm | ±13.7° | ±5.5° |
| 600mm (±150mm) | 100% | 0.50 | ±75mm | ±12.5° | ±5° |

### 重要行為

- **非線性 cosine easing 曲線**：縮放過程平滑，兩端導數為零，避免線性縮放造成的瞬間加速度過大
- **縮放是軟限制（clamp），不是硬限制（freeze）**：當姿態超出縮放後的有效範圍時，會被飽和到邊界值，平台仍可繼續動作
- **硬限制（maxX/Y/Z/Roll/Pitch/Yaw）超出時才會凍結**
- 縮放啟用/關閉時會觸發單次 UI 警報聲與 log 訊息
- 參數可透過 `Par/machine.xml` 的 `<DynamicLimitStartRatio>` 和 `<DynamicLimitMinScale>` 調整

---

## 錯誤處理

### 常見錯誤類型

| 錯誤訊息 | 原因 | 解決方案 |
|---------|------|---------|
| `腿X: 目標距離超出可達範圍` | 姿態太極端，連桿無法到達 | 減小位移或角度 |
| `腿X: 力臂角度超出限制` | 力臂需要超過 ±75° | 調整姿態或增大限制 |
| `腿X: 連桿長度誤差` | 迭代未收斂 | 檢查機構參數 |
| `姿態求解失敗` | 正運動學無法收斂 | 提供更好的初始猜測 |
| `Pose6D 限制違規` | 超出硬限制（maxX/Y/Z/Roll/Pitch/Yaw） | 調整姿態至安全範圍 |

### 錯誤處理範例

```cpp
auto result = StewartIK::CalculateInverse(params, pose);

if (!result.valid) {
    for (const auto& error : result.errors) {
        // 記錄錯誤
        logger.error(error);
        
        // 解析錯誤類型
        if (error.find("超出可達範圍") != std::string::npos) {
            // 處理超出範圍錯誤
        } else if (error.find("力臂角度") != std::string::npos) {
            // 處理角度限制錯誤
        }
    }
}
```

---

## 效能優化

### 計算複雜度

| 函式 | 複雜度 | 說明 |
|-----|--------|------|
| CalculateInverse | O(1) | 固定 6 支力臂，每支最多 50 次迭代 |
| CalculateForward | O(1) | 最多 50 次迭代 × 2 個初始猜測 |

### 優化建議

1. **快取機構參數**: 如果參數不變，可以預先計算基座和平台頂點

2. **初始猜測**: 正運動學時提供接近實際值的初始姿態可加速收斂

3. **批次計算**: 連續計算時使用上一次結果作為下一次初始值

---

## 常見問題

### Q1: 為什麼正運動學結果與輸入姿態有微小誤差？

**A**: 這是正常的浮點數精度限制。使用 `long double` 後，典型誤差在 1e-12 mm 以下，對實際應用無影響。

### Q2: 如何處理多解問題？

**A**: Stewart 平台的逆運動學在正常工作範圍內通常只有一個可行解。如果力臂角度超過限制，表示該姿態不可達。

### Q3: 正運動學求解失敗怎麼辦？

**A**: 
1. 確認輸入的力臂角度在合理範圍內
2. 提供更接近實際的初始猜測
3. 檢查機構參數是否正確

### Q4: 可以修改力臂角度限制嗎？

**A**: 可以，修改 `params.servoAngleLimit`。但請確保實際機構能達到該角度。

### Q5: 動態縮放何時觸發？如何調整門檻？

**A**: 當平台 Z 位置達 maxZ 的 50% 時自動啟動，使用 cosine easing 非線性曲線平滑縮小。預設常數集中定義於 `include/StewartIK.h` 的 `Pose6DDynamicDefaults`，runtime 可透過 `Par/machine.xml` `<MaxPoseLimits>` 覆寫。

### Q6: 動態縮放會讓平台卡住嗎？

**A**: 不會。縮放是**軟限制（clamp/飽和）**，超出時會將姿態飽和到有效邊界值繼續動作，不會凍結。只有超出原始硬限制（maxX=±150mm 等）才會真正凍結。

### Q7: 如何驗證我的機構參數是否正確？

**A**: 
1. 使用中立位置測試：Z=450, 其他為 0
2. 所有力臂角度應該相等
3. 進行 IK→FK 往返測試，結果應該一致

---

## 版本歷史

| 版本 | 日期 | 變更內容 |
|-----|------|---------|
| 2.1 | 2026-05-28 | 新增 Pose6DLimits / ValidatePose6D；Z 觸發動態行程縮放（clamp 軟限制）；PDO filter 追蹤誤差雙向卡住修正 |
| 2.0 | 2026-01-30 | 修正逆運動學角度輸出 bug；正運動學改用解析雅可比矩陣 |
| 1.0 | 2025-xx-xx | 初始版本 |

---

## 相關檔案

- `include/StewartIK.h` - 頭文件
- `src/StewartIK.cpp` - 實作
- `tests/test_stewart_ik.cpp` - 單元測試
- `resources/web/stewart_platform.html` - Web 版本計算器

---

## 授權

本模組為 WuSOEM 專案的一部分。
