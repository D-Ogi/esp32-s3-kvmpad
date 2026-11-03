// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// RGB parallel LCD (TASK-020): PSRAM RGB565 frame buffer(s), bounce-buffer variants for the
// A/B benchmark (TECH-020) and VSYNC telemetry.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t num_fbs;       // 1..3 frame buffers in PSRAM (3 = LVGL triple buffering, TASK-060A)
    uint16_t bounce_lines; // lines per bounce buffer (the driver allocates two); 0 = DMA from PSRAM
} board_lcd_cfg_t;

/** Benchmark variant A of TECH-020: one frame buffer + double 10-line bounce buffer. */
#define BOARD_LCD_CFG_DEFAULT { .num_fbs = 1, .bounce_lines = 10 }

typedef struct {
    board_lcd_cfg_t cfg;
    uint32_t vsync_count;
    uint32_t vsync_min_us;     // shortest VSYNC interval since the last reset of the stats
    uint32_t vsync_max_us;     // longest VSYNC interval since the last reset of the stats
    uint64_t vsync_sum_us;     // sum of intervals (mean = sum / (count - 1))
    uint32_t vsync_late;       // intervals > 1.5x the nominal frame time
    uint32_t bounce_frames;    // frames fully pushed through the bounce buffers
    // Phase of the driver's "last bounce chunk copied" callback relative to the preceding VSYNC.
    // In a synchronised scan-out it is constant; a desync of one bounce chunk (bounce_lines
    // lines wrapped from the top of the picture to the bottom) moves it by one chunk time.
    uint32_t phase_min_us;
    uint32_t phase_max_us;
    uint32_t phase_last_us;
} board_lcd_stats_t;

/** Nominal refresh in mHz from the pinned timing: pclk / (htotal * vtotal). */
uint32_t board_lcd_nominal_mhz(void);

/** Create the RGB panel (panel supply + reset via the expander first). Backlight untouched. */
esp_err_t board_lcd_init(const board_lcd_cfg_t *cfg);

/** Delete the panel and free its buffers (used by the A/B benchmark). */
esp_err_t board_lcd_deinit(void);

esp_lcd_panel_handle_t board_lcd_panel(void);

/** Frame buffer `index` (0..num_fbs-1), RGB565, BOARD_LCD_H_RES x BOARD_LCD_V_RES, no padding; NULL if absent. */
uint16_t *board_lcd_fb(int index);

/** Make CPU writes to frame buffer `index` visible to the panel (cache write-back) and show it. */
esp_err_t board_lcd_present(int index);

void board_lcd_get_stats(board_lcd_stats_t *out);
void board_lcd_reset_stats(void);

#ifdef __cplusplus
}
#endif
