// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-070C status details with the real LVGL 9.6 (headless 1024x600, LVGL test pointer, ASan + UBSan), the
// real router, overlay stack, store, mailbox, theme and action gateway:
// - FR-003: each overlay opens through ui_overlay_show and renders the wording of 01 §6.1 / §6.2;
// - 07 §12 item 2: before a producer has published, every row reads "Unknown" - nothing is guessed;
// - the values are live projections: a publish while the card is open updates it;
// - FR-401/403: a Wi-Fi link is a separate row and never upgrades the CLIENT status (Wi-Fi connected +
//   CLIENT DISABLED still reads "Off"), and the card says so in words;
// - the Close button submits UI_ACT_DISMISS_OVERLAY (and falls back to the router when no local sink took it);
// - AT-UIFW-10: the blocking scrim absorbs a click aimed at a button in the view below;
// - no SSID, IP, token or pairing secret is rendered;
// - lifecycle: 1000 open/close cycles and a store deleted before the router, without live-allocation growth.
#include <string.h>

#include "hid_core.h"
#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_actions.h"
#include "ui_overlay.h"
#include "ui_router.h"
#include "ui_store.h"
#include "ui_theme.h"
#include "ui_tokens.h"
#include "ui_view_status.h"

#define SCR_W 1024
#define SCR_H 600
#define TOP_H UI_TOPBAR_H

// ---- fixture -------------------------------------------------------------------------------
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static bool s_store_on;
static ui_actions_t s_act;
static ui_view_status_t s_st;
static ui_router_t s_r;
static lv_obj_t *s_scr;
static lv_obj_t *s_home;
static lv_obj_t *s_below; // a clickable button in the view under the overlay
static int s_below_clicks;
static uint32_t s_seq;
static uint64_t s_clock;
static bool s_local_sink_on;
static int s_dismiss_actions;

static uint64_t fake_now(void)
{
    return s_clock += 1000;
}

static bool local_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    if (a->type != UI_ACT_DISMISS_OVERLAY) {
        return false;
    }
    s_dismiss_actions++;
    CHECK_EQ(a->control_id, UI_CTRL_STATUS_CLOSE);
    ui_overlay_dismiss(&s_r); // the shell owns navigation (07 §3); safe from a click inside the overlay
    return true;
}

static void below_clicked(lv_event_t *e)
{
    (void)e;
    s_below_clicks++;
}

static lv_obj_t *view_create(void *ctx, lv_obj_t *slot)
{
    (void)ctx;
    lv_obj_t *root = lv_obj_create(slot);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, lv_pct(100), lv_pct(100));
    s_below = lv_button_create(root);
    lv_obj_remove_style_all(s_below);
    ui_theme_apply_button(s_below, UI_BTN_PRIMARY);
    lv_obj_set_size(s_below, 240, UI_TOUCH_COMFORT);
    lv_obj_align(s_below, LV_ALIGN_BOTTOM_LEFT, UI_CONTENT_INSET, -UI_CONTENT_INSET);
    lv_obj_add_event_cb(s_below, below_clicked, LV_EVENT_CLICKED, NULL);
    return root;
}

static ui_view_descriptor_t s_desc;

