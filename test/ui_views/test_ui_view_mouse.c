// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-070A with the real LVGL 9.6 (headless 1024x600, LVGL test pointer, ASan + UBSan), the real router, store,
// mailbox and action gateway; the domain sink is a fake HOST that tracks what is held:
// - Right-click: tap = DOWN then UP, hold = DOWN only until release, sliding off = UP (PRESS_LOST), FR-108;
// - HOST not mounted: buttons disabled, nothing submitted, "Host disconnected" shown; HOST lost while held = UP;
// - navigation while Right-click is held: the router's release request and the view's own UP (AT-UIFW-08);
// - a refused DOWN never leaves a local "held" flag (no UP owed);
// - FR-116/602: a one-finger wheel tap submits a single step, only while the HOST is mounted;
// - Drag lock: the click asks for the opposite of the authoritative state; the button follows the store only
//   (FR-107/115, AGENTS-UI §5);
// - geometry: the pad rectangle reported to the touch router excludes the foot buttons, targets >= 56 px;
// - gesture help: "?" opens a blocking card that takes the pad away from the gesture engine and blocks navigation;
//   Close, a tap on the card and teardown with the card open send nothing to the HOST;
// - lifecycle: store deleted before the router, 1000 view create/destroy cycles without live-allocation growth
//   (AT-UIFW-09).
#include <string.h>

#include "hid_core.h"
#include "host_lv_mem.h"
#include "kvm_hid_intent.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_overlay.h"
#include "ui_router.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_view_mouse.h"

#define SCR_W 1024
#define SCR_H 600
#define TOP_H 80

// ---- fake domain ---------------------------------------------------------------------------
typedef struct {
    ui_action_type_t type;
    uint16_t value;
} rec_t;

static rec_t s_rec[64];
static int s_rec_n;
static bool s_host_right_down; // what the HOST holds
static bool s_refuse;          // the sink refuses the next actions
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
    if (a->value == KVM_MOUSE_BTN_RIGHT && a->type == UI_ACT_MOUSE_BUTTON_DOWN) {
        s_host_right_down = true;
    }
    if (a->value == KVM_MOUSE_BTN_RIGHT && a->type == UI_ACT_MOUSE_BUTTON_UP) {
        s_host_right_down = false;
    }
    return true;
}

static int s_cancels;
static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    (void)why;
    s_cancels++;
    s_host_right_down = false; // RELEASE_ALL accepted by the HID core
    return true;
}

static const ui_nav_hooks_t k_hooks = {.cancel_input = hook_cancel};

// ---- second view, for navigation -----------------------------------------------------------
static lv_obj_t *kbd_create(void *ctx, lv_obj_t *slot)
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
static ui_view_mouse_t s_mouse;
static ui_view_descriptor_t s_desc[2];
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static uint32_t s_seq;

