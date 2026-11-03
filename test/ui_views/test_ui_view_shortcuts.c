// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-070C SHORTCUTS with the real LVGL 9.6 (headless 1024x600, LVGL test pointer, ASan + UBSan), the real
// router, store, mailbox, theme and action gateway, and the real chord allowlist of hid_core:
// - each of the six reference cards submits exactly one SHORTCUT_REQUEST carrying its chord id (FR-301..306);
// - the Media row submits the ADR-018 / FR-311 Consumer chords;
// - the FR-744 layout chord follows the init parameter, and an unknown one disables the tile;
// - FR-734 / ADR-023: with the policy on nothing goes out until Confirm, Cancel sends nothing, Confirm sends
//   once; with the policy off the card submits directly;
// - FR-209: the action carries a chord id only - no usage, no modifier ever leaves this view;
// - FR-006 / FR-308: HOST not mounted, maintenance or a REMOTE owner disables every tile and sends nothing,
//   and a pending confirmation is abandoned instead of fired;
// - a refused submit says so and is not retried;
// - every chord id the view can emit is on the compiled allowlist (kvm_chord_get);
// - geometry: 13 targets inside the content slot, >= 44 px, no overlaps;
// - lifecycle: store deleted before the router, 1000 view create/destroy cycles without live-allocation growth
//   (AT-UIFW-09).
#include <string.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_modal.h"
#include "ui_router.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_toast.h"
#include "ui_tokens.h"
#include "ui_view_shortcuts.h"

// The real allowlist table: hid_core is not part of the host ui_stack library, and the point of the check is
// that the view's ids are the shipped ones, not a copy made for the test.
#include "../../components/hid_core/src/hid_keymap.c"

#define SCR_W 1024
#define SCR_H 600
#define TOP_H UI_TOPBAR_H

// ---- fake domain ---------------------------------------------------------------------------
typedef struct {
    ui_action_type_t type;
    uint16_t value;
    uint16_t control_id;
} rec_t;

static rec_t s_rec[64];
static int s_rec_n;
static int s_sink_calls; // every call, accepted or refused
static bool s_refuse;
static uint64_t s_clock;

static uint64_t fake_now(void)
{
    return s_clock += 1000;
}

static bool fake_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    s_sink_calls++;
    if (s_refuse) {
        return false;
    }
    if (s_rec_n < (int)(sizeof(s_rec) / sizeof(s_rec[0]))) {
        s_rec[s_rec_n++] = (rec_t){a->type, a->value, a->control_id};
    }
    return true;
}

static int s_cancels;
static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    (void)why;
    s_cancels++;
    return true;
}

static const ui_nav_hooks_t k_hooks = {.cancel_input = hook_cancel};

// ---- FR-734 policy -------------------------------------------------------------------------
static bool s_confirm_policy;
static int s_policy_reads;

static bool policy_confirm_alt_f4(void *ctx)
{
    CHECK(ctx == (void *)&s_confirm_policy);
    s_policy_reads++;
    return s_confirm_policy;
}

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
static ui_view_shortcuts_t s_sc;
static ui_view_descriptor_t s_desc[2];
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static uint32_t s_seq;

static void publish(uint8_t host, uint8_t owner, bool maintenance)
{
    ui_hid_slice_t h = {.host_usb_state = host, .owner_kind = owner, .maintenance = maintenance};
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static bool start_with(uint16_t layout_chord)
{
    memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0;
    s_sink_calls = 0;
    s_refuse = false;
    s_cancels = 0;
    s_policy_reads = 0;
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    s_store_on = true;
    ui_actions_init(&s_act);
    ui_actions_set_domain_sink(&s_act, fake_sink, NULL);
    ui_view_shortcuts_init(&s_sc, &s_store, &s_act, fake_now, layout_chord, policy_confirm_alt_f4,
                           &s_confirm_policy);
    s_desc[0] = s_sc.desc;
    s_desc[1] = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = other_create};
    s_scr = lv_obj_create(NULL);
    ui_theme_apply(s_scr, UI_STYLE_SCREEN, LV_STATE_DEFAULT);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    bool ok = ui_router_init(&s_r, s_scr, &lay, &s_store, s_desc, 2, NULL, 0, &k_hooks);
    ui_view_shortcuts_set_layers(&s_sc, NULL, s_r.toast_layer);
    ok = ok && ui_router_show(&s_r, UI_VIEW_SHORTCUTS);
    publish(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, false);
    lv_test_wait(20);
    return ok;
}

