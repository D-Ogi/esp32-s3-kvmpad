// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Settings of the DEVICE (TASK-070C). See ui_view_settings.h for the contract.
//
// Structure: the card is built once per open and never changes shape afterwards. Every section pane and
// every one of the UI_SETTINGS_CLIENTS_MAX client rows exists from the start and is hidden when it is not
// needed, so a section change and a publish both run through refresh() alone - no object is created or
// deleted while an event is being dispatched, and a section change allocates nothing (07 §5).
//
// refresh() is the single renderer: it reads `s->v` and writes labels and states, and it is the only thing a
// tap triggers locally. That is what makes "the switch does not move when you tap it" structural rather than
// a promise (AGENTS-UI MUST 6): there is no code path from a tap to a value on screen.
#include "ui_view_settings.h"

#include <stdio.h>
#include <string.h>

#include "kbd_layouts.h" // FR-741: the generated HOST layout tables the picker lists
#include "ui_icon_star.h" // ADR-039: the pinned / not pinned star of the picker
#include "ui_notices_generated.h"
#include "ui_theme.h"
#include "ui_tokens.h"

// ---- geometry ------------------------------------------------------------------------------
// Derived from ui_tokens.h: nothing here invents a color or a font size, and every tappable box is at least
// UI_TOUCH_MIN on both axes (07 §7, AT-UIFW-01).
#define CARD_W 920
#define CARD_H 560
#define INNER_W (CARD_W - 2 * UI_MODAL_PAD - 2 * UI_BORDER_W_BOLD) // the card border is inside the box too
#define HEADER_H UI_MODAL_BTN_H
#define BODY_H (CARD_H - 2 * UI_MODAL_PAD - HEADER_H - UI_SPACE_M)
#define TAB_W 220
#define TAB_H UI_TOUCH_COMFORT
#define PANE_W (INNER_W - TAB_W - UI_SPACE_L)
#define CLOSE_W 168
#define ROW_H 76
#define TOGGLE_W 132
#define TOGGLE_H UI_TOUCH_PREFERRED
#define APPROVAL_W 150
#define UNICODE_W 136 // "Windows hex" / "WinCompose" at the body font, with margin
#define PAIR_W 220
#define CLIENT_ROW_H UI_TOUCH_MIN
#define LANG_ROW_H UI_TOUCH_MIN // ADR-037: a compact row, still at the 44 px touch floor
#define CLIENT_BTN_W 120
#define STEP_W UI_TOUCH_PREFERRED
// ADR-039 picker geometry: the tiles, the chips, the preview, the star, the index strip and the search keys.
#define FAV_W 148
#define FAV_H 60
#define RECENT_W 96
#define PREVIEW_W 36
#define PREVIEW_H 36
#define STAR_W UI_TOUCH_MIN
#define INDEX_W UI_TOUCH_MIN
#define QKEY_W 52
#define CONFIRM_W UI_MODAL_W
#define CONFIRM_BTN_W 200

#define UNKNOWN_TEXT "Unknown"

// ---- wording -------------------------------------------------------------------------------
// ADR-023 FR-731: every setting shows a one-sentence description of its effect. The notes are deliberately
// short - a note that wraps past two lines at UI_FONT_LABEL would push the control out of its row.

static const char *const k_section_name[UI_SETTINGS_SEC_COUNT] = {
    "Remote", "Paired clients", "Shortcuts", "Input", "Keyboard", "About",
};

static const char *const k_section_icon[UI_SETTINGS_SEC_COUNT] = {
    LV_SYMBOL_WIFI, LV_SYMBOL_USB, LV_SYMBOL_LIST, LV_SYMBOL_GPS, LV_SYMBOL_KEYBOARD, LV_SYMBOL_SETTINGS,
};

static const char *const k_approval_label[UI_CONTROL_APPROVAL_COUNT] = {"Auto", "Trusted", "Always"};

static const char *const k_approval_note[UI_CONTROL_APPROVAL_COUNT] = {
    "Auto: every paired client takes control without asking.",
    "Trusted: clients marked trusted take control without asking; the rest ask.",
    "Always: every control request waits for a tap on this screen.",
};

static const char *const k_pair_note = "Opens a short window on this device and shows a 6-digit code.";
static const char *const k_pair_blocked_note = "Switch remote on first; pairing needs a Wi-Fi link.";
static const char *const k_clients_empty = "No client is paired with this device.";
static const char *const k_layout_note =
    "The layout database is not in this build: the host layout stays US and cannot be changed here.";
// FR-742: this device cannot read the HOST's setting, so it says what it draws and asks for the same on the HOST.
static const char *const k_layout_pick_note = "Set the same layout on the host.";
/**
 * ADR-039 section 5: the keys of the picker's own keyboard. Letters as on the KEYBOARD view so the finger goes
 * where it expects, then the digits and the four characters an xkeyboard-config id can hold. It types into this
 * device only and shares no code with the KEYBOARD view, which emits USB HID (AGENTS-UI MUST 4).
 */
static const char k_query_keys[UI_SETTINGS_QUERY_KEYS] = {
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p',
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '-',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', '_', '(', ')',
    ' ', '1',
};
/** The caption of each of those keys. Static storage: a button keeps the pointer it is given. */
static const char *const k_query_caps[UI_SETTINGS_QUERY_KEYS] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "a", "s", "d", "f", "g", "h", "j", "k", "l", "-", "z", "x", "c", "v", "b", "n", "m", "_", "(", ")", "space", "1",
};
/** ADR-039 section 7: the caption of each index bucket, in the order bucket_range() defines. */
static const char *const k_index_caps[UI_SETTINGS_PICK_INDEX] = {
    "A-C", "D-F", "G-I", "J-L", "M-O", "P-R", "S-U", "V-X", "Y-Z",
};
static const char *const k_unknown_note = "No settings have been published yet, so nothing can be changed.";

static const char *const k_unicode_name[UI_UNICODE_MODE_COUNT] = {
    "Host IME", "Linux", "macOS", "Windows hex", "WinCompose", // the hint below names the mechanism
};

// FR-747: the one-screen setup hint of each mode.
static const char *const k_unicode_hint[UI_UNICODE_MODE_COUNT] = {
    "Text composed here that the host layout cannot type is skipped; the host's own input method is used instead.",
    "Linux: GTK and IBus apps take Ctrl+Shift+U, the hex code and Space out of the box.",
    "macOS: add the \"Unicode Hex Input\" input source once and keep it selected while typing.",
    "Windows: set EnableHexNumpad = \"1\" under HKCU\\Control Panel\\Input Method once, then sign in again.",
    "WinCompose: install it on the host and keep its compose key on Right Alt.",
};

typedef struct {
    ui_setting_id_t id;
    const char *caption;
    const char *note;
} toggle_def_t;

static const toggle_def_t k_toggles[] = {
    {UI_SETTING_REMOTE_ENABLED, "Remote access",
     "Lets a paired client reach this device over the network. Off after every factory reset."},
    {UI_SETTING_PAIR_REQUIRE_ACCEPT, "Ask before pairing",
     "On: a correct code still waits for Accept on this screen, with the client name."},
    {UI_SETTING_CONFIRM_ALT_F4, "Confirm Close window",
     "On: Alt+F4 needs a second tap within 2 s before it reaches the host."},
    {UI_SETTING_KEEPAWAKE_ENABLED, "Keep awake",
     "Sends a 0 px mouse report while the host is idle, so it does not fall asleep."},
    // ADR-037: the three composed languages. Their rows are compact (one line each, no per-row note), so the
    // note of the block above them carries the one sentence FR-731 asks for.
    {UI_SETTING_LANG_KO, "Korean", NULL},
    {UI_SETTING_LANG_ZH, "Chinese (pinyin)", NULL},
    {UI_SETTING_LANG_JA, "Japanese", NULL},
};

#define TOGGLE_COUNT ((uint8_t)(sizeof(k_toggles) / sizeof(k_toggles[0])))

/** The three ADR-037 language rows, in the order the pane lists them and the language key cycles them. */
static const ui_setting_id_t k_lang_toggles[3] = {UI_SETTING_LANG_KO, UI_SETTING_LANG_ZH, UI_SETTING_LANG_JA};

static const char *const k_lang_note =
    "The language key on the keyboard cycles through English and the languages you turn on here.";

static const char *const k_setting_key[UI_SETTING_COUNT] = {
    "remote.enabled",      "pair.require_accept",      "control.approval", "shortcuts.confirm_alt_f4",
    "keepawake.enabled",   "keepawake.interval_s",     "client.trusted",   "client.forget",
    "kbd.unicode_mode",    "kbd.lang_ko",              "kbd.lang_zh",      "kbd.lang_ja",
    "kbd.host_layout",
};

const char *ui_setting_key(ui_setting_id_t id)
{
    return (unsigned)id < UI_SETTING_COUNT ? k_setting_key[id] : "unknown";
}

// ---- helpers -------------------------------------------------------------------------------

/**
 * Bounded, printable-only copy of a name that arrived over the network (SEC-105 / 07 §7): control bytes and
 * anything outside printable ASCII become '.', so a crafted name cannot reflow the card or hide characters.
 */
static void copy_printable(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    if (cap == 0u) {
        return;
    }
    for (; src != NULL && src[n] != '\0' && n + 1u < cap; n++) {
        const unsigned char ch = (unsigned char)src[n];
        dst[n] = (ch >= 0x20u && ch < 0x7Fu) ? (char)ch : '.';
    }
    dst[n] = '\0';
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
    lv_label_set_text_static(l, txt != NULL ? txt : "");
    return l;
}

/** Like text(), but the label owns a copy: LVGL refuses LV_LABEL_LONG_MODE_DOTS on a static string. */
static lv_obj_t *text_copy(lv_obj_t *parent, const char *txt, ui_style_id_t style)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_theme_apply(l, style, LV_STATE_DEFAULT);
    lv_label_set_text(l, txt != NULL ? txt : "");
    return l;
}

/** One line of its own font height: DOTS only clips when the label has a fixed height. */
static void one_line_dots(lv_obj_t *label)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
}

static lv_obj_t *wrapped(lv_obj_t *parent, const char *txt, ui_style_id_t style)
{
    lv_obj_t *l = text(parent, txt, style);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

/** A button with a centered legend. The legend object is child 0, so a refresh can recolor it. */
static lv_obj_t *button(lv_obj_t *parent, const char *label, int32_t w, int32_t h, ui_btn_variant_t variant,
                        lv_event_cb_t cb, void *ctx)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    ui_theme_apply_button(b, variant);
    ui_theme_apply(b, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(b, w, h);
    lv_obj_set_scrollable(b, false);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text_static(l, label);
    lv_obj_center(l);
    if (cb != NULL) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ctx);
    }
    return b;
}

/** Enable or disable a control and repaint its legend: LVGL hands no state to child objects. */
static void set_enabled(lv_obj_t *btn, bool on)
{
    if (btn == NULL) {
        return;
    }
    lv_obj_set_disabled(btn, !on);
    lv_obj_set_clickable(btn, on);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, lv_color_hex(on ? UI_COLOR_INK : UI_COLOR_DISABLED_INK),
                                    LV_PART_MAIN);
    }
}

