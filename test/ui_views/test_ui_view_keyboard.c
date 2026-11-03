// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-070B with the real LVGL 9.6 (headless 1024x600, LVGL test pointer, ASan + UBSan), the real router, store,
// mailbox, action gateway, theme and UiActionKey; the domain sink is a fake HOST that tracks which usages are
// held:
// - every cap of both layers: tap = KEY_DOWN then KEY_UP with the usage of hid_keymap.h (AT-KEY-01/02/03);
// - hold 700 ms = one DOWN and no repeat, sliding off = UP through PRESS_LOST (FR-203/208);
// - modifier tap = MODIFIER_TOGGLE only; OFF / ARMED / LOCKED are three distinct looks taken from
//   UI_SUBJ_MODS_ARMED / UI_SUBJ_MODS_LOCKED alone (FR-204, AT-KEY-04/06);
// - CapsLock: an ordinary DOWN/UP key, indicator On / Off / Unknown from the LED subject (FR-206, AT-KEY-07);
// - F1-F12: a local layer, no action, and a held key is released before the rows swap (FR-207, AT-KEY-03);
// - HOST not mounted: every cap disabled and silent; HOST lost while a key is held = one UP;
// - navigation while a key is held: the router's release request and the view's own UP (AT-UIFW-08);
// - a refused DOWN never leaves a local "held" flag (no UP owed);
// - every usage the view can emit is a valid keyboard usage or a modifier (FR-202);
// - geometry: every visible cap inside the content slot, >= 44 px in both axes, no two caps overlap;
// - lifecycle: store deleted before the router, 1000 view create/destroy cycles without live-allocation growth
//   (AT-UIFW-09);
// - KO layer (ADR-024 FR-746/748): disabled while the delivery mode is host-ime; on = TEXT_CANCEL first, letters
//   feed the 2-set composer and only committed syllables leave as TEXT_CHAR, Backspace edits the preedit locally,
//   any other key commits then goes as TEXT_KEY, Shift is a local jamo shift, Ctrl/Alt/Win/AltGr/Caps disabled,
//   never a KEY_DOWN; off commits the preedit; leaving the view or host-ime discards it with TEXT_CANCEL.
// - ZH layer (FR-746/748) over a deterministic fake decoder with the ime_pinyin contract: enabled only with a
//   decoder, a delivery mode and a HOST; letters and ' build the input, candidates page by six, a tap or a digit
//   fixes one, a complete choice commits the sentence as TEXT_CHAR, Space / Enter / Esc / Backspace / any other key
//   as documented, full-width punctuation with nothing composed, the 26-letter cap, KO <-> ZH without a
//   TEXT_CANCEL after the committed text, discard with TEXT_CANCEL on leave / host-ime / decoder removal / HOST
//   loss, never a KEY_DOWN or MODIFIER_TOGGLE, and the strip's keys are real 44 px tap targets.
// - the language key (ADR-037): one cap whose legend follows the language; a tap cycles EN -> KO -> ZH -> JA -> EN
//   and skips anything turned off in Settings or unavailable; with English alone a tap only toasts; a long press
//   opens the blocking picker and is never also a tap; a pick switches and closes; a language turned off while it
//   is active falls back to English committing what is composed; without a router there is no picker and no toast.
#include <stdint.h>
#include <string.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_overlay.h"
#include "ui_router.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_tokens.h"
#include "ui_view_keyboard.h"

#define SCR_W UI_SCREEN_W
#define SCR_H UI_SCREEN_H
#define TOP_H UI_TOPBAR_H

// ---- fake domain ---------------------------------------------------------------------------
typedef struct {
    ui_action_type_t type;
    uint16_t value;
} rec_t;

static rec_t s_rec[256];
static int s_rec_n;
static bool s_host_down[256]; // what the HOST holds, by usage
static int s_host_down_n;
static bool s_refuse;
static uint64_t s_clock;

static uint64_t fake_now(void)
{
    return s_clock += 1000;
}

static bool fake_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    if (s_refuse) {
        return false;
    }
    if (s_rec_n < (int)(sizeof(s_rec) / sizeof(s_rec[0]))) {
        s_rec[s_rec_n++] = (rec_t){a->type, a->value};
    }
    if (a->type == UI_ACT_KEY_DOWN && a->value < 256u && !s_host_down[a->value]) {
        s_host_down[a->value] = true;
        s_host_down_n++;
    }
    if (a->type == UI_ACT_KEY_UP && a->value < 256u && s_host_down[a->value]) {
        s_host_down[a->value] = false;
        s_host_down_n--;
    }
    return true;
}

static int s_cancels;
static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    (void)why;
    s_cancels++;
    memset(s_host_down, 0, sizeof(s_host_down)); // RELEASE_ALL accepted by the HID core
    s_host_down_n = 0;
    return true;
}

static const ui_nav_hooks_t k_hooks = {.cancel_input = hook_cancel};

// ---- second view, for navigation -----------------------------------------------------------
static lv_obj_t *other_create(void *ctx, lv_obj_t *slot)
{
    (void)ctx;
    lv_obj_t *o = lv_obj_create(slot);
    lv_obj_set_size(o, lv_pct(100), lv_pct(100));
    return o;
}

// ---- fixture -------------------------------------------------------------------------------
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static bool s_store_on;
static ui_actions_t s_act;
static ui_view_keyboard_t s_kbd;
static ui_view_descriptor_t s_desc[2];
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static uint32_t s_seq;

static void publish_full(uint8_t host, uint8_t armed, uint8_t locked, bool caps_known, bool caps_on)
{
    ui_hid_slice_t h = {
        .host_usb_state = host,
        .mods_armed = armed,
        .mods_locked = locked,
        .caps_led_known = caps_known,
        .caps_led_active = caps_on,
    };
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static void publish(uint8_t host)
{
    publish_full(host, 0u, 0u, false, false);
}

static bool start(void)
{
    memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0;
    memset(s_host_down, 0, sizeof(s_host_down));
    s_host_down_n = 0;
    s_refuse = false;
    s_cancels = 0;
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    s_store_on = true;
    ui_actions_init(&s_act);
    ui_actions_set_domain_sink(&s_act, fake_sink, NULL);
    ui_view_keyboard_init(&s_kbd, &s_store, &s_act, fake_now);
    s_desc[0] = s_kbd.desc;
    s_desc[1] = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = other_create};
    s_scr = lv_obj_create(NULL);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    uint8_t n_ov = 0;
    const ui_overlay_descriptor_t *ov = ui_view_keyboard_overlays(&s_kbd, &n_ov);
    bool ok = ui_router_init(&s_r, s_scr, &lay, &s_store, s_desc, 2, ov, n_ov, &k_hooks);
    ui_view_keyboard_bind_router(&s_kbd, &s_r);
    ok = ok && ui_router_show(&s_r, UI_VIEW_KEYBOARD);
    // ADR-037: the fixture turns all three composed languages on, so the layer tests below can select any of
    // them. The default (English alone) is exercised by test_lang_english_only and test_lang_turned_off.
    ui_view_keyboard_set_languages(&s_kbd, true, true, true);
    publish(KVM_HOST_USB_MOUNTED);
    lv_test_wait(20);
    return ok;
}

static void stop(void)
{
    ui_router_deinit(&s_r);
    if (s_store_on) {
        ui_store_deinit(&s_store);
        s_store_on = false;
    }
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_delete(s_scr);
    lv_test_wait(10);
}

// ---- helpers -------------------------------------------------------------------------------
static void press_obj(lv_obj_t *o)
{
    lv_area_t a;
    if (o == NULL) {
        CHECK(o != NULL);
        return;
    }
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(50);
}

static void release(void)
{
    lv_test_mouse_release();
    lv_test_wait(50);
}

static void click_obj(lv_obj_t *o)
{
    if (o == NULL) {
        CHECK(o != NULL);
        return;
    }
    press_obj(o);
    release();
}

static int count(ui_action_type_t t, uint16_t v)
{
    int n = 0;
    for (int i = 0; i < s_rec_n; i++) {
        n += s_rec[i].type == t && s_rec[i].value == v;
    }
    return n;
}

/**
 * Mirror of kvm_usage_is_valid_key() (components/hid_core/src/hid_keymap.c): the ui_views host test links
 * ui_core + ui_theme + ui_components only, so hid_keymap.c is unavailable here and the rule is restated
 * instead of guessed. kvm_usage_is_modifier() is a header inline and is used directly.
 */
static bool usage_is_valid_key(uint8_t usage)
{
    return (usage >= KVM_KEY_A && usage <= KVM_KEY_LAST_REGULAR) || kvm_usage_is_modifier(usage);
}

static uint32_t index_of_usage(uint8_t usage)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        if (ui_view_keyboard_key_usage(i) == usage) {
            return i;
        }
    }
    return UINT32_MAX;
}

/** First visible cap carrying `usage`, else NULL. */
static lv_obj_t *visible_key(uint8_t usage)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        lv_obj_t *o = ui_view_keyboard_key_obj(&s_kbd, i);
        if (o != NULL && ui_view_keyboard_key_usage(i) == usage && !lv_obj_is_hidden(lv_obj_get_parent(o))) {
            return o;
        }
    }
    return NULL;
}

static lv_obj_t *fn_key_obj(void)
{
    return ui_view_keyboard_key_obj(&s_kbd, index_of_usage(0u));
}

// ---- the language key (ADR-037) ------------------------------------------------------------
static lv_obj_t *lang_key_obj(void)
{
    return ui_view_keyboard_lang_key(&s_kbd);
}

/** The code the cap shows right now: the icon is child 0, the mutable legend the last child. */
static const char *lang_key_text(void)
{
    lv_obj_t *key = lang_key_obj();
    return key != NULL ? lv_label_get_text(lv_obj_get_child(key, -1)) : "";
}

/** A tap: far shorter than LVGL's long-press time, so SHORT_CLICKED fires and LONG_PRESSED does not. */
static void tap_lang(void)
{
    click_obj(lang_key_obj());
}

/** A long press: held past the long-press time, so LONG_PRESSED fires and the short click is suppressed. */
static void hold_lang(void)
{
    press_obj(lang_key_obj());
    lv_test_wait(600);
    release();
}

/** Open the picker and tap one language's row: deterministic whatever the current language is. */
static void pick_lang(ui_kbd_lang_t lang)
{
    hold_lang();
    CHECK(ui_view_keyboard_picker_open(&s_kbd));
    click_obj(ui_view_keyboard_lang_pick_button(&s_kbd, lang));
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
}

static void type_letters(const char *s)
{
    for (; *s != '\0'; s++) {
        click_obj(visible_key((uint8_t)(KVM_KEY_A + (*s - 'a'))));
    }
}

// ---- tests ---------------------------------------------------------------------------------
// AT-KEY-01/02/03: every cap the user can reach, in both layers, gives DOWN then UP with its own usage.
static bool s_tapped[UI_VIEW_KBD_KEY_CAP];

static void tap_all_visible(int *tapped)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        lv_obj_t *o = ui_view_keyboard_key_obj(&s_kbd, i);
        const uint8_t usage = ui_view_keyboard_key_usage(i);
        if (o == NULL || usage == 0u || kvm_usage_is_modifier(usage) || s_tapped[i] || lv_obj_is_hidden(o) ||
            lv_obj_is_hidden(lv_obj_get_parent(o))) {
            continue;
        }
        s_tapped[i] = true;
        s_rec_n = 0;
        click_obj(o);
        if (s_rec_n != 2 || s_rec[0].type != UI_ACT_KEY_DOWN || s_rec[0].value != usage ||
            s_rec[1].type != UI_ACT_KEY_UP || s_rec[1].value != usage) {
            printf("    cap %u (%s, usage 0x%02X): %d records\n", (unsigned)i, ui_view_keyboard_key_name(i),
                   (unsigned)usage, s_rec_n);
        }
        CHECK_EQ(s_rec_n, 2);
        CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
        CHECK_EQ(s_rec[0].value, usage);
        CHECK_EQ(s_rec[1].type, UI_ACT_KEY_UP);
        CHECK_EQ(s_rec[1].value, usage);
        CHECK_EQ(s_host_down_n, 0);
        CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
        (*tapped)++;
    }
}

/** tap_all_visible() for a selected layout: the expected usage is the live one (FR-742), not the table's. */
static void tap_all_visible_live(int *tapped)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        lv_obj_t *o = ui_view_keyboard_key_obj(&s_kbd, i);
        const uint8_t usage = ui_view_keyboard_key_live_usage(&s_kbd, i);
        if (o == NULL || usage == 0u || kvm_usage_is_modifier(usage) || s_tapped[i] || lv_obj_is_hidden(o) ||
            lv_obj_is_hidden(lv_obj_get_parent(o))) {
            continue;
        }
        s_tapped[i] = true;
        s_rec_n = 0;
        click_obj(o);
        if (s_rec_n != 2 || s_rec[0].value != usage || s_rec[1].value != usage) {
            printf("    cap %u (%s, live usage 0x%02X): %d records\n", (unsigned)i,
                   ui_view_keyboard_key_name(i), (unsigned)usage, s_rec_n);
        }
        CHECK_EQ(s_rec_n, 2);
        CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
        CHECK_EQ(s_rec[0].value, usage);
        CHECK_EQ(s_rec[1].type, UI_ACT_KEY_UP);
        CHECK_EQ(s_rec[1].value, usage);
        CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
        (*tapped)++;
    }
}

static void test_tap_every_key(void)
{
    CHECK(start());
    memset(s_tapped, 0, sizeof(s_tapped));
    int tapped = 0;
    tap_all_visible(&tapped);
    click_obj(fn_key_obj()); // F1-F12 layer: the F-row becomes reachable
    CHECK(s_kbd.fn_layer);
    tap_all_visible(&tapped);
    printf("    %d caps tapped (both layers), %u in the table\n", tapped,
           (unsigned)ui_view_keyboard_key_count());
    // 5 modifiers + F1-F12 + the language cap, and the 8 caps of the other geometry families, which are hidden
    // while no layout is selected (FR-742; test_host_layout_geometry taps those).
    CHECK_EQ(tapped, (int)ui_view_keyboard_key_count() - 7 - 8);
    CHECK_EQ(s_kbd.c.key_down, (uint32_t)tapped);
    CHECK_EQ(s_kbd.c.key_up, (uint32_t)tapped);
    stop();
}

static void test_hold_no_repeat(void)
{
    CHECK(start());
    lv_obj_t *a = visible_key(KVM_KEY_A);
    CHECK(a != NULL);
    press_obj(a);
    lv_test_wait(700); // past LVGL's long-press and repeat timers: still one DOWN (FR-208)
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
    CHECK_EQ(s_rec[0].value, KVM_KEY_A);
    CHECK(s_host_down[KVM_KEY_A]);
    release();
    CHECK_EQ(count(UI_ACT_KEY_DOWN, KVM_KEY_A), 1);
    CHECK_EQ(count(UI_ACT_KEY_UP, KVM_KEY_A), 1);
    CHECK(!s_host_down[KVM_KEY_A]);
    stop();
}

