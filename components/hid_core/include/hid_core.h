// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// HID core (TASK-050 core part): InputArbiter (owner + epoch + lease, 03 §4), pressed-ledger
// (02 §7.2), bounded intent queue with move coalescing (TECH-005), HidScheduler (02 §7.3 chord
// phases, dx/dy split, 03 §5) and the keyboard modifier FSM (ADR-016).
//
// Pure C, no RTOS, no allocation, monotonic time injected by the caller (TECH-006). Exactly one
// context (hid_dispatch) owns a kvm_hid_core_t and calls every function below (03 INV-001); reports
// leave only through kvm_hid_port_t.send.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hid_keymap.h"
#include "kbd_fsm.h"
#include "kvm_hid_intent.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Ownership (03 §4.1, names as in the contract) -----------------------------------------
typedef enum {
    OWNER_NONE = 0,
    OWNER_LOCAL,
    OWNER_REMOTE,
} kvm_owner_kind_t;

typedef struct {
    kvm_owner_kind_t kind;
    uint64_t session_id;       // 0 when local/none
    uint32_t epoch;            // +1 on every ownership change
    uint64_t lease_expires_us; // only REMOTE; monotonic clock
} kvm_control_owner_t;

/** HOST USB state as seen by the core (same order as 01 §6.1 / usb_hid_device kvm_usb_state_t). */
typedef enum {
    KVM_HOST_USB_UNINITIALIZED = 0,
    KVM_HOST_USB_DETACHED,
    KVM_HOST_USB_ENUMERATING,
    KVM_HOST_USB_MOUNTED,
    KVM_HOST_USB_SUSPENDED,
    KVM_HOST_USB_ERROR,
} kvm_host_usb_t;

/** Result codes (03 §6.5, §7). */
typedef enum {
    KVM_HID_OK = 0,
    KVM_HID_ERR_HOST_OFFLINE,
    KVM_HID_ERR_USB_SUSPENDED,
    KVM_HID_ERR_STALE_EPOCH,
    KVM_HID_ERR_SESSION,
    KVM_HID_ERR_CLIENT_STALE,
    KVM_HID_ERR_CONTROL_DENIED,
    KVM_HID_ERR_USAGE,
    KVM_HID_ERR_RANGE,
    KVM_HID_ERR_HID_QUEUE_OVERFLOW,
    KVM_HID_ERR_OTA_IN_PROGRESS,
    KVM_HID_ERR_BAD_INTENT,
    KVM_HID_ERR_BUSY, // ADR-024 FR-748: a text burst is in flight; keyboard input waits for it or cancels it
} kvm_hid_err_t;

typedef enum {
    KVM_RA_REQUEST = 0,        // explicit RELEASE_ALL intent / UI Release button
    KVM_RA_OWNER_CHANGE,
    KVM_RA_LOCAL_OVERRIDE,
    KVM_RA_LEASE_TIMEOUT,
    KVM_RA_REMOTE_RELEASE,
    KVM_RA_REMOTE_DISCONNECT,
    KVM_RA_USB_LOSS,
    KVM_RA_USB_SUSPEND,
    KVM_RA_MAINTENANCE,
    KVM_RA_QUEUE_OVERFLOW,
    KVM_RA_COMPLETION_TIMEOUT,
    KVM_RA_GESTURE_CANCEL,
    KVM_RA_VIEW_CHANGE,
    KVM_RA_COUNT,
} kvm_release_reason_t;

typedef enum {
    KVM_HID_EV_RELEASE_ALL = 0,  // a = reason
    KVM_HID_EV_REMOTE_GRANTED,   // session, epoch
    KVM_HID_EV_REMOTE_REVOKED,   // session, epoch (new), a = reason
    KVM_HID_EV_CHORD_DELIVERED,  // a = chord id: every phase handed to and read from the USB stack (not "executed by the HOST")
    KVM_HID_EV_CHORD_ABORTED,    // a = chord id
    KVM_HID_EV_QUEUE_OVERFLOW,
    KVM_HID_EV_BURST_DONE,       // a = steps sent: every step of the burst handed to and read from the USB stack
    KVM_HID_EV_BURST_ABORTED,    // a = steps sent before the abort (cancelled or released)
} kvm_hid_event_kind_t;

