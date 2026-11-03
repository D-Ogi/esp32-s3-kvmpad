// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiToast (TASK-060D, 07 §7): "short events, queue and fade-out". Not allowed here: blocking the
// touchpad while in use - the toast is created non-clickable and never covers the full parent, so
// a gesture in progress on the PAD keeps running underneath it.
//
// Lifetime: the toast owns one `lv_timer` with repeat count 1 and deletes itself when it fires.
// The timer is stored in the widget's user data, and the widget's delete event removes the timer,
// so dismissing early leaves nothing running. The component never calls lv_timer_handler() itself
// (AGENTS-UI MUST 3): the LVGL owner's loop drives it.
#pragma once

#include <stdint.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show `text` at the bottom of `parent` (usually the shell's toast layer).
 * `duration_ms` of 0 means UI_TOAST_DEFAULT_MS; UI_TOAST_NO_TIMEOUT keeps it until dismissed.
 * Returns the toast, or NULL if `parent`, `text` or the theme is unavailable.
 */
lv_obj_t *ui_toast_show(lv_obj_t *parent, const char *text, uint32_t duration_ms);

/** Value of `duration_ms` that disables the timeout (the caller dismisses it). */
#define UI_TOAST_NO_TIMEOUT UINT32_MAX

/** Delete the toast and its timer now. Call it from the LVGL owner context, not from the timer. */
void ui_toast_dismiss(lv_obj_t *toast);

/** The timeout timer, or NULL when the toast has none. */
lv_timer_t *ui_toast_timer(lv_obj_t *toast);

#ifdef __cplusplus
}
#endif
