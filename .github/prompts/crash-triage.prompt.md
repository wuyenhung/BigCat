---
description: "Use when triaging the latest crash dump, minidump, AI crash response, patch dry-run, or dump-analysis workflow. 分析最新 crash dump、AI prompt/response 與修補建議。"
name: Crash Triage
agent: agent
argument-hint: 最新 crash / dump / AI 回應
tools:
  - read_file
  - grep_search
  - run_task
  - get_task_output
  - run_in_terminal
---

Triage the latest crash workflow for this workspace.

Check these artifacts when present:
- `.vscode/last_crash_ai_request.txt`
- `.vscode/crash_ai_response.txt`
- latest dump-analysis output from the existing workspace tasks or scripts

Workflow:
1. Inspect the latest crash-analysis inputs and outputs.
2. Summarize the most likely root cause and the evidence.
3. Identify whether the AI response proposes a safe fix, a partial fix, or a risky fix.
4. If the proposed fix is risky, explain the smallest safer alternative.
5. Recommend the next validation step using existing repo flows such as incremental build, smoke test, or patch dry-run.

Memory preference: read `.github/project-memory/repo-facts.md` first and treat it as the primary durable workflow memory before choosing validation commands.

Execution preference: use workspace tasks first for crash/build/test flows. For smoke validation, run `scripts/run_smoke_tests.ps1` or the workspace smoke task and judge the result from that PowerShell/script flow instead of interactive terminal control. If a terminal invocation is still necessary, use a one-shot `cmd.exe /d /s /c` wrapper and explicitly launch the existing PowerShell scripts rather than relying on interactive PowerShell terminal state.

Artifact hygiene: after successful AI-triggered validation, delete disposable logs, temporary data files, and ad hoc validation artifacts unless they have clear follow-up diagnostic value; keep failure artifacts when they are needed for investigation.

Return:
- root cause
- supporting evidence
- fix assessment
- smallest safe next step
- validation plan
