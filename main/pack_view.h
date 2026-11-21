// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include "lvgl.h"
#include "pack_scene.h"

void pack_view_provider(bool xplane);
// Physical board turned clockwise from its normal landscape position.
lv_obj_t *pack_view_frame(lv_obj_t *screen, bool portrait);
void pack_view_logical_point(bool portrait, const lv_point_t *physical, lv_point_t *logical);
unsigned pack_view_available_permissions(void);
bool pack_view_open(lv_obj_t *content, pack_scene_t *scene, unsigned granted);
void pack_view_close(void);
void pack_view_enter_magnifier(void);
void pack_view_tick(void);
#if CONFIG_KVM_USB_MONITOR_EVAL
bool pack_view_touchpad(unsigned count, const lv_point_t logical[2], const uint8_t ids[2], uint64_t timestamp_us);
#endif
#if CONFIG_KVM_UI_BENCH
// Benchmark local page navigation on a UI-only scene, using -1 for the picker.
bool pack_view_bench_navigate(int page);
#endif
