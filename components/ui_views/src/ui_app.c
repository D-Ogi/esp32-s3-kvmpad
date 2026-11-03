// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shell composition (07 §5); see ui_app.h.
#include "ui_app.h"

#include <string.h>

#include "hid_core.h"
#include "ui_contracts.h"
#include "ui_overlay.h"
#include "ui_status_item.h"
#include "ui_theme.h"
#include "ui_tokens.h"
#include "ui_topbar.h"

// 01 §6.1: HOST USB states (kvm_host_usb_t).
static const ui_status_row_t s_host_rows[] = {
    {KVM_HOST_USB_UNINITIALIZED, "Initializing", UI_SEVERITY_UNKNOWN},
    {KVM_HOST_USB_DETACHED, "Disconnected", UI_SEVERITY_OFFLINE},
    {KVM_HOST_USB_ENUMERATING, "Connecting", UI_SEVERITY_WARNING},
    {KVM_HOST_USB_MOUNTED, "Active", UI_SEVERITY_OK},
    {KVM_HOST_USB_SUSPENDED, "Suspended", UI_SEVERITY_WARNING},
    {KVM_HOST_USB_ERROR, "Error", UI_SEVERITY_OFFLINE},
};

// 01 §6.2: CLIENT states (ui_client_state_t). "Off" is a chosen state, not a fault.
static const ui_status_row_t s_client_rows[] = {
    {UI_CLIENT_DISABLED, "Off", UI_SEVERITY_UNKNOWN},
    {UI_CLIENT_NETWORK_DOWN, "Offline", UI_SEVERITY_OFFLINE},
    {UI_CLIENT_LISTENING, "Available", UI_SEVERITY_OK},
    {UI_CLIENT_AUTHENTICATING, "Connecting", UI_SEVERITY_WARNING},
    {UI_CLIENT_CONNECTED, "Connected", UI_SEVERITY_OK},
    {UI_CLIENT_CONTROL_GRANTED, "Controlling", UI_SEVERITY_OK},
    {UI_CLIENT_STALE, "Reconnecting", UI_SEVERITY_WARNING},
};

static const ui_status_desc_t s_host_desc = {
    .title = "Host",
    .rows = s_host_rows,
    .row_count = sizeof(s_host_rows) / sizeof(s_host_rows[0]),
    .control_id = UI_CTRL_STATUS_HOST,
    .clickable = true,
};

static const ui_status_desc_t s_client_desc = {
    .title = "Client",
    .rows = s_client_rows,
    .row_count = sizeof(s_client_rows) / sizeof(s_client_rows[0]),
    .control_id = UI_CTRL_STATUS_CLIENT,
    .clickable = true,
};

static const ui_topbar_desc_t s_bar_desc = {
    .brand = "KVMpad",
    .status = {[UI_TOPBAR_HOST] = &s_host_desc, [UI_TOPBAR_CLIENT] = &s_client_desc},
};

static const struct {
    ui_view_id_t id;
    const char *label;
    const char *icon;
} s_nav_names[] = {
    {UI_VIEW_MOUSE, "Mouse", LV_SYMBOL_GPS},
    {UI_VIEW_KEYBOARD, "Keyboard", LV_SYMBOL_KEYBOARD},
    {UI_VIEW_SHORTCUTS, "Shortcuts", LV_SYMBOL_LIST},
};

static uint64_t now_of(const ui_app_t *app)
{
    return app->hooks.now_us ? app->hooks.now_us() : 0;
}

// --- pad and router hooks -----------------------------------------------------------------------------------------

void ui_app_refresh_pad(ui_app_t *app)
{
    if (!app || !app->ready || !app->hooks.pad_changed) {
        return;
    }
    lv_area_t a;
    if (ui_router_pad_area(&app->router, &a)) {
        app->hooks.pad_changed(app->hooks.ctx, &a);
    } else {
        app->hooks.pad_changed(app->hooks.ctx, NULL);
    }
}

static bool cancel_input(void *ctx, ui_cancel_reason_t why)
{
    ui_app_t *app = ctx;
    return app->hooks.cancel_input ? app->hooks.cancel_input(app->hooks.ctx, why) : true;
}

static void input_area_changed(void *ctx)
{
    ui_app_refresh_pad(ctx);
}

