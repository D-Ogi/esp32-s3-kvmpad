# `ui_theme`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Tokens, fonts, LVGL styles, colors, spacing, minimum touch targets.

| | |
|---|---|
| Tasks | TASK-060D |
| Specification | 07 §7; the `reference/kvm-essential-800x480.html` mockup (visually only) |
| Tests | SDL snapshot (AT-UIFW-01) |

**Target files (07 §2):** `include/ui_tokens.h`, `ui_theme.c`

**Rules (MUST):**

- No dependency on USB, networking or pairing logic.
- Fonts/assets statically in flash.

Governing documents: `AGENTS.md`, `AGENTS-UI.md`.
