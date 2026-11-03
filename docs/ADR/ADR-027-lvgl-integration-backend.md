# ADR-027 — LVGL integration backend: `esp_lvgl_adapter` 0.7.4 (closes the PENDING-HW part of ADR-013)
Status: ACCEPTED (owner, 2026-11-02)
Date: 2026-11-02
Approver: project owner (decision of 2026-11-02, accepted as proposed)

## Context

- ADR-013 (`docs/spec/06`, expanded in `07` §0 and §9) fixed the UI stack on LVGL 9.6 and named `espressif/esp_lvgl_adapter`
  0.7.4 as the proof-of-compatibility candidate with `espressif/esp_lvgl_port` 2.9.0 as a controlled fallback, to be decided
  by TASK-060A with separate binaries and HIL evidence. AGENTS-UI MUST 10 forbids changing the backend outside that task.
- Board: ESP32-S3-Touch-LCD-7B (ADR-026), 1024×600 RGB565 at 30 MHz pixel clock — the panel scans out at **32.745 Hz**
  (`board_lcd`, measured in M1), so no backend can show more than ~32.7 distinct frames per second.
- The RGB panel is created by `board_lcd` (TASK-020, bounce buffers 2 × 10 lines, frame buffers in PSRAM). Both backends
  take the panel handle and its frame buffers; `board_lcd` now allows 1..3 frame buffers.
- Internal RAM is the scarce resource: TinyUSB, the GT911 sampler and the console leave about 137 KB free before LVGL;
  Wi-Fi + TLS (M7/M8) will need a large share of it.

## Measurements (TASK-060A, board #1, firmware `d36ab0a` + uncommitted M0–M5 tree, IDF v6.1, LVGL 9.6.0~1)

Same `ui_platform` code, same scenes (`components/ui_platform/src/ui_bench.c`: KVMpad-like 1024×600 screen; `small` = a
28 px dot + a counter label, `full` = `small` + pad and screen background recolored every frame, `scroll` = a 60-row flex
list auto-scrolling, `idle` = nothing animates), 10 s windows, FPS = `LV_EVENT_RENDER_READY` per second, CPU = busy per
mille per core from the idle tasks, two software draw units, LVGL task priority 4 on core 1. LVGL's own benchmark demo
(16 scenes) run once per binary. Raw logs and SHA256 in `test/reports/2026-11-02_M5_TASK-060A_d36ab0a.yaml`.

| Binary | Panel fbs | `small` fps / render avg | `scroll` fps | `full` fps / render avg / CPU c0,c1 | LVGL demo fps / render / flush | internal free after start | PSRAM free |
|---|---|---|---|---|---|---|---|
| adapter 0.7.4 `TRIPLE_PARTIAL`, 24-line internal strip | 3 | **32.8** / 29.0 ms | 32.8 | **11.6** / 84 ms / 46 %, 95 % | 13 / 81 ms / 22 ms | **63 KB** | 3.07 MB |
| adapter 0.7.4 `DOUBLE_DIRECT` | 2 | **32.8** / 27.9 ms | 32.8 | 8.2 / 120 ms / 76 %, 69 % | 11 / 73 ms / 18 ms | 112 KB | 4.30 MB |
| port 2.9.0 `avoid_tearing` (direct) | 2 | 32.7 / 27.9 ms | 32.7 | 8.1 / 119 ms / 73 %, 71 % | 12 / 73 ms / 17 ms | 118 KB | 4.37 MB |

Also measured: `idle` costs 1.6–2.4 % of core 0 and ~1 % of core 1 in every binary; USB keep-alive traffic (`hid keepalive
on 1`) changes nothing (32.8 / 11.6 fps on the adapter); a 50-line internal strip (100 KB) did **not** fit next to TinyUSB
and the sampler (allocation failed, display registration refused) — 24 lines (48 KB) do; the adapter's own FPS counter
agrees with the event-based one (32 / 11 / 8). Screens captured through the console (`ui snap 4`, decoded by
`test/hil/ui_snapshot.py`) are pixel-identical in layout for both backends.

Found and fixed during the spike (not backend-related): the benchmark rebuilt the screen without deleting the previous one
(internal heap fell from 63 KB to 19 B over seven runs) and the metrics reset in the middle of a cycle produced 14–58 s
"maximum" times; both fixed before the numbers above were taken.

## Options

