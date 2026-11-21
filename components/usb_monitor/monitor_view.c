// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "monitor_present.h"
#include "monitor_wire.h"
#include "kvm_control_icons.h"
#include "ui_theme.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"

static lv_obj_t *screen, *previous, *image, *status, *guide_card, *instructions, *black_hint;
static lv_obj_t *home_button, *native_hint;
static bool native_mode, home_revealed;
static uint32_t reveal_tick;
static lv_timer_t *timer;
static lv_image_dsc_t descriptor;
static usb_monitor_redraw_t redraw;
static uint32_t redraw_started;
static bool split_redraw;
static bool refresh_pending, flushed, selected_nonblack, timing_black, link_mounted, link_suspended, decode_failed;
static uint32_t first_black_tick;
#ifdef KVM_USB_MOTION_CHECK
#include "esp_timer.h"
#include "src/misc/lv_timer_private.h"
static bool merge_test_regions;
static bool wait_period_for_test;
static int64_t selected_us, refresh_started_us;
static unsigned wait_us, draw_us;
void usb_monitor_view_merge_test_regions(bool merge) { merge_test_regions = merge; }
void usb_monitor_view_test_wait_period(bool wait) { wait_period_for_test = wait; }
unsigned usb_monitor_view_test_poll(bool slow)
{
    if (slow) lv_timer_set_period(timer, 25);
    return timer->period;
}
void usb_monitor_view_test_refresh_times(unsigned *wait, unsigned *draw)
{
    *wait = wait_us;
    *draw = draw_us;
}
#endif
static enum { WAIT_FIRST_PICTURE, SHOWING_DESKTOP, SHOWING_BLACK, VIDEO_STOPPED } picture_state;
static void (*return_home)(void);
static bool (*switch_usb)(bool);

static void update_captions(void)
{
    bool picture = picture_state == SHOWING_DESKTOP || picture_state == SHOWING_BLACK;
    const char *caption;
    if (link_suspended)
        caption = picture ? "USB suspended  /  Last picture held" : "USB suspended";
    else if (decode_failed)
        caption = picture ? "USB picture error  /  Last picture held" : "USB picture could not be decoded";
    else if (picture_state == SHOWING_BLACK)
        caption = "Last USB picture is black";
    else if (picture)
        caption = native_mode ? "Windows desktop  /  1024 x 600" : "Windows desktop  /  640 x 480";
    else if (link_mounted)
        caption = "USB connected  /  Waiting for a picture";
    else
        caption = picture_state == VIDEO_STOPPED ? "USB video stopped" : "Waiting for the first picture";
    const char *guide = link_suspended ? "Windows has paused the USB link.\nVideo resumes when the link wakes."
                      : decode_failed ? "Waiting for a new picture from Windows.\nTry moving a window on the KVMpad display."
                      : link_mounted ? "In Windows, press Win+P and choose Extend.\nMove a window onto the KVMpad display."
                      : "Connect the PC to the USB data port.\nIn Windows, press Win+P and choose Extend.";
    if (lv_label_get_text(status) != caption) lv_label_set_text_static(status, caption);
    if (lv_label_get_text(instructions) != guide) lv_label_set_text_static(instructions, guide);
    lv_obj_set_hidden(black_hint, picture_state != SHOWING_BLACK || link_suspended || decode_failed);
}

void usb_monitor_view_set_link(bool mounted, bool suspended)
{
    if (!screen || (link_mounted == mounted && link_suspended == suspended)) return;
    link_mounted = mounted;
    link_suspended = suspended;
    update_captions();
}

bool usb_monitor_view_active(void)
{
    return screen != NULL;
}

bool usb_monitor_view_has_link(bool (*set_monitor)(bool))
{
    return screen && lv_screen_active() == screen && switch_usb == set_monitor;
}

static bool picture_is_black(const uint16_t *pixels)
{
    for (size_t i = 0; i < descriptor.data_size / sizeof(*pixels); ++i)
        if (pixels[i] != 0) return false;
    return true;
}

const char *usb_monitor_view_decoded_picture(void)
{
    if (!screen || lv_screen_active() != screen || lv_obj_is_hidden(image))
        return "absent";

    // The worker cannot reuse this surface until the LVGL owner selects another one.
    return picture_is_black((const uint16_t *)descriptor.data) ? "all_black" : "has_nonblack";
}

