// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_theme (TASK-060D) against the real LVGL 9.6:
// - the token set honors the absolute floors of 07 §7 (spacing 4/8/12/16, touch 44/48) and the
//   1024x600 geometry of ADR-026 TECH-087;
// - every typography role resolves to a Montserrat size the firmware actually compiles in;
// - init/deinit twice leaks no LVGL allocation and the second init does not reset a live style;
// - severity carries a distinct color AND a distinct glyph (FR-005: never color alone);
// - the topbar width budget of ui_tokens.h actually fits inside the panel.
#include <string.h>

#include "host_lv_mem.h"
#include "kvm_test.h"
#include "lvgl.h"
#include "ui_test_env.h"
#include "ui_theme.h"
#include "ui_tokens.h"

static void test_token_floors(void)
{
    // 07 §7: spacing 4/8/12/16 verbatim.
    CHECK_EQ(UI_SPACE_XS, 4);
    CHECK_EQ(UI_SPACE_S, 8);
    CHECK_EQ(UI_SPACE_M, 12);
    CHECK_EQ(UI_SPACE_L, 16);
    // 07 §7 and FR-003: touch-min 44, preferred >= 48. Never scaled.
    CHECK_EQ(UI_TOUCH_MIN, 44);
    CHECK(UI_TOUCH_PREFERRED >= 48);
    CHECK(UI_TOUCH_COMFORT >= UI_TOUCH_PREFERRED);
    CHECK(UI_STATUS_ITEM_H >= UI_TOUCH_MIN);
    CHECK(UI_MODAL_BTN_H >= UI_TOUCH_PREFERRED);
    // ADR-026 TECH-087 geometry.
    CHECK_EQ(UI_SCREEN_W, 1024);
    CHECK_EQ(UI_SCREEN_H, 600);
    CHECK_EQ(UI_CONTENT_H, UI_SCREEN_H - UI_TOPBAR_H);
    CHECK(UI_TOPBAR_H > 65);  // scaled up from the 800x480 reference, not copied
    CHECK(UI_TOPBAR_H < 100); // FR-001: the work area stays dominant
    // Radii and dividers are ordered and non-degenerate.
    CHECK(UI_RADIUS_S < UI_RADIUS_M);
    CHECK(UI_RADIUS_M < UI_RADIUS_L);
    CHECK(UI_RADIUS_L < UI_RADIUS_XL);
    CHECK_EQ(UI_DIVIDER_W, 1);
    // A tab inside the nav slot still clears the touch minimum.
    CHECK((UI_TOPBAR_NAV_W - 2 * UI_SEGMENTED_PAD) / 3 >= UI_TOUCH_MIN);
    CHECK(UI_SEGMENTED_H - 2 * UI_SEGMENTED_PAD >= UI_TOUCH_PREFERRED);
}

// 07 §7: the topbar must not grow with the length of the IP/SSID, so its slots are fixed. Prove the
// fixed budget fits the panel with room left for the spacer.
static void test_topbar_budget_fits(void)
{
    const int inner = UI_SCREEN_W - 2 * UI_TOPBAR_PAD;
    const int fixed = UI_TOPBAR_BRAND_W + UI_TOPBAR_NAV_W + UI_TOPBAR_KEEP_AWAKE_W +
                      2 * UI_TOPBAR_STATUS_W + UI_TOPBAR_BTN_W; // 07 §7: two statuses
    const int gaps = 6 * UI_TOPBAR_GAP; // 7 slots including the spacer
    printf("    topbar budget: inner %d, fixed %d, gaps %d, spacer %d\n", inner, fixed, gaps,
           inner - fixed - gaps);
    CHECK(fixed + gaps <= inner);
    CHECK(inner - fixed - gaps >= UI_SPACE_S);
}