typedef struct {
    kvm_hid_event_kind_t kind;
    uint32_t a;
    uint32_t epoch;
    uint64_t session_id;
} kvm_hid_event_t;

/**
 * Transport to the USB HID device. `send` must return true only when the report was accepted by the
 * IN endpoint; kvm_hid_core_on_report_complete() is then expected when the HOST has read it.
 */
typedef struct {
    bool (*send)(void *ctx, uint8_t itf, const uint8_t *report, uint8_t len);
    uint8_t (*protocol)(void *ctx, uint8_t itf); // KVM_HID_PROTOCOL_BOOT / _REPORT (usb_hid_reports.h)
    void (*event)(void *ctx, const kvm_hid_event_t *ev); // optional
    void *ctx;
} kvm_hid_port_t;

typedef struct {
    uint64_t lease_us;          // REMOTE lease from the last heartbeat (03 §4.3: 4 s)
    uint64_t stale_us;          // no sending after this long without heartbeat (03 §4.3: 3 s)
    uint64_t local_idle_us;     // LOCAL counts as idle for a control request after this quiet time
    uint64_t completion_timeout_us; // report not read by the HOST in time -> fail-safe release (02 §7.3)
    int32_t move_accum_limit;   // |sum| cap of one coalesced move entry (>= 32768: one int16 intent is never clipped); the excess is dropped + counted
} kvm_hid_config_t;

#define KVM_HID_CONFIG_DEFAULT                                                                              \
    {                                                                                                       \
        .lease_us = 4000000u, .stale_us = 3000000u, .local_idle_us = 2000000u,                              \
        .completion_timeout_us = 250000u, .move_accum_limit = 32768,                                        \
    }

#define KVM_HID_QUEUE_CAP 128 // 02 §11.2: explicit cap of 128 critical events + coalescing mouse move
#define KVM_HID_ITFS 3

typedef struct {
    uint32_t accepted;
    uint32_t rejected_offline;
    uint32_t rejected_suspended;
    uint32_t rejected_epoch;
    uint32_t rejected_session;
    uint32_t rejected_stale;
    uint32_t rejected_usage;
    uint32_t rejected_maintenance;
    uint32_t rejected_origin;
    uint32_t queue_overflows;
    uint32_t moves_dropped;      // motion that could not be coalesced or exceeded the cap
    uint32_t stale_dropped;      // queued entries of an old epoch discarded at dequeue
    uint32_t release_all;
    uint32_t release_by_reason[KVM_RA_COUNT];
    uint32_t reports_sent;
    uint32_t send_refused;       // endpoint busy: report kept and retried
    uint32_t completion_timeouts;
    uint32_t completion_timeouts_itf[KVM_HID_ITFS]; // per interface: keyboard, mouse, consumer
    uint32_t keyup_without_down; // 03 §3 rule 5 metric
    uint32_t rollover_rejected;  // 7th simultaneous key (AT-HID-03)
    uint32_t wheel_dropped_boot; // wheel/pan while the mouse is in boot protocol (03 §3 rule 4 fallback)
    uint32_t chords_delivered;
    uint32_t chords_aborted;
    uint32_t keepalive_sent;
    uint32_t keepalive_rejected;
    uint32_t remote_grants;
    uint32_t remote_revokes;
    uint32_t queue_high_water;
    uint32_t bursts_started;   // ADR-024 FR-748
    uint32_t bursts_done;
    uint32_t bursts_cancelled; // ended through kvm_hid_core_burst_cancel (release keys, cancel key, RELEASE_ALL)
    uint32_t bursts_aborted;   // ended by any other RELEASE_ALL (USB loss, owner change, timeout, ...)
} kvm_hid_counters_t;

/** Read-only projection for the UI / status (07 §3): authoritative, never a UI guess. */
typedef struct {
    kvm_host_usb_t host_usb;
    kvm_owner_kind_t owner_kind;
    uint32_t owner_epoch;
    bool maintenance;
    uint8_t modifiers_down; // ledger modifier bitmap (what the HOST was told)
    uint8_t mods_armed;     // ADR-016 one-shot modifiers
    uint8_t mods_locked;    // ADR-016 locked modifiers
    uint8_t keys_down;      // number of ordinary keys in the ledger
    uint8_t buttons_down;   // mouse button mask
    uint16_t consumer_down; // Consumer usage held (0 = none)
    bool chord_active;
    bool keepalive_enabled;
    bool ledger_empty;
    bool burst_active;      // FR-748 text burst in flight (sending or cancelling)
} kvm_hid_snapshot_t;

