// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_platform (TASK-060A; 07 §0 ADR-013, §9): LVGL 9.6 on the board_hal RGB panel through one of
// two integration backends (esp_lvgl_adapter 0.7.4 / esp_lvgl_port 2.9.0, Kconfig choice). This
// component is the only owner of the LVGL context: every other task touches LVGL only between
// ui_platform_lock() / ui_platform_unlock(). It never reads the GT911 (TASK-060E brings the touch
// frames from touch_input) and never includes TinyUSB.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Panel frame buffers the selected backend / tearing mode needs from board_lcd_init(). */
uint8_t ui_platform_required_fbs(void);

/** Bounce-buffer lines board_lcd should keep (the TECH-020 default; 0 would be DMA from PSRAM). */
uint16_t ui_platform_bounce_lines(void);

/**
 * Initialize LVGL, register the board_lcd panel as the default display and start the LVGL task.
 * board_lcd_init() must have run with num_fbs == ui_platform_required_fbs().
 */
esp_err_t ui_platform_start(void);

bool ui_platform_is_running(void);

/** Recursive LVGL lock. timeout_ms < 0 waits forever. */
bool ui_platform_lock(int32_t timeout_ms);
void ui_platform_unlock(void);

lv_display_t *ui_platform_display(void);

const char *ui_platform_backend_name(void); // "esp_lvgl_adapter 0.7.4" / "esp_lvgl_port 2.9.0"
const char *ui_platform_mode_name(void);    // tearing / buffer mode in words

typedef struct {
    uint32_t refr_cycles;      // LV_EVENT_REFR_READY count since reset
    uint32_t renders;          // LV_EVENT_RENDER_READY count (cycles that drew something)
    uint32_t flushes;          // LV_EVENT_FLUSH_FINISH count
    uint64_t window_us;        // time since the last reset
    uint32_t render_avg_us;    // RENDER_START -> RENDER_READY
    uint32_t render_max_us;
    uint32_t flush_avg_us;     // FLUSH_START -> FLUSH_FINISH (includes the wait for the panel)
    uint32_t flush_max_us;
    uint32_t cycle_max_us;     // REFR_START -> REFR_READY
    uint32_t adapter_fps;      // backend's own counter when it has one, else 0
    uint32_t lvgl_task_stack_hwm; // bytes left on the LVGL task stack, 0 if unknown
} ui_platform_metrics_t;

void ui_platform_metrics_reset(void);
void ui_platform_metrics_get(ui_platform_metrics_t *out);

/** Rendered FPS over the metrics window: renders * 1e6 / window_us (x100 fixed point). */
uint32_t ui_platform_fps_x100(void);

/**
 * Snapshot of the active screen as RGB565, point-sampled every `div` pixels, delivered row by row
 * (each row `w` pixels). Used by the `ui snap` console command and test/hil/ui_snapshot.py.
 */
typedef void (*ui_platform_row_cb_t)(const uint16_t *row, uint16_t w, uint16_t y, void *ctx);
esp_err_t ui_platform_snapshot(uint16_t div, ui_platform_row_cb_t cb, void *ctx, uint16_t *out_w, uint16_t *out_h);

/**
 * LVGL heap (ADR-027 TECH-091): live lv_malloc() blocks and bytes, and how many fell back to internal
 * RAM because PSRAM was exhausted. All zero when LVGL uses the C library allocator.
 */
void ui_platform_lv_heap(size_t *live_cnt, size_t *live_bytes, size_t *internal_fallbacks);

#ifdef __cplusplus
}
#endif
