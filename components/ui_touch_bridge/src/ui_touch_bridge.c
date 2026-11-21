// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UI touch bridge task (TASK-060E). One task owns the router and the gesture FSM; samples and
// commands arrive through one queue, so routing, gesture state and the HID sink never race. The LVGL
// indev read callback consumes pointer samples and resets in the LVGL owner task.
#include "ui_touch_bridge.h"
#include "local_contacts.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui_platform.h"
#if KVM_HID_HAS_HPE
#include "hid_dispatch.h"
#include "ptp_contacts.h"
#endif

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
static bool s_known_lift;
#if KVM_HID_HAS_HPE
static hpe_frame_t s_active;
static kvm_hid_snapshot_t s_domain;
static bool s_wait_lift, s_had_finger, s_have_seq;
static uint32_t s_prev_seq, s_dropped;
static uint64_t s_last_frame_us;
#endif

// shared with the LVGL task / readers
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ptr_pressed;
static bool s_reset_pending;
static int16_t s_ptr_x, s_ptr_y;
static ui_touch_bridge_stats_t s_stats;
static local_contacts_queue_t s_local;
static bool s_local_mode;
static bool s_local_captured; // LVGL owner only
static bool s_contact_down, s_buffered_contact; // LVGL owner only

static void discard_local_contacts(void)
{
    // Called with s_mux held. An idle route change has already seen the
    // physical lift. Its cancellation marker must not consume the next tap.
    local_contacts_cancel(&s_local);
    if (s_known_lift) {
        s_local.blocked = false;
        s_local.last.count = 0;
    }
}

static bool buffered_target(lv_indev_t *indev, lv_point_t point)
{
    if (!s_cfg.buffered_target) return false;
    lv_display_t *display = lv_indev_get_display(indev);
    lv_display_rotate_point(display, &point);
    if (lv_indev_search_obj(lv_display_get_layer_sys(display), &point) ||
        lv_indev_search_obj(lv_display_get_layer_top(display), &point)) return false;
    lv_obj_t *target = lv_indev_search_obj(lv_display_get_screen_active(display), &point);
    return target && s_cfg.buffered_target(target);
}

static void indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    for (;;) {
        local_contacts_t frame;
        portENTER_CRITICAL(&s_mux);
        bool reset = s_reset_pending;
        s_reset_pending = false;
        bool queued = s_local.used && (!reset || s_local.pending[0].count > 2);
        local_pointer_t pointer = {s_ptr_x, s_ptr_y, s_ptr_pressed && !(s_local_mode && s_local.blocked)};
        bool live_press = pointer.pressed;
        if (queued) {
            frame = s_local.pending[0];
            memmove(s_local.pending, s_local.pending + 1, --s_local.used * sizeof(frame));
            pointer = frame.pointer;
        }
        bool more = s_local.used != 0;
        data->continue_reading = more || (reset && live_press);
        s_stats.indev_reads++;
        portEXIT_CRITICAL(&s_mux);

        data->point = (lv_point_t){pointer.x, pointer.y};
        bool captured = s_local_captured;
        if (queued) {
            bool taken = s_cfg.local_contacts(frame.count, frame.points, frame.timestamp_us);
            reset |= frame.count > 2 || (taken && !captured);
            captured |= taken;
            s_local_captured = frame.count > 0 && frame.count <= 2 && captured;
        } else if (reset) {
            s_local_captured = false;
        }
        if (reset) {
            s_contact_down = s_buffered_contact = false;
            portENTER_CRITICAL(&s_mux);
            s_stats.indev_resets++;
            portEXIT_CRITICAL(&s_mux);
            lv_indev_reset(indev, NULL);
        }
        bool pressed = pointer.pressed && !reset && !captured;
        if (queued && pressed && more) {
            // Only the keyboard opts in to historical presses. A tap on another
            // surface must never be replayed into a newly opened menu or dialog.
            bool allowed = frame.count == 1 && buffered_target(indev, data->point);
            if (!s_contact_down) s_buffered_contact = allowed;
            s_contact_down = true;
            if (!allowed || !s_buffered_contact) continue;
        } else {
            if (!s_contact_down || !pressed) s_buffered_contact = false;
            s_contact_down = pressed;
        }
        data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return;
    }
}