static void test_fonts_are_compiled_in(void)
{
    const lv_font_t *seen[UI_FONT_COUNT];
    for (int role = 0; role < UI_FONT_COUNT; role++) {
        const lv_font_t *f = ui_theme_font((ui_font_role_t)role);
        CHECK(f != NULL);
        CHECK(f->line_height > 0);
        seen[role] = f;
    }
    // The four roles map to four distinct sizes, ordered from the largest.
    CHECK(seen[UI_FONT_TITLE]->line_height > seen[UI_FONT_HEADING]->line_height);
    CHECK(seen[UI_FONT_HEADING]->line_height > seen[UI_FONT_BODY]->line_height);
    CHECK(seen[UI_FONT_BODY]->line_height > seen[UI_FONT_LABEL]->line_height);
    CHECK(seen[UI_FONT_TITLE] == &lv_font_montserrat_24);
    CHECK(seen[UI_FONT_HEADING] == &lv_font_montserrat_20);
    CHECK(seen[UI_FONT_BODY] == &lv_font_montserrat_16);
    CHECK(seen[UI_FONT_LABEL] == &lv_font_montserrat_14);
    // An out-of-range role falls back, it does not return NULL.
    CHECK(ui_theme_font((ui_font_role_t)99) == seen[UI_FONT_BODY]);
}

static void test_severity_is_not_color_alone(void)
{
    CHECK(ui_theme_init());
    for (int a = 0; a < UI_SEVERITY_COUNT; a++) {
        const char *icon_a = ui_theme_severity_icon((ui_severity_t)a);
        CHECK(icon_a != NULL && icon_a[0] != '\0');
        for (int b = a + 1; b < UI_SEVERITY_COUNT; b++) {
            // FR-005: a distinct glyph for every severity, so color is never the only signal.
            CHECK(strcmp(icon_a, ui_theme_severity_icon((ui_severity_t)b)) != 0);
            const lv_color_t ca = ui_theme_severity_color((ui_severity_t)a);
            const lv_color_t cb = ui_theme_severity_color((ui_severity_t)b);
            CHECK(ca.red != cb.red || ca.green != cb.green || ca.blue != cb.blue);
        }
    }
    // An out-of-range severity reads as unknown, never as ok.
    const lv_color_t bad = ui_theme_severity_color((ui_severity_t)42);
    const lv_color_t unknown = ui_theme_severity_color(UI_SEVERITY_UNKNOWN);
    CHECK(bad.red == unknown.red && bad.green == unknown.green && bad.blue == unknown.blue);
    CHECK(strcmp(ui_theme_severity_icon((ui_severity_t)42), ui_theme_severity_icon(UI_SEVERITY_UNKNOWN)) == 0);
    ui_theme_deinit();
}

static void test_init_deinit_twice(void)
{
    const size_t base = host_lv_live_count();
    for (int round = 0; round < 2; round++) {
        CHECK(!ui_theme_is_ready());
        CHECK(ui_theme_style(UI_STYLE_CARD) == NULL);
        CHECK(ui_theme_init());
        CHECK(ui_theme_is_ready());
        CHECK(ui_theme_init()); // idempotent
        for (int id = 0; id < UI_STYLE_COUNT; id++) {
            CHECK(ui_theme_style((ui_style_id_t)id) != NULL);
        }
        CHECK(ui_theme_style(UI_STYLE_COUNT) == NULL);
        CHECK(ui_theme_style((ui_style_id_t)-1) == NULL);
        ui_theme_deinit();
        ui_theme_deinit(); // idempotent
    }
    CHECK_EQ(host_lv_live_count(), base);
}

