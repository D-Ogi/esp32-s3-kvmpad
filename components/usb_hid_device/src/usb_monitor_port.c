// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "usb_monitor_port.h"
#include "usb_hid_device.h"

#include <stdatomic.h>
#include <uchar.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_monitor.h"

#if CFG_TUD_VENDOR != 1 || CFG_TUD_VENDOR_TXRX_BUFFERED
#error "The monitor evaluation profile requires a single unbuffered vendor class."
#endif

// Espressif usb_extend_screen and the xfz1986 single-function device profile.
enum { PRODUCT = 2, SERIAL = 3, VENDOR_INTERFACE = 4, CONTROLS_KEYBOARD = 5, CONTROLS_MOUSE = 6 };
static const tusb_desc_device_t s_monitor_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x2987,
    .bcdDevice = 0x0101, .iManufacturer = 1, .iProduct = PRODUCT,
    .iSerialNumber = SERIAL, .bNumConfigurations = 1,
};
static const uint8_t s_monitor_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 500),
    TUD_VENDOR_DESCRIPTOR(0, VENDOR_INTERFACE, 0x01, 0x81, 64),
};

// The signed Windows INF binds 303a:2986 only on vendor interface MI_00.
static const tusb_desc_device_t s_controls_device = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .idVendor = 0x303a, .idProduct = 0x2986,
    .bcdDevice = 0x0101, .iManufacturer = 1, .iProduct = PRODUCT,
    .iSerialNumber = SERIAL, .bNumConfigurations = 1,
};
enum { CONTROLS_LENGTH = TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + 2 * TUD_HID_DESC_LEN };
static const uint8_t s_controls_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, CONTROLS_LENGTH, 0, 500),
    TUD_VENDOR_DESCRIPTOR(0, VENDOR_INTERFACE, 0x01, 0x81, 64),
    TUD_HID_DESCRIPTOR(1, CONTROLS_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof((uint8_t[]){TUD_HID_REPORT_DESC_KEYBOARD()}), 0x82, 8, 1),
    TUD_HID_DESCRIPTOR(2, CONTROLS_MOUSE, HID_ITF_PROTOCOL_MOUSE,
                       sizeof((uint8_t[]){TUD_HID_REPORT_DESC_MOUSE()}), 0x83, 8, 1),
};
_Static_assert(sizeof(s_controls_config) == CONTROLS_LENGTH, "composite display descriptor length");

// esp_tinyusb's generic string callback truncates after 31 UTF-16 characters.
// Both the product string and the vendor interface must report the complete limits.
#define DISPLAY_STRING(name, value) static const struct { uint8_t length, type; char16_t text[sizeof(value)/2]; } \
    name = {sizeof(value), TUSB_DESC_STRING, value}
DISPLAY_STRING(s_monitor_caps, u"esp32s3udisp0_R640x480_Ejpg4_Fps10_Bl96");
DISPLAY_STRING(s_native_caps, u"esp32s3udisp0_R1024x600_Ejpg4_Fps10_Bl96");
_Static_assert(sizeof(char16_t) == 2, "USB string code units");

DISPLAY_STRING(s_windows_caps, u"esp32s3udisp0_Ejpg4_Fps10_Bl96");
enum { PORT_HID, PORT_VGA, PORT_NATIVE, PORT_CONTROLS_VGA, PORT_CONTROLS_NATIVE, PORT_WINDOWS, PORT_CONTROLS_WINDOWS };
static atomic_uchar s_wanted;
static _Atomic uint32_t s_passed_bytes, s_filtered_bytes;
static _Atomic kvm_usb_state_t s_link = KVM_USB_DETACHED;
// Only the USB owner changes descriptors or accesses the two state flags.
static uint8_t s_monitor;
static bool s_disconnected;
static bool s_saw_reset;
static esp_timer_handle_t s_reconnect;
static void (*s_lost_hid)(void);

static bool with_controls(void)
{
    return s_monitor == PORT_CONTROLS_VGA || s_monitor == PORT_CONTROLS_NATIVE || s_monitor == PORT_CONTROLS_WINDOWS;
}

uint8_t usb_hid_device_interface_count(void)
{
    if (s_disconnected) return 0;
    if (with_controls()) return 2;
    return s_monitor == PORT_HID ? KVM_HID_ITF_COUNT : 0;
}

bool usb_monitor_port_owns_bus(void)
{
    return s_monitor || s_disconnected;
}

kvm_usb_state_t usb_hid_device_monitor_state(void)
{
    return atomic_load(&s_link);
}

bool usb_hid_device_monitor_mounted(void)
{
    return usb_hid_device_monitor_state() == KVM_USB_MOUNTED;
}

usb_monitor_port_bytes_t usb_hid_device_monitor_bytes(void)
{
    return (usb_monitor_port_bytes_t){
        .passed_bytes = atomic_load_explicit(&s_passed_bytes, memory_order_relaxed),
        .filtered_bytes = atomic_load_explicit(&s_filtered_bytes, memory_order_relaxed),
    };
}

static void set_link(kvm_usb_state_t state)
{
    if (atomic_exchange(&s_link, state) != state)
        ESP_LOGI("usb_hid", "USB_MONITOR mount=%u suspended=%u",
                 (unsigned)(state == KVM_USB_MOUNTED), (unsigned)(state == KVM_USB_SUSPENDED));
}