static void publish_hid(uint8_t host, uint8_t owner, uint32_t epoch, bool caps_known, bool caps_on,
                        bool maintenance, uint16_t err)
{
    ui_hid_slice_t h = {.host_usb_state = host,
                        .owner_kind = owner,
                        .owner_epoch = epoch,
                        .caps_led_known = caps_known,
                        .caps_led_active = caps_on,
                        .maintenance = maintenance,
                        .error_code = err};
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static void publish_net(uint8_t client, bool wifi)
{
    ui_net_slice_t n = {.client_state = client, .wifi_connected = wifi};
    ui_mailbox_publish_net(&s_mb, &n, ++s_seq);
    ui_store_tick(&s_store);
    lv_test_wait(5);
}

static bool start(void)
{
    s_below_clicks = 0;
    s_dismiss_actions = 0;
    s_below = NULL;
    ui_mailbox_init(&s_mb, NULL);
    CHECK(ui_store_init(&s_store, &s_mb));
    s_store_on = true;
    ui_actions_init(&s_act);
    if (s_local_sink_on) {
        ui_actions_set_local_sink(&s_act, local_sink, NULL);
    }
    ui_view_status_init(&s_st, &s_store, &s_act, &s_r, fake_now);
    uint8_t n_ov = 0;
    const ui_overlay_descriptor_t *ov = ui_view_status_overlays(&s_st, &n_ov);
    CHECK_EQ(n_ov, UI_VIEW_STATUS_PANELS);
    s_desc = (ui_view_descriptor_t){.id = UI_VIEW_MOUSE, .create = view_create};
    s_scr = lv_obj_create(NULL);
    ui_theme_apply(s_scr, UI_STYLE_SCREEN, LV_STATE_DEFAULT);
    lv_screen_load(s_scr);
    const ui_shell_layout_t lay = {.width = SCR_W, .height = SCR_H, .topbar_h = TOP_H};
    bool ok = ui_router_init(&s_r, s_scr, &lay, &s_store, &s_desc, 1, ov, n_ov, NULL);
    ok = ok && ui_router_show(&s_r, UI_VIEW_MOUSE);
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

static void click_obj(lv_obj_t *o)
{
    lv_area_t a;
    lv_obj_update_layout(o);
    lv_obj_get_coords(o, &a);
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
}

static const char *row(uint16_t id, int index)
{
    const char *t = ui_view_status_row_text(&s_st, id, (uint8_t)index);
    return t != NULL ? t : "<missing>";
}

static lv_obj_t *close_button(uint16_t id)
{
    lv_obj_t *card = ui_view_status_card(&s_st, id);
    CHECK(card != NULL);
    lv_obj_t *foot = lv_obj_get_child(card, (int32_t)lv_obj_get_child_count(card) - 1);
    CHECK(foot != NULL);
    return lv_obj_get_child(foot, 0);
}

// Row indexes of the HOST card (ui_view_status.c k_host_rows) and the CLIENT card.
#define H_USB 0
#define H_OWNER 1
#define H_EPOCH 2
#define H_CAPS 3
#define H_MAINT 4
#define H_ERROR 5
#define C_SESSION 0
#define C_WIFI 1

// ---- tests ---------------------------------------------------------------------------------
static void test_unknown_until_published(void)
{
    CHECK(start());
    // Nothing published yet: both cards must read Unknown everywhere, not a cheerful default.
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_HOST_DETAILS);
    CHECK(ui_overlay_blocking(&s_r));
    for (int i = 0; i < ui_view_status_row_count(UI_OVERLAY_HOST_DETAILS); i++) {
        CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, i), "Unknown") == 0);
        CHECK(ui_view_status_row_caption(UI_OVERLAY_HOST_DETAILS, (uint8_t)i) != NULL);
    }
    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    CHECK(ui_view_status_card(&s_st, UI_OVERLAY_HOST_DETAILS) == NULL);

    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_CLIENT_DETAILS));
    for (int i = 0; i < ui_view_status_row_count(UI_OVERLAY_CLIENT_DETAILS); i++) {
        CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, i), "Unknown") == 0);
    }
    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    stop();
}