static void publish_protocol(uint8_t host, bool drag, bool boot)
{
    ui_hid_slice_t h = {.host_usb_state = host, .mouse_boot_protocol = boot};
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_gesture_slice_t g = {.drag_lock_active = drag};
    ui_mailbox_publish_gesture(&s_mb, &g, s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static void publish(uint8_t host, bool drag)
{
    publish_protocol(host, drag, false);
}

static bool start(void)
{
    memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0;
    s_host_right_down = false;
    s_refuse = false;
    s_cancels = 0;
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    s_store_on = true;
    ui_actions_init(&s_act);
    ui_actions_set_domain_sink(&s_act, fake_sink, NULL);
    ui_view_mouse_init(&s_mouse, &s_store, &s_act, fake_now);
    s_desc[0] = s_mouse.desc;
    s_desc[1] = (ui_view_descriptor_t){.id = UI_VIEW_KEYBOARD, .create = kbd_create};
    s_scr = lv_obj_create(NULL);
    lv_screen_load(s_scr);
    lv_display_t *display = lv_display_get_default();
    const ui_shell_layout_t lay = {.width = lv_display_get_horizontal_resolution(display),
                                   .height = lv_display_get_vertical_resolution(display), .topbar_h = TOP_H};
    uint8_t n_ov = 0;
    const ui_overlay_descriptor_t *ov = ui_view_mouse_overlays(&s_mouse, &n_ov);
    bool ok = ui_router_init(&s_r, s_scr, &lay, &s_store, s_desc, 2, ov, n_ov, &k_hooks);
    ui_view_mouse_bind_router(&s_mouse, &s_r);
    ok = ok && ui_router_show(&s_r, UI_VIEW_MOUSE);
    publish(KVM_HOST_USB_MOUNTED, false);
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

static void center_of(lv_obj_t *o, int32_t *x, int32_t *y)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

static void press_obj(lv_obj_t *o)
{
    int32_t x, y;
    center_of(o, &x, &y);
    lv_test_mouse_move_to(x, y);
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

// ---- tests ---------------------------------------------------------------------------------
static void test_right_click_tap(void)
{
    CHECK(start());
    click_obj(s_mouse.btn_right);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[0].type, UI_ACT_MOUSE_BUTTON_DOWN);
    CHECK_EQ(s_rec[0].value, KVM_MOUSE_BTN_RIGHT);
    CHECK_EQ(s_rec[1].type, UI_ACT_MOUSE_BUTTON_UP);
    CHECK_EQ(s_rec[1].value, KVM_MOUSE_BTN_RIGHT);
    CHECK(!s_host_right_down);
    CHECK(!(s_mouse.held_mask & 2u));
    stop();
}

static void test_right_click_hold_and_slide_off(void)
{
    CHECK(start());
    press_obj(s_mouse.btn_right);
    lv_test_wait(700); // longer than LVGL's long-press time: still one DOWN, no UP, no repeat
    CHECK_EQ(s_rec_n, 1);
    CHECK(s_host_right_down);
    release();
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK(!s_host_right_down);

    // press, then slide onto the pad and lift there: PRESS_LOST must give the UP
    s_rec_n = 0;
    press_obj(s_mouse.btn_right);
    CHECK(s_host_right_down);
    int32_t x, y;
    center_of(s_mouse.pad, &x, &y);
    lv_test_mouse_move_to(x, y);
    lv_test_wait(50);
    release();
    CHECK(!s_host_right_down);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1);
    stop();
}

static void test_host_not_mounted(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_DETACHED, false);
    CHECK(lv_obj_has_state(s_mouse.btn_right, LV_STATE_DISABLED));
    CHECK(lv_obj_has_state(s_mouse.btn_drag, LV_STATE_DISABLED));
    CHECK(!lv_obj_is_hidden(s_mouse.offline));
    click_obj(s_mouse.btn_right);
    click_obj(s_mouse.btn_drag);
    CHECK_EQ(s_rec_n, 0);

    // back: enabled again, overlay hidden
    publish(KVM_HOST_USB_MOUNTED, false);
    CHECK(!lv_obj_has_state(s_mouse.btn_right, LV_STATE_DISABLED));
    CHECK(lv_obj_is_hidden(s_mouse.offline));

    // HOST lost while Right-click is held: the owed UP goes out at once
    press_obj(s_mouse.btn_right);
    CHECK((s_mouse.held_mask & 2u));
    publish(KVM_HOST_USB_SUSPENDED, false);
    CHECK(!(s_mouse.held_mask & 2u));
    CHECK(!s_host_right_down);
    CHECK_EQ(s_mouse.c.forced_releases, 1);
    release();
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1); // not twice
    stop();
}