static bool start(void)
{
    return start_with(KVM_CHORD_LAYOUT_WIN);
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

static void center_of(lv_obj_t *o, int32_t *x, int32_t *y)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

static void click_obj(lv_obj_t *o)
{
    int32_t x, y;
    center_of(o, &x, &y);
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

static void click_xy(int32_t x, int32_t y)
{
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

static lv_obj_t *confirm_button(int index)
{
    lv_obj_t *card = ui_modal_card(s_sc.modal);
    CHECK(card != NULL);
    lv_obj_t *row = lv_obj_get_child(card, (int32_t)lv_obj_get_child_count(card) - 1);
    CHECK(row != NULL);
    return lv_obj_get_child(row, index);
}

#define CANCEL_BTN 0
#define CONFIRM_BTN 1

// Every clickable target of the view, in a stable order.
#define TARGETS ((int)UI_SHORTCUT_COMMAND_COUNT + (int)UI_SHORTCUT_MEDIA_COUNT + 1)

static void collect(lv_obj_t *out[TARGETS])
{
    int n = 0;
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        out[n++] = s_sc.card[i];
    }
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        out[n++] = s_sc.media[i];
    }
    out[n++] = s_sc.layout_key;
    CHECK_EQ(n, TARGETS);
}

// ---- tests ---------------------------------------------------------------------------------
static void test_commands_submit_their_chord(void)
{
    s_confirm_policy = false; // FR-734 off: Close window goes out directly
    CHECK(start());
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        s_rec_n = 0;
        click_obj(s_sc.card[i]);
        const uint16_t chord = ui_view_shortcuts_command_chord((ui_shortcut_command_t)i);
        CHECK_EQ(s_rec_n, 1); // exactly one action per tap
        CHECK_EQ(s_rec[0].type, UI_ACT_SHORTCUT_REQUEST);
        CHECK_EQ(s_rec[0].value, chord);
        CHECK_EQ(s_rec[0].control_id, UI_CTRL_SHORTCUT_BASE + chord);
        // FR-209 / 03 §3 rule 2: the value is a chord id, never a usage or a modifier bitmap.
        CHECK(s_rec[0].value < KVM_CHORD_COUNT);
        CHECK(s_sc.toast != NULL);
        CHECK(strstr(s_sc.toast_text, "Requested: ") == s_sc.toast_text);
        CHECK(strstr(s_sc.toast_text, ui_view_shortcuts_command_name((ui_shortcut_command_t)i)) != NULL);
    }
    CHECK_EQ(s_sc.c.requests, UI_SHORTCUT_COMMAND_COUNT);
    CHECK_EQ(s_sc.c.refused, 0);
    CHECK_EQ(s_sc.c.blocked, 0);
    CHECK_EQ(s_sc.c.confirms_opened, 0);
    CHECK_EQ(s_policy_reads, 1); // only the Alt+F4 card asks the policy
    // The reference order: the featured tile is the secure attention sequence.
    CHECK_EQ(ui_view_shortcuts_command_chord(UI_SHORTCUT_SECURE_ATTENTION), KVM_CHORD_CTRL_ALT_DEL);
    stop();
}

