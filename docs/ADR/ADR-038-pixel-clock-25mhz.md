# ADR-038 — RGB pixel clock 25 MHz (27.3 Hz scan-out) for bounce-buffer headroom under touch

Status: REJECTED (owner, 2026-11-03): at 25 MHz the scrim under modals flickers constantly; the pixel clock stays at the vendor's 30 MHz. The measurements below stay as the record; the touch-time jumps are open again.
Date: 2026-11-02
Approver: project owner (finger test on board #1 at 25 MHz: 25 -> 2 missed frames; then rejected for the flicker under modals)
Amends: ADR-035 (scan-out stability), TASK-020 timing (`BOARD_LCD_PCLK_HZ`)

## Context

After ADR-035 the picture no longer jumped on view switches, but on board #1 the owner still saw jumps while a
finger was on the screen during rendering: holding the language key until its picker opened, holding `F1-F12`,
opening and closing modals. Measurements with `lcd stats` once per second (`missed` = VSYNC count minus completed
frames, i.e. restarted DMA frames), `DOUBLE_PARTIAL`, 12-line strip, -Og:

| Case (board #1) | 30 MHz (32.7 Hz) | 25 MHz (27.3 Hz) |
|---|---|---|
| 8 view switches from the console, no touch | 0 | — |
| 3 modal open/close from the console, no touch | 0 | — |
| 70 s owner hold sequence (F1-F12 tap and hold, language key hold + picker, presses after close) | **25** | **2** |
| `ui bench scroll 20` (continuous rendering, no touch) | 2 | 1 |

Without touch the rendering alone fits; touch (GT911 reads, the touch bridge and LVGL input processing, all running
code from PSRAM under `CONFIG_SPIRAM_XIP_FROM_PSRAM`) adds enough PSRAM traffic that the bounce-buffer refill
(a CPU copy from the PSRAM frame buffer in the LCD interrupt) misses its deadline. At 30 MHz a 10-line refill has
about 0.46 ms; at 25 MHz about 0.55 ms, and the scan-out itself needs 17 % less PSRAM bandwidth.

Rejected: one LVGL draw unit instead of two (`LV_DRAW_SW_DRAW_UNIT_CNT=1`) — no change on the benches.
Not tried: larger bounce buffers (each extra line costs 4 KiB of internal RAM, about 47 KiB are free), PSRAM at
120 MHz (TECH-024: experimental, not qualified).

## Decision

1. `BOARD_LCD_PCLK_HZ` = 25 MHz: 27.3 Hz scan-out on the 1024×600 panel. Porches unchanged.
2. LVGL renders at most one frame per scan-out as before; UI pacing follows the slower VSYNC.

## Consequences

- 17 % fewer frames per second on screen; the owner saw no flicker and no change of colors or sharpness.
- Remaining: a jump when a modal closes and on the first key press after it (owner, 2026-11-02; 2 missed frames in
  70 s). The simulator shows that close renders one screen and the key press 1.9 % of it, the same as a view switch
  that does not jump, so the cause is something only the board does; a timeline capture (missed frame, render
  start/end, I²C transactions, touch samples) is the next step.
- Release gate unchanged: `lcd stats` `missed` = 0 across view switches, now also checked with a finger held.
