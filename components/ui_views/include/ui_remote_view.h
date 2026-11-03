// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pairing and control-approval prompts of the DEVICE (TASK-100 UI part; FR-722, FR-725, FR-733, SEC-104,
// ADR-022, ADR-023, ADR-029): the two blocking cards the local user decides on, plus the toast that reports
// the outcome of a pairing attempt.
//
// The UI decides nothing here. It renders `ui_pair_view_t` / `ui_approval_view_t` exactly as the domain
// adapter publishes them and reports a tap as one typed `ui_remote_action_t`; the window, the code, the
// countdowns, the lockout and every outcome stay in `secure_remote`. In particular a window the user sees
// expiring disappears only when the adapter says it is closed (AGENTS-UI MUST 5 and 6): no timer in this
// view closes a prompt, grants control or claims a client is paired.
//
// Secrets (SEC-105): the 6-digit code reaches a label and nothing else - no log, no `printf`, no counter, no
// snapshot. The client name arrives from the network, so it is copied through a bounded, printable-only copy
// before it is rendered.
//
// One prompt at a time. `window_open` wins over a pending control approval, because the pairing window is
// opened by a physical tap and has a hard 180 s budget (FR-722), while a control request that is not answered
// simply expires and is denied - fail closed, never granted behind the card.
//
// LVGL owner context only; ui_theme_init() must have run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_overlay.h"
#include "ui_router.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest client name the prompts render, including the terminating NUL (FR-724 host name). */
#define UI_PAIR_NAME_CAP 24

/**
 * Overlay ids of the two prompts. Both are blocking: the router cancels the touch and asks for the HID
 * release before the card appears, takes the pad away while it is up and gives it back on dismiss.
 */
#define UI_OVERLAY_PAIRING 0x0301u
#define UI_OVERLAY_APPROVAL 0x0302u

/** Control ids (07 §3: UI-local ids, never keycodes). */
#define UI_CTRL_PAIR_OPEN 0x0310u   // "Pair a client" in the CLIENT details card
#define UI_CTRL_PAIR_CANCEL 0x0311u
#define UI_CTRL_PAIR_REJECT 0x0312u
#define UI_CTRL_PAIR_ACCEPT 0x0313u
#define UI_CTRL_APPROVAL_DENY 0x0314u
#define UI_CTRL_APPROVAL_ALLOW 0x0315u

/** Outcome of one pairing attempt, as the adapter reports it (SEC-104 counts the attempts, not the UI). */
typedef enum {
    UI_PAIR_RESULT_NONE = 0,
    UI_PAIR_RESULT_PAIRED,
    UI_PAIR_RESULT_REJECTED,
    UI_PAIR_RESULT_FAILED,
    UI_PAIR_RESULT_LOCKED,
    UI_PAIR_RESULT_EXPIRED,
} ui_pair_result_t;

/**
 * Pairing state the adapter publishes (FR-722, FR-725). `code` holds 6 ASCII digits and a NUL, or "" when
 * no code exists; it is shown as "ddd ddd" and never logged. The two remaining times are rendered as they
 * arrive - the view runs no clock of its own.
 */
typedef struct {
    bool window_open;   // a pairing window is open: show the card
    char code[8];       // 6 ASCII digits + NUL, "" when none
    uint32_t code_remaining_ms;
    uint32_t window_remaining_ms;
    bool accept_pending; // a CLIENT proved the code: show Accept / Reject (FR-722, FR-725)
    char client_name[UI_PAIR_NAME_CAP];
    uint32_t accept_remaining_ms;
    ui_pair_result_t result;  // toasted once, whenever `result_seq` changes
    uint32_t result_seq;      // 0 = nothing to report yet
    char result_name[UI_PAIR_NAME_CAP];
} ui_pair_view_t;

/** Control approval state (FR-733 `always` / `trusted`, PROC-006 step 5; 30 s prompt, 03 §6.5). */
typedef struct {
    bool pending;
    uint64_t session_id;
    char client_name[UI_PAIR_NAME_CAP];
    uint32_t remaining_ms;
} ui_approval_view_t;

/** What the local user asked for. Carries a session id for the approval actions only. */
typedef enum {
    UI_REMOTE_ACT_PAIR_OPEN = 0,
    UI_REMOTE_ACT_PAIR_CANCEL,
    UI_REMOTE_ACT_PAIR_ACCEPT,
    UI_REMOTE_ACT_PAIR_REJECT,
    UI_REMOTE_ACT_APPROVE,
    UI_REMOTE_ACT_DENY,
} ui_remote_action_t;

typedef void (*ui_remote_sink_fn)(void *ctx, ui_remote_action_t act, uint64_t session_id);

