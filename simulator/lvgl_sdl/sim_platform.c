// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// See sim_platform.h.
#include "sim_platform.h"

#include <stdio.h>
#include <string.h>

#include "ui_test_env.h"
#include "ui_theme.h"

// ---- hooks and sinks --------------------------------------------------------------------------------------------

static sim_app_t *s_active; // the shell the hooks belong to (one shell at a time, like the device)

static void log_put(sim_app_t *s, const sim_log_t *e)
{
    if (!s) {
        return;
    }
    sim_log_t entry = *e;
    entry.seq = ++s->log_seq;
    if (s->log_n < SIM_LOG_CAP) {
        s->log[s->log_n++] = entry;
    } else {
        s->log_lost++;
    }
}

static uint64_t sim_now(void)
{
    // A fixed step per read: monotonic, reproducible and never a wall clock.
    return s_active ? (s_active->clock_us += 1000u) : 0u;
}

static bool sim_domain_sink(void *ctx, const ui_action_t *a)
{
    sim_app_t *s = ctx;
    const sim_log_t e = {.kind = SIM_LOG_ACTION, .type = a->type, .control_id = a->control_id, .value = a->value};
    log_put(s, &e);
    return true; // the fake domain always accepts; a refusal path belongs to L2 (07 §10)
}

static bool sim_cancel_input(void *ctx, ui_cancel_reason_t why)
{
    sim_app_t *s = ctx;
    const sim_log_t e = {.kind = SIM_LOG_CANCEL, .cancel_why = (uint8_t)why};
    log_put(s, &e);
    return true;
}

static void sim_pad_changed(void *ctx, const lv_area_t *pad)
{
    sim_app_t *s = ctx;
    const sim_log_t e = {.kind = SIM_LOG_PAD, .pad_on = pad != NULL};
    log_put(s, &e);
    s->pad_on = pad != NULL;
    if (pad) {
        s->pad = *pad;
    }
}

static bool sim_confirm_alt_f4(void *ctx)
{
    const sim_app_t *s = ctx;
    return s->confirm_alt_f4;
}

static void sim_remote_action(void *ctx, ui_remote_action_t act, uint64_t session_id)
{
    sim_app_t *s = ctx;
    const sim_log_t e = {.kind = SIM_LOG_REMOTE, .remote_act = (uint8_t)act, .session_id = session_id};
    log_put(s, &e);
}

/**
 * The Settings change hook. The simulator holds no config_store, so the request is recorded and nothing is
 * written or republished: that is exactly the state a device is in between the tap and the NVS write, and it
 * is what makes the golden of a tapped toggle show the OLD value (AGENTS-UI MUST 6).
 */
static void sim_settings_change(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    sim_app_t *s = ctx;
    const sim_log_t e = {.kind = SIM_LOG_SETTING,
                         .setting_id = (uint8_t)id,
                         .setting_val = value,
                         .client_index = client_index};
    log_put(s, &e);
}

// ---- environment ------------------------------------------------------------------------------------------------

size_t sim_env_start(void)
{
    return ui_test_env_start(); // headless 1024x600 RGB565, test indevs, font warm-up (shared with TASK-060D)
}

void sim_env_stop(size_t baseline)
{
    ui_test_env_stop(baseline);
}

// ---- shell lifecycle --------------------------------------------------------------------------------------------

bool sim_app_start(sim_app_t *s)
{
    if (!s) {
        return false;
    }
    s_active = s;
    s->clock_us = 0;
    s->domain_seq = 0;
    s->log_n = 0;
    s->log_seq = 0;
    s->log_lost = 0;
    s->pad_on = false;
    memset(&s->pad, 0, sizeof(s->pad));

    ui_mailbox_init(&s->mb, NULL);
    if (!ui_store_init(&s->store, &s->mb)) {
        return false;
    }
    ui_actions_init(&s->actions);
    ui_actions_set_domain_sink(&s->actions, sim_domain_sink, s);

    const ui_app_hooks_t hooks = {
        .now_us = sim_now,
        .cancel_input = sim_cancel_input,
        .pad_changed = sim_pad_changed,
        .confirm_alt_f4 = sim_confirm_alt_f4,
        .remote_action = sim_remote_action,
        .settings_change = sim_settings_change,
        .ctx = s,
        .layout_chord = s->layout_chord,
    };
    if (!ui_app_init(&s->app, &s->store, &s->actions, &hooks)) {
        ui_store_deinit(&s->store);
        return false;
    }
    lv_screen_load(s->app.screen);
    ui_app_show(&s->app);
    lv_test_wait(20);
    s->started = true;
    return true;
}

void sim_app_stop(sim_app_t *s)
{
    if (!s || !s->started) {
        return;
    }
    lv_obj_t *home = lv_obj_create(NULL);
    lv_screen_load(home);
    lv_test_wait(5);
    ui_app_deinit(&s->app);
    ui_store_deinit(&s->store);
    lv_test_wait(10);
    lv_obj_delete(home);
    lv_test_wait(5);
    s->started = false;
    s_active = NULL;
}