static void test_slide_off(void)
{
    CHECK(start());
    lv_obj_t *a = visible_key(KVM_KEY_A);
    lv_obj_t *s = visible_key(0x16); // S, the neighbor
    CHECK(a != NULL && s != NULL);
    press_obj(a);
    CHECK(s_host_down[KVM_KEY_A]);
    lv_area_t area;
    lv_obj_get_coords(s, &area);
    lv_test_mouse_move_to((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2); // PRESS_LOST on A
    lv_test_wait(50);
    CHECK(!s_host_down[KVM_KEY_A]);
    CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
    release();
    CHECK_EQ(count(UI_ACT_KEY_DOWN, KVM_KEY_A), 1);
    CHECK_EQ(count(UI_ACT_KEY_UP, KVM_KEY_A), 1);
    CHECK_EQ(count(UI_ACT_KEY_DOWN, 0x16), 0); // the cap it slid onto was never pressed
    CHECK_EQ(s_host_down_n, 0);
    stop();
}

// FR-204 / AT-KEY-04 / AT-KEY-06: the tap only asks; the three looks come from the store.
static void test_modifier_follows_store(void)
{
    CHECK(start());
    static const uint8_t mods[5] = {KVM_KEY_LEFT_SHIFT, KVM_KEY_LEFT_CTRL, KVM_KEY_LEFT_ALT, KVM_KEY_LEFT_GUI,
                                    KVM_KEY_RIGHT_ALT};
    for (int i = 0; i < 5; i++) {
        s_rec_n = 0;
        lv_obj_t *o = visible_key(mods[i]);
        CHECK(o != NULL);
        click_obj(o);
        CHECK_EQ(s_rec_n, 1);
        CHECK_EQ(s_rec[0].type, UI_ACT_MODIFIER_TOGGLE);
        CHECK_EQ(s_rec[0].value, mods[i]);
        CHECK(!lv_obj_is_checked(o));                      // never optimistic (AGENTS-UI §5)
        CHECK(!lv_obj_has_state(o, LV_STATE_USER_2));
    }
    CHECK_EQ(s_kbd.c.mod_toggles, 5u);
    CHECK_EQ(count(UI_ACT_KEY_DOWN, KVM_KEY_LEFT_SHIFT), 0); // a modifier is never an ordinary key
    CHECK_EQ(s_host_down_n, 0);

    lv_obj_t *shift = visible_key(KVM_KEY_LEFT_SHIFT);
    lv_obj_t *ctrl = visible_key(KVM_KEY_LEFT_CTRL);
    const lv_color_t off = lv_obj_get_style_bg_color(shift, LV_PART_MAIN);

    publish_full(KVM_HOST_USB_MOUNTED, KVM_MOD_BIT(KVM_KEY_LEFT_SHIFT), 0u, false, false);
    CHECK(lv_obj_is_checked(shift));
    CHECK(!lv_obj_has_state(shift, LV_STATE_USER_2));
    CHECK(!lv_obj_is_checked(ctrl)); // only the armed bit
    const lv_color_t armed = lv_obj_get_style_bg_color(shift, LV_PART_MAIN);

    publish_full(KVM_HOST_USB_MOUNTED, 0u, KVM_MOD_BIT(KVM_KEY_LEFT_SHIFT), false, false);
    CHECK(lv_obj_has_state(shift, LV_STATE_USER_2));
    CHECK(!lv_obj_is_checked(shift)); // LOCKED is not shown as ARMED
    const lv_color_t locked = lv_obj_get_style_bg_color(shift, LV_PART_MAIN);

    printf("    modifier looks: off %06X, armed %06X, locked %06X\n", (unsigned)lv_color_to_u32(off) & 0xFFFFFFu,
           (unsigned)lv_color_to_u32(armed) & 0xFFFFFFu, (unsigned)lv_color_to_u32(locked) & 0xFFFFFFu);
    CHECK(lv_color_to_u32(off) != lv_color_to_u32(armed)); // three distinct looks (FR-204)
    CHECK(lv_color_to_u32(off) != lv_color_to_u32(locked));
    CHECK(lv_color_to_u32(armed) != lv_color_to_u32(locked));

    publish_full(KVM_HOST_USB_MOUNTED, 0u, 0u, false, false);
    CHECK(!lv_obj_is_checked(shift));
    CHECK(!lv_obj_has_state(shift, LV_STATE_USER_2));
    stop();
}

// FR-207 / AT-KEY-03: a local layer, no HID, and never swapped under a held key.
static void test_fn_layer(void)
{
    CHECK(start());
    lv_obj_t *fn = fn_key_obj();
    CHECK(fn != NULL);
    CHECK(!lv_obj_is_hidden(s_kbd.row_number));
    CHECK(lv_obj_is_hidden(s_kbd.row_fn));
    CHECK(visible_key(KVM_KEY_1) != NULL);
    CHECK(visible_key(KVM_KEY_F1) == NULL);

    click_obj(fn);
    CHECK_EQ(s_rec_n, 0); // never a HID action
    CHECK(s_kbd.fn_layer);
    CHECK(lv_obj_is_checked(fn));
    CHECK(lv_obj_is_hidden(s_kbd.row_number));
    CHECK(!lv_obj_is_hidden(s_kbd.row_fn));
    CHECK(visible_key(KVM_KEY_F1) != NULL);
    CHECK(visible_key(KVM_KEY_1) == NULL);
    CHECK(!lv_obj_is_hidden(s_kbd.row_top)); // the main field is not covered

    // the number row is still only swapped, not destroyed
    click_obj(fn);
    CHECK(!s_kbd.fn_layer);
    CHECK(!lv_obj_is_checked(fn));
    CHECK(visible_key(KVM_KEY_1) != NULL);
    CHECK_EQ(s_kbd.c.fn_switches, 2u);
    CHECK_EQ(s_rec_n, 0);

    // a second touch point on F1-F12 while a key of another row is held: the key is released first
    lv_obj_t *q = visible_key(0x14);
    CHECK(q != NULL);
    press_obj(q);
    CHECK(s_host_down[0x14]);
    const uint32_t leaves = s_kbd.c.leave_releases;
    (void)lv_obj_send_event(fn, LV_EVENT_PRESSED, NULL);
    (void)lv_obj_send_event(fn, LV_EVENT_RELEASED, NULL);
    CHECK(s_kbd.fn_layer);
    CHECK_EQ(s_kbd.c.leave_releases, leaves + 1);
    CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
    CHECK(!s_host_down[0x14]);
    CHECK_EQ(count(UI_ACT_KEY_UP, 0x14), 1);
    release();
    CHECK_EQ(count(UI_ACT_KEY_UP, 0x14), 1); // not twice
    stop();
}

// FR-206 / AT-KEY-07: the indicator is the LED report or the word "Unknown".
static void test_caps_indicator(void)
{
    CHECK(start());
    lv_obj_t *caps = visible_key(KVM_KEY_CAPS_LOCK);
    CHECK(caps != NULL);
    CHECK(s_kbd.caps_sub != NULL);
    CHECK_EQ(ui_store_get(&s_store, UI_SUBJ_CAPS_LED), UI_STORE_UNKNOWN);
    CHECK(strcmp(lv_label_get_text(s_kbd.caps_sub), "Unknown") == 0);
    CHECK(!lv_obj_is_checked(caps)); // an unknown LED is never drawn as "on"

    publish_full(KVM_HOST_USB_MOUNTED, 0u, 0u, true, true);
    CHECK(strcmp(lv_label_get_text(s_kbd.caps_sub), "On") == 0);
    CHECK(lv_obj_is_checked(caps));

    publish_full(KVM_HOST_USB_MOUNTED, 0u, 0u, true, false);
    CHECK(strcmp(lv_label_get_text(s_kbd.caps_sub), "Off") == 0);
    CHECK(!lv_obj_is_checked(caps));

    // CapsLock is an ordinary key: DOWN/UP of usage 0x39, not a MODIFIER_TOGGLE
    s_rec_n = 0;
    click_obj(caps);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
    CHECK_EQ(s_rec[0].value, KVM_KEY_CAPS_LOCK);
    CHECK_EQ(s_rec[1].type, UI_ACT_KEY_UP);
    CHECK_EQ(count(UI_ACT_MODIFIER_TOGGLE, KVM_KEY_CAPS_LOCK), 0);
    stop();
}

static void test_host_not_mounted(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_DETACHED);
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        CHECK(lv_obj_is_disabled(ui_view_keyboard_key_obj(&s_kbd, i)));
    }
    click_obj(visible_key(KVM_KEY_A));
    click_obj(visible_key(KVM_KEY_LEFT_SHIFT));
    click_obj(fn_key_obj());
    CHECK_EQ(s_rec_n, 0);
    CHECK(!s_kbd.fn_layer);

    publish(KVM_HOST_USB_MOUNTED);
    CHECK(!lv_obj_is_disabled(visible_key(KVM_KEY_A)));

    // HOST lost while a key is held: the owed UP goes out at once, exactly once
    press_obj(visible_key(KVM_KEY_A));
    CHECK_EQ(s_kbd.held, index_of_usage(KVM_KEY_A));
    const uint32_t leaves = s_kbd.c.leave_releases;
    publish(KVM_HOST_USB_SUSPENDED);
    CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
    CHECK(!s_host_down[KVM_KEY_A]);
    CHECK_EQ(s_kbd.c.leave_releases, leaves + 1);
    release();
    CHECK_EQ(count(UI_ACT_KEY_UP, KVM_KEY_A), 1);
    stop();
}

// FR-604: a field of disabled caps is not an explanation. The KEYBOARD view carries the same notice the MOUSE
// view does, and its legends stay readable instead of fading into the page.
static void test_offline_notice_and_disabled_legibility(void)
{
    CHECK(start());
    CHECK(s_kbd.offline != NULL);
    CHECK(lv_obj_is_hidden(s_kbd.offline));

    publish(KVM_HOST_USB_DETACHED);
    lv_obj_update_layout(s_kbd.root);
    CHECK(!lv_obj_is_hidden(s_kbd.offline));
    // The notice floats over the field; it must not push a row out of the key area.
    lv_area_t notice, root;
    lv_obj_get_coords(s_kbd.offline, &notice);
    lv_obj_get_coords(s_kbd.root, &root);
    CHECK(notice.x1 >= root.x1 && notice.x2 <= root.x2);
    CHECK(notice.y1 >= root.y1 && notice.y2 <= root.y2);

    // A disabled cap keeps full coverage and the disabled ink: never a blanket opacity.
    lv_obj_t *a = ui_view_keyboard_key_obj(&s_kbd, index_of_usage(KVM_KEY_A));
    CHECK(lv_obj_is_disabled(a));
    CHECK_EQ(lv_obj_get_style_opa(a, LV_PART_MAIN), LV_OPA_COVER);
    CHECK_EQ(lv_obj_get_style_text_color(a, LV_PART_MAIN).red, (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);

    publish(KVM_HOST_USB_MOUNTED);
    CHECK(lv_obj_is_hidden(s_kbd.offline));
    stop();
}

// FR-207: the F1-F12 layer sends keys like any letter, so its caps wear the enabled `normal` look, not the
// grey modifier look that reads as unavailable.
static void test_fn_caps_look_enabled(void)
{
    CHECK(start());
    click_obj(fn_key_obj());
    CHECK(s_kbd.fn_layer);
    lv_obj_t *f1 = ui_view_keyboard_key_obj(&s_kbd, index_of_usage(KVM_KEY_F1));
    lv_obj_t *q = ui_view_keyboard_key_obj(&s_kbd, index_of_usage(0x14)); // Q, the reference `normal` cap
    CHECK(!lv_obj_is_disabled(f1));
    CHECK_EQ(lv_obj_get_style_bg_color(f1, LV_PART_MAIN).red, lv_obj_get_style_bg_color(q, LV_PART_MAIN).red);
    CHECK_EQ(lv_obj_get_style_text_color(f1, LV_PART_MAIN).red,
             lv_obj_get_style_text_color(q, LV_PART_MAIN).red);
    stop();
}

// AT-UIFW-08: a forced mode switch during KEY_DOWN clears the state and loses no UP.
static void test_navigation_while_held(void)
{
    CHECK(start());
    press_obj(visible_key(KVM_KEY_A));
    CHECK(s_host_down[KVM_KEY_A]);
    const int cancels = s_cancels;
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK_EQ(s_cancels, cancels + 1);
    CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY);
    CHECK(!s_host_down[KVM_KEY_A]);
    release(); // the finger lifts on the new screen
    CHECK_EQ(count(UI_ACT_KEY_UP, KVM_KEY_A), 1);

    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    click_obj(visible_key(KVM_KEY_A)); // still usable after coming back
    CHECK_EQ(count(UI_ACT_KEY_DOWN, KVM_KEY_A), 2);
    CHECK_EQ(s_host_down_n, 0);
    stop();
}

static void test_refused_down(void)
{
    CHECK(start());
    s_refuse = true;
    press_obj(visible_key(KVM_KEY_A));
    CHECK_EQ(s_kbd.held, UI_VIEW_KBD_NO_KEY); // no DOWN went out, so no UP is owed
    s_refuse = false;
    release();
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_kbd.c.refused, 1u);
    CHECK_EQ(s_kbd.c.key_down, 0u);
    CHECK_EQ(s_kbd.c.key_up, 0u);
    stop();
}