#define UI_REMOTE_OVERLAYS 2

typedef struct {
    uint32_t pair_opens;    // pairing cards built
    uint32_t pair_closes;   // pairing cards removed
    uint32_t appr_opens;    // approval cards built
    uint32_t appr_closes;
    uint32_t taps[6];       // one counter per ui_remote_action_t reported to the sink
    uint32_t toasts;        // outcome toasts shown
    uint32_t refused;       // a prompt the router would not open, or a tap with no sink
} ui_remote_view_counters_t;

typedef struct {
    // bound at init
    ui_router_t *router; // overlay stack + toast layer; must outlive the view
    ui_remote_sink_fn sink;
    void *sink_ctx;

    ui_overlay_descriptor_t ov[UI_REMOTE_OVERLAYS]; // hand these to ui_router_init()

    // last published state (the only source of what the cards render)
    ui_pair_view_t pair;
    ui_approval_view_t approval;
    uint32_t toasted_seq; // result_seq already reported

    // objects of the open pairing card (NULL while it is closed)
    lv_obj_t *pair_card;
    lv_obj_t *pair_heading;
    lv_obj_t *pair_body;
    lv_obj_t *code_box;
    lv_obj_t *code_label;
    lv_obj_t *code_timer;
    lv_obj_t *window_timer;
    lv_obj_t *accept_box;
    lv_obj_t *accept_timer;
    lv_obj_t *btn_cancel;
    lv_obj_t *btn_reject;
    lv_obj_t *btn_accept;

    // objects of the open approval card
    lv_obj_t *appr_card;
    lv_obj_t *appr_heading;
    lv_obj_t *appr_body;
    lv_obj_t *appr_timer;
    lv_obj_t *btn_deny;
    lv_obj_t *btn_allow;

    lv_obj_t *toast; // lives on the router's toast layer; cleared by its own delete event
    char toast_text[80];
    char name_text[UI_PAIR_NAME_CAP]; // printable-only copy of the name the open card renders
    char code_text[8];                // "ddd ddd"

    ui_remote_view_counters_t c;
} ui_remote_view_t;

/**
 * Bind the view to the router (overlay stack and toast layer) and fill `v->ov`. Builds no object.
 * `sink` may be NULL: the buttons then report nothing and are counted as refused.
 */
void ui_remote_view_init(ui_remote_view_t *v, ui_router_t *router, ui_remote_sink_fn sink, void *sink_ctx);

/** The descriptor array to register with the router; `count` receives UI_REMOTE_OVERLAYS. */
const ui_overlay_descriptor_t *ui_remote_view_overlays(ui_remote_view_t *v, uint8_t *count);

/**
 * Publish the pairing state: opens, updates or closes the pairing card and toasts a new `result_seq`.
 * Call it from the LVGL owner context on every change (the countdowns are only text).
 */
void ui_remote_view_set_pairing(ui_remote_view_t *v, const ui_pair_view_t *state);

/** Publish the control-approval state: opens, updates or closes the approval card. */
void ui_remote_view_set_approval(ui_remote_view_t *v, const ui_approval_view_t *state);

/** Before the screen goes away: drop the outcome toast (no transient object outlives the shell). */
void ui_remote_view_close(ui_remote_view_t *v);

/** The open card of an overlay id, or NULL when that prompt is not up. For tests and diagnostics. */
lv_obj_t *ui_remote_view_card(const ui_remote_view_t *v, uint16_t overlay_id);

/**
 * The code as the open card renders it ("ddd ddd"), or NULL when no pairing card is up or it shows no code.
 * In memory only - a caller must not log it (SEC-105).
 */
const char *ui_remote_view_code_text(const ui_remote_view_t *v);

/** Heading, explanation and countdown line of an open prompt, or NULL when it is not up. */
const char *ui_remote_view_heading_text(const ui_remote_view_t *v, uint16_t overlay_id);
const char *ui_remote_view_body_text(const ui_remote_view_t *v, uint16_t overlay_id);
const char *ui_remote_view_timer_text(const ui_remote_view_t *v, uint16_t overlay_id);

/** The second countdown of the pairing card - the whole window (FR-722) - while the code is shown. */
const char *ui_remote_view_window_text(const ui_remote_view_t *v);

/** Text of the outcome toast while it is up, else NULL. */
const char *ui_remote_view_toast_text(const ui_remote_view_t *v);

/** True while the pairing card asks the Accept / Reject question instead of showing the code (FR-725). */
bool ui_remote_view_accept_mode(const ui_remote_view_t *v);

#ifdef __cplusplus
}
#endif