/** ADR-039 section 3: row `r` shows the solid star when its layout is pinned, the hollow one when it is not. */
static void set_star(ui_view_settings_t *s, uint8_t r, bool pinned)
{
    if (s->pick_star_icon[r] == NULL) {
        return;
    }
    lv_image_set_src(s->pick_star_icon[r], pinned ? &ui_icon_star_filled : &ui_icon_star_hollow);
    // Alpha-only images take their color from the style, so the two states differ in weight and in tone.
    lv_obj_set_style_image_recolor(s->pick_star_icon[r],
                                   lv_color_hex(pinned ? UI_COLOR_ACCENT : UI_COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_image_recolor_opa(s->pick_star_icon[r], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_image_opa(s->pick_star_icon[r], pinned ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
}

static void set_checked(lv_obj_t *btn, bool checked)
{
    if (btn == NULL) {
        return;
    }
    if (checked) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(btn, LV_STATE_CHECKED);
    }
}

/** True when a control may act: a sink exists and the domain has published a record at least once. */
static bool controls_live(const ui_view_settings_t *s)
{
    return s->sink != NULL && s->v.seq != 0u;
}

/** The published value of a 0/1 setting. The one place that maps a toggle id onto the snapshot. */
static bool toggle_value(const ui_view_settings_t *s, ui_setting_id_t id, bool *known)
{
    if (known != NULL) {
        *known = true;
    }
    switch (id) {
    case UI_SETTING_REMOTE_ENABLED: return s->v.remote_enabled;
    case UI_SETTING_PAIR_REQUIRE_ACCEPT: return s->v.pair_require_accept;
    case UI_SETTING_CONFIRM_ALT_F4: return s->v.confirm_alt_f4;
    case UI_SETTING_KEEPAWAKE_ENABLED: return s->v.keepawake_enabled;
    case UI_SETTING_LANG_KO: return s->v.lang_ko;
    case UI_SETTING_LANG_ZH: return s->v.lang_zh;
    case UI_SETTING_LANG_JA: return s->v.lang_ja;
    default: break;
    }
    if (known != NULL) {
        *known = false; // not a 0/1 setting: the caller must not write it
    }
    return false;
}

static uint64_t now_of(const ui_view_settings_t *s)
{
    return s->now_us != NULL ? s->now_us() : 0u;
}

// ---- policy relaxation (ADR-023 FR-736) ----------------------------------------------------

bool ui_view_settings_relaxes_policy(const ui_view_settings_t *s, ui_setting_id_t id, int32_t value)
{
    if (s == NULL || s->v.seq == 0u) {
        return false; // nothing published: there is no policy to relax and no control is live anyway
    }
    switch (id) {
    case UI_SETTING_PAIR_REQUIRE_ACCEPT:
        // ON -> OFF drops the Accept screen of FR-725: the laxer value is 0.
        return s->v.pair_require_accept && value == 0;
    case UI_SETTING_CONTROL_APPROVAL:
        // Higher = stricter (kvm_control_approval_t), so any move down the scale is a relaxation. That
        // covers `always -> trusted` as well as `always -> auto`; only the direction matters.
        return value >= 0 && value < (int32_t)s->v.control_approval;
    default:
        return false;
    }
}

// ---- reporting -----------------------------------------------------------------------------

/** Hand one accepted change to the domain. The UI changes nothing: it re-renders the published snapshot. */
static void report(ui_view_settings_t *s, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    if (!controls_live(s) || (unsigned)id >= UI_SETTING_COUNT) {
        s->c.refused++;
        return;
    }
    s->c.reported++;
    s->sink(s->sink_ctx, id, value, client_index);
}

static void refresh(ui_view_settings_t *s);
static void render_picker(ui_view_settings_t *s);
static void picker_close(ui_view_settings_t *s);
static void picker_say(ui_view_settings_t *s, const char *note);

// ---- confirmation card ---------------------------------------------------------------------

static bool confirm_build(void *ctx, lv_obj_t *card);
static void confirm_closed(void *ctx);

/**
 * Ask for the second, explicit tap. `heading` and `body` are already formatted into s->confirm_text; the
 * change itself is held in s->pending and is reported only from the Yes button.
 */
static void ask_confirmation(ui_view_settings_t *s, ui_setting_id_t id, int32_t value, uint8_t client_index,
                            const char *body)
{
    s->pending.id = id;
    s->pending.value = value;
    s->pending.client_index = client_index;
    s->pending.valid = true;
    (void)snprintf(s->confirm_text, sizeof(s->confirm_text), "%s", body);
    if (!ui_overlay_show(s->router, UI_OVERLAY_SETTINGS_CONFIRM)) {
        // The stack refused: nothing is asked and nothing is written. Fail closed.
        s->pending.valid = false;
        s->c.refused++;
        return;
    }
    s->c.confirms_shown++;
}

static void confirm_yes_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (!s->pending.valid) {
        return;
    }
    const ui_setting_id_t id = s->pending.id;
    const int32_t value = s->pending.value;
    const uint8_t index = s->pending.client_index;
    s->pending.valid = false;
    s->c.confirms_yes++;
    ui_overlay_dismiss(s->router); // pops the confirmation only; the Settings card stays open
    report(s, id, value, index);
    refresh(s);
}

static void confirm_no_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    s->pending.valid = false;
    s->c.confirms_no++;
    ui_overlay_dismiss(s->router);
}

static bool confirm_build(void *ctx, lv_obj_t *card)
{
    ui_view_settings_t *s = ctx;
    if (!s->pending.valid || !ui_theme_is_ready()) {
        return false;
    }
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_width(card, CONFIRM_W);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim below
    lv_obj_center(card);

    const bool forget = s->pending.id == UI_SETTING_CLIENT_FORGET;
    s->confirm_heading = text(card, forget ? "Forget this client?" : "Relax a security setting?",
                              UI_STYLE_TEXT_HEADING);
    s->confirm_body = wrapped(card, s->confirm_text, UI_STYLE_TEXT_BODY);

    lv_obj_t *foot = plain(card);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s->confirm_no = button(foot, "Keep as it is", CONFIRM_BTN_W, UI_MODAL_BTN_H, UI_BTN_SECONDARY,
                           confirm_no_clicked, s);
    s->confirm_yes = button(foot, forget ? "Forget" : "Change it", CONFIRM_BTN_W, UI_MODAL_BTN_H,
                            UI_BTN_PRIMARY, confirm_yes_clicked, s);
    s->confirm_card = card;
    return true;
}

static void confirm_closed(void *ctx)
{
    ui_view_settings_t *s = ctx;
    s->confirm_card = NULL;
    s->confirm_heading = NULL;
    s->confirm_body = NULL;
    s->confirm_yes = NULL;
    s->confirm_no = NULL;
    s->pending.valid = false; // a dismiss from anywhere (shutdown, dismiss_all) writes nothing
}

// ---- taps on the Settings card --------------------------------------------------------------

static void close_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    s->c.close_taps++;
    // 07 §3: the card asks, the shell navigates. The router fallback keeps Close working in a shell with no
    // local action sink installed.
    const uint32_t rid = s->actions != NULL ? ui_actions_submit(s->actions, UI_ACT_DISMISS_OVERLAY,
                                                                UI_CTRL_SETTINGS_CLOSE, 0u, now_of(s), NULL)
                                            : 0u;
    if (rid == 0u && s->router != NULL) {
        ui_overlay_dismiss(s->router);
    }
}

static void section_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *tab = lv_event_get_current_target(e);
    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        if (s->tab[i] == tab) {
            s->c.section_taps++;
            (void)ui_view_settings_show_section(s, (ui_settings_section_t)i);
            return;
        }
    }
}

static void toggle_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    for (uint8_t i = 0; i < TOGGLE_COUNT; i++) {
        const ui_setting_id_t id = k_toggles[i].id;
        if (s->toggle[id].control != btn) {
            continue;
        }
        if (!controls_live(s)) {
            s->c.refused++;
            return;
        }
        bool is_toggle = false;
        const bool current = toggle_value(s, id, &is_toggle);
        if (!is_toggle) {
            s->c.refused++;
            return;
        }
        const int32_t want = current ? 0 : 1;
        if (ui_view_settings_relaxes_policy(s, id, want)) {
            ask_confirmation(s, id, want, 0u,
                             "Pairing will then finish on the correct code alone, without the Accept screen "
                             "and without showing you the client name.");
        } else {
            report(s, id, want, 0u);
        }
        refresh(s); // the control keeps the published value until the domain republishes (MUST 6)
        return;
    }
}

static void approval_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    for (uint8_t i = 0; i < UI_CONTROL_APPROVAL_COUNT; i++) {
        if (s->approval[i] != btn) {
            continue;
        }
        if (!controls_live(s)) {
            s->c.refused++;
            return;
        }
        if ((uint8_t)i == s->v.control_approval) {
            return; // already the active policy: nothing to write
        }
        if (ui_view_settings_relaxes_policy(s, UI_SETTING_CONTROL_APPROVAL, (int32_t)i)) {
            ask_confirmation(s, UI_SETTING_CONTROL_APPROVAL, (int32_t)i, 0u, k_approval_note[i]);
        } else {
            report(s, UI_SETTING_CONTROL_APPROVAL, (int32_t)i, 0u);
        }
        refresh(s);
        return;
    }
}

static void interval_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    const bool up = btn == s->interval_up;
    if (!controls_live(s)) {
        s->c.refused++;
        return;
    }
    uint32_t v = s->v.keepawake_interval_s;
    if (v < UI_SETTINGS_INTERVAL_MIN) {
        v = UI_SETTINGS_INTERVAL_MIN;
    } else if (v > UI_SETTINGS_INTERVAL_MAX) {
        v = UI_SETTINGS_INTERVAL_MAX;
    }
    // Clamped here as well as in config_store: a tap at the end of the range must report nothing rather
    // than an out-of-range value the record would reject (ADR-020: 10..300 s).
    if (up && v + UI_SETTINGS_INTERVAL_STEP <= UI_SETTINGS_INTERVAL_MAX) {
        v += UI_SETTINGS_INTERVAL_STEP;
    } else if (!up && v >= UI_SETTINGS_INTERVAL_MIN + UI_SETTINGS_INTERVAL_STEP) {
        v -= UI_SETTINGS_INTERVAL_STEP;
    } else {
        s->c.refused++;
        return;
    }
    report(s, UI_SETTING_KEEPAWAKE_INTERVAL_S, (int32_t)v, 0u);
    refresh(s);
}

static void unicode_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    const bool next = lv_event_get_current_target(e) == s->unicode_next;
    if (!controls_live(s)) {
        s->c.refused++;
        return;
    }
    uint32_t v = s->v.unicode_mode < UI_UNICODE_MODE_COUNT ? s->v.unicode_mode : UI_UNICODE_MODE_HOST_IME;
    v = next ? (v + 1u) % UI_UNICODE_MODE_COUNT : (v + UI_UNICODE_MODE_COUNT - 1u) % UI_UNICODE_MODE_COUNT;
    // No optimistic change: the label follows the next published record (07 §3).
    report(s, UI_SETTING_UNICODE_MODE, (int32_t)v, 0u);
    refresh(s);
}

static void pair_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (!s->v.remote_enabled || s->v.seq == 0u) {
        return; // SEC-106 fail closed; a disabled button should not emit a click at all
    }
    s->c.pair_taps++;
    // The pairing prompt is a blocking overlay of its own and the stack holds one decision at a time, so the
    // Settings card closes first. It opens no window and learns nothing about the outcome.
    const uint32_t rid = s->actions != NULL ? ui_actions_submit(s->actions, UI_ACT_DISMISS_OVERLAY,
                                                                UI_CTRL_SETTINGS_PAIR, 0u, now_of(s), NULL)
                                            : 0u;
    if (rid == 0u && s->router != NULL) {
        ui_overlay_dismiss(s->router);
    }
    if (s->pair_action != NULL) {
        s->pair_action(s->pair_ctx); // the card is gone from here on
    }
}

static void trusted_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        if (s->client_row[i].trusted_btn != btn) {
            continue;
        }
        if (!controls_live(s) || i >= s->v.client_count) {
            s->c.refused++;
            return;
        }
        // Marking a client trusted lets it take control without asking while `control.approval` is
        // `trusted`, so it is confirmed like any other relaxation; clearing the flag is stricter and is not.
        if (!s->v.client[i].trusted) {
            char body[sizeof(s->confirm_text)];
            copy_printable(s->name_text, sizeof(s->name_text), s->v.client[i].name);
            (void)snprintf(body, sizeof(body),
                           "While control approval is Trusted, %s will take control without asking.",
                           s->name_text);
            ask_confirmation(s, UI_SETTING_CLIENT_TRUSTED, 1, i, body);
        } else {
            report(s, UI_SETTING_CLIENT_TRUSTED, 0, i);
        }
        refresh(s);
        return;
    }
}

static void forget_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target(e);
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        if (s->client_row[i].forget_btn != btn) {
            continue;
        }
        if (!controls_live(s) || i >= s->v.client_count) {
            s->c.refused++;
            return;
        }
        char body[sizeof(s->confirm_text)];
        copy_printable(s->name_text, sizeof(s->name_text), s->v.client[i].name);
        (void)snprintf(body, sizeof(body), "%s will have to pair again before it can reach this device.",
                       s->name_text);
        ask_confirmation(s, UI_SETTING_CLIENT_FORGET, 0, i, body);
        refresh(s);
        return;
    }
}

// ---- rendering -----------------------------------------------------------------------------

static const char *onoff(const ui_view_settings_t *s, bool value)
{
    return s->v.seq == 0u ? UNKNOWN_TEXT : (value ? "On" : "Off");
}

static void refresh_toggle(ui_view_settings_t *s, ui_setting_id_t id, bool value)
{
    ui_settings_row_t *row = &s->toggle[id];
    if (row->control == NULL) {
        return;
    }
    lv_label_set_text_static(row->value, onoff(s, value));
    set_checked(row->control, s->v.seq != 0u && value);
    set_enabled(row->control, controls_live(s));
}

