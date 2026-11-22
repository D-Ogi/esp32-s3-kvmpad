// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// The product shell composition (ui_app, 07 §5) with the real LVGL 9.6 (headless 1024x600, LVGL test pointer,
// ASan + UBSan), the real router, store, mailbox, action gateway, top bar, mode switcher and views:
// - the indicators show "Unknown" until a domain source publishes, then the published HOST / CLIENT state, never
//   a guessed one (FR-401/403, 07 §12 item 2);
// - the pad rectangle reported to the platform lies inside the content area, below the 80 px top bar;
// - a tap on an indicator opens its blocking details card (FR-003): input cancelled first, the pad taken away,
//   the bar below unreachable, the pad back after the dismiss;
// - FR-731 / FR-751: the gear opens the blocking Settings card the same way, it renders the record published
//   through ui_app_set_settings() and reports one typed change per tap through hooks.settings_change;
// - the mode switcher has one tab per registered view, the active tab follows the router, and a tab tap
//   navigates through the action gateway (a view change asks the platform to cancel input first);
// - ADR-022 step 1: the CLIENT details card carries the pairing entry point; it reports one PAIR_OPEN and
//   dismisses itself, and SEC-106 disables it while remote is off or the Wi-Fi link is down;
// - FR-722 / FR-733: a pairing or approval prompt published by the adapter blocks the pad and the top bar,
//   cancels the input below it, reports Accept / Allow with the right session and drops a pending FR-734
//   confirmation that would otherwise cover it;
// - ui_app_leave() releases a held mouse button (AGENTS.md prohibition 3);
// - 300 init / deinit cycles, half of them with a press, leave the live LVGL allocation count unchanged and the
//   local sink detached (AT-UIFW-02, AT-UIFW-09).
#include <string.h>

#include "hid_core.h"
#include "hid_keymap.h"
#include "host_lv_mem.h"
#include "kvm_control_icons.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_app.h"
#include "ui_overlay.h"
#include "ui_status_item.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_topbar.h"

#define SCR_W 1024
#define SCR_H 600
#define TOP_H 80

// ---- fake platform and domain -----------------------------------------------------------------------------------
typedef struct {
    ui_action_type_t type;
    uint16_t value;
} rec_t;

static rec_t s_rec[64];
static int s_rec_n;
static uint64_t s_clock;
static int s_cancels;
static int s_pad_reports;
static bool s_pad_on;
static lv_area_t s_pad;

static uint64_t fake_now(void)
{
    return s_clock += 1000;
}

static bool fake_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    if (s_rec_n < (int)(sizeof(s_rec) / sizeof(s_rec[0]))) {
        s_rec[s_rec_n++] = (rec_t){a->type, a->value};
    }
    return true;
}

static bool hook_cancel(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    (void)why;
    s_cancels++;
    return true;
}

static void hook_pad(void *ctx, const lv_area_t *pad)
{
    (void)ctx;
    s_pad_reports++;
    s_pad_on = pad != NULL;
    if (pad) {
        s_pad = *pad;
    }
}

static bool s_confirm_alt_f4;

static bool hook_confirm(void *ctx)
{
    (void)ctx;
    return s_confirm_alt_f4;
}

#define REMOTE_MAX 8
static struct {
    ui_remote_action_t act;
    uint64_t session;
} s_remote[REMOTE_MAX];
static int s_remote_n;

static void hook_remote(void *ctx, ui_remote_action_t act, uint64_t session)
{
    (void)ctx;
    if (s_remote_n < REMOTE_MAX) {
        s_remote[s_remote_n].act = act;
        s_remote[s_remote_n].session = session;
        s_remote_n++;
    }
}

#define SETTINGS_MAX 8
static struct {
    ui_setting_id_t id;
    int32_t value;
    uint8_t index;
} s_setting[SETTINGS_MAX];
static int s_setting_n;

static void hook_settings(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    (void)ctx;
    if (s_setting_n < SETTINGS_MAX) {
        s_setting[s_setting_n].id = id;
        s_setting[s_setting_n].value = value;
        s_setting[s_setting_n].index = client_index;
        s_setting_n++;
    }
}

static const ui_app_hooks_t k_hooks = {.now_us = fake_now,
                                       .cancel_input = hook_cancel,
                                       .pad_changed = hook_pad,
                                       .confirm_alt_f4 = hook_confirm,
                                       .remote_action = hook_remote,
                                       .settings_change = hook_settings};

// ---- fixture ----------------------------------------------------------------------------------------------------
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static ui_actions_t s_act;
static ui_app_t s_app;
static lv_obj_t *s_home;
static uint32_t s_seq;

