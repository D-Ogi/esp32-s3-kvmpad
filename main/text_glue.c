// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// text_glue: see text_glue.h. One esp_timer (20 ms) drives a small state machine; the queue is shared with the
// producers under a spinlock. No keystroke, character or code point is ever logged (AGENTS.md section 4).
#include "text_glue.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hid_dispatch.h"
#include "kbd_text.h"
#include "kvm_config.h"

static const char *TAG = "text";

#define QUEUE_LEN 64
#define TICK_US 20000
#define START_WAIT_US 200000 // the core answers a burst request within a few pumps (10 ms tick)

typedef enum { ST_IDLE = 0, ST_WAIT_START, ST_RUNNING } state_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static kvm_text_item_t s_q[QUEUE_LEN];
static uint16_t s_head, s_count;
static uint32_t s_cancel_gen; // bumped by text_glue_cancel(); the timer drops what it was doing
static uint8_t s_mode = KVM_UNICODE_MODE_HOST_IME;
static text_glue_stats_t s_stats;

// timer task only
static state_t s_state;
static uint32_t s_gen;
static uint32_t s_started_base, s_end_base, s_done_base;
static int64_t s_deadline;
static esp_timer_handle_t s_timer;

static kvm_uni_mode_t uni_mode(uint8_t m)
{
    switch (m) {
    case KVM_UNICODE_MODE_LINUX: return KVM_UNI_LINUX;
    case KVM_UNICODE_MODE_MACOS: return KVM_UNI_MACOS;
    case KVM_UNICODE_MODE_WINDOWS_HEX: return KVM_UNI_WINDOWS_HEX;
    case KVM_UNICODE_MODE_WINCOMPOSE: return KVM_UNI_WINCOMPOSE;
    default: return KVM_UNI_HOST_IME;
    }
}

