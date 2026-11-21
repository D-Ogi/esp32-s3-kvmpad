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
#include "kvm_control_icons.h"
#include "ui_theme.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hid_core.h"
#include "hid_dispatch.h"
#if KVM_HID_HAS_HPE
#include "hpe_studio.h"
#endif
#include "hid_keymap.h"
#include "ime_glue.h"
#include "kbd_layouts.h" // FR-741: the picker reports an index into the generated tables
#include "lvgl.h"
#include "remote_glue.h"
#include "ui_app.h"
#include "ui_glue.h"
#include "ui_overlay.h"
#include "ui_platform.h"
#include "ui_touch_bridge.h"
#include "pack_panel.h"
#include "hpe_panel.h"
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#include "media_panel.h"
#endif
#include "host_panel.h"
#include "kvm_hid_intent.h"
#include "ui_tokens.h"

static const char *TAG = "ui_shell";

#define LOCK_MS 200
#define REMOTE_POLL_MS 100

static ui_app_t s_app;
static bool s_built;
static bool s_suspending;
static bool s_tools_active;
static lv_obj_t *s_tools_screen;
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

#if KVM_HID_HAS_HPE
static void sync_shell_profile(void)
{
    const bool settings = ui_overlay_top(&s_app.router) == UI_OVERLAY_SETTINGS;
    const bool keyboard_only = s_app.router.active == UI_VIEW_KEYBOARD && s_app.keyboard.workspace == UI_KBD_FULL;
    // Windows can request native precision contacts at any time. A workspace with a pad must accept them.
    hid_dispatch_set_profile(settings ? HPE_SETTINGS_PROFILE : keyboard_only ? HPE_KEYBOARD_PROFILE : HPE_MOUSE_PROFILE);
}
#endif

