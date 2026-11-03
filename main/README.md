# `main/`

Firmware entry point (`app_main.c`) — only the assembly of components and the start of tasks.
Spec: 02 §3, §4, §11; 05 TASK-000. Status: M0–M1 bring-up firmware (`IMPLEMENTED_SW`, run on board unit #1).

| File | Content |
|---|---|
| `app_main.c` | order: board HAL (expander safe state verified) → USB only if EXIO5 = LOW is confirmed → NVS boot counter → LCD + diagnostics → touch → `BOOT_OK` → console |
| `kvm_notice.c/h` | ADR-025 TECH-084: retained `.rodata` string with the SPDX id and the Required Notice (`strings kvmpad.bin`, AT-EXT-29); boot counter |
| `kvm_diag.c/h` | M1 diagnostics: the single display owner task, test screens, touch capture, CPU load, TECH-020 bench (`lcd bench`, `lcd variant`, `lcd drawload`), fault injection (`lcd glitch`), stress (Wi-Fi scan + NVS + flash + PSRAM) |
| `kvm_console.c/h` | UART console (`help` lists the commands); used by `test/hil/kvm_serial.py` |
| `ime_glue.c/h` | TASK-072: priority-1 worker that mounts `ime_data`, maps and validates the hanzi font (`ui_font_blob`) and opens PinyinIME; the shell's 100 ms LVGL timer then installs the font (`ui_theme_set_hanzi_font`) and the decoder (`ui_app_set_zh_engine`) once. A missing or bad image leaves the ZH layer disabled |
| `lcd_pattern.c/h` | pure RGB565 test patterns (bars, ramps, swatches, alignment frame, 5×7 font); host test `test_lcd_pattern.c` |

The diagnostics are bring-up tooling for M1. They never emit HID and are replaced by the UI of M3 (TASK-060..070);
a release build disables them with the rest of the debug surface (TECH-047: "debug disabled in production").
