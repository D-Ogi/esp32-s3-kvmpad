// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_modal and ui_toast (TASK-060D) against the real LVGL 9.6.
//
// - AT-UIFW-10: while a modal is open, a press aimed at the button underneath it never reaches
//   that button, and after the close the same press works again. 1000 open/close repetitions must
//   leave the pad usable and the LVGL heap unchanged;
// - a modal button reports its control id once, closing is idempotent, and a tap on the card does
//   not trigger the scrim dismiss;
// - the toast deletes itself when its timer fires, dismissing it early removes the timer, and it
//   is never clickable, so a gesture running on the PAD underneath is not blocked (07 §7).
#include <stdio.h>
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_components.h"
#include "ui_test_env.h"

#define CTRL_UNDER  0x0501
#define CTRL_CANCEL 0x0601
#define CTRL_CONFIRM 0x0602

typedef struct {
    int presses;
} under_log_t;

static void under_fn(uint16_t control_id, bool pressed, void *user_data)
{
    under_log_t *log = user_data;
    if (pressed && control_id == CTRL_UNDER) {
        log->presses++;
    }
}

typedef struct {
    int calls;
    uint16_t last_id;
    lv_obj_t *last_modal;
} modal_log_t;

static void modal_fn(lv_obj_t *modal, uint16_t control_id, void *user_data)
{
    modal_log_t *log = user_data;
    log->calls++;
    log->last_id = control_id;
    log->last_modal = modal;
}

static const ui_modal_button_t k_buttons[] = {
    {"Cancel", CTRL_CANCEL, UI_BTN_SECONDARY, true},
    {"Confirm", CTRL_CONFIRM, UI_BTN_PRIMARY, true},
};

static const ui_modal_desc_t k_modal_desc = {
    .title = "Close the window on the host?",
    .body = "Alt+F4 is sent to the computer connected over USB. The device cannot undo it.",
    .buttons = k_buttons,
    .button_count = 2,
    .dismiss_on_scrim = false,
    .overlay_id = 7,
};

static const ui_action_key_desc_t k_under_desc = {
    .label = "Trackpad action",
    .control_id = CTRL_UNDER,
    .variant = UI_KEY_NORMAL,
    .width = 400,
    .height = 120,
    .font = UI_FONT_BODY,
};

// A pad-sized button standing in for whatever the modal must cover.
static lv_obj_t *build_under(lv_obj_t *root, ui_action_key_sink_t *sink)
{
    lv_obj_t *key = ui_action_key_create(root, &k_under_desc, sink);
    CHECK(key != NULL);
    lv_obj_set_pos(key, UI_CONTENT_INSET, UI_CONTENT_TOP + UI_SPACE_XXL);
    lv_obj_update_layout(root);
    return key;
}

static void test_modal_blocks_and_releases(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    under_log_t under = {0};
    ui_action_key_sink_t under_sink = {.fn = under_fn, .user_data = &under};
    lv_obj_t *key = build_under(root, &under_sink);
    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(key, &x, &y));

    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 1); // the pad works before any overlay exists

    modal_log_t log = {0};
    ui_modal_sink_t sink = {.fn = modal_fn, .user_data = &log};
    lv_obj_t *modal = ui_modal_create(root, &k_modal_desc, &sink);
    CHECK(modal != NULL);
    CHECK_EQ(ui_modal_overlay_id(modal), 7);
    CHECK(ui_modal_card(modal) != NULL);
    CHECK(!ui_modal_is_closing(modal));
    lv_obj_update_layout(root);
    lv_refr_now(NULL);

    // AT-UIFW-10: the press lands on the scrim, not on the button underneath.
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 1);
    CHECK_EQ(log.calls, 0); // the scrim does not dismiss and raises no control id
    CHECK(!ui_action_key_is_down(key));

    // A tap on the card itself is also absorbed.
    int32_t cx = 0;
    int32_t cy = 0;
    CHECK(ui_test_center_of(ui_modal_card(modal), &cx, &cy));
    lv_test_mouse_click_at(cx, cy);
    CHECK_EQ(under.presses, 1);
    CHECK_EQ(log.calls, 0);

    // The Confirm button reports its id once and closes.
    lv_obj_t *row = lv_obj_get_child(ui_modal_card(modal), 2);
    CHECK(row != NULL);
    CHECK_EQ(lv_obj_get_child_count(row), 2);
    int32_t bx = 0;
    int32_t by = 0;
    CHECK(ui_test_center_of(lv_obj_get_child(row, 1), &bx, &by));
    // lv_test_mouse_click_at() pumps lv_timer_handler(), so the asynchronous delete has already
    // run when it returns: `modal` is a dangling pointer from here on and only the recorded
    // pointer value may be compared. That is also true in the firmware, where the LVGL owner
    // pumps the handler after dispatching the event.
    lv_test_mouse_click_at(bx, by);
    CHECK_EQ(log.calls, 1);
    CHECK_EQ(log.last_id, CTRL_CONFIRM);
    CHECK(log.last_modal == modal);
    modal = NULL;

    // 07 §7: the overlay must not keep the pad blocked after closing.
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 2);
    CHECK_EQ(log.calls, 1);

    // Closing is idempotent while the delete is still pending (no second queued delete).
    lv_obj_t *again = ui_modal_create(root, &k_modal_desc, &sink);
    CHECK(again != NULL);
    CHECK(!ui_modal_is_closing(again));
    ui_modal_close(again);
    CHECK(ui_modal_is_closing(again));
    CHECK(!lv_obj_is_clickable(again)); // stops swallowing presses the moment it is decided
    ui_modal_close(again);
    ui_modal_close(again);
    lv_test_wait(20);
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 3);

    lv_obj_delete(root);
    ui_theme_deinit();
}

