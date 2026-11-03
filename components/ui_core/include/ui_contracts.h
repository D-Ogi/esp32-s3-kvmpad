// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI contracts (TASK-060B, 07 §3): views, typed UI actions and the state snapshot the domain
// publishes for the UI. Pure C: no LVGL, no TinyUSB, no network headers. The HOST USB and owner
// enums are the domain ones of hid_core (no competing copies, 07 §3); the CLIENT states mirror
// 01 §6.2 until the network core (TASK-080/100) owns them.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hid_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_VIEW_MOUSE = 0,
    UI_VIEW_KEYBOARD,
    UI_VIEW_SHORTCUTS,
    UI_VIEW_SETTINGS,
    UI_VIEW_COUNT,
} ui_view_id_t;

/** 07 §3, order kept. Values below UI_ACT_KEY_DOWN are local to the UI; the rest go to the domain. */
typedef enum {
    UI_ACT_NAVIGATE = 0,       // value = ui_view_id_t
    UI_ACT_OPEN_HOST_DETAILS,
    UI_ACT_OPEN_CLIENT_DETAILS,
    UI_ACT_OPEN_SETTINGS,
    UI_ACT_DISMISS_OVERLAY,
    UI_ACT_KEY_DOWN,           // value = usage (page 0x07, 0x04..0xE7)
    UI_ACT_KEY_UP,
    UI_ACT_SHORTCUT_REQUEST,   // value = chord id of the compiled allowlist (hid_keymap.h), never a keycode
    UI_ACT_MOUSE_BUTTON_DOWN,  // value = KVM_MOUSE_BTN_*
    UI_ACT_MOUSE_BUTTON_UP,
    UI_ACT_MODIFIER_TOGGLE,    // value = modifier usage 0xE0..0xE7 (ADR-016 FSM)
    UI_ACT_DRAG_LOCK_REQUEST,  // value = 0 off / 1 on; the result comes back as authoritative state
    // ADR-024 FR-746..748: text composed on the DEVICE. The domain keeps one ordered queue of these and sends each
    // item as one cancellable burst (never mixed with UI_ACT_KEY_DOWN/UP, which the core refuses during a burst).
    UI_ACT_TEXT_CHAR,          // value = BMP code point 0x20..0xFFFF, not a surrogate (U+0x7F..0x9F excluded)
    UI_ACT_TEXT_KEY,           // value = non-modifier usage 0x04..0xDD, | UI_TEXT_KEY_SHIFT to type it with Shift
    UI_ACT_TEXT_CANCEL,        // value = 0: drop the queued text, cancel the running burst, release everything
    UI_ACT_COUNT,
} ui_action_type_t;

/** UI_ACT_TEXT_KEY flag: the key is typed with Left Shift held for this one keystroke. */
#define UI_TEXT_KEY_SHIFT 0x0100u

typedef struct {
    ui_action_type_t type;
    uint32_t request_id;  // identifies the command, never the HOST state (07 §3 security note)
    uint64_t monotonic_us;
    uint16_t control_id;  // UI-local stable id of the control that produced the action
    uint16_t value;
} ui_action_t;

/** 01 §6.2 CLIENT states (presentation contract; TASK-062 approves the final owner of the enum). */
typedef enum {
    UI_CLIENT_DISABLED = 0,
    UI_CLIENT_NETWORK_DOWN,
    UI_CLIENT_LISTENING,
    UI_CLIENT_AUTHENTICATING,
    UI_CLIENT_CONNECTED,
    UI_CLIENT_CONTROL_GRANTED,
    UI_CLIENT_STALE,
    UI_CLIENT_COUNT,
} ui_client_state_t;

/** Producers of the snapshot. A field whose source never published is UNKNOWN, never guessed (07 §12 item 2). */
#define UI_SRC_HID     (1u << 0) // host_usb_state, owner_*, modifiers_down, mods_armed/locked, caps_led_*, maintenance, error_code
#define UI_SRC_NET     (1u << 1) // client_state, wifi_connected
#define UI_SRC_TOUCH   (1u << 2) // touch_healthy
#define UI_SRC_GESTURE (1u << 3) // drag_lock_active
#define UI_SRC_ALL     (UI_SRC_HID | UI_SRC_NET | UI_SRC_TOUCH | UI_SRC_GESTURE)

/** 07 §3 ui_state_snapshot_t plus `sources` and `maintenance`. Plain data: no pointers. */
typedef struct {
    uint32_t version;        // set by the mailbox: +1 for every publish that changed a field
    uint32_t sources;        // UI_SRC_* that have published at least once
    uint8_t host_usb_state;  // kvm_host_usb_t
    uint8_t client_state;    // ui_client_state_t
    uint8_t owner_kind;      // kvm_owner_kind_t
    uint32_t owner_epoch;
    bool wifi_connected;
    bool touch_healthy;
    bool drag_lock_active;   // authoritative HID/gesture state, not a GUI wish
    uint8_t modifiers_down;  // keyboard ledger projection (what the HOST was told)
    uint8_t mods_armed;      // ADR-016 one-shot modifier bits (FR-204 ARMED), from the HID core FSM
    uint8_t mods_locked;     // ADR-016 locked modifier bits (FR-204 LOCKED)
    bool caps_led_known;     // the HOST has sent at least one LED output report
    bool caps_led_active;
    bool maintenance;        // OTA / factory reset: input blocked
    uint16_t error_code;     // 0 = none; a persistent domain error (e.g. KVM_HID_ERR_* of the USB state)
} ui_state_snapshot_t;

/** Per-source slices: each producer owns its fields and publishes only them. */
typedef struct {
    uint8_t host_usb_state;
    uint8_t owner_kind;
    uint32_t owner_epoch;
    uint8_t modifiers_down;
    uint8_t mods_armed;
    uint8_t mods_locked;
    bool caps_led_known;
    bool caps_led_active;
    bool maintenance;
    uint16_t error_code;
} ui_hid_slice_t;

typedef struct {
    uint8_t client_state;
    bool wifi_connected;
} ui_net_slice_t;

typedef struct {
    bool touch_healthy;
} ui_touch_slice_t;

typedef struct {
    bool drag_lock_active;
} ui_gesture_slice_t;

/** Transient domain feedback for one action (a refusal is shown, never turned into an optimistic state). */
typedef enum {
    UI_EV_ACTION_RESULT = 0, // request_id, code = kvm_hid_err_t (0 = accepted)
    UI_EV_RELEASE_ALL,       // code = kvm_release_reason_t
    UI_EV_COUNT,
} ui_event_kind_t;

typedef struct {
    ui_event_kind_t kind;
    uint32_t request_id;
    uint16_t code;
} ui_event_t;

/** True for the action types the UI handles itself (navigation, overlays); false for domain actions. */
static inline bool ui_action_is_local(ui_action_type_t t)
{
    return t <= UI_ACT_DISMISS_OVERLAY;
}

#ifdef __cplusplus
}
#endif
