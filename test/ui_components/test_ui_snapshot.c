// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Composition check for TASK-060D: build a 1024x600 shell out of the six components only, in the
// three visual states the views will use, and assert the geometry rule of 01 §2.1 -
// `no_overlap && all_within_bounds && min_hitbox >= 44`. This is a layout proof, not a view: the
// screens themselves belong to TASK-070A/B/C.
//
// It also writes an RGB565 PPM of each state when the build was given a snapshot directory, for
// the visual review 07 §10 asks for at L1. The snapshots are evidence, not an assertion: a PPM is
// not a PASS and the comparison against the SDL golden set is TASK-070D's job.
#include <stdio.h>
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_components.h"
#include "ui_test_env.h"

#define MAX_HIT 32

static lv_obj_t *s_hit[MAX_HIT];
static int s_hit_count;

static void track(lv_obj_t *obj)
{
    CHECK(obj != NULL);
    CHECK(s_hit_count < MAX_HIT);
    if (obj && s_hit_count < MAX_HIT) {
        s_hit[s_hit_count++] = obj;
    }
}

static bool overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

static void check_geometry(const char *state)
{
    int bad_box = 0;
    int bad_bounds = 0;
    int bad_overlap = 0;
    for (int i = 0; i < s_hit_count; i++) {
        lv_obj_update_layout(s_hit[i]);
        lv_area_t a;
        lv_obj_get_coords(s_hit[i], &a);
        const int32_t w = a.x2 - a.x1 + 1;
        const int32_t h = a.y2 - a.y1 + 1;
        if (w < UI_TOUCH_MIN || h < UI_TOUCH_MIN) {
            printf("    %s: hit box %d is %dx%d\n", state, i, (int)w, (int)h);
            bad_box++;
        }
        if (a.x1 < 0 || a.y1 < 0 || a.x2 >= UI_SCREEN_W || a.y2 >= UI_SCREEN_H) {
            printf("    %s: hit box %d at [%d..%d]x[%d..%d] leaves the panel\n", state, i, (int)a.x1,
                   (int)a.x2, (int)a.y1, (int)a.y2);
            bad_bounds++;
        }
        for (int j = i + 1; j < s_hit_count; j++) {
            lv_area_t b;
            lv_obj_get_coords(s_hit[j], &b);
            if (overlaps(&a, &b)) {
                printf("    %s: hit boxes %d and %d overlap\n", state, i, j);
                bad_overlap++;
            }
        }
    }
    printf("    %s: %d hit boxes, %d too small, %d out of bounds, %d overlapping\n", state,
           s_hit_count, bad_box, bad_bounds, bad_overlap);
    CHECK_EQ(bad_box, 0);
    CHECK_EQ(bad_bounds, 0);
    CHECK_EQ(bad_overlap, 0);
}

// ---------------------------------------------------------------------------------------------
// Fixture data. Plain numbers, not domain enums: the components do not depend on hid_core (07 §2).
// ---------------------------------------------------------------------------------------------
static const ui_status_row_t k_host_rows[] = {
    {1, "Disconnected", UI_SEVERITY_OFFLINE},
    {3, "Active", UI_SEVERITY_OK},
    {4, "Suspended", UI_SEVERITY_WARNING},
};
static const ui_status_row_t k_client_rows[] = {
    {0, "Off", UI_SEVERITY_OFFLINE},
    {4, "Connected", UI_SEVERITY_OK},
};
static const ui_status_desc_t k_host = {.title = "Host", .rows = k_host_rows, .row_count = 3,
                                        .width = UI_TOPBAR_STATUS_W, .control_id = 0x0101,
                                        .clickable = true};
static const ui_status_desc_t k_client = {.title = "Client", .rows = k_client_rows, .row_count = 2,
                                          .width = UI_TOPBAR_STATUS_W, .control_id = 0x0102,
                                          .clickable = true};

static const ui_segment_t k_modes[] = {
    {"Mouse", LV_SYMBOL_GPS, 0x0201},
    {"Keyboard", LV_SYMBOL_KEYBOARD, 0x0202},
    {"Shortcuts", LV_SYMBOL_LIST, 0x0203},
};
static const ui_segmented_desc_t k_modes_desc = {.segments = k_modes, .count = 3,
                                                 .width = UI_TOPBAR_NAV_W, .height = UI_SEGMENTED_H};

static const ui_action_key_desc_t k_settings = {.icon = LV_SYMBOL_SETTINGS, .control_id = 0x0300,
                                                .variant = UI_KEY_NORMAL, .width = UI_TOPBAR_BTN_W,
                                                .height = UI_TOPBAR_BTN_W, .font = UI_FONT_BODY};
