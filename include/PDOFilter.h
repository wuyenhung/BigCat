#pragma once
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <utility>
#include <functional>
#include <array>

class PDOFilter {
public:
    // 違規類型枚舉
    enum class ViolationType : uint8_t {
        None            = 0,
        Velocity        = 1 << 0,
        Acceleration    = 1 << 1,
        Deceleration    = 1 << 2,
        HardLimitMin    = 1 << 3,
        HardLimitMax    = 1 << 4,
        InputAnomaly    = 1 << 5,  // MMF 輸入異常（跳變）
        StaleInput      = 1 << 6,  // MMF 輸入持續未更新
        TrackingError   = 1 << 7   // 命令值與實際值偏差過大
    };

    // Helper functions for ViolationType
    static constexpr bool HasFlag(ViolationType value, ViolationType flag) {
        return (static_cast<uint8_t>(value) & static_cast<uint8_t>(flag)) != 0;
    }

    // Global bitwise operators for ViolationType (to allow flag combinations)
    friend ViolationType operator|(ViolationType a, ViolationType b) {
        return static_cast<ViolationType>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }
    friend ViolationType& operator|=(ViolationType& a, ViolationType b) {
        a = static_cast<ViolationType>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
        return a;
    }
    friend ViolationType operator&(ViolationType a, ViolationType b) {
        return static_cast<ViolationType>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }

    static constexpr double DEFAULT_ACCEL_VIOLATION_DELAY_SEC = 0.2;
    static constexpr double DEFAULT_VELOCITY_VIOLATION_DELAY_SEC = 0.5;
    static constexpr double DEFAULT_ACCEL_VIOLATION_THRESHOLD_SCALE = 1.2;
    static constexpr double DEFAULT_VELOCITY_VIOLATION_THRESHOLD_SCALE = 0.6;
    static constexpr double POST_SOFT_START_LIMIT_PREVIEW_MARGIN_DEG = 2.0;

    void SetDynamicViolationConfig(double velocityThresholdScale,
                                   double accelerationThresholdScale,
                                   double velocityDelaySec,
                                   double accelerationDelaySec) {
        SetVelocityViolationThresholdScale(velocityThresholdScale);
        SetAccelViolationThresholdScale(accelerationThresholdScale);
        SetVelocityViolationDelaySec(velocityDelaySec);
        SetAccelViolationDelaySec(accelerationDelaySec);
    }
    void SetAccelViolationThresholdScale(double scale) {
        ioAccelThresholdScale_ = (std::isfinite(scale) && scale >= 0.0) ? scale : DEFAULT_ACCEL_VIOLATION_THRESHOLD_SCALE;
    }
    double GetAccelViolationThresholdScale() const { return ioAccelThresholdScale_; }
    void SetVelocityViolationThresholdScale(double scale) {
        ioVelocityThresholdScale_ = (std::isfinite(scale) && scale >= 0.0) ? scale : DEFAULT_VELOCITY_VIOLATION_THRESHOLD_SCALE;
    }
    double GetVelocityViolationThresholdScale() const { return ioVelocityThresholdScale_; }
    void SetAccelViolationDelaySec(double delaySec) {
        ioAccelViolationDelaySec_ = (std::isfinite(delaySec) && delaySec >= 0.0) ? delaySec : DEFAULT_ACCEL_VIOLATION_DELAY_SEC;
    }
    double GetAccelViolationDelaySec() const { return ioAccelViolationDelaySec_; }
    void SetVelocityViolationDelaySec(double delaySec) {
        ioVelocityViolationDelaySec_ = (std::isfinite(delaySec) && delaySec >= 0.0) ? delaySec : DEFAULT_VELOCITY_VIOLATION_DELAY_SEC;
    }
    double GetVelocityViolationDelaySec() const { return ioVelocityViolationDelaySec_; }

    PDOFilter();
    ~PDOFilter() = default;

    // 違規狀態回調函數類型
    using ViolationCallback = std::function<void(ViolationType violationType, double rawVelocity, 
                                                  double rawAcceleration, bool limitingEnabled, 
                                                  double violationDuration)>;

    // 設置違規狀態回調
    void SetViolationCallback(ViolationCallback callback) { ioViolationCallback_ = callback; }