static void test_media_row(void)
{
    static const uint16_t expect[UI_SHORTCUT_MEDIA_COUNT] = {
        KVM_CHORD_MEDIA_PLAY_PAUSE, KVM_CHORD_MEDIA_PREV,        KVM_CHORD_MEDIA_NEXT,
        KVM_CHORD_MEDIA_MUTE,       KVM_CHORD_MEDIA_VOLUME_DOWN, KVM_CHORD_MEDIA_VOLUME_UP,
    };
    s_confirm_policy = false;
    CHECK(start());
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        s_rec_n = 0;
        click_obj(s_sc.media[i]);
        CHECK_EQ(s_rec_n, 1);
        CHECK_EQ(s_rec[0].type, UI_ACT_SHORTCUT_REQUEST);
        CHECK_EQ(s_rec[0].value, expect[i]);
        CHECK_EQ(ui_view_shortcuts_media_chord((ui_shortcut_media_t)i), expect[i]);
        // ADR-018 / TECH-075: the Media group is Consumer Control, with a usage from the allowlist.
        const kvm_chord_t *c = kvm_chord_get(s_rec[0].value);
        CHECK(c != NULL);
        CHECK_EQ(c->kind, KVM_CHORD_KIND_CONSUMER);
        CHECK(kvm_consumer_is_allowed(c->usage));
    }
    CHECK_EQ(s_policy_reads, 0);
    stop();
}

static void test_layout_chord_follows_init(void)
{
    s_confirm_policy = false;
    CHECK(start_with(KVM_CHORD_LAYOUT_WIN));
    CHECK(strcmp(lv_label_get_text(s_sc.layout_sub), "Win + Space") == 0);
    click_obj(s_sc.layout_key);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].value, KVM_CHORD_LAYOUT_WIN);
    stop();

    CHECK(start_with(KVM_CHORD_LAYOUT_CTRL)); // FR-744: macOS / some Linux desktops
    CHECK(strcmp(lv_label_get_text(s_sc.layout_sub), "Ctrl + Space") == 0);
    click_obj(s_sc.layout_key);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].value, KVM_CHORD_LAYOUT_CTRL);
    stop();

    CHECK(start_with(KVM_CHORD_NONE)); // no host OS selected yet: the tile is inert, never a guess
    CHECK(strcmp(lv_label_get_text(s_sc.layout_sub), "Not configured") == 0);
    CHECK(lv_obj_has_state(s_sc.layout_key, LV_STATE_DISABLED));
    click_obj(s_sc.layout_key);
    CHECK_EQ(s_rec_n, 0);
    stop();
}

static void test_alt_f4_confirmation(void)
{
    s_confirm_policy = true; // ADR-023: shortcuts.confirm_alt_f4 = ON
    CHECK(start());
    lv_obj_t *card = s_sc.card[UI_SHORTCUT_CLOSE_WINDOW];

    // Tap: a blocking modal, nothing submitted yet.
    click_obj(card);
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_sink_calls, 0);
    CHECK(s_sc.modal != NULL);
    CHECK_EQ(s_sc.c.confirms_opened, 1);
    CHECK_EQ(s_sc.pending_chord, KVM_CHORD_CLOSE_WINDOW);

    // Cancel sends nothing.
    click_obj(confirm_button(CANCEL_BTN));
    lv_test_wait(30); // the modal closes asynchronously
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_sink_calls, 0);
    CHECK(s_sc.modal == NULL);
    CHECK_EQ(s_sc.c.confirms_closed, 1);
    CHECK_EQ(s_sc.pending_chord, KVM_CHORD_NONE);

    // Confirm sends the chord exactly once.
    click_obj(card);
    CHECK(s_sc.modal != NULL);
    CHECK_EQ(s_sc.c.confirms_opened, 2);
    click_obj(confirm_button(CONFIRM_BTN));
    lv_test_wait(30);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].type, UI_ACT_SHORTCUT_REQUEST);
    CHECK_EQ(s_rec[0].value, KVM_CHORD_CLOSE_WINDOW);
    CHECK_EQ(s_sc.c.confirms_taken, 1);
    CHECK_EQ(s_sc.c.requests, 1);
    CHECK(s_sc.modal == NULL);
    CHECK(strstr(s_sc.toast_text, "Close window") != NULL);

    // The scrim is a decision the user must make: a tap outside the card neither closes nor sends (FR-734).
    click_obj(card);
    CHECK(s_sc.modal != NULL);
    click_xy(40, SCR_H - 40);
    CHECK(s_sc.modal != NULL);
    CHECK_EQ(s_rec_n, 1);

    // A view change abandons the pending decision (AGENTS-UI §6): nothing is sent on the way out.
    const int cancels = s_cancels;
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    CHECK_EQ(s_cancels, cancels + 1); // the router asked for the touch cancel + HID release
    CHECK(s_sc.modal == NULL);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_sc.c.confirms_closed, 2);
    CHECK(ui_router_show(&s_r, UI_VIEW_SHORTCUTS));

    // The other cards never ask.
    click_obj(s_sc.card[UI_SHORTCUT_SCREENSHOT]);
    CHECK(s_sc.modal == NULL);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[1].value, KVM_CHORD_SCREENSHOT);

    // Policy off at the next tap: the view reads it every time, it caches no decision.
    s_confirm_policy = false;
    click_obj(card);
    CHECK(s_sc.modal == NULL);
    CHECK_EQ(s_rec_n, 3);
    CHECK_EQ(s_rec[2].value, KVM_CHORD_CLOSE_WINDOW);
    CHECK_EQ(s_sc.c.confirms_opened, 3);
    stop();
}

