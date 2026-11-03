// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shared environment of the TASK-060D host suites. The display is the headless one LVGL ships with
// its own tests, switched to RGB565 and 1024x600 so the geometry and the pixel format match the
// target panel (ADR-026 TECH-087). The input devices are LVGL's test indevs, which is how a press
// is delivered through the real hit-test path instead of a hand-rolled read callback.
#include "ui_test_env.h"

#include <stdio.h>
#include <string.h>

#include "host_lv_mem.h"
#include "ui_theme.h"
#include "ui_tokens.h"

// Every printable ASCII glyph plus the symbols the components use. LVGL allocates a draw-data
// cache entry the first time it rasterizes a glyph at a given size, and that entry is long lived;
// rendering the whole set once per font moves those allocations out of the measured region.
#define WARM_TEXT                                                                                 \
    " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"                         \
    "abcdefghijklmnopqrstuvwxyz{|}~" LV_SYMBOL_OK LV_SYMBOL_WARNING LV_SYMBOL_CLOSE                \
    LV_SYMBOL_MINUS LV_SYMBOL_PLUS LV_SYMBOL_EYE_OPEN LV_SYMBOL_SETTINGS LV_SYMBOL_KEYBOARD        \
    LV_SYMBOL_GPS LV_SYMBOL_LIST LV_SYMBOL_LOOP LV_SYMBOL_TRASH LV_SYMBOL_USB LV_SYMBOL_WIFI

static lv_display_t *s_disp;

size_t ui_test_env_start(void)
{
    lv_init();
    s_disp = lv_test_display_create(UI_SCREEN_W, UI_SCREEN_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_test_indev_create_all();
    lv_refr_now(NULL);
    // The first render with a given font creates long-lived renderer state (glyph cache, draw unit
    // buffers). Warm every typography role up with the glyphs and symbols the components use, so a
    // later create/delete cycle can return exactly to the baseline.
    for (int role = 0; role < UI_FONT_COUNT; role++) {
        lv_obj_t *warm = lv_label_create(lv_screen_active());
        lv_obj_set_style_text_font(warm, ui_theme_font((ui_font_role_t)role), LV_PART_MAIN);
        lv_label_set_text(warm, WARM_TEXT);
        lv_refr_now(NULL);
        lv_obj_delete(warm);
    }
    lv_refr_now(NULL);
    return host_lv_live_count();
}

void ui_test_env_stop(size_t baseline)
{
    (void)baseline;
    lv_test_indev_delete_all();
    lv_display_delete(s_disp);
    s_disp = NULL;
    lv_deinit();
}

lv_obj_t *ui_test_root(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_clickable(root, false);
    // A local style, not a shared one: the root must survive ui_theme_deinit() in the theme suite.
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    return root;
}

bool ui_test_center_of(lv_obj_t *obj, int32_t *x, int32_t *y)
{
    if (!obj) {
        return false;
    }
    lv_obj_update_layout(obj);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    if (a.x2 < a.x1 || a.y2 < a.y1) {
        return false;
    }
    *x = a.x1 + (a.x2 - a.x1) / 2;
    *y = a.y1 + (a.y2 - a.y1) / 2;
    return true;
}

bool ui_test_snapshots_enabled(void)
{
#ifdef KVM_SNAPSHOT_DIR
    return true;
#else
    return false;
#endif
}

void ui_test_snapshot(lv_obj_t *obj, const char *name)
{
#ifdef KVM_SNAPSHOT_DIR
    if (!obj || !name) {
        return;
    }
    lv_obj_update_layout(obj);
    lv_refr_now(NULL);
    lv_draw_buf_t *buf = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        printf("    snapshot %s: lv_snapshot_take failed\n", name);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", KVM_SNAPSHOT_DIR, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("    snapshot %s: cannot write %s\n", name, path);
        lv_draw_buf_destroy(buf);
        return;
    }
    const uint32_t w = buf->header.w;
    const uint32_t h = buf->header.h;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *src = buf->data + (size_t)row * buf->header.stride;
        for (uint32_t col = 0; col < w; col++) {
            const uint16_t px = (uint16_t)(src[col * 2] | (uint16_t)(src[col * 2 + 1] << 8));
            const uint8_t rgb[3] = {
                (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u),
                (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u),
                (uint8_t)((px & 0x1Fu) * 255u / 31u),
            };
            fwrite(rgb, 1, sizeof(rgb), f);
        }
    }
    fclose(f);
    lv_draw_buf_destroy(buf);
    printf("    snapshot %s -> %s (%ux%u RGB565)\n", name, path, w, h);
#else
    (void)obj;
    (void)name;
#endif
}