// ---- Text burst (ADR-024 FR-748) -------------------------------------------------------------
/** One step of a burst: a page 0x07 usage pressed or released (modifiers as 0xE0..0xE7). */
typedef struct {
    uint8_t usage;
    bool down;
} kvm_hid_burst_step_t;

#define KVM_HID_BURST_CAP 160          // one Unicode sequence (kbd_unicode.h KVM_UNI_MAX_STEPS) or a short run of keys
#define KVM_HID_BURST_CANCEL_CAP 4     // the delivery mode's cancel key (e.g. Esc DOWN/UP)
#define KVM_HID_BURST_STEP_US 10000u   // >= 10 ms per step: <= 100 key events/s, inside 03 §6.5 (120 msg/s), ADR-019

// ---- Internal state (exposed for static allocation only; use the API) ----------------------
typedef struct {
    uint8_t state;      // 0 idle, 1 sending, 2 cancelling
    uint16_t n, pos;
    uint32_t epoch;     // owner epoch at start; any change ends the burst
    uint64_t next_us;
    kvm_release_reason_t cancel_reason;
    uint8_t ncancel;
    kvm_hid_burst_step_t steps[KVM_HID_BURST_CAP];
    kvm_hid_burst_step_t cancel[KVM_HID_BURST_CANCEL_CAP];
} kvm_hid_burst_t;

typedef struct {
    uint8_t kind;
    uint8_t a;      // usage / button / modifier mask
    uint16_t b;     // chord id / consumer usage
    uint32_t epoch;
    int32_t x, y;   // move dx/dy or wheel horizontal/vertical, accumulated
} kvm_hid_qentry_t;

typedef struct {
    bool pending;   // built, not yet accepted by the endpoint
    uint8_t len;
    uint8_t buf[8];
} kvm_hid_staged_t;

typedef struct {
    kvm_hid_port_t port;
    kvm_hid_config_t cfg;
    kvm_control_owner_t owner;
    uint64_t last_heartbeat_us;
    kvm_host_usb_t usb;
    uint32_t usb_epoch;
    bool maintenance;

    // ledger (02 §7.2)
    uint8_t modifiers;
    uint8_t keys[6];      // press order, 0 = empty slot
    uint8_t buttons;
    uint16_t consumer;

    kvm_kbd_fsm_t kbd;

    // queue
    kvm_hid_qentry_t q[KVM_HID_QUEUE_CAP];
    uint16_t q_head, q_count;

    // scheduler per interface
    bool in_flight[KVM_HID_ITFS];
    uint64_t in_flight_since[KVM_HID_ITFS];
    bool neutral_pending[KVM_HID_ITFS];
    kvm_hid_staged_t staged[KVM_HID_ITFS];

    // chord in progress
    bool chord_active;
    uint16_t chord_id;
    uint8_t chord_phase;

    // keep-alive (ADR-020)
    bool keepalive_enabled;
    uint64_t keepalive_interval_us;
    uint64_t last_real_input_us;
    uint32_t keepalive_epoch;

    kvm_hid_burst_t burst;

    uint64_t last_local_input_us;
    uint64_t now_us;
    kvm_hid_counters_t c;
} kvm_hid_core_t;

void kvm_hid_core_init(kvm_hid_core_t *core, const kvm_hid_port_t *port, const kvm_hid_config_t *cfg);

/** USB state from usb_hid_device. A new `usb_epoch` with MOUNTED means a fresh mount (TECH-035). */
void kvm_hid_core_set_usb(kvm_hid_core_t *core, kvm_host_usb_t state, uint32_t usb_epoch, uint64_t now_us);

/** The HOST read (or the stack failed) the last report on `itf`. */
void kvm_hid_core_on_report_complete(kvm_hid_core_t *core, uint8_t itf);

/** Run the scheduler and the timers (lease expiry, completion timeout, keep-alive). Call often. */
void kvm_hid_core_poll(kvm_hid_core_t *core, uint64_t now_us);

