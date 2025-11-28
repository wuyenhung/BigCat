---
description: 除錯 EtherCAT、CiA402 與運動控制問題
agent: agent
tools:
  - read_file
  - grep_search
  - run_task
  - get_task_output
  - run_in_terminal
  - semantic_search
---

Diagnose the issue in #selection or relevant code. Check: slave discovery/AL state/PDO mapping, CiA402 transitions/fault recovery, motion validation/limits/feedback, thread scheduling/PDO jitter.

Read `.github/project-memory/repo-facts.md` first and treat it as the primary durable workflow memory before choosing validation commands.

Prefer workspace tasks for build/test/ctest flows when available. For smoke validation, run `scripts/run_smoke_tests.ps1` or the workspace smoke task and judge the result from that PowerShell/script flow instead of interactive terminal control. If ad hoc execution is still required, use a one-shot `cmd.exe /d /s /c` wrapper and explicitly launch the repo's existing PowerShell scripts instead of depending on interactive PowerShell terminal state.

After successful AI-triggered validation, delete disposable logs, temporary data files, and ad hoc validation artifacts unless they have clear follow-up diagnostic value; keep failure artifacts when they are needed for investigation.

Return: root cause → evidence → smallest fix → validation steps.
