// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "usb_hid_reports.h"

#include <string.h>

size_t kvm_hid_build_keyboard(uint8_t out[KVM_HID_KEYBOARD_REPORT_LEN], uint8_t modifiers, const uint8_t keys[6])
{
    out[0] = modifiers;
    out[1] = 0;
    if (keys != NULL) {
        memcpy(&out[2], keys, 6);
    } else {
        memset(&out[2], 0, 6);
    }
    return KVM_HID_KEYBOARD_REPORT_LEN;
}

size_t kvm_hid_build_mouse(uint8_t out[KVM_HID_MOUSE_REPORT_LEN], uint8_t protocol, uint8_t buttons, int8_t dx,
                           int8_t dy, int8_t wheel, int8_t pan)
{
    out[0] = buttons & KVM_HID_MOUSE_BUTTON_MASK;
    out[1] = (uint8_t)dx;
    out[2] = (uint8_t)dy;
    if (protocol == KVM_HID_PROTOCOL_BOOT) {
        return KVM_HID_MOUSE_BOOT_LEN;
    }
    out[3] = (uint8_t)wheel;
    out[4] = (uint8_t)pan;
    return KVM_HID_MOUSE_REPORT_LEN;
}

size_t kvm_hid_build_consumer(uint8_t out[KVM_HID_CONSUMER_REPORT_LEN], uint16_t usage)
{
    out[0] = (uint8_t)(usage & 0xFF);
    out[1] = (uint8_t)(usage >> 8);
    return KVM_HID_CONSUMER_REPORT_LEN;
}

size_t kvm_hid_build_neutral(uint8_t itf, uint8_t protocol, uint8_t out[8])
{
    switch (itf) {
    case KVM_HID_ITF_KEYBOARD:
        return kvm_hid_build_keyboard(out, 0, NULL);
    case KVM_HID_ITF_MOUSE:
        return kvm_hid_build_mouse(out, protocol, 0, 0, 0, 0, 0);
    case KVM_HID_ITF_CONSUMER:
        return kvm_hid_build_consumer(out, 0);
    default:
        return 0;
    }
}