static void wheel_position(int percent)
{
    lv_area_t area;
    lv_obj_get_coords(s_mouse.scroll_wheel, &area);
    lv_test_mouse_move_to((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2 - percent * (area.y2 - area.y1) / 200);
}

static void wheel_tap(int position)
{
    wheel_position(position);
    lv_test_mouse_press();
    lv_test_wait(40);
    release();
}

static void test_wheel_steps_and_host_loss(void)
{
    CHECK(start());
    press_obj(s_mouse.scroll_wheel);
    lv_test_wait(600);
    CHECK_EQ(s_rec_n, 0); // neutral
    wheel_position(25);
    lv_test_wait(800);
    int slow = s_rec_n;
    CHECK(slow >= 1);
    CHECK_EQ(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP), slow);
    wheel_position(-90);
    lv_test_wait(800);
    CHECK(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_DOWN) > slow);
    wheel_position(0);
    lv_test_wait(60);
    int stopped = s_rec_n;
    lv_test_wait(600);
    CHECK_EQ(s_rec_n, stopped);
    release();
    wheel_tap(-80);
    CHECK_EQ(s_rec_n, stopped + 1);
    lv_test_wait(500);
    CHECK_EQ(s_rec_n, stopped + 1);

    wheel_position(90);
    lv_test_mouse_press();
    lv_test_wait(40);
    publish(KVM_HOST_USB_SUSPENDED, false);
    CHECK(lv_obj_has_state(s_mouse.scroll_wheel, LV_STATE_DISABLED));
    stopped = s_rec_n;
    publish(KVM_HOST_USB_MOUNTED, false);
    lv_test_wait(500);
    release();
    CHECK_EQ(s_rec_n, stopped);
    stop();
}

static void test_wheel_tread_in_each_palette(void)
{
    CHECK(start());
    lv_obj_t *meter = lv_obj_get_child(s_mouse.scroll_wheel, 0);
    for (unsigned i = 0; i < UI_PALETTE_COUNT; ++i) {
        ui_theme_set_control_palette((ui_control_palette_t)i);
        lv_draw_buf_t *image = lv_snapshot_take(meter, LV_COLOR_FORMAT_RGB565);
        CHECK(image != NULL);
        if (!image) continue;
        // Each horizontal rib has a pixel boundary on both sides. A vertical track
        // with a round knob has a nearly uniform center column.
        uint32_t x = image->header.w / 2;
        unsigned transitions = 0;
        for (uint32_t y = 3; y + 3 < image->header.h; ++y) {
            const void *above = lv_draw_buf_goto_xy(image, x, y - 1);
            const void *pixel = lv_draw_buf_goto_xy(image, x, y);
            if (memcmp(above, pixel, 2) != 0) ++transitions;
        }
        CHECK(transitions >= 16);
        lv_draw_buf_destroy(image);
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
    stop();
}

static void test_wheel_capture_past_both_ends(void)
{
    CHECK(start());
    lv_area_t rail;
    lv_obj_get_coords(s_mouse.scroll_wheel, &rail);
    const int32_t x = (rail.x1 + rail.x2) / 2;
    press_obj(s_mouse.scroll_wheel);
    lv_test_mouse_move_to(x, rail.y1 - 12);
    lv_test_wait(240);
    int up = count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP);
    CHECK(up >= 2);
    lv_test_wait(240);
    CHECK(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP) > up);
    wheel_position(0);
    lv_test_wait(60);
    int neutral = s_rec_n;
    lv_test_wait(240);
    CHECK_EQ(s_rec_n, neutral);
    lv_test_mouse_move_to(rail.x2 + 5, rail.y2 + 12);
    lv_test_wait(240);
    CHECK(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_DOWN) >= 2);
    release();
    int released = s_rec_n;
    lv_test_wait(240);
    CHECK_EQ(s_rec_n, released);
    CHECK_EQ(s_rec_n, count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP) + count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_DOWN));
    stop();
}

static bool overlaps(const lv_obj_t *a, const lv_obj_t *b)
{
    lv_area_t x, y;
    lv_obj_get_coords(a, &x);
    lv_obj_get_coords(b, &y);
    return x.x1 <= y.x2 && y.x1 <= x.x2 && x.y1 <= y.y2 && y.y1 <= x.y2;
}

