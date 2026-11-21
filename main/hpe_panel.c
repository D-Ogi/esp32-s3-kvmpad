// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "hpe_panel.h"
#include "hpe_access_config.h"
#include "hid_dispatch.h"
#if KVM_HID_HAS_HPE
#include "lvgl.h"
#include "ui_theme.h"
#include <math.h>
#include <stdint.h>
#include <string.h>

enum { HOME, DIAL, STUDIO, ACCESS, OBS, TIMING };
enum { DIAL_RADIUS = 208, DIAL_BUTTON_RADIUS = 84 };
static void render(unsigned page);
static void show_access_state(void);
static lv_obj_t *bank_switch, *tiles[HPE_OBS_TILES];
static hpe_studio_status_t bank_status;
static int scan_latched = -1;
static bool paused, cooling, timing_loaded, select_down;
static uint32_t select_at, cooldown_at;
static lv_obj_t *access_keys[6], *access_tools[7], *timing_values[4], *timing_message;
static lv_obj_t *armed_tile;
static uint32_t armed_peer;
static hpe_access_timing_t timing = HPE_ACCESS_DEFAULTS;
static hpe_access_timing_t edit_timing;
static const char *const obs_names[] = {"Start recording", "Stop recording", "Mute input"};
static lv_obj_t *screen, *body, *status, *selection;
static lv_timer_t *timer;
static void (*home_fn)(void);
static unsigned view, sticky, scan_index;
static uint32_t generation, scan_at, pressed_at, sticky_at;
static bool switching, fine, turning, angle_valid, held, scanning, dwell, fired;
static int dwell_key;
static lv_point_t dwell_origin;
static float last_angle, residual;
static const char *const keys[] = {"Tab", "Enter", "Escape", "Backspace", "Left", "Right"};
static const uint8_t usages[] = {KVM_KEY_TAB, KVM_KEY_ENTER, KVM_KEY_ESCAPE, KVM_KEY_BACKSPACE,
                                 KVM_KEY_LEFT, KVM_KEY_RIGHT};

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                        lv_event_cb_t cb, lv_event_code_t code, uintptr_t id)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_INVERSE), 0);
    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_theme_font(h >= 100 ? UI_FONT_TITLE : UI_FONT_HEADING), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(obj, cb, code, (void *)id);
    return obj;
}

static void reset_controls(void)
{
    held = turning = angle_valid = scanning = fired = paused = cooling = select_down = false;
    armed_tile = NULL;
    sticky = 0;
    residual = 0;
    scan_latched = -1;
    dwell_key = -1;
    if (selection && view == ACCESS) show_access_state();
}

static void release(void)
{
    kvm_hid_snapshot_t s;
    hid_dispatch_get_snapshot(&s, NULL);
    generation = s.release_generation;
    switching = true;
    reset_controls();
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_wait_release(indev);
}

static bool input_ready(const kvm_hid_snapshot_t *s)
{
    if (switching && generation != s->release_generation && !s->neutralizing) {
        switching = false;
        generation = s->release_generation; // release() already canceled this gesture.
    } else if (generation != s->release_generation && !switching) {
        generation = s->release_generation;
        reset_controls();
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_indev_wait_release(indev);
            return false;
        }
    }
    return !switching && !s->neutralizing && !s->maintenance && s->host_usb == KVM_HOST_USB_MOUNTED;
}

static bool ready(void)
{
    kvm_hid_snapshot_t s;
    hid_dispatch_get_snapshot(&s, NULL);
    return input_ready(&s);
}

// A CLICKED is delivered on lift. A contact started offline or in an older
// ownership/Studio session must never acquire permission on that lift.
static bool fresh_click(lv_event_t *e, uint32_t peer)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESS_LOST && code != LV_EVENT_CLICKED) return false;
    lv_obj_t *tile = lv_event_get_target_obj(e);
    bool enabled = ready();
    if (code == LV_EVENT_PRESSED) {
        armed_tile = enabled ? tile : NULL;
        armed_peer = peer;
        return false;
    }
    bool send = code == LV_EVENT_CLICKED && enabled && armed_tile == tile && armed_peer == peer;
    armed_tile = NULL;
    return send;
}

