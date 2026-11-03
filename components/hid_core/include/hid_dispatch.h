// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// hid_dispatch: the firmware owner of the HID core (03 INV-001). Producers (UI, gesture engine,
// remote session) post typed intents from their own tasks; the core runs only in the TinyUSB task
// (TECH-034) through the usb_hid_device transport hooks. RELEASE_ALL requests are a separate,
// never-dropped path (TECH-005).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the transport hooks and the timers. Call before usb_hid_device_start(). */
esp_err_t hid_dispatch_start(void);

/**
 * LOCAL intent from any task (not ISR). ESP_ERR_TIMEOUT when the producer queue is full: the
 * dispatcher then performs a fail-safe RELEASE_ALL (no lost UP can leave a key pressed).
 */
esp_err_t hid_dispatch_local(const kvm_hid_intent_t *intent);

/** Keyboard view input through the modifier FSM (ADR-016). */
esp_err_t hid_dispatch_ui_modifier_tap(uint8_t usage);
esp_err_t hid_dispatch_ui_key_down(uint8_t usage);
esp_err_t hid_dispatch_ui_key_up(uint8_t usage);

/**
 * ADR-024 FR-748 text burst (composed text, Unicode delivery sequence). One at a time: ESP_ERR_INVALID_STATE
 * while the previous request has not reached the core; the core then admits or refuses it like
 * kvm_hid_core_burst_start (refusals counted; progress in the snapshot `burst_active` and the counters).
 */
esp_err_t hid_dispatch_burst_start(const kvm_hid_burst_step_t *steps, uint16_t n, const kvm_hid_burst_step_t *cancel,
                                   uint8_t ncancel);
/** Cancel the running burst (release its keys, cancel key, RELEASE_ALL with `reason`). */
esp_err_t hid_dispatch_burst_cancel(kvm_release_reason_t reason);

/** Any task: RELEASE_ALL as soon as the TinyUSB task runs; requests are merged, never dropped. */
void hid_dispatch_release_all(kvm_release_reason_t reason);

/** OTA / factory reset maintenance (PROC-009): revoke, release, block input. */
esp_err_t hid_dispatch_set_maintenance(bool on);

/** ADR-020 keep-alive; default OFF. The interval is clamped to 10..300 s. */
esp_err_t hid_dispatch_set_keepalive(bool enabled, uint32_t interval_s);

/*
 * REMOTE path (03 §4.3), for the session task only, after remote_protocol has validated the frame
 * (auth, seq, lease epoch, ranges). The arbiter in the core checks owner and epoch again.
 */
/** Validated REMOTE intent; asynchronous like hid_dispatch_local (arbiter refusals are counted by the core). */
esp_err_t hid_dispatch_remote(uint64_t session_id, uint32_t lease_epoch, const kvm_hid_intent_t *intent);
/** Is a control request admissible now? Waits up to 100 ms; KVM_HID_ERR_CONTROL_DENIED on timeout. */
kvm_hid_err_t hid_dispatch_remote_request(uint64_t session_id);
/** After the physical approval: new epoch, release, owner = REMOTE; `*lease_epoch` receives the epoch. */
kvm_hid_err_t hid_dispatch_remote_grant(uint64_t session_id, uint32_t *lease_epoch);
void hid_dispatch_remote_heartbeat(uint64_t session_id);
/** The CLIENT gives control back (control.release). */
void hid_dispatch_remote_release(uint64_t session_id);
/** Session lost (disconnect, TLS error, revoke, timeout): RELEASE_ALL with `reason`, then the lease ends. */
void hid_dispatch_remote_disconnect(uint64_t session_id, kvm_release_reason_t reason);
/** Same, and wait (up to 100 ms) for the revoke; returns the lease epoch in force afterwards. */
uint32_t hid_dispatch_remote_disconnect_sync(uint64_t session_id, kvm_release_reason_t reason);

/** Authoritative projection for the UI and the status service (07 §3). `seq` changes on update. */
void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *out, uint32_t *seq);
void hid_dispatch_get_counters(kvm_hid_counters_t *out);

typedef struct {
    uint32_t posted;
    uint32_t producer_queue_full;
    uint32_t release_requests;
    uint32_t pumps;
    uint32_t max_pump_us;
    uint32_t events_release_all;
    uint32_t events_chord_delivered;
    uint32_t events_chord_aborted;
    uint32_t events_burst_done;
    uint32_t events_burst_aborted;
} hid_dispatch_stats_t;

void hid_dispatch_get_stats(hid_dispatch_stats_t *out);

#ifdef __cplusplus
}
#endif
