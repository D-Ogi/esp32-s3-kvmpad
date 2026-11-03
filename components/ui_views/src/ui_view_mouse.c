// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// MOUSE view (TASK-070A). See ui_view_mouse.h for the contract. Layout: 01 §2.1 scaled to the content slot below
// the top bar (one dominant touch surface, a footer row of >= 72 px keys), light theme tokens of ui_theme (FR-008).
#include "ui_view_mouse.h"

#include <string.h>

#include "hid_core.h"
#include "kvm_hid_intent.h"
#include "ui_overlay.h"
#include "ui_theme.h"
#include "ui_tokens.h"

#define FOOT_H 72
#define KEY_W 196

static const ui_action_key_desc_t k_left = {.label = "Left click", .icon = LV_SYMBOL_LEFT,
                                            .control_id = UI_CTRL_MOUSE_LEFT, .variant = UI_KEY_NORMAL,
                                            .width = KEY_W, .height = FOOT_H, .font = UI_FONT_BODY};
static const ui_action_key_desc_t k_right = {.label = "Right click", .icon = LV_SYMBOL_RIGHT,
                                             .control_id = UI_CTRL_MOUSE_RIGHT, .variant = UI_KEY_NORMAL,
                                             .width = KEY_W, .height = FOOT_H, .font = UI_FONT_BODY};
// Not checkable: LV_STATE_CHECKED is a projection of the authoritative drag-lock state, never a local toggle.
static const ui_action_key_desc_t k_drag = {.label = "Drag lock", .icon = LV_SYMBOL_LOOP,
                                            .control_id = UI_CTRL_MOUSE_DRAG_LOCK, .variant = UI_KEY_MODIFIER,
                                            .width = KEY_W, .height = FOOT_H, .font = UI_FONT_BODY};

#define HELP_W 72
#define HELP_CARD_W 680
#define HELP_CLOSE_W 168

// The gesture card. Thresholds as configured in gesture_engine (KVM_GESTURE_CFG_DEFAULT: 550 ms hold, 300 ms double
// tap window, 10 s drag-lock idle); keep the wording in step when they change.
static const char *const k_help_gesture[UI_VIEW_MOUSE_HELP_ROWS] = {
    "Move one finger",      "Tap",
    "Hold still (0.5 s)",   "Tap with two fingers",
    "Slide two fingers",    "Double-tap, then drag",
    "Drag lock",            "Three or more fingers",
};
static const char *const k_help_result[UI_VIEW_MOUSE_HELP_ROWS] = {
    "Moves the pointer",
    "Left click",
    "Right click",
    "Right click",
    "Scrolls (up/down or sideways)",
    "Drags; lift the finger to drop",
    "Drag without holding; Release or 10 s idle drops",
    "Cancels: nothing is sent",
};

const char *ui_view_mouse_help_gesture(uint8_t row)
{
    return row < UI_VIEW_MOUSE_HELP_ROWS ? k_help_gesture[row] : NULL;
}

const char *ui_view_mouse_help_result(uint8_t row)
{
    return row < UI_VIEW_MOUSE_HELP_ROWS ? k_help_result[row] : NULL;
}

static bool host_mounted(const ui_view_mouse_t *m)
{
    return ui_store_get(m->store, UI_SUBJ_HOST_USB) == (int32_t)KVM_HOST_USB_MOUNTED;
}

static bool drag_lock_on(const ui_view_mouse_t *m)
{
    return ui_store_get(m->store, UI_SUBJ_DRAG_LOCK) == 1;
}

static uint64_t now(const ui_view_mouse_t *m)
{
    return m->now_us != NULL ? m->now_us() : 0u;
}

static bool submit(ui_view_mouse_t *m, ui_action_type_t type, uint16_t ctrl, uint16_t value)
{
    const bool ok = m->actions != NULL && ui_actions_submit(m->actions, type, ctrl, value, now(m), NULL) != 0u;
    if (!ok) {
        m->c.refused++;
    }
    return ok;
}

static uint8_t button_of(uint16_t control_id)
{
    switch (control_id) {
    case UI_CTRL_MOUSE_LEFT: return KVM_MOUSE_BTN_LEFT;
    case UI_CTRL_MOUSE_RIGHT: return KVM_MOUSE_BTN_RIGHT;
    default: return 0;
    }
}