#ifdef KVM_USB_PICTURE_CHECK
usb_monitor_picture_t usb_monitor_view_picture_check(void)
{
    if (!screen || lv_screen_active() != screen || lv_obj_is_hidden(image))
        return (usb_monitor_picture_t){"absent", "absent", 0};
    return monitor_checked_picture((const uint16_t *)descriptor.data, usb_monitor_view_decoded_picture());
}
#endif

usb_monitor_redraw_t usb_monitor_view_redraw(void)
{
    return redraw;
}

static void select_frame(const uint16_t *pixels, bool has_nonblack, const monitor_damage_t *damage, unsigned count)
{
    flushed = false;
    selected_nonblack = has_nonblack;
    lv_image_cache_drop(&descriptor);
    if (!pixels) {
        refresh_pending = false;
        home_revealed = false;
        lv_obj_set_hidden(home_button, false);
        timing_black = false;
        lv_image_set_src(image, NULL);
        lv_obj_set_hidden(image, true);
        lv_obj_set_hidden(guide_card, false);
        if (picture_state == SHOWING_DESKTOP || picture_state == SHOWING_BLACK) picture_state = VIDEO_STOPPED;
        update_captions();
        return;
    }

    // A VGA buffer can change address even when every pixel matches. The image
    // keeps this descriptor until reset, and the worker may now reuse the old buffer.
    descriptor.data = (const uint8_t *)pixels;
    const unsigned width = descriptor.header.w, height = descriptor.header.h;
    const bool first = lv_obj_is_hidden(image);
    if (first) {
        lv_image_set_src(image, &descriptor);
        lv_obj_set_hidden(image, false);
        lv_obj_set_hidden(guide_card, true);
        if (native_mode && !home_revealed) lv_obj_set_hidden(home_button, true);
    }
    if (!first && damage && !count) {
        ++redraw.unchanged;
        redraw.last_pixels = 0;
        return;
    }
    refresh_pending = true;
#ifdef KVM_USB_MOTION_CHECK
    selected_us = esp_timer_get_time();
    refresh_started_us = 0;
    monitor_damage_t box;
    if (damage && count == 2 && merge_test_regions) {
        box = (monitor_damage_t){LV_MIN(damage[0].x1, damage[1].x1), LV_MIN(damage[0].y1, damage[1].y1),
                                 LV_MAX(damage[0].x2, damage[1].x2), LV_MAX(damage[0].y2, damage[1].y2)};
        damage = &box;
        count = 1;
    }
#endif
    split_redraw = !first && damage && count == 2;
    redraw_started = lv_tick_get();
    if (first || !damage || (count == 1 && damage->x1 == 0 && damage->y1 == 0 &&
        damage->x2 == (int)width - 1 && damage->y2 == (int)height - 1)) {
        redraw.last_pixels = width * height;
        lv_obj_invalidate(image);
        return;
    }
    lv_area_t at;
    lv_obj_get_coords(image, &at);
    redraw.last_pixels = 0;
    for (unsigned i = 0; i < count; ++i) {
        const lv_area_t changed = {at.x1 + damage[i].x1, at.y1 + damage[i].y1,
                                   at.x1 + damage[i].x2, at.y1 + damage[i].y2};
        lv_obj_invalidate_area(image, &changed);
        redraw.last_pixels += (unsigned)lv_area_get_size(&changed);
    }
}

static void refreshed(lv_event_t *event)
{
    if (!refresh_pending || lv_screen_active() != screen) return;
#ifdef KVM_USB_MOTION_CHECK
    if (lv_event_get_code(event) == LV_EVENT_REFR_START) {
        if (!refresh_started_us) refresh_started_us = esp_timer_get_time();
        return;
    }
#endif
    if (lv_event_get_code(event) == LV_EVENT_FLUSH_FINISH) flushed = lv_display_flush_is_last(lv_event_get_target(event));
    else if (flushed) {
#ifdef KVM_USB_MOTION_CHECK
        wait_us = refresh_started_us ? (unsigned)(refresh_started_us - selected_us) : 0;
        draw_us = refresh_started_us ? (unsigned)(esp_timer_get_time() - refresh_started_us) : 0;
#endif
        uint32_t ms = lv_tick_elaps(redraw_started);
        if (split_redraw) ++redraw.split;
        if (redraw.last_pixels == (unsigned)descriptor.header.w * descriptor.header.h) {
            ++redraw.full;
            redraw.full_ms += ms;
        } else {
            ++redraw.partial;
            redraw.partial_pixels += redraw.last_pixels;
            redraw.partial_ms += ms;
        }
        monitor_refresh_complete();
        refresh_pending = false;
        if (selected_nonblack) {
            timing_black = false;
        } else if (!timing_black && picture_state != SHOWING_BLACK) {
            first_black_tick = lv_tick_get();
            timing_black = true;
        }
        if (picture_state == WAIT_FIRST_PICTURE || picture_state == VIDEO_STOPPED ||
            (picture_state == SHOWING_BLACK && selected_nonblack)) {
            picture_state = SHOWING_DESKTOP;
            update_captions();
        }
    }
}

