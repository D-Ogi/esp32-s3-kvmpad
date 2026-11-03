// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-060A benchmark scenes. Geometry follows reference/kvm-essential-800x480.html scaled to
// 1024x600 (ADR-026): top status bar, left navigation rail, the pad, bottom button bar.
#include "ui_bench.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "lvgl.h"
#include "ui_platform.h"
#if LV_USE_DEMO_BENCHMARK
#include "demos/lv_demos.h"
#endif

static const char *TAG = "ui_bench";

#define SCR_W 1024
#define SCR_H 600
#define TOP_H 56
#define RAIL_W 112
#define BOTTOM_H 72
#define DOT_D 28

static lv_obj_t *s_pad;
static lv_obj_t *s_dot;
static lv_obj_t *s_counter;
static lv_obj_t *s_list;
static lv_obj_t *s_screen;
static uint32_t s_frames;
static ui_bench_scene_t s_scene;
static bool s_animating;
static ui_bench_demo_result_t s_demo;
static lv_obj_t *s_touch_label;
static uint32_t s_clicks;
static const char *s_last_click;

static const char *const k_names[UI_BENCH_COUNT] = {"idle", "small", "full", "scroll"};

const char *ui_bench_scene_name(ui_bench_scene_t s)
{
    return s < UI_BENCH_COUNT ? k_names[s] : "?";
}

bool ui_bench_scene_from_name(const char *name, ui_bench_scene_t *out)
{
    for (int i = 0; i < UI_BENCH_COUNT; i++) {
        if (name != NULL && strcmp(name, k_names[i]) == 0) {
            *out = (ui_bench_scene_t)i;
            return true;
        }
    }
    return false;
}

static lv_obj_t *panel(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 8, LV_PART_MAIN);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    return l;
}

static void click_cb(lv_event_t *e)
{
    // LVGL task. `txt` points to a string literal of build_screen_locked(), valid for the whole run.
    const char *txt = (const char *)lv_event_get_user_data(e);
    s_clicks++;
    s_last_click = txt;
    if (s_touch_label != NULL) {
        lv_label_set_text_fmt(s_touch_label, LV_SYMBOL_OK " %s (%" PRIu32 ")", txt, s_clicks);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, int32_t w, int32_t h, uint32_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5B8CFF), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b, click_cb, LV_EVENT_CLICKED, (void *)txt);
    lv_obj_t *l = label(b, txt, &lv_font_montserrat_20, 0xF2F4F8);
    lv_obj_center(l);
    return b;
}

uint32_t ui_bench_clicks(char *last, size_t last_len)
{
    uint32_t n = 0;
    if (ui_platform_lock(1000)) {
        n = s_clicks;
        if (last != NULL && last_len > 0) {
            snprintf(last, last_len, "%s", s_last_click != NULL ? s_last_click : "-");
        }
        ui_platform_unlock();
    }
    return n;
}