// Edge sink of the Left / Right keys. ui_action_key pairs the edges (press, then exactly one of release / press
// lost / disable / delete); the view adds the domain side: an UP is owed only for a DOWN the sink accepted.
static void key_edge(uint16_t control_id, bool pressed, void *user_data)
{
    ui_view_mouse_t *m = user_data;
    const uint8_t btn = button_of(control_id);
    if (btn == 0) {
        return;
    }
    const uint8_t bit = (uint8_t)(1u << (btn - 1));
    if (pressed) {
        if ((m->held_mask & bit) == 0 && host_mounted(m) &&
            submit(m, UI_ACT_MOUSE_BUTTON_DOWN, control_id, btn)) {
            m->held_mask |= bit;
            m->c.button_down++;
        }
        return;
    }
    if ((m->held_mask & bit) == 0) {
        return;
    }
    m->held_mask &= (uint8_t)~bit;
    m->c.button_up++;
    if (m->leaving || !host_mounted(m)) {
        m->c.forced_releases++;
    }
    // Submitted even if the sink may refuse it: the HID core stays the safety owner (RELEASE_ALL on every loss);
    // the local flag must never outlive the press.
    (void)submit(m, UI_ACT_MOUSE_BUTTON_UP, control_id, btn);
}

static void drag_clicked(lv_event_t *e)
{
    ui_view_mouse_t *m = lv_event_get_user_data(e);
    if (!host_mounted(m)) {
        return;
    }
    // Ask for the opposite of the authoritative state; the key changes only when the gesture engine says so.
    m->c.drag_requests++;
    (void)submit(m, UI_ACT_DRAG_LOCK_REQUEST, UI_CTRL_MOUSE_DRAG_LOCK, drag_lock_on(m) ? 0u : 1u);
}

static void help_clicked(lv_event_t *e)
{
    ui_view_mouse_t *m = lv_event_get_user_data(e);
    // Local only: the router shows the card; nothing reaches the domain. Blocking, so the pad goes quiet meanwhile.
    if (m->router == NULL || !ui_overlay_show(m->router, UI_OVERLAY_GESTURE_HELP)) {
        m->c.help_refused++;
    }
}

static void help_close_clicked(lv_event_t *e)
{
    ui_view_mouse_t *m = lv_event_get_user_data(e);
    // 07 §3 as in the status cards: ask the shell first, fall back to the router.
    const uint32_t rid = m->actions != NULL ? ui_actions_submit(m->actions, UI_ACT_DISMISS_OVERLAY,
                                                                UI_CTRL_MOUSE_HELP_CLOSE, 0u, now(m), NULL)
                                            : 0u;
    if (rid == 0u && m->router != NULL) {
        ui_overlay_dismiss(m->router);
    }
}

static lv_obj_t *text(lv_obj_t *parent, const char *txt, ui_style_id_t style);
static lv_obj_t *plain(lv_obj_t *parent);

static bool help_build(void *ctx, lv_obj_t *card)
{
    ui_view_mouse_t *m = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    // As the status cards: the theme card instead of the default style, centering put back.
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, HELP_CARD_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim below
    lv_obj_center(card);

    (void)text(card, "Touchpad gestures", UI_STYLE_TEXT_HEADING);
    for (uint8_t i = 0; i < UI_VIEW_MOUSE_HELP_ROWS; i++) {
        lv_obj_t *line = plain(card);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, UI_SPACE_M, LV_PART_MAIN);
        (void)text(line, k_help_gesture[i], UI_STYLE_TEXT_LABEL);
        (void)text(line, k_help_result[i], UI_STYLE_TEXT_MUTED);
    }

    lv_obj_t *foot = plain(card);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(foot, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *close = lv_button_create(foot);
    lv_obj_remove_style_all(close);
    ui_theme_apply_button(close, UI_BTN_SECONDARY);
    lv_obj_set_size(close, HELP_CLOSE_W, UI_MODAL_BTN_H);
    lv_obj_set_scrollable(close, false);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text_static(label, "Close");
    lv_obj_center(label);
    lv_obj_add_event_cb(close, help_close_clicked, LV_EVENT_CLICKED, m);

    m->help_card = card;
    m->c.help_opens++;
    return true;
}

static void help_on_close(void *ctx)
{
    ui_view_mouse_t *m = ctx;
    m->help_card = NULL;
    m->c.help_closes++;
}

