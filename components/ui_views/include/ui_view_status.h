// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// HOST and CLIENT status details (TASK-070C; 01 §6 FR-001..FR-010, FR-401..FR-403, reference connections
// popover): the two blocking overlay cards FR-003 opens when an indicator in the top bar is tapped.
//
// Each card is a live projection of the store subjects - observers are bound to the card object, so they die
// with the overlay (07 §4, AT-UIFW-09) and the store may even be deleted first. The cards read state and
// nothing else: no domain action, no HID, no pairing, no network call. FR-401/403: a value whose source never
// published reads "Unknown", a Wi-Fi link is shown separately from the CLIENT session and never upgrades it.
//
// 07 §7 forbids a password, token, session id, SSID or IP address in the status chrome, so no such field is
// rendered or even taken by this view.
//
// The CLIENT card carries one action: "Pair a client" (ADR-022 step 1). It is the physically present entry
// point to pairing - it dismisses the card and reports the request through `pair_action`; the window, the code
// and the outcome belong to `secure_remote` and are shown by ui_remote_view. SEC-106: the button is disabled
// while remote is off or the Wi-Fi link is down, and a value the store has not received yet counts as off.
//
// LVGL owner context only; ui_theme_init() must have run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_overlay.h"
#include "ui_router.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Overlay ids. The shell maps the typed action to the id:
 *   UI_ACT_OPEN_HOST_DETAILS   -> ui_overlay_show(r, UI_OVERLAY_HOST_DETAILS)
 *   UI_ACT_OPEN_CLIENT_DETAILS -> ui_overlay_show(r, UI_OVERLAY_CLIENT_DETAILS)
 */
#define UI_OVERLAY_HOST_DETAILS 0x0201u
#define UI_OVERLAY_CLIENT_DETAILS 0x0202u

/** Control id of the Close button (both cards): it submits UI_ACT_DISMISS_OVERLAY. */
#define UI_CTRL_STATUS_CLOSE 0x0210u

/**
 * Control id of "Pair a client" on the CLIENT card: it dismisses the card and reports the request to the
 * shell (ADR-022 step 1). It is a local, physically present action - the card itself opens no window.
 */
#define UI_CTRL_STATUS_PAIR 0x0211u

#define UI_VIEW_STATUS_PANELS 2
#define UI_VIEW_STATUS_ROWS_MAX 6

typedef struct ui_view_status_s ui_view_status_t;

/** One open details card. `rows` value labels, in the order of the panel's row table. */
typedef struct {
    ui_view_status_t *owner;
    bool client; // false = the HOST card
    lv_obj_t *card;
    lv_obj_t *value[UI_VIEW_STATUS_ROWS_MAX];
    lv_obj_t *icon[UI_VIEW_STATUS_ROWS_MAX]; // severity glyph (FR-005), NULL for a plain row
    uint8_t rows;
    lv_obj_t *pair_btn;  // CLIENT card only: "Pair a client" (NULL on the HOST card)
    lv_obj_t *pair_note; // one line that says what pairing does, or why it is unavailable (FR-005)
} ui_status_panel_t;

struct ui_view_status_s {
    // bound at init
    ui_store_t *store;
    ui_actions_t *actions; // Close submits UI_ACT_DISMISS_OVERLAY here (a local action, 07 §3)
    ui_router_t *router;   // optional fallback: used only when no local sink took the action
    uint64_t (*now_us)(void);

    /**
     * ADR-022 step 1: the local user asked to pair a client. The card dismisses itself and calls this; it
     * opens no window, generates no code and learns nothing about the outcome. NULL = no entry point.
     */
    void (*pair_action)(void *ctx);
    void *pair_ctx;

    ui_overlay_descriptor_t ov[UI_VIEW_STATUS_PANELS]; // hand these to ui_router_init()
    ui_status_panel_t panel[UI_VIEW_STATUS_PANELS];    // [0] HOST, [1] CLIENT

    uint32_t opens;
    uint32_t closes;
    uint32_t close_taps;
    uint32_t pair_taps; // "Pair a client" taps reported to the shell
};

/**
 * Fill s->ov with the two overlay descriptors. Builds no object. `actions` may be NULL (then Close falls back
 * to `router`); `router` may be NULL when the shell handles UI_ACT_DISMISS_OVERLAY itself.
 */
void ui_view_status_init(ui_view_status_t *s, ui_store_t *store, ui_actions_t *actions, ui_router_t *router,
                         uint64_t (*now_us)(void));

/** The descriptor array to register with the router; `count` receives UI_VIEW_STATUS_PANELS. */
const ui_overlay_descriptor_t *ui_view_status_overlays(ui_view_status_t *s, uint8_t *count);

/**
 * Install the "Pair a client" entry point of the CLIENT card (ADR-022 step 1). Without it the button is not
 * built at all: a shell that cannot pair shows no path to pairing.
 */
void ui_view_status_set_pair_action(ui_view_status_t *s, void (*fn)(void *ctx), void *ctx);

/** The open card of an overlay id, or NULL when it is not open. */
lv_obj_t *ui_view_status_card(ui_view_status_t *s, uint16_t overlay_id);

/** "Pair a client" of the open CLIENT card, or NULL when it is not open or was not installed. */
lv_obj_t *ui_view_status_pair_button(ui_view_status_t *s);

/** True while SEC-106 allows a pairing window: remote not off and the Wi-Fi link up (fail closed). */
bool ui_view_status_pair_available(const ui_view_status_t *s);

/**
 * Text currently rendered in row `index` of overlay `overlay_id`, or NULL when that overlay is not open or the
 * index is out of range. For tests and for a diagnostics dump.
 */
const char *ui_view_status_row_text(ui_view_status_t *s, uint16_t overlay_id, uint8_t index);

/** Caption of row `index` of `overlay_id` ("USB", "Wi-Fi link", ...), independent of whether it is open. */
const char *ui_view_status_row_caption(uint16_t overlay_id, uint8_t index);

/** Number of rows an overlay renders; 0 for an unknown id. */
uint8_t ui_view_status_row_count(uint16_t overlay_id);

#ifdef __cplusplus
}
#endif
