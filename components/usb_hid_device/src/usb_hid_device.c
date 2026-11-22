// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TinyUSB device: descriptors, the tud_* callbacks, USB state and the transport for the HID
// scheduler. Every tud_hid_* call in the firmware lives in this file and runs in the TinyUSB task
// (TECH-034): the scheduler is executed there through usbd_defer_func() (TinyUSB usbd_pvt.h of the
// pinned esp_tinyusb 3.0.0), never in the producer tasks.
#include "usb_hid_device.h"

#include <stdio.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "device/usbd_pvt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "usb_monitor_port.h"
#endif

#if KVM_HPE_FOUR
#include "hpe_descriptors.h"
#include "hpe_studio.h"
#include "esp_app_desc.h"
#include "hpe_cert.h"
#endif
_Static_assert(KVM_HID_ITF_COUNT == CONFIG_TINYUSB_HID_COUNT && KVM_HID_ITF_COUNT == CFG_TUD_HID,
               "USB configuration must match the number of TinyUSB HID instances");

static const char *TAG = "usb_hid";

// ---- Identity --------------------------------------------------------------------------------
// TECH-037: Espressif's VID with the esp_tinyusb default PID for 3 HID interfaces is for the
// prototype only. A distributed product needs its own VID/PID (separate ADR).
#define KVM_USB_VID       0x303A
#define KVM_USB_PID       0x400C
#if KVM_HPE_FOUR
#define KVM_USB_BCD       0x0101
#elif KVM_HID_ITF_COUNT == 2
#define KVM_USB_BCD       0x0102
#else
#define KVM_USB_BCD       0x0100
#endif
#define KVM_USB_POWER_MA  500

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_KEYBOARD,
    STRID_MOUSE,
#if KVM_HPE_FOUR
    STRID_PTP,
#endif
#if KVM_HID_ITF_COUNT > 2
    STRID_CONSUMER,
#endif
    STRID_COUNT,
};

#define EP_KEYBOARD  0x81
#define EP_MOUSE     0x82
#define EP_CONSUMER  0x83
#define EP_SIZE      8
#define POLL_MS_INPUT    1     // keyboard and mouse: bInterval 1 ms (full speed)
#define POLL_MS_CONSUMER 10

static const uint8_t s_report_keyboard[] = {TUD_HID_REPORT_DESC_KEYBOARD()};
static const uint8_t s_report_mouse[] = {TUD_HID_REPORT_DESC_MOUSE()};
#if KVM_HPE_FOUR
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 3 * TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
_Static_assert(CFG_TUD_HID_EP_BUFSIZE == 64, "IF3 has a 64-byte interrupt OUT transfer");
#else
#if KVM_HID_ITF_COUNT == 3
static const uint8_t s_report_consumer[] = {TUD_HID_REPORT_DESC_CONSUMER()};
#endif
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + KVM_HID_ITF_COUNT * TUD_HID_DESC_LEN)
#endif

// No remote-wakeup attribute: waking the HOST is not a v1 feature (spec 03 §5.3).
static const uint8_t s_config_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, KVM_HID_ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, KVM_USB_POWER_MA),
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_KEYBOARD, STRID_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_keyboard),
                       EP_KEYBOARD, EP_SIZE, POLL_MS_INPUT),
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_MOUSE, STRID_MOUSE, HID_ITF_PROTOCOL_MOUSE, sizeof(s_report_mouse), EP_MOUSE,
                       EP_SIZE, POLL_MS_INPUT),
#if KVM_HPE_FOUR
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_PTP, STRID_PTP, HID_ITF_PROTOCOL_NONE, sizeof(s_report_ptp), 0x83, 64, 1),
    TUD_HID_INOUT_DESCRIPTOR(KVM_HID_ITF_CONSUMER, STRID_CONSUMER, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_report_control), 0x05, 0x84, 64, 4),
#elif KVM_HID_ITF_COUNT == 3
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_CONSUMER, STRID_CONSUMER, HID_ITF_PROTOCOL_NONE, sizeof(s_report_consumer),
                       EP_CONSUMER, EP_SIZE, POLL_MS_CONSUMER),
#endif
};
_Static_assert(sizeof(s_config_fs) == CONFIG_TOTAL_LEN, "USB configuration length");

static const tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00, // class per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KVM_USB_VID,
    .idProduct = KVM_USB_PID,
    .bcdDevice = KVM_USB_BCD,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

