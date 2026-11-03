// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pairing and control-approval prompts (TASK-100 UI part). See ui_remote_view.h for the contract.
//
// Both prompts are router overlays with `blocking` set, which is what makes them safe: the router cancels the
// pending touch and asks for the HID release before the card exists, hands the pad back only after it is gone
// and puts a full-screen scrim between the card and everything below it. The view adds no second scrim, no
// timer and no state of its own - `reconcile()` only compares the state the adapter published with the card
// that is currently up.
//
// The card geometry is the one UiModal uses (UI_MODAL_W, UI_MODAL_PAD, UI_MODAL_BTN_H), so the pairing
// question looks exactly like the FR-734 confirmation the user already knows.
#include "ui_remote_view.h"

#include <stdio.h>
#include <string.h>

#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_tokens.h"

// Two equal halves of the button row: Accept and Reject are the same size, in the same variant, and neither is
// the default - a decision the user must read, not confirm by reflex (FR-603, FR-725).
// The card border eats into the content box too; without it the pair overflows by 2 px and the row, aligned to
// the end, clips the left button.
#define BTN_W ((UI_MODAL_W - 2 * UI_MODAL_PAD - 2 * UI_BORDER_W_BOLD - UI_SPACE_S) / 2)
#define CODE_H 72
#define CODE_TRACK 6 // letter spacing of the code, so two readers can speak the digits apart (FR-604)

// A pairing outcome stays up for the standard toast time; the SEC-104 lockout warning stays longer, because it
// reports a possible intruder on the network and the user may not be looking at the panel.
#define RESULT_TOAST_MS UI_TOAST_DEFAULT_MS
#define WARN_TOAST_MS 6000u

static const char *const k_code_body =
    "In the client app on your computer, pick this device and type the code below.";
static const char *const k_no_code_body = "No code to show. Cancel and start pairing again.";
static const char *const k_accept_body =
    "Accept only if you just typed this code into that client. Pairing does not give it control of the host.";
static const char *const k_appr_body =
    "Allowing lets this client move the pointer and type on the host. Local input always takes control back.";

// ---- small helpers -------------------------------------------------------------------------

static uint32_t secs(uint32_t ms)
{
    return (ms + 999u) / 1000u; // a running millisecond must never read as "0 s"
}

/**
 * Bounded, printable-ASCII copy of a name that arrived over the network (FR-724): anything else becomes '?',
 * so a crafted host name can neither break the card layout nor smuggle control characters into the panel.
 * An empty name reads "Unknown client" - the card never shows a nameless peer as if it had identified itself.
 */
static void copy_name(char *out, size_t cap, const char *in)
{
    size_t n = 0;
    if (in != NULL) {
        for (; n + 1u < cap && in[n] != '\0'; n++) {
            const unsigned char ch = (unsigned char)in[n];
            out[n] = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : '?';
        }
    }
    out[n] = '\0';
    if (n == 0u) {
        (void)snprintf(out, cap, "Unknown client");
    }
}

/**
 * "ddd ddd" of an exactly 6-digit code (FR-722). Anything else - short, long, non-digit - is refused and the
 * card says it has no code instead of rendering a partial one.
 */
static bool format_code(const char *code, char *out, size_t cap)
{
    out[0] = '\0';
    if (code == NULL || cap < 8u) {
        return false;
    }
    for (size_t i = 0; i < 6u; i++) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
    }
    if (code[6] != '\0') {
        return false;
    }
    out[0] = code[0];
    out[1] = code[1];
    out[2] = code[2];
    out[3] = ' ';
    out[4] = code[3];
    out[5] = code[4];
    out[6] = code[5];
    out[7] = '\0';
    return true;
}