static void send_radial(bool pressed, int16_t delta)
{
    kvm_hid_intent_t i = {.type = KVM_HID_RADIAL, .data.radial = {.pressed = pressed, .delta = delta}};
    if (hid_dispatch_local(&i) != ESP_OK) release();
}

static void dial(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (!ready()) return;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (held) send_radial(false, 0);
        held = turning = false;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_area_t bounds;
    lv_point_t p;
    lv_obj_get_coords(lv_event_get_target_obj(e), &bounds);
    lv_indev_get_point(indev, &p);
    int x = p.x - (bounds.x1 + bounds.x2) / 2, y = p.y - (bounds.y1 + bounds.y2) / 2;
    int radius_squared = x * x + y * y;
    bool center = radius_squared <= DIAL_BUTTON_RADIUS * DIAL_BUTTON_RADIUS;
    bool ring = !center && radius_squared <= DIAL_RADIUS * DIAL_RADIUS;
    if (code == LV_EVENT_PRESSED) {
        held = center;
        turning = angle_valid = ring;
        if (held) send_radial(true, 0);
        residual = 0;
        last_angle = atan2f((float)y, (float)x);
    } else if (turning) {
        if (!ring) {
            // Crossing the hub or leaving the rim supplies no angular travel.
            // Keep the original ring capture and rebase at the first valid sample.
            angle_valid = false;
            residual = 0;
            return;
        }
        float angle = atan2f((float)y, (float)x);
        if (!angle_valid) {
            angle_valid = true;
            last_angle = angle;
            return;
        }
        float step = (angle - last_angle) * 572.957795f;
        last_angle = angle;
        if (step > 1800) step -= 3600;
        if (step < -1800) step += 3600;
        residual += step * (fine ? 0.25f : 1.0f);
        if (fabsf(residual) >= 2) {
            int16_t delta = (int16_t)residual;
            residual -= delta;
            send_radial(false, delta);
        }
    }
}

static void fine_click(lv_event_t *e)
{
    fine = !fine;
    lv_label_set_text_static(lv_obj_get_child(lv_event_get_target_obj(e), 0), fine ? "Fine on" : "Fine off");
}

static void key_tap(uint8_t usage, unsigned modifiers)
{
    kvm_hid_burst_step_t steps[10];
    uint16_t n = 0;
    for (uint8_t i = 0; i < 4; i++)
        if (modifiers & (1u << i)) steps[n++] = (kvm_hid_burst_step_t){(uint8_t)(KVM_KEY_LEFT_CTRL + i), true};
    steps[n++] = (kvm_hid_burst_step_t){usage, true};
    steps[n++] = (kvm_hid_burst_step_t){usage, false};
    for (uint8_t i = 4; i > 0; i--)
        if (modifiers & (1u << (i-1))) steps[n++] = (kvm_hid_burst_step_t){(uint8_t)(KVM_KEY_LEFT_CTRL+i-1), false};
    if (hid_dispatch_burst_start(steps, n, NULL, 0) != ESP_OK) release();
}

static const char *const studio_names[] = {
    "Play / pause", "Previous", "Next", "Mute", "Volume down", "Volume up",
    "Undo", "Redo", "Copy", "Paste", "Save", "Select all"
};

static void studio_tap(lv_event_t *e)
{
    if (!fresh_click(e, 0)) return;
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    static const uint8_t media[] = {KVM_CHORD_MEDIA_PLAY_PAUSE,KVM_CHORD_MEDIA_PREV,KVM_CHORD_MEDIA_NEXT,
                                     KVM_CHORD_MEDIA_MUTE,KVM_CHORD_MEDIA_VOLUME_DOWN,KVM_CHORD_MEDIA_VOLUME_UP};
    if (idx < sizeof(media)) {
        kvm_hid_intent_t i = {.type = KVM_HID_CHORD, .data.chord.chord_id = media[idx]};
        if (hid_dispatch_local(&i) != ESP_OK) release();
    } else {
        static const uint8_t letters[] = {'Z','Y','C','V','S','A'};
        key_tap((uint8_t)(KVM_KEY_A + letters[idx - 6] - 'A'), 1);
    }
}

