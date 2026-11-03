// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// SHORTCUTS view (TASK-070C). See ui_view_shortcuts.h for the contract.
//
// Layout inside the content slot (1024 x 520 below the UI_TOPBAR_H bar, ADR-026): the reference commands-grid
// scaled up - three columns by two rows of command cards, then the FR-311 Media row, then the FR-744 footer with
// the "Switch HOST layout" tile. Every length comes from ui_tokens.h; every target is well above UI_TOUCH_COMFORT.
#include "ui_view_shortcuts.h"

#include <stdio.h>
#include <string.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "ui_action_key.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_tokens.h"

#define MEDIA_H 72
#define FOOT_H 64
#define LAYOUT_W 320
#define CHIP 40

// Control id of a tile. A cast keeps the arithmetic inside uint16_t for the strict build.
#define CTRL_OF(chord) ((uint16_t)(UI_CTRL_SHORTCUT_BASE + (unsigned)(chord)))

// Style selector of the pressed main part (LVGL's part and state enums are distinct types).
#define SEL_PRESSED ((lv_style_selector_t)LV_PART_MAIN | (lv_style_selector_t)LV_STATE_PRESSED)

typedef struct {
    const char *symbol; // chip glyph (LV_SYMBOL_*, present in every built-in Montserrat font)
    const char *name;
    const char *sub; // the chord written out, as in the reference
    uint16_t chord;  // kvm_chord_id_t of the compiled allowlist
    bool featured;   // the reference marks Ctrl+Alt+Del as the one dark tile
} cmd_def_t;

static const cmd_def_t k_cmd[UI_SHORTCUT_COMMAND_COUNT] = {
    [UI_SHORTCUT_SECURE_ATTENTION] = {LV_SYMBOL_WARNING, "Secure attention", "Ctrl + Alt + Del",
                                      KVM_CHORD_CTRL_ALT_DEL, true},
    [UI_SHORTCUT_SWITCH_WINDOW] = {LV_SYMBOL_LOOP, "Switch window", "Alt + Tab", KVM_CHORD_ALT_TAB, false},
    [UI_SHORTCUT_TASK_MANAGER] = {LV_SYMBOL_LIST, "Task manager", "Ctrl + Shift + Esc", KVM_CHORD_TASK_MANAGER,
                                  false},
    [UI_SHORTCUT_DESKTOP] = {LV_SYMBOL_HOME, "Desktop", "Win + D", KVM_CHORD_SHOW_DESKTOP, false},
    [UI_SHORTCUT_CLOSE_WINDOW] = {LV_SYMBOL_CLOSE, "Close window", "Alt + F4", KVM_CHORD_CLOSE_WINDOW, false},
    [UI_SHORTCUT_SCREENSHOT] = {LV_SYMBOL_IMAGE, "Screenshot", "Print Screen", KVM_CHORD_SCREENSHOT, false},
};

// Media tiles are UiActionKey widgets (TASK-060D) used for their touch target and feedback only: a shortcut is
// one click, not a down/up edge, so the sink is NULL and the view listens for LV_EVENT_CLICKED.
#define MEDIA_KEY(icon_, label_, chord_)                                                                      \
    {.label = (label_), .icon = (icon_), .control_id = CTRL_OF(chord_), .variant = UI_KEY_NORMAL, .width = 0, \
     .height = MEDIA_H, .font = UI_FONT_LABEL}

