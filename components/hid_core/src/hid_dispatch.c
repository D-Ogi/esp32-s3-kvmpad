// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware glue around the pure HID core. Threading:
// - producers: hid_dispatch_* from any task -> bounded FreeRTOS queue (static) + request_pump;
// - RELEASE_ALL: a bitmask under a spinlock, merged and never dropped;
// - the core: only in the TinyUSB task (pump / on_state / on_complete hooks), the single context
//   that changes the pressed-ledger (03 INV-001) and calls tud_hid_* (TECH-034);
// - a 10 ms esp_timer keeps the core timers running (lease, completion timeout, keep-alive).
// No allocation after start; logs carry reasons and counters only, never key usages (AGENTS.md §4).
#include "hid_dispatch.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#if KVM_HID_HAS_HPE
#include "esp_random.h"
#endif
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "usb_hid_device.h"

static const char *TAG = "hid";

#define PRODUCER_QUEUE_LEN 64
#define PUMP_BATCH         32
#define TICK_PERIOD_US     10000
#define REMOTE_SYNC_MS     100 // request / grant answer from the TinyUSB task

typedef enum {
    MSG_LOCAL = 0,
    MSG_UI_TAP,
    MSG_UI_DOWN,
    MSG_UI_UP,
    MSG_MAINTENANCE,
    MSG_KEEPALIVE,
    MSG_REMOTE,
    MSG_REMOTE_REQUEST,
    MSG_REMOTE_GRANT,
    MSG_REMOTE_HEARTBEAT,
    MSG_REMOTE_EPOCH_SYNC,
    MSG_BURST_START,  // steps in s_burst_slot (one slot: one burst request at a time)
    MSG_BURST_CANCEL, // value = kvm_release_reason_t
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    uint32_t input_generation;
    union {
        struct {
            uint8_t usage;
            bool flag;
            uint32_t value; // keep-alive interval, burst cancel reason or sync call number
        };
        uint64_t remote_sequence;
    };
    uint64_t session; // MSG_REMOTE_*
    kvm_hid_intent_t intent;
} msg_t;

static kvm_hid_core_t s_core; // TinyUSB task only after start
static StaticQueue_t s_q_struct;
static uint8_t s_q_storage[PRODUCER_QUEUE_LEN * sizeof(msg_t)];
static QueueHandle_t s_q;
static esp_timer_handle_t s_tick;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_input_generation; // invalidates producer events across release and USB epochs
static bool s_local_activity; // rejected local input still takes ownership, even with a full queue
static uint32_t s_release_bits; // pending RELEASE_ALL reasons (bit = kvm_release_reason_t)
// A full input queue cannot block a release. Only the current lease can fill this slot.
// Producers select it from the grant/revoke events; the USB owner rechecks it before acting.
typedef enum { REMOTE_NO_STOP, REMOTE_CLEAR, REMOTE_OVERFLOW, REMOTE_REVOKE } remote_stop_kind_t;
typedef struct {
    remote_stop_kind_t kind;
    uint64_t session, through;
    uint32_t epoch, grant_call;
    kvm_release_reason_t reason;
} remote_stop_t;
static remote_stop_t s_remote_stop;
static uint64_t s_remote_sequence; // assigned before publishing a remote input to the queue
static uint64_t s_remote_cutoff;   // USB owner only, reset at grant
static uint32_t s_epoch_waiter;
// A timed-out grant can still be queued when its connection closes.
static uint64_t s_grant_session;
static uint32_t s_grant_call;
static uint32_t s_applying_grant; // USB owner only
static bool s_grant_cancelled;
static kvm_release_reason_t s_grant_cancel_reason;
static kvm_hid_snapshot_t s_snap;
static kvm_hid_counters_t s_counters;
#if KVM_HID_HAS_HPE
static hpe_studio_status_t s_studio;
static uint8_t s_profile;
#endif
static uint32_t s_snap_seq;
static hid_dispatch_stats_t s_stats;

// FR-748: the burst steps travel outside the queue (a queue element stays small); the slot is
// filled by a producer and emptied by the TinyUSB task, under s_mux.
static struct {
    bool full;
    uint16_t n;
    uint8_t ncancel;
    kvm_hid_burst_step_t steps[KVM_HID_BURST_CAP];
    kvm_hid_burst_step_t cancel[KVM_HID_BURST_CANCEL_CAP];
} s_burst_slot;
static kvm_hid_burst_step_t s_burst_steps[KVM_HID_BURST_CAP]; // TinyUSB task copy
static kvm_hid_burst_step_t s_burst_cancel[KVM_HID_BURST_CANCEL_CAP];