// FR-202: a usage is never derived from a legend; every one must be in the allowlisted ranges.
static void test_usages_are_valid(void)
{
    int fn_keys = 0;
    int modifiers = 0;
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        const uint8_t usage = ui_view_keyboard_key_usage(i);
        if (usage == 0u) {
            fn_keys++; // the local keys (F1-F12 and the language cap), never sent to the HOST
            continue;
        }
        if (!usage_is_valid_key(usage) && !kvm_usage_is_modifier(usage)) {
            printf("    cap %u (%s) has usage 0x%02X\n", (unsigned)i, ui_view_keyboard_key_name(i),
                   (unsigned)usage);
        }
        CHECK(usage_is_valid_key(usage) || kvm_usage_is_modifier(usage));
        CHECK(ui_action_valid(kvm_usage_is_modifier(usage) ? UI_ACT_MODIFIER_TOGGLE : UI_ACT_KEY_DOWN, usage));
        modifiers += kvm_usage_is_modifier(usage) ? 1 : 0;
    }
    CHECK_EQ(fn_keys, 2); // ADR-037: F1-F12 and the one language cap
    CHECK_EQ(modifiers, 5); // Shift, Ctrl, Alt, Win, AltGr (FR-201)
    // FR-201 inventory: the named keys must all exist in the table
    static const uint8_t required[] = {
        KVM_KEY_A,      KVM_KEY_Z,         KVM_KEY_1,        KVM_KEY_0,        KVM_KEY_ENTER,
        KVM_KEY_ESCAPE, KVM_KEY_BACKSPACE, KVM_KEY_TAB,      KVM_KEY_SPACE,    KVM_KEY_MINUS,
        KVM_KEY_EQUAL,  KVM_KEY_LEFT_BRACKET, KVM_KEY_RIGHT_BRACKET, KVM_KEY_BACKSLASH,
        KVM_KEY_SEMICOLON, KVM_KEY_APOSTROPHE, KVM_KEY_GRAVE, KVM_KEY_COMMA,   KVM_KEY_PERIOD,
        KVM_KEY_SLASH,  KVM_KEY_CAPS_LOCK, KVM_KEY_F1,       KVM_KEY_F12,      KVM_KEY_INSERT,
        KVM_KEY_HOME,   KVM_KEY_PAGE_UP,   KVM_KEY_DELETE,   KVM_KEY_END,      KVM_KEY_PAGE_DOWN,
        KVM_KEY_RIGHT,  KVM_KEY_LEFT,      KVM_KEY_DOWN,     KVM_KEY_UP,       KVM_KEY_LEFT_CTRL,
        KVM_KEY_LEFT_SHIFT, KVM_KEY_LEFT_ALT, KVM_KEY_LEFT_GUI, KVM_KEY_RIGHT_ALT,
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (index_of_usage(required[i]) == UINT32_MAX) {
            printf("    usage 0x%02X is missing from the layout\n", (unsigned)required[i]);
        }
        CHECK(index_of_usage(required[i]) != UINT32_MAX);
    }
    // all twelve F-keys and the whole alphabet
    for (uint8_t u = KVM_KEY_F1; u <= KVM_KEY_F12; u++) {
        CHECK(index_of_usage(u) != UINT32_MAX);
    }
    for (uint8_t u = KVM_KEY_A; u <= KVM_KEY_Z; u++) {
        CHECK(index_of_usage(u) != UINT32_MAX);
    }
}

static void check_layer_geometry(const char *what)
{
    lv_area_t slot;
    lv_obj_update_layout(s_kbd.root);
    lv_obj_get_coords(s_r.content_slot, &slot);

    lv_area_t box[UI_VIEW_KBD_KEY_CAP];
    uint32_t n = 0;
    int32_t min_w = SCR_W;
    int32_t min_h = SCR_H;
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        lv_obj_t *o = ui_view_keyboard_key_obj(&s_kbd, i);
        // A cap hidden by the geometry family (FR-742) occupies nothing and is not a touch target: the audit is
        // about what the user can actually hit.
        if (o == NULL || lv_obj_is_hidden(o) || lv_obj_is_hidden(lv_obj_get_parent(o))) {
            continue;
        }
        lv_obj_get_coords(o, &box[n]);
        const int32_t w = lv_area_get_width(&box[n]);
        const int32_t h = lv_area_get_height(&box[n]);
        if (w < UI_TOUCH_MIN || h < UI_TOUCH_MIN) {
            printf("    %s: cap %u (%s) is %dx%d\n", what, (unsigned)i, ui_view_keyboard_key_name(i), (int)w,
                   (int)h);
        }
        CHECK(w >= UI_TOUCH_MIN);
        CHECK(h >= UI_TOUCH_MIN);
        if (box[n].x1 < slot.x1 || box[n].x2 > slot.x2 || box[n].y1 < slot.y1 || box[n].y2 > slot.y2) {
            printf("    %s: cap %u (%s) at %d..%d / %d..%d leaves the slot %d..%d / %d..%d\n", what, (unsigned)i,
                   ui_view_keyboard_key_name(i), (int)box[n].x1, (int)box[n].x2, (int)box[n].y1, (int)box[n].y2,
                   (int)slot.x1, (int)slot.x2, (int)slot.y1, (int)slot.y2);
        }
        CHECK(box[n].x1 >= slot.x1 && box[n].x2 <= slot.x2);
        CHECK(box[n].y1 >= slot.y1 && box[n].y2 <= slot.y2);
        min_w = w < min_w ? w : min_w;
        min_h = h < min_h ? h : min_h;
        n++;
    }
    int overlaps = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            if (box[i].x1 <= box[j].x2 && box[j].x1 <= box[i].x2 && box[i].y1 <= box[j].y2 &&
                box[j].y1 <= box[i].y2) {
                printf("    %s: caps %u and %u overlap\n", what, (unsigned)i, (unsigned)j);
                overlaps++;
            }
        }
    }
    printf("    %s: %u visible caps, smallest %dx%d, %d overlaps\n", what, (unsigned)n, (int)min_w, (int)min_h,
           overlaps);
    CHECK_EQ(overlaps, 0);
    // the whole field minus the hidden layer row and the 8 caps of the other geometry families, not a collapsed row
    CHECK(n + 14u + 8u >= ui_view_keyboard_key_count());
}

static void test_geometry(void)
{
    CHECK(start());
    check_layer_geometry("number row");
    click_obj(fn_key_obj());
    check_layer_geometry("F1-F12 row");
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    pick_lang(UI_KBD_LANG_KO);
    CHECK(s_kbd.hangul);
    check_layer_geometry("KO layer with the preedit strip");
    stop();
}

// ---- HOST layout (FR-741/742/743, ADR-024) --------------------------------------------------
// The tables are generated, so these tests state what the layout *data* says and check that the field draws it;
// they never restate a layout by hand. A tree whose generator has not run ships no layout at all, and then every
// one of them reports that and passes: the field stays US ANSI, which is exactly the documented fallback.
static bool have_layouts(void)
{
    if (kbd_layouts_count_get() == 0u) {
        printf("    no generated layout tables in this tree: FR-741/742 fall back to the US ANSI field\n");
        return false;
    }
    return true;
}

static uint32_t cap_of_slot(kbd_slot_t slot)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        if (ui_view_keyboard_key_slot(i) == slot) {
            return i;
        }
    }
    return UINT32_MAX;
}

/** Every legend of every cap that owns a slot is what the tables carry for it, under one layout. */
static void check_legends(const char *id)
{
    const kbd_layout_t *lay = kbd_layouts_find(id);
    CHECK(lay != NULL);
    if (lay == NULL) {
        return;
    }
    ui_view_keyboard_set_host_layout(&s_kbd, id);
    CHECK(ui_view_keyboard_layout(&s_kbd) == lay);
    CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)lay->geometry);
    int checked = 0, altgr = 0, hidden = 0, fallback = 0;
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        const kbd_slot_t slot = ui_view_keyboard_key_slot(i);
        if (slot == KBD_SLOT_COUNT) {
            continue;
        }
        const kbd_key_t *key = kbd_layouts_key(lay, slot);
        if (key == NULL) {
            // A key this family does not have: hidden, and nothing it could send (FR-742).
            CHECK(!ui_view_keyboard_key_shown(&s_kbd, i));
            hidden++;
            continue;
        }
        CHECK(ui_view_keyboard_key_shown(&s_kbd, i));
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, i), key->usage);
        const char *base = kbd_layouts_legend(key, 0);
        const char *shift = kbd_layouts_legend(key, 1);
        const char *l0 = ui_view_keyboard_key_legend(&s_kbd, i, 0);
        const char *l1 = ui_view_keyboard_key_legend(&s_kbd, i, 1);
        const char *l2 = ui_view_keyboard_key_legend(&s_kbd, i, 2);
        CHECK(l0 != NULL && l1 != NULL && l2 != NULL);
        // TECH-078: a legend whose script the shipped fonts leave out is drawn as the replacement character, so
        // every comparison below accepts that one placeholder - and nothing else.
        const char *fb = ui_theme_legend_fallback();
        if (base[0] == '\0' && shift[0] == '\0') {
            CHECK(l0[0] != '\0'); // an IME key keeps the name of the key, never an empty cap
        } else {
            // What is on the cap is one of the two levels, and the corner is the other one or nothing: no legend
            // is ever invented and none of them is a keymap (FR-743).
            CHECK(strcmp(l0, base) == 0 || strcmp(l0, shift) == 0 || strcmp(l0, fb) == 0);
            CHECK(l1[0] == '\0' || strcmp(l1, shift) == 0 || strcmp(l1, fb) == 0);
            fallback += strcmp(l0, fb) == 0;
        }
        CHECK(strcmp(l2, kbd_layouts_legend(key, 2)) == 0 || strcmp(l2, fb) == 0);
        altgr += l2[0] != '\0';
        checked++;
    }
    printf("    %s (%s, geometry %d): %d caps from the tables, %d with an AltGr legend, %d hidden, %d drawn as\n"
           "        the TECH-078 fallback\n", id, lay->name, (int)lay->geometry, checked, altgr, hidden, fallback);
    CHECK(checked >= 40); // every layout defines at least the alphanumeric block
    const char *hint = ui_view_keyboard_hint_text(&s_kbd);
    CHECK(hint != NULL && strstr(hint, "HOST") != NULL); // FR-742: say that the HOST must match
    CHECK(strstr(hint, lay->name) != NULL);
}

static void test_host_layout_default_is_us_ansi(void)
{
    CHECK(start());
    // Nothing selected: the compiled-in field, no hint, and not one cap of another family.
    CHECK(ui_view_keyboard_layout(&s_kbd) == NULL);
    CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_ANSI);
    CHECK_EQ(ui_view_keyboard_hint_text(&s_kbd)[0], '\0');
    CHECK(!ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_LSGT)));
    CHECK(!ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_AE13)));
    CHECK(!ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_HNGL)));
    const uint32_t q = cap_of_slot(KBD_SLOT_AD01);
    CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, q, 0), "Q") == 0);
    CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, q), 0x14u);
    stop();
}

static void test_host_layout_slots_are_unique(void)
{
    // The cap -> slot map is by usage: two caps must never claim the same slot, and every slot a cap claims must
    // be one the tables know.
    int seen[KBD_SLOT_COUNT] = {0};
    int slotted = 0;
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        const kbd_slot_t s = ui_view_keyboard_key_slot(i);
        if (s == KBD_SLOT_COUNT) {
            continue;
        }
        CHECK_EQ(seen[s], 0);
        seen[s] = 1;
        slotted++;
    }
    printf("    %d of %u caps own a key slot\n", slotted, (unsigned)ui_view_keyboard_key_count());
    CHECK_EQ(slotted, 55); // every slot except SPCE, which no layout changes
}

static void test_host_layout_us(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("us");
        const uint32_t q = cap_of_slot(KBD_SLOT_AD01);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, q, 0), "Q") == 0); // a letter cap prints one legend
        CHECK_EQ(ui_view_keyboard_key_legend(&s_kbd, q, 1)[0], '\0');
        const uint32_t t = cap_of_slot(KBD_SLOT_TLDE);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, t, 0), "`") == 0);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, t, 1), "~") == 0);
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_ANSI);
        check_layer_geometry("us ANSI");
    }
    stop();
}

static void test_host_layout_iso(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("de");
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_ISO);
        const uint32_t lsgt = cap_of_slot(KBD_SLOT_LSGT);
        CHECK(ui_view_keyboard_key_shown(&s_kbd, lsgt));
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, lsgt), KVM_KEY_NON_US_BACKSLASH);
        // ISO Enter: the `#` cap sits in the home row, next to Enter, and sends 0x32 instead of 0x31.
        const uint32_t bksl = cap_of_slot(KBD_SLOT_BKSL);
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, bksl), KVM_KEY_NON_US_HASH);
        CHECK(lv_obj_get_parent(ui_view_keyboard_key_obj(&s_kbd, bksl)) == s_kbd.row_mid);
        // An AltGr level the layout has is on the cap (FR-742): German AD01 is q / Q / @.
        const uint32_t q = cap_of_slot(KBD_SLOT_AD01);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, q, 2), "@") == 0);
        check_layer_geometry("de ISO");
        // ... and ANSI puts the cap back where it was.
        ui_view_keyboard_set_host_layout(&s_kbd, "us");
        CHECK(lv_obj_get_parent(ui_view_keyboard_key_obj(&s_kbd, bksl)) == s_kbd.row_top);
        CHECK(!ui_view_keyboard_key_shown(&s_kbd, lsgt));
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, bksl), KVM_KEY_BACKSLASH);
    }
    stop();
}

static void test_host_layout_altgr(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("fr");
        const kbd_layout_t *fr = kbd_layouts_find("fr");
        CHECK((fr->flags & KBD_LAYOUT_ALTGR) != 0u);
        int with_altgr = 0;
        for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
            with_altgr += ui_view_keyboard_key_legend(&s_kbd, i, 2) != NULL &&
                          ui_view_keyboard_key_legend(&s_kbd, i, 2)[0] != '\0';
        }
        printf("    fr: %d caps show an AltGr legend\n", with_altgr);
        CHECK(with_altgr >= 10);
        // A dead key shows its accent and nothing else happens to it (the HOST latches it, FR-743).
        const kbd_key_t *ad11 = kbd_layouts_key(fr, KBD_SLOT_AD11);
        CHECK((ad11->flags & KBD_KEY_DEAD) != 0u);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, cap_of_slot(KBD_SLOT_AD11), 0),
                     kbd_layouts_legend(ad11, 0)) == 0);
    }
    stop();
}

static void test_host_layout_jis(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("jp");
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_JIS);
        const struct {
            kbd_slot_t slot;
            uint8_t usage;
            const char *name;
        } jis[] = {
            {KBD_SLOT_AE13, 0x89u, "Yen"}, {KBD_SLOT_AB11, 0x87u, "Ro"},
            {KBD_SLOT_MUHE, 0x8Bu, "Muhenkan"}, {KBD_SLOT_HENK, 0x8Au, "Henkan"},
            {KBD_SLOT_HKTG, 0x88u, "Kana"},
        };
        for (size_t j = 0; j < sizeof(jis) / sizeof(jis[0]); j++) {
            const uint32_t i = cap_of_slot(jis[j].slot);
            CHECK(i != UINT32_MAX);
            CHECK(ui_view_keyboard_key_shown(&s_kbd, i));
            // The usage comes from the tables and is the one this file hardcodes for the cap: a disagreement is
            // this failing check, not a wrong HID report (FR-743).
            CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, i), jis[j].usage);
            CHECK_EQ(ui_view_keyboard_key_usage(i), jis[j].usage);
        }
        // The three IME caps carry no legend in the tables and keep their names.
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, cap_of_slot(KBD_SLOT_HENK), 0), "Henkan") == 0);
        check_layer_geometry("jp JIS");
        // Every reachable JIS cap gives DOWN then UP with the layout's usage.
        memset(s_tapped, 0, sizeof(s_tapped));
        int tapped = 0;
        tap_all_visible_live(&tapped);
        printf("    jp JIS: %d caps tapped\n", tapped);
        // 5 modifiers + F1-F12 + the language cap + the 13 caps of the hidden F-row + the 3 caps of the families
        // this one does not have (LSGT, Hangul, Hanja).
        CHECK_EQ(tapped, (int)ui_view_keyboard_key_count() - 23);
    }
    stop();
}

