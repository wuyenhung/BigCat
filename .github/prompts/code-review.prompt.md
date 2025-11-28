---
description: 審查 C++ 程式碼的運動控制風險與回歸
mode: agent
tools:
  - read_file
  - grep_search
  - list_code_usages
---

Review #selection or #codebase. Prioritize: motion safety regressions, CiA402 misuse, PDO data consistency, thread/race/jitter risks, resource lifetime issues.

Return concise findings with file+line, impact, and smallest safe fix.
