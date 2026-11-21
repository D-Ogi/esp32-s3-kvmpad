// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "lvgl.h"

bool pack_desktop_viewport(lv_obj_t *parent,int width,lv_obj_t **image);
bool pack_desktop_pad(lv_obj_t *parent,lv_obj_t *content);
bool pack_desktop_ready(void);
bool pack_desktop_change_mode(bool native);
void pack_desktop_select(lv_obj_t *image,lv_obj_t *pad,bool portrait);
bool pack_desktop_contacts(unsigned count,const lv_point_t logical[2],const uint8_t ids[2],uint64_t timestamp_us);
void pack_desktop_tick(lv_obj_t *keyboard);
void pack_desktop_close(void);
