// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Spring-centered vertical control. A touch above center steps +1, below center -1.
 * Holding farther from center repeats faster. Dragging past either end keeps
 * repeating at full speed until release or cancellation. The callback runs on the LVGL task.
 * The caller may resize the returned target and set its user data. */
lv_obj_t *ui_rate_slider_create(lv_obj_t *parent, void (*step)(int direction, void *ctx), void *ctx);

/** Ridged scroll wheel for a mouse surface. Uses the same capture and repeat behavior. */
lv_obj_t *ui_rate_wheel_create(lv_obj_t *parent, void (*step)(int direction, void *ctx), void *ctx);

/** Stop repeating immediately. A new press is required to resume. */
void ui_rate_slider_cancel(lv_obj_t *slider);

#ifdef __cplusplus
}
#endif
