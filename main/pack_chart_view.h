// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "lvgl.h"
#include "pack_scene.h"

size_t pack_chart_view_storage(void);
size_t pack_chart_view_budget(unsigned width, unsigned count);
bool pack_chart_view_open(lv_obj_t *parent, const pack_scene_t *scene, const pack_widget_t *widget, void *storage);
// LVGL owner only. Pass logical screen coordinates; send count=0 on full release.
// A claimed pair and its remaining contact stay consumed until that release.
bool pack_chart_view_touch(unsigned count, const lv_point_t points[2]);
// Read on the LVGL owner. A hidden chart has no visible zoom.
unsigned pack_chart_view_visible_zoom(void);
void pack_chart_view_close(void);
