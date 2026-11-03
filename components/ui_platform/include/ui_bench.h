// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060A benchmark scenes (07 §9: "a benchmark without USB traffic, then under load"). Pure
// LVGL widgets in a KVMpad-like 1024x600 layout; no touch, no HID, no domain state. Removed or
// moved under test/ once ui_views exist. Every call takes the LVGL lock itself.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_BENCH_IDLE = 0, // static KVMpad-like screen, nothing animates
    UI_BENCH_SMALL,    // moving 28 px dot + a counter label (typical pointer feedback)
    UI_BENCH_FULL,     // SMALL + the pad and the screen background change color every frame
    UI_BENCH_SCROLL,   // a 60-row list auto-scrolls inside the pad area
    UI_BENCH_COUNT,
} ui_bench_scene_t;

const char *ui_bench_scene_name(ui_bench_scene_t s);
bool ui_bench_scene_from_name(const char *name, ui_bench_scene_t *out);

/** Build the KVMpad-like screen and start the scene's animations (replaces the active screen). */
esp_err_t ui_bench_start(ui_bench_scene_t scene);

/** Stop the animations; the static screen stays. */
void ui_bench_stop(void);

/** Screen rectangle of the bench pad (inclusive), for the touch router. False before the first scene. */
bool ui_bench_pad_area(int16_t *x1, int16_t *y1, int16_t *x2, int16_t *y2);

/**
 * Touch feedback of the bench screen (TASK-060E bring-up): every button shows a pressed state and
 * counts LVGL CLICKED events; the top bar names the last one. The buttons send nothing to the HOST.
 * Returns the click count since boot; `last` (may be NULL) receives the label of the last clicked button.
 */
uint32_t ui_bench_clicks(char *last, size_t last_len);

/**
 * LVGL's own benchmark demo (LV_USE_DEMO_BENCHMARK): a standardized set of scenes with a summary.
 * `on_end` runs in the LVGL task when the demo finishes; the summary is also kept for ui_bench_demo_result().
 */
typedef struct {
    bool done;
    int32_t fps_avg;
    int32_t cpu_avg;
    int32_t render_avg_ms;
    int32_t flush_avg_ms;
    int32_t scenes;
} ui_bench_demo_result_t;

esp_err_t ui_bench_demo_start(void);
void ui_bench_demo_result(ui_bench_demo_result_t *out);

#ifdef __cplusplus
}
#endif
