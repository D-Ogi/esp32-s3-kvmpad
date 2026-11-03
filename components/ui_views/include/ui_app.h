// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Product shell composition (07 §5), platform-free: one screen with the ui_core router, the UiTopBar (brand, mode
// switcher, HOST and CLIENT indicators), the primary views of ui_views and the local action sink (navigation,
// overlays). The firmware (main/ui_shell.c) and the desktop simulator build the very same composition; only the
// hooks differ. Nothing here sends HID: view buttons submit typed actions, a view change or a blocking overlay
// asks the platform to cancel touch and release HID (cancel_input), and the pad rectangle goes to the platform
// (pad_changed), which hands it to the touch bridge.
//
// The shell also owns the two prompts the local user decides on - pairing (FR-722) and control approval
// (FR-733). Their state is published by the domain adapter through ui_app_set_pairing() /
// ui_app_set_approval() and a tap on one of them comes back as a typed ui_remote_action_t; the shell adds no
// policy, no timer and no outcome of its own.
//
// Settings (FR-731..FR-736, FR-751) work the same way: the gear in the top bar submits UI_ACT_OPEN_SETTINGS,
// which the local sink turns into the blocking Settings overlay; the domain adapter publishes the record
// through ui_app_set_settings() and a tap comes back as one typed ui_setting_id_t change through
// hooks.settings_change. Nothing here writes a setting and no control moves until the domain republishes.
// LVGL owner context only; ui_theme_init() is called by ui_app_init().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_remote_view.h"
#include "ui_router.h"
#include "ui_segmented.h"
#include "ui_store.h"
#include "ui_view_keyboard.h"
#include "ui_view_mouse.h"
#include "ui_view_settings.h"
#include "ui_view_shortcuts.h"
#include "ui_view_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Control ids of the shell chrome (07 §3: UI ids, never keycodes).
#define UI_CTRL_STATUS_HOST 0x0001u
#define UI_CTRL_STATUS_CLIENT 0x0002u
#define UI_CTRL_NAV_BASE 0x0010u // + ui_view_id_t

typedef struct {
    uint64_t (*now_us)(void);
    /** Touch cancel + HID release request (router hook). False = refused (counted by the router). */
    bool (*cancel_input)(void *ctx, ui_cancel_reason_t why);
    /** The pad rectangle changed: `pad` NULL = the pad takes no touch now (no pad view, blocking overlay). */
    void (*pad_changed)(void *ctx, const lv_area_t *pad);
    /** FR-734 / ADR-023 policy, read on every Close window tap; NULL = no confirmation. */
    bool (*confirm_alt_f4)(void *ctx);
    /**
     * A local decision about a remote client (ADR-022 / ADR-023): the pairing entry point and the buttons of
     * the pairing and control-approval prompts. `session_id` is meaningful for APPROVE / DENY only. NULL =
     * the shell builds no pairing entry point and the prompt buttons report nothing.
     */
    void (*remote_action)(void *ctx, ui_remote_action_t act, uint64_t session_id);
    /**
     * One accepted Settings change (FR-731, SEC-107): the local user asked for `id` to become `value`;
     * `client_index` identifies the paired client for the two per-client ids and is 0 otherwise. The domain
     * validates, writes and republishes - until it does, the screen keeps showing the old value. NULL = every
     * Settings control is disabled instead of pretending to work.
     */
    void (*settings_change)(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index);
    void *ctx;
    uint16_t layout_chord; // FR-744: KVM_CHORD_LAYOUT_WIN / _CTRL for the HOST OS chosen in Settings
} ui_app_hooks_t;

typedef struct {
    uint32_t nav_requests;     // NAVIGATE submitted from the mode switcher
    uint32_t details_requests; // OPEN_HOST/CLIENT_DETAILS submitted from the indicators
    uint32_t local_refused;    // a local action the shell could not carry out (unknown overlay, refused switch)
    uint32_t remote_actions;   // local decisions about a remote client reported through hooks.remote_action
    uint32_t settings_changes; // Settings changes reported through hooks.settings_change
} ui_app_counters_t;