static void test_modal_scrim_dismiss(void)
{
    static const ui_modal_desc_t desc = {
        .title = "Host details",
        .body = "USB device mounted. Owner: local.",
        .dismiss_on_scrim = true,
        .overlay_id = 2,
    };
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    under_log_t under = {0};
    ui_action_key_sink_t under_sink = {.fn = under_fn, .user_data = &under};
    lv_obj_t *key = build_under(root, &under_sink);
    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(key, &x, &y));

    lv_obj_t *modal = ui_modal_create(root, &desc, NULL); // no sink: a details popover
    CHECK(modal != NULL);
    lv_obj_update_layout(root);
    // A tap on the card must not dismiss; a tap outside must.
    int32_t cx = 0;
    int32_t cy = 0;
    CHECK(ui_test_center_of(ui_modal_card(modal), &cx, &cy));
    const uint32_t children = lv_obj_get_child_count(root);
    lv_test_mouse_click_at(cx, cy);
    lv_test_wait(20);
    CHECK(!ui_modal_is_closing(modal)); // a tap on the card leaves it open
    CHECK_EQ(lv_obj_get_child_count(root), children);
    // A tap outside the card closes it. lv_test_mouse_click_at() pumps lv_timer_handler(), so the
    // overlay is already deleted when it returns and `modal` must not be dereferenced again.
    lv_test_mouse_click_at(UI_SCREEN_W - 8, UI_SCREEN_H - 8);
    CHECK_EQ(lv_obj_get_child_count(root), children - 1);
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 1);
    lv_obj_delete(root);
    ui_theme_deinit();
}

// AT-UIFW-10: 1000 open/close repetitions. The pad must be usable after every close and the LVGL
// heap must be back at the baseline at the end.
#define MODAL_CYCLES 1000

static void test_modal_cycles(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    under_log_t under = {0};
    ui_action_key_sink_t under_sink = {.fn = under_fn, .user_data = &under};
    lv_obj_t *key = build_under(root, &under_sink);
    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(key, &x, &y));
    modal_log_t log = {0};
    ui_modal_sink_t sink = {.fn = modal_fn, .user_data = &log};
    lv_refr_now(NULL);

    const size_t base_cnt = host_lv_live_count();
    const size_t base_bytes = host_lv_live_bytes();
    int blocked = 0;
    for (int i = 0; i < MODAL_CYCLES; i++) {
        lv_obj_t *modal = ui_modal_create(root, &k_modal_desc, &sink);
        CHECK(modal != NULL);
        lv_obj_update_layout(root);
        const int before = under.presses;
        lv_test_mouse_click_at(x, y);
        if (under.presses == before) {
            blocked++;
        }
        ui_modal_close(modal);
        lv_test_wait(20);
        lv_test_mouse_click_at(x, y);
    }
    lv_refr_now(NULL);
    printf("    %d modal cycles: blocked %d/%d, pad presses %d, live allocations %zu -> %zu, "
           "bytes %zu -> %zu\n",
           MODAL_CYCLES, blocked, MODAL_CYCLES, under.presses, base_cnt, host_lv_live_count(),
           base_bytes, host_lv_live_bytes());
    CHECK_EQ(blocked, MODAL_CYCLES);          // every open modal blocked the pad
    CHECK_EQ(under.presses, MODAL_CYCLES);    // every close handed it back
    CHECK_EQ(log.calls, 0);
    CHECK_EQ(host_lv_live_count(), base_cnt);
    CHECK_EQ(host_lv_live_bytes(), base_bytes);

    lv_obj_delete(root);
    ui_theme_deinit();
}