static bool push(kvm_text_item_t it)
{
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    if (s_count < QUEUE_LEN) {
        s_q[(s_head + s_count) % QUEUE_LEN] = it;
        s_count++;
        s_stats.queued++;
        ok = true;
    } else {
        s_stats.dropped++;
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

bool text_glue_push_char(uint32_t cp)
{
    return push((kvm_text_item_t){.kind = KVM_TEXT_CHAR, .value = cp});
}

bool text_glue_push_key(uint16_t usage_and_shift)
{
    return push((kvm_text_item_t){.kind = KVM_TEXT_KEY, .value = usage_and_shift});
}

static void drop_all_locked(void)
{
    s_stats.dropped += s_count;
    s_head = 0;
    s_count = 0;
}

void text_glue_cancel(kvm_release_reason_t reason)
{
    portENTER_CRITICAL(&s_mux);
    drop_all_locked();
    s_cancel_gen++;
    s_stats.cancels++;
    portEXIT_CRITICAL(&s_mux);
    kvm_hid_snapshot_t sn;
    hid_dispatch_get_snapshot(&sn, NULL);
    if (sn.burst_active) {
        (void)hid_dispatch_burst_cancel(reason); // release its keys, its cancel key, then RELEASE_ALL
    } else {
        hid_dispatch_release_all(reason); // also clears one-shot modifiers armed before the composition
    }
}

void text_glue_set_mode(uint8_t unicode_mode)
{
    portENTER_CRITICAL(&s_mux);
    s_mode = unicode_mode < KVM_UNICODE_MODE_COUNT ? unicode_mode : KVM_UNICODE_MODE_HOST_IME;
    portEXIT_CRITICAL(&s_mux);
}

void text_glue_get_stats(text_glue_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}

static void pop_head(void)
{
    portENTER_CRITICAL(&s_mux);
    if (s_count > 0) {
        s_head = (uint16_t)((s_head + 1) % QUEUE_LEN);
        s_count--;
    }
    portEXIT_CRITICAL(&s_mux);
}

static void count(uint32_t *field)
{
    portENTER_CRITICAL(&s_mux);
    (*field)++;
    portEXIT_CRITICAL(&s_mux);
}

/** Start the head item. Returns true when a burst request went to the core. */
static bool start_head(const kvm_hid_counters_t *c)
{
    kvm_text_item_t it;
    uint8_t mode;
    portENTER_CRITICAL(&s_mux);
    bool have = s_count > 0;
    if (have) {
        it = s_q[s_head];
    }
    mode = s_mode;
    portEXIT_CRITICAL(&s_mux);
    if (!have) {
        return false;
    }
    static kvm_uni_step_t steps[KVM_UNI_MAX_STEPS]; // timer task only
    static kvm_hid_burst_step_t burst[KVM_HID_BURST_CAP];
    const kvm_uni_mode_t um = uni_mode(mode);
    int n = kvm_text_item_steps(&it, um, steps, KVM_UNI_MAX_STEPS);
    if (n <= 0 || n > KVM_HID_BURST_CAP) {
        pop_head();
        count(&s_stats.skipped); // FR-747: a character no mode can carry is counted, never half-sent
        return false;
    }
    kvm_uni_step_t cancel_u[KVM_HID_BURST_CANCEL_CAP];
    int nc = it.kind == KVM_TEXT_CHAR ? kvm_uni_cancel(um, cancel_u, KVM_HID_BURST_CANCEL_CAP) : 0;
    kvm_hid_burst_step_t cancel[KVM_HID_BURST_CANCEL_CAP];
    if (nc < 0) {
        nc = 0;
    }
    for (int i = 0; i < n; i++) {
        burst[i] = (kvm_hid_burst_step_t){.usage = steps[i].usage, .down = steps[i].down};
    }
    for (int i = 0; i < nc; i++) {
        cancel[i] = (kvm_hid_burst_step_t){.usage = cancel_u[i].usage, .down = cancel_u[i].down};
    }
    s_started_base = c->bursts_started;
    if (hid_dispatch_burst_start(burst, (uint16_t)n, cancel, (uint8_t)nc) != ESP_OK) {
        return false; // slot busy or producer queue full: retried on the next tick
    }
    s_deadline = esp_timer_get_time() + START_WAIT_US;
    return true;
}

static void tick(void *arg)
{
    (void)arg;
    kvm_hid_snapshot_t sn;
    hid_dispatch_get_snapshot(&sn, NULL);
    kvm_hid_counters_t c;
    hid_dispatch_get_counters(&c);
    const uint32_t ends = c.bursts_done + c.bursts_cancelled + c.bursts_aborted;

    portENTER_CRITICAL(&s_mux);
    const uint32_t gen = s_cancel_gen;
    if (sn.host_usb != KVM_HOST_USB_MOUNTED && s_count > 0) {
        drop_all_locked(); // no HOST: nothing waits for it (FR-748, never replayed after a remount)
    }
    portEXIT_CRITICAL(&s_mux);
    if (gen != s_gen) { // a cancel: whatever ran has been cancelled by text_glue_cancel()
        s_gen = gen;
        s_state = ST_IDLE;
    }

    switch (s_state) {
    case ST_IDLE:
        if (sn.host_usb == KVM_HOST_USB_MOUNTED && !sn.burst_active && start_head(&c)) {
            s_state = ST_WAIT_START;
            s_end_base = ends;
            s_done_base = c.bursts_done;
        }
        break;
    case ST_WAIT_START:
        if (c.bursts_started != s_started_base) {
            s_state = ST_RUNNING;
        } else if (esp_timer_get_time() > s_deadline) {
            // Refused by the core (a key still held, maintenance ...). Never retried: a late start of the same
            // request would then type the character twice.
            pop_head();
            count(&s_stats.refused);
            s_state = ST_IDLE;
            break;
        } else {
            break;
        }
        /* fall through */
    case ST_RUNNING:
        if (ends != s_end_base && !sn.burst_active) {
            if (c.bursts_done != s_done_base) {
                pop_head();
                count(&s_stats.sent);
            } else {
                portENTER_CRITICAL(&s_mux);
                drop_all_locked(); // ended by a release: the rest of the text is not sent
                portEXIT_CRITICAL(&s_mux);
            }
            s_state = ST_IDLE;
        }
        break;
    }
}

esp_err_t text_glue_start(void)
{
    ESP_RETURN_ON_FALSE(s_timer == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    const esp_timer_create_args_t a = {
        .callback = tick, .dispatch_method = ESP_TIMER_TASK, .name = "text_glue", .skip_unhandled_events = true};
    ESP_RETURN_ON_ERROR(esp_timer_create(&a, &s_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, TICK_US), TAG, "timer start");
    ESP_LOGI(TAG, "text queue ready (%d items)", QUEUE_LEN);
    return ESP_OK;
}
