// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_components (TASK-060D): status item, topbar, action key and segmented control against the
// real LVGL 9.6 on a headless 1024x600 RGB565 display.
//
// - a value the descriptor table does not list reads "Unknown" with the unknown severity, -1
//   included (07 §12 item 2, FR-401/403);
// - a bound item follows its subject, and the binding survives the subject being deleted before
//   or after the widget (07 §4, AT-UIFW-09; the address sanitizer is the real assertion here);
// - the topbar keeps its width and height with a 200-character status string and every slot stays
//   inside the bar (07 §7: the bar must not grow with the IP/SSID);
// - every action key and every tab is at least 44x44 px (07 §7, FR-003);
// - an action key emits exactly one down edge and one matching up edge, including when the press
//   is lost, when the key is disabled while held and when the widget is deleted while held
//   (AGENTS.md prohibition 3 - a view change must never leave a key DOWN);
// - sliding a press from one key onto its neighbor produces no phantom down on the neighbor;
// - 10 000 create/delete cycles of each component leave the live LVGL allocation count unchanged
//   (AT-UIFW-02 at component level).
#include <stdio.h>
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_components.h"
#include "ui_test_env.h"

// ---------------------------------------------------------------------------------------------
// Fixtures. Values mirror the HOST wording of 01 §6.1 without including any domain header: the
// components must not depend on hid_core (07 §2 dependency rule).
// ---------------------------------------------------------------------------------------------
#define HOST_DETACHED    1
#define HOST_ENUMERATING 2
#define HOST_MOUNTED     3
#define HOST_SUSPENDED   4
#define HOST_ERROR       5

static const ui_status_row_t k_host_rows[] = {
    {HOST_DETACHED, "Disconnected", UI_SEVERITY_OFFLINE},
    {HOST_ENUMERATING, "Connecting", UI_SEVERITY_WARNING},
    {HOST_MOUNTED, "Active", UI_SEVERITY_OK},
    {HOST_SUSPENDED, "Suspended", UI_SEVERITY_WARNING},
    {HOST_ERROR, "Error", UI_SEVERITY_OFFLINE},
};

static const ui_status_desc_t k_host_desc = {
    .title = "Host",
    .rows = k_host_rows,
    .row_count = (uint16_t)(sizeof(k_host_rows) / sizeof(k_host_rows[0])),
    .unknown_text = "Unknown",
    .width = UI_TOPBAR_STATUS_W,
    .control_id = 0x0101,
    .clickable = true,
};

static const ui_status_row_t k_client_rows[] = {
    {0, "Off", UI_SEVERITY_OFFLINE},
    {4, "Connected", UI_SEVERITY_OK},
};

static const ui_status_desc_t k_client_desc = {
    .title = "Client",
    .rows = k_client_rows,
    .row_count = 2,
    .unknown_text = NULL, // => "Unknown"
    .width = UI_TOPBAR_STATUS_W,
    .control_id = 0x0102,
    .clickable = true,
};

static const ui_segment_t k_modes[] = {
    {"Mouse", LV_SYMBOL_GPS, 0x0201},
    {"Keyboard", LV_SYMBOL_KEYBOARD, 0x0202},
    {"Shortcuts", LV_SYMBOL_LIST, 0x0203},
};

static const ui_segmented_desc_t k_modes_desc = {
    .segments = k_modes,
    .count = 3,
    .width = UI_TOPBAR_NAV_W,
    .height = UI_SEGMENTED_H,
};

// ---------------------------------------------------------------------------------------------
// Recorded edges from the key and segmented sinks.
// ---------------------------------------------------------------------------------------------
typedef struct {
    int downs;
    int ups;
    uint16_t last_id;
    bool last_pressed;
    int balance; // downs - ups; must be 0 whenever no finger is on a key
} key_log_t;

static void key_sink_fn(uint16_t control_id, bool pressed, void *user_data)
{
    key_log_t *log = user_data;
    log->last_id = control_id;
    log->last_pressed = pressed;
    if (pressed) {
        log->downs++;
        log->balance++;
    } else {
        log->ups++;
        log->balance--;
    }
}