static void emit(const kvm_gesture_out_t *out)
{
    for (uint8_t i = 0; i < out->n; i++) {
        kvm_hid_intent_t intent = out->ev[i];
#if KVM_HID_HAS_HPE
        intent.pad_input = true;
        intent.pad_generation = s_domain.pad_generation;
#endif
        bool ok = s_cfg.sink != NULL && s_cfg.sink(s_cfg.sink_ctx, &intent);
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

#if KVM_HID_HAS_HPE
static void cancel_pad(bool release)
{
    s_wait_lift |= s_had_finger;
    s_active.count = 0;
    touch_router_cancel(&s_router);
    if (release) hid_dispatch_release_all(KVM_RA_GESTURE_CANCEL);
    kvm_gesture_out_t ignored = {0};
    kvm_gesture_cancel(&s_gesture, KVM_GEST_CANCEL_EXTERNAL, &ignored);
}

static void check_domain(void)
{
    kvm_hid_snapshot_t snap;
    hid_dispatch_get_snapshot(&snap, NULL);
    if (snap.release_generation != s_domain.release_generation || snap.pad_generation != s_domain.pad_generation ||
        snap.host_usb != s_domain.host_usb || snap.maintenance != s_domain.maintenance) cancel_pad(false);
    s_domain = snap;
    portENTER_CRITICAL(&s_mux);
    uint32_t dropped = s_stats.queue_full;
    portEXIT_CRITICAL(&s_mux);
    if (s_dropped != dropped) {
        s_dropped = dropped;
        s_wait_lift = true;
        cancel_pad(true);
    }
}

static void native_touch(const kvm_touch_sample_t *sample)
{
    if (!s_domain.pad_surface_enabled || !s_router.pad_enabled) return;
    hpe_frame_t frames[2];
    int n = ptp_contacts_update(&s_active, &s_router.pad, sample, frames);
    if (n < 0) { cancel_pad(true); return; }
    for (int i = 0; i < n; i++) {
        kvm_hid_intent_t intent = {.type = KVM_HID_PTP_FRAME, .origin = KVM_ORIGIN_LOCAL,
                                   .timestamp_us = sample->timestamp_us};
        intent.data.ptp.frame = frames[i];
        intent.data.ptp.generation = s_domain.pad_generation;
        if (!s_cfg.sink || !s_cfg.sink(s_cfg.sink_ctx, &intent)) {
            cancel_pad(true);
            return;
        }
    }
}
#endif

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
#if KVM_HID_HAS_HPE
        const kvm_touch_sample_t *sample = &m->u.sample;
        // A newly ready scan can follow a stall before either watchdog ran.
        // Require a full physical lift before reusing a held sensor ID.
        bool gap = s_active.count && (sample->timestamp_us < s_last_frame_us ||
                                     sample->timestamp_us - s_last_frame_us > KVM_TOUCH_GAP_CANCEL_US);
        if (!sample->valid || gap || (s_have_seq && sample->sample_seq != s_prev_seq + 1)) {
            s_wait_lift = true;
            cancel_pad(true);
        }
        s_have_seq = true;
        s_prev_seq = sample->sample_seq;
        s_last_frame_us = sample->timestamp_us;
        if (sample->valid) {
            s_had_finger = sample->point_count > 0;
            if (!s_had_finger) s_wait_lift = false;
        }
#endif
        touch_route_out_t ro;
        touch_router_route(&s_router, &m->u.sample, &ro);
        portENTER_CRITICAL(&s_mux);
        s_known_lift = m->u.sample.valid && !m->u.sample.point_count;
        s_local_mode = s_cfg.local_contacts && !s_router.pad_enabled && !s_hid_on;
        if (s_local_mode)
            local_contacts_post(&s_local, &m->u.sample, (local_pointer_t){ro.ptr_x, ro.ptr_y, ro.ptr_pressed});
        else if (s_known_lift) {
            s_local.blocked = false;
            s_local.last.count = 0;
        }
        s_ptr_pressed = ro.ptr_pressed;
        s_reset_pending |= ro.ptr_cancel;
        s_ptr_x = ro.ptr_x;
        s_ptr_y = ro.ptr_y;
        portEXIT_CRITICAL(&s_mux);
#if KVM_HID_HAS_HPE
        if (s_wait_lift || s_domain.maintenance || s_domain.host_usb != KVM_HOST_USB_MOUNTED) break;
        if (s_hid_on && s_domain.pad_native_requested) {
            native_touch(&ro.pad);
            break;
        }
#endif
        if (s_hid_on) {
            kvm_gesture_feed(&s_gesture, &ro.pad, &out);
            emit(&out);
        }
        break;
    }
    case MSG_SET_PAD:
#if KVM_HID_HAS_HPE
        if (s_active.count && (!m->u.pad.enabled || memcmp(&m->u.pad.rect, &s_router.pad, sizeof(s_router.pad)))) cancel_pad(true);
#endif
        if (!m->u.pad.enabled) {
            gesture_cancel(); // pad leaves: nothing may stay held
        }
        if (m->u.pad.enabled && !s_router.pad_enabled && s_local_mode) {
            portENTER_CRITICAL(&s_mux);
            discard_local_contacts();
            s_reset_pending = true;
            s_local_mode = false;
            portEXIT_CRITICAL(&s_mux);
        }
        touch_router_set_pad(&s_router, &m->u.pad.rect, m->u.pad.enabled);
        break;
    case MSG_CANCEL:
#if KVM_HID_HAS_HPE
        cancel_pad(s_active.count != 0);
#endif
        touch_router_cancel(&s_router);
        s_router.cancel_pending = false; // reset right here instead of on the next sample
        gesture_cancel();
        portENTER_CRITICAL(&s_mux);
        s_reset_pending = true;
        if (s_cfg.local_contacts) discard_local_contacts();
        s_ptr_pressed = false;
        portEXIT_CRITICAL(&s_mux);
        break;
    case MSG_HID:
        if (m->u.on && !s_hid_on && s_cfg.local_contacts) {
            portENTER_CRITICAL(&s_mux);
            discard_local_contacts();
            s_reset_pending = true;
            s_local_mode = false;
            portEXIT_CRITICAL(&s_mux);
        }
        if (!m->u.on && s_hid_on) {
#if KVM_HID_HAS_HPE
            cancel_pad(true);
#endif
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
#if KVM_HID_HAS_HPE
        check_domain();
#endif
        if (got) {
            handle(&m);
        }
        uint64_t now = (uint64_t)esp_timer_get_time();
        if (now - last_tick >= TICK_US) {
            last_tick = now;
            if (!got) {
                portENTER_CRITICAL(&s_mux);
                if (s_local_mode) local_contacts_expire(&s_local, now);
                portEXIT_CRITICAL(&s_mux);
            }
#if KVM_HID_HAS_HPE
            if (s_active.count && now - s_last_frame_us > KVM_TOUCH_GAP_CANCEL_US) cancel_pad(true);
            bool mouse = !s_domain.pad_native_requested && !s_wait_lift;
#else
            bool mouse = true;
#endif
            if (s_hid_on && mouse) {
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
    s_router.require_lift_after_error = true;
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