// The static KVMpad-like screen (must be called under the LVGL lock).
static void build_screen_locked(void)
{
    lv_obj_t *old = s_screen;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x10141C), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(s_screen, false);

    // top status bar
    lv_obj_t *top = panel(s_screen, 0, 0, SCR_W, TOP_H, 0x1B2230);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 28, LV_PART_MAIN);
    lv_obj_set_style_pad_left(top, 20, LV_PART_MAIN);
    label(top, "KVMpad", &lv_font_montserrat_24, 0xF2F4F8);
    label(top, LV_SYMBOL_OK " HOST active", &lv_font_montserrat_16, 0x7BD88F);
    label(top, LV_SYMBOL_WIFI " CLIENT -", &lv_font_montserrat_16, 0x9AA3B2);
    label(top, LV_SYMBOL_USB " mounted", &lv_font_montserrat_16, 0x9AA3B2);
    label(top, "Layout: US", &lv_font_montserrat_16, 0x9AA3B2);
    s_counter = label(top, "frame 0", &lv_font_montserrat_16, 0xF2C14E);
    s_touch_label = label(top, "touch: tap a button", &lv_font_montserrat_16, 0x9AA3B2);

    // left navigation rail
    lv_obj_t *rail = panel(s_screen, 0, TOP_H, RAIL_W, SCR_H - TOP_H - BOTTOM_H, 0x161C27);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(rail, 12, LV_PART_MAIN);
    static const char *const nav[4] = {"Mouse", "Keys", "Macro", "Setup"};
    for (int i = 0; i < 4; i++) {
        button(rail, nav[i], RAIL_W - 16, 84, i == 0 ? 0x2F6FED : 0x24304A);
    }

    // the pad
    s_pad = panel(s_screen, RAIL_W + 16, TOP_H + 16, SCR_W - RAIL_W - 32, SCR_H - TOP_H - BOTTOM_H - 32, 0x242B38);
    lv_obj_set_style_radius(s_pad, 18, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pad, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pad, lv_color_hex(0x3A4660), LV_PART_MAIN);
    lv_obj_t *hint = label(s_pad, "Touchpad  |  1 finger: move/tap  |  2 fingers: scroll  |  long press: right click",
                           &lv_font_montserrat_14, 0x6F7A90);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    // bottom button bar
    lv_obj_t *bottom = panel(s_screen, 0, SCR_H - BOTTOM_H, SCR_W, BOTTOM_H, 0x1B2230);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    button(bottom, "LEFT", 240, 52, 0x24304A);
    button(bottom, "MIDDLE", 160, 52, 0x24304A);
    button(bottom, "RIGHT", 240, 52, 0x24304A);
    button(bottom, "Drag lock", 160, 52, 0x24304A);

    // the moving dot (hidden until a scene uses it)
    s_dot = lv_obj_create(s_pad);
    lv_obj_set_size(s_dot, DOT_D, DOT_D);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(0xF2C14E), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dot, 0, LV_PART_MAIN);
    lv_obj_set_hidden(s_dot, true);

    s_list = NULL;
    lv_screen_load(s_screen);
    if (old != NULL) {
        lv_obj_delete(old); // the previous scene: every widget, no leak between runs
    }
}

// Animation step: `v` runs 0..3599 (tenths of a degree) and repeats forever.
static void anim_step(void *var, int32_t v)
{
    (void)var;
    s_frames++;
    const int32_t pw = lv_obj_get_width(s_pad) - DOT_D - 16;
    const int32_t ph = lv_obj_get_height(s_pad) - DOT_D - 16;
    // Lissajous 3:2 inside the pad; lv_trigo_sin returns -32767..32767 for degrees.
    const int32_t sx = lv_trigo_sin((int16_t)((v * 3 / 10) % 360));
    const int32_t sy = lv_trigo_sin((int16_t)((v * 2 / 10 + 90) % 360));
    lv_obj_set_pos(s_dot, (pw / 2) + (sx * (pw / 2)) / 32767, (ph / 2) + (sy * (ph / 2)) / 32767);
    lv_label_set_text_fmt(s_counter, "frame %" PRIu32, s_frames);

    if (s_scene == UI_BENCH_FULL) {
        const uint16_t hue = (uint16_t)((v / 10) % 360);
        lv_obj_set_style_bg_color(s_pad, lv_color_hsv_to_rgb(hue, 55, 45), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_screen, lv_color_hsv_to_rgb((uint16_t)((hue + 180) % 360), 40, 18), LV_PART_MAIN);
    } else if (s_scene == UI_BENCH_SCROLL && s_list != NULL) {
        // triangle wave over the list height
        const int32_t span = 60 * 52 - lv_obj_get_height(s_list);
        int32_t t = (v % 1800);
        if (t > 900) {
            t = 1800 - t;
        }
        lv_obj_scroll_to_y(s_list, (span * t) / 900, LV_ANIM_OFF);
    }
}