static void publish_link(uint8_t host, uint8_t client, bool wifi)
{
    ui_hid_slice_t h = {.host_usb_state = host};
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_net_slice_t n = {.client_state = client, .wifi_connected = wifi};
    ui_mailbox_publish_net(&s_mb, &n, s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static void publish(uint8_t host, uint8_t client)
{
    publish_link(host, client, false);
}

static bool start(void)
{
    memset(s_rec, 0, sizeof(s_rec));
    memset(s_remote, 0, sizeof(s_remote));
    s_rec_n = 0;
    s_remote_n = 0;
    s_cancels = 0;
    s_pad_reports = 0;
    s_pad_on = false;
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    ui_actions_init(&s_act);
    ui_actions_set_domain_sink(&s_act, fake_sink, NULL);
    bool ok = ui_app_init(&s_app, &s_store, &s_act, &k_hooks);
    if (ok) {
        lv_screen_load(s_app.screen);
        ui_app_show(&s_app);
        lv_test_wait(20);
    }
    return ok;
}

static void stop(void)
{
    lv_screen_load(s_home);
    lv_test_wait(5);
    ui_app_deinit(&s_app);
    ui_store_deinit(&s_store);
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

static const char *status_text(ui_topbar_status_t slot)
{
    lv_obj_t *item = ui_topbar_status_item(s_app.bar, slot);
    lv_obj_t *label = item ? ui_status_item_value_label(item) : NULL;
    return label ? lv_label_get_text(label) : "";
}

// ---- tests ------------------------------------------------------------------------------------------------------
static void test_build_and_indicators(void)
{
    CHECK(start());
    CHECK(s_app.screen != NULL);
    CHECK(s_app.bar != NULL);
    CHECK(s_app.nav != NULL);
    CHECK_EQ(ui_router_active(&s_app.router), UI_VIEW_MOUSE);
    CHECK(strcmp(status_text(UI_TOPBAR_HOST), "Unknown") == 0);   // nothing published yet: never a guess
    CHECK(strcmp(status_text(UI_TOPBAR_CLIENT), "Unknown") == 0);
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    CHECK(strcmp(status_text(UI_TOPBAR_HOST), "Active") == 0);
    CHECK(strcmp(status_text(UI_TOPBAR_CLIENT), "Off") == 0);
    publish(KVM_HOST_USB_SUSPENDED, UI_CLIENT_STALE);
    CHECK(strcmp(status_text(UI_TOPBAR_HOST), "Suspended") == 0);
    CHECK(strcmp(status_text(UI_TOPBAR_CLIENT), "Reconnecting") == 0);
    stop();
}

static void test_pad_rectangle(void)
{
    CHECK(start());
    CHECK(s_pad_reports >= 1);
    CHECK(s_pad_on);
    CHECK(s_pad.y1 >= TOP_H);
    CHECK(s_pad.x1 >= 0 && s_pad.x2 < SCR_W && s_pad.y2 < SCR_H);
    CHECK(s_pad.x2 - s_pad.x1 > SCR_W / 2);
    printf("    pad x %d..%d y %d..%d\n", (int)s_pad.x1, (int)s_pad.x2, (int)s_pad.y1, (int)s_pad.y2);
    stop();
    CHECK(!s_pad_on); // deinit takes the pad away
}

static unsigned extension_clicks;
static void test_diagnostics_overlay(void)
{
    CHECK(start());
    ui_settings_view_t v = {.diag_overlay = true};
    ui_app_set_settings(&s_app, &v);
    CHECK(s_app.diagnostics == NULL); // no published setting
    v.seq = 1;
    ui_app_set_settings(&s_app, &v);
    CHECK(s_app.diagnostics != NULL);
    CHECK(strcmp(lv_label_get_text(s_app.diagnostics), "Mouse HID  Unknown    Touch  Unknown") == 0);
    ui_hid_slice_t h = {.host_usb_state = KVM_HOST_USB_MOUNTED, .mouse_boot_protocol = true};
    ui_touch_slice_t t = {.touch_healthy = true};
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_mailbox_publish_touch(&s_mb, &t, s_seq);
    ui_store_tick(&s_store);
    CHECK(strcmp(lv_label_get_text(s_app.diagnostics), "Mouse HID  Boot    Touch  Ready") == 0);
    lv_screen_load(s_home);
    lv_test_wait(5);
    lv_obj_t *layer = lv_display_get_layer_top(lv_obj_get_display(s_home));
    CHECK(lv_obj_get_parent(s_app.diagnostics) == layer);
    lv_point_t point;
    center_of(s_app.diagnostics, &point.x, &point.y);
    CHECK(lv_indev_search_obj(layer, &point) == NULL); // top-layer hit test used by touchpad surfaces
    h.mouse_boot_protocol = false;
    t.touch_healthy = false;
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_mailbox_publish_touch(&s_mb, &t, s_seq);
    ui_store_tick(&s_store);
    CHECK(strcmp(lv_label_get_text(s_app.diagnostics), "Mouse HID  Report    Touch  Fault") == 0);
    publish(KVM_HOST_USB_DETACHED, UI_CLIENT_DISABLED);
    CHECK(strcmp(lv_label_get_text(s_app.diagnostics), "Mouse HID  Disconnected    Touch  Fault") == 0);
    v.diag_overlay = false;
    ui_app_set_settings(&s_app, &v);
    CHECK(s_app.diagnostics == NULL);
    v.diag_overlay = true;
    ui_app_set_settings(&s_app, &v);
    stop(); // destroy the app with observers still bound
}

static void extensions_clicked(lv_event_t *event)
{
    CHECK(lv_event_get_user_data(event) == &s_app);
    ++extension_clicks;
}

static void leave_for_extension(lv_event_t *event)
{
    extensions_clicked(event);
    ui_app_leave_for_extensions(&s_app);
}

static bool contains_visible_label(lv_obj_t *root, const char *text)
{
    if (!lv_obj_is_visible(root)) return false;
    if (lv_obj_check_type(root, &lv_label_class) && strstr(lv_label_get_text(root), text)) return true;
    for (uint32_t n = 0; n < lv_obj_get_child_count(root); n++) {
        if (contains_visible_label(lv_obj_get_child(root, (int32_t)n), text)) return true;
    }
    return false;
}

static unsigned visible_mask_pixels(const lv_draw_buf_t *shown, const lv_draw_buf_t *hidden,
                                    const lv_area_t *box)
{
    unsigned count = 0;
    for (int32_t y = box->y1; y <= box->y2; ++y) {
        const uint16_t *a = (const uint16_t *)(const void *)(shown->data + (size_t)y * shown->header.stride);
        const uint16_t *b = (const uint16_t *)(const void *)(hidden->data + (size_t)y * hidden->header.stride);
        for (int32_t x = box->x1; x <= box->x2; ++x) {
            int red = (int)(a[x] >> 11) - (int)(b[x] >> 11);
            int green = (int)((a[x] >> 5) & 63) - (int)((b[x] >> 5) & 63);
            int blue = (int)(a[x] & 31) - (int)(b[x] & 31);
            if (LV_ABS(red) >= 8 || LV_ABS(green) >= 16 || LV_ABS(blue) >= 8) ++count;
        }
    }
    return count;
}

static void check_drawer_icons(void)
{
    static const lv_image_dsc_t *const expected[] = {&kvm_icon_home, &kvm_icon_shortcuts, &kvm_icon_host,
                                                      &kvm_icon_client, &kvm_icon_settings, &kvm_icon_extensions};
    lv_obj_t *masks[6] = {0};
    lv_area_t boxes[6];
    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *button = s_app.keyboard.rail_nav[i];
        for (unsigned n = 0; n < lv_obj_get_child_count(button); ++n) {
            lv_obj_t *child = lv_obj_get_child(button, (int32_t)n);
            if (lv_obj_check_type(child, &lv_image_class) && lv_image_get_src(child) == expected[i])
                masks[i] = child;
        }
        CHECK(masks[i] && lv_obj_is_visible(masks[i]));
        if (!masks[i]) return;
        lv_obj_get_coords(masks[i], &boxes[i]);
        bool inside = boxes[i].x1 >= 0 && boxes[i].y1 >= 0 && boxes[i].x2 < SCR_W && boxes[i].y2 < SCR_H;
        CHECK(inside);
        if (!inside) return;
    }
    for (unsigned palette = 0; palette < UI_PALETTE_COUNT; ++palette) {
        ui_theme_set_control_palette((ui_control_palette_t)palette);
        lv_draw_buf_t *shown = lv_snapshot_take(s_app.screen, LV_COLOR_FORMAT_RGB565);
        for (unsigned i = 0; i < 6; ++i) lv_obj_set_hidden(masks[i], true);
        lv_draw_buf_t *hidden = lv_snapshot_take(s_app.screen, LV_COLOR_FORMAT_RGB565);
        for (unsigned i = 0; i < 6; ++i) lv_obj_set_hidden(masks[i], false);
        CHECK(shown && hidden);
        if (shown && hidden) {
            for (unsigned i = 0; i < 6; ++i) {
                unsigned pixels = visible_mask_pixels(shown, hidden, &boxes[i]);
                printf("    drawer palette %u icon %u: %u contrasting pixels\n", palette, i, pixels);
                CHECK(pixels >= 16);
            }
        }
        if (shown) lv_draw_buf_destroy(shown);
        if (hidden) lv_draw_buf_destroy(hidden);
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
}

static void check_drawer_geometry(bool closed)
{
    lv_test_wait(400);
    CHECK_EQ(s_app.keyboard.rail_collapsed, closed);
    lv_area_t pad, handle;
    lv_obj_t *tab = s_app.keyboard.rail_handle;
    CHECK(tab && lv_obj_is_visible(tab) && s_pad_on && ui_router_pad_area(&s_app.router, &pad));
    if (!tab || !s_pad_on || !ui_router_pad_area(&s_app.router, &pad)) return;
    lv_obj_get_coords(tab, &handle);
    CHECK_EQ(pad.y1, 0);
    CHECK_EQ(lv_area_get_height(&pad), 213);
    CHECK(closed ? lv_area_get_width(&pad) > 780 : lv_area_get_width(&pad) == 780);
    CHECK(pad.x1 == s_pad.x1 && pad.y1 == s_pad.y1 && pad.x2 == s_pad.x2 && pad.y2 == s_pad.y2);
    CHECK(handle.x1 >= 0 && handle.x2 < SCR_W && handle.y1 >= 0 && handle.y2 < SCR_H);
    CHECK(lv_area_get_width(&handle) >= 44 && lv_area_get_height(&handle) >= 44);
    CHECK(handle.x2 < pad.x1 || handle.x1 > pad.x2);
    if (closed) {
        CHECK_EQ(lv_area_get_width(&handle), 44);
        for (unsigned j = 0; j < 6; j++) CHECK(!lv_obj_is_visible(s_app.keyboard.rail_nav[j]));
    }
}

static void swipe_drawer_handle(int direction)
{
    lv_area_t a;
    lv_obj_get_coords(s_app.keyboard.rail_handle, &a);
    int32_t x = direction > 0 ? a.x1 + 3 : a.x2 - 3;
    const int32_t y = (a.y1 + a.y2) / 2;
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(45);
    for (int i = 1; i <= 5; ++i) {
        lv_test_mouse_move_to(LV_CLAMP(0, x + direction * 13 * i, SCR_W - 1), y);
        lv_test_wait(20);
    }
    release();
}

static void test_workspace_navigation(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    CHECK(ui_app_add_extensions_button(&s_app, leave_for_extension) != NULL);
    extension_clicks = 0;
    const unsigned modes[] = {UI_KBD_PAD_ABOVE, UI_KBD_PAD_ABOVE_BUTTONS_LEFT};
    const uint16_t overlays[] = {UI_OVERLAY_HOST_DETAILS, UI_OVERLAY_CLIENT_DETAILS, UI_OVERLAY_SETTINGS};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(ui_router_show(&s_app.router, UI_VIEW_KEYBOARD));
        lv_test_wait(30);
        click_obj(s_app.keyboard.workspace_button);
        CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_KBD_WORKSPACE);
        click_obj(s_app.keyboard.workspace_choices[modes[i]]);
        CHECK(s_pad_on && s_pad.y1 == 0 && !lv_obj_is_visible(s_app.bar));
        check_drawer_geometry(false);
        check_drawer_icons();
        const lv_area_t open_pad = s_pad;
        click_obj(s_app.keyboard.rail_handle);
        CHECK(s_app.keyboard.rail_moving && !s_pad_on);
        check_drawer_geometry(true);
        // The area reclaimed from the navigation controls must route only to the touchpad.
        int32_t reclaimed_x = i == 0 ? (open_pad.x2 + s_pad.x2) / 2 : (s_pad.x1 + open_pad.x1) / 2;
        CHECK(reclaimed_x >= s_pad.x1 && reclaimed_x <= s_pad.x2 &&
              (reclaimed_x < open_pad.x1 || reclaimed_x > open_pad.x2));
        lv_test_mouse_click_at(reclaimed_x, (s_pad.y1 + s_pad.y2) / 2);
        lv_test_wait(60);
        CHECK(s_app.keyboard.rail_collapsed && ui_overlay_top(&s_app.router) == UI_OVERLAY_NONE &&
              ui_router_active(&s_app.router) == UI_VIEW_KEYBOARD);
        click_obj(s_app.keyboard.rail_handle);
        check_drawer_geometry(false);
        int outward = i == 0 ? 1 : -1;
        swipe_drawer_handle(outward);
        check_drawer_geometry(true);
        swipe_drawer_handle(-outward);
        check_drawer_geometry(false);
        publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING);
        CHECK(contains_visible_label(s_app.keyboard.rail_nav[2], status_text(UI_TOPBAR_HOST)));
        CHECK(contains_visible_label(s_app.keyboard.rail_nav[3], status_text(UI_TOPBAR_CLIENT)));
        publish(KVM_HOST_USB_SUSPENDED, UI_CLIENT_STALE);
        CHECK(contains_visible_label(s_app.keyboard.rail_nav[2], status_text(UI_TOPBAR_HOST)));
        CHECK(contains_visible_label(s_app.keyboard.rail_nav[3], status_text(UI_TOPBAR_CLIENT)));
        publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING);
        click_obj(s_app.keyboard.workspace_button);
        click_obj(s_app.keyboard.workspace_choices[UI_KBD_FULL]);
        CHECK(!s_pad_on && lv_obj_is_visible(s_app.bar));
        lv_area_t content;
        lv_obj_get_coords(s_app.router.content_slot, &content);
        CHECK_EQ(content.y1, TOP_H);
        click_obj(s_app.keyboard.workspace_button);
        click_obj(s_app.keyboard.workspace_choices[modes[i]]);
        CHECK(s_pad_on && s_pad.y1 == 0 && !lv_obj_is_visible(s_app.bar));
        for (unsigned j = 0; j < 6; ++j) {
            lv_area_t bounds;
            lv_obj_t *button = s_app.keyboard.rail_nav[j];
            lv_obj_get_coords(button, &bounds);
            CHECK(lv_obj_is_visible(button) && lv_area_get_width(&bounds) >= 44 && lv_area_get_height(&bounds) >= 44);
            CHECK(bounds.y1 >= 0 && bounds.y2 <= s_pad.y2);
            CHECK(i == 0 ? bounds.x1 > s_pad.x2 : bounds.x2 < s_pad.x1);
        }
        for (unsigned j = 0; j < 3; ++j) {
            click_obj(s_app.keyboard.rail_nav[j + 2]);
            CHECK_EQ(ui_overlay_top(&s_app.router), overlays[j]);
            CHECK(!s_pad_on);
            ui_overlay_dismiss(&s_app.router);
            CHECK(s_pad_on && s_pad.y1 == 0);
        }
        for (unsigned j = 0; j < 2; ++j) {
            click_obj(s_app.keyboard.rail_nav[j]);
            CHECK_EQ(ui_router_active(&s_app.router), j == 0 ? UI_VIEW_MOUSE : UI_VIEW_SHORTCUTS);
            CHECK(lv_obj_is_visible(s_app.bar));
            CHECK(ui_router_show(&s_app.router, UI_VIEW_KEYBOARD));
            CHECK(s_pad_on && s_pad.y1 == 0 && !lv_obj_is_visible(s_app.bar));
        }
        ui_app_leave(&s_app);
        CHECK(lv_obj_is_visible(s_app.bar) && !s_pad_on);
        ui_app_show(&s_app);
        CHECK(s_pad_on && s_pad.y1 == 0 && !lv_obj_is_visible(s_app.bar));
        click_obj(s_app.keyboard.rail_nav[5]);
        CHECK_EQ(extension_clicks, i+1);
        CHECK(lv_obj_is_visible(s_app.bar) && !s_pad_on);
        ui_app_show(&s_app);
        CHECK_EQ(ui_router_active(&s_app.router), UI_VIEW_MOUSE);
    }
    stop();
}