static const ui_action_key_desc_t k_media[UI_SHORTCUT_MEDIA_COUNT] = {
    [UI_SHORTCUT_MEDIA_PLAY_PAUSE] = MEDIA_KEY(LV_SYMBOL_PLAY, "Play", KVM_CHORD_MEDIA_PLAY_PAUSE),
    [UI_SHORTCUT_MEDIA_PREV] = MEDIA_KEY(LV_SYMBOL_PREV, "Previous", KVM_CHORD_MEDIA_PREV),
    [UI_SHORTCUT_MEDIA_NEXT] = MEDIA_KEY(LV_SYMBOL_NEXT, "Next", KVM_CHORD_MEDIA_NEXT),
    [UI_SHORTCUT_MEDIA_MUTE] = MEDIA_KEY(LV_SYMBOL_MUTE, "Mute", KVM_CHORD_MEDIA_MUTE),
    [UI_SHORTCUT_MEDIA_VOLUME_DOWN] = MEDIA_KEY(LV_SYMBOL_VOLUME_MID, "Volume -", KVM_CHORD_MEDIA_VOLUME_DOWN),
    [UI_SHORTCUT_MEDIA_VOLUME_UP] = MEDIA_KEY(LV_SYMBOL_VOLUME_MAX, "Volume +", KVM_CHORD_MEDIA_VOLUME_UP),
};

// FR-734 / ADR-023: the confirmation is a decision the user must make, so the scrim does not dismiss it.
static const ui_modal_button_t k_confirm_buttons[2] = {
    {.label = "Keep it open", .control_id = UI_CTRL_SHORTCUT_CANCEL, .variant = UI_BTN_SECONDARY, .closes = true},
    {.label = "Close window", .control_id = UI_CTRL_SHORTCUT_CONFIRM, .variant = UI_BTN_PRIMARY, .closes = true},
};

static const ui_modal_desc_t k_confirm_desc = {
    .title = "Close the active window?",
    .body = "Alt + F4 closes whatever window is in front on the host. Unsaved work there may be lost.",
    .buttons = k_confirm_buttons,
    .button_count = 2,
    .dismiss_on_scrim = false,
    .overlay_id = 0,
};

// ---- public helpers ------------------------------------------------------------------------

uint16_t ui_view_shortcuts_command_chord(ui_shortcut_command_t cmd)
{
    return (unsigned)cmd < UI_SHORTCUT_COMMAND_COUNT ? k_cmd[cmd].chord : 0u;
}

uint16_t ui_view_shortcuts_media_chord(ui_shortcut_media_t media)
{
    return (unsigned)media < UI_SHORTCUT_MEDIA_COUNT
               ? (uint16_t)(k_media[media].control_id - UI_CTRL_SHORTCUT_BASE)
               : 0u;
}

const char *ui_view_shortcuts_command_name(ui_shortcut_command_t cmd)
{
    return (unsigned)cmd < UI_SHORTCUT_COMMAND_COUNT ? k_cmd[cmd].name : NULL;
}

bool ui_view_shortcuts_input_allowed(const ui_view_shortcuts_t *s)
{
    if (s == NULL || s->store == NULL) {
        return false;
    }
    ui_store_t *st = s->store;
    // FR-308: no chord without HOST_MOUNTED. FR-006: none during maintenance. A REMOTE owner holds control, so
    // local input must not inject either. OWNER_NONE / UNKNOWN stay allowed: local input is what claims the owner.
    return ui_store_get(st, UI_SUBJ_HOST_USB) == (int32_t)KVM_HOST_USB_MOUNTED &&
           ui_store_get(st, UI_SUBJ_MAINTENANCE) != 1 &&
           ui_store_get(st, UI_SUBJ_OWNER_KIND) != (int32_t)OWNER_REMOTE;
}

// ---- toast and modal -----------------------------------------------------------------------

static lv_obj_t *layer(const ui_view_shortcuts_t *s, lv_obj_t *preferred)
{
    if (preferred != NULL) {
        return preferred;
    }
    return s->root != NULL ? lv_obj_get_screen(s->root) : NULL;
}

/**
 * The toast is bottom-centered on the shell's toast layer (07 §7 keeps it off the touchpad), which on this
 * screen is the band the footer already uses. Rather than wedge the pill between the static hint and the
 * layout tile, the view fades the hint out while a toast is up: both speak for the footer row, and only one
 * of them is live. A hint that explains why input is blocked never fades - no toast exists then. The label
 * keeps its box (opacity, not `hidden`), so the layout tile beside it does not move when a toast appears.
 */
