# `ui_platform`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** LVGL ↔ `esp_lcd` integration: the chosen adapter, display init, lock, render metrics. The only owner of the LVGL context.

| | |
|---|---|
| Tasks | TASK-060A |
| Specification | 07 §0 (ADR-013), §9; 02 §2, §6 |
| Tests | separate binaries for `esp_lvgl_adapter` 0.7.4 and the `esp_lvgl_port` 2.9.0 fallback; benchmark without and with USB load; HIL |

**Rules (MUST):**

- The adapter is chosen only in TASK-060A after a build + HIL and recorded in ADR-013 — do not change it "on the fly".
- The LVGL lock does not cover I²C, TLS, USB or NVS.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
