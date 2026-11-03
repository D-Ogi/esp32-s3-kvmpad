// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Acceptance checks of the desktop simulator (TASK-070D, 07 §10 level L1). Everything here is software evidence
// on the host: it says nothing about the panel, the GT911 or the USB HOST, and a hardware result stays
// NOT_RUN_HW (AGENTS-UI §12).
//
//   per fixture   AT-UIFW-01  geometry of the visible interactive objects: inside 1024x600, hit box >= 44 px,
//                             no overlap between two unrelated hit targets (01 §2.1 rule)
//                 hidden      a hidden object never receives a press (no LV_STATE_PRESSED, no key down edge)
//   once          AT-UIFW-02  N view switches: live LVGL allocations and store observer counts unchanged
//                 AT-UIFW-04  a press on the touch surface is no LVGL input: no action, no tab click, no view
//                             change (the pad belongs to ui_touch_bridge, 07 §6)
//                 AT-UIFW-08  a forced view switch while a key is held: KEY_UP is submitted, once, before the
//                             switch reports its new input area
//                 AT-UIFW-10  N open/close cycles of a blocking details card: the pad comes back every time
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sim_platform.h"

typedef struct {
    uint32_t hit_targets;   // visible interactive objects found
    uint32_t too_small;     // hit box below UI_TOUCH_MIN
    uint32_t out_of_bounds; // leaves the 1024x600 panel
    uint32_t overlapping;   // pairs of unrelated hit targets that overlap
    uint32_t label_clipped; // warning: a visible label is not contained in its parent
    uint32_t hidden_probed;
    uint32_t hidden_reached; // a hidden object that took a press
} sim_check_result_t;

/** Geometry scan of the current screen. Read-only: it touches nothing and renders nothing. */
bool sim_check_geometry(sim_app_t *s, const char *fixture, sim_check_result_t *out);

/** Press the centre of every hidden interactive object and prove none of them reacted. Mutates state. */
bool sim_check_hidden(sim_app_t *s, const char *fixture, sim_check_result_t *out);

/** AT-UIFW-02: `switches` view changes with the live allocation and observer counts held flat. */
bool sim_check_at02(sim_app_t *s, uint32_t switches);

/** AT-UIFW-04: a press on the touch surface produces no action and no navigation. */
bool sim_check_at04(sim_app_t *s);

/** AT-UIFW-08: forced view switch with a key held. */
bool sim_check_at08(sim_app_t *s);

/** AT-UIFW-10: `cycles` details-card open/close repetitions; the pad is available again after each one. */
bool sim_check_at10(sim_app_t *s, uint32_t cycles);
