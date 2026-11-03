// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UiModal (TASK-060D, 07 §7). Storage without an allocation of our own: the scrim's user data is
// the caller's sink, the card's user data is the caller's descriptor, each button's user data is
// its row in the descriptor table, and the pending-close flag is LV_STATE_USER_1 on the scrim.
//
// Tree: scrim > card > { title, body, button row > buttons }.
#include "ui_modal.h"

#include "ui_theme.h"

#define CHILD_CARD 0

lv_obj_t *ui_modal_card(lv_obj_t *modal)
{
    if (!modal || lv_obj_get_child_count(modal) == 0) {
        return NULL;
    }
    return lv_obj_get_child(modal, CHILD_CARD);
}

static const ui_modal_desc_t *desc_of(lv_obj_t *modal)
{
    lv_obj_t *card = ui_modal_card(modal);
    return card ? (const ui_modal_desc_t *)lv_obj_get_user_data(card) : NULL;
}

uint16_t ui_modal_overlay_id(lv_obj_t *modal)
{
    const ui_modal_desc_t *d = desc_of(modal);
    return d ? d->overlay_id : 0u;
}

bool ui_modal_is_closing(const lv_obj_t *modal)
{
    return modal && lv_obj_has_state(modal, LV_STATE_USER_1);
}

void ui_modal_close(lv_obj_t *modal)
{
    if (!modal || ui_modal_is_closing(modal)) {
        return; // idempotent: never queue two asynchronous deletes for the same overlay
    }
    lv_obj_set_state_user_1(modal, true);
    lv_obj_set_clickable(modal, false); // stop swallowing presses the moment the close is decided
    lv_obj_delete_async(modal);
}

static void scrim_clicked_cb(lv_event_t *e)
{
    ui_modal_close(lv_event_get_current_target_obj(e));
}

static void modal_button_cb(lv_event_t *e)
{
    lv_obj_t *modal = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    const ui_modal_button_t *row = lv_obj_get_user_data(btn);
    ui_modal_sink_t *sink = modal ? lv_obj_get_user_data(modal) : NULL;
    if (!row || ui_modal_is_closing(modal)) {
        return;
    }
    if (sink && sink->fn) {
        sink->fn(modal, row->control_id, sink->user_data);
    }
    if (row->closes) {
        ui_modal_close(modal);
    }
}

static void add_buttons(lv_obj_t *card, lv_obj_t *modal, const ui_modal_desc_t *desc)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(row, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (uint16_t i = 0; i < desc->button_count; i++) {
        lv_obj_t *btn = lv_button_create(row);
        lv_obj_remove_style_all(btn);
        ui_theme_apply_button(btn, desc->buttons[i].variant);
        lv_obj_set_user_data(btn, (void *)&desc->buttons[i]);
        lv_obj_set_height(btn, UI_MODAL_BTN_H);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, desc->buttons[i].label ? desc->buttons[i].label : "");
        lv_obj_center(label);
        lv_obj_add_event_cb(btn, modal_button_cb, LV_EVENT_CLICKED, modal);
    }
}

lv_obj_t *ui_modal_create(lv_obj_t *parent, const ui_modal_desc_t *desc, ui_modal_sink_t *sink)
{
    if (!parent || !desc || !ui_theme_is_ready()) {
        return NULL;
    }
    lv_obj_t *modal = lv_obj_create(parent);
    lv_obj_remove_style_all(modal);
    ui_theme_apply(modal, UI_STYLE_SCRIM, LV_STATE_DEFAULT);
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(modal, false);
    lv_obj_set_clickable(modal, true); // AT-UIFW-10: nothing below the scrim may be pressed
    lv_obj_set_user_data(modal, sink);

    lv_obj_t *card = lv_obj_create(modal);
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_user_data(card, (void *)desc);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim's dismiss
    lv_obj_set_width(card, UI_MODAL_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, UI_MODAL_MAX_H, LV_PART_MAIN);
    lv_obj_set_style_min_width(card, UI_MODAL_MIN_W, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(card);

    lv_obj_t *title = lv_label_create(card);
    ui_theme_apply(title, UI_STYLE_TEXT_HEADING, LV_STATE_DEFAULT);
    lv_label_set_text(title, desc->title ? desc->title : "");
    lv_obj_set_width(title, LV_PCT(100));

    lv_obj_t *body = lv_label_create(card);
    ui_theme_apply(body, UI_STYLE_TEXT_BODY, LV_STATE_DEFAULT);
    lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(body, desc->body ? desc->body : "");
    lv_obj_set_width(body, LV_PCT(100));

    if (desc->buttons && desc->button_count > 0) {
        add_buttons(card, modal, desc);
    }
    if (desc->dismiss_on_scrim) {
        lv_obj_add_event_cb(modal, scrim_clicked_cb, LV_EVENT_CLICKED, NULL);
    }
    return modal;
}