static void refresh_clients(ui_view_settings_t *s)
{
    if (s->clients_heading == NULL) {
        return;
    }
    const uint8_t n = s->v.client_count <= UI_SETTINGS_CLIENTS_MAX ? s->v.client_count : 0u;
    if (s->v.seq == 0u) {
        lv_label_set_text_static(s->clients_heading, "Paired clients: unknown");
    } else {
        lv_label_set_text_fmt(s->clients_heading, "Paired clients: %u of %u", (unsigned)n,
                              (unsigned)UI_SETTINGS_CLIENTS_MAX);
    }
    lv_obj_set_hidden(s->clients_empty, !(s->v.seq != 0u && n == 0u));
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        ui_settings_client_row_t *row = &s->client_row[i];
        const bool used = i < n;
        lv_obj_set_hidden(row->row, !used);
        if (!used) {
            continue;
        }
        char name[UI_SETTINGS_NAME_CAP];
        copy_printable(name, sizeof(name), s->v.client[i].name);
        lv_label_set_text(row->name, name); // a copy: the snapshot may be republished under us
        lv_label_set_text_static(row->trusted_label, s->v.client[i].trusted ? "Trusted" : "Not trusted");
        set_checked(row->trusted_btn, s->v.client[i].trusted);
        set_enabled(row->trusted_btn, controls_live(s));
        set_enabled(row->forget_btn, controls_live(s));
    }
}

static void refresh(ui_view_settings_t *s)
{
    if (s->card == NULL) {
        return;
    }
    for (uint8_t i = 0; i < TOGGLE_COUNT; i++) {
        refresh_toggle(s, k_toggles[i].id, toggle_value(s, k_toggles[i].id, NULL));
    }

    // FR-733: exactly one policy is marked, and nothing is marked while no record has been published.
    const bool known = s->v.seq != 0u;
    for (uint8_t i = 0; i < UI_CONTROL_APPROVAL_COUNT; i++) {
        set_checked(s->approval[i], known && (uint8_t)i == s->v.control_approval);
        set_enabled(s->approval[i], controls_live(s));
    }
    if (s->approval_note != NULL) {
        lv_label_set_text_static(s->approval_note,
                                 known && s->v.control_approval < UI_CONTROL_APPROVAL_COUNT
                                     ? k_approval_note[s->v.control_approval]
                                     : k_unknown_note);
    }

    if (s->pair_btn != NULL) {
        const bool on = known && s->v.remote_enabled && s->pair_action != NULL;
        set_enabled(s->pair_btn, on);
        lv_label_set_text_static(s->pair_note, on ? k_pair_note : k_pair_blocked_note);
    }

    if (s->interval_value != NULL) {
        if (!known) {
            lv_label_set_text_static(s->interval_value, UNKNOWN_TEXT);
        } else {
            lv_label_set_text_fmt(s->interval_value, "%u s", (unsigned)s->v.keepawake_interval_s);
        }
        set_enabled(s->interval_down, controls_live(s) && s->v.keepawake_interval_s >
                                                             UI_SETTINGS_INTERVAL_MIN);
        set_enabled(s->interval_up, controls_live(s) && s->v.keepawake_interval_s <
                                                            UI_SETTINGS_INTERVAL_MAX);
    }

    refresh_clients(s);

    if (s->layout_value != NULL) {
        char id[UI_SETTINGS_LAYOUT_CAP];
        copy_printable(id, sizeof(id), s->v.host_layout);
        lv_label_set_text(s->layout_value, (known && id[0] != '\0') ? id : UNKNOWN_TEXT);
        // A copy, not a static pointer: these two labels clip with DOTS, which LVGL refuses on static text.
        lv_label_set_text(s->unicode_value, known && s->v.unicode_mode < UI_UNICODE_MODE_COUNT
                                                ? k_unicode_name[s->v.unicode_mode]
                                                : UNKNOWN_TEXT);
        lv_label_set_text_static(s->unicode_hint, known && s->v.unicode_mode < UI_UNICODE_MODE_COUNT
                                                      ? k_unicode_hint[s->v.unicode_mode]
                                                      : k_unknown_note);
        set_enabled(s->unicode_prev, controls_live(s));
        set_enabled(s->unicode_next, controls_live(s));
        // FR-741: the name of the stored layout next to its id, so the row reads like the picker. An id the
        // tables do not have keeps its own text and gets no name invented for it.
        const kbd_layout_t *cur = (known && id[0] != '\0') ? kbd_layouts_find(id) : NULL;
        const bool pickable = s->v.layout_db_available && kbd_layouts_count_get() > 0u;
        if (cur != NULL) {
            snprintf(s->layout_btn_text, sizeof(s->layout_btn_text), "%s - %s", id, cur->name);
            lv_label_set_text(s->layout_value, s->layout_btn_text);
        }
        set_enabled(s->layout_btn, controls_live(s) && pickable);
        // Nothing to add when the picker is there: the row's own description already says what the layout does
        // and asks for the same one on the HOST. The note speaks only when the row cannot be used.
        lv_obj_set_hidden(s->layout_note, pickable);
        if (s->pick_open && !pickable) {
            picker_close(s); // the database went away under an open picker: fail closed, back to the pane
        } else if (s->pick_open) {
            render_picker(s); // ADR-039: the favorites, the recents and the check mark are snapshot state
        }
        lv_label_set_text_static(s->layout_note,
                                 pickable ? ""
                                          : (s->v.layout_db_available
                                                 ? "The layouts of this build are not available to this screen."
                                                 : k_layout_note));
        if (s->pick_open && !pickable) {
            picker_close(s); // the database went away under an open list: back to the rows, never a stale list
        } else if (s->pick_open) {
            render_picker(s); // the mark follows the record the domain published, not the tap
        }
    }

    if (s->version_value != NULL) {
        char version[UI_SETTINGS_VERSION_CAP];
        copy_printable(version, sizeof(version), s->v.firmware_version);
        lv_label_set_text(s->version_value, version[0] != '\0' ? version : UNKNOWN_TEXT);
    }
}

// ---- construction --------------------------------------------------------------------------

/** Caption and note on the left, the control on the right: one row shape for every setting. */
static lv_obj_t *setting_row(lv_obj_t *pane, const char *caption, const char *note, lv_obj_t **out_left)
{
    lv_obj_t *row = plain(pane);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SPACE_M, LV_PART_MAIN);

    lv_obj_t *left = plain(row);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1); // the note wraps inside the row instead of pushing the control out
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, UI_SPACE_XS, LV_PART_MAIN);
    (void)wrapped(left, caption, UI_STYLE_TEXT_BODY);
    (void)wrapped(left, note, UI_STYLE_TEXT_MUTED);
    *out_left = left;
    return row;
}

static lv_obj_t *new_pane(ui_view_settings_t *s, lv_obj_t *holder, ui_settings_section_t section, int32_t gap)
{
    lv_obj_t *pane = plain(holder);
    lv_obj_set_size(pane, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, gap, LV_PART_MAIN);
    s->pane[section] = pane;
    return pane;
}

static void add_toggle_row(ui_view_settings_t *s, lv_obj_t *pane, ui_setting_id_t id)
{
    const toggle_def_t *def = NULL;
    for (uint8_t i = 0; i < TOGGLE_COUNT; i++) {
        if (k_toggles[i].id == id) {
            def = &k_toggles[i];
        }
    }
    if (def == NULL) {
        return;
    }
    lv_obj_t *left = NULL;
    lv_obj_t *row = setting_row(pane, def->caption, def->note, &left);
    lv_obj_t *btn = button(row, "", TOGGLE_W, TOGGLE_H, UI_BTN_SECONDARY, toggle_clicked, s);
    s->toggle[id].row = row;
    s->toggle[id].control = btn;
    s->toggle[id].value = lv_obj_get_child(btn, 0);
}

static void build_remote_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *pane = new_pane(s, holder, UI_SETTINGS_SEC_REMOTE, UI_SPACE_S);
    add_toggle_row(s, pane, UI_SETTING_REMOTE_ENABLED);
    add_toggle_row(s, pane, UI_SETTING_PAIR_REQUIRE_ACCEPT);

    lv_obj_t *block = plain(pane);
    lv_obj_set_width(block, lv_pct(100));
    lv_obj_set_height(block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(block, UI_SPACE_XS, LV_PART_MAIN);
    (void)wrapped(block, "Control approval", UI_STYLE_TEXT_BODY);
    lv_obj_t *buttons = plain(block);
    lv_obj_set_width(buttons, lv_pct(100));
    lv_obj_set_height(buttons, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, UI_SPACE_S, LV_PART_MAIN);
    // Strictest first, so the laxer values are not the ones the thumb lands on.
    for (int i = (int)UI_CONTROL_APPROVAL_COUNT - 1; i >= 0; i--) {
        s->approval[i] = button(buttons, k_approval_label[i], APPROVAL_W, TOGGLE_H, UI_BTN_SECONDARY,
                                approval_clicked, s);
        s->approval_label[i] = lv_obj_get_child(s->approval[i], 0);
    }
    s->approval_note = wrapped(block, k_unknown_note, UI_STYLE_TEXT_MUTED);

    if (s->pair_action != NULL) {
        lv_obj_t *row = plain(pane);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, UI_SPACE_XS, LV_PART_MAIN);
        // Secondary, like the entry point on the CLIENT details card: set_enabled() repaints the legend in
        // ink, which is unreadable on the inverse surface of a primary button.
        s->pair_btn = button(row, "Pair a client", PAIR_W, UI_MODAL_BTN_H, UI_BTN_SECONDARY, pair_clicked, s);
        s->pair_note = wrapped(row, k_pair_blocked_note, UI_STYLE_TEXT_MUTED);
    }
}

static void build_clients_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *pane = new_pane(s, holder, UI_SETTINGS_SEC_CLIENTS, UI_SPACE_XS + 2);
    // Eight full rows are taller than the pane: the list scrolls instead of clipping the last client.
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pane, LV_SCROLLBAR_MODE_AUTO);
    s->clients_heading = text(pane, "Paired clients", UI_STYLE_TEXT_BODY);
    s->clients_empty = wrapped(pane, k_clients_empty, UI_STYLE_TEXT_MUTED);
    // Every row exists from the start and the unused ones are hidden: a client that is forgotten or paired
    // while the card is open never creates or deletes an object inside an event (07 §4).
    for (uint8_t i = 0; i < UI_SETTINGS_CLIENTS_MAX; i++) {
        ui_settings_client_row_t *row = &s->client_row[i];
        row->row = plain(pane);
        lv_obj_set_width(row->row, lv_pct(100));
        // Content height plus room for the button border: a row exactly as tall as its buttons clips their
        // top and bottom edges.
        lv_obj_set_height(row->row, CLIENT_ROW_H + 2 * UI_BORDER_W_BOLD);
        lv_obj_set_flex_flow(row->row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row->row, UI_SPACE_S, LV_PART_MAIN);
        row->name = text_copy(row->row, "", UI_STYLE_TEXT_BODY);
        lv_obj_set_flex_grow(row->name, 1);
        one_line_dots(row->name);
        row->trusted_btn = button(row->row, "Not trusted", CLIENT_BTN_W, CLIENT_ROW_H, UI_BTN_SECONDARY,
                                  trusted_clicked, s);
        row->trusted_label = lv_obj_get_child(row->trusted_btn, 0);
        row->forget_btn = button(row->row, LV_SYMBOL_TRASH " Forget", CLIENT_BTN_W, CLIENT_ROW_H,
                                 UI_BTN_SECONDARY, forget_clicked, s);
        lv_obj_set_hidden(row->row, true);
    }
}

static void build_shortcuts_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *pane = new_pane(s, holder, UI_SETTINGS_SEC_SHORTCUTS, UI_SPACE_S);
    add_toggle_row(s, pane, UI_SETTING_CONFIRM_ALT_F4);
}

static void build_input_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *pane = new_pane(s, holder, UI_SETTINGS_SEC_INPUT, UI_SPACE_S);
    add_toggle_row(s, pane, UI_SETTING_KEEPAWAKE_ENABLED);

    lv_obj_t *left = NULL;
    lv_obj_t *row = setting_row(pane, "Idle interval",
                                "How long the host must be idle before a keep-awake report. 10 to 300 s.",
                                &left);
    lv_obj_t *stepper = plain(row);
    lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stepper, UI_SPACE_S, LV_PART_MAIN);
    s->interval_down = button(stepper, LV_SYMBOL_MINUS, STEP_W, STEP_W, UI_BTN_SECONDARY, interval_clicked, s);
    s->interval_value = text(stepper, UNKNOWN_TEXT, UI_STYLE_TEXT_BODY);
    lv_obj_set_width(s->interval_value, 72);
    lv_obj_set_style_text_align(s->interval_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    s->interval_up = button(stepper, LV_SYMBOL_PLUS, STEP_W, STEP_W, UI_BTN_SECONDARY, interval_clicked, s);
}