    // 輸入異常（跳變）檢測回調
    // originalSet: 原始輸入值 (MMF)
    // replacedSet: 取代後的預測值
    // inputChange: 原始輸入變化量
    // maxAllowedChange: 允許的最大變化量
    // reason: 異常分類（See InputAnomalyReason）
    enum class InputAnomalyReason : uint8_t {
        Unknown = 0,
        ExceededAccelLimit,
        StaleRecovery,
        PDOLimitJump,
        MeasurementMismatch
    };
    static const char* InputAnomalyReasonToString(InputAnomalyReason r) {
        switch (r) {
            case InputAnomalyReason::ExceededAccelLimit: return "ExceededAccelLimit";
            case InputAnomalyReason::StaleRecovery: return "StaleRecovery";
            case InputAnomalyReason::PDOLimitJump: return "PDOLimitJump";
            case InputAnomalyReason::MeasurementMismatch: return "MeasurementMismatch";
            default: return "Unknown";
        }
    }
    using InputAnomalyCallback = std::function<void(double originalSet, double replacedSet, double inputChange, double maxAllowedChange, InputAnomalyReason reason)>;
    void SetInputAnomalyCallback(InputAnomalyCallback callback) { ioInputAnomalyCallback_ = callback; }

    // 重置濾波器狀態
    void Reset();

    // 配置濾波器參數 (速度/加速度/週期)
    void Configure(double iMaxVelocity, double iMaxAcceleration, 
                   double iMaxDeceleration, double iJerk,
                   uint32_t iCycleTimeUs);

    // 計算並返回濾波後的目標位置
    // 第三個參數為可選的「量測速度」，在可用時可用來改善濾波器對回授速度的判斷
    double Filter(double iSetValue, double iActValue, double iMeasuredVel = std::numeric_limits<double>::quiet_NaN());
    double FilterWithInputState(double iSetValue,
                                double iActValue,
                                double iMeasuredVel,
                                bool iInputUpdateKnown,
                                bool iInputUpdated,
                                double iInputElapsedSec,
                                bool iAllowInputPrediction);

    double GetPositionGain() const { return iPositionGain_; }
    void SetPositionGain(double iGain) { iPositionGain_ = (iGain > 0.0) ? iGain : 100.0; }
    double GetPositionDeadband() const { return iPositionDeadband_; }
    void SetPositionDeadband(double db) { iPositionDeadband_ = (db >= 0.0) ? db : 0.0; }
    double GetFeedforwardGain() const { return iFeedforwardGain_; }
    void SetFeedforwardGain(double g) { iFeedforwardGain_ = (g >= 0.0 && g <= 2.0) ? g : 1.0; }

    // 獲取當前狀態 (UI/診斷介面)
    // 這些方法返回「外部可觀察」的運動量（由濾波後位置差分計算），
    // 適合用於 UI 或遠端監控；內部控制狀態請使用 GetInternal*()。
    double GetCurrentVelocity() const { return oOutputVelocity_; }
    double GetCurrentAcceleration() const { return oOutputAcceleration_; }
    // 內部控制迴路狀態（僅供調試）
    double GetInternalVelocity() const { return oCurrentVelocity_; }
    double GetInternalAcceleration() const { return oCurrentAcceleration_; }
    double GetFilteredOutput() const { return oFilteredOutput_; }
    bool IsInitialized() const { return ioInitialized_; }
    
    // 取得配置參數
    double GetMaxVelocity() const { return iMaxVelocity_; }
    double GetMaxAcceleration() const { return iMaxAcceleration_; }
    double GetMaxDeceleration() const { return iMaxDeceleration_; }

    // Get configured PDO hard limits (min, max). Returns true if set.
    std::pair<double,double> GetPDOLimits() const { return { iMinPDO_, iMaxPDO_ }; }


    // Set PDO limits for safety check
    void SetPDOLimits(double iMin, double iMax);

    // Note: input-force-raw behaviour was removed; keep API surface small.

    // 設定速度/加速度限制（立即套用）。
    // NOTE: ramp 時間的支援已移除；如需漸變行為，請實作外部節點來逐步調整限制。
    void SetLimitsWithRamp(double maxVel, double maxAccel, double maxDecel);

