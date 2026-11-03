// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI touch bridge task (TASK-060E). One task owns the router and the gesture FSM; samples and
// commands arrive through one queue, so routing, gesture state and the HID sink never race. The LVGL
// indev read callback (LVGL task) only copies the pointer state under a spinlock; a cancelled press is
// reset with lv_indev_reset() under the UI lock, so its release is never a click.
#include "ui_touch_bridge.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_platform.h"

static const char *TAG = "ui_touch";

#define QUEUE_LEN 16
#define TICK_US 10000 // gesture timers (long press, gap cancel, drag-lock idle): every ~10 ms
#define TASK_STACK 3072 // measured peak ~1.2 KiB on board #1 (2026-11-02)
#define TASK_PRIO 6   // above the LVGL task (4), below the GT911 sampler and TinyUSB

typedef enum { MSG_SAMPLE = 0, MSG_SET_PAD, MSG_CANCEL, MSG_HID, MSG_DRAG_LOCK } msg_kind_t;

typedef struct {
    msg_kind_t kind;
    union {
        kvm_touch_sample_t sample;
        struct {
            touch_rect_t rect;
            bool enabled;
        } pad;
        bool on;
    } u;
} msg_t;

static QueueHandle_t s_q;
static lv_indev_t *s_indev;
static ui_touch_bridge_cfg_t s_cfg;
static bool s_running;

// bridge-task state
static touch_router_t s_router;
static kvm_gesture_t s_gesture;
static bool s_hid_on;
static bool s_reset_pending;

// shared with the LVGL task / readers
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ptr_pressed;
static int16_t s_ptr_x, s_ptr_y;
static ui_touch_bridge_stats_t s_stats;

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    portENTER_CRITICAL(&s_mux);
    data->point.x = s_ptr_x;
    data->point.y = s_ptr_y;
    data->state = s_ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    s_stats.indev_reads++;
    portEXIT_CRITICAL(&s_mux);
}

static void emit(const kvm_gesture_out_t *out)
{
    for (uint8_t i = 0; i < out->n; i++) {
        bool ok = s_cfg.sink != NULL && s_cfg.sink(s_cfg.sink_ctx, &out->ev[i]);
        portENTER_CRITICAL(&s_mux);
        if (ok) {
            s_stats.intents++;
        } else {
            s_stats.intents_refused++;
        }
        portEXIT_CRITICAL(&s_mux);
    }
}

static void gesture_cancel(void)
{
    kvm_gesture_out_t out = {0};
    kvm_gesture_cancel(&s_gesture, KVM_GEST_CANCEL_EXTERNAL, &out);
    emit(&out); // only releases: allowed even while pad -> HID is being switched off
}

static void reset_lvgl_press(void)
{
    if (ui_platform_lock(100)) {
        lv_indev_reset(s_indev, NULL); // press lost: LVGL waits for the release, no click
        ui_platform_unlock();
        s_reset_pending = false;
        portENTER_CRITICAL(&s_mux);
        s_stats.indev_resets++;
        portEXIT_CRITICAL(&s_mux);
    } else {
        s_reset_pending = true; // retried on the next loop
    }
}

static void publish_state(void)
{
    portENTER_CRITICAL(&s_mux);
    s_stats.router = s_router.st;
    s_stats.gesture = s_gesture.st;
    s_stats.hid_enabled = s_hid_on;
    s_stats.drag_lock = s_gesture.drag_lock;
    s_stats.left_held = s_gesture.left_held;
    s_stats.gesture_state = s_gesture.state;
    s_stats.pad = s_router.pad;
    s_stats.pad_enabled = s_router.pad_enabled;
    portEXIT_CRITICAL(&s_mux);
}

