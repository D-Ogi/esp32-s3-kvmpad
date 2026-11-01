# `ui_views`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** 800×480 screens — exclusively composition of components + local view state.

| | |
|---|---|
| Tasks | TASK-070A (Mouse), TASK-070B (Keyboard), TASK-070C (Shortcuts/Settings/status) |
| Specification | 01 (screen, gestures, keys); 07 §2, §7; AGENTS-UI.md |
| Tests | AT-UI-01..08, AT-KEY-01..10, AT-UIFW-01/04/08 |

**Target files (07 §2):** `ui_view_mouse.c`, `ui_view_keyboard.c`, `ui_view_shortcuts.c`, `ui_view_settings.c`

**Rules (MUST):**

- Dependencies: `ui_views → ui_components → ui_theme`, `ui_views → ui_core`.
- No view callback emits HID — only a `UiAction` to the typed queue.
- Code written by hand in C/LVGL — without UI generators and without `ui_generated/` (ADR-014).

**Extensions (`docs/EXTENSIONS.md`):**

- ADR-018: the "Media" group in SHORTCUTS (FR-311).
- ADR-020: Keep awake toggle (FR-711).
- ADR-022: Settings → Remote screens: *Pair client* (DEVICE name, `ddd ddd` code in a large font, countdown, Accept/Reject
  with the client name) and *Paired clients* (list, Remove, Remove all) — FR-722/725/726.
- ADR-023: Settings → Remote: `Require accept when pairing` (ON), `Control approval: Always / Trusted clients / Auto` (Always),
  with a one-sentence description of the effect; Settings → Shortcuts: Alt+F4 confirmation (FR-731..734). The policy is visible in the status (FR-735).
- ADR-024: the keyboard renders legends and geometry from `kbd_layouts` for the selected `Host layout` (FR-741/742) and shows the
  layout label (FR-508) with the "set the same layout on the HOST" hint; composition mode with preview, candidate bar and the
  Japanese flick grid (FR-746); Settings → Keyboard: `Host layout`, `Unicode input on HOST` with a setup hint per mode (FR-747);
  SHORTCUTS: "Switch HOST layout" (FR-744).
- ADR-025: `Settings → About` — firmware version, license + Required Notice, scrollable third-party notices generated from
  `THIRD_PARTY_NOTICES.md` at build time (FR-751); a plain view without HID or network.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
