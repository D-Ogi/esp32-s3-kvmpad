// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware integration of ui_core (TASK-060B, 07 §3 "Handling"):
// 1. A 20 ms esp_timer publisher copies the authoritative domain state (hid_dispatch snapshot,
//    usb_hid_device LED report, touch_input health) into the UI mailbox. It never calls LVGL.
// 2. An lv_timer in the LVGL task takes the newest snapshot and updates the Subjects (ui_store).
// 3. The domain sink hands UI actions to hid_dispatch (InputArbiter -> HidScheduler); drag lock requests go to
//    the gesture engine in the touch bridge, which reports the result back as its slice.
// 4. The product shell: ui_core router (TASK-060C) with the MOUSE view (TASK-070A) on its own screen; bench scenes
//    (TASK-060A) suspend it and `ui shell` brings it back.
// The remote core does not exist in this build (M7), so CLIENT is published as DISABLED; Wi-Fi comes from
// net_status (TASK-080) and drag lock from the touch bridge's gesture FSM (TASK-060E).
#include "text_glue.h"
#include "ui_glue.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "board_pins.h"
#include "hid_dispatch.h"
#include "kvm_diag.h"
#include "net_status.h"
#include "remote_glue.h"
#include "touch_input.h"
#include "ui_platform.h"
#include "ui_shell.h"
#include "ui_touch_bridge.h"
#include "usb_hid_device.h"
#if CONFIG_KVM_UI_BENCH
#include "ui_bench.h"
#endif

static const char *TAG = "ui_glue";

#define PUBLISH_PERIOD_US 20000 // 50 Hz: well inside the 100 ms status budget, coalesced when unchanged
#define TOUCH_WINDOW_TICKS 50   // touch health judged over 1 s windows
#define UI_TICK_MS 20

static portMUX_TYPE s_mb_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_mailbox_t s_mb;
static ui_store_t s_store;
static ui_actions_t s_actions;
static esp_timer_handle_t s_pub_timer;
static lv_timer_t *s_tick_timer;
static bool s_running;

static uint32_t s_seq_hid, s_seq_net, s_seq_touch, s_seq_gesture;
static uint32_t s_touch_ticks;
static uint32_t s_touch_err_last;

static void mb_lock(void *ctx)
{
    portENTER_CRITICAL((portMUX_TYPE *)ctx);
}

static void mb_unlock(void *ctx)
{
    portEXIT_CRITICAL((portMUX_TYPE *)ctx);
}

static void publish_cb(void *arg)
{
    (void)arg;
    kvm_hid_snapshot_t hs;
    hid_dispatch_get_snapshot(&hs, NULL);
    usb_hid_status_t us;
    usb_hid_device_get_status(&us);
    ui_hid_slice_t h = {
        .host_usb_state = (uint8_t)hs.host_usb,
        .owner_kind = (uint8_t)hs.owner_kind,
        .owner_epoch = hs.owner_epoch,
        .modifiers_down = hs.modifiers_down,
        .mods_armed = hs.mods_armed,
        .mods_locked = hs.mods_locked,
        .caps_led_known = us.led_reports > 0,
        .caps_led_active = (us.led_state & 0x02u) != 0, // HID LED usage 2 = Caps Lock (bit 1)
        .maintenance = hs.maintenance,
        .error_code = 0, // no persistent domain error source yet; USB_ERROR is visible as host_usb_state
    };
    ui_mailbox_publish_hid(&s_mb, &h, ++s_seq_hid);

    if (++s_touch_ticks >= TOUCH_WINDOW_TICKS) {
        s_touch_ticks = 0;
        touch_input_info_t ti;
        touch_input_get_info(&ti);
        touch_input_stats_t ts;
        touch_input_get_stats(&ts);
        ui_touch_slice_t t = {
            .touch_healthy = ti.i2c_addr != 0 && ts.i2c_errors == s_touch_err_last,
        };
        s_touch_err_last = ts.i2c_errors;
        ui_mailbox_publish_touch(&s_mb, &t, ++s_seq_touch);
    }

    if (net_status_running()) { // TASK-080 link + TASK-100 transport: the CLIENT indicator (FR-403)
        net_status_info_t ni;
        net_status_get(&ni);
        remote_glue_link_t rl;
        remote_glue_get_link(&rl);
        uint8_t cs = UI_CLIENT_DISABLED;
        if (rl.running && rl.enabled) {
            if (!ni.link.ip_present) {
                cs = UI_CLIENT_NETWORK_DOWN;
            } else if (hs.owner_kind == OWNER_REMOTE) {
                cs = UI_CLIENT_CONTROL_GRANTED; // the arbiter's owner, never the transport's opinion
            } else if (rl.control_sessions > 0) {
                cs = UI_CLIENT_CONNECTED;
            } else if (rl.pairing_sessions > 0) {
                cs = UI_CLIENT_AUTHENTICATING;
            } else {
                cs = UI_CLIENT_LISTENING;
            }
        }
        ui_net_slice_t n = {.client_state = cs, .wifi_connected = ni.link.ip_present};
        ui_mailbox_publish_net(&s_mb, &n, ++s_seq_net);
    }

    if (ui_touch_bridge_running()) { // the gesture FSM owns drag lock (authoritative, 07 §3)
        ui_touch_bridge_stats_t bs;
        ui_touch_bridge_get_stats(&bs);
        ui_gesture_slice_t g = {.drag_lock_active = bs.drag_lock};
        ui_mailbox_publish_gesture(&s_mb, &g, ++s_seq_gesture);
    }
}