static char s_serial[13];
#if CONFIG_KVM_USB_MONITOR_EVAL
const char *usb_hid_device_serial(void) { return s_serial; }
#endif
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "D-Ogi",
    [STRID_PRODUCT] = "KVMpad",
    [STRID_SERIAL] = s_serial,
    [STRID_KEYBOARD] = "KVMpad Keyboard",
    [STRID_MOUSE] = "KVMpad Mouse",
#if KVM_HPE_FOUR
    [STRID_PTP] = "KVMpad Precision Touchpad",
    [STRID_CONSUMER] = "KVMpad Radial and Control Deck",
#elif KVM_HID_ITF_COUNT == 3
    [STRID_CONSUMER] = "KVMpad Consumer Control",
#endif
};

// ---- State -----------------------------------------------------------------------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_hid_status_t s_st;
static bool s_neutral_pending[KVM_HID_ITF_COUNT]; // TinyUSB task only
static usb_hid_transport_t s_tr;                   // set before start, read-only afterwards
static volatile bool s_pump_queued;                // a deferred pump call is in the TinyUSB queue
static TaskHandle_t s_tusb_task;                   // recorded in the TinyUSB-task callbacks
// Keyboard idle timing runs in the TinyUSB task. The rate is in units of 4 ms.
static uint8_t s_keyboard_idle;
static int64_t s_keyboard_last_report, s_keyboard_deadline;

#define ST_LOCK()   portENTER_CRITICAL(&s_mux)
#define ST_UNLOCK() portEXIT_CRITICAL(&s_mux)

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static struct {
    bool active, available;
    int64_t start, last, queued, deadline;
    usb_hid_service_window_t window;
} s_service;

static uint32_t link_changes(void)
{
    return s_st.mounts + s_st.umounts + s_st.suspends + s_st.resumes;
}

// Count both partial intervals at the window edges, including a window with no service.
static void service_gap(int64_t now)
{
    uint32_t gap = (uint32_t)(now - s_service.last);
    if (gap > s_service.window.max_gap_us) s_service.window.max_gap_us = gap;
    if (gap > 20000) ++s_service.window.gaps_over_20ms;
    s_service.last = now;
}

static void service_close(int64_t now)
{
    usb_hid_service_window_t *w = &s_service.window;
    w->expired = now >= s_service.deadline;
    int64_t end = w->expired ? s_service.deadline : now;
    service_gap(end);
    w->elapsed_us = (uint32_t)(end - s_service.start);
    w->pending_us = s_service.queued ? (uint32_t)(end - s_service.queued) : 0;
    w->sent = s_st.reports_sent - w->sent;
    w->complete = s_st.report_complete - w->complete;
    w->refused = s_st.reports_refused - w->refused;
    w->failed = s_st.report_failed - w->failed;
    w->link_changes = link_changes() - w->link_changes;
    s_service.active = false;
}

bool usb_hid_service_begin(unsigned limit_ms)
{
    if (limit_ms < 100 || limit_ms > 120000) return false;
    ST_LOCK();
    int64_t now = esp_timer_get_time();
    if (s_service.active && now >= s_service.deadline) service_close(now);
    bool ok = !s_service.active && s_st.state == KVM_USB_MOUNTED && s_tr.pump;
    if (ok) {
        s_service.active = s_service.available = true;
        s_service.last = s_service.start = now;
        s_service.deadline = now + (int64_t)limit_ms * 1000;
        s_service.queued = 0;
        s_service.window = (usb_hid_service_window_t){
            .sent = s_st.reports_sent, .complete = s_st.report_complete, .refused = s_st.reports_refused,
            .failed = s_st.report_failed, .link_changes = link_changes(),
        };
    }
    ST_UNLOCK();
    return ok;
}

bool usb_hid_service_end(usb_hid_service_window_t *out)
{
    ST_LOCK();
    bool available = s_service.available;
    if (available) {
        if (s_service.active) service_close(esp_timer_get_time());
        *out = s_service.window;
        s_service.available = false;
    }
    ST_UNLOCK();
    return available;
}