static void handle(const msg_t *m)
{
    kvm_gesture_out_t out = {0};
    switch (m->kind) {
    case MSG_SAMPLE: {
        touch_route_out_t ro;
        touch_router_route(&s_router, &m->u.sample, &ro);
        portENTER_CRITICAL(&s_mux);
        s_ptr_pressed = ro.ptr_pressed;
        s_ptr_x = ro.ptr_x;
        s_ptr_y = ro.ptr_y;
        portEXIT_CRITICAL(&s_mux);
        if (ro.ptr_cancel) {
            reset_lvgl_press();
        }
        if (s_hid_on) {
            kvm_gesture_feed(&s_gesture, &ro.pad, &out);
            emit(&out);
        }
        break;
    }
    case MSG_SET_PAD:
        if (!m->u.pad.enabled) {
            gesture_cancel(); // pad leaves: nothing may stay held
        }
        touch_router_set_pad(&s_router, &m->u.pad.rect, m->u.pad.enabled);
        break;
    case MSG_CANCEL:
        touch_router_cancel(&s_router);
        s_router.cancel_pending = false; // reset right here instead of on the next sample
        gesture_cancel();
        portENTER_CRITICAL(&s_mux);
        s_ptr_pressed = false;
        portEXIT_CRITICAL(&s_mux);
        reset_lvgl_press();
        break;
    case MSG_HID:
        if (!m->u.on && s_hid_on) {
            gesture_cancel();
        }
        if (m->u.on && !s_hid_on) {
            const kvm_gesture_cfg_t cfg = s_gesture.cfg; // init clears the struct before copying cfg
            kvm_gesture_init(&s_gesture, &cfg);          // fresh FSM: no state from before
        }
        s_hid_on = m->u.on;
        break;
    case MSG_DRAG_LOCK:
        kvm_gesture_set_drag_lock(&s_gesture, m->u.on, (uint64_t)esp_timer_get_time(), &out);
        emit(&out);
        break;
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    msg_t m;
    uint64_t last_tick = (uint64_t)esp_timer_get_time();
    for (;;) {
        bool got = xQueueReceive(s_q, &m, pdMS_TO_TICKS(TICK_US / 1000)) == pdTRUE;
        if (got) {
            handle(&m);
        }
        if (s_reset_pending) {
            reset_lvgl_press();
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (now - last_tick >= TICK_US) {
            last_tick = now;
            if (s_hid_on) {
                kvm_gesture_out_t out = {0};
                kvm_gesture_tick(&s_gesture, now, &out);
                emit(&out);
            }
        }
        publish_state();
    }
}

static bool send_cmd(const msg_t *m)
{
    if (!s_running) {
        return false;
    }
    if (xQueueSend(s_q, m, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "command %d not queued", (int)m->kind);
        return false;
    }
    return true;
}

esp_err_t ui_touch_bridge_start(const ui_touch_bridge_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL && !s_running, ESP_ERR_INVALID_STATE, TAG, "state");
    ESP_RETURN_ON_FALSE(ui_platform_is_running(), ESP_ERR_INVALID_STATE, TAG, "ui_platform not running");
    s_cfg = *cfg;
    touch_router_init(&s_router);
    kvm_gesture_cfg_t gc = KVM_GESTURE_CFG_DEFAULT;
    gc.h_res = cfg->h_res;
    gc.v_res = cfg->v_res;
    kvm_gesture_init(&s_gesture, &gc);
    s_hid_on = cfg->hid_enabled;

    s_q = xQueueCreate(QUEUE_LEN, sizeof(msg_t));
    ESP_RETURN_ON_FALSE(s_q != NULL, ESP_ERR_NO_MEM, TAG, "queue");

    ESP_RETURN_ON_FALSE(ui_platform_lock(2000), ESP_ERR_TIMEOUT, TAG, "lock");
    s_indev = lv_indev_create();
    if (s_indev != NULL) {
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, indev_read_cb);
        lv_indev_set_display(s_indev, ui_platform_display());
    }
    ui_platform_unlock();
    ESP_RETURN_ON_FALSE(s_indev != NULL, ESP_ERR_NO_MEM, TAG, "indev");

    s_running = true;
    if (xTaskCreatePinnedToCore(bridge_task, "ui_touch", TASK_STACK, NULL, TASK_PRIO, NULL, 1) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "touch bridge running: LVGL pointer + gesture FSM, pad -> HID %s", s_hid_on ? "ON" : "OFF");
    return ESP_OK;
}

bool ui_touch_bridge_running(void)
{
    return s_running;
}

void ui_touch_bridge_post(const kvm_touch_sample_t *s)
{
    if (!s_running || s == NULL) {
        return;
    }
    msg_t m = {.kind = MSG_SAMPLE};
    m.u.sample = *s;
    bool ok = xQueueSend(s_q, &m, 0) == pdTRUE;
    portENTER_CRITICAL(&s_mux);
    if (ok) {
        s_stats.posted++;
    } else {
        s_stats.queue_full++;
    }
    portEXIT_CRITICAL(&s_mux);
}

void ui_touch_bridge_set_pad(const touch_rect_t *pad, bool enabled)
{
    msg_t m = {.kind = MSG_SET_PAD};
    if (pad) {
        m.u.pad.rect = *pad;
    }
    m.u.pad.enabled = enabled && pad != NULL;
    send_cmd(&m);
}

void ui_touch_bridge_cancel(void)
{
    msg_t m = {.kind = MSG_CANCEL};
    send_cmd(&m);
}

void ui_touch_bridge_set_hid_enabled(bool on)
{
    msg_t m = {.kind = MSG_HID};
    m.u.on = on;
    send_cmd(&m);
}

void ui_touch_bridge_set_drag_lock(bool on)
{
    msg_t m = {.kind = MSG_DRAG_LOCK};
    m.u.on = on;
    send_cmd(&m);
}

void ui_touch_bridge_get_stats(ui_touch_bridge_stats_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_mux);
}