/** 03 §4.3 on_local_input: LOCAL input with override. `intent->origin/epoch` are set by the core. */
kvm_hid_err_t kvm_hid_core_local(kvm_hid_core_t *core, const kvm_hid_intent_t *intent, uint64_t now_us);

/** High-priority RELEASE_ALL (TECH-005): logical state empty now, neutral reports as soon as possible. */
void kvm_hid_core_release_all(kvm_hid_core_t *core, kvm_release_reason_t reason);

// ---- Keyboard FSM inputs from the UI (ADR-016); LOCAL input, same override rules -------------
kvm_hid_err_t kvm_hid_core_ui_modifier_tap(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us);
kvm_hid_err_t kvm_hid_core_ui_key_down(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us);
kvm_hid_err_t kvm_hid_core_ui_key_up(kvm_hid_core_t *core, uint8_t usage, uint64_t now_us);

// ---- Text burst (ADR-024 FR-748): LOCAL, one at a time, paced, cancellable ---------------------
/**
 * Start a burst of key steps (a Unicode delivery sequence or plain keystrokes of composed text).
 * Admitted like LOCAL input. Refused with KVM_HID_ERR_BUSY while another burst runs or while any
 * keyboard state is held or queued; KVM_HID_ERR_BAD_INTENT unless `steps` is balanced (every DOWN of
 * a key not held, every UP of a held key, nothing held at the end) and within the caps;
 * KVM_HID_ERR_USAGE for an invalid usage. `cancel` (may be empty) is the mode's cancel key, sent only
 * by kvm_hid_core_burst_cancel. While the burst runs, other LOCAL keyboard input is refused with
 * KVM_HID_ERR_BUSY (pointer input is not affected); every RELEASE_ALL ends it at once.
 */
kvm_hid_err_t kvm_hid_core_burst_start(kvm_hid_core_t *core, const kvm_hid_burst_step_t *steps, uint16_t n,
                                       const kvm_hid_burst_step_t *cancel, uint8_t ncancel, uint64_t now_us);
/**
 * Cancel the burst: release the keys it holds, send its cancel key, then RELEASE_ALL(`reason`).
 * When the HOST cannot be written to (not mounted, maintenance, not LOCAL) it is RELEASE_ALL at once.
 * No-op when no burst runs.
 */
void kvm_hid_core_burst_cancel(kvm_hid_core_t *core, kvm_release_reason_t reason);
bool kvm_hid_core_burst_active(const kvm_hid_core_t *core);

// ---- REMOTE (03 §4.3); authentication, sequence and payload checks belong to remote_protocol ----
/** Is a control request admissible now (LOCAL idle, no other lease, no maintenance)? */
kvm_hid_err_t kvm_hid_core_remote_request(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us);
/** After physical approval: new epoch, release, owner = REMOTE. `*lease_epoch` receives the epoch. */
kvm_hid_err_t kvm_hid_core_remote_grant(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us,
                                        uint32_t *lease_epoch);
void kvm_hid_core_remote_heartbeat(kvm_hid_core_t *core, uint64_t session_id, uint64_t now_us);
kvm_hid_err_t kvm_hid_core_remote(kvm_hid_core_t *core, uint64_t session_id, uint32_t lease_epoch,
                                  const kvm_hid_intent_t *intent, uint64_t now_us);
void kvm_hid_core_remote_release(kvm_hid_core_t *core, uint64_t session_id);
void kvm_hid_core_remote_disconnect(kvm_hid_core_t *core, uint64_t session_id);

// ---- Maintenance (OTA, factory reset) and keep-alive -----------------------------------------
void kvm_hid_core_set_maintenance(kvm_hid_core_t *core, bool on);
/** ADR-020: interval clamped to 10..300 s. */
void kvm_hid_core_set_keepalive(kvm_hid_core_t *core, bool enabled, uint32_t interval_s);

void kvm_hid_core_snapshot(const kvm_hid_core_t *core, kvm_hid_snapshot_t *out);
const kvm_hid_counters_t *kvm_hid_core_counters(const kvm_hid_core_t *core);
const kvm_control_owner_t *kvm_hid_core_owner(const kvm_hid_core_t *core);

const char *kvm_hid_err_name(kvm_hid_err_t e);
const char *kvm_release_reason_name(kvm_release_reason_t r);

#ifdef __cplusplus
}
#endif