// Called only on entry to the real TinyUSB task callback with s_mux held.
static void service_arrived(void)
{
    int64_t now = esp_timer_get_time();
    if (now >= s_service.deadline) {
        service_close(now);
        return;
    }
    service_gap(now);
    ++s_service.window.services;
    if (s_service.queued) {
        uint32_t defer = (uint32_t)(now - s_service.queued);
        if (defer > s_service.window.max_defer_us) s_service.window.max_defer_us = defer;
        ++s_service.window.timed_requests;
        s_service.queued = 0;
    }
    bool ready = s_st.state == KVM_USB_MOUNTED;
    for (unsigned itf = 0; ready && itf < usb_hid_device_interface_count(); ++itf)
        ready = tud_hid_n_ready(itf);
    if (ready) ++s_service.window.all_ready;
}
#endif

const char *usb_hid_state_name(kvm_usb_state_t s)
{
    switch (s) {
    case KVM_USB_UNINITIALIZED: return "USB_UNINITIALIZED";
    case KVM_USB_DETACHED: return "USB_DETACHED";
    case KVM_USB_ENUMERATING: return "USB_ENUMERATING";
    case KVM_USB_MOUNTED: return "USB_MOUNTED";
    case KVM_USB_SUSPENDED: return "USB_SUSPENDED";
    case KVM_USB_ERROR: return "USB_ERROR";
    default: return "?";
    }
}

void usb_hid_device_get_status(usb_hid_status_t *out)
{
    ST_LOCK();
    *out = s_st;
    ST_UNLOCK();
}

static void set_state(kvm_usb_state_t s)
{
    ST_LOCK();
    s_st.state = s;
    ST_UNLOCK();
}

// TinyUSB task: try to queue the neutral report of `itf`; keeps it pending when the IN
// endpoint is still busy (retried from tud_hid_report_complete_cb).
static void send_neutral(uint8_t itf)
{
    uint8_t buf[HPE_PTP_WIRE];
    uint8_t protocol = tud_hid_n_get_protocol(itf);
    size_t len = kvm_hid_build_neutral(itf, protocol, buf);
    uint8_t id = 0;
#if KVM_HPE_FOUR
    if (itf == KVM_HID_ITF_PTP) {
        const hpe_frame_t empty = {0};
        len = hpe_encode_ptp(buf, &empty);
    } else if (itf == KVM_HID_ITF_CONSUMER) {
        id = HPE_CONSUMER;
    }
#endif
    bool ok = len && tud_hid_n_ready(itf) && tud_hid_n_report(itf, id, buf, (uint16_t)len);
    s_neutral_pending[itf] = !ok;
    ST_LOCK();
    if (ok) {
        s_st.neutral_sent++;
    } else {
        s_st.neutral_failed++;
    }
    ST_UNLOCK();
}

// TinyUSB task: forward the current state to the scheduler.
static void notify_state(void)
{
    if (s_tr.on_state == NULL) {
        return;
    }
    ST_LOCK();
    kvm_usb_state_t st = s_st.state;
    uint32_t epoch = s_st.usb_epoch;
    ST_UNLOCK();
    s_tr.on_state(s_tr.ctx, st, epoch);
}

static void keyboard_deadline(int64_t deadline)
{
    if ((s_keyboard_deadline != 0) != (deadline != 0)) tud_sof_cb_enable(deadline != 0);
    s_keyboard_deadline = deadline;
}

static void keyboard_reported(void)
{
    s_keyboard_last_report = esp_timer_get_time();
    keyboard_deadline(s_keyboard_idle ? s_keyboard_last_report + 4000 * s_keyboard_idle : 0);
}

static void configuration_lost(kvm_usb_state_t state)
{
    s_keyboard_idle = 0;
    keyboard_deadline(0);
    memset(s_neutral_pending, 0, sizeof(s_neutral_pending));
    ST_LOCK();
    s_st.state = state;
    s_st.ptp = (hpe_features_t)HPE_FEATURE_DEFAULTS;
    s_st.led_state = 0;
    s_st.led_known = false;
    s_st.remote_wakeup_enabled = false;
    ST_UNLOCK();
    notify_state();
}

