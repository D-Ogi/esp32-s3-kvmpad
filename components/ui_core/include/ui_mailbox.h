// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Bounded UI mailbox (TASK-060B, 07 §3 "Handling"/"Coalescing"). Domain producers publish their
// slice of the state from their own task; the UI owner takes the newest merged snapshot once per
// tick. The state part is level-triggered: it holds the latest value of every field, so a slow UI
// never derives state from lost events and a late publish of an older domain sequence is refused.
// The event part is a small ring for transient feedback (action results); on overflow the oldest
// event is dropped and the next take reports a resync. Pure C, fixed size, no allocation.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ui_contracts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_MAILBOX_EVENT_CAP 16
#define UI_MAILBOX_SOURCES   4

/** Critical-section hooks (FreeRTOS spinlock in firmware, nothing or a mutex on the host). */
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void *ctx;
} ui_mailbox_lock_t;

typedef enum {
    UI_PUB_APPLIED = 0, // a field changed: version + 1
    UI_PUB_UNCHANGED,   // same content (coalesced, version kept)
    UI_PUB_STALE,       // domain sequence not newer than the last one of this source (late publish)
    UI_PUB_INVALID,     // enum out of range: refused, the previous value stays
} ui_pub_result_t;

typedef struct {
    uint32_t published;
    uint32_t applied;
    uint32_t unchanged;
    uint32_t stale;
    uint32_t invalid;
    uint32_t events_posted;
    uint32_t events_lost;    // dropped by ring overflow (the UI resyncs from the state)
    uint32_t takes;          // takes that returned a new version
    uint32_t event_high_water;
} ui_mailbox_counters_t;

typedef struct {
    ui_mailbox_lock_t lk;
    ui_state_snapshot_t state;
    uint32_t last_seq[UI_MAILBOX_SOURCES];
    ui_event_t ev[UI_MAILBOX_EVENT_CAP];
    uint16_t ev_head, ev_count;
    bool resync_pending;
    ui_mailbox_counters_t c;
} ui_mailbox_t;

/** `lk` may be NULL (single-threaded use). Every field starts UNKNOWN (sources = 0). */
void ui_mailbox_init(ui_mailbox_t *mb, const ui_mailbox_lock_t *lk);

/**
 * Publish one source's slice. `domain_seq` is the producer's own monotonic counter (wrap-safe);
 * a value that is not newer than the last accepted one of that source is refused as STALE.
 */
ui_pub_result_t ui_mailbox_publish_hid(ui_mailbox_t *mb, const ui_hid_slice_t *s, uint32_t domain_seq);
ui_pub_result_t ui_mailbox_publish_net(ui_mailbox_t *mb, const ui_net_slice_t *s, uint32_t domain_seq);
ui_pub_result_t ui_mailbox_publish_touch(ui_mailbox_t *mb, const ui_touch_slice_t *s, uint32_t domain_seq);
ui_pub_result_t ui_mailbox_publish_gesture(ui_mailbox_t *mb, const ui_gesture_slice_t *s, uint32_t domain_seq);

/** Transient event; never blocks. Returns false when an older event had to be dropped. */
bool ui_mailbox_post_event(ui_mailbox_t *mb, const ui_event_t *ev);

/**
 * UI owner only. Copies the merged snapshot when its version differs from `*seen_version` (or a
 * resync is pending) and updates `*seen_version`. `*resync` is set when events were lost since the
 * previous take: the caller must re-apply the whole snapshot. Returns true when `out` was written.
 */
bool ui_mailbox_take(ui_mailbox_t *mb, uint32_t *seen_version, ui_state_snapshot_t *out, bool *resync);

/** UI owner only: pop the oldest event. */
bool ui_mailbox_take_event(ui_mailbox_t *mb, ui_event_t *out);

void ui_mailbox_counters(ui_mailbox_t *mb, ui_mailbox_counters_t *out);

const char *ui_pub_result_name(ui_pub_result_t r);

#ifdef __cplusplus
}
#endif
