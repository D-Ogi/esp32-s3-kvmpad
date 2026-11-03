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

#if CONFIG_TINYUSB_HID_COUNT != 3
#error "ADR-018: CONFIG_TINYUSB_HID_COUNT must be 3 (keyboard, mouse, consumer)"
#endif

static const char *TAG = "usb_hid";

// ---- Identity --------------------------------------------------------------------------------
// TECH-037: Espressif's VID with the esp_tinyusb default PID for 3 HID interfaces is for the
// prototype only. A distributed product needs its own VID/PID (separate ADR).
#define KVM_USB_VID       0x303A
#define KVM_USB_PID       0x400C
#define KVM_USB_BCD       0x0100   // device release 1.00 (prototype)
#define KVM_USB_POWER_MA  500

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_KEYBOARD,
    STRID_MOUSE,
    STRID_CONSUMER,
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
static const uint8_t s_report_consumer[] = {TUD_HID_REPORT_DESC_CONSUMER()};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + KVM_HID_ITF_COUNT * TUD_HID_DESC_LEN)

// No remote-wakeup attribute: waking the HOST is not a v1 feature (spec 03 §5.3).
static const uint8_t s_config_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, KVM_HID_ITF_COUNT, 0, CONFIG_TOTAL_LEN, 0, KVM_USB_POWER_MA),
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_KEYBOARD, STRID_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_report_keyboard),
                       EP_KEYBOARD, EP_SIZE, POLL_MS_INPUT),
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_MOUSE, STRID_MOUSE, HID_ITF_PROTOCOL_MOUSE, sizeof(s_report_mouse), EP_MOUSE,
                       EP_SIZE, POLL_MS_INPUT),
    TUD_HID_DESCRIPTOR(KVM_HID_ITF_CONSUMER, STRID_CONSUMER, HID_ITF_PROTOCOL_NONE, sizeof(s_report_consumer),
                       EP_CONSUMER, EP_SIZE, POLL_MS_CONSUMER),
};

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
static const char *s_strings[STRID_COUNT] = {
    [STRID_LANGID] = (const char[]){0x09, 0x04},
    [STRID_MANUFACTURER] = "D-Ogi",
    [STRID_PRODUCT] = "KVMpad",
    [STRID_SERIAL] = s_serial,
    [STRID_KEYBOARD] = "KVMpad Keyboard",
    [STRID_MOUSE] = "KVMpad Mouse",
    [STRID_CONSUMER] = "KVMpad Consumer Control",
};

// ---- State -----------------------------------------------------------------------------------
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_hid_status_t s_st;
static bool s_neutral_pending[KVM_HID_ITF_COUNT]; // TinyUSB task only
static usb_hid_transport_t s_tr;                   // set before start, read-only afterwards
static volatile bool s_pump_queued;                // a deferred pump call is in the TinyUSB queue
static TaskHandle_t s_tusb_task;                   // recorded in the TinyUSB-task callbacks

#define ST_LOCK()   portENTER_CRITICAL(&s_mux)
#define ST_UNLOCK() portEXIT_CRITICAL(&s_mux)

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
    uint8_t buf[8];
    uint8_t protocol = tud_hid_n_get_protocol(itf);
    size_t len = kvm_hid_build_neutral(itf, protocol, buf);
    bool ok = tud_hid_n_ready(itf) && tud_hid_n_report(itf, 0, buf, (uint16_t)len);
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

