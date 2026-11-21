// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "host_panel.h"
#include "kvm_control_icons.h"
#include "kvmbridge.h"
#include "lvgl.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

static lv_obj_t *screen, *previous, *description, *feedback, *action_notice;
static lv_timer_t *refresh;
static void (*go_home)(void);

static void back(lv_event_t *e)
{
    (void)e;
    void (*home)(void)=go_home;
    host_panel_leave();
    if (home) home();
}
static void stop_click(lv_event_t *e)
{
    (void)e;
    lv_label_set_text_static(action_notice, "");
    kvmbridge_stop();
}
typedef struct {
    const char *caption;
    const char *command;
    bool obs, xplane;
} host_action_t;

static const host_action_t obs_actions[] = {
    {"OBS status", "obs.record.status", true, false},
    {"Start recording", "obs.record.start", true, false},
    {"Stop recording", "obs.record.stop", true, false},
    {"Select scene", "obs.scene.select", true, false},
    {"Mute input", "obs.input.mute", true, false},
    {"Unmute input", "obs.input.unmute", true, false},
};
static const host_action_t simulator_actions[] = {
    {"MSFS flaps up", "sim.flaps.up", false, false},
    {"MSFS flaps down", "sim.flaps.down", false, false},
    {"X-Plane flaps up", "sim.flaps.up", false, true},
    {"X-Plane flaps down", "sim.flaps.down", false, true},
};

static void action_click(lv_event_t *e)
{
    const host_action_t *a = lv_event_get_user_data(e);
    bool started = kvmbridge_action(a->obs, a->xplane, a->command);
    lv_label_set_text_static(action_notice, started ? "" : "Could not start this HOST action.");
}

static lv_obj_t *panel_label(const char *text, int x, int y, ui_font_role_t role)
{
    lv_obj_t *obj = lv_label_create(screen);
    lv_label_set_text_static(obj, text);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, ui_theme_font(role), 0);
    return obj;
}

static void button(const char *text, int x, int y, int width, const lv_image_dsc_t *mask,
                   lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_button_create(screen);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, width, 56);
    ui_theme_control_button(b);
    lv_obj_t *t = lv_label_create(b);
    lv_label_set_text_static(t, text);
    lv_obj_set_style_text_font(t, ui_theme_font(mask ? UI_FONT_BODY : UI_FONT_HEADING), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, mask ? 12 : 0, 0);
    if (mask)
        lv_obj_align(ui_theme_control_mask(b, mask), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
}

static void action_grid(const host_action_t *items, size_t count, int columns, int y)
{
    const int gap = 12;
    const int width = (976 - gap * (columns - 1)) / columns;
    for (size_t i = 0; i < count; ++i) {
        int x = 24 + ((int)i % columns) * (width + gap);
        int row_y = y + ((int)i / columns) * (56 + gap);
        button(items[i].caption, x, row_y, width, NULL, action_click, (void *)&items[i]);
    }
}
static void tick(lv_timer_t *unused)
{
    (void)unused;
    char endpoint[24], label[96], status[96];
    if (kvmbridge_host_endpoint(endpoint))
        snprintf(label,sizeof(label),"HOST ready at %s",endpoint);
    else
        snprintf(label,sizeof(label),"In KVMpad Client, connect to this device and choose Start HOST.");
    if (strcmp(lv_label_get_text(description),label)) lv_label_set_text(description,label);
    kvmbridge_status(status);
    if (strcmp(lv_label_get_text(feedback),status)) lv_label_set_text(feedback,status);
}
bool host_panel_open(void (*home)(void))
{
    if (screen) return false;
    previous=lv_screen_active(); go_home=home;
    screen=lv_obj_create(NULL);
    lv_obj_set_scrollable(screen, false);
    ui_theme_apply(screen,UI_STYLE_CONTROL_PAGE,LV_STATE_DEFAULT);
    button("Home", 24, 16, 134, &kvm_icon_home, back, NULL);
    panel_label("HOST Bridge", 180, 29, UI_FONT_TITLE);
    button("Stop session", 804, 16, 196, &kvm_icon_disconnect, stop_click, NULL);
    description = panel_label("", 24, 98, UI_FONT_HEADING);
    feedback = panel_label("", 24, 132, UI_FONT_BODY);
    lv_obj_set_width(feedback, 976);
    panel_label("OBS Studio", 24, 194, UI_FONT_HEADING);
    panel_label("Flight simulators", 24, 396, UI_FONT_HEADING);
    action_notice = panel_label("", 24, 526, UI_FONT_BODY);
    tick(NULL);
    action_grid(obs_actions, sizeof(obs_actions) / sizeof(obs_actions[0]), 3, 232);
    action_grid(simulator_actions, sizeof(simulator_actions) / sizeof(simulator_actions[0]), 4, 434);
    lv_screen_load(screen);
    refresh=lv_timer_create(tick,150,NULL);
    return true;
}
void host_panel_leave(void)
{
    if (!screen) return;
    kvmbridge_stop();
    if (refresh) lv_timer_delete(refresh);
    if (lv_screen_active()==screen && previous) lv_screen_load(previous);
    lv_obj_delete_async(screen); screen=NULL; previous=NULL; refresh=NULL; go_home=NULL;
}