// Synchronous REMOTE calls (request / grant): one caller at a time (s_sync_lock). The TinyUSB task
// writes the answer into static storage tagged with the call number, so an answer that arrives
// after the caller timed out is ignored instead of landing on a dead stack frame.
static StaticSemaphore_t s_sync_lock_mem;
static SemaphoreHandle_t s_sync_lock;
static StaticSemaphore_t s_sync_done_mem;
static SemaphoreHandle_t s_sync_done;
static uint32_t s_sync_call;     // caller side, under s_sync_lock
static uint32_t s_sync_answered; // TinyUSB task side, under s_mux
static kvm_hid_err_t s_sync_err;
static uint32_t s_sync_epoch;

// ---- core port (TinyUSB task) ------------------------------------------------------------------
static bool port_send(void *ctx, uint8_t itf, const uint8_t *report, uint8_t len)
{
    (void)ctx;
    return usb_hid_device_send(itf, report, len);
}

static uint8_t port_protocol(void *ctx, uint8_t itf)
{
    (void)ctx;
    return usb_hid_device_protocol(itf);
}

static void port_event(void *ctx, const kvm_hid_event_t *ev)
{
    (void)ctx;
    switch (ev->kind) {
    case KVM_HID_EV_RELEASE_ALL:
        s_stats.events_release_all++;
        ESP_LOGD(TAG, "RELEASE_ALL (%s)", kvm_release_reason_name((kvm_release_reason_t)ev->a));
        break;
    case KVM_HID_EV_CHORD_DELIVERED:
        s_stats.events_chord_delivered++;
        break;
    case KVM_HID_EV_CHORD_ABORTED:
        s_stats.events_chord_aborted++;
        break;
    case KVM_HID_EV_QUEUE_OVERFLOW:
        ESP_LOGW(TAG, "HID queue overflow: fail-safe RELEASE_ALL");
        break;
    case KVM_HID_EV_BURST_DONE:
        s_stats.events_burst_done++;
        break;
    case KVM_HID_EV_BURST_ABORTED:
        s_stats.events_burst_aborted++; // step count only: never which keys (no keystroke in logs)
        ESP_LOGD(TAG, "burst ended after %lu steps", (unsigned long)ev->a);
        break;
    case KVM_HID_EV_REMOTE_GRANTED:
    case KVM_HID_EV_REMOTE_REVOKED:
        portENTER_CRITICAL(&s_mux);
        s_remote_stop = (remote_stop_t){0};
        if (ev->kind == KVM_HID_EV_REMOTE_GRANTED) {
            s_remote_stop.session = ev->session_id;
            s_remote_stop.epoch = ev->epoch;
            s_remote_stop.grant_call = s_applying_grant;
            s_remote_cutoff = 0;
            if (s_applying_grant != s_grant_call || s_grant_cancelled) {
                s_remote_stop.kind = REMOTE_REVOKE;
                s_remote_stop.reason = s_applying_grant == s_grant_call ? s_grant_cancel_reason : KVM_RA_REMOTE_DISCONNECT;
            }
        }
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "remote %s, epoch %lu", ev->kind == KVM_HID_EV_REMOTE_GRANTED ? "granted" : "revoked",
                 (unsigned long)ev->epoch);
        break;
    default:
        break;
    }
}

static kvm_host_usb_t map_usb(kvm_usb_state_t s)
{
    switch (s) {
    case KVM_USB_DETACHED: return KVM_HOST_USB_DETACHED;
    case KVM_USB_ENUMERATING: return KVM_HOST_USB_ENUMERATING;
    case KVM_USB_MOUNTED: return KVM_HOST_USB_MOUNTED;
    case KVM_USB_SUSPENDED: return KVM_HOST_USB_SUSPENDED;
    case KVM_USB_ERROR: return KVM_HOST_USB_ERROR;
    default: return KVM_HOST_USB_UNINITIALIZED;
    }
}

typedef enum { SNAP_UPDATE, INPUT_INVALIDATED, USB_TRANSITION } publish_reason_t;

static void publish(publish_reason_t reason)
{
    kvm_hid_snapshot_t snap;
    kvm_hid_core_snapshot(&s_core, &snap);
    portENTER_CRITICAL(&s_mux);
    if (reason == USB_TRANSITION) s_local_activity = false;
    if (reason != SNAP_UPDATE) s_input_generation++; // publish admission and its generation together
    if (memcmp(&snap, &s_snap, sizeof(snap)) != 0) {
        s_snap = snap;
        s_snap_seq++;
    }
#if KVM_HID_HAS_HPE
    s_studio = s_core.studio.status;
#endif
    s_counters = *kvm_hid_core_counters(&s_core);
    portEXIT_CRITICAL(&s_mux);
}