    // Pose6D 動態行程縮放啟動時，將加減速限制降為設定值的 0.5 倍；
    // 關閉時恢復為設定值。使用 min() 確保不覆蓋 soft-start 等更嚴格的限制。
    void ApplyDynamicScalingAccelLimits(bool active) {
        if (active) {
            if (!ioDynamicScalingAccelActive_) {
                ioDynamicScalingAccelActive_ = true;
                iMaxAcceleration_ = (std::min)(iMaxAcceleration_, iConfiguredMaxAcceleration_ * 0.5);
                iMaxDeceleration_ = (std::min)(iMaxDeceleration_, iConfiguredMaxDeceleration_ * 0.5);
            }
        } else {
            if (ioDynamicScalingAccelActive_) {
                ioDynamicScalingAccelActive_ = false;
                iMaxAcceleration_ = iConfiguredMaxAcceleration_;
                iMaxDeceleration_ = iConfiguredMaxDeceleration_;
            }
        }
    }

    // Query whether the local limiting state machine has engaged
    bool IsLimitingEnabled() const { return ioLimitingEnabled_; }

    // Allow external control for globally enabling/disabling limiting for this filter
    // When globally enabling limiting, apply the same temporary acceleration
    // reduction used by soft-start, but cap velocity at the dynamic violation
    // threshold instead of soft-start crawl speed. When disabling, restore
    // configured limits through the normal recovery ramp.
    //
    // ioGlobalLimitingForced_ tracks whether the master has externally enforced
    // limiting on this slave. While true, the per-slave recovery in Filter()
    // must NOT auto-clear ioLimitingEnabled_; otherwise the master would
    // re-apply limiting on the next cycle, producing chatter (repeated
    // "違規解除" log spam every RECOVERY_DELAY_SEC seconds for slaves that
    // never had their own violation but were force-limited by the master).
    void SetGlobalLimitingEnabled(bool enabled) {
        if (enabled) {
            ioGlobalLimitingForced_ = true;
            if (!ioLimitingEnabled_) {
                ioLimitingEnabled_ = true;
                // Only apply limiting values if NOT in soft start.
                // Soft Start has its own stricter limits (0.2 ratio) which should take precedence
                // over the standard limiting (e.g. 0.5 ratio).
                if (!ioSoftStartActive_) {
                    iMaxAcceleration_ = iConfiguredMaxAcceleration_ * SOFT_START_ACCEL_RATIO;
                    iMaxDeceleration_ = iConfiguredMaxDeceleration_ * SOFT_START_ACCEL_RATIO;
                    // Cap at the dynamic violation threshold so the axis keeps
                    // moving while returning inside the guard.
                    iMaxVelocity_ = ComputeDynamicLimitMaxVelocity();
                }
            }
        } else {
            ioGlobalLimitingForced_ = false;
            if (ioLimitingEnabled_) {
                // Start a smooth accel/decel ramp to avoid sudden step when global limiting
                // is cleared; Filter() will interpolate iMaxAcceleration_/iMaxDeceleration_
                // when ioAccelRampActive_ is true.
                ioLimitingEnabled_ = false;
                ioAccelRampActive_ = true;
                ioAccelRampStartTime_ = std::chrono::steady_clock::now();
                ioAccelRampStartAcc_ = iMaxAcceleration_;
                ioAccelRampStartDec_ = iMaxDeceleration_;
            }
        }
    }

    // 觸發 soft-start（緩啟動）
    void TriggerSoftStart();

    // soft-start readiness 判定
    bool IsSoftStartReady() const;

    // 強制結束 soft-start
    void CompleteSoftStart();
    void ResetStaleInputMonitoring();

    // Hold current position: activates hold-position mode so Filter() 
    // decelerates to zero velocity and holds position.
    // Use this to stop motion during emergency/smooth stop.
    void HoldCurrentPosition();
    // Check if filter is in hold-position mode
    bool IsHoldingPosition() const { return ioHoldingPosition_; }
    // Release hold-position mode (allow normal setpoint tracking again)
    void ReleaseHoldPosition();