static void test_extensions_hit_target(void)
{
    CHECK(start());
    CHECK(!ui_app_add_extensions_button(NULL, extensions_clicked));
    lv_obj_t *btn = ui_app_add_extensions_button(&s_app, extensions_clicked);
    CHECK(btn != NULL && ui_app_add_extensions_button(&s_app, extensions_clicked) == btn);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    CHECK(label && !strcmp(lv_label_get_text(label), "Extensions"));
    lv_obj_t *mask = lv_obj_get_child(btn, 1);
    CHECK(mask && lv_obj_check_type(mask, &lv_image_class) && !lv_obj_is_clickable(mask));
    const uint32_t ink[] = {0x1D1D1F, 0xF0F5FA, 0xFFFFFF};
    const uint32_t selected[] = {0x005497, 0xA8DAFF, 0x000000};
    for (unsigned i = 0; i < UI_PALETTE_COUNT; ++i) {
        ui_theme_set_control_palette((ui_control_palette_t)i);
        CHECK_EQ(lv_color_to_u32(lv_obj_get_style_image_recolor(mask, 0)) & 0xffffff, ink[i]);
        lv_obj_t *tab = ui_segmented_tab(s_app.nav, 0);
        lv_obj_t *active_mask = lv_obj_get_child(tab, 0);
        CHECK(lv_obj_has_state(tab, LV_STATE_CHECKED) && lv_obj_check_type(active_mask, &lv_image_class));
        CHECK_EQ(lv_color_to_u32(lv_obj_get_style_image_recolor(active_mask, 0)) & 0xffffff, selected[i]);
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
    extension_clicks = 0;
    for (unsigned enabled = 0; enabled <= 1; ++enabled) {
        ui_settings_view_t settings = {.seq=enabled+1, .keepawake_enabled=enabled != 0};
        ui_app_set_settings(&s_app, &settings);
        lv_test_wait(30);
        lv_obj_update_layout(s_app.screen);
        lv_area_t target, text, tab, gear;
        lv_obj_get_coords(btn,&target);
        lv_obj_get_coords(label,&text);
        lv_obj_get_coords(s_app.nav,&tab);
        lv_obj_get_coords(s_app.settings_btn,&gear);
        CHECK(target.x1 >= 0 && target.y1 >= 0 && target.x2 < tab.x1 && target.y2 < TOP_H);
        CHECK(target.x2-target.x1+1 >= 96 && target.y2-target.y1+1 >= 56);
        CHECK(text.x1 >= target.x1 && text.x2 <= target.x2 && text.y1 >= target.y1 && text.y2 <= target.y2);
        lv_area_t icon_box;
        lv_obj_get_coords(mask, &icon_box);
        CHECK(icon_box.y1 >= target.y1 && icon_box.y2 < text.y1 && icon_box.x2 <= target.x2);
        CHECK(s_pad_on && target.y2 < s_pad.y1 && gear.x2 < SCR_W);
        CHECK(lv_obj_is_hidden(ui_topbar_keep_awake(s_app.bar)) == (enabled == 0));
        lv_area_t last = tab;
        lv_obj_t *slots[] = {ui_topbar_keep_awake(s_app.bar), ui_topbar_status_item(s_app.bar,UI_TOPBAR_HOST),
                             ui_topbar_status_item(s_app.bar,UI_TOPBAR_CLIENT), s_app.settings_btn};
        for (unsigned i=0; i<4; ++i) {
            if (lv_obj_is_hidden(slots[i])) continue;
            lv_area_t next;
            lv_obj_get_coords(slots[i], &next);
            CHECK(next.x1 > last.x2 && next.x2 < SCR_W && next.y1 >= 0 && next.y2 < TOP_H);
            last = next;
        }
        printf("    Extensions %d..%d x %d..%d, label %d px, pad top %d, Settings right %d, Keep Awake %u\n",
               (int)target.x1,(int)target.x2,(int)target.y1,(int)target.y2,(int)(text.x2-text.x1+1),
               (int)s_pad.y1,(int)gear.x2,enabled);
        lv_test_mouse_move_to(target.x1+3,target.y1+3);
        lv_test_mouse_press(); lv_test_wait(60); release();
        click_obj(btn);
        CHECK_EQ(extension_clicks, (enabled+1)*2);
    }
    CHECK_EQ(s_rec_n,0);
    click_obj(s_app.settings_btn);
    CHECK_EQ(ui_overlay_top(&s_app.router),UI_OVERLAY_SETTINGS);
    click_obj(btn);
    CHECK_EQ(extension_clicks,4u);
    stop();
}

static void test_indicator_details(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    const int cancels = s_cancels;
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_HOST));
    CHECK_EQ(s_app.c.details_requests, 1u);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_HOST_DETAILS);
    CHECK(ui_view_status_card(&s_app.status, UI_OVERLAY_HOST_DETAILS) != NULL);
    CHECK(!s_pad_on);              // a blocking card takes the pad away
    CHECK(s_cancels > cancels);    // and asked for touch cancel + release first
    // the bar below the scrim is not reachable: a tap on the CLIENT indicator opens nothing
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT));
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_HOST_DETAILS);
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    CHECK(s_pad_on);               // the pad is back
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT));
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_CLIENT_DETAILS);
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    CHECK_EQ(s_rec_n, 0);          // nothing reached the domain
    stop();
}