// ---- HOST layout picker (FR-741, ADR-039) ---------------------------------------------------
// The generated tables are sorted by id in byte order, and a variant id is the layout id plus "(variant)", so
// every variant of a layout follows it immediately: "de", "de(T3)", "de(neo)", then "dk". The two list levels
// are therefore walked over the table itself - no index is built, nothing is allocated, and the picker holds
// one group number and one page offset.
//
// ADR-039 adds three more ways in, all over the same six row widgets and the same table walk: the favorites and
// the recents of the record (HOME), a substring search over the names and the ids (SEARCH), and a nine-bucket
// alphabetical strip that jumps the list to a letter (LIST). The UI stores nothing: a pick and a pin are
// reported as table indices, and the lists are redrawn from the snapshot the shell publishes back.

/** Length of the layout part of an id ("de(neo)" -> 2). */
static size_t base_len(const char *id)
{
    size_t n = 0;
    while (id[n] != '\0' && id[n] != '(') {
        n++;
    }
    return n;
}

/** Whether `i` starts a new layout group, i.e. its layout part differs from the entry before it. */
static bool group_starts_at(size_t i)
{
    const kbd_layout_t *cur = kbd_layouts_at(i);
    if (cur == NULL) {
        return false;
    }
    if (i == 0u) {
        return true;
    }
    const kbd_layout_t *prev = kbd_layouts_at(i - 1u);
    const size_t a = base_len(cur->id), b = base_len(prev->id);
    return a != b || strncmp(cur->id, prev->id, a) != 0;
}

static uint16_t group_count(void)
{
    uint16_t n = 0;
    for (size_t i = 0; i < kbd_layouts_count_get(); i++) {
        n = (uint16_t)(n + (group_starts_at(i) ? 1u : 0u));
    }
    return n;
}

/** Table index where group `g` starts, or the table size when there is no such group. */
static uint16_t group_first(uint16_t g)
{
    uint16_t seen = 0;
    for (size_t i = 0; i < kbd_layouts_count_get(); i++) {
        if (group_starts_at(i)) {
            if (seen == g) {
                return (uint16_t)i;
            }
            seen++;
        }
    }
    return (uint16_t)kbd_layouts_count_get();
}

static uint16_t group_size(uint16_t g)
{
    const uint16_t first = group_first(g);
    uint16_t n = 0;
    for (size_t i = first; i < kbd_layouts_count_get(); i++) {
        if (i > first && group_starts_at(i)) {
            break;
        }
        n++;
    }
    return n;
}

/** The group that holds table index `idx`, or 0. */
static uint16_t group_of_index(uint16_t idx)
{
    uint16_t g = 0;
    for (size_t i = 1u; i <= idx && i < kbd_layouts_count_get(); i++) {
        g = (uint16_t)(g + (group_starts_at(i) ? 1u : 0u));
    }
    return g;
}

// ---- search (ADR-039 section 5) --------------------------------------------------------------

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/** Case-insensitive substring: 0 = no match, 1 = at the start, 2 = anywhere else. */
static uint8_t find_ci(const char *hay, const char *needle)
{
    if (needle[0] == '\0') {
        return 1u;
    }
    for (size_t i = 0; hay[i] != '\0'; i++) {
        size_t j = 0;
        while (needle[j] != '\0' && lower(hay[i + j]) == lower(needle[j])) {
            j++;
        }
        if (needle[j] == '\0') {
            return i == 0u ? 1u : 2u;
        }
    }
    return 0u;
}

/**
 * How well entry `i` answers the query: 0 none, 1 the id starts with it, 2 the name starts with it,
 * 3 either contains it. The order of ADR-039 section 5.
 */
static uint8_t match_rank(const kbd_layout_t *lay, const char *q)
{
    if (lay == NULL) {
        return 0u;
    }
    const uint8_t in_id = find_ci(lay->id, q);
    const uint8_t in_name = find_ci(lay->name, q);
    if (in_id == 1u) {
        return 1u;
    }
    if (in_name == 1u) {
        return 2u;
    }
    return (in_id != 0u || in_name != 0u) ? 3u : 0u;
}

/**
 * The `n`-th search hit, walking the table once per rank so the three groups come out in order without any
 * sorting and without a result array. Returns the table size when there is no such hit.
 */
static uint16_t search_entry(const ui_view_settings_t *s, uint16_t n)
{
    const size_t total = kbd_layouts_count_get();
    uint16_t seen = 0;
    for (uint8_t rank = 1u; rank <= 3u; rank++) {
        for (size_t i = 0; i < total; i++) {
            if (match_rank(kbd_layouts_at(i), s->query) == rank) {
                if (seen == n) {
                    return (uint16_t)i;
                }
                seen++;
            }
        }
    }
    return (uint16_t)total;
}

static uint16_t search_total(const ui_view_settings_t *s)
{
    uint16_t n = 0;
    for (size_t i = 0; i < kbd_layouts_count_get(); i++) {
        n = (uint16_t)(n + (match_rank(kbd_layouts_at(i), s->query) != 0u ? 1u : 0u));
    }
    return n;
}

// ---- the open level --------------------------------------------------------------------------

/** Entries on the level that is open: the search hits, the variants of one group, or the groups. */
static uint16_t pick_total(const ui_view_settings_t *s)
{
    switch (s->pick_screen) {
    case UI_PICK_SEARCH: return search_total(s);
    case UI_PICK_VARIANTS: return group_size(s->pick_group);
    case UI_PICK_LIST: return group_count();
    case UI_PICK_HOME:
    default: return 0u;
    }
}

/** The table index of entry `n` of the open level. */
static uint16_t pick_entry(const ui_view_settings_t *s, uint16_t n)
{
    switch (s->pick_screen) {
    case UI_PICK_SEARCH: return search_entry(s, n);
    case UI_PICK_VARIANTS: return (uint16_t)(group_first(s->pick_group) + n);
    case UI_PICK_LIST: return group_first(n);
    case UI_PICK_HOME:
    default: return (uint16_t)kbd_layouts_count_get();
    }
}

// ---- favorites and recents (ADR-039 sections 3 and 4) ----------------------------------------

static bool is_favorite(const ui_view_settings_t *s, const char *id)
{
    for (uint8_t i = 0; i < s->v.host_fav_count && i < UI_SETTINGS_FAV_MAX; i++) {
        if (strcmp(s->v.host_fav[i], id) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * The recents worth drawing: an entry the tables no longer have is skipped, and so is one that is already a
 * favorite tile (ADR-039 section 4). Returns the layout or NULL, and never reorders: the shell owns the order.
 */
static const kbd_layout_t *recent_shown(const ui_view_settings_t *s, uint8_t slot)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < s->v.host_recent_count && i < UI_SETTINGS_RECENT_MAX; i++) {
        const kbd_layout_t *lay = kbd_layouts_find(s->v.host_recent[i]);
        if (lay == NULL || is_favorite(s, s->v.host_recent[i])) {
            continue;
        }
        if (seen == slot) {
            return lay;
        }
        seen++;
    }
    return NULL;
}

/** The favorite of tile `slot`, skipping ids the tables no longer have. NULL when the tile stays empty. */
static const kbd_layout_t *fav_shown(const ui_view_settings_t *s, uint8_t slot)
{
    uint8_t seen = 0;
    for (uint8_t i = 0; i < s->v.host_fav_count && i < UI_SETTINGS_FAV_MAX; i++) {
        const kbd_layout_t *lay = kbd_layouts_find(s->v.host_fav[i]);
        if (lay == NULL) {
            continue;
        }
        if (seen == slot) {
            return lay;
        }
        seen++;
    }
    return NULL;
}

// ---- the index strip (ADR-039 section 7) -----------------------------------------------------

/** First letter of bucket `b` and the letter after its last, so a bucket is [from, to). */
static void bucket_range(uint8_t b, char *from, char *to)
{
    // Nine buckets of three letters, the last one taking Y and Z.
    *from = (char)('A' + 3 * b);
    *to = (char)(*from + (b == UI_SETTINGS_PICK_INDEX - 1u ? 2 : 3));
}

/** The first group whose id starts inside bucket `b`, or group_count() when the bucket is empty. */
static uint16_t bucket_group(uint8_t b)
{
    char from = 'A', to = 'A';
    bucket_range(b, &from, &to);
    const uint16_t groups = group_count();
    for (uint16_t g = 0; g < groups; g++) {
        const kbd_layout_t *lay = kbd_layouts_at(group_first(g));
        if (lay == NULL) {
            continue;
        }
        const char c = (char)(lower(lay->id[0]) - 'a' + 'A');
        if (c >= from && c < to) {
            return g;
        }
    }
    return groups;
}

// ---- drawing ----------------------------------------------------------------------------------

/** The row text: the id, the English name, and a mark on the entry the record holds. */
static void pick_row_text(const ui_view_settings_t *s, uint16_t n, char *out, size_t cap)
{
    const kbd_layout_t *lay = kbd_layouts_at(pick_entry(s, n));
    if (lay == NULL) {
        out[0] = '\0';
        return;
    }
    const bool selected = strcmp(lay->id, s->v.host_layout) == 0;
    const uint16_t variants = s->pick_screen == UI_PICK_LIST ? group_size(n) : 1u;
    if (variants > 1u) {
        // The layout level says how many variants are behind the row, so a tap that opens a list is no surprise.
        snprintf(out, cap, "%s%s  %s  (%u variants)", selected ? LV_SYMBOL_OK " " : "", lay->id, lay->name,
                 (unsigned)variants);
    } else {
        snprintf(out, cap, "%s%s  %s", selected ? LV_SYMBOL_OK " " : "", lay->id, lay->name);
    }
}

/** The home-row preview of the current layout (ADR-039 section 2): AC01..AC12, base legends, inert. */
static void render_preview(ui_view_settings_t *s)
{
    const kbd_layout_t *cur = kbd_layouts_find(s->v.host_layout);
    for (uint8_t i = 0; i < UI_SETTINGS_PREVIEW_KEYS; i++) {
        const kbd_key_t *key = cur != NULL ? kbd_layouts_key(cur, (kbd_slot_t)(KBD_SLOT_AC01 + i)) : NULL;
        const char *legend = (key != NULL) ? kbd_layouts_legend(key, 0u) : "";
        // A legend no shipped font can draw would be a blank chip, which reads as a missing key.
        legend = ui_theme_drawable(lv_obj_get_style_text_font(s->pick_preview[i], LV_PART_MAIN), legend);
        snprintf(s->pick_preview_text[i], sizeof(s->pick_preview_text[i]), "%s", legend);
        lv_label_set_text(s->pick_preview[i], s->pick_preview_text[i]);
        lv_obj_set_hidden(s->pick_preview[i], s->pick_preview_text[i][0] == '\0');
    }
}

static void render_home(ui_view_settings_t *s)
{
    const kbd_layout_t *cur = kbd_layouts_find(s->v.host_layout);
    if (cur != NULL) {
        snprintf(s->pick_cur_text, sizeof(s->pick_cur_text), "%s - %s", cur->name, cur->id);
    } else {
        snprintf(s->pick_cur_text, sizeof(s->pick_cur_text), "%s - not in this build",
                 s->v.host_layout[0] != '\0' ? s->v.host_layout : UNKNOWN_TEXT);
    }
    lv_label_set_text(s->pick_cur_name, s->pick_cur_text);
    render_preview(s);

    uint8_t shown = 0;
    for (uint8_t i = 0; i < UI_SETTINGS_FAV_MAX; i++) {
        const kbd_layout_t *lay = fav_shown(s, i);
        if (lay != NULL) {
            snprintf(s->pick_fav_text[i], sizeof(s->pick_fav_text[i]), "%s\n%s", lay->id, lay->name);
            shown++;
        } else {
            s->pick_fav_text[i][0] = '\0';
        }
        lv_label_set_text(s->pick_fav_label[i], s->pick_fav_text[i]);
        lv_obj_set_hidden(s->pick_fav[i], lay == NULL);
    }
    lv_obj_set_hidden(s->pick_fav_empty, shown != 0u);

    uint8_t recents = 0;
    for (uint8_t i = 0; i < UI_SETTINGS_RECENT_MAX; i++) {
        const kbd_layout_t *lay = recent_shown(s, i);
        if (lay != NULL) {
            snprintf(s->pick_recent_text[i], sizeof(s->pick_recent_text[i]), "%s", lay->id);
            recents++;
        } else {
            s->pick_recent_text[i][0] = '\0';
        }
        lv_label_set_text(s->pick_recent_label[i], s->pick_recent_text[i]);
        lv_obj_set_hidden(s->pick_recent[i], lay == NULL);
    }
    lv_obj_set_hidden(s->pick_recent_empty, recents != 0u);
}

static void render_query(ui_view_settings_t *s)
{
    snprintf(s->query_shown, sizeof(s->query_shown), "Search: %s_",
             s->query_len > 0u ? s->query : "");
    lv_label_set_text(s->pick_query_label, s->query_shown);
}

/** Draw the open screen. Relabels, hides and shows; allocates nothing and creates no widget. */
/**
 * How many of the shared rows the pane has room for. The search keyboard, the index strip and whatever the
 * card's other sections do to the pane all change it, and a row that does not fit is not merely ugly: it is
 * laid out past its parent's edge, where it is drawn nowhere and no touch can reach it. So the page is as
 * long as the room, and never longer (ADR-039 section 6).
 */
static uint8_t rows_fit(ui_view_settings_t *s)
{
    lv_obj_update_layout(s->pick_box);
    const int32_t room = lv_obj_get_content_height(s->pick_rows_box);
    const int32_t each = UI_TOUCH_MIN + UI_SPACE_XS;
    int32_t fit = each > 0 ? (room + UI_SPACE_XS) / each : 1;
    if (fit < 1) {
        fit = 1;
    }
    if (fit > UI_SETTINGS_PICK_ROWS) {
        fit = UI_SETTINGS_PICK_ROWS;
    }
    s->pick_rows_fit = (uint8_t)fit;
    return s->pick_rows_fit;
}

static void render_picker(ui_view_settings_t *s)
{
    if (s->pick_box == NULL) {
        return;
    }
    lv_obj_set_hidden(s->pick_box, !s->pick_open);
    if (s->kbd_main != NULL) {
        lv_obj_set_hidden(s->kbd_main, s->pick_open);
    }
    if (!s->pick_open) {
        return;
    }
    const bool home = s->pick_screen == UI_PICK_HOME;
    const bool search = s->pick_screen == UI_PICK_SEARCH;
    const bool list = s->pick_screen == UI_PICK_LIST;
    lv_obj_set_hidden(s->pick_home, !home);
    // The rows take the height they are given, so they have to leave the screen the home needs: a container
    // that is merely empty still eats the space, and its children then sit outside the home's clip - drawn
    // nowhere and, worse, out of reach of the touch search.
    lv_obj_set_hidden(s->pick_list_box, home);
    lv_obj_set_hidden(s->pick_search_box, !search);
    lv_obj_set_hidden(s->pick_index_box, !list);
    lv_obj_set_hidden(s->pick_foot, home);
    lv_label_set_text_static(s->pick_note, s->pick_note_text);
    lv_obj_set_hidden(s->pick_note, s->pick_note_text[0] == '\0');

    if (home) {
        snprintf(s->pick_title_text, sizeof(s->pick_title_text), "Host layout");
        lv_label_set_text(s->pick_title, s->pick_title_text);
        render_home(s);
        for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
            s->pick_text[r][0] = '\0';
            lv_label_set_text(s->pick_label[r], s->pick_text[r]);
            lv_obj_set_hidden(s->pick_row[r], true);
        lv_obj_set_hidden(lv_obj_get_parent(s->pick_row[r]), true);
        }
        return;
    }

    const uint16_t total = pick_total(s);
    const uint8_t fit = rows_fit(s);
    if (s->pick_page >= total) {
        s->pick_page = 0u;
    }
    if (search) {
        render_query(s);
        snprintf(s->pick_title_text, sizeof(s->pick_title_text), "Search - %u found", (unsigned)total);
    } else if (s->pick_screen == UI_PICK_VARIANTS) {
        const kbd_layout_t *lay = kbd_layouts_at(group_first(s->pick_group));
        snprintf(s->pick_title_text, sizeof(s->pick_title_text), "%s - variant",
                 lay != NULL ? lay->name : "Layout");
    } else {
        snprintf(s->pick_title_text, sizeof(s->pick_title_text), "All layouts - %u", (unsigned)total);
    }
    lv_label_set_text(s->pick_title, s->pick_title_text);

    for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
        const uint16_t n = (uint16_t)(s->pick_page + r);
        if (r >= fit || n >= total) {
            s->pick_text[r][0] = '\0';
            lv_label_set_text(s->pick_label[r], s->pick_text[r]);
            lv_obj_set_hidden(s->pick_row[r], true);
        lv_obj_set_hidden(lv_obj_get_parent(s->pick_row[r]), true);
            continue;
        }
        pick_row_text(s, n, s->pick_text[r], sizeof(s->pick_text[r]));
        lv_label_set_text(s->pick_label[r], s->pick_text[r]);
        lv_obj_set_hidden(s->pick_row[r], false);
        lv_obj_set_hidden(lv_obj_get_parent(s->pick_row[r]), false);
        // Solid where the layout is pinned, hollow where it is not, so a glance at the list says what is
        // pinned without reading anything.
        const kbd_layout_t *lay = kbd_layouts_at(pick_entry(s, n));
        set_star(s, r, lay != NULL && is_favorite(s, lay->id));
    }
    for (uint8_t b = 0; b < UI_SETTINGS_PICK_INDEX; b++) {
        set_enabled(s->pick_index[b], list && bucket_group(b) < group_count());
    }

    const unsigned page = (unsigned)(s->pick_page / fit + 1u);
    const unsigned pages = (unsigned)((total + fit - 1u) / fit);
    snprintf(s->pick_page_text, sizeof(s->pick_page_text), "%u / %u", page, pages == 0u ? 1u : pages);
    lv_label_set_text_static(s->pick_page_label, s->pick_page_text);
    set_enabled(s->pick_prev, s->pick_page > 0u);
    set_enabled(s->pick_next, (uint32_t)s->pick_page + fit < total);
}

