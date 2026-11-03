// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiTopBar (TASK-060D, 07 §7): "navigation + two independent HOST/CLIENT statuses". Not allowed
// here: computing the real connection - the bar renders the domain projection it is bound to.
//
// Fixed geometry (07 §7, FR-003): the bar is exactly UI_TOPBAR_H tall and as wide as its parent,
// whatever the status strings contain. Every slot has a fixed width from ui_tokens.h and every
// status label clips with LV_LABEL_LONG_MODE_DOTS, so a long IP or SSID cannot push a slot out of
// the bar or make it grow. 07 §7 also forbids putting a password, token or session id here; the
// component takes no such field.
//
// FR-712 (ADR-020): the Keep awake indicator is text + icon, never color alone. It is hidden
// unless its subject reads exactly 1, so an unknown value hides it rather than claiming the
// feature is on.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_status_item.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 07 §7 gives the bar exactly two statuses. A third fixed slot does not fit beside a
 * reference-width navigation at 1024 px (see the budget in ui_tokens.h), so ownership and link
 * detail belong in the UiStatusItem details overlay, not in the bar.
 */
typedef enum {
    UI_TOPBAR_HOST = 0,
    UI_TOPBAR_CLIENT,
    UI_TOPBAR_STATUS_COUNT,
} ui_topbar_status_t;

/** Caller-owned descriptor; the status descriptors must outlive the bar. */
typedef struct {
    const char *brand;                                           // NULL => no brand text
    const ui_status_desc_t *status[UI_TOPBAR_STATUS_COUNT];       // NULL => that slot is omitted
    const char *keep_awake_text;                                  // FR-712; NULL => no indicator
    const char *keep_awake_icon;                                  // FR-712; NULL => LV_SYMBOL_EYE_OPEN
} ui_topbar_desc_t;

/** Subjects to bind. Any member may be NULL; an unbound status stays at its unknown text. */
typedef struct {
    lv_subject_t *status[UI_TOPBAR_STATUS_COUNT];
    lv_subject_t *keep_awake; // 0/1; the indicator shows only while the value is exactly 1
} ui_topbar_bind_t;

/** Returns NULL if `parent`, `desc` or the theme is unavailable. `bind` may be NULL. */
lv_obj_t *ui_topbar_create(lv_obj_t *parent, const ui_topbar_desc_t *desc,
                           const ui_topbar_bind_t *bind);

/** Empty fixed-width container for the navigation control (UI_TOPBAR_NAV_W x UI_SEGMENTED_H). */
lv_obj_t *ui_topbar_nav_slot(lv_obj_t *bar);

/** Empty fixed-width container at the right edge (settings button, UI_TOPBAR_BTN_W wide). */
lv_obj_t *ui_topbar_trailing_slot(lv_obj_t *bar);

/** The status item of a slot, or NULL when the descriptor omitted it. */
lv_obj_t *ui_topbar_status_item(lv_obj_t *bar, ui_topbar_status_t slot);

/** The FR-712 indicator, or NULL when the descriptor omitted it. */
lv_obj_t *ui_topbar_keep_awake(lv_obj_t *bar);

#ifdef __cplusplus
}
#endif