void sim_publish(sim_app_t *s, const sim_state_t *st)
{
    if (!s || !st) {
        return;
    }
    const uint32_t seq = ++s->domain_seq;
    const ui_hid_slice_t hid = {
        .host_usb_state = st->host_usb_state,
        .owner_kind = st->owner_kind,
        .owner_epoch = st->owner_epoch,
        .modifiers_down = st->modifiers_down,
        .mods_armed = st->mods_armed,
        .mods_locked = st->mods_locked,
        .caps_led_known = st->caps_led_known,
        .caps_led_active = st->caps_led_active,
        .maintenance = st->maintenance,
        .error_code = st->error_code,
    };
    const ui_net_slice_t net = {.client_state = st->client_state, .wifi_connected = st->wifi_connected};
    const ui_touch_slice_t touch = {.touch_healthy = st->touch_healthy};
    const ui_gesture_slice_t gesture = {.drag_lock_active = st->drag_lock_active};
    ui_mailbox_publish_hid(&s->mb, &hid, seq);
    ui_mailbox_publish_net(&s->mb, &net, seq);
    ui_mailbox_publish_touch(&s->mb, &touch, seq);
    ui_mailbox_publish_gesture(&s->mb, &gesture, seq);
    ui_store_tick(&s->store);
    lv_test_wait(10);
}

void sim_settle(void)
{
    lv_test_wait(30);
    // The UI uses no lv_anim at all (07 §9: motion is optional and this image has none), so one wait is enough;
    // the loop is here so a future animation cannot silently leak a half-drawn frame into a golden.
    for (int i = 0; i < 40 && lv_anim_count_running() > 0; i++) {
        lv_test_wait(16);
    }
}

// ---- touch playback ---------------------------------------------------------------------------------------------

static bool center_of(lv_obj_t *obj, int32_t *x, int32_t *y)
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

void sim_press_point(int32_t x, int32_t y)
{
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_test_wait(50);
}

void sim_press_obj(lv_obj_t *obj)
{
    int32_t x = 0, y = 0;
    if (center_of(obj, &x, &y)) {
        sim_press_point(x, y);
    }
}

void sim_release(void)
{
    lv_test_mouse_release();
    lv_test_wait(50);
}

void sim_click_obj(lv_obj_t *obj)
{
    sim_press_obj(obj);
    sim_release();
}

void sim_long_press_obj(lv_obj_t *obj)
{
    // LV_INDEV_DEF_LONG_PRESS_TIME is 400 ms; 600 ms of hold leaves room for the 30 ms test tick.
    sim_press_obj(obj);
    lv_test_wait(600);
    sim_release();
}

void sim_flick_obj(lv_obj_t *obj, uint8_t dir)
{
    static const int8_t k_dx[5] = {0, -40, 0, 40, 0};
    static const int8_t k_dy[5] = {0, 0, -40, 0, 40};
    int32_t x = 0, y = 0;
    if (dir > 4u || !center_of(obj, &x, &y)) {
        return;
    }
    sim_press_point(x, y);
    for (int32_t s = 1; s <= 4; s++) {
        lv_test_mouse_move_to(x + k_dx[dir] * s / 4, y + k_dy[dir] * s / 4);
        lv_test_wait(20);
    }
    sim_release();
}
// ---- log queries ------------------------------------------------------------------------------------------------

uint32_t sim_count_action(const sim_app_t *s, ui_action_type_t type, uint16_t value)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->log_n; i++) {
        if (s->log[i].kind == SIM_LOG_ACTION && s->log[i].type == type &&
            (value == SIM_ANY_VALUE || s->log[i].value == value)) {
            n++;
        }
    }
    return n;
}

uint32_t sim_first_action_seq(const sim_app_t *s, ui_action_type_t type, uint16_t value)
{
    for (uint32_t i = 0; i < s->log_n; i++) {
        if (s->log[i].kind == SIM_LOG_ACTION && s->log[i].type == type &&
            (value == SIM_ANY_VALUE || s->log[i].value == value)) {
            return s->log[i].seq;
        }
    }
    return 0;
}

uint32_t sim_last_pad_seq(const sim_app_t *s)
{
    uint32_t seq = 0;
    for (uint32_t i = 0; i < s->log_n; i++) {
        if (s->log[i].kind == SIM_LOG_PAD) {
            seq = s->log[i].seq;
        }
    }
    return seq;
}

uint32_t sim_domain_actions(const sim_app_t *s)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->log_n; i++) {
        n += s->log[i].kind == SIM_LOG_ACTION;
    }
    return n;
}

uint32_t sim_count_remote(const sim_app_t *s, ui_remote_action_t act)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->log_n; i++) {
        n += s->log[i].kind == SIM_LOG_REMOTE && s->log[i].remote_act == (uint8_t)act;
    }
    return n;
}

uint32_t sim_count_setting(const sim_app_t *s, ui_setting_id_t id)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->log_n; i++) {
        n += s->log[i].kind == SIM_LOG_SETTING && s->log[i].setting_id == (uint8_t)id;
    }
    return n;
}

void sim_log_reset(sim_app_t *s)
{
    s->log_n = 0;
    s->log_lost = 0;
}

uint32_t sim_submit(sim_app_t *s, ui_action_type_t type, uint16_t control_id, uint16_t value)
{
    return ui_actions_submit(&s->actions, type, control_id, value, sim_now(), NULL);
}