static void test_host_details(void)
{
    CHECK(start());
    publish_hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 7, true, true, false, 0);
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_USB), "Active") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_OWNER), "Local") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_EPOCH), "7") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_CAPS), "On") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_MAINT), "No") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_ERROR), "None") == 0);

    // Live while open: the card follows the store, it does not snapshot at open time.
    publish_hid(KVM_HOST_USB_SUSPENDED, OWNER_REMOTE, 8, true, false, true, 0x0042);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_USB), "Suspended") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_OWNER), "Remote") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_EPOCH), "8") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_CAPS), "Off") == 0);
    CHECK(strstr(row(UI_OVERLAY_HOST_DETAILS, H_MAINT), "input blocked") != NULL); // FR-006
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_ERROR), "0x0042") == 0);

    publish_hid(KVM_HOST_USB_DETACHED, OWNER_NONE, 8, false, false, false, 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_USB), "Disconnected") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_OWNER), "None") == 0);
    // FR-206: no LED output report from the HOST -> Unknown, never "Off" by assumption.
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_CAPS), "Unknown") == 0);

    publish_hid(KVM_HOST_USB_ERROR, OWNER_NONE, 9, false, false, false, 0x1234);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_USB), "Error") == 0);
    CHECK(strcmp(row(UI_OVERLAY_HOST_DETAILS, H_ERROR), "0x1234") == 0);

    // Opening the same overlay twice is refused; the card is not rebuilt underneath itself.
    const uint32_t opens = s_st.opens;
    CHECK(!ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    CHECK_EQ(s_st.opens, opens);
    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    stop();
}

static void test_client_details_wifi_is_not_a_session(void)
{
    CHECK(start());
    // FR-401/403: a Wi-Fi association is not a client connection. In this build the CLIENT is DISABLED.
    publish_net(UI_CLIENT_DISABLED, true);
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_CLIENT_DETAILS));
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_SESSION), "Off") == 0);
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_WIFI), "Connected") == 0);
    CHECK(strcmp(ui_view_status_row_caption(UI_OVERLAY_CLIENT_DETAILS, C_WIFI), "Wi-Fi link") == 0);

    // The card states the rule in words, not only by keeping the rows apart (FR-005).
    lv_obj_t *card = ui_view_status_card(&s_st, UI_OVERLAY_CLIENT_DETAILS);
    bool note = false;
    for (uint32_t i = 0; i < lv_obj_get_child_count(card); i++) {
        lv_obj_t *child = lv_obj_get_child(card, (int32_t)i);
        if (lv_obj_check_type(child, &lv_label_class) &&
            strstr(lv_label_get_text(child), "not a client connection") != NULL) {
            note = true;
        }
    }
    CHECK(note);

    publish_net(UI_CLIENT_NETWORK_DOWN, false);
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_SESSION), "Offline") == 0);
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_WIFI), "Not connected") == 0);
    publish_net(UI_CLIENT_CONNECTED, true);
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_SESSION), "Connected") == 0);
    publish_net(UI_CLIENT_CONTROL_GRANTED, true);
    CHECK(strcmp(row(UI_OVERLAY_CLIENT_DETAILS, C_SESSION), "Controlling") == 0);

    // Nothing in the card is an SSID, an address or a secret: the only numbers come from the HOST card.
    CHECK(ui_view_status_row_count(UI_OVERLAY_CLIENT_DETAILS) == 2);
    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    stop();
}

static void test_close_button(void)
{
    s_local_sink_on = true;
    CHECK(start());
    publish_hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 1, true, false, false, 0);
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    click_obj(close_button(UI_OVERLAY_HOST_DETAILS));
    lv_test_wait(30);
    CHECK_EQ(s_dismiss_actions, 1);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK(ui_view_status_card(&s_st, UI_OVERLAY_HOST_DETAILS) == NULL);
    CHECK_EQ(s_st.closes, 1);
    CHECK_EQ(s_st.close_taps, 1);
    stop();

    // No local sink installed: the router fallback still closes the card.
    s_local_sink_on = false;
    CHECK(start());
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_CLIENT_DETAILS));
    click_obj(close_button(UI_OVERLAY_CLIENT_DETAILS));
    lv_test_wait(30);
    CHECK_EQ(s_dismiss_actions, 0);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_NONE);
    CHECK_EQ(s_st.close_taps, 1);
    stop();
}

