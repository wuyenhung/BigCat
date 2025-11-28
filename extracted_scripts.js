

        // Stewart平台計�??��?JavaScript實現
        class StewartPlatform {
            constructor() {
                this.params = {
                    // ?��?形尺寸�???- ?�大�?工�?空�?設�?
                    platformEdgeA: 620,   // 平台?��? (mm)
                    platformEdgeB: 100,   // 平台?��? (mm)
                    baseEdgeA: 620,       // ?�座?��? (mm)
                    baseEdgeB: 100,       // ?�座?��? (mm)
                    
                    // ?��??�數
                    rodLength: 490,       // ??��桿長�?
                    servoArmLength: 300,  // ?��??�度
                    servoInitialAngle: 10,// ?�新增�??��??��?角度 (?�設10°)
                    baseHeight: 0,
                    servoAxisAngle: [120, 300, 0, 180, 240, 60], // ?��?安�?角度，�??�中設�?
                    servoPointMap: [2, 1, 0, 5, 4, 3], // ?��?形�?點索引�??�表
                    servoAngleLimit: 75   // ?��?角度?�制以�??��??��?�?
                    ,
                    // 魚眼軸承?�大�?度�???(?��?軸承?��?)
                    fisheyeAngleLimit: 30, // 魚眼軸承?�大傾?��?�?(°)

                    // 奇�?檢測?�數
                    singularityVerticalAngle: 18,   // ?�直奇�?角度?��?(?�Z軸夾�?< 此值�?觸發)
                    singularityHorizontalAngle: 87, // 水平奇�?角度?��?(?�Z軸夾�?> 此值�?觸發)
                    singularityExtendRatio: 95,     // 伸�?奇�?比�??��?(%)
                    singularityContractRatio: 105,  // ?�縮奇�?比�??��?(%)

                    // 6DoF ?�大正負姿?��???(?�於?�佳�??��??�制?��???
                    maxPoseLimits: {
                        x: 100,   // 左右移�? (mm)
                        y: 100,   // ?��?移�? (mm)
                        z: 100,   // 上�?移�? (mm)
                        roll: 30, // ?��??��? (°)
                        pitch: 30,// 左右?��? (°)
                        yaw: 15   // ?��?角度 (°)
                    }
                };

                // 注�?：UI 使用?��??��?籤已?�中?�表示�?左右?��? / ?��??��?）�?
                // 但在工�?定義中�?依照下�?變數語�?使用�?
                // - roll  = �?X 軸�?轉�?左右?��?�?
                // - pitch = �?Y 軸�?轉�??��??��?�?
                // 角度?�度??(°)?��?式內?��?算�?getRotationMatrix）�??�這�?變數
                // ?�別表示上面?�述�?軸�?轉�?不�?交�?變數?�稱?��??��?
                this.pose = {
                    x: 0,
                    y: 0,
                    z: 450,  // 調整?�更居中?��?度�??��?干�?並�?供更好�?工�?空�?
                    roll: 0,
                    pitch: 0,
                    yaw: 0
                };
                this.platformPose = {
                    x: 0,
                    y: 0,
                    z: 0,
                    roll: 0,
                    pitch: 0,
                    yaw: 0
                };
                this.poseInput = {
                    x: 0,
                    y: 0,
                    z: 0,
                    roll: 0,
                    pitch: 0,
                    yaw: 0
                };
                this.initialHeight = 450;

                // 座�?系選?��?'world' ??'platform'
                this.coordinateSystem = 'world';
                // ?��??�戶?�平?�座標系下�?輸入?��??��??��?始姿?��?
                this.platformCoordInput = { x: 0, y: 0, z: 450 };

                // ?�設視�??�為等軸視�?
                this.view = 'iso';

                // 視覺?�控?��???
                const isMobile = window.innerWidth <= 768;
                this.visualControl = {
                    scale: isMobile ? 0.3 : 0.5,      // ?��??��?設縮小以?��??��? (0.55 -> 0.3), 桌�??�設 0.5
                    perspective: false,               // ?�否?��??��??�深
                    rotationX: 0,
                    rotationY: 0,
                    rotationZ: 0,
                    panX: 0,
                    panY: 0,
                    isDragging: false,
                    isPanning: false,
                    dragAction: null,
                    lastMouseX: 0,
                    lastMouseY: 0
                };
                this.userHasZoomed = false; // 追蹤?�戶?�否?��?縮放??
                this.userHasPanned = false; // 追蹤?�戶?�否?��?平移??

                // 顯示?��??��?：顯示�?�?-> ?�本?�數索�?
                // 依照要�?：顯�?=??, 2=??, 3=??, 4=??, 5=??, 6=??
                this.displayMap = [2, 1, 0, 5, 4, 3];
                // ?��??��?：�?始索�?-> 顯示位置
                this.inverseMap = (function (m) {
                    const inv = new Array(m.length);
                    for (let i = 0; i < m.length; i++) inv[m[i]] = i;
                    return inv;
                })(this.displayMap);

                // ?��?角度輸入（用?�正?��?學�?
                this.inputServoAngles = [0, 0, 0, 0, 0, 0];

                // �???�學計�?結�?
                this.forwardResult = {
                    pose: { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 },
                    platformPose: { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 },
                    initialHeight: 450,
                    valid: false,
                    errors: [],
                    warnings: []
                };
                this.inverseRequestId = 0;
                this.forwardRequestId = 0;
                this.parameterPresetGeneration = 0;
                this.inverseAbortController = null;
                this.forwardAbortController = null;
                this.stewartInverseEventSource = null;
                this.stewartInverseStreamConnected = false;
                this.pdoEventSource = null;
                this.pdoStreamConnected = false;
                this.mmf1InputMode = 'Unknown';
                this.savedMmf1InputMode = 'ServoAngle';
                this.selectedMmf1InputMode = 'ServoAngle';
                this.mmf1InputModeConfigLoaded = false;
                this.mmf1InputModeConfigSaving = false;
                this.useMmfPoseInput = false;
                this.useMmfServoAngleInput = false;
                this.previousCoordinateSystemBeforeMmf = null;
                this.mmfPoseInput = { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };
                this.mmfServoAngleInput = [0, 0, 0, 0, 0, 0];
                this.lastMmfPoseSignature = '';
                this.lastMmfServoAngleSignature = '';
                this.stewartInverseStreamSeq = 0;
                this.stewartForwardStreamSeq = 0;
                this.stewartNativeTargetSeq = 0;
                this.nativePlannerAutoDragActive = false;
                this.nativePlannerTargetSyncInFlight = false;
                this.nativePlannerPendingTargetPose = null;
                this.nativePlannerConfig = {
                    linearSpeedMmPerSec: 600,
                    angularSpeedDegPerSec: 120
                };
                this.nativePlannerState = {
                    available: false,
                    initialized: false,
                    moving: false,
                    valid: true,
                    seq: 0,
                    cycleCounter: 0,
                    updatePeriodUs: 0,
                    linearSpeedMmPerSec: 0,
                    angularSpeedDegPerSec: 0,
                    currentPose: { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 },
                    targetPose: { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 },
                    errors: []
                };
                this.pendingStewartInverseResponses = new Map();
                this.pendingStewartForwardResponses = new Map();
                this.stewartInverseStreamTimeoutMs = 2000;
                this.stewartForwardStreamTimeoutMs = 2000;
                this.drawFramePending = false;
                this.interactiveUpdateScheduled = false;
                this.interactiveUpdatePending = false;
                this.interactiveUpdateInFlightCount = 0;
                this.interactiveUpdateTimer = null;
                this.interactiveUpdateLastStartedAt = 0;
                this.interactiveUpdateMinIntervalMs = 16;
                this.forwardInteractiveScheduled = false;
                this.forwardInteractivePending = false;
                this.forwardInteractiveTimer = null;
                this.forwardInteractiveLastStartedAt = 0;
                this.forwardInteractiveMinIntervalMs = 16;
                this.previewRenderMinArmCount = 6;
                this.lastRenderableResult = null;

                this.result = {
                    servoAngles: [0, 0, 0, 0, 0, 0],
                    armEndPoints: [],
                    platformPoints: [],
                    basePoints: [],
                    rodLengths: [0, 0, 0, 0, 0, 0],
                    fisheyeAngles: [0, 0, 0, 0, 0, 0],
                    valid: true,
                    errors: [],
                    warnings: [],
                    singularArms: [], // 追蹤?��?奇�??��??�索�?
                    singularityInfo: {},
                    errorArms: [], // 追蹤?��??�誤?��??�索�?
                    warningArms: [],
                    fisheyeWarningArms: []
                };

                // 軌跡顯示?��??�數 - ?�建顯示
                this.trajectorySettings = {
                    trajectorySteps: 36, // 軌跡線段?��? (�?0度�??��?)
                    trajectoryOpacity: 0.3
                };

                this.canvas = document.getElementById('canvas');
                this.ctx = this.canvas.getContext('2d');
                this.setupStewartInverseStream();

                this.initEventListeners();
                this.setupCanvas();

                // 使用 resetToDefaults ?��??��??��?確�??��??�?��??�置?��?一??
                this.resetToDefaults();

                // ?�用?�設視�??��??�?��?不�?覆寫 scale，除??setView ?�確寫入�?
                this.setView(this.view);

                // ?��??��?跡�?例顯示�??��??�步 checkbox
                const trajCheckbox = document.getElementById('showTrajectory');
                const trajLegend = document.getElementById('trajectoryLegend');
                if (trajCheckbox && trajLegend) {
                    // ?�隱?��?跡�?例內容本身�?不觸?�父容器，避?�影?�其他�?�?
                    if (trajCheckbox.checked) {
                        trajLegend.style.visibility = 'visible';
                        trajLegend.style.opacity = '1';
                        trajLegend.setAttribute('aria-hidden', 'false');
                        // ?�復任�??�部?��???display
                        trajLegend.querySelectorAll('*').forEach(el => el.style.display = '');
                    } else {
                        // 使用 visibility ?��?，�??��?局?�父容器顯示
                        trajLegend.style.visibility = 'hidden';
                        trajLegend.style.opacity = '0';
                        trajLegend.setAttribute('aria-hidden', 'true');
                    }
                }

                void this.refreshNativePlannerStatus({ silent: true });
                void this.refreshMmf1InputModeConfig({ silent: true });
                this.setupPdoInputStream();
                void this.refreshPdoInputStatus({ silent: true });
            }

            // ?�步UI顯示??
            syncUIValues() {
                document.getElementById('platformEdgeA').value = this.params.platformEdgeA;
                document.getElementById('platformEdgeB').value = this.params.platformEdgeB;
                document.getElementById('baseEdgeA').value = this.params.baseEdgeA;
                document.getElementById('baseEdgeB').value = this.params.baseEdgeB;
                this.syncMmf1InputModeConfigUi();
                this.syncCoordinateSystemRadios();
                this.updateCoordinateSystemHint();
                // ?�步?�大姿?��?????UI
                if (this.params.maxPoseLimits) {
                    document.getElementById('limitPoseX').value = this.params.maxPoseLimits.x;
                    document.getElementById('limitPoseY').value = this.params.maxPoseLimits.y;
                    document.getElementById('limitPoseZ').value = this.params.maxPoseLimits.z;
                    document.getElementById('limitPoseRoll').value = this.params.maxPoseLimits.roll;
                    document.getElementById('limitPosePitch').value = this.params.maxPoseLimits.pitch;
                    document.getElementById('limitPoseYaw').value = this.params.maxPoseLimits.yaw;
                }
                document.getElementById('rodLength').value = this.params.rodLength;
                document.getElementById('servoArmLength').value = this.params.servoArmLength;
                document.getElementById('servoInitialAngle').value = (this.params.servoInitialAngle !== undefined) ? this.params.servoInitialAngle : 15;
                
                // 計�?並顯示�?始�?�?
                document.getElementById('initialHeight').value = this.initialHeight.toFixed(3);

                document.getElementById('servoAngleLimit').value = this.params.servoAngleLimit;
                // ?�步魚眼軸承角度?�制
                document.getElementById('fisheyeAngleLimit').value = this.params.fisheyeAngleLimit;
                const nativePlannerLinearSpeed = document.getElementById('nativePlannerLinearSpeed');
                if (nativePlannerLinearSpeed) {
                    nativePlannerLinearSpeed.value = this.nativePlannerConfig.linearSpeedMmPerSec;
                }
                const nativePlannerAngularSpeed = document.getElementById('nativePlannerAngularSpeed');
                if (nativePlannerAngularSpeed) {
                    nativePlannerAngularSpeed.value = this.nativePlannerConfig.angularSpeedDegPerSec;
                }
                
                // ?�步奇�??�數
                document.getElementById('singularityVerticalAngle').value = this.params.singularityVerticalAngle || 18;
                document.getElementById('singularityHorizontalAngle').value = this.params.singularityHorizontalAngle || 87;
                document.getElementById('singularityExtendRatio').value = this.params.singularityExtendRatio || 95;
                document.getElementById('singularityContractRatio').value = this.params.singularityContractRatio || 105;
                
                // ?�步?��?安�?角度欄�?，�?顯示?��??��?
                for (let j = 0; j < 6; j++) {
                    const mapped = this.displayMap[j];
                    const el = document.getElementById(`servoAxisAngle${j}`);
                    if (el) el.value = this.params.servoAxisAngle[mapped];
                    // 也更??label ?�稱（顯示為 ?��?1..6�?
                    const labelEl = el ? el.previousElementSibling : null;
                    if (labelEl && labelEl.classList && labelEl.classList.contains('label-small')) {
                        labelEl.textContent = `?��?${j + 1}`;
                    }
                }
            }

            // ?�新?��?始�?度相?��?UI (不影?�正?�編輯�?輸入�?
            updateInitialHeightUI() {
                const el = document.getElementById('initialHeight');
                if (el) el.value = this.initialHeight.toFixed(3);

                this.updatePoseInputsForCoordSystem();
            }

            sanitizePositiveNumber(value, fallbackValue) {
                const numericValue = Number(value);
                return Number.isFinite(numericValue) && numericValue > 0 ? numericValue : fallbackValue;
            }

            normalizePoseValues(pose) {
                const source = pose && typeof pose === 'object' ? pose : {};
                return {
                    x: Number(source.x) || 0,
                    y: Number(source.y) || 0,
                    z: Number(source.z) || 0,
                    roll: Number(source.roll) || 0,
                    pitch: Number(source.pitch) || 0,
                    yaw: Number(source.yaw) || 0
                };
            }

            buildPoseSignature(pose) {
                const normalizedPose = this.normalizePoseValues(pose);
                return [
                    normalizedPose.x,
                    normalizedPose.y,
                    normalizedPose.z,
                    normalizedPose.roll,
                    normalizedPose.pitch,
                    normalizedPose.yaw
                ].map(value => Number(value).toFixed(6)).join('|');
            }

            buildNumericArraySignature(values) {
                if (!Array.isArray(values)) {
                    return '';
                }

                return values.map(value => Number(value).toFixed(6)).join('|');
            }

            normalizeMmf1InputMode(mode) {
                const normalizedMode = String(mode || '').trim().toLowerCase();
                if (normalizedMode === 'pose6d') {
                    return 'Pose6D';
                }
                if (normalizedMode === 'servoangle') {
                    return 'ServoAngle';
                }
                return 'Unknown';
            }

            normalizeCoordinateSystem(value) {
                return String(value || '').trim().toLowerCase() === 'platform' ? 'platform' : 'world';
            }

            getMmf1InputModeDisplayName(mode) {
                return this.normalizeMmf1InputMode(mode) === 'Pose6D' ? '6DOF 座�??? : '伺�?角度??;
            }

            getConfiguredMmf1InputMode() {
                const savedMode = this.normalizeMmf1InputMode(this.savedMmf1InputMode);
                if (savedMode !== 'Unknown') {
                    return savedMode;
                }

                const activeMode = this.normalizeMmf1InputMode(this.mmf1InputMode);
                if (activeMode !== 'Unknown') {
                    return activeMode;
                }

                return 'ServoAngle';
            }

            getSelectedMmf1InputMode() {
                const selectedMode = this.normalizeMmf1InputMode(this.selectedMmf1InputMode);
                if (selectedMode !== 'Unknown') {
                    return selectedMode;
                }

                return this.getConfiguredMmf1InputMode();
            }

            setMmf1InputModeConfigSelection(mode) {
                const normalizedMode = this.normalizeMmf1InputMode(mode);
                if (normalizedMode === 'Unknown') {
                    return;
                }

                this.selectedMmf1InputMode = normalizedMode;
                this.syncMmf1InputModeConfigUi();
            }

            syncMmf1InputModeConfigUi() {
                const selectedMode = this.getSelectedMmf1InputMode();
                const servoAngleRadio = document.getElementById('mmf1InputModeServoAngle');
                const pose6dRadio = document.getElementById('mmf1InputModePose6D');

                if (servoAngleRadio) {
                    servoAngleRadio.checked = selectedMode === 'ServoAngle';
                    servoAngleRadio.disabled = !!this.mmf1InputModeConfigSaving;
                }

                if (pose6dRadio) {
                    pose6dRadio.checked = selectedMode === 'Pose6D';
                    pose6dRadio.disabled = !!this.mmf1InputModeConfigSaving;
                }

                this.updateMmf1InputModeConfigHint();
            }

            updateMmf1InputModeConfigHint() {
                const hint = document.getElementById('mmf1InputModeConfigHint');
                if (!hint) {
                    return;
                }

                if (this.mmf1InputModeConfigSaving) {
                    hint.textContent = '�?��寫入?��?檔�? Par/machine.xml...';
                    return;
                }

                const activeMode = this.normalizeMmf1InputMode(this.mmf1InputMode);
                if (activeMode === 'Unknown') {
                    hint.textContent = '?��??��??��? MMF1 模�??�?��?;
                    return;
                }

                const activeName = this.getMmf1InputModeDisplayName(activeMode);
                hint.textContent = `?��??��?: ${activeName}`;
            }

            applyMmf1InputModeConfigResponse(response) {
                const activeMode = this.normalizeMmf1InputMode(response?.activeMode);
                if (activeMode !== 'Unknown') {
                    this.mmf1InputMode = activeMode;
                }

                const savedMode = this.normalizeMmf1InputMode(response?.savedMode);
                this.savedMmf1InputMode = savedMode !== 'Unknown'
                    ? savedMode
                    : this.getConfiguredMmf1InputMode();
                this.selectedMmf1InputMode = this.savedMmf1InputMode;
                this.mmf1InputModeConfigLoaded = response?.configLoaded !== false;

                this.syncMmf1InputModeConfigUi();
                this.updatePoseSourceUi();
                this.updateForwardInputSourceUi();
                this.syncForwardKinematicsUI();
            }

            isMmfPoseModeAvailable() {
                return this.mmf1InputMode === 'Pose6D';
            }

            isMmfServoAngleModeAvailable() {
                return this.mmf1InputMode === 'ServoAngle';
            }

            isUsingMmfPoseSource() {
                return this.useMmfPoseInput && this.isMmfPoseModeAvailable();
            }

            isUsingMmfServoAngleSource() {
                return this.useMmfServoAngleInput && this.isMmfServoAngleModeAvailable();
            }

            isCanvasInteractionLocked() {
                return this.isUsingMmfPoseSource() || this.isUsingMmfServoAngleSource();
            }

            refreshCanvasInteractionState() {
                if (!this.canvas) {
                    return;
                }

                this.canvas.style.cursor = this.isCanvasInteractionLocked() ? 'default' : 'grab';
            }

            getActiveForwardServoAngles() {
                const sourceValues = this.isUsingMmfServoAngleSource()
                    ? this.mmfServoAngleInput
                    : this.inputServoAngles;
                if (!Array.isArray(sourceValues) || sourceValues.length < 6) {
                    return [0, 0, 0, 0, 0, 0];
                }

                return sourceValues.slice(0, 6).map(value => Number(value) || 0);
            }

            syncCoordinateSystemRadios() {
                const worldRadio = document.getElementById('coordSystemWorld');
                const platformRadio = document.getElementById('coordSystemPlatform');
                if (worldRadio) {
                    worldRadio.checked = this.coordinateSystem === 'world';
                }
                if (platformRadio) {
                    platformRadio.checked = this.coordinateSystem === 'platform';
                }
            }

            updateCoordinateSystemHint() {
                const hint = document.getElementById('coordSystemHint');
                if (!hint) {
                    return;
                }

                if (this.isUsingMmfPoseSource()) {
                    hint.textContent = this.coordinateSystem === 'world'
                        ? 'MMF1 6DoF 輸入?��??��??�座標系�??（Z ?�相對�?移�?'
                        : 'MMF1 6DoF 輸入?��??�平?�座標系�??（XYZ 沿平?�本身�?軸移?��?';
                    return;
                }

                if (this.coordinateSystem === 'world') {
                    hint.textContent = '?��?使用世�?座�?系移??;
                } else {
                    hint.textContent = '?��?使用平台座�?系移?��?XYZ 沿平?�本身�?軸移?��?';
                }
            }

            updatePoseSourceUi() {
                const checkbox = document.getElementById('useMmfPoseInput');
                const label = document.getElementById('poseInputSourceLabel');
                const hint = document.getElementById('poseInputSourceHint');
                const usingMmfPoseSource = this.isUsingMmfPoseSource();
                const mmfPoseAvailable = this.isMmfPoseModeAvailable();

                if (checkbox) {
                    checkbox.checked = usingMmfPoseSource;
                    checkbox.disabled = !mmfPoseAvailable;
                }
                if (label) {
                    label.classList.toggle('pose-source-disabled', !mmfPoseAvailable);
                }

                ['X', 'Y', 'Z', 'Roll', 'Pitch', 'Yaw'].forEach((axis) => {
                    const input = document.getElementById(`pose${axis}`);
                    if (!input) {
                        return;
                    }
                    input.readOnly = usingMmfPoseSource;
                    input.classList.toggle('pose-source-locked', usingMmfPoseSource);
                });

                if (hint) {
                    if (!mmfPoseAvailable) {
                        hint.textContent = 'MMF1 ?��?不是 6DoF 模�?，�??�使??web UI 輸入??;
                    } else if (usingMmfPoseSource) {
                        hint.textContent = '?��?使用 MMF1 6DoF ?��??��??��???1=Sway 2=Surge 3=Heave 4=Pitch 5=Roll 6=FYaw，�??�座標�?平台座�??��??�決定這�??��?�???��?，並禁用作�??�曳??;
                    } else {
                        hint.textContent = '?��?使用 web UI 輸入；勾?��??�改??MMF1 6DoF ?��??��??��?�?=Sway 2=Surge 3=Heave 4=Pitch 5=Roll 6=FYaw）�?;
                    }
                }

                this.refreshCanvasInteractionState();
                this.updateCoordinateSystemHint();
            }

            updateForwardInputSourceUi() {
                const checkbox = document.getElementById('useMmfServoInput');
                const label = document.getElementById('forwardInputSourceLabel');
                const hint = document.getElementById('forwardInputSourceHint');
                const calculateButton = document.getElementById('calculateForwardBtn');
                const usingMmfServoSource = this.isUsingMmfServoAngleSource();
                const mmfServoAvailable = this.isMmfServoAngleModeAvailable();

                if (checkbox) {
                    checkbox.checked = usingMmfServoSource;
                    checkbox.disabled = !mmfServoAvailable;
                }
                if (label) {
                    label.classList.toggle('pose-source-disabled', !mmfServoAvailable);
                }

                for (let i = 0; i < 6; i++) {
                    const input = document.getElementById(`inputServoAngle${i}`);
                    if (!input) {
                        continue;
                    }
                    input.readOnly = usingMmfServoSource;
                    input.classList.toggle('pose-source-locked', usingMmfServoSource);
                }

                const syncButton = document.getElementById('syncFromResultBtn');
                if (syncButton) {
                    syncButton.disabled = usingMmfServoSource;
                }
                if (calculateButton) {
                    calculateButton.disabled = usingMmfServoSource;
                }

                this.refreshCanvasInteractionState();

                if (hint) {
                    if (!mmfServoAvailable) {
                        hint.textContent = 'MMF1 ?��?不是角度模�?，�??�使??web UI 輸入??;
                    } else if (usingMmfServoSource) {
                        hint.textContent = '?��?使用 MMF1 ?��?角度?��?此�?源�?覆�? web UI ?��?角度輸入?��??��??�並禁用作�??�曳??;
                    } else {
                        hint.textContent = '?��?使用 web UI 輸入；勾?��??�改??MMF1 ?��?角度?��?;
                    }
                }
            }

            setUseMmfPoseInput(shouldUse, options = {}) {
                const { forceRecalculate = false } = options;
                const previousUseMmfPoseSource = this.isUsingMmfPoseSource();
                const nextUseMmfPoseSource = shouldUse && this.isMmfPoseModeAvailable();

                if (nextUseMmfPoseSource === previousUseMmfPoseSource) {
                    this.updatePoseSourceUi();
                    this.updatePoseInputsForCoordSystem();
                    return;
                }

                this.useMmfPoseInput = nextUseMmfPoseSource;
                if (nextUseMmfPoseSource) {
                    this.visualControl.isDragging = false;
                    this.visualControl.isPanning = false;
                    this.visualControl.dragAction = null;
                } else if (previousUseMmfPoseSource) {
                    this.previousCoordinateSystemBeforeMmf = null;
                }
                this.syncCoordinateSystemRadios();
                this.updatePoseSourceUi();
                this.updatePoseInputsForCoordSystem();

                if (forceRecalculate) {
                    if (nextUseMmfPoseSource) {
                        this.requestInteractiveUpdateAndDraw(true);
                    } else {
                        void this.updateAndDraw();
                    }
                }
            }

            setUseMmfServoAngleInput(shouldUse) {
                const previousUseMmfServoSource = this.isUsingMmfServoAngleSource();
                const nextUseMmfServoSource = shouldUse && this.isMmfServoAngleModeAvailable();

                if (previousUseMmfServoSource === nextUseMmfServoSource) {
                    this.updateForwardInputSourceUi();
                    this.syncForwardKinematicsUI();
                    return;
                }

                this.useMmfServoAngleInput = nextUseMmfServoSource;
                if (nextUseMmfServoSource) {
                    this.visualControl.isDragging = false;
                    this.visualControl.isPanning = false;
                    this.visualControl.dragAction = null;
                } else {
                    this.cancelForwardInteractiveUpdate();
                }
                this.updateForwardInputSourceUi();
                this.syncForwardKinematicsUI();

                if (nextUseMmfServoSource) {
                    this.requestForwardInteractiveUpdate(true);
                }
            }

            extractMmfPoseInputFromPdoPayload(payload) {
                if (!payload || !Array.isArray(payload.slaves) || payload.slaves.length < 6) {
                    return null;
                }

                const values = payload.slaves.slice(0, 6).map((slave) => Number(slave?.mmfIn));
                if (!values.every(Number.isFinite)) {
                    return null;
                }

                // MMF1 6DoF input order: 1=Sway, 2=Surge, 3=Heave, 4=Pitch, 5=Roll, 6=FYaw.
                return {
                    x: values[0],
                    y: values[1],
                    z: values[2],
                    roll: values[3],
                    pitch: values[4],
                    yaw: values[5]
                };
            }

            extractMmfServoAngleInputFromPdoPayload(payload) {
                if (!payload || !Array.isArray(payload.slaves) || payload.slaves.length < 6) {
                    return null;
                }

                const values = payload.slaves.slice(0, 6).map((slave) => Number(slave?.mmfIn));
                if (!values.every(Number.isFinite)) {
                    return null;
                }

                return values;
            }

            handlePdoInputPayload(payload) {
                const previousUseMmfPoseSource = this.isUsingMmfPoseSource();
                const previousUseMmfServoSource = this.isUsingMmfServoAngleSource();
                this.mmf1InputMode = this.normalizeMmf1InputMode(payload?.mmf1InputMode);

                const nextMmfPoseInput = this.extractMmfPoseInputFromPdoPayload(payload);
                let poseChanged = false;
                if (nextMmfPoseInput) {
                    const nextSignature = this.buildPoseSignature(nextMmfPoseInput);
                    poseChanged = nextSignature !== this.lastMmfPoseSignature;
                    this.mmfPoseInput = this.normalizePoseValues(nextMmfPoseInput);
                    this.lastMmfPoseSignature = nextSignature;
                }

                const nextMmfServoAngleInput = this.extractMmfServoAngleInputFromPdoPayload(payload);
                let servoAnglesChanged = false;
                if (nextMmfServoAngleInput) {
                    const nextSignature = this.buildNumericArraySignature(nextMmfServoAngleInput);
                    servoAnglesChanged = nextSignature !== this.lastMmfServoAngleSignature;
                    this.mmfServoAngleInput = nextMmfServoAngleInput.map(value => Number(value) || 0);
                    this.lastMmfServoAngleSignature = nextSignature;
                }

                if (previousUseMmfPoseSource && !this.isMmfPoseModeAvailable()) {
                    this.setUseMmfPoseInput(false, { forceRecalculate: true });
                }

                if (previousUseMmfServoSource && !this.isMmfServoAngleModeAvailable()) {
                    this.setUseMmfServoAngleInput(false);
                }

                this.updatePoseSourceUi();
                this.updateForwardInputSourceUi();
                this.syncMmf1InputModeConfigUi();

                if (this.isUsingMmfPoseSource()) {
                    this.updatePoseInputsForCoordSystem();
                    if (poseChanged) {
                        this.requestInteractiveUpdateAndDraw();
                    }
                }

                if (this.isUsingMmfServoAngleSource()) {
                    this.syncForwardKinematicsUI();
                    if (servoAnglesChanged) {
                        this.requestForwardInteractiveUpdate();
                    }
                }
            }

            normalizePointArray(points) {
                if (!Array.isArray(points)) {
                    return [];
                }

                return points
                    .filter(point => Array.isArray(point) && point.length >= 3)
                    .map(point => [
                        Number(point[0]) || 0,
                        Number(point[1]) || 0,
                        Number(point[2]) || 0
                    ]);
            }

            normalizeErrorArms(errorArms, errors) {
                if (Array.isArray(errorArms)) {
                    return [...new Set(errorArms.map(value => Number(value)).filter(Number.isInteger))];
                }

                if (!Array.isArray(errors)) {
                    return [];
                }

                const normalized = [];
                errors.forEach((error) => {
                    const message = String(error || '');
                    const match = message.match(/(?:?�|?��?)\s*(\d+)/u);
                    if (!match) {
                        return;
                    }

                    const zeroBasedIndex = Number(match[1]) - 1;
                    if (Number.isInteger(zeroBasedIndex) && zeroBasedIndex >= 0 && zeroBasedIndex < 6) {
                        normalized.push(zeroBasedIndex);
                    }
                });

                return [...new Set(normalized)];
            }

            getCurrentDisplayPose() {
                if (this.coordinateSystem === 'platform') {
                    return this.normalizePoseValues(this.platformPose);
                }

                return {
                    x: Number(this.pose.x) || 0,
                    y: Number(this.pose.y) || 0,
                    z: (Number(this.pose.z) || 0) - (Number(this.initialHeight) || 0),
                    roll: Number(this.pose.roll) || 0,
                    pitch: Number(this.pose.pitch) || 0,
                    yaw: Number(this.pose.yaw) || 0
                };
            }

            syncPoseInputFromResolvedPose() {
                this.poseInput = this.getCurrentDisplayPose();
            }

            syncNativePlannerSpeedInputs() {
                const nativePlannerLinearSpeed = document.getElementById('nativePlannerLinearSpeed');
                if (nativePlannerLinearSpeed) {
                    nativePlannerLinearSpeed.value = this.nativePlannerConfig.linearSpeedMmPerSec;
                }
                const nativePlannerAngularSpeed = document.getElementById('nativePlannerAngularSpeed');
                if (nativePlannerAngularSpeed) {
                    nativePlannerAngularSpeed.value = this.nativePlannerConfig.angularSpeedDegPerSec;
                }
            }

            updateNativePlannerConfigFromInput() {
                const nativePlannerLinearSpeed = document.getElementById('nativePlannerLinearSpeed');
                const nativePlannerAngularSpeed = document.getElementById('nativePlannerAngularSpeed');

                if (nativePlannerLinearSpeed) {
                    this.nativePlannerConfig.linearSpeedMmPerSec = this.sanitizePositiveNumber(
                        nativePlannerLinearSpeed.value,
                        this.nativePlannerConfig.linearSpeedMmPerSec || 600
                    );
                    nativePlannerLinearSpeed.value = this.nativePlannerConfig.linearSpeedMmPerSec;
                }

                if (nativePlannerAngularSpeed) {
                    this.nativePlannerConfig.angularSpeedDegPerSec = this.sanitizePositiveNumber(
                        nativePlannerAngularSpeed.value,
                        this.nativePlannerConfig.angularSpeedDegPerSec || 120
                    );
                    nativePlannerAngularSpeed.value = this.nativePlannerConfig.angularSpeedDegPerSec;
                }
            }

            // 計�?對稱?��?形�??��??��? (對�?平�?，A-B-A-B-A-B交替)
            calculateSymmetricHexagonPoints(edgeA, edgeB, zHeight) {
                const points = [];

                // ?�正?��?稱六?�形：A-B-A-B-A-B交替?��?，�??�平�?
                // 不能簡單使用不�??��?，�??�那�???�產?�正確�??�長
                // 使用?�步構建?�方法�?從第一?��?點�?始�?確�?每�??�長�?��

                // �??��?點�??�在正X軸�?
                points.push([edgeA, 0, zHeight]);

                // �??��?點�?從第0?��?點出?��??�長?�edgeA，�?�?0�?
                const angle1 = Math.PI / 3; // 60�?
                points.push([
                    points[0][0] + edgeA * Math.cos(angle1),
                    points[0][1] + edgeA * Math.sin(angle1),
                    zHeight
                ]);

                // �??��?點�?從第1?��?點出?��??�長?�edgeB，�?�?0�?
                const angle2 = angle1 + Math.PI / 3;
                points.push([
                    points[1][0] + edgeB * Math.cos(angle2),
                    points[1][1] + edgeB * Math.sin(angle2),
                    zHeight
                ]);

                // �??��?點�?從第2?��?點出?��??�長?�edgeA，�?�?0�?
                const angle3 = angle2 + Math.PI / 3;
                points.push([
                    points[2][0] + edgeA * Math.cos(angle3),
                    points[2][1] + edgeA * Math.sin(angle3),
                    zHeight
                ]);

                // �??��?點�?從第3?��?點出?��??�長?�edgeB，�?�?0�?
                const angle4 = angle3 + Math.PI / 3;
                points.push([
                    points[3][0] + edgeB * Math.cos(angle4),
                    points[3][1] + edgeB * Math.sin(angle4),
                    zHeight
                ]);

                // �??��?點�?從第4?��?點出?��??�長?�edgeA，�?�?0�?
                const angle5 = angle4 + Math.PI / 3;
                points.push([
                    points[4][0] + edgeA * Math.cos(angle5),
                    points[4][1] + edgeA * Math.sin(angle5),
                    zHeight
                ]);

                // 計�??��?形�??��?（質心�?
                let centerX = 0, centerY = 0;
                for (let i = 0; i < 6; i++) {
                    centerX += points[i][0];
                    centerY += points[i][1];
                }
                centerX /= 6;
                centerY /= 6;

                // 將�??��?點平移�?使�?心�??��?�?
                for (let i = 0; i < 6; i++) {
                    points[i][0] -= centerX;
                    points[i][1] -= centerY;
                }

                return points;
            }

            setupCanvas() {
                const resizeCanvas = () => {
                    const container = this.canvas.parentElement;
                    const rect = container.getBoundingClientRect();
                    // 使用容器?�部寬�?，去?��?餘�?�?
                    const canvasWidth = Math.max(10, Math.floor(rect.width));
                    const canvasHeight = Math.max(10, Math.floor(rect.height));

                    // ?�援�?DPI 顯示??(devicePixelRatio)，�??��??��?機�??��???
                    const dpr = window.devicePixelRatio || 1;

                    // 設置?��??�內?��?素大小�??��?維�? CSS 寬�??�容?�大�?
                    this.canvas.width = Math.floor(canvasWidth * dpr);
                    this.canvas.height = Math.floor(canvasHeight * dpr);
                    this.canvas.style.width = canvasWidth + 'px';
                    this.canvas.style.height = canvasHeight + 'px';

                    // ?�置?�縮?�繪?��?下�?，避?��?�?scale 導致?�誤
                    if (this.ctx && typeof this.ctx.setTransform === 'function') {
                        this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
                    }

                    // ?�適?�縮?��?如�??��?機�?且�??��??�縮?��?
                    if (!this.userHasZoomed && window.innerWidth <= 768) {
                        // ?��?寬度比�?調整縮放，確保模?��??�顯�?
                        // ?��?寬度 360px (常�??��?) -> scale 0.45
                        const ratio = rect.width / 360;
                        let newScale = 0.3 * ratio;
                        // ?�制?��??��???
                        this.visualControl.scale = Math.max(0.3, Math.min(0.6, newScale));
                    }

                    // ?�新繪製 (resize 不�??�新計�?)
                    this.draw();
                };

                // ?�露 resize 以便外部?�叫（�?如全?��??��?後�?要�??�調?�大小�?
                this.resizeCanvas = resizeCanvas;

                // ?��?設置
                setTimeout(resizeCanvas, 100);

                    // ??��視�?大�?變�??�方?��??��?行�?裝置�?
                    window.addEventListener('resize', () => {
                        setTimeout(resizeCanvas, 100);
                    });
                    window.addEventListener('orientationchange', () => {
                        setTimeout(resizeCanvas, 150);
                    });
            }

            initEventListeners() {
                // 對稱?��?形�??��???
                const platformEdgeAInput = document.getElementById('platformEdgeA');
                platformEdgeAInput.addEventListener('input', (e) => {
                    this.params.platformEdgeA = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                platformEdgeAInput.addEventListener('change', () => this.updateAndDraw());


                const platformEdgeBInput = document.getElementById('platformEdgeB');
                platformEdgeBInput.addEventListener('input', (e) => {
                    this.params.platformEdgeB = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                platformEdgeBInput.addEventListener('change', () => this.updateAndDraw());

                const baseEdgeAInput = document.getElementById('baseEdgeA');
                baseEdgeAInput.addEventListener('input', (e) => {
                    this.params.baseEdgeA = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                baseEdgeAInput.addEventListener('change', () => this.updateAndDraw());

                const baseEdgeBInput = document.getElementById('baseEdgeB');
                baseEdgeBInput.addEventListener('input', (e) => {
                    this.params.baseEdgeB = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                baseEdgeBInput.addEventListener('change', () => this.updateAndDraw());

                // 機�??�數 (?�在?�唯讀，由?�長計�?得出) - 顯示?�只讀欄�?，無?�??��輸入事件

                const rodLengthInput = document.getElementById('rodLength');
                rodLengthInput.addEventListener('input', (e) => {
                    this.params.rodLength = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                rodLengthInput.addEventListener('change', () => this.updateAndDraw());

                const servoArmLengthInput = document.getElementById('servoArmLength');
                servoArmLengthInput.addEventListener('input', (e) => {
                    this.params.servoArmLength = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                servoArmLengthInput.addEventListener('change', () => this.updateAndDraw());

                const servoInitialAngleInput = document.getElementById('servoInitialAngle');
                servoInitialAngleInput.addEventListener('input', (e) => {
                    this.params.servoInitialAngle = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                servoInitialAngleInput.addEventListener('change', () => this.updateAndDraw());

                const servoAngleLimitInput = document.getElementById('servoAngleLimit');
                servoAngleLimitInput.addEventListener('input', (e) => {
                    this.params.servoAngleLimit = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                servoAngleLimitInput.addEventListener('change', () => this.updateAndDraw());

                // 魚眼軸承角度?�制?�制
                const fisheyeAngleLimitInput = document.getElementById('fisheyeAngleLimit');
                fisheyeAngleLimitInput.addEventListener('input', (e) => {
                    this.params.fisheyeAngleLimit = Number(e.target.value);
                    this.requestInteractiveUpdateAndDraw();
                });
                fisheyeAngleLimitInput.addEventListener('change', () => this.updateAndDraw());

                // 奇�??�數?�制
                const singularityVerticalAngleInput = document.getElementById('singularityVerticalAngle');
                singularityVerticalAngleInput.addEventListener('input', (e) => {
                    this.params.singularityVerticalAngle = Number(e.target.value);
                    this.updateAndDraw();
                });
                singularityVerticalAngleInput.addEventListener('change', () => this.updateAndDraw());

                const singularityHorizontalAngleInput = document.getElementById('singularityHorizontalAngle');
                singularityHorizontalAngleInput.addEventListener('input', (e) => {
                    this.params.singularityHorizontalAngle = Number(e.target.value);
                    this.updateAndDraw();
                });
                singularityHorizontalAngleInput.addEventListener('change', () => this.updateAndDraw());

                const singularityExtendRatioInput = document.getElementById('singularityExtendRatio');
                singularityExtendRatioInput.addEventListener('input', (e) => {
                    this.params.singularityExtendRatio = Number(e.target.value);
                    this.updateAndDraw();
                });
                singularityExtendRatioInput.addEventListener('change', () => this.updateAndDraw());

                const singularityContractRatioInput = document.getElementById('singularityContractRatio');
                singularityContractRatioInput.addEventListener('input', (e) => {
                    this.params.singularityContractRatio = Number(e.target.value);
                    this.updateAndDraw();
                });
                singularityContractRatioInput.addEventListener('change', () => this.updateAndDraw());

                // 軌跡顯示?�制
                document.getElementById('showTrajectory').addEventListener('change', (e) => {
                    this.trajectorySettings.trajectoryOpacity = e.target.checked ? 0.3 : 0.0;
                    const trajLegend = document.getElementById('trajectoryLegend');
                    if (trajLegend) {
                        if (e.target.checked) {
                            trajLegend.style.visibility = 'visible';
                            trajLegend.style.opacity = '1';
                            trajLegend.setAttribute('aria-hidden', 'false');
                            trajLegend.querySelectorAll('*').forEach(el => el.style.display = '');
                        } else {
                            trajLegend.style.visibility = 'hidden';
                            trajLegend.style.opacity = '0';
                            trajLegend.setAttribute('aria-hidden', 'true');
                        }
                    }
                    // 立即?�繪?�更?�顯�?
                    this.draw();
                    this.updateDisplay();
                });

                // ?�深?��??�制
                document.getElementById('enablePerspective').addEventListener('change', (e) => {
                    this.visualControl.perspective = e.target.checked;
                    this.draw();
                });

                // 將輸?��?定到實�??�數索�?（�?顯示?��? map�?
                for (let i = 0; i < 6; i++) {
                    const axisInput = document.getElementById(`servoAxisAngle${i}`);
                    const paramIndex = this.displayMap[i];
                    if (!axisInput) continue;
                    axisInput.addEventListener('input', (e) => {
                        this.params.servoAxisAngle[paramIndex] = Number(e.target.value);
                        this.requestInteractiveUpdateAndDraw();
                    });
                    axisInput.addEventListener('change', () => this.updateAndDraw());
                }

                const mmf1InputModeConfigRadios = document.querySelectorAll('input[name="mmf1InputModeConfig"]');
                mmf1InputModeConfigRadios.forEach(radio => {
                    radio.addEventListener('change', (e) => {
                        if (!e.target.checked) {
                            return;
                        }

                        this.setMmf1InputModeConfigSelection(e.target.value);
                    });
                });

                // 座�?系選??
                const coordSystemRadios = document.querySelectorAll('input[name="coordSystem"]');
                coordSystemRadios.forEach(radio => {
                    radio.addEventListener('change', (e) => {
                        this.coordinateSystem = e.target.value;
                        this.updateCoordinateSystemHint();
                        
                        // 座�?系�??��?，�??�當?��??�座標�?�?
                        // ?�新輸入框顯示為對�?座�?系�???
                        this.updatePoseInputsForCoordSystem();
                        // ?��??�新�???�學結�?顯示
                        this.updateForwardDisplay();

                        if (this.isUsingMmfPoseSource()) {
                            this.requestInteractiveUpdateAndDraw(true);
                        }
                    });
                });

                const useMmfPoseInput = document.getElementById('useMmfPoseInput');
                if (useMmfPoseInput) {
                    useMmfPoseInput.addEventListener('change', (e) => {
                        if (!this.isMmfPoseModeAvailable()) {
                            e.target.checked = false;
                            this.setUseMmfPoseInput(false);
                            return;
                        }

                        this.setUseMmfPoseInput(!!e.target.checked, { forceRecalculate: true });
                    });
                }

                const useMmfServoInput = document.getElementById('useMmfServoInput');
                if (useMmfServoInput) {
                    useMmfServoInput.addEventListener('change', (e) => {
                        if (!this.isMmfServoAngleModeAvailable()) {
                            e.target.checked = false;
                            this.setUseMmfServoAngleInput(false);
                            return;
                        }

                        this.setUseMmfServoAngleInput(!!e.target.checked);
                    });
                }

                // 姿�??�制
                const poseControls = ['X', 'Y', 'Z', 'Roll', 'Pitch', 'Yaw'];
                poseControls.forEach(axis => {
                    const input = document.getElementById(`pose${axis}`);

                    input.addEventListener('input', (e) => {
                        if (this.isUsingMmfPoseSource()) {
                            return;
                        }
                        const value = Number(e.target.value);
                        this.poseInput[axis.toLowerCase()] = value;
                        this.requestInteractiveUpdateAndDraw();
                    });
                    // ?�用?��??�輸?��?change/blur/Enter）�?，執行�??��?�?
                    input.addEventListener('change', () => this.updateAndDraw());
                    input.addEventListener('blur', () => this.updateAndDraw());
                    input.addEventListener('keydown', (ev) => {
                        if (ev.key === 'Enter') this.updateAndDraw();
                    });
                });

                // 視�??��? (桌�? + 行�? overlay)
                const bindViewButton = (id, viewName) => {
                    const el = document.getElementById(id);
                    if (el) el.addEventListener('click', () => this.setView(viewName));
                };

                // 綁�?桌�??��?（�??��??��?
                bindViewButton('viewIso', 'iso');
                bindViewButton('viewSide', 'side');
                bindViewButton('viewTop', 'top');

                // 綁�?行�? overlay（若存在，ID ??viewIsoMobile 等�?
                bindViewButton('viewIsoMobile', 'iso');
                bindViewButton('viewSideMobile', 'side');
                bindViewButton('viewTopMobile', 'top');

                // 視�?調整?��?
                // 使用增�??��?，�??��??��??��?標軸?��?
                const rotationStep = 15; // 每次?��??��?�?
                
                document.getElementById('rotateXUp').addEventListener('click', () => {
                    this.applyWorldAxisRotation('X', rotationStep);
                    this.draw();
                });
                document.getElementById('rotateXDown').addEventListener('click', () => {
                    this.applyWorldAxisRotation('X', -rotationStep);
                    this.draw();
                });
                document.getElementById('rotateYUp').addEventListener('click', () => {
                    this.applyWorldAxisRotation('Y', rotationStep);
                    this.draw();
                });
                document.getElementById('rotateYDown').addEventListener('click', () => {
                    this.applyWorldAxisRotation('Y', -rotationStep);
                    this.draw();
                });
                document.getElementById('rotateZUp').addEventListener('click', () => {
                    this.applyWorldAxisRotation('Z', rotationStep);
                    this.draw();
                });
                document.getElementById('rotateZDown').addEventListener('click', () => {
                    this.applyWorldAxisRotation('Z', -rotationStep);
                    this.draw();
                });

                // ?��?交�?事件
                this.setupCanvasInteraction();

                // �???�學?��?事件??��??
                this.setupForwardKinematicsEventListeners();

                // ?�置?�設?��???
                document.getElementById('resetDefaultsBtn').addEventListener('click', () => this.resetToDefaults());

                // ?�佳�??�數?��?
                document.getElementById('optimizeParamsBtn').addEventListener('click', () => this.optimizeParameters());

                // ?�出?�數?��?
                document.getElementById('exportParamsBtn').addEventListener('click', () => this.exportParameters());

                // ?�入?�數?��?
                document.getElementById('importParamsBtn').addEventListener('click', () => this.importParameters());

                const nativePlannerLinearSpeed = document.getElementById('nativePlannerLinearSpeed');
                if (nativePlannerLinearSpeed) {
                    nativePlannerLinearSpeed.addEventListener('input', () => {
                        this.updateNativePlannerConfigFromInput();
                        this.updateNativePlannerDisplay();
                    });
                    nativePlannerLinearSpeed.addEventListener('change', () => {
                        this.updateNativePlannerConfigFromInput();
                        this.updateNativePlannerDisplay();
                    });
                }

                const nativePlannerAngularSpeed = document.getElementById('nativePlannerAngularSpeed');
                if (nativePlannerAngularSpeed) {
                    nativePlannerAngularSpeed.addEventListener('input', () => {
                        this.updateNativePlannerConfigFromInput();
                        this.updateNativePlannerDisplay();
                    });
                    nativePlannerAngularSpeed.addEventListener('change', () => {
                        this.updateNativePlannerConfigFromInput();
                        this.updateNativePlannerDisplay();
                    });
                }

                // ??�� poseLimits 輸入變�?
                const limitIds = ['limitPoseX', 'limitPoseY', 'limitPoseZ', 'limitPoseRoll', 'limitPosePitch', 'limitPoseYaw'];
                limitIds.forEach(id => {
                    const el = document.getElementById(id);
                    if (!el) return;
                    el.addEventListener('change', (e) => {
                        const v = Number(e.target.value);
                        switch (id) {
                            case 'limitPoseX': this.params.maxPoseLimits.x = v; break;
                            case 'limitPoseY': this.params.maxPoseLimits.y = v; break;
                            case 'limitPoseZ': this.params.maxPoseLimits.z = v; break;
                            case 'limitPoseRoll': this.params.maxPoseLimits.roll = v; break;
                            case 'limitPosePitch': this.params.maxPoseLimits.pitch = v; break;
                            case 'limitPoseYaw': this.params.maxPoseLimits.yaw = v; break;
                        }
                    });
                });
            }

            setupCanvasInteraction() {
                // 設置默�?游�??�可?��??�??
                this.refreshCanvasInteractionState();

                // 滾輪縮放 (?��??�新繪製，�?必�??��?�?
                this.canvas.addEventListener('wheel', (e) => {
                    e.preventDefault();
                    this.userHasZoomed = true; // 標�??�戶已�??�縮??
                    const delta = e.deltaY > 0 ? 0.9 : 1.1;
                    this.visualControl.scale = Math.max(0.1, Math.min(10.0, this.visualControl.scale * delta));
                    this.requestDraw();
                });

                // 鼠�??�拽?��?（統一 dragAction�?pan'|'posePan'|'poseRotate'�?
                this.canvas.addEventListener('mousedown', (e) => {
                    if (this.isCanvasInteractionLocked()) {
                        return;
                    }

                    e.preventDefault();

                    // ?�設標�?
                    this.visualControl.isDragging = true;
                    this.visualControl.dragAction = null;
                    this.syncPoseInputFromResolvedPose();

                    if (e.button === 0) { // 左鍵
                        if (e.shiftKey) {
                            // Shift+左鍵 -> canvas 平移
                            this.visualControl.dragAction = 'pan';
                            this.visualControl.isPanning = true;
                            this.userHasPanned = true; // 標�?已�??�平�?
                            this.canvas.style.cursor = 'move';
                        } else {
                            // 左鍵 -> 調整平台位移 (pose.x / pose.y)
                            this.visualControl.dragAction = 'posePan';
                            this.visualControl.isPanning = false;
                            this.canvas.style.cursor = 'grabbing';
                        }
                    } else if (e.button === 1) { // 中鍵 -> 調整 Z (上�?) ??Yaw (左右?��?)
                        this.visualControl.dragAction = 'poseZRotate';
                        this.visualControl.isPanning = false;
                        this.canvas.style.cursor = 'move';
                    } else if (e.button === 2) { // ?�鍵 -> 調整平台角度 (pitch/roll)
                        this.visualControl.dragAction = 'poseRotate';
                        this.visualControl.isPanning = false;
                        this.canvas.style.cursor = 'grabbing';
                    }

                    this.visualControl.lastMouseX = e.clientX;
                    this.visualControl.lastMouseY = e.clientY;

                });

                // 禁用?�鍵?�單
                this.canvas.addEventListener('contextmenu', (e) => {
                    e.preventDefault();
                });

                document.addEventListener('mousemove', (e) => {
                    if (!this.visualControl.isDragging) return;

                    const deltaX = e.clientX - this.visualControl.lastMouseX;
                    const deltaY = e.clientY - this.visualControl.lastMouseY;

                    // ?��??�確實修?�平??pose ?��??�要�??��?�?
                    let poseChanged = false;

                    switch (this.visualControl.dragAction) {
                        case 'poseZRotate': {
                            const zSensitivity = 0.5; // Z軸�??�度
                            const yawSensitivity = 0.3; // ?��??��?�?
                            this.poseInput.z -= deltaY * zSensitivity;
                            
                            // 左右?�曳 -> Yaw?��?
                            this.poseInput.yaw -= deltaX * yawSensitivity;

                            const normAngle = (a) => {
                                if (a > 180) return ((a + 180) % 360) - 180;
                                if (a < -180) return ((a - 180) % 360) + 180;
                                return a;
                            };
                            this.poseInput.yaw = normAngle(this.poseInput.yaw);

                            document.getElementById('poseZ').value = this.poseInput.z.toFixed(3);
                            document.getElementById('poseYaw').value = this.poseInput.yaw.toFixed(3);
                            poseChanged = true;
                            break;
                        }
                        case 'poseRotate': {
                            const rotSensitivity = 0.15;
                            this.poseInput.pitch += deltaX * rotSensitivity;
                            this.poseInput.roll -= deltaY * rotSensitivity;

                            const normAngle = (a) => {
                                if (a > 180) return ((a + 180) % 360) - 180;
                                if (a < -180) return ((a - 180) % 360) + 180;
                                return a;
                            };
                            this.poseInput.roll = normAngle(this.poseInput.roll);
                            this.poseInput.pitch = normAngle(this.poseInput.pitch);

                            document.getElementById('poseRoll').value = this.poseInput.roll.toFixed(3);
                            document.getElementById('posePitch').value = this.poseInput.pitch.toFixed(3);
                            poseChanged = true;
                            break;
                        }
                        case 'posePan': {
                            const sensitivity = 0.3;
                            this.poseInput.x += deltaX * sensitivity;
                            this.poseInput.y -= deltaY * sensitivity;
                            document.getElementById('poseX').value = this.poseInput.x.toFixed(3);
                            document.getElementById('poseY').value = this.poseInput.y.toFixed(3);
                            poseChanged = true;
                            break;
                        }
                        case 'pan': {
                            const panSensitivity = 1.0;
                            this.visualControl.panX += deltaX * panSensitivity;
                            this.visualControl.panY += deltaY * panSensitivity;
                            break;
                        }
                        default:
                            break;
                    }

                    this.visualControl.lastMouseX = e.clientX;
                    this.visualControl.lastMouseY = e.clientY;

                    if (poseChanged) {
                        void this.startNativePlannerForDrag();
                        this.requestDraw();
                        this.requestInteractiveUpdateAndDraw();
                    } else {
                        this.requestDraw();
                    }
                });

                document.addEventListener('mouseup', () => {
                    if (this.visualControl.isDragging) {
                        const shouldFinalizePose = this.visualControl.dragAction && this.visualControl.dragAction.startsWith('pose');
                        this.visualControl.isDragging = false;
                        this.visualControl.isPanning = false;
                        this.visualControl.dragAction = null;
                        this.refreshCanvasInteractionState();
                        if (shouldFinalizePose) {
                            this.requestInteractiveUpdateAndDraw(true);
                            void this.stopNativePlannerForDrag();
                        }
                    }
                });

                // 觸控?�援
                // ?��??��??��??�觸?��??��?作�??��?誤觸
                // ?�在桌面?��??�觸?�支??
                const isMobileDevice = () => window.innerWidth <= 768;
                
                this.canvas.addEventListener('touchstart', (e) => {
                    if (this.isCanvasInteractionLocked()) {
                        return;
                    }

                    // ?��??��?完全不�??�觸?��?讓�??�正常滾??
                    if (isMobileDevice()) {
                        return; // 不�??��?不�???
                    }
                    
                    // 桌面??(平板?�觸?�螢�?：正常�???
                    if (e.touches.length === 1) {
                        this.visualControl.isDragging = true;
                        this.visualControl.lastMouseX = e.touches[0].clientX;
                        this.visualControl.lastMouseY = e.touches[0].clientY;
                    } else if (e.touches.length === 2) {
                        this.visualControl.isDragging = false;
                        this.visualControl.isPanning = true;
                        const touch1 = e.touches[0];
                        const touch2 = e.touches[1];
                        this.visualControl.lastMouseX = (touch1.clientX + touch2.clientX) / 2;
                        this.visualControl.lastMouseY = (touch1.clientY + touch2.clientY) / 2;
                        e.preventDefault(); // ?��??��??�截
                    }
                }, { passive: false });

                this.canvas.addEventListener('touchmove', (e) => {
                    if (this.isCanvasInteractionLocked()) {
                        return;
                    }

                    // ?��??��?完全不�??�觸??
                    if (isMobileDevice()) {
                        return;
                    }
                    
                    if (e.touches.length === 1 && this.visualControl.isDragging) {
                        e.preventDefault();
                        const touchX = e.touches[0].clientX;
                        const touchY = e.touches[0].clientY;
                        const deltaX = touchX - this.visualControl.lastMouseX;
                        const deltaY = touchY - this.visualControl.lastMouseY;

                        const sensitivity = 0.4;
                        this.visualControl.rotationY -= deltaX * sensitivity;
                        this.visualControl.rotationX -= deltaY * sensitivity;

                        this.visualControl.rotationX = Math.max(-89, Math.min(89, this.visualControl.rotationX));

                        this.visualControl.lastMouseX = touchX;
                        this.visualControl.lastMouseY = touchY;

                        this.requestDraw();
                    } else if (e.touches.length === 2 && this.visualControl.isPanning) {
                        e.preventDefault();
                        // ?��?平移
                        const touch1 = e.touches[0];
                        const touch2 = e.touches[1];
                        const centerX = (touch1.clientX + touch2.clientX) / 2;
                        const centerY = (touch1.clientY + touch2.clientY) / 2;

                        const deltaX = centerX - this.visualControl.lastMouseX;
                        const deltaY = centerY - this.visualControl.lastMouseY;

                        const panSensitivity = 1.0;
                        this.visualControl.panX += deltaX * panSensitivity;
                        this.visualControl.panY += deltaY * panSensitivity;

                        this.visualControl.lastMouseX = centerX;
                        this.visualControl.lastMouseY = centerY;

                        this.requestDraw();
                    }
                }, { passive: false });

                this.canvas.addEventListener('touchend', () => {
                    if (this.isCanvasInteractionLocked()) {
                        this.visualControl.isDragging = false;
                        this.visualControl.isPanning = false;
                        this.refreshCanvasInteractionState();
                        return;
                    }

                    // ?��??��?不�???
                    if (isMobileDevice()) {
                        return;
                    }
                    this.visualControl.isDragging = false;
                    this.visualControl.isPanning = false;
                    this.refreshCanvasInteractionState();
                });
            }

            // 設置�???�學?��?事件??��??
            setupForwardKinematicsEventListeners() {
                // ?��?角度輸入事件
                for (let i = 0; i < 6; i++) {
                    const input = document.getElementById(`inputServoAngle${i}`);
                    if (input) {
                        input.addEventListener('input', (e) => {
                            if (this.isUsingMmfServoAngleSource()) {
                                return;
                            }
                            const displayIndex = i; // UI顯示?��?
                            const paramIndex = this.displayMap[displayIndex]; // 實�??�數索�?
                            this.inputServoAngles[paramIndex] = Number(e.target.value);
                        });
                    }
                }

                // 計�?6DoF姿�??��?
                const calculateBtn = document.getElementById('calculateForwardBtn');
                if (calculateBtn) {
                    calculateBtn.addEventListener('click', async () => {
                        if (this.isUsingMmfServoAngleSource()) {
                            return;
                        }
                        await this.calculateForwardKinematics();
                    });
                }

                // 從逆�??�學結�??�步?��?角度?��?
                const syncBtn = document.getElementById('syncFromResultBtn');
                if (syncBtn) {
                    syncBtn.addEventListener('click', async () => {
                        if (this.isUsingMmfServoAngleSource()) {
                            return;
                        }
                        if (this.result.servoAngles && this.result.servoAngles.length === 6) {
                            // 從逆�??�學結�?復制角度?�正?��?學輸??
                            for (let i = 0; i < 6; i++) {
                                this.inputServoAngles[i] = this.result.servoAngles[i];
                            }
                            this.syncForwardKinematicsUI();
                            // ?��?計�?�???�學
                            await this.calculateForwardKinematics();
                        }
                    });
                }


            }

            // ?�步�???�學UI顯示
            syncForwardKinematicsUI() {
                const activeServoAngles = this.getActiveForwardServoAngles();
                for (let i = 0; i < 6; i++) {
                    const displayIndex = i;
                    const paramIndex = this.displayMap[displayIndex];
                    const input = document.getElementById(`inputServoAngle${i}`);
                    if (input) {
                        input.value = activeServoAngles[paramIndex].toFixed(7);
                    }
                }
            }

            // ?�新�???�學計�?結�?顯示（�???null 檢查以避?�在缺�??��??��??��?
            updateForwardDisplay() {
                const resultSection = document.getElementById('forwardResultSection');
                const resultTitle = document.getElementById('forwardResultTitle');
                const errorList = document.getElementById('forwardErrorList');

                // ?�主要顯示�?段�?標�??��?存在，�?沒�?必�?繼�?
                if (!resultSection && !resultTitle && !errorList) return;

                // ?�新標�??��?塊樣式�??��?素�??��??�新�?
                if (resultTitle) {
                    // 使用科學計數法顯示極小誤�?
                    let errorStr = '';
                    if (this.forwardResult.totalError !== undefined) {
                        const err = this.forwardResult.totalError;
                        if (err < 1e-9) {
                            errorStr = ` (誤差: ${err.toExponential(3)})`;
                        } else {
                            errorStr = ` (誤差: ${err.toFixed(9)}mm)`;
                        }
                    }
                    resultTitle.innerHTML = `6DoF姿�?輸出 (�???�學) <span class="status-icon ${this.forwardResult.valid ? 'status-ok' : 'status-error'}">${this.forwardResult.valid ? '?? : '??}</span> ${errorStr}`;
                }
                if (resultSection) {
                    resultSection.className = `section section-compact ${this.forwardResult.valid ? 'section-green' : 'section-red'}`;
                }

                // ?�助?��?：�??�設�?input ??
                const safeSetValue = (id, text) => {
                    const el = document.getElementById(id);
                    if (el) el.value = text;
                };

                // ?�新姿�?結�?顯示（�?就更?��??�就跳�?�?
                if (this.forwardResult.valid) {
                    const pose = this.forwardResult.pose;
                    const platformPose = this.forwardResult.platformPose || { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };
                    const initialHeight = Number(this.forwardResult.initialHeight) || this.initialHeight;
                    
                    // ?��??��?座�?系顯�?XYZ ??
                    if (this.coordinateSystem === 'world') {
                        // 世�?座�?系�??�接顯示世�?座�?
                        safeSetValue('calcPoseX', pose.x.toFixed(3));
                        safeSetValue('calcPoseY', pose.y.toFixed(3));
                        safeSetValue('calcPoseZ', (pose.z - initialHeight).toFixed(3));
                    } else {
                        safeSetValue('calcPoseX', Number(platformPose.x || 0).toFixed(3));
                        safeSetValue('calcPoseY', Number(platformPose.y || 0).toFixed(3));
                        safeSetValue('calcPoseZ', Number(platformPose.z || 0).toFixed(3));
                    }
                    
                    // Roll, Pitch, Yaw ?�種座�?系都一�?
                    safeSetValue('calcPoseRoll', pose.roll.toFixed(7));
                    safeSetValue('calcPosePitch', pose.pitch.toFixed(7));
                    safeSetValue('calcPoseYaw', pose.yaw.toFixed(7));
                } else {
                    ['calcPoseX','calcPoseY','calcPoseZ','calcPoseRoll','calcPosePitch','calcPoseYaw']
                        .forEach(id => safeSetValue(id, '---'));
                }

                // ?�新?�誤?�表（若?��?存在?��?�?style/innerHTML�?
                if (errorList) {
                    const messages = [
                        ...(Array.isArray(this.forwardResult.errors) ? this.forwardResult.errors : []),
                        ...(Array.isArray(this.forwardResult.warnings) ? this.forwardResult.warnings : [])
                    ];
                    if (messages.length > 0) {
                        errorList.style.display = 'block';
                        errorList.innerHTML = messages.map(error => `<div>${error}</div>`).join('');
                    } else {
                        errorList.style.display = 'none';
                    }
                }
            }

            setView(newView) {
                this.view = newView;

                // ?��?視�?類�?設置標�??��?轉�?�?
                switch (newView) {
                    case 'iso':
                        // 後俯視�?：�?後方俯�?30度�?角度觀??
                        this.visualControl.rotationX = -45;
                        this.visualControl.rotationY = 0;
                        this.visualControl.rotationZ = 0;
                        // 不改�?scale：�??�使?�者目?��?縮放設�?，�??�置平移
                        this.visualControl.panX = 0;
                        this.visualControl.panY = 0;
                        break;
                    case 'side':
                        // ?��???- �?���?��視�?不使?��?外全局?��?�?
                        // ?�們使??project ?��?射�?顯示 Y-Z 平面，�?以�??��?轉為 0
                        this.visualControl.rotationX = 0;
                        this.visualControl.rotationY = 0;
                        this.visualControl.rotationZ = 0;
                        this.visualControl.panX = 0;
                        this.visualControl.panY = 0;
                        break;
                    case 'top':
                        // 俯�???- 使用 project ?��?顯示 X-Y 平面，�??��?轉為 0
                        this.visualControl.rotationX = 0;
                        this.visualControl.rotationY = 0;
                        this.visualControl.rotationZ = 0;
                        this.visualControl.panX = 0;
                        this.visualControl.panY = 0;
                        break;
                }

                // ?�新?��??�??
                document.querySelectorAll('.button-group .button').forEach(btn => {
                    btn.className = 'button button-secondary';
                });

                const viewButtons = {
                    'iso': 'viewIso',
                    'side': 'viewSide',
                    'top': 'viewTop'
                };

                document.getElementById(viewButtons[newView]).className = 'button button-primary';
                // ?��?視�??��??�繪，�??�改變平??pose，�?此�??�新計�?
                this.requestDraw();
            }

            requestDraw() {
                if (this.drawFramePending) {
                    return;
                }

                this.drawFramePending = true;
                requestAnimationFrame(() => {
                    this.drawFramePending = false;
                    this.draw();
                });
            }

            requestInteractiveUpdateAndDraw(force = false) {
                this.interactiveUpdatePending = true;

                if (force && this.interactiveUpdateTimer) {
                    clearTimeout(this.interactiveUpdateTimer);
                    this.interactiveUpdateTimer = null;
                    this.interactiveUpdateScheduled = false;
                }

                if (this.interactiveUpdateScheduled) {
                    return;
                }

                const now = performance.now();
                const remainingDelay = force
                    ? 0
                    : Math.max(0, this.interactiveUpdateMinIntervalMs - (now - this.interactiveUpdateLastStartedAt));

                this.interactiveUpdateScheduled = true;
                const scheduleFlush = () => {
                    this.interactiveUpdateScheduled = false;
                    this.interactiveUpdateTimer = null;
                    void this.flushInteractiveUpdateAndDraw();
                };

                if (remainingDelay === 0) {
                    scheduleFlush();
                } else {
                    this.interactiveUpdateTimer = setTimeout(scheduleFlush, remainingDelay);
                }
            }

            cancelForwardInteractiveUpdate() {
                this.forwardInteractivePending = false;

                if (this.forwardInteractiveTimer) {
                    clearTimeout(this.forwardInteractiveTimer);
                    this.forwardInteractiveTimer = null;
                }

                this.forwardInteractiveScheduled = false;

                if (this.forwardAbortController) {
                    this.forwardAbortController.abort();
                    this.forwardAbortController = null;
                }
            }

            requestForwardInteractiveUpdate(force = false) {
                this.forwardInteractivePending = true;

                if (force && this.forwardInteractiveTimer) {
                    clearTimeout(this.forwardInteractiveTimer);
                    this.forwardInteractiveTimer = null;
                    this.forwardInteractiveScheduled = false;
                }

                if (this.forwardInteractiveScheduled) {
                    return;
                }

                const now = performance.now();
                const remainingDelay = force
                    ? 0
                    : Math.max(0, this.forwardInteractiveMinIntervalMs - (now - this.forwardInteractiveLastStartedAt));

                this.forwardInteractiveScheduled = true;
                const scheduleFlush = () => {
                    this.forwardInteractiveScheduled = false;
                    this.forwardInteractiveTimer = null;
                    void this.flushForwardInteractiveUpdate();
                };

                if (remainingDelay === 0) {
                    scheduleFlush();
                } else {
                    this.forwardInteractiveTimer = setTimeout(scheduleFlush, remainingDelay);
                }
            }

            isPoseDragPreviewActive() {
                return this.visualControl.isDragging
                    && typeof this.visualControl.dragAction === 'string'
                    && this.visualControl.dragAction.startsWith('pose');
            }

            transformLocalPointToWorld(point, pose) {
                const rot = this.getRotationMatrix(pose.roll, pose.pitch, pose.yaw);
                return [
                    rot[0][0] * point[0] + rot[0][1] * point[1] + rot[0][2] * point[2] + pose.x,
                    rot[1][0] * point[0] + rot[1][1] * point[1] + rot[1][2] * point[2] + pose.y,
                    rot[2][0] * point[0] + rot[2][1] * point[1] + rot[2][2] * point[2] + pose.z
                ];
            }

            hasRenderableGeometry(result) {
                return !!result
                    && Array.isArray(result.armEndPoints)
                    && Array.isArray(result.platformPoints)
                    && result.armEndPoints.length >= this.previewRenderMinArmCount
                    && result.platformPoints.length >= this.previewRenderMinArmCount;
            }

            cloneRenderResult(result) {
                if (!result) {
                    return null;
                }

                return {
                    ...result,
                    servoAngles: Array.isArray(result.servoAngles) ? [...result.servoAngles] : [0, 0, 0, 0, 0, 0],
                    armEndPoints: this.normalizePointArray(result.armEndPoints),
                    platformPoints: this.normalizePointArray(result.platformPoints),
                    basePoints: this.normalizePointArray(result.basePoints),
                    rodLengths: Array.isArray(result.rodLengths) ? [...result.rodLengths] : [0, 0, 0, 0, 0, 0],
                    fisheyeAngles: Array.isArray(result.fisheyeAngles) ? [...result.fisheyeAngles] : [0, 0, 0, 0, 0, 0],
                    errors: Array.isArray(result.errors) ? [...result.errors] : [],
                    warnings: Array.isArray(result.warnings) ? [...result.warnings] : [],
                    singularArms: Array.isArray(result.singularArms) ? [...result.singularArms] : [],
                    singularityInfo: result.singularityInfo ? { ...result.singularityInfo } : {},
                    errorArms: Array.isArray(result.errorArms) ? [...result.errorArms] : [],
                    warningArms: Array.isArray(result.warningArms) ? [...result.warningArms] : [],
                    fisheyeWarningArms: Array.isArray(result.fisheyeWarningArms) ? [...result.fisheyeWarningArms] : []
                };
            }

            rememberRenderableResult(result) {
                if (!this.hasRenderableGeometry(result)) {
                    return;
                }

                this.lastRenderableResult = this.cloneRenderResult(result);
            }

            buildPreviewRenderResult() {
                const baseResult = this.hasRenderableGeometry(this.result)
                    ? this.result
                    : (this.lastRenderableResult || this.result);

                if (!this.isPoseDragPreviewActive()) {
                    return {
                        ...baseResult,
                        valid: this.result.valid,
                        errors: this.result.errors,
                        warnings: this.result.warnings,
                        singularArms: this.result.singularArms,
                        singularityInfo: this.result.singularityInfo,
                        errorArms: this.result.errorArms,
                        warningArms: this.result.warningArms,
                        fisheyeWarningArms: this.result.fisheyeWarningArms
                    };
                }

                const armEndPoints = this.hasRenderableGeometry(baseResult)
                    ? baseResult.armEndPoints
                    : [];

                return {
                    ...baseResult,
                    armEndPoints,
                    platformPoints: this.normalizePointArray(baseResult.platformPoints),
                    valid: this.result.valid,
                    errors: this.result.errors,
                    warnings: this.result.warnings,
                    singularArms: this.result.singularArms,
                    singularityInfo: this.result.singularityInfo,
                    errorArms: this.result.errorArms,
                    warningArms: this.result.warningArms,
                    fisheyeWarningArms: this.result.fisheyeWarningArms,
                    previewPending: this.interactiveUpdatePending || this.interactiveUpdateInFlightCount > 0
                };
            }

            async flushInteractiveUpdateAndDraw() {
                if (!this.interactiveUpdatePending) {
                    return;
                }

                this.interactiveUpdatePending = false;
                this.interactiveUpdateLastStartedAt = performance.now();
                this.interactiveUpdateInFlightCount += 1;

                // 立即使用?�端?�地 IK 計�?並繪?��?不�? API
                try {
                    const worldPose = this.resolveWorldPoseFromInput();
                    const localResult = this.calculateLocalInverse(worldPose);
                    if (localResult.armEndPoints.length === 6 && localResult.platformPoints.length === 6) {
                        this.pose = worldPose;
                        this.initialHeight = this.calculateLocalInitialHeight();
                        this.result = localResult;
                        this.rememberRenderableResult(this.result);
                        this.draw();
                        this.updateDisplay();
                    }
                } catch { /* ?�地計�?失�??��??�??API */ }

                // ?�景?��?API 以�?得�??�診?��?奇異?�、警?��?�?
                void this.calculate({ interactive: true })
                    .then(() => {
                        this.queueNativePlannerTargetSync();
                        this.draw();
                        this.updateDisplay();
                    })
                    .finally(() => {
                        this.interactiveUpdateInFlightCount = Math.max(0, this.interactiveUpdateInFlightCount - 1);
                        if (this.interactiveUpdatePending) {
                            this.requestInteractiveUpdateAndDraw();
                        }
                    });
            }

                async flushForwardInteractiveUpdate() {
                    if (!this.forwardInteractivePending) {
                        return;
                    }

                    this.forwardInteractivePending = false;
                    this.forwardInteractiveLastStartedAt = performance.now();

                    void this.calculateForwardKinematics({ interactive: true });
                }

            async updateAndDraw() {
                await this.calculate({ interactive: false });
                this.draw();
                this.updateDisplay();
            }

            beginParameterPresetMutation() {
                const generation = ++this.parameterPresetGeneration;
                this.interactiveUpdatePending = false;
                this.interactiveUpdateScheduled = false;
                if (this.interactiveUpdateTimer) {
                    clearTimeout(this.interactiveUpdateTimer);
                    this.interactiveUpdateTimer = null;
                }
                if (this.inverseAbortController) {
                    this.inverseAbortController.abort();
                    this.inverseAbortController = null;
                }
                this.lastRenderableResult = null;
                return generation;
            }

            isCurrentParameterPresetMutation(generation) {
                return generation === this.parameterPresetGeneration;
            }

            async updateAndDrawForParameterPreset(generation) {
                if (!this.isCurrentParameterPresetMutation(generation)) {
                    return false;
                }

                try {
                    const worldPose = this.resolveWorldPoseFromInput();
                    const localResult = this.calculateLocalInverse(worldPose);
                    if (localResult.armEndPoints.length === 6 && localResult.platformPoints.length === 6) {
                        this.pose = worldPose;
                        this.initialHeight = this.calculateLocalInitialHeight();
                        this.result = localResult;
                        this.rememberRenderableResult(this.result);
                        this.draw();
                        this.updateDisplay();
                    }
                } catch { /* API calculation below will surface any persistent issue */ }

                await this.calculate({ interactive: false });
                if (!this.isCurrentParameterPresetMutation(generation)) {
                    return false;
                }

                this.draw();
                this.updateDisplay();
                return true;
            }

            // ?�新UI顯示?��?從pose?�步?�輸?��?�?
            updateUIFromPose() {
                this.updatePoseInputsForCoordSystem();
            }

            // ?��??��?座�?系更?�XYZ輸入框�?顯示??
            updatePoseInputsForCoordSystem() {
                if (this.isUsingMmfPoseSource()) {
                    const activePose = this.normalizePoseValues(this.mmfPoseInput);
                    document.getElementById('poseX').value = activePose.x.toFixed(3);
                    document.getElementById('poseY').value = activePose.y.toFixed(3);
                    document.getElementById('poseZ').value = activePose.z.toFixed(3);
                    document.getElementById('poseRoll').value = activePose.roll.toFixed(3);
                    document.getElementById('posePitch').value = activePose.pitch.toFixed(3);
                    document.getElementById('poseYaw').value = activePose.yaw.toFixed(3);
                    return;
                }

                this.syncPoseInputFromResolvedPose();
                document.getElementById('poseX').value = this.poseInput.x.toFixed(3);
                document.getElementById('poseY').value = this.poseInput.y.toFixed(3);
                document.getElementById('poseZ').value = this.poseInput.z.toFixed(3);
                document.getElementById('poseRoll').value = this.poseInput.roll.toFixed(3);
                document.getElementById('posePitch').value = this.poseInput.pitch.toFixed(3);
                document.getElementById('poseYaw').value = this.poseInput.yaw.toFixed(3);
            }

            // ?�座??���?(?��?位置) - 對稱?��?�?
            getBasePoints() {
                return this.calculateSymmetricHexagonPoints(
                    this.params.baseEdgeA,
                    this.params.baseEdgeB,
                    this.params.baseHeight
                );
            }

            // 平台??���?(局?��?�? - 對稱?��?�?
            getPlatformPointsLocal() {
                return this.calculateSymmetricHexagonPoints(
                    this.params.platformEdgeA,
                    this.params.platformEdgeB,
                    0
                );
            }

            // ?�端?�地?��??�學：直?�在 JS 中�?算幾何�?位�?伺�?角度，跳??API 延遲
            calculateLocalInverse(pose) {
                const p = this.params;
                const pm = Array.isArray(p.servoPointMap) ? p.servoPointMap : [0,1,2,3,4,5];
                const basePoints = this.getBasePoints();
                const platformLocal = this.getPlatformPointsLocal();
                const rot = this.getRotationMatrix(pose.roll, pose.pitch, pose.yaw);

                const platformPoints = platformLocal.map(pt => [
                    rot[0][0]*pt[0] + rot[0][1]*pt[1] + rot[0][2]*pt[2] + pose.x,
                    rot[1][0]*pt[0] + rot[1][1]*pt[1] + rot[1][2]*pt[2] + pose.y,
                    rot[2][0]*pt[0] + rot[2][1]*pt[1] + rot[2][2]*pt[2] + pose.z
                ]);

                const servoAngles = new Array(6);
                const armEndPoints = new Array(6);
                const rodLengths = new Array(6);
                const errors = [];
                let valid = true;
                const a = p.servoArmLength;
                const b = p.rodLength;
                const axAngles = Array.isArray(p.servoAxisAngle) ? p.servoAxisAngle : [0,0,0,0,0,0];

                for (let i = 0; i < 6; i++) {
                    const pi = pm[i];
                    const bp = basePoints[pi];
                    const pp = platformPoints[pi];
                    const tx = pp[0] - bp[0];
                    const ty = pp[1] - bp[1];
                    const tz = pp[2] - bp[2];
                    const c = Math.hypot(tx, ty, tz);

                    const maxReach = a + b;
                    const minReach = Math.abs(b - a);
                    if (c > maxReach || c < minReach) {
                        errors.push(`??{i+1}: ?��?距離 ${c.toFixed(2)} mm 超出?��?範�?`);
                        valid = false;
                        servoAngles[i] = 0;
                        armEndPoints[i] = [bp[0], bp[1], bp[2]];
                        rodLengths[i] = 0;
                        continue;
                    }

                    const radialAngle = axAngles[i] * Math.PI / 180;
                    const radialDirX = Math.cos(radialAngle);
                    const radialDirY = Math.sin(radialAngle);
                    const dotRadial = tx * radialDirX + ty * radialDirY;
                    const targetAngleInPlane = Math.atan2(tz, dotRadial);

                    const cosAlpha = Math.max(-1, Math.min(1, (a*a + c*c - b*b) / (2 * a * c)));
                    const alpha = Math.acos(cosAlpha);
                    let armAngle = targetAngleInPlane - alpha;

                    // Newton refinement
                    for (let it = 0; it < 20; it++) {
                        const ex = pp[0] - (bp[0] + a * Math.cos(radialAngle) * Math.cos(armAngle));
                        const ey = pp[1] - (bp[1] + a * Math.sin(radialAngle) * Math.cos(armAngle));
                        const ez = pp[2] - (bp[2] + a * Math.sin(armAngle));
                        const curLen = Math.hypot(ex, ey, ez);
                        const err = curLen - b;
                        if (Math.abs(err) < 1e-10) break;
                        const delta = 1e-7;
                        const test = armAngle + delta;
                        const tx2 = pp[0] - (bp[0] + a * Math.cos(radialAngle) * Math.cos(test));
                        const ty2 = pp[1] - (bp[1] + a * Math.sin(radialAngle) * Math.cos(test));
                        const tz2 = pp[2] - (bp[2] + a * Math.sin(test));
                        const len2 = Math.hypot(tx2, ty2, tz2);
                        const grad = (len2 - curLen) / delta;
                        if (Math.abs(grad) > 1e-10) {
                            armAngle -= err / grad;
                        } else {
                            break;
                        }
                    }

                    const ax = bp[0] + a * Math.cos(radialAngle) * Math.cos(armAngle);
                    const ay = bp[1] + a * Math.sin(radialAngle) * Math.cos(armAngle);
                    const az = bp[2] + a * Math.sin(armAngle);
                    let angleDeg = armAngle * 180 / Math.PI;
                    while (angleDeg > 180) angleDeg -= 360;
                    while (angleDeg < -180) angleDeg += 360;

                    if (Math.abs(angleDeg) > p.servoAngleLimit) {
                        errors.push(`??{i+1}: ?��?角度 ${angleDeg.toFixed(2)}° 超出 ±${p.servoAngleLimit}° ?�制`);
                        valid = false;
                    }

                    servoAngles[i] = angleDeg;
                    armEndPoints[i] = [ax, ay, az];
                    const dx = pp[0] - ax, dy = pp[1] - ay, dz = pp[2] - az;
                    rodLengths[i] = Math.hypot(dx, dy, dz);
                }

                const fisheyeDiag = this.computeStewartStreamFisheyeDiagnostics(basePoints, armEndPoints, platformPoints);

                return {
                    basePoints, platformPoints, armEndPoints, servoAngles, rodLengths,
                    servoPointMap: pm,
                    valid: valid && fisheyeDiag.fisheyeWarningArms.length === 0,
                    errors: [...errors, ...fisheyeDiag.warnings],
                    warnings: fisheyeDiag.warnings,
                    singularArms: [], singularityInfo: {},
                    errorArms: valid ? [] : errors.map((_, idx) => idx),
                    warningArms: fisheyeDiag.warningArms,
                    fisheyeWarningArms: fisheyeDiag.fisheyeWarningArms,
                    fisheyeAngles: fisheyeDiag.fisheyeAngles
                };
            }

            // ?�端?�地計�??��?高度（匹?��?�?CalculateInitialHeight�?
            calculateLocalInitialHeight() {
                const p = this.params;
                const pm = Array.isArray(p.servoPointMap) ? p.servoPointMap : [0,1,2,3,4,5];
                const basePoints = this.getBasePoints();
                const platformLocal = this.getPlatformPointsLocal();
                const pi = pm[0];
                const bp = basePoints[pi];
                const pp = platformLocal[pi];
                const axisRad = (Array.isArray(p.servoAxisAngle) ? p.servoAxisAngle[0] : 0) * Math.PI / 180;
                const servoRad = (p.servoInitialAngle || 15) * Math.PI / 180;
                const ax = bp[0] + p.servoArmLength * Math.cos(axisRad) * Math.cos(servoRad);
                const ay = bp[1] + p.servoArmLength * Math.sin(axisRad) * Math.cos(servoRad);
                const az = bp[2] + p.servoArmLength * Math.sin(servoRad);
                const dx = pp[0] - ax;
                const dy = pp[1] - ay;
                const hDistSq = dx * dx + dy * dy;
                const remainSq = p.rodLength * p.rodLength - hDistSq;
                return remainSq < 0 ? 450 : az + Math.sqrt(remainSq);
            }

            // 從當?�輸?�解?��??�姿?��??��?後端 ResolveStewartPoseInput�?
            resolveWorldPoseFromInput() {
                const input = this.isUsingMmfPoseSource()
                    ? this.normalizePoseValues(this.mmfPoseInput)
                    : this.normalizePoseValues(this.poseInput);
                const h = this.calculateLocalInitialHeight();
                const roll = input.roll;
                const pitch = input.pitch;
                const yaw = input.yaw;

                if (this.coordinateSystem === 'platform') {
                    const rot = this.getRotationMatrix(roll, pitch, yaw);
                    const wx = rot[0][0]*input.x + rot[0][1]*input.y + rot[0][2]*input.z;
                    const wy = rot[1][0]*input.x + rot[1][1]*input.y + rot[1][2]*input.z;
                    const wz = rot[2][0]*input.x + rot[2][1]*input.y + rot[2][2]*input.z;
                    return { x: wx, y: wy, z: h + wz, roll, pitch, yaw };
                }

                return { x: input.x, y: input.y, z: h + input.z, roll, pitch, yaw };
            }

            // ?��??�陣
            getRotationMatrix(roll, pitch, yaw) {
                const cr = Math.cos(roll * Math.PI / 180);
                const sr = Math.sin(roll * Math.PI / 180);
                const cp = Math.cos(pitch * Math.PI / 180);
                const sp = Math.sin(pitch * Math.PI / 180);
                const cy = Math.cos(yaw * Math.PI / 180);
                const sy = Math.sin(yaw * Math.PI / 180);

                return [
                    [cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr],
                    [sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr],
                                       [-sp, cp * sr, cp * cr]
                ];
            }

            normalizePlannerPose(pose) {
                return this.normalizePoseValues(pose);
            }

            buildStewartApiParams() {
                return {
                    platformEdgeA: this.params.platformEdgeA,
                    platformEdgeB: this.params.platformEdgeB,
                    baseEdgeA: this.params.baseEdgeA,
                    baseEdgeB: this.params.baseEdgeB,
                    baseHeight: this.params.baseHeight,
                    rodLength: this.params.rodLength,
                    servoArmLength: this.params.servoArmLength,
                    servoAxisAngle: Array.isArray(this.params.servoAxisAngle)
                        ? this.params.servoAxisAngle.map(value => Number(value) || 0)
                        : [0, 0, 0, 0, 0, 0],
                    servoPointMap: Array.isArray(this.params.servoPointMap)
                        ? this.params.servoPointMap.map(value => Number(value) || 0)
                        : [0, 1, 2, 3, 4, 5],
                    servoAngleLimit: this.params.servoAngleLimit,
                    fisheyeAngleLimit: this.params.fisheyeAngleLimit || 30,
                    servoInitialAngle: this.params.servoInitialAngle || 15,
                    singularityVerticalAngle: this.params.singularityVerticalAngle || 18,
                    singularityHorizontalAngle: this.params.singularityHorizontalAngle || 87,
                    singularityExtendRatio: this.params.singularityExtendRatio || 95,
                    singularityContractRatio: this.params.singularityContractRatio || 105
                };
            }

            buildPoseFieldPayload(prefix, pose) {
                const normalizedPose = this.normalizePoseValues(pose);
                return {
                    [`${prefix}X`]: normalizedPose.x,
                    [`${prefix}Y`]: normalizedPose.y,
                    [`${prefix}Z`]: normalizedPose.z,
                    [`${prefix}Roll`]: normalizedPose.roll,
                    [`${prefix}Pitch`]: normalizedPose.pitch,
                    [`${prefix}Yaw`]: normalizedPose.yaw
                };
            }

            buildInversePayload() {
                const activePoseInput = this.isUsingMmfPoseSource()
                    ? this.mmfPoseInput
                    : this.poseInput;

                return {
                    ...this.buildStewartApiParams(),
                    inputCoordinateSystem: this.coordinateSystem,
                    ...this.buildPoseFieldPayload('inputPose', activePoseInput)
                };
            }

            buildForwardPayload() {
                const activeServoAngles = this.getActiveForwardServoAngles();
                return {
                    ...this.buildStewartApiParams(),
                    ...this.buildPoseFieldPayload('initialPose', this.pose),
                    inputServoAngles: activeServoAngles
                };
            }

            buildWorldPosePayload(poseOverride = this.pose) {
                return {
                    ...this.buildStewartApiParams(),
                    ...this.buildPoseFieldPayload('pose', poseOverride)
                };
            }

            updateNativePlannerState(response) {
                if (!response || typeof response !== 'object') {
                    return;
                }

                const hasNativeState = response.type === 'stewartNative'
                    || Object.prototype.hasOwnProperty.call(response, 'initialized')
                    || Object.prototype.hasOwnProperty.call(response, 'moving')
                    || Object.prototype.hasOwnProperty.call(response, 'seq')
                    || Object.prototype.hasOwnProperty.call(response, 'cycleCounter')
                    || Object.prototype.hasOwnProperty.call(response, 'updatePeriodUs')
                    || Object.prototype.hasOwnProperty.call(response, 'currentPose')
                    || Object.prototype.hasOwnProperty.call(response, 'targetPose');

                if (!hasNativeState) {
                    return;
                }

                this.nativePlannerState = {
                    ...this.nativePlannerState,
                    available: true,
                    initialized: !!response.initialized,
                    moving: !!response.moving,
                    valid: Object.prototype.hasOwnProperty.call(response, 'valid')
                        ? !!response.valid
                        : this.nativePlannerState.valid,
                    seq: Number(response.seq) || 0,
                    cycleCounter: Number(response.cycleCounter) || 0,
                    updatePeriodUs: Number(response.updatePeriodUs) || 0,
                    linearSpeedMmPerSec: Number(response.linearSpeedMmPerSec) || 0,
                    angularSpeedDegPerSec: Number(response.angularSpeedDegPerSec) || 0,
                    currentPose: this.normalizePlannerPose(response.currentPose),
                    targetPose: this.normalizePlannerPose(response.targetPose),
                    errors: Array.isArray(response.errors) ? response.errors.map(String) : []
                };

                if (this.nativePlannerState.linearSpeedMmPerSec > 0) {
                    this.nativePlannerConfig.linearSpeedMmPerSec = this.nativePlannerState.linearSpeedMmPerSec;
                }
                if (this.nativePlannerState.angularSpeedDegPerSec > 0) {
                    this.nativePlannerConfig.angularSpeedDegPerSec = this.nativePlannerState.angularSpeedDegPerSec;
                }
                this.syncNativePlannerSpeedInputs();
            }

            formatPlannerPose(pose) {
                const normalizedPose = this.normalizePlannerPose(pose);
                const relativeZ = normalizedPose.z - this.initialHeight;
                return `X${normalizedPose.x.toFixed(1)} Y${normalizedPose.y.toFixed(1)} Z${relativeZ.toFixed(1)} | R${normalizedPose.roll.toFixed(1)} P${normalizedPose.pitch.toFixed(1)} Y${normalizedPose.yaw.toFixed(1)}`;
            }

            updateNativePlannerDisplay() {
                const lamp = document.getElementById('nativePlannerLamp');
                const summary = document.getElementById('nativePlannerSummary');
                const streamState = document.getElementById('nativePlannerStreamState');
                const motionState = document.getElementById('nativePlannerMotionState');
                const validity = document.getElementById('nativePlannerValidity');
                const seq = document.getElementById('nativePlannerSeq');
                const cycle = document.getElementById('nativePlannerCycle');
                const speed = document.getElementById('nativePlannerSpeed');
                const currentPose = document.getElementById('nativePlannerCurrentPose');
                const targetPose = document.getElementById('nativePlannerTargetPose');
                const errorList = document.getElementById('nativePlannerErrorList');

                if (!lamp || !summary || !streamState || !motionState || !validity || !seq || !cycle || !speed || !currentPose || !targetPose || !errorList) {
                    return;
                }

                const state = this.nativePlannerState;
                const plannerAvailable = !!state.available;
                const plannerInitialized = plannerAvailable && !!state.initialized;
                let lampClass = 'planner-lamp-offline';
                let summaryText = 'Planner ?��?�?;

                if (plannerAvailable) {
                    if (!plannerInitialized) {
                        lampClass = 'planner-lamp-ready';
                        summaryText = '已�??，未?��???;
                    } else if (!state.valid) {
                        lampClass = 'planner-lamp-error';
                        summaryText = plannerInitialized
                            ? `IK ?��? #${state.seq}`
                            : '?�就�?;
                    } else if (state.moving) {
                        lampClass = 'planner-lamp-moving';
                        summaryText = `移�?�?#${state.seq}`;
                    } else if (plannerInitialized) {
                        lampClass = 'planner-lamp-ready';
                        summaryText = `就�? #${state.seq}`;
                    }
                } else if (this.stewartInverseStreamConnected) {
                    summaryText = 'SSE 線�?，�??�??;
                }

                lamp.className = `planner-lamp ${lampClass}`;
                summary.textContent = summaryText;
                streamState.textContent = this.stewartInverseStreamConnected ? '線�?' : '?��?';
                motionState.textContent = !plannerAvailable
                    ? '?��?�?
                    : (state.moving ? '移�?�? : (plannerInitialized ? '待命' : '?��?始�?'));
                validity.textContent = !plannerAvailable
                    ? '?�知'
                    : (plannerInitialized ? (state.valid ? '?��?' : '?��?') : '?��?始�?');
                seq.textContent = plannerInitialized ? String(state.seq) : '-';
                cycle.textContent = plannerAvailable && state.updatePeriodUs > 0
                    ? `${state.updatePeriodUs}us · ${state.cycleCounter}`
                    : '-';
                speed.textContent = plannerAvailable
                    ? `${state.linearSpeedMmPerSec.toFixed(0)} mm/s · ${state.angularSpeedDegPerSec.toFixed(0)} deg/s`
                    : `${this.nativePlannerConfig.linearSpeedMmPerSec.toFixed(0)} mm/s · ${this.nativePlannerConfig.angularSpeedDegPerSec.toFixed(0)} deg/s`;
                currentPose.textContent = `?��? ${plannerInitialized ? this.formatPlannerPose(state.currentPose) : '-'}`;
                targetPose.textContent = `?��? ${plannerInitialized ? this.formatPlannerPose(state.targetPose) : '-'}`;

                if (state.errors.length > 0) {
                    errorList.style.display = 'block';
                    errorList.innerHTML = state.errors.map(error => `<div>${error}</div>`).join('');
                } else {
                    errorList.style.display = 'none';
                    errorList.innerHTML = '';
                }
            }

            setupStewartInverseStream() {
                if (!window.EventSource || this.stewartInverseEventSource) {
                    return;
                }

                try {
                    this.stewartInverseEventSource = new EventSource('/api/stewart/sse');
                    this.stewartInverseEventSource.onopen = () => {
                        this.stewartInverseStreamConnected = true;
                        this.updateNativePlannerDisplay();
                        void this.refreshNativePlannerStatus({ silent: true });
                    };
                    this.stewartInverseEventSource.onerror = () => {
                        this.cancelPendingStewartInverseResponses('Stewart SSE disconnected');
                        this.cancelPendingStewartForwardResponses('Stewart SSE disconnected');
                        this.stewartInverseStreamConnected = false;
                        this.updateNativePlannerDisplay();
                    };
                    this.stewartInverseEventSource.onmessage = (event) => {
                        let data = null;
                        try {
                            data = JSON.parse(event.data);
                        } catch {
                            return;
                        }

                        if (!data) {
                            return;
                        }

                        if (data.type === 'stewartNative') {
                            this.updateNativePlannerState(data);
                            this.updateNativePlannerDisplay();
                            return;
                        }

                        if (data.type === 'stewartInverse') {
                            const pending = this.pendingStewartInverseResponses.get(Number(data.seq));
                            if (!pending) {
                                return;
                            }

                            clearTimeout(pending.timeoutId);
                            this.pendingStewartInverseResponses.delete(Number(data.seq));
                            pending.resolve(data);
                            return;
                        }

                        if (data.type !== 'stewartForward') {
                            return;
                        }

                        const pending = this.pendingStewartForwardResponses.get(Number(data.seq));
                        if (!pending) {
                            return;
                        }

                        clearTimeout(pending.timeoutId);
                        this.pendingStewartForwardResponses.delete(Number(data.seq));
                        pending.resolve(data);
                    };
                } catch {
                    this.cancelPendingStewartInverseResponses('Stewart SSE unavailable');
                    this.cancelPendingStewartForwardResponses('Stewart SSE unavailable');
                    this.stewartInverseEventSource = null;
                    this.stewartInverseStreamConnected = false;
                    this.updateNativePlannerDisplay();
                }
            }

            setupPdoInputStream() {
                if (!window.EventSource || this.pdoEventSource) {
                    return;
                }

                try {
                    this.pdoEventSource = new EventSource('/pdo_sse');
                    this.pdoEventSource.onopen = () => {
                        this.pdoStreamConnected = true;
                    };
                    this.pdoEventSource.onerror = () => {
                        this.pdoStreamConnected = false;
                    };
                    this.pdoEventSource.onmessage = (event) => {
                        let data = null;
                        try {
                            data = JSON.parse(event.data);
                        } catch {
                            return;
                        }

                        if (!data || data.type) {
                            return;
                        }

                        this.handlePdoInputPayload(data);
                    };
                } catch {
                    this.pdoEventSource = null;
                    this.pdoStreamConnected = false;
                }
            }

            async refreshPdoInputStatus(options = {}) {
                const { silent = false } = options;

                try {
                    const response = await this.getStewartApi('/pdo_data', { retryCount: 0 });
                    this.handlePdoInputPayload(response);
                    return response;
                } catch (error) {
                    if (!silent) {
                        console.warn('MMF1 6DoF ?�?��??�失??', error);
                    }
                    return null;
                }
            }

            async refreshMmf1InputModeConfig(options = {}) {
                const { silent = false } = options;

                try {
                    const response = await this.getStewartApi('/api/mmf/input_mode', { retryCount: 0 });
                    this.applyMmf1InputModeConfigResponse(response);
                    return response;
                } catch (error) {
                    this.mmf1InputModeConfigLoaded = false;
                    this.syncMmf1InputModeConfigUi();
                    if (!silent) {
                        console.warn('MMF1 輸入模�?設�?讀?�失??', error);
                    }
                    return null;
                }
            }

            createAbortError(message = 'operation aborted') {
                if (typeof DOMException === 'function') {
                    return new DOMException(message, 'AbortError');
                }

                const error = new Error(message);
                error.name = 'AbortError';
                return error;
            }

            cancelPendingStewartInverseResponses(message = 'Stewart SSE superseded') {
                if (this.pendingStewartInverseResponses.size === 0) {
                    return;
                }

                const abortError = this.createAbortError(message);
                const pendingResponses = Array.from(this.pendingStewartInverseResponses.values());
                this.pendingStewartInverseResponses.clear();

                for (const pending of pendingResponses) {
                    clearTimeout(pending.timeoutId);
                    try {
                        pending.reject(abortError);
                    } catch {
                    }
                }
            }

            cancelPendingStewartForwardResponses(message = 'Stewart SSE superseded') {
                if (this.pendingStewartForwardResponses.size === 0) {
                    return;
                }

                const abortError = this.createAbortError(message);
                const pendingResponses = Array.from(this.pendingStewartForwardResponses.values());
                this.pendingStewartForwardResponses.clear();

                for (const pending of pendingResponses) {
                    clearTimeout(pending.timeoutId);
                    try {
                        pending.reject(abortError);
                    } catch {
                    }
                }
            }

            computeStewartStreamRodLengths(armEndPoints, platformPoints) {
                if (!Array.isArray(armEndPoints) || armEndPoints.length !== 6 || !Array.isArray(platformPoints) || platformPoints.length !== 6) {
                    return [0, 0, 0, 0, 0, 0];
                }

                const pm = Array.isArray(this.params.servoPointMap) ? this.params.servoPointMap : [0,1,2,3,4,5];
                return armEndPoints.map((armEndPoint, index) => {
                    const platformPoint = platformPoints[pm[index]] || [0, 0, 0];
                    const dx = Number(platformPoint[0] || 0) - Number(armEndPoint[0] || 0);
                    const dy = Number(platformPoint[1] || 0) - Number(armEndPoint[1] || 0);
                    const dz = Number(platformPoint[2] || 0) - Number(armEndPoint[2] || 0);
                    return Math.hypot(dx, dy, dz);
                });
            }

            computeStewartStreamFisheyeDiagnostics(basePoints, armEndPoints, platformPoints) {
                const fisheyeAngles = [0, 0, 0, 0, 0, 0];
                const fisheyeWarningArms = [];
                const warnings = [];

                if (!Array.isArray(basePoints) || basePoints.length !== 6 || !Array.isArray(armEndPoints) || armEndPoints.length !== 6 || !Array.isArray(platformPoints) || platformPoints.length !== 6) {
                    return {
                        fisheyeAngles,
                        fisheyeWarningArms,
                        warningArms: [],
                        warnings
                    };
                }

                for (let index = 0; index < 6; index += 1) {
                    const pm = Array.isArray(this.params.servoPointMap) ? this.params.servoPointMap : [0,1,2,3,4,5];
                    const pi = pm[index];
                    const basePoint = basePoints[pi] || [0, 0, 0];
                    const armEndPoint = armEndPoints[index] || [0, 0, 0];
                    const platformPoint = platformPoints[pi] || [0, 0, 0];
                    const armVecX = Number(armEndPoint[0] || 0) - Number(basePoint[0] || 0);
                    const armVecY = Number(armEndPoint[1] || 0) - Number(basePoint[1] || 0);
                    const rodVecX = Number(platformPoint[0] || 0) - Number(armEndPoint[0] || 0);
                    const rodVecY = Number(platformPoint[1] || 0) - Number(armEndPoint[1] || 0);
                    const armLenXY = Math.hypot(armVecX, armVecY);
                    const rodLenXY = Math.hypot(rodVecX, rodVecY);

                    if (armLenXY <= 1e-9 || rodLenXY <= 1e-9) {
                        continue;
                    }

                    const dot = armVecX * rodVecX + armVecY * rodVecY;
                    let fisheyeAngle = Math.acos(Math.max(-1, Math.min(1, dot / (armLenXY * rodLenXY)))) * 180 / Math.PI;
                    if (fisheyeAngle > 90) {
                        fisheyeAngle = 180 - fisheyeAngle;
                    }

                    fisheyeAngles[index] = fisheyeAngle;
                    if (fisheyeAngle > this.params.fisheyeAngleLimit) {
                        fisheyeWarningArms.push(index);
                        warnings.push(`?��?${index + 1}: 魚眼軸承角度 ${fisheyeAngle.toFixed(3)}° 超�??�制`);
                    }
                }

                return {
                    fisheyeAngles,
                    fisheyeWarningArms,
                    warningArms: [...fisheyeWarningArms],
                    warnings
                };
            }

            applyStewartStreamResult(response) {
                const basePoints = this.normalizePointArray(response.basePoints);
                const armEndPoints = this.normalizePointArray(response.armEndPoints);
                const platformPoints = this.normalizePointArray(response.platformPoints);
                const normalizedBasePoints = basePoints.length === 6 ? basePoints : this.getBasePoints();
                const rodLengths = Array.isArray(response.rodLengths) && response.rodLengths.length === 6
                    ? response.rodLengths.map(Number)
                    : this.computeStewartStreamRodLengths(armEndPoints, platformPoints);
                const fisheyeDiagnostics = Array.isArray(response.fisheyeAngles) && response.fisheyeAngles.length === 6
                    ? {
                        fisheyeAngles: response.fisheyeAngles.map(Number),
                        fisheyeWarningArms: Array.isArray(response.fisheyeWarningArms) ? response.fisheyeWarningArms.map(Number) : [],
                        warningArms: Array.isArray(response.warningArms) ? response.warningArms.map(Number) : (Array.isArray(response.fisheyeWarningArms) ? response.fisheyeWarningArms.map(Number) : []),
                        warnings: Array.isArray(response.warnings) ? response.warnings.map(String) : []
                    }
                    : this.computeStewartStreamFisheyeDiagnostics(normalizedBasePoints, armEndPoints, platformPoints);

                this.applyStewartResult({
                    ...response,
                    basePoints: normalizedBasePoints,
                    armEndPoints,
                    platformPoints,
                    rodLengths,
                    fisheyeAngles: fisheyeDiagnostics.fisheyeAngles,
                    warnings: fisheyeDiagnostics.warnings,
                    warningArms: fisheyeDiagnostics.warningArms,
                    fisheyeWarningArms: fisheyeDiagnostics.fisheyeWarningArms,
                    singularArms: Array.isArray(response.singularArms) ? response.singularArms.map(Number) : [],
                    singularityInfo: response.singularityInfo ? { ...response.singularityInfo } : {}
                });
            }

            applyStewartResult(response) {
                this.updateNativePlannerState(response);

                if (response.worldPose) {
                    this.pose = this.normalizePoseValues(response.worldPose);
                }
                if (response.platformPose) {
                    this.platformPose = this.normalizePoseValues(response.platformPose);
                }
                if (Number.isFinite(Number(response.initialHeight))) {
                    this.initialHeight = Number(response.initialHeight);
                }

                this.result = {
                    servoAngles: Array.isArray(response.servoAngles) ? response.servoAngles.map(Number) : [0, 0, 0, 0, 0, 0],
                    armEndPoints: this.normalizePointArray(response.armEndPoints),
                    platformPoints: this.normalizePointArray(response.platformPoints),
                    basePoints: this.normalizePointArray(response.basePoints),
                    servoPointMap: Array.isArray(response.servoPointMap) ? response.servoPointMap.map(Number) : this.params.servoPointMap,
                    rodLengths: Array.isArray(response.rodLengths) ? response.rodLengths.map(Number) : [0, 0, 0, 0, 0, 0],
                    fisheyeAngles: Array.isArray(response.fisheyeAngles) ? response.fisheyeAngles.map(Number) : [0, 0, 0, 0, 0, 0],
                    valid: !!response.valid,
                    errors: Array.isArray(response.errors) ? response.errors.map(String) : [],
                    warnings: Array.isArray(response.warnings) ? response.warnings.map(String) : [],
                    singularArms: Array.isArray(response.singularArms) ? response.singularArms.map(Number) : [],
                    singularityInfo: response.singularityInfo ? { ...response.singularityInfo } : {},
                    errorArms: this.normalizeErrorArms(response.errorArms, response.errors),
                    warningArms: Array.isArray(response.warningArms) ? response.warningArms.map(Number) : [],
                    fisheyeWarningArms: Array.isArray(response.fisheyeWarningArms) ? response.fisheyeWarningArms.map(Number) : []
                };
                this.rememberRenderableResult(this.result);
            }

            applyForwardKinematicsResult(response, fallbackServoAngles = null) {
                const pose = response?.pose || this.forwardResult.pose;
                const round = (value, decimals) => {
                    const factor = Math.pow(10, decimals);
                    return Math.round(value * factor) / factor;
                };
                const roundedPose = {
                    x: round(Number(pose.x) || 0, 6),
                    y: round(Number(pose.y) || 0, 6),
                    z: round(Number(pose.z) || 0, 6),
                    roll: round(Number(pose.roll) || 0, 7),
                    pitch: round(Number(pose.pitch) || 0, 7),
                    yaw: round(Number(pose.yaw) || 0, 7)
                };
                const platformPose = this.normalizePoseValues(response?.platformPose || this.forwardResult.platformPose);
                const responseInitialHeight = Number(response?.initialHeight);
                const initialHeight = Number.isFinite(responseInitialHeight)
                    ? responseInitialHeight
                    : this.initialHeight;
                const servoAngles = Array.isArray(response?.servoAngles) && response.servoAngles.length === 6
                    ? response.servoAngles.map(value => Number(value) || 0)
                    : (Array.isArray(fallbackServoAngles)
                        ? fallbackServoAngles.map(value => Number(value) || 0)
                        : this.getActiveForwardServoAngles());

                this.forwardResult = {
                    pose: roundedPose,
                    platformPose,
                    initialHeight,
                    valid: !!response?.valid,
                    errors: Array.isArray(response?.errors) ? response.errors.map(String) : [],
                    warnings: Array.isArray(response?.warnings) ? response.warnings.map(String) : []
                };

                if (this.forwardResult.valid) {
                    this.pose = { ...roundedPose };
                    this.platformPose = platformPose;
                    this.initialHeight = initialHeight;
                    this.updatePoseInputsForCoordSystem();

                    this.result = {
                        servoAngles,
                        armEndPoints: this.normalizePointArray(response.armEndPoints),
                        platformPoints: this.normalizePointArray(response.platformPoints),
                        basePoints: this.normalizePointArray(response.basePoints),
                        servoPointMap: Array.isArray(response.servoPointMap) ? response.servoPointMap.map(Number) : this.params.servoPointMap,
                        rodLengths: Array.isArray(response.rodLengths) ? response.rodLengths.map(Number) : [0, 0, 0, 0, 0, 0],
                        fisheyeAngles: Array.isArray(response.fisheyeAngles) ? response.fisheyeAngles.map(Number) : [0, 0, 0, 0, 0, 0],
                        valid: true,
                        errors: Array.isArray(response.errors) ? response.errors.map(String) : [],
                        warnings: Array.isArray(response.warnings) ? response.warnings.map(String) : [],
                        singularArms: Array.isArray(response.singularArms) ? response.singularArms.map(Number) : [],
                        singularityInfo: response.singularityInfo ? { ...response.singularityInfo } : {},
                        errorArms: this.normalizeErrorArms(response.errorArms, response.errors),
                        warningArms: Array.isArray(response.warningArms) ? response.warningArms.map(Number) : [],
                        fisheyeWarningArms: Array.isArray(response.fisheyeWarningArms) ? response.fisheyeWarningArms.map(Number) : []
                    };

                    this.rememberRenderableResult(this.result);
                    this.draw();
                    this.updateDisplay();
                }

                this.updateForwardDisplay();
            }

            async getStewartApi(endpoint, options = {}) {
                const {
                    signal = undefined,
                    retryCount = 1
                } = options;
                let lastError = null;

                for (let attempt = 0; attempt <= retryCount; attempt++) {
                    try {
                        const response = await fetch(endpoint, {
                            method: 'GET',
                            cache: 'no-store',
                            headers: {
                                'Accept': 'application/json'
                            },
                            signal
                        });

                        let data = null;
                        try {
                            data = await response.json();
                        } catch {
                            data = null;
                        }

                        if (!response.ok) {
                            const message = data?.error || `HTTP ${response.status}`;
                            throw new Error(message);
                        }

                        return data || {};
                    } catch (error) {
                        lastError = error;

                        if (error?.name === 'AbortError') {
                            throw error;
                        }

                        if (!this.isTransientNetworkError(error) || attempt === retryCount) {
                            throw error;
                        }
                    }
                }

                throw lastError || new Error('unknown Stewart API error');
            }

            async getStewartTextApi(endpoint, options = {}) {
                const {
                    signal = undefined,
                    retryCount = 1
                } = options;

                let lastError = null;

                for (let attempt = 0; attempt <= retryCount; attempt++) {
                    try {
                        const response = await fetch(endpoint, {
                            method: 'GET',
                            cache: 'no-store',
                            headers: {
                                'Accept': 'application/xml, text/plain, application/json'
                            },
                            signal
                        });

                        const responseText = await response.text();
                        let data = null;
                        if (responseText) {
                            try {
                                data = JSON.parse(responseText);
                            } catch {
                                data = null;
                            }
                        }

                        if (!response.ok) {
                            const message = data?.error || responseText.trim() || `HTTP ${response.status}`;
                            throw new Error(message);
                        }

                        return responseText;
                    } catch (error) {
                        lastError = error;

                        if (error?.name === 'AbortError') {
                            throw error;
                        }

                        if (!this.isTransientNetworkError(error) || attempt === retryCount) {
                            throw error;
                        }
                    }
                }

                throw lastError || new Error('unknown Stewart API text error');
            }

            async refreshNativePlannerStatus(options = {}) {
                const {
                    silent = false
                } = options;

                try {
                    const response = await this.getStewartApi('/api/stewart/native/status', { retryCount: 0 });
                    this.updateNativePlannerState(response);
                    this.updateNativePlannerDisplay();
                    return response;
                } catch (error) {
                    if (silent) {
                        this.updateNativePlannerDisplay();
                        return null;
                    }

                    if (!silent) {
                        this.nativePlannerState = {
                            ...this.nativePlannerState,
                            available: false,
                            errors: [`?�?��??�失?? ${String(error.message || error)}`]
                        };
                        this.updateNativePlannerDisplay();
                    }
                    throw error;
                }
            }

            async stopNativePlanner() {
                try {
                    this.updateNativePlannerConfigFromInput();
                    const response = await this.postStewartApi('/api/stewart/native/stop', {}, { retryCount: 0 });
                    this.updateNativePlannerState(response);
                    this.updateNativePlannerDisplay();
                    return response;
                } catch (error) {
                    this.nativePlannerState = {
                        ...this.nativePlannerState,
                        errors: [`?�止失�?: ${String(error.message || error)}`]
                    };
                    this.updateNativePlannerDisplay();
                    throw error;
                }
            }

            async startNativePlannerForDrag() {
                if (this.nativePlannerAutoDragActive) {
                    return;
                }

                this.nativePlannerAutoDragActive = true;
                this.nativePlannerPendingTargetPose = null;

                try {
                    await this.initializeNativePlannerFromCurrentPose();
                } catch {
                }
            }

            async stopNativePlannerForDrag() {
                if (!this.nativePlannerAutoDragActive) {
                    return;
                }

                this.nativePlannerAutoDragActive = false;
                this.nativePlannerPendingTargetPose = null;

                try {
                    await this.stopNativePlanner();
                } catch {
                }
            }

            queueNativePlannerTargetSync() {
                if (!this.nativePlannerAutoDragActive) {
                    return;
                }

                this.nativePlannerPendingTargetPose = this.normalizePoseValues(this.pose);
                if (this.nativePlannerTargetSyncInFlight) {
                    return;
                }

                void this.flushNativePlannerTargetSync();
            }

            async flushNativePlannerTargetSync() {
                if (!this.nativePlannerAutoDragActive || !this.nativePlannerPendingTargetPose) {
                    return;
                }

                const targetPose = this.nativePlannerPendingTargetPose;
                this.nativePlannerPendingTargetPose = null;
                this.nativePlannerTargetSyncInFlight = true;

                try {
                    const response = await this.submitStewartNativeTarget(targetPose, { snapToTarget: false });
                    this.updateNativePlannerState(response);
                    this.updateNativePlannerDisplay();
                } catch {
                } finally {
                    this.nativePlannerTargetSyncInFlight = false;
                    if (this.nativePlannerAutoDragActive && this.nativePlannerPendingTargetPose) {
                        void this.flushNativePlannerTargetSync();
                    }
                }
            }

            async initializeNativePlannerFromCurrentPose() {
                try {
                    const response = await this.submitStewartNativeTarget(this.pose, { snapToTarget: true });
                    this.updateNativePlannerState(response);
                    this.updateNativePlannerDisplay();
                    return response;
                } catch (error) {
                    this.nativePlannerState = {
                        ...this.nativePlannerState,
                        errors: [`?��??�失?? ${String(error.message || error)}`]
                    };
                    this.updateNativePlannerDisplay();
                    throw error;
                }
            }

            async submitStewartNativeTarget(poseOverride = this.pose, options = {}) {
                const {
                    snapToTarget = false
                } = options;

                this.updateNativePlannerConfigFromInput();
                const seq = ++this.stewartNativeTargetSeq;
                return this.postStewartApi('/api/stewart/native/target', {
                    ...this.buildWorldPosePayload(poseOverride),
                    seq,
                    linearSpeedMmPerSec: this.nativePlannerConfig.linearSpeedMmPerSec,
                    angularSpeedDegPerSec: this.nativePlannerConfig.angularSpeedDegPerSec,
                    snapToTarget
                }, { retryCount: 0 });
            }

            async requestStewartInverseViaStream(options = {}) {
                const {
                    signal = undefined
                } = options;

                if (!this.stewartInverseStreamConnected) {
                    throw new Error('Stewart SSE unavailable');
                }

                const seq = ++this.stewartInverseStreamSeq;
                const payload = {
                    ...this.buildInversePayload(),
                    seq
                };

                return new Promise((resolve, reject) => {
                    if (signal?.aborted) {
                        reject(this.createAbortError('Stewart SSE aborted'));
                        return;
                    }

                    this.cancelPendingStewartInverseResponses('Stewart SSE superseded');

                    let settled = false;
                    const cleanup = () => {
                        if (signal) {
                            signal.removeEventListener('abort', handleAbort);
                        }
                    };
                    const settleReject = (error) => {
                        if (settled) {
                            return;
                        }

                        settled = true;
                        clearTimeout(timeoutId);
                        this.pendingStewartInverseResponses.delete(seq);
                        cleanup();
                        reject(error);
                    };
                    const settleResolve = (data) => {
                        if (settled) {
                            return;
                        }

                        settled = true;
                        clearTimeout(timeoutId);
                        this.pendingStewartInverseResponses.delete(seq);
                        cleanup();
                        resolve(data);
                    };
                    const handleAbort = () => {
                        settleReject(this.createAbortError('Stewart SSE aborted'));
                    };
                    const timeoutId = setTimeout(() => {
                        settleReject(new Error('Stewart SSE timeout'));
                    }, this.stewartInverseStreamTimeoutMs);

                    this.pendingStewartInverseResponses.set(seq, {
                        resolve: settleResolve,
                        reject: settleReject,
                        timeoutId
                    });

                    if (signal) {
                        signal.addEventListener('abort', handleAbort, { once: true });
                    }

                    this.postStewartApi('/api/stewart/inverse/stream', payload, { retryCount: 0, signal })
                        .catch((error) => {
                            settleReject(error);
                        });
                });
            }

            async requestStewartForwardViaStream(options = {}) {
                const {
                    signal = undefined
                } = options;

                if (!this.stewartInverseStreamConnected) {
                    throw new Error('Stewart SSE unavailable');
                }

                const seq = ++this.stewartForwardStreamSeq;
                const payload = {
                    ...this.buildForwardPayload(),
                    seq
                };

                return new Promise((resolve, reject) => {
                    if (signal?.aborted) {
                        reject(this.createAbortError('Stewart SSE aborted'));
                        return;
                    }

                    this.cancelPendingStewartForwardResponses('Stewart SSE superseded');

                    let settled = false;
                    const cleanup = () => {
                        if (signal) {
                            signal.removeEventListener('abort', handleAbort);
                        }
                    };
                    const settleReject = (error) => {
                        if (settled) {
                            return;
                        }

                        settled = true;
                        clearTimeout(timeoutId);
                        this.pendingStewartForwardResponses.delete(seq);
                        cleanup();
                        reject(error);
                    };
                    const settleResolve = (data) => {
                        if (settled) {
                            return;
                        }

                        settled = true;
                        clearTimeout(timeoutId);
                        this.pendingStewartForwardResponses.delete(seq);
                        cleanup();
                        resolve(data);
                    };
                    const handleAbort = () => {
                        settleReject(this.createAbortError('Stewart SSE aborted'));
                    };
                    const timeoutId = setTimeout(() => {
                        settleReject(new Error('Stewart SSE timeout'));
                    }, this.stewartForwardStreamTimeoutMs);

                    this.pendingStewartForwardResponses.set(seq, {
                        resolve: settleResolve,
                        reject: settleReject,
                        timeoutId
                    });

                    if (signal) {
                        signal.addEventListener('abort', handleAbort, { once: true });
                    }

                    this.postStewartApi('/api/stewart/forward/stream', payload, { retryCount: 0, signal })
                        .catch((error) => {
                            settleReject(error);
                        });
                });
            }

            isTransientNetworkError(error) {
                if (!error) {
                    return false;
                }

                if (error.name === 'AbortError') {
                    return false;
                }

                const message = String(error.message || error);
                return message.includes('Failed to fetch') || message.includes('NetworkError');
            }

            async postStewartApi(endpoint, payload, options = {}) {
                const {
                    signal = undefined,
                    retryCount = 1
                } = options;
                let lastError = null;

                for (let attempt = 0; attempt <= retryCount; attempt++) {
                    try {
                        const response = await fetch(endpoint, {
                            method: 'POST',
                            cache: 'no-store',
                            headers: {
                                'Content-Type': 'application/json'
                            },
                            signal,
                            body: JSON.stringify(payload)
                        });

                        let data = null;
                        try {
                            data = await response.json();
                        } catch {
                            data = null;
                        }

                        if (!response.ok) {
                            const message = data?.error || `HTTP ${response.status}`;
                            throw new Error(message);
                        }

                        return data || {};
                    } catch (error) {
                        lastError = error;

                        if (error?.name === 'AbortError') {
                            throw error;
                        }

                        if (!this.isTransientNetworkError(error) || attempt === retryCount) {
                            throw error;
                        }
                    }
                }

                throw lastError || new Error('unknown Stewart API error');
            }

            async postStewartTextApi(endpoint, payloadText, options = {}) {
                const {
                    signal = undefined,
                    retryCount = 1,
                    contentType = 'application/xml; charset=utf-8'
                } = options;

                let lastError = null;

                for (let attempt = 0; attempt <= retryCount; attempt++) {
                    try {
                        const response = await fetch(endpoint, {
                            method: 'POST',
                            cache: 'no-store',
                            headers: {
                                'Content-Type': contentType,
                                'Accept': 'application/json'
                            },
                            signal,
                            body: String(payloadText ?? '')
                        });

                        const responseText = await response.text();
                        let data = null;
                        if (responseText) {
                            try {
                                data = JSON.parse(responseText);
                            } catch {
                                data = null;
                            }
                        }

                        if (!response.ok) {
                            const message = data?.error || responseText.trim() || `HTTP ${response.status}`;
                            throw new Error(message);
                        }

                        return data || {};
                    } catch (error) {
                        lastError = error;

                        if (error?.name === 'AbortError') {
                            throw error;
                        }

                        if (!this.isTransientNetworkError(error) || attempt === retryCount) {
                            throw error;
                        }
                    }
                }

                throw lastError || new Error('unknown Stewart API text post error');
            }

            // 計�??��??�學
            async calculate(options = {}) {
                const {
                    interactive = false
                } = options;
                const requestId = ++this.inverseRequestId;
                const useInteractiveStream = interactive && this.isUsingMmfPoseSource() && this.stewartInverseStreamConnected;

                if (this.inverseAbortController) {
                    this.inverseAbortController.abort();
                }

                const abortController = new AbortController();
                this.inverseAbortController = abortController;

                try {
                    const response = useInteractiveStream
                        ? await this.requestStewartInverseViaStream({ signal: abortController.signal })
                        : await this.postStewartApi(
                            '/api/stewart/inverse',
                            this.buildInversePayload(),
                            {
                                retryCount: interactive ? 0 : 1,
                                signal: abortController.signal
                            }
                        );
                    if (requestId !== this.inverseRequestId) {
                        return;
                    }

                    if (this.inverseAbortController === abortController) {
                        this.inverseAbortController = null;
                    }

                    if (useInteractiveStream) {
                        this.applyStewartStreamResult(response);
                    } else {
                        this.applyStewartResult(response);
                    }
                } catch (error) {
                    if (this.inverseAbortController === abortController) {
                        this.inverseAbortController = null;
                    }

                    if (error?.name === 'AbortError') {
                        return;
                    }

                    if (requestId !== this.inverseRequestId) {
                        return;
                    }

                    if (this.isTransientNetworkError(error) && this.lastRenderableResult) {
                        this.result = {
                            ...this.cloneRenderResult(this.lastRenderableResult),
                            valid: this.result.valid,
                            errors: Array.isArray(this.result.errors) ? [...this.result.errors] : [],
                            singularArms: Array.isArray(this.result.singularArms) ? [...this.result.singularArms] : [],
                            singularityInfo: this.result.singularityInfo ? { ...this.result.singularityInfo } : {},
                            errorArms: Array.isArray(this.result.errorArms) ? [...this.result.errorArms] : []
                        };
                        return;
                    }

                    this.result = {
                        servoAngles: [0, 0, 0, 0, 0, 0],
                        armEndPoints: [],
                        platformPoints: [],
                        basePoints: [],
                        rodLengths: [0, 0, 0, 0, 0, 0],
                        fisheyeAngles: [0, 0, 0, 0, 0, 0],
                        valid: false,
                        errors: [`?��??�學 API 失�?: ${error.message}`],
                        warnings: [],
                        singularArms: [],
                        singularityInfo: {},
                        errorArms: [],
                        warningArms: [],
                        fisheyeWarningArms: []
                    };
                }
            }

            // �???�學計�?：根?��??��?度�?算平?�姿??
            async calculateForwardKinematics(options = {}) {
                const {
                    interactive = false
                } = options;
                const requestId = ++this.forwardRequestId;
                const useInteractiveStream = interactive && this.isUsingMmfServoAngleSource() && this.stewartInverseStreamConnected;

                if (this.forwardAbortController) {
                    this.forwardAbortController.abort();
                }

                const abortController = new AbortController();
                this.forwardAbortController = abortController;
                const requestedServoAngles = this.getActiveForwardServoAngles();

                try {
                    const response = useInteractiveStream
                        ? await this.requestStewartForwardViaStream({ signal: abortController.signal })
                        : await this.postStewartApi(
                            '/api/stewart/forward',
                            this.buildForwardPayload(),
                            {
                                retryCount: interactive ? 0 : 1,
                                signal: abortController.signal
                            }
                        );
                    if (requestId !== this.forwardRequestId) {
                        return;
                    }

                    if (this.forwardAbortController === abortController) {
                        this.forwardAbortController = null;
                    }
                    this.applyForwardKinematicsResult(response, requestedServoAngles);
                } catch (error) {
                    if (this.forwardAbortController === abortController) {
                        this.forwardAbortController = null;
                    }

                    if (error?.name === 'AbortError') {
                        return;
                    }

                    if (requestId !== this.forwardRequestId) {
                        return;
                    }

                    this.forwardResult = {
                        pose: this.forwardResult.pose,
                        platformPose: this.forwardResult.platformPose,
                        initialHeight: this.forwardResult.initialHeight,
                        valid: false,
                        errors: [`�???�學 API 失�?: ${error.message}`],
                        warnings: []
                    };
                    this.updateForwardDisplay();
                }
            }

            // 繞固定�??��?標軸?��?視�?
            // ?�使得無論當?��?角�?何�?X??X??總是繞畫?��??�到??X 軸�??��?
            applyWorldAxisRotation(axis, angleDeg) {
                const angleRad = angleDeg * Math.PI / 180;

                // ?��??��??��??�陣 (從�??��?計�?)
                const currentMatrix = this.getViewRotationMatrix();

                // 建�?繞�?定軸?��?轉矩??
                let deltaMatrix;
                const c = Math.cos(angleRad);
                const s = Math.sin(angleRad);

                if (axis === 'X') {
                    deltaMatrix = [
                        [1, 0, 0],
                        [0, c, -s],
                        [0, s, c]
                    ];
                } else if (axis === 'Y') {
                    deltaMatrix = [
                        [c, 0, s],
                        [0, 1, 0],
                        [-s, 0, c]
                    ];
                } else {
                    deltaMatrix = [
                        [c, -s, 0],
                        [s, c, 0],
                        [0, 0, 1]
                    ];
                }

                // ?�矩??= currentMatrix * deltaMatrix (?��?：�??��?視�??�軸線�?�?
                const newMatrix = this.multiplyMatrix3x3(currentMatrix, deltaMatrix);

                // 從�?轉矩????��??��? (Y-X-Z ?��?)
                const euler = this.matrixToEulerYXZ(newMatrix);

                this.visualControl.rotationX = euler.x;
                this.visualControl.rotationY = euler.y;
                this.visualControl.rotationZ = euler.z;
            }

            // ?��??��?視�??��??�陣 (Y-X-Z 歐�?角�?�?
            getViewRotationMatrix() {
                const rotX = this.visualControl.rotationX * Math.PI / 180;
                const rotY = this.visualControl.rotationY * Math.PI / 180;
                const rotZ = this.visualControl.rotationZ * Math.PI / 180;
                
                const cx = Math.cos(rotX), sx = Math.sin(rotX);
                const cy = Math.cos(rotY), sy = Math.sin(rotY);
                const cz = Math.cos(rotZ), sz = Math.sin(rotZ);
                
                // Y 軸�?�?
                const Ry = [
                    [cy, 0, sy],
                    [0, 1, 0],
                    [-sy, 0, cy]
                ];
                
                // X 軸�?�?
                const Rx = [
                    [1, 0, 0],
                    [0, cx, -sx],
                    [0, sx, cx]
                ];
                
                // Z 軸�?�?
                const Rz = [
                    [cz, -sz, 0],
                    [sz, cz, 0],
                    [0, 0, 1]
                ];
                
                // 組�??��?: Rz * Rx * Ry (??project ?��?一??
                const RxRy = this.multiplyMatrix3x3(Rx, Ry);
                return this.multiplyMatrix3x3(Rz, RxRy);
            }

            // 3x3 ?�陣乘�?
            multiplyMatrix3x3(A, B) {
                const result = [[0,0,0], [0,0,0], [0,0,0]];
                for (let i = 0; i < 3; i++) {
                    for (let j = 0; j < 3; j++) {
                        for (let k = 0; k < 3; k++) {
                            result[i][j] += A[i][k] * B[k][j];
                        }
                    }
                }
                return result;
            }

            // 從�?轉矩????��??��? (Y-X-Z ?��?)
            matrixToEulerYXZ(m) {
                // ?�設 R = Rz * Rx * Ry
                // m[2][1] = sin(rotX)
                let rotX, rotY, rotZ;
                
                const sinX = m[2][1];
                
                if (Math.abs(sinX) < 0.9999) {
                    rotX = Math.asin(sinX);
                    rotY = Math.atan2(-m[2][0], m[2][2]);
                    rotZ = Math.atan2(-m[0][1], m[1][1]);
                } else {
                    // Gimbal lock
                    rotX = sinX > 0 ? Math.PI / 2 : -Math.PI / 2;
                    rotY = Math.atan2(m[0][2], m[0][0]);
                    rotZ = 0;
                }
                
                return {
                    x: rotX * 180 / Math.PI,
                    y: rotY * 180 / Math.PI,
                    z: rotZ * 180 / Math.PI
                };
            }

            // 3D??D?�影
            project(point, viewType = 'iso') {
                let [x, y, z] = point;

                // ?�用?��?變�?（�??��??�都?�用�?
                const rotX = this.visualControl.rotationX * Math.PI / 180;
                const rotY = this.visualControl.rotationY * Math.PI / 180;
                const rotZ = this.visualControl.rotationZ * Math.PI / 180;

                // ?�Y軸�?轉�?水平轉�?�?
                let tempX = x * Math.cos(rotY) + z * Math.sin(rotY);
                let tempZ = -x * Math.sin(rotY) + z * Math.cos(rotY);
                x = tempX;
                z = tempZ;

                // ?�X軸�?轉�?俯仰�?
                let tempY = y * Math.cos(rotX) - z * Math.sin(rotX);
                tempZ = y * Math.sin(rotX) + z * Math.cos(rotX);
                y = tempY;
                z = tempZ;

                // ?�後Z軸�?轉�?翻滾�?
                tempX = x * Math.cos(rotZ) - y * Math.sin(rotZ);
                tempY = x * Math.sin(rotZ) + y * Math.cos(rotZ);
                x = tempX;
                y = tempY;

                let scale = this.visualControl.scale; // 使用 let 以便?��?修改

                // 如�??��??��? (Perspective)
                if (this.visualControl.perspective) {
                    const cameraDistance = 2500; // ?��?距離 (mm)

                    // ?��?不�?視�?模�??��?�?��?�深度軸
                    // 注�?：此?��? x,y,z ?�已經�??��?轉矩????��??��?�?
                    let depth = z; // ?�設?�用??'iso' ??'top' (?�影??XY 平面，Z ?�深�?

                    if (viewType === 'side') {
                        depth = x; // Side 視�??�影??YZ 平面，X ?�深�?(?�直?�螢�?
                    }

                    // ?��??��?：�?大�?�?
                    // factor = distance / (distance - depth)
                    const perspectiveFactor = cameraDistance / (cameraDistance - depth);
                    scale *= perspectiveFactor;
                }

                // 使用 clientWidth/Height (CSS?��?) 以�???ctx.scale(dpr, dpr)
                // ?�樣?�以確�???High DPI ?��?上�?中�?點�?算正�?(不�??��??��?�?
                const dpr = window.devicePixelRatio || 1;
                const width = this.canvas.clientWidth || (this.canvas.width / dpr);
                const height = this.canvas.clientHeight || (this.canvas.height / dpr);
                const offsetX = width / 2 + this.visualControl.panX;
                const offsetY = height / 2 + this.visualControl.panY;

                switch (viewType) {
                    case 'iso':
                        // 等�?/�?��?�影：忽?�透�?，直?��??��?後�?三維?��??��? scale ?��??�畫�?
                        // ?�裡使用 orthographic mapping，�??�長度�?例�?被透�??�曲
                        // panX/panY already factored into offsetX/offsetY above ??avoid applying twice
                        return [
                            offsetX + x * scale,
                            offsetY - y * scale,
                            z // Depth
                        ];
                    case 'side':
                        // ?��??��?顯示Y-Z平面（�?X軸方?��?�?
                        // ?��?：Y -> X, Z -> Y
                        return [
                            offsetX + y * scale,
                            offsetY - z * scale,
                            x // Depth
                        ];
                    case 'top':
                        // 俯�??��?顯示X-Y平面（�?Z軸方?��?�?
                        return [
                            offsetX + x * scale,
                            offsetY - y * scale,  // Y軸�?上為�??修正?��?
                            z // Depth
                        ];
                    default:
                        return [
                            offsetX + x * scale,
                            offsetY - y * scale,
                            z // Depth
                        ];
                }
            }

            // 繪製?��??�景（�??�空?��?�?
            drawEnvironmentBackground(width, height) {
                const dpr = window.devicePixelRatio || 1;
                const w = width / dpr;
                const h = height / dpr;
                
                this.ctx.save();
                
                // 1. 天空漸層?�景
                const skyGradient = this.ctx.createLinearGradient(0, 0, 0, h);
                skyGradient.addColorStop(0, 'rgba(15, 25, 45, 0.9)');
                skyGradient.addColorStop(0.3, 'rgba(18, 32, 55, 0.7)');
                skyGradient.addColorStop(0.6, 'rgba(22, 38, 62, 0.5)');
                skyGradient.addColorStop(1, 'rgba(25, 42, 68, 0.3)');
                this.ctx.fillStyle = skyGradient;
                this.ctx.fillRect(0, 0, w, h);
                
                // 2. ?��??��??��?（模?��?源�?
                const glowX = w * 0.25;
                const glowY = h * 0.2;
                const glowRadius = Math.min(w, h) * 0.6;
                const glowGradient = this.ctx.createRadialGradient(
                    glowX, glowY, 0,
                    glowX, glowY, glowRadius
                );
                glowGradient.addColorStop(0, 'rgba(60, 100, 150, 0.12)');
                glowGradient.addColorStop(0.3, 'rgba(50, 85, 130, 0.08)');
                glowGradient.addColorStop(0.6, 'rgba(40, 70, 110, 0.04)');
                glowGradient.addColorStop(1, 'rgba(30, 55, 90, 0)');
                this.ctx.fillStyle = glowGradient;
                this.ctx.fillRect(0, 0, w, h);
                
                // 3. 網格?�面?��?（根?��??�調?��?
                this.drawGridFloor(w, h);
                
                // 4. ?�平線�?角�??��?vignette�?
                const vignetteGradient = this.ctx.createRadialGradient(
                    w / 2, h / 2, Math.min(w, h) * 0.2,
                    w / 2, h / 2, Math.max(w, h) * 0.8
                );
                vignetteGradient.addColorStop(0, 'rgba(0, 0, 0, 0)');
                vignetteGradient.addColorStop(0.5, 'rgba(0, 0, 0, 0)');
                vignetteGradient.addColorStop(0.8, 'rgba(0, 5, 15, 0.15)');
                vignetteGradient.addColorStop(1, 'rgba(0, 5, 15, 0.35)');
                this.ctx.fillStyle = vignetteGradient;
                this.ctx.fillRect(0, 0, w, h);
                
                this.ctx.restore();
            }
            
            // 繪製網格?�面
            drawGridFloor(w, h) {
                const groundZ = -150;
                const gridSize = 50; // 網格?��? (mm)
                const gridExtent = 500; // 網格範�? (mm)
                
                this.ctx.save();
                this.ctx.strokeStyle = 'rgba(80, 120, 160, 0.08)';
                this.ctx.lineWidth = 1;
                
                // 繪製 X ?��?網格�?
                for (let x = -gridExtent; x <= gridExtent; x += gridSize) {
                    const start = this.project([x, -gridExtent, groundZ], this.view);
                    const end = this.project([x, gridExtent, groundZ], this.view);
                    
                    // 計�?線�??��?度�?距離中�?越�?越淡�?
                    const distFactor = 1 - Math.abs(x) / gridExtent;
                    const alpha = 0.03 + distFactor * 0.07;
                    
                    this.ctx.strokeStyle = `rgba(80, 130, 180, ${alpha})`;
                    this.ctx.beginPath();
                    this.ctx.moveTo(start[0], start[1]);
                    this.ctx.lineTo(end[0], end[1]);
                    this.ctx.stroke();
                }
                
                // 繪製 Y ?��?網格�?
                for (let y = -gridExtent; y <= gridExtent; y += gridSize) {
                    const start = this.project([-gridExtent, y, groundZ], this.view);
                    const end = this.project([gridExtent, y, groundZ], this.view);
                    
                    const distFactor = 1 - Math.abs(y) / gridExtent;
                    const alpha = 0.03 + distFactor * 0.07;
                    
                    this.ctx.strokeStyle = `rgba(80, 130, 180, ${alpha})`;
                    this.ctx.beginPath();
                    this.ctx.moveTo(start[0], start[1]);
                    this.ctx.lineTo(end[0], end[1]);
                    this.ctx.stroke();
                }
                
                // 繪製中�?軸�?（�?亮�?
                this.ctx.strokeStyle = 'rgba(100, 150, 200, 0.15)';
                this.ctx.lineWidth = 1.5;
                
                // X 軸�?
                let start = this.project([-gridExtent, 0, groundZ], this.view);
                let end = this.project([gridExtent, 0, groundZ], this.view);
                this.ctx.beginPath();
                this.ctx.moveTo(start[0], start[1]);
                this.ctx.lineTo(end[0], end[1]);
                this.ctx.stroke();
                
                // Y 軸�?
                start = this.project([0, -gridExtent, groundZ], this.view);
                end = this.project([0, gridExtent, groundZ], this.view);
                this.ctx.beginPath();
                this.ctx.moveTo(start[0], start[1]);
                this.ctx.lineTo(end[0], end[1]);
                this.ctx.stroke();
                
                // 繪製?�面中�??��?
                const centerProj = this.project([0, 0, groundZ], this.view);
                const floorGlow = this.ctx.createRadialGradient(
                    centerProj[0], centerProj[1], 0,
                    centerProj[0], centerProj[1], 200 * this.visualControl.scale
                );
                floorGlow.addColorStop(0, 'rgba(60, 100, 150, 0.08)');
                floorGlow.addColorStop(0.5, 'rgba(50, 90, 140, 0.04)');
                floorGlow.addColorStop(1, 'rgba(40, 80, 130, 0)');
                this.ctx.fillStyle = floorGlow;
                this.ctx.fillRect(0, 0, w, h);
                
                this.ctx.restore();
            }

            // 繪制?��?軸�??��??��?確繪製�??��??�輯?�誤�?
            drawCoordinateAxes() {
                const axisLength = 150; // 世�??��?系中?�軸線長�?(mm) - ?�平?�軸線�???
                const origin = [0, 0, 0]; // ?��?
                const [ox, oy] = this.project(origin, this.view);

                const drawAxis = (endPoint, color, label) => {
                    // ?�接使用?�影點�??�樣就能?��?套用?��??��?
                    const [ex, ey] = this.project(endPoint, this.view);
                    
                    const dx = ex - ox;
                    const dy = ey - oy;
                    const distance = Math.hypot(dx, dy);
                    
                    // 如�??�影後長度太小�?跳�?繪製
                    if (distance < 2) return;

                    // ?��?
                    this.ctx.beginPath();
                    this.ctx.moveTo(ox, oy);
                    this.ctx.lineTo(ex, ey);
                    this.ctx.strokeStyle = color;
                    this.ctx.lineWidth = 2; // ?�平?�軸線�???
                    this.ctx.setLineDash([]);
                    this.ctx.stroke();

                    // ?�箭??
                    const angle = Math.atan2(dy, dx);
                    const headLen = 10;
                    this.ctx.beginPath();
                    this.ctx.moveTo(ex, ey);
                    this.ctx.lineTo(ex - headLen * Math.cos(angle - Math.PI / 6), ey - headLen * Math.sin(angle - Math.PI / 6));
                    this.ctx.lineTo(ex - headLen * Math.cos(angle + Math.PI / 6), ey - headLen * Math.sin(angle + Math.PI / 6));
                    this.ctx.lineTo(ex, ey);
                    this.ctx.fillStyle = color;
                    this.ctx.fill();

                    this.ctx.fillStyle = color;
                    this.ctx.font = 'bold 12px Arial'; // ?�平?�軸線�???
                    this.ctx.fillText(label, ex + 5, ey);
                };

                // ?�?��??�都顯示 X, Y, Z �?
                drawAxis([axisLength, 0, 0], '#FF0000', 'X');
                drawAxis([0, axisLength, 0], '#008800', 'Y');
                drawAxis([0, 0, axisLength], '#0000FF', 'Z');

                // ?��?標�?
                this.ctx.fillStyle = '#333333';
                this.ctx.beginPath();
                this.ctx.arc(ox, oy, 4, 0, 2 * Math.PI);
                this.ctx.fill();
            }

            // 繪制?��?端�??��??��?�?
            drawServoTrajectory(basePoint, servoIndex) {
                // 如�?軌跡?��?度為0，�?不繪製�?�?
                if (this.trajectorySettings.trajectoryOpacity <= 0) return;

                const armLength = this.params.servoArmLength;

                // 計�??��??�軸角度 (?�接使用servoAxisAngle?�數)
                const servoRadialAngle = this.params.servoAxisAngle[servoIndex] * Math.PI / 180;

                const currentServoAngle = this.result.servoAngles[servoIndex] * Math.PI / 180;

                // 設置?��?�??
                this.ctx.setLineDash([6, 3]);
                this.ctx.strokeStyle = `rgba(255, 140, 140, ${this.trajectorySettings.trajectoryOpacity})`;
                this.ctx.lineWidth = 2;

                // 繪制完整?��??��?跡�?360度�?
                // ?�個�?跡是?��??��??�平?�內?��??�路�?
                const trajectoryPoints = [];
                const steps = 72; // �?度�??��?，形?�平滑�?�?

                for (let step = 0; step <= steps; step++) {
                    const armAngleRad = (step / steps) * 2 * Math.PI - Math.PI; // -180度到+180�?

                    // 使用?�實?��?算相?��??��?計�?軌跡�?
                    const armEndPoint = [
                        basePoint[0] + armLength * Math.cos(servoRadialAngle) * Math.cos(armAngleRad),
                        basePoint[1] + armLength * Math.sin(servoRadialAngle) * Math.cos(armAngleRad),
                        basePoint[2] + armLength * Math.sin(armAngleRad)
                    ];

                    trajectoryPoints.push(armEndPoint);
                }

                // 繪制完整?��??��?�?
                this.ctx.beginPath();
                trajectoryPoints.forEach((point, index) => {
                    const [px, py] = this.project(point, this.view);
                    if (index === 0) {
                        this.ctx.moveTo(px, py);
                    } else {
                        this.ctx.lineTo(px, py);
                    }
                });
                this.ctx.closePath(); // ?��??��??��?�?
                this.ctx.stroke();

                // ?�復實�?
                this.ctx.setLineDash([]);

                // 繪制?��??��?端�?位置?��?亮�?�?
                // 使用?�實?��?算相?��??��?
                const currentArmEndPoint = [
                    basePoint[0] + armLength * Math.cos(servoRadialAngle) * Math.cos(currentServoAngle),
                    basePoint[1] + armLength * Math.sin(servoRadialAngle) * Math.cos(currentServoAngle),
                    basePoint[2] + armLength * Math.sin(currentServoAngle)
                ];

                const [cx, cy] = this.project(currentArmEndPoint, this.view);

                // 繪制?��?位置?��?亮�?
                this.ctx.fillStyle = '#ff6600';
                this.ctx.strokeStyle = '#ffffff';
                this.ctx.lineWidth = 2;

                this.ctx.beginPath();
                this.ctx.arc(cx, cy, 5, 0, 2 * Math.PI);
                this.ctx.fill();
                this.ctx.stroke();

                // 繪制從�??�中心到?��?端�??��??�方?��?
                const [bx, by] = this.project(basePoint, this.view);
                this.ctx.strokeStyle = `rgba(255, 120, 120, ${this.trajectorySettings.trajectoryOpacity + 0.2})`;
                this.ctx.lineWidth = 1;
                this.ctx.setLineDash([4, 2]);

                this.ctx.beginPath();
                this.ctx.moveTo(bx, by);
                this.ctx.lineTo(cx, cy);
                this.ctx.stroke();

                this.ctx.setLineDash([]); // ?�復實�?
            }

            // 繪製帶�?高亮?�景?��?字�?�?
            drawLabelWithHighlight(text, x, y, color = '#000000', bgColor = 'rgba(255, 255, 0, 0.85)', align = 'left') {
                this.ctx.save();
                // 字�??�大 2 ??
                this.ctx.font = 'bold 22px Arial';
                const metrics = this.ctx.measureText(text);
                const padding = 4;
                
                // 計�??�景框�?�?
                let boxX = x;
                if (align === 'center') boxX = x - metrics.width / 2;
                else if (align === 'right') boxX = x - metrics.width;
                
                const boxY = y - 18; // ?��??�移以適?��?大�?�?
                const boxHeight = 28;
                
                // ?��???
                this.ctx.fillStyle = bgColor;
                this.ctx.fillRect(boxX - padding, boxY, metrics.width + padding * 2, boxHeight);
                // ?��?�?
                this.ctx.strokeStyle = '#333';
                this.ctx.lineWidth = 0.8;
                this.ctx.strokeRect(boxX - padding, boxY, metrics.width + padding * 2, boxHeight);
                
                // ?��?�?
                this.ctx.fillStyle = color;
                this.ctx.textAlign = 'left'; 
                this.ctx.fillText(text, boxX, boxY + 20);
                
                this.ctx.restore();
            }

            // 繪制?��?形�??��?�?
            drawHexagonEdgeLabels(points, label, edgeA, edgeB, color) {
                // 不�??�接設�? fillStyle，由 drawLabelWithHighlight ?��?
                
                for (let i = 0; i < 6; i++) {
                    const currentPoint = points[i];
                    const nextPoint = points[(i + 1) % 6];

                    // 計�?實�??�長
                    const actualLength = Math.sqrt(
                        Math.pow(nextPoint[0] - currentPoint[0], 2) +
                        Math.pow(nextPoint[1] - currentPoint[1], 2) +
                        Math.pow(nextPoint[2] - currentPoint[2], 2)
                    );

                    // 計�??��?中�?
                    const midPoint = [
                        (currentPoint[0] + nextPoint[0]) / 2,
                        (currentPoint[1] + nextPoint[1]) / 2,
                        (currentPoint[2] + nextPoint[2]) / 2
                    ];

                    const [mx, my] = this.project(midPoint, this.view);

                    // ?�顯示實?��?算�??�長
                    const labelText = `${actualLength.toFixed(3)}`;

                    // 調整標註位置，避?��???
                    const offsetX = Math.cos(i * Math.PI / 3) * 15;
                    const offsetY = Math.sin(i * Math.PI / 3) * 15;

                    this.drawLabelWithHighlight(labelText, mx + offsetX, my + offsetY, '#000000');
                }
            }

            // 繪制??��?�度標註
            drawRodLengthLabels(armEndPoints, platformPoints) {
                for (let i = 0; i < 6; i++) {
                    const armEnd = armEndPoints[i];
                    const platformPoint = platformPoints[i];

                    // 計�???��?�度
                    const rodLength = Math.sqrt(
                        Math.pow(platformPoint[0] - armEnd[0], 2) +
                        Math.pow(platformPoint[1] - armEnd[1], 2) +
                        Math.pow(platformPoint[2] - armEnd[2], 2)
                    );

                    // 計�???��中�?
                    const midPoint = [
                        (armEnd[0] + platformPoint[0]) / 2,
                        (armEnd[1] + platformPoint[1]) / 2,
                        (armEnd[2] + platformPoint[2]) / 2
                    ];

                    const [mx, my] = this.project(midPoint, this.view);

                    // 顯示??��?�度
                    const labelText = `${rodLength.toFixed(3)}`;

                    // 調整標註位置，避?��??��?標註?��?
                    const offsetX = Math.cos(i * Math.PI / 3) * 20;
                    const offsetY = Math.sin(i * Math.PI / 3) * 20;

                    this.drawLabelWithHighlight(labelText, mx + offsetX, my + offsetY, '#000000', 'rgba(230, 255, 230, 0.9)'); // 淺�??�景?�??
                }
            }

            // 繪制平台?��??�基座�?心�? Z 軸�?�?
            drawCentroidHeight() {
                // ?�座?��? (0,0,0)
                const baseCenter = [0, 0, 0];
                // 平台?��?
                const platCenter = [this.pose.x, this.pose.y, this.pose.z];
                // 平台?��???XY 平面?��?影�? (??Z=0)
                const platProjection = [this.pose.x, this.pose.y, 0];

                const [bx, by] = this.project(baseCenter, this.view);
                const [px, py] = this.project(platCenter, this.view);
                const [ppx, ppy] = this.project(platProjection, this.view);

                this.ctx.save();

                // 1. 繪製?�座中�?�?(黑色實�???
                this.ctx.beginPath();
                this.ctx.arc(bx, by, 3, 0, Math.PI * 2);
                this.ctx.fillStyle = '#000';
                this.ctx.fill();

                // 2. 繪製水平位移�?(?�座中�? -> 平台?�影�? - ?�色?��?
                this.ctx.beginPath();
                this.ctx.moveTo(bx, by);
                this.ctx.lineTo(ppx, ppy);
                this.ctx.strokeStyle = '#666';
                this.ctx.lineWidth = 1;
                this.ctx.setLineDash([3, 3]);
                this.ctx.stroke();

                // 3. 繪製?�直高度�?(平台?�影�?-> 平台中�?) - ?�色點�?�?
                this.ctx.beginPath();
                this.ctx.moveTo(ppx, ppy);
                this.ctx.lineTo(px, py);
                this.ctx.strokeStyle = '#0000FF';
                this.ctx.lineWidth = 1.5;
                this.ctx.setLineDash([5, 3]); // 點�?線�???
                this.ctx.stroke();

                // --- ?��?：繪製平?��???Z 軸延伸�? (確�??��??? ---
                // 計�?局?�軸?��?
                const rotMatrix = this.getRotationMatrix(this.pose.roll, this.pose.pitch, this.pose.yaw);
                // X軸�?始�???[1,0,0], Y�?[0,1,0], Z�?[0,0,1]
                const xAxisVec = [rotMatrix[0][0], rotMatrix[1][0], rotMatrix[2][0]];
                const yAxisVec = [rotMatrix[0][1], rotMatrix[1][1], rotMatrix[2][1]];
                const zAxisVec = [rotMatrix[0][2], rotMatrix[1][2], rotMatrix[2][2]];
                
                const axisLen = 150; // 軸長�?

                // 繪製箭頭?��?
                const drawArrow = (endPt, color, label) => {
                    const [ex, ey] = this.project(endPt, this.view);
                    this.ctx.beginPath();
                    this.ctx.moveTo(px, py); // 從平?�中心出??
                    this.ctx.lineTo(ex, ey);
                    this.ctx.strokeStyle = color;
                    this.ctx.lineWidth = 2;
                    this.ctx.setLineDash([]);
                    this.ctx.stroke();

                    // 箭頭
                    const angle = Math.atan2(ey - py, ex - px);
                    const headLen = 10;
                    this.ctx.beginPath();
                    this.ctx.moveTo(ex, ey);
                    this.ctx.lineTo(ex - headLen * Math.cos(angle - Math.PI / 6), ey - headLen * Math.sin(angle - Math.PI / 6));
                    this.ctx.lineTo(ex - headLen * Math.cos(angle + Math.PI / 6), ey - headLen * Math.sin(angle + Math.PI / 6));
                    this.ctx.lineTo(ex, ey);
                    this.ctx.fillStyle = color;
                    this.ctx.fill();

                    // ?��?
                    this.ctx.font = 'bold 12px Arial';
                    this.ctx.fillStyle = color;
                    this.ctx.fillText(label, ex + 5, ey);
                };

                // 計�?終�?並繪�?
                const xEnd = [platCenter[0] + xAxisVec[0] * axisLen, platCenter[1] + xAxisVec[1] * axisLen, platCenter[2] + xAxisVec[2] * axisLen];
                const yEnd = [platCenter[0] + yAxisVec[0] * axisLen, platCenter[1] + yAxisVec[1] * axisLen, platCenter[2] + yAxisVec[2] * axisLen];
                const zEnd = [platCenter[0] + zAxisVec[0] * axisLen, platCenter[1] + zAxisVec[1] * axisLen, platCenter[2] + zAxisVec[2] * axisLen];

                drawArrow(xEnd, '#FF0000', 'X-Axis'); // 紅色 X
                drawArrow(yEnd, '#008800', 'Y-Axis'); // 綠色 Y
                drawArrow(zEnd, '#0000FF', 'Z-Axis'); // ?�色 Z (?��?標�??�色以�???
                
                // ------------------------------------------------

                // 4. 繪製平台中�?�?(紅色實�???
                this.ctx.beginPath();
                this.ctx.arc(px, py, 4, 0, Math.PI * 2);
                this.ctx.fillStyle = '#FF0000';
                this.ctx.fill();
                this.ctx.stroke();

                // 5. 標示高度?��?(高亮顯示)
                // 計�?標籤位置 (?�直線中�?
                const midX = (px + ppx) / 2;
                const midY = (py + ppy) / 2;

                this.ctx.font = 'bold 13px Arial';
                const text = `H=${this.pose.z.toFixed(2)}`;
                const metrics = this.ctx.measureText(text);
                
                // 繪製高亮?�景 (黃色)
                this.ctx.fillStyle = 'rgba(255, 255, 0, 0.85)'; // 黃色高亮?�景
                this.ctx.fillRect(midX + 6, midY - 11, metrics.width + 6, 16);
                // ?��?
                this.ctx.strokeStyle = '#000';
                this.ctx.lineWidth = 1;
                this.ctx.strokeRect(midX + 6, midY - 11, metrics.width + 6, 16);

                this.ctx.fillStyle = '#000000'; // 黑色?��?對�??��?
                this.ctx.textAlign = 'left';
                this.ctx.textBaseline = 'middle';
                this.ctx.fillText(text, midX + 9, midY - 3);

                this.ctx.restore();
            }

            // 繪製 3D ?�柱�?(模擬立�???
            // start3D, end3D: 世�??��? [x, y, z]
            // radius: ?�柱?��? (世�??��?系單�?
            // baseColor: ?��?顏色 (hex ??rgb)
            // isHidden: ?�否?�被?��??�隱?��? (?��?+淡�?)
            drawCylinder3D(start3D, end3D, radius, baseColor, isHidden = false) {
                const [sx, sy, sz] = this.project(start3D, this.view);
                const [ex, ey, ez] = this.project(end3D, this.view);
                
                // 計�? 2D ?��??�長�?
                const dx = ex - sx;
                const dy = ey - sy;
                const len2D = Math.hypot(dx, dy);
                
                if (len2D < 1) return; // 太短不繪�?
                
                // 計�??�直?��? (?�於?�柱寬度)
                const perpX = -dy / len2D;
                const perpY = dx / len2D;
                
                // ?��??��?縮放?��?
                const avgZ = (sz + ez) / 2;
                let scaledRadius = radius * this.visualControl.scale;
                if (this.visualControl.perspective) {
                    const cameraDistance = 2500;
                    const perspFactor = cameraDistance / (cameraDistance - avgZ);
                    scaledRadius *= perspFactor;
                }
                scaledRadius = Math.max(2, Math.min(scaledRadius, 20)); // ?�制範�?
                
                this.ctx.save();
                
                if (isHidden) {
                    // ?��?線�??��? + 淡�?
                    this.ctx.setLineDash([4, 4]);
                    this.ctx.globalAlpha = 0.4;
                }
                
                // �??顏色
                const color = this.parseColor(baseColor);
                
                // 建�?漸層模擬?�柱立�???
                // 漸層?��??��??��???
                const gradStartX = sx + perpX * scaledRadius;
                const gradStartY = sy + perpY * scaledRadius;
                const gradEndX = sx - perpX * scaledRadius;
                const gradEndY = sy - perpY * scaledRadius;
                
                const gradient = this.ctx.createLinearGradient(gradStartX, gradStartY, gradEndX, gradEndY);
                
                // ?��??�照?��?：根?��?源方?��?算�???
                // ?��??��?：左上�???[-0.5, 0.5, 0.8]
                const lightAngle = Math.atan2(0.5, -0.5); // ?��???XY 平面?��?�?
                const cylAngle = Math.atan2(perpY, perpX);
                const angleDiff = Math.abs(cylAngle - lightAngle);
                const lightFactor = Math.cos(angleDiff) * 0.5 + 0.5; // 0~1 範�?
                
                const highlightColor = this.lightenColor(color, 0.3 + lightFactor * 0.3);
                const midColor = baseColor;
                const shadowColorCyl = this.darkenColor(color, 0.2 + (1 - lightFactor) * 0.25);
                
                gradient.addColorStop(0, highlightColor);
                gradient.addColorStop(0.25, this.lightenColor(color, 0.15));
                gradient.addColorStop(0.5, midColor);
                gradient.addColorStop(0.75, this.darkenColor(color, 0.15));
                gradient.addColorStop(1, shadowColorCyl);
                
                // 繪製?�柱主�? (使用粗�?條模??
                this.ctx.strokeStyle = gradient;
                this.ctx.lineWidth = scaledRadius * 2;
                this.ctx.lineCap = 'round';
                this.ctx.beginPath();
                this.ctx.moveTo(sx, sy);
                this.ctx.lineTo(ex, ey);
                this.ctx.stroke();
                
                // 繪製?�緣�?(增�?立�???
                if (!isHidden && scaledRadius > 4) {
                    this.ctx.strokeStyle = shadowColorCyl;
                    this.ctx.lineWidth = 1;
                    this.ctx.lineCap = 'butt';
                    
                    // 上�?�?
                    this.ctx.beginPath();
                    this.ctx.moveTo(sx + perpX * scaledRadius, sy + perpY * scaledRadius);
                    this.ctx.lineTo(ex + perpX * scaledRadius, ey + perpY * scaledRadius);
                    this.ctx.stroke();
                    
                    // 下�?�?
                    this.ctx.beginPath();
                    this.ctx.moveTo(sx - perpX * scaledRadius, sy - perpY * scaledRadius);
                    this.ctx.lineTo(ex - perpX * scaledRadius, ey - perpY * scaledRadius);
                    this.ctx.stroke();
                }
                
                this.ctx.restore();
            }

            // 繪製 3D ?��? (?��?)
            drawSphere3D(center3D, radius, baseColor, isHidden = false) {
                const [cx, cy, cz] = this.project(center3D, this.view);
                
                let scaledRadius = radius * this.visualControl.scale;
                if (this.visualControl.perspective) {
                    const cameraDistance = 2500;
                    const perspFactor = cameraDistance / (cameraDistance - cz);
                    scaledRadius *= perspFactor;
                }
                scaledRadius = Math.max(3, Math.min(scaledRadius, 25));
                
                this.ctx.save();
                
                if (isHidden) {
                    this.ctx.globalAlpha = 0.3;
                }
                
                // ?��?漸層模擬?��?（�?源�??�左上�??��?
                const highlightOffsetX = -scaledRadius * 0.35;
                const highlightOffsetY = -scaledRadius * 0.35;
                const gradient = this.ctx.createRadialGradient(
                    cx + highlightOffsetX, cy + highlightOffsetY, scaledRadius * 0.05,
                    cx, cy, scaledRadius
                );
                
                const color = this.parseColor(baseColor);
                const specularColor = this.lightenColor(color, 0.7); // 高�?
                const highlightColor = this.lightenColor(color, 0.45);
                const shadowColorSph = this.darkenColor(color, 0.45);
                const rimColor = this.darkenColor(color, 0.55); // ?�緣?�部
                
                gradient.addColorStop(0, specularColor);    // 高�?中�?
                gradient.addColorStop(0.15, highlightColor); // 亮部
                gradient.addColorStop(0.45, baseColor);      // 中�???
                gradient.addColorStop(0.75, shadowColorSph); // ?�部
                gradient.addColorStop(1, rimColor);          // ?�緣
                
                this.ctx.fillStyle = gradient;
                this.ctx.beginPath();
                this.ctx.arc(cx, cy, scaledRadius, 0, Math.PI * 2);
                this.ctx.fill();
                
                // ?��??�遮?��?�????
                if (!isHidden && scaledRadius > 5) {
                    // 底部?�影弧�?（模?�地?��?射�?
                    const aoGradient = this.ctx.createLinearGradient(
                        cx, cy - scaledRadius,
                        cx, cy + scaledRadius
                    );
                    aoGradient.addColorStop(0, 'rgba(0, 0, 0, 0)');
                    aoGradient.addColorStop(0.6, 'rgba(0, 0, 0, 0)');
                    aoGradient.addColorStop(1, 'rgba(0, 0, 0, 0.15)');
                    
                    this.ctx.fillStyle = aoGradient;
                    this.ctx.beginPath();
                    this.ctx.arc(cx, cy, scaledRadius, 0, Math.PI * 2);
                    this.ctx.fill();
                    
                    // ?�緣?��?
                    this.ctx.strokeStyle = shadowColorSph;
                    this.ctx.lineWidth = 1;
                    this.ctx.beginPath();
                    this.ctx.arc(cx, cy, scaledRadius, 0, Math.PI * 2);
                    this.ctx.stroke();
                }
                
                this.ctx.restore();
            }

            // 繪製?��??�影線�??�於?��??��?��?�地?�陰影�?
            drawShadowLine(start3D, end3D, width, opacity) {
                const [sx, sy] = this.project(start3D, this.view);
                const [ex, ey] = this.project(end3D, this.view);
                
                const len2D = Math.hypot(ex - sx, ey - sy);
                if (len2D < 1) return;
                
                // 計�??�直?��??�於漸層
                const dx = ex - sx;
                const dy = ey - sy;
                const perpX = -dy / len2D;
                const perpY = dx / len2D;
                
                let scaledWidth = width * this.visualControl.scale;
                scaledWidth = Math.max(2, Math.min(scaledWidth, 15));
                
                this.ctx.save();
                
                // ?�建?�影漸層?��?
                const gradient = this.ctx.createLinearGradient(
                    sx + perpX * scaledWidth, sy + perpY * scaledWidth,
                    sx - perpX * scaledWidth, sy - perpY * scaledWidth
                );
                gradient.addColorStop(0, `rgba(0, 0, 0, 0)`);
                gradient.addColorStop(0.3, `rgba(0, 0, 0, ${opacity})`);
                gradient.addColorStop(0.7, `rgba(0, 0, 0, ${opacity})`);
                gradient.addColorStop(1, `rgba(0, 0, 0, 0)`);
                
                // 繪製?��??�影
                this.ctx.strokeStyle = gradient;
                this.ctx.lineWidth = scaledWidth * 2;
                this.ctx.lineCap = 'round';
                this.ctx.shadowBlur = 8;
                this.ctx.shadowColor = `rgba(0, 0, 0, ${opacity * 0.5})`;
                
                this.ctx.beginPath();
                this.ctx.moveTo(sx, sy);
                this.ctx.lineTo(ex, ey);
                this.ctx.stroke();
                
                this.ctx.restore();
            }

            // 顏色工具?��?
            parseColor(color) {
                if (color.startsWith('#')) {
                    const r = parseInt(color.slice(1, 3), 16);
                    const g = parseInt(color.slice(3, 5), 16);
                    const b = parseInt(color.slice(5, 7), 16);
                    return { r, g, b };
                }
                return { r: 128, g: 128, b: 128 };
            }

            lightenColor(color, amount) {
                const r = Math.min(255, Math.round(color.r + (255 - color.r) * amount));
                const g = Math.min(255, Math.round(color.g + (255 - color.g) * amount));
                const b = Math.min(255, Math.round(color.b + (255 - color.b) * amount));
                return `rgb(${r},${g},${b})`;
            }

            darkenColor(color, amount) {
                const r = Math.max(0, Math.round(color.r * (1 - amount)));
                const g = Math.max(0, Math.round(color.g * (1 - amount)));
                const b = Math.max(0, Math.round(color.b * (1 - amount)));
                return `rgb(${r},${g},${b})`;
            }

            // 檢查線段?�否被平?�遮??
            isSegmentOccludedByPlatform(start3D, end3D, platformPoints) {
                // 計�?線段中�?
                const mid3D = [
                    (start3D[0] + end3D[0]) / 2,
                    (start3D[1] + end3D[1]) / 2,
                    (start3D[2] + end3D[2]) / 2
                ];
                
                // ?�影
                const [mx, my, mz] = this.project(mid3D, this.view);
                const projPlatPoints = platformPoints.map(p => this.project(p, this.view));
                const platZ = projPlatPoints.reduce((acc, p) => acc + p[2], 0) / 6;
                
                // 深度比�?：�??��?段中點在平台後面
                if (mz < platZ - 10) { // 10 ?�容�?
                    // 檢查?�影點是?�在平台多�?形內
                    if (this.isPointInPolygon(mx, my, projPlatPoints)) {
                        return true;
                    }
                }
                return false;
            }

            // 點是?�在多�?形內
            isPointInPolygon(x, y, polygon) {
                let inside = false;
                for (let i = 0, j = polygon.length - 1; i < polygon.length; j = i++) {
                    const xi = polygon[i][0], yi = polygon[i][1];
                    const xj = polygon[j][0], yj = polygon[j][1];
                    
                    if (((yi > y) !== (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi)) {
                        inside = !inside;
                    }
                }
                return inside;
            }

            // 繪制3D?��???
            draw() {
                const width = this.canvas.width;
                const height = this.canvas.height;
                const shouldDrawShadows = true;
                const shouldCheckOcclusion = true;
                const shouldDrawLabels = true;
                const shouldDrawTrajectories = true;
                const renderResult = this.buildPreviewRenderResult();

                this.ctx.clearRect(0, 0, width, height);
                
                // 繪製?��??�景（天空漸層、�??�、地平�?�?
                this.drawEnvironmentBackground(width, height);

                // 繪制 XYZ ?��?�?(作為?�景?��?
                this.drawCoordinateAxes();

                // 使用統�??�基座�?計�?
                const basePoints = this.normalizePointArray(renderResult.basePoints);
                const { armEndPoints, platformPoints } = renderResult;
                const pointMap = Array.isArray(renderResult.servoPointMap) && renderResult.servoPointMap.length === 6
                    ? renderResult.servoPointMap
                    : (Array.isArray(this.params.servoPointMap) ? this.params.servoPointMap : [0,1,2,3,4,5]);

                if (basePoints.length === 0 || armEndPoints.length === 0 || platformPoints.length === 0) return;

                // 準�??�?�可渲�??�件，統一?��?深度?��?
                const allRenderItems = [];

                // ?�照?�陰影�???
                const lightDir = [-0.5, 0.5, 0.8]; 
                const lightLen = Math.sqrt(lightDir[0]*lightDir[0] + lightDir[1]*lightDir[1] + lightDir[2]*lightDir[2]);
                const normLight = [lightDir[0]/lightLen, lightDir[1]/lightLen, lightDir[2]/lightLen];
                const groundZ = -150;
                
                // ?��??�影計�??�數
                const projectToGround = (point) => {
                    const t = (groundZ - point[2]) / normLight[2];
                    return [
                        point[0] + t * normLight[0],
                        point[1] + t * normLight[1],
                        groundZ
                    ];
                };
                
                // 計�?平台?�影�?
                if (shouldDrawShadows) {
                    const shadowPoints = platformPoints.map(p => projectToGround(p));
                    const projShadowPoints = shadowPoints.map(p => this.project(p, this.view));
                    const shadowZ = projShadowPoints.reduce((acc, p) => acc + p[2], 0) / 6;

                    // 計�??�影中�??�於漸層
                    const shadowCenterX = projShadowPoints.reduce((acc, p) => acc + p[0], 0) / 6;
                    const shadowCenterY = projShadowPoints.reduce((acc, p) => acc + p[1], 0) / 6;

                    allRenderItems.push({
                        z: shadowZ - 1500,
                        type: 'ground_plane',
                        draw: () => {
                            const groundSize = 400;
                            const groundCorners = [
                                [-groundSize, -groundSize, groundZ],
                                [groundSize, -groundSize, groundZ],
                                [groundSize, groundSize, groundZ],
                                [-groundSize, groundSize, groundZ]
                            ].map(p => this.project(p, this.view));

                            const groundGradient = this.ctx.createRadialGradient(
                                shadowCenterX, shadowCenterY, 0,
                                shadowCenterX, shadowCenterY, 300
                            );
                            groundGradient.addColorStop(0, 'rgba(40, 50, 70, 0.15)');
                            groundGradient.addColorStop(1, 'rgba(40, 50, 70, 0)');

                            this.ctx.fillStyle = groundGradient;
                            this.ctx.beginPath();
                            groundCorners.forEach((p, i) => {
                                if (i === 0) this.ctx.moveTo(p[0], p[1]);
                                else this.ctx.lineTo(p[0], p[1]);
                            });
                            this.ctx.closePath();
                            this.ctx.fill();
                        }
                    });

                    allRenderItems.push({
                        z: shadowZ - 1000,
                        type: 'platform_shadow',
                        draw: () => {
                            this.ctx.save();
                            this.ctx.shadowBlur = 35;
                            this.ctx.shadowColor = 'rgba(0, 0, 0, 0.25)';
                            this.ctx.shadowOffsetX = 0;
                            this.ctx.shadowOffsetY = 0;
                            this.ctx.fillStyle = 'rgba(0, 0, 0, 0.18)';
                            this.ctx.beginPath();
                            projShadowPoints.forEach((p, i) => {
                                if (i === 0) this.ctx.moveTo(p[0], p[1]);
                                else this.ctx.lineTo(p[0], p[1]);
                            });
                            this.ctx.closePath();
                            this.ctx.fill();

                            this.ctx.shadowBlur = 0;
                            const innerScale = 0.7;
                            const innerShadowPoints = projShadowPoints.map(p => [
                                shadowCenterX + (p[0] - shadowCenterX) * innerScale,
                                shadowCenterY + (p[1] - shadowCenterY) * innerScale
                            ]);

                            this.ctx.fillStyle = 'rgba(0, 0, 0, 0.12)';
                            this.ctx.beginPath();
                            innerShadowPoints.forEach((p, i) => {
                                if (i === 0) this.ctx.moveTo(p[0], p[1]);
                                else this.ctx.lineTo(p[0], p[1]);
                            });
                            this.ctx.closePath();
                            this.ctx.fill();
                            this.ctx.restore();
                        }
                    });

                    for (let i = 0; i < 6; i++) {
                        const pi = pointMap[i];
                        const bp = basePoints[pi];
                        const ae = armEndPoints[i];
                        const pp = platformPoints[pi];

                        const armShadowStart = projectToGround(bp);
                        const armShadowEnd = projectToGround(ae);
                        const armShadowMidZ = (this.project(armShadowStart, this.view)[2] + this.project(armShadowEnd, this.view)[2]) / 2;

                        allRenderItems.push({
                            z: armShadowMidZ - 800,
                            type: `arm_shadow_${i}`,
                            draw: () => {
                                this.drawShadowLine(armShadowStart, armShadowEnd, 6, 0.1);
                            }
                        });

                        const rodShadowStart = projectToGround(ae);
                        const rodShadowEnd = projectToGround(pp);
                        const rodShadowMidZ = (this.project(rodShadowStart, this.view)[2] + this.project(rodShadowEnd, this.view)[2]) / 2;

                        allRenderItems.push({
                            z: rodShadowMidZ - 800,
                            type: `rod_shadow_${i}`,
                            draw: () => {
                                this.drawShadowLine(rodShadowStart, rodShadowEnd, 4, 0.08);
                            }
                        });
                    }
                }

                // ?��?計�?平台?�影 (?�於?��?檢測)
                const projPlatPoints = platformPoints.map(p => this.project(p, this.view));
                const platZ = projPlatPoints.reduce((acc, p) => acc + p[2], 0) / 6;

                // 1. ?�座 (Base) - ?��?形�???
                const projBasePoints = basePoints.map(p => this.project(p, this.view));
                const baseZ = projBasePoints.reduce((acc, p) => acc + p[2], 0) / 6;
                
                // ?�座?�緣 (?�柱)
                for (let i = 0; i < 6; i++) {
                    const start = basePoints[i];
                    const end = basePoints[(i + 1) % 6];
                    const midZ = (this.project(start, this.view)[2] + this.project(end, this.view)[2]) / 2;
                    const isHidden = shouldCheckOcclusion && this.isSegmentOccludedByPlatform(start, end, platformPoints);
                    
                    allRenderItems.push({
                        z: midZ,
                        type: `base_edge_${i}`,
                        draw: () => {
                            this.drawCylinder3D(start, end, 8, '#555555', isHidden);
                        }
                    });
                }
                
                // ?�座中�?�?
                allRenderItems.push({
                    z: baseZ,
                    type: 'base_center',
                    draw: () => {
                        this.drawSphere3D([0, 0, 0], 12, '#333333', false);
                        if (shouldDrawLabels) {
                            this.drawHexagonEdgeLabels(basePoints, '?�座', this.params.baseEdgeA, this.params.baseEdgeB, '#444');
                        }
                    }
                });

                // 2. ?��??��?�� (Arms & Rods)
                for (let i = 0; i < 6; i++) {
                    const pi = pointMap[i];
                    const bp = basePoints[pi];
                    const ae = armEndPoints[i];
                    const pp = platformPoints[pi];
                    
                    const pbp = this.project(bp, this.view);
                    const pae = this.project(ae, this.view);
                    const ppp = this.project(pp, this.view);
                    
                    // 軌跡 (保�??��??�輯)
                    if (shouldDrawTrajectories) {
                        allRenderItems.push({
                            z: pbp[2] - 1,
                            type: `traj_${i}`,
                            draw: () => this.drawServoTrajectory(bp, i)
                        });
                    }

                    // ?�座??���?(?��?)
                    allRenderItems.push({
                        z: pbp[2],
                        type: `base_joint_${i}`,
                        draw: () => {
                            const isHidden = shouldCheckOcclusion && this.isSegmentOccludedByPlatform(bp, bp, platformPoints);
                            this.drawSphere3D(bp, 15, '#222222', isHidden);
                        }
                    });

                    // ?��? (?�柱)
                    const armMidZ = (pbp[2] + pae[2]) / 2;
                    const armIsHidden = shouldCheckOcclusion && this.isSegmentOccludedByPlatform(bp, ae, platformPoints);
                    
                    allRenderItems.push({
                        z: armMidZ,
                        type: `arm_${i}`,
                        draw: () => {
                            // 檢查該�??�是?��?奇�?，�?奇�??�顯示�??��??��?�?��顏色
                            const hasSingularity = renderResult.singularArms && renderResult.singularArms.includes(i);
                            const armColor = hasSingularity ? '#ff0000' : '#cc3333';
                            this.drawCylinder3D(bp, ae, 12, armColor, armIsHidden);
                            
                            // 標註
                            if (shouldDrawLabels && !armIsHidden) {
                                const midArmX = (pbp[0] + pae[0]) / 2;
                                const midArmY = (pbp[1] + pae[1]) / 2;
                                const armDisplayIndex = this.inverseMap[i] !== undefined ? this.inverseMap[i] : i;
                                this.drawLabelWithHighlight(`#${armDisplayIndex + 1} L=${this.params.servoArmLength}`, midArmX, midArmY, '#000', 'rgba(255, 220, 220, 0.9)');
                                
                                // 如�??��?點�?顯示奇�?資�?
                                if (hasSingularity && renderResult.singularityInfo && renderResult.singularityInfo[i]) {
                                    const info = renderResult.singularityInfo[i];
                                    let singularLabel = '';
                                    if (info.type === 'vertical') {
                                        singularLabel = `奇�?: ${info.value}°<${info.threshold}°`;
                                    } else if (info.type === 'horizontal') {
                                        singularLabel = `奇�?: ${info.value}°>${info.threshold}°`;
                                    } else if (info.type === 'extend') {
                                        singularLabel = `奇�?: ${info.value}%>${info.threshold}%`;
                                    } else if (info.type === 'contract') {
                                        singularLabel = `奇�?: ${info.value}%<${info.threshold}%`;
                                    }
                                    this.drawLabelWithHighlight(singularLabel, midArmX, midArmY + 15, '#fff', 'rgba(255, 0, 0, 0.85)');
                                }
                            }
                        }
                    });

                    // ?��?端�??��? (?��?)
                    allRenderItems.push({
                        z: pae[2],
                        type: `arm_end_joint_${i}`,
                        draw: () => {
                            const isHidden = shouldCheckOcclusion && this.isSegmentOccludedByPlatform(ae, ae, platformPoints);
                            this.drawSphere3D(ae, 10, '#ff4444', isHidden);
                        }
                    });

                    // ??�� (?�柱)
                    const rodMidZ = (pae[2] + ppp[2]) / 2;
                    const rodIsHidden = shouldCheckOcclusion && this.isSegmentOccludedByPlatform(ae, pp, platformPoints);
                    
                    allRenderItems.push({
                        z: rodMidZ,
                        type: `rod_${i}`,
                        draw: () => {
                            const hasSingularity = renderResult.singularArms && renderResult.singularArms.includes(i);
                            const hasError = renderResult.errorArms && renderResult.errorArms.includes(i);
                            const singularityInfo = renderResult.singularityInfo && renderResult.singularityInfo[i];
                            // ?��?點�??�誤顯示紅色，否?�正常�???
                            const rodColor = (hasSingularity || hasError) ? '#ff0000' : '#228833';
                            this.drawCylinder3D(ae, pp, 8, rodColor, rodIsHidden);
                            
                            // 如�??�幾何�?點�??�直?�水平�?，繪製延伸�?標示夾�?
                            if (shouldDrawLabels && hasSingularity && singularityInfo && (singularityInfo.type === 'vertical' || singularityInfo.type === 'horizontal')) {
                                // 計�???��?��?
                                const rodVec = [pp[0] - ae[0], pp[1] - ae[1], pp[2] - ae[2]];
                                const rodLen = Math.sqrt(rodVec[0] ** 2 + rodVec[1] ** 2 + rodVec[2] ** 2);
                                
                                // 延伸??��線�??��?�?
                                const extendLen = 80;
                                const extendEnd = [
                                    ae[0] - (rodVec[0] / rodLen) * extendLen,
                                    ae[1] - (rodVec[1] / rodLen) * extendLen,
                                    ae[2] - (rodVec[2] / rodLen) * extendLen
                                ];
                                const projExtendEnd = this.project(extendEnd, this.view);
                                
                                // 繪製??��延伸?��?
                                this.ctx.save();
                                this.ctx.setLineDash([5, 3]);
                                this.ctx.strokeStyle = '#ff6600';
                                this.ctx.lineWidth = 2;
                                this.ctx.beginPath();
                                this.ctx.moveTo(pae[0], pae[1]);
                                this.ctx.lineTo(projExtendEnd[0], projExtendEnd[1]);
                                this.ctx.stroke();
                                this.ctx.setLineDash([]);
                                this.ctx.restore();
                                
                                // 繪製Z軸�??��?（�??��?端�??�直?��?/?��?�?
                                const zRefLen = 80;
                                const zRefEnd = [ae[0], ae[1], ae[2] + zRefLen];
                                const projZRefEnd = this.project(zRefEnd, this.view);
                                
                                this.ctx.save();
                                this.ctx.setLineDash([3, 3]);
                                this.ctx.strokeStyle = '#0088ff';
                                this.ctx.lineWidth = 2;
                                this.ctx.beginPath();
                                this.ctx.moveTo(pae[0], pae[1]);
                                this.ctx.lineTo(projZRefEnd[0], projZRefEnd[1]);
                                this.ctx.stroke();
                                this.ctx.setLineDash([]);
                                this.ctx.restore();
                                
                                // 繪製夾�?弧�?
                                const angleRad = parseFloat(singularityInfo.value) * Math.PI / 180;
                                const arcRadius = 25;
                                
                                // 計�?弧�??�起始�?結�?角度（在?��?座�?系�?
                                const zVecScreen = [projZRefEnd[0] - pae[0], projZRefEnd[1] - pae[1]];
                                const rodVecScreen = [ppp[0] - pae[0], ppp[1] - pae[1]];
                                
                                const zAngle = Math.atan2(zVecScreen[1], zVecScreen[0]);
                                const rodAngle = Math.atan2(rodVecScreen[1], rodVecScreen[0]);
                                
                                this.ctx.save();
                                this.ctx.strokeStyle = '#ffcc00';
                                this.ctx.lineWidth = 2;
                                this.ctx.beginPath();
                                // 繪製從Z軸到??��?�弧�?
                                const startAngle = Math.min(zAngle, rodAngle);
                                const endAngle = Math.max(zAngle, rodAngle);
                                // ?��?較�??�弧
                                if (endAngle - startAngle > Math.PI) {
                                    this.ctx.arc(pae[0], pae[1], arcRadius, endAngle, startAngle + 2 * Math.PI);
                                } else {
                                    this.ctx.arc(pae[0], pae[1], arcRadius, startAngle, endAngle);
                                }
                                this.ctx.stroke();
                                this.ctx.restore();
                                
                                // ?�弧線�?標註角度
                                const midAngle = (zAngle + rodAngle) / 2;
                                const labelX = pae[0] + Math.cos(midAngle) * (arcRadius + 15);
                                const labelY = pae[1] + Math.sin(midAngle) * (arcRadius + 15);
                                this.drawLabelWithHighlight(`${singularityInfo.value}°`, labelX, labelY, '#fff', 'rgba(255, 100, 0, 0.9)');
                            }
                            
                            // ??��標註
                            if (shouldDrawLabels && !rodIsHidden) {
                                const rodLength = Array.isArray(renderResult.rodLengths)
                                    ? Number(renderResult.rodLengths[i] || 0)
                                    : Math.sqrt(
                                        Math.pow(pp[0] - ae[0], 2) +
                                        Math.pow(pp[1] - ae[1], 2) +
                                        Math.pow(pp[2] - ae[2], 2)
                                    );
                                const midRodX = (pae[0] + ppp[0]) / 2;
                                const midRodY = (pae[1] + ppp[1]) / 2;
                                const labelText = `${rodLength.toFixed(3)}`;
                                const offsetX = Math.cos(i * Math.PI / 3) * 20;
                                const offsetY = Math.sin(i * Math.PI / 3) * 20;
                                this.drawLabelWithHighlight(labelText, midRodX + offsetX, midRodY + offsetY, '#000000', 'rgba(230, 255, 230, 0.9)');

                                // 魚眼角度
                                const fisheyeAngle = Array.isArray(renderResult.fisheyeAngles)
                                    ? Number(renderResult.fisheyeAngles[i] || 0)
                                    : 0;
                                const limit = this.params.fisheyeAngleLimit || 30;
                                let angleColor = '#0066cc';
                                if (fisheyeAngle > limit * 0.9) angleColor = '#ff6600';
                                if (fisheyeAngle > limit) angleColor = '#cc0000';
                                if (fisheyeAngle > 0) {
                                    this.drawLabelWithHighlight(`${fisheyeAngle.toFixed(3)}°`, pae[0] + 8, pae[1] - 8, angleColor);
                                }
                            }
                            
                            // 編�?
                            const displayIndex = this.inverseMap[i] !== undefined ? this.inverseMap[i] : i;
                            if (shouldDrawLabels) {
                                this.ctx.fillStyle = '#000';
                                this.ctx.font = 'bold 12px Arial';
                                this.ctx.fillText(`${displayIndex + 1}`, pbp[0] + 5, pbp[1] - 5);
                            }
                        }
                    });

                    // 平台??��點�?節 (?��?)
                    allRenderItems.push({
                        z: ppp[2] + 1, // 稍微?�平?��???
                        type: `plat_joint_${i}`,
                        draw: () => {
                            const hasError = renderResult.errorArms && renderResult.errorArms.includes(i);
                            const jointColor = hasError ? '#cc0000' : '#0066cc';
                            this.drawSphere3D(pp, 10, jointColor, false);
                        }
                    });
                }
                
                // 3. 平台 (Platform)
                const platformItem = {
                    z: platZ,
                    type: 'platform',
                    draw: () => {
                        const valid = renderResult.valid;
                        
                        // ?�斷�????
                        let signedArea = 0;
                        for (let i = 0; i < 6; i++) {
                            const cur = projPlatPoints[i];
                            const next = projPlatPoints[(i + 1) % 6];
                            signedArea += (cur[0] * next[1] - next[0] * cur[1]);
                        }
                        const isFrontFace = signedArea > 0;
                        
                        // ?��?�???��??��??�選?��???
                        let fillColor, strokeColor;
                        if (valid) {
                            fillColor = isFrontFace ? 'rgba(0, 50, 150, 0.85)' : 'rgba(100, 150, 220, 0.85)';
                            strokeColor = '#0066cc';
                        } else {
                            fillColor = isFrontFace ? 'rgba(150, 50, 50, 0.85)' : 'rgba(220, 100, 100, 0.85)'; 
                            strokeColor = '#cc0000';
                        }
                        
                        // 繪製平台??
                        this.ctx.fillStyle = fillColor;
                        this.ctx.strokeStyle = strokeColor;
                        this.ctx.lineWidth = 3;
                        this.ctx.beginPath();
                        projPlatPoints.forEach((p, i) => {
                            if (i === 0) this.ctx.moveTo(p[0], p[1]);
                            else this.ctx.lineTo(p[0], p[1]);
                        });
                        this.ctx.closePath();
                        this.ctx.fill();
                        this.ctx.stroke();

                        if (shouldDrawLabels) {
                            this.drawHexagonEdgeLabels(platformPoints, '平台', this.params.platformEdgeA, this.params.platformEdgeB, strokeColor);
                        }
                    }
                };
                allRenderItems.push(platformItem);

                // 平台?�緣 (?�柱框架)
                for (let i = 0; i < 6; i++) {
                    const start = platformPoints[i];
                    const end = platformPoints[(i + 1) % 6];
                    const midZ = (this.project(start, this.view)[2] + this.project(end, this.view)[2]) / 2;
                    
                    allRenderItems.push({
                        z: midZ + 0.5, // 稍微?�平?�面上方
                        type: `plat_edge_${i}`,
                        draw: () => {
                            const valid = renderResult.valid;
                            this.drawCylinder3D(start, end, 6, valid ? '#0055aa' : '#aa3333', false);
                        }
                    });
                }

                // --- 統�?深度?��?並繪�?---
                // Z ?��?小�??��??��??�畫）�?Z ?��?大�??��??��?後畫�?
                allRenderItems.sort((a, b) => a.z - b.z);
                
                allRenderItems.forEach(item => {
                    item.draw();
                });

                // 繪制平台?��??�基座�?心�? Z 軸�?度�?�?(?�後畫)
                this.drawCentroidHeight();

                // 繪制魚眼軸承角度?��?
                this.drawFisheyeAngleLegend();
            }

            // 繪制魚眼軸承角度?��?
            drawFisheyeAngleLegend() {
                const limit = this.params.fisheyeAngleLimit || 30;
                const legendX = this.canvas.width - 180;
                const legendY = 20;

                // ?�景�?
                this.ctx.fillStyle = 'rgba(255, 255, 255, 0.9)';
                this.ctx.fillRect(legendX - 10, legendY - 15, 170, 80);

                // ?��?
                this.ctx.strokeStyle = '#333';
                this.ctx.lineWidth = 1;
                this.ctx.strokeRect(legendX - 10, legendY - 15, 170, 80);

                // 標�?
                this.ctx.fillStyle = '#000';
                this.ctx.font = '12px Arial';
                this.ctx.fillText('魚眼軸承中�??�移角度', legendX, legendY);

                // �?��範�?
                this.ctx.fillStyle = '#0066cc';
                this.ctx.fillRect(legendX, legendY + 8, 12, 12);
                this.ctx.fillStyle = '#000';
                this.ctx.fillText(`�?�� (< ${(limit * 0.9).toFixed(0)}°)`, legendX + 18, legendY + 18);

                // ?��??�制
                this.ctx.fillStyle = '#ff6600';
                this.ctx.fillRect(legendX, legendY + 28, 12, 12);
                this.ctx.fillStyle = '#000';
                this.ctx.fillText(`?��??�制 (${(limit * 0.9).toFixed(0)}°-${limit.toFixed(0)}°)`, legendX + 18, legendY + 38);

                // 超�??�制
                this.ctx.fillStyle = '#cc0000';
                this.ctx.fillRect(legendX, legendY + 48, 12, 12);
                this.ctx.fillStyle = '#000';
                this.ctx.fillText(`超�??�制 (> ${limit.toFixed(0)}°)`, legendX + 18, legendY + 58);
            }

            updateDisplay() {
                // ?�新結�?顯示
                const resultSection = document.getElementById('resultSection');
                const resultTitle = document.getElementById('resultTitle');
                const errorList = document.getElementById('errorList');

                // ?�新標�??�樣�?
                resultTitle.innerHTML = `?��?角度輸出 (?��??�學) <span class="status-icon ${this.result.valid ? 'status-ok' : 'status-error'}">${this.result.valid ? '?? : '??}</span>`;
                resultSection.className = `section section-compact ${this.result.valid ? 'section-green' : 'section-red'}`;

                // 依顯示�?序更??outputServoAngle 輸入�?
                for (let i = 0; i < 6; i++) {
                    const paramIndex = this.displayMap[i];
                    const angle = this.result.servoAngles[paramIndex] ?? 0;
                    const outputEl = document.getElementById(`outputServoAngle${i}`);
                    if (outputEl) {
                        outputEl.value = angle.toFixed(7);
                    }
                }

                // ?�新?�誤?�表
                const messages = [
                    ...(Array.isArray(this.result.errors) ? this.result.errors : []),
                    ...(Array.isArray(this.result.warnings) ? this.result.warnings : [])
                ];
                if (messages.length > 0) {
                    errorList.style.display = 'block';
                    errorList.innerHTML = messages.map(error => `<div>${error}</div>`).join('');
                } else {
                    errorList.style.display = 'none';
                }

                this.updateNativePlannerDisplay();
            }

            // ?�置?�數?��?設�?
            resetToDefaults() {
                const presetGeneration = this.beginParameterPresetMutation();

                // ?�置機�??�數
                this.params.platformEdgeA = 620;
                this.params.platformEdgeB = 100;
                this.params.baseEdgeA = 620;
                this.params.baseEdgeB = 100;
                this.params.rodLength = 490;
                this.params.servoArmLength = 300;
                this.params.servoInitialAngle = 10; // ?�設 10 �?
                this.params.servoAxisAngle = [120, 300, 0, 180, 240, 60];
                this.params.servoPointMap = [2, 1, 0, 5, 4, 3];
                this.params.servoAngleLimit = 75;
                this.nativePlannerConfig.linearSpeedMmPerSec = 600;
                this.nativePlannerConfig.angularSpeedDegPerSec = 120;

                // ?�置魚眼軸承角度?�制
                this.params.fisheyeAngleLimit = 30;

                // ?�置奇�??�數
                this.params.singularityVerticalAngle = 18;
                this.params.singularityHorizontalAngle = 87;
                this.params.singularityExtendRatio = 95;
                this.params.singularityContractRatio = 105;

                // ?�置?�大姿?��??�到?�設??
                this.params.maxPoseLimits = {
                    x: 100,   // 左右移�? (mm)
                    y: 100,   // ?��?移�? (mm)
                    z: 100,   // 上�?移�? (mm)
                    roll: 30, // ?��??��? (°)
                    pitch: 30,// 左右?��? (°)
                    yaw: 15   // ?��?角度 (°)
                };
                this.coordinateSystem = 'world';
                this.previousCoordinateSystemBeforeMmf = null;

                // ?�置姿�?
                this.pose.x = 0;
                this.pose.y = 0;
                this.pose.z = this.initialHeight;
                this.pose.roll = 0;
                this.pose.pitch = 0;
                this.pose.yaw = 0;
                this.platformPose = { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };
                this.poseInput = { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };

                // 維�??��?視覺?�控?�設定�?不�?置�?角�?

                // ?�新UI顯示??
                this.syncUIValues();
                this.updateUIFromPose();
                this.updatePoseSourceUi();
                this.updateForwardInputSourceUi();
                this.syncForwardKinematicsUI();

                // ?�新計�?並繪�?
                void this.updateAndDrawForParameterPreset(presetGeneration);

                // ?�新�???�學UI
                this.syncForwardKinematicsUI();
                this.updateForwardDisplay();
            }

            // ?�佳�?機�??�數以獲得�?大�?程�?角度?��?
            async optimizeParameters() {
                const presetGeneration = this.beginParameterPresetMutation();

                // 使用使用?�設定�??��?大姿?��???(設�??��?)?��??��??��?調整機�?尺寸?��?�?
                const limits = this.params.maxPoseLimits || { x: 100, y: 100, z: 100, roll: 30, pitch: 30, yaw: 15 };

                // 對�??��?角度?�求�??��?算縮?��?�?
                const defaultLinearTarget = 100; // UI ?�設�?��?�典?��?程目�?(mm)
                const maxLinear = Math.max(Math.abs(limits.x), Math.abs(limits.y), Math.abs(limits.z));
                let linearScale = (maxLinear / defaultLinearTarget) || 1;

                // ?��?行�?設�??��??��?極�?縮�?
                if (maxLinear < defaultLinearTarget * 0.5) {
                    linearScale *= 0.8; // 小�?程�?額�?縮�?20%
                } else if (maxLinear < defaultLinearTarget * 0.7) {
                    linearScale *= 0.9; // 中�?行�??��?外縮�?0%
                }

                linearScale = Math.max(0.3, Math.min(2.0, linearScale)); // ?�大縮放範�?

                const defaultAngularTarget = 30; // ?�設角度?��? (°)
                const maxAngular = Math.max(Math.abs(limits.roll), Math.abs(limits.pitch), Math.abs(limits.yaw));
                let angularScale = (maxAngular / defaultAngularTarget) || 1;

                // ?��?角度設�??��??��?極�?縮�?
                if (maxAngular < defaultAngularTarget * 0.5) {
                    angularScale *= 0.85; // 小�?度�?額�?縮�?15%
                }

                angularScale = Math.max(0.4, Math.min(1.8, angularScale)); // ?�大縮放範�?

                // 綜�?縮放：�?較大?�縮?�以滿足?�?�格?�求�?但�?小設計目標更?��?
                const scale = Math.max(linearScale, angularScale);

                // ?��?縮放調整?�座/平台/?�長/??��
                const baseSize = Math.round(650 * scale);
                const platformSize = Math.round(550 * scale);
                const armLength = Math.round(280 * scale);

                this.params.baseEdgeA = baseSize;
                this.params.baseEdgeB = Math.max(15, Math.round(baseSize / 6)); // ?��??�小值以?�許?��?機�?
                this.params.platformEdgeA = platformSize;
                this.params.platformEdgeB = Math.max(15, Math.round(platformSize / 6));

                this.params.servoArmLength = armLength;
                // ??��?�度?��?例並保�??��?範�?
                this.params.rodLength = Math.max(100, Math.round(armLength * 1.8)); // ?��??�小值確保穩定�?

                // 注�?：�??�根?�設計目標自?�調?��?度�??��?保�??�戶設�??��?

                // 計�??�佳�?始�??��?�?(?��?決�??��?高度)
                if (!await this.optimizeServoInitialAngle(presetGeneration)) {
                    return;
                }
                if (!this.isCurrentParameterPresetMutation(presetGeneration)) {
                    return;
                }

                // ?��?標�??��?（�?要�??�微調�?
                this.params.servoAxisAngle = [120, 300, 0, 180, 240, 60];
                this.params.servoPointMap = [2, 1, 0, 5, 4, 3];

                // ?�新評估並設定�??��?度�???
                this.optimizeServoAngleLimit();

                // 奇�??��??��?
                this.optimizeForSingularityAvoidance();

                // ?��?設�??��?調整?�終姿?��?始值�?保�?居中�?
                this.pose.x = 0;
                this.pose.y = 0;
                this.pose.roll = 0;
                this.pose.pitch = 0;
                this.pose.yaw = 0;
                this.pose.z = this.initialHeight;
                this.poseInput = { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };

                // ?�新UI並�?繪�?保�?視�?�?
                this.syncUIValues();
                this.updateUIFromPose();
                if (!await this.updateAndDrawForParameterPreset(presetGeneration)) {
                    return;
                }
                this.syncForwardKinematicsUI();
                this.updateForwardDisplay();

                // ?�佳�?完�?（移?��?示�??�顯示�?
            }

            // 尋找?�佳�??��?始�?�?(0~45�?，以?�大�?極�?姿�?下�??��???
            async optimizeServoInitialAngle(presetGeneration) {
                let bestAngle = 15;
                let maxScore = -1;
                const previousCoordinateSystem = this.coordinateSystem;
                this.coordinateSystem = 'world';
                
                // 定義測試點�?依�? maxPoseLimits 建�?測試?��?
                // 測試點�??��??��??�X/Y/Z 極�??��?度極?�、以?��?些�???
                const limits = this.params.maxPoseLimits || { x:100, y:100, z:100, roll:30, pitch:30, yaw:15 };
                
                // 權�?：基礎�?置�??��?，其次是?�軸極�?
                // 每個測試�??�們檢?�是??valid (?�錯�?
                const testCases = [
                    { pose: {x:0, y:0, z:0, r:0, p:0, yaw:0}, weight: 100 }, // Home
                    // ?�軸極�?
                    { pose: {x:limits.x, y:0, z:0, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:-limits.x, y:0, z:0, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:limits.y, z:0, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:-limits.y, z:0, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:limits.z, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:-limits.z, r:0, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:limits.roll, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:-limits.roll, p:0, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:0, p:limits.pitch, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:0, p:-limits.pitch, yaw:0}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:0, p:0, yaw:limits.yaw}, weight: 10 },
                    { pose: {x:0, y:0, z:0, r:0, p:0, yaw:-limits.yaw}, weight: 10 },
                    // 組�?極�? (較嚴?��??�用70%)
                    { pose: {x:limits.x*0.7, y:limits.y*0.7, z:0, r:0, p:0, yaw:0}, weight: 5 },
                    { pose: {x:0, y:0, z:0, r:limits.roll*0.7, p:limits.pitch*0.7, yaw:0}, weight: 5 }
                ];

                for (let angle = 0; angle <= 45; angle += 1) {
                    if (!this.isCurrentParameterPresetMutation(presetGeneration)) {
                        return false;
                    }
                    this.params.servoInitialAngle = angle;
                    
                    let score = 0;
                    for (let test of testCases) {
                        if (!this.isCurrentParameterPresetMutation(presetGeneration)) {
                            return false;
                        }
                        this.poseInput = {
                            x: test.pose.x,
                            y: test.pose.y,
                            z: test.pose.z,
                            roll: test.pose.r,
                            pitch: test.pose.p,
                            yaw: test.pose.yaw
                        };
                        
                        await this.calculate();
                        if (!this.isCurrentParameterPresetMutation(presetGeneration)) {
                            return false;
                        }
                        
                        if (this.result.valid) {
                            score += test.weight;
                        }
                    }
                    
                    // ?�新?�佳解
                    if (score > maxScore) {
                        maxScore = score;
                        bestAngle = angle;
                    }
                }
                
                // 套用?�佳�???
                if (!this.isCurrentParameterPresetMutation(presetGeneration)) {
                    return false;
                }
                this.params.servoInitialAngle = bestAngle;
                // ?�復?��?姿�?
                this.pose.x = 0; 
                this.pose.y = 0; 
                this.pose.roll = 0; 
                this.pose.pitch = 0; 
                this.pose.yaw = 0;
                this.poseInput = { x: 0, y: 0, z: 0, roll: 0, pitch: 0, yaw: 0 };
                this.coordinateSystem = previousCoordinateSystem;
                return true;
            }

            // ?��??��?角度?�制，考慮魚眼??��桿�??�種?�制?��?
            optimizeServoAngleLimit() {
                // 1. 魚眼軸承角度?�制（使?�者設定�??�高優?��??�制�?
                const fisheyeLimit = this.params.fisheyeAngleLimit || 30;

                // 2. ?�形?��??��??�制
                // 標�?魚眼??��桿�?形�?節?�常?�許±35°?��?5°?�擺??
                const sphericalJointLimit = 42; // 保�?估�?42°

                // 3. 幾�?約�? - ?�於??��桿�??��??�度計�?
                const rodToArmRatio = this.params.rodLength / this.params.servoArmLength;
                let geometricLimit;

                if (rodToArmRatio >= 2.0) {
                    // ?��?��桿�?幾�??�制較�?，主要�??�形?��??�制
                    geometricLimit = sphericalJointLimit;
                } else if (rodToArmRatio >= 1.5) {
                    // 中�?比�?：�?算幾何�???
                    const maxAngleRatio = Math.asin((this.params.rodLength - this.params.servoArmLength) /
                        (this.params.rodLength + this.params.servoArmLength));
                    geometricLimit = maxAngleRatio * 180 / Math.PI;
                } else {
                    // ?��?��桿�?幾�??�制較嚴??
                    const maxAngleRatio = Math.asin((this.params.rodLength - this.params.servoArmLength) /
                        (this.params.rodLength + this.params.servoArmLength));
                    geometricLimit = maxAngleRatio * 180 / Math.PI * 0.9; // 90%使用??
                }

                // 4. ??��桿�?干�??�慮
                // 計�??�鄰??��桿�??��?小�?度�?�?
                const baseRadius = (this.params.baseEdgeA + this.params.baseEdgeB) / 2;
                const platformRadius = (this.params.platformEdgeA + this.params.platformEdgeB) / 2;

                // 估�???��桿�??�干涉�?�?
                const minRodSpacing = Math.min(baseRadius, platformRadius) * 0.15; // ?�於?��???5%作為安全?��?
                const interferenceLimit = Math.atan(minRodSpacing / this.params.rodLength) * 180 / Math.PI * 2;

                // 5. ?�座/平台干�??�慮
                // ?��?��桿�?度太大�?，可?��??�到?�座?�平??
                const baseInterferenceLimit = Math.asin(this.params.baseHeight / (this.params.rodLength * 0.8)) * 180 / Math.PI;

                // 6. 綜�??�?��??��?素�?魚眼軸承?�制?��?高優?��?
                const allLimits = [
                    fisheyeLimit,             // 魚眼軸承角度?�制（�?高優?��?�?
                    sphericalJointLimit,      // ?�形?��??��??�制
                    geometricLimit,           // 幾�?約�?
                    interferenceLimit * 2,    // ??��桿�?干�?（�??��??��?
                    baseInterferenceLimit * 3 // ?�座干�?（�??��??��?
                ];

                // ?�濾?�無?�值�?NaN?��??��?
                const validLimits = allLimits.filter(limit => !isNaN(limit) && limit > 0);

                // ?��??��??�中?��?小值�??��??�用安全係數
                const minLimit = Math.min(...validLimits);
                const safetyFactor = 0.85; // 85%安全係數，�?之�???0%?��?�?

                // 計�?建議?��?度�??��?但�?修改?�戶設�??��?
                const suggestedLimit = Math.max(15, Math.round(minLimit * safetyFactor));

                // 確�?建議?�在?��?範�??��?但以魚眼軸承?�制?��???
                const finalSuggested = Math.min(suggestedLimit, fisheyeLimit); // 不能超�?魚眼軸承?�制
                const finalSuggestedLimited = Math.max(finalSuggested, 15); // ?�小�?低於15°

                // 注�?：�??�自?�修?��?度�??��?保�??�戶設�??��?
            }


            // 奇�??��??��? - 調整?�數以�?少�?點發??
            optimizeForSingularityAvoidance() {

                // 1. ?��???��桿長度�?例以?��??��?奇�?
                // ??��桿長度�?該在?��?範�??��??��??�長?��???
                const baseRadius = (this.params.baseEdgeA + this.params.baseEdgeB) / 2;
                const platformRadius = (this.params.platformEdgeA + this.params.platformEdgeB) / 2;

                // 計�??��?上�??�小�??�大�?��桿長�?
                const minRodLength = Math.abs(baseRadius - platformRadius) + 50; // ?��?安全裕度
                const maxRodLength = baseRadius + platformRadius - 50; // 減去安全裕度

                // 調整??��桿長度以?��??��?奇�?
                if (this.params.rodLength < minRodLength) {
                    this.params.rodLength = Math.round(minRodLength);
                } else if (this.params.rodLength > maxRodLength) {
                    this.params.rodLength = Math.round(maxRodLength);
                }

                // 2. ?��??�座?�平?�尺寸�?例以?��?幾�?奇�?
                // 確�??�座?�平?��??�出?��?點共線�??��?
                const baseToPlatformRatio = baseRadius / platformRadius;

                if (baseToPlatformRatio < 1.2) {
                    // ?�座太�?，�??�基座尺�?
                    const newBaseRadius = platformRadius * 1.3;
                    this.params.baseEdgeA = Math.round(newBaseRadius * 2);
                    this.params.baseEdgeB = Math.round(newBaseRadius * 2 / 6);
                } else if (baseToPlatformRatio > 3.0) {
                    // ?�座太大，�?小基座尺�?
                    const newBaseRadius = platformRadius * 2.8;
                    this.params.baseEdgeA = Math.round(newBaseRadius * 2);
                    this.params.baseEdgeB = Math.round(newBaseRadius * 2 / 6);
                }

                // 3. ?��??��?安�?角度以�?少內?��?點�???
                // 確�??��?角度佈�?不�?導致??��桿平行於?�座
                const angleSpread = Math.max(...this.params.servoAxisAngle) - Math.min(...this.params.servoAxisAngle);
                if (angleSpread < 240) {
                    // 角度?��?太�?中�?調整?�更?�勻?��?�?
                    this.params.servoAxisAngle = [0, 60, 120, 180, 240, 300];
                }

                // 4. 評估工�?空�??�制以避?�接近�?點�???
                // 注�?：�??�自?�調?��?度�??��?保�??�戶設�??��?

                // 6. 評估奇�?風險
                this.evaluateSingularityRisk();
            }

            // 評估奇�?風險
            evaluateSingularityRisk() {
                const baseRadius = (this.params.baseEdgeA + this.params.baseEdgeB) / 2;
                const platformRadius = (this.params.platformEdgeA + this.params.platformEdgeB) / 2;
                const rodRatio = this.params.rodLength / this.params.servoArmLength;

                let riskLevel = '�?;
                let riskFactors = [];

                // 評估?��?奇�?風險
                const boundaryRisk = Math.abs(this.params.rodLength - (baseRadius + platformRadius)) /
                    (baseRadius + platformRadius);
                if (boundaryRisk < 0.1) {
                    riskFactors.push('?��??��?奇�?');
                    riskLevel = '�?;
                } else if (boundaryRisk < 0.2) {
                    riskFactors.push('?��?奇�?風險中�?');
                    riskLevel = riskLevel === '�? ? '�? : riskLevel;
                }

                // 評估幾�?奇�?風險
                const geometryRisk = Math.abs(baseRadius - platformRadius) / Math.max(baseRadius, platformRadius);
                if (geometryRisk < 0.1) {
                    riskFactors.push('幾�?奇�?風險�?);
                    riskLevel = '�?;
                }

                // 評估?�部奇�?風險
                if (rodRatio < 1.3 || rodRatio > 2.5) {
                    riskFactors.push('?�部奇�?風險�?);
                    riskLevel = riskLevel === '�? ? '�? : '�?;
                }
            }

            // ?�出機�??�數?��?端固�?machine.xml
            async exportParameters() {
                this.mmf1InputModeConfigSaving = true;
                this.syncMmf1InputModeConfigUi();

                try {
                    const xmlContent = this.generateParametersXML();

                    await this.postStewartTextApi('/api/machine_config/xml', xmlContent, { retryCount: 0 });
                    this.savedMmf1InputMode = this.getSelectedMmf1InputMode();
                    this.mmf1InputModeConfigLoaded = true;
                    await this.refreshMmf1InputModeConfig({ silent: true });
                    alert('機�??�數已�??�寫?�執行�???Par/machine.xml�?);
                } catch (error) {
                    console.error('寫入?��? machine.xml ?�發?�錯�?', error);
                    alert('?�出??machine.xml 失�?: ' + error.message);
                } finally {
                    this.mmf1InputModeConfigSaving = false;
                    this.syncMmf1InputModeConfigUi();
                }
            }

            // ?��??�數??XML ?��?
            generateParametersXML() {
                const params = this.params;
                const coordinateSystem = this.normalizeCoordinateSystem(this.coordinateSystem);
                const mmf1InputMode = this.getSelectedMmf1InputMode();
                
                // 建�??��??��? XML 字串
                let xml = '<?xml version="1.0" encoding="UTF-8"?>\n';
                xml += '<MachineParameters>\n';
                xml += '  <Platform>\n';
                xml += `    <EdgeA>${params.platformEdgeA}</EdgeA>\n`;
                xml += `    <EdgeB>${params.platformEdgeB}</EdgeB>\n`;
                xml += '  </Platform>\n';
                xml += '  <Base>\n';
                xml += `    <EdgeA>${params.baseEdgeA}</EdgeA>\n`;
                xml += `    <EdgeB>${params.baseEdgeB}</EdgeB>\n`;
                xml += '  </Base>\n';
                xml += '  <Actuators>\n';
                xml += `    <RodLength>${params.rodLength}</RodLength>\n`;
                xml += `    <ServoArmLength>${params.servoArmLength}</ServoArmLength>\n`;
                xml += `    <ServoInitialAngle>${params.servoInitialAngle}</ServoInitialAngle>\n`;
                xml += `    <ServoAngleLimit>${params.servoAngleLimit}</ServoAngleLimit>\n`;
                xml += '    <ServoAxisAngles>\n';
                // machine.xml ?�接保�?實�?機�??��???Servo1~6??
                for (let i = 0; i < 6; i++) {
                    xml += `      <Servo${i + 1}>${params.servoAxisAngle[i]}</Servo${i + 1}>\n`;
                }
                xml += '    </ServoAxisAngles>\n';
                xml += '    <ServoPointMap>\n';
                const pm = Array.isArray(params.servoPointMap) ? params.servoPointMap : [0,1,2,3,4,5];
                for (let i = 0; i < 6; i++) {
                    xml += `      <Servo${i + 1}>${pm[i]}</Servo${i + 1}>\n`;
                }
                xml += '    </ServoPointMap>\n';
                xml += '  </Actuators>\n';
                xml += '  <Limits>\n';
                xml += `    <FisheyeAngleLimit>${params.fisheyeAngleLimit}</FisheyeAngleLimit>\n`;
                xml += '    <Singularity>\n';
                xml += `      <VerticalAngle>${params.singularityVerticalAngle || 18}</VerticalAngle>\n`;
                xml += `      <HorizontalAngle>${params.singularityHorizontalAngle || 87}</HorizontalAngle>\n`;
                xml += `      <ExtendRatio>${params.singularityExtendRatio || 95}</ExtendRatio>\n`;
                xml += `      <ContractRatio>${params.singularityContractRatio || 105}</ContractRatio>\n`;
                xml += '    </Singularity>\n';
                xml += '    <MaxPoseLimits>\n';
                xml += `      <X>${params.maxPoseLimits.x}</X>\n`;
                xml += `      <Y>${params.maxPoseLimits.y}</Y>\n`;
                xml += `      <Z>${params.maxPoseLimits.z}</Z>\n`;
                xml += `      <Roll>${params.maxPoseLimits.roll}</Roll>\n`;
                xml += `      <Pitch>${params.maxPoseLimits.pitch}</Pitch>\n`;
                xml += `      <Yaw>${params.maxPoseLimits.yaw}</Yaw>\n`;
                xml += '    </MaxPoseLimits>\n';
                xml += '  </Limits>\n';
                xml += '  <InverseInput>\n';
                xml += `    <CoordinateSystem>${coordinateSystem}</CoordinateSystem>\n`;
                xml += '  </InverseInput>\n';
                xml += '  <MMF1>\n';
                xml += `    <InputMode>${mmf1InputMode}</InputMode>\n`;
                xml += '  </MMF1>\n';
                xml += '</MachineParameters>\n';
                
                return xml;
            }

            // 從�?端固�?machine.xml ?�入機�??�數
            async importParameters(options = {}) {
                const silent = options.silent === true;
                try {
                    const xmlText = await this.getStewartTextApi('/api/machine_config/xml', { retryCount: 0 });
                    this.parseAndApplyParametersXML(xmlText, { silent });
                    await this.refreshMmf1InputModeConfig({ silent: true });
                    return true;
                } catch (error) {
                    console.error('從固�?machine.xml ?�入?�數?�發?�錯�?', error);
                    if (!silent) {
                        alert('�?machine.xml ?�入失�?: ' + error.message);
                    }
                    return false;
                }
            }

            // �?? XML 並�??��???
            parseAndApplyParametersXML(xmlText, options = {}) {
                const silent = options.silent === true;
                // 使用 DOMParser �?? XML
                const parser = new DOMParser();
                const xmlDoc = parser.parseFromString(xmlText, 'text/xml');
                
                // 檢查�???�誤
                const parserError = xmlDoc.querySelector('parsererror');
                if (parserError) {
                    throw new Error('XML ?��??�誤');
                }
                
                // ?��??��?�?
                const root = xmlDoc.querySelector('MachineParameters');
                if (!root) {
                    throw new Error('?��???MachineParameters 節�?);
                }
                
                // 讀?�平?��???
                const platform = root.querySelector('Platform');
                if (platform) {
                    const edgeA = platform.querySelector('EdgeA');
                    const edgeB = platform.querySelector('EdgeB');
                    if (edgeA) this.params.platformEdgeA = parseFloat(edgeA.textContent);
                    if (edgeB) this.params.platformEdgeB = parseFloat(edgeB.textContent);
                }
                
                // 讀?�基座�???
                const base = root.querySelector('Base');
                if (base) {
                    const edgeA = base.querySelector('EdgeA');
                    const edgeB = base.querySelector('EdgeB');
                    if (edgeA) this.params.baseEdgeA = parseFloat(edgeA.textContent);
                    if (edgeB) this.params.baseEdgeB = parseFloat(edgeB.textContent);
                }
                
                // 讀?�執行器?�數
                const actuators = root.querySelector('Actuators');
                if (actuators) {
                    const rodLength = actuators.querySelector('RodLength');
                    const servoArmLength = actuators.querySelector('ServoArmLength');
                    const servoInitialAngle = actuators.querySelector('ServoInitialAngle');
                    const servoAngleLimit = actuators.querySelector('ServoAngleLimit');
                    
                    if (rodLength) this.params.rodLength = parseFloat(rodLength.textContent);
                    if (servoArmLength) this.params.servoArmLength = parseFloat(servoArmLength.textContent);
                    if (servoInitialAngle) this.params.servoInitialAngle = parseFloat(servoInitialAngle.textContent);
                    if (servoAngleLimit) this.params.servoAngleLimit = parseFloat(servoAngleLimit.textContent);
                    
                    // 讀?�伺?�軸角度（machine.xml ?�接對�??�實?��?構�?序�?
                    const servoAxisAngles = actuators.querySelector('ServoAxisAngles');
                    if (servoAxisAngles) {
                        for (let i = 0; i < 6; i++) {
                            const servo = servoAxisAngles.querySelector(`Servo${i + 1}`);
                            if (servo) {
                                this.params.servoAxisAngle[i] = parseFloat(servo.textContent);
                            }
                        }
                    }

                    // 讀?�伺?��?點�?射表
                    const servoPointMap = actuators.querySelector('ServoPointMap');
                    if (servoPointMap) {
                        if (!Array.isArray(this.params.servoPointMap)) {
                            this.params.servoPointMap = [0, 1, 2, 3, 4, 5];
                        }
                        for (let i = 0; i < 6; i++) {
                            const servo = servoPointMap.querySelector(`Servo${i + 1}`);
                            if (servo) {
                                const val = parseInt(servo.textContent, 10);
                                if (val >= 0 && val <= 5) {
                                    this.params.servoPointMap[i] = val;
                                }
                            }
                        }
                    }
                }
                
                // 讀?��??��???
                const limits = root.querySelector('Limits');
                if (limits) {
                    const fisheyeAngleLimit = limits.querySelector('FisheyeAngleLimit');
                    if (fisheyeAngleLimit) {
                        this.params.fisheyeAngleLimit = parseFloat(fisheyeAngleLimit.textContent);
                    }
                    
                    // 讀?��?點�???
                    const singularity = limits.querySelector('Singularity');
                    if (singularity) {
                        const verticalAngle = singularity.querySelector('VerticalAngle');
                        const horizontalAngle = singularity.querySelector('HorizontalAngle');
                        const extendRatio = singularity.querySelector('ExtendRatio');
                        const contractRatio = singularity.querySelector('ContractRatio');
                        
                        if (verticalAngle) this.params.singularityVerticalAngle = parseFloat(verticalAngle.textContent);
                        if (horizontalAngle) this.params.singularityHorizontalAngle = parseFloat(horizontalAngle.textContent);
                        if (extendRatio) this.params.singularityExtendRatio = parseFloat(extendRatio.textContent);
                        if (contractRatio) this.params.singularityContractRatio = parseFloat(contractRatio.textContent);
                    }
                    
                    const maxPoseLimits = limits.querySelector('MaxPoseLimits');
                    if (maxPoseLimits) {
                        const x = maxPoseLimits.querySelector('X');
                        const y = maxPoseLimits.querySelector('Y');
                        const z = maxPoseLimits.querySelector('Z');
                        const roll = maxPoseLimits.querySelector('Roll');
                        const pitch = maxPoseLimits.querySelector('Pitch');
                        const yaw = maxPoseLimits.querySelector('Yaw');
                        
                        if (x) this.params.maxPoseLimits.x = parseFloat(x.textContent);
                        if (y) this.params.maxPoseLimits.y = parseFloat(y.textContent);
                        if (z) this.params.maxPoseLimits.z = parseFloat(z.textContent);
                        if (roll) this.params.maxPoseLimits.roll = parseFloat(roll.textContent);
                        if (pitch) this.params.maxPoseLimits.pitch = parseFloat(pitch.textContent);
                        if (yaw) this.params.maxPoseLimits.yaw = parseFloat(yaw.textContent);
                    }
                }

                const inverseInput = root.querySelector('InverseInput');
                const coordinateSystem = inverseInput?.querySelector('CoordinateSystem')?.textContent;
                this.coordinateSystem = this.normalizeCoordinateSystem(coordinateSystem);
                this.previousCoordinateSystemBeforeMmf = null;

                const mmf1 = root.querySelector('MMF1');
                const importedMmf1InputMode = this.normalizeMmf1InputMode(mmf1?.querySelector('InputMode')?.textContent);
                if (importedMmf1InputMode !== 'Unknown') {
                    this.savedMmf1InputMode = importedMmf1InputMode;
                    this.selectedMmf1InputMode = importedMmf1InputMode;
                    this.mmf1InputModeConfigLoaded = true;
                }
                this.syncMmf1InputModeConfigUi();
                
                // 套用?�數後更??UI ?��??�繪�?
                this.syncUIValues();
                this.updateUIFromPose();
                this.updatePoseSourceUi();
                this.updateForwardInputSourceUi();
                this.updateAndDraw();
                this.syncForwardKinematicsUI();
                this.updateForwardDisplay();
                
                if (!silent) {
                    alert('機�??�數已�??��?檔�? Par/machine.xml ?��??�入�?);
                }
            }
        }

        // ?��??��???
        window.addEventListener('DOMContentLoaded', async () => {
            await window.Auth.ready();

            if (!window._loggedIn) {
                window.Auth.showPinModal();
                window.addEventListener('wusoem-auth-change', (event) => {
                    const loggedIn = !!(event && event.detail && event.detail.loggedIn);
                    if (loggedIn) {
                        window.location.reload();
                    }
                }, { once: true });
                return;
            }

            const app = new StewartPlatform();
            const keepalive = window.Auth.createKeepalive();
            await app.importParameters({ silent: true });

            // 移除行�?裝置?�制?�板?��??�能，改?�可?��??�容
            const controlPanel = document.querySelector('.control-panel');
            
            if (controlPanel) {

                // ?�螢幕畫布�??��???(?�援?��?/桌�?)
                const fullscreenBtn = document.getElementById('toggleCanvasFullscreen');
                const canvasContainer = document.querySelector('.canvas-container');

                function setCanvasFullscreen(enabled) {
                    if (!canvasContainer) return;
                    if (enabled) {
                        canvasContainer.classList.add('canvas-fullscreen');
                        // 顯示一?�浮?��??��???
                        let closeBtn = document.getElementById('canvasFullscreenClose');
                        if (!closeBtn) {
                            closeBtn = document.createElement('button');
                            closeBtn.id = 'canvasFullscreenClose';
                            closeBtn.className = 'canvas-fullscreen-close';
                            closeBtn.setAttribute('aria-label', '?��??�螢�?);
                            closeBtn.textContent = '??;
                            document.body.appendChild(closeBtn);
                            closeBtn.addEventListener('click', () => setCanvasFullscreen(false));
                        }

                        // ?�新?�?�並?�新調整?��?大�?
                        fullscreenBtn && fullscreenBtn.setAttribute('aria-pressed', 'true');
                        app.resizeCanvas && app.resizeCanvas();
                    } else {
                        canvasContainer.classList.remove('canvas-fullscreen');
                        const closeBtn = document.getElementById('canvasFullscreenClose');
                        if (closeBtn) closeBtn.remove();

                        fullscreenBtn && fullscreenBtn.setAttribute('aria-pressed', 'false');
                        app.resizeCanvas && app.resizeCanvas();
                    }
                }

                if (fullscreenBtn) {
                    fullscreenBtn.addEventListener('click', () => {
                        const isFs = canvasContainer && canvasContainer.classList.contains('canvas-fullscreen');
                        setCanvasFullscreen(!isFs);
                    });

                    // ??�� ESC ?��??�螢�?
                    window.addEventListener('keydown', (ev) => {
                        if (ev.key === 'Escape' || ev.key === 'Esc') {
                            if (canvasContainer && canvasContainer.classList.contains('canvas-fullscreen')) {
                                setCanvasFullscreen(false);
                            }
                        }
                    });
                }
            }
        });

        window.Auth.installBfcacheReload();
    

