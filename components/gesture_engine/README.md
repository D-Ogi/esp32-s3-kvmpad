# `gesture_engine`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** A pure, deterministic gesture state machine: `TouchSample` → `HidIntent` (tap, long-press, two-finger scroll, double-tap-drag, drag lock).

| | |
|---|---|
| Tasks | TASK-040 |
| Specification | 01 §3 (FR-101..116); 03 §2, §2.1, §8 (VEC-001..004); ADR-007 |
| Tests | AT-GEST-01..12; ≥100k property/fuzz sequences nightly (`test/host_unit/gesture_*.c`, `test/fuzz/`) |

**Rules (MUST):**

- Pure C: without FreeRTOS, ESP-IDF, LVGL, TinyUSB — compiles and is tested on the host.
- Bounded state, zero dynamic allocations.
- Monotonic time only, injected from the outside (TECH-006).

Governing documents: `AGENTS.md`.