static void usb_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    s_tusb_task = xTaskGetCurrentTaskHandle();
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED: {
        ST_LOCK();
        s_st.state = KVM_USB_MOUNTED;
        s_st.mounts++;
        s_st.usb_epoch++;
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
            for (uint8_t i = 0; i < KVM_HID_ITF_COUNT; i++) {
                send_neutral(i);
            }
        }
        ESP_LOGI(TAG, "mounted (epoch %lu)", (unsigned long)epoch);
        break;
    }
    case TINYUSB_EVENT_DETACHED:
        // Logical state resets now; nothing queued for the old epoch is replayed (TECH-035).
        memset(s_neutral_pending, 0, sizeof(s_neutral_pending));
        ST_LOCK();
        s_st.state = KVM_USB_DETACHED;
        s_st.umounts++;
        s_st.led_state = 0;
        ST_UNLOCK();
        notify_state();
        ESP_LOGI(TAG, "detached");
        break;
    case TINYUSB_EVENT_SUSPENDED:
        ST_LOCK();
        s_st.state = KVM_USB_SUSPENDED;
        s_st.suspends++;
        s_st.remote_wakeup_enabled = event->suspended.remote_wakeup;
        ST_UNLOCK();
        notify_state();
        ESP_LOGI(TAG, "suspended (remote wakeup %s by HOST)", event->suspended.remote_wakeup ? "enabled" : "disabled");
        break;
    case TINYUSB_EVENT_RESUMED:
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
    case KVM_HID_ITF_CONSUMER: return s_report_consumer;
    default: return NULL;
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)report_id;
    ST_LOCK();
    s_st.get_report_requests++;
    uint8_t led = s_st.led_state;
    ST_UNLOCK();
    if (instance == KVM_HID_ITF_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT && reqlen >= 1) {
        buffer[0] = led;
        return 1;
    }
    if (report_type == HID_REPORT_TYPE_INPUT && instance < KVM_HID_ITF_COUNT) {
        // GET_REPORT(Input) returns the neutral state: this layer never holds a pressed state.
        uint8_t tmp[8];
        size_t len = kvm_hid_build_neutral(instance, tud_hid_n_get_protocol(instance), tmp);
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
    (void)report_id;
    if (instance != KVM_HID_ITF_KEYBOARD || bufsize < 1) {
        return;
    }
    if (report_type == HID_REPORT_TYPE_OUTPUT || report_type == HID_REPORT_TYPE_INVALID) {
        uint8_t led = buffer[0] & 0x1F;
        ST_LOCK();
        bool changed = led != s_st.led_state;
        s_st.led_state = led;
        s_st.led_reports++;
        ST_UNLOCK();
        if (changed) {
            // Lock-LED state only: authoritative domain data for the UI (AGENTS-UI #5), not a keystroke.
            ESP_LOGI(TAG, "LED output 0x%02X (num=%d caps=%d scroll=%d)", led, !!(led & KVM_HID_LED_NUM_LOCK),
                     !!(led & KVM_HID_LED_CAPS_LOCK), !!(led & KVM_HID_LED_SCROLL_LOCK));
        }
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

bool tud_hid_set_idle_cb(uint8_t instance, uint8_t idle_rate)
{
    (void)instance;
    (void)idle_rate;
    return true; // reports are sent on change only; idle rate accepted, not used
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    (void)report;
    (void)len;
    ST_LOCK();
    s_st.report_complete++;
    ST_UNLOCK();
    if (instance >= KVM_HID_ITF_COUNT) {
        return;
    }
    if (s_tr.on_complete != NULL) {
        s_tr.on_complete(s_tr.ctx, instance);
    } else if (s_neutral_pending[instance]) {
        send_neutral(instance);
    }
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type, uint8_t const *report,
                              uint16_t xferred_bytes)
{
    (void)report_type;
    (void)report;
    (void)xferred_bytes;
    ST_LOCK();
    s_st.report_failed++;
    ST_UNLOCK();
    if (instance < KVM_HID_ITF_COUNT && s_tr.on_complete != NULL) {
        s_tr.on_complete(s_tr.ctx, instance); // the endpoint is free again; the scheduler decides
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
    ST_UNLOCK();
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
    bool ok = itf < KVM_HID_ITF_COUNT && tud_mounted() && !tud_suspended() && tud_hid_n_ready(itf) &&
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
    if (itf >= KVM_HID_ITF_COUNT || s_tusb_task == NULL || xTaskGetCurrentTaskHandle() != s_tusb_task) {
        return KVM_HID_PROTOCOL_REPORT;
    }
    return tud_hid_n_get_protocol(itf);
}

// ---- Start -----------------------------------------------------------------------------------
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
    esp_err_t err = tinyusb_driver_install(&cfg);
    if (err != ESP_OK) {
        set_state(KVM_USB_ERROR);
        ESP_LOGE(TAG, "tinyusb_driver_install: %s", esp_err_to_name(err));
        return err;
    }
    ST_LOCK();
    if (s_st.state == KVM_USB_UNINITIALIZED) {
        s_st.state = KVM_USB_ENUMERATING;
    }
    ST_UNLOCK();
    ESP_LOGI(TAG, "TinyUSB started: VID 0x%04X PID 0x%04X (prototype IDs, TECH-037), 3 HID interfaces, serial %s",
             KVM_USB_VID, KVM_USB_PID, s_serial);
    return ESP_OK;
}
