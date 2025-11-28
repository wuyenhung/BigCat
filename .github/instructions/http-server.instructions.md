---
description: "Use when editing HttpServer, HTTP API handlers, auth/session logic, static file serving, SSE, or auto-flow routes in src/server. Covers AppController routing, security-policy references, and response-handling conventions."
name: "HTTP Server Guidelines"
applyTo:
  - "src/server/**"
  - "include/HttpServer.h"
---

# HTTP Server Guidelines

- Route lifecycle and motion-affecting actions through `AppController` or existing API helper seams instead of rebuilding command policy inline in handlers.
- Prefer extending existing helpers in `HttpServer.API.Helpers.cpp` and related response-planning seams before adding parallel parsing or routing logic.
- Keep HTTP policy checks centralized. If an auth, rate-limit, or auto-flow guard already exists, extend that seam instead of duplicating state checks in each route.
- Preserve the split between auth/session handling, API routing, and static-file serving; do not mix those concerns in one helper unless the existing design already does.
- For network hardening and connection policy, reference the existing docs instead of copying policy text: `docs/SATURATION_ATTACK_PROTECTION.md`, `docs/SATURATION_ATTACK_TESTING.md`, and `docs/SINGLE_CONNECTION_PER_IP.md`.
- Prefer returning through existing response helpers and metadata plans rather than hand-assembling ad hoc JSON or status-code branches.
- When changing parsing or routing behavior, add or update focused tests under `tests/` if a matching seam already exists.
- Validate server-side behavior with `scripts/build_incremental-Crash.ps1`; run smoke tests when HTTP commands, auth/session behavior, or auto-flow routes change.
