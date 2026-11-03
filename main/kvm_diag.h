// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// M0-M2 bring-up diagnostics: display owner (test card / touch screen), the TASK-020 A/B
// benchmark, the TECH-020/021 stress run and CPU-load sampling. Replaced by ui_core in M3.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "kvm_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DIAG_SCREEN_CARD = 0,
    DIAG_SCREEN_TOUCH,
    DIAG_SCREEN_RED,
    DIAG_SCREEN_GREEN,
    DIAG_SCREEN_BLUE,
    DIAG_SCREEN_WHITE,
    DIAG_SCREEN_BLACK,
    DIAG_SCREEN_BARS,
    DIAG_SCREEN_RAMPS,
    DIAG_SCREEN_ALIGN,
} diag_screen_t;

/** Start the display owner task (consumes touch samples, draws the touch screen). */
esp_err_t diag_start(void);

esp_err_t diag_show(diag_screen_t screen);
bool diag_screen_from_name(const char *name, diag_screen_t *out);

/** Every touch sample received by the display task is also handed to `hook` (TASK-060E bridge); NULL removes it. */
typedef void (*diag_touch_hook_t)(const kvm_touch_sample_t *s);
void diag_set_touch_hook(diag_touch_hook_t hook);

/** Capture up to `max` touch samples for `ms` milliseconds into a static ring, then print them. */
void diag_touch_capture(uint32_t ms, uint32_t max);

typedef struct {
    uint32_t cpu_busy_permille[2]; // per core, 1000 = fully busy
} diag_cpu_t;

/** CPU load over `ms` (FreeRTOS runtime stats, idle tasks). */
esp_err_t diag_cpu_sample(uint32_t ms, diag_cpu_t *out);

/** TASK-020 A/B benchmark: variants A (1 fb, 2x10 lines), B (1 fb, 2x20 lines), C (2 fb, 2x10 lines). */
esp_err_t diag_lcd_bench(uint32_t seconds_per_variant);

/** Stress: Wi-Fi scan + NVS writes + flash erase/write + PSRAM copy while measuring VSYNC/touch/USB. */
esp_err_t diag_stress(uint32_t seconds, bool wifi);

/**
 * Fault injection for TECH-021 / R-06: hold interrupts off on both cores for `us` microseconds
 * (1..5000), as a long critical section would, so the bounce-buffer refill is late. Used to show
 * that the scan-out resynchronises by itself afterwards (VSYNC-to-frame phase back to normal).
 */
esp_err_t diag_lcd_glitch(uint32_t us);

/** Re-create the RGB panel with another TECH-020 variant (1|2 frame buffers, bounce lines; 0 = none). */
esp_err_t diag_lcd_variant(uint8_t num_fbs, uint16_t bounce_lines);

/**
 * Synthetic touch-screen drawing load: the drawing of one touch sample `hz` times per second for
 * `seconds`, then one DRAWLOAD line with the frames the scan-out lost (vsync - frames_done).
 */
esp_err_t diag_lcd_drawload(uint32_t hz, uint32_t seconds);

/** Print one line with VSYNC statistics since the last reset of the stats. */
void diag_print_lcd_stats(const char *label);

#ifdef __cplusplus
}
#endif
