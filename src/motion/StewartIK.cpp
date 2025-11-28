#include "../include/StewartIK.h"
#include <cmath>
#include <sstream>
#include <limits>

namespace WuSOEM {

static const long double PI = acosl(-1.0L);

std::array<std::array<long double,3>,6> StewartIK::CalculateSymmetricHexagonPoints(long double edgeA, long double edgeB, long double zHeight) {
    std::array<std::array<long double,3>,6> points;
    points[0] = {edgeA, 0.0L, zHeight};
    const long double a1 = PI / 3.0L;
    points[1] = { points[0][0] + edgeA * std::cosl(a1), points[0][1] + edgeA * std::sinl(a1), zHeight };
    const long double a2 = a1 + PI / 3.0L;
    points[2] = { points[1][0] + edgeB * std::cosl(a2), points[1][1] + edgeB * std::sinl(a2), zHeight };
    const long double a3 = a2 + PI / 3.0L;
    points[3] = { points[2][0] + edgeA * std::cosl(a3), points[2][1] + edgeA * std::sinl(a3), zHeight };
    const long double a4 = a3 + PI / 3.0L;
    points[4] = { points[3][0] + edgeB * std::cosl(a4), points[3][1] + edgeB * std::sinl(a4), zHeight };
    const long double a5 = a4 + PI / 3.0L;
    points[5] = { points[4][0] + edgeA * std::cosl(a5), points[4][1] + edgeA * std::sinl(a5), zHeight };

    // centroid
    long double cx = 0.0L, cy = 0.0L;
    for (int i=0;i<6;++i){ cx += points[i][0]; cy += points[i][1]; }
    cx /= 6.0L; cy /= 6.0L;
    for (int i=0;i<6;++i){ points[i][0] -= cx; points[i][1] -= cy; }
    return points;
}

std::array<std::array<long double,3>,3> StewartIK::GetRotationMatrix(long double roll, long double pitch, long double yaw) {
    long double cr = std::cosl(roll * PI / 180.0L);
    long double sr = std::sinl(roll * PI / 180.0L);
    long double cp = std::cosl(pitch * PI / 180.0L);
    long double sp = std::sinl(pitch * PI / 180.0L);
    long double cy = std::cosl(yaw * PI / 180.0L);
    long double sy = std::sinl(yaw * PI / 180.0L);
    std::array<std::array<long double,3>,3> m;
    m[0] = { cy*cp, cy*sp*sr - sy*cr, cy*sp*cr + sy*sr };
    m[1] = { sy*cp, sy*sp*sr + cy*cr, sy*sp*cr - cy*sr };
    m[2] = { -sp  , cp*sr,            cp*cr };
    return m;
}

static std::array<long double,3> transformVec(const std::array<long double,3>& v, const std::array<std::array<long double,3>,3>& m, const std::array<long double,3>& t) {
    std::array<long double,3> r;
    r[0] = m[0][0]*v[0] + m[0][1]*v[1] + m[0][2]*v[2] + t[0];
    r[1] = m[1][0]*v[0] + m[1][1]*v[1] + m[1][2]*v[2] + t[1];
    r[2] = m[2][0]*v[0] + m[2][1]*v[1] + m[2][2]*v[2] + t[2];
    return r;
}

long double StewartIK::NormalizeAngleDeg(long double a) {
    while (a > 180.0L) a -= 360.0L;
    while (a < -180.0L) a += 360.0L;
    return a;
}

std::array<long double,3> StewartIK::TransformPlatformToWorldDelta(
    const std::array<long double,3>& localDelta,
    const Pose6D& currentPose) {
    // 獲取平台當前姿態的旋轉矩陣
    auto rot = GetRotationMatrix(currentPose.roll, currentPose.pitch, currentPose.yaw);
    
    // 將本地座標系的位移向量旋轉到世界座標系
    // 注意：這裡只旋轉方向，不需要平移分量
    std::array<long double,3> worldDelta;
    worldDelta[0] = rot[0][0]*localDelta[0] + rot[0][1]*localDelta[1] + rot[0][2]*localDelta[2];
    worldDelta[1] = rot[1][0]*localDelta[0] + rot[1][1]*localDelta[1] + rot[1][2]*localDelta[2];
    worldDelta[2] = rot[2][0]*localDelta[0] + rot[2][1]*localDelta[1] + rot[2][2]*localDelta[2];
    
    return worldDelta;
}

std::array<long double,3> StewartIK::TransformWorldToPlatformDelta(
    const std::array<long double,3>& worldDelta,
    const Pose6D& currentPose) {
    // 獲取平台當前姿態的旋轉矩陣
    auto rot = GetRotationMatrix(currentPose.roll, currentPose.pitch, currentPose.yaw);
    
    // 使用旋轉矩陣的轉置進行逆轉換
    // 對於正交旋轉矩陣，R^(-1) = R^T
    std::array<long double,3> platformDelta;
    platformDelta[0] = rot[0][0]*worldDelta[0] + rot[1][0]*worldDelta[1] + rot[2][0]*worldDelta[2];
    platformDelta[1] = rot[0][1]*worldDelta[0] + rot[1][1]*worldDelta[1] + rot[2][1]*worldDelta[2];
    platformDelta[2] = rot[0][2]*worldDelta[0] + rot[1][2]*worldDelta[1] + rot[2][2]*worldDelta[2];
    
    return platformDelta;
}

IKResult StewartIK::CalculateInverse(const StewartParams &params, const Pose6D &pose) {
    IKResult res;
    // Cache hexagon points per thread: geometry depends only on params, not pose.
    struct CachedHexPoints {
        long double baseEdgeA = 0, baseEdgeB = 0, baseHeight = 0;
        long double platEdgeA = 0, platEdgeB = 0;
        std::array<std::array<long double,3>,6> basePoints;
        std::array<std::array<long double,3>,6> platformLocal;
    };
    static thread_local CachedHexPoints s_invCache;
    if (s_invCache.baseEdgeA != params.baseEdgeA || s_invCache.baseEdgeB != params.baseEdgeB ||
        s_invCache.baseHeight != params.baseHeight || s_invCache.platEdgeA != params.platformEdgeA ||
        s_invCache.platEdgeB != params.platformEdgeB) {
        s_invCache.basePoints = CalculateSymmetricHexagonPoints(params.baseEdgeA, params.baseEdgeB, params.baseHeight);
        s_invCache.platformLocal = CalculateSymmetricHexagonPoints(params.platformEdgeA, params.platformEdgeB, 0.0L);
        s_invCache.baseEdgeA = params.baseEdgeA;
        s_invCache.baseEdgeB = params.baseEdgeB;
        s_invCache.baseHeight = params.baseHeight;
        s_invCache.platEdgeA = params.platformEdgeA;
        s_invCache.platEdgeB = params.platformEdgeB;
    }
    const auto& basePoints = s_invCache.basePoints;
    const auto& platformLocal = s_invCache.platformLocal;

    auto rot_ld = GetRotationMatrix(pose.roll, pose.pitch, pose.yaw);
    std::array<std::array<long double,3>,3> rot = rot_ld;
    std::array<long double,3> trans{{pose.x, pose.y, pose.z}};

    // transform platform points to world
    for (int i=0;i<6;++i) {
            auto p = platformLocal[i];
            std::array<long double,3> pl{{p[0], p[1], p[2]}};
            auto wp_ld = transformVec(pl, rot, trans);
            res.platformPoints[i] = wp_ld;
    }

    std::array<std::array<long double,3>,6> armEnds;

    bool overallValid = true;

    for (int i=0;i<6;++i) {
        int pi = params.servoPointMap[i];
        const auto &bp = basePoints[pi];
        const auto &pp = res.platformPoints[pi];

        // target vector from base to platform point
        long double tx = pp[0] - bp[0];
        long double ty = pp[1] - bp[1];
        long double tz = pp[2] - bp[2];
        long double c = std::sqrtl(tx*tx + ty*ty + tz*tz);

        long double maxReach = params.servoArmLength + params.rodLength;
        long double minReach = std::fabsl(params.rodLength - params.servoArmLength);
        if (c > maxReach || c < minReach) {
            std::ostringstream ss; ss << "腿" << (i+1) << ": 目標距離 " << (double)c << " mm 超出可達範圍";
            res.errors.push_back(ss.str()); overallValid = false; res.servoAngles[i]=0.0L; armEnds[i] = bp; continue;
        }

        long double radialAngle = params.servoAxisAngle[i] * PI / 180.0L;
        long double radialDirX = std::cosl(radialAngle);
        long double radialDirY = std::sinl(radialAngle);

        long double dotRadial = tx * radialDirX + ty * radialDirY;
        long double targetAngleInPlane = std::atan2l(tz, dotRadial);

        long double a = params.servoArmLength;
        long double b = params.rodLength;
        long double cosAlpha = (a*a + c*c - b*b) / (2.0L * a * c);
        if (cosAlpha > 1.0L) cosAlpha = 1.0L; if (cosAlpha < -1.0L) cosAlpha = -1.0L;
        long double alpha = acosl(cosAlpha);

        long double servoAngleDeg = (targetAngleInPlane - alpha) * 180.0L / PI;
        servoAngleDeg = NormalizeAngleDeg(servoAngleDeg);

        // compute initial arm end point (使用初始估計)
        long double armRad = servoAngleDeg * PI / 180.0L;
        long double cosRadial = radialDirX; // reuse precomputed cos/sin of radialAngle
        long double sinRadial = radialDirY;
        long double ax = bp[0] + a * cosRadial * std::cosl(armRad);
        long double ay = bp[1] + a * sinRadial * std::cosl(armRad);
        long double az = bp[2] + a * std::sinl(armRad);

        // refine with Newton iterations on arm angle to match rod length
        // 【重要】使用更嚴格的參數來匹配 web 版本
        const int maxIter = 50;           // 增加迭代次數
        const long double tol = 1e-12L;   // 極嚴格的容差
        long double armAngle = armRad;
        for (int it=0; it<maxIter; ++it) {
            long double sinArm = std::sinl(armAngle);
            long double cosArm = std::cosl(armAngle);
            long double ex = pp[0] - (bp[0] + a * cosRadial * cosArm);
            long double ey = pp[1] - (bp[1] + a * sinRadial * cosArm);
            long double ez = pp[2] - (bp[2] + a * sinArm);
            long double curLen = std::sqrtl(ex*ex + ey*ey + ez*ez);
            long double err = curLen - b;
            if (std::fabsl(err) < tol) break;

            if (curLen < 1e-15L) break;

            // Analytic gradient: d(curLen)/d(armAngle)
            // de/dα = [a*cosRadial*sinArm, a*sinRadial*sinArm, -a*cosArm]
            long double grad = (ex * a * cosRadial * sinArm
                              + ey * a * sinRadial * sinArm
                              - ez * a * cosArm) / curLen;

            if (std::fabsl(grad) > 1e-10L) {
                armAngle -= err / grad;
            } else {
                break;
            }
        }

        {
            long double sinArm = std::sinl(armAngle);
            long double cosArm = std::cosl(armAngle);
            ax = bp[0] + a * cosRadial * cosArm;
            ay = bp[1] + a * sinRadial * cosArm;
            az = bp[2] + a * sinArm;
        }

        // 【重要修正】使用優化後的角度（這是原本的 bug！）
        servoAngleDeg = armAngle * 180.0L / PI;
        servoAngleDeg = NormalizeAngleDeg(servoAngleDeg);

        // 檢查力臂角度限制（在優化後進行）
        if (std::fabsl(servoAngleDeg) > params.servoAngleLimit) {
            std::ostringstream ss; ss << "腿" << (i+1) << ": 力臂角度 " << (double)servoAngleDeg << "° 超出 ±" << (double)params.servoAngleLimit << "° 限制";
            res.errors.push_back(ss.str()); overallValid = false;
        }

        armEnds[i] = {ax, ay, az};

        // final rod length check
        long double dx = pp[0] - ax; long double dy = pp[1] - ay; long double dz = pp[2] - az;
        long double finalLen = std::sqrtl(dx*dx + dy*dy + dz*dz);
        if (std::fabsl(finalLen - b) > 0.1L) {
            std::ostringstream ss; ss << "腿" << (i+1) << ": 連桿長度誤差 實際=" << (double)finalLen << " 設定=" << (double)b;
            res.errors.push_back(ss.str()); overallValid = false;
        }

        res.servoAngles[i] = servoAngleDeg;
    }

    res.valid = overallValid && res.errors.empty();
    res.armEndPoints = armEnds;
    return res;
}

long double StewartIK::CalculateInitialHeight(const StewartParams &params, long double servoInitialAngle) {
    const auto basePoints = CalculateSymmetricHexagonPoints(
        params.baseEdgeA,
        params.baseEdgeB,
        params.baseHeight);
    const auto platformLocal = CalculateSymmetricHexagonPoints(
        params.platformEdgeA,
        params.platformEdgeB,
        0.0L);

    const int pi = params.servoPointMap[0];
    const auto& basePoint = basePoints[pi];
    const auto& platformPoint = platformLocal[pi];
    const long double axisAngleRad = params.servoAxisAngle[0] * PI / 180.0L;
    const long double servoAngleRad = servoInitialAngle * PI / 180.0L;

    const long double armEndX =
        basePoint[0] + params.servoArmLength * std::cosl(axisAngleRad) * std::cosl(servoAngleRad);
    const long double armEndY =
        basePoint[1] + params.servoArmLength * std::sinl(axisAngleRad) * std::cosl(servoAngleRad);
    const long double armEndZ = basePoint[2] + params.servoArmLength * std::sinl(servoAngleRad);

    const long double dx = platformPoint[0] - armEndX;
    const long double dy = platformPoint[1] - armEndY;
    const long double horizontalDistanceSquared = dx * dx + dy * dy;
    const long double remainingSquared = params.rodLength * params.rodLength - horizontalDistanceSquared;
    if (remainingSquared < 0.0L) {
        return 450.0L;
    }

    return armEndZ + std::sqrtl(remainingSquared);
}

// ------------------------- Forward Kinematics -------------------------
// 計算解析雅可比矩陣的輔助函數
// ∂L_i/∂(x,y,z,roll,pitch,yaw) 其中 L_i 是第 i 支連桿長度
static void computeAnalyticJacobian(
    const Pose6D &pose,
    const std::array<std::array<long double,3>,6> &platformLocal,
    const std::array<std::array<long double,3>,6> &armEndPoints,
    const std::array<std::array<long double,3>,6> &platformWorld,
    long double J[6][6]) 
{
    const long double deg2rad = PI / 180.0L;
    
    // 角度（弧度）
    const long double roll = pose.roll * deg2rad;
    const long double pitch = pose.pitch * deg2rad;
    const long double yaw = pose.yaw * deg2rad;
    
    const long double cr = std::cosl(roll), sr = std::sinl(roll);
    const long double cp = std::cosl(pitch), sp = std::sinl(pitch);
    const long double cy = std::cosl(yaw), sy = std::sinl(yaw);
    
    for (int i = 0; i < 6; ++i) {
        const auto &p = platformLocal[i]; // 平台點（本地坐標）
        const auto &a = armEndPoints[i];  // 力臂端點
        const auto &pw = platformWorld[i]; // 平台點（世界坐標）
        
        // 差向量 d = platformWorld - armEndPoint
        const long double dx = pw[0] - a[0];
        const long double dy = pw[1] - a[1];
        const long double dz = pw[2] - a[2];
        
        // 連桿長度 L
        const long double L = std::sqrtl(dx*dx + dy*dy + dz*dz);
        if (L < 1e-12L) {
            // 避免除以零
            for (int j = 0; j < 6; ++j) J[i][j] = 0;
            continue;
        }
        
        // ∂L/∂x = dx/L, ∂L/∂y = dy/L, ∂L/∂z = dz/L
        J[i][0] = dx / L;
        J[i][1] = dy / L;
        J[i][2] = dz / L;
        
        // 計算 ∂P/∂roll, ∂P/∂pitch, ∂P/∂yaw
        // R = Rz(yaw) * Ry(pitch) * Rx(roll)
        // 需要計算 ∂R/∂roll, ∂R/∂pitch, ∂R/∂yaw
        
        const long double px = p[0], py = p[1], pz = p[2];
        
        // ∂R/∂roll * p (對 roll 的偏導)
        // R[0] = [cy*cp, cy*sp*sr - sy*cr, cy*sp*cr + sy*sr]
        // R[1] = [sy*cp, sy*sp*sr + cy*cr, sy*sp*cr - cy*sr]
        // R[2] = [-sp, cp*sr, cp*cr]
        
        // ∂R/∂roll:
        // [0, cy*sp*cr + sy*sr, -cy*sp*sr + sy*cr] 
        // [0, sy*sp*cr - cy*sr, -sy*sp*sr - cy*cr]
        // [0, cp*cr, -cp*sr]
        const long double dR_droll_x = (cy*sp*cr + sy*sr) * py + (-cy*sp*sr + sy*cr) * pz;
        const long double dR_droll_y = (sy*sp*cr - cy*sr) * py + (-sy*sp*sr - cy*cr) * pz;
        const long double dR_droll_z = cp*cr * py + (-cp*sr) * pz;
        
        // ∂R/∂pitch:
        // [-cy*sp, cy*cp*sr, cy*cp*cr]
        // [-sy*sp, sy*cp*sr, sy*cp*cr]
        // [-cp, -sp*sr, -sp*cr]
        const long double dR_dpitch_x = -cy*sp * px + cy*cp*sr * py + cy*cp*cr * pz;
        const long double dR_dpitch_y = -sy*sp * px + sy*cp*sr * py + sy*cp*cr * pz;
        const long double dR_dpitch_z = -cp * px + (-sp*sr) * py + (-sp*cr) * pz;
        
        // ∂R/∂yaw:
        // [-sy*cp, -sy*sp*sr - cy*cr, -sy*sp*cr + cy*sr]
        // [cy*cp, cy*sp*sr - sy*cr, cy*sp*cr + sy*sr]
        // [0, 0, 0]
        const long double dR_dyaw_x = -sy*cp * px + (-sy*sp*sr - cy*cr) * py + (-sy*sp*cr + cy*sr) * pz;
        const long double dR_dyaw_y = cy*cp * px + (cy*sp*sr - sy*cr) * py + (cy*sp*cr + sy*sr) * pz;
        const long double dR_dyaw_z = 0;
        
        // ∂L/∂angle = (d · ∂P/∂angle) / L * deg2rad
        J[i][3] = (dx * dR_droll_x + dy * dR_droll_y + dz * dR_droll_z) / L * deg2rad;
        J[i][4] = (dx * dR_dpitch_x + dy * dR_dpitch_y + dz * dR_dpitch_z) / L * deg2rad;
        J[i][5] = (dx * dR_dyaw_x + dy * dR_dyaw_y + dz * dR_dyaw_z) / L * deg2rad;
    }
}

// 高斯消元法求解線性方程組 A * x = b，返回 x
static bool solveLinearSystem6x6(long double JtJ[6][6], long double Jtb[6], long double delta[6]) {
    // 複製到增廣矩陣
    long double A[6][7];
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) A[r][c] = JtJ[r][c];
        A[r][6] = Jtb[r];
    }
    
    // 高斯消元法
    for (int i = 0; i < 6; ++i) {
        // 選擇主元
        int maxRow = i;
        long double maxVal = std::fabsl(A[i][i]);
        for (int k = i + 1; k < 6; ++k) {
            if (std::fabsl(A[k][i]) > maxVal) {
                maxVal = std::fabsl(A[k][i]);
                maxRow = k;
            }
        }
        if (maxVal < 1e-12L) return false; // 奇異矩陣
        
        // 交換行
        if (maxRow != i) {
            for (int c = i; c < 7; ++c) std::swap(A[i][c], A[maxRow][c]);
        }
        
        // 歸一化
        long double pivot = A[i][i];
        for (int c = i; c < 7; ++c) A[i][c] /= pivot;
        
        // 消元
        for (int r = i + 1; r < 6; ++r) {
            long double f = A[r][i];
            for (int c = i; c < 7; ++c) A[r][c] -= f * A[i][c];
        }
    }
    
    // 回代
    for (int i = 5; i >= 0; --i) {
        long double val = A[i][6];
        for (int c = i + 1; c < 6; ++c) val -= A[i][c] * delta[c];
        delta[i] = val / A[i][i];
    }
    
    return true;
}