/** Say one line under the screen, or nothing. The text is static storage, never a format of user input. */
static void picker_say(ui_view_settings_t *s, const char *note)
{
    snprintf(s->pick_note_text, sizeof(s->pick_note_text), "%s", note != NULL ? note : "");
}

static void picker_show(ui_view_settings_t *s, ui_pick_screen_t screen)
{
    s->pick_screen = screen;
    s->pick_page = 0u;
    render_picker(s);
}

/** Open the picker on HOME: the layout in force, what is pinned, and what was used last (ADR-039). */
static void picker_open(ui_view_settings_t *s)
{
    s->pick_open = true;
    s->query[0] = '\0';
    s->query_len = 0u;
    picker_say(s, "");
    picker_show(s, UI_PICK_HOME);
}

/** Open the full list at the group that holds the stored id, so the list starts where the user already is. */
static void picker_open_list(ui_view_settings_t *s)
{
    s->pick_screen = UI_PICK_LIST;
    const kbd_layout_t *cur = kbd_layouts_find(s->v.host_layout);
    uint16_t at = 0u;
    if (cur != NULL) {
        at = group_of_index((uint16_t)(cur - kbd_layouts_at(0)));
    }
    const uint8_t fit = s->pick_rows_fit > 0u ? s->pick_rows_fit : 1u;
    s->pick_page = (uint16_t)(at - at % fit);
    render_picker(s);
}

static void picker_close(ui_view_settings_t *s)
{
    s->pick_open = false;
    s->pick_screen = UI_PICK_HOME;
    s->pick_page = 0u;
    s->query[0] = '\0';
    s->query_len = 0u;
    picker_say(s, "");
    render_picker(s);
}

/** Apply entry `idx` of the tables: report it and go back to the pane. The record decides what is shown. */
static void picker_apply(ui_view_settings_t *s, uint16_t idx)
{
    if (idx >= kbd_layouts_count_get()) {
        return;
    }
    // A pick is reported and nothing on screen moves until the domain republishes the record it wrote
    // (AGENTS-UI MUST 5/6): the id drawn below is still the one the record holds.
    report(s, UI_SETTING_HOST_LAYOUT, (int32_t)idx, 0u);
    picker_close(s);
}

static void layout_open_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (s == NULL || s->card == NULL) {
        return;
    }
    if (!controls_live(s) || !s->v.layout_db_available || kbd_layouts_count_get() == 0u) {
        s->c.refused++; // nothing published, or no tables: the row stays read-only (07 §12, fail closed)
        return;
    }
    picker_open(s);
}

static void layout_back_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    picker_say(s, "");
    switch (s->pick_screen) {
    case UI_PICK_VARIANTS:
        // Back to the layout list, not out of the picker - and to the page it was left on, so a look into
        // a group and back does not send the user to the other end of the alphabet.
        s->pick_screen = UI_PICK_LIST;
        s->pick_page = s->pick_list_page;
        render_picker(s);
        break;
    case UI_PICK_LIST:
    case UI_PICK_SEARCH:
        picker_show(s, UI_PICK_HOME);
        break;
    case UI_PICK_HOME:
    default:
        picker_close(s);
        break;
    }
}

static void layout_step_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    const uint16_t total = pick_total(s);
    const uint8_t fit = s->pick_rows_fit > 0u ? s->pick_rows_fit : 1u;
    if (btn == s->pick_next && (uint32_t)s->pick_page + fit < total) {
        s->pick_page = (uint16_t)(s->pick_page + fit);
    } else if (btn == s->pick_prev && s->pick_page >= fit) {
        s->pick_page = (uint16_t)(s->pick_page - fit);
    } else {
        return;
    }
    render_picker(s);
}

/** Which row a control belongs to, or UI_SETTINGS_PICK_ROWS when it is none of them. */
static uint8_t row_of(lv_obj_t *const *set, const lv_obj_t *btn)
{
    for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
        if (set[r] == btn) {
            return r;
        }
    }
    return UI_SETTINGS_PICK_ROWS;
}

static void layout_row_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    const uint8_t row = row_of(s->pick_row, btn);
    const uint16_t n = (uint16_t)(s->pick_page + row);
    if (row >= UI_SETTINGS_PICK_ROWS || n >= pick_total(s)) {
        return;
    }
    picker_say(s, "");
    if (s->pick_screen == UI_PICK_LIST && group_size(n) > 1u) {
        s->pick_group = n; // second level: this layout has variants, so pick one of them
        s->pick_list_page = s->pick_page;
        picker_show(s, UI_PICK_VARIANTS);
        return;
    }
    picker_apply(s, pick_entry(s, n));
}

/** The star of a row, and the long press on a tile: both ask the shell to pin or unpin one layout. */
static void layout_pin(ui_view_settings_t *s, const kbd_layout_t *lay)
{
    if (lay == NULL) {
        return;
    }
    if (!is_favorite(s, lay->id) && s->v.host_fav_count >= UI_SETTINGS_FAV_MAX) {
        // ADR-039 section 3: the device never throws away a favorite the user pinned by hand.
        picker_say(s, "8 favorites is the limit - unpin one first.");
        s->c.refused++;
        render_picker(s);
        return;
    }
    picker_say(s, "");
    report(s, UI_SETTING_HOST_FAV_TOGGLE, (int32_t)(lay - kbd_layouts_at(0)), 0u);
    render_picker(s);
}

static void layout_star_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    const uint8_t row = row_of(s->pick_star, btn);
    const uint16_t n = (uint16_t)(s->pick_page + row);
    if (row >= UI_SETTINGS_PICK_ROWS || n >= pick_total(s)) {
        return;
    }
    layout_pin(s, kbd_layouts_at(pick_entry(s, n)));
}

static void layout_fav_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    for (uint8_t i = 0; i < UI_SETTINGS_FAV_MAX; i++) {
        if (s->pick_fav[i] != btn) {
            continue;
        }
        const kbd_layout_t *lay = fav_shown(s, i);
        if (lay == NULL) {
            return;
        }
        // A tap applies, a long press unpins (ADR-039 section 3). LVGL sends LONG_PRESSED before the click,
        // so the flag the long press set tells the click to stay out of the way.
        if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
            s->pick_long_press = true;
            layout_pin(s, lay);
        } else if (s->pick_long_press) {
            s->pick_long_press = false;
        } else {
            picker_apply(s, (uint16_t)(lay - kbd_layouts_at(0)));
        }
        return;
    }
}

static void layout_recent_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    for (uint8_t i = 0; i < UI_SETTINGS_RECENT_MAX; i++) {
        if (s->pick_recent[i] == btn) {
            const kbd_layout_t *lay = recent_shown(s, i);
            if (lay != NULL) {
                picker_apply(s, (uint16_t)(lay - kbd_layouts_at(0)));
            }
            return;
        }
    }
}