// The HOST-lost notice explains the state; it must not swallow the pad's own header line, and the invitation
// it contradicts goes away while it is up.
static void test_offline_notice_leaves_the_header_readable(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_DETACHED, false);
    lv_obj_update_layout(s_mouse.pad);
    CHECK(!lv_obj_is_hidden(s_mouse.offline));
    CHECK(lv_obj_is_hidden(s_mouse.idle_title));
    CHECK(lv_obj_is_hidden(s_mouse.idle_sub));
    CHECK(lv_obj_is_hidden(s_mouse.idle_contact));
    CHECK(lv_obj_is_hidden(s_mouse.live));

    CHECK(!overlaps(s_mouse.offline, lv_obj_get_child(s_mouse.pad, 0))); // Touchpad heading
    // The notice is sized by its text, not by the pad: it leaves room above and below.
    lv_area_t notice, pad;
    lv_obj_get_coords(s_mouse.offline, &notice);
    lv_obj_get_coords(s_mouse.pad, &pad);
    CHECK(notice.y1 > pad.y1 && notice.y2 < pad.y2);

    publish(KVM_HOST_USB_MOUNTED, false);
    CHECK(lv_obj_is_hidden(s_mouse.offline));
    CHECK(!lv_obj_is_hidden(s_mouse.idle_title));
    CHECK(!lv_obj_is_hidden(s_mouse.idle_sub));
    CHECK(!lv_obj_is_hidden(s_mouse.idle_contact));
    CHECK(!lv_obj_is_hidden(s_mouse.live));
    stop();
}

static void test_navigation_while_held(void)
{
    CHECK(start());
    press_obj(s_mouse.btn_right);
    CHECK(s_host_right_down);
    const int cancels = s_cancels;
    CHECK(ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK_EQ(s_cancels, cancels + 1);      // router: touch cancel + HID release request
    CHECK(!(s_mouse.held_mask & 2u));            // view: local press state gone
    CHECK(!s_host_right_down);
    release();                             // the finger lifts on the new screen
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK(ui_router_show(&s_r, UI_VIEW_MOUSE));
    click_obj(s_mouse.btn_right); // still usable after coming back
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_RIGHT), 2);
    CHECK(!s_host_right_down);
    stop();
}

static void test_left_click_and_both(void)
{
    CHECK(start());
    click_obj(s_mouse.btn_left);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[0].type, UI_ACT_MOUSE_BUTTON_DOWN);
    CHECK_EQ(s_rec[0].value, KVM_MOUSE_BTN_LEFT);
    CHECK_EQ(s_rec[1].type, UI_ACT_MOUSE_BUTTON_UP);
    CHECK_EQ(s_rec[1].value, KVM_MOUSE_BTN_LEFT);
    CHECK_EQ(s_mouse.held_mask, 0);
    // hold Left, then the HOST goes away: exactly one UP for Left, none for Right
    s_rec_n = 0;
    press_obj(s_mouse.btn_left);
    CHECK_EQ(s_mouse.held_mask, 1);
    publish(KVM_HOST_USB_DETACHED, false);
    CHECK_EQ(s_mouse.held_mask, 0);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_LEFT), 1);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 0);
    release();
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_LEFT), 1);
    stop();
}

static void test_refused_down(void)
{
    CHECK(start());
    s_refuse = true;
    press_obj(s_mouse.btn_right);
    CHECK(!(s_mouse.held_mask & 2u));
    s_refuse = false;
    release();
    CHECK_EQ(s_rec_n, 0); // no UP owed for a DOWN that never went out
    CHECK_EQ(s_mouse.c.refused, 1);
    stop();
}