// A style the theme hands out must stay valid for the whole life of the widget that uses it, and a
// second ui_theme_init() must not reset it (07 §7: created once at start-up).
static void test_styles_survive_reinit_and_widgets(void)
{
    const size_t base = host_lv_live_count();
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_remove_style_all(card);
    ui_theme_apply(card, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_set_size(card, 300, 120);
    lv_refr_now(NULL);
    const lv_color_t before = lv_obj_get_style_bg_color(card, LV_PART_MAIN);
    CHECK(ui_theme_init());
    lv_refr_now(NULL);
    const lv_color_t after = lv_obj_get_style_bg_color(card, LV_PART_MAIN);
    CHECK(before.red == after.red && before.green == after.green && before.blue == after.blue);
    CHECK_EQ(after.red, 0xFF); // UI_COLOR_SURFACE
    // ui_theme_apply with an unavailable id must be a silent no-op, not a crash.
    ui_theme_apply(card, UI_STYLE_COUNT, LV_STATE_DEFAULT);
    ui_theme_apply(NULL, UI_STYLE_CARD, LV_STATE_DEFAULT);
    lv_obj_delete(root);
    ui_theme_deinit();
    CHECK_EQ(host_lv_live_count(), base);
}

// ui_theme_apply_button/key must install the base, the variant and all three state styles, for
// every variant and for an out-of-range one.
static void test_variant_application(void)
{
    const size_t base = host_lv_live_count();
    CHECK(ui_theme_init());
    lv_obj_t *root = ui_test_root();
    for (int v = 0; v <= UI_BTN_VARIANT_COUNT; v++) {
        lv_obj_t *btn = lv_button_create(root);
        lv_obj_remove_style_all(btn);
        ui_theme_apply_button(btn, (ui_btn_variant_t)v);
        lv_obj_set_size(btn, 160, UI_TOUCH_PREFERRED);
        lv_obj_update_layout(btn);
        CHECK(lv_obj_get_style_min_height(btn, LV_PART_MAIN) >= UI_TOUCH_PREFERRED);
        lv_obj_set_disabled(btn, true);
        // FR-604: a disabled control is dimmed by its ink and its surface, never by a blanket opacity that
        // would wash the legend out of the page.
        CHECK_EQ(lv_obj_get_style_opa(btn, LV_PART_MAIN), LV_OPA_COVER);
        CHECK_EQ(lv_obj_get_style_text_color(btn, LV_PART_MAIN).red, (UI_COLOR_DISABLED_INK >> 16) & 0xFFu);
        CHECK_EQ(lv_obj_get_style_bg_color(btn, LV_PART_MAIN).red, (UI_COLOR_DISABLED_SURFACE >> 16) & 0xFFu);
        lv_obj_set_disabled(btn, false);
        lv_obj_set_checked(btn, true);
        CHECK_EQ(lv_obj_get_style_bg_color(btn, LV_PART_MAIN).blue, 0xFF); // UI_COLOR_CHECKED_SURFACE
    }
    for (int v = 0; v <= UI_KEY_VARIANT_COUNT; v++) {
        lv_obj_t *key = lv_button_create(root);
        lv_obj_remove_style_all(key);
        ui_theme_apply_key(key, (ui_key_variant_t)v);
        lv_obj_set_size(key, 80, 80);
        lv_obj_update_layout(key);
        CHECK(lv_obj_get_style_radius(key, LV_PART_MAIN) == UI_RADIUS_M);
    }
    lv_refr_now(NULL);
    lv_obj_delete(root);
    ui_theme_deinit();
    CHECK_EQ(host_lv_live_count(), base);
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
    printf("test_ui_theme (LVGL %d.%d.%d, %dx%d RGB565)\n", LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR,
           LVGL_VERSION_PATCH, UI_SCREEN_W, UI_SCREEN_H);
    const size_t base = ui_test_env_start();
    printf("  live allocations baseline: %zu\n", base);
    RUN(test_token_floors);
    RUN(test_topbar_budget_fits);
    RUN(test_fonts_are_compiled_in);
    RUN_MEM(test_severity_is_not_color_alone);
    RUN_MEM(test_init_deinit_twice);
    RUN_MEM(test_styles_survive_reinit_and_widgets);
    RUN_MEM(test_variant_application);
    lv_obj_clean(lv_screen_active());
    lv_refr_now(NULL);
    printf("  live allocations after all tests: %zu (baseline %zu)\n", host_lv_live_count(), base);
    CHECK_EQ(host_lv_live_count(), base);
    ui_test_env_stop(base);
    TEST_MAIN_END("test_ui_theme");
}