static void layout_index_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open || s->pick_screen != UI_PICK_LIST) {
        return;
    }
    for (uint8_t b = 0; b < UI_SETTINGS_PICK_INDEX; b++) {
        if (s->pick_index[b] != btn) {
            continue;
        }
        const uint16_t g = bucket_group(b);
        if (g >= group_count()) {
            return; // an empty bucket is disabled, so this is only reachable from a test
        }
        // The bucket's first layout becomes the first row: rounding down to a page boundary would open the
        // jump on the letter before it, which is not what the finger asked for.
        s->pick_page = g;
        render_picker(s);
        return;
    }
}

static void layout_search_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    s->query[0] = '\0';
    s->query_len = 0u;
    picker_say(s, "");
    picker_show(s, UI_PICK_SEARCH);
}

static void layout_all_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    if (s == NULL || !s->pick_open) {
        return;
    }
    picker_say(s, "");
    picker_open_list(s);
}

static void layout_key_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open || s->pick_screen != UI_PICK_SEARCH) {
        return;
    }
    for (uint8_t i = 0; i < UI_SETTINGS_QUERY_KEYS; i++) {
        if (s->pick_key[i] != btn) {
            continue;
        }
        const char c = k_query_keys[i];
        if (s->query_len + 1u < sizeof(s->query)) {
            s->query[s->query_len++] = c;
            s->query[s->query_len] = '\0';
        }
        s->pick_page = 0u;
        render_picker(s);
        return;
    }
}

static void layout_erase_clicked(lv_event_t *e)
{
    ui_view_settings_t *s = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_current_target_obj(e);
    if (s == NULL || !s->pick_open || s->pick_screen != UI_PICK_SEARCH) {
        return;
    }
    if (btn == s->pick_clear || s->query_len == 0u) {
        s->query[0] = '\0';
        s->query_len = 0u;
    } else {
        s->query[--s->query_len] = '\0';
    }
    s->pick_page = 0u;
    render_picker(s);
}

/** A compact language row: the name on the left, its toggle on the right. One line, no per-row note. */
static void add_lang_row(ui_view_settings_t *s, lv_obj_t *parent, ui_setting_id_t id, const char *caption)
{
    lv_obj_t *row = plain(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LANG_ROW_H);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_t *name = text(row, caption, UI_STYLE_TEXT_BODY);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_t *btn = button(row, "", TOGGLE_W, LANG_ROW_H, UI_BTN_SECONDARY, toggle_clicked, s);
    s->toggle[id].row = row;
    s->toggle[id].control = btn;
    s->toggle[id].value = lv_obj_get_child(btn, 0);
}

