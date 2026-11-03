// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// LVGL 9.6 configuration of the desktop simulator (TASK-070D, 07 §10 level L1). Identical to the host test
// configuration of test/ui_store and test/ui_components - RGB565, Observer, Snapshot, the Montserrat sizes, the
// counting allocator, the headless test display and the test input devices - plus LV_USE_LODEPNG, because the
// golden images are written and read as PNG with the lodepng that LVGL already vendors (zlib license; no new
// dependency, nothing installed).
//
// The virtual clock of lv_test_wait() (lv_tick_inc per millisecond) is what makes a render reproducible: no wall
// clock reaches the renderer, so the same fixture always produces the same pixels.
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565 // LV_COLOR_DEPTH is deprecated in 9.6

#define LV_USE_STDLIB_MALLOC  LV_STDLIB_CUSTOM // host_lv_mem.c: plain malloc + live allocation counter
#define LV_USE_STDLIB_STRING  LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_USE_OS LV_OS_NONE
#define LV_DRAW_SW_DRAW_UNIT_CNT 1
#define LV_DEF_REFR_PERIOD 16

#define LV_USE_OBSERVER 1
#define LV_USE_SNAPSHOT 1
#define LV_OBJ_STYLE_CACHE 1

#define LV_USE_LODEPNG 1 // PNG writer / reader of the golden set (simulator only, never in the firmware image)

#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 1
#define LV_USE_ASSERT_OBJ 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_USE_FONT_COMPRESSED 1 // the compressed hanzi font blob (TASK-072), as in the firmware

#define LV_USE_TEST 1

#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0

#endif // LV_CONF_H
