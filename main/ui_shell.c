// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Firmware adapter of the product shell: the composition itself is ui_app (components/ui_views), shared with the
// desktop simulator. This file adds the platform: the recursive LVGL lock, the esp_timer clock, the touch bridge
// (pad rectangle, cancel, pad -> HID switch) and the RELEASE_ALL request through hid_dispatch. Every function takes
// the lock, so the console and the LVGL task may call it alike.
//
// TASK-100: a 100 ms LVGL timer projects the pairing / approval view of remote_glue into the two prompts, and a
// tap on them is queued back to remote_glue (never a transport call from the LVGL task). The code reaches the
// prompt label only; nothing here logs it (SEC-105).
#include "text_glue.h"
#include "ui_shell.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hid_core.h"
#include "hid_dispatch.h"
#include "hid_keymap.h"
#include "ime_glue.h"
#include "kbd_layouts.h" // FR-741: the picker reports an index into the generated tables
#include "lvgl.h"
#include "remote_glue.h"
#include "ui_app.h"
#include "ui_glue.h"
#include "ui_platform.h"
#include "ui_touch_bridge.h"

static const char *TAG = "ui_shell";

#define LOCK_MS 200
#define REMOTE_POLL_MS 100

static ui_app_t s_app;
static bool s_built;
static lv_timer_t *s_remote_timer;
static uint32_t s_remote_seq; // last remote_glue view projected; 0 forces a republish
static bool s_remote_fresh;
static uint32_t s_settings_seq; // last app_config_seq() published to Settings; 0 forces a republish
// Static: about 0.5 KB each, kept off the LVGL task stack.
static app_config_settings_t s_cfg_copy;
static ui_settings_view_t s_settings_view;

_Static_assert((int)UI_CONTROL_APPROVAL_AUTO == (int)KVM_CONTROL_APPROVAL_AUTO &&
                   (int)UI_CONTROL_APPROVAL_TRUSTED == (int)KVM_CONTROL_APPROVAL_TRUSTED &&
                   (int)UI_CONTROL_APPROVAL_ALWAYS == (int)KVM_CONTROL_APPROVAL_ALWAYS,
               "ui_control_approval_t mirrors kvm_control_approval_t");
_Static_assert(UI_SETTINGS_CLIENTS_MAX >= KVM_CONFIG_MAX_PAIRED, "every register entry fits the Settings list");

_Static_assert((int)UI_REMOTE_ACT_PAIR_OPEN == (int)REMOTE_GLUE_ACT_PAIR_OPEN &&
                   (int)UI_REMOTE_ACT_PAIR_CANCEL == (int)REMOTE_GLUE_ACT_PAIR_CANCEL &&
                   (int)UI_REMOTE_ACT_PAIR_ACCEPT == (int)REMOTE_GLUE_ACT_PAIR_ACCEPT &&
                   (int)UI_REMOTE_ACT_PAIR_REJECT == (int)REMOTE_GLUE_ACT_PAIR_REJECT &&
                   (int)UI_REMOTE_ACT_APPROVE == (int)REMOTE_GLUE_ACT_APPROVE &&
                   (int)UI_REMOTE_ACT_DENY == (int)REMOTE_GLUE_ACT_DENY,
               "ui_remote_action_t and remote_glue_action_t must stay one-to-one");
_Static_assert((int)UI_PAIR_RESULT_PAIRED == (int)REMOTE_GLUE_RESULT_PAIRED &&
                   (int)UI_PAIR_RESULT_REJECTED == (int)REMOTE_GLUE_RESULT_REJECTED &&
                   (int)UI_PAIR_RESULT_FAILED == (int)REMOTE_GLUE_RESULT_FAILED &&
                   (int)UI_PAIR_RESULT_LOCKED == (int)REMOTE_GLUE_RESULT_LOCKED &&
                   (int)UI_PAIR_RESULT_EXPIRED == (int)REMOTE_GLUE_RESULT_EXPIRED,
               "ui_pair_result_t and remote_glue_result_t must stay one-to-one");
_Static_assert(UI_PAIR_NAME_CAP == REMOTE_GLUE_NAME_CAP, "client name capacity");

static uint64_t now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static bool on_screen(void)
{
    return s_built && lv_screen_active() == s_app.screen;
}

static void pad_changed(void *ctx, const lv_area_t *pad)
{
    (void)ctx;
    if (!on_screen()) {
        return; // a bench scene owns the pad rectangle while the shell is off screen
    }
    if (pad) {
        const touch_rect_t r = {(int16_t)pad->x1, (int16_t)pad->y1, (int16_t)pad->x2, (int16_t)pad->y2};
        ui_touch_bridge_set_pad(&r, true);
    } else {
        ui_touch_bridge_set_pad(NULL, false); // no pad view, or a blocking overlay holds the input
    }
}