static void report(ui_remote_view_t *v, ui_remote_action_t act, uint64_t session_id)
{
    if ((unsigned)act < (sizeof(v->c.taps) / sizeof(v->c.taps[0]))) {
        v->c.taps[act]++;
    }
    if (v->sink == NULL) {
        v->c.refused++;
        return;
    }
    // Last statement on purpose: the sink may publish a new state synchronously, which deletes this card.
    v->sink(v->sink_ctx, act, session_id);
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, ui_style_id_t style)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_theme_apply(l, style, LV_STATE_DEFAULT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_text(l, ""); // every text here changes with the state: an owned buffer, never a static one
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *label, lv_event_cb_t cb, ui_remote_view_t *v)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    ui_theme_apply_button(b, UI_BTN_SECONDARY);
    ui_theme_apply(b, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(b, BTN_W, UI_MODAL_BTN_H);
    lv_obj_set_scrollable(b, false);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_static(l, label);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, v);
    return b;
}

static lv_obj_t *button_row(lv_obj_t *card)
{
    lv_obj_t *row = plain(card);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SPACE_S, LV_PART_MAIN);
    return row;
}

/** The card the router centers on its scrim, dressed as the shared modal surface. */
static void dress_card(lv_obj_t *card)
{
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, UI_MODAL_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim below it
    lv_obj_center(card);
}

/** A prompt the user cannot see must not be reachable either: hidden and not clickable. */
static void show_obj(lv_obj_t *obj, bool on)
{
    lv_obj_set_hidden(obj, !on);
    lv_obj_set_clickable(obj, on);
}

// ---- pairing card --------------------------------------------------------------------------

static void forget_pair(ui_remote_view_t *v)
{
    v->pair_card = v->pair_heading = v->pair_body = NULL;
    v->code_box = v->code_label = v->code_timer = v->window_timer = NULL;
    v->accept_box = v->accept_timer = NULL;
    v->btn_cancel = v->btn_reject = v->btn_accept = NULL;
}

static void paint_pair(ui_remote_view_t *v)
{
    if (v->pair_card == NULL) {
        return;
    }
    const ui_pair_view_t *p = &v->pair;
    const bool accept = p->accept_pending;
    const bool code_ok = format_code(p->code, v->code_text, sizeof(v->code_text));

    if (accept) {
        // FR-725: the client that proved the code is named, and the question is about that name.
        copy_name(v->name_text, sizeof(v->name_text), p->client_name);
        lv_label_set_text_fmt(v->pair_heading, "%s wants to pair", v->name_text);
        lv_label_set_text(v->pair_body, k_accept_body);
        lv_label_set_text_fmt(v->accept_timer, "Rejected automatically in %u s",
                              (unsigned)secs(p->accept_remaining_ms));
    } else {
        lv_label_set_text(v->pair_heading, "Pair a client");
        lv_label_set_text(v->pair_body, code_ok ? k_code_body : k_no_code_body);
        lv_label_set_text(v->code_label, code_ok ? v->code_text : "");
        lv_label_set_text_fmt(v->code_timer, "Code expires in %u s", (unsigned)secs(p->code_remaining_ms));
        lv_label_set_text_fmt(v->window_timer, "Pairing window closes in %u s",
                              (unsigned)secs(p->window_remaining_ms));
    }
    // The code leaves the screen the moment it has been used: the question is now about the client, not the code.
    lv_obj_set_hidden(v->code_box, accept);
    lv_obj_set_hidden(v->accept_box, !accept);
    show_obj(v->btn_cancel, !accept);
    show_obj(v->btn_reject, accept);
    show_obj(v->btn_accept, accept);
}

static void pair_cancel_cb(lv_event_t *e)
{
    report(lv_event_get_user_data(e), UI_REMOTE_ACT_PAIR_CANCEL, 0u);
}

static void pair_reject_cb(lv_event_t *e)
{
    report(lv_event_get_user_data(e), UI_REMOTE_ACT_PAIR_REJECT, 0u);
}

static void pair_accept_cb(lv_event_t *e)
{
    report(lv_event_get_user_data(e), UI_REMOTE_ACT_PAIR_ACCEPT, 0u);
}

