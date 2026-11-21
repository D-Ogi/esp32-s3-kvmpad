// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"
#include "usb_monitor.h"

// LVGL owner. Pass a system-owned image object, sized to the authored viewport.
// The source mode is retained while moving between pages.
bool monitor_viewport_start(lv_obj_t *image, bool native);
usb_monitor_mode_result_t monitor_viewport_change_mode(bool native, bool (*set_profile)(bool native));
void monitor_viewport_target(lv_obj_t *image);
void monitor_viewport_stop(void);
bool monitor_viewport_redraw(usb_monitor_redraw_t *result);