typedef struct {
    int taps;
    uint16_t last_id;
    uint16_t last_index;
} seg_log_t;

static void seg_sink_fn(uint16_t control_id, uint16_t index, void *user_data)
{
    seg_log_t *log = user_data;
    log->taps++;
    log->last_id = control_id;
    log->last_index = index;
}

static void check_hit_area(lv_obj_t *obj, const char *what)
{
    CHECK(obj != NULL);
    if (!obj) {
        return;
    }
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const int32_t w = a.x2 - a.x1 + 1;
    const int32_t h = a.y2 - a.y1 + 1;
    if (w < UI_TOUCH_MIN || h < UI_TOUCH_MIN) {
        printf("    %s hit area %dx%d is below %dx%d\n", what, (int)w, (int)h, UI_TOUCH_MIN, UI_TOUCH_MIN);
    }
    CHECK(w >= UI_TOUCH_MIN);
    CHECK(h >= UI_TOUCH_MIN);
}

// ---------------------------------------------------------------------------------------------
// Status item
// ---------------------------------------------------------------------------------------------
static void test_status_unknown_by_default(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    lv_obj_t *item = ui_status_item_create(root, &k_host_desc, NULL);
    CHECK(item != NULL);
    lv_obj_t *value = ui_status_item_value_label(item);
    CHECK(value != NULL);
    CHECK(strcmp(lv_label_get_text(value), "Unknown") == 0);

    // -1 is the store's UNKNOWN; so is any other unlisted value. Neither may read as a real status.
    const int32_t unlisted[] = {-1, 0, 99, INT32_MIN, INT32_MAX};
    for (size_t i = 0; i < sizeof(unlisted) / sizeof(unlisted[0]); i++) {
        ui_status_item_set_value(item, unlisted[i]);
        CHECK(strcmp(lv_label_get_text(value), "Unknown") == 0);
        CHECK_EQ(ui_status_item_severity_of(&k_host_desc, unlisted[i]), UI_SEVERITY_UNKNOWN);
    }
    // A descriptor without an unknown text still reads "Unknown", never an empty label.
    CHECK(strcmp(ui_status_item_text_of(&k_client_desc, -1), "Unknown") == 0);
    CHECK(strcmp(ui_status_item_text_of(NULL, -1), "Unknown") == 0);

    for (uint16_t i = 0; i < k_host_desc.row_count; i++) {
        ui_status_item_set_value(item, k_host_rows[i].value);
        CHECK(strcmp(lv_label_get_text(value), k_host_rows[i].text) == 0);
        const lv_color_t want = ui_theme_severity_color(k_host_rows[i].severity);
        const lv_color_t got = lv_obj_get_style_text_color(value, LV_PART_MAIN);
        CHECK(want.red == got.red && want.green == got.green && want.blue == got.blue);
    }
    lv_refr_now(NULL);
    lv_obj_delete(root);
    ui_theme_deinit();
}

static void test_status_follows_subject(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    lv_subject_t *subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    CHECK(subject != NULL);
    lv_subject_set_int(subject, -1); // UI_STORE_UNKNOWN
    lv_obj_t *item = ui_status_item_create(root, &k_host_desc, subject);
    lv_obj_t *value = ui_status_item_value_label(item);
    CHECK(strcmp(lv_label_get_text(value), "Unknown") == 0); // notified once on subscription

    lv_subject_set_int(subject, HOST_MOUNTED);
    CHECK(strcmp(lv_label_get_text(value), "Active") == 0);
    const lv_color_t ok = ui_theme_severity_color(UI_SEVERITY_OK);
    lv_color_t got = lv_obj_get_style_text_color(value, LV_PART_MAIN);
    CHECK(ok.red == got.red && ok.green == got.green && ok.blue == got.blue);

    lv_subject_set_int(subject, HOST_ERROR);
    CHECK(strcmp(lv_label_get_text(value), "Error") == 0);
    const lv_color_t bad = ui_theme_severity_color(UI_SEVERITY_OFFLINE);
    got = lv_obj_get_style_text_color(value, LV_PART_MAIN);
    CHECK(bad.red == got.red && bad.green == got.green && bad.blue == got.blue);

    // A value the domain never defined must fall back, not keep the last good status.
    lv_subject_set_int(subject, 77);
    CHECK(strcmp(lv_label_get_text(value), "Unknown") == 0);
    lv_refr_now(NULL);
    lv_obj_delete(root);
    lv_subject_delete(subject);
    ui_theme_deinit();
}

