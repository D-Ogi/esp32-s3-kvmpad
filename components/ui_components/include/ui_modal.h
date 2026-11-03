// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiModal (TASK-060D, 07 §7): "controlled overlay, focus trap and dismiss". Not allowed here:
// modifying the HID owner - the modal reports which button was tapped and nothing else.
//
// Input blocking (AT-UIFW-10): the returned object is a full-size clickable scrim created as the
// last child of `parent`, so LVGL hit-tests it first and nothing underneath receives a press. The
// dialog card is a child of the scrim. After the close the scrim and the card are gone and the view
// below is tappable again - 07 §7 "focus does not block the pad after closing".
//
// Lifetime: ui_modal_close() schedules an asynchronous delete, which is the only safe way to remove
// the overlay from inside a button callback, and is idempotent - a second call while the close is
// pending does nothing. The deletion happens in the next lv_timer_handler() of the LVGL owner, so
// a pointer held by the caller is dangling from that moment on. A button with `closes` set
// therefore invalidates the pointer before the owner's next tick: clear it in the sink callback,
// or register an LV_EVENT_DELETE callback on the returned object and clear it there.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Caller-owned button row. The array must outlive the modal (the factory keeps the pointer). */
typedef struct {
    const char *label;
    uint16_t control_id;       // 07 §3: a UI-local id, resolved by the view's mapping
    ui_btn_variant_t variant;  // primary / secondary / quiet
    bool closes;               // close the modal after the callback returns
} ui_modal_button_t;

typedef void (*ui_modal_fn)(lv_obj_t *modal, uint16_t control_id, void *user_data);

/** Caller-owned sink; it must outlive the modal. */
typedef struct {
    ui_modal_fn fn;
    void *user_data;
} ui_modal_sink_t;

/** Caller-owned descriptor; it must outlive the modal. */
typedef struct {
    const char *title;
    const char *body;
    const ui_modal_button_t *buttons;
    uint16_t button_count;
    bool dismiss_on_scrim;     // tapping outside the card closes it; false for a decision the user must make
    uint16_t overlay_id;       // echoed to the router's overlay projection by the caller
} ui_modal_desc_t;

/**
 * Create the scrim and the card under `parent` (usually the shell's overlay layer).
 * Returns the scrim, which is what ui_modal_close() takes. NULL if `parent`, `desc` or the theme
 * is unavailable.
 */
lv_obj_t *ui_modal_create(lv_obj_t *parent, const ui_modal_desc_t *desc, ui_modal_sink_t *sink);

/** Schedule the close. Safe from a button callback and safe to call twice. */
void ui_modal_close(lv_obj_t *modal);

/** True once the close has been scheduled. */
bool ui_modal_is_closing(const lv_obj_t *modal);

/** The dialog card inside the scrim (for a view that adds its own content). */
lv_obj_t *ui_modal_card(lv_obj_t *modal);

/** The overlay id of the descriptor, or 0. */
uint16_t ui_modal_overlay_id(lv_obj_t *modal);

#ifdef __cplusplus
}
#endif