static void obs_refresh(void)
{
    static const char *const results[] = {
        "OBS confirmed", "OBS offline", "Permission denied", "Timed out",
        "Unsupported", "Agent error", "Effect unverified"
    };
    static const char *const phases[] = {"Ready", "Requested", "Delivered to agent", "Accepted by agent"};
    uint8_t available = bank_status.bank & bank_status.supported;
    bool pending = bank_status.phase > HPE_IDLE && bank_status.phase < HPE_FINISHED;
    for (unsigned i = 0; i < HPE_OBS_TILES; i++) {
        if ((available & (1u << i)) && !pending) lv_obj_remove_state(tiles[i], LV_STATE_DISABLED);
        else lv_obj_add_state(tiles[i], LV_STATE_DISABLED);
        const char *state = bank_status.state[i] == 2 ? "Active" : bank_status.state[i] == 1 ? "Inactive" : "Unknown";
        lv_label_set_text_fmt(lv_obj_get_child(tiles[i], 0), "%s\n%s", obs_names[i], state);
    }
    if (!available) lv_label_set_text_static(selection, "Arm the Studio USB agent on the connected computer");
    else if (bank_status.phase == HPE_IDLE) lv_label_set_text_static(selection, "OBS agent ready");
    else if (bank_status.tile_id >= 1 && bank_status.tile_id <= HPE_OBS_TILES) {
        const char *stage = bank_status.phase == HPE_FINISHED ? results[bank_status.result] : phases[bank_status.phase];
        unsigned i = bank_status.tile_id-1;
        if (bank_status.label[i][0] && strcmp(bank_status.label[i], obs_names[i]) != 0)
            lv_label_set_text_fmt(selection, "%s (%s)  |  %s", obs_names[i], bank_status.label[i], stage);
        else lv_label_set_text_fmt(selection, "%s  |  %s", obs_names[i], stage);
    }
}

static void obs_tap(lv_event_t *e)
{
    if (!fresh_click(e, bank_status.peer_nonce)) return;
    uint16_t tile = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    kvm_hid_intent_t intent = {.type=KVM_HID_STUDIO_ACTION,
        .data.studio={.tile_id=tile, .action_id=(uint16_t)(0x100 + tile), .peer_nonce=bank_status.peer_nonce}};
    if (hid_dispatch_local(&intent) != ESP_OK) release();
}

static void switch_bank(lv_event_t *e)
{
    (void)e;
    render(view == ACCESS ? TIMING : view == TIMING ? ACCESS : view == STUDIO ? OBS : STUDIO);
}

static void show_access_state(void)
{
    for (unsigned i = 2; i < 7; i++) lv_obj_set_hidden(access_tools[i], scanning);
    lv_obj_set_pos(access_tools[1], 158, scanning ? 310 : 332);
    lv_obj_set_size(access_tools[1], scanning ? 842 : 134, scanning ? 140 : 76);
    lv_obj_set_y(selection, scanning ? 452 : 432);
    for (unsigned i = 0; i < 6; i++) {
        lv_obj_set_style_border_width(access_keys[i], scanning && scan_index == i ? 5 : 0, 0);
        lv_obj_set_style_border_color(access_keys[i], lv_color_hex(UI_COLOR_ACCENT), 0);
    }
    char mode[64];
    if (scanning) lv_snprintf(mode, sizeof(mode), "Scanning %u of 6: %s", scan_index+1, keys[scan_index]);
    else if (paused) lv_snprintf(mode, sizeof(mode), "Scan paused");
    else if (dwell) lv_snprintf(mode, sizeof(mode), "Dwell: %u ms", (unsigned)timing.dwell_ms);
    else lv_snprintf(mode, sizeof(mode), "Direct input");
    lv_label_set_text_fmt(selection, "%s%s%s%s%s", mode, sticky & 1 ? "   Ctrl" : "", sticky & 2 ? "   Shift" : "",
                          sticky & 4 ? "   Alt" : "", sticky & 8 ? "   Super" : "");
}

