# WuSOEM_C++ Copilot Instructions

## Project
EtherCAT 6-DOF Stewart Platform motion control — C++17, Win32, SOEM, CiA402/DS402.
Prefer concise answers and minimal code changes. Do not repeat rules from this file in prompts.
This repo runs on Windows 10 rather than an RTOS; for any code change, evaluate near-real-time PDO synchronization impact first and avoid weakening the existing timing behavior unless the task explicitly requires it.

## Project Memory
- Repository-owned AI memory lives in `.github/project-memory/`.
- Read `.github/project-memory/repo-facts.md` before substantial code changes, architecture work, or build/test workflow edits.
- Read repo memory selectively: start with the top repo-facts section and targeted searches for the touched subsystem instead of loading the whole file when a narrow fact lookup is enough.
- Before AI-triggered build/test/triage work, read `.github/project-memory/repo-facts.md` first and prefer it over transient/local memory when choosing validation commands, artifact retention, or workflow policy.
- On the first substantive AI interaction in a new workspace/session for this repo, inspect `/memories/repo/` and migrate valuable, verified repo-level facts into `.github/project-memory/repo-facts.md` before doing substantial work.
- When verified repo-level knowledge changes, update `.github/project-memory/repo-facts.md` in the same patch.
- During normal work, treat newly verified repo facts as requiring a `.github/project-memory/repo-facts.md` update before the task is considered complete.
- Prefer `.github/project-memory/` over transient agent memory when there is overlap.
- Keep only durable repo facts in `.github/project-memory/repo-facts.md`; do not copy transient plans, speculative notes, or user-global preferences.
- If a user command or requested change appears to conflict with established repo memory or project rules, ask the user to confirm before proceeding instead of silently overriding the existing guidance.
- If a candidate memory is ambiguous, conflicts with existing repo facts, or appears to suggest a better but not yet validated approach, ask the user before recording it in `.github/project-memory/repo-facts.md`.
- If `/memories/repo/` contains overlapping or stale notes, migrate verified facts into `.github/project-memory/repo-facts.md` and avoid keeping two active copies.
- Cross-machine persistence requires committing and pushing `.github/project-memory/` and related `.github` instruction files; local Copilot memory alone is not sufficient.

## AI Agent Workflow
1. Read the local repo memory and the nearest owning code/doc surface first, then form one local hypothesis and one cheap validation check before editing.
2. Keep the AI role focused on code reading, reasoning, patching, and result interpretation. Prefer repository-local PowerShell scripts for build/test/triage execution instead of ad hoc terminal control or broad MCP/remote orchestration.
3. Use targeted searches and ranged file reads; avoid loading broad files or full logs into chat when a script summary, grep, or saved log tail is enough.
4. Make the smallest root-cause edit that can be validated, then run the focused check immediately before widening scope.
5. When behavior, workflow, build/test policy, or durable repo knowledge changes, update the relevant docs or `.github/project-memory/repo-facts.md` in the same patch.
6. After every AI-triggered test/build run, including interrupted or terminated runs, remove disposable logs, temp data, and ad hoc validation files unless they are needed for failure diagnosis or were explicitly requested as retained artifacts.

## Architecture
- `AppController` is the single entry point for lifecycle, HTTP, and auto-flow coordination. Prefer routing UI/HTTP commands through `AppController` instead of reassembling flows in new call sites.
- Keep module boundaries explicit: `src/ethercat` for bus/PDO/SDO work, `src/motion` for CiA402 and kinematics, `src/server` for HTTP/auth/static content, `src/ui` for Win32 orchestration and controls.
- Preserve the current split-file structure in large areas such as `MainWindow.*.cpp`; prefer extracting named helpers over moving logic back into monolithic files.
- Link to existing docs instead of duplicating them: `docs/PROJECT_STRUCTURE_GUIDE.md`, `docs/APP_CONTROLLER_INTERFACE_CONTRACT.md`, `docs/ARCHITECTURE_OPTIMIZATION_PLAN.md`, `docs/StewartIK_Manual.md`.

## Code Priorities
1. Fix root causes, not symptoms.
2. Keep public behavior stable unless the task requires change.
3. Modern C++17 and RAII. CamelCase for classes, snake_case for functions/variables.
4. Focused edits over broad refactors. Preserve crash handling and diagnostics.

## Key Files
include/EtherCATMaster.h, MotionController.h, StewartIK.h, CrashHandler.h; src/

