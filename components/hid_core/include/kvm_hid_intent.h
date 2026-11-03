// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Internal HID intent format (spec 03 §3, kvm-contracts/1.0). The enum values of 03 are kept in
// order; ADR-018 and ADR-020 append KVM_HID_CONSUMER_DOWN/UP and KVM_HID_KEEPALIVE at the end
// (docs/EXTENSIONS.md, "Changes to the 03 §3 contract"). Pure C, no ESP-IDF dependency.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_ORIGIN_LOCAL = 1,
    KVM_ORIGIN_REMOTE = 2,
    KVM_ORIGIN_SYSTEM = 3,
} kvm_origin_t;

typedef enum {
    KVM_HID_KEY_DOWN,
    KVM_HID_KEY_UP,
    KVM_HID_MOUSE_MOVE,
    KVM_HID_MOUSE_BTN_DOWN,
    KVM_HID_MOUSE_BTN_UP,
    KVM_HID_MOUSE_WHEEL,
    KVM_HID_RELEASE_ALL,
    KVM_HID_CHORD,
    KVM_HID_CONSUMER_DOWN, // ADR-018 (TECH-074)
    KVM_HID_CONSUMER_UP,   // ADR-018 (TECH-074)
    KVM_HID_KEEPALIVE,     // ADR-020 (TECH-076), origin SYSTEM only
} kvm_hid_action_type_t;

/** Mouse button ids of 03 §3 (button mask bits 0..2, 03 §5.3). */
#define KVM_MOUSE_BTN_LEFT   1
#define KVM_MOUSE_BTN_RIGHT  2
#define KVM_MOUSE_BTN_MIDDLE 3

typedef struct {
    kvm_hid_action_type_t type;
    kvm_origin_t origin;
    uint32_t epoch;
    uint32_t seq;
    uint64_t timestamp_us;
    union {
        struct {
            uint8_t usage_id; // USB HID usage page 0x07; 0xE0..0xE7 are the modifiers
        } key;
        struct {
            int16_t dx, dy;
        } mouse_move;
        struct {
            uint8_t button_id; // 1 = L, 2 = R, 3 = Middle
        } mouse_button;
        struct {
            int16_t horizontal, vertical;
        } wheel;
        struct {
            uint16_t chord_id; // compiled allowlist (hid_keymap.h), never raw keycodes
        } chord;
        struct {
            uint16_t usage; // usage page 0x0C, allowlist only (TECH-075)
        } consumer;
    } data;
} kvm_hid_intent_t;

#ifdef __cplusplus
}
#endif