    // 查詢 soft-start 狀態
    bool IsSoftStartActive() const { return ioSoftStartActive_; }
    // 是否曾經觸發過軟啟動（用於判斷是否需要 pre-soft-start 輸入鎖定）
    bool HasSoftStartEverBeenTriggered() const { return ioSoftStartEverTriggered_; }
    
    // === 違規狀態查詢 ===
    // 取得當前違規類型
    ViolationType GetCurrentViolationType() const { return ioCurrentViolationType_; }
    // 檢查是否有特定違規
    bool HasVelocityViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::Velocity); }
    bool HasAccelerationViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::Acceleration); }
    bool HasDecelerationViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::Deceleration); }
    bool HasHardLimitViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::HardLimitMin) || 
                                                HasFlag(ioCurrentViolationType_, ViolationType::HardLimitMax); }
    bool HasInputAnomalyViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::InputAnomaly); }
    bool HasStaleInputViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::StaleInput); }
    bool HasTrackingErrorViolation() const { return HasFlag(ioCurrentViolationType_, ViolationType::TrackingError); }
    // -1 / 0 / +1 = lower clamp engaged / not engaged / upper clamp engaged
    int GetTrackingClampDirection() const { return ioTrackingClampDirection_; }
    // 檢查本軸是否處於本地違規/限制狀態
    bool HasLocalViolation() const { return ioViolationActive_; }
    // 取得違規持續時間
    // 取得原始速度/加速度（最後一次計算值）
    double GetRawVelocity() const { return ioLastRawVelocity_; }
    double GetRawAcceleration() const { return ioLastRawAcceleration_; }
    // Expose configured limits and computed violation thresholds for diagnostics
    double GetConfiguredMaxVelocity() const { return iConfiguredMaxVelocity_; }
    double GetConfiguredMaxAcceleration() const { return iConfiguredMaxAcceleration_; }
    double GetConfiguredMaxDeceleration() const { return iConfiguredMaxDeceleration_; }
    double GetSoftStartMaxVelocity() const { return iConfiguredMaxVelocity_ * SOFT_START_VEL_RATIO; }

    double GetVelocityViolationThreshold() const { return iConfiguredMaxVelocity_ * ioVelocityThresholdScale_; }
    double GetAccelViolationThresholdPos() const { return iConfiguredMaxAcceleration_ * ioAccelThresholdScale_; }
    double GetAccelViolationThresholdNeg() const { return -iConfiguredMaxDeceleration_ * ioAccelThresholdScale_; }
    double GetTrackingErrorLimit() const {
        return ioSoftStartActive_ ? TRACKING_ERROR_SOFT_START_MAX_DEVIATION : TRACKING_ERROR_MAX_DEVIATION;
    }
    
