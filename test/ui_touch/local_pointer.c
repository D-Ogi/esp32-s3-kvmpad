// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "local_pointer_platform.h"
#include "ui_touch_bridge.h"
#include "ui_platform.h"
#include "hid_dispatch.h"
#include <setjmp.h>
#include <stdio.h>

static StaticQueue_t queue;
static uint8_t storage[4096];
static void (*task)(void *);
static jmp_buf drained;
static uint32_t sequence;
static lv_indev_t *pointer;
static lv_obj_t *keys[2];
static char pressed[32], clicked[32];
static unsigned npress, nclick, captured_starts;
static bool captured;
static uint32_t idle_us, next_wait_us;

// A deterministic scheduler runs the real bridge task up to its next empty-queue wait.
// LVGL gets a single read only when the test explicitly requests one.
QueueHandle_t xQueueCreate(unsigned cap, unsigned width)
{
    assert(cap * width <= sizeof(storage));
    return xQueueCreateStatic(cap, width, storage, &queue);
}
bool pointer_queue_send(QueueHandle_t q, const void *item, unsigned wait)
{
    (void)wait;
    return xQueueSend(q, item, 0);
}
bool pointer_queue_receive(QueueHandle_t q, void *item, unsigned wait)
{
    assert(wait);
    if (xQueueReceive(q, item, 0)) return true;
    if (next_wait_us) {
        idle_us += next_wait_us;
        next_wait_us = 0;
        return false;
    }
    longjmp(drained, 1);
}
int xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, unsigned stack, void *arg,
                            unsigned priority, void *handle, unsigned core)
{
    assert(name && stack && priority && !handle && !arg && core == 1);
    task = entry;
    return pdPASS;
}
static void drain(void)
{
    if (!setjmp(drained))
        task(NULL);
    assert(!uxQueueMessagesWaiting(&queue));
}
int64_t esp_timer_get_time(void)
{
    return (int64_t)sequence * 5000 + idle_us;
}
bool ui_platform_is_running(void)
{
    return true;
}
bool ui_platform_lock(int32_t ms)
{
    return ms > 0;
}
void ui_platform_unlock(void) {}
lv_display_t *ui_platform_display(void)
{
    return lv_display_get_default();
}
void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *out, uint32_t *seq)
{
    (void)seq;
    *out = (kvm_hid_snapshot_t){.host_usb = KVM_HOST_USB_MOUNTED};
}
void hid_dispatch_release_all(kvm_release_reason_t reason)
{
    (void)reason;
}

static bool local_contacts(unsigned count, const kvm_touch_point_t points[2], uint64_t timestamp)
{
    assert(timestamp || count > 2);
    if (!count || count > 2) {
        captured = false;
        return false;
    }
    // The third button occupies the local touchpad fixture's rectangle.
    if (!captured && 599 - points[0].y_px >= 300) {
        captured = true;
        captured_starts++;
    }
    return captured;
}

static void post(unsigned count, int x, int y)
{
    kvm_touch_sample_t s = {.valid = true, .point_count = (uint8_t)count, .sample_seq = ++sequence};
    s.timestamp_us = (uint64_t)esp_timer_get_time();
    s.point[0] = (kvm_touch_point_t){.track_id = 1, .x_px = (int16_t)y, .y_px = (int16_t)(599 - x)};
    ui_touch_bridge_post(&s);
}
static void read_pointer(void)
{
    drain();
    lv_indev_read(pointer);
}

static void events(lv_event_t *event)
{
    char id = *(const char *)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        assert(npress < sizeof(pressed) - 1);
        pressed[npress++] = id;
    }
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        assert(nclick < sizeof(clicked) - 1);
        clicked[nclick++] = id;
    }
}
static bool buffered_target(lv_obj_t *target)
{
    return target == keys[0] || target == keys[1];
}
static lv_obj_t *button(lv_obj_t *screen, int x, const char *id)
{
    lv_obj_t *b = lv_button_create(screen);
    lv_obj_set_pos(b, x, 80);
    lv_obj_set_size(b, 100, 70);
    lv_obj_add_event_cb(b, events, LV_EVENT_ALL, (void *)id);
    return b;
}