// ---------------------------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------------------------
static void test_toast_self_deletes(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    under_log_t under = {0};
    ui_action_key_sink_t under_sink = {.fn = under_fn, .user_data = &under};
    lv_obj_t *key = build_under(root, &under_sink);
    int32_t x = 0;
    int32_t y = 0;
    CHECK(ui_test_center_of(key, &x, &y));

    const uint32_t children = lv_obj_get_child_count(root);
    lv_obj_t *toast = ui_toast_show(root, "Shortcut sent to the host", 500);
    CHECK(toast != NULL);
    CHECK(ui_toast_timer(toast) != NULL);
    CHECK(!lv_obj_is_clickable(toast)); // never steals a press (07 §7)
    CHECK_EQ(lv_obj_get_child_count(root), children + 1);
    lv_obj_update_layout(root);
    lv_refr_now(NULL);

    // 07 §7: the toast must not block the touchpad while it is visible.
    lv_test_mouse_click_at(x, y);
    CHECK_EQ(under.presses, 1);

    lv_test_wait(300);
    CHECK_EQ(lv_obj_get_child_count(root), children + 1); // still up before the timeout
    lv_test_wait(400);
    CHECK_EQ(lv_obj_get_child_count(root), children); // the timer deleted it and itself

    // An early dismiss must take the timer with it.
    lv_obj_t *second = ui_toast_show(root, "Release all", 0);
    CHECK(second != NULL);
    CHECK(ui_toast_timer(second) != NULL);
    ui_toast_dismiss(second);
    CHECK_EQ(lv_obj_get_child_count(root), children);
    lv_test_wait(UI_TOAST_DEFAULT_MS + 100); // the removed timer must not fire on a dead widget

    // A toast without a timeout stays until it is dismissed.
    lv_obj_t *sticky = ui_toast_show(root, "Maintenance mode", UI_TOAST_NO_TIMEOUT);
    CHECK(sticky != NULL);
    CHECK(ui_toast_timer(sticky) == NULL);
    lv_test_wait(UI_TOAST_DEFAULT_MS + 100);
    CHECK_EQ(lv_obj_get_child_count(root), children + 1);
    ui_toast_dismiss(sticky);
    CHECK_EQ(lv_obj_get_child_count(root), children);

    CHECK(ui_toast_show(root, NULL, 0) == NULL);
    CHECK(ui_toast_show(NULL, "x", 0) == NULL);
    CHECK(ui_toast_timer(NULL) == NULL);
    ui_toast_dismiss(NULL);

    lv_obj_delete(root);
    ui_theme_deinit();
}

static void test_toast_cycles(void)
{
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    // Warm-up: the first rendered toast leaves one long-lived renderer block behind, like the first
    // text render. Render and dismiss one before taking the baseline.
    ui_toast_dismiss(ui_toast_show(root, "Owner changed to remote", UI_TOAST_NO_TIMEOUT));
    lv_refr_now(NULL);
    const size_t base_cnt = host_lv_live_count();
    const size_t base_bytes = host_lv_live_bytes();
    for (int i = 0; i < 2000; i++) {
        lv_obj_t *toast = ui_toast_show(root, "Owner changed to remote", 20);
        CHECK(toast != NULL);
        if (i % 2 == 0) {
            lv_test_wait(30); // let the timer delete it
            CHECK_EQ(lv_obj_get_child_count(root), 0);
        } else {
            ui_toast_dismiss(toast); // delete it before the timer fires
        }
    }
    lv_test_wait(100);
    lv_refr_now(NULL);
    printf("    2000 toast cycles: live allocations %zu -> %zu, bytes %zu -> %zu\n", base_cnt,
           host_lv_live_count(), base_bytes, host_lv_live_bytes());
    CHECK_EQ(host_lv_live_count(), base_cnt);
    CHECK_EQ(host_lv_live_bytes(), base_bytes);
    lv_obj_delete(root);
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
    printf("test_ui_overlay (LVGL %d.%d.%d, %dx%d RGB565)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH, UI_SCREEN_W, UI_SCREEN_H);
    const size_t base = ui_test_env_start();
    printf("  live allocations baseline: %zu\n", base);
    RUN_MEM(test_modal_blocks_and_releases);
    RUN_MEM(test_modal_scrim_dismiss);
    RUN_MEM(test_modal_cycles);
    RUN_MEM(test_toast_self_deletes);
    RUN_MEM(test_toast_cycles);
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
    printf("  live allocations after all tests: %zu (baseline %zu)\n", host_lv_live_count(), base);
    CHECK_EQ(host_lv_live_count(), base);
    ui_test_env_stop(base);
    TEST_MAIN_END("test_ui_overlay");
}