static void test_drag_lock_follows_store(void)
{
    CHECK(start());
    CHECK(!lv_obj_has_state(s_mouse.btn_drag, LV_STATE_CHECKED));
    click_obj(s_mouse.btn_drag);
    CHECK_EQ(s_rec_n, 1);
    CHECK_EQ(s_rec[0].type, UI_ACT_DRAG_LOCK_REQUEST);
    CHECK_EQ(s_rec[0].value, 1);
    CHECK(!lv_obj_has_state(s_mouse.btn_drag, LV_STATE_CHECKED)); // not optimistic
    CHECK(strcmp(lv_label_get_text(s_mouse.live), "Ready") == 0);

    publish(KVM_HOST_USB_MOUNTED, true); // the gesture engine says: on
    CHECK(lv_obj_has_state(s_mouse.btn_drag, LV_STATE_CHECKED));
    CHECK(strcmp(lv_label_get_text(s_mouse.live), "Dragging") == 0);
    CHECK(strstr(lv_label_get_text(s_mouse.hint), "Drag lock is on") != NULL);
    CHECK(strcmp(lv_label_get_text(s_mouse.drag_label), "Release") == 0); // FR-107

    click_obj(s_mouse.btn_drag);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(s_rec[1].value, 0); // the opposite of the authoritative state
    publish(KVM_HOST_USB_MOUNTED, false);
    CHECK(!lv_obj_has_state(s_mouse.btn_drag, LV_STATE_CHECKED));
    CHECK(strcmp(lv_label_get_text(s_mouse.live), "Ready") == 0);
    CHECK(strcmp(lv_label_get_text(s_mouse.drag_label), "Drag lock") == 0);
    stop();
}

static void test_drag_lock_available_and_disabled_palettes(void)
{
    CHECK(start());
    ui_theme_apply(s_scr, UI_STYLE_SCREEN, LV_PART_MAIN);
    for (unsigned p = 0; p < UI_PALETTE_COUNT; ++p) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        publish(KVM_HOST_USB_MOUNTED, false);
        const uint16_t available_ink = lv_color_to_u16(lv_obj_get_style_text_color_filtered(s_mouse.drag_label, 0));
        const uint16_t available_fill = lv_color_to_u16(lv_obj_get_style_bg_color_filtered(s_mouse.btn_drag, 0));
        click_obj(s_mouse.btn_drag);
        CHECK_EQ(count(UI_ACT_DRAG_LOCK_REQUEST, 1), (int)p + 1);
        CHECK(!lv_obj_has_state(s_mouse.btn_drag, LV_STATE_CHECKED));
        publish(KVM_HOST_USB_MOUNTED, true);
        CHECK(strcmp(lv_label_get_text(s_mouse.drag_label), "Release") == 0);
        CHECK(available_fill != lv_color_to_u16(lv_obj_get_style_bg_color_filtered(s_mouse.btn_drag, 0)));
        publish(KVM_HOST_USB_DETACHED, false);
        CHECK(lv_obj_has_state(s_mouse.btn_drag, LV_STATE_DISABLED));
        CHECK(available_ink != lv_color_to_u16(lv_obj_get_style_text_color_filtered(s_mouse.drag_label, 0)));
        CHECK(available_fill != lv_color_to_u16(lv_obj_get_style_bg_color_filtered(s_mouse.btn_drag, 0)));
        click_obj(s_mouse.btn_drag);
        CHECK_EQ(count(UI_ACT_DRAG_LOCK_REQUEST, 1), (int)p + 1);
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
    stop();
}

static lv_obj_t *help_close_button(void)
{
    // the card's last child is the footer; its only child is Close
    lv_obj_t *foot = lv_obj_get_child(s_mouse.help_card, -1);
    return foot != NULL ? lv_obj_get_child(foot, 0) : NULL;
}

static bool visible_label(lv_obj_t *root, const char *caption)
{
    if (lv_obj_check_type(root, &lv_label_class) && lv_obj_is_visible(root) &&
        strcmp(lv_label_get_text(root), caption) == 0) return true;
    for (uint32_t i = 0; i < lv_obj_get_child_count(root); i++) {
        if (visible_label(lv_obj_get_child(root, (int32_t)i), caption)) return true;
    }
    return false;
}

