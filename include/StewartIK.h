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
};

} // namespace WuSOEM
