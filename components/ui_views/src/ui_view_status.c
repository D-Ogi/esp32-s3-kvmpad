// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// HOST and CLIENT status details (TASK-070C). See ui_view_status.h for the contract.
//
// One row table per card drives both the build and every refresh, so a row cannot exist in one and be missing
// from the other. The wording of a domain value always comes from ui_present (01 §6.1 / §6.2); a value this
// view formats itself - epoch, LED, maintenance, error, Wi-Fi link - has an explicit "Unknown" branch, because
// the store holds UI_STORE_UNKNOWN until the producer has published (07 §12 item 2).
#include "ui_view_status.h"

#include <string.h>

#include "hid_core.h"
#include "ui_present.h"
#include "ui_theme.h"
#include "ui_tokens.h"

#define CARD_W UI_MODAL_W
#define CLOSE_W 168
#define PAIR_W 200

typedef enum {
    VAL_HOST_USB = 0, // ui_present_host
    VAL_OWNER,        // ui_present_owner
    VAL_EPOCH,
    VAL_CAPS_LED,
    VAL_MAINTENANCE,
    VAL_ERROR,
    VAL_CLIENT, // ui_present_client
    VAL_WIFI,
} value_kind_t;

typedef struct {
    const char *caption;
    ui_subject_id_t subject;
    value_kind_t kind;
    bool severity; // FR-005: this row also carries a severity glyph, not color alone
} row_def_t;

// 01 §6.1: the HOST card answers "what does the USB side look like and who may drive it".
static const row_def_t k_host_rows[] = {
    {"USB", UI_SUBJ_HOST_USB, VAL_HOST_USB, true},
    {"Control owner", UI_SUBJ_OWNER_KIND, VAL_OWNER, false},
    {"Owner epoch", UI_SUBJ_OWNER_EPOCH, VAL_EPOCH, false},
    {"Caps Lock LED", UI_SUBJ_CAPS_LED, VAL_CAPS_LED, false},
    {"Maintenance", UI_SUBJ_MAINTENANCE, VAL_MAINTENANCE, false},
    {"Error code", UI_SUBJ_ERROR, VAL_ERROR, false},
};

// 01 §6.2 + FR-403: the session and the link are two separate rows, and the note says why.
static const row_def_t k_client_rows[] = {
    {"Client session", UI_SUBJ_CLIENT, VAL_CLIENT, true},
    {"Wi-Fi link", UI_SUBJ_WIFI, VAL_WIFI, false},
};

#define HOST_ROWS ((uint8_t)(sizeof(k_host_rows) / sizeof(k_host_rows[0])))
#define CLIENT_ROWS ((uint8_t)(sizeof(k_client_rows) / sizeof(k_client_rows[0])))

static const char *const k_client_note =
    "A Wi-Fi link is not a client connection: the client status needs an authenticated session.";

// One line beside the action, so the state of the button is never carried by its ink alone (FR-005).
static const char *const k_pair_note = "Pairing opens a short window on this device and shows a 6-digit code.";
static const char *const k_pair_blocked_note = "Pairing needs remote switched on and a Wi-Fi link.";

static const row_def_t *rows_of(bool client, uint8_t *count)
{
    *count = client ? CLIENT_ROWS : HOST_ROWS;
    return client ? k_client_rows : k_host_rows;
}

// ---- value formatting ----------------------------------------------------------------------

static ui_severity_t host_severity(int32_t v)
{
    switch (v) {
    case KVM_HOST_USB_MOUNTED: return UI_SEVERITY_OK;
    case KVM_HOST_USB_ENUMERATING:
    case KVM_HOST_USB_SUSPENDED: return UI_SEVERITY_WARNING;
    case KVM_HOST_USB_DETACHED:
    case KVM_HOST_USB_ERROR: return UI_SEVERITY_OFFLINE;
    default: return UI_SEVERITY_UNKNOWN;
    }
}

static ui_severity_t client_severity(int32_t v)
{
    switch (v) {
    case UI_CLIENT_CONNECTED:
    case UI_CLIENT_CONTROL_GRANTED: return UI_SEVERITY_OK;
    case UI_CLIENT_LISTENING:
    case UI_CLIENT_AUTHENTICATING:
    case UI_CLIENT_STALE: return UI_SEVERITY_WARNING;
    case UI_CLIENT_DISABLED:
    case UI_CLIENT_NETWORK_DOWN: return UI_SEVERITY_OFFLINE;
    default: return UI_SEVERITY_UNKNOWN;
    }
}

