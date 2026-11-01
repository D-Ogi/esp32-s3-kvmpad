# `ui_core`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Typed state mailbox, `ui_store` (presentation projection), router, overlay, lifecycle, typed actions.

| | |
|---|---|
| Tasks | TASK-060B (store/subjects/mailbox), TASK-060C (router/overlay) |
| Specification | 07 §1–5; AGENTS-UI.md |
| Tests | `test/ui_store/`, `test/ui_nav/`; AT-UIFW-02/03/08/09/10 |

**Target files (07 §2):** `include/ui_core.h`, `ui_runtime.c`, `ui_store.c`, `ui_router.c`, `ui_overlay.c`, `ui_actions.c`

**Rules (MUST):**

- Data binding through the native LVGL Observer — without a custom Redux/DOM/DSL.
- The UI is not the source of truth for HOST_ACTIVE, CLIENT_CONNECTED, ownership, Caps LED, drag lock, DOWN keys.
- No `tusb.h` / `tinyusb.h` and no network transport headers.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
