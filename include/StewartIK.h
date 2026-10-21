#pragma once

#include <array>
#include <vector>
#include <string>

namespace WuSOEM {

struct StewartParams {
    long double platformEdgeA = 620.0L;
    long double platformEdgeB = 100.0L;
    long double baseEdgeA = 620.0L;
    long double baseEdgeB = 100.0L;
    long double rodLength = 490.0L;
    long double servoArmLength = 300.0L;
    long double servoAngleLimit = 75.0L; // degrees
    long double fisheyeAngleLimit = 30.0L; // degrees
    long double baseHeight = 0.0L;
    std::array<long double,6> servoAxisAngle{{120,300,0,180,240,60}};
    std::array<int,6> servoPointMap{{2,1,0,5,4,3}}; // hexagon point index per servo
    long double servoInitialAngle = 10.0L; // degrees - home position servo angle
};

struct Pose6D {
    long double x = 0.0L;
    long double y = 0.0L;
    long double z = 450.0L;
    long double roll = 0.0L;   // degrees
    long double pitch = 0.0L;  // degrees
    long double yaw = 0.0L;    // degrees
};

struct IKResult {
    std::array<long double,6> servoAngles; // degrees
    std::array<std::array<long double,3>,6> armEndPoints; // world coords
    std::array<std::array<long double,3>,6> platformPoints; // world coords
    bool valid = true;
    std::vector<std::string> errors;
};

struct ForwardResult {
    Pose6D pose;
    bool valid = false;
    std::vector<std::string> errors;
};

// ========================================================================
// Pose6D 動態行程縮放 — 全區常數（避免多處重複定義）
// 修改此處即可同步所有預設值；runtime 值仍由 Par/machine.xml 覆寫。
// ========================================================================
namespace Pose6DDynamicDefaults {
    constexpr long double kStartRatio = 0.50L;   // Z 達此比例開始縮放（50%）
    constexpr long double kMinScale   = 0.50L;   // Z=100% 時其他軸範圍縮至 50%
}

// ========================================================================
// Pose6D 前置驗證：在 IK 計算前檢查姿態是否超出硬體/安全限制
// ========================================================================
struct Pose6DLimits {
    // 最大姿態限制（世界座標系，相對於初始高度）
    long double maxX = 150.0L;      // 左右移動 X (mm)
    long double maxY = 150.0L;      // 前後移動 Y (mm)
    long double maxZ = 150.0L;      // 上下移動 Z (mm)
    long double maxRoll = 25.0L;    // 前後傾斜 Roll (°)
    long double maxPitch = 25.0L;   // 左右傾斜 Pitch (°)
    long double maxYaw = 10.0L;     // 旋轉 Yaw (°)
    // 魚眼軸承最大角度限制 (°)
    long double fisheyeAngleLimit = 35.0L;
    // 力臂擺動範圍 (°)
    long double servoAngleLimit = 65.0L;
    // 奇點門檻
    long double singularityVerticalAngle = 18.0L;     // 垂直角度門檻 (°)
    long double singularityHorizontalAngle = 87.0L;   // 水平角度門檻 (°)
    long double singularityExtendRatio = 95.0L;       // 伸展比例門檻 (%)
    long double singularityContractRatio = 105.0L;    // 收縮比例門檻 (%)
    // 動態行程縮放：Z 達極限位置時縮小 X/Y/Roll/Pitch/Yaw 範圍
    // 預設值來自 Pose6DDynamicDefaults，runtime 可透過 Par/machine.xml 覆寫
    long double dynamicLimitStartRatio = Pose6DDynamicDefaults::kStartRatio;
    long double dynamicLimitMinScale   = Pose6DDynamicDefaults::kMinScale;
};

enum class Pose6DViolationKind {
    None,
    X, Y, Z,
    Roll, Pitch, Yaw,
};

struct Pose6DValidationResult {
    bool valid = true;
    Pose6DViolationKind violationKind = Pose6DViolationKind::None;
    long double limitValue = 0.0L;     // 被違規的限制值
    long double actualValue = 0.0L;    // 實際值
    std::string message;               // 人類可讀的錯誤訊息 (UTF-8)
    std::wstring messageW;             // 人類可讀的錯誤訊息 (UTF-16, 用於 UI 日誌)
    bool dynamicScalingActive = false; // Z 位置觸發的動態行程縮放是否啟用中
    Pose6D clampedPose;                // 動態縮放後的飽和姿態（供 IK 使用，無縮放時等同輸入）
};

class StewartIK {
public:
    // 對稱六邊形頂點（A-B-A-B-A-B），回傳 6 個三維點
    static std::array<std::array<long double,3>,6> CalculateSymmetricHexagonPoints(long double edgeA, long double edgeB, long double zHeight);

    // 取得 3x3 旋轉矩陣 (row-major) from roll/pitch/yaw (degrees)
    static std::array<std::array<long double,3>,3> GetRotationMatrix(long double roll, long double pitch, long double yaw);

    // 逆運動學主函式：給定機構參數與 6DoF 姿態，回傳每條腿的力臂角度與其他資訊
    static IKResult CalculateInverse(const StewartParams &params, const Pose6D &pose);

    // 根據力臂初始角度推導 home 姿態對應的初始高度 (Z)
    static long double CalculateInitialHeight(const StewartParams &params, long double servoInitialAngle);

    // 正運動學：給定力臂角度 (degrees) 計算平台姿態 (Pose6D)
    static ForwardResult CalculateForward(const StewartParams &params, const std::array<long double,6> &inputServoAngles, const Pose6D &initialPose);

    // 座標系轉換：將平台本地座標系的 XYZ 移動量轉換到世界座標系
    // localDelta: 在平台本地座標系下的位移 {dx, dy, dz}
    // currentPose: 平台當前姿態（用於建立旋轉矩陣）
    // 返回: 世界座標系下的位移向量 {dx_world, dy_world, dz_world}
    static std::array<long double,3> TransformPlatformToWorldDelta(
        const std::array<long double,3>& localDelta,
        const Pose6D& currentPose);

    // 座標系轉換（逆轉換）：將世界座標系的 XYZ 移動量轉換到平台本地座標系
    // worldDelta: 在世界座標系下的位移 {dx, dy, dz}
    // currentPose: 平台當前姿態（用於建立旋轉矩陣）
    // 返回: 平台本地座標系下的位移向量 {dx_platform, dy_platform, dz_platform}
    static std::array<long double,3> TransformWorldToPlatformDelta(
        const std::array<long double,3>& worldDelta,
        const Pose6D& currentPose);

    // 工具：角度正規化到 [-180,180]
    static long double NormalizeAngleDeg(long double a);

    // ========================================================================
    // Pose6D 前置驗證層：在 IK 計算前檢查姿態是否在安全限制範圍內
    // ========================================================================
    // pose: 世界座標系姿態，z 為絕對 Z 值
    // initialHeight: 初始高度 (home position Z)，用於計算相對位移
    // limits: 姿態限制設定
    // 返回: 驗證結果，valid==false 表示超出限制
    static Pose6DValidationResult ValidatePose6D(
        const Pose6D& pose,
        long double initialHeight,
        const Pose6DLimits& limits);
};

} // namespace WuSOEM