static void poll(lv_timer_t *unused)
{
    (void)unused;
    // Finish a selected repaint before accepting damage relative to that picture.
    if (!refresh_pending) {
        monitor_present(select_frame);
        // The USB receiver lock has been released. Only a newly selected,
        // changed picture needs the display timer to run at the next opportunity.
        bool ready = refresh_pending;
#ifdef KVM_USB_MOTION_CHECK
        ready = ready && !wait_period_for_test;
#endif
        if (ready) {
            lv_timer_t *refresh = lv_display_get_refr_timer(lv_obj_get_display(image));
            if (refresh) lv_timer_ready(refresh);
        }
    }
    if (home_revealed && lv_tick_elaps(reveal_tick) >= 3000 && !lv_obj_has_state(home_button, LV_STATE_PRESSED)) {
        home_revealed = false;
        lv_obj_set_hidden(home_button, true);
    }
    bool failed = monitor_last_decode_failed();
    if (decode_failed != failed) {
        decode_failed = failed;
        update_captions();
    }
    if (timing_black && lv_tick_elaps(first_black_tick) >= 1000 && lv_screen_active() == screen) {
        timing_black = false;
        picture_state = SHOWING_BLACK;
        update_captions();
    }
}

static void show_home(lv_event_t *event)
{
    (void)event;
    reveal_tick = lv_tick_get();
    home_revealed = true;
    lv_obj_set_hidden(home_button, false);
}

static void home_clicked(lv_event_t *event)
{
    (void)event;
    void (*home)(void) = return_home;
    usb_monitor_view_close();
    home();
}

static void prepare_mode(bool native)
{
    native_mode = native;
    const unsigned width = monitor_width(native), height = monitor_height(native);
    descriptor = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .w = (uint16_t)width, .h = (uint16_t)height,
                   .cf = LV_COLOR_FORMAT_RGB565, .stride = (uint16_t)(width * 2)},
        .data_size = width * height * 2};
    redraw = (usb_monitor_redraw_t){0};
    picture_state = WAIT_FIRST_PICTURE;
    home_revealed = timing_black = refresh_pending = flushed = false;
    link_mounted = link_suspended = decode_failed = false;
    lv_obj_set_hidden(home_button, false);
    lv_obj_set_hidden(native_hint, !native);
    lv_obj_set_hidden(image, true);
    lv_obj_set_hidden(guide_card, false);
    lv_obj_align(image, LV_ALIGN_CENTER, 0, native ? 0 : 24);
    lv_obj_set_clickable(image, native);
    lv_obj_move_to_index(home_button, native ? -1 : 0);
    update_captions();
}

bool usb_monitor_view_change_mode(bool native, bool (*set_profile)(bool))
{
    if (!screen || lv_screen_active() != screen || !set_profile) return false;
    if (native == native_mode) return true;
    lv_image_set_src(image, NULL);
    lv_image_cache_drop(&descriptor);
    descriptor.data = NULL;
    usb_monitor_mode_result_t result = monitor_change_receiver(native, set_profile);
    if (result == USB_MONITOR_MODE_STOPPED) usb_monitor_view_close();
    else prepare_mode(result == USB_MONITOR_MODE_CHANGED ? native : !native);
    return result == USB_MONITOR_MODE_CHANGED;
}

bool usb_monitor_view_open(void (*home)(void), bool (*set_monitor)(bool))
{
    return usb_monitor_view_open_mode(home, set_monitor, false);
}

