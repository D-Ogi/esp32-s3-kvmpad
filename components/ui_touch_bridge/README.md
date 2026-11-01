# `ui_touch_bridge`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Router of a single GT911 sample: capture → LVGL indev **and** a stream to GestureFSM, without contention.

| | |
|---|---|
| Tasks | TASK-060E |
| Specification | 07 §6; 03 §2 |
| Tests | `test/ui_touch/`; AT-UIFW-04 (pad + tab bar without a phantom click) |

**Rules (MUST):**

- Does not read GT911 on its own — it uses `touch_input`.
- No access to the USB stack.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