static void release_held(ui_view_mouse_t *m)
{
    m->leaving = true;
    // Disabling a held key makes ui_action_key emit its release edge (PRESS_LOST) before the key goes inert.
    if (m->btn_left != NULL && ui_action_key_is_down(m->btn_left)) {
        lv_obj_send_event(m->btn_left, LV_EVENT_PRESS_LOST, NULL);
    }
    if (m->btn_right != NULL && ui_action_key_is_down(m->btn_right)) {
        lv_obj_send_event(m->btn_right, LV_EVENT_PRESS_LOST, NULL);
    }
    m->leaving = false;
}

static void refresh(ui_view_mouse_t *m)
{
    if (m->root == NULL) {
        return;
    }
    const bool host = host_mounted(m);
    const bool drag = drag_lock_on(m);
    if (!host) {
        m->leaving = true; // a release caused by the HOST loss is a forced one
    }
    ui_action_key_set_enabled(m->btn_left, host);
    ui_action_key_set_enabled(m->btn_right, host);
    ui_action_key_set_enabled(m->btn_drag, host);
    m->leaving = false;
    ui_action_key_set_checked(m->btn_drag, drag);
    lv_label_set_text_static(m->drag_label, drag ? "Release" : "Drag lock"); // FR-107: a visible Release
    lv_obj_set_hidden(m->offline, host);
    // The invitation and the notice say opposite things: only one of them belongs on the pad at a time.
    lv_obj_set_hidden(m->idle_title, !host);
    lv_obj_set_hidden(m->idle_sub, !host);
    lv_label_set_text_static(m->live, drag ? "Dragging" : "Ready");
    lv_obj_set_style_text_color(m->live, lv_color_hex(drag ? UI_COLOR_ACCENT_INK : UI_COLOR_MUTED), LV_PART_MAIN);
    lv_label_set_text_static(m->hint, drag ? "Drag lock is on - move to drag." : "Tap to click. Hold for a right-click.");
}

static void subject_cb(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)subj;
    refresh(lv_observer_get_user_data(obs));
}