static void test_wheel_protocol_change(void)
{
    const char *boot_hint = "Host boot protocol has no wheel";
    CHECK(start());
    press_obj(s_mouse.scroll_wheel);
    publish_protocol(KVM_HOST_USB_MOUNTED, false, true);
    publish_protocol(KVM_HOST_USB_MOUNTED, false, false);
    release();
    CHECK_EQ(s_rec_n, 0);
    click_obj(s_mouse.btn_help);
    CHECK(s_mouse.help_card != NULL);
    CHECK(!visible_label(s_mouse.help_card, boot_hint));
    publish_protocol(KVM_HOST_USB_MOUNTED, false, true);
    CHECK(visible_label(s_mouse.help_card, boot_hint));
    CHECK(lv_obj_has_state(s_mouse.scroll_wheel, LV_STATE_DISABLED));
    click_obj(help_close_button());
    wheel_tap(80);
    wheel_tap(-80);
    CHECK_EQ(s_rec_n, 0);
    click_obj(s_mouse.btn_left);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_LEFT), 1);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_LEFT), 1);
    click_obj(s_mouse.btn_help);
    CHECK(visible_label(s_mouse.help_card, boot_hint));
    publish_protocol(KVM_HOST_USB_MOUNTED, false, false);
    CHECK(!visible_label(s_mouse.help_card, boot_hint));
    CHECK(!lv_obj_has_state(s_mouse.scroll_wheel, LV_STATE_DISABLED));
    click_obj(help_close_button());
    s_rec_n = 0;
    wheel_tap(80);
    wheel_tap(-80);
    CHECK_EQ(s_rec_n, 2);
    CHECK_EQ(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP), 1);
    CHECK_EQ(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_DOWN), 1);
    stop();
}

static void test_gesture_help(void)
{
    CHECK(start());
    lv_area_t pad;
    CHECK(ui_router_pad_area(&s_r, &pad));
    CHECK(s_mouse.help_card == NULL);
    click_obj(s_mouse.btn_help);
    CHECK(s_mouse.help_card != NULL);
    CHECK_EQ(s_mouse.c.help_opens, 1);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_GESTURE_HELP);
    CHECK(ui_overlay_blocking(&s_r));
    CHECK(!ui_router_pad_area(&s_r, &pad)); // the gesture engine gets no touches meanwhile
    CHECK_EQ(s_rec_n, 0);                   // opening it sends nothing

    // the card fits the screen, lists every row and has a Close target of >= 56 px
    lv_obj_update_layout(s_mouse.help_card);
    lv_area_t card;
    lv_obj_get_coords(s_mouse.help_card, &card);
    printf("    help card %dx%d at %d,%d\n", (int)lv_area_get_width(&card), (int)lv_area_get_height(&card), (int)card.x1,
           (int)card.y1);
    CHECK(card.x1 >= 0 && card.y1 >= 0 && card.x2 < SCR_W && card.y2 < SCR_H);
    CHECK_EQ(lv_obj_get_child_count(s_mouse.help_card), 1u + UI_VIEW_MOUSE_HELP_ROWS + 1u);
    for (uint8_t i = 0; i < UI_VIEW_MOUSE_HELP_ROWS; i++) {
        CHECK(ui_view_mouse_help_gesture(i) != NULL && ui_view_mouse_help_result(i) != NULL);
    }
    CHECK(ui_view_mouse_help_gesture(UI_VIEW_MOUSE_HELP_ROWS) == NULL);
    CHECK(ui_view_mouse_help_result(UI_VIEW_MOUSE_HELP_ROWS) == NULL);
    lv_obj_t *close = help_close_button();
    CHECK(close != NULL);
    lv_area_t cb;
    lv_obj_get_coords(close, &cb);
    CHECK(lv_area_get_height(&cb) >= 56 && lv_area_get_width(&cb) >= 56);

    // a tap on the card body: still open, nothing sent; navigation is refused while it is open
    lv_test_mouse_move_to(card.x1 + 20, card.y1 + 20);
    lv_test_mouse_press();
    lv_test_wait(50);
    release();
    CHECK(s_mouse.help_card != NULL);
    CHECK(!ui_router_show(&s_r, UI_VIEW_KEYBOARD));
    CHECK_EQ(s_rec_n, 0);

    press_obj(close);
    lv_area_t pressed_card, pressed_button;
    lv_obj_get_coords(s_mouse.help_card, &pressed_card);
    lv_obj_get_coords(close, &pressed_button);
    CHECK(pressed_card.x1 == card.x1 && pressed_card.y1 == card.y1 &&
          pressed_card.x2 == card.x2 && pressed_card.y2 == card.y2);
    CHECK_EQ(pressed_button.y1, cb.y1 + 1);
    CHECK_EQ(lv_area_get_height(&pressed_button), lv_area_get_height(&cb));
    release();
    CHECK(s_mouse.help_card == NULL);
    CHECK_EQ(s_mouse.c.help_closes, 1);
    CHECK_EQ(ui_overlay_top(&s_r), 0);
    CHECK(ui_router_pad_area(&s_r, &pad));
    CHECK_EQ(s_rec_n, 0);
    click_obj(s_mouse.btn_right); // the footer works again
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    stop();

    // teardown with the card open: on_close runs, nothing leaks (RUN_MEM)
    CHECK(start());
    click_obj(s_mouse.btn_help);
    CHECK(s_mouse.help_card != NULL);
    stop();
    CHECK(s_mouse.help_card == NULL);
    CHECK_EQ(s_mouse.c.help_closes, 1);

    // no router bound: "?" is refused and counted
    CHECK(start());
    s_mouse.router = NULL;
    click_obj(s_mouse.btn_help);
    CHECK(s_mouse.help_card == NULL);
    CHECK_EQ(s_mouse.c.help_refused, 1);
    CHECK_EQ(s_rec_n, 0);
    stop();
}