| | Option | Assessment |
|---|---|---|
| A | `esp_lvgl_adapter` 0.7.4 | **CHOSEN.** Equal to the port on widget updates (both scan-out bound), +42 % on full-screen repaints with `TRIPLE_PARTIAL` (a mode the port does not have), FPS statistics, pause/resume and auto-sleep hooks for FR-7xx power behavior, maintained by Espressif for IDF ≥ 5.5.5. Cost: 9 transitive managed components (button, knob, esp_lv_decoder, esp_lv_fs, freetype, libpng, zlib, qoi, esp_new_jpeg; ~24 KB more flash than the port), version 0.7.x before 1.0 |
| B | `esp_lvgl_port` 2.9.0 | Same numbers as the adapter in direct mode, 6 KB more internal RAM free, two dependencies only, API stable since esp-bsp 1.x. Rejected only because it offers no partial-triple mode and no measurement hooks; it stays the documented fallback |
| C | Own `esp_lcd` ↔ LVGL glue | Rejected (AGENTS-UI MUST 1: do not build a framework; both components already do this correctly) |

## Decision

1. **TECH-089 MUST — backend.** `ui_platform` integrates LVGL 9.6 through `espressif/esp_lvgl_adapter` **0.7.4**
   (`CONFIG_KVM_UI_BACKEND_ADAPTER`). The `esp_lvgl_port` backend source and its pin stay in the tree **until TASK-060E
   (touch bridge) passes HIL on the adapter**, then both are removed in one change; until then nothing outside
   `ui_platform` may reference either component.
2. **TECH-090 MUST — tearing mode.** Default `ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT` with 2 panel frame buffers
   (`CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_DIRECT`): the UI is widget-update bound (32.8 fps either way) and the 49 KB of
   internal RAM and 1.2 MB of PSRAM matter more than full-screen repaint speed. `TRIPLE_PARTIAL` remains a Kconfig flip
   for a later ADR if full-screen transitions above 8 fps become a requirement.
3. **TECH-091 SHOULD — LVGL heap placement.** With the C-library allocator LVGL objects land in internal RAM
   (`SPIRAM_MALLOC_ALWAYSINTERNAL` = 16 KB): the LVGL benchmark demo drove the internal minimum to 364 B on the
   `TRIPLE_PARTIAL` binary and 7.8 KB on the others. TASK-060B must route LVGL's object heap to PSRAM (custom allocator
   on `heap_caps_malloc(MALLOC_CAP_SPIRAM)` or a builtin pool placed in PSRAM) and prove the internal minimum stays above
   the Wi-Fi/TLS budget of M7 (`AT-UIFW-07`).
4. **Ownership stays as in 07 §12.** The LVGL task runs at priority 4 on core 1, below the GT911 sampler and the TinyUSB
   task; the measured `idle` cost (≤ 2.4 % of one core) and the unchanged keep-alive behavior confirm TECH-030/TECH-034
   are not disturbed. Touch is not routed yet (TASK-060E); no HID is emitted by anything in `ui_platform`.
5. `board_lcd` accepts `num_fbs` 1..3 (change approved for TASK-060A inside TASK-010's component); the M1 VSYNC statistics
   are not maintained while a backend owns the panel callbacks, and the raw `lcd <screen>` commands are refused then.

## Consequences

- `dependencies.lock` now pins LVGL 9.6.0~1, the adapter, the port and the adapter's transitive components (list in the
  report and `THIRD_PARTY_NOTICES.md`); the port pin disappears with item 1.
- `sdkconfig.defaults` carries the LVGL configuration (RGB565, FreeRTOS OS layer, two draw units, 16 ms refresh period,
  Montserrat 14/16/20/24, Observer, Snapshot) and, **for the spike only**, `LV_USE_DEMO_BENCHMARK` + `LV_USE_PERF_MONITOR`
  in log mode (one upstream warning in `lv_sysmon.c` with log mode on). TASK-060B turns the demo off.
- The `ui bench` / `ui demo` / `ui snap` console commands and `test/hil/ui_snapshot.py` remain the measurement path for
  `AT-UIFW-07` and the SDL-vs-board comparison of TASK-070D.
- Real-finger latency and the three-mode HIL of M5 are still open: `NOT_RUN_HW` until TASK-060E and a person at the bench.

## Revision

2026-11-02: the requirement IDs were renumbered from TECH-086..088 to TECH-089..091 while PROPOSED, because ADR-026
(ACCEPTED) already registers TECH-085..088 in `docs/EXTENSIONS.md`. The content is unchanged.