private:
    // Hold-position mode flag
    bool ioHoldingPosition_;      // 是否處於保持位置模式

    // 配置參數 (輸入)
    double iMaxVelocity_;        // 最大速度 (角度/秒)
    double iMaxAcceleration_;    // 最大加速度 (角度/秒²)
    double iMaxDeceleration_;    // 最大減速度 (角度/秒²)
    double iJerk_;               // 變更限制時的最大 jerk（未必會完全使用，但保留以供 API 相容）
    double iCycleTime_;          // 循環時間 (秒)
    
    // 原始配置限制（Configure() 時設定，用於緩啟動完成後恢復）
    double iConfiguredMaxVelocity_;
    double iConfiguredMaxAcceleration_;
    double iConfiguredMaxDeceleration_;

    // 狀態變量 (輸出)
    double oFilteredOutput_;     // 上一個濾波後的輸出位置 (角度)
    double oCurrentVelocity_;    // 內部控制迴路速度 (角度/秒)
    double oCurrentAcceleration_;// 內部控制迴路加速度 (角度/秒²)
    
    // 實際輸出的速度/加速度 (從位置差分計算，用於外部顯示)
    double oOutputVelocity_;     // 實際輸出速度 = d(oFilteredOutput_)/dt
    double oOutputAcceleration_; // 實際輸出加速度 = d(oOutputVelocity_)/dt
    double ioPrevFilteredOutput_;// 上一週期的輸出位置（用於計算速度）
    double ioPrevOutputVelocity_;// 上一週期的輸出速度（用於計算加速度）
    // Output tracking used for prediction/extrapolation
    // (previously tracked post-PT2 output; PT2 filter removed)
    
    // 速度指令平滑（低通濾波）
    
    bool ioInitialized_;          // 是否已初始化
    double ioPrevActValue_;       // 上一個實際位置，用於估算回放速度
    double iMinPDO_, iMaxPDO_;    // PDO 上下限
    double ioOriginalMinPDO_, ioOriginalMaxPDO_;  // 緩啟動期間保存的原始 PDO 限制
    bool ioSoftStartLimitsAdjusted_;  // 緩啟動是否調整了限制

    // 內部輔助函數
    // 計算實際輸出速度和加速度（從位置差分）
    void UpdateOutputVelocityAndAcceleration();
    // Variant: compute output velocity/accel from an externally provided final output
    void UpdateOutputVelocityAndAcceleration(double currentFinalOutput, double prevFinalOutput);
    double ComputeDynamicLimitMaxVelocity() const;

    // 回授融合調整 (已移除)
    
    // 緩啟動 (Soft Start) 相關參數
    double ioSoftStartElapsedSec_;   // 緩啟動已經過的時間（秒）
    bool ioSoftStartActive_;         // 緩啟動是否啟用
    bool ioSoftStartEverTriggered_ = false; // 曾經觸發過軟啟動（用於 pre-soft-start 輸入鎖定）
    // 使用 steady_clock 的起始時間點，以避免主執行緒延遲影響軟啟動計時
    std::chrono::steady_clock::time_point ioSoftStartStartTime_;
    
    static constexpr double SOFT_START_ACCEL_RATIO = 0.2;  // 緩啟動時使用的加速度比例
    // 緩啟動時最大速度縮減比例 (0..1)。目前使用既有 0.2 再下修 0.7 倍，
    // 也就是軟啟動期間限制為設定最高速度的 14%。
    static constexpr double SOFT_START_VEL_RATIO = 0.14;
    static constexpr double SOFT_START_SETTLE_TIME = 0.20;  // 位置接近後需要持續的時間（秒）
    static constexpr double SOFT_START_POSITION_TOLERANCE = 0.08; // 剎車死區閥值（濾波器何時停止移動）
    static constexpr double SOFT_START_READY_TOLERANCE = 0.15;  // Tightened from 0.5° so the asymptotic settle approach can converge closer to the target before CompleteSoftStart(); prevents the post-soft-start velocity jump
    // 在接近目標時提前減速的因子 (0..1)。已移除可動態設定API，使用內部預設值。
    double ioPositionCloseStartTime_;  // 位置開始接近的時間戳（秒）
    bool ioPositionWasClose_;          // 上一週期位置是否接近
    bool ioSoftStartReady_;            // 本軸已經在 soft-start tolerance 內並持續達到 settle time
    // When soft-start completes, blending is used for a short time to avoid
    // a sudden switch of velocity source which can create a large velDiff
    // and cause oscillation. Blend duration is configurable.
    static constexpr double SOFT_START_VEL_BLEND_TIME = 0.05; // seconds
    std::chrono::steady_clock::time_point ioSoftStartCompleteTime_; // time of soft-start completion
    bool ioVelocityBlendActive_; // whether blending is currently active
    // After soft-start completes, gradually restore configured accel/decel
    // over a short transition to avoid abrupt changes. Duration default: 3s
    bool ioAccelRampActive_;
    std::chrono::steady_clock::time_point ioAccelRampStartTime_;
    double ioAccelRampDurationSec_;
    double ioAccelRampStartAcc_;   // starting accel at beginning of ramp
    double ioAccelRampStartDec_;   // starting decel at beginning of ramp
    double ioAccelRampStartVel_;   // starting velocity at beginning of ramp
    double iPositionGain_;             // P 增益 (Kp)
    double iPositionDeadband_;         // position error deadband
    double iFeedforwardGain_;          // 前饋增益 (0..2, 預設 0.7) (if abs(distance) < deadband -> no correction)

    // Online trajectory/profile support
    double profileTargetPos_; // 當前正在追蹤的目標位置
    bool profileActive_;      // profile 是否已建立
    // Trapezoid profile internal state
    std::chrono::steady_clock::time_point profileStartTime_;
    double profileStartPos_;
    double profilePeakVel_;
    double profileTacc_;
    double profileTflat_;
    double profileTdec_;
    double profileDuration_;
    int profileDir_; // +1 or -1

    // Programmatic profile start (previously exposed via StartTrapezoidProfile)
    // was removed to reduce public API surface; profile activation is handled
    // internally (e.g., during hold/soft-start behaviors).

    // 延遲限制機制 (Delayed Limiting)
    // 正常時輸入等於輸出，只有持續違規才啟用限制
    static constexpr double STALE_VIOLATION_DELAY_SEC = 1.0;   // MMF 輸入持續未更新超過此時間視為違規
    static constexpr double INPUT_ANOMALY_VIOLATION_DELAY_SEC = 0.5; // MMF 輸入異常持續超過此時間視為違規
    static constexpr double RECOVERY_DELAY_SEC = 3;   // 無違規持續的時間（秒）後自動恢復
    double ioAccelViolationDelaySec_;
    double ioVelocityViolationDelaySec_;
    double ioViolationStartTime_;    // 違規開始時間（秒）
    double ioAccelViolationStartTime_;   // 加速度/減速度違規開始時間（秒）
    double ioVelocityViolationStartTime_; // 速度違規開始時間（秒）
    double ioStaleViolationStartTime_;   // Stale 輸入違規開始時間（秒）
    double ioAnomalyViolationStartTime_; // 輸入異常違規開始時間（秒）
    double ioNoViolationStartTime_;  // 無違規開始時間（秒）
    double ioViolationClearStartTime_; // 違規解除計時開始時間（秒）
    bool ioViolationActive_;         // 當前是否處於違規狀態（本軸）
    bool ioLimitingEnabled_;         // 是否啟用限制（本軸，持續違規後為 true）
    bool ioGlobalLimitingForced_ = false; // 主站強制全域限制旗標：為 true 時 Filter() 不可自動解除 ioLimitingEnabled_
    bool ioDynamicScalingAccelActive_ = false; // Pose6D 動態縮放已啟動加減速限制 (0.5x)
    // Direction in which the tracking-error window clamp is currently engaged
    // on the final filter output: +1 = pinned at iActValue+window (output above
    // actual), -1 = pinned at iActValue-window (output below actual), 0 = not
    // engaged. Updated each Filter() cycle. Used by ComputeCsvVelocityCommand
    // to inhibit the closed-loop position correction ONLY while the safety
    // boundary is engaged AND the correction would push actual further outward,
    // so that recovery (input direction reverses) can resume normal tracking.
    int ioTrackingClampDirection_ = 0;
    std::chrono::steady_clock::time_point ioFilterStartTime_;  // Filter 啟動的時間點
    bool ioFilterStartTimeInitialized_;  // 時間點是否已初始化
    
    // === 違規狀態追蹤 ===
    ViolationType ioCurrentViolationType_;  // 當前違規類型
    ViolationType ioPrevViolationType_;     // 上一週期違規類型（用於偵測類型變更）
    double ioLastRawVelocity_;              // 最後一次計算的原始速度
    double ioLastRawAcceleration_;          // 最後一次計算的原始加速度
    bool ioMeasuredVelocityProvided_;       // 上一週期是否有外部量測速度提供
    // Per-instance tolerances (fractional, e.g., 0.1 == 10%)
    double ioAccelThresholdScale_;          // 加速度違規門檻縮放 (1.0 == 100%)
    double ioVelocityThresholdScale_;       // 速度違規門檻縮放 (1.0 == 100%)
    // ioPrevInputVelocity_ removed: raw input velocity is retained in
    // ioLastRawVelocity_ and historical smoothing buffers; this member was
    // unused and has been removed to avoid confusion.
    bool ioPrevLimitingEnabled_;            // 上一週期限制是否啟用（用於檢測解除狀態變化）
    int ioAccelCheckSkipCount_;             // 跳過前幾個週期的加速度檢測（需要歷史數據）
    ViolationCallback ioViolationCallback_; // 違規狀態回調函數
    bool ioViolationDetectionSuspended_;    // 當持續違規時暫停新的違規偵測，直到違規解除

    // === 輸入異常（跳變）狀態 ===
    InputAnomalyCallback ioInputAnomalyCallback_; // 異常跳變檢測回調
    bool ioInputAnomalyActive_;                   // 異常狀態是否仍持續（避免重複通知）
    InputAnomalyReason ioLastInputAnomalyReason_;
    
    // === 移動平均線 (Moving Average) 用於平滑速度計算 ===
    // Set to 16 (approx 16ms @ 1kHz) to match typical 60Hz MMF update rates.
    // This ensures the "Staircase" input is averaged into a correct constant velocity
    // for prediction, avoiding aliasing/bursts from smaller windows.
    // Reduce the window to improve acceleration sensitivity for short bursts
    static constexpr size_t VELOCITY_MA_SIZE = 16;  
    std::array<double, VELOCITY_MA_SIZE> ioVelocityBuffer_;  // 速度緩衝區
    size_t ioVelocityBufferIndex_;           // 緩衝區當前索引
    size_t ioVelocityBufferCount_;           // 緩衝區已填充數量
    double ioPrevSmoothedVelocity_;          // 上一週期的平滑速度（用於計算加速度）
    
    // === 外推預測用速度歷史緩衝區 ===
    // 儲存最近 N 次的平滑速度，用於計算多點加權平均以改善外推預測準確度
    static constexpr size_t EXTRAPOLATION_HISTORY_SIZE = 5;  // 使用最近 5 次速度
    std::array<double, EXTRAPOLATION_HISTORY_SIZE> ioExtrapolationVelocityBuffer_;  // 外推速度歷史
    size_t ioExtrapolationVelocityIndex_;    // 外推速度緩衝區索引
    size_t ioExtrapolationVelocityCount_;    // 外推速度緩衝區已填充數量
    
    // 計算外推速度（多點加權平均）
    double ComputeExtrapolationVelocity() const;
    // 命令值與實際值允許的最大偏差（機械單位）。
    double ComputeTrackingErrorWindow() const;
    
    // === 輸入移動平均線 (Input Moving Average) 用於平滑輸入訊號 ===
    static constexpr size_t INPUT_MA_SIZE = 4;  // 輸入移動平均線窗口大小（降低以提高高頻響應）
    std::array<double, INPUT_MA_SIZE> ioInputBuffer_;  // 輸入緩衝區
    size_t ioInputBufferIndex_;              // 緩衝區當前索引
    size_t ioInputBufferCount_;              // 緩衝區已填充數量
    // Filter() 接收的 set/act 都是機械單位。
    // 正常運轉預設使用 15 度偏差上限，soft-start 期間收緊到 5 度。
    static constexpr double TRACKING_ERROR_SOFT_START_MAX_DEVIATION = 5.0;
    static constexpr double TRACKING_ERROR_MAX_DEVIATION = 15.0;
    
    
    // PT2 filter removed: previous implementation used a 2nd-order lowpass
    // (TwinCAT P-Tn). State and methods have been removed.

    // 計算線上目標速度/加速度，支援 feedforward 參數
    std::pair<double,double> ComputeOnlineTargetVelAccel(double targetPos, double feedforwardVel);

    // Previous set value for feedforward calculation
    double ioPrevSetValue_;
    double ioPrevEffectiveSetpoint_;
    double ioSoftStartCommandVelocity_;
    // Prediction support: when MMF input is static, filter can predict next setpoint
    double ioLastPredictedSetpoint_; // last predicted setpoint (mechanical units)
    bool ioPredictedThisCycle_;      // flag: prediction used in most recent Filter() call
    bool ioPreviousCyclePredicted_;  // true when the previous Filter() call used prediction
    // Debug/log counter to throttle diagnostic output
    int ioDebugLogCounter_;
    int ioStaleCounter_; // Tracks consecutive stale input cycles
    bool ioStaleViolationLatched_; // stale condition occurred; wait for next MMF change to re-soft-start

public:
    // Query whether Filter() used a predicted setpoint in the last call
    bool WasPredictionUsed() const { return ioPredictedThisCycle_; }
    // Get the last predicted setpoint (valid only if WasPredictionUsed() == true)
    double GetLastPredictedSetpoint() const { return ioLastPredictedSetpoint_; }
};