static void answer_sync(uint32_t call, kvm_hid_err_t err, uint32_t epoch)
{
    portENTER_CRITICAL(&s_mux);
    bool current = call == s_sync_call;
    if (current) {
        s_sync_err = err;
        s_sync_epoch = epoch;
        s_sync_answered = call;
    }
    portEXIT_CRITICAL(&s_mux);
    if (current) xSemaphoreGive(s_sync_done);
}

static void service_priority_input(uint64_t now)
{
    portENTER_CRITICAL(&s_mux);
    bool local_activity = s_local_activity;
    s_local_activity = false;
    if (!local_activity && s_remote_stop.kind == REMOTE_NO_STOP && !s_epoch_waiter) {
        portEXIT_CRITICAL(&s_mux);
        return;
    }
    remote_stop_t stop = s_remote_stop;
    s_remote_stop.kind = REMOTE_NO_STOP;
    uint32_t waiter = s_epoch_waiter;
    s_epoch_waiter = 0;
    portEXIT_CRITICAL(&s_mux);
    if (local_activity) (void)kvm_hid_core_local_activity(&s_core, now);
    const kvm_control_owner_t *owner = kvm_hid_core_owner(&s_core);
    if (stop.kind != REMOTE_NO_STOP && owner->kind == OWNER_REMOTE &&
        owner->session_id == stop.session && owner->epoch == stop.epoch) {
        if (stop.kind == REMOTE_REVOKE) {
            kvm_hid_core_remote_revoke(&s_core, stop.session, stop.reason);
        } else if (stop.kind == REMOTE_OVERFLOW) {
            kvm_hid_core_release_all(&s_core, KVM_RA_QUEUE_OVERFLOW);
            s_remote_cutoff = stop.through;
        } else {
            const kvm_hid_intent_t release = {.type = KVM_HID_RELEASE_ALL, .origin = KVM_ORIGIN_REMOTE};
            if (kvm_hid_core_remote(&s_core, stop.session, stop.epoch, &release, now) == KVM_HID_OK)
                s_remote_cutoff = stop.through;
        }
    }
    if (waiter) answer_sync(waiter, KVM_HID_OK, owner->epoch);
}

static void apply(const msg_t *m, uint64_t now)
{
    portENTER_CRITICAL(&s_mux);
    bool stale = m->input_generation != s_input_generation;
    if (m->kind == MSG_REMOTE_GRANT && (m->value != s_grant_call || s_grant_cancelled)) stale = true;
    portEXIT_CRITICAL(&s_mux);
    if (stale && (m->kind == MSG_LOCAL || m->kind == MSG_REMOTE || m->kind == MSG_UI_TAP ||
                  m->kind == MSG_UI_DOWN || m->kind == MSG_UI_UP)) return;
    switch (m->kind) {
    case MSG_LOCAL:
        (void)kvm_hid_core_local(&s_core, &m->intent, now); // rejections are counted by the core
        break;
    case MSG_UI_TAP:
        (void)kvm_hid_core_ui_modifier_tap(&s_core, m->usage, now);
        break;
    case MSG_UI_DOWN:
        (void)kvm_hid_core_ui_key_down(&s_core, m->usage, now);
        break;
    case MSG_UI_UP:
        (void)kvm_hid_core_ui_key_up(&s_core, m->usage, now);
        break;
    case MSG_MAINTENANCE:
        kvm_hid_core_set_maintenance(&s_core, m->flag);
        break;
    case MSG_KEEPALIVE:
        kvm_hid_core_set_keepalive(&s_core, m->flag, m->value);
        break;
    case MSG_REMOTE:
        service_priority_input(now);
        if (m->remote_sequence <= s_remote_cutoff) break;
        (void)kvm_hid_core_remote(&s_core, m->session, m->intent.epoch, &m->intent, now);
        break;
    case MSG_REMOTE_REQUEST:
    case MSG_REMOTE_GRANT: {
        uint32_t epoch = 0;
        kvm_hid_err_t e = KVM_HID_OK;
        if (stale) {
            // Answer approvals queued before a USB/ownership transition without granting a new lease.
            e = KVM_HID_ERR_CONTROL_DENIED;
        } else if (m->kind == MSG_REMOTE_REQUEST) {
            e = kvm_hid_core_remote_request(&s_core, m->session, now);
        } else {
            s_applying_grant = m->value;
            e = kvm_hid_core_remote_grant(&s_core, m->session, now, &epoch);
            s_applying_grant = 0;
            service_priority_input(now);
            if (e == KVM_HID_OK && kvm_hid_core_owner(&s_core)->epoch != epoch) {
                e = KVM_HID_ERR_CONTROL_DENIED;
                epoch = 0;
            }
        }
        answer_sync(m->value, e, epoch);
        break;
    }
    case MSG_REMOTE_HEARTBEAT:
        kvm_hid_core_remote_heartbeat(&s_core, m->session, now);
        break;
    case MSG_REMOTE_EPOCH_SYNC: // answered outside the input queue by service_priority_input
        break;
    case MSG_BURST_START: {
        portENTER_CRITICAL(&s_mux);
        const bool full = s_burst_slot.full;
        const uint16_t n = s_burst_slot.n;
        const uint8_t nc = s_burst_slot.ncancel;
        if (full) {
            memcpy(s_burst_steps, s_burst_slot.steps, (size_t)n * sizeof(s_burst_steps[0]));
            memcpy(s_burst_cancel, s_burst_slot.cancel, (size_t)nc * sizeof(s_burst_cancel[0]));
            s_burst_slot.full = false;
        }
        portEXIT_CRITICAL(&s_mux);
        if (full && !stale) {
            (void)kvm_hid_core_burst_start(&s_core, s_burst_steps, n, s_burst_cancel, nc, now); // refusals counted
        }
        break;
    }
    case MSG_BURST_CANCEL:
        kvm_hid_core_burst_cancel(&s_core, (kvm_release_reason_t)m->value);
        break;
    }
}

