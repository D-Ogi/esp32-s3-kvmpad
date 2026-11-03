# ADR-035 — Scan-out stability: DOUBLE_PARTIAL default, chained panel telemetry, LCD_CAM stop on restart

Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (accepted after the visual check of view switches on the -O2 `DOUBLE_PARTIAL` image: no jump)
Amends: ADR-027 item 2 (TECH-090 tearing mode) and item 5 (VSYNC statistics while a backend owns the panel)

## Context

On board #1 the owner saw the picture jump up and down by 10-15 px several times within 0.5-1 s after every view
switch (mouse, keyboard, shortcuts). Cause, from the pinned ESP-IDF v6.1 RGB driver (`esp_lcd_panel_rgb.c`):

- The panel is fed through two 10-line bounce buffers in internal RAM. The GDMA end-of-frame interrupt refills one
  buffer from the frame buffer in PSRAM per 10 lines (about 0.45 ms at 30 MHz PCLK).
- In `DOUBLE_DIRECT` LVGL redraws a whole screen straight into the second PSRAM frame buffer, and the adapter then
  copies the dirty areas from one PSRAM frame buffer to the other. While that runs the refill is starved; when two
  end-of-frame events fall into one interrupt, one is lost.
- At VSYNC `lcd_rgb_panel_try_restart_transmission()` sees `bb_eof_count < expect_eof_count` and restarts the DMA.
  That frame is shifted by one bounce chunk (10 lines) and never reports "frame complete". Each lost event is one
  visible jump.

Two facts made this hard to see. First, the adapter's call to `esp_lcd_rgb_panel_register_event_callbacks()` replaces
the whole callback set, so `board_lcd`'s VSYNC and frame-complete counters stopped as soon as LVGL started (ADR-027
item 5). Second, the jump lasts one frame, so screenshots never show it.

A related finding of TASK-110: a requested restart (`esp_restart()`) often ended through the RTC watchdog backstop
(`rst:0x10 RTCWDT_RTC_RST`, about 2 s late) instead of a CPU reset: 3 of 6 runs on board #1. `esp_restart_noos()`
resets GDMA but not LCD_CAM, so the scan-out keeps running into the restart. External reports of the same symptom on
ESP32-S3 boards with a 7-inch RGB panel: github.com/ericdahl-dev/phrasedeck/issues/150 (4/10 watchdog resets, 0/10
with LCD_CAM stopped and reset in a shutdown handler), github.com/prestonbrown/helixscreen/issues/1752.

## Measurements (board #1, 2026-11-02)

Each row: 8 view switches through `ui nav` (keyboard, mouse, shortcuts, mixed order), `lcd stats reset` 3 s after
each switch. `missed` = VSYNC count minus completed frames, which counts the DMA restarts described above. LCD
timing unchanged (1024x600, 30 MHz PCLK, 32.746 Hz, 2 frame buffers, 10-line bounce buffers).

| Variant | missed (8 switches) | VSYNC interval min..max | render avg / max | internal RAM free |
|---|---|---|---|---|
| `DOUBLE_DIRECT`, 24-line setting, -Og (ADR-027 default) | **32** (3-5 per switch) | 29.4..31.7 ms | 159 / 247 ms | 72 KB |
| `DOUBLE_PARTIAL`, 12-line internal strip, -Og | **0** | 30.49..30.59 ms | 265 / 454 ms | 48 KB |
| `DOUBLE_PARTIAL`, 12-line internal strip, -O2 | **0** | 30.52..30.56 ms | 228 / 403 ms | 52 KB |

Idle (no switch) is `missed=0` in every variant. One draw unit instead of two did not change the `DOUBLE_DIRECT`
render time (241 / 88 / 134 ms for keyboard / mouse / shortcuts), so the render is bound by PSRAM bandwidth, not by
the CPU. Restart with the LCD_CAM shutdown handler: 8 of 8 runs `rst:0xc RTC_SW_CPU_RST` (4 from a task pinned to
core 1, 4 from the console), on the same configuration that gave 3 of 6 `rst:0x10` before. Report:
`test/reports/2026-11-02_M5_ui-scanout-drift_d36ab0a.yaml`.

## Decision

1. **TECH-090 (amended) — tearing mode.** Default `ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL`
   (`CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL`) with 2 panel frame buffers and a partial draw buffer of
   `CONFIG_KVM_UI_DRAW_BUF_LINES` = 12 lines in internal RAM (24,576 B, one buffer, allocated by the adapter). LVGL
   renders into internal RAM and the adapter writes each strip once into the PSRAM frame buffer, so the bounce refill
   keeps its share of PSRAM bandwidth. `DOUBLE_DIRECT` stays a Kconfig choice for measurements.
2. **Panel telemetry survives the backend.** `board_hal` links with
   `-Wl,--wrap=esp_lcd_rgb_panel_register_event_callbacks`. For the `board_lcd` panel the wrapper stores the caller's
   callbacks and context and installs `board_lcd`'s IRAM trampolines, which count and then forward. `on_color_trans_done`
   and `on_bounce_empty` are installed only when the caller set them (a non-NULL `on_bounce_empty` replaces the driver's
   own refill). The IRAM and internal-RAM checks of the driver are applied to the chained set. `lcd stats` prints
   `missed`. ADR-027 item 5 no longer applies to the statistics; the raw `lcd <screen>` commands stay refused while a
   backend owns the panel.
3. **LCD_CAM stops before a requested restart.** `board_lcd` registers an `esp_register_shutdown_handler()` that calls
   `lcd_ll_stop()` and resets LCD_CAM under the RCC lock (register access only: no lock wait, no heap). Panic and
   watchdog resets skip shutdown handlers and are not covered.
4. **Acceptance metric.** A UI or memory change that touches rendering or PSRAM traffic is checked on the board with
   `lcd stats` across view switches: `missed` must stay 0.

## Consequences

- A full view switch renders about 100 ms slower (-Og: 265 ms instead of 159 ms on average). The picture no longer
  jumps. The switch latency stays below the 0.5-1 s the jumps took before.
- Internal RAM: 24,576 B more in use (free 48-52 KB instead of 72 KB in steady state). The low-water mark after the
  LVGL benchmark scenes was about 10 KB, the same class as earlier runs (M5 demo 7.8 KB, M6 13.7 KB). The strip cannot
  move to PSRAM: rendering outside PSRAM is the point. The internal margin stays the tightest resource (ADR-027 item 3,
  AT-UIFW-07) and is rechecked at the release gate with Wi-Fi, TLS and the IME loaded.
- `CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2) gives another 14 % on render time and built clean after five `-Werror`
  fixes (one real defect: the pairing handlers wrote through a NULL `out`, now `KVM_PAIR_ERR_ARG`). Owner's decision
  (2026-11-02): -O2 is enabled for release builds only; development builds stay at -Og (`CONFIG_COMPILER_OPTIMIZATION_DEBUG`)
  for debugging. The release gate builds and checks the -O2 image (`lcd stats` `missed` = 0 across view switches).
- `sdkconfig.defaults` changes with this ADR: `CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL=y`,
  `CONFIG_KVM_UI_DRAW_BUF_LINES=12`.
- The owner checked view switches on the -O2 `DOUBLE_PARTIAL` image: no jump. Not verified yet: behavior during OTA and
  NVS writes with `DOUBLE_PARTIAL` (the ADR-034 rule keeps the cache on, so no change is expected).
