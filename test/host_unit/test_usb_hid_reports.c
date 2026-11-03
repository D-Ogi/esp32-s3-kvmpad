// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-050 USB part / 03 §5 / ADR-018: report formats, boot vs report protocol, neutral reports.
#include <string.h>

#include "kvm_test.h"
#include "usb_hid_reports.h"

static void keyboard_report_layout(void)
{
    uint8_t r[8];
    const uint8_t keys[6] = {0x04, 0x05, 0, 0, 0, 0};
    CHECK_EQ(kvm_hid_build_keyboard(r, 0x05, keys), 8);
    CHECK_EQ(r[0], 0x05);
    CHECK_EQ(r[1], 0); // reserved
    CHECK_EQ(r[2], 0x04);
    CHECK_EQ(r[3], 0x05);
    memset(r, 0xFF, sizeof(r));
    kvm_hid_build_keyboard(r, 0, NULL);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(r[i], 0);
    }
}

static void mouse_boot_is_three_bytes(void)
{
    uint8_t r[5] = {0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    CHECK_EQ(kvm_hid_build_mouse(r, KVM_HID_PROTOCOL_BOOT, 0x01, -1, 2, 3, 4), 3);
    CHECK_EQ(r[0], 0x01);
    CHECK_EQ((int8_t)r[1], -1);
    CHECK_EQ((int8_t)r[2], 2);
    CHECK_EQ(r[3], 0xEE); // wheel and pan never leak into a boot report
    CHECK_EQ(r[4], 0xEE);
}

static void mouse_report_has_wheel_and_pan(void)
{
    uint8_t r[5];
    CHECK_EQ(kvm_hid_build_mouse(r, KVM_HID_PROTOCOL_REPORT, 0xFF, 127, -127, -1, 1), 5);
    CHECK_EQ(r[0], KVM_HID_MOUSE_BUTTON_MASK); // only 5 buttons exist
    CHECK_EQ((int8_t)r[1], 127);
    CHECK_EQ((int8_t)r[2], -127);
    CHECK_EQ((int8_t)r[3], -1);
    CHECK_EQ((int8_t)r[4], 1);
}

static void consumer_report_little_endian(void)
{
    uint8_t r[2];
    CHECK_EQ(kvm_hid_build_consumer(r, 0x00E9), 2); // Volume Increment
    CHECK_EQ(r[0], 0xE9);
    CHECK_EQ(r[1], 0x00);
    kvm_hid_build_consumer(r, 0x0238); // AC Pan usage id, high byte check
    CHECK_EQ(r[0], 0x38);
    CHECK_EQ(r[1], 0x02);
}

static void neutral_reports_are_all_zero(void)
{
    uint8_t r[8];
    for (uint8_t proto = 0; proto <= 1; proto++) {
        for (uint8_t itf = 0; itf < KVM_HID_ITF_COUNT; itf++) {
            memset(r, 0xAA, sizeof(r));
            size_t n = kvm_hid_build_neutral(itf, proto, r);
            CHECK(n > 0);
            for (size_t i = 0; i < n; i++) {
                CHECK_EQ(r[i], 0);
            }
        }
    }
    CHECK_EQ(kvm_hid_build_neutral(KVM_HID_ITF_MOUSE, KVM_HID_PROTOCOL_BOOT, r), 3);
    CHECK_EQ(kvm_hid_build_neutral(KVM_HID_ITF_MOUSE, KVM_HID_PROTOCOL_REPORT, r), 5);
    CHECK_EQ(kvm_hid_build_neutral(KVM_HID_ITF_KEYBOARD, KVM_HID_PROTOCOL_BOOT, r), 8);
    CHECK_EQ(kvm_hid_build_neutral(KVM_HID_ITF_CONSUMER, KVM_HID_PROTOCOL_REPORT, r), 2);
    CHECK_EQ(kvm_hid_build_neutral(7, KVM_HID_PROTOCOL_REPORT, r), 0);
}

int main(void)
{
    printf("test_usb_hid_reports\n");
    RUN(keyboard_report_layout);
    RUN(mouse_boot_is_three_bytes);
    RUN(mouse_report_has_wheel_and_pan);
    RUN(consumer_report_little_endian);
    RUN(neutral_reports_are_all_zero);
    TEST_MAIN_END("test_usb_hid_reports");
}
