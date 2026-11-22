// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// QEMU can claim the explicitly selected GUD device through inbox WinUSB.
#include <stdint.h>
#include "tusb.h"
#include "esp_log.h"

bool usb_monitor_port_gud_selected(void);

enum { VENDOR_CODE = 0x29 };
static const struct __attribute__((packed)) {
    uint8_t header[10], configuration[8], function[8], compatible_id[20];
    uint16_t length, descriptor_type, property_type, name_bytes;
    uint16_t name[21], value_bytes, value[40];
} descriptors = {
    // USBCCGP groups all three bench interfaces using the IAD. WinUSB owns
    // the resulting function so its input endpoints can go to the guest.
    .header = {10, 0, 0, 0, U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(178)},
    .configuration = {8, 0, 1, 0, 0, 0, U16_TO_U8S_LE(168)},
    .function = {8, 0, 2, 0, 0, 0, U16_TO_U8S_LE(160)},
    .compatible_id = {20, 0, 3, 0, 'W', 'I', 'N', 'U', 'S', 'B'},
    .length = 132, .descriptor_type = 4, .property_type = 7, .name_bytes = 42,
    .name = u"DeviceInterfaceGUIDs", .value_bytes = 80,
    .value = u"{8822F6B8-18B6-4638-A8C3-88E7AE6F78F8}\0",
};
_Static_assert(sizeof(descriptors) == 178, "Microsoft OS 2.0 descriptor set size");

const uint8_t *tud_descriptor_bos_cb(void)
{
    static const uint8_t bos[] = {
        TUD_BOS_DESCRIPTOR(TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN, 1),
        TUD_BOS_MS_OS_20_DESCRIPTOR(sizeof(descriptors), VENDOR_CODE)
    };
    if (!usb_monitor_port_gud_selected()) return NULL;
    ESP_LOGI("gud_guest", "USB host requested BOS");
    return bos;
}

bool gud_test_winusb_request(uint8_t port, uint8_t stage, const tusb_control_request_t *request)
{
    if (!usb_monitor_port_gud_selected() || request->bmRequestType != 0xc0 ||
        request->bRequest != VENDOR_CODE || request->wValue || request->wIndex != 7)
        return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    ESP_LOGI("gud_guest", "USB host requested OS descriptor set, length %u", request->wLength);
    return tud_control_xfer(port, request, (void *)&descriptors, sizeof(descriptors));
}