static bool confirm_alt_f4(void *ctx)
{
    (void)ctx;
    return app_config_confirm_alt_f4();
}

static bool cancel_input(void *ctx, ui_cancel_reason_t why)
{
    (void)ctx;
    (void)why;
    ui_touch_bridge_cancel();                     // pending touch and gesture state of the pad
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE); // the domain decides the USB state, the UI only asks
    return true;
}

static void remote_action(void *ctx, ui_remote_action_t act, uint64_t session_id)
{
    (void)ctx;
    if (!remote_glue_post_action((remote_glue_action_t)act, session_id)) {
        ESP_LOGW(TAG, "remote action %d not queued", (int)act); // the prompt stays and expires (fail closed)
    }
}

static void remote_project(void)
{
    remote_glue_view_t v;
    uint32_t seq = 0;
    remote_glue_get_view(&v, &seq);
    if (s_remote_fresh && seq == s_remote_seq) {
        return;
    }
    s_remote_seq = seq;
    s_remote_fresh = true;
    ui_pair_view_t p;
    memset(&p, 0, sizeof(p));
    p.window_open = v.window_open;
    memcpy(p.code, v.code, sizeof(p.code));
    p.code[sizeof(p.code) - 1u] = '\0';
    p.code_remaining_ms = v.code_remaining_ms;
    p.window_remaining_ms = v.window_remaining_ms;
    p.accept_pending = v.accept_pending;
    memcpy(p.client_name, v.client_name, sizeof(p.client_name));
    p.client_name[sizeof(p.client_name) - 1u] = '\0';
    p.accept_remaining_ms = v.accept_remaining_ms;
    p.result = (ui_pair_result_t)v.result;
    p.result_seq = v.result_seq;
    memcpy(p.result_name, v.result_name, sizeof(p.result_name));
    p.result_name[sizeof(p.result_name) - 1u] = '\0';
    ui_app_set_pairing(&s_app, &p);

    ui_approval_view_t a;
    memset(&a, 0, sizeof(a));
    a.pending = v.approval_pending;
    a.session_id = v.approval_session;
    memcpy(a.client_name, v.approval_name, sizeof(a.client_name));
    a.client_name[sizeof(a.client_name) - 1u] = '\0';
    a.remaining_ms = v.approval_remaining_ms;
    ui_app_set_approval(&s_app, &a);
    memset(&v, 0, sizeof(v)); // the code does not linger on the stack
    memset(&p, 0, sizeof(p));
}

// ---- Settings (TASK-070C) -------------------------------------------------------------------------
static void apply_keepawake(void)
{
    static app_config_settings_t k; // remote_glue task only (s_cfg_copy belongs to the LVGL task)
    if (app_config_settings(&k, 200)) {
        (void)hid_dispatch_set_keepalive(k.keepawake_enabled, k.keepawake_interval_s);
    }
}