static void test_scrim_blocks_the_view_below(void)
{
    CHECK(start());
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 1); // the button is reachable while no overlay is open

    lv_area_t a;
    lv_obj_update_layout(s_below);
    lv_obj_get_coords(s_below, &a);
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    // AT-UIFW-10: the same coordinates now hit the scrim, not the button.
    lv_test_mouse_move_to((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_mouse_press();
    lv_test_wait(50);
    lv_test_mouse_release();
    lv_test_wait(50);
    CHECK_EQ(s_below_clicks, 1);
    CHECK_EQ(ui_overlay_top(&s_r), UI_OVERLAY_HOST_DETAILS); // and it does not dismiss itself either

    ui_overlay_dismiss(&s_r);
    lv_test_wait(30);
    click_obj(s_below);
    CHECK_EQ(s_below_clicks, 2); // the view is tappable again (07 §7)
    stop();
}

static void test_lifecycle(void)
{
    // store deleted first, while the open card still observes its subjects
    CHECK(start());
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    ui_store_deinit(&s_store);
    s_store_on = false;
    lv_test_wait(10);
    stop();

    CHECK(start());
    publish_hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, 3, true, true, false, 0);
    publish_net(UI_CLIENT_DISABLED, true);
    // Warm-up: the first render of each card fills LVGL's one-time caches (glyph draw data for the digits of
    // the epoch row, among others). The leak check starts from the warm state.
    for (int i = 0; i < 4; i++) {
        CHECK(ui_overlay_show(&s_r, (i % 2 == 0) ? UI_OVERLAY_HOST_DETAILS : UI_OVERLAY_CLIENT_DETAILS));
        publish_hid(KVM_HOST_USB_MOUNTED, OWNER_LOCAL, (uint32_t)(1234567 + i), true, true, false, 0);
        ui_overlay_dismiss(&s_r);
        lv_test_wait(10);
    }
    const size_t base = host_lv_live_count();
    for (int i = 0; i < 1000; i++) {
        const uint16_t id = (i % 2 == 0) ? UI_OVERLAY_HOST_DETAILS : UI_OVERLAY_CLIENT_DETAILS;
        CHECK(ui_overlay_show(&s_r, id));
        if (i % 7 == 0) {
            publish_hid(KVM_HOST_USB_SUSPENDED, OWNER_REMOTE, (uint32_t)i, true, false, false, 0);
        }
        ui_overlay_dismiss(&s_r);
        lv_test_wait(10);
    }
    printf("    1000 overlay open/close cycles: live allocations %zu -> %zu\n", base, host_lv_live_count());
    CHECK_EQ(host_lv_live_count(), base);
    CHECK_EQ(s_st.opens, s_st.closes);
    stop();
}

#define RUN_MEM(t)                                       \
    do {                                                 \
        size_t before_ = host_lv_live_count();           \
        RUN(t);                                          \
        CHECK_EQ(host_lv_live_count(), before_);         \
    } while (0)

int main(void)
{
    printf("test_ui_view_status (LVGL %d.%d.%d)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    lv_init();
    lv_display_t *disp = lv_test_display_create(SCR_W, SCR_H);
    CHECK(disp != NULL);
    lv_test_indev_create_all();
    CHECK(ui_theme_init());
    s_home = lv_obj_create(NULL);
    lv_screen_load(s_home);
    s_local_sink_on = true;
    CHECK(start()); // warm-up: first text render, first overlay, first press
    CHECK(ui_overlay_show(&s_r, UI_OVERLAY_HOST_DETAILS));
    ui_overlay_dismiss(&s_r);
    lv_test_wait(20);
    click_obj(s_below);
    stop();

    RUN_MEM(test_unknown_until_published);
    RUN_MEM(test_host_details);
    RUN_MEM(test_client_details_wifi_is_not_a_session);
    RUN_MEM(test_close_button);
    RUN_MEM(test_scrim_blocks_the_view_below);
    RUN_MEM(test_lifecycle);

    lv_test_indev_delete_all();
    ui_theme_deinit();
    lv_display_delete(disp);
    lv_deinit();
    TEST_MAIN_END("test_ui_view_status");
}