// TinyUSB calls its class reset in the USB task on bus reset, unplug and
// SET_CONFIGURATION(0). A bus reset alone does not invoke tud_umount_cb.
void __real_hidd_reset(uint8_t rhport);
void __wrap_hidd_reset(uint8_t rhport)
{
    // On unplug the ISR clears cfg_num before the class reset; the SOF consumer
    // bit is still available here and is erased before the detach callback.
    s_keyboard_idle = 0;
    keyboard_deadline(0);
    __real_hidd_reset(rhport);
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (usb_monitor_port_owns_bus()) {
        usb_monitor_port_reset();
        if (!usb_hid_device_interface_count()) return;
    }
#endif
    // The class reset runs before TinyUSB clears the previous configuration.
    if (tud_mounted())
        configuration_lost(KVM_USB_ENUMERATING);
}

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    s_tusb_task = xTaskGetCurrentTaskHandle();
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (usb_monitor_port_owns_bus()) {
        if (event->id == TINYUSB_EVENT_ATTACHED)
            usb_monitor_port_mount();
        if (event->id == TINYUSB_EVENT_SUSPENDED)
            usb_monitor_port_suspend(true);
        if (event->id == TINYUSB_EVENT_RESUMED)
            usb_monitor_port_suspend(false);
        if (event->id == TINYUSB_EVENT_DETACHED)
            usb_monitor_port_unmount();
        if (!usb_hid_device_interface_count()) return;
        if ((event->id == TINYUSB_EVENT_ATTACHED || event->id == TINYUSB_EVENT_RESUMED) &&
            !usb_hid_device_monitor_mounted()) return;
    }
#endif
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED: {
        s_keyboard_idle = 0;
        keyboard_reported();
        ST_LOCK();
        s_st.state = KVM_USB_MOUNTED;
        s_st.mounts++;
        s_st.ptp = (hpe_features_t)HPE_FEATURE_DEFAULTS;
        s_st.ptp_generation++;
        s_st.usb_epoch++;
        s_st.led_known = false;
        s_st.led_state = 0;
        s_st.last_mount_us = (uint64_t)esp_timer_get_time();
        for (int i = 0; i < KVM_HID_ITF_COUNT; i++) {
            s_st.protocol[i] = KVM_HID_PROTOCOL_REPORT; // HID 1.11: report protocol after configuration
        }
        uint32_t epoch = s_st.usb_epoch;
        ST_UNLOCK();
        if (s_tr.on_state != NULL) {
            s_tr.on_state(s_tr.ctx, KVM_USB_MOUNTED, epoch); // the scheduler sends the neutral start (TECH-035)
        } else {
            // TECH-035 without a scheduler: start every mount from a neutral keyboard, mouse and consumer.
            for (uint8_t i = 0; i < usb_hid_device_interface_count(); i++) {
                send_neutral(i);
            }
        }
        ESP_LOGI(TAG, "mounted (epoch %lu)", (unsigned long)epoch);
        break;
    }
    case TINYUSB_EVENT_DETACHED:
        // Logical state resets now; nothing queued for the old epoch is replayed (TECH-035).
        ST_LOCK();
        s_st.umounts++;
        ST_UNLOCK();
        configuration_lost(KVM_USB_DETACHED);
        ESP_LOGI(TAG, "detached");
        break;
    case TINYUSB_EVENT_SUSPENDED:
        keyboard_deadline(0);
        ST_LOCK();
        s_st.state = KVM_USB_SUSPENDED;
        s_st.suspends++;
        s_st.remote_wakeup_enabled = event->suspended.remote_wakeup;
        ST_UNLOCK();
        notify_state();
        ESP_LOGI(TAG, "suspended (remote wakeup %s by HOST)", event->suspended.remote_wakeup ? "enabled" : "disabled");
        break;
    case TINYUSB_EVENT_RESUMED:
        keyboard_reported();
        ST_LOCK();
        s_st.state = tud_mounted() ? KVM_USB_MOUNTED : KVM_USB_DETACHED;
        s_st.resumes++;
        ST_UNLOCK();
        notify_state();
        ESP_LOGI(TAG, "resumed");
        break;
    default:
        break;
    }
}

