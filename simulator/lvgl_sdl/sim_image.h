// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Golden images of the simulator (TASK-070D): render an object tree to RGB888, write and read 24-bit PNG, and
// compare two images with a per-channel tolerance and a budget of differing pixels.
//
// The render goes through lv_snapshot_take() in RGB565 - the pixel format of the target panel (ADR-026,
// TECH-087) - and is expanded to RGB888 for the PNG, so a golden shows exactly the quantization the panel
// shows. PNG is handled by the lodepng that LVGL vendors (LV_USE_LODEPNG), not by a hand-written codec.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    uint32_t w;
    uint32_t h;
    uint8_t *rgb; // w * h * 3, row major, no padding; malloc'ed
} sim_image_t;

typedef struct {
    uint32_t pixels_compared;
    uint32_t pixels_differing; // beyond the tolerance
    uint32_t max_channel_delta;
    int32_t first_x, first_y; // first differing pixel, -1 when none
} sim_diff_t;

/** Render `obj` (normally the shell screen) to an RGB888 image. False when LVGL could not snapshot it. */
bool sim_image_render(lv_obj_t *obj, sim_image_t *out);

/** Read a 24-bit PNG. False when the file is missing or malformed (the reason is printed). */
bool sim_image_read_png(const char *path, sim_image_t *out);

/** Write `img` as a 24-bit PNG. False on an encode or write error (the reason is printed). */
bool sim_image_write_png(const char *path, const sim_image_t *img);

void sim_image_free(sim_image_t *img);

/**
 * Compare two images. A pixel differs when any channel delta exceeds `tolerance`. Returns true when the
 * geometry matches and the number of differing pixels is at most `budget`.
 */
bool sim_image_compare(const sim_image_t *a, const sim_image_t *b, uint8_t tolerance, uint32_t budget,
                       sim_diff_t *out);