typedef struct {
    ui_store_t *store;
    ui_actions_t *actions;
    ui_app_hooks_t hooks;

    lv_obj_t *screen; // created by ui_app_init, never loaded by it
    lv_obj_t *bar;
    lv_obj_t *nav;
    lv_obj_t *settings_btn; // the gear in the top bar's trailing slot; submits UI_ACT_OPEN_SETTINGS
    ui_router_t router;
    ui_view_mouse_t mouse;
    ui_view_keyboard_t keyboard;
    ui_view_shortcuts_t shortcuts;
    ui_view_status_t status;    // HOST / CLIENT details overlays (FR-003)
    ui_remote_view_t remote;    // pairing and control-approval prompts (FR-722 / FR-733)
    ui_view_settings_t settings; // Settings card and its confirmation (FR-731..FR-736, FR-751)

    /**
     * Every overlay the router knows: the gesture card, the language picker, two status cards, two remote
     * prompts, Settings and its confirmation.
     */
    ui_overlay_descriptor_t overlays[UI_MOUSE_OVERLAYS + UI_KBD_OVERLAYS + UI_VIEW_STATUS_PANELS +
                                     UI_REMOTE_OVERLAYS + UI_SETTINGS_OVERLAYS];
    uint8_t n_overlays;

    ui_view_descriptor_t views[UI_VIEW_COUNT];
    uint8_t n_views;
    ui_segment_t segments[UI_VIEW_COUNT];
    ui_view_id_t segment_view[UI_VIEW_COUNT];
    ui_segmented_desc_t nav_desc;
    ui_segmented_sink_t nav_sink;
    bool ready;
    ui_app_counters_t c;
} ui_app_t;

/**
 * Build the shell on a new screen (not loaded) and install the local action sink on `actions`. The store must
 * already exist (its subjects are bound). Returns false on invalid arguments or when LVGL / the theme is not
 * available; nothing is kept then.
 */
bool ui_app_init(ui_app_t *app, ui_store_t *store, ui_actions_t *actions, const ui_app_hooks_t *hooks);

/** Remove the local sink, dismiss overlays, leave and destroy the views, delete the screen. */
void ui_app_deinit(ui_app_t *app);

/** First view (MOUSE) if none is active yet, sync the mode switcher, report the pad rectangle. */
void ui_app_show(ui_app_t *app);

/** Before another screen takes over: dismiss every overlay and let the active view drop its press state. */
void ui_app_leave(ui_app_t *app);

/** Report the current pad rectangle through hooks.pad_changed. */
void ui_app_refresh_pad(ui_app_t *app);

/**
 * Publish the pairing state of the domain (FR-722, FR-725): opens, updates or closes the pairing prompt and
 * toasts a new outcome. Call it from the LVGL owner context whenever the state changes; the UI adds no clock
 * and decides nothing, so a window stays up until this says it is closed.
 */
void ui_app_set_pairing(ui_app_t *app, const ui_pair_view_t *v);

/** Publish the control-approval state (FR-733, PROC-006 step 5): opens, updates or closes the prompt. */
void ui_app_set_approval(ui_app_t *app, const ui_approval_view_t *v);

/**
 * Publish the settings record (FR-731, FR-751): an open Settings card is re-rendered from it, a closed one
 * renders it on its next open. This is the only way a value on that screen changes - a tap never moves a
 * control by itself (AGENTS-UI MUST 5 and 6). Call it from the LVGL owner context.
 */
void ui_app_set_settings(ui_app_t *app, const ui_settings_view_t *v);

/**
 * Install (or with NULL remove) the pinyin decoder behind the KEYBOARD view's ZH layer (TASK-072). The
 * table is caller-owned and must outlive the app; removing it discards a composition in progress. Call it
 * from the LVGL owner context - the decoder is then only ever called from that context too.
 */
void ui_app_set_zh_engine(ui_app_t *app, const ui_kbd_conv_engine_t *engine);

/** The same for the romaji / kana-kanji decoder behind the JA layer (TASK-072, FR-746). */
void ui_app_set_ja_engine(ui_app_t *app, const ui_kbd_conv_engine_t *engine);

#ifdef __cplusplus
}
#endif