static bool build_pair(void *ctx, lv_obj_t *card)
{
    ui_remote_view_t *v = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    dress_card(card);
    v->pair_card = card;
    v->pair_heading = text(card, UI_STYLE_TEXT_HEADING);
    v->pair_body = text(card, UI_STYLE_TEXT_BODY);

    v->code_box = plain(card);
    lv_obj_set_width(v->code_box, lv_pct(100));
    lv_obj_set_height(v->code_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->code_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v->code_box, UI_SPACE_XS, LV_PART_MAIN);

    lv_obj_t *well = plain(v->code_box);
    ui_theme_apply(well, UI_STYLE_WELL, LV_STATE_DEFAULT);
    lv_obj_set_width(well, lv_pct(100));
    lv_obj_set_height(well, CODE_H);
    v->code_label = lv_label_create(well);
    ui_theme_apply(v->code_label, UI_STYLE_TEXT_TITLE, LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(v->code_label, CODE_TRACK, LV_PART_MAIN);
    lv_label_set_text(v->code_label, "");
    lv_obj_center(v->code_label);

    v->code_timer = text(v->code_box, UI_STYLE_TEXT_MUTED);
    v->window_timer = text(v->code_box, UI_STYLE_TEXT_MUTED);

    v->accept_box = plain(card);
    lv_obj_set_width(v->accept_box, lv_pct(100));
    lv_obj_set_height(v->accept_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v->accept_box, LV_FLEX_FLOW_COLUMN);
    v->accept_timer = text(v->accept_box, UI_STYLE_TEXT_MUTED);

    lv_obj_t *row = button_row(card);
    v->btn_cancel = button(row, "Cancel", pair_cancel_cb, v);
    v->btn_reject = button(row, "Reject", pair_reject_cb, v);
    v->btn_accept = button(row, "Accept", pair_accept_cb, v);

    v->c.pair_opens++;
    paint_pair(v);
    return true;
}

static void close_pair(void *ctx)
{
    ui_remote_view_t *v = ctx;
    forget_pair(v);
    v->c.pair_closes++;
}

// ---- approval card -------------------------------------------------------------------------

static void forget_approval(ui_remote_view_t *v)
{
    v->appr_card = v->appr_heading = v->appr_body = v->appr_timer = NULL;
    v->btn_deny = v->btn_allow = NULL;
}

static void paint_approval(ui_remote_view_t *v)
{
    if (v->appr_card == NULL) {
        return;
    }
    copy_name(v->name_text, sizeof(v->name_text), v->approval.client_name);
    lv_label_set_text_fmt(v->appr_heading, "%s requests control of the HOST", v->name_text);
    lv_label_set_text_fmt(v->appr_timer, "Denied automatically in %u s",
                          (unsigned)secs(v->approval.remaining_ms));
}

static void approval_deny_cb(lv_event_t *e)
{
    ui_remote_view_t *v = lv_event_get_user_data(e);
    report(v, UI_REMOTE_ACT_DENY, v->approval.session_id);
}

static void approval_allow_cb(lv_event_t *e)
{
    ui_remote_view_t *v = lv_event_get_user_data(e);
    // The session id of the request this card was built for: a grant can never land on a newer session.
    report(v, UI_REMOTE_ACT_APPROVE, v->approval.session_id);
}

static bool build_approval(void *ctx, lv_obj_t *card)
{
    ui_remote_view_t *v = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    dress_card(card);
    v->appr_card = card;
    v->appr_heading = text(card, UI_STYLE_TEXT_HEADING);
    v->appr_body = text(card, UI_STYLE_TEXT_BODY);
    lv_label_set_text(v->appr_body, k_appr_body);
    v->appr_timer = text(card, UI_STYLE_TEXT_MUTED);

    lv_obj_t *row = button_row(card);
    v->btn_deny = button(row, "Deny", approval_deny_cb, v);
    v->btn_allow = button(row, "Allow", approval_allow_cb, v);

    v->c.appr_opens++;
    paint_approval(v);
    return true;
}

static void close_approval(void *ctx)
{
    ui_remote_view_t *v = ctx;
    forget_approval(v);
    v->c.appr_closes++;
}

// ---- outcome toast -------------------------------------------------------------------------

static void toast_deleted(lv_event_t *e)
{
    ui_remote_view_t *v = lv_event_get_user_data(e);
    v->toast = NULL;
}

/**
 * One line per outcome. A name is shown only where the local user has already seen and judged it (paired,
 * rejected): a failed or locked-out attempt carries an unauthenticated name, so the toast names no one and
 * reports the attempt instead (SEC-104, SEC-105).
 */
static uint32_t result_line(ui_remote_view_t *v, ui_pair_result_t r, const char *name)
{
    switch (r) {
    case UI_PAIR_RESULT_PAIRED:
        copy_name(v->name_text, sizeof(v->name_text), name);
        (void)snprintf(v->toast_text, sizeof(v->toast_text), "Paired: %s", v->name_text);
        return RESULT_TOAST_MS;
    case UI_PAIR_RESULT_REJECTED:
        copy_name(v->name_text, sizeof(v->name_text), name);
        (void)snprintf(v->toast_text, sizeof(v->toast_text), "Pairing rejected: %s", v->name_text);
        return RESULT_TOAST_MS;
    case UI_PAIR_RESULT_FAILED:
        (void)snprintf(v->toast_text, sizeof(v->toast_text), "Pairing attempt failed - the code did not match.");
        return RESULT_TOAST_MS;
    case UI_PAIR_RESULT_LOCKED:
        (void)snprintf(v->toast_text, sizeof(v->toast_text),
                       "Pairing locked - repeated failed attempts on the network.");
        return WARN_TOAST_MS;
    case UI_PAIR_RESULT_EXPIRED:
    default:
        (void)snprintf(v->toast_text, sizeof(v->toast_text), "Pairing window closed - the code expired.");
        return RESULT_TOAST_MS;
    }
}

static void say_result(ui_remote_view_t *v, ui_pair_result_t r, const char *name)
{
    lv_obj_t *parent = v->router != NULL ? v->router->toast_layer : NULL;
    if (parent == NULL) {
        v->c.refused++;
        return;
    }
    const uint32_t ms = result_line(v, r, name);
    if (v->toast != NULL) {
        ui_toast_dismiss(v->toast); // one outcome at a time; its delete event clears the pointer
    }
    v->toast = ui_toast_show(parent, v->toast_text, ms);
    if (v->toast == NULL) {
        v->c.refused++;
        return;
    }
    lv_obj_add_event_cb(v->toast, toast_deleted, LV_EVENT_DELETE, v);
    v->c.toasts++;
}

// ---- reconciliation ------------------------------------------------------------------------

/**
 * The prompt the published state asks for. A pairing window wins over a waiting control approval: the window
 * was opened by a tap on this panel and dies at a hard 180 s deadline (FR-722), while an unanswered control
 * request is denied by its own timeout - deferring it refuses control, it never grants it.
 */
static uint16_t wanted(const ui_remote_view_t *v)
{
    if (v->pair.window_open) {
        return UI_OVERLAY_PAIRING;
    }
    if (v->approval.pending) {
        return UI_OVERLAY_APPROVAL;
    }
    return UI_OVERLAY_NONE;
}

static uint16_t standing(const ui_remote_view_t *v)
{
    if (v->pair_card != NULL) {
        return UI_OVERLAY_PAIRING;
    }
    return v->appr_card != NULL ? UI_OVERLAY_APPROVAL : UI_OVERLAY_NONE;
}

static void reconcile(ui_remote_view_t *v)
{
    const uint16_t want = wanted(v);
    const uint16_t open = standing(v);
    if (open == want) {
        paint_pair(v);
        paint_approval(v);
        return;
    }
    if (open != UI_OVERLAY_NONE) {
        if (ui_overlay_top(v->router) != open) {
            // Our card is open but something sits on top of it: do not pop a stranger's overlay.
            v->c.refused++;
            return;
        }
        ui_overlay_dismiss(v->router); // on_close drops our pointers
    }
    if (want != UI_OVERLAY_NONE && !ui_overlay_show(v->router, want)) {
        // Counted, never faked: the adapter keeps publishing, so the next update retries the prompt.
        v->c.refused++;
    }
}

// ---- public API ----------------------------------------------------------------------------

void ui_remote_view_init(ui_remote_view_t *v, ui_router_t *router, ui_remote_sink_fn sink, void *sink_ctx)
{
    if (v == NULL) {
        return;
    }
    memset(v, 0, sizeof(*v));
    v->router = router;
    v->sink = sink;
    v->sink_ctx = sink_ctx;
    v->ov[0] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_PAIRING,
        .create = build_pair,
        .on_close = close_pair,
        .blocking = true, // the router cancels the touch, releases HID and takes the pad away
        .ctx = v,
    };
    v->ov[1] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_APPROVAL,
        .create = build_approval,
        .on_close = close_approval,
        .blocking = true,
        .ctx = v,
    };
}