// ---- TinyUSB HID callbacks (TinyUSB task) --------------------------------------------------------
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    switch (instance) {
    case KVM_HID_ITF_KEYBOARD: return s_report_keyboard;
    case KVM_HID_ITF_MOUSE: return s_report_mouse;
#if KVM_HPE_FOUR
    case KVM_HID_ITF_PTP: return s_report_ptp;
    case KVM_HID_ITF_CONSUMER: return s_report_control;
#elif KVM_HID_ITF_COUNT == 3
    case KVM_HID_ITF_CONSUMER: return s_report_consumer;
#endif
    default: return NULL;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    ST_LOCK();
    s_st.get_report_requests++;
    uint8_t led = s_st.led_state;
    ST_UNLOCK();
#if KVM_HPE_FOUR
    if (instance == KVM_HID_ITF_PTP && report_type == HID_REPORT_TYPE_FEATURE) {
        return (uint16_t)hpe_feature_get(&s_st.ptp, report_id, buffer, reqlen);
    }
    if (instance == KVM_HID_ITF_CONSUMER && report_type == HID_REPORT_TYPE_FEATURE && report_id == HPE_STUDIO_FEATURE) {
        // Capabilities reflect the transport implemented here. Agent feedback needs a session.
        uint8_t caps[15] = {1, 0, HPE_DEVICE_CAPS, 0, 8, 51};
        caps[6] = s_tr.get_profile ? s_tr.get_profile(s_tr.ctx) : HPE_MOUSE_PROFILE;
        memcpy(caps + 7, esp_app_get_description()->app_elf_sha256, 4);
        uint16_t n = reqlen < sizeof(caps) ? reqlen : (uint16_t)sizeof(caps);
        memcpy(buffer, caps, n);
        return n;
    }
    if (instance >= KVM_HID_ITF_PTP || report_id != 0) return 0;
#endif
    if (instance == KVM_HID_ITF_KEYBOARD && report_id == 0 && report_type == HID_REPORT_TYPE_OUTPUT && reqlen >= 1) {
        buffer[0] = led;
        return 1;
    }
    if (report_type == HID_REPORT_TYPE_INPUT && report_id == 0 && instance < usb_hid_device_interface_count()) {
        uint8_t tmp[8];
        uint8_t protocol = tud_hid_n_get_protocol(instance);
        size_t len = s_tr.get_input_report ? s_tr.get_input_report(s_tr.ctx, instance, protocol, tmp)
                                           : kvm_hid_build_neutral(instance, protocol, tmp);
        if (len > reqlen) {
            len = reqlen;
        }
        memcpy(buffer, tmp, len);
        return (uint16_t)len;
    }
    return 0; // STALL
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer,
                           uint16_t bufsize)
{
#if KVM_HPE_FOUR
    if (instance == KVM_HID_ITF_PTP && report_type == HID_REPORT_TYPE_FEATURE) {
        ST_LOCK();
        hpe_features_t prev = s_st.ptp;
        bool accepted = hpe_feature_set(&s_st.ptp, report_id, buffer, bufsize);
        bool changed = accepted && (prev.mode != s_st.ptp.mode || ((prev.selective ^ s_st.ptp.selective) & 1));
        if (changed) s_st.ptp_generation++;
        uint32_t generation = s_st.ptp_generation;
        bool native = s_st.ptp.mode == 3;
        bool surface = (s_st.ptp.selective & 1) != 0;
        ST_UNLOCK();
        if (changed && s_tr.on_ptp_route) s_tr.on_ptp_route(s_tr.ctx, native, surface, generation);
        return;
    }
    if (instance == KVM_HID_ITF_CONSUMER && report_type == HID_REPORT_TYPE_OUTPUT) {
        hpe_packet_t packet;
        bool valid = hpe_decode_output(report_id, buffer, bufsize, &packet);
        valid = valid && s_tr.on_studio_output && s_tr.on_studio_output(s_tr.ctx, &packet);
        ST_LOCK();
        if (valid) s_st.studio_frames++;
        else s_st.studio_invalid++;
        ST_UNLOCK();
        return;
    }
#endif
    if (instance != KVM_HID_ITF_KEYBOARD || report_id != 0 || report_type != HID_REPORT_TYPE_OUTPUT ||
        !buffer || bufsize != 1) {
        return;
    }
    uint8_t led = buffer[0] & 0x1F;
    ST_LOCK();
    if (s_st.state != KVM_USB_MOUNTED) {
        ST_UNLOCK();
        return;
    }
    bool changed = !s_st.led_known || led != s_st.led_state;
    s_st.led_state = led;
    s_st.led_known = true;
    s_st.led_reports++;
    ST_UNLOCK();
    if (changed) {
        // Lock-LED state only: authoritative domain data for the UI (AGENTS-UI #5), not a keystroke.
        ESP_LOGI(TAG, "LED output 0x%02X (num=%d caps=%d scroll=%d)", led, !!(led & KVM_HID_LED_NUM_LOCK),
                 !!(led & KVM_HID_LED_CAPS_LOCK), !!(led & KVM_HID_LED_SCROLL_LOCK));
    }
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    if (instance >= KVM_HID_ITF_COUNT) {
        return;
    }
    ST_LOCK();
    s_st.protocol[instance] = protocol;
    s_st.set_protocol_count++;
    ST_UNLOCK();
    ESP_LOGI(TAG, "itf %u SET_PROTOCOL(%s)", instance, protocol == KVM_HID_PROTOCOL_BOOT ? "BOOT" : "REPORT");
}

