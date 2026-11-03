# `board_hal`

> **Status:** `IMPLEMENTED_SW` + bench-verified on board unit #1 (7B, 2026-11-02) — TASK-010 / TASK-020.
> Hardware results in `hardware/boards.md` and `test/reports/`; `PASS` only where a test was run on the board.

**Role:** Board support for the **ESP32-S3-Touch-LCD-7B** (ADR-026): one I²C master bus with a recursive lock, the
CH32V003 I/O expander at `0x24` (EXIO1..6 shadow + read-back, `USB_SEL`, resets, panel supply, inverted backlight PWM),
the RGB LCD (`board_lcd.*`: PSRAM frame buffer(s), bounce-buffer variants, VSYNC and scan-out phase telemetry).

| | |
|---|---|
| Tasks | TASK-010 (I²C / expander / USB mux), TASK-020 (RGB LCD, stable frame) |
| Specification | 02 §1, §1.1, §1.2, §6; ADR-004, ADR-008, ADR-009; **ADR-026** (7B: TECH-085..088) |
| Tests | host `test/host_unit/test_io_ext.c`, `test_lcd_pattern.c`; HIL I²C scan, EXIO5 read-back, 100× boot, A/B/C bench, glitch + stress (AT-UI-06, AT-FAULT-03) |

| File | Content |
|---|---|
| `include/board_pins.h` | 7B pin map, expander registers, panel timing — each value with its source |
| `include/io_ext.h`, `src/io_ext.c` | pure shadow logic: safe default, EXIO5 = HIGH refused, read-back compare, inverted PWM |
| `src/board_hal.c` | bus + expander driver; refuses to return OK unless the read-back confirms EXIO5 = LOW |
| `src/board_lcd.c` | `esp_lcd` RGB panel, stats (`vsync_*`, `phase_*` = VSYNC → last bounce chunk copied) |

**Rules (MUST):**

- `EXIO5=LOW` → USB (not CAN); USB is started only after the read-back confirms it; never start from an all-HIGH shadow.
- The expander MCU is not reset with the ESP32-S3: always re-apply and verify the safe byte at init.
- A single owner of the I²C bus; never hold the I²C lock during LVGL animations.
- Do not raise PCLK "blindly"; the timing is the vendor's (ADR-026 TECH-087).
- `CONFIG_LCD_RGB_RESTART_IN_VSYNC` stays **off**: with it on, one late bounce refill left the picture permanently shifted by
  one bounce block (measured; see `sdkconfig.defaults` and the TASK-020 report).
- No `lvgl.h` and no `tusb.h` / `tinyusb.h`.

Governing documents: `AGENTS.md`.
