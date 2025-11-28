#include "StewartIK.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>

// Diagnostic: compare 3 candidate servoAxisAngle sets against the actual
// hexagonal geometry and show each one's yaw vs roll servo-angle patterns.
// A correct set should produce OPPOSITE sign within each physical pair for
// yaw (Z rotation), and clearly different shapes for yaw vs roll.

namespace {

struct AxisAngleSet {
    const char* name;
    std::array<long double, 6> angles;
    bool swapGeometry = false;
    std::array<int, 6> pointMap{{0,1,2,3,4,5}};
};

// Returns true if every physical pair has opposite signs for yaw mode.
bool CheckYawPairOpposition(
    const std::array<long double, 6>& yawPattern,
    const std::array<std::pair<int,int>, 3>& pairs) {
    bool ok = true;
    for (const auto& [a, b] : pairs) {
        if (yawPattern[a] * yawPattern[b] >= 0) {
            std::cerr << "  FAIL: pair (" << a << "," << b
                      << ") same sign: " << (double)yawPattern[a]
                      << " vs " << (double)yawPattern[b] << '\n';
            ok = false;
        }
    }
    return ok;
}

}  // namespace

int main() {
    // --- Geometry from Par/machine.xml ---
    WuSOEM::StewartParams params;
    params.baseEdgeA = 620.0L;
    params.baseEdgeB = 100.0L;
    params.platformEdgeA = 620.0L;
    params.platformEdgeB = 100.0L;
    params.rodLength = 490.0L;
    params.servoArmLength = 300.0L;
    params.servoInitialAngle = 10.0L;

    // --- Compute base point centered positions ---
    auto basePoints = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(
        params.baseEdgeA, params.baseEdgeB, 0.0L);

    std::cout << "=== Base point angular positions (from center) ===\n";
    for (int i = 0; i < 6; ++i) {
        long double angle = std::atan2l(basePoints[i][1], basePoints[i][0]) * 180.0L / acosl(-1.0L);
        if (angle < 0) angle += 360.0L;
        std::cout << "  point[" << i << "]: ("
                  << std::fixed << std::setprecision(1)
                  << (double)basePoints[i][0] << ", "
                  << (double)basePoints[i][1] << ")  angle="
                  << (double)angle << " deg\n";
    }

    // Physical pairs: edges of length B connect (1,2), (3,4), (5,0)
    // (A-B-A-B-A-B pattern: 0->1=A, 1->2=B, 2->3=A, 3->4=B, 4->5=A, 5->0=B)
    std::array<std::pair<int,int>, 3> pairs{{{5, 0}, {1, 2}, {3, 4}}};
    std::cout << "\nPhysical B-edge pairs: (5,0) (1,2) (3,4)\n";

    // --- Candidate servoAxisAngle sets ---
    std::array<AxisAngleSet, 7> candidates{{
        {"StewartIK default  {0,300,120,60,240,180}",
         {0, 300, 120, 60, 240, 180}, false, {0,1,2,3,4,5}},
        {"machine.xml old    {120,300,0,180,240,60}",
         {120, 300, 0, 180, 240, 60}, false, {0,1,2,3,4,5}},
        {"questioned         {330,30,90,150,210,270}",
         {330, 30, 90, 150, 210, 270}, false, {0,1,2,3,4,5}},
        {"SWAP A/B + old axes {120,300,0,180,240,60}",
         {120, 300, 0, 180, 240, 60}, true, {0,1,2,3,4,5}},
        {"SWAP A/B + default  {0,300,120,60,240,180}",
         {0, 300, 120, 60, 240, 180}, true, {0,1,2,3,4,5}},
        {"POINTMAP-A old axes + map{2,1,0,5,4,3}",
         {120, 300, 0, 180, 240, 60}, false, {2,1,0,5,4,3}},
        {"POINTMAP-B old axes + map{1,2,5,0,3,4}",
         {120, 300, 0, 180, 240, 60}, false, {1,2,5,0,3,4}},
    }};

    // Small perturbation angles (degrees)
    const long double dAngle = 1.0L;
    const long double height = WuSOEM::StewartIK::CalculateInitialHeight(params, params.servoInitialAngle);
    std::cout << "\nInitial height = " << (double)height << " mm\n";

    int failCount = 0;

    for (size_t ci = 0; ci < candidates.size(); ++ci) {
        const auto& candidate = candidates[ci];

        // For candidates with swapGeometry, swap EdgeA/EdgeB to shift B-edge pairing
        // from (5,0)(1,2)(3,4) to (0,1)(2,3)(4,5) matching physical motor pairs.
        if (candidate.swapGeometry) {
            params.baseEdgeA = 100.0L;
            params.baseEdgeB = 620.0L;
            params.platformEdgeA = 100.0L;
            params.platformEdgeB = 620.0L;
        } else {
            params.baseEdgeA = 620.0L;
            params.baseEdgeB = 100.0L;
            params.platformEdgeA = 620.0L;
            params.platformEdgeB = 100.0L;
        }

        // B-edge pairs depend on geometry
        std::array<std::pair<int,int>, 3> activePairs;
        if (candidate.swapGeometry) {
            // Swapped: A-edge=100 (short first), B-edge=620
            // Short edges: 0->1, 2->3, 4->5
            activePairs = {{{0, 1}, {2, 3}, {4, 5}}};
        } else {
            activePairs = pairs;
        }

        params.servoAxisAngle = candidate.angles;
        params.servoPointMap = candidate.pointMap;

        // Show geometry info for swapped configurations
        if (candidate.swapGeometry) {
            auto curBase = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(
                params.baseEdgeA, params.baseEdgeB, 0.0L);
            std::cout << "\n=== " << candidate.name << " ===\n";
            std::cout << "(Geometry: EdgeA=100 EdgeB=620 - swapped)\n";
            std::cout << "A-edge pairs (short=100): (0,1) (2,3) (4,5)\n";
            std::cout << "Base points:\n";
            for (int i = 0; i < 6; ++i) {
                long double angle = std::atan2l(curBase[i][1], curBase[i][0]) * 180.0L / acosl(-1.0L);
                if (angle < 0) angle += 360.0L;
                std::cout << "  point[" << i << "]: ("
                          << std::fixed << std::setprecision(1)
                          << (double)curBase[i][0] << ", "
                          << (double)curBase[i][1] << ")  angle="
                          << (double)angle << " deg\n";
            }
        } else {
            std::cout << "\n=== " << candidate.name << " ===\n";
            // Show point map if non-identity
            if (candidate.pointMap != std::array<int,6>{{0,1,2,3,4,5}}) {
                std::cout << "PointMap: servo[i] uses hexPoint[map[i]]\n";
                auto curBase = WuSOEM::StewartIK::CalculateSymmetricHexagonPoints(
                    params.baseEdgeA, params.baseEdgeB, 0.0L);
                for (int i = 0; i < 6; ++i) {
                    int pi = candidate.pointMap[i];
                    long double angle = std::atan2l(curBase[pi][1], curBase[pi][0]) * 180.0L / acosl(-1.0L);
                    if (angle < 0) angle += 360.0L;
                    std::cout << "  servo[" << i << "] axis=" << (double)candidate.angles[i]
                              << " -> point[" << pi << "] at "
                              << std::fixed << std::setprecision(1)
                              << (double)angle << " deg  offset="
                              << (double)(candidate.angles[i] - angle) << "\n";
                }
            }
        }

        // Home pose
        WuSOEM::Pose6D home;
        home.z = WuSOEM::StewartIK::CalculateInitialHeight(params, params.servoInitialAngle);

        auto ikHome = WuSOEM::StewartIK::CalculateInverse(params, home);
        if (!ikHome.valid) {
            std::cerr << "Home IK invalid for " << candidate.name << '\n';
            failCount++;
            continue;
        }

        // +1 deg Yaw
        WuSOEM::Pose6D yawPose = home;
        yawPose.yaw = dAngle;
        auto ikYaw = WuSOEM::StewartIK::CalculateInverse(params, yawPose);

        // +1 deg Roll
        WuSOEM::Pose6D rollPose = home;
        rollPose.roll = dAngle;
        auto ikRoll = WuSOEM::StewartIK::CalculateInverse(params, rollPose);

        // +1 deg Pitch
        WuSOEM::Pose6D pitchPose = home;
        pitchPose.pitch = dAngle;
        auto ikPitch = WuSOEM::StewartIK::CalculateInverse(params, pitchPose);

        // Compute deltas from home
        std::array<long double, 6> dYaw{}, dRoll{}, dPitch{};
        for (int i = 0; i < 6; ++i) {
            dYaw[i] = ikYaw.servoAngles[i] - ikHome.servoAngles[i];
            dRoll[i] = ikRoll.servoAngles[i] - ikHome.servoAngles[i];
            dPitch[i] = ikPitch.servoAngles[i] - ikHome.servoAngles[i];
        }

        // +1 mm Sway (X translation)
        WuSOEM::Pose6D swayPose = home;
        swayPose.x = 1.0L;
        auto ikSway = WuSOEM::StewartIK::CalculateInverse(params, swayPose);

        // +1 mm Surge (Y translation)
        WuSOEM::Pose6D surgePose = home;
        surgePose.y = 1.0L;
        auto ikSurge = WuSOEM::StewartIK::CalculateInverse(params, surgePose);

        std::array<long double, 6> dSway{}, dSurge{};
        for (int i = 0; i < 6; ++i) {
            dSway[i] = ikSway.servoAngles[i] - ikHome.servoAngles[i];
            dSurge[i] = ikSurge.servoAngles[i] - ikHome.servoAngles[i];
        }

        std::cout << "\n=== " << candidate.name << " ===\n";
        std::cout << "Home servo angles: ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::fixed << std::setprecision(4)
                      << (double)ikHome.servoAngles[i] << " ";
        std::cout << '\n';

        std::cout << "Yaw  +1 deg delta: ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::showpos << std::fixed << std::setprecision(4)
                      << (double)dYaw[i] << " ";
        std::cout << std::noshowpos << '\n';

        std::cout << "Roll +1 deg delta: ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::showpos << std::fixed << std::setprecision(4)
                      << (double)dRoll[i] << " ";
        std::cout << std::noshowpos << '\n';

        std::cout << "Pitch+1 deg delta: ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::showpos << std::fixed << std::setprecision(4)
                      << (double)dPitch[i] << " ";
        std::cout << std::noshowpos << '\n';

        std::cout << "Sway +1mm  delta:  ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::showpos << std::fixed << std::setprecision(4)
                      << (double)dSway[i] << " ";
        std::cout << std::noshowpos << '\n';

        std::cout << "Surge+1mm  delta:  ";
        for (int i = 0; i < 6; ++i)
            std::cout << std::showpos << std::fixed << std::setprecision(4)
                      << (double)dSurge[i] << " ";
        std::cout << std::noshowpos << '\n';

        // Check: within each pair, yaw should produce OPPOSITE sign
        std::cout << "Yaw pair opposition check (B-edge pairs):\n";
        bool yawOk = CheckYawPairOpposition(dYaw, activePairs);

        // Check: sway should NOT look like roll (low cosine similarity)
        long double dotSR = 0, normS = 0, normRl = 0;
        for (int i = 0; i < 6; ++i) {
            dotSR += dSway[i] * dRoll[i];
            normS += dSway[i] * dSway[i];
            normRl += dRoll[i] * dRoll[i];
        }
        long double cosSwayRoll = (normS > 0 && normRl > 0)
            ? dotSR / std::sqrtl(normS * normRl) : 0;
        std::cout << "Cosine similarity sway vs roll: "
                  << std::fixed << std::setprecision(4) << (double)cosSwayRoll
                  << "  (want ~0, high = sway becomes roll)\n";

        // Check: surge should NOT look like pitch
        long double dotSP = 0, normSu = 0, normPi = 0;
        for (int i = 0; i < 6; ++i) {
            dotSP += dSurge[i] * dPitch[i];
            normSu += dSurge[i] * dSurge[i];
            normPi += dPitch[i] * dPitch[i];
        }
        long double cosSurgePitch = (normSu > 0 && normPi > 0)
            ? dotSP / std::sqrtl(normSu * normPi) : 0;
        std::cout << "Cosine similarity surge vs pitch: "
                  << std::fixed << std::setprecision(4) << (double)cosSurgePitch
                  << "  (want ~0, high = surge becomes pitch)\n";
        if (yawOk) {
            std::cout << "  PASS: all pairs have opposite sign for yaw\n";
        } else {
            std::cerr << "  >>> Yaw pair opposition FAILED - "
                         "yaw mode will alias into translation/roll!\n";
            failCount++;
        }

        // Compute cosine similarity between yaw and roll patterns
        long double dotYR = 0, normY = 0, normR = 0;
        for (int i = 0; i < 6; ++i) {
            dotYR += dYaw[i] * dRoll[i];
            normY += dYaw[i] * dYaw[i];
            normR += dRoll[i] * dRoll[i];
        }
        long double cosSim = (normY > 0 && normR > 0)
            ? dotYR / std::sqrtl(normY * normR) : 0;
        std::cout << "Cosine similarity yaw vs roll: "
                  << std::fixed << std::setprecision(4) << (double)cosSim
                  << "  (0=orthogonal, +1=identical, -1=inverse)\n";

        // Check: yaw should NOT look like sway (low cosine similarity)
        long double dotYSw = 0;
        for (int i = 0; i < 6; ++i) dotYSw += dYaw[i] * dSway[i];
        long double cosYawSway = (normY > 0 && normS > 0)
            ? dotYSw / std::sqrtl(normY * normS) : 0;
        std::cout << "Cosine similarity yaw vs sway: "
                  << std::fixed << std::setprecision(4) << (double)cosYawSway
                  << "  (want ~0, |high| = yaw becomes sway)\n";

        // Check: yaw should NOT look like surge
        long double dotYSu = 0;
        for (int i = 0; i < 6; ++i) dotYSu += dYaw[i] * dSurge[i];
        long double cosYawSurge = (normY > 0 && normSu > 0)
            ? dotYSu / std::sqrtl(normY * normSu) : 0;
        std::cout << "Cosine similarity yaw vs surge: "
                  << std::fixed << std::setprecision(4) << (double)cosYawSurge
                  << "  (want ~0, |high| = yaw becomes surge)\n";

        // Summary verdict
        bool swayOk = std::fabsl(cosSwayRoll) < 0.5L;
        bool surgeOk = std::fabsl(cosSurgePitch) < 0.5L;
        bool yawSwayOk = std::fabsl(cosYawSway) < 0.3L;
        bool yawSurgeOk = std::fabsl(cosYawSurge) < 0.3L;
        std::cout << "VERDICT: yaw=" << (yawOk ? "OK" : "FAIL")
                  << " sway=" << (swayOk ? "OK" : "FAIL(aliased to roll)")
                  << " surge=" << (surgeOk ? "OK" : "FAIL(aliased to pitch)")
                  << " yaw-sway=" << (yawSwayOk ? "OK" : "FAIL(yaw becomes sway)")
                  << " yaw-surge=" << (yawSurgeOk ? "OK" : "FAIL(yaw becomes surge)")
                  << '\n';
        if (!yawOk || !swayOk || !surgeOk || !yawSwayOk || !yawSurgeOk) failCount++;
    }

    if (failCount > 0) {
        std::cout << "\n>>> " << failCount
                  << " candidate(s) failed yaw pair opposition.\n";
    }

    return (failCount > 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
