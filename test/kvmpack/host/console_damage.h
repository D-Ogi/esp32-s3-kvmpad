// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "monitor_present.h"
#include "lvgl.h"

void check_console_damage(lv_obj_t *screen,const uint16_t *initial,bool native,
                          void (*offer)(const uint16_t *,const monitor_damage_t *,unsigned));
