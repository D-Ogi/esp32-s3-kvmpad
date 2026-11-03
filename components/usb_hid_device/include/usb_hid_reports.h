// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pure HID report builders (spec 03 §5, ADR-018). No TinyUSB dependency: host unit tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_HID_ITF_KEYBOARD   0
#define KVM_HID_ITF_MOUSE      1
#define KVM_HID_ITF_CONSUMER   2
#define KVM_HID_ITF_COUNT      3

#define KVM_HID_PROTOCOL_BOOT   0   // HID 1.11 §7.2.6 SET_PROTOCOL wValue
#define KVM_HID_PROTOCOL_REPORT 1

#define KVM_HID_KEYBOARD_REPORT_LEN   8   // modifiers, reserved, 6 usages
#define KVM_HID_MOUSE_BOOT_LEN        3   // buttons, x, y
#define KVM_HID_MOUSE_REPORT_LEN      5   // buttons, x, y, wheel, AC Pan (TECH-073)
#define KVM_HID_CONSUMER_REPORT_LEN   2   // one 16-bit Consumer usage

#define KVM_HID_MOUSE_BUTTON_MASK     0x1F // left, right, middle, back, forward

/** LED output report bits (HID usage page 0x08). */
#define KVM_HID_LED_NUM_LOCK    0x01
#define KVM_HID_LED_CAPS_LOCK   0x02
#define KVM_HID_LED_SCROLL_LOCK 0x04
#define KVM_HID_LED_COMPOSE     0x08
#define KVM_HID_LED_KANA        0x10

/** 8-byte boot keyboard report; `keys` may be NULL (all released). Returns 8. */
size_t kvm_hid_build_keyboard(uint8_t out[KVM_HID_KEYBOARD_REPORT_LEN], uint8_t modifiers, const uint8_t keys[6]);

/**
 * Mouse report for the protocol the HOST selected: boot -> 3 bytes (wheel and pan dropped),
 * report -> 5 bytes. Buttons outside bits 0..4 are cleared. Returns the length.
 */
size_t kvm_hid_build_mouse(uint8_t out[KVM_HID_MOUSE_REPORT_LEN], uint8_t protocol, uint8_t buttons, int8_t dx,
                           int8_t dy, int8_t wheel, int8_t pan);

/** 2-byte Consumer Control report (little-endian usage, 0 = released). Returns 2. */
size_t kvm_hid_build_consumer(uint8_t out[KVM_HID_CONSUMER_REPORT_LEN], uint16_t usage);

/** Neutral (all released, no motion) report for interface `itf` under `protocol`. Returns the length, 0 if unknown. */
size_t kvm_hid_build_neutral(uint8_t itf, uint8_t protocol, uint8_t out[8]);

#ifdef __cplusplus
}
#endif
