# `update_manager`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Signed OTA, maintenance mode, health-check, rollback/recovery.

| | |
|---|---|
| Tasks | TASK-110 |
| Specification | 02 §9 (TECH-051..055); ADR-009 (flash size → partitions), ADR-012 |
| Tests | AT-FAULT-07/08, AT-PERF-09, AT-SEC-09 |

**Rules (MUST):**

- Before OTA: `RELEASE_ALL`, disconnecting the client, input lock, `Updating` state.
- The partition table only after the flash has been confirmed (ADR-009, `hardware/boards.md`).
- Secure Boot / Flash Encryption (eFuse) exclusively by a physical operator.

Governing documents: `AGENTS.md`.