// ---- transport hooks (TinyUSB task) ------------------------------------------------------------
static void pump(void *ctx)
{
    (void)ctx;
    const int64_t t0 = esp_timer_get_time();
    const uint64_t now = (uint64_t)t0;
    portENTER_CRITICAL(&s_mux);
#if KVM_HID_HAS_HPE
    s_core.now_us = now; // A pending release can emit a PTP report before the regular poll.
    s_core.active_profile = s_profile;
#endif
    uint32_t bits = s_release_bits;
    s_release_bits = 0;
    portEXIT_CRITICAL(&s_mux);
    for (int r = 0; r < KVM_RA_COUNT; r++) {
        if (bits & (1u << r)) {
            kvm_hid_core_release_all(&s_core, (kvm_release_reason_t)r);
        }
    }
    service_priority_input(now);
    msg_t m;
    for (int i = 0; i < PUMP_BATCH && xQueueReceive(s_q, &m, 0) == pdTRUE; i++) {
        apply(&m, now);
    }
    service_priority_input(now); // a close or release may have arrived during this batch
    kvm_hid_core_poll(&s_core, now);
    publish(SNAP_UPDATE);
    const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    s_stats.pumps++;
    if (dt > s_stats.max_pump_us) {
        s_stats.max_pump_us = dt;
    }
    if (uxQueueMessagesWaiting(s_q) != 0) {
        usb_hid_device_request_pump(); // bounded batch per run; the TinyUSB task keeps serving USB in between
    }
}

static void on_state(void *ctx, kvm_usb_state_t state, uint32_t usb_epoch)
{
    (void)ctx;
    const uint64_t now = (uint64_t)esp_timer_get_time();
#if KVM_HID_HAS_HPE
    uint32_t nonce = 0;
    if (state == KVM_USB_MOUNTED) {
        nonce = esp_random();
        if (!nonce) nonce = 1;
    }
    hpe_studio_start(&s_core.studio, nonce);
#endif
    if (state == KVM_USB_MOUNTED) {
        s_core.interface_count = usb_hid_device_interface_count();
#if CONFIG_KVM_USB_MONITOR_EVAL && KVM_HID_ITFS == 2
        s_core.display_controls = usb_hid_device_monitor_mounted();
#endif
    }
    kvm_hid_core_set_usb(&s_core, map_usb(state), usb_epoch, now);
    kvm_hid_core_poll(&s_core, now); // a mount sends the neutral start reports right away (TECH-035)
    publish(USB_TRANSITION);
}

static size_t get_input_report(void *ctx, uint8_t itf, uint8_t protocol, uint8_t report[8])
{
    (void)ctx;
    // The host may poll EP0 while the interrupt endpoint is still busy.
    // Relative movement belongs to its single interrupt transfer.
    switch (itf) {
    case KVM_HID_ITF_KEYBOARD:
        return kvm_hid_build_keyboard(report, s_core.modifiers, s_core.keys);
    case KVM_HID_ITF_MOUSE:
        return kvm_hid_build_mouse(report, protocol, s_core.buttons, 0, 0, 0, 0);
#if KVM_HID_ITF_COUNT == 3
    case KVM_HID_ITF_CONSUMER:
        return kvm_hid_build_consumer(report, s_core.consumer);
#endif
    default:
        return 0;
    }
}