static void test_scroll_preferences(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    ui_settings_view_t settings = {.seq = 1, .two_finger_scroll = false};
    ui_app_set_settings(&s_app, &settings);
    CHECK(strstr(lv_label_get_text(s_app.mouse.idle_sub), "scroll faster") != NULL);
    CHECK(!lv_obj_has_state(s_app.mouse.scroll_wheel, LV_STATE_DISABLED));
    click_obj(s_app.mouse.btn_right);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1);
    click_obj(s_app.mouse.btn_help);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_GESTURE_HELP);
    CHECK(s_app.mouse.help_scroll_result &&
          strcmp(lv_label_get_text(s_app.mouse.help_scroll_result), "KVMpad scroll gesture is off") == 0);
    CHECK(s_app.mouse.help_hold_result &&
          strcmp(lv_label_get_text(s_app.mouse.help_hold_result), "Hold to right-click is off") == 0);
    settings.two_finger_scroll = true;
    settings.longpress_right_click = true;
    ++settings.seq;
    ui_app_set_settings(&s_app, &settings);
    CHECK(s_app.mouse.help_scroll_result &&
          strcmp(lv_label_get_text(s_app.mouse.help_scroll_result), ui_view_mouse_help_result(4)) == 0);
    CHECK(s_app.mouse.help_hold_result &&
          strcmp(lv_label_get_text(s_app.mouse.help_hold_result), "Right click") == 0);
    ui_overlay_dismiss(&s_app.router);
    CHECK(ui_router_show(&s_app.router, UI_VIEW_KEYBOARD));
    lv_test_wait(30);
    click_obj(s_app.keyboard.workspace_button);
    click_obj(s_app.keyboard.workspace_choices[UI_KBD_PAD_LEFT]);
    CHECK(lv_obj_is_visible(s_app.keyboard.hybrid_scroll_hint));
    settings.two_finger_scroll = false;
    ++settings.seq;
    ui_app_set_settings(&s_app, &settings);
    CHECK(!lv_obj_is_visible(s_app.keyboard.hybrid_scroll_hint));
    stop();
}