/** Writes the value of one row into its label. Returns the severity of the row (UNKNOWN when it has none). */
static ui_severity_t render_row(lv_obj_t *label, const row_def_t *def, int32_t v)
{
    switch (def->kind) {
    case VAL_HOST_USB:
        lv_label_set_text_static(label, ui_present_host(v));
        return host_severity(v);
    case VAL_CLIENT:
        lv_label_set_text_static(label, ui_present_client(v));
        return client_severity(v);
    case VAL_OWNER:
        lv_label_set_text_static(label, ui_present_owner(v));
        return UI_SEVERITY_UNKNOWN;
    case VAL_EPOCH:
        if (v == UI_STORE_UNKNOWN) {
            lv_label_set_text_static(label, "Unknown");
        } else {
            lv_label_set_text_fmt(label, "%ld", (long)v);
        }
        return UI_SEVERITY_UNKNOWN;
    case VAL_CAPS_LED:
        // FR-206: without an LED output report from the HOST the state is Unknown, never guessed.
        lv_label_set_text_static(label, v == 1 ? "On" : (v == 0 ? "Off" : "Unknown"));
        return UI_SEVERITY_UNKNOWN;
    case VAL_MAINTENANCE:
        // FR-006: an update or a factory reset blocks input; say so instead of only dimming controls.
        lv_label_set_text_static(label, v == 1 ? "Active - input blocked" : (v == 0 ? "No" : "Unknown"));
        return UI_SEVERITY_UNKNOWN;
    case VAL_ERROR:
        if (v == UI_STORE_UNKNOWN) {
            lv_label_set_text_static(label, "Unknown");
        } else if (v == 0) {
            lv_label_set_text_static(label, "None");
        } else {
            lv_label_set_text_fmt(label, "0x%04lX", (unsigned long)v);
        }
        return UI_SEVERITY_UNKNOWN;
    case VAL_WIFI:
    default:
        // FR-403: the link is reported on its own and never promoted to a CLIENT connection.
        lv_label_set_text_static(label, v == 1 ? "Connected" : (v == 0 ? "Not connected" : "Unknown"));
        return UI_SEVERITY_UNKNOWN;
    }
}

/**
 * SEC-106: a pairing window may be opened only when remote is enabled and the device is on Wi-Fi. A state
 * the store has not received yet is treated as "not available", never as a permission (07 §12 item 2).
 */
bool ui_view_status_pair_available(const ui_view_status_t *s)
{
    if (s == NULL || s->store == NULL) {
        return false;
    }
    const int32_t client = ui_store_get(s->store, UI_SUBJ_CLIENT);
    return client != UI_STORE_UNKNOWN && client != (int32_t)UI_CLIENT_DISABLED &&
           ui_store_get(s->store, UI_SUBJ_WIFI) == 1;
}

static void refresh_pair(ui_status_panel_t *p)
{
    if (p->pair_btn == NULL) {
        return;
    }
    const bool on = ui_view_status_pair_available(p->owner);
    lv_obj_set_disabled(p->pair_btn, !on);
    lv_obj_set_clickable(p->pair_btn, on);
    lv_obj_t *label = lv_obj_get_child(p->pair_btn, 0);
    if (label != NULL) {
        // LVGL hands no state to child objects, so the legend of a disabled button is repainted here.
        lv_obj_set_style_text_color(label, lv_color_hex(on ? UI_COLOR_INK : UI_COLOR_DISABLED_INK),
                                    LV_PART_MAIN);
    }
    lv_label_set_text_static(p->pair_note, on ? k_pair_note : k_pair_blocked_note);
}

static void refresh(ui_status_panel_t *p)
{
    if (p->card == NULL) {
        return;
    }
    refresh_pair(p);
    uint8_t n = 0;
    const row_def_t *defs = rows_of(p->client, &n);
    for (uint8_t i = 0; i < n && i < UI_VIEW_STATUS_ROWS_MAX; i++) {
        const int32_t v = ui_store_get(p->owner->store, defs[i].subject);
        const ui_severity_t sev = render_row(p->value[i], &defs[i], v);
        if (p->icon[i] != NULL) {
            lv_label_set_text_static(p->icon[i], ui_theme_severity_icon(sev));
            lv_obj_set_style_text_color(p->icon[i], ui_theme_severity_color(sev), LV_PART_MAIN);
            lv_obj_set_style_text_color(p->value[i], ui_theme_severity_color(sev), LV_PART_MAIN);
        }
    }
}

static void subject_cb(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)subj;
    refresh(lv_observer_get_user_data(obs));
}

// ---- construction --------------------------------------------------------------------------