static void test_host_layout_abnt2_and_kr(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("br");
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_ABNT2);
        CHECK(ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_LSGT)));
        CHECK(ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_AB11)));
        CHECK(!ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_AE13)));
        check_layer_geometry("br ABNT2");

        check_legends("kr");
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_KR);
        const uint32_t hngl = cap_of_slot(KBD_SLOT_HNGL);
        const uint32_t hjcv = cap_of_slot(KBD_SLOT_HJCV);
        CHECK(ui_view_keyboard_key_shown(&s_kbd, hngl));
        CHECK(ui_view_keyboard_key_shown(&s_kbd, hjcv));
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, hngl), 0x90u);
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, hjcv), 0x91u);
        CHECK(!ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_LSGT)));
        check_layer_geometry("kr");
    }
    stop();
}

static void test_host_layout_cyrillic_and_best_effort(void)
{
    CHECK(start());
    if (have_layouts()) {
        check_legends("ru");
        // A Cyrillic letter cap prints one legend, like a Latin one: the case rule is not ASCII-only.
        const uint32_t ad01 = cap_of_slot(KBD_SLOT_AD01);
        const kbd_key_t *key = kbd_layouts_key(kbd_layouts_find("ru"), KBD_SLOT_AD01);
        CHECK(strcmp(ui_view_keyboard_key_legend(&s_kbd, ad01, 0), kbd_layouts_legend(key, 1)) == 0);
        CHECK_EQ(ui_view_keyboard_key_legend(&s_kbd, ad01, 1)[0], '\0');
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, ad01), 0x14u); // the usage is still the position
    }
    stop();
}

static void test_host_layout_script_without_glyphs(void)
{
    CHECK(start());
    if (have_layouts()) {
        // TECH-078: a layout from a script the shipped fonts leave out - the generator marks these best-effort,
        // because they also need shaping LVGL does not do. Every cap of its own script shows the replacement
        // character, no cap is empty, and the usages are untouched (FR-743).
        const char *id = NULL;
        for (size_t i = 0; i < kbd_layouts_count_get() && id == NULL; i++) {
            const kbd_layout_t *l = kbd_layouts_at(i);
            if ((l->flags & KBD_LAYOUT_BEST_EFFORT) != 0u) {
                id = l->id;
            }
        }
        CHECK(id != NULL);
        check_legends(id);
        const char *fb = ui_theme_legend_fallback();
        int placeholders = 0, empty = 0;
        for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
            if (ui_view_keyboard_key_slot(i) == KBD_SLOT_COUNT || !ui_view_keyboard_key_shown(&s_kbd, i)) {
                continue;
            }
            const char *l0 = ui_view_keyboard_key_legend(&s_kbd, i, 0);
            placeholders += strcmp(l0, fb) == 0;
            empty += l0[0] == '\0';
        }
        printf("    %s: %d caps show the fallback, %d are empty\n", id, placeholders, empty);
        CHECK_EQ(empty, 0);
        CHECK(placeholders > 0 || !ui_theme_has_legend_font());
        const uint32_t a = cap_of_slot(KBD_SLOT_AD01);
        CHECK_EQ(ui_view_keyboard_key_live_usage(&s_kbd, a), 0x14u);
    }
    stop();
}

static void test_host_layout_unknown_id_changes_nothing(void)
{
    CHECK(start());
    if (have_layouts()) {
        ui_view_keyboard_set_host_layout(&s_kbd, "de");
        const uint32_t before = s_kbd.c.relabels;
        ui_view_keyboard_set_host_layout(&s_kbd, "zz(nope)");
        CHECK_EQ(s_kbd.c.layout_unknown, 1u);
        CHECK_EQ(s_kbd.c.relabels, before); // nothing was redrawn, nothing invented
        CHECK(strcmp(ui_view_keyboard_layout(&s_kbd)->id, "de") == 0);
        CHECK(strcmp(ui_view_keyboard_host_layout(&s_kbd), "zz(nope)") == 0); // the id is remembered as it came
        // And back to no layout at all.
        ui_view_keyboard_set_host_layout(&s_kbd, NULL);
        CHECK(ui_view_keyboard_layout(&s_kbd) == NULL);
        CHECK_EQ(ui_view_keyboard_hint_text(&s_kbd)[0], '\0');
    }
    stop();
}

static void test_host_layout_sends_the_layout_usage(void)
{
    CHECK(start());
    if (have_layouts()) {
        // `de` is the one case where a cap's usage differs from the US field: BKSL is 0x32 on ISO.
        ui_view_keyboard_set_host_layout(&s_kbd, "de");
        lv_obj_t *o = ui_view_keyboard_key_obj(&s_kbd, cap_of_slot(KBD_SLOT_BKSL));
        s_rec_n = 0;
        click_obj(o);
        CHECK_EQ(s_rec_n, 2);
        CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
        CHECK_EQ(s_rec[0].value, KVM_KEY_NON_US_HASH);
        CHECK_EQ(s_rec[1].type, UI_ACT_KEY_UP);
        CHECK_EQ(s_rec[1].value, KVM_KEY_NON_US_HASH);
        // A held key keeps the usage it went down with even when the layout changes under it (AGENTS.md 3).
        press_obj(o);
        CHECK_EQ(s_kbd.c.key_down, 2u);
        ui_view_keyboard_set_host_layout(&s_kbd, "us");
        s_rec_n = 0;
        release();
        CHECK_EQ(s_rec_n, 1);
        CHECK_EQ(s_rec[0].type, UI_ACT_KEY_UP);
        CHECK_EQ(s_rec[0].value, KVM_KEY_BACKSLASH);
    }
    stop();
}

static void test_host_layout_before_the_view_exists(void)
{
    // Settings may publish the layout before the view is ever built: the field must come up drawn for it.
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    s_store_on = true;
    ui_actions_init(&s_act);
    ui_view_keyboard_init(&s_kbd, &s_store, &s_act, fake_now);
    ui_view_keyboard_set_host_layout(&s_kbd, "de");
    s_desc[0] = s_kbd.desc;
    s_desc[1] = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = other_create};
    s_scr = lv_obj_create(NULL);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    CHECK(ui_router_init(&s_r, s_scr, &lay, &s_store, s_desc, 2, NULL, 0, &k_hooks));
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    publish(KVM_HOST_USB_MOUNTED);
    lv_test_wait(20);
    if (have_layouts()) {
        CHECK_EQ((int)ui_view_keyboard_geometry(&s_kbd), (int)KBD_GEOM_ISO);
        CHECK(ui_view_keyboard_key_shown(&s_kbd, cap_of_slot(KBD_SLOT_LSGT)));
        CHECK(ui_view_keyboard_hint_text(&s_kbd)[0] != '\0');
    }
    stop();
}

// ---- KO layer (ADR-024 FR-746/747/748) ------------------------------------------------------
static int count_type(ui_action_type_t t)
{
    int n = 0;
    for (int i = 0; i < s_rec_n; i++) {
        n += s_rec[i].type == t;
    }
    return n;
}

static void test_ko_needs_text_mode(void)
{
    CHECK(start());
    lv_obj_t *key = lang_key_obj();
    CHECK(key != NULL);
    // host-ime (the default): the HOST's own IME composes, so no composed language can be selected.
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_KO));
    CHECK(strcmp(ui_view_keyboard_lang_reason(&s_kbd, UI_KBD_LANG_KO), "Needs a Unicode delivery mode") == 0);
    tap_lang(); // the cap is live, but the cycle finds nothing to switch to
    CHECK(!s_kbd.hangul);
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_kbd.c.lang_refused, 1u);
    CHECK(strcmp(lang_key_text(), "EN") == 0);
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    CHECK(ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_KO));
    CHECK(ui_view_keyboard_lang_reason(&s_kbd, UI_KBD_LANG_KO) == NULL);
    publish(KVM_HOST_USB_DETACHED);
    CHECK(lv_obj_is_disabled(key)); // the whole field, the language cap with it
    stop();
}

static void test_ko_compose(void)
{
    CHECK(start());
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    lv_obj_t *key = lang_key_obj();
    CHECK(lv_obj_is_hidden(s_kbd.row_compose));
    pick_lang(UI_KBD_LANG_KO);
    CHECK(s_kbd.hangul);
    CHECK(lv_obj_is_checked(key));
    CHECK(strcmp(lang_key_text(), "KO") == 0);
    CHECK(!lv_obj_is_hidden(s_kbd.row_compose));
    CHECK_EQ(s_rec_n, 1); // FR-748: armed modifiers and queued keys are cleared before any text
    CHECK_EQ(s_rec[0].type, UI_ACT_TEXT_CANCEL);

    // modifiers other than Shift and CapsLock are disabled in the layer
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_CTRL)));
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_ALT)));
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_GUI)));
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_RIGHT_ALT)));
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_CAPS_LOCK)));
    CHECK(!lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_SHIFT)));

    // "dkssud" = 안녕: the first syllable is committed by the second ㄴ, the second stays in the preedit
    s_rec_n = 0;
    type_letters("dkssud");
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].type, UI_ACT_TEXT_CHAR);
    CHECK_EQ(s_rec[0].value, 0xC548u); // 안
    CHECK_EQ(kvm_hangul_preedit(&s_kbd.ime), 0xB155u); // 녕
    CHECK(strcmp(lv_label_get_text(s_kbd.preedit), "\xEB\x85\x95") == 0);
    // TECH-078: the strip uses the Hangul subset; Hangul comes from it, Latin from its Montserrat fallback
    const lv_font_t *hf = ui_theme_font(UI_FONT_HANGUL);
    CHECK(lv_obj_get_style_text_font(s_kbd.preedit, LV_PART_MAIN) == hf);
    lv_font_glyph_dsc_t g;
    CHECK(lv_font_get_glyph_dsc(hf, &g, 0xB155u, 0u));
    CHECK(g.resolved_font == hf);
    CHECK(lv_font_get_glyph_dsc(hf, &g, 0x3132u, 0u)); // a lone jamo
    CHECK(g.resolved_font == hf);
    CHECK(lv_font_get_glyph_dsc(hf, &g, 'A', 0u));
    // The Hangul subset's own fallback is Montserrat 24, which is what it was generated against; the legend
    // fonts of FR-742 sit on the text roles and do not join this chain.
    CHECK(g.resolved_font == &lv_font_montserrat_24);

    // Backspace edits the composition locally
    click_obj(visible_key(KVM_KEY_BACKSPACE));
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(kvm_hangul_preedit(&s_kbd.ime), 0xB140u); // 녀

    // Space commits, then goes as a key, in that order
    click_obj(visible_key(KVM_KEY_SPACE));
    CHECK_EQ(s_rec_n, 3);
    CHECK_EQ(s_rec[1].type, UI_ACT_TEXT_CHAR);
    CHECK_EQ(s_rec[1].value, 0xB140u);
    CHECK_EQ(s_rec[2].type, UI_ACT_TEXT_KEY);
    CHECK_EQ(s_rec[2].value, KVM_KEY_SPACE);
    CHECK_EQ(kvm_hangul_preedit(&s_kbd.ime), 0u);
    CHECK(strcmp(lv_label_get_text(s_kbd.preedit), "") == 0);

    // Shift is a one-tap local jamo shift: Shift+r = ㄲ, then back to plain
    click_obj(visible_key(KVM_KEY_LEFT_SHIFT));
    CHECK(s_kbd.jamo_shift);
    CHECK_EQ(s_rec_n, 3); // never a MODIFIER_TOGGLE
    type_letters("r");
    CHECK(!s_kbd.jamo_shift);
    CHECK_EQ(kvm_hangul_preedit(&s_kbd.ime), 0x3132u); // ㄲ

    // back to English commits what is on screen. With KO in effect the cycle always finds English, so a tap
    // is enough and the switch semantics are the same as the picker's.
    tap_lang();
    CHECK(!s_kbd.hangul);
    CHECK(strcmp(lang_key_text(), "EN") == 0);
    CHECK(!lv_obj_is_checked(key));
    CHECK(lv_obj_is_hidden(s_kbd.row_compose));
    CHECK_EQ(s_rec_n, 4);
    CHECK_EQ(s_rec[3].type, UI_ACT_TEXT_CHAR);
    CHECK_EQ(s_rec[3].value, 0x3132u);
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0); // the layer never sends an ordinary key
    CHECK_EQ(count_type(UI_ACT_MODIFIER_TOGGLE), 0);
    CHECK_EQ(s_kbd.c.text_chars, 3u);
    CHECK_EQ(s_kbd.c.text_keys, 1u);
    CHECK(!lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_CTRL)));

    // back to ordinary keys
    s_rec_n = 0;
    click_obj(visible_key(KVM_KEY_A));
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[0].type, UI_ACT_KEY_DOWN);
    stop();
}

static void test_ko_leave_and_host_ime_discard(void)
{
    CHECK(start());
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    pick_lang(UI_KBD_LANG_KO);
    s_rec_n = 0;
    type_letters("r"); // ㄱ in the preedit
    CHECK_EQ(s_rec_n, 0);
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK_EQ(count_type(UI_ACT_TEXT_CANCEL), 1); // discarded, never half-sent
    CHECK_EQ(count_type(UI_ACT_TEXT_CHAR), 0);
    CHECK_EQ(kvm_hangul_preedit(&s_kbd.ime), 0u);
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));

    s_rec_n = 0;
    CHECK(s_kbd.hangul);
    type_letters("r");
    ui_view_keyboard_set_text_mode(&s_kbd, false); // host-ime: the layer cannot be carried any more
    CHECK(!s_kbd.hangul);
    CHECK_EQ(count_type(UI_ACT_TEXT_CANCEL), 1);
    CHECK_EQ(count_type(UI_ACT_TEXT_CHAR), 0);
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_KO));
    CHECK(strcmp(lang_key_text(), "EN") == 0);
    CHECK(lv_obj_is_hidden(s_kbd.row_compose));
    stop();
}

// ---- ZH layer (ADR-024 FR-746/748) ------------------------------------------------------------
// Fake decoder: every letter but 'v' is one syllable whose hanzi is U+4E00 + (letter - 'a'); input parsing stops
// at the first 'v'. With syllables left: candidate 0 = the whole sentence, 1 = the next syllable, 2 = the next two
// (or a filler when one is left), 3..9 = fillers U+5000 + i of one syllable. All fixed: candidate 0 only.
typedef struct {
    char in[UI_VIEW_KBD_INPUT_CAP + 1u];
    size_t len;
    size_t pos[UI_VIEW_KBD_INPUT_CAP]; // input position of each syllable
    size_t nsyl;
    size_t fixed;
    size_t stack[UI_VIEW_KBD_INPUT_CAP + 1u];
    size_t depth;
    unsigned searches;
    unsigned resets;
} fake_zh_t;

static fake_zh_t s_fz;

static size_t fz_count(const fake_zh_t *f)
{
    return f->nsyl == 0u ? 0u : (f->fixed >= f->nsyl ? 1u : 10u);
}

