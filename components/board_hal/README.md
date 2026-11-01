# `board_hal`

> **Status:** `DESIGNED` — directory created in the repository skeleton; no implementation yet.
> Build files (`CMakeLists.txt`, `idf_component.yml`) are created in TASK-000 from a working
> build — do not write them "from memory".

**Role:** Board support: central I²C manager with a lock, CH422G (EXIO1..5, reset, `USB_SEL`), RGB LCD init, backlight.

| | |
|---|---|
| Tasks | TASK-010 (I²C/CH422G/USB mux), TASK-020 (RGB LCD, stable frame) |
| Specification | 02 §1, §1.1 (pinmap), §1.2, §6; ADR-004, ADR-008, ADR-009 |
| Tests | I²C scan with the expected addresses, confirmed EXIO5, 100× boot, 800×480 test pattern |

**Rules (MUST):**

- `EXIO5=LOW` → USB (not CAN); do not occupy GPIO19/20 for CAN.
- A single owner of the I²C bus; never hold the I²C lock during LVGL animations.
- Do not raise PCLK "blindly"; timings per the Waveshare examples.
- No `lvgl.h` and no `tusb.h` / `tinyusb.h`.

Governing documents: `AGENTS.md`.