static void close_clicked(lv_event_t *e)
{
    ui_status_panel_t *p = lv_event_get_user_data(e);
    ui_view_status_t *s = p->owner;
    s->close_taps++;
    // 07 §3: the card asks, the shell navigates. The router fallback keeps the button working in a shell that
    // has not installed a local action sink.
    const uint32_t rid = s->actions != NULL
                             ? ui_actions_submit(s->actions, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0u,
                                                 s->now_us != NULL ? s->now_us() : 0u, NULL)
                             : 0u;
    if (rid == 0u && s->router != NULL) {
        ui_overlay_dismiss(s->router);
    }
}

/**
 * ADR-022 step 1. The card closes first - the pairing prompt is a blocking overlay of its own and the stack
 * holds one decision at a time - and only then is the request reported. The view neither opens a window nor
 * assumes one was opened: if the domain refuses, no prompt appears and nothing here claims otherwise.
 */
static void pair_clicked(lv_event_t *e)
{
    ui_status_panel_t *p = lv_event_get_user_data(e);
    ui_view_status_t *s = p->owner;
    if (!ui_view_status_pair_available(s)) {
        return; // a disabled button should not emit a click at all; refused here as well (fail closed)
    }
    s->pair_taps++;
    const uint32_t rid = s->actions != NULL
                             ? ui_actions_submit(s->actions, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_PAIR, 0u,
                                                 s->now_us != NULL ? s->now_us() : 0u, NULL)
                             : 0u;
    if (rid == 0u && s->router != NULL) {
        ui_overlay_dismiss(s->router);
    }
    if (s->pair_action != NULL) {
        s->pair_action(s->pair_ctx); // `p` is gone from here on: the card was dismissed above
    }
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const char *txt, ui_style_id_t style)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_theme_apply(l, style, LV_STATE_DEFAULT);
    lv_label_set_text_static(l, txt);
    return l;
}