void tud_sof_cb(uint32_t frame_count)
{
    (void)frame_count;
    if (!s_keyboard_deadline || esp_timer_get_time() < s_keyboard_deadline ||
        !tud_hid_n_ready(KVM_HID_ITF_KEYBOARD)) return;
    if (s_tr.repeat_keyboard) s_tr.repeat_keyboard(s_tr.ctx);
    else if (!s_tr.pump) send_neutral(KVM_HID_ITF_KEYBOARD);
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (instance >= usb_hid_device_interface_count()) return;
#endif
    (void)report;
    (void)len;
    ST_LOCK();
    s_st.report_complete++;
    ST_UNLOCK();
    if (instance >= KVM_HID_ITF_COUNT) {
        return;
    }
    if (instance == KVM_HID_ITF_KEYBOARD && tud_mounted() && !tud_suspended()) keyboard_reported();
    if (s_tr.on_complete != NULL) {
        s_tr.on_complete(s_tr.ctx, instance);
    } else if (s_neutral_pending[instance]) {
        send_neutral(instance);
    }
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type, uint8_t const *report,
                              uint16_t xferred_bytes)
{
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (instance >= usb_hid_device_interface_count()) return;
#endif
    (void)report_type;
    (void)report;
    (void)xferred_bytes;
    ST_LOCK();
    s_st.report_failed++;
    ST_UNLOCK();
    if (report_type == HID_REPORT_TYPE_INPUT && instance < KVM_HID_ITF_COUNT && s_tr.on_failed) {
        s_tr.on_failed(s_tr.ctx, instance);
    }
}

// ---- Transport for the HID scheduler -----------------------------------------------------------
esp_err_t usb_hid_device_set_transport(const usb_hid_transport_t *t)
{
    ESP_RETURN_ON_FALSE(t != NULL && t->pump != NULL, ESP_ERR_INVALID_ARG, TAG, "transport");
    ESP_RETURN_ON_FALSE(s_st.state == KVM_USB_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "set before start");
    s_tr = *t;
    return ESP_OK;
}

static void pump_trampoline(void *param)
{
    (void)param;
    s_tusb_task = xTaskGetCurrentTaskHandle();
    ST_LOCK();
    s_pump_queued = false; // requests made while the pump runs schedule the next run
    s_st.pumps++;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (s_service.active) service_arrived();
#endif
    ST_UNLOCK();
#if CONFIG_KVM_USB_MONITOR_EVAL
    usb_monitor_port_poll_reconnect();
#endif
    s_tr.pump(s_tr.ctx);
}

void usb_hid_device_request_pump(void)
{
    if (s_tr.pump == NULL) {
        return;
    }
    bool queue_it = false;
    ST_LOCK();
    if (!s_pump_queued && s_st.state != KVM_USB_UNINITIALIZED && s_st.state != KVM_USB_ERROR) {
        s_pump_queued = true;
        queue_it = true;
        s_st.pump_requests++;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (s_service.active) {
            int64_t now = esp_timer_get_time();
            if (now >= s_service.deadline) service_close(now);
            else s_service.queued = now;
        }
#endif
    }
    ST_UNLOCK();
    if (queue_it) {
        // At most one pending call: pump requests never fill the TinyUSB event queue.
        usbd_defer_func(pump_trampoline, NULL, false);
    }
}

