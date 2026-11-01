# `diagnostics`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Metrics, error counters, bounded ring log, diagnostics screen.

| | |
|---|---|
| Tasks | TASK-120 (+ support for TASK-030/080) |
| Specification | 02 §10 (TECH-060..064), §11.2 |
| Tests | AT-PERF-01..09 |

**Rules (MUST):**

- Metrics from TECH-061 (`touch_samples`, `hid_release_all`, `touch_to_hid_us`, …).
- Never log raw keys or full payloads; mask tokens.

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-022: counters `pair_attempts`, `pair_failures`, `pair_lockouts` (SEC-104); the pairing code, the PAKE key and the token never in logs (SEC-105).

Governing documents: `AGENTS.md`.