static bool build(void *ctx, lv_obj_t *card)
{
    ui_status_panel_t *p = ctx;
    ui_view_status_t *s = p->owner;
    if (s->store == NULL || !ui_theme_is_ready()) {
        return false;
    }
    // The router created the card with the default object style and centered it: take the theme card instead
    // and put the centering back (lv_obj_center is a local style that remove_style_all clears).
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim below
    lv_obj_center(card);

    (void)text(card, p->client ? "Client - remote session" : "Host - USB HID", UI_STYLE_TEXT_HEADING);

    uint8_t n = 0;
    const row_def_t *defs = rows_of(p->client, &n);
    p->rows = n;
    for (uint8_t i = 0; i < n && i < UI_VIEW_STATUS_ROWS_MAX; i++) {
        lv_obj_t *line = plain(card);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, UI_SPACE_S, LV_PART_MAIN);
        (void)text(line, defs[i].caption, UI_STYLE_TEXT_MUTED);

        lv_obj_t *group = plain(line);
        lv_obj_set_size(group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(group, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(group, UI_SPACE_XS, LV_PART_MAIN);
        p->icon[i] = defs[i].severity ? text(group, "", UI_STYLE_TEXT_BODY) : NULL;
        p->value[i] = text(group, "", UI_STYLE_TEXT_BODY);
    }

    const bool pairing = p->client && s->pair_action != NULL;
    if (p->client) {
        lv_obj_t *note = text(card, k_client_note, UI_STYLE_TEXT_MUTED);
        lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(note, lv_pct(100));
    }
    if (pairing) {
        p->pair_note = text(card, k_pair_note, UI_STYLE_TEXT_MUTED);
        lv_label_set_long_mode(p->pair_note, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(p->pair_note, lv_pct(100));
    }

    lv_obj_t *foot = plain(card);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    // With an action beside it the row splits; alone, Close stays where every card has always put it.
    lv_obj_set_flex_align(foot, pairing ? LV_FLEX_ALIGN_SPACE_BETWEEN : LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (pairing) {
        p->pair_btn = lv_button_create(foot);
        lv_obj_remove_style_all(p->pair_btn);
        ui_theme_apply_button(p->pair_btn, UI_BTN_SECONDARY);
        ui_theme_apply(p->pair_btn, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
        lv_obj_set_size(p->pair_btn, PAIR_W, UI_MODAL_BTN_H);
        lv_obj_set_scrollable(p->pair_btn, false);
        lv_obj_t *pair_label = lv_label_create(p->pair_btn);
        lv_label_set_text_static(pair_label, "Pair a client");
        lv_obj_center(pair_label);
        lv_obj_add_event_cb(p->pair_btn, pair_clicked, LV_EVENT_CLICKED, p);
    }
    lv_obj_t *close = lv_button_create(foot);
    lv_obj_remove_style_all(close);
    ui_theme_apply_button(close, UI_BTN_SECONDARY);
    lv_obj_set_size(close, CLOSE_W, UI_MODAL_BTN_H);
    lv_obj_set_scrollable(close, false);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text_static(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(close, close_clicked, LV_EVENT_CLICKED, p);

    p->card = card;
    // Observers bound to the card: dismissing the overlay deletes the card and detaches them (AT-UIFW-09).
    for (uint8_t i = 0; i < n && i < UI_VIEW_STATUS_ROWS_MAX; i++) {
        lv_subject_add_observer_obj(ui_store_subject(s->store, defs[i].subject), subject_cb, card, p);
    }
    refresh(p);
    s->opens++;
    return true;
}

static void on_close(void *ctx)
{
    ui_status_panel_t *p = ctx;
    p->card = NULL;
    p->pair_btn = NULL;
    p->pair_note = NULL;
    memset(p->value, 0, sizeof(p->value));
    memset(p->icon, 0, sizeof(p->icon));
    p->rows = 0;
    p->owner->closes++;
}

// ---- public API ----------------------------------------------------------------------------

void ui_view_status_init(ui_view_status_t *s, ui_store_t *store, ui_actions_t *actions, ui_router_t *router,
                         uint64_t (*now_us)(void))
{
    memset(s, 0, sizeof(*s));
    s->store = store;
    s->actions = actions;
    s->router = router;
    s->now_us = now_us;
    for (int i = 0; i < UI_VIEW_STATUS_PANELS; i++) {
        s->panel[i].owner = s;
        s->panel[i].client = (i == 1);
        s->ov[i] = (ui_overlay_descriptor_t){
            .id = (i == 1) ? UI_OVERLAY_CLIENT_DETAILS : UI_OVERLAY_HOST_DETAILS,
            .create = build,
            .on_close = on_close,
            .blocking = true, // FR-003 details are a modal card: the view below gets no input meanwhile
            .ctx = &s->panel[i],
        };
    }
}

void ui_view_status_set_pair_action(ui_view_status_t *s, void (*fn)(void *ctx), void *ctx)
{
    if (s != NULL) {
        s->pair_action = fn;
        s->pair_ctx = ctx;
    }
}

const ui_overlay_descriptor_t *ui_view_status_overlays(ui_view_status_t *s, uint8_t *count)
{
    if (count != NULL) {
        *count = UI_VIEW_STATUS_PANELS;
    }
    return s->ov;
}

static ui_status_panel_t *panel_of(ui_view_status_t *s, uint16_t overlay_id)
{
    for (int i = 0; i < UI_VIEW_STATUS_PANELS; i++) {
        if (s->ov[i].id == overlay_id) {
            return &s->panel[i];
        }
    }
    return NULL;
}

lv_obj_t *ui_view_status_card(ui_view_status_t *s, uint16_t overlay_id)
{
    ui_status_panel_t *p = panel_of(s, overlay_id);
    return p != NULL ? p->card : NULL;
}

lv_obj_t *ui_view_status_pair_button(ui_view_status_t *s)
{
    ui_status_panel_t *p = s != NULL ? panel_of(s, UI_OVERLAY_CLIENT_DETAILS) : NULL;
    return p != NULL ? p->pair_btn : NULL;
}

const char *ui_view_status_row_text(ui_view_status_t *s, uint16_t overlay_id, uint8_t index)
{
    ui_status_panel_t *p = panel_of(s, overlay_id);
    if (p == NULL || p->card == NULL || index >= p->rows || p->value[index] == NULL) {
        return NULL;
    }
    return lv_label_get_text(p->value[index]);
}

const char *ui_view_status_row_caption(uint16_t overlay_id, uint8_t index)
{
    if (overlay_id != UI_OVERLAY_HOST_DETAILS && overlay_id != UI_OVERLAY_CLIENT_DETAILS) {
        return NULL;
    }
    uint8_t n = 0;
    const row_def_t *defs = rows_of(overlay_id == UI_OVERLAY_CLIENT_DETAILS, &n);
    return index < n ? defs[index].caption : NULL;
}

uint8_t ui_view_status_row_count(uint16_t overlay_id)
{
    if (overlay_id == UI_OVERLAY_HOST_DETAILS) {
        return HOST_ROWS;
    }
    return overlay_id == UI_OVERLAY_CLIENT_DETAILS ? CLIENT_ROWS : 0u;
}