// --- local decisions about a remote client (ADR-022 / ADR-023) ----------------------------------------------------

static void remote_action(void *ctx, ui_remote_action_t act, uint64_t session_id)
{
    ui_app_t *app = ctx;
    app->c.remote_actions++;
    if (app->hooks.remote_action) {
        app->hooks.remote_action(app->hooks.ctx, act, session_id);
    }
}

/** The CLIENT details card or Settings asked for a pairing window; the card has already dismissed itself. */
static void pair_requested(void *ctx)
{
    remote_action(ctx, UI_REMOTE_ACT_PAIR_OPEN, 0u);
}

// --- Settings (FR-731, SEC-107) -----------------------------------------------------------------------------------

/** One accepted Settings change on its way to the domain adapter. The shell adds no policy of its own. */
static void settings_change(void *ctx, ui_setting_id_t id, int32_t value, uint8_t client_index)
{
    ui_app_t *app = ctx;
    app->c.settings_changes++;
    if (app->hooks.settings_change) {
        app->hooks.settings_change(app->hooks.ctx, id, value, client_index);
    }
}

/**
 * ADR-039 section 8: a layout chosen in the KEYBOARD view's quick switch. It is the same typed change the
 * Settings picker reports, through the same hook, so there is one write path for one record field.
 */
static void kbd_layout_picked(void *ctx, int32_t table_index)
{
    settings_change(ctx, UI_SETTING_HOST_LAYOUT, table_index, 0u);
}

static void settings_clicked(lv_event_t *e)
{
    ui_app_t *app = lv_event_get_user_data(e);
    ui_actions_submit(app->actions, UI_ACT_OPEN_SETTINGS, UI_CTRL_SETTINGS_OPEN, 0, now_of(app), NULL);
}

static void add_overlays(ui_app_t *app, const ui_overlay_descriptor_t *ov, uint8_t n)
{
    const uint8_t cap = (uint8_t)(sizeof(app->overlays) / sizeof(app->overlays[0]));
    for (uint8_t i = 0; i < n && app->n_overlays < cap; i++) {
        app->overlays[app->n_overlays++] = ov[i];
    }
}

// --- navigation and indicators ------------------------------------------------------------------------------------

static void sync_nav(ui_app_t *app)
{
    if (!app->nav) {
        return;
    }
    const int active = ui_router_active(&app->router);
    int32_t index = -1;
    for (uint16_t i = 0; i < app->nav_desc.count; i++) {
        if ((int)app->segment_view[i] == active) {
            index = i;
        }
    }
    ui_segmented_set_active(app->nav, index); // -1 clears it: never a guessed view
}

static void nav_clicked(uint16_t control_id, uint16_t index, void *user_data)
{
    ui_app_t *app = user_data;
    if (index >= app->nav_desc.count) {
        return;
    }
    app->c.nav_requests++;
    ui_actions_submit(app->actions, UI_ACT_NAVIGATE, control_id, (uint16_t)app->segment_view[index], now_of(app),
                      NULL);
}

static void status_clicked(lv_event_t *e)
{
    ui_app_t *app = lv_event_get_user_data(e);
    lv_obj_t *item = lv_event_get_current_target(e);
    const bool host = item == ui_topbar_status_item(app->bar, UI_TOPBAR_HOST);
    app->c.details_requests++;
    ui_actions_submit(app->actions, host ? UI_ACT_OPEN_HOST_DETAILS : UI_ACT_OPEN_CLIENT_DETAILS,
                      host ? UI_CTRL_STATUS_HOST : UI_CTRL_STATUS_CLIENT, 0, now_of(app), NULL);
}

/** Local actions (07 §3): navigation and overlays. */
static bool local_sink(void *ctx, const ui_action_t *a)
{
    ui_app_t *app = ctx;
    bool ok = false;
    switch (a->type) {
    case UI_ACT_NAVIGATE:
        ok = ui_router_show(&app->router, (ui_view_id_t)a->value);
        sync_nav(app);
        break;
    case UI_ACT_OPEN_HOST_DETAILS:
        ok = ui_overlay_show(&app->router, UI_OVERLAY_HOST_DETAILS);
        break;
    case UI_ACT_OPEN_CLIENT_DETAILS:
        ok = ui_overlay_show(&app->router, UI_OVERLAY_CLIENT_DETAILS);
        break;
    case UI_ACT_OPEN_SETTINGS:
        ok = ui_overlay_show(&app->router, UI_OVERLAY_SETTINGS);
        break;
    case UI_ACT_DISMISS_OVERLAY:
        ui_overlay_dismiss(&app->router);
        ok = true;
        break;
    default:
        break;
    }
    if (!ok) {
        app->c.local_refused++;
    }
    ui_app_refresh_pad(app);
    return ok;
}