const ui_overlay_descriptor_t *ui_remote_view_overlays(ui_remote_view_t *v, uint8_t *count)
{
    if (count != NULL) {
        *count = UI_REMOTE_OVERLAYS;
    }
    return v != NULL ? v->ov : NULL;
}

void ui_remote_view_set_pairing(ui_remote_view_t *v, const ui_pair_view_t *state)
{
    if (v == NULL || state == NULL || v->router == NULL) {
        return;
    }
    v->pair = *state;
    reconcile(v);
    // Reported after the card has been reconciled, so a closing window and its outcome appear in that order.
    if (state->result != UI_PAIR_RESULT_NONE && state->result_seq != v->toasted_seq) {
        say_result(v, state->result, state->result_name);
    }
    v->toasted_seq = state->result_seq;
}

void ui_remote_view_set_approval(ui_remote_view_t *v, const ui_approval_view_t *state)
{
    if (v == NULL || state == NULL || v->router == NULL) {
        return;
    }
    v->approval = *state;
    reconcile(v);
}

void ui_remote_view_close(ui_remote_view_t *v)
{
    if (v == NULL) {
        return;
    }
    if (v->toast != NULL) {
        ui_toast_dismiss(v->toast); // the toast lives outside the cards: it must not outlive the screen
    }
}