static void test_local_input_not_allowed(void)
{
    lv_obj_t *t[TARGETS];
    s_confirm_policy = false;
    CHECK(start());
    collect(t);

    // FR-308: no HOST_MOUNTED -> every tile disabled, nothing submitted.
    publish(KVM_HOST_USB_DETACHED, OWNER_NONE, false);
    CHECK(!ui_view_shortcuts_input_allowed(&s_sc));
    for (int i = 0; i < TARGETS; i++) {
        CHECK(lv_obj_has_state(t[i], LV_STATE_DISABLED));
        click_obj(t[i]);
    }
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_sink_calls, 0);
    CHECK(strstr(lv_label_get_text(s_sc.hint), "Host disconnected") != NULL);

    // FR-006: maintenance blocks input even with the HOST mounted.
    publish(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, true);
    CHECK(!ui_view_shortcuts_input_allowed(&s_sc));
    CHECK(lv_obj_has_state(t[0], LV_STATE_DISABLED));
    click_obj(t[0]);
    CHECK_EQ(s_rec_n, 0);
    CHECK(strstr(lv_label_get_text(s_sc.hint), "Maintenance") != NULL);

    // A REMOTE owner holds control: local input must not inject either.
    publish(KVM_HOST_USB_MOUNTED, OWNER_REMOTE, false);
    CHECK(!ui_view_shortcuts_input_allowed(&s_sc));
    click_obj(t[0]);
    CHECK_EQ(s_rec_n, 0);
    CHECK(strstr(lv_label_get_text(s_sc.hint), "remote client") != NULL);

    // Back to a usable state.
    publish(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, false);
    CHECK(ui_view_shortcuts_input_allowed(&s_sc));
    for (int i = 0; i < TARGETS; i++) {
        CHECK(!lv_obj_has_state(t[i], LV_STATE_DISABLED));
    }
    click_obj(t[0]);
    CHECK_EQ(s_rec_n, 1);

    // A HOST loss while the FR-734 confirmation is open abandons it; nothing is sent.
    s_confirm_policy = true;
    click_obj(s_sc.card[UI_SHORTCUT_CLOSE_WINDOW]);
    CHECK(s_sc.modal != NULL);
    publish(KVM_HOST_USB_SUSPENDED, OWNER_LOCAL, false);
    CHECK(s_sc.modal == NULL);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_sc.pending_chord, KVM_CHORD_NONE);
    s_confirm_policy = false;
    stop();
}

