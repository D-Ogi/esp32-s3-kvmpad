// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiActionKey (TASK-060D, 07 §7): "pressed/released feedback and touch target". Not allowed here:
// a direct `tud_hid_*` call - the key hands the caller a control id and a down/up edge, and the
// caller turns that into a typed intent for the InputArbiter (AGENTS.md prohibition 2,
// AGENTS-UI MUST 4). There is no TinyUSB include anywhere in this component.
//
// Down/up guarantee (AGENTS.md prohibition 3): the widget keeps its own down flag in
// LV_STATE_USER_1 and emits exactly one `pressed = true` and one matching `pressed = false`. The
// false edge is also emitted when the press is lost, when the key is disabled while held and when
// the widget is deleted - so a view change or a screen teardown can never leave a key DOWN.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/** `pressed` is the edge, not a state query. Called in the LVGL owner context. */
typedef void (*ui_action_key_fn)(uint16_t control_id, bool pressed, void *user_data);

/** Caller-owned sink; it must outlive every key created with it. */
typedef struct {
    ui_action_key_fn fn;
    void *user_data;
} ui_action_key_sink_t;

/** Caller-owned descriptor; it must outlive the widget (the factory keeps the pointer). */
typedef struct {
    const char *label;        // NULL => icon only
    const char *icon;         // LV_SYMBOL_* or NULL
    uint16_t control_id;      // UI-local stable id, resolved by the view's mapping (07 §3)
    ui_key_variant_t variant; // normal / modifier / destructive
    bool checkable;           // LVGL toggles LV_STATE_CHECKED on click (e.g. a sticky modifier)
    int32_t width;            // 0 => flex grow 1
    int32_t height;           // 0 => UI_TOUCH_PREFERRED
    ui_font_role_t font;      // typography role of the label
} ui_action_key_desc_t;

/** Returns NULL if `parent`, `desc` or the theme is unavailable. `sink` may be NULL (inert key). */
lv_obj_t *ui_action_key_create(lv_obj_t *parent, const ui_action_key_desc_t *desc,
                               ui_action_key_sink_t *sink);

/** Disabling a held key emits the release edge first, then blocks further edges. */
void ui_action_key_set_enabled(lv_obj_t *key, bool enabled);

void ui_action_key_set_checked(lv_obj_t *key, bool checked);

/** Replace the label text (copied); a no-op for a key created without a label. */
void ui_action_key_set_label(lv_obj_t *key, const char *text);
bool ui_action_key_is_down(const lv_obj_t *key);
uint16_t ui_action_key_control_id(lv_obj_t *key);

#ifdef __cplusplus
}
#endif