static void on_complete(void *ctx, uint8_t itf)
{
    (void)ctx;
    kvm_hid_core_on_report_complete(&s_core, itf);
    uint64_t now = (uint64_t)esp_timer_get_time();
    service_priority_input(now);
    kvm_hid_core_poll(&s_core, now); // next phase / next report without waiting for a tick
    publish(SNAP_UPDATE);
}

static void repeat_keyboard(void *ctx)
{
    // Apply any queued release, owner change or key transition before a duplicate.
    pump(ctx);
    if (!uxQueueMessagesWaiting(s_q)) kvm_hid_core_repeat_keyboard(&s_core);
}

static void on_failed(void *ctx, uint8_t itf)
{
    (void)ctx;
#if KVM_HID_HAS_HPE
    s_core.now_us = (uint64_t)esp_timer_get_time();
#endif
    uint32_t releases = s_core.c.release_all;
    kvm_hid_core_on_report_failed(&s_core, itf);
    publish(releases != s_core.c.release_all ? INPUT_INVALIDATED : SNAP_UPDATE);
}

#if KVM_HID_HAS_HPE
void hid_dispatch_set_profile(uint8_t profile)
{
    if (profile > HPE_SETTINGS_PROFILE) return;
    portENTER_CRITICAL(&s_mux);
    const bool changed = profile != s_profile;
    if (changed) {
        s_profile = profile;
        s_input_generation++;
        s_release_bits |= 1u << KVM_RA_VIEW_CHANGE;
    }
    portEXIT_CRITICAL(&s_mux);
    if (changed) usb_hid_device_request_pump();
}

void hid_dispatch_studio_status(hpe_studio_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_studio;
    portEXIT_CRITICAL(&s_mux);
}

static uint8_t on_profile(void *ctx)
{
    (void)ctx;
    return s_core.active_profile;
}

static bool on_studio(void *ctx, const hpe_packet_t *packet)
{
    (void)ctx;
    return hpe_studio_receive(&s_core.studio, packet, (uint64_t)esp_timer_get_time());
}

static void on_ptp_route(void *ctx, bool native, bool surface_enabled, uint32_t generation)
{
    (void)ctx;
    s_core.now_us = (uint64_t)esp_timer_get_time(); // SET_FEATURE can arrive between scheduler ticks.
    portENTER_CRITICAL(&s_mux);
    s_input_generation++;
    portEXIT_CRITICAL(&s_mux);
    kvm_hid_core_ptp_route(&s_core, native, surface_enabled, generation);
    publish(SNAP_UPDATE);
}
#endif

static void tick_cb(void *arg)
{
    (void)arg;
    usb_hid_device_request_pump();
}

// ---- producer API (any task) -------------------------------------------------------------------
static void request_remote_stop(uint64_t session, uint32_t epoch, remote_stop_kind_t kind,
                                kvm_release_reason_t reason)
{
    portENTER_CRITICAL(&s_mux);
    if (kind == REMOTE_REVOKE && session && session == s_grant_session && !s_grant_cancelled) {
        s_grant_cancelled = true;
        s_grant_cancel_reason = reason;
    }
    if (session && session == s_remote_stop.session && (kind == REMOTE_REVOKE || epoch == s_remote_stop.epoch)) {
        if (kind > s_remote_stop.kind) {
            s_remote_stop.kind = kind;
            s_remote_stop.reason = reason;
        }
        s_remote_stop.through = s_remote_sequence;
        s_stats.release_requests++;
    }
    portEXIT_CRITICAL(&s_mux);
    usb_hid_device_request_pump();
}