void usb_monitor_port_unmount(void)
{
    set_link(KVM_USB_DETACHED);
    usb_monitor_reset();
}

void usb_monitor_port_reset(void)
{
    usb_monitor_port_unmount();
    s_saw_reset = s_monitor && !s_disconnected;
}

void usb_monitor_port_mount(void)
{
    if (!s_saw_reset || s_disconnected || !s_monitor || !tud_mounted()) return;
    set_link(KVM_USB_MOUNTED);
}

void usb_monitor_port_suspend(bool suspended)
{
    // The configured endpoints and the current picture survive bus suspend.
    if (usb_hid_device_monitor_state() != KVM_USB_DETACHED && tud_mounted()) {
        set_link(suspended ? KVM_USB_SUSPENDED : KVM_USB_MOUNTED);
    }
}

const uint8_t *__real_tud_descriptor_device_cb(void);
const uint8_t *__wrap_tud_descriptor_device_cb(void)
{
    if (with_controls()) return (const uint8_t *)&s_controls_device;
    return s_monitor ? (const uint8_t *)&s_monitor_device : __real_tud_descriptor_device_cb();
}

const uint8_t *__real_tud_descriptor_configuration_cb(uint8_t index);
const uint8_t *__wrap_tud_descriptor_configuration_cb(uint8_t index)
{
    if (!s_monitor) return __real_tud_descriptor_configuration_cb(index);
    if (index != 0) return NULL;
    return with_controls() ? s_controls_config : s_monitor_config;
}

const uint16_t *__real_tud_descriptor_string_cb(uint8_t index, uint16_t langid);
const uint16_t *__wrap_tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    if (s_monitor) {
        if (index == PRODUCT || index == VENDOR_INTERFACE) {
            if (s_monitor == PORT_WINDOWS || s_monitor == PORT_CONTROLS_WINDOWS) return (const uint16_t *)&s_windows_caps;
            return (s_monitor == PORT_NATIVE || s_monitor == PORT_CONTROLS_NATIVE) ?
                       (const uint16_t *)&s_native_caps : (const uint16_t *)&s_monitor_caps;
        }
        if (with_controls() && (index == CONTROLS_KEYBOARD || index == CONTROLS_MOUSE))
            return __real_tud_descriptor_string_cb(index - 1, langid); // Existing keyboard and mouse names.
        if (index > VENDOR_INTERFACE) return NULL;
    }
    return __real_tud_descriptor_string_cb(index, langid);
}

static void reconnect(void *arg)
{
    (void)arg;
    s_monitor = atomic_load(&s_wanted);
    s_disconnected = false;
    s_saw_reset = false;
    tud_connect();
}

static void timer_cb(void *arg)
{
    (void)arg;
    usbd_defer_func(reconnect, NULL, false);
}

static void select_on_bus(void *arg)
{
    (void)arg;
    if (s_disconnected || atomic_load(&s_wanted) == s_monitor) return;
    s_disconnected = true;
    tud_disconnect();
    s_lost_hid();
    usb_monitor_port_unmount();
    // Hold the soft-disconnect long enough for Windows to remove the previous device.
    if (esp_timer_start_once(s_reconnect, 250000) != ESP_OK) {
        // Preserve the previous identity on an unexpected timer failure.
        s_disconnected = false;
        tud_connect();
    }
}

void usb_monitor_port_select(bool monitor, bool native, bool controls, bool windows_modes)
{
    uint8_t profile = PORT_HID;
    if (monitor && windows_modes) profile = controls ? PORT_CONTROLS_WINDOWS : PORT_WINDOWS;
    else if (monitor) profile = controls ? (native ? PORT_CONTROLS_NATIVE : PORT_CONTROLS_VGA) :
                                     (native ? PORT_NATIVE : PORT_VGA);
    atomic_store(&s_wanted, profile);
    usbd_defer_func(select_on_bus, NULL, false);
}

esp_err_t usb_monitor_port_init(void (*lost_hid)(void))
{
    s_lost_hid = lost_hid;
    const esp_timer_create_args_t timer = {.callback = timer_cb, .name = "usb_reconnect"};
    return esp_timer_create(&timer, &s_reconnect);
}

void usb_monitor_port_deinit(void)
{
    esp_timer_delete(s_reconnect);
    s_reconnect = NULL;
}

#if CFG_TUD_API_V0_19_COMPAT
void tud_vendor_rx_cb(uint8_t index, const uint8_t *data, uint16_t length)
#else
void tud_vendor_rx_cb(uint8_t index, const uint8_t *data, uint32_t length)
#endif
{
    // A later suspend IRQ can precede delivery of this already completed OUT.
    // The USB task updates the link state in the same event order as these callbacks.
    if (index != 0 || !length) return;
    if (usb_hid_device_monitor_mounted() && tud_mounted() && data) {
        atomic_fetch_add_explicit(&s_passed_bytes, length, memory_order_relaxed);
        usb_monitor_feed(data, length);
    } else {
        atomic_fetch_add_explicit(&s_filtered_bytes, length, memory_order_relaxed);
    }
}
