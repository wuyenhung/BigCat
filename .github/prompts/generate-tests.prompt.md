---
description: 為運動控制程式碼產生聚焦測試
mode: agent
tools:
  - read_file
  - create_file
  - grep_search
---

Create tests for #selection. Prefer few high-value cases over broad boilerplate: boundary/invalid inputs, CiA402 transitions, PDO data/motion math, safety (limits, stop, fault recovery).

Place under tests/, match existing style.