static void show_hint_unless_toast(ui_view_shortcuts_t *s)
{
    if (s->hint != NULL) {
        const bool quiet = s->toast != NULL && ui_view_shortcuts_input_allowed(s);
        lv_obj_set_style_opa(s->hint, quiet ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    }
}

static void toast_deleted(lv_event_t *e)
{
    ui_view_shortcuts_t *s = lv_event_get_user_data(e);
    s->toast = NULL;
    show_hint_unless_toast(s);
}

// One toast at a time: the newest message replaces the previous one instead of stacking over the grid.
static void say(ui_view_shortcuts_t *s, const char *prefix, const char *name)
{
    lv_obj_t *parent = layer(s, s->toast_parent);
    if (parent == NULL) {
        return;
    }
    if (s->toast != NULL) {
        ui_toast_dismiss(s->toast); // its delete event clears s->toast
    }
    (void)snprintf(s->toast_text, sizeof(s->toast_text), "%s%s", prefix, name);
    s->toast = ui_toast_show(parent, s->toast_text, 0);
    if (s->toast != NULL) {
        lv_obj_add_event_cb(s->toast, toast_deleted, LV_EVENT_DELETE, s);
    }
    show_hint_unless_toast(s);
}

static uint64_t now(const ui_view_shortcuts_t *s)
{
    return s->now_us != NULL ? s->now_us() : 0u;
}

/**
 * The only path from a tile to the domain: one typed UI_ACT_SHORTCUT_REQUEST carrying a chord id. No usage, no
 * modifier bitmap, nothing inherited from the KEYBOARD screen (FR-209); a refusal is reported, never retried.
 */
static void submit_chord(ui_view_shortcuts_t *s, uint16_t chord, const char *name)
{
    if (!ui_view_shortcuts_input_allowed(s)) {
        s->c.blocked++;
        return;
    }
    const bool ok = s->actions != NULL &&
                    ui_actions_submit(s->actions, UI_ACT_SHORTCUT_REQUEST, CTRL_OF(chord), chord, now(s), NULL) != 0u;
    if (ok) {
        s->c.requests++;
    } else {
        s->c.refused++;
    }
    // AGENTS-UI §5: "Requested" is all the UI knows - it never claims the HOST executed the chord.
    say(s, ok ? "Requested: " : "Not sent: ", name);
}

static void modal_deleted(lv_event_t *e)
{
    ui_view_shortcuts_t *s = lv_event_get_user_data(e);
    s->modal = NULL;
    if (s->pending_chord != KVM_CHORD_NONE) {
        s->pending_chord = KVM_CHORD_NONE; // closed by the scrim, a view change or a teardown: nothing is sent
        s->c.confirms_closed++;
    }
}

static void modal_button(lv_obj_t *modal, uint16_t control_id, void *user_data)
{
    (void)modal;
    ui_view_shortcuts_t *s = user_data;
    const uint16_t chord = s->pending_chord;
    s->pending_chord = KVM_CHORD_NONE; // decided: modal_deleted must not count it again
    if (control_id == UI_CTRL_SHORTCUT_CONFIRM && chord != KVM_CHORD_NONE) {
        s->c.confirms_taken++;
        submit_chord(s, chord, k_cmd[UI_SHORTCUT_CLOSE_WINDOW].name);
    } else {
        s->c.confirms_closed++;
    }
}

/** Synchronous when the view is going away (no async delete may outlive the shell); async from a click. */
static void drop_confirmation(ui_view_shortcuts_t *s, bool sync)
{
    if (s->modal == NULL) {
        return;
    }
    if (sync) {
        lv_obj_delete(s->modal); // modal_deleted clears the pointer and counts the cancel
    } else {
        ui_modal_close(s->modal);
    }
}

static bool ask_confirmation(ui_view_shortcuts_t *s, uint16_t chord)
{
    lv_obj_t *parent = layer(s, s->modal_parent);
    if (parent == NULL || s->modal != NULL) {
        return false; // one confirmation at a time; a second tap while it is open changes nothing
    }
    s->modal = ui_modal_create(parent, &k_confirm_desc, &s->modal_sink);
    if (s->modal == NULL) {
        return false;
    }
    lv_obj_add_event_cb(s->modal, modal_deleted, LV_EVENT_DELETE, s);
    s->pending_chord = chord;
    s->c.confirms_opened++;
    return true;
}

// ---- tile callbacks ------------------------------------------------------------------------

static void card_clicked(lv_event_t *e)
{
    ui_view_shortcuts_t *s = lv_event_get_user_data(e);
    const cmd_def_t *d = lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (d == NULL) {
        return;
    }
    if (!ui_view_shortcuts_input_allowed(s)) {
        s->c.blocked++; // a disabled card should not even emit a click; counted in case it does
        return;
    }
    // FR-734 / ADR-023: the policy is read here and owned elsewhere (Settings). A confirmation sends nothing yet.
    if (d->chord == KVM_CHORD_CLOSE_WINDOW && s->confirm_alt_f4 != NULL && s->confirm_alt_f4(s->policy_ctx) &&
        ask_confirmation(s, d->chord)) {
        return;
    }
    submit_chord(s, d->chord, d->name);
}

static void media_clicked(lv_event_t *e)
{
    ui_view_shortcuts_t *s = lv_event_get_user_data(e);
    lv_obj_t *key = lv_event_get_current_target_obj(e);
    const uint16_t ctrl = ui_action_key_control_id(key);
    if (ctrl < UI_CTRL_SHORTCUT_BASE) {
        return;
    }
    const uint16_t chord = (uint16_t)(ctrl - UI_CTRL_SHORTCUT_BASE);
    const ui_action_key_desc_t *d = lv_obj_get_user_data(key);
    submit_chord(s, chord, d != NULL && d->label != NULL ? d->label : "Media");
}

static void layout_clicked(lv_event_t *e)
{
    ui_view_shortcuts_t *s = lv_event_get_user_data(e);
    // FR-744: the chord belongs to the HOST OS selected in Settings; the view only forwards what it was given.
    if (s->layout_chord > KVM_CHORD_NONE && s->layout_chord < KVM_CHORD_COUNT) {
        submit_chord(s, s->layout_chord, "Switch HOST layout");
    }
}

// ---- state projection ----------------------------------------------------------------------

static const char *layout_subtitle(uint16_t chord)
{
    switch (chord) {
    case KVM_CHORD_LAYOUT_WIN: return "Win + Space";
    case KVM_CHORD_LAYOUT_CTRL: return "Ctrl + Space";
    default: return "Not configured";
    }
}

static const char *hint_text(const ui_view_shortcuts_t *s)
{
    ui_store_t *st = s->store;
    if (ui_store_get(st, UI_SUBJ_MAINTENANCE) == 1) {
        return "Maintenance in progress - input is blocked.";
    }
    if (ui_store_get(st, UI_SUBJ_OWNER_KIND) == (int32_t)OWNER_REMOTE) {
        return "A remote client holds control - commands are unavailable.";
    }
    if (ui_store_get(st, UI_SUBJ_HOST_USB) != (int32_t)KVM_HOST_USB_MOUNTED) {
        return "Host disconnected - commands are unavailable.";
    }
    return "Each command sends one short, complete chord.";
}

static void enable_card(lv_obj_t *card, bool on)
{
    lv_obj_set_disabled(card, !on);
    lv_obj_set_clickable(card, on);
}

/**
 * Ink of a tile's own children. LV_STATE_DISABLED repaints the card (UI_STYLE_STATE_DISABLED), but LVGL hands
 * no state to child objects, and a label built with ui_theme_apply(UI_STYLE_TEXT_*) carries its own text color
 * - so without this the dark tile would keep white ink and the five plain tiles near-black ink, both on a pale
 * disabled surface. Disabled, every tile wears the one disabled palette; the pale surface alone is not a state.
 */
typedef struct {
    uint32_t name;
    uint32_t sub;
    uint32_t glyph;
    uint32_t chip;
} tile_ink_t;

static tile_ink_t tile_ink(bool featured, bool on)
{
    if (!on) {
        return (tile_ink_t){UI_COLOR_DISABLED_INK, UI_COLOR_DISABLED_INK, UI_COLOR_DISABLED_INK, UI_COLOR_WELL};
    }
    if (featured) {
        return (tile_ink_t){UI_COLOR_ON_INVERSE, UI_COLOR_SUBTLE, UI_COLOR_ON_INVERSE, UI_COLOR_CHIP_FEATURED};
    }
    return (tile_ink_t){UI_COLOR_INK, UI_COLOR_MUTED, UI_COLOR_INK, UI_COLOR_WELL};
}

static void paint_card(const ui_view_shortcuts_t *s, int i, bool on)
{
    if (s->card_name[i] == NULL) {
        return;
    }
    const tile_ink_t c = tile_ink(k_cmd[i].featured, on);
    lv_obj_set_style_text_color(s->card_name[i], lv_color_hex(c.name), LV_PART_MAIN);
    lv_obj_set_style_text_color(s->card_sub[i], lv_color_hex(c.sub), LV_PART_MAIN);
    lv_obj_set_style_text_color(s->card_glyph[i], lv_color_hex(c.glyph), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s->card_chip[i], lv_color_hex(c.chip), LV_PART_MAIN);
}

/** The FR-744 tile is a UiActionKey, but its two labels carry their own ink for the same reason. */
static void paint_layout_tile(const ui_view_shortcuts_t *s, bool on)
{
    if (s->layout_label == NULL) {
        return;
    }
    lv_obj_set_style_text_color(s->layout_label, lv_color_hex(on ? UI_COLOR_INK : UI_COLOR_DISABLED_INK),
                                LV_PART_MAIN);
    lv_obj_set_style_text_color(s->layout_sub, lv_color_hex(on ? UI_COLOR_MUTED : UI_COLOR_DISABLED_INK),
                                LV_PART_MAIN);
}

static void refresh(ui_view_shortcuts_t *s)
{
    if (s->root == NULL) {
        return;
    }
    const bool on = ui_view_shortcuts_input_allowed(s);
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        enable_card(s->card[i], on);
        paint_card(s, i, on);
    }
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        ui_action_key_set_enabled(s->media[i], on);
    }
    const bool layout_ok = s->layout_chord > KVM_CHORD_NONE && s->layout_chord < KVM_CHORD_COUNT;
    ui_action_key_set_enabled(s->layout_key, on && layout_ok);
    paint_layout_tile(s, on && layout_ok);
    lv_label_set_text_static(s->layout_sub, layout_subtitle(s->layout_chord));
    lv_label_set_text_static(s->hint, hint_text(s));
    show_hint_unless_toast(s);
    if (!on) {
        drop_confirmation(s, true); // the HOST or the owner is gone: the pending Alt+F4 is abandoned, not sent
    }
}