// AT-UIFW-09: both orders of destruction, with a publish after each one. The address sanitizer
// reports a use-after-free if an observer outlives either side.
static void test_subject_lifetime_both_orders(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();

    // (a) the widget goes first; later publishes must not reach it
    lv_subject_t *subject = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject, HOST_MOUNTED);
    lv_obj_t *item = ui_status_item_create(root, &k_host_desc, subject);
    CHECK(item != NULL);
    CHECK_EQ(lv_ll_get_len(&subject->subs_ll), 1);
    lv_obj_delete(item);
    CHECK_EQ(lv_ll_get_len(&subject->subs_ll), 0);
    lv_subject_set_int(subject, HOST_DETACHED);
    lv_subject_delete(subject);

    // (b) the subject goes first; deleting the widget afterwards must not touch the freed subject
    lv_subject_t *subject2 = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(subject2, HOST_SUSPENDED);
    lv_obj_t *item2 = ui_status_item_create(root, &k_host_desc, subject2);
    lv_obj_t *seg = ui_segmented_create(root, &k_modes_desc, NULL);
    ui_segmented_bind_active(seg, subject2);
    CHECK(item2 != NULL && seg != NULL);
    CHECK_EQ(lv_ll_get_len(&subject2->subs_ll), 2);
    lv_subject_delete(subject2);
    lv_refr_now(NULL);
    lv_obj_delete(item2);
    lv_obj_delete(seg);

    lv_obj_delete(root);
    ui_theme_deinit();
}