esp_err_t ui_bench_start(ui_bench_scene_t scene)
{
    ESP_RETURN_ON_FALSE(scene < UI_BENCH_COUNT, ESP_ERR_INVALID_ARG, TAG, "scene");
    ESP_RETURN_ON_FALSE(ui_platform_lock(2000), ESP_ERR_TIMEOUT, TAG, "lock");
    if (s_animating) {
        lv_anim_delete(NULL, anim_step);
        s_animating = false;
    }
    build_screen_locked();
    s_scene = scene;
    s_frames = 0;
    if (scene == UI_BENCH_SCROLL) {
        // lv_list is deprecated in 9.6: a scrollable flex column of buttons (60 rows x 52 px)
        s_list = lv_obj_create(s_pad);
        lv_obj_set_size(s_list, lv_obj_get_width(s_pad) - 32, lv_obj_get_height(s_pad) - 48);
        lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_row(s_list, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_list, lv_color_hex(0x1B2230), LV_PART_MAIN);
        lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
        for (int i = 0; i < 60; i++) {
            char txt[48];
            lv_snprintf(txt, sizeof(txt), "%s  Shortcut %02d   Ctrl+Alt+%c", i % 3 == 0 ? LV_SYMBOL_KEYBOARD : LV_SYMBOL_RIGHT,
                        i + 1, 'A' + (i % 26));
            lv_obj_t *b = lv_button_create(s_list);
            lv_obj_set_size(b, LV_PCT(100), 52);
            lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(b, lv_color_hex(i % 2 ? 0x24304A : 0x1F2940), LV_PART_MAIN);
            lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
            lv_obj_t *l = label(b, txt, &lv_font_montserrat_16, 0xF2F4F8);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
        }
    }
    if (scene != UI_BENCH_IDLE) {
        lv_obj_set_hidden(s_dot, false);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_dot);
        lv_anim_set_exec_cb(&a, anim_step);
        lv_anim_set_values(&a, 0, 3599);
        lv_anim_set_duration(&a, 6000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        s_animating = true;
    }
    ui_platform_unlock();
    return ESP_OK;
}

void ui_bench_stop(void)
{
    if (!ui_platform_lock(2000)) {
        return;
    }
    if (s_animating) {
        lv_anim_delete(NULL, anim_step);
        s_animating = false;
    }
    ui_platform_unlock();
}

#if LV_USE_DEMO_BENCHMARK
static void demo_end(const lv_demo_benchmark_summary_t *s)
{
    // The summary holds sums over the valid scenes; the demo's own table divides by valid_scene_cnt.
    const int32_t n = s->valid_scene_cnt > 0 ? s->valid_scene_cnt : 1;
    s_demo.fps_avg = s->total_avg_fps / n;
    s_demo.cpu_avg = s->total_avg_cpu / n;
    s_demo.render_avg_ms = s->total_avg_render_time / n;
    s_demo.flush_avg_ms = s->total_avg_flush_time / n;
    s_demo.scenes = s->valid_scene_cnt;
    s_demo.done = true;
}
#endif

esp_err_t ui_bench_demo_start(void)
{
#if LV_USE_DEMO_BENCHMARK
    ESP_RETURN_ON_FALSE(ui_platform_lock(2000), ESP_ERR_TIMEOUT, TAG, "lock");
    if (s_animating) {
        lv_anim_delete(NULL, anim_step);
        s_animating = false;
    }
    memset(&s_demo, 0, sizeof(s_demo));
    // The demo builds on the active screen and may delete our widgets: forget them, so neither
    // ui_bench_pad_area() nor a late callback touches a deleted object (the screen itself stays ours).
    s_pad = NULL;
    s_dot = NULL;
    s_counter = NULL;
    s_list = NULL;
    s_touch_label = NULL;
    lv_demo_benchmark_set_end_cb(demo_end);
    lv_demo_benchmark();
    ui_platform_unlock();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void ui_bench_demo_result(ui_bench_demo_result_t *out)
{
    if (out != NULL) {
        *out = s_demo;
    }
}

bool ui_bench_pad_area(int16_t *x1, int16_t *y1, int16_t *x2, int16_t *y2)
{
    bool ok = false;
    if (ui_platform_lock(1000)) {
        if (s_pad != NULL) {
            lv_obj_update_layout(s_pad);
            lv_area_t a;
            lv_obj_get_coords(s_pad, &a);
            *x1 = (int16_t)a.x1;
            *y1 = (int16_t)a.y1;
            *x2 = (int16_t)a.x2;
            *y2 = (int16_t)a.y2;
            ok = true;
        }
        ui_platform_unlock();
    }
    return ok;
}