static esp_err_t post(const msg_t *m)
{
    if (s_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    msg_t entry = *m;
    portENTER_CRITICAL(&s_mux);
    bool keyboard = m->kind == MSG_UI_TAP || m->kind == MSG_UI_DOWN || m->kind == MSG_UI_UP || m->kind == MSG_BURST_START;
    bool input = m->kind == MSG_LOCAL || m->kind == MSG_REMOTE;
    if ((keyboard && (s_snap.boot_unread & (1u << KVM_HID_ITF_KEYBOARD))) ||
        (input && kvm_hid_boot_input_unready(s_snap.boot_unread, m->intent.type))) {
        bool local = m->kind != MSG_REMOTE;
        if (local) s_local_activity = true;
        portEXIT_CRITICAL(&s_mux);
        if (local) usb_hid_device_request_pump();
        return ESP_ERR_INVALID_STATE;
    }
    entry.input_generation = s_input_generation;
    if (m->kind == MSG_REMOTE) entry.remote_sequence = ++s_remote_sequence;
    portEXIT_CRITICAL(&s_mux);
    if (xQueueSend(s_q, &entry, 0) != pdTRUE) {
        // TECH-005: never let a lost UP leave a key pressed; release everything instead.
        portENTER_CRITICAL(&s_mux);
        s_stats.producer_queue_full++;
        bool remote = m->kind == MSG_REMOTE || m->kind == MSG_REMOTE_HEARTBEAT ||
                      m->kind == MSG_REMOTE_REQUEST || m->kind == MSG_REMOTE_GRANT;
        if (!remote) {
            s_input_generation++;
            s_release_bits |= 1u << KVM_RA_QUEUE_OVERFLOW;
            if (keyboard || m->kind == MSG_LOCAL) s_local_activity = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (m->kind == MSG_REMOTE)
            request_remote_stop(m->session, m->intent.epoch, REMOTE_OVERFLOW, KVM_RA_QUEUE_OVERFLOW);
        else usb_hid_device_request_pump();
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_mux);
    s_stats.posted++;
    portEXIT_CRITICAL(&s_mux);
    usb_hid_device_request_pump();
    return ESP_OK;
}

// Return a synchronous refusal before publishing a command the USB profile cannot complete.
static bool supports_intent(const kvm_hid_intent_t *intent)
{
    if (KVM_HID_ITFS > 2) return true;
    if (intent->type == KVM_HID_CONSUMER_DOWN || intent->type == KVM_HID_CONSUMER_UP) return false;
    if (intent->type == KVM_HID_CHORD) {
        const kvm_chord_t *chord = kvm_chord_get(intent->data.chord.chord_id);
        if (chord && chord->kind == KVM_CHORD_KIND_CONSUMER) return false;
    }
    return true;
}

esp_err_t hid_dispatch_local(const kvm_hid_intent_t *intent)
{
    ESP_RETURN_ON_FALSE(intent != NULL, ESP_ERR_INVALID_ARG, TAG, "intent");
    if (!supports_intent(intent)) return ESP_ERR_NOT_SUPPORTED;
    if (intent->type == KVM_HID_RELEASE_ALL) {
        hid_dispatch_release_all(KVM_RA_REQUEST);
        return ESP_OK;
    }
    msg_t m = {.kind = MSG_LOCAL, .intent = *intent};
    m.intent.origin = KVM_ORIGIN_LOCAL;
    return post(&m);
}

esp_err_t hid_dispatch_ui_modifier_tap(uint8_t usage)
{
    msg_t m = {.kind = MSG_UI_TAP, .usage = usage};
    return post(&m);
}

esp_err_t hid_dispatch_ui_key_down(uint8_t usage)
{
    msg_t m = {.kind = MSG_UI_DOWN, .usage = usage};
    return post(&m);
}

esp_err_t hid_dispatch_ui_key_up(uint8_t usage)
{
    msg_t m = {.kind = MSG_UI_UP, .usage = usage};
    return post(&m);
}

esp_err_t hid_dispatch_burst_start(const kvm_hid_burst_step_t *steps, uint16_t n, const kvm_hid_burst_step_t *cancel,
                                   uint8_t ncancel)
{
    ESP_RETURN_ON_FALSE(steps != NULL && n > 0 && n <= KVM_HID_BURST_CAP && ncancel <= KVM_HID_BURST_CANCEL_CAP &&
                            (ncancel == 0 || cancel != NULL),
                        ESP_ERR_INVALID_ARG, TAG, "burst");
    bool taken = false;
    portENTER_CRITICAL(&s_mux);
    if (!s_burst_slot.full) {
        memcpy(s_burst_slot.steps, steps, (size_t)n * sizeof(steps[0]));
        if (ncancel > 0) {
            memcpy(s_burst_slot.cancel, cancel, (size_t)ncancel * sizeof(cancel[0]));
        }
        s_burst_slot.n = n;
        s_burst_slot.ncancel = ncancel;
        s_burst_slot.full = true;
        taken = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!taken) {
        return ESP_ERR_INVALID_STATE;
    }
    msg_t m = {.kind = MSG_BURST_START};
    esp_err_t err = post(&m);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        s_burst_slot.full = false; // the request never reaches the core (post() already released)
        portEXIT_CRITICAL(&s_mux);
    }
    return err;
}

esp_err_t hid_dispatch_burst_cancel(kvm_release_reason_t reason)
{
    if ((unsigned)reason >= KVM_RA_COUNT) {
        reason = KVM_RA_REQUEST;
    }
    msg_t m = {.kind = MSG_BURST_CANCEL, .value = (uint32_t)reason};
    esp_err_t err = post(&m);
    if (err != ESP_OK) {
        hid_dispatch_release_all(reason); // a lost cancel still ends the burst (any RELEASE_ALL does)
    }
    return err;
}

void hid_dispatch_release_all(kvm_release_reason_t reason)
{
    if ((unsigned)reason >= KVM_RA_COUNT) {
        reason = KVM_RA_REQUEST;
    }
    portENTER_CRITICAL(&s_mux);
    s_input_generation++;
    s_release_bits |= 1u << reason;
    s_stats.release_requests++;
    portEXIT_CRITICAL(&s_mux);
    usb_hid_device_request_pump();
}

esp_err_t hid_dispatch_set_maintenance(bool on)
{
    msg_t m = {.kind = MSG_MAINTENANCE, .flag = on};
    if (on) {
        hid_dispatch_release_all(KVM_RA_MAINTENANCE); // released even if the queue is congested
    }
    return post(&m);
}

esp_err_t hid_dispatch_set_keepalive(bool enabled, uint32_t interval_s)
{
    msg_t m = {.kind = MSG_KEEPALIVE, .flag = enabled, .value = interval_s};
    return post(&m);
}

// ---- REMOTE (03 §4.3): only the session task, after remote_protocol has validated the frame ------
esp_err_t hid_dispatch_remote(uint64_t session_id, uint32_t lease_epoch, const kvm_hid_intent_t *intent)
{
    ESP_RETURN_ON_FALSE(intent != NULL, ESP_ERR_INVALID_ARG, TAG, "intent");
    if (!supports_intent(intent)) return ESP_ERR_NOT_SUPPORTED;
    if (intent->type == KVM_HID_RELEASE_ALL) {
        request_remote_stop(session_id, lease_epoch, REMOTE_CLEAR, KVM_RA_REQUEST);
        return ESP_OK;
    }
    msg_t m = {.kind = MSG_REMOTE, .session = session_id, .intent = *intent};
    m.intent.origin = KVM_ORIGIN_REMOTE;
    m.intent.epoch = lease_epoch;
    return post(&m);
}

static kvm_hid_err_t remote_sync(msg_kind_t kind, uint64_t session_id, uint32_t *lease_epoch)
{
    // Fail closed: no answer in time means no control.
    if (s_sync_lock == NULL || xSemaphoreTake(s_sync_lock, pdMS_TO_TICKS(REMOTE_SYNC_MS)) != pdTRUE) {
        return KVM_HID_ERR_CONTROL_DENIED;
    }
    (void)xSemaphoreTake(s_sync_done, 0); // drop the late answer of an earlier, timed-out call
    portENTER_CRITICAL(&s_mux);
    if (++s_sync_call == 0) ++s_sync_call;
    const uint32_t call = s_sync_call;
    if (kind == MSG_REMOTE_GRANT) {
        s_grant_session = session_id;
        s_grant_call = call;
        s_grant_cancelled = false;
    }
    if (kind == MSG_REMOTE_EPOCH_SYNC) s_epoch_waiter = call;
    portEXIT_CRITICAL(&s_mux);
    msg_t m = {.kind = kind, .session = session_id, .value = call};
    kvm_hid_err_t e = KVM_HID_ERR_CONTROL_DENIED;
    bool answered = false;
    if (kind == MSG_REMOTE_EPOCH_SYNC) usb_hid_device_request_pump();
    if (kind == MSG_REMOTE_EPOCH_SYNC || post(&m) == ESP_OK) {
        const TickType_t t0 = xTaskGetTickCount();
        while (xSemaphoreTake(s_sync_done, pdMS_TO_TICKS(REMOTE_SYNC_MS)) == pdTRUE) {
            portENTER_CRITICAL(&s_mux);
            const bool mine = s_sync_answered == call;
            const kvm_hid_err_t err = s_sync_err;
            const uint32_t epoch = s_sync_epoch;
            portEXIT_CRITICAL(&s_mux);
            if (mine) {
                answered = true;
                e = err;
                if (lease_epoch != NULL) {
                    *lease_epoch = epoch;
                }
                break;
            }
            if (xTaskGetTickCount() - t0 > pdMS_TO_TICKS(REMOTE_SYNC_MS)) {
                break;
            }
        }
    }
    if (kind == MSG_REMOTE_GRANT && !answered) {
        // Timeout can precede grant application, or follow it before the caller receives the
        // acknowledgement. Only revoke a lease actually installed by this exact call.
        portENTER_CRITICAL(&s_mux);
        if (!s_grant_cancelled) {
            s_grant_cancelled = true;
            s_grant_cancel_reason = KVM_RA_REMOTE_DISCONNECT;
        }
        if (s_remote_stop.grant_call == call && s_remote_stop.session == session_id &&
            s_remote_stop.kind != REMOTE_REVOKE) {
            s_remote_stop.kind = REMOTE_REVOKE;
            s_remote_stop.reason = s_grant_cancel_reason;
        }
        portEXIT_CRITICAL(&s_mux);
        usb_hid_device_request_pump();
    }
    xSemaphoreGive(s_sync_lock);
    return e;
}

kvm_hid_err_t hid_dispatch_remote_request(uint64_t session_id)
{
    return remote_sync(MSG_REMOTE_REQUEST, session_id, NULL);
}

kvm_hid_err_t hid_dispatch_remote_grant(uint64_t session_id, uint32_t *lease_epoch)
{
    return remote_sync(MSG_REMOTE_GRANT, session_id, lease_epoch);
}

void hid_dispatch_remote_heartbeat(uint64_t session_id)
{
    msg_t m = {.kind = MSG_REMOTE_HEARTBEAT, .session = session_id};
    (void)post(&m); // a lost heartbeat only shortens the lease: the safe direction
}

void hid_dispatch_remote_release(uint64_t session_id)
{
    hid_dispatch_remote_disconnect(session_id, KVM_RA_REMOTE_RELEASE);
}

uint32_t hid_dispatch_remote_disconnect_sync(uint64_t session_id, kvm_release_reason_t reason)
{
    hid_dispatch_remote_disconnect(session_id, reason);
    uint32_t epoch = 0;
    if (remote_sync(MSG_REMOTE_EPOCH_SYNC, session_id, &epoch) != KVM_HID_OK) {
        kvm_hid_snapshot_t snap;
        hid_dispatch_get_snapshot(&snap, NULL);
        epoch = snap.owner_epoch;
    }
    return epoch;
}

void hid_dispatch_remote_disconnect(uint64_t session_id, kvm_release_reason_t reason)
{
    if ((unsigned)reason >= KVM_RA_COUNT) reason = KVM_RA_REMOTE_DISCONNECT;
    request_remote_stop(session_id, 0, REMOTE_REVOKE, reason);
}

void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *out, uint32_t *seq)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_snap;
    if (seq != NULL) {
        *seq = s_snap_seq;
    }
    portEXIT_CRITICAL(&s_mux);
}