static size_t fz_search(void *ctx, const char *s, size_t len)
{
    fake_zh_t *f = ctx;
    f->searches++;
    memset(f->in, 0, sizeof(f->in));
    memcpy(f->in, s, len < UI_VIEW_KBD_INPUT_CAP ? len : UI_VIEW_KBD_INPUT_CAP);
    f->len = len;
    f->nsyl = 0;
    for (size_t i = 0; i < len && s[i] != 'v'; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            f->pos[f->nsyl++] = i;
        }
    }
    f->fixed = 0;
    f->depth = 0;
    return fz_count(f);
}

static uint16_t fz_hanzi(const fake_zh_t *f, size_t syl)
{
    return (uint16_t)(0x4E00u + (uint16_t)(f->in[f->pos[syl]] - 'a'));
}

static size_t fz_candidate(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    fake_zh_t *f = ctx;
    size_t n = 0;
    if (i >= fz_count(f) || cap < 2u) {
        return 0;
    }
    const size_t left = f->nsyl - f->fixed;
    if (i == 0u) {
        for (size_t s = 0; s < f->nsyl && n + 1u < cap; s++) {
            out[n++] = fz_hanzi(f, s);
        }
    } else if (i == 1u) {
        out[n++] = fz_hanzi(f, f->fixed);
    } else if (i == 2u && left >= 2u) {
        out[n++] = fz_hanzi(f, f->fixed);
        out[n++] = fz_hanzi(f, f->fixed + 1u);
    } else {
        out[n++] = (uint16_t)(0x5000u + i);
    }
    out[n] = 0;
    return n;
}

static size_t fz_choose(void *ctx, size_t i)
{
    fake_zh_t *f = ctx;
    if (i >= fz_count(f) || f->fixed >= f->nsyl) {
        return fz_count(f);
    }
    f->stack[f->depth++] = f->fixed;
    const size_t left = f->nsyl - f->fixed;
    f->fixed = i == 0u ? f->nsyl : f->fixed + ((i == 2u && left >= 2u) ? 2u : 1u);
    return fz_count(f);
}

static size_t fz_cancel_last(void *ctx)
{
    fake_zh_t *f = ctx;
    if (f->depth > 0u) {
        f->fixed = f->stack[--f->depth];
    }
    return fz_count(f);
}

static size_t fz_fixed_len(void *ctx)
{
    return ((fake_zh_t *)ctx)->fixed;
}

static size_t fz_parsed(void *ctx)
{
    const fake_zh_t *f = ctx;
    return f->nsyl == 0u ? 0u : f->pos[f->nsyl - 1u] + 1u;
}

static size_t fz_consumed(void *ctx)
{
    const fake_zh_t *f = ctx;
    if (f->fixed == 0u) {
        return 0;
    }
    return f->fixed < f->nsyl ? f->pos[f->fixed] : fz_parsed(ctx);
}

static bool fz_complete(void *ctx)
{
    const fake_zh_t *f = ctx;
    return f->nsyl > 0u && f->fixed >= f->nsyl;
}

static void fz_reset(void *ctx)
{
    fake_zh_t *f = ctx;
    f->resets++;
    f->len = f->nsyl = f->fixed = f->depth = 0;
}

static const ui_kbd_conv_engine_t k_fake_zh = {
    .ctx = &s_fz,
    .search = fz_search,
    .candidate = fz_candidate,
    .choose = fz_choose,
    .cancel_last = fz_cancel_last,
    .fixed_len = fz_fixed_len,
    .consumed = fz_consumed,
    .parsed = fz_parsed,
    .complete = fz_complete,
    .reset = fz_reset,
};

static lv_obj_t *layer_key(const char *name)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        if (strcmp(ui_view_keyboard_key_name(i), name) == 0) {
            return ui_view_keyboard_key_obj(&s_kbd, i);
        }
    }
    return NULL;
}

static const char *cand_label(uint32_t slot)
{
    lv_obj_t *key = s_kbd.zh_cand[slot];
    return key != NULL ? lv_label_get_text(lv_obj_get_child(key, -1)) : "";
}

static bool rec_is(int i, ui_action_type_t t, uint16_t v)
{
    return i < s_rec_n && s_rec[i].type == t && s_rec[i].value == v;
}

static void zh_on(void)
{
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    ui_view_keyboard_set_zh_engine(&s_kbd, &k_fake_zh);
    pick_lang(UI_KBD_LANG_ZH);
    CHECK(s_kbd.converting);
}

static void test_zh_needs_engine(void)
{
    CHECK(start());
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH)); // host-ime and no decoder
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    // a delivery mode, still no decoder (ime_data not mounted yet)
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH));
    CHECK(strcmp(ui_view_keyboard_lang_reason(&s_kbd, UI_KBD_LANG_ZH), "No pinyin decoder installed") == 0);
    // the picker lists it, disabled, with that reason: nothing is composed and nothing is sent
    hold_lang();
    CHECK(ui_view_keyboard_picker_open(&s_kbd));
    CHECK(lv_obj_is_disabled(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_ZH)));
    click_obj(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_ZH));
    CHECK(ui_view_keyboard_picker_open(&s_kbd)); // a disabled row sends no click: the card stays up
    CHECK(!s_kbd.converting);
    CHECK_EQ(s_rec_n, 0);
    click_obj(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_EN)); // Current: closes, changes nothing
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
    ui_kbd_conv_engine_t partial = k_fake_zh;
    partial.reset = NULL;
    ui_view_keyboard_set_zh_engine(&s_kbd, &partial); // an incomplete table is refused
    CHECK(s_kbd.zh == NULL);
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH));
    ui_view_keyboard_set_zh_engine(&s_kbd, &k_fake_zh);
    CHECK(ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH));
    // the strip font is the theme's hanzi role, re-applied when the decoder (and with it the font) arrives
    CHECK(lv_obj_get_style_text_font(s_kbd.zh_input, LV_PART_MAIN) == ui_theme_font(UI_FONT_HANZI));
    publish(KVM_HOST_USB_DETACHED);
    CHECK(lv_obj_is_disabled(lang_key_obj())); // and never without a HOST
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    stop();
}

static void test_zh_compose(void)
{
    CHECK(start());
    CHECK(lv_obj_is_hidden(s_kbd.row_zh));
    zh_on();
    CHECK(lv_obj_is_checked(lang_key_obj()));
    CHECK(strcmp(lang_key_text(), "ZH") == 0);
    CHECK(!lv_obj_is_hidden(s_kbd.row_zh));
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CANCEL, 0)); // FR-748: cleared before any text
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_CTRL)));
    CHECK(lv_obj_is_disabled(visible_key(KVM_KEY_CAPS_LOCK)));
    CHECK(!lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_SHIFT)));
    for (uint32_t i = 0; i < UI_VIEW_KBD_CANDS; i++) {
        CHECK(lv_obj_is_disabled(s_kbd.zh_cand[i])); // nothing decoded yet
    }

    // "abc": composed locally, nothing sent; six candidates on the first page of ten
    s_rec_n = 0;
    type_letters("abc");
    CHECK_EQ(s_rec_n, 0);
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "abc") == 0);
    CHECK_EQ(s_kbd.zh_count, 10u);
    CHECK(strcmp(cand_label(0), "1 \xE4\xB8\x80\xE4\xB8\x81\xE4\xB8\x82") == 0); // 一丁丂: the sentence
    CHECK(strcmp(cand_label(1), "2 \xE4\xB8\x80") == 0);
    CHECK(strcmp(cand_label(2), "3 \xE4\xB8\x80\xE4\xB8\x81") == 0);
    CHECK(lv_obj_is_disabled(s_kbd.zh_prev));
    CHECK(!lv_obj_is_disabled(s_kbd.zh_next));
    click_obj(s_kbd.zh_next);
    CHECK_EQ(s_kbd.zh_page, UI_VIEW_KBD_CANDS);
    CHECK(strcmp(cand_label(0), "1 \xE5\x80\x86") == 0); // U+5006, candidate 6
    CHECK(lv_obj_is_disabled(s_kbd.zh_cand[4])); // 10 candidates: slots 4 and 5 of page 2 are empty
    CHECK(lv_obj_is_disabled(s_kbd.zh_next));
    click_obj(s_kbd.zh_prev);
    CHECK_EQ(s_kbd.zh_page, 0u);

    // a partial choice keeps the rest decoding; the input shows the fixed hanzi, then the letters left
    click_obj(s_kbd.zh_cand[2]); // 一丁
    CHECK_EQ(s_rec_n, 0);
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "\xE4\xB8\x80\xE4\xB8\x81" "c") == 0);
    CHECK(strcmp(cand_label(0), "1 \xE4\xB8\x82") == 0); // the sentence minus the fixed part
    click_obj(visible_key(KVM_KEY_BACKSPACE)); // undoes the choice, keeps the letters
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "abc") == 0);
    click_obj(visible_key(KVM_KEY_1 + 1)); // digit 2 = candidate 1 (一)
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "\xE4\xB8\x80" "bc") == 0);
    CHECK_EQ(s_rec_n, 0);

    // Space commits the sentence
    click_obj(visible_key(KVM_KEY_SPACE));
    CHECK_EQ(s_rec_n, 3);
    CHECK(rec_is(0, UI_ACT_TEXT_CHAR, 0x4E00) && rec_is(1, UI_ACT_TEXT_CHAR, 0x4E01) &&
          rec_is(2, UI_ACT_TEXT_CHAR, 0x4E02));
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "") == 0);
    CHECK_EQ(s_kbd.zh_count, 0u);
    CHECK_EQ(s_kbd.c.zh_commits, 1u);

    // Enter: the letters as typed; after a partial choice the fixed hanzi first
    s_rec_n = 0;
    type_letters("ab");
    click_obj(visible_key(KVM_KEY_ENTER));
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_KEY, KVM_KEY_A) && rec_is(1, UI_ACT_TEXT_KEY, KVM_KEY_A + 1));
    s_rec_n = 0;
    type_letters("ab");
    click_obj(s_kbd.zh_cand[1]); // 一
    click_obj(visible_key(KVM_KEY_ENTER));
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_CHAR, 0x4E00) && rec_is(1, UI_ACT_TEXT_KEY, KVM_KEY_A + 1));

    // Esc discards; Backspace with one letter left ends the composition without a record
    s_rec_n = 0;
    type_letters("a");
    click_obj(visible_key(KVM_KEY_ESCAPE));
    type_letters("a");
    click_obj(visible_key(KVM_KEY_BACKSPACE));
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_kbd.py_len, 0u);

    // letters the decoder cannot segment follow the sentence as typed
    type_letters("abv");
    click_obj(visible_key(KVM_KEY_SPACE));
    CHECK(s_rec_n == 3 && rec_is(0, UI_ACT_TEXT_CHAR, 0x4E00) && rec_is(1, UI_ACT_TEXT_CHAR, 0x4E01) &&
          rec_is(2, UI_ACT_TEXT_KEY, KVM_KEY_A + ('v' - 'a')));

    // nothing composed: Chinese punctuation, Shift as a one-shot local Shift, other keys as keys
    s_rec_n = 0;
    click_obj(visible_key(KVM_KEY_COMMA));
    click_obj(visible_key(KVM_KEY_PERIOD));
    click_obj(visible_key(KVM_KEY_LEFT_SHIFT));
    CHECK(s_kbd.zh_shift);
    click_obj(visible_key(KVM_KEY_SLASH));
    CHECK(!s_kbd.zh_shift);
    click_obj(visible_key(KVM_KEY_LEFT_SHIFT));
    type_letters("a"); // Shift+letter with nothing composed: a Latin capital, not pinyin
    click_obj(visible_key(KVM_KEY_BACKSPACE));
    CHECK_EQ(s_rec_n, 5);
    CHECK(rec_is(0, UI_ACT_TEXT_CHAR, 0xFF0C) && rec_is(1, UI_ACT_TEXT_CHAR, 0x3002) &&
          rec_is(2, UI_ACT_TEXT_CHAR, 0xFF1F) && rec_is(3, UI_ACT_TEXT_KEY, KVM_KEY_A | UI_TEXT_KEY_SHIFT) &&
          rec_is(4, UI_ACT_TEXT_KEY, KVM_KEY_BACKSPACE));
    CHECK_EQ(s_kbd.py_len, 0u);

    // another key while composing: the sentence first, then the key
    s_rec_n = 0;
    type_letters("a");
    click_obj(visible_key(KVM_KEY_COMMA));
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_CHAR, 0x4E00) && rec_is(1, UI_ACT_TEXT_CHAR, 0xFF0C));

    // the syllable separator never doubles; the input stops at 26 characters
    s_rec_n = 0;
    type_letters("a");
    click_obj(visible_key(KVM_KEY_APOSTROPHE));
    click_obj(visible_key(KVM_KEY_APOSTROPHE));
    CHECK(strcmp(s_kbd.py, "a'") == 0);
    type_letters("bcdefghijklmnopqrstuwxyzabcdef");
    CHECK_EQ(s_kbd.py_len, UI_VIEW_KBD_PINYIN_CAP);
    CHECK_EQ(s_rec_n, 0);
    click_obj(visible_key(KVM_KEY_ESCAPE));

    // switching back to English commits what is on screen
    type_letters("ab");
    pick_lang(UI_KBD_LANG_EN);
    CHECK(!s_kbd.converting);
    CHECK(lv_obj_is_hidden(s_kbd.row_zh));
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_KEY, KVM_KEY_A) && rec_is(1, UI_ACT_TEXT_KEY, KVM_KEY_A + 1));
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0);
    CHECK_EQ(count_type(UI_ACT_MODIFIER_TOGGLE), 0);
    CHECK(!lv_obj_is_disabled(visible_key(KVM_KEY_LEFT_CTRL)));
    s_rec_n = 0;
    click_obj(visible_key(KVM_KEY_A)); // back to ordinary keys
    CHECK(rec_is(0, UI_ACT_KEY_DOWN, KVM_KEY_A));
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    stop();
}

static void test_zh_exclusive_and_discard(void)
{
    CHECK(start());
    zh_on();
    s_rec_n = 0;
    type_letters("a");
    pick_lang(UI_KBD_LANG_KO); // ZH -> KO: its text is committed and not cancelled afterwards
    CHECK(s_kbd.hangul && !s_kbd.converting);
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_KEY, KVM_KEY_A));
    type_letters("r"); // ㄱ in the KO preedit
    pick_lang(UI_KBD_LANG_ZH); // KO -> ZH: the same
    CHECK(s_kbd.converting && !s_kbd.hangul);
    CHECK(s_rec_n == 2 && rec_is(1, UI_ACT_TEXT_CHAR, 0x3131));
    CHECK(lv_obj_is_hidden(s_kbd.row_compose));
    CHECK(!lv_obj_is_hidden(s_kbd.row_zh));

    // leaving the view discards the composition
    s_rec_n = 0;
    type_letters("ab");
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK_EQ(count_type(UI_ACT_TEXT_CANCEL), 1);
    CHECK_EQ(count_type(UI_ACT_TEXT_KEY) + count_type(UI_ACT_TEXT_CHAR), 0);
    CHECK_EQ(s_kbd.py_len, 0u);
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK(s_kbd.converting);

    // the HOST goes away: nothing waits for it
    type_letters("ab");
    publish(KVM_HOST_USB_DETACHED);
    CHECK_EQ(s_kbd.py_len, 0u);
    publish(KVM_HOST_USB_MOUNTED);

    // host-ime cannot carry the layer: off, discarded, TEXT_CANCEL
    s_rec_n = 0;
    type_letters("ab");
    ui_view_keyboard_set_text_mode(&s_kbd, false);
    CHECK(!s_kbd.converting);
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CANCEL, 0));
    CHECK(lv_obj_is_hidden(s_kbd.row_zh));
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH));

    // the decoder goes away (ime_data unmounted): the same
    zh_on();
    s_rec_n = 0;
    type_letters("ab");
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    CHECK(!s_kbd.converting);
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CANCEL, 0));
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_ZH));
    stop();
}