/**
 * FR-731 / FR-751 through the shell: the gear submits UI_ACT_OPEN_SETTINGS, the local sink turns it into the
 * blocking Settings overlay (pad away, input cancelled, the bar unreachable), the adapter's record is what
 * the card renders, and one tap comes back through hooks.settings_change as one typed change.
 */
static void test_settings_overlay(void)
{
    CHECK(start());
    s_setting_n = 0;
    CHECK(s_app.settings_btn != NULL);
    const int cancels = s_cancels;
    click_obj(s_app.settings_btn);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_SETTINGS);
    CHECK(ui_view_settings_card(&s_app.settings, UI_OVERLAY_SETTINGS) != NULL);
    CHECK_EQ(ui_view_settings_section(&s_app.settings), UI_SETTINGS_SEC_APPEARANCE);
    CHECK(!s_pad_on);           // a blocking card takes the pad away
    CHECK(s_cancels > cancels); // and asked for touch cancel + release first
    CHECK_EQ(s_rec_n, 0);       // nothing reached the domain action sink

    // The bar below the scrim is unreachable: a tap on the CLIENT indicator opens nothing.
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT));
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_SETTINGS);

    ui_settings_view_t v = {0};
    v.seq = 1u;
    v.pair_require_accept = true;
    v.control_approval = UI_CONTROL_APPROVAL_ALWAYS;
    v.keepawake_interval_s = 30u;
    (void)snprintf(v.host_layout, sizeof(v.host_layout), "us");
    (void)snprintf(v.firmware_version, sizeof(v.firmware_version), "1.0.0");
    ui_app_set_settings(&s_app, &v);
    lv_test_wait(10);
    CHECK(strstr(ui_view_settings_version_text(&s_app.settings), "1.0.0") != NULL);
    CHECK(!s_app.keyboard.text_mode_ok); // FR-747: host-ime (0) keeps the KO layer off
    v.seq = 2u;
    v.unicode_mode = UI_UNICODE_MODE_LINUX;
    ui_app_set_settings(&s_app, &v);
    CHECK(s_app.keyboard.text_mode_ok);
    v.seq = 3u;
    v.unicode_mode = UI_UNICODE_MODE_HOST_IME;
    ui_app_set_settings(&s_app, &v);
    CHECK(!s_app.keyboard.text_mode_ok);

    // ADR-037: the language key offers exactly the languages the record turned on
    CHECK_EQ(ui_view_keyboard_languages(&s_app.keyboard), UI_KBD_LANG_BIT(UI_KBD_LANG_EN));
    v.seq = 4u;
    v.lang_ko = true;
    v.lang_ja = true;
    ui_app_set_settings(&s_app, &v);
    CHECK_EQ(ui_view_keyboard_languages(&s_app.keyboard),
             UI_KBD_LANG_BIT(UI_KBD_LANG_EN) | UI_KBD_LANG_BIT(UI_KBD_LANG_KO) |
                 UI_KBD_LANG_BIT(UI_KBD_LANG_JA));
    v.seq = 5u;
    v.lang_ko = false;
    v.lang_ja = false;
    ui_app_set_settings(&s_app, &v);
    CHECK_EQ(ui_view_keyboard_languages(&s_app.keyboard), UI_KBD_LANG_BIT(UI_KBD_LANG_EN));

    click_obj(s_app.settings.tab[UI_SETTINGS_SEC_REMOTE]);
    click_obj(ui_view_settings_toggle(&s_app.settings, UI_SETTING_REMOTE_ENABLED));
    CHECK_EQ(s_setting_n, 1);
    CHECK_EQ(s_setting[0].id, UI_SETTING_REMOTE_ENABLED);
    CHECK_EQ(s_setting[0].value, 1);
    CHECK_EQ(s_app.c.settings_changes, 1u);
    CHECK_EQ(s_rec_n, 0); // a setting is not a domain input action

    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_SETTINGS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    CHECK(s_pad_on); // the pad is back
    click_obj(s_app.settings_btn);
    CHECK_EQ(ui_view_settings_section(&s_app.settings), UI_SETTINGS_SEC_REMOTE);
    CHECK(lv_obj_is_visible(ui_view_settings_pair_button(&s_app.settings)));
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_SETTINGS_CLOSE, 0, fake_now(), NULL);
    stop();
}

/**
 * ADR-037: the language picker is one of the shell's overlays, so a long press on the KEYBOARD view's language
 * cap opens it through the same stack as every other card - blocking, and the pad goes with it.
 */
static void test_keyboard_language_picker(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    CHECK(ui_router_show(&s_app.router, UI_VIEW_KEYBOARD));
    lv_test_wait(10);
    ui_view_keyboard_set_text_mode(&s_app.keyboard, true);
    ui_view_keyboard_set_languages(&s_app.keyboard, true, false, false);
    lv_obj_t *key = ui_view_keyboard_lang_key(&s_app.keyboard);
    CHECK(key != NULL);

    press_obj(key);
    lv_test_wait(600); // past LVGL's long-press time
    release();
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_KBD_LANG);
    CHECK(ui_view_keyboard_picker_open(&s_app.keyboard));
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_EN); // a long press is not also a tap
    CHECK_EQ(s_rec_n, 0);

    click_obj(ui_view_keyboard_lang_pick_button(&s_app.keyboard, UI_KBD_LANG_KO));
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE); // the shell's DISMISS_OVERLAY closed it
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_KO);
    CHECK_EQ(count(UI_ACT_KEY_DOWN, 0u), 0); // the cap sends no HID, ever
    stop();
}

static void test_mode_switcher(void)
{
    CHECK(start());
    CHECK_EQ((int)s_app.nav_desc.count, (int)s_app.n_views);
    CHECK_EQ((int)s_app.nav_desc.count, 3); // MOUSE, KEYBOARD, SHORTCUTS
    CHECK_EQ(ui_segmented_get_active(s_app.nav), 0);
    for (uint16_t i = 0; i < s_app.nav_desc.count; i++) {
        const int cancels = s_cancels;
        click_obj(ui_segmented_tab(s_app.nav, i));
        CHECK_EQ(ui_router_active(&s_app.router), (int)s_app.segment_view[i]);
        CHECK_EQ(ui_segmented_get_active(s_app.nav), (int32_t)i);
        if (i > 0) {
            CHECK(s_cancels > cancels); // every real view change asked for a release first
        }
    }
    CHECK_EQ(s_app.c.nav_requests, (uint32_t)s_app.nav_desc.count);
    stop();
}

