---
description: "Use when editing EtherCATMaster, MotionController, PDOFilter, StewartIK, CiA402 state machine, or PDO/cyclic logic in src/ethercat and src/motion. Covers PDO cycle safety, CiA402 sequencing, CRITICAL_SECTION lock ordering, CSV mode, and drive-specific SDO differences."
name: "EtherCAT & Motion Guidelines"
applyTo:
  - "src/ethercat/**"
  - "src/motion/**"
  - "include/EtherCATMaster.h"
  - "include/MotionController.h"
  - "include/MotionLogic.h"
  - "include/PDOFilter.h"
  - "include/PDOHelpers.h"
  - "include/PDOModel.h"
  - "include/StewartIK.h"
  - "include/EsiParser.h"
---

# EtherCAT & Motion Guidelines

- Treat PDO cycle stability as a first-order constraint: no blocking calls, heap churn, console output, or long mutex holds on the cyclic path. Pre-allocate cycle-path buffers.
- Preserve existing CPU affinity, thread priority, timer, and scheduling tuning unless the task explicitly targets them and validates cycle stability.
- Preserve the split-file structure (`EtherCATMaster.Init/Cyclic/SDO/Simulation/MMF/EMCY/Diagnostics.cpp`); extract named helpers instead of collapsing logic.
- CiA402 transitions in standard order, never skip controlword sequencing. Enable: Fault Reset → Shutdown → Switch On → Enable Operation. Stop: Quick Stop / Disable Operation first, then remove torque after state confirms safe.
- Treat statusword, operation mode, target/actual position, and fault code as one set; on fault log controlword/statusword/mode/alarm together.
- CSV (mode 9) is the default cyclic operation mode; `0x60FF` is a closed-loop velocity command derived from filter feedforward plus bounded `(filtered target - actual position)` correction, not a pure position-error loop.
- Respect the `CRITICAL_SECTION` lock-ordering hierarchy documented in `include/EtherCATMaster.h` (7 sections). Do not introduce a new acquisition order that conflicts.
- Drive differences: DVS (vid 0x445653) torque-limit SDOs `0x60E0`/`0x60E1`; X1S (vid 0x766) uses `0x2019`/`0x201A` (same PDO byte layout). Use `IsDvsDrive()`/`IsX1sDrive()`.
- No motion commands before Operation Enabled + safety checks pass. For CSP/CSV/CST/homing, verify mode-switch acknowledgement before cyclic commands.
- Simulation mode must mirror the real PDO pipeline behavior (`pdoPipelinePrimed_`, `pdoCycleEvent`, staged outputs, MMF polling); do not add simulation-specific branches in `PDOFilter.cpp`.
- Validate with `scripts/build_incremental-Crash.ps1`; run `scripts/run_smoke_tests.ps1` when PDO filter, CiA402, or cyclic logic changes.