lv_obj_t *ui_remote_view_card(const ui_remote_view_t *v, uint16_t overlay_id)
{
    if (v == NULL) {
        return NULL;
    }
    if (overlay_id == UI_OVERLAY_PAIRING) {
        return v->pair_card;
    }
    return overlay_id == UI_OVERLAY_APPROVAL ? v->appr_card : NULL;
}

const char *ui_remote_view_code_text(const ui_remote_view_t *v)
{
    if (v == NULL || v->code_label == NULL || v->pair.accept_pending) {
        return NULL;
    }
    const char *txt = lv_label_get_text(v->code_label);
    return (txt != NULL && txt[0] != '\0') ? txt : NULL;
}

static const char *label_text(const lv_obj_t *label)
{
    return label != NULL ? lv_label_get_text((lv_obj_t *)label) : NULL;
}

const char *ui_remote_view_heading_text(const ui_remote_view_t *v, uint16_t overlay_id)
{
    if (v == NULL) {
        return NULL;
    }
    if (overlay_id == UI_OVERLAY_PAIRING) {
        return label_text(v->pair_heading);
    }
    return overlay_id == UI_OVERLAY_APPROVAL ? label_text(v->appr_heading) : NULL;
}

const char *ui_remote_view_body_text(const ui_remote_view_t *v, uint16_t overlay_id)
{
    if (v == NULL) {
        return NULL;
    }
    if (overlay_id == UI_OVERLAY_PAIRING) {
        return label_text(v->pair_body);
    }
    return overlay_id == UI_OVERLAY_APPROVAL ? label_text(v->appr_body) : NULL;
}

const char *ui_remote_view_timer_text(const ui_remote_view_t *v, uint16_t overlay_id)
{
    if (v == NULL) {
        return NULL;
    }
    if (overlay_id == UI_OVERLAY_PAIRING) {
        return label_text(v->pair.accept_pending ? v->accept_timer : v->code_timer);
    }
    return overlay_id == UI_OVERLAY_APPROVAL ? label_text(v->appr_timer) : NULL;
}

const char *ui_remote_view_window_text(const ui_remote_view_t *v)
{
    return (v != NULL && !v->pair.accept_pending) ? label_text(v->window_timer) : NULL;
}

const char *ui_remote_view_toast_text(const ui_remote_view_t *v)
{
    return (v != NULL && v->toast != NULL) ? v->toast_text : NULL;
}

bool ui_remote_view_accept_mode(const ui_remote_view_t *v)
{
    return v != NULL && v->pair_card != NULL && v->pair.accept_pending;
}