void hid_dispatch_get_counters(kvm_hid_counters_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_counters;
    portEXIT_CRITICAL(&s_mux);
}

void hid_dispatch_get_stats(hid_dispatch_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t hid_dispatch_start(void)
{
    ESP_RETURN_ON_FALSE(s_q == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    const kvm_hid_port_t port = {.send = port_send, .protocol = port_protocol, .event = port_event, .ctx = NULL};
    kvm_hid_core_init(&s_core, &port, NULL);
    kvm_hid_core_snapshot(&s_core, &s_snap);
    s_q = xQueueCreateStatic(PRODUCER_QUEUE_LEN, sizeof(msg_t), s_q_storage, &s_q_struct);
    ESP_RETURN_ON_FALSE(s_q != NULL, ESP_ERR_NO_MEM, TAG, "queue");
    s_sync_lock = xSemaphoreCreateMutexStatic(&s_sync_lock_mem);
    s_sync_done = xSemaphoreCreateBinaryStatic(&s_sync_done_mem);
    ESP_RETURN_ON_FALSE(s_sync_lock != NULL && s_sync_done != NULL, ESP_ERR_NO_MEM, TAG, "sync");
    const usb_hid_transport_t tr = {.pump = pump, .on_state = on_state, .on_complete = on_complete,
#if KVM_HID_HAS_HPE
        .on_ptp_route = on_ptp_route, .on_studio_output = on_studio, .get_profile = on_profile,
#endif
        .get_input_report = get_input_report, .repeat_keyboard = repeat_keyboard,
        .on_failed = on_failed, .ctx = NULL};
    ESP_RETURN_ON_ERROR(usb_hid_device_set_transport(&tr), TAG, "transport");
    const esp_timer_create_args_t targs = {
        .callback = tick_cb, .dispatch_method = ESP_TIMER_TASK, .name = "hid_tick", .skip_unhandled_events = true};
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_tick), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_tick, TICK_PERIOD_US), TAG, "timer start");
    ESP_LOGI(TAG, "dispatch ready: core in the TinyUSB task, producer queue %d, tick %d ms", PRODUCER_QUEUE_LEN,
             TICK_PERIOD_US / 1000);
    return ESP_OK;
}
