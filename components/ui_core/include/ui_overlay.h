// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Overlay stack of the router shell (TASK-060C, 07 §5): at most UI_OVERLAY_STACK_MAX entries, no duplicate
// ids, no nested modal leaks. A blocking overlay sits on a full-screen scrim that absorbs every click, and
// opening it cancels the input below (touch cancel + HID release request) and takes the pad away; closing
// the last blocking overlay gives the pad back. LVGL owner context only.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Push an overlay (refused when unknown, already open, the stack is full or inside a switch). */
bool ui_overlay_show(ui_router_t *r, uint16_t overlay_id);

/** Pop the top overlay (no-op when none). Safe to call from a click handler inside the overlay. */
void ui_overlay_dismiss(ui_router_t *r);

/** Pop every overlay. */
void ui_overlay_dismiss_all(ui_router_t *r);

/** Id of the top overlay, or UI_OVERLAY_NONE. */
uint16_t ui_overlay_top(const ui_router_t *r);

/** True when a blocking overlay is open (the pad and the views below get no input). */
bool ui_overlay_blocking(const ui_router_t *r);

#ifdef __cplusplus
}
#endif