static void test_wheel_controls_at_supported_sizes(void)
{
    const lv_point_t sizes[] = {{SCR_W, SCR_H}, {800, 480}, {640, 480}};
    lv_display_t *display = lv_display_get_default();
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        lv_test_mouse_move_to(0, 0);
        lv_test_wait(40);
        lv_display_set_resolution(display, sizes[i].x, sizes[i].y);
        CHECK(start());
        lv_obj_t *buttons[] = {s_mouse.btn_help, s_mouse.btn_left, s_mouse.btn_right, s_mouse.btn_drag,
                               s_mouse.scroll_wheel};
        lv_area_t pad;
        lv_obj_update_layout(s_mouse.root);
        CHECK(ui_router_pad_area(&s_r, &pad));
        for (unsigned j = 0; j < sizeof(buttons) / sizeof(buttons[0]); j++) {
            lv_area_t target;
            lv_obj_get_coords(buttons[j], &target);
            CHECK(lv_obj_is_visible(buttons[j]));
            CHECK(target.x1 >= 0 && target.x2 < sizes[i].x && target.y1 >= TOP_H && target.y2 < sizes[i].y);
            if (buttons[j] == s_mouse.scroll_wheel) {
                CHECK(target.x1 > pad.x2);
                CHECK(target.y1 <= pad.y2 && target.y2 >= pad.y1);
            } else {
                CHECK(target.y1 > pad.y2);
            }
            CHECK(lv_area_get_width(&target) >= 44 && lv_area_get_height(&target) >= 44);
            for (unsigned k = 0; k < j; k++) CHECK(!overlaps(buttons[k], buttons[j]));
        }
        wheel_tap(80);
        wheel_tap(-80);
        CHECK_EQ(s_rec_n, 2);
        CHECK_EQ(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_UP), 1);
        CHECK_EQ(count(UI_ACT_MOUSE_WHEEL_STEP, UI_WHEEL_DOWN), 1);
        stop();
    }
    lv_display_set_resolution(display, SCR_W, SCR_H);
}

