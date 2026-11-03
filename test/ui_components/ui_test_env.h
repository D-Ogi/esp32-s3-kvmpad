// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shared environment of the TASK-060D host suites: a headless 1024x600 RGB565 display (the target
// geometry of ADR-026 TECH-087), LVGL's own test input devices, and the live-allocation baseline.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/**
 * Create the display and the test indevs, warm the text renderer up and return the live-allocation
 * baseline. Every later create/delete cycle must come back to this number.
 */
size_t ui_test_env_start(void);

/** Delete the indevs and the display. `baseline` is the value ui_test_env_start() returned. */
void ui_test_env_stop(size_t baseline);

/** A full-screen root with the screen style, cleared between tests. */
lv_obj_t *ui_test_root(void);

/**
 * Centre of `obj` in screen coordinates after a layout update. Returns false when the object has
 * no area yet.
 */
bool ui_test_center_of(lv_obj_t *obj, int32_t *x, int32_t *y);

/** Render `obj` to RGB565 and write a binary PPM under the snapshot directory. */
void ui_test_snapshot(lv_obj_t *obj, const char *name);

/** True when the build was given a snapshot directory. */
bool ui_test_snapshots_enabled(void);