static void subject_cb(lv_observer_t *obs, lv_subject_t *subj)
{
    (void)subj;
    refresh(lv_observer_get_user_data(obs));
}

// ---- construction --------------------------------------------------------------------------

/** Drop every pointer into the view tree. The objects themselves belong to the root the router deletes. */
static void forget_objects(ui_view_shortcuts_t *s)
{
    s->root = s->grid = s->media_row = s->foot_row = NULL;
    s->layout_key = s->layout_label = s->layout_sub = s->hint = NULL;
    memset(s->card, 0, sizeof(s->card));
    memset(s->card_chip, 0, sizeof(s->card_chip));
    memset(s->card_glyph, 0, sizeof(s->card_glyph));
    memset(s->card_name, 0, sizeof(s->card_name));
    memset(s->card_sub, 0, sizeof(s->card_sub));
    memset(s->media, 0, sizeof(s->media));
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

static lv_obj_t *row(lv_obj_t *parent, int32_t height, int32_t gap)
{
    lv_obj_t *o = plain(parent);
    lv_obj_set_width(o, lv_pct(100));
    if (height > 0) {
        lv_obj_set_height(o, height);
    } else {
        lv_obj_set_flex_grow(o, 1);
    }
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, LV_PART_MAIN);
    return o;
}

static lv_obj_t *make_card(lv_obj_t *parent, int index, ui_view_shortcuts_t *s)
{
    const cmd_def_t *d = &k_cmd[index];
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    ui_theme_apply(card, UI_STYLE_STATE_PRESSED, LV_STATE_PRESSED);
    ui_theme_apply(card, UI_STYLE_STATE_DISABLED, LV_STATE_DISABLED);
    ui_theme_apply(card, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_user_data(card, (void *)d);
    lv_obj_set_height(card, lv_pct(100));
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(card, UI_SPACE_S, LV_PART_MAIN);

    lv_obj_t *chip = plain(card);
    lv_obj_set_size(chip, CHIP, CHIP);
    lv_obj_set_style_radius(chip, UI_RADIUS_M, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, lv_color_hex(d->featured ? 0x424249u : UI_COLOR_WELL), LV_PART_MAIN);
    lv_obj_t *glyph = lv_label_create(chip);
    lv_label_set_text_static(glyph, d->symbol);
    lv_obj_center(glyph);

    lv_obj_t *group = plain(card);
    lv_obj_set_width(group, lv_pct(100));
    lv_obj_set_height(group, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(group, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_t *name = text(group, d->name, UI_STYLE_TEXT_HEADING);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_t *sub = text(group, d->sub, UI_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(sub, lv_pct(100));

    if (d->featured) {
        // The reference's single dark tile. Only the colors change: the geometry and the touch target do not.
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_INVERSE), LV_PART_MAIN);
        lv_obj_set_style_border_color(card, lv_color_hex(UI_COLOR_INVERSE), LV_PART_MAIN);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x33343Au), SEL_PRESSED);
        lv_obj_set_style_text_color(card, lv_color_hex(UI_COLOR_ON_INVERSE), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_COLOR_PRESSED_QUIET), SEL_PRESSED);
    }
    s->card_chip[index] = chip;
    s->card_glyph[index] = glyph;
    s->card_name[index] = name;
    s->card_sub[index] = sub;
    paint_card(s, index, true); // the enabled palette; refresh() reconciles it with the real state
    lv_obj_add_event_cb(card, card_clicked, LV_EVENT_CLICKED, s);
    return card;
}