// ---------------------------------------------------------------------------------------------
// Topbar
// ---------------------------------------------------------------------------------------------
static void test_topbar_fixed_with_long_text(void)
{
    static char long_text[201];
    memset(long_text, 'W', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    static const ui_status_row_t rows[] = {{HOST_MOUNTED, long_text, UI_SEVERITY_OK}};
    static const ui_status_desc_t desc = {
        .title = "Host",
        .rows = rows,
        .row_count = 1,
        .width = UI_TOPBAR_STATUS_W,
        .clickable = true,
    };
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    lv_subject_t *awake = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(awake, 0);
    lv_subject_t *host = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(host, -1);

    const ui_topbar_desc_t bar_desc = {
        .brand = "KVMpad",
        .status = {&desc, &k_client_desc},
        .keep_awake_text = "Keep awake",
        .keep_awake_icon = LV_SYMBOL_EYE_OPEN,
    };
    const ui_topbar_bind_t bind = {.status = {host, NULL}, .keep_awake = awake};
    lv_obj_t *bar = ui_topbar_create(root, &bar_desc, &bind);
    CHECK(bar != NULL);
    lv_obj_update_layout(bar);
    lv_area_t before;
    lv_obj_get_coords(bar, &before);
    CHECK_EQ(before.x2 - before.x1 + 1, UI_SCREEN_W);
    CHECK_EQ(before.y2 - before.y1 + 1, UI_TOPBAR_H);

    lv_obj_t *item = ui_topbar_status_item(bar, UI_TOPBAR_HOST);
    CHECK(item != NULL);
    CHECK(ui_topbar_status_item(bar, UI_TOPBAR_STATUS_COUNT) == NULL);
    // An omitted status leaves a hidden placeholder, so the remaining slots keep their indices and
    // the bar keeps its width.
    const ui_topbar_desc_t half_desc = {.brand = "KVMpad", .status = {NULL, &k_client_desc}};
    lv_obj_t *half = ui_topbar_create(root, &half_desc, NULL);
    CHECK(half != NULL);
    CHECK(ui_topbar_status_item(half, UI_TOPBAR_HOST) == NULL);
    CHECK(ui_topbar_status_item(half, UI_TOPBAR_CLIENT) != NULL);
    CHECK(ui_topbar_keep_awake(half) == NULL); // no keep_awake_text in this descriptor
    lv_obj_update_layout(half);
    lv_area_t half_box;
    lv_obj_get_coords(half, &half_box);
    CHECK_EQ(half_box.x2 - half_box.x1 + 1, UI_SCREEN_W);
    lv_obj_delete(half);
    lv_subject_set_int(host, HOST_MOUNTED); // 200-character status text
    lv_obj_update_layout(bar);
    lv_refr_now(NULL);

    lv_area_t after;
    lv_obj_get_coords(bar, &after);
    CHECK_EQ(after.x2 - after.x1 + 1, UI_SCREEN_W); // the bar did not grow
    CHECK_EQ(after.y2 - after.y1 + 1, UI_TOPBAR_H);
    lv_area_t slot;
    lv_obj_get_coords(item, &slot);
    CHECK_EQ(slot.x2 - slot.x1 + 1, UI_TOPBAR_STATUS_W); // the slot clipped instead of growing
    CHECK(slot.y1 >= after.y1 && slot.y2 <= after.y2);
    // The 200-character status is clipped with dots, not wrapped: the label stays one line high
    // and inside the slot, and LVGL shortened what it draws.
    lv_obj_t *value = ui_status_item_value_label(item);
    lv_area_t vbox;
    lv_obj_get_coords(value, &vbox);
    CHECK(vbox.x1 >= slot.x1 && vbox.x2 <= slot.x2);
    CHECK(vbox.y1 >= slot.y1 && vbox.y2 <= slot.y2);
    CHECK(vbox.y2 - vbox.y1 + 1 < UI_STATUS_ITEM_H);
    CHECK_EQ(lv_label_get_long_mode(value), LV_LABEL_LONG_MODE_DOTS);
    CHECK(strlen(lv_label_get_text(value)) < 200);

    // Every slot stays inside the bar, keep-awake visible or not.
    lv_obj_t *keep = ui_topbar_keep_awake(bar);
    CHECK(keep != NULL);
    CHECK(lv_obj_is_hidden(keep)); // FR-712: hidden while the value is 0
    lv_subject_set_int(awake, -1);
    CHECK(lv_obj_is_hidden(keep)); // unknown never claims the feature is on
    lv_subject_set_int(awake, 1);
    CHECK(!lv_obj_is_hidden(keep));
    lv_obj_update_layout(bar);
    lv_refr_now(NULL);

    lv_obj_t *nav = ui_topbar_nav_slot(bar);
    lv_obj_t *trailing = ui_topbar_trailing_slot(bar);
    CHECK(nav != NULL && trailing != NULL);
    lv_obj_t *seg = ui_segmented_create(nav, &k_modes_desc, NULL);
    CHECK(seg != NULL);
    static const ui_action_key_desc_t settings = {
        .icon = LV_SYMBOL_SETTINGS,
        .control_id = 0x0300,
        .variant = UI_KEY_NORMAL,
        .width = UI_TOPBAR_BTN_W,
        .height = UI_TOPBAR_BTN_W,
        .font = UI_FONT_BODY,
    };
    lv_obj_t *btn = ui_action_key_create(trailing, &settings, NULL);
    lv_obj_update_layout(bar);
    lv_refr_now(NULL);

    lv_obj_get_coords(bar, &after);
    CHECK_EQ(after.x2 - after.x1 + 1, UI_SCREEN_W);
    CHECK_EQ(after.y2 - after.y1 + 1, UI_TOPBAR_H);
    lv_obj_t *const inside[] = {keep, item, ui_topbar_status_item(bar, UI_TOPBAR_CLIENT), seg, btn};
    for (size_t i = 0; i < sizeof(inside) / sizeof(inside[0]); i++) {
        lv_area_t a;
        lv_obj_get_coords(inside[i], &a);
        if (a.x1 < after.x1 || a.x2 > after.x2 || a.y1 < after.y1 || a.y2 > after.y2) {
            printf("    slot %zu at [%d..%d]x[%d..%d] leaves the bar [%d..%d]x[%d..%d]\n", i, (int)a.x1,
                   (int)a.x2, (int)a.y1, (int)a.y2, (int)after.x1, (int)after.x2, (int)after.y1,
                   (int)after.y2);
        }
        CHECK(a.x1 >= after.x1 && a.x2 <= after.x2);
        CHECK(a.y1 >= after.y1 && a.y2 <= after.y2);
    }
    check_hit_area(item, "topbar host status");
    check_hit_area(btn, "topbar settings button");
    for (uint16_t i = 0; i < k_modes_desc.count; i++) {
        check_hit_area(ui_segmented_tab(seg, i), "mode tab");
    }

    lv_obj_delete(root);
    lv_subject_delete(host);
    lv_subject_delete(awake);
    ui_theme_deinit();
}

// ---------------------------------------------------------------------------------------------
// Action key
// ---------------------------------------------------------------------------------------------
static void build_keypad(lv_obj_t *root, lv_obj_t **out, const ui_action_key_desc_t *descs,
                         int count, ui_action_key_sink_t *sink)
{
    lv_obj_t *row = lv_obj_create(root);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, UI_CONTENT_INSET, UI_CONTENT_TOP);
    lv_obj_set_size(row, UI_SCREEN_W - 2 * UI_CONTENT_INSET, 96);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_style_pad_gap(row, UI_SPACE_S, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < count; i++) {
        out[i] = ui_action_key_create(row, &descs[i], sink);
        CHECK(out[i] != NULL);
    }
    lv_obj_update_layout(root);
}

