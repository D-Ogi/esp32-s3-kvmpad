// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"
#include <stdbool.h>

// LVGL owner only. Open registers one authored viewport. Tick builds only while visible.
bool pack_autopilot_view_open(lv_obj_t *parent,int width,int height);
void pack_autopilot_view_tick(void);
void pack_autopilot_view_close(void);
#if CONFIG_KVM_UI_BENCH
bool pack_autopilot_view_waiting_for_body(void);
#endif