bool usb_monitor_view_open_mode(void (*home)(void), bool (*set_monitor)(bool), bool native)
{
    if (!home || !set_monitor || screen || !monitor_start(native)) return false;
    previous = lv_screen_active();
    return_home = home;
    screen = lv_obj_create(NULL);
    if (!screen) {
        previous = NULL;
        return_home = NULL;
        monitor_stop();
        return false;
    }
    lv_obj_set_scrollable(screen, false);
    ui_theme_apply(screen, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    lv_obj_t *button = home_button = lv_button_create(screen);
    if (!button) goto failed;
    lv_obj_set_pos(button, 12, 8);
    lv_obj_set_size(button, 112, 52);
    ui_theme_control_button(button);
    lv_obj_t *mask = ui_theme_control_mask(button, &kvm_icon_home);
    if (!mask) goto failed;
    lv_obj_align(mask, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *label = lv_label_create(button);
    if (!label) goto failed;
    lv_label_set_text_static(label, "Home");
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);
    if (!lv_obj_add_event_cb(button, home_clicked, LV_EVENT_CLICKED, NULL)) goto failed;
    label = lv_label_create(screen);
    if (!label) goto failed;
    lv_obj_set_style_text_font(label, ui_theme_font(UI_FONT_TITLE), 0);
    lv_obj_set_pos(label, 146, 18);
    lv_label_set_text_static(label, "USB monitor");
    status = lv_label_create(screen);
    if (!status) goto failed;
    lv_obj_set_style_text_font(status, ui_theme_font(UI_FONT_BODY), 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -24, 24);
    guide_card = lv_obj_create(screen);
    if (!guide_card) goto failed;
    ui_theme_apply(guide_card, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    ui_theme_apply(guide_card, UI_STYLE_LIBRARY_TILE, LV_STATE_DEFAULT);
    lv_obj_set_size(guide_card, 700, 144);
    lv_obj_align(guide_card, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_pad_all(guide_card, 24, 0);
    lv_obj_set_scrollable(guide_card, false);
    lv_obj_set_clickable(guide_card, false);
    lv_obj_t *icon = lv_image_create(guide_card);
    if (!icon) goto failed;
    lv_image_set_src(icon, &kvm_icon_host);
    ui_theme_apply(icon, UI_STYLE_CONTROL_MASK, LV_STATE_DEFAULT);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);
    instructions = lv_label_create(guide_card);
    if (!instructions) goto failed;
    lv_obj_set_style_text_font(instructions, ui_theme_font(UI_FONT_HEADING), 0);
    lv_obj_align(instructions, LV_ALIGN_LEFT_MID, 56, 0);
    black_hint = lv_label_create(screen);
    if (!black_hint) goto failed;
    lv_label_set_text_static(black_hint, "Move a window onto the KVMpad display to check the picture.");
    lv_obj_set_style_text_font(black_hint, ui_theme_font(UI_FONT_BODY), 0);
    lv_obj_align(black_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    native_hint = lv_label_create(screen);
    if (!native_hint) goto failed;
    lv_label_set_text_static(native_hint, "Tap the desktop to show Home.");
    lv_obj_align(native_hint, LV_ALIGN_BOTTOM_MID, 0, -20);
    image = lv_image_create(screen);
    if (!image) goto failed;
    if (!lv_obj_add_event_cb(image, show_home, LV_EVENT_SHORT_CLICKED, NULL)) goto failed;
    prepare_mode(native);
    poll(NULL);
    timer = lv_timer_create(poll, MONITOR_POLL_MS, NULL);
    lv_display_t *display = lv_obj_get_display(screen);
#ifdef KVM_USB_MOTION_CHECK
    if (!lv_display_add_event_cb(display, refreshed, LV_EVENT_REFR_START, NULL)) goto failed;
#endif
    if (!timer || !lv_display_add_event_cb(display, refreshed, LV_EVENT_FLUSH_FINISH, NULL) ||
        !lv_display_add_event_cb(display, refreshed, LV_EVENT_REFR_READY, NULL) ||
        !set_monitor(true)) goto failed;
    switch_usb = set_monitor;
    lv_screen_load(screen);
    return true;
failed:
    usb_monitor_view_close();
    return false;
}

void usb_monitor_view_close(void)
{
    if (!screen) return;
    timing_black = false;
    if (switch_usb) switch_usb(false);
    switch_usb = NULL;
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(screen), refreshed, NULL);
    if (timer) lv_timer_delete(timer);
    timer = NULL;
    bool was_visible = lv_screen_active() == screen;
    if (was_visible) lv_screen_load(previous);
    if (image) lv_image_set_src(image, NULL);
    lv_image_cache_drop(&descriptor);
    descriptor.data = NULL;
    if (was_visible) lv_obj_delete_async(screen);
    else lv_obj_delete(screen);
    screen = previous = image = status = guide_card = instructions = black_hint = home_button = native_hint = NULL;
    return_home = NULL;
    monitor_stop();
}