static const ui_action_key_desc_t k_keys[] = {
    {.label = "A", .control_id = 0x0401, .variant = UI_KEY_NORMAL, .height = 88, .font = UI_FONT_TITLE},
    {.label = "Ctrl", .control_id = 0x0402, .variant = UI_KEY_MODIFIER, .checkable = true, .height = 88,
     .font = UI_FONT_BODY},
    {.label = "Reset", .icon = LV_SYMBOL_TRASH, .control_id = 0x0403, .variant = UI_KEY_DESTRUCTIVE,
     .height = 88, .font = UI_FONT_BODY},
    // A descriptor asking for less than the touch minimum must be raised, not honored.
    {.label = "tiny", .control_id = 0x0404, .variant = UI_KEY_NORMAL, .width = 10, .height = 10,
     .font = UI_FONT_LABEL},
};

static void test_action_key_edges(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    key_log_t log = {0};
    ui_action_key_sink_t sink = {.fn = key_sink_fn, .user_data = &log};
    lv_obj_t *keys[4];
    build_keypad(root, keys, k_keys, 4, &sink);
    for (int i = 0; i < 4; i++) {
        check_hit_area(keys[i], k_keys[i].label);
    }

    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(keys[0], &x, &y));
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(log.downs, 1);
    CHECK_EQ(log.ups, 1);
    CHECK_EQ(log.balance, 0);
    CHECK_EQ(log.last_id, 0x0401);
    CHECK(!ui_action_key_is_down(keys[0]));
    CHECK_EQ(ui_action_key_control_id(keys[0]), 0x0401);

    // A checkable key toggles on click without changing the edge contract.
    int32_t cx = 0;
    int32_t cy = 0;
    CHECK(ui_test_center_of(keys[1], &cx, &cy));
    lv_test_mouse_click_at(cx, cy);
    CHECK_EQ(log.downs, 2);
    CHECK_EQ(log.ups, 2);
    CHECK(lv_obj_has_state(keys[1], LV_STATE_CHECKED));
    lv_test_mouse_click_at(cx, cy);
    CHECK(!lv_obj_has_state(keys[1], LV_STATE_CHECKED));
    CHECK_EQ(log.balance, 0);

    // Press lost: slide the finger from key 0 onto key 2. The up edge belongs to key 0 and key 2
    // must not report a phantom down (LVGL suppresses PRESSED while the previous press is open).
    int32_t tx = 0;
    int32_t ty = 0;
    CHECK(ui_test_center_of(keys[2], &tx, &ty));
    const int downs = log.downs;
    const int ups = log.ups;
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(60);
    CHECK_EQ(log.downs, downs + 1);
    CHECK(ui_action_key_is_down(keys[0]));
    lv_test_mouse_move_to(tx, ty);
    lv_test_wait(60);
    CHECK_EQ(log.ups, ups + 1);
    CHECK(!ui_action_key_is_down(keys[0]));
    CHECK(!ui_action_key_is_down(keys[2]));
    CHECK_EQ(log.downs, downs + 1); // no phantom down on the neighbor
    lv_test_mouse_release();
    lv_test_wait(60);
    CHECK_EQ(log.balance, 0);
    CHECK_EQ(log.downs, downs + 1);

    lv_obj_delete(root);
    ui_theme_deinit();
}