static void pad_changed(void *ctx, const lv_area_t *pad)
{
    (void)ctx;
    if (!on_screen()) {
        return; // a bench scene owns the pad rectangle while the shell is off screen
    }
#if KVM_HID_HAS_HPE
    sync_shell_profile();
#endif
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
    case UI_SETTING_PALETTE:
        ok = v < UI_PALETTE_COUNT && app_config_set_control_palette((uint8_t)v);
        break;
    case UI_SETTING_BRIGHTNESS:
        ok = app_config_set_field(KVM_CFG_F_BRIGHTNESS, v);
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
    uint8_t palette = app_config_control_palette();
    if (seq == 0 || (seq == s_settings_seq && palette == s_settings_view.palette)) {
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
    v->brightness = s_cfg_copy.brightness;
    v->palette = palette;
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
#if CONFIG_KVM_USB_MONITOR_EVAL
    bool native;
    if (remote_glue_take_display_profile(&native)) pack_panel_remote_monitor_mode(native);
#endif
    ui_theme_set_control_palette((ui_control_palette_t)app_config_control_palette());
    if (on_screen()) {
#if KVM_HID_HAS_HPE
        sync_shell_profile();
#endif
        remote_project();
        settings_project();
        if (s_settings_view.seq != 0) {
            const bool local_composition = s_app.keyboard.text_mode_ok;
            ime_glue_sync(&s_app, local_composition && s_settings_view.lang_zh,
                          local_composition && s_settings_view.lang_ja, local_composition && s_settings_view.lang_ko);
        }
    } else {
        if (s_tools_active) {
            // A dictionary load can finish after the keyboard has left the screen.
            ime_glue_sync(&s_app, false, false, false);
        }
        s_remote_fresh = false; // a bench scene owns the screen: republish when the shell comes back
        s_settings_seq = 0;
    }
}

static void pack_home(void)
{
    s_tools_active = false;
    ui_touch_bridge_cancel();
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    lv_screen_load(s_app.screen);
    ui_router_show(&s_app.router, UI_VIEW_MOUSE);
    ui_app_show(&s_app);
    ui_touch_bridge_set_hid_enabled(!s_suspending);
}

enum { TOOLS_HOME, TOOLS_PROFILES, TOOLS_HOST };
static void open_device_tools(void);

static void leave_shell_input(void)
{
    ui_touch_bridge_cancel();
    ui_touch_bridge_set_hid_enabled(false);
    ui_app_leave_for_extensions(&s_app);
    s_tools_active = true;
    ime_glue_sync(&s_app, false, false, false);
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
}

esp_err_t ui_shell_browse_card(void)
{
    if (!s_built)
        return ESP_ERR_INVALID_STATE;
    if (!ui_platform_lock(LOCK_MS))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (on_screen() && !s_app.router.depth) {
        leave_shell_input();
        if (pack_panel_open(pack_home,open_device_tools))
            result = ESP_OK;
        else {
            pack_home();
            result = ESP_ERR_NO_MEM;
        }
    }
    ui_platform_unlock();
    return result;
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static bool synthetic_media;

static bool help_state(bool open)
{
    return on_screen() && s_app.router.active == UI_VIEW_MOUSE &&
           (open ? s_app.router.depth == 1 && s_app.router.stack[0].d->id == UI_OVERLAY_GESTURE_HELP &&
                   s_app.mouse.help_card : !s_app.router.depth && !s_app.mouse.help_card);
}

bool ui_shell_help_state(bool open)
{
    if (!ui_platform_lock(LOCK_MS)) return false;
    bool ok = help_state(open);
    ui_platform_unlock();
    return ok;
}

static void help_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    const touch_route_out_t *out = lv_indev_get_user_data(indev);
    data->point = (lv_point_t){out->ptr_x, out->ptr_y};
    data->state = out->ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_obj_t *help_close_button(lv_obj_t *parent)
{
    if (lv_obj_check_type(parent, &lv_label_class) &&
        !strcmp(lv_label_get_text(parent), "Close") && lv_obj_check_type(lv_obj_get_parent(parent), &lv_button_class))
        return lv_obj_get_parent(parent);
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *button = help_close_button(lv_obj_get_child(parent, i));
        if (button) return button;
    }
    return NULL;
}

// Only the LVGL task runs the pointer and button callbacks. The console holds
// no pointer to a timer, object or stack data that can expire on timeout.
static struct {
    lv_timer_t *timer;
    lv_indev_t *indev;
    lv_display_t *display;
    touch_router_t router;
    touch_route_out_t contact;
    kvm_touch_sample_t sample;
    uint64_t deadline, pressed_at;
    uint32_t hold_us;
    bool open, painted;
    esp_err_t result;
} help_test;

static void help_painted(lv_event_t *event)
{
    (void)event;
    help_test.painted = true;
}

static void help_finish(esp_err_t result)
{
    if (help_test.indev) {
        lv_display_remove_event_cb_with_user_data(help_test.display, help_painted, &help_test);
        lv_indev_reset(help_test.indev, NULL);
        lv_indev_delete(help_test.indev);
        help_test.indev = NULL;
    }
    lv_timer_delete(help_test.timer);
    help_test.timer = NULL;
    help_test.result = result;
}

static bool help_press(void)
{
    lv_obj_t *button = help_test.open ? s_app.mouse.btn_help : help_close_button(s_app.mouse.help_card);
    if (!button) return false;
    lv_obj_update_layout(s_app.screen);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t *hit = lv_indev_search_obj(lv_display_get_layer_sys(disp), &point);
    if (!hit) hit = lv_indev_search_obj(lv_display_get_layer_top(disp), &point);
    if (!hit) hit = lv_indev_search_obj(s_app.screen, &point);
    if (hit != button) return false;

    touch_router_init(&help_test.router);
    if (ui_router_pad_area(&s_app.router, &area)) {
        touch_rect_t pad = {area.x1, area.y1, area.x2, area.y2};
        touch_router_set_pad(&help_test.router, &pad, true);
    }
    help_test.sample = (kvm_touch_sample_t){.valid = true, .timestamp_us = now_us(), .sample_seq = 1, .point_count = 1};
    help_test.sample.point[0].x_px = point.x;
    help_test.sample.point[0].y_px = point.y;
    touch_router_route(&help_test.router, &help_test.sample, &help_test.contact);
    if (!help_test.contact.ptr_pressed || help_test.contact.pad.point_count) return false;
    help_test.display = disp;
    help_test.indev = lv_indev_create();
    if (!help_test.indev) return false;
    lv_indev_set_type(help_test.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(help_test.indev, disp);
    lv_indev_set_mode(help_test.indev, LV_INDEV_MODE_EVENT);
    lv_indev_set_user_data(help_test.indev, &help_test.contact);
    lv_indev_set_read_cb(help_test.indev, help_pointer_read);
    if (!lv_display_add_event_cb(disp, help_painted, LV_EVENT_RENDER_READY, &help_test)) return false;
    help_test.pressed_at = now_us();
    lv_indev_read(help_test.indev);
    lv_obj_invalidate(button);
    return true;
}

static void help_step(lv_timer_t *timer)
{
    (void)timer;
    uint64_t now = now_us();
    if (now >= help_test.deadline || !help_state(!help_test.open)) {
        help_finish(ESP_ERR_TIMEOUT);
        return;
    }
    if (!help_test.indev) {
        if (!help_press()) help_finish(ESP_FAIL);
        return;
    }
    if (!help_test.painted || now - help_test.pressed_at < 90000) return;
    help_test.sample.point_count = 0;
    ++help_test.sample.sample_seq;
    help_test.sample.timestamp_us = now;
    touch_router_route(&help_test.router, &help_test.sample, &help_test.contact);
    lv_indev_read(help_test.indev);
    help_test.hold_us = (uint32_t)(now - help_test.pressed_at);
    help_finish(ESP_OK);
}

esp_err_t ui_shell_help_touch(bool open)
{
    if (!ui_platform_lock(LOCK_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!help_test.timer && help_state(!open)) {
        memset(&help_test, 0, sizeof(help_test));
        help_test.open = open;
        help_test.deadline = now_us() + 2000000;
        help_test.timer = lv_timer_create(help_step, 8, NULL);
        result = help_test.timer ? ESP_OK : ESP_ERR_NO_MEM;
    }
    ui_platform_unlock();
    return result;
}

esp_err_t ui_shell_help_completion(uint32_t *hold_us)
{
    if (!ui_platform_lock(LOCK_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t result = help_test.timer ? ESP_ERR_NOT_FINISHED : help_test.result;
    *hold_us = help_test.hold_us;
    ui_platform_unlock();
    return result;
}

static esp_err_t start_card_test(const char *file, const char *mode, const char *source, bool pixel_size)
{
    if (!s_built)
        return ESP_ERR_INVALID_STATE;
    if (!ui_platform_lock(LOCK_MS))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (on_screen() && !s_app.router.depth && s_app.router.active == UI_VIEW_MOUSE) {
        leave_shell_input();
        if (file ? pack_panel_card_run(pack_home, file, mode, source, pixel_size) : pack_panel_card_reference(pack_home))
            result = ESP_OK;
        else {
            if (s_tools_active)
                pack_home();
            result = ESP_FAIL;
        }
    }
    ui_platform_unlock();
    return result;
}

esp_err_t ui_shell_card_reference(void) { return start_card_test(NULL, NULL, NULL, false); }

esp_err_t ui_shell_card_run(const char *file, const char *mode, const char *source, bool pixel_size)
{
    return file && mode && source ? start_card_test(file, mode, source, pixel_size) : ESP_ERR_INVALID_ARG;
}

esp_err_t ui_shell_card_action(const char *action)
{
    if (!s_built || !ui_platform_lock(LOCK_MS))
        return ESP_ERR_INVALID_STATE;
    bool clicked = pack_panel_card_action(action);
    ui_platform_unlock();
    return clicked ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t ui_shell_card_home(void)
{
    if (!s_built || !ui_platform_lock(LOCK_MS))
        return ESP_ERR_INVALID_STATE;
    bool left = pack_panel_card_home();
    ui_platform_unlock();
    return left ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static void synthetic_home(void)
{
    synthetic_media = false;
    pack_home();
}

static esp_err_t open_synthetic_media(unsigned fps, unsigned frames, kvm_live_codec_t codec, bool pattern, bool paced)
{
    if (!s_built)
        return ESP_ERR_INVALID_STATE;
    if (!ui_platform_lock(LOCK_MS))
        return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (!synthetic_media && on_screen() && !s_app.router.depth) {
        leave_shell_input();
        synthetic_media = pattern ? media_panel_pixel_pattern(synthetic_home)
                          : codec == KVM_LIVE_JPEG ? media_panel_synthetic_jpeg_paced(fps, frames, synthetic_home)
                          : paced ? media_panel_synthetic_h264_paced(fps, frames, synthetic_home)
                          : codec == KVM_LIVE_H264_INTRA ? media_panel_synthetic_h264_start(fps, frames, synthetic_home)
                               : media_panel_synthetic_start(fps, frames, synthetic_home);
        result = synthetic_media ? ESP_OK : ESP_ERR_NO_MEM;
        if (!synthetic_media)
            pack_home();
    }
    ui_platform_unlock();
    return result;
}

esp_err_t ui_shell_synthetic_media(unsigned fps, unsigned frames, kvm_live_codec_t codec, bool paced)
{
    return open_synthetic_media(fps, frames, codec, false, paced);
}

esp_err_t ui_shell_pixel_pattern(bool scan)
{
    if (!scan) return open_synthetic_media(0, 0, KVM_LIVE_JPEG, true, false);
    if (!s_built || !ui_platform_lock(LOCK_MS)) return ESP_ERR_INVALID_STATE;
    bool ok = synthetic_media && media_panel_pixel_pattern_scan();
    ui_platform_unlock();
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void ui_shell_stop_synthetic_media(void)
{
    if (!s_built || !ui_platform_lock(LOCK_MS))
        return;
    if (synthetic_media) {
        media_panel_close();
        synthetic_home();
    }
    ui_platform_unlock();
}
#endif

static void tools_select(lv_event_t *event)
{
    unsigned choice = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (!s_tools_screen || lv_screen_active() != s_tools_screen)
        return;
    lv_screen_load(s_app.screen);
    lv_obj_delete_async(s_tools_screen);
    s_tools_screen = NULL;
    bool opened = false;
    if (choice == TOOLS_PROFILES)
        opened = hpe_panel_open(pack_home);
    if (choice == TOOLS_HOST)
        opened = host_panel_open(pack_home);
    if (!opened)
        pack_home();
}

static void tools_button(const char *name, const lv_image_dsc_t *mask, unsigned choice, int y)
{
    lv_obj_t *btn = lv_button_create(s_tools_screen);
    lv_obj_set_pos(btn, 24, y);
    lv_obj_set_size(btn, 360, 60);
    ui_theme_control_button(btn);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text_static(label, name);
    lv_obj_center(label);
    lv_obj_align(ui_theme_control_mask(btn, mask), LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(btn, tools_select, LV_EVENT_CLICKED, (void *)(uintptr_t)choice);
}

static void open_device_tools(void)
{
    if (!on_screen())
        return;
    leave_shell_input();
    s_tools_screen = lv_obj_create(NULL);
    ui_theme_apply(s_tools_screen, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    tools_button("Home", &kvm_icon_home, TOOLS_HOME, 16);
    tools_button("HOST Bridge", &kvm_icon_host, TOOLS_HOST, 100);
#if KVM_HID_HAS_HPE
    tools_button("HID profiles", &kvm_icon_keyboard, TOOLS_PROFILES, 184);
#endif
    lv_screen_load(s_tools_screen);
}

static void extensions_direct(lv_event_t *e)
{
    (void)e;
    ui_shell_browse_card();
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
        ui_theme_set_control_palette((ui_control_palette_t)app_config_control_palette());
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
            ui_app_add_extensions_button(&s_app, extensions_direct);
            ESP_LOGI(TAG, "shell built: %u views, nav %s", s_app.n_views, s_app.nav ? "yes" : "no");
        } else {
            ESP_LOGE(TAG, "shell build failed");
            err = ESP_FAIL;
        }
    }
    if (s_built) {
        s_tools_active = false;
        pack_panel_leave();
        host_panel_leave();
        if (lv_screen_active() != s_app.screen) {
            lv_screen_load(s_app.screen);
        }
        if (s_tools_screen) {
            lv_obj_delete_async(s_tools_screen);
            s_tools_screen = NULL;
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
        s_suspending = true;
        pack_panel_leave();
        host_panel_leave();
        if (s_tools_screen || !on_screen())
            lv_screen_load(s_app.screen);
        if (s_tools_screen) {
            lv_obj_delete_async(s_tools_screen);
            s_tools_screen = NULL;
        }
        hpe_panel_leave();
        if (on_screen()) {
            ui_app_leave(&s_app);
        }
        s_suspending = false;
        s_tools_active = false;
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

esp_err_t ui_shell_keyboard_workspace(ui_kbd_workspace_t workspace)
{
    if (!s_built || !ui_platform_lock(LOCK_MS)) return ESP_ERR_INVALID_STATE;
    const bool ok = on_screen() &&
        ui_actions_submit(ui_glue_actions(), UI_ACT_NAVIGATE, 0, UI_VIEW_KEYBOARD, now_us(), NULL) &&
        ui_view_keyboard_select_workspace(&s_app.keyboard, workspace);
    ui_platform_unlock();
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
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
