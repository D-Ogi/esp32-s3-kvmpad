// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include "lvgl.h"

/** LVGL owner only. The parent owns the surface and its layout. */
lv_obj_t *pack_keyboard_surface_open(lv_obj_t *parent, int width, int height, bool (*ready)(void));
/** Apply the host LED report to the matrix returned by open. LVGL owner only. */
void pack_keyboard_surface_set_caps(lv_obj_t *matrix, bool known, bool on);
/** Cancel the current press and selected modifiers before hiding a page or losing the USB session. */
void pack_keyboard_surface_release(void);
bool pack_keyboard_surface_buffered_target(lv_obj_t *target);
