# `touch_input`

> **Status:** `IMPLEMENTED_SW` + bench-verified on board unit #1 (7B, 2026-11-02) — TASK-030.
> Multi-finger (2..5 points) trials need a person at the panel; see `test/reports/` for what was and was not run.

**Role:** The single reader of GT911: 0..5 points, IRQ → task notification, stable `track_id` order, explicit transform to
1024×600 (ADR-026 TECH-088), error recovery with backoff, a cancel sample on any error or a > 120 ms gap.

| | |
|---|---|
| Tasks | TASK-030 |
| Specification | 02 §5; 03 §2 (TouchInput); 07 §6; ADR-026 |
| Tests | host `test/host_unit/test_touch_sample.c` (incl. a seeded property test); HIL fault injection (`touch fault N`), AT-FAULT-01 |

| File | Content |
|---|---|
| `include/kvm_touch.h`, `src/touch_sample.c` | contract types + pure GT911 frame decoder (bounds, duplicates, sort by `track_id`) |
| `include/touch_input.h`, `src/touch_input.c` | address detection, the sampler task, queue (drop-oldest), stats, fault injection |

**Rules (MUST):**

- `ESP_LCD_TOUCH_MAX_POINTS=5`; `esp_lcd_touch_gt911` 1.2.1 is used for init and IRQ registration only — the frame is read
  by this component (status `0x814E`, points `0x814F`, then the status is cleared).
- Zero `malloc/free`, logs and `printf` per sample.
- A read error or a gap > 120 ms ends the active gesture (TECH-064): a `valid=false` sample is published at once.
- Consumers (LVGL, GestureFSM) receive data exclusively through `ui_touch_bridge` — no second reader.

Governing documents: `AGENTS.md`.
