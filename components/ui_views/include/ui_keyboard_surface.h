// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "kbd_layouts.h"
#include "lvgl.h"

typedef struct {
    bool (*ready)(void *ctx);
    bool (*edge)(void *ctx, bool down, uint8_t usage);
    void (*release_all)(void *ctx);
    void *ctx;
} ui_keyboard_surface_hooks_t;

/** The parent owns the returned surface. The layout and callback context must
 * outlive it. All calls and callbacks run on the LVGL owner. Minimum 440 x 176. */
lv_obj_t *ui_keyboard_surface_create(lv_obj_t *parent, int width, int height,
                                     const kbd_layout_t *layout, const ui_keyboard_surface_hooks_t *hooks);
void ui_keyboard_surface_set_caps(lv_obj_t *surface, bool known, bool on);
void ui_keyboard_surface_cancel(lv_obj_t *surface);
bool ui_keyboard_surface_buffered_target(lv_obj_t *target);
