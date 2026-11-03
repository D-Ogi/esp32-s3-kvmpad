// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Fake platform of the desktop simulator (TASK-070D, 07 §10 level L1): the real product shell composition
// (ui_app) on the real LVGL 9.6, with everything below the UI replaced by a recorder.
//
// What is real here: LVGL, ui_core (mailbox, store, router, overlays, action gateway), ui_theme, ui_components
// and ui_views, built from the same sources as the firmware, and the same ui_app_init() composition that
// main/ui_shell.c builds.
//
// What is fake here:
//   * the clock - a counter, so a render is reproducible (no wall clock reaches the UI);
//   * the domain sink - it records the typed actions instead of handing them to the InputArbiter. The
//     simulator contains no HID path at all: no tusb.h, no hid_core state machine, nothing that could send a
//     report (AGENTS-UI §4, AGENTS.md prohibition 2);
//   * the domain publishers - a fixture is published through ui_mailbox_publish_* exactly as the HID, net,
//     touch and gesture tasks publish on the device, so the UI still derives every state from an
//     authoritative snapshot and never from a simulator shortcut (AGENTS-UI §5);
//   * cancel_input / pad_changed - recorded, so the order "cancel first, then the new view" is observable;
//   * remote_action - recorded: the simulator holds no pairing state machine, so a tap on Accept or Allow is
//     noted and nothing else happens. A fixture publishes the pairing / approval state the same way the
//     secure_remote adapter does, through ui_app_set_pairing() / ui_app_set_approval().
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_app.h"
#include "ui_contracts.h"
#include "ui_mailbox.h"
#include "ui_store.h"

#define SIM_LOG_CAP 1024

typedef enum {
    SIM_LOG_ACTION = 0, // a typed action reached the fake domain sink
    SIM_LOG_CANCEL,     // hooks.cancel_input
    SIM_LOG_PAD,        // hooks.pad_changed
    SIM_LOG_REMOTE,     // hooks.remote_action: a local decision about a remote client
    SIM_LOG_SETTING,    // hooks.settings_change: one accepted Settings change
} sim_log_kind_t;

typedef struct {
    sim_log_kind_t kind;
    uint32_t seq; // one monotonic order over every recorded platform call
    ui_action_type_t type;
    uint16_t control_id;
    uint16_t value;
    uint8_t cancel_why;  // ui_cancel_reason_t
    uint8_t remote_act;  // ui_remote_action_t
    uint64_t session_id; // SIM_LOG_REMOTE: the session an APPROVE / DENY names
    uint8_t setting_id;  // SIM_LOG_SETTING: ui_setting_id_t
    int32_t setting_val; // SIM_LOG_SETTING: the value asked for
    uint8_t client_index;
    bool pad_on;
} sim_log_t;

/** The domain state a fixture publishes: the four producer slices of 07 §3. */
typedef struct {
    uint8_t host_usb_state;
    uint8_t client_state;
    uint8_t owner_kind;
    uint32_t owner_epoch;
    uint8_t modifiers_down;
    uint8_t mods_armed;
    uint8_t mods_locked;
    bool caps_led_known;
    bool caps_led_active;
    bool maintenance;
    uint16_t error_code;
    bool wifi_connected;
    bool touch_healthy;
    bool drag_lock_active;
} sim_state_t;

typedef struct {
    ui_mailbox_t mb;
    ui_store_t store;
    ui_actions_t actions;
    ui_app_t app;

    uint64_t clock_us;
    uint32_t domain_seq;

    sim_log_t log[SIM_LOG_CAP];
    uint32_t log_n;
    uint32_t log_seq;
    uint32_t log_lost; // entries dropped because the ring was full

    bool pad_on;
    lv_area_t pad;

    bool confirm_alt_f4;  // the FR-734 policy this run reports to the view
    uint16_t layout_chord; // FR-744 chord of the HOST OS
    bool started;
} sim_app_t;

/** Headless 1024x600 RGB565 display, LVGL test input devices, theme and font warm-up. Returns the baseline. */
size_t sim_env_start(void);
void sim_env_stop(size_t baseline);

/** Build the shell. `confirm_alt_f4` and `layout_chord` must be set on `s` before the call. */
bool sim_app_start(sim_app_t *s);
void sim_app_stop(sim_app_t *s);

/** Publish every slice of `st` as four domain sources and let the store apply the snapshot. */
void sim_publish(sim_app_t *s, const sim_state_t *st);

/** Let LVGL finish its pending work; asserts that nothing is animating (the UI uses no lv_anim). */
void sim_settle(void);

/** Touch playback through the real LVGL hit test (the test pointer indev). */
void sim_press_obj(lv_obj_t *obj);
void sim_press_point(int32_t x, int32_t y);
void sim_release(void);
void sim_click_obj(lv_obj_t *obj);
/** Press the middle of obj, hold past LVGL's long-press time, release: LONG_PRESSED fires, SHORT_CLICKED does not. */
void sim_long_press_obj(lv_obj_t *obj);
/** Press the middle of obj, slide 40 px in dir (0 none, 1 left, 2 up, 3 right, 4 down) in steps, release. */
void sim_flick_obj(lv_obj_t *obj, uint8_t dir);

/** Recorded actions of one type (and optionally one value; pass SIM_ANY_VALUE for any). */
#define SIM_ANY_VALUE 0xFFFFu
uint32_t sim_count_action(const sim_app_t *s, ui_action_type_t type, uint16_t value);

/** Sequence number of the first / last recorded entry of a kind, or 0 when there is none. */
uint32_t sim_first_action_seq(const sim_app_t *s, ui_action_type_t type, uint16_t value);
uint32_t sim_last_pad_seq(const sim_app_t *s);

/** Number of domain actions recorded (everything the sink saw). */
uint32_t sim_domain_actions(const sim_app_t *s);

/** Recorded local decisions about a remote client (pairing, control approval). */
uint32_t sim_count_remote(const sim_app_t *s, ui_remote_action_t act);

/** Recorded Settings changes of one id (the simulator stores nothing: it only records the request). */
uint32_t sim_count_setting(const sim_app_t *s, ui_setting_id_t id);

void sim_log_reset(sim_app_t *s);

/** Submit a typed action through the real gateway (used where a held press blocks a second touch). */
uint32_t sim_submit(sim_app_t *s, ui_action_type_t type, uint16_t control_id, uint16_t value);
