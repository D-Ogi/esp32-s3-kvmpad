// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"

bool pack_touchpad_drawer_active(void);
void pack_touchpad_drawer_open(lv_obj_t *content);
void pack_touchpad_drawer_close(void);
void pack_touchpad_drawer_update(bool ready);