// AGENTS.md prohibition 3: a key held down when it is disabled, or when the screen that owns it is
// torn down for a mode switch, must still produce its up edge.
static void test_action_key_release_on_disable_and_delete(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    key_log_t log = {0};
    ui_action_key_sink_t sink = {.fn = key_sink_fn, .user_data = &log};
    lv_obj_t *keys[4];
    build_keypad(root, keys, k_keys, 4, &sink);

    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(keys[0], &x, &y));
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(60);
    CHECK_EQ(log.balance, 1);
    ui_action_key_set_enabled(keys[0], false);
    CHECK_EQ(log.balance, 0); // released before the key went inert
    CHECK_EQ(log.ups, 1);
    CHECK(lv_obj_has_state(keys[0], LV_STATE_DISABLED));
    lv_test_mouse_release();
    lv_test_wait(60);
    CHECK_EQ(log.balance, 0);

    // A disabled key accepts no new press at all.
    const int downs = log.downs;
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(log.downs, downs);
    ui_action_key_set_enabled(keys[0], true);
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(log.downs, downs + 1);
    CHECK_EQ(log.balance, 0);

    // Held down, then the owning screen is deleted (what a mode switch does).
    int32_t hx = 0;
    int32_t hy = 0;
    CHECK(ui_test_center_of(keys[1], &hx, &hy));
    lv_test_mouse_move_to(hx, hy);
    lv_test_mouse_press();
    lv_test_wait(60);
    CHECK_EQ(log.balance, 1);
    lv_obj_delete(root);
    CHECK_EQ(log.balance, 0); // the delete event released it
    lv_test_mouse_release();
    lv_test_wait(60);
    CHECK_EQ(log.balance, 0);
    ui_theme_deinit();
}

// ---------------------------------------------------------------------------------------------
// Segmented control
// ---------------------------------------------------------------------------------------------
static void test_segmented_selection(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    seg_log_t log = {0};
    ui_segmented_sink_t sink = {.fn = seg_sink_fn, .user_data = &log};
    lv_obj_t *holder = lv_obj_create(root);
    lv_obj_remove_style_all(holder);
    lv_obj_set_pos(holder, UI_CONTENT_INSET, UI_SPACE_S);
    lv_obj_set_size(holder, UI_TOPBAR_NAV_W, UI_SEGMENTED_H);
    lv_obj_t *seg = ui_segmented_create(holder, &k_modes_desc, &sink);
    CHECK(seg != NULL);
    lv_obj_update_layout(root);
    for (uint16_t i = 0; i < k_modes_desc.count; i++) {
        check_hit_area(ui_segmented_tab(seg, i), k_modes[i].label);
    }
    CHECK(ui_segmented_tab(seg, 3) == NULL);
    CHECK_EQ(ui_segmented_get_active(seg), -1); // unknown active view: nothing selected

    // 07 §5: a tap reports; it does not move the selection. The router does that.
    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(ui_segmented_tab(seg, 1), &x, &y));
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(log.taps, 1);
    CHECK_EQ(log.last_id, 0x0202);
    CHECK_EQ(log.last_index, 1);
    CHECK_EQ(ui_segmented_get_active(seg), -1);

    ui_segmented_set_active(seg, 1);
    CHECK_EQ(ui_segmented_get_active(seg), 1);
    ui_segmented_set_active(seg, 2);
    CHECK_EQ(ui_segmented_get_active(seg), 2);
    CHECK(!lv_obj_has_state(ui_segmented_tab(seg, 1), LV_STATE_CHECKED)); // exactly one selected
    ui_segmented_set_active(seg, 9);
    CHECK_EQ(ui_segmented_get_active(seg), -1);
    ui_segmented_set_active(seg, -1);
    CHECK_EQ(ui_segmented_get_active(seg), -1);

    // Bound to a subject the way the router publishes the active view.
    lv_subject_t *view = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(view, -1);
    ui_segmented_bind_active(seg, view);
    CHECK_EQ(ui_segmented_get_active(seg), -1);
    lv_subject_set_int(view, 0);
    CHECK_EQ(ui_segmented_get_active(seg), 0);
    lv_subject_set_int(view, 2);
    CHECK_EQ(ui_segmented_get_active(seg), 2);
    lv_subject_set_int(view, 3); // UI_VIEW_SETTINGS is not a tab: clear, do not guess
    CHECK_EQ(ui_segmented_get_active(seg), -1);
    lv_refr_now(NULL);

    // A rejected create returns NULL instead of a half-built widget.
    const ui_segmented_desc_t empty = {.segments = k_modes, .count = 0};
    CHECK(ui_segmented_create(root, &empty, &sink) == NULL);
    CHECK(ui_segmented_create(root, NULL, &sink) == NULL);
    CHECK(ui_segmented_create(NULL, &k_modes_desc, &sink) == NULL);

    lv_obj_delete(root);
    lv_subject_delete(view);
    ui_theme_deinit();
}