// --- build --------------------------------------------------------------------------------------------------------

static void add_view(ui_app_t *app, const ui_view_descriptor_t *d)
{
    if (app->n_views < UI_VIEW_COUNT) {
        app->views[app->n_views++] = *d;
    }
}

static void build_nav(ui_app_t *app)
{
    uint16_t n = 0;
    for (size_t i = 0; i < sizeof(s_nav_names) / sizeof(s_nav_names[0]); i++) {
        for (uint8_t v = 0; v < app->n_views; v++) {
            if (app->views[v].id == s_nav_names[i].id) {
                app->segments[n] = (ui_segment_t){.label = s_nav_names[i].label,
                                                  .icon = s_nav_names[i].icon,
                                                  .control_id = (uint16_t)(UI_CTRL_NAV_BASE + s_nav_names[i].id)};
                app->segment_view[n] = s_nav_names[i].id;
                n++;
            }
        }
    }
    app->nav_desc = (ui_segmented_desc_t){.segments = app->segments, .count = n};
    app->nav_sink = (ui_segmented_sink_t){.fn = nav_clicked, .user_data = app};
    lv_obj_t *slot = ui_topbar_nav_slot(app->bar);
    app->nav = slot && n ? ui_segmented_create(slot, &app->nav_desc, &app->nav_sink) : NULL;
}