int main(void)
{
    lv_init();
    lv_display_t *d = lv_test_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_rotation(d, LV_DISPLAY_ROTATION_90);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    keys[0] = button(screen, 20, "A");
    keys[1] = button(screen, 160, "B");
    button(screen, 310, "C");
    lv_obj_set_y(button(screen, 20, "D"), 180);
    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 20, 260);
    lv_obj_set_size(slider, 240, 28);
    lv_obj_update_layout(screen);
    const ui_touch_bridge_cfg_t cfg = {
        .h_res = 1024, .v_res = 600, .local_contacts = local_contacts, .buffered_target = buffered_target};
    assert(ui_touch_bridge_start(&cfg) == ESP_OK);
    pointer = lv_indev_get_next(NULL);
    assert(pointer && !lv_indev_get_next(pointer));
    post(1, 60, 110);
    post(0, 0, 0);
    post(1, 200, 110);
    post(0, 0, 0);
    read_pointer();
    printf("One LVGL read after two taps: pressed='%s' clicked='%s'\n", pressed, clicked);
    fflush(stdout);
    assert(!strcmp(pressed, "AB") && !strcmp(clicked, "AB"));
    read_pointer();
    assert(npress == 2 && nclick == 2);
    post(1, 60, 200);
    post(0, 0, 0);
    read_pointer();
    assert(npress == 2 && nclick == 2);
    post(1, 32, 274);
    read_pointer();
    for (int x = 50; x <= 242; x += 8) {
        post(1, x, 274);
        drain();
    }
    read_pointer();
    assert(lv_slider_get_value(slider) > 85 && lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED);
    post(0, 0, 0);
    read_pointer();
    assert(nclick == 2 && npress == 2);

    post(1, 350, 110);
    post(1, 200, 110);
    post(0, 0, 0);
    post(1, 60, 110);
    post(0, 0, 0);
    read_pointer();
    assert(captured_starts == 1 && !strcmp(pressed, "ABA") && !strcmp(clicked, "ABA"));

    post(1, 200, 110);
    read_pointer();
    assert(!strcmp(pressed, "ABAB"));
    ui_touch_bridge_cancel();
    post(1, 200, 110);
    post(0, 0, 0);
    read_pointer();
    assert(!strcmp(clicked, "ABA"));
    post(1, 60, 110);
    ui_touch_bridge_cancel();
    post(0, 0, 0);
    post(1, 200, 110);
    post(0, 0, 0);
    read_pointer();
    assert(!strcmp(pressed, "ABABB") && !strcmp(clicked, "ABAB"));
    // A live press and release still work on an ordinary control.
    post(1, 60, 200);
    read_pointer();
    post(0, 0, 0);
    read_pointer();
    assert(!strcmp(pressed, "ABABBD") && !strcmp(clicked, "ABABD"));
    // A popup can cover a key between collecting the taps and the LVGL read.
    post(1, 60, 110);
    post(0, 0, 0);
    lv_obj_t *popup = button(lv_layer_top(), 20, "P");
    lv_obj_update_layout(popup);
    read_pointer();
    assert(!strcmp(pressed, "ABABBD") && !strcmp(clicked, "ABABD"));
    lv_obj_delete(popup);
    // A screen change after a known physical lift preserves the first new tap.
    ui_touch_bridge_cancel();
    post(1, 60, 110);
    post(0, 0, 0);
    read_pointer();
    assert(!strcmp(clicked, "ABABDA"));
    // A lift seen by the system pad also rearms the local keyboard.
    touch_rect_t pad = {0,0,320,240};
    post(1, 60, 110);
    ui_touch_bridge_set_pad(&pad,true);
    post(0, 0, 0);
    ui_touch_bridge_set_pad(NULL,false);
    post(1, 200, 110);
    post(0, 0, 0);
    read_pointer();
    assert(!strcmp(clicked, "ABABDAB"));
    // A silent touch controller must release a captured pad without an up frame.
    post(1, 350, 110);
    read_pointer();
    assert(captured);
    next_wait_us = KVM_TOUCH_GAP_CANCEL_US + 1;
    read_pointer();
    assert(!captured);
    post(1, 350, 110);
    read_pointer();
    assert(!captured);
    post(0, 0, 0);
    post(1, 350, 110);
    read_pointer();
    assert(captured);
    post(0, 0, 0);
    read_pointer();
    assert(!captured && !strcmp(clicked, "ABABDAB"));
    ui_touch_bridge_stats_t stats;
    ui_touch_bridge_get_stats(&stats);
    assert(stats.queue_full == 0 && stats.indev_resets >= 2 && stats.posted == sequence);
    lv_indev_delete(pointer);
    lv_display_delete(d);
    lv_deinit();
    puts("PASS portrait button edges, live and buffered target filtering, popup, slider motion, touchpad "
         "capture, cancel and fresh tap");
}