static lv_obj_t *make_layout_tile(lv_obj_t *parent, ui_view_shortcuts_t *s)
{
    lv_obj_t *key = lv_button_create(parent);
    lv_obj_remove_style_all(key);
    ui_theme_apply_key(key, UI_KEY_NORMAL);
    lv_obj_set_size(key, LAYOUT_W, FOOT_H);
    lv_obj_set_scrollable(key, false);
    lv_obj_set_flex_flow(key, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(key, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_font(key, ui_theme_font(UI_FONT_BODY), LV_PART_MAIN);
    lv_obj_t *glyph = lv_label_create(key);
    lv_label_set_text_static(glyph, LV_SYMBOL_KEYBOARD);
    s->layout_label = text(key, "Switch HOST layout", UI_STYLE_TEXT_BODY);
    s->layout_sub = text(key, layout_subtitle(s->layout_chord), UI_STYLE_TEXT_MUTED);
    lv_obj_add_event_cb(key, layout_clicked, LV_EVENT_CLICKED, s);
    return key;
}

static lv_obj_t *create_cb(void *ctx, lv_obj_t *slot)
{
    ui_view_shortcuts_t *s = ctx;
    lv_obj_t *root = plain(slot);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_hor(root, UI_CONTENT_INSET, LV_PART_MAIN);
    lv_obj_set_style_pad_top(root, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(root, UI_CONTENT_INSET, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    s->root = root;

    // Commands: three columns by two rows, every card sized by flex so nothing can leave the slot.
    s->grid = plain(root);
    lv_obj_set_width(s->grid, lv_pct(100));
    lv_obj_set_flex_grow(s->grid, 1);
    lv_obj_set_flex_flow(s->grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s->grid, UI_SPACE_M, LV_PART_MAIN);
    for (int r = 0; r < 2; r++) {
        lv_obj_t *line = row(s->grid, 0, UI_SPACE_M);
        for (int col = 0; col < 3; col++) {
            const int i = r * 3 + col;
            s->card[i] = make_card(line, i, s);
        }
    }

    s->media_row = row(root, MEDIA_H, UI_SPACE_M);
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        s->media[i] = ui_action_key_create(s->media_row, &k_media[i], NULL);
        if (s->media[i] == NULL) {
            s->root = NULL;
            forget_objects(s);
            lv_obj_delete(root);
            return NULL;
        }
        lv_obj_add_event_cb(s->media[i], media_clicked, LV_EVENT_CLICKED, s);
    }

    s->foot_row = row(root, FOOT_H, UI_SPACE_M);
    s->hint = text(s->foot_row, "", UI_STYLE_TEXT_MUTED);
    lv_label_set_long_mode(s->hint, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_flex_grow(s->hint, 1);
    s->layout_key = make_layout_tile(s->foot_row, s);

    // Observers die with their object (AT-UIFW-09): bound to the root, so deleting the view detaches them.
    lv_subject_add_observer_obj(ui_store_subject(s->store, UI_SUBJ_HOST_USB), subject_cb, root, s);
    lv_subject_add_observer_obj(ui_store_subject(s->store, UI_SUBJ_MAINTENANCE), subject_cb, root, s);
    lv_subject_add_observer_obj(ui_store_subject(s->store, UI_SUBJ_OWNER_KIND), subject_cb, root, s);
    refresh(s);
    return root;
}

static void on_enter_cb(void *ctx)
{
    refresh(ctx);
}

// AGENTS-UI §6: a mode switch cancels the pending decision; nothing is sent on the way out.
static void on_leave_cb(void *ctx)
{
    drop_confirmation(ctx, true);
}

static void destroy_cb(void *ctx)
{
    ui_view_shortcuts_t *s = ctx;
    drop_confirmation(s, true);
    if (s->toast != NULL) {
        ui_toast_dismiss(s->toast); // the toast lives outside the view root: it must not outlive the view
    }
    forget_objects(s);
}

void ui_view_shortcuts_init(ui_view_shortcuts_t *s, ui_store_t *store, ui_actions_t *actions,
                            uint64_t (*now_us)(void), uint16_t layout_chord, bool (*confirm_alt_f4)(void *ctx),
                            void *policy_ctx)
{
    memset(s, 0, sizeof(*s));
    s->store = store;
    s->actions = actions;
    s->now_us = now_us;
    s->layout_chord = layout_chord;
    s->confirm_alt_f4 = confirm_alt_f4;
    s->policy_ctx = policy_ctx;
    s->modal_sink = (ui_modal_sink_t){.fn = modal_button, .user_data = s};
    s->desc = (ui_view_descriptor_t){
        .id = UI_VIEW_SHORTCUTS,
        .create = create_cb,
        .on_enter = on_enter_cb,
        .on_leave = on_leave_cb,
        .destroy = destroy_cb,
        .pad_obj = NULL, // no touchpad on this screen: the gesture engine gets no rectangle here
        .ctx = s,
    };
}

void ui_view_shortcuts_dismiss_modal(ui_view_shortcuts_t *s)
{
    if (s != NULL) {
        drop_confirmation(s, false); // asynchronous: safe from a click handler and from a domain update
    }
}

void ui_view_shortcuts_set_layers(ui_view_shortcuts_t *s, lv_obj_t *modal_parent, lv_obj_t *toast_parent)
{
    if (modal_parent != NULL) {
        s->modal_parent = modal_parent;
    }
    if (toast_parent != NULL) {
        s->toast_parent = toast_parent;
    }
}