## Build And Test
- Default build validation: `scripts/build_incremental-Crash.ps1` or the workspace task `Build: Incremental Crash`.
- Use `scripts/build_ForceFull.ps1` only for stale-cache or hard build failures.
- Use `scripts/build_release_hardened.ps1` only for release/performance tasks.
- For regression validation, use `scripts/run_smoke_tests.ps1` or the workspace task `Smoke Test`.
- For AI-triggered smoke validation, invoke `scripts/run_smoke_tests.ps1` or the workspace smoke task and judge the result from that PowerShell/script flow; do not drive smoke runs through interactive terminal control.
- For AI-triggered execution inside VS Code, prefer the existing workspace tasks (`Build: Incremental Crash`, `CTest: Run Smoke`, `CMake: Configure`, `CMake: Build Release`) over direct integrated-terminal sessions, because this repo's Copilot/VS Code PowerShell terminal runs can hang or hide completion.
- If no suitable task exists and terminal execution is still required, prefer a one-shot `cmd.exe /d /s /c` wrapper that explicitly launches an existing repository PowerShell script with `-NoLogo -NoProfile -ExecutionPolicy Bypass -File` rather than depending on interactive PowerShell terminal state.
- Prefer focused executable checks first, then incremental build, then smoke tests, and reserve full/release hardened builds for stale-cache, packaging, or performance-sensitive tasks.
- After AI-triggered validation finishes or is stopped, delete disposable smoke logs, temporary data files, and ad hoc validation artifacts unless they have clear follow-up diagnostic value. Keep failure logs/artifacts when they are needed for investigation.
- VS Code diagnostics can report false include-path or incomplete-type errors in this repo; treat the incremental build plus smoke tests as the source of truth.

## Conventions
- Keep crash-analysis automation working. Scripts under `scripts/` are intended to run from any working directory, so preserve `PSScriptRoot`/project-root based path resolution and avoid VS Code-only assumptions.
- Prefer updating existing helpers and seams that already have tests instead of introducing parallel code paths. This repo has active extraction work in `MainWindow.*`, HTTP API helpers, and AppController seams.
- When editing HTTP or auto-flow behavior, check whether an existing contract doc or helper already owns that policy before adding new state checks inline.
- Save source files, headers, scripts, docs, JSON/XML/config text, and other project text files as UTF-8 without BOM only. If a file contains Chinese text, it must remain UTF-8 without BOM to avoid mojibake; do not re-save project files as ANSI, Big5, UTF-8 BOM, or other locale-dependent encodings.
- Prefer UTF-8 without BOM for exchanged text payloads and persisted project text. For Win32 UI strings, continue using wide strings / UTF-16 conversion helpers where the Windows API requires them.

## Encoding
- MSVC builds in this repo rely on UTF-8 source handling (`/utf-8` in CMake). Treat UTF-8 without BOM as the repository default for edited text files.
- If comment text is damaged, fix the file encoding or rewrite the comment text directly; do not leave mojibake in committed sources.

## EtherCAT / CiA402 Rules
- Verify slave discovery, state, and PDO mapping before enabling motion.
- Keep PDO layout, scaling, and endian assumptions explicit and consistent.
- CiA402 transitions in standard order; never skip controlword sequencing.
  Enable: Fault Reset → Shutdown → Switch On → Enable Operation.
  Stop: Quick Stop / Disable Operation first, then remove torque after state confirms safe.
- Treat statusword, operation mode, target/actual position, and fault code as a coherent set. On fault, log controlword, statusword, mode, and alarm together.
- No motion commands before Operation Enabled + safety checks pass. For CSP/CSV/CST/homing, verify mode switch acknowledgement before cyclic commands.

## Windows Threading (PDO Cycle Stability)
- Separate EtherCAT cyclic work from UI, logging, HTTP, and I/O threads.
- No blocking calls, heap churn, console output, or long mutex holds in the PDO cycle. Pre-allocate all cycle-path buffers.
- Use high-resolution timers or measured deadlines; record jitter. Pin cyclic thread to isolated cores; raise priority carefully.
- Use lock-free handoff between cyclic thread and slower subsystems. Measure cycle deviation before changing affinity/priority.
- Treat PDO synchronization and near-real-time behavior as a first-order constraint for all edits, not only EtherCAT-specific files; avoid introducing extra latency, contention, scheduling drift, or background work on paths that can influence the cyclic thread.
- Preserve existing CPU affinity, thread priority, timer, and scheduling tuning unless the task explicitly targets those areas and the change is validated against cycle-stability expectations.

## Testing and Safety
- Test motion with small amplitude first; verify EtherCAT communication and servo enable before motion.
- Enforce software limits; handle emergency stop as a first-class path.
- Use status logging and crash artifacts for diagnosis.
- For network/auth hardening or connection policy changes, reference `docs/SATURATION_ATTACK_PROTECTION.md`, `docs/SATURATION_ATTACK_TESTING.md`, and `docs/SINGLE_CONNECTION_PER_IP.md` instead of restating those rules here.