/** The gear of the top bar's trailing slot: it submits UI_ACT_OPEN_SETTINGS and navigates nothing itself. */
static void build_settings_button(ui_app_t *app)
{
    lv_obj_t *slot = ui_topbar_trailing_slot(app->bar);
    if (!slot) {
        return;
    }
    lv_obj_t *btn = lv_button_create(slot);
    lv_obj_remove_style_all(btn);
    ui_theme_apply_button(btn, UI_BTN_QUIET);
    ui_theme_apply(btn, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
    lv_obj_set_size(btn, UI_TOPBAR_BTN_W, UI_TOPBAR_BTN_W);
    lv_obj_set_scrollable(btn, false);
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text_static(icon, LV_SYMBOL_SETTINGS);
    lv_obj_center(icon);
    lv_obj_add_event_cb(btn, settings_clicked, LV_EVENT_CLICKED, app);
    app->settings_btn = btn;
}

bool ui_app_init(ui_app_t *app, ui_store_t *store, ui_actions_t *actions, const ui_app_hooks_t *hooks)
{
    if (!app) {
        return false;
    }
    memset(app, 0, sizeof(*app)); // a refused init leaves a struct that ui_app_deinit() accepts as a no-op
    if (!store || !actions || !lv_is_initialized() || !ui_theme_init()) {
        return false;
    }
    app->store = store;
    app->actions = actions;
    if (hooks) {
        app->hooks = *hooks;
    }

    ui_view_mouse_init(&app->mouse, store, actions, app->hooks.now_us);
    ui_view_mouse_bind_router(&app->mouse, &app->router); // the gesture card (UI_OVERLAY_GESTURE_HELP)
    add_view(app, &app->mouse.desc);
    ui_view_keyboard_init(&app->keyboard, store, actions, app->hooks.now_us);
    ui_view_keyboard_bind_router(&app->keyboard, &app->router); // the pickers (language, HOST layout)
    if (app->hooks.settings_change) {
        // Without the hook the strip offers nothing instead of a card that could not change anything.
        ui_view_keyboard_set_layout_action(&app->keyboard, kbd_layout_picked, app);
    }
    add_view(app, &app->keyboard.desc);
    ui_view_shortcuts_init(&app->shortcuts, store, actions, app->hooks.now_us, app->hooks.layout_chord,
                           app->hooks.confirm_alt_f4, app->hooks.ctx);
    add_view(app, &app->shortcuts.desc);
    ui_view_status_init(&app->status, store, actions, &app->router, app->hooks.now_us);
    // Without a remote hook there is no pairing path at all: no entry point in the CLIENT card and prompt
    // buttons that report nothing (the adapter that would publish a pairing state is the one missing).
    ui_remote_view_init(&app->remote, &app->router, app->hooks.remote_action ? remote_action : NULL, app);
    if (app->hooks.remote_action) {
        ui_view_status_set_pair_action(&app->status, pair_requested, app);
    }
    // Settings exists whatever the hooks are, because About (FR-751) and the published values are worth
    // reading on their own; without a change hook every control is disabled rather than inert.
    ui_view_settings_init(&app->settings, actions, &app->router, app->hooks.now_us,
                          app->hooks.settings_change ? settings_change : NULL, app);
    if (app->hooks.remote_action) {
        ui_view_settings_set_pair_action(&app->settings, pair_requested, app);
    }
    uint8_t n = 0;
    add_overlays(app, ui_view_mouse_overlays(&app->mouse, &n), n);
    add_overlays(app, ui_view_keyboard_overlays(&app->keyboard, &n), n);
    add_overlays(app, ui_view_status_overlays(&app->status, &n), n);
    add_overlays(app, ui_remote_view_overlays(&app->remote, &n), n);
    add_overlays(app, ui_view_settings_overlays(&app->settings, &n), n);

    app->screen = lv_obj_create(NULL);
    if (!app->screen) {
        return false;
    }
    lv_obj_remove_style_all(app->screen);
    ui_theme_apply(app->screen, UI_STYLE_SCREEN, LV_PART_MAIN);
    lv_obj_set_scrollable(app->screen, false);

    // The same scrim the UiModal of ui_components paints, so the two blocking surfaces dim the screen alike.
    const ui_shell_layout_t layout = {.width = UI_SCREEN_W,
                                      .height = UI_SCREEN_H,
                                      .topbar_h = UI_TOPBAR_H,
                                      .scrim_color = UI_COLOR_INK,
                                      .scrim_opa = UI_SCRIM_OPA};
    const ui_nav_hooks_t nav_hooks = {.cancel_input = cancel_input, .input_area_changed = input_area_changed,
                                      .ctx = app};
    if (!ui_router_init(&app->router, app->screen, &layout, store, app->views, app->n_views, app->overlays,
                        app->n_overlays, &nav_hooks)) {
        lv_obj_delete(app->screen);
        memset(app, 0, sizeof(*app));
        return false;
    }
    app->ready = true; // from here on ui_app_deinit() undoes everything
    ui_view_shortcuts_set_layers(&app->shortcuts, NULL, app->router.toast_layer); // modal stays on the screen

    const ui_topbar_bind_t bind = {
        .status = {[UI_TOPBAR_HOST] = ui_store_subject(store, UI_SUBJ_HOST_USB),
                   [UI_TOPBAR_CLIENT] = ui_store_subject(store, UI_SUBJ_CLIENT)},
    };
    app->bar = ui_topbar_create(app->router.topbar_slot, &s_bar_desc, &bind);
    if (app->bar) {
        for (int slot = 0; slot < UI_TOPBAR_STATUS_COUNT; slot++) {
            lv_obj_t *item = ui_topbar_status_item(app->bar, (ui_topbar_status_t)slot);
            if (item) {
                lv_obj_add_event_cb(item, status_clicked, LV_EVENT_CLICKED, app);
            }
        }
        build_nav(app);
        build_settings_button(app);
    }
    ui_actions_set_local_sink(actions, local_sink, app);
    return true;
}

void ui_app_deinit(ui_app_t *app)
{
    if (!app || !app->ready) {
        return;
    }
    ui_actions_set_local_sink(app->actions, NULL, NULL);
    ui_remote_view_close(&app->remote); // drop the outcome toast before its layer goes
    ui_router_deinit(&app->router);      // dismisses overlays, leaves and destroys the views
    lv_obj_delete(app->screen);     // the top bar and the switcher go with it
    if (app->hooks.pad_changed) {
        app->hooks.pad_changed(app->hooks.ctx, NULL);
    }
    memset(app, 0, sizeof(*app));
}

void ui_app_show(ui_app_t *app)
{
    if (!app || !app->ready) {
        return;
    }
    if (ui_router_active(&app->router) < 0) {
        ui_router_show(&app->router, UI_VIEW_MOUSE);
    }
    sync_nav(app);
    lv_obj_update_layout(app->screen);
    ui_app_refresh_pad(app);
}

void ui_app_set_pairing(ui_app_t *app, const ui_pair_view_t *v)
{
    if (!app || !app->ready || !v) {
        return;
    }
    if (v->window_open) {
        // The FR-734 confirmation has its own scrim outside the overlay stack, so it would cover the prompt.
        ui_view_shortcuts_dismiss_modal(&app->shortcuts);
    }
    ui_remote_view_set_pairing(&app->remote, v);
    ui_app_refresh_pad(app);
}

void ui_app_set_approval(ui_app_t *app, const ui_approval_view_t *v)
{
    if (!app || !app->ready || !v) {
        return;
    }
    if (v->pending) {
        ui_view_shortcuts_dismiss_modal(&app->shortcuts);
    }
    ui_remote_view_set_approval(&app->remote, v);
    ui_app_refresh_pad(app);
}

void ui_app_set_settings(ui_app_t *app, const ui_settings_view_t *v)
{
    if (!app || !app->ready || !v) {
        return;
    }
    ui_view_settings_set(&app->settings, v);
    // FR-747: the KO layer composes on this device only when a Unicode delivery mode can carry its text;
    // an unpublished record (seq 0) keeps it off (fail closed).
    ui_view_keyboard_set_text_mode(&app->keyboard,
                                   v->seq != 0u && v->unicode_mode != UI_UNICODE_MODE_HOST_IME &&
                                       v->unicode_mode < UI_UNICODE_MODE_COUNT);
    // ADR-037: the language key offers exactly what Settings turned on; an unpublished record leaves English
    // alone, which is also the record's default.
    ui_view_keyboard_set_languages(&app->keyboard, v->seq != 0u && v->lang_ko, v->seq != 0u && v->lang_zh,
                                   v->seq != 0u && v->lang_ja);
    // FR-741/742: the KEYBOARD view draws the layout the record holds. An unpublished record leaves the US ANSI
    // field, which is the record's own default, and an id this build does not ship changes nothing (the view
    // counts it); the legends never come from anywhere but the generated tables.
    ui_view_keyboard_set_host_layout(&app->keyboard, v->seq != 0u ? v->host_layout : "");
    // ADR-039 section 8: the quick switch offers the record's own two lists, in the record's order. An
    // unpublished record has nothing to offer, so the strip stays a label.
    _Static_assert(UI_SETTINGS_LAYOUT_CAP == UI_VIEW_KBD_LAYOUT_CAP, "one layout id width for both views");
    _Static_assert(UI_SETTINGS_FAV_MAX == UI_VIEW_KBD_FAV_MAX, "one favorites cap for both views");
    _Static_assert(UI_SETTINGS_RECENT_MAX == UI_VIEW_KBD_RECENT_MAX, "one recents cap for both views");
    ui_view_keyboard_set_host_lists(&app->keyboard, v->host_fav, v->seq != 0u ? v->host_fav_count : 0u,
                                    v->host_recent, v->seq != 0u ? v->host_recent_count : 0u);
}

void ui_app_set_zh_engine(ui_app_t *app, const ui_kbd_conv_engine_t *engine)
{
    if (!app || !app->ready) {
        return;
    }
    ui_view_keyboard_set_zh_engine(&app->keyboard, engine);
}

void ui_app_set_ja_engine(ui_app_t *app, const ui_kbd_conv_engine_t *engine)
{
    if (!app || !app->ready) {
        return;
    }
    ui_view_keyboard_set_conv_engine(&app->keyboard, UI_KBD_LAYER_JA, engine);
}

void ui_app_leave(ui_app_t *app)
{
    if (!app || !app->ready) {
        return;
    }
    ui_remote_view_close(&app->remote); // the outcome toast must not outlive the screen
    ui_overlay_dismiss_all(&app->router);
    const int active = ui_router_active(&app->router);
    for (uint8_t i = 0; i < app->n_views; i++) {
        if ((int)app->views[i].id == active && app->views[i].on_leave) {
            app->views[i].on_leave(app->views[i].ctx); // held buttons owe their UP before the screen goes
        }
    }
}