static void access_send(unsigned idx)
{
    key_tap(usages[idx], sticky);
    sticky = 0;
    show_access_state();
}

static void access_key(lv_event_t *e)
{
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_CLICKED &&
        code != LV_EVENT_RELEASED && code != LV_EVENT_PRESS_LOST) return;
    if (scanning) return;
    if (!dwell) {
        if (fresh_click(e, 0)) access_send(idx);
        return;
    }
    if (!ready()) return;
    if (code == LV_EVENT_PRESSED && dwell) {
        if (cooling && lv_tick_elaps(cooldown_at) < timing.cooldown_ms) return;
        dwell_key = (int)idx;
        fired = false;
        pressed_at = lv_tick_get();
        lv_indev_get_point(lv_indev_active(), &dwell_origin);
    } else if (code == LV_EVENT_PRESSING && dwell_key == (int)idx && !fired) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        if (!lv_obj_hit_test(lv_event_get_target_obj(e), &p)) {
            dwell_key = -1;
            return;
        }
        int dx = p.x - dwell_origin.x, dy = p.y - dwell_origin.y;
        if (dx*dx + dy*dy > 144) {
            pressed_at = lv_tick_get();
            dwell_origin = p;
        } else if (lv_tick_elaps(pressed_at) >= timing.dwell_ms) {
            fired = true;
            cooling = true;
            cooldown_at = lv_tick_get();
            access_send(idx);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        dwell_key = -1;
    }
}

static void access_tool(lv_event_t *e)
{
    unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (id == 1) {
        if (code == LV_EVENT_PRESSED && scanning) {
            select_down = true;
            scan_latched = ready() && !cooling ? (int)scan_index : -1;
            select_at = lv_tick_get();
        }
        if (code == LV_EVENT_PRESS_LOST) { select_down = false; scan_latched = -1; }
        if (select_down && code == LV_EVENT_PRESSING && lv_tick_elaps(select_at) >= 2000) {
            release();
            paused = true;
            show_access_state();
        }
        if (code == LV_EVENT_CLICKED) {
            if (ready() && scan_latched >= 0 && scanning && lv_tick_elaps(select_at) >= timing.debounce_ms) {
                access_send((unsigned)scan_latched);
                cooling = true;
                scan_at = cooldown_at = lv_tick_get();
            }
            scan_latched = -1;
            select_down = false;
        }
        return;
    }
    if (id == 0) {
        if (scanning) { release(); paused = true; }
        else if (ready()) { paused = false; scanning = true; scan_index = 0; scan_at = lv_tick_get(); }

    } else if (id == 2) {
        dwell = !dwell;
        dwell_key = -1;
    } else if (id >= 3 && id <= 6) {
        if (!fresh_click(e, 0)) return;
        sticky ^= 1u << (id - 3);
        sticky_at = lv_tick_get();
    }
    show_access_state();
}

static void timing_labels(void)
{
    uint16_t values[] = {edit_timing.scan_ms, edit_timing.dwell_ms, edit_timing.debounce_ms, edit_timing.cooldown_ms};
    for (unsigned i=0; i<4; i++) lv_label_set_text_fmt(timing_values[i], "%u ms", (unsigned)values[i]);
}

static void timing_click(lv_event_t *e)
{
    unsigned id = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (!timing_loaded) return;
    if (id == 8) {
        if (app_config_access_save(edit_timing)) {
            timing = edit_timing;
            lv_label_set_text_static(timing_message, "Timings applied");
        } else lv_label_set_text_static(timing_message, "Settings busy. Tap Apply again.");
        return;
    }
    const unsigned min[] = {600,600,20,100}, max[] = {4000,1800,200,1000}, step[] = {100,100,10,50};
    uint16_t *values[] = {&edit_timing.scan_ms, &edit_timing.dwell_ms, &edit_timing.debounce_ms, &edit_timing.cooldown_ms};
    unsigned i = id/2;
    int value = (int)*values[i] + (id%2 ? (int)step[i] : -(int)step[i]);
    if (value < (int)min[i]) value = (int)min[i];
    if (value > (int)max[i]) value = (int)max[i];
    *values[i]=(uint16_t)value;
    lv_label_set_text_static(timing_message,"");
    timing_labels();
}