// Pad gestures -> typed HID intents -> hid_dispatch (InputArbiter -> HidScheduler). Only reached while
// pad -> HID is enabled (`ui pad on`); releases from a cancel always pass.
static bool pad_hid_sink(void *ctx, const kvm_hid_intent_t *intent)
{
    (void)ctx;
    return hid_dispatch_local(intent) == ESP_OK;
}

static void touch_bridge_start(void)
{
    ui_touch_bridge_cfg_t tc = {
        .h_res = BOARD_TOUCH_H_RES,
        .v_res = BOARD_TOUCH_V_RES,
        .sink = pad_hid_sink,
        .sink_ctx = NULL,
        .hid_enabled = false, // turned on by ui_shell_show(): bench scenes never move the HOST pointer
    };
    esp_err_t err = ui_touch_bridge_start(&tc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui_touch_bridge_start: %s", esp_err_to_name(err));
        return;
    }
    diag_set_touch_hook(ui_touch_bridge_post);
    ui_glue_refresh_pad();
}

ui_store_t *ui_glue_store(void)
{
    return &s_store;
}

ui_actions_t *ui_glue_actions(void)
{
    return &s_actions;
}

void ui_glue_refresh_pad(void)
{
    if (ui_shell_refresh_pad()) { // the product shell is on screen and owns the pad rectangle
        return;
    }
#if CONFIG_KVM_UI_BENCH
    touch_rect_t r;
    if (ui_bench_pad_area(&r.x1, &r.y1, &r.x2, &r.y2)) {
        ui_touch_bridge_set_pad(&r, true);
        ESP_LOGI(TAG, "pad area x %d..%d y %d..%d", r.x1, r.x2, r.y1, r.y2);
        return;
    }
#endif
    ui_touch_bridge_set_pad(NULL, false);
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    ui_store_tick(&s_store);
}