bool usb_hid_device_send(uint8_t itf, const uint8_t *report, uint8_t len)
{
    if (s_tusb_task == NULL || xTaskGetCurrentTaskHandle() != s_tusb_task) {
        ST_LOCK();
        s_st.send_wrong_context++;
        ST_UNLOCK();
        return false; // TECH-034
    }
    // The scheduler stages the complete on-wire bytes, including IDs for IF2/IF3.
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (itf >= usb_hid_device_interface_count() || (usb_monitor_port_owns_bus() && !usb_hid_device_monitor_mounted())) return false;
#endif
    bool ok = report && len && len <= CFG_TUD_HID_EP_BUFSIZE && itf < KVM_HID_ITF_COUNT && tud_mounted() && !tud_suspended() && tud_hid_n_ready(itf) &&
              tud_hid_n_report(itf, 0, report, len);
    ST_LOCK();
    if (ok) {
        s_st.reports_sent++;
    } else {
        s_st.reports_refused++;
    }
    ST_UNLOCK();
    return ok;
}

uint8_t usb_hid_device_protocol(uint8_t itf)
{
    if (s_tusb_task == NULL || xTaskGetCurrentTaskHandle() != s_tusb_task || itf >= usb_hid_device_interface_count()) {
        return KVM_HID_PROTOCOL_REPORT;
    }
    return tud_hid_n_get_protocol(itf);
}

// The pinned HID class reports SETUP.wLength at ACK even for a short OUT. For the
// one-byte keyboard report, the core copies the received byte from its EP0 buffer
// to our byte before DATA. Start them distinct: equality then proves receipt for
// every possible value, including 0 (all LEDs off). Publish only after ACK.
static bool keyboard_led_control(uint8_t port, uint8_t stage, tusb_control_request_t const *r)
{
    static uint8_t led;
    static uint32_t epoch;
    if (stage == CONTROL_STAGE_SETUP) {
        ST_LOCK();
        bool mounted = s_st.state == KVM_USB_MOUNTED;
        epoch = s_st.usb_epoch;
        ST_UNLOCK();
        if (!mounted) return false;
        led = (uint8_t)~usbd_get_ctrl_buf()[0];
        return tud_control_xfer(port, r, &led, 1);
    }
    if (stage == CONTROL_STAGE_DATA) return led == usbd_get_ctrl_buf()[0];
    if (stage == CONTROL_STAGE_ACK) {
        ST_LOCK();
        bool current = s_st.usb_epoch == epoch && s_st.state == KVM_USB_MOUNTED;
        ST_UNLOCK();
        if (current) tud_hid_set_report_cb(KVM_HID_ITF_KEYBOARD, 0, HID_REPORT_TYPE_OUTPUT, &led, 1);
    }
    return true;
}

bool __real_hidd_control_xfer_cb(uint8_t port, uint8_t stage, tusb_control_request_t const *request);
bool __wrap_hidd_control_xfer_cb(uint8_t port, uint8_t stage, tusb_control_request_t const *r)
{
    uint8_t keyboard = KVM_HID_ITF_KEYBOARD;
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (usb_monitor_port_owns_bus()) {
        if (!usb_hid_device_interface_count() || !usb_hid_device_monitor_mounted()) return false;
        keyboard = 1; // Composite MI_00 is the display or screenshot storage.
        // Neither boot device implements feature reports. In particular MI_02
        // is now the mouse and must not expose the normal touchpad's certificate.
        if ((r->bmRequestType == 0xa1 && r->bRequest == HID_REQ_CONTROL_GET_REPORT) ||
            (r->bmRequestType == 0x21 && r->bRequest == HID_REQ_CONTROL_SET_REPORT)) {
            if ((r->wValue >> 8) == HID_REPORT_TYPE_FEATURE) return false;
        }
    }
#endif
    if (r->bmRequestType == 0x21 && r->bRequest == HID_REQ_CONTROL_SET_REPORT &&
        r->wIndex == keyboard && r->wValue == (HID_REPORT_TYPE_OUTPUT << 8) && r->wLength == 1)
        return keyboard_led_control(port, stage, r);
    if (r->wIndex == keyboard && r->bmRequestType == 0x21 && r->bRequest == HID_REQ_CONTROL_SET_IDLE) {
        if ((r->wValue & 0xff) || r->wLength) return false; // Boot keyboard has no report IDs.
        if (stage == CONTROL_STAGE_SETUP) return tud_control_status(port, r);
        if (stage == CONTROL_STAGE_ACK) {
            int64_t now = esp_timer_get_time();
            s_keyboard_idle = (uint8_t)(r->wValue >> 8);
            // HID 1.11 section 7.2.4: finish the current period if fewer than 4 ms remain.
            if (!s_keyboard_deadline || now <= s_keyboard_deadline - 4000 || now >= s_keyboard_deadline)
                keyboard_deadline(s_keyboard_idle ? s_keyboard_last_report + 4000 * s_keyboard_idle : 0);
        }
        return true;
    }
    if (r->wIndex == keyboard && r->bmRequestType == 0xa1 && r->bRequest == HID_REQ_CONTROL_GET_IDLE) {
        if (r->wValue || r->wLength != 1) return false;
        return stage != CONTROL_STAGE_SETUP || tud_control_xfer(port, r, &s_keyboard_idle, 1);
    }
#if KVM_HPE_FOUR
    // Delegate the 257-byte certificate directly to the core's multi-packet transfer.
    if (r->bmRequestType == 0xa1 && r->bRequest == HID_REQ_CONTROL_GET_REPORT &&
        r->wIndex == KVM_HID_ITF_PTP && r->wValue == ((HID_REPORT_TYPE_FEATURE << 8) | HPE_CERT)) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        return tud_control_xfer(port, r, (void *)s_ptp_cert_report, sizeof(s_ptp_cert_report));
    }
