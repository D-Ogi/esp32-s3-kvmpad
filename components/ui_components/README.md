# `ui_components`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Ready-made LVGL components of the product (without USB/network logic).

| | |
|---|---|
| Tasks | TASK-060D |
| Specification | 07 §7 |
| Tests | SDL snapshot, observer lifecycle (AT-UIFW-09) |

**Target files (07 §2):** `ui_status_item.c`, `ui_topbar.c`, `ui_action_key.c`, `ui_modal.c`, `ui_toast.c`

**Rules (MUST):**

- Dependency exclusively: `ui_components → ui_theme` (+ LVGL).
- Factory functions of 10–50 lines, descriptor tables.
- Code written by hand in C/LVGL — without EEZ Studio, LVGL Pro Editor, XML/JSON DSL (ADR-014).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-020: Keep awake indicator in the topbar — text + icon (FR-712).

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