// FR-604 / FR-005: a tile's own labels carry their own text color, and LVGL hands LV_STATE_DISABLED to no
// child object. Every disabled tile must therefore be repainted - the dark one so its white title does not
// vanish on the pale disabled surface, the five plain ones so their near-black title does not read as live.
static void test_disabled_tiles_share_one_ink(void)
{
    CHECK(start());
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        CHECK(s_sc.card_name[i] != NULL);
    }
    const uint8_t enabled_featured = lv_obj_get_style_text_color(s_sc.card_name[0], LV_PART_MAIN).red;
    const uint8_t enabled_plain = lv_obj_get_style_text_color(s_sc.card_name[1], LV_PART_MAIN).red;
    CHECK_EQ(enabled_featured, (UI_COLOR_ON_INVERSE >> 16) & 0xFFu);
    CHECK_EQ(enabled_plain, (UI_COLOR_INK >> 16) & 0xFFu);
    CHECK(enabled_featured != enabled_plain); // the two enabled looks really do differ

    // FR-308 (no HOST), FR-006 (maintenance) and a REMOTE owner all gate local input the same way.
    const struct {
        uint8_t host;
        uint8_t owner;
        bool maint;
    } blocked[3] = {{KVM_HOST_USB_DETACHED, OWNER_NONE, false},
                    {KVM_HOST_USB_MOUNTED, OWNER_LOCAL, true},
                    {KVM_HOST_USB_MOUNTED, OWNER_REMOTE, false}};
    for (int g = 0; g < 3; g++) {
        publish(blocked[g].host, blocked[g].owner, blocked[g].maint);
        CHECK(!ui_view_shortcuts_input_allowed(&s_sc));
        for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
            CHECK(lv_obj_has_state(s_sc.card[i], LV_STATE_DISABLED));
            CHECK_EQ(lv_obj_get_style_opa(s_sc.card[i], LV_PART_MAIN), LV_OPA_COVER);
            CHECK_EQ(lv_obj_get_style_text_color(s_sc.card_name[i], LV_PART_MAIN).red,
                     (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
            CHECK_EQ(lv_obj_get_style_text_color(s_sc.card_sub[i], LV_PART_MAIN).red,
                     (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
            CHECK_EQ(lv_obj_get_style_text_color(s_sc.card_glyph[i], LV_PART_MAIN).red,
                     (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
        }
        // FR-744: the layout tile goes through the same gate, so it must not look live either.
        CHECK(lv_obj_has_state(s_sc.layout_key, LV_STATE_DISABLED));
        CHECK_EQ(lv_obj_get_style_text_color(s_sc.layout_label, LV_PART_MAIN).red,
                 (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
        CHECK_EQ(lv_obj_get_style_text_color(s_sc.layout_sub, LV_PART_MAIN).red,
                 (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
    }

    publish(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, false);
    CHECK_EQ(lv_obj_get_style_text_color(s_sc.card_name[0], LV_PART_MAIN).red, enabled_featured);
    CHECK_EQ(lv_obj_get_style_text_color(s_sc.card_name[1], LV_PART_MAIN).red, enabled_plain);
    CHECK(!lv_obj_has_state(s_sc.layout_key, LV_STATE_DISABLED));
    CHECK_EQ(lv_obj_get_style_text_color(s_sc.layout_label, LV_PART_MAIN).red, (UI_COLOR_INK >> 16) & 0xFFu);
    stop();
}

// An unknown FR-744 chord leaves the tile inert even while the HOST is mounted: it must look inert too.
static void test_layout_tile_without_a_chord_looks_inert(void)
{
    CHECK(start_with(KVM_CHORD_NONE));
    CHECK(ui_view_shortcuts_input_allowed(&s_sc));
    CHECK(lv_obj_has_state(s_sc.layout_key, LV_STATE_DISABLED));
    CHECK_EQ(lv_obj_get_style_text_color(s_sc.layout_label, LV_PART_MAIN).red,
             (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
    stop();
}

// The toast lands in the footer band. The static hint steps aside while it is up, without giving up its box,
// so the "Switch HOST layout" tile beside it does not move.
static void test_hint_steps_aside_for_the_toast(void)
{
    CHECK(start());
    lv_obj_update_layout(s_sc.root);
    lv_area_t tile_before;
    lv_obj_get_coords(s_sc.layout_key, &tile_before);
    CHECK_EQ(lv_obj_get_style_opa(s_sc.hint, LV_PART_MAIN), LV_OPA_COVER);

    click_obj(s_sc.card[UI_SHORTCUT_DESKTOP]);
    CHECK(s_sc.toast != NULL);
    lv_obj_update_layout(s_sc.root);
    CHECK_EQ(lv_obj_get_style_opa(s_sc.hint, LV_PART_MAIN), LV_OPA_TRANSP);
    lv_area_t tile_during;
    lv_obj_get_coords(s_sc.layout_key, &tile_during);
    CHECK(tile_before.x1 == tile_during.x1 && tile_before.x2 == tile_during.x2);
    // The toast keeps clear of the tile: it is centered in the free lane, not stacked on a control.
    lv_area_t toast;
    lv_obj_update_layout(s_sc.toast);
    lv_obj_get_coords(s_sc.toast, &toast);
    CHECK(toast.x2 < tile_during.x1 || toast.x1 > tile_during.x2 || toast.y2 < tile_during.y1 ||
          toast.y1 > tile_during.y2);

    ui_toast_dismiss(s_sc.toast);
    lv_test_wait(5);
    CHECK(s_sc.toast == NULL);
    CHECK_EQ(lv_obj_get_style_opa(s_sc.hint, LV_PART_MAIN), LV_OPA_COVER);
    stop();
}

static void test_refused_request(void)
{
    s_confirm_policy = false;
    CHECK(start());
    s_refuse = true;
    click_obj(s_sc.card[UI_SHORTCUT_SWITCH_WINDOW]);
    CHECK_EQ(s_sink_calls, 1); // submitted once and refused: nothing is retried
    CHECK_EQ(s_rec_n, 0);
    CHECK_EQ(s_sc.c.refused, 1);
    CHECK_EQ(s_sc.c.requests, 0);
    CHECK(strstr(s_sc.toast_text, "Not sent: ") == s_sc.toast_text);
    CHECK(strstr(s_sc.toast_text, "Switch window") != NULL);

    lv_test_wait(100);
    CHECK_EQ(s_sink_calls, 1); // still no retry after the toast

    s_refuse = false;
    click_obj(s_sc.card[UI_SHORTCUT_SWITCH_WINDOW]);
    CHECK_EQ(s_sink_calls, 2);
    CHECK_EQ(s_rec_n, 1);
    CHECK(strstr(s_sc.toast_text, "Requested: ") == s_sc.toast_text);
    stop();
}

static void test_every_chord_is_allowlisted(void)
{
    // 03 §3 rule 2: the view may only name ids the compiled allowlist knows.
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        const uint16_t id = ui_view_shortcuts_command_chord((ui_shortcut_command_t)i);
        const kvm_chord_t *c = kvm_chord_get(id);
        CHECK(c != NULL);
        CHECK_EQ(c->id, id);
        CHECK(ui_action_valid(UI_ACT_SHORTCUT_REQUEST, id));
    }
    for (int i = 0; i < UI_SHORTCUT_MEDIA_COUNT; i++) {
        const uint16_t id = ui_view_shortcuts_media_chord((ui_shortcut_media_t)i);
        CHECK(kvm_chord_get(id) != NULL);
        CHECK(ui_action_valid(UI_ACT_SHORTCUT_REQUEST, id));
    }
    CHECK(kvm_chord_get(KVM_CHORD_LAYOUT_WIN) != NULL);
    CHECK(kvm_chord_get(KVM_CHORD_LAYOUT_CTRL) != NULL);
    CHECK(kvm_chord_get(KVM_CHORD_NONE) == NULL);
    CHECK(!ui_action_valid(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_NONE));
    // The six reference commands are keyboard chords: modifiers DOWN -> key DOWN -> key UP -> modifiers UP.
    for (int i = 0; i < UI_SHORTCUT_COMMAND_COUNT; i++) {
        const kvm_chord_t *c = kvm_chord_get(ui_view_shortcuts_command_chord((ui_shortcut_command_t)i));
        CHECK_EQ(c->kind, KVM_CHORD_KIND_KEYBOARD);
        CHECK(kvm_usage_is_valid_key((uint8_t)c->usage));
    }
}

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void test_geometry(void)
{
    lv_obj_t *t[TARGETS];
    lv_area_t a[TARGETS];
    CHECK(start());
    collect(t);
    lv_obj_update_layout(s_sc.root);
    for (int i = 0; i < TARGETS; i++) {
        lv_obj_get_coords(t[i], &a[i]);
        const int32_t w = lv_area_get_width(&a[i]);
        const int32_t h = lv_area_get_height(&a[i]);
        CHECK(w >= UI_TOUCH_MIN && h >= UI_TOUCH_MIN);            // 07 §7 / FR-003 floor
        CHECK(a[i].x1 >= 0 && a[i].x2 < SCR_W);                   // FR-002: nothing leaves the panel
        CHECK(a[i].y1 >= TOP_H && a[i].y2 < SCR_H);               // and nothing covers the permanent top bar
        if (i < UI_SHORTCUT_COMMAND_COUNT) {
            CHECK(w >= UI_TOUCH_COMFORT && h >= UI_TOUCH_COMFORT); // the commands are large cards
        }
    }
    for (int i = 0; i < TARGETS; i++) {
        for (int j = i + 1; j < TARGETS; j++) {
            CHECK(!overlaps(&a[i], &a[j]));
        }
    }
    printf("    card0 %dx%d at (%d,%d); media0 %dx%d; layout %dx%d\n", (int)lv_area_get_width(&a[0]),
           (int)lv_area_get_height(&a[0]), (int)a[0].x1, (int)a[0].y1,
           (int)lv_area_get_width(&a[UI_SHORTCUT_COMMAND_COUNT]),
           (int)lv_area_get_height(&a[UI_SHORTCUT_COMMAND_COUNT]), (int)lv_area_get_width(&a[TARGETS - 1]),
           (int)lv_area_get_height(&a[TARGETS - 1]));
    lv_area_t pad;
    CHECK(!ui_router_pad_area(&s_r, &pad)); // no touchpad on this screen
    stop();
}

static void test_lifecycle(void)
{
    // store deleted first, while the view still observes its subjects
    s_confirm_policy = false;
    CHECK(start());
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_test_wait(10);
    stop();

    // 1000 create / destroy cycles through the router, half of them with a tap and a live toast
    CHECK(start());
    stop();
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 1000; i++) {
        CHECK(start());
        if (i % 2 == 0) {
            click_obj(s_sc.card[i % UI_SHORTCUT_COMMAND_COUNT]);
        }
        stop();
    }
    printf("    1000 view create/destroy cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);

    // a confirmation open while the view is torn down must not outlive it
    s_confirm_policy = true;
    const size_t base2 = host_lv_live_count();
    for (int i = 0; i < 50; i++) {
        CHECK(start());
        click_obj(s_sc.card[UI_SHORTCUT_CLOSE_WINDOW]);
        CHECK(s_sc.modal != NULL);
        stop();
        CHECK(s_sc.modal == NULL);
    }
    CHECK_EQ(host_lv_live_count(), base2);
    s_confirm_policy = false;
}

#define RUN_MEM(t)                                       \
    do {                                                 \
        size_t before_ = host_lv_live_count();           \
        RUN(t);                                          \
        CHECK_EQ(host_lv_live_count(), before_);         \
    } while (0)

int main(void)
{
    printf("test_ui_view_shortcuts (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    s_confirm_policy = false;
    CHECK(start()); // warm-up: first text render, first press, first toast
    click_obj(s_sc.card[UI_SHORTCUT_SCREENSHOT]);
    stop();

    RUN_MEM(test_commands_submit_their_chord);
    RUN_MEM(test_media_row);
    RUN_MEM(test_layout_chord_follows_init);
    RUN_MEM(test_alt_f4_confirmation);
    RUN_MEM(test_local_input_not_allowed);
    RUN_MEM(test_disabled_tiles_share_one_ink);
    RUN_MEM(test_layout_tile_without_a_chord_looks_inert);
    RUN_MEM(test_hint_steps_aside_for_the_toast);
    RUN_MEM(test_refused_request);
    RUN(test_every_chord_is_allowlisted);
    RUN_MEM(test_geometry);
    RUN_MEM(test_lifecycle);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_view_shortcuts");
}