static const ui_action_key_desc_t k_footer[] = {
    {.label = "Left click", .control_id = 0x0411, .variant = UI_KEY_NORMAL, .height = 72,
     .font = UI_FONT_BODY},
    {.label = "Right click", .control_id = 0x0412, .variant = UI_KEY_NORMAL, .height = 72,
     .font = UI_FONT_BODY},
    {.label = "Drag lock", .icon = LV_SYMBOL_LOOP, .control_id = 0x0413, .variant = UI_KEY_MODIFIER,
     .checkable = true, .height = 72, .font = UI_FONT_BODY},
    {.label = "Release all", .icon = LV_SYMBOL_CLOSE, .control_id = 0x0414,
     .variant = UI_KEY_DESTRUCTIVE, .height = 72, .font = UI_FONT_BODY},
};

enum { SUBJ_HOST = 0, SUBJ_CLIENT, SUBJ_AWAKE, SUBJ_VIEW, SUBJ_COUNT };

typedef struct {
    lv_obj_t *root;
    lv_obj_t *bar;
    lv_obj_t *seg;
    lv_obj_t *pad;
    lv_obj_t *footer[4];
    lv_subject_t *subj[SUBJ_COUNT];
} shell_t;

static void shell_build(shell_t *sh)
{
    s_hit_count = 0;
    sh->root = ui_test_root();
    for (int i = 0; i < SUBJ_COUNT; i++) {
        sh->subj[i] = lv_subject_create(LV_SUBJECT_TYPE_INT);
        CHECK(sh->subj[i] != NULL);
        lv_subject_set_int(sh->subj[i], -1); // every projection starts unknown (07 §12 item 2)
    }

    static const ui_topbar_desc_t bar_desc = {
        .brand = "KVMpad",
        .status = {&k_host, &k_client},
        .keep_awake_text = "Keep awake",
        .keep_awake_icon = LV_SYMBOL_EYE_OPEN,
    };
    const ui_topbar_bind_t bind = {
        .status = {sh->subj[SUBJ_HOST], sh->subj[SUBJ_CLIENT]},
        .keep_awake = sh->subj[SUBJ_AWAKE],
    };
    sh->bar = ui_topbar_create(sh->root, &bar_desc, &bind);
    CHECK(sh->bar != NULL);
    sh->seg = ui_segmented_create(ui_topbar_nav_slot(sh->bar), &k_modes_desc, NULL);
    ui_segmented_bind_active(sh->seg, sh->subj[SUBJ_VIEW]);
    lv_obj_t *settings = ui_action_key_create(ui_topbar_trailing_slot(sh->bar), &k_settings, NULL);

    for (uint16_t i = 0; i < k_modes_desc.count; i++) {
        track(ui_segmented_tab(sh->seg, i));
    }
    track(ui_topbar_status_item(sh->bar, UI_TOPBAR_HOST));
    track(ui_topbar_status_item(sh->bar, UI_TOPBAR_CLIENT));
    track(settings);

    // Content area of 01 §2.1 scaled to 1024x600: one dominant PAD plus a footer row (FR-001).
    sh->pad = lv_obj_create(sh->root);
    lv_obj_remove_style_all(sh->pad);
    ui_theme_apply(sh->pad, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(sh->pad, lv_color_hex(UI_COLOR_WELL), LV_PART_MAIN);
    lv_obj_set_scrollable(sh->pad, false);
    lv_obj_set_pos(sh->pad, UI_CONTENT_INSET, UI_CONTENT_TOP + UI_SPACE_S);
    lv_obj_set_size(sh->pad, UI_SCREEN_W - 2 * UI_CONTENT_INSET,
                    UI_CONTENT_H - 3 * UI_SPACE_S - 72 - UI_SPACE_XL);
    lv_obj_t *hint = lv_label_create(sh->pad);
    ui_theme_apply(hint, UI_STYLE_TEXT_TITLE, LV_STATE_DEFAULT);
    lv_label_set_text(hint, "Trackpad");
    lv_obj_center(hint);

    lv_obj_t *footer = lv_obj_create(sh->root);
    lv_obj_remove_style_all(footer);
    lv_obj_set_scrollable(footer, false);
    lv_obj_set_pos(footer, UI_CONTENT_INSET, UI_SCREEN_H - 72 - UI_SPACE_XL);
    lv_obj_set_size(footer, UI_SCREEN_W - 2 * UI_CONTENT_INSET, 72);
    lv_obj_set_style_pad_gap(footer, UI_SPACE_M, LV_PART_MAIN);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    for (int i = 0; i < 4; i++) {
        sh->footer[i] = ui_action_key_create(footer, &k_footer[i], NULL);
        track(sh->footer[i]);
    }
    lv_obj_update_layout(sh->root);
}

static void shell_free(shell_t *sh)
{
    lv_obj_delete(sh->root);
    for (int i = 0; i < SUBJ_COUNT; i++) {
        lv_subject_delete(sh->subj[i]);
    }
}

static void test_shell_states(void)
{
    CHECK(ui_theme_init());
    shell_t sh;
    shell_build(&sh);

    // State 1: every projection unknown. Nothing may read as a working connection.
    lv_refr_now(NULL);
    check_geometry("unknown");
    CHECK(strcmp(lv_label_get_text(ui_status_item_value_label(
                     ui_topbar_status_item(sh.bar, UI_TOPBAR_HOST))), "Unknown") == 0);
    ui_test_snapshot(sh.root, "shell_unknown");

    // State 2: HOST active, CLIENT connected, Keep awake on, Mouse selected.
    lv_subject_set_int(sh.subj[SUBJ_HOST], 3);
    lv_subject_set_int(sh.subj[SUBJ_CLIENT], 4);
    lv_subject_set_int(sh.subj[SUBJ_AWAKE], 1);
    lv_subject_set_int(sh.subj[SUBJ_VIEW], 0);
    ui_action_key_set_checked(sh.footer[2], true);
    lv_obj_update_layout(sh.root);
    lv_refr_now(NULL);
    check_geometry("active");
    CHECK_EQ(ui_segmented_get_active(sh.seg), 0);
    CHECK(!lv_obj_is_hidden(ui_topbar_keep_awake(sh.bar)));
    ui_test_snapshot(sh.root, "shell_active");

    // State 3: HOST lost, CLIENT off, a disabled key and a toast (01 §2 FR-006).
    lv_subject_set_int(sh.subj[SUBJ_HOST], 1);
    lv_subject_set_int(sh.subj[SUBJ_CLIENT], 0);
    lv_subject_set_int(sh.subj[SUBJ_AWAKE], 0);
    lv_subject_set_int(sh.subj[SUBJ_VIEW], 2);
    ui_action_key_set_enabled(sh.footer[0], false);
    ui_action_key_set_enabled(sh.footer[1], false);
    lv_obj_t *toast = ui_toast_show(sh.root, "Host disconnected - input is not sent",
                                    UI_TOAST_NO_TIMEOUT);
    CHECK(toast != NULL);
    lv_obj_update_layout(sh.root);
    lv_refr_now(NULL);
    check_geometry("host_lost"); // the toast is not a hit box: it is never clickable
    CHECK_EQ(ui_segmented_get_active(sh.seg), 2);
    CHECK(lv_obj_is_hidden(ui_topbar_keep_awake(sh.bar)));
    ui_test_snapshot(sh.root, "shell_host_lost");

    // State 4: a modal over the shell.
    static const ui_modal_button_t buttons[] = {
        {"Not now", 0x0601, UI_BTN_SECONDARY, true},
        {"Send Alt+F4", 0x0602, UI_BTN_PRIMARY, true},
    };
    static const ui_modal_desc_t modal_desc = {
        .title = "Close the window on the host?",
        .body = "Alt+F4 goes to the computer connected over USB. The device cannot undo it.",
        .buttons = buttons,
        .button_count = 2,
        .overlay_id = 7,
    };
    lv_obj_t *modal = ui_modal_create(sh.root, &modal_desc, NULL);
    CHECK(modal != NULL);
    lv_obj_update_layout(sh.root);
    lv_refr_now(NULL);
    lv_area_t card;
    lv_obj_get_coords(ui_modal_card(modal), &card);
    CHECK(card.x1 >= 0 && card.y1 >= 0 && card.x2 < UI_SCREEN_W && card.y2 < UI_SCREEN_H);
    CHECK_EQ(card.x2 - card.x1 + 1, UI_MODAL_W);
    ui_test_snapshot(sh.root, "shell_modal");
    ui_modal_close(modal);
    lv_test_wait(20);

    ui_toast_dismiss(toast);
    shell_free(&sh);
    ui_theme_deinit();
}

int main(void)
{
    printf("test_ui_snapshot (LVGL %d.%d.%d, %dx%d RGB565)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH, UI_SCREEN_W, UI_SCREEN_H);
    const size_t base = ui_test_env_start();
    printf("  snapshots %s\n", ui_test_snapshots_enabled() ? "on" : "off (pass -DKVM_SNAPSHOT_DIR=...)");
    RUN(test_shell_states);
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
    printf("  live allocations after all tests: %zu (baseline %zu)\n", host_lv_live_count(), base);
    CHECK_EQ(host_lv_live_count(), base);
    ui_test_env_stop(base);
    TEST_MAIN_END("test_ui_snapshot");
}