static bool domain_sink(void *ctx, const ui_action_t *a)
{
    (void)ctx;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    kvm_hid_intent_t in;
    memset(&in, 0, sizeof(in));
    in.timestamp_us = a->monotonic_us;
    switch (a->type) {
    case UI_ACT_KEY_DOWN:
        err = hid_dispatch_ui_key_down((uint8_t)a->value);
        break;
    case UI_ACT_KEY_UP:
        err = hid_dispatch_ui_key_up((uint8_t)a->value);
        break;
    case UI_ACT_MODIFIER_TOGGLE:
        err = hid_dispatch_ui_modifier_tap((uint8_t)a->value);
        break;
    case UI_ACT_MOUSE_BUTTON_DOWN:
    case UI_ACT_MOUSE_BUTTON_UP:
        in.type = a->type == UI_ACT_MOUSE_BUTTON_DOWN ? KVM_HID_MOUSE_BTN_DOWN : KVM_HID_MOUSE_BTN_UP;
        in.data.mouse_button.button_id = (uint8_t)a->value;
        err = hid_dispatch_local(&in);
        break;
    case UI_ACT_SHORTCUT_REQUEST:
        in.type = KVM_HID_CHORD;
        in.data.chord.chord_id = a->value;
        err = hid_dispatch_local(&in);
        break;
    case UI_ACT_DRAG_LOCK_REQUEST: // the gesture engine owns drag lock; the UI learns the result from its slice
        ui_touch_bridge_set_drag_lock(a->value != 0);
        err = ESP_OK;
        break;
    case UI_ACT_TEXT_CHAR: // ADR-024 FR-748: composed text -> ordered queue -> one burst per item
        err = text_glue_push_char(a->value) ? ESP_OK : ESP_ERR_NO_MEM;
        break;
    case UI_ACT_TEXT_KEY:
        err = text_glue_push_key(a->value) ? ESP_OK : ESP_ERR_NO_MEM;
        break;
    case UI_ACT_TEXT_CANCEL:
        text_glue_cancel(KVM_RA_VIEW_CHANGE);
        err = ESP_OK;
        break;
    default:
        break;
    }
    return err == ESP_OK;
}

esp_err_t ui_glue_start(void)
{
    ESP_RETURN_ON_FALSE(ui_platform_is_running(), ESP_ERR_INVALID_STATE, TAG, "ui_platform not running");
    ESP_RETURN_ON_FALSE(!s_running, ESP_ERR_INVALID_STATE, TAG, "already running");
    ui_mailbox_lock_t lk = {.lock = mb_lock, .unlock = mb_unlock, .ctx = &s_mb_mux};
    ui_mailbox_init(&s_mb, &lk);
    ui_actions_init(&s_actions);
    ui_actions_set_domain_sink(&s_actions, domain_sink, NULL);

    // The network core is not part of this build (M7): CLIENT is truthfully DISABLED.
    ui_net_slice_t n = {.client_state = UI_CLIENT_DISABLED, .wifi_connected = false};
    ui_mailbox_publish_net(&s_mb, &n, ++s_seq_net);

    ESP_RETURN_ON_FALSE(ui_platform_lock(2000), ESP_ERR_TIMEOUT, TAG, "lock");
    bool ok = ui_store_init(&s_store, &s_mb);
    if (ok) {
        s_tick_timer = lv_timer_create(tick_cb, UI_TICK_MS, NULL);
        ok = s_tick_timer != NULL;
        if (!ok) {
            ui_store_deinit(&s_store);
        }
    }
    ui_platform_unlock();
    ESP_RETURN_ON_FALSE(ok, ESP_ERR_NO_MEM, TAG, "store / timer");

    const esp_timer_create_args_t args = {.callback = publish_cb, .name = "ui_pub"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_pub_timer), TAG, "timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_pub_timer, PUBLISH_PERIOD_US), TAG, "timer start");
    s_running = true;
    ESP_LOGI(TAG, "ui_core store running: publish %d ms, ui tick %d ms", PUBLISH_PERIOD_US / 1000, UI_TICK_MS);
    touch_bridge_start(); // TASK-060E
    return ESP_OK;
}

bool ui_glue_running(void)
{
    return s_running;
}

void ui_glue_status(ui_state_snapshot_t *applied, ui_store_counters_t *store, ui_mailbox_counters_t *mb,
                    ui_actions_counters_t *actions)
{
    if (ui_platform_lock(1000)) {
        if (applied) {
            *applied = s_store.applied;
        }
        if (store) {
            *store = s_store.c;
        }
        if (actions) {
            *actions = s_actions.c;
        }
        ui_platform_unlock();
    }
    if (mb) {
        ui_mailbox_counters(&s_mb, mb);
    }
}