static void test_leave_releases_held_button(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    press_obj(s_app.mouse.btn_right);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_DOWN, KVM_MOUSE_BTN_RIGHT), 1);
    CHECK(s_pad_on);
    ui_app_leave(&s_app);
    CHECK(!s_pad_on);
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1);
    release();
    CHECK_EQ(count(UI_ACT_MOUSE_BUTTON_UP, KVM_MOUSE_BTN_RIGHT), 1); // no second UP for the same press
    ui_app_show(&s_app);
    CHECK(s_pad_on && s_pad.y1 >= TOP_H);
    click_obj(s_app.settings_btn);
    CHECK(!s_pad_on && ui_overlay_top(&s_app.router) == UI_OVERLAY_SETTINGS);
    ui_app_leave(&s_app);
    CHECK(!s_pad_on); // dismissing Settings while leaving must not recapture the new screen
    ui_app_show(&s_app);
    CHECK(s_pad_on);
    stop();
}

// ---- pairing and control approval (ADR-022 / ADR-023) -----------------------------------------------------------

static ui_pair_view_t open_window(const char *code)
{
    ui_pair_view_t p = {0};
    p.window_open = true;
    (void)snprintf(p.code, sizeof(p.code), "%s", code);
    p.code_remaining_ms = 120000u;
    p.window_remaining_ms = 180000u;
    return p;
}

static void test_pair_entry_point(void)
{
    CHECK(start());
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING, true);
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT));
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_CLIENT_DETAILS);
    lv_obj_t *pair = ui_view_status_pair_button(&s_app.status);
    CHECK(pair != NULL);
    CHECK(ui_view_status_pair_available(&s_app.status));
    CHECK(lv_obj_is_clickable(pair));
    lv_area_t a;
    lv_obj_update_layout(pair);
    lv_obj_get_coords(pair, &a);
    CHECK(a.x2 - a.x1 + 1 >= UI_TOUCH_MIN && a.y2 - a.y1 + 1 >= UI_TOUCH_MIN);

    // One tap: the card closes and the request is reported. The card itself opens no window.
    click_obj(pair);
    lv_test_wait(30);
    CHECK_EQ(s_remote_n, 1);
    CHECK_EQ(s_remote[0].act, UI_REMOTE_ACT_PAIR_OPEN);
    CHECK_EQ(s_app.c.remote_actions, 1u);
    CHECK_EQ(s_app.status.pair_taps, 1u);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    CHECK(s_pad_on);      // nothing is blocking yet: the domain has not opened a window
    CHECK_EQ(s_rec_n, 0); // and nothing reached the domain action sink

    // SEC-106: remote off -> no pairing, and the card says why instead of only dimming the button.
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED, true);
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT));
    pair = ui_view_status_pair_button(&s_app.status);
    CHECK(pair != NULL);
    CHECK(!ui_view_status_pair_available(&s_app.status));
    CHECK(!lv_obj_is_clickable(pair));
    CHECK(lv_obj_has_state(pair, LV_STATE_DISABLED));
    click_obj(pair);
    CHECK_EQ(s_remote_n, 1); // no second request
    bool said = false;
    lv_obj_t *card = ui_view_status_card(&s_app.status, UI_OVERLAY_CLIENT_DETAILS);
    for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
        lv_obj_t *child = lv_obj_get_child(card, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            strstr(lv_label_get_text(child), "needs remote switched on") != NULL) {
            said = true;
        }
    }
    CHECK(said);
    // Wi-Fi down with remote on is refused as well (TECH-040).
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING, false);
    CHECK(!ui_view_status_pair_available(&s_app.status));
    CHECK(!lv_obj_is_clickable(ui_view_status_pair_button(&s_app.status)));
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    stop();
}

static void test_pairing_prompt_owns_the_screen(void)
{
    CHECK(start());
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING, true);
    const int cancels = s_cancels;
    ui_pair_view_t p = open_window("482913");
    ui_app_set_pairing(&s_app, &p);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_PAIRING);
    CHECK(!s_pad_on);           // the pad is gone while a decision is on the screen
    CHECK(s_cancels > cancels); // after a touch cancel + HID release request
    CHECK(strcmp(ui_remote_view_code_text(&s_app.remote), "482 913") == 0);

    // The top bar below the scrim is unreachable: a tap on an indicator opens no details card.
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_HOST));
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_PAIRING);
    for (uint16_t i = 0; i < s_app.nav_desc.count; i++) {
        click_obj(ui_segmented_tab(s_app.nav, i));
    }
    CHECK_EQ(ui_router_active(&s_app.router), UI_VIEW_MOUSE); // and so is the mode switcher
    CHECK_EQ(s_rec_n, 0);

    // The client answered: the same card asks the Accept / Reject question (FR-725).
    p.accept_pending = true;
    (void)snprintf(p.client_name, sizeof(p.client_name), "DESKTOP-7F3A");
    p.accept_remaining_ms = 30000u;
    ui_app_set_pairing(&s_app, &p);
    lv_test_wait(10);
    CHECK(ui_remote_view_accept_mode(&s_app.remote));
    click_obj(s_app.remote.btn_accept);
    CHECK_EQ(s_remote_n, 1);
    CHECK_EQ(s_remote[0].act, UI_REMOTE_ACT_PAIR_ACCEPT);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_PAIRING); // closed by the domain, not by the tap

    // The domain reports the outcome: the card goes, the pad comes back, one toast appears.
    const ui_pair_view_t done = {.result = UI_PAIR_RESULT_PAIRED, .result_seq = 1u,
                                 .result_name = "DESKTOP-7F3A"};
    ui_app_set_pairing(&s_app, &done);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    CHECK(s_pad_on);
    CHECK_EQ(s_app.remote.c.toasts, 1u);
    CHECK(strcmp(ui_remote_view_toast_text(&s_app.remote), "Paired: DESKTOP-7F3A") == 0);
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_HOST)); // the bar is live again
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_HOST_DETAILS);
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    stop();
}

static void test_approval_prompt_through_the_shell(void)
{
    CHECK(start());
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_CONNECTED, true);
    ui_approval_view_t a = {.pending = true, .session_id = 0xABCDEF01u, .remaining_ms = 30000u};
    (void)snprintf(a.client_name, sizeof(a.client_name), "STUDIO-PC");
    ui_app_set_approval(&s_app, &a);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_APPROVAL);
    CHECK(!s_pad_on);
    CHECK(strstr(ui_remote_view_heading_text(&s_app.remote, UI_OVERLAY_APPROVAL), "requests control") != NULL);
    CHECK(strcmp(ui_remote_view_timer_text(&s_app.remote, UI_OVERLAY_APPROVAL),
                 "Denied automatically in 30 s") == 0);
    click_obj(s_app.remote.btn_allow);
    CHECK_EQ(s_remote_n, 1);
    CHECK_EQ(s_remote[0].act, UI_REMOTE_ACT_APPROVE);
    CHECK_EQ(s_remote[0].session, 0xABCDEF01u);
    CHECK_EQ(s_rec_n, 0); // the UI emitted no HID intent of its own

    const ui_approval_view_t gone = {0};
    ui_app_set_approval(&s_app, &gone);
    lv_test_wait(20);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    CHECK(s_pad_on);
    stop();
}

