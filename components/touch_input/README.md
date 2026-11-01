# `touch_input`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** The single reader of GT911: 0..5 points, IRQ → notification, `track_id`, 800×480 mapping, error recovery.

| | |
|---|---|
| Tasks | TASK-030 |
| Specification | 02 §5; 03 §2 (TouchInput); 07 §6 |
| Tests | AT-GEST-07 (precondition), AT-FAULT-01, I²C error injection |

**Rules (MUST):**

- `ESP_LCD_TOUCH_MAX_POINTS=5`; `esp_lcd_touch_gt911` in a pinned version — check the headers.
- Zero `malloc/free`, logs and `printf` per sample.
- A read error > 120 ms ends the active gesture (TECH-064).
- Consumers (LVGL, GestureFSM) receive data exclusively through `ui_touch_bridge` — no second reader.

Governing documents: `AGENTS.md`.
