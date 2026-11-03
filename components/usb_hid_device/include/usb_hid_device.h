// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// usb_hid_device (TASK-050, USB part): TinyUSB composite of three HID interfaces —
// boot keyboard, boot mouse, Consumer Control (ADR-018). HID input reaches the HOST exclusively
// through InputArbiter -> HidScheduler (hid_core), driven by hid_dispatch through the transport
// hooks below. TECH-034: every hook runs in the TinyUSB task, the only caller of tud_hid_*.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_hid_reports.h"

#ifdef __cplusplus
extern "C" {
#endif

/** USB states (spec 01 §6.1). */
typedef enum {
    KVM_USB_UNINITIALIZED = 0,
    KVM_USB_DETACHED,     // not configured by a HOST (unplugged, or SET_CONFIGURATION(0))
    KVM_USB_ENUMERATING,  // stack running, waiting for SET_CONFIGURATION
    KVM_USB_MOUNTED,
    KVM_USB_SUSPENDED,
    KVM_USB_ERROR,
} kvm_usb_state_t;

typedef struct {
    kvm_usb_state_t state;
    uint32_t mounts;
    uint32_t umounts;
    uint32_t suspends;
    uint32_t resumes;
    uint32_t usb_epoch;                       // +1 on every mount; old work must not replay across it
    uint8_t protocol[KVM_HID_ITF_COUNT];      // KVM_HID_PROTOCOL_BOOT / _REPORT per interface
    uint32_t set_protocol_count;
    uint8_t led_state;                        // last LED output report (Num/Caps/Scroll/...)
    uint32_t led_reports;
    uint32_t neutral_sent;                    // fallback neutral reports (no transport hooks installed)
    uint32_t neutral_failed;                  // fallback neutral report not accepted (endpoint busy)
    uint32_t reports_sent;                    // reports accepted through usb_hid_device_send()
    uint32_t reports_refused;                 // endpoint not ready / not mounted
    uint32_t send_wrong_context;              // usb_hid_device_send() outside the TinyUSB task (refused)
    uint32_t pumps;                           // pump hook runs (TinyUSB task)
    uint32_t pump_requests;                   // deferred pump requests actually queued
    uint32_t report_complete;
    uint32_t report_failed;
    uint32_t get_report_requests;
    bool remote_wakeup_enabled;               // HOST-enabled flag at the last suspend
    uint64_t last_mount_us;
} usb_hid_status_t;

/**
 * Install TinyUSB on the native OTG port with the KVMpad descriptors. Call right after
 * board_hal_init() has confirmed EXIO5=LOW (TECH-030).
 */
esp_err_t usb_hid_device_start(void);

/**
 * Transport hooks for the HID scheduler. All of them are called in the TinyUSB task:
 * - `pump`: requested with usb_hid_device_request_pump(); runs the scheduler.
 * - `on_state`: USB state change (mount / umount / suspend / resume) with the current usb_epoch.
 * - `on_complete`: the HOST read the last report of `itf` (or the transfer failed).
 * Install before usb_hid_device_start(). With hooks installed this component no longer sends the
 * neutral start reports itself: the scheduler does (TECH-035).
 */
typedef struct {
    void (*pump)(void *ctx);
    void (*on_state)(void *ctx, kvm_usb_state_t state, uint32_t usb_epoch);
    void (*on_complete)(void *ctx, uint8_t itf);
    void *ctx;
} usb_hid_transport_t;

esp_err_t usb_hid_device_set_transport(const usb_hid_transport_t *t);

/** Any task (not ISR): run the pump hook in the TinyUSB task soon. Coalesces repeated requests. */
void usb_hid_device_request_pump(void);

/** TinyUSB task only (inside a hook): hand one input report to the IN endpoint of `itf`. */
bool usb_hid_device_send(uint8_t itf, const uint8_t *report, uint8_t len);

/** TinyUSB task only: current protocol of `itf` (KVM_HID_PROTOCOL_BOOT / _REPORT). */
uint8_t usb_hid_device_protocol(uint8_t itf);

void usb_hid_device_get_status(usb_hid_status_t *out);

const char *usb_hid_state_name(kvm_usb_state_t s);

#ifdef __cplusplus
}
#endif