static void test_geometry(void)
{
    CHECK(start());
    lv_area_t pad, br, bd;
    CHECK(ui_router_pad_area(&s_r, &pad));
    lv_obj_get_coords(s_mouse.btn_right, &br);
    lv_obj_get_coords(s_mouse.btn_drag, &bd);
    printf("    pad x %d..%d y %d..%d; right-click %dx%d at y %d; drag lock %dx%d\n", (int)pad.x1, (int)pad.x2,
           (int)pad.y1, (int)pad.y2, (int)lv_area_get_width(&br), (int)lv_area_get_height(&br), (int)br.y1,
           (int)lv_area_get_width(&bd), (int)lv_area_get_height(&bd));
    CHECK(pad.y1 >= TOP_H);
    CHECK(pad.x1 >= 0 && pad.x2 < SCR_W && pad.y2 < SCR_H);
    lv_area_t bl;
    lv_obj_get_coords(s_mouse.btn_left, &bl);
    CHECK(pad.y2 < br.y1 && pad.y2 < bd.y1 && pad.y2 < bl.y1); // the buttons are outside the gesture rectangle
    CHECK(bl.x2 < br.x1 && br.x2 < bd.x1);                       // no overlap in the footer
    CHECK(lv_area_get_height(&bl) >= 56);
    CHECK(lv_area_get_height(&br) >= 56 && lv_area_get_width(&br) >= 56);
    CHECK(lv_area_get_height(&bd) >= 56 && lv_area_get_width(&bd) >= 56);
    CHECK(br.y2 < SCR_H && bd.x2 < SCR_W);
    CHECK(lv_area_get_width(&pad) * lv_area_get_height(&pad) > SCR_W * SCR_H / 2); // the pad dominates
    for (unsigned i = 0; i < UI_PALETTE_COUNT; ++i) {
        ui_theme_set_control_palette((ui_control_palette_t)i);
        lv_obj_update_layout(s_mouse.root);
        lv_area_t current;
        CHECK(ui_router_pad_area(&s_r, &current));
        CHECK(current.x1 == pad.x1 && current.x2 == pad.x2 && current.y1 == pad.y1 && current.y2 == pad.y2);
        CHECK(!lv_obj_is_clickable(s_mouse.idle_contact) && !lv_obj_is_clickable(s_mouse.pad));
        CHECK(!overlaps(s_mouse.idle_contact, s_mouse.idle_title));
        CHECK(!overlaps(s_mouse.idle_title, s_mouse.idle_sub));
        CHECK_EQ(lv_obj_get_style_bg_opa(s_mouse.pad, 0), LV_OPA_COVER);
        CHECK_EQ(lv_obj_get_style_bg_grad_dir(s_mouse.pad, 0), LV_GRAD_DIR_NONE);
        if (i == UI_PALETTE_HIGH_CONTRAST) {
            CHECK_EQ(lv_obj_get_style_border_side(s_mouse.pad, 0), LV_BORDER_SIDE_FULL);
            CHECK_EQ(lv_obj_get_style_outline_width(s_mouse.pad, 0), 0);
        }
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
    stop();
}

static void test_lifecycle(void)
{
    // store deleted first, while the view still observes its subjects: the view is destroyed afterwards
    CHECK(start());
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_test_wait(10);
    stop();

    // 1000 create / destroy cycles of the view through the router
    CHECK(start());
    stop();
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 1000; i++) {
        CHECK(start());
        if (i % 2 == 0) {
            click_obj(s_mouse.btn_right);
        }
        stop();
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
    printf("test_ui_view_mouse (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    CHECK(start()); // warm-up: first text render and first press
    click_obj(s_mouse.btn_right);
    stop();

    RUN_MEM(test_right_click_tap);
    RUN_MEM(test_right_click_hold_and_slide_off);
    RUN_MEM(test_host_not_mounted);
    RUN_MEM(test_wheel_steps_and_host_loss);
    RUN_MEM(test_wheel_tread_in_each_palette);
    RUN_MEM(test_wheel_capture_past_both_ends);
    RUN_MEM(test_offline_notice_leaves_the_header_readable);
    RUN_MEM(test_navigation_while_held);
    RUN_MEM(test_left_click_and_both);
    RUN_MEM(test_refused_down);
    RUN_MEM(test_drag_lock_follows_store);
    RUN_MEM(test_drag_lock_available_and_disabled_palettes);
    RUN_MEM(test_gesture_help);
    RUN_MEM(test_geometry);
    RUN_MEM(test_wheel_controls_at_supported_sizes);
    RUN_MEM(test_wheel_protocol_change);
    RUN_MEM(test_lifecycle);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_view_mouse");
}