static void build_keyboard_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *outer = new_pane(s, holder, UI_SETTINGS_SEC_KEYBOARD, UI_SPACE_S);
    // Two children, one of them visible: the ordinary rows, and the FR-741 layout picker. The rows are taller
    // than BODY_H, so that container scrolls, like the Paired clients pane, instead of clipping the last row.
    lv_obj_t *pane = plain(outer);
    lv_obj_set_width(pane, lv_pct(100));
    lv_obj_set_height(pane, lv_pct(100));
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pane, LV_SCROLLBAR_MODE_AUTO);
    s->kbd_main = pane;

    // ADR-037: the languages the KEYBOARD view's language key offers, English first and not switchable.
    lv_obj_t *langs = plain(pane);
    lv_obj_set_width(langs, lv_pct(100));
    lv_obj_set_height(langs, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(langs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(langs, UI_SPACE_XS, LV_PART_MAIN);
    (void)wrapped(langs, "Input languages", UI_STYLE_TEXT_BODY);
    (void)wrapped(langs, k_lang_note, UI_STYLE_TEXT_MUTED);

    lv_obj_t *en = plain(langs);
    lv_obj_set_width(en, lv_pct(100));
    lv_obj_set_height(en, LANG_ROW_H);
    lv_obj_set_flex_flow(en, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(en, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(en, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_t *en_name = text(en, "English", UI_STYLE_TEXT_BODY);
    lv_obj_set_flex_grow(en_name, 1);
    lv_obj_t *en_value = text(en, "Always on", UI_STYLE_TEXT_MUTED);
    lv_obj_set_width(en_value, TOGGLE_W);
    lv_obj_set_style_text_align(en_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    for (uint8_t i = 0; i < 3u; i++) {
        const ui_setting_id_t id = k_lang_toggles[i];
        const char *caption = "";
        for (uint8_t t = 0; t < TOGGLE_COUNT; t++) {
            if (k_toggles[t].id == id) {
                caption = k_toggles[t].caption;
            }
        }
        add_lang_row(s, langs, id, caption);
    }

    lv_obj_t *left = NULL;
    lv_obj_t *row = setting_row(pane, "Unicode delivery",
                                "How text composed on this device is typed on the host when its layout cannot.",
                                &left);
    lv_obj_t *stepper = plain(row);
    lv_obj_set_size(stepper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(stepper, UI_SPACE_S, LV_PART_MAIN);
    s->unicode_prev = button(stepper, LV_SYMBOL_LEFT, STEP_W, STEP_W, UI_BTN_SECONDARY, unicode_clicked, s);
    s->unicode_value = text_copy(stepper, UNKNOWN_TEXT, UI_STYLE_TEXT_BODY);
    lv_obj_set_width(s->unicode_value, UNICODE_W);
    lv_obj_set_style_text_align(s->unicode_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    one_line_dots(s->unicode_value);
    s->unicode_next = button(stepper, LV_SYMBOL_RIGHT, STEP_W, STEP_W, UI_BTN_SECONDARY, unicode_clicked, s);
    s->unicode_hint = wrapped(pane, k_unknown_note, UI_STYLE_TEXT_MUTED);

    // FR-741: the row carries the stored id as text and, when this build has the tables, the control that opens
    // the picker. Without the tables there is no control at all - a screen that cannot change a value must not
    // offer to (07 §12).
    lv_obj_t *lrow = setting_row(pane, "Host layout",
                                 "The xkeyboard-config layout the legends and the one-keystroke characters "
                                 "follow. Set the same layout on the host.", &left);
    s->layout_value = text_copy(lrow, UNKNOWN_TEXT, UI_STYLE_TEXT_BODY);
    lv_obj_set_width(s->layout_value, TOGGLE_W);
    lv_obj_set_style_text_align(s->layout_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    one_line_dots(s->layout_value);
    if (kbd_layouts_count_get() > 0u) {
        s->layout_btn = button(lrow, "Change", TOGGLE_W, TOGGLE_H, UI_BTN_SECONDARY, layout_open_clicked, s);
    }
    s->layout_note = wrapped(pane, k_layout_note, UI_STYLE_TEXT_MUTED);

    // ADR-039: the picker. One heading row with Back, then the four screens - HOME, the six shared rows with
    // their stars and the index strip (LIST and VARIANTS), and the search box - and the paging footer. Every
    // widget is built once here and only ever relabelled, hidden or shown, so no screen, page, search or jump
    // allocates anything (AT-UIFW-09/10).
    lv_obj_t *box = plain(outer);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, lv_pct(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_hidden(box, true);
    s->pick_box = box;

    lv_obj_t *head = plain(box);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, TOGGLE_H);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, UI_SPACE_S, LV_PART_MAIN);
    s->pick_back = button(head, LV_SYMBOL_LEFT " Back", CLIENT_BTN_W, TOGGLE_H, UI_BTN_SECONDARY,
                          layout_back_clicked, s);
    s->pick_title_text[0] = '\0';
    s->pick_title = text_copy(head, "", UI_STYLE_TEXT_BODY);
    lv_obj_set_flex_grow(s->pick_title, 1);
    one_line_dots(s->pick_title);

    // ---- HOME: the current-layout card, the favorites, the recents, and the two ways into the catalog ----
    lv_obj_t *home = plain(box);
    lv_obj_set_width(home, lv_pct(100));
    lv_obj_set_flex_grow(home, 1);
    lv_obj_set_flex_flow(home, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(home, UI_SPACE_XS, LV_PART_MAIN);
    s->pick_home = home;

    lv_obj_t *cur = plain(home);
    lv_obj_set_width(cur, lv_pct(100));
    lv_obj_set_height(cur, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cur, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cur, UI_SPACE_XS, LV_PART_MAIN);
    s->pick_cur_text[0] = '\0';
    s->pick_cur_name = text_copy(cur, "", UI_STYLE_TEXT_BODY);
    one_line_dots(s->pick_cur_name);

    // The home row of the layout in force: the row that tells QWERTY, QWERTZ, AZERTY, Dvorak and Cyrillic
    // apart at a glance (ADR-039 section 2). Inert chips - they emit nothing and take no touch.
    lv_obj_t *preview = plain(cur);
    lv_obj_set_width(preview, lv_pct(100));
    lv_obj_set_height(preview, PREVIEW_H);
    lv_obj_set_flex_flow(preview, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(preview, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(preview, UI_SPACE_XS, LV_PART_MAIN);
    for (uint8_t i = 0; i < UI_SETTINGS_PREVIEW_KEYS; i++) {
        lv_obj_t *chip = plain(preview);
        lv_obj_set_size(chip, PREVIEW_W, PREVIEW_H);
        ui_theme_apply(chip, UI_STYLE_WELL, LV_STATE_DEFAULT);
        s->pick_preview_text[i][0] = '\0';
        s->pick_preview[i] = text_copy(chip, "", UI_STYLE_TEXT_LABEL);
        lv_obj_center(s->pick_preview[i]);
    }

    (void)text(home, "Favorites", UI_STYLE_TEXT_LABEL);
    lv_obj_t *favs = plain(home);
    lv_obj_set_width(favs, lv_pct(100));
    lv_obj_set_height(favs, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(favs, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(favs, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_style_pad_column(favs, UI_SPACE_XS, LV_PART_MAIN);
    s->pick_fav_box = favs;
    for (uint8_t i = 0; i < UI_SETTINGS_FAV_MAX; i++) {
        s->pick_fav_text[i][0] = '\0';
        s->pick_fav[i] = button(favs, "", FAV_W, FAV_H, UI_BTN_SECONDARY, layout_fav_clicked, s);
        lv_obj_add_event_cb(s->pick_fav[i], layout_fav_clicked, LV_EVENT_LONG_PRESSED, s);
        s->pick_fav_label[i] = lv_obj_get_child(s->pick_fav[i], 0);
        lv_obj_set_style_text_align(s->pick_fav_label[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_hidden(s->pick_fav[i], true);
    }
    s->pick_fav_empty = text(home, "Nothing pinned yet - the star on a row pins a layout.", UI_STYLE_TEXT_MUTED);
    one_line_dots(s->pick_fav_empty);

    (void)text(home, "Recent", UI_STYLE_TEXT_LABEL);
    lv_obj_t *recents = plain(home);
    lv_obj_set_width(recents, lv_pct(100));
    lv_obj_set_height(recents, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(recents, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(recents, UI_SPACE_XS, LV_PART_MAIN);
    s->pick_recent_box = recents;
    for (uint8_t i = 0; i < UI_SETTINGS_RECENT_MAX; i++) {
        s->pick_recent_text[i][0] = '\0';
        s->pick_recent[i] = button(recents, "", RECENT_W, TOGGLE_H, UI_BTN_QUIET, layout_recent_clicked, s);
        s->pick_recent_label[i] = lv_obj_get_child(s->pick_recent[i], 0);
        lv_obj_set_hidden(s->pick_recent[i], true);
    }
    s->pick_recent_empty = text(home, "No layout used yet.", UI_STYLE_TEXT_MUTED);
    one_line_dots(s->pick_recent_empty);

    lv_obj_t *ways = plain(home);
    lv_obj_set_width(ways, lv_pct(100));
    lv_obj_set_height(ways, TOGGLE_H);
    lv_obj_set_flex_flow(ways, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ways, UI_SPACE_S, LV_PART_MAIN);
    s->pick_search_btn = button(ways, LV_SYMBOL_KEYBOARD " Search", PAIR_W, TOGGLE_H, UI_BTN_PRIMARY,
                                layout_search_clicked, s);
    s->pick_all_btn = button(ways, LV_SYMBOL_LIST " All layouts", PAIR_W, TOGGLE_H, UI_BTN_SECONDARY,
                             layout_all_clicked, s);
    // Two fixed widths plus the gap can be wider than the pane, and a button that hangs over the edge is
    // clipped away from the touch search as well as from the eye: let the row share what it has.
    lv_obj_set_flex_grow(s->pick_search_btn, 1);
    lv_obj_set_flex_grow(s->pick_all_btn, 1);
    lv_obj_set_width(s->pick_search_btn, 0);
    lv_obj_set_width(s->pick_all_btn, 0);

    // ---- SEARCH: the query field and the picker's own keyboard -------------------------------------------
    lv_obj_t *sbox = plain(box);
    lv_obj_set_width(sbox, lv_pct(100));
    lv_obj_set_height(sbox, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(sbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sbox, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_hidden(sbox, true);
    s->pick_search_box = sbox;

    lv_obj_t *field = plain(sbox);
    lv_obj_set_width(field, lv_pct(100));
    lv_obj_set_height(field, TOGGLE_H);
    lv_obj_set_flex_flow(field, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(field, UI_SPACE_S, LV_PART_MAIN);
    ui_theme_apply(field, UI_STYLE_WELL, LV_STATE_DEFAULT);
    s->query_shown[0] = '\0';
    s->pick_query_label = text_copy(field, "", UI_STYLE_TEXT_BODY);
    lv_obj_set_flex_grow(s->pick_query_label, 1);
    one_line_dots(s->pick_query_label);
    s->pick_erase = button(field, LV_SYMBOL_BACKSPACE, STEP_W, STEP_W, UI_BTN_SECONDARY, layout_erase_clicked, s);
    s->pick_clear = button(field, LV_SYMBOL_CLOSE, STEP_W, STEP_W, UI_BTN_QUIET, layout_erase_clicked, s);

    lv_obj_t *keys = plain(sbox);
    lv_obj_set_width(keys, lv_pct(100));
    lv_obj_set_height(keys, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(keys, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(keys, UI_SPACE_XS, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keys, UI_SPACE_XS, LV_PART_MAIN);
    for (uint8_t i = 0; i < UI_SETTINGS_QUERY_KEYS; i++) {
        const bool wide = k_query_keys[i] == ' ';
        s->pick_key[i] = button(keys, k_query_caps[i], wide ? QKEY_W * 2 : QKEY_W, UI_TOUCH_MIN,
                                UI_BTN_SECONDARY, layout_key_clicked, s);
    }

    // ---- LIST and VARIANTS: the shared rows, each with its star, under the A-Z strip ----------------------
    // The strip lies across the top, not down the side: nine buckets at the 44 px touch floor are 396 px, and
    // the pane is wider than it is tall, so a column of them would run off the bottom and out of reach.
    lv_obj_t *listing = plain(box);
    lv_obj_set_width(listing, lv_pct(100));
    lv_obj_set_flex_grow(listing, 1);
    lv_obj_set_flex_flow(listing, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(listing, UI_SPACE_S, LV_PART_MAIN);
    s->pick_list_box = listing;

    lv_obj_t *strip = plain(listing);
    lv_obj_set_width(strip, lv_pct(100));
    lv_obj_set_height(strip, UI_TOUCH_MIN);
    lv_obj_set_flex_flow(strip, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(strip, 0, LV_PART_MAIN);
    lv_obj_set_hidden(strip, true);
    s->pick_index_box = strip;
    for (uint8_t b = 0; b < UI_SETTINGS_PICK_INDEX; b++) {
        // The caption never changes, so the label stays static; only the enabled state follows the tables.
        s->pick_index[b] = button(strip, k_index_caps[b], INDEX_W, UI_TOUCH_MIN, UI_BTN_QUIET,
                                  layout_index_clicked, s);
        lv_obj_set_flex_grow(s->pick_index[b], 1);
        lv_obj_set_width(s->pick_index[b], 0);
    }

    lv_obj_t *rows = plain(listing);
    s->pick_rows_box = rows;
    lv_obj_set_width(rows, lv_pct(100));
    lv_obj_set_flex_grow(rows, 1);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(rows, UI_SPACE_XS, LV_PART_MAIN);
    for (uint8_t r = 0; r < UI_SETTINGS_PICK_ROWS; r++) {
        lv_obj_t *line = plain(rows);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, UI_TOUCH_MIN);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(line, UI_SPACE_XS, LV_PART_MAIN);
        s->pick_text[r][0] = '\0';
        s->pick_row[r] = button(line, "", lv_pct(100), UI_TOUCH_MIN, UI_BTN_SECONDARY, layout_row_clicked, s);
        lv_obj_set_flex_grow(s->pick_row[r], 1);
        lv_obj_set_width(s->pick_row[r], 0); // grow decides the width; a full-width row would run under the star
        s->pick_label[r] = lv_obj_get_child(s->pick_row[r], 0);
        lv_label_set_text(s->pick_label[r], s->pick_text[r]);
        lv_obj_set_style_text_align(s->pick_label[r], LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_align(s->pick_label[r], LV_ALIGN_LEFT_MID, UI_SPACE_S, 0);
        one_line_dots(s->pick_label[r]);
        // The star is a control of its own, so a tap on the row still picks the layout (ADR-039 section 3).
        // It carries a drawn star, not a check mark: the row's own text already uses a check for the layout in
        // force, and one glyph cannot mean two things on the same line.
        s->pick_star[r] = button(line, "", STAR_W, UI_TOUCH_MIN, UI_BTN_QUIET, layout_star_clicked, s);
        lv_obj_clean(s->pick_star[r]); // the empty caption label would only sit under the icon
        s->pick_star_icon[r] = lv_image_create(s->pick_star[r]);
        lv_image_set_src(s->pick_star_icon[r], &ui_icon_star_hollow);
        lv_obj_center(s->pick_star_icon[r]);
        lv_obj_set_clickable(s->pick_star_icon[r], false); // the button owns the touch
        lv_obj_set_hidden(s->pick_row[r], true);
        lv_obj_set_hidden(lv_obj_get_parent(s->pick_row[r]), true);
    }

    s->pick_note_text[0] = '\0';
    s->pick_note = text(box, s->pick_note_text, UI_STYLE_TEXT_MUTED);
    one_line_dots(s->pick_note);

    lv_obj_t *foot = plain(box);
    lv_obj_set_width(foot, lv_pct(100));
    lv_obj_set_height(foot, TOGGLE_H);
    lv_obj_set_flex_flow(foot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(foot, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(foot, UI_SPACE_S, LV_PART_MAIN);
    s->pick_foot = foot;
    lv_obj_t *hint = text_copy(foot, k_layout_pick_note, UI_STYLE_TEXT_MUTED);
    lv_obj_set_flex_grow(hint, 1);
    one_line_dots(hint);
    s->pick_prev = button(foot, LV_SYMBOL_LEFT, STEP_W, STEP_W, UI_BTN_SECONDARY, layout_step_clicked, s);
    s->pick_page_text[0] = '\0';
    s->pick_page_label = text(foot, s->pick_page_text, UI_STYLE_TEXT_BODY);
    lv_obj_set_width(s->pick_page_label, 72);
    lv_obj_set_style_text_align(s->pick_page_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    s->pick_next = button(foot, LV_SYMBOL_RIGHT, STEP_W, STEP_W, UI_BTN_SECONDARY, layout_step_clicked, s);
}

/**
 * FR-751 / TECH-084. The license lines and the third-party notices are the generated strings of
 * ui_notices_generated.h, which tools/gen_third_party_notices.py derives from THIRD_PARTY_NOTICES.md at
 * build time - the screen cannot drift from the record, and no text is retyped here.
 *
 * The notices are long, so they live in a scrolling container whose single child is sized to its content and
 * holds the wrapped label. That keeps the label inside its own parent (the geometry scan would otherwise
 * report it as spilling) while the container scrolls.
 */
static void build_about_pane(ui_view_settings_t *s, lv_obj_t *holder)
{
    lv_obj_t *pane = new_pane(s, holder, UI_SETTINGS_SEC_ABOUT, UI_SPACE_S);

    lv_obj_t *row = plain(pane);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    (void)text(row, "Firmware", UI_STYLE_TEXT_BODY);
    s->version_value = text_copy(row, UNKNOWN_TEXT, UI_STYLE_TEXT_BODY);
    lv_obj_set_width(s->version_value, 320);
    lv_obj_set_style_text_align(s->version_value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    one_line_dots(s->version_value);

    (void)wrapped(pane, ui_notices_license(), UI_STYLE_TEXT_LABEL);
    (void)text(pane, "Third-party notices", UI_STYLE_TEXT_BODY);

    lv_obj_t *scroll = plain(pane);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_scrollable(scroll, true);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    ui_theme_apply(scroll, UI_STYLE_WELL, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(scroll, UI_SPACE_S, LV_PART_MAIN);

    lv_obj_t *page = plain(scroll);
    lv_obj_set_width(page, PANE_W - 2 * UI_SPACE_S - UI_SPACE_M);
    lv_obj_set_height(page, LV_SIZE_CONTENT);
    (void)wrapped(page, ui_notices_third_party(), UI_STYLE_TEXT_LABEL);
}

static bool build(void *ctx, lv_obj_t *card)
{
    ui_view_settings_t *s = ctx;
    if (!ui_theme_is_ready()) {
        return false;
    }
    // The router created the card with the default object style and centered it: take the theme card and put
    // the centering back (lv_obj_center is a local style that remove_style_all clears).
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_pad_all(card, UI_MODAL_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(card, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_clickable(card, true); // a tap on the card must not reach the scrim below
    lv_obj_center(card);

    lv_obj_t *header = plain(card);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, HEADER_H);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    (void)text(header, "Settings", UI_STYLE_TEXT_TITLE);
    (void)button(header, "Close", CLOSE_W, UI_MODAL_BTN_H, UI_BTN_SECONDARY, close_clicked, s);

    lv_obj_t *body = plain(card);
    lv_obj_set_size(body, lv_pct(100), BODY_H);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(body, UI_SPACE_L, LV_PART_MAIN);

    lv_obj_t *list = plain(body);
    lv_obj_set_size(list, TAB_W, BODY_H);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, UI_SPACE_XS, LV_PART_MAIN);
    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        char legend[48];
        (void)snprintf(legend, sizeof(legend), "%s  %s", k_section_icon[i], k_section_name[i]);
        s->tab[i] = button(list, "", TAB_W, TAB_H, UI_BTN_QUIET, section_clicked, s);
        lv_obj_t *label = lv_obj_get_child(s->tab[i], 0);
        lv_label_set_text(label, legend); // a copy: `legend` is on the stack
        lv_obj_align(label, LV_ALIGN_LEFT_MID, UI_SPACE_M, 0);
    }

    lv_obj_t *holder = plain(body);
    lv_obj_set_size(holder, PANE_W, BODY_H);
    lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_COLUMN);

    build_remote_pane(s, holder);
    build_clients_pane(s, holder);
    build_shortcuts_pane(s, holder);
    build_input_pane(s, holder);
    build_keyboard_pane(s, holder);
    build_about_pane(s, holder);

    s->card = card;
    (void)ui_view_settings_show_section(s, s->section);
    refresh(s);
    s->c.opens++;
    return true;
}

static void on_close(void *ctx)
{
    ui_view_settings_t *s = ctx;
    s->card = NULL;
    memset(s->tab, 0, sizeof(s->tab));
    memset(s->pane, 0, sizeof(s->pane));
    memset(s->toggle, 0, sizeof(s->toggle));
    memset(s->approval, 0, sizeof(s->approval));
    memset(s->approval_label, 0, sizeof(s->approval_label));
    memset(s->client_row, 0, sizeof(s->client_row));
    s->approval_note = NULL;
    s->pair_btn = NULL;
    s->pair_note = NULL;
    s->interval_value = NULL;
    s->interval_down = NULL;
    s->interval_up = NULL;
    s->clients_heading = NULL;
    s->clients_empty = NULL;
    s->layout_value = NULL;
    s->unicode_value = NULL;
    s->unicode_prev = NULL;
    s->unicode_next = NULL;
    s->unicode_hint = NULL;
    s->layout_note = NULL;
    // FR-741: the picker dies with the card, and the level it was on is forgotten - reopening Settings starts at
    // the layout the record holds, never at a page from a previous visit.
    s->kbd_main = NULL;
    s->layout_btn = NULL;
    s->pick_box = NULL;
    s->pick_title = NULL;
    s->pick_back = NULL;
    s->pick_prev = NULL;
    s->pick_next = NULL;
    s->pick_page_label = NULL;
    memset(s->pick_row, 0, sizeof(s->pick_row));
    memset(s->pick_label, 0, sizeof(s->pick_label));
    memset(s->pick_star, 0, sizeof(s->pick_star));
    memset(s->pick_star_icon, 0, sizeof(s->pick_star_icon));
    memset(s->pick_fav, 0, sizeof(s->pick_fav));
    memset(s->pick_fav_label, 0, sizeof(s->pick_fav_label));
    memset(s->pick_recent, 0, sizeof(s->pick_recent));
    memset(s->pick_recent_label, 0, sizeof(s->pick_recent_label));
    memset(s->pick_preview, 0, sizeof(s->pick_preview));
    memset(s->pick_index, 0, sizeof(s->pick_index));
    memset(s->pick_key, 0, sizeof(s->pick_key));
    s->pick_foot = NULL;
    s->pick_home = NULL;
    s->pick_cur_name = NULL;
    s->pick_fav_box = NULL;
    s->pick_fav_empty = NULL;
    s->pick_recent_box = NULL;
    s->pick_recent_empty = NULL;
    s->pick_search_btn = NULL;
    s->pick_all_btn = NULL;
    s->pick_note = NULL;
    s->pick_rows_box = NULL;
    s->pick_rows_fit = UI_SETTINGS_PICK_ROWS;
    s->pick_list_page = 0u;
    s->pick_list_box = NULL;
    s->pick_index_box = NULL;
    s->pick_search_box = NULL;
    s->pick_query_label = NULL;
    s->pick_erase = NULL;
    s->pick_clear = NULL;
    s->pick_open = false;
    s->pick_screen = UI_PICK_HOME;
    s->pick_long_press = false;
    s->pick_group = 0u;
    s->pick_page = 0u;
    s->query[0] = '\0';
    s->query_len = 0u;
    s->pick_note_text[0] = '\0';
    s->version_value = NULL;
    s->c.closes++;
}

// ---- public API ----------------------------------------------------------------------------

void ui_view_settings_init(ui_view_settings_t *s, ui_actions_t *actions, ui_router_t *router,
                           uint64_t (*now_us)(void), ui_settings_sink_fn sink, void *sink_ctx)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->actions = actions;
    s->router = router;
    s->now_us = now_us;
    s->sink = sink;
    s->sink_ctx = sink_ctx;
    s->section = UI_SETTINGS_SEC_REMOTE;
    s->ov[0] = (ui_overlay_descriptor_t){
        .id = UI_OVERLAY_SETTINGS, .create = build, .on_close = on_close, .blocking = true, .ctx = s};
    s->ov[1] = (ui_overlay_descriptor_t){.id = UI_OVERLAY_SETTINGS_CONFIRM,
                                         .create = confirm_build,
                                         .on_close = confirm_closed,
                                         .blocking = true,
                                         .ctx = s};
}

const ui_overlay_descriptor_t *ui_view_settings_overlays(ui_view_settings_t *s, uint8_t *count)
{
    if (count != NULL) {
        *count = UI_SETTINGS_OVERLAYS;
    }
    return s != NULL ? s->ov : NULL;
}

void ui_view_settings_set_pair_action(ui_view_settings_t *s, void (*fn)(void *ctx), void *ctx)
{
    if (s != NULL) {
        s->pair_action = fn;
        s->pair_ctx = ctx;
    }
}

void ui_view_settings_set(ui_view_settings_t *s, const ui_settings_view_t *v)
{
    if (s == NULL || v == NULL) {
        return;
    }
    s->v = *v;
    if (s->v.client_count > UI_SETTINGS_CLIENTS_MAX) {
        s->v.client_count = UI_SETTINGS_CLIENTS_MAX; // a malformed publish shows fewer rows, never more
    }
    refresh(s);
}

const ui_settings_view_t *ui_view_settings_snapshot(const ui_view_settings_t *s)
{
    return s != NULL ? &s->v : NULL;
}

lv_obj_t *ui_view_settings_card(const ui_view_settings_t *s, uint16_t overlay_id)
{
    if (s == NULL) {
        return NULL;
    }
    if (overlay_id == UI_OVERLAY_SETTINGS) {
        return s->card;
    }
    return overlay_id == UI_OVERLAY_SETTINGS_CONFIRM ? s->confirm_card : NULL;
}

bool ui_view_settings_show_section(ui_view_settings_t *s, ui_settings_section_t section)
{
    if (s == NULL || (unsigned)section >= UI_SETTINGS_SEC_COUNT) {
        return false;
    }
    s->section = section;
    if (s->card == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < UI_SETTINGS_SEC_COUNT; i++) {
        lv_obj_set_hidden(s->pane[i], (ui_settings_section_t)i != section);
        set_checked(s->tab[i], (ui_settings_section_t)i == section);
    }
    refresh(s);
    return true;
}

ui_settings_section_t ui_view_settings_section(const ui_view_settings_t *s)
{
    return s != NULL ? s->section : UI_SETTINGS_SEC_REMOTE;
}

lv_obj_t *ui_view_settings_toggle(const ui_view_settings_t *s, ui_setting_id_t id)
{
    if (s == NULL || (unsigned)id >= UI_SETTING_COUNT) {
        return NULL;
    }
    return s->toggle[id].control;
}

const char *ui_view_settings_toggle_text(const ui_view_settings_t *s, ui_setting_id_t id)
{
    if (s == NULL || (unsigned)id >= UI_SETTING_COUNT || s->toggle[id].value == NULL) {
        return NULL;
    }
    return lv_label_get_text(s->toggle[id].value);
}

lv_obj_t *ui_view_settings_approval_button(const ui_view_settings_t *s, ui_control_approval_t which)
{
    if (s == NULL || (unsigned)which >= UI_CONTROL_APPROVAL_COUNT) {
        return NULL;
    }
    return s->approval[which];
}

lv_obj_t *ui_view_settings_pair_button(const ui_view_settings_t *s)
{
    return s != NULL ? s->pair_btn : NULL;
}

const char *ui_view_settings_unicode_hint(const ui_view_settings_t *s)
{
    return (s != NULL && s->unicode_hint != NULL) ? lv_label_get_text(s->unicode_hint) : NULL;
}

lv_obj_t *ui_view_settings_unicode_button(const ui_view_settings_t *s, bool next)
{
    if (s == NULL) {
        return NULL;
    }
    return next ? s->unicode_next : s->unicode_prev;
}

const char *ui_view_settings_interval_text(const ui_view_settings_t *s)
{
    return (s != NULL && s->interval_value != NULL) ? lv_label_get_text(s->interval_value) : NULL;
}

lv_obj_t *ui_view_settings_interval_button(const ui_view_settings_t *s, bool up)
{
    if (s == NULL) {
        return NULL;
    }
    return up ? s->interval_up : s->interval_down;
}

lv_obj_t *ui_view_settings_client_trusted_button(const ui_view_settings_t *s, uint8_t index)
{
    if (s == NULL || index >= UI_SETTINGS_CLIENTS_MAX || index >= s->v.client_count) {
        return NULL;
    }
    return s->client_row[index].trusted_btn;
}

lv_obj_t *ui_view_settings_client_forget_button(const ui_view_settings_t *s, uint8_t index)
{
    if (s == NULL || index >= UI_SETTINGS_CLIENTS_MAX || index >= s->v.client_count) {
        return NULL;
    }
    return s->client_row[index].forget_btn;
}

const char *ui_view_settings_client_name_text(const ui_view_settings_t *s, uint8_t index)
{
    if (s == NULL || index >= UI_SETTINGS_CLIENTS_MAX || s->client_row[index].name == NULL) {
        return NULL;
    }
    return lv_label_get_text(s->client_row[index].name);
}

// ---- FR-741 HOST layout picker -------------------------------------------------------------
lv_obj_t *ui_view_settings_layout_button(const ui_view_settings_t *s)
{
    return s != NULL ? s->layout_btn : NULL;
}

bool ui_view_settings_picker_open(const ui_view_settings_t *s)
{
    return s != NULL && s->card != NULL && s->pick_open;
}

const char *ui_view_settings_picker_title(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_title_text : "";
}

lv_obj_t *ui_view_settings_picker_row(const ui_view_settings_t *s, uint8_t row)
{
    if (!ui_view_settings_picker_open(s) || row >= UI_SETTINGS_PICK_ROWS) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_row[row]) ? NULL : s->pick_row[row];
}

const char *ui_view_settings_picker_row_text(const ui_view_settings_t *s, uint8_t row)
{
    if (!ui_view_settings_picker_open(s) || row >= UI_SETTINGS_PICK_ROWS) {
        return "";
    }
    return s->pick_text[row];
}

const char *ui_view_settings_picker_page_text(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_page_text : "";
}

lv_obj_t *ui_view_settings_picker_back(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_back : NULL;
}

lv_obj_t *ui_view_settings_picker_step(const ui_view_settings_t *s, bool next)
{
    if (!ui_view_settings_picker_open(s)) {
        return NULL;
    }
    return next ? s->pick_next : s->pick_prev;
}

ui_pick_screen_t ui_view_settings_picker_screen(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_screen : UI_PICK_HOME;
}

const char *ui_view_settings_picker_current_text(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_cur_text : "";
}

const char *ui_view_settings_picker_preview_text(const ui_view_settings_t *s, uint8_t slot)
{
    if (!ui_view_settings_picker_open(s) || slot >= UI_SETTINGS_PREVIEW_KEYS) {
        return "";
    }
    return s->pick_preview_text[slot];
}

lv_obj_t *ui_view_settings_picker_fav(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_FAV_MAX) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_fav[i]) ? NULL : s->pick_fav[i];
}

const char *ui_view_settings_picker_fav_text(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_FAV_MAX) {
        return "";
    }
    return s->pick_fav_text[i];
}

lv_obj_t *ui_view_settings_picker_recent(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_RECENT_MAX) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_recent[i]) ? NULL : s->pick_recent[i];
}

const char *ui_view_settings_picker_recent_text(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_RECENT_MAX) {
        return "";
    }
    return s->pick_recent_text[i];
}

lv_obj_t *ui_view_settings_picker_search_button(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_search_btn : NULL;
}

lv_obj_t *ui_view_settings_picker_all_button(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_all_btn : NULL;
}

const char *ui_view_settings_picker_note(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->pick_note_text : "";
}

lv_obj_t *ui_view_settings_picker_star(const ui_view_settings_t *s, uint8_t row)
{
    if (!ui_view_settings_picker_open(s) || row >= UI_SETTINGS_PICK_ROWS) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_row[row]) ? NULL : s->pick_star[row];
}

lv_obj_t *ui_view_settings_picker_index(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_PICK_INDEX) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_index_box) ? NULL : s->pick_index[i];
}

lv_obj_t *ui_view_settings_picker_key(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_QUERY_KEYS) {
        return NULL;
    }
    return lv_obj_is_hidden(s->pick_search_box) ? NULL : s->pick_key[i];
}

const char *ui_view_settings_picker_query(const ui_view_settings_t *s)
{
    return ui_view_settings_picker_open(s) ? s->query : "";
}

char ui_view_settings_picker_key_char(const ui_view_settings_t *s, uint8_t i)
{
    if (!ui_view_settings_picker_open(s) || i >= UI_SETTINGS_QUERY_KEYS) {
        return '\0';
    }
    return k_query_keys[i];
}

lv_obj_t *ui_view_settings_picker_erase(const ui_view_settings_t *s)
{
    if (!ui_view_settings_picker_open(s) || lv_obj_is_hidden(s->pick_search_box)) {
        return NULL;
    }
    return s->pick_erase;
}

lv_obj_t *ui_view_settings_picker_clear(const ui_view_settings_t *s)
{
    if (!ui_view_settings_picker_open(s) || lv_obj_is_hidden(s->pick_search_box)) {
        return NULL;
    }
    return s->pick_clear;
}

const char *ui_view_settings_layout_text(const ui_view_settings_t *s)
{
    return (s != NULL && s->layout_value != NULL) ? lv_label_get_text(s->layout_value) : NULL;
}

const char *ui_view_settings_unicode_text(const ui_view_settings_t *s)
{
    return (s != NULL && s->unicode_value != NULL) ? lv_label_get_text(s->unicode_value) : NULL;
}

const char *ui_view_settings_version_text(const ui_view_settings_t *s)
{
    return (s != NULL && s->version_value != NULL) ? lv_label_get_text(s->version_value) : NULL;
}

bool ui_view_settings_confirm_open(const ui_view_settings_t *s)
{
    return s != NULL && s->confirm_card != NULL;
}

const char *ui_view_settings_confirm_heading(const ui_view_settings_t *s)
{
    return (s != NULL && s->confirm_heading != NULL) ? lv_label_get_text(s->confirm_heading) : NULL;
}

const char *ui_view_settings_confirm_body(const ui_view_settings_t *s)
{
    return (s != NULL && s->confirm_body != NULL) ? lv_label_get_text(s->confirm_body) : NULL;
}

lv_obj_t *ui_view_settings_confirm_button(const ui_view_settings_t *s, bool yes)
{
    if (s == NULL) {
        return NULL;
    }
    return yes ? s->confirm_yes : s->confirm_no;
}
