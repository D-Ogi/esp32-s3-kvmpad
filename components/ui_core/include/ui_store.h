// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI store (TASK-060B, 07 §3-4): the presentation projection of the domain snapshot as native LVGL
// 9.6 Subjects. Views bind to the subjects through objects (lv_obj_bind_* / lv_subject_add_observer_obj),
// so an observer dies with its widget. The store is not a source of truth: every domain subject holds
// UI_STORE_UNKNOWN until its producer has published. Called only from the LVGL owner context
// (ui_platform task or under ui_platform_lock()).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_contracts.h"
#include "ui_mailbox.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_STORE_UNKNOWN (-1)

typedef enum {
    // domain projection (UI_STORE_UNKNOWN until the source publishes)
    UI_SUBJ_HOST_USB = 0, // kvm_host_usb_t
    UI_SUBJ_CLIENT,       // ui_client_state_t
    UI_SUBJ_OWNER_KIND,   // kvm_owner_kind_t
    UI_SUBJ_OWNER_EPOCH,  // low 31 bits of the epoch (change detection / diagnostics)
    UI_SUBJ_WIFI,         // 0 / 1
    UI_SUBJ_TOUCH_OK,     // 0 / 1
    UI_SUBJ_DRAG_LOCK,    // 0 / 1
    UI_SUBJ_MODIFIERS,    // ledger modifier bitmap
    UI_SUBJ_MODS_ARMED,   // ADR-016 one-shot modifier bits (FR-204 ARMED)
    UI_SUBJ_MODS_LOCKED,  // ADR-016 locked modifier bits (FR-204 LOCKED)
    UI_SUBJ_CAPS_LED,     // 0 / 1; UNKNOWN also while the HOST has sent no LED report
    UI_SUBJ_MAINTENANCE,  // 0 / 1
    UI_SUBJ_ERROR,        // 0 = none
    UI_SUBJ_DOMAIN_COUNT,
    // local UI state (not security relevant, 07 §2)
    UI_SUBJ_ACTIVE_VIEW = UI_SUBJ_DOMAIN_COUNT, // ui_view_id_t, written by the router (TASK-060C)
    UI_SUBJ_OVERLAY,                            // 0 = none, otherwise an overlay id
    UI_SUBJ_COUNT,
} ui_subject_id_t;

typedef struct {
    uint32_t ticks;
    uint32_t applied;   // snapshots applied
    uint32_t resyncs;   // full re-apply after lost events
    uint32_t events;    // events delivered to the handler
} ui_store_counters_t;

typedef void (*ui_store_event_fn)(void *ctx, const ui_event_t *ev);

typedef struct {
    lv_subject_t *subj[UI_SUBJ_COUNT]; // lv_subject_create() (LVGL 9.6; the init API is deprecated)
    ui_mailbox_t *mb;
    uint32_t seen_version;
    ui_state_snapshot_t applied; // plain copy of the last applied snapshot (diagnostics)
    ui_store_event_fn on_event;
    void *event_ctx;
    ui_store_counters_t c;
    bool ready;
} ui_store_t;

/**
 * Create every subject (domain ones UNKNOWN, view = MOUSE, overlay = 0). `mb` may be NULL.
 * Returns false (and leaves nothing allocated) when LVGL is out of memory.
 */
bool ui_store_init(ui_store_t *s, ui_mailbox_t *mb);

/** Delete the subjects: every observer still attached is removed first (07 §4, AT-UIFW-09). */
void ui_store_deinit(ui_store_t *s);

/** Transient events (action results, release notices) are handed to `fn` during ui_store_tick(). */
void ui_store_set_event_handler(ui_store_t *s, ui_store_event_fn fn, void *ctx);

/** One UI tick: take the newest snapshot (if any) and apply it, then drain the events. */
bool ui_store_tick(ui_store_t *s);

/** Apply a snapshot to the subjects (only changed values notify observers). */
void ui_store_apply(ui_store_t *s, const ui_state_snapshot_t *snap);

lv_subject_t *ui_store_subject(ui_store_t *s, ui_subject_id_t id);
int32_t ui_store_get(ui_store_t *s, ui_subject_id_t id);

/** Local UI state written by the router/overlay (UI context only). */
void ui_store_set_active_view(ui_store_t *s, ui_view_id_t v);
void ui_store_set_overlay(ui_store_t *s, uint16_t overlay_id);

const char *ui_subject_name(ui_subject_id_t id);

#ifdef __cplusplus
}
#endif