static void test_prompt_drops_a_pending_confirmation(void)
{
    s_confirm_alt_f4 = true;
    CHECK(start());
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING, true);
    click_obj(ui_segmented_tab(s_app.nav, 2)); // SHORTCUTS
    CHECK_EQ(ui_router_active(&s_app.router), UI_VIEW_SHORTCUTS);
    click_obj(s_app.shortcuts.card[UI_SHORTCUT_CLOSE_WINDOW]);
    CHECK(s_app.shortcuts.modal != NULL); // FR-734 confirmation, on its own scrim outside the stack
    const uint32_t closed = s_app.shortcuts.c.confirms_closed;

    const ui_pair_view_t p = open_window("135790");
    ui_app_set_pairing(&s_app, &p);
    lv_test_wait(30);
    // The confirmation is abandoned (nothing is sent) so the pairing card is the only decision on screen.
    CHECK(s_app.shortcuts.modal == NULL);
    CHECK_EQ(s_app.shortcuts.c.confirms_closed, closed + 1u);
    CHECK_EQ(s_app.shortcuts.c.confirms_taken, 0u);
    CHECK_EQ(count(UI_ACT_SHORTCUT_REQUEST, KVM_CHORD_CLOSE_WINDOW), 0);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_PAIRING);

    const ui_pair_view_t off = {0};
    ui_app_set_pairing(&s_app, &off);
    lv_test_wait(20);
    stop();
    s_confirm_alt_f4 = false;
}

static void test_prompts_refuse_bad_arguments(void)
{
    const ui_pair_view_t p = {0};
    const ui_approval_view_t a = {0};
    ui_app_set_pairing(NULL, &p);
    ui_app_set_approval(NULL, &a);
    CHECK(start());
    ui_app_set_pairing(&s_app, NULL);
    ui_app_set_approval(&s_app, NULL);
    CHECK_EQ(ui_overlay_top(&s_app.router), UI_OVERLAY_NONE);
    stop();
    ui_app_set_pairing(&s_app, &p); // not ready any more: a no-op
    ui_app_set_approval(&s_app, &a);
}

static void test_cycles(void)
{
    CHECK(start());
    stop();
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 300; i++) {
        CHECK(start());
        if (i % 2 == 0) {
            publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING);
            click_obj(s_app.mouse.btn_left);
        }
        stop();
        CHECK(s_act.local_sink == NULL); // no dangling sink into a deleted shell
    }
    printf("    300 shell init/deinit cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);
}

static size_t zh_none(void *ctx, const char *py, size_t len)
{
    (void)ctx;
    (void)py;
    (void)len;
    return 0u;
}

static size_t zh_cand(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    (void)ctx;
    (void)i;
    if (cap > 0u) {
        out[0] = 0u;
    }
    return 0u;
}

static size_t zh_n(void *ctx, size_t i)
{
    (void)ctx;
    (void)i;
    return 0u;
}

static size_t zh_z(void *ctx)
{
    (void)ctx;
    return 0u;
}

static bool zh_f(void *ctx)
{
    (void)ctx;
    return false;
}

static void zh_r(void *ctx)
{
    (void)ctx;
}

static void test_zh_engine_pass_through(void)
{
    static const ui_kbd_conv_engine_t eng = {
        .search = zh_none, .candidate = zh_cand, .choose = zh_n, .cancel_last = zh_z, .fixed_len = zh_z,
        .consumed = zh_z, .parsed = zh_z, .complete = zh_f, .reset = zh_r};
    ui_app_set_zh_engine(NULL, &eng); // a no-op
    CHECK(start());
    CHECK(s_app.keyboard.zh == NULL);
    ui_app_set_zh_engine(&s_app, &eng);
    CHECK(s_app.keyboard.zh == &eng);
    ui_app_set_zh_engine(&s_app, NULL);
    CHECK(s_app.keyboard.zh == NULL);
    ui_app_set_ja_engine(NULL, &eng); // a no-op
    ui_app_set_ja_engine(&s_app, &eng);
    CHECK(s_app.keyboard.ja == &eng && s_app.keyboard.zh == NULL);
    ui_app_set_ja_engine(&s_app, NULL);
    CHECK(s_app.keyboard.ja == NULL);
    stop();
}

static void enter_extension(lv_event_t *e)
{
    (void)e;
    ui_app_leave_for_extensions(&s_app);
    lv_screen_load(s_home);
}

static lv_obj_t *keyboard_key(const char *name)
{
    for (uint32_t i = 0; i < ui_view_keyboard_key_count(); ++i) {
        if (!strcmp(ui_view_keyboard_key_name(i), name)) return ui_view_keyboard_key_obj(&s_app.keyboard, i);
    }
    return NULL;
}

static void test_extensions_release_view_cache(void)
{
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    lv_obj_t *extensions = ui_app_add_extensions_button(&s_app, enter_extension);
    lv_obj_t *home = s_app.mouse.root;
    ui_view_keyboard_set_host_layout(&s_app.keyboard, "de");
    click_obj(ui_segmented_tab(s_app.nav, 1));
    click_obj(ui_segmented_tab(s_app.nav, 0));
    CHECK(s_app.keyboard.root && !s_app.shortcuts.root);
    const size_t keyboard_cached = host_lv_live_bytes();
    click_obj(extensions);
    lv_screen_load(s_app.screen);
    ui_app_show(&s_app);
    lv_test_wait(20);
    const size_t keyboard_freed = keyboard_cached - host_lv_live_bytes();
    CHECK(keyboard_cached > host_lv_live_bytes() + 48u * 1024u);
    printf("    Keyboard only: %zu LVGL bytes recovered on Home\n", keyboard_freed);
    size_t released = 0, baseline = 0;
    for (unsigned pass = 0; pass < 32; ++pass) {
        click_obj(ui_segmented_tab(s_app.nav, 1));
        lv_obj_t *keys = s_app.keyboard.root;
        CHECK(keys != NULL);
        if (!pass) {
            click_obj(keyboard_key("F1-F12"));
            CHECK(s_app.keyboard.fn_layer);
        }
        click_obj(ui_segmented_tab(s_app.nav, 2));
        CHECK(s_app.shortcuts.root && s_app.keyboard.root == keys);
        click_obj(ui_segmented_tab(s_app.nav, 1));
        CHECK(s_app.keyboard.root == keys && s_app.keyboard.fn_layer);
        const size_t loaded = host_lv_live_bytes();
        if (!pass) {
            press_obj(keyboard_key("A"));
            CHECK_EQ(count(UI_ACT_KEY_DOWN, KVM_KEY_A), 1);
            lv_obj_send_event(extensions, LV_EVENT_CLICKED, NULL);
            release();
            CHECK_EQ(count(UI_ACT_KEY_UP, KVM_KEY_A), 1);
        } else {
            click_obj(extensions);
        }
        CHECK(lv_screen_active() == s_home && !s_pad_on);
        CHECK(!s_app.keyboard.root && !s_app.shortcuts.root && s_app.mouse.root == home);
        publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING); // exercise the store while both trees are absent
        lv_screen_load(s_app.screen);
        ui_app_show(&s_app);
        lv_test_wait(20);
        CHECK(ui_router_active(&s_app.router) == UI_VIEW_MOUSE && s_pad_on);
        CHECK(!strcmp(ui_view_keyboard_host_layout(&s_app.keyboard), "de"));
        const size_t remaining = host_lv_live_bytes();
        CHECK(loaded > remaining + 48u * 1024u);
        if (pass == 1) { released = loaded - remaining; baseline = remaining; }
        if (pass > 1) CHECK_EQ(remaining, baseline);
    }
    printf("    32 Keyboard/Shortcuts -> Extensions -> Mouse cycles: %zu bytes returned to LVGL, %zu bytes live on Home\n",
           released, baseline);
    stop();
}