static void test_zh_strip_geometry(void)
{
    CHECK(start());
    zh_on();
    type_letters("abc");
    lv_obj_update_layout(s_kbd.root);
    lv_area_t slot;
    lv_obj_get_coords(s_r.content_slot, &slot);
    lv_obj_t *keys[UI_VIEW_KBD_CANDS + 2u];
    memcpy(keys, s_kbd.zh_cand, sizeof(s_kbd.zh_cand));
    keys[UI_VIEW_KBD_CANDS] = s_kbd.zh_prev;
    keys[UI_VIEW_KBD_CANDS + 1u] = s_kbd.zh_next;
    for (uint32_t i = 0; i < UI_VIEW_KBD_CANDS + 2u; i++) {
        lv_area_t a;
        lv_obj_get_coords(keys[i], &a);
        CHECK(lv_area_get_width(&a) >= UI_TOUCH_MIN && lv_area_get_height(&a) >= UI_TOUCH_MIN);
        CHECK(a.x1 >= slot.x1 && a.x2 <= slot.x2 && a.y1 >= slot.y1 && a.y2 <= slot.y2);
    }
    check_layer_geometry("ZH layer with the candidate strip");
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    stop();
}

// ---- JA layer (FR-746, romaji -> kana -> kanji) ----------------------------------------------------
// The ZH fake with a reading: each parsed, unfixed letter reads as the hiragana U+3041 + (letter - 'a').
static size_t fz_preedit(void *ctx, uint16_t *out, size_t cap)
{
    const fake_zh_t *f = ctx;
    size_t n = 0;
    for (size_t s = f->fixed; s < f->nsyl && n + 1u < cap; s++) {
        out[n++] = (uint16_t)(0x3041u + (uint16_t)(f->in[f->pos[s]] - 'a'));
    }
    out[n] = 0;
    return n;
}

static ui_kbd_conv_engine_t s_fake_ja;

static void test_ja_layer(void)
{
    CHECK(start());
    s_fake_ja = k_fake_zh;
    s_fake_ja.preedit = fz_preedit;
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    ui_view_keyboard_set_zh_engine(&s_kbd, &k_fake_zh);
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_JA)); // the ZH decoder does not enable JA
    CHECK(strcmp(ui_view_keyboard_lang_reason(&s_kbd, UI_KBD_LANG_JA), "No Japanese decoder installed") == 0);
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, &s_fake_ja);
    CHECK(ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_JA));
    ui_view_keyboard_set_conv_engine(&s_kbd, 7u, &s_fake_ja); // no such layer: ignored
    pick_lang(UI_KBD_LANG_JA);
    CHECK(s_kbd.converting && s_kbd.conv_layer == UI_KBD_LAYER_JA && s_kbd.conv == &s_fake_ja);
    CHECK(lv_obj_is_checked(lang_key_obj()) && strcmp(lang_key_text(), "JA") == 0);
    CHECK(lv_obj_get_style_text_font(s_kbd.zh_input, LV_PART_MAIN) == ui_theme_font(UI_FONT_KANJI));
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CANCEL, 0));

    // the input shows the reading (kana), not the romaji; a partial choice puts the fixed text first
    s_rec_n = 0;
    type_letters("abc");
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "\xE3\x81\x81\xE3\x81\x82\xE3\x81\x83") == 0); // ぁあぃ
    click_obj(s_kbd.zh_cand[1]); // 一
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "\xE4\xB8\x80\xE3\x81\x82\xE3\x81\x83") == 0); // 一あぃ
    CHECK_EQ(s_rec_n, 0);

    click_obj(visible_key(KVM_KEY_ENTER)); // Enter: the fixed text, then the reading
    CHECK(s_rec_n == 3 && rec_is(0, UI_ACT_TEXT_CHAR, 0x4E00) && rec_is(1, UI_ACT_TEXT_CHAR, 0x3042) &&
          rec_is(2, UI_ACT_TEXT_CHAR, 0x3043));

    // '-' is input (a new search, as every letter); letters the decoder did not parse follow as keys
    s_rec_n = 0;
    type_letters("ab");
    click_obj(visible_key(KVM_KEY_MINUS));
    CHECK(strcmp(s_kbd.py, "ab-") == 0);
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "\xE3\x81\x81\xE3\x81\x82-") == 0);
    click_obj(visible_key(KVM_KEY_ENTER));
    CHECK_EQ(s_rec_n, 3);
    CHECK(rec_is(0, UI_ACT_TEXT_CHAR, 0x3041) && rec_is(1, UI_ACT_TEXT_CHAR, 0x3042) &&
          rec_is(2, UI_ACT_TEXT_KEY, KVM_KEY_MINUS));
    CHECK_EQ(s_kbd.py_len, 0u);

    // nothing composed: Japanese punctuation
    s_rec_n = 0;
    click_obj(visible_key(KVM_KEY_COMMA));
    click_obj(visible_key(KVM_KEY_PERIOD));
    click_obj(visible_key(KVM_KEY_SLASH));
    click_obj(visible_key(KVM_KEY_LEFT_BRACKET));
    click_obj(visible_key(KVM_KEY_RIGHT_BRACKET));
    click_obj(visible_key(KVM_KEY_LEFT_SHIFT));
    click_obj(visible_key(KVM_KEY_SLASH));
    CHECK_EQ(s_rec_n, 6);
    CHECK(rec_is(0, UI_ACT_TEXT_CHAR, 0x3001) && rec_is(1, UI_ACT_TEXT_CHAR, 0x3002) &&
          rec_is(2, UI_ACT_TEXT_CHAR, 0x30FB) && rec_is(3, UI_ACT_TEXT_CHAR, 0x300C) &&
          rec_is(4, UI_ACT_TEXT_CHAR, 0x300D) && rec_is(5, UI_ACT_TEXT_CHAR, 0xFF1F));

    // JA -> ZH commits the reading and switches decoder and font without a TEXT_CANCEL in between
    s_rec_n = 0;
    type_letters("ab");
    pick_lang(UI_KBD_LANG_ZH);
    CHECK(s_kbd.converting && s_kbd.conv_layer == UI_KBD_LAYER_ZH && s_kbd.conv == &k_fake_zh);
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_CHAR, 0x3041) && rec_is(1, UI_ACT_TEXT_CHAR, 0x3042));
    CHECK(lv_obj_get_style_text_font(s_kbd.zh_input, LV_PART_MAIN) == ui_theme_font(UI_FONT_HANZI));
    type_letters("ab");
    CHECK(strcmp(lv_label_get_text(s_kbd.zh_input), "ab") == 0); // ZH shows the letters
    click_obj(visible_key(KVM_KEY_MINUS)); // not part of a pinyin input: the sentence, then the key
    CHECK_EQ(s_kbd.py_len, 0u);
    CHECK_EQ(count_type(UI_ACT_TEXT_CANCEL), 0);

    // removing the JA decoder while ZH is on leaves ZH alone; while JA is on it discards
    s_rec_n = 0;
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, NULL);
    CHECK(s_kbd.converting && s_kbd.conv_layer == UI_KBD_LAYER_ZH);
    CHECK(!ui_view_keyboard_lang_available(&s_kbd, UI_KBD_LANG_JA));
    CHECK_EQ(s_rec_n, 0);
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, &s_fake_ja);
    pick_lang(UI_KBD_LANG_JA);
    type_letters("ab");
    s_rec_n = 0;
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, NULL);
    CHECK(!s_kbd.converting && s_kbd.conv == NULL);
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CANCEL, 0));
    CHECK(lv_obj_is_hidden(s_kbd.row_zh));
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    stop();
}

// ---- JA flick pad (FR-746) ------------------------------------------------------------------------------------------
static lv_obj_t *pad_kana(uint32_t q)
{
    return s_kbd.flick_key[(q / 3u) * 4u + q % 3u];
}

static lv_obj_t *pad_fn(uint32_t row)
{
    return s_kbd.flick_key[row * 4u + 3u];
}

/** Press the middle of `o`, slide by (dx, dy) in steps, release. */
static void flick_obj(lv_obj_t *o, int32_t dx, int32_t dy)
{
    lv_area_t a;
    CHECK(o != NULL);
    if (o == NULL) {
        return;
    }
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    const int32_t x = (a.x1 + a.x2) / 2;
    const int32_t y = (a.y1 + a.y2) / 2;
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(50);
    for (int32_t s = 1; s <= 4; s++) {
        lv_test_mouse_move_to(x + dx * s / 4, y + dy * s / 4);
        lv_test_wait(20);
    }
    lv_test_mouse_release();
    lv_test_wait(50);
}

static void test_ja_flick(void)
{
    CHECK(start());
    s_fake_ja = k_fake_zh;
    s_fake_ja.preedit = fz_preedit;
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, &s_fake_ja);
    CHECK(lv_obj_is_hidden(s_kbd.flick_toggle)); // JA only
    pick_lang(UI_KBD_LANG_JA);
    CHECK(!lv_obj_is_hidden(s_kbd.flick_toggle));
    CHECK(lv_obj_is_hidden(s_kbd.flick_pad));
    s_rec_n = 0;
    click_obj(s_kbd.flick_toggle);
    CHECK(s_kbd.flick);
    CHECK(!lv_obj_is_hidden(s_kbd.flick_pad));
    CHECK(lv_obj_is_hidden(s_kbd.row_top) && lv_obj_is_hidden(s_kbd.row_number));
    CHECK(!lv_obj_is_hidden(s_kbd.row_bottom));
    CHECK_EQ(s_rec_n, 0);

    // the pad's keys meet the touch floor and stay inside the slot
    lv_obj_update_layout(s_kbd.root);
    lv_area_t slot;
    lv_obj_get_coords(s_r.content_slot, &slot);
    for (uint32_t g = 0; g < UI_VIEW_KBD_FLICK_KEYS; g++) {
        lv_area_t a;
        lv_obj_get_coords(s_kbd.flick_key[g], &a);
        CHECK(lv_area_get_width(&a) >= UI_TOUCH_MIN && lv_area_get_height(&a) >= UI_TOUCH_MIN);
        CHECK(a.x1 >= slot.x1 && a.x2 <= slot.x2 && a.y1 >= slot.y1 && a.y2 <= slot.y2);
    }

    // tap = a-row; left / up / right / down = i / u / e / o; a short slide is still a tap
    flick_obj(pad_kana(1), 0, 0);   // か
    flick_obj(pad_kana(2), -40, 0); // し
    flick_obj(pad_kana(3), 0, -40); // つ
    flick_obj(pad_kana(4), 40, 0);  // ね
    flick_obj(pad_kana(5), 0, 40);  // ほ
    flick_obj(pad_kana(0), 10, 10); // あ (under UI_FLICK_MIN)
    CHECK(strcmp(s_kbd.py, "kasitunehoa") == 0);
    CHECK_EQ(s_kbd.flick_n, 6u);
    CHECK_EQ(s_kbd.c.flicks, 6u);
    CHECK_EQ(s_rec_n, 0); // composed locally
    // the key shows its resting legend again after the press
    CHECK(strcmp(lv_label_get_text(s_kbd.flick_label[1]), "\xE3\x81\x8B") == 0);

    // ゛゜小 cycles the last kana: あ -> ぁ -> あ (no dakuten)
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasitunehoxa") == 0);
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasitunehoa") == 0);
    // Backspace takes one kana, however many letters
    click_obj(pad_fn(0));
    click_obj(pad_fn(0));
    CHECK(strcmp(s_kbd.py, "kasitune") == 0);
    click_obj(pad_fn(0));
    // つ: plain -> づ -> っ -> つ
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasidu") == 0);
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasixtu") == 0);
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasitu") == 0);
    // は: ば, ぱ; し: じ; わ row: を ん ー
    flick_obj(pad_kana(5), 0, 0);
    flick_obj(pad_kana(9), 0, 0);
    flick_obj(pad_kana(9), 0, 0);
    CHECK(strcmp(s_kbd.py, "kasitupa") == 0);
    flick_obj(pad_kana(10), -40, 0);
    flick_obj(pad_kana(10), 0, -40);
    flick_obj(pad_kana(10), 40, 0);
    flick_obj(pad_kana(10), 0, 40); // nothing below わ
    CHECK(strcmp(s_kbd.py, "kasitupawonn-") == 0);
    CHECK_EQ(s_rec_n, 0);

    // Enter commits the reading; the punctuation key commits what is composed, then the mark
    click_obj(pad_fn(2));
    CHECK(s_rec_n > 0 && count_type(UI_ACT_TEXT_CHAR) > 0);
    CHECK_EQ(s_kbd.py_len, 0u);
    CHECK_EQ(s_kbd.flick_n, 0u);
    s_rec_n = 0;
    flick_obj(pad_kana(1), 0, 0);   // か
    flick_obj(pad_kana(11), -40, 0); // 。
    CHECK(s_rec_n >= 2 && s_rec[0].type == UI_ACT_TEXT_CHAR && rec_is(s_rec_n - 1, UI_ACT_TEXT_CHAR, 0x3002));
    CHECK_EQ(s_kbd.py_len, 0u);
    CHECK_EQ(s_kbd.flick_n, 0u);
    s_rec_n = 0;
    flick_obj(pad_kana(7), -40, 0); // 「 (や flicked left)
    flick_obj(pad_kana(11), 0, -40); // ？
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_CHAR, 0x300C) && rec_is(1, UI_ACT_TEXT_CHAR, 0xFF1F));
    // Space and Backspace with nothing composed pass through as keys
    s_rec_n = 0;
    click_obj(pad_fn(1));
    click_obj(pad_fn(0));
    CHECK(s_rec_n == 2 && rec_is(0, UI_ACT_TEXT_KEY, KVM_KEY_SPACE) && rec_is(1, UI_ACT_TEXT_KEY, KVM_KEY_BACKSPACE));
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0); // never a HID key from the pad

    // the input stops at UI_VIEW_KBD_JA_CAP letters
    for (int i = 0; i < 30; i++) {
        flick_obj(pad_kana(1), 0, 0);
    }
    CHECK(s_kbd.py_len <= UI_VIEW_KBD_JA_CAP && s_kbd.py_len >= UI_VIEW_KBD_JA_CAP - 1u);
    click_obj(visible_key(KVM_KEY_ESCAPE)); // the utility row stays: Esc discards
    CHECK_EQ(s_kbd.py_len, 0u);

    // HOST lost: the pad is disabled and a flick does nothing
    publish(KVM_HOST_USB_DETACHED);
    CHECK(lv_obj_has_state(pad_kana(1), LV_STATE_DISABLED));
    publish(KVM_HOST_USB_MOUNTED);
    CHECK(!lv_obj_has_state(pad_kana(1), LV_STATE_DISABLED));

    // ABC goes back to QWERTY; leaving JA hides the pad and the toggle and forgets the choice
    click_obj(pad_fn(3));
    CHECK(!s_kbd.flick && lv_obj_is_hidden(s_kbd.flick_pad) && !lv_obj_is_hidden(s_kbd.row_top));
    click_obj(s_kbd.flick_toggle);
    CHECK(s_kbd.flick);
    pick_lang(UI_KBD_LANG_EN);
    CHECK(!s_kbd.converting && !s_kbd.flick);
    CHECK(lv_obj_is_hidden(s_kbd.flick_pad) && lv_obj_is_hidden(s_kbd.flick_toggle));
    CHECK(!lv_obj_is_hidden(s_kbd.row_top) && !lv_obj_is_hidden(s_kbd.row_number));
    // the F1-F12 layer still swaps its row while the pad is off
    click_obj(layer_key("F1-F12"));
    CHECK(lv_obj_is_hidden(s_kbd.row_number) && !lv_obj_is_hidden(s_kbd.row_fn));
    click_obj(layer_key("F1-F12"));
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, NULL);
    stop();
}

