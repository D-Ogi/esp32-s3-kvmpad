// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "sdkconfig.h"
#include "usb_gud_port.h"
#include "gud_panel.h"
#include "tusb.h"
#include <string.h>

#if (CONFIG_KVM_USB_GUD_VID + 0) <= 0 || (CONFIG_KVM_USB_GUD_VID + 0) >= 0xffff || \
    (CONFIG_KVM_USB_GUD_PID + 0) <= 0 || (CONFIG_KVM_USB_GUD_PID + 0) > 0xffff
#error "A GUD transport build requires an authorized device VID and PID."
#endif
#if (CONFIG_KVM_USB_GUD_VID + 0) == 0x303a && ((CONFIG_KVM_USB_GUD_PID + 0) == 0x400c || \
    (CONFIG_KVM_USB_GUD_PID + 0) == 0x2986 || (CONFIG_KVM_USB_GUD_PID + 0) == 0x2987 || \
    (CONFIG_KVM_USB_GUD_PID + 0) == 0x400a)
#error "Select a distinct GUD USB identity, separate from the display, storage and startup HID profiles."
#endif

static const tusb_desc_device_t device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
#ifdef KVM_GUD_GUEST_CHECK
    .bcdUSB = 0x0210, // Request BOS on the Windows passthrough test bench.
    .bDeviceClass = TUSB_CLASS_MISC, .bDeviceSubClass = 2, .bDeviceProtocol = 1,
#else
    .bcdUSB = 0x0200,
#endif
    .bMaxPacketSize0 = 64,
    .idVendor = CONFIG_KVM_USB_GUD_VID, .idProduct = CONFIG_KVM_USB_GUD_PID,
#ifdef KVM_GUD_GUEST_CHECK
    .bcdDevice = 0x0103,
#else
    .bcdDevice = 0x0101,
#endif
    .iManufacturer = 1, .iProduct = 2,
    .iSerialNumber = 3, .bNumConfigurations = 1,
};

// Interface zero receives the framebuffer. TinyUSB assigns HID instances in
// descriptor order, keeping the scheduler's existing keyboard and mouse slots.
#ifdef KVM_GUD_GUEST_CHECK
// Windows passes the whole bench function to the Linux VM via one WinUSB handle.
#define GUD_BENCH_IAD_LENGTH 8
#else
#define GUD_BENCH_IAD_LENGTH 0
#endif
enum { CONFIG_LENGTH = GUD_BENCH_IAD_LENGTH + TUD_CONFIG_DESC_LEN + 9 + 7 + 2 * TUD_HID_DESC_LEN };
static const uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONFIG_LENGTH, 0, 500),
#ifdef KVM_GUD_GUEST_CHECK
    8, TUSB_DESC_INTERFACE_ASSOCIATION, 0, 3, TUSB_CLASS_VENDOR_SPECIFIC, 0, 0, 0,
#endif
    9, TUSB_DESC_INTERFACE, 0, 0, 1, TUSB_CLASS_VENDOR_SPECIFIC, 0, 0, 0,
    7, TUSB_DESC_ENDPOINT, 0x01, TUSB_XFER_BULK, 64, 0, 0,
    TUD_HID_DESCRIPTOR(1, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof((uint8_t[]){TUD_HID_REPORT_DESC_KEYBOARD()}), 0x82, 8, 1),
    TUD_HID_DESCRIPTOR(2, 0, HID_ITF_PROTOCOL_MOUSE,
                       sizeof((uint8_t[]){TUD_HID_REPORT_DESC_MOUSE()}), 0x83, 8, 1),
};
_Static_assert(sizeof(configuration) == CONFIG_LENGTH, "GUD composite descriptor length");
static const uint16_t product[] = {
    (TUSB_DESC_STRING << 8) | 22, 'K', 'V', 'M', 'p', 'a', 'd', ' ', 'G', 'U', 'D'
};

const uint8_t *usb_gud_device_descriptor(void) { return (const uint8_t *)&device; }
const uint8_t *usb_gud_config_descriptor(void) { return configuration; }
const uint16_t *usb_gud_product_string(void) { return product; }

// Both 24-byte mode records fit in one EP0 IN packet.
bool tud_vendor_control_xfer_cb(uint8_t port, uint8_t stage, const tusb_control_request_t *r)
{
    static uint8_t payload[48];
#ifdef KVM_GUD_GUEST_CHECK
    extern bool gud_test_winusb_request(uint8_t, uint8_t, const tusb_control_request_t *);
    if (r->bmRequestType == 0xc0) return gud_test_winusb_request(port, stage, r);
#endif
    if (!usb_monitor_port_gud_ready() || r->wIndex != 0) return false;
    if (r->bmRequestType == 0xc1) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        size_t cap = r->wLength < sizeof(payload) ? r->wLength : sizeof(payload);
        int len = gud_panel_get(r->bRequest, r->wValue, payload, cap);
        return len >= 0 && (size_t)len <= cap && tud_control_xfer(port, r, payload, (uint16_t)len);
    }
    if (r->bmRequestType != 0x41) return false;
    if (stage == CONTROL_STAGE_SETUP) {
        if (r->wLength > 26) {
            (void)gud_panel_set(r->bRequest, r->wValue, NULL, r->wLength);
            return false;
        }
        if (!r->wLength)
            return gud_panel_set(r->bRequest, r->wValue, NULL, 0) && tud_control_status(port, r);
        // TinyUSB reports wLength even after a short OUT. Every valid SET
        // payload of this fixed R1 profile ends in 0 or 1. An unwritten tail
        // remains invalid and the receiver records the appropriate status.
        memset(payload, 0xff, r->wLength);
        return tud_control_xfer(port, r, payload, r->wLength);
    }
    if (stage == CONTROL_STAGE_DATA)
        return gud_panel_set(r->bRequest, r->wValue, payload, r->wLength);
    return true;
}