// Runs in the remote_glue task: validate, write the record, and let the next projection show the result.
static void settings_apply(uint8_t id, int32_t value, uint8_t index)
{
    bool ok = false;
    const uint32_t v = value < 0 ? 0u : (uint32_t)value;
    switch ((ui_setting_id_t)id) {
    case UI_SETTING_REMOTE_ENABLED:
        ok = app_config_set_remote_enabled(v != 0);
        break;
    case UI_SETTING_PAIR_REQUIRE_ACCEPT:
        ok = app_config_set_field(KVM_CFG_F_PAIR_REQUIRE_ACCEPT, v != 0 ? 1u : 0u);
        break;
    case UI_SETTING_CONTROL_APPROVAL:
        ok = app_config_set_field(KVM_CFG_F_CONTROL_APPROVAL, v);
        break;
    case UI_SETTING_CONFIRM_ALT_F4:
        ok = app_config_set_field(KVM_CFG_F_CONFIRM_ALT_F4, v != 0 ? 1u : 0u);
        break;
    case UI_SETTING_KEEPAWAKE_ENABLED:
        ok = app_config_set_field(KVM_CFG_F_KEEPAWAKE_ENABLED, v != 0 ? 1u : 0u);
        if (ok) {
            apply_keepawake();
        }
        break;
    case UI_SETTING_KEEPAWAKE_INTERVAL_S:
        ok = app_config_set_field(KVM_CFG_F_KEEPAWAKE_INTERVAL_S, v);
        if (ok) {
            apply_keepawake();
        }
        break;
    case UI_SETTING_CLIENT_TRUSTED:
        ok = app_config_set_client_trusted(index, v != 0);
        break;
    case UI_SETTING_CLIENT_FORGET:
        ok = remote_glue_forget_client(index); // closes that client's sessions, then removes the entry
        break;
    case UI_SETTING_UNICODE_MODE:
        ok = app_config_set_field(KVM_CFG_F_UNICODE_MODE, v); // the record validates the range
        break;
    case UI_SETTING_LANG_KO: // ADR-037
        ok = app_config_set_field(KVM_CFG_F_LANG_KO, v != 0 ? 1u : 0u);
        break;
    case UI_SETTING_LANG_ZH:
        ok = app_config_set_field(KVM_CFG_F_LANG_ZH, v != 0 ? 1u : 0u);
        break;
    case UI_SETTING_LANG_JA:
        ok = app_config_set_field(KVM_CFG_F_LANG_JA, v != 0 ? 1u : 0u);
        break;
    case UI_SETTING_HOST_LAYOUT: { // FR-741: the picker reports an index into the generated tables
        const kbd_layout_t *lay = kbd_layouts_at((size_t)v);
        ok = lay != NULL && app_config_set_layout(lay->id);
        if (ok) {
            // ADR-039 section 4: what was just applied heads the recents. A refused list write does not
            // undo the layout - the two are separate records of the same choice.
            (void)app_config_note_layout_use(lay->id);
        }
        break;
    }
    case UI_SETTING_HOST_FAV_TOGGLE: { // ADR-039 section 3: the star, and the long press on a tile
        const kbd_layout_t *lay = kbd_layouts_at((size_t)v);
        ok = lay != NULL && app_config_pin_layout(lay->id);
        break;
    }
    default:
        break;
    }
    ESP_LOGI(TAG, "setting %u -> %s", (unsigned)id, ok ? "saved" : "refused"); // no value, no name
}

static void settings_change(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    (void)ctx;
    if (!remote_glue_post_setting(settings_apply, (uint8_t)id, value, client_index)) {
        ESP_LOGW(TAG, "setting %d not queued", (int)id); // the control stays put: nothing was written
    }
}

static void settings_project(void)
{
    const uint32_t seq = app_config_seq();
    if (seq == 0 || seq == s_settings_seq) {
        return;
    }
    if (!app_config_settings(&s_cfg_copy, 5)) {
        return; // the writer holds the record (flash write): try again on the next tick
    }
    ui_settings_view_t *v = &s_settings_view;
    memset(v, 0, sizeof(*v));
    v->seq = seq;
    v->remote_enabled = s_cfg_copy.remote_enabled;
    v->pair_require_accept = s_cfg_copy.pair_require_accept;
    v->control_approval = s_cfg_copy.control_approval;
    v->confirm_alt_f4 = s_cfg_copy.confirm_alt_f4;
    v->keepawake_enabled = s_cfg_copy.keepawake_enabled;
    v->keepawake_interval_s = s_cfg_copy.keepawake_interval_s;
    v->unicode_mode = s_cfg_copy.unicode_mode;
    text_glue_set_mode(s_cfg_copy.unicode_mode); // FR-747: the delivery mode follows the saved record
    v->lang_ko = s_cfg_copy.lang_ko;             // ADR-037: what the language key offers
    v->lang_zh = s_cfg_copy.lang_zh;
    v->lang_ja = s_cfg_copy.lang_ja;
    // snprintf: truncates and terminates (strncpy here tripped -Wstringop-truncation in the -O2 build)
    snprintf(v->host_layout, sizeof(v->host_layout), "%s", s_cfg_copy.host_layout);
    // ADR-039: the two lists are the record's, and the screen only draws them.
    v->host_fav_count = s_cfg_copy.host_fav_count <= UI_SETTINGS_FAV_MAX ? s_cfg_copy.host_fav_count
                                                                        : UI_SETTINGS_FAV_MAX;
    for (uint8_t i = 0; i < v->host_fav_count; i++) {
        snprintf(v->host_fav[i], sizeof(v->host_fav[i]), "%s", s_cfg_copy.host_fav[i]);
    }
    v->host_recent_count = s_cfg_copy.host_recent_count <= UI_SETTINGS_RECENT_MAX
                               ? s_cfg_copy.host_recent_count
                               : UI_SETTINGS_RECENT_MAX;
    for (uint8_t i = 0; i < v->host_recent_count; i++) {
        snprintf(v->host_recent[i], sizeof(v->host_recent[i]), "%s", s_cfg_copy.host_recent[i]);
    }
    // FR-741: the layouts are a generated table compiled into this image, so "available" is exactly "this build
    // has them". A build without them shows the stored id read-only (the view says why).
    v->layout_db_available = kbd_layouts_count_get() > 0u;
    snprintf(v->firmware_version, sizeof(v->firmware_version), "%s", esp_app_get_description()->version);
    v->client_count = s_cfg_copy.client_count;
    for (uint8_t i = 0; i < s_cfg_copy.client_count && i < UI_SETTINGS_CLIENTS_MAX; i++) {
        strncpy(v->client[i].name, s_cfg_copy.client[i].name, sizeof(v->client[i].name) - 1u);
        v->client[i].trusted = s_cfg_copy.client[i].trusted;
    }
    ui_app_set_settings(&s_app, v);
    s_settings_seq = seq;
}