// ---- the language key (ADR-037) ------------------------------------------------------------
/** The picker's Close button: the card's last child is the footer, whose only child it is. */
static lv_obj_t *picker_close_btn(void)
{
    lv_obj_t *card = s_kbd.lang_card;
    if (card == NULL) {
        return NULL;
    }
    lv_obj_t *foot = lv_obj_get_child(card, -1);
    return foot != NULL ? lv_obj_get_child(foot, 0) : NULL;
}

static void both_engines(void)
{
    s_fake_ja = k_fake_zh;
    s_fake_ja.preedit = fz_preedit;
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    ui_view_keyboard_set_zh_engine(&s_kbd, &k_fake_zh);
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, &s_fake_ja);
}

static void drop_engines(void)
{
    ui_view_keyboard_set_zh_engine(&s_kbd, NULL);
    ui_view_keyboard_set_conv_engine(&s_kbd, UI_KBD_LAYER_JA, NULL);
}

/** A tap cycles EN -> KO -> ZH -> JA -> EN while everything is on and available. */
static void test_lang_cycle(void)
{
    CHECK(start());
    both_engines();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    CHECK(strcmp(lang_key_text(), "EN") == 0);
    CHECK_EQ(ui_view_keyboard_languages(&s_kbd), 0x0Fu);

    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_KO);
    CHECK(s_kbd.hangul && strcmp(lang_key_text(), "KO") == 0);
    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_ZH);
    CHECK(s_kbd.converting && s_kbd.conv_layer == UI_KBD_LAYER_ZH && strcmp(lang_key_text(), "ZH") == 0);
    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_JA);
    CHECK(s_kbd.converting && s_kbd.conv_layer == UI_KBD_LAYER_JA && strcmp(lang_key_text(), "JA") == 0);
    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    CHECK(!s_kbd.hangul && !s_kbd.converting && strcmp(lang_key_text(), "EN") == 0);
    CHECK_EQ(s_kbd.c.lang_switches, 4u);
    CHECK_EQ(s_kbd.c.lang_refused, 0u);
    CHECK_EQ(s_kbd.c.picker_opens, 0u); // four taps, never a long press
    // the cap is a local control: no HID, and the only domain traffic is the text protocol
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0);
    CHECK_EQ(count_type(UI_ACT_MODIFIER_TOGGLE), 0);
    drop_engines();
    stop();
}

/** A language that is off in Settings, or whose decoder is missing, is skipped by the cycle. */
static void test_lang_cycle_skips_unavailable(void)
{
    CHECK(start());
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    ui_view_keyboard_set_zh_engine(&s_kbd, &k_fake_zh); // ZH only: JA has no decoder
    ui_view_keyboard_set_languages(&s_kbd, false, true, true); // Korean off in Settings
    CHECK(strcmp(ui_view_keyboard_lang_reason(&s_kbd, UI_KBD_LANG_KO), "Off in Settings") == 0);

    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_ZH); // KO off, so the cycle lands on ZH
    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN); // JA has no decoder, so back to English
    CHECK_EQ(s_kbd.c.lang_switches, 2u);
    CHECK_EQ(s_kbd.c.lang_refused, 0u);

    // the picker lists only what Settings turned on
    hold_lang();
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_EN) != NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_KO) == NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_ZH) != NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_JA) != NULL);
    CHECK(lv_obj_is_disabled(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_JA)));
    click_obj(picker_close_btn());
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
    CHECK_EQ(s_kbd.c.picker_opens, 1u);
    CHECK_EQ(s_kbd.c.picker_closes, 1u);
    drop_engines();
    stop();
}

/** The default record: English alone. A tap says where the others are and changes nothing. */
static void test_lang_english_only(void)
{
    CHECK(start());
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    both_engines();
    ui_view_keyboard_set_languages(&s_kbd, false, false, false); // the record's default
    CHECK_EQ(ui_view_keyboard_languages(&s_kbd), 0x01u);

    tap_lang();
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    CHECK_EQ(s_kbd.c.lang_switches, 0u);
    CHECK_EQ(s_kbd.c.lang_refused, 1u);
    CHECK_EQ(s_kbd.c.toasts, 1u);
    CHECK(strcmp(ui_view_keyboard_toast_text(&s_kbd), "Add input languages in Settings > Keyboard") == 0);
    CHECK_EQ(s_rec_n, 0); // nothing reaches the domain

    // the picker still opens and lists English alone, marked Current
    hold_lang();
    CHECK(ui_view_keyboard_picker_open(&s_kbd));
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_EN) != NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_KO) == NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_ZH) == NULL);
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_JA) == NULL);
    click_obj(picker_close_btn());
    drop_engines();
    stop();
}

/** A long press is not also a tap: the picker opens and the language stays where it was. */
static void test_lang_long_press_is_not_a_tap(void)
{
    CHECK(start());
    both_engines();
    hold_lang();
    CHECK(ui_view_keyboard_picker_open(&s_kbd));
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN); // the cycle did not run as well
    CHECK_EQ(s_kbd.c.lang_switches, 0u);
    CHECK_EQ(s_rec_n, 0);

    // tapping the current row closes the card and changes nothing
    click_obj(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_EN));
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    CHECK_EQ(s_kbd.c.lang_switches, 0u);

    // a pick switches and closes in one tap
    pick_lang(UI_KBD_LANG_JA);
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_JA);
    CHECK_EQ(s_kbd.c.lang_switches, 1u);
    CHECK_EQ(s_kbd.c.picker_opens, 2u);
    CHECK_EQ(s_kbd.c.picker_closes, 2u);

    // the router's own dismiss closes it too, and the view forgets the rows
    hold_lang();
    CHECK(ui_view_keyboard_picker_open(&s_kbd));
    ui_overlay_dismiss(&s_r);
    lv_test_wait(5);
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
    CHECK(ui_view_keyboard_lang_pick_button(&s_kbd, UI_KBD_LANG_EN) == NULL);
    CHECK_EQ(s_kbd.c.picker_opens, 3u);
    CHECK_EQ(s_kbd.c.picker_closes, 3u);
    drop_engines();
    stop();
}

/** A language turned off in Settings while it is active falls back to English, committing what is composed. */
static void test_lang_turned_off_while_active(void)
{
    CHECK(start());
    ui_view_keyboard_set_text_mode(&s_kbd, true);
    pick_lang(UI_KBD_LANG_KO);
    s_rec_n = 0;
    type_letters("r"); // ㄱ in the preedit
    CHECK_EQ(s_rec_n, 0);

    ui_view_keyboard_set_languages(&s_kbd, false, false, false);
    CHECK(!s_kbd.hangul);
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    CHECK(strcmp(lang_key_text(), "EN") == 0);
    CHECK(lv_obj_is_hidden(s_kbd.row_compose));
    CHECK(s_rec_n == 1 && rec_is(0, UI_ACT_TEXT_CHAR, 0x3131)); // committed, not discarded
    CHECK_EQ(count_type(UI_ACT_TEXT_CANCEL), 0);

    // turning the same language on again does not switch back on its own
    ui_view_keyboard_set_languages(&s_kbd, true, false, false);
    CHECK_EQ(ui_view_keyboard_lang(&s_kbd), UI_KBD_LANG_EN);
    // ... and an unchanged set is a no-op
    const uint32_t switches = s_kbd.c.lang_switches;
    ui_view_keyboard_set_languages(&s_kbd, true, false, false);
    CHECK_EQ(s_kbd.c.lang_switches, switches);
    stop();
}

/** Without a bound router there is no picker and no toast, and the refusal is still counted. */
static void test_lang_without_router(void)
{
    CHECK(start());
    ui_view_keyboard_bind_router(&s_kbd, NULL);
    ui_view_keyboard_set_languages(&s_kbd, false, false, false);
    tap_lang();
    CHECK_EQ(s_kbd.c.lang_refused, 1u);
    CHECK_EQ(s_kbd.c.toasts, 0u);
    CHECK(strcmp(ui_view_keyboard_toast_text(&s_kbd), "") == 0);
    hold_lang();
    CHECK(!ui_view_keyboard_picker_open(&s_kbd));
    CHECK_EQ(s_kbd.c.lang_refused, 2u);
    ui_view_keyboard_bind_router(&s_kbd, &s_r);
    stop();
}

/** The codes and names the cap and the picker use, including the out-of-range guard. */
static void test_lang_names(void)
{
    CHECK(strcmp(ui_view_keyboard_lang_code(UI_KBD_LANG_EN), "EN") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_code(UI_KBD_LANG_KO), "KO") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_code(UI_KBD_LANG_ZH), "ZH") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_code(UI_KBD_LANG_JA), "JA") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_name(UI_KBD_LANG_EN), "English") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_name(UI_KBD_LANG_KO), "Korean") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_name(UI_KBD_LANG_ZH), "Chinese (pinyin)") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_name(UI_KBD_LANG_JA), "Japanese") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_code(UI_KBD_LANG_COUNT), "?") == 0);
    CHECK(strcmp(ui_view_keyboard_lang_name(UI_KBD_LANG_COUNT), "?") == 0);
    // the cap keeps a stable diagnostic name although its legend follows the language
    uint32_t langs = 0;
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); i++) {
        langs += strcmp(ui_view_keyboard_key_name(i), "Lang") == 0 ? 1u : 0u;
    }
    CHECK_EQ(langs, 1u);
    // NULL is answered, never dereferenced
    CHECK(ui_view_keyboard_lang_key(NULL) == NULL);
    CHECK(ui_view_keyboard_lang_pick_button(NULL, UI_KBD_LANG_EN) == NULL);
    CHECK(!ui_view_keyboard_picker_open(NULL));
    CHECK(!ui_view_keyboard_lang_available(NULL, UI_KBD_LANG_EN));
    CHECK_EQ(ui_view_keyboard_languages(NULL), 0u);
    CHECK_EQ(ui_view_keyboard_lang(NULL), UI_KBD_LANG_EN);
    CHECK(strcmp(ui_view_keyboard_toast_text(NULL), "") == 0);
    ui_view_keyboard_set_languages(NULL, true, true, true);
    ui_view_keyboard_bind_router(NULL, NULL);
    uint8_t n = 7;
    CHECK(ui_view_keyboard_overlays(NULL, &n) == NULL);
    CHECK_EQ(n, 0);
}

// ---- the HOST layout quick switch (ADR-039 section 8) ---------------------------------------
static int32_t s_quick_idx;
static int s_quick_n;

/** The sink the shell gives the view: it only records, exactly as the view only reports. */
static void fake_layout_sink(void *ctx, int32_t table_index)
{
    (void)ctx;
    s_quick_idx = table_index;
    s_quick_n++;
}

