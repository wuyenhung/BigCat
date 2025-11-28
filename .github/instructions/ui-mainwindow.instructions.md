---
description: "Use when editing MainWindow, AutoWindow, Win32 UI orchestration, connect/disconnect flow, or busy/auto status handling in src/ui. Covers helper extraction, state ordering, and validation for MainWindow-heavy changes."
name: "UI MainWindow Guidelines"
applyTo:
  - "src/ui/**"
  - "include/MainWindow*.h"
  - "include/MainWindow_Internal.h"
  - "include/AutoWindow.h"
---

# UI MainWindow Guidelines

- Preserve the split-file structure in `MainWindow.*.cpp`; do not collapse logic back into a monolithic file.
- Treat `MainWindow` methods as orchestration-first. Prefer extracting named helpers beside the current file over adding more inline branches.
- Reuse existing state helpers and cleanup seams before introducing new flags or duplicated reset logic.
- Keep UI command entry aligned with `AppController` contracts. Do not create new UI-side flows that bypass the controller for lifecycle, HTTP-triggered, or auto actions.
- Preserve message-post ordering, guard lifetimes, and busy-flag cleanup order in connect, disconnect, stop, and auto flows.
- If a change touches connect/disconnect, auto procedures, jog, stop, or fault reset paths, prefer incremental extraction with behavior-preserving edits.
- Avoid introducing blocking work, extra heap churn, or long lock holds on paths that interact with EtherCAT cyclic activity.
- Validate UI behavior changes with `scripts/build_incremental-Crash.ps1` or the `Build: Incremental Crash` task. Run smoke tests as well when the edit changes connect/disconnect, stop, or auto-flow behavior.