// Nothing may be built while the theme is down: a factory must refuse, not read a reset style.
static void test_factories_refuse_without_theme(void)
{
    lv_obj_t *root = ui_test_root();
    CHECK(!ui_theme_is_ready());
    CHECK(ui_status_item_create(root, &k_host_desc, NULL) == NULL);
    CHECK(ui_segmented_create(root, &k_modes_desc, NULL) == NULL);
    CHECK(ui_action_key_create(root, &k_keys[0], NULL) == NULL);
    const ui_topbar_desc_t bar = {.brand = "KVMpad"};
    CHECK(ui_topbar_create(root, &bar, NULL) == NULL);
    // Accessors on a NULL or foreign object must not crash either.
    CHECK(ui_status_item_value_label(root) == NULL);
    CHECK(ui_status_item_value_label(NULL) == NULL);
    CHECK(ui_topbar_nav_slot(root) == NULL);
    CHECK(ui_topbar_keep_awake(NULL) == NULL);
    CHECK(ui_segmented_tab(root, 0) == NULL);
    CHECK_EQ(ui_segmented_get_active(root), -1);
    CHECK_EQ(ui_action_key_control_id(NULL), 0);
    CHECK(!ui_action_key_is_down(NULL));
    ui_status_item_set_value(root, 1);
    ui_status_item_set_width(root, 100);
    ui_action_key_set_enabled(NULL, true);
    ui_action_key_set_checked(NULL, true);
    ui_segmented_set_active(root, 0);
    lv_obj_delete(root);
}

// AT-UIFW-02 at component level: 10 000 create/bind/delete cycles with snapshots arriving between
// them. Live LVGL allocations and the observer count must return to the baseline.
#define CYCLES 10000