static int32_t table_index_of(const char *id)
{
    for (size_t i = 0; i < kbd_layouts_count_get(); i++) {
        const kbd_layout_t *at = kbd_layouts_at(i);
        if (at != NULL && strcmp(at->id, id) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

static lv_obj_t *strip_obj(void)
{
    return ui_view_keyboard_layout_strip(&s_kbd);
}

/** A long press on the strip, as the finger does it: held past the long-press time. */
static void hold_strip(void)
{
    press_obj(strip_obj());
    lv_test_wait(600);
    release();
}

/** The quick switch's Close button: the card's last child is the footer, whose only child it is. */
static lv_obj_t *quick_close_btn(void)
{
    lv_obj_t *card = ui_view_keyboard_layout_card(&s_kbd);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_t *foot = lv_obj_get_child(card, -1);
    return foot != NULL ? lv_obj_get_child(foot, 0) : NULL;
}

/** The fixture of the lists: two pinned, two recent, one of them already pinned. */
static void quick_lists(void)
{
    static const char fav[UI_VIEW_KBD_FAV_MAX][UI_VIEW_KBD_LAYOUT_CAP] = {"us", "de"};
    static const char recent[UI_VIEW_KBD_RECENT_MAX][UI_VIEW_KBD_LAYOUT_CAP] = {"de", "fr(oss)"};
    s_quick_idx = -1;
    s_quick_n = 0;
    ui_view_keyboard_set_host_layout(&s_kbd, "us");
    ui_view_keyboard_set_host_lists(&s_kbd, fav, 2u, recent, 2u);
    ui_view_keyboard_set_layout_action(&s_kbd, fake_layout_sink, NULL);
}

/**
 * A long press opens the card, the lists are offered once each in order, and one tap reports the layout and
 * closes. Nothing is applied by the tap itself: the field still shows what the record last published.
 */
static void test_quick_switch_picks_a_layout(void)
{
    CHECK(start());
    quick_lists();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);

    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    // favorites in their stored order, then the recents that are not pinned: us, de, fr(oss)
    CHECK(strcmp(ui_view_keyboard_layout_pick_text(&s_kbd, 0), "us\nEnglish (US)") == 0);
    CHECK(strncmp(ui_view_keyboard_layout_pick_text(&s_kbd, 1), "de\n", 3) == 0);
    CHECK(strncmp(ui_view_keyboard_layout_pick_text(&s_kbd, 2), "fr(oss)\n", 8) == 0);
    CHECK(ui_view_keyboard_layout_pick_button(&s_kbd, 3) == NULL); // "de" is not offered twice
    CHECK_EQ(s_kbd.layout_pick_count, 3u);
    // the layout in force is marked, and it is still tappable
    CHECK(lv_obj_has_state(ui_view_keyboard_layout_pick_button(&s_kbd, 0), LV_STATE_CHECKED));
    CHECK(!lv_obj_has_state(ui_view_keyboard_layout_pick_button(&s_kbd, 1), LV_STATE_CHECKED));

    click_obj(ui_view_keyboard_layout_pick_button(&s_kbd, 2));
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK_EQ(s_quick_n, 1);
    CHECK_EQ(s_quick_idx, table_index_of("fr(oss)"));
    CHECK_EQ(s_kbd.c.quick_picks, 1u);
    CHECK_EQ(s_kbd.c.quick_opens, 1u);
    CHECK_EQ(s_kbd.c.quick_closes, 1u);
    // the UI is not the source of truth: the field changes when the record comes back, not on the tap
    CHECK(strcmp(ui_view_keyboard_host_layout(&s_kbd), "us") == 0);
    // and no HID or key traffic came out of a layout pick
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0);
    CHECK_EQ(count_type(UI_ACT_MODIFIER_TOGGLE), 0);

    // Close takes the card down and changes nothing
    hold_strip();
    click_obj(quick_close_btn());
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK_EQ(s_quick_n, 1);
    CHECK_EQ(s_kbd.c.quick_closes, 2u);

    // the router's own dismiss closes it too, and the view forgets the tiles
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    ui_overlay_dismiss(&s_r);
    lv_test_wait(5);
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK(ui_view_keyboard_layout_pick_button(&s_kbd, 0) == NULL);
    CHECK(strcmp(ui_view_keyboard_layout_pick_text(&s_kbd, 0), "") == 0);
    CHECK_EQ(s_kbd.c.quick_closes, 3u);
    ui_view_keyboard_set_layout_action(&s_kbd, NULL, NULL);
    stop();
}

/**
 * The strip carries an affordance (icon, short id, name, chevron) and both gestures open the card: the
 * chevron promises a list, so the ordinary tap has to deliver it. Neither gesture changes a layout by itself.
 */
static void test_quick_switch_opens_on_tap_and_hold(void)
{
    CHECK(start());
    quick_lists();
    // what the strip says: the short id of the layout in force and its name, not a clipped sentence
    CHECK(strcmp(ui_view_keyboard_layout_code(&s_kbd), "US") == 0);
    CHECK(strcmp(ui_view_keyboard_layout_name(&s_kbd), "English (US)") == 0);
    // ... while the FR-742 sentence is still the one the accessor reports
    CHECK(strstr(ui_view_keyboard_hint_text(&s_kbd), "HOST") != NULL);

    click_obj(strip_obj());
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    CHECK_EQ(s_kbd.c.quick_opens, 1u);
    CHECK_EQ(s_quick_n, 0); // opening is not choosing
    click_obj(quick_close_btn());

    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    CHECK_EQ(s_kbd.c.quick_opens, 2u);
    CHECK_EQ(s_quick_n, 0);
    click_obj(quick_close_btn());
    CHECK_EQ(s_kbd.c.quick_refused, 0u);
    // the strip is a control of its own: no key, no HID and no setting came out of either gesture
    CHECK_EQ(count_type(UI_ACT_KEY_DOWN), 0);
    CHECK_EQ(count_type(UI_ACT_MODIFIER_TOGGLE), 0);

    // an id the tables do not ship leaves the strip hidden, and then there is nothing to press
    ui_view_keyboard_set_host_layout(&s_kbd, "");
    CHECK(lv_obj_is_hidden(strip_obj()));
    CHECK(strcmp(ui_view_keyboard_layout_code(&s_kbd), "") == 0);
    ui_view_keyboard_set_layout_action(&s_kbd, NULL, NULL);
    stop();
}

/** The short id drops the variant and goes upper case: "fr(oss)" reads "FR" on the strip. */
static void test_quick_switch_strip_short_id(void)
{
    CHECK(start());
    quick_lists();
    ui_view_keyboard_set_host_layout(&s_kbd, "fr(oss)");
    CHECK(strcmp(ui_view_keyboard_layout_code(&s_kbd), "FR") == 0);
    CHECK(strcmp(ui_view_keyboard_layout_name(&s_kbd), "French (alt.)") == 0);
    ui_view_keyboard_set_host_layout(&s_kbd, "de");
    CHECK(strcmp(ui_view_keyboard_layout_code(&s_kbd), "DE") == 0);
    CHECK(strcmp(ui_view_keyboard_layout_code(NULL), "") == 0);
    CHECK(strcmp(ui_view_keyboard_layout_name(NULL), "") == 0);
    ui_view_keyboard_set_layout_action(&s_kbd, NULL, NULL);
    stop();
}

/** Without a sink, without lists, or with ids this build does not ship, the strip offers nothing. */
static void test_quick_switch_offers_nothing_to_offer(void)
{
    static const char gone[UI_VIEW_KBD_FAV_MAX][UI_VIEW_KBD_LAYOUT_CAP] = {"zz(nope)"};
    static const char fav[UI_VIEW_KBD_FAV_MAX][UI_VIEW_KBD_LAYOUT_CAP] = {"zz(nope)", "de"};
    CHECK(start());
    s_quick_n = 0;
    ui_view_keyboard_set_host_layout(&s_kbd, "us");

    // no sink: a screen that cannot change a value does not offer to
    ui_view_keyboard_set_host_lists(&s_kbd, fav, 2u, NULL, 0u);
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK_EQ(s_kbd.c.quick_refused, 1u);

    // a sink but no list
    ui_view_keyboard_set_layout_action(&s_kbd, fake_layout_sink, NULL);
    ui_view_keyboard_set_host_lists(&s_kbd, NULL, 0u, NULL, 0u);
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK_EQ(s_kbd.c.quick_refused, 2u);

    // a list of ids this build's tables do not ship: the same, and the record keeps them
    ui_view_keyboard_set_host_lists(&s_kbd, gone, 1u, NULL, 0u);
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);
    CHECK_EQ(s_kbd.c.quick_refused, 3u);
    CHECK(strcmp(s_kbd.host_fav[0], "zz(nope)") == 0);

    // one unknown and one shipped id: only the shipped one gets a tile, at position 0
    ui_view_keyboard_set_host_lists(&s_kbd, fav, 2u, NULL, 0u);
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    CHECK_EQ(s_kbd.layout_pick_count, 1u);
    CHECK(strncmp(ui_view_keyboard_layout_pick_text(&s_kbd, 0), "de\n", 3) == 0);
    click_obj(ui_view_keyboard_layout_pick_button(&s_kbd, 0));
    CHECK_EQ(s_quick_n, 1);
    CHECK_EQ(s_quick_idx, table_index_of("de"));

    // a list that changes while the card is up takes the card down rather than leave stale tiles
    hold_strip();
    CHECK(ui_view_keyboard_layout_card(&s_kbd) != NULL);
    ui_view_keyboard_set_host_lists(&s_kbd, gone, 1u, NULL, 0u);
    lv_test_wait(5);
    CHECK(ui_view_keyboard_layout_card(&s_kbd) == NULL);

    // NULL is answered, never dereferenced
    CHECK(ui_view_keyboard_layout_strip(NULL) == NULL);
    CHECK(ui_view_keyboard_layout_card(NULL) == NULL);
    CHECK(ui_view_keyboard_layout_pick_button(NULL, 0) == NULL);
    CHECK(strcmp(ui_view_keyboard_layout_pick_text(NULL, 0), "") == 0);
    ui_view_keyboard_set_host_lists(NULL, fav, 2u, NULL, 0u);
    ui_view_keyboard_set_layout_action(NULL, NULL, NULL);
    ui_view_keyboard_set_layout_action(&s_kbd, NULL, NULL);
    stop();
}

/**
 * AT-UIFW-01 for the card: the strip and every tile are at least UI_TOUCH_MIN tall, no tile leaves the card
 * or the screen, and each tile's center lies inside every one of its ancestors - a control laid out past its
 * parent's edge is clipped out of drawing and out of touch alike.
 */
static void test_quick_switch_geometry(void)
{
    CHECK(start());
    quick_lists();
    lv_area_t strip;
    lv_obj_update_layout(strip_obj());
    lv_obj_get_coords(strip_obj(), &strip);
    CHECK(strip.y2 - strip.y1 + 1 >= UI_TOUCH_MIN);
    // the text used to run off the right edge of the panel; the strip is a bounded box that ends inside it
    CHECK(strip.x1 >= 0 && strip.x2 < SCR_W);
    for (uint32_t i = 0; i < lv_obj_get_child_count(strip_obj()); i++) {
        lv_area_t ch;
        lv_obj_get_coords(lv_obj_get_child(strip_obj(), (int32_t)i), &ch);
        CHECK(ch.x1 >= strip.x1 && ch.x2 <= strip.x2);
    }

    hold_strip();
    lv_obj_t *card = ui_view_keyboard_layout_card(&s_kbd);
    CHECK(card != NULL);
    lv_obj_update_layout(card);
    lv_area_t ca;
    lv_obj_get_coords(card, &ca);
    CHECK(ca.x1 >= 0 && ca.y1 >= 0 && ca.x2 < SCR_W && ca.y2 < SCR_H);
    for (uint8_t i = 0; i < s_kbd.layout_pick_count; i++) {
        lv_obj_t *tile = ui_view_keyboard_layout_pick_button(&s_kbd, i);
        CHECK(tile != NULL);
        lv_area_t a;
        lv_obj_get_coords(tile, &a);
        CHECK(a.x2 - a.x1 + 1 >= UI_TOUCH_MIN);
        CHECK(a.y2 - a.y1 + 1 >= UI_TOUCH_MIN);
        CHECK(a.x1 >= ca.x1 && a.x2 <= ca.x2 && a.y1 >= ca.y1 && a.y2 <= ca.y2);
        const lv_point_t mid = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        CHECK(lv_obj_hit_test(tile, &mid));
        // lv_indev_search_obj never enters a parent whose area misses the point, whether that parent is
        // clickable or not, so containment is checked by area all the way up to the screen.
        for (lv_obj_t *up = lv_obj_get_parent(tile); up != NULL; up = lv_obj_get_parent(up)) {
            lv_area_t pa;
            lv_obj_update_layout(up);
            lv_obj_get_coords(up, &pa);
            CHECK(mid.x >= pa.x1 && mid.x <= pa.x2 && mid.y >= pa.y1 && mid.y <= pa.y2);
        }
    }
    click_obj(quick_close_btn());
    ui_view_keyboard_set_layout_action(&s_kbd, NULL, NULL);
    stop();
}

// A lean create/destroy cycle: the screen is never loaded, so nothing is rendered and the run stays short.
static void cycle_once(void)
{
    ui_mailbox_init(&s_mb, NULL);
    if (!ui_store_init(&s_store, &s_mb)) {
        CHECK(false);
        return;
    }
    ui_actions_init(&s_act);
    ui_actions_set_domain_sink(&s_act, fake_sink, NULL);
    ui_view_keyboard_init(&s_kbd, &s_store, &s_act, fake_now);
    s_desc[0] = s_kbd.desc;
    s_desc[1] = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = other_create};
    lv_obj_t *scr = lv_obj_create(NULL);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    if (ui_router_init(&s_r, scr, &lay, &s_store, s_desc, 2, NULL, 0, &k_hooks)) {
        (void)ui_router_show(&s_r, UI_VIEW_KEYBOARD);
        ui_router_deinit(&s_r);
    } else {
        CHECK(false);
    }
    ui_store_deinit(&s_store);
    lv_obj_delete(scr);
}

static void test_lifecycle(void)
{
    // store deleted first, while the view still observes its subjects: the view is destroyed afterwards
    CHECK(start());
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_test_wait(10);
    stop();

    // one full rendered cycle, then 1000 lean ones
    CHECK(start());
    stop();
    cycle_once();
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 1000; i++) {
        cycle_once();
    }
    printf("    1000 view create/destroy cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);
}

#define RUN_MEM(t)                                                                                      \
    do {                                                                                                \
        size_t before_ = host_lv_live_count();                                                          \
        RUN(t);                                                                                         \
        CHECK_EQ(host_lv_live_count(), before_);                                                        \
    } while (0)

int main(void)
{
    printf("test_ui_view_keyboard (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    CHECK(ui_theme_init());
    lv_test_indev_create_all();
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    CHECK(start()); // warm-up: first text render and first press
    click_obj(visible_key(KVM_KEY_A));
    stop();

    RUN(test_usages_are_valid);
    RUN_MEM(test_tap_every_key);
    RUN_MEM(test_hold_no_repeat);
    RUN_MEM(test_slide_off);
    RUN_MEM(test_modifier_follows_store);
    RUN_MEM(test_fn_layer);
    RUN_MEM(test_caps_indicator);
    RUN_MEM(test_host_not_mounted);
    RUN_MEM(test_offline_notice_and_disabled_legibility);
    RUN_MEM(test_fn_caps_look_enabled);
    RUN_MEM(test_navigation_while_held);
    RUN_MEM(test_refused_down);
    RUN_MEM(test_ko_needs_text_mode);
    RUN_MEM(test_ko_compose);
    RUN_MEM(test_ko_leave_and_host_ime_discard);
    RUN_MEM(test_zh_needs_engine);
    RUN_MEM(test_zh_compose);
    RUN_MEM(test_zh_exclusive_and_discard);
    RUN_MEM(test_zh_strip_geometry);
    RUN_MEM(test_ja_layer);
    RUN_MEM(test_ja_flick);
    RUN(test_lang_names);
    RUN_MEM(test_lang_cycle);
    RUN_MEM(test_lang_cycle_skips_unavailable);
    RUN_MEM(test_lang_english_only);
    RUN_MEM(test_lang_long_press_is_not_a_tap);
    RUN_MEM(test_lang_turned_off_while_active);
    RUN_MEM(test_lang_without_router);
    RUN(test_host_layout_slots_are_unique);
    RUN_MEM(test_host_layout_default_is_us_ansi);
    RUN_MEM(test_host_layout_us);
    RUN_MEM(test_host_layout_iso);
    RUN_MEM(test_host_layout_altgr);
    RUN_MEM(test_host_layout_jis);
    RUN_MEM(test_host_layout_abnt2_and_kr);
    RUN_MEM(test_host_layout_cyrillic_and_best_effort);
    RUN_MEM(test_host_layout_script_without_glyphs);
    RUN_MEM(test_host_layout_unknown_id_changes_nothing);
    RUN_MEM(test_host_layout_sends_the_layout_usage);
    RUN_MEM(test_host_layout_before_the_view_exists);
    RUN_MEM(test_quick_switch_picks_a_layout);
    RUN_MEM(test_quick_switch_opens_on_tap_and_hold);
    RUN_MEM(test_quick_switch_strip_short_id);
    RUN_MEM(test_quick_switch_offers_nothing_to_offer);
    RUN_MEM(test_quick_switch_geometry);
    RUN_MEM(test_geometry);
    RUN_MEM(test_lifecycle);

    lv_test_indev_delete_all();
    lv_display_delete(disp);
    ui_theme_deinit();
    lv_deinit();
    TEST_MAIN_END("test_ui_view_keyboard");
}