// Calculate platform pose from given servo arm angles (degrees)
// 使用解析雅可比矩陣 + Levenberg-Marquardt 算法（與 web 版本一致）
ForwardResult StewartIK::CalculateForward(const StewartParams &params, const std::array<long double,6> &inputServoAngles, const Pose6D &initialPose) {
    ForwardResult out;
    // Cache hexagon points per thread: geometry depends only on params.
    struct CachedHexPoints {
        long double baseEdgeA = 0, baseEdgeB = 0, baseHeight = 0;
        long double platEdgeA = 0, platEdgeB = 0;
        std::array<std::array<long double,3>,6> basePoints;
        std::array<std::array<long double,3>,6> platformLocal;
    };
    static thread_local CachedHexPoints s_fwdCache;
    if (s_fwdCache.baseEdgeA != params.baseEdgeA || s_fwdCache.baseEdgeB != params.baseEdgeB ||
        s_fwdCache.baseHeight != params.baseHeight || s_fwdCache.platEdgeA != params.platformEdgeA ||
        s_fwdCache.platEdgeB != params.platformEdgeB) {
        s_fwdCache.basePoints = CalculateSymmetricHexagonPoints(params.baseEdgeA, params.baseEdgeB, params.baseHeight);
        s_fwdCache.platformLocal = CalculateSymmetricHexagonPoints(params.platformEdgeA, params.platformEdgeB, 0.0L);
        s_fwdCache.baseEdgeA = params.baseEdgeA;
        s_fwdCache.baseEdgeB = params.baseEdgeB;
        s_fwdCache.baseHeight = params.baseHeight;
        s_fwdCache.platEdgeA = params.platformEdgeA;
        s_fwdCache.platEdgeB = params.platformEdgeB;
    }
    const auto& basePoints = s_fwdCache.basePoints;
    const auto& platformLocal = s_fwdCache.platformLocal;

    // compute arm end points from servo angles (reuse precomputed cos/sin of radialAngle)
    std::array<std::array<long double,3>,6> armEndPoints;
    for (int i=0;i<6;++i) {
        int pi = params.servoPointMap[i];
        long double servoAngle = inputServoAngles[i] * PI / 180.0L;
        long double servoRadial = params.servoAxisAngle[i] * PI / 180.0L;
        long double cosRadial = std::cosl(servoRadial);
        long double sinRadial = std::sinl(servoRadial);
        long double cosServo = std::cosl(servoAngle);
        long double sinServo = std::sinl(servoAngle);
        long double ax = basePoints[pi][0] + params.servoArmLength * cosRadial * cosServo;
        long double ay = basePoints[pi][1] + params.servoArmLength * sinRadial * cosServo;
        long double az = basePoints[pi][2] + params.servoArmLength * sinServo;
        armEndPoints[i] = {ax, ay, az};
    }

    // check servo limits
    std::vector<std::string> errors;
    bool valid = true;
    for (int i=0;i<6;++i) {
        if (std::fabsl(inputServoAngles[i]) > params.servoAngleLimit) {
            std::ostringstream ss; ss << "力臂" << (i+1) << ": 角度 " << (double)inputServoAngles[i] << "° 超出 ±" << (double)params.servoAngleLimit << "° 限制";
            errors.push_back(ss.str()); valid = false;
        }
    }
    if (!valid) { out.valid = false; out.errors = errors; return out; }

    // Newton-Raphson with Levenberg-Marquardt using analytic Jacobian
    auto newtonRaphsonLM = [&](Pose6D pose, int maxIter, long double tol)->std::tuple<Pose6D,bool,long double> {
        long double lambda = 1e-8L;
        
        for (int iter = 0; iter < maxIter; ++iter) {
            // 計算旋轉矩陣
            auto rot = GetRotationMatrix(pose.roll, pose.pitch, pose.yaw);
            std::array<long double,3> trans{{pose.x, pose.y, pose.z}};
            
            // 計算當前平台點位置
            std::array<std::array<long double,3>,6> platformWorld;
            for (int i = 0; i < 6; ++i) {
                platformWorld[i] = transformVec({platformLocal[i][0], platformLocal[i][1], platformLocal[i][2]}, rot, trans);
            }
            
            // 計算誤差向量
            long double errs[6];
            long double totalError = 0;
            // 建立按伺服順序排列的平台點陣列
            std::array<std::array<long double,3>,6> platformLocalByServo;
            std::array<std::array<long double,3>,6> platformWorldByServo;
            for (int i = 0; i < 6; ++i) {
                int pi = params.servoPointMap[i];
                platformLocalByServo[i] = platformLocal[pi];
                platformWorldByServo[i] = platformWorld[pi];
            }
            for (int i = 0; i < 6; ++i) {
                long double dx = platformWorldByServo[i][0] - armEndPoints[i][0];
                long double dy = platformWorldByServo[i][1] - armEndPoints[i][1];
                long double dz = platformWorldByServo[i][2] - armEndPoints[i][2];
                long double L = std::sqrtl(dx*dx + dy*dy + dz*dz);
                errs[i] = L - params.rodLength;
                totalError += std::fabsl(errs[i]);
            }
            
            if (totalError < tol) {
                return {pose, true, totalError};
            }
            
            // 計算解析雅可比矩陣
            long double J[6][6];
            computeAnalyticJacobian(pose, platformLocalByServo, armEndPoints, platformWorldByServo, J);
            
            // LM: (J^T * J + λ * diag(J^T * J)) * delta = J^T * errs
            long double JtJ[6][6] = {};
            long double Jtb[6] = {};
            
            // J^T * J
            for (int r = 0; r < 6; ++r) {
                for (int c = 0; c < 6; ++c) {
                    for (int k = 0; k < 6; ++k) {
                        JtJ[r][c] += J[k][r] * J[k][c];
                    }
                }
            }
            
            // J^T * errs
            for (int r = 0; r < 6; ++r) {
                for (int k = 0; k < 6; ++k) {
                    Jtb[r] += J[k][r] * errs[k];
                }
            }
            
            // 添加 LM 正則化
            for (int i = 0; i < 6; ++i) {
                JtJ[i][i] *= (1 + lambda);
            }
            
            // 求解線性方程組
            long double delta[6];
            if (!solveLinearSystem6x6(JtJ, Jtb, delta)) {
                lambda *= 10;
                continue;
            }
            
            // 嘗試新姿態
            Pose6D newPose;
            newPose.x = pose.x - delta[0];
            newPose.y = pose.y - delta[1];
            newPose.z = pose.z - delta[2];
            newPose.roll = NormalizeAngleDeg(pose.roll - delta[3]);
            newPose.pitch = NormalizeAngleDeg(pose.pitch - delta[4]);
            newPose.yaw = NormalizeAngleDeg(pose.yaw - delta[5]);
            
            // 計算新誤差（須以 servo 順序對應的平台點計算，與 errs[]、Jacobian 一致）
            auto newRot = GetRotationMatrix(newPose.roll, newPose.pitch, newPose.yaw);
            std::array<long double,3> newTrans{{newPose.x, newPose.y, newPose.z}};
            
            long double newTotalError = 0;
            for (int i = 0; i < 6; ++i) {
                const auto& plocal = platformLocalByServo[i];
                auto newPw = transformVec({plocal[0], plocal[1], plocal[2]}, newRot, newTrans);
                long double dx = newPw[0] - armEndPoints[i][0];
                long double dy = newPw[1] - armEndPoints[i][1];
                long double dz = newPw[2] - armEndPoints[i][2];
                long double L = std::sqrtl(dx*dx + dy*dy + dz*dz);
                newTotalError += std::fabsl(L - params.rodLength);
            }
            
            if (newTotalError < totalError) {
                pose = newPose;
                lambda = std::fmaxl(lambda * 0.1L, 1e-12L);
            } else {
                lambda = std::fminl(lambda * 10.0L, 1e6L);
            }
        }
        
        // 計算最終誤差（同樣以 servo 順序對應的平台點計算）
        auto rot = GetRotationMatrix(pose.roll, pose.pitch, pose.yaw);
        std::array<long double,3> trans{{pose.x, pose.y, pose.z}};
        long double finalError = 0;
        for (int i = 0; i < 6; ++i) {
            int pi = params.servoPointMap[i];
            const auto& plocal = platformLocal[pi];
            auto pw = transformVec({plocal[0], plocal[1], plocal[2]}, rot, trans);
            long double dx = pw[0] - armEndPoints[i][0];
            long double dy = pw[1] - armEndPoints[i][1];
            long double dz = pw[2] - armEndPoints[i][2];
            long double L = std::sqrtl(dx*dx + dy*dy + dz*dz);
            finalError += std::fabsl(L - params.rodLength);
        }
        
        return {pose, finalError < 1e-6L, finalError};
    };

    // 減少初始猜測數量（解析雅可比更高效）
    std::vector<Pose6D> guesses;
    guesses.push_back(initialPose);
    Pose6D neutral; neutral.x=0; neutral.y=0; neutral.z=450.0L; neutral.roll=0; neutral.pitch=0; neutral.yaw=0;
    guesses.push_back(neutral);

    // 減少迭代次數（解析雅可比更高效）
    const int maxIterations = 50;
    const long double tolerance = 1e-12L;

    Pose6D bestPose = initialPose; 
    long double bestErr = std::numeric_limits<long double>::max(); 
    bool bestValid = false;
    
    for (auto &g: guesses) {
        auto [pose, ok, totalError] = newtonRaphsonLM(g, maxIterations, tolerance);
        if (totalError < bestErr) {
            bestErr = totalError;
            bestPose = pose;
            bestValid = ok;
        }
        if (bestErr < tolerance) break;
    }

    const long double acceptableTolerance = 1e-6L;
    if (bestValid && bestErr < acceptableTolerance) {
        out.pose = bestPose; 
        out.valid = true; 
        return out;
    } else {
        out.pose = bestPose; 
        out.valid = false;
        std::ostringstream ss; 
        ss << "姿態求解失敗，最小誤差: " << (double)bestErr << " mm";
        out.errors.push_back(ss.str());
        return out;
    }
}

} // namespace WuSOEM