// One full shell: the topbar with two bound statuses and the keep-awake indicator, the mode tabs
// bound to the view subject, and a four-key row.
static void build_shell(lv_obj_t *root, lv_subject_t *host, lv_subject_t *view,
                        ui_action_key_sink_t *sink, ui_segmented_sink_t *ssink)
{
    static const ui_topbar_desc_t bar_desc = {
        .brand = "KVMpad",
        .status = {&k_host_desc, &k_client_desc},
        .keep_awake_text = "Keep awake",
    };
    lv_obj_t *holder = lv_obj_create(root);
    lv_obj_remove_style_all(holder);
    lv_obj_set_size(holder, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_flex_flow(holder, LV_FLEX_FLOW_COLUMN);
    const ui_topbar_bind_t bind = {.status = {host, NULL}, .keep_awake = view};
    lv_obj_t *bar = ui_topbar_create(holder, &bar_desc, &bind);
    CHECK(bar != NULL);
    lv_obj_t *seg = ui_segmented_create(ui_topbar_nav_slot(bar), &k_modes_desc, ssink);
    ui_segmented_bind_active(seg, view);
    lv_obj_t *keys[4];
    build_keypad(holder, keys, k_keys, 4, sink);
}

static void test_cycles_no_growth(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    lv_subject_t *host = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_t *view = lv_subject_create(LV_SUBJECT_TYPE_INT);
    lv_subject_set_int(host, -1);
    lv_subject_set_int(view, 0);
    key_log_t log = {0};
    ui_action_key_sink_t sink = {.fn = key_sink_fn, .user_data = &log};
    seg_log_t slog = {0};
    ui_segmented_sink_t ssink = {.fn = seg_sink_fn, .user_data = &slog};

    // Warm-up: the first rendered shell leaves one long-lived renderer block behind, exactly as the
    // first text render does. Build, render and delete one shell before taking the baseline, so the
    // measurement covers only the repeated work.
    build_shell(root, host, view, &sink, &ssink);
    lv_refr_now(NULL);
    lv_obj_delete(lv_obj_get_child(root, 0));
    lv_refr_now(NULL);

    const size_t base_cnt = host_lv_live_count();
    const size_t base_bytes = host_lv_live_bytes();
    for (uint32_t cyc = 1; cyc <= CYCLES; cyc++) {
        build_shell(root, host, view, &sink, &ssink);
        lv_subject_set_int(host, (int32_t)(cyc % 7) - 1);
        lv_subject_set_int(view, (int32_t)(cyc % 4));
        if (cyc % 1000 == 0) {
            lv_refr_now(NULL);
        }
        lv_obj_delete(lv_obj_get_child(root, 0));
    }
    lv_refr_now(NULL);
    printf("    %d cycles: live allocations %zu -> %zu, bytes %zu -> %zu\n", CYCLES, base_cnt,
           host_lv_live_count(), base_bytes, host_lv_live_bytes());
    CHECK_EQ(host_lv_live_count(), base_cnt);
    CHECK_EQ(host_lv_live_bytes(), base_bytes);
    CHECK_EQ(lv_ll_get_len(&host->subs_ll), 0);
    CHECK_EQ(lv_ll_get_len(&view->subs_ll), 0);
    CHECK_EQ(log.balance, 0);

    lv_obj_delete(root);
    lv_subject_delete(host);
    lv_subject_delete(view);
    ui_theme_deinit();
}

#define RUN_MEM(t)                                                                              \
    do {                                                                                        \
        size_t before_ = host_lv_live_count();                                                  \
        RUN(t);                                                                                 \
        if (host_lv_live_count() != before_) {                                                  \
            printf("    %s: live allocations %zu -> %zu\n", #t, before_, host_lv_live_count()); \
        }                                                                                       \
        CHECK_EQ(host_lv_live_count(), before_);                                                \
    } while (0)

int main(void)
{
    printf("test_ui_components (LVGL %d.%d.%d, %dx%d RGB565)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH, UI_SCREEN_W, UI_SCREEN_H);
    const size_t base = ui_test_env_start();
    printf("  live allocations baseline: %zu, snapshots %s\n", base,
           ui_test_snapshots_enabled() ? "on" : "off");
    RUN_MEM(test_status_unknown_by_default);
    RUN_MEM(test_status_follows_subject);
    RUN_MEM(test_subject_lifetime_both_orders);
    RUN_MEM(test_topbar_fixed_with_long_text);
    RUN_MEM(test_action_key_edges);
    RUN_MEM(test_action_key_release_on_disable_and_delete);
    RUN_MEM(test_segmented_selection);
    RUN_MEM(test_factories_refuse_without_theme);
    RUN_MEM(test_cycles_no_growth);
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
    printf("  live allocations after all tests: %zu (baseline %zu)\n", host_lv_live_count(), base);
    CHECK_EQ(host_lv_live_count(), base);
    ui_test_env_stop(base);
    TEST_MAIN_END("test_ui_components");
}
