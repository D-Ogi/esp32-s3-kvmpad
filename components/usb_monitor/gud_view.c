// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "gud_view.h"
#include "usb_monitor.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "ui_theme.h"
#include <string.h>

static lv_obj_t *screen, *previous, *image, *waiting, *blank, *back, *title;
static lv_timer_t *timer;
static uint8_t *indexed;
static lv_image_dsc_t descriptor;
static void (*go_home)(void);
static bool (*switch_link)(bool active);
enum { PALETTE_BYTES = 8 };

static void refresh(lv_timer_t *unused)
{
    (void)unused;
    gud_rect_t dirty;
    gud_view_state_t state;
    uint16_t width, height;
    if (!gud_surface_read(indexed + PALETTE_BYTES, &dirty, &state, &width, &height)) return;
    bool was_hidden = lv_obj_is_hidden(image);
    if (dirty.width) lv_image_cache_drop(&descriptor);
    if (descriptor.header.w != width || descriptor.header.h != height) {
        descriptor.header.w = width;
        descriptor.header.h = height;
        descriptor.header.stride = width / 8;
        descriptor.data_size = PALETTE_BYTES + width / 8u * height;
        lv_image_set_src(image, &descriptor);
        bool full_height = height == GUD_MAX_HEIGHT;
        lv_obj_set_hidden(title, full_height);
        lv_obj_set_width(back, full_height ? 88 : 116);
        lv_obj_set_size(blank, width, height);
        lv_obj_align(blank, LV_ALIGN_CENTER, 0, full_height ? 0 : 24);
        lv_obj_align(image, LV_ALIGN_CENTER, 0, full_height ? 0 : 24);
    }
    lv_obj_set_hidden(waiting, state != GUD_VIEW_WAITING);
    lv_obj_set_hidden(blank, state != GUD_VIEW_BLANK);
    lv_obj_set_hidden(image, state != GUD_VIEW_PICTURE);
    if (state == GUD_VIEW_PICTURE && !was_hidden && dirty.width) {
        lv_area_t at;
        lv_obj_get_coords(image, &at);
        lv_area_t damage = {at.x1 + dirty.x, at.y1 + dirty.y,
                            at.x1 + dirty.x + dirty.width - 1, at.y1 + dirty.y + dirty.height - 1};
        lv_obj_invalidate_area(image, &damage);
    }
}

static void leave(lv_event_t *event)
{
    (void)event;
    void (*home)(void) = go_home;
    usb_monitor_gud_close();
    home();
}

bool usb_monitor_gud_open(void (*home)(void), bool (*select)(bool))
{
    if (!home || !select || screen || usb_monitor_view_active() || !gud_surface_open()) return false;
    indexed = heap_caps_calloc(1, PALETTE_BYTES + GUD_FRAME_BYTES, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!indexed) goto failed;
    // LVGL I1 has two BGRA8888 palette entries followed by MSB-first rows.
    indexed[3] = 255;
    memset(indexed + 4, 255, 4);
    descriptor = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_I1, .w = GUD_WIDTH,
                   .h = GUD_HEIGHT, .stride = GUD_WIDTH / 8},
        .data_size = PALETTE_BYTES + GUD_R1_MAX_BYTES, .data = indexed};
    previous = lv_screen_active();
    go_home = home;
    screen = lv_obj_create(NULL);
    if (!screen) goto failed;
    lv_obj_set_scrollable(screen, false);
    ui_theme_apply(screen, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    back = lv_button_create(screen);
    if (!back) goto failed;
    lv_obj_set_size(back, 116, 54);
    lv_obj_set_pos(back, 12, 8);
    ui_theme_control_button(back);
    if (!lv_obj_add_event_cb(back, leave, LV_EVENT_CLICKED, NULL)) goto failed;
    lv_obj_t *label = lv_label_create(back);
    if (!label) goto failed;
    lv_label_set_text_static(label, "Home");
    lv_obj_center(label);
    title = lv_label_create(screen);
    if (!title) goto failed;
    lv_label_set_text_static(title, "USB monitor  /  Monochrome");
    lv_obj_set_style_text_font(title, ui_theme_font(UI_FONT_TITLE), 0);
    lv_obj_set_pos(title, 148, 16);
    waiting = lv_label_create(screen);
    if (!waiting) goto failed;
    lv_label_set_text_static(waiting, "Waiting for the computer to send a picture");
    lv_obj_center(waiting);
    blank = lv_obj_create(screen);
    if (!blank) goto failed;
    lv_obj_remove_style_all(blank);
    lv_obj_set_clickable(blank, false);
    lv_obj_set_size(blank, GUD_WIDTH, GUD_HEIGHT);
    lv_obj_align(blank, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_bg_color(blank, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
    lv_obj_set_hidden(blank, true);
    image = lv_image_create(screen);
    if (!image) goto failed;
    lv_image_set_src(image, &descriptor);
    lv_obj_align(image, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_hidden(image, true);
    timer = lv_timer_create(refresh, 16, NULL);
    if (!timer || !select(true)) goto failed;
    switch_link = select;
    lv_screen_load(screen);
    return true;
failed:
    usb_monitor_gud_close();
    return false;
}

bool usb_monitor_gud_active(void)
{
    return screen != NULL;
}

void usb_monitor_gud_close(void)
{
    if (switch_link) switch_link(false);
    switch_link = NULL;
    if (timer) lv_timer_delete(timer);
    timer = NULL;
    if (image) lv_image_set_src(image, NULL);
    if (indexed) lv_image_cache_drop(&descriptor);
    if (screen) {
        if (lv_screen_active() == screen) {
            lv_screen_load(previous);
            lv_obj_delete_async(screen);
        } else lv_obj_delete(screen);
    }
    screen = previous = image = waiting = blank = back = title = NULL;
    go_home = NULL;
    heap_caps_free(indexed);
    indexed = NULL;
    descriptor.data = NULL;
    gud_surface_close();
}