static void test_extension_restores_language_after_decoder(void)
{
    static const ui_kbd_conv_engine_t eng = {
        .search = zh_none, .candidate = zh_cand, .choose = zh_n, .cancel_last = zh_z, .fixed_len = zh_z,
        .consumed = zh_z, .parsed = zh_z, .complete = zh_f, .reset = zh_r};
    CHECK(start());
    publish(KVM_HOST_USB_MOUNTED, UI_CLIENT_DISABLED);
    lv_obj_t *extensions = ui_app_add_extensions_button(&s_app, enter_extension);
    ui_view_keyboard_set_text_mode(&s_app.keyboard, true);
    ui_view_keyboard_set_languages(&s_app.keyboard, false, false, true);
    ui_app_set_ja_engine(&s_app, &eng);
    click_obj(ui_segmented_tab(s_app.nav, 1));
    click_obj(ui_view_keyboard_lang_key(&s_app.keyboard));
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_JA);
    click_obj(s_app.keyboard.flick_toggle);
    CHECK(s_app.keyboard.flick);
    click_obj(extensions);
    ui_app_set_ja_engine(&s_app, NULL);
    lv_screen_load(s_app.screen);
    ui_app_show(&s_app);
    click_obj(ui_segmented_tab(s_app.nav, 1));
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_EN);
    const int actions = s_rec_n;
    ui_app_set_ja_engine(&s_app, &eng);
    CHECK_EQ(s_rec_n, actions);
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_JA);
    CHECK(s_app.keyboard.flick && !lv_obj_is_hidden(s_app.keyboard.flick_pad));
    CHECK_EQ(s_app.keyboard.py_len, 0);
    click_obj(extensions);
    ui_app_set_ja_engine(&s_app, NULL);
    lv_screen_load(s_app.screen);
    ui_app_show(&s_app);
    click_obj(ui_segmented_tab(s_app.nav, 1));
    click_obj(keyboard_key("A"));
    const int after_key = s_rec_n;
    ui_app_set_ja_engine(&s_app, &eng);
    CHECK_EQ(s_rec_n, after_key);
    CHECK_EQ(ui_view_keyboard_lang(&s_app.keyboard), UI_KBD_LANG_EN);
    CHECK(s_app.keyboard.held == UI_VIEW_KBD_NO_KEY);
    stop();
}

static void test_refuses_bad_arguments(void)
{
    ui_app_t app;
    CHECK(!ui_app_init(&app, NULL, &s_act, &k_hooks));
    CHECK(!ui_app_init(NULL, &s_store, &s_act, &k_hooks));
    ui_app_deinit(&app); // not ready: a no-op
    ui_app_show(NULL);
    ui_app_leave(NULL);
}

#define RUN_MEM(t)                                                                                      \
    do {                                                                                                \
        size_t before_ = host_lv_live_count();                                                          \
        RUN(t);                                                                                         \
        CHECK_EQ(host_lv_live_count(), before_);                                                        \
    } while (0)

int main(void)
{
    printf("test_ui_app (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    CHECK(start()); // warm-up: first text render, first press, first card of every kind
    click_obj(s_app.mouse.btn_left);
    publish_link(KVM_HOST_USB_MOUNTED, UI_CLIENT_LISTENING, true);
    click_obj(ui_topbar_status_item(s_app.bar, UI_TOPBAR_CLIENT)); // the card with the pairing entry point
    ui_actions_submit(&s_act, UI_ACT_DISMISS_OVERLAY, UI_CTRL_STATUS_CLOSE, 0, fake_now(), NULL);
    lv_test_wait(20);
    ui_pair_view_t warm = open_window("012345");
    ui_app_set_pairing(&s_app, &warm);
    warm.accept_pending = true;
    (void)snprintf(warm.client_name, sizeof(warm.client_name), "WARM-UP");
    warm.accept_remaining_ms = 30000u;
    ui_app_set_pairing(&s_app, &warm);
    const ui_pair_view_t warm_done = {.result = UI_PAIR_RESULT_PAIRED, .result_seq = 1u,
                                      .result_name = "WARM-UP"};
    ui_app_set_pairing(&s_app, &warm_done);
    const ui_approval_view_t warm_appr = {.pending = true, .client_name = "WARM-UP", .remaining_ms = 30000u};
    ui_app_set_approval(&s_app, &warm_appr);
    lv_test_wait(20);
    click_obj(s_app.remote.btn_deny);
    const ui_approval_view_t warm_gone = {0};
    ui_app_set_approval(&s_app, &warm_gone);
    lv_test_wait(3000); // outlive the outcome toast: nothing transient may cross the baseline
    stop();

    RUN_MEM(test_build_and_indicators);
    RUN_MEM(test_pad_rectangle);
    RUN_MEM(test_extensions_hit_target);
    RUN_MEM(test_workspace_navigation);
    RUN_MEM(test_indicator_details);
    RUN_MEM(test_settings_overlay);
    RUN_MEM(test_diagnostics_overlay);
    RUN_MEM(test_scroll_preferences);
    RUN_MEM(test_keyboard_language_picker);
    RUN_MEM(test_mode_switcher);
    RUN_MEM(test_leave_releases_held_button);
    RUN_MEM(test_extensions_release_view_cache);
    RUN_MEM(test_extension_restores_language_after_decoder);
    RUN_MEM(test_pair_entry_point);
    RUN_MEM(test_pairing_prompt_owns_the_screen);
    RUN_MEM(test_approval_prompt_through_the_shell);
    RUN_MEM(test_prompt_drops_a_pending_confirmation);
    RUN_MEM(test_prompts_refuse_bad_arguments);
    RUN_MEM(test_cycles);
    RUN_MEM(test_zh_engine_pass_through);
    RUN_MEM(test_refuses_bad_arguments);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_app");
}