static void remote_timer_cb(lv_timer_t *t)
{
    (void)t; // LVGL task, lock held
    ime_glue_install(&s_app); // TASK-072: a no-op until the background load is done, then once
    if (on_screen()) {
        remote_project();
        settings_project();
    } else {
        s_remote_fresh = false; // a bench scene owns the screen: republish when the shell comes back
        s_settings_seq = 0;
    }
}

esp_err_t ui_shell_show(void)
{
    if (!ui_glue_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ui_platform_lock(LOCK_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (!s_built) {
        // FR-744: the v1 CLIENT and the reference HOST are Windows (ADR-021); a HOST OS setting comes with Settings.
        const ui_app_hooks_t hooks = {.now_us = now_us,
                                      .cancel_input = cancel_input,
                                      .pad_changed = pad_changed,
                                      .confirm_alt_f4 = confirm_alt_f4,
                                      .remote_action = remote_action,
                                      .settings_change = settings_change,
                                      .layout_chord = KVM_CHORD_LAYOUT_WIN};
        s_built = ui_app_init(&s_app, ui_glue_store(), ui_glue_actions(), &hooks);
        if (s_built) {
            ESP_LOGI(TAG, "shell built: %u views, nav %s", s_app.n_views, s_app.nav ? "yes" : "no");
        } else {
            ESP_LOGE(TAG, "shell build failed");
            err = ESP_FAIL;
        }
    }
    if (s_built) {
        if (lv_screen_active() != s_app.screen) {
            lv_screen_load(s_app.screen);
        }
        ui_app_show(&s_app);
        s_remote_fresh = false;
        remote_project();
        s_settings_seq = 0;
        settings_project();
        if (s_remote_timer == NULL) {
            s_remote_timer = lv_timer_create(remote_timer_cb, REMOTE_POLL_MS, NULL);
        }
    }
    ui_platform_unlock();
    if (err == ESP_OK) {
        ui_touch_bridge_set_hid_enabled(true); // the shell owns the screen: pad gestures may reach the HOST
    }
    return err;
}

void ui_shell_suspend(void)
{
    ui_touch_bridge_cancel();
    ui_touch_bridge_set_hid_enabled(false);
    if (s_built && ui_platform_lock(LOCK_MS)) {
        if (on_screen()) {
            ui_app_leave(&s_app);
        }
        ui_platform_unlock();
    }
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
}

bool ui_shell_active(void)
{
    if (!s_built || !ui_platform_lock(LOCK_MS)) {
        return false;
    }
    const bool on = on_screen();
    ui_platform_unlock();
    return on;
}

bool ui_shell_refresh_pad(void)
{
    if (!s_built || !ui_platform_lock(LOCK_MS)) {
        return false;
    }
    const bool on = on_screen();
    if (on) {
        ui_app_refresh_pad(&s_app);
    }
    ui_platform_unlock();
    return on;
}

esp_err_t ui_shell_local_action(ui_action_type_t type, uint16_t value)
{
    if (!ui_action_is_local(type)) {
        return ESP_ERR_NOT_SUPPORTED; // never HID from a console line
    }
    if (!s_built || !ui_platform_lock(LOCK_MS)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (on_screen()) {
        const uint32_t id = ui_actions_submit(ui_glue_actions(), type, 0, value, now_us(), NULL);
        err = id != 0 ? ESP_OK : ESP_FAIL;
    }
    ui_platform_unlock();
    return err;
}

void ui_shell_status(ui_shell_counters_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!s_built || !ui_platform_lock(LOCK_MS)) {
        return;
    }
    out->router = s_app.router.c;
    out->app = s_app.c;
    out->mouse = s_app.mouse.c;
    out->keyboard = s_app.keyboard.c;
    out->shortcuts = s_app.shortcuts.c;
    out->details_opens = s_app.status.opens;
    out->details_closes = s_app.status.closes;
    out->views = s_app.n_views;
    ui_platform_unlock();
}