static void render_timing(void)
{
    static const char *const names[] = {"Scan interval","Dwell time","Switch debounce","Switch cooldown"};
    edit_timing = timing;
    for (unsigned i=0; i<4; i++) {
        int y = 18 + (int)i*91;
        lv_obj_t *label = lv_label_create(body);
        lv_label_set_text_static(label,names[i]);
        lv_obj_set_pos(label,60,y+20);
        button(body,405,y,118,70,"-",timing_click,LV_EVENT_CLICKED,i*2);
        timing_values[i]=lv_label_create(body);
        lv_obj_set_pos(timing_values[i],574,y+20);
        button(body,750,y,118,70,"+",timing_click,LV_EVENT_CLICKED,i*2+1);
    }
    timing_message=lv_label_create(body);
    lv_obj_set_pos(timing_message,60,415);
    lv_label_set_text_static(timing_message,timing_loaded ? "" : "Loading settings");
    button(body,710,388,210,72,"Apply",timing_click,LV_EVENT_CLICKED,8);
    timing_labels();
}

static void render(unsigned page)
{
    release();
    selection = NULL;
    lv_obj_clean(body);
    view = page;
    hid_dispatch_set_profile(page == DIAL ? HPE_DIAL_PROFILE : (page == ACCESS || page == TIMING) ? HPE_ACCESS_PROFILE : HPE_STUDIO_PROFILE);
    if (page != DIAL) {
        lv_obj_set_hidden(bank_switch, false);
        lv_label_set_text_static(lv_obj_get_child(bank_switch,0), page == OBS ? "Studio Basic" : page == ACCESS ? "Access timing" : page == TIMING ? "Access controls" : "OBS bank");
    } else lv_obj_set_hidden(bank_switch, true);
    if (page == DIAL) {
        lv_obj_t *ring = button(body, 304, 38, DIAL_RADIUS*2, DIAL_RADIUS*2, "Press / Hold", dial, LV_EVENT_ALL, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 18, 0);
        lv_obj_set_style_border_color(ring, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_t *hub = lv_obj_create(ring);
        lv_obj_remove_style_all(hub);
        lv_obj_set_clickable(hub, false);
        lv_obj_set_scrollable(hub, false);
        lv_obj_set_size(hub, DIAL_BUTTON_RADIUS*2, DIAL_BUTTON_RADIUS*2);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(hub, 2, 0);
        lv_obj_set_style_border_color(hub, lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_center(hub);
        lv_obj_t *compat = lv_label_create(body);
        lv_obj_set_pos(compat, 44, 285);
        lv_obj_set_width(compat, 226);
        lv_label_set_text_static(compat, "Host Radial support unverified");
        button(body, 44, 192, 172, 70, fine ? "Fine on" : "Fine off", fine_click, LV_EVENT_CLICKED, 0);
    } else if (page == STUDIO) {
        for (unsigned i = 0; i < 12; i++)
            button(body, 16 + (int)(i % 4) * 250, 20 + (int)(i / 4) * 154, 240, 140,
                   studio_names[i], studio_tap, LV_EVENT_ALL, i);
    } else if (page == OBS) {
        for (unsigned i = 0; i < HPE_OBS_TILES; i++)
            tiles[i] = button(body, 16 + (int)i*332, 64, 320, 200, obs_names[i], obs_tap, LV_EVENT_ALL, i+1);
        selection = lv_label_create(body);
        lv_obj_set_pos(selection, 16, 334);
        hid_dispatch_studio_status(&bank_status);
        obs_refresh();
    } else if (page == TIMING) {
        render_timing();
    } else {
        for (unsigned i = 0; i < 6; i++)
            access_keys[i]=button(body, 16 + (int)(i % 3) * 332, 12 + (int)(i / 3) * 152, 320, 140,
                   keys[i], access_key, LV_EVENT_ALL, i);
        const char *tools[] = {"Scan / Stop", "Select", "Dwell", "Ctrl", "Shift", "Alt", "Super"};
        for (unsigned i = 0; i < 7; i++)
            access_tools[i] = button(body, 16 + (int)i * 142, 332, 134, 76, tools[i], access_tool,
                                     i == 0 || i == 2 ? LV_EVENT_CLICKED : LV_EVENT_ALL, i);
        selection = lv_label_create(body);
        lv_obj_set_pos(selection, 16, 432);
        show_access_state();
    }
}

static void navigate(lv_event_t *e)
{
    unsigned page = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (page != HOME) { render(page); return; }
    if (home_fn) home_fn();
}

static void tick(lv_timer_t *t)
{
    (void)t;
    kvm_hid_snapshot_t snapshot;
    hid_dispatch_get_snapshot(&snapshot, NULL);
    bool mounted = input_ready(&snapshot);
    if (!timing_loaded && app_config_access_read(&timing)) {
        timing_loaded = true;
        if (view == TIMING) {
            edit_timing = timing;
            timing_labels();
            lv_label_set_text_static(timing_message,"");
        }
    }
    const char *host = snapshot.host_usb == KVM_HOST_USB_SUSPENDED ? "Host asleep" :
                       snapshot.host_usb != KVM_HOST_USB_MOUNTED ? "Connect USB to host" :
                       mounted ? "USB connected" : "USB input paused";
    const char *agent = "", *mode = "";
    if (view == STUDIO || view == OBS) {
        hpe_studio_status_t studio;
        hid_dispatch_studio_status(&studio);
        if (view == OBS && memcmp(&studio, &bank_status, sizeof(studio))) {
            bank_status = studio;
            obs_refresh();
        }
        agent = snapshot.host_usb == KVM_HOST_USB_MOUNTED && studio.peer_nonce ?
                    "   |   Studio agent connected" : "   |   No host agent";
        if (view == STUDIO) mode = "   |   Basic controls";
    }
    char message[96];
    lv_snprintf(message, sizeof(message), "%s%s%s", host, agent, mode);
    if (strcmp(lv_label_get_text(status), message)) lv_label_set_text(status, message);
    if (sticky && lv_tick_elaps(sticky_at) >= 10000) { sticky = 0; show_access_state(); }
    if (cooling && !select_down && lv_tick_elaps(cooldown_at) >= timing.cooldown_ms) {
        cooling = false;
        scan_at = lv_tick_get();
    }
    if (scanning && !select_down && !cooling && lv_tick_elaps(scan_at) >= timing.scan_ms) {
        scan_at = lv_tick_get();
        scan_index = (scan_index + 1) % 6;
        show_access_state();
    }
}

static void unloaded(lv_event_t *e)
{
    (void)e;
    hpe_panel_leave();
}

void hpe_panel_leave(void)
{
    if (!screen) return;
    release();
    hid_dispatch_set_profile(HPE_MOUSE_PROFILE);
    lv_timer_delete(timer);
    lv_obj_delete_async(screen);
    screen = body = status = selection = NULL;
    timer = NULL;
    home_fn = NULL;
}

bool hpe_panel_open(void (*home)(void))
{
    if (!home || screen) return false;
    home_fn = home;
    timing_loaded = false;
    screen = lv_obj_create(NULL);
    lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
    status = lv_label_create(screen);
    lv_label_set_text_static(status, "USB status pending");
    lv_obj_set_pos(status, 16, 10);
    bank_switch = button(screen, 785, 0, 230, 44, "OBS bank", switch_bank, LV_EVENT_CLICKED, 0);
    body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 0, 44);
    lv_obj_set_size(body, 1024, 488);
    lv_obj_set_scrollable(body, false);
    static const char *const names[] = {"Home", "Dial", "Studio Basic", "Access"};
    for (unsigned i = 0; i < 4; i++) button(screen, (int)i * 256 + 3, 535, 250, 62, names[i], navigate, LV_EVENT_CLICKED, i);
    timer = lv_timer_create(tick, 50, NULL);
    render(DIAL);
    lv_screen_load(screen);
    tick(timer);
    return true;
}
#else
bool hpe_panel_open(void (*home)(void)) { (void)home; return false; }
void hpe_panel_leave(void) {}
#endif