#endif
    return __real_hidd_control_xfer_cb(port, stage, r);
}

// ---- Start -----------------------------------------------------------------------------------
#if CONFIG_KVM_USB_MONITOR_EVAL
static void monitor_lost_hid(void)
{
    configuration_lost(KVM_USB_DETACHED);
}

esp_err_t usb_hid_device_set_monitor(bool active)
{
    return usb_hid_device_set_monitor_mode(active, false);
}

static bool display_port_ready(void)
{
    ST_LOCK();
    bool ready = s_st.state != KVM_USB_UNINITIALIZED && s_st.state != KVM_USB_ERROR;
    ST_UNLOCK();
    return ready;
}

#if CONFIG_KVM_USB_GUD
esp_err_t usb_hid_device_set_gud(bool active)
{
    if (!display_port_ready()) return ESP_ERR_INVALID_STATE;
    return usb_monitor_port_select_gud(active) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
#endif

static esp_err_t select_display(bool active, bool native, bool controls, bool windows_modes)
{
    if (!display_port_ready()) return ESP_ERR_INVALID_STATE;
    return usb_monitor_port_select(active, native, controls, windows_modes) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t usb_hid_device_set_monitor_mode(bool active, bool native)
{
    return select_display(active, native, false, false);
}

esp_err_t usb_hid_device_set_monitor_controls(bool active, bool native)
{
    return select_display(active, native, true, false);
}

esp_err_t usb_hid_device_set_monitor_windows(bool controls)
{
    return select_display(true, true, controls, true);
}
#else
uint8_t usb_hid_device_interface_count(void)
{
    return KVM_HID_ITF_COUNT;
}
#endif

esp_err_t usb_hid_device_start(void)
{
    ESP_RETURN_ON_FALSE(s_st.state == KVM_USB_UNINITIALIZED, ESP_ERR_INVALID_STATE, TAG, "already started");
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_efuse_mac_get_default(mac), TAG, "mac");
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    tinyusb_config_t cfg = TINYUSB_DEFAULT_CONFIG(usb_event_cb, NULL);
    cfg.descriptor.device = &s_device;
    cfg.descriptor.string = s_strings;
    cfg.descriptor.string_count = STRID_COUNT;
    cfg.descriptor.full_speed_config = s_config_fs;
#if CONFIG_KVM_USB_MONITOR_EVAL
    ESP_RETURN_ON_ERROR(usb_monitor_port_init(monitor_lost_hid), TAG, "monitor port");
#endif
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
#if CONFIG_KVM_USB_MONITOR_EVAL
        usb_monitor_port_deinit();
#endif
        set_state(KVM_USB_ERROR);
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    ST_LOCK();
    if (s_st.state == KVM_USB_UNINITIALIZED) {
        s_st.state = KVM_USB_ENUMERATING;
    }
    ST_UNLOCK();
    ESP_LOGI(TAG, "TinyUSB started: VID 0x%04X PID 0x%04X (prototype IDs, TECH-037), %d HID interfaces, serial %s",
             KVM_USB_VID, KVM_USB_PID, KVM_HID_ITF_COUNT, s_serial);
    return ESP_OK;
}