static lv_obj_t *text(lv_obj_t *parent, const char *txt, ui_style_id_t style)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_theme_apply(l, style, LV_STATE_DEFAULT);
    lv_label_set_text_static(l, txt);
    return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *create_cb(void *ctx, lv_obj_t *slot)
{
    ui_view_mouse_t *m = ctx;
    lv_obj_t *root = plain(slot);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_hor(root, UI_CONTENT_INSET, LV_PART_MAIN);
    lv_obj_set_style_pad_top(root, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(root, UI_CONTENT_INSET, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    // Touch surface: visual only (the bridge routes its touches to the gesture engine, 07 §6).
    lv_obj_t *pad = plain(root);
    ui_theme_apply(pad, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(pad, lv_color_hex(UI_COLOR_WELL), LV_PART_MAIN);
    lv_obj_set_width(pad, lv_pct(100));
    lv_obj_set_flex_grow(pad, 1);

    lv_obj_t *tag = text(pad, "TOUCH SURFACE", UI_STYLE_TEXT_MUTED);
    lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 0, 0);
    m->live = text(pad, "Ready", UI_STYLE_TEXT_LABEL);
    lv_obj_align(m->live, LV_ALIGN_TOP_RIGHT, 0, 0);
    m->idle_title = text(pad, "Just move.", UI_STYLE_TEXT_TITLE);
    lv_obj_align(m->idle_title, LV_ALIGN_CENTER, 0, -18);
    m->idle_sub = text(pad, "Move your finger to control the host.  Two fingers to scroll.", UI_STYLE_TEXT_MUTED);
    lv_obj_align(m->idle_sub, LV_ALIGN_CENTER, 0, 18);

    // The notice is sized by its own text and centered, so the "TOUCH SURFACE" / state line at the top of the
    // pad stays readable: a card the size of the pad's content area would start exactly on top of them.
    m->offline = plain(pad);
    ui_theme_apply(m->offline, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_size(m->offline, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(m->offline, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_row(m->offline, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(m->offline, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m->offline, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(m->offline);
    (void)text(m->offline, "Host disconnected", UI_STYLE_TEXT_HEADING);
    (void)text(m->offline, "Input will resume when the connection returns.", UI_STYLE_TEXT_MUTED);

    lv_obj_t *foot = plain(root);
    lv_obj_set_size(foot, lv_pct(100), FOOT_H);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, UI_SPACE_M, LV_PART_MAIN);

    // "?" sits outside the pad on purpose: a touch inside the pad rectangle goes to the gesture engine (a click on
    // the HOST), so the help key must not be on the surface it explains.
    m->btn_help = lv_button_create(foot);
    lv_obj_remove_style_all(m->btn_help);
    ui_theme_apply_button(m->btn_help, UI_BTN_SECONDARY);
    lv_obj_set_size(m->btn_help, HELP_W, FOOT_H);
    lv_obj_set_scrollable(m->btn_help, false);
    lv_obj_t *qm = lv_label_create(m->btn_help);
    ui_theme_apply(qm, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
    lv_label_set_text_static(qm, "?");
    lv_obj_center(qm);
    lv_obj_add_event_cb(m->btn_help, help_clicked, LV_EVENT_CLICKED, m);

    m->hint = text(foot, "", UI_STYLE_TEXT_MUTED);
    lv_obj_set_flex_grow(m->hint, 1);
    lv_label_set_long_mode(m->hint, LV_LABEL_LONG_MODE_DOTS);
    m->btn_left = ui_action_key_create(foot, &k_left, &m->sink);
    m->btn_right = ui_action_key_create(foot, &k_right, &m->sink);
    m->btn_drag = ui_action_key_create(foot, &k_drag, NULL);
    if (m->btn_left == NULL || m->btn_right == NULL || m->btn_drag == NULL) {
        lv_obj_delete(root);
        return NULL;
    }
    m->drag_label = lv_obj_get_child(m->btn_drag, (int32_t)lv_obj_get_child_count(m->btn_drag) - 1);
    lv_obj_add_event_cb(m->btn_drag, drag_clicked, LV_EVENT_CLICKED, m);

    m->root = root;
    m->pad = pad;
    // Observers die with their object (AT-UIFW-09): bound to the root, so deleting the view detaches them.
    lv_subject_add_observer_obj(ui_store_subject(m->store, UI_SUBJ_HOST_USB), subject_cb, root, m);
    lv_subject_add_observer_obj(ui_store_subject(m->store, UI_SUBJ_DRAG_LOCK), subject_cb, root, m);
    refresh(m);
    return root;
}

static void on_enter_cb(void *ctx)
{
    refresh(ctx);
}

static void on_leave_cb(void *ctx)
{
    release_held(ctx);
}

static void destroy_cb(void *ctx)
{
    ui_view_mouse_t *m = ctx;
    release_held(m);
    m->root = m->pad = m->live = m->offline = m->hint = NULL;
    m->idle_title = m->idle_sub = NULL;
    m->btn_left = m->btn_right = m->btn_drag = m->drag_label = m->btn_help = NULL;
}

static lv_obj_t *pad_obj_cb(void *ctx)
{
    ui_view_mouse_t *m = ctx;
    return m->pad;
}

void ui_view_mouse_init(ui_view_mouse_t *m, ui_store_t *store, ui_actions_t *actions, uint64_t (*now_us)(void))
{
    memset(m, 0, sizeof(*m));
    m->store = store;
    m->actions = actions;
    m->now_us = now_us;
    m->sink = (ui_action_key_sink_t){.fn = key_edge, .user_data = m};
    m->desc = (ui_view_descriptor_t){
        .id = UI_VIEW_MOUSE,
        .create = create_cb,
        .on_enter = on_enter_cb,
        .on_leave = on_leave_cb,
        .destroy = destroy_cb,
        .pad_obj = pad_obj_cb,
        .ctx = m,
    };
    m->ov[0] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_GESTURE_HELP,
        .create = help_build,
        .on_close = help_on_close,
        .blocking = true, // the pad must not take the taps meant for the card
        .ctx = m,
    };
}

void ui_view_mouse_bind_router(ui_view_mouse_t *m, ui_router_t *router)
{
    if (m != NULL) {
        m->router = router;
    }
}

const ui_overlay_descriptor_t *ui_view_mouse_overlays(ui_view_mouse_t *m, uint8_t *n)
{
    if (n != NULL) {
        *n = m != NULL ? UI_MOUSE_OVERLAYS : 0;
    }
    return m != NULL ? m->ov : NULL;
}
