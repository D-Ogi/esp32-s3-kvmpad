// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Typed UI action gateway (TASK-060B). See ui_actions.h.
#include "ui_actions.h"

#include <string.h>

#include "hid_keymap.h"

void ui_actions_init(ui_actions_t *a)
{
    memset(a, 0, sizeof(*a));
    a->next_request_id = 1;
}

void ui_actions_set_local_sink(ui_actions_t *a, ui_action_sink_fn fn, void *ctx)
{
    a->local_sink = fn;
    a->local_ctx = ctx;
}

void ui_actions_set_domain_sink(ui_actions_t *a, ui_action_sink_fn fn, void *ctx)
{
    a->domain_sink = fn;
    a->domain_ctx = ctx;
}

bool ui_action_valid(ui_action_type_t type, uint16_t value)
{
    switch (type) {
    case UI_ACT_NAVIGATE:
        return value < UI_VIEW_COUNT;
    case UI_ACT_OPEN_HOST_DETAILS:
    case UI_ACT_OPEN_CLIENT_DETAILS:
    case UI_ACT_OPEN_SETTINGS:
    case UI_ACT_DISMISS_OVERLAY:
        return value == 0;
    case UI_ACT_KEY_DOWN:
    case UI_ACT_KEY_UP:
        return value >= 0x04 && value <= 0xE7; // keyboard page usages; 0x00..0x03 are error codes
    case UI_ACT_SHORTCUT_REQUEST:
        return value > KVM_CHORD_NONE && value < KVM_CHORD_COUNT;
    case UI_ACT_MOUSE_BUTTON_DOWN:
    case UI_ACT_MOUSE_BUTTON_UP:
        return value >= KVM_MOUSE_BTN_LEFT && value <= KVM_MOUSE_BTN_MIDDLE;
    case UI_ACT_MODIFIER_TOGGLE:
        return value >= 0xE0 && value <= 0xE7;
    case UI_ACT_DRAG_LOCK_REQUEST:
        return value <= 1;
    case UI_ACT_TEXT_CHAR:
        return value >= 0x20 && value != 0x7F && !(value >= 0x80 && value <= 0x9F) &&
               !(value >= 0xD800 && value <= 0xDFFF);
    case UI_ACT_TEXT_KEY: {
        const uint16_t usage = (uint16_t)(value & ~UI_TEXT_KEY_SHIFT);
        return (value & ~(UI_TEXT_KEY_SHIFT | 0xFFu)) == 0 && usage >= 0x04 && usage <= 0xDD;
    }
    case UI_ACT_TEXT_CANCEL:
        return value == 0;
    case UI_ACT_COUNT:
        break;
    }
    return false;
}

uint32_t ui_actions_submit(ui_actions_t *a, ui_action_type_t type, uint16_t control_id, uint16_t value,
                           uint64_t now_us, ui_action_t *out)
{
    ui_action_t act = {
        .type = type, .request_id = 0, .monotonic_us = now_us, .control_id = control_id, .value = value,
    };
    a->c.submitted++;
    uint32_t rid = 0;
    if (!ui_action_valid(type, value)) {
        a->c.rejected_invalid++;
    } else {
        bool local = ui_action_is_local(type);
        ui_action_sink_fn fn = local ? a->local_sink : a->domain_sink;
        void *ctx = local ? a->local_ctx : a->domain_ctx;
        act.request_id = a->next_request_id;
        if (++a->next_request_id == 0) {
            a->next_request_id = 1; // 0 means "refused"
        }
        if (!fn) {
            a->c.rejected_no_sink++;
        } else if (!fn(ctx, &act)) {
            a->c.rejected_by_sink++;
        } else {
            if (local) {
                a->c.local++;
            } else {
                a->c.domain++;
            }
            rid = act.request_id;
        }
    }
    if (out) {
        *out = act;
    }
    return rid;
}

const char *ui_action_name(ui_action_type_t type)
{
    static const char *const names[UI_ACT_COUNT] = {
        "NAVIGATE", "OPEN_HOST_DETAILS", "OPEN_CLIENT_DETAILS", "OPEN_SETTINGS", "DISMISS_OVERLAY", "KEY_DOWN",
        "KEY_UP", "SHORTCUT_REQUEST", "MOUSE_BUTTON_DOWN", "MOUSE_BUTTON_UP", "MODIFIER_TOGGLE",
        "DRAG_LOCK_REQUEST", "TEXT_CHAR", "TEXT_KEY", "TEXT_CANCEL",
    };
    return (unsigned)type < UI_ACT_COUNT ? names[type] : "?";
}
