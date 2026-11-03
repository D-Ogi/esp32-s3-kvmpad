// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Fixtures of the desktop simulator (TASK-070D, 07 §10 level L1): one plain-text file per deterministic state,
// parsed here and replayed through the real publishers and the real LVGL hit test.
//
// Format - one directive per line, `#` starts a comment, unknown keys are an error (a typo must not silently
// produce a different golden):
//
//   title   <free text>                         caption in the summary table
//   view    mouse | keyboard | shortcuts        the view to navigate to before the steps
//   host    uninitialized | detached | enumerating | mounted | suspended | error
//   client  disabled | network_down | listening | authenticating | connected | control_granted | stale
//   owner   none | local | remote
//   epoch   <u32>
//   wifi    0 | 1
//   touch   0 | 1
//   drag    0 | 1
//   mods    <hex>                               modifier ledger bitmap (what the HOST was told)
//   armed   <hex>                               ADR-016 one-shot bits
//   locked  <hex>                               ADR-016 locked bits
//   caps    unknown | on | off                  CapsLock LED report of the HOST
//   maint   0 | 1
//   error   <u16>                               kvm_hid_err_t carried in the snapshot
//   confirm 0 | 1                               FR-734 policy the shell reports to the SHORTCUTS view
//   layout  win | ctrl                          FR-744 chord of the HOST OS
//   step    <step>                              touch playback, in order (see sim_step_kind_t)
//
// Pairing and control approval (TASK-100), published exactly as the secure_remote adapter publishes them:
//
//   pair     0 | 1                              a pairing window is open (FR-722)
//   code     <6 digits>                         the code the card shows as "ddd ddd"
//   code_ms  <u32>                              code countdown, default 120000
//   win_ms   <u32>                              window countdown, default 180000
//   accept   0 | 1                              a client proved the code: Accept / Reject (FR-725)
//   accept_ms <u32>                             Accept countdown, default 30000
//   peer     <name>                             client name of the prompts and of the outcome toast
//   result   none | paired | rejected | failed | locked | expired
//   approval 0 | 1                              a paired client requests control (FR-733)
//   appr_ms  <u32>                              approval countdown, default 30000
//   session  <u32>                              session id an Allow / Deny names
//
// Settings (TASK-070C), published exactly as the domain adapter publishes the config_store record:
//
//   settings 0 | 1                              open the Settings card (through the gear, real hit test)
//   section  remote | clients | shortcuts | input | keyboard | about
//   cfg_seq  <u32>                              snapshot sequence; 0 = nothing published yet (all Unknown)
//   remote   0 | 1                              remote.enabled (FR-506)
//   req_accept 0 | 1                            pair.require_accept (FR-732)
//   approval always | trusted | auto            control.approval (FR-733)
//   altf4    0 | 1                              shortcuts.confirm_alt_f4 (FR-734)
//   awake    0 | 1                              keepawake.enabled (FR-711)
//   awake_s  <10..300>                          keepawake.interval_s (ADR-020)
//   layout_id <id>                              host_layout, e.g. us or de(neo) (FR-741)
//   layout_db 0 | 1                             whether the domain reports a layout database; 1 by default,
//                                               and 0 keeps the Host layout row read-only (FR-741)
//   layout_fav <id>[ <id>...]                   the pinned layouts, in order, up to 8 (ADR-039)
//   layout_recent <id>[ <id>...]                the last used layouts, newest first, up to 5 (ADR-039)
//   unicode  host_ime | linux | macos | windows_hex | wincompose   (FR-747)
//   lang_ko  0 | 1                              Korean turned on in the input language list (ADR-037)
//   lang_zh  0 | 1                              Chinese (pinyin) turned on in the list (ADR-037)
//   lang_ja  0 | 1                              Japanese turned on in the list (ADR-037)
//   fw       <version>                          firmware version string of the About pane (FR-751)
//   client   <name>                             append a paired client (FR-726)
//   tclient  <name>                             append a paired client with the trusted flag set
//
// IME (TASK-072):
//
//   zh       0 | 1                              install the canned pinyin decoder (sim_zh.h); the golden needs the
//                                               hanzi font blob (kvm_sim --hanzi), otherwise it is skipped
//   ja       0 | 1                              install the canned romaji / kana-kanji decoder (sim_ja.h); the
//                                               golden needs the kanji font blob (kvm_sim --kanji)
//
// Steps: tap_nav <0..2>, tap_host, tap_client, tap_key <legend>, hold_key <legend>, tap_mouse left|right|drag|help,
//        tap_shortcut <0..5>, tap_media <0..5>, tap_layout, tap_pair, tap_settings, tap_section <0..5>,
//        tap_toggle <ui_setting_id_t>, tap_approval <0..2>, tap_trusted <index>, tap_forget <index>,
//        tap_confirm 0 | 1, tap_lang, hold_lang, pick_lang en|ko|zh|ja, tap_flick_mode, flick <key>:<dir>,
//        layout_open, layout_back, layout_page prev|next, layout_row <0..6>, layout_all, layout_search,
//        layout_type <text>, layout_star <0..6>, layout_pick_fav <0..7>, layout_pick_recent <0..4>,
//        layout_index <0..8>, wait <ms>.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sim_platform.h"

#define SIM_FIX_STEPS_MAX 16
#define SIM_FIX_NAME_MAX 64
#define SIM_FIX_TITLE_MAX 128
#define SIM_FIX_TEXT_MAX 32

typedef enum {
    SIM_STEP_TAP_NAV = 0,
    SIM_STEP_TAP_HOST,
    SIM_STEP_TAP_CLIENT,
    SIM_STEP_TAP_KEY,
    SIM_STEP_HOLD_KEY,
    SIM_STEP_TAP_MOUSE,
    SIM_STEP_TAP_SHORTCUT,
    SIM_STEP_TAP_MEDIA,
    SIM_STEP_TAP_LAYOUT,
    SIM_STEP_TAP_PAIR, // "Pair a client" in the open CLIENT details card (ADR-022 step 1)
    SIM_STEP_TAP_SETTINGS, // the gear in the top bar
    SIM_STEP_TAP_SECTION,
    SIM_STEP_TAP_TOGGLE,
    SIM_STEP_TAP_APPROVAL,
    SIM_STEP_TAP_TRUSTED,
    SIM_STEP_TAP_FORGET,
    SIM_STEP_TAP_CONFIRM,
    SIM_STEP_TAP_LANG,       // short tap on the language key: cycle to the next available language (ADR-037)
    SIM_STEP_HOLD_LANG,      // long press on the language key: open the picker (ADR-037)
    SIM_STEP_PICK_LANG,      // tap one row of the open picker
    SIM_STEP_TAP_LAYOUT_OPEN, // FR-741: the Host layout row of the Keyboard pane opens the picker
    SIM_STEP_TAP_LAYOUT_BACK, // one level up in the open picker, or back to the pane
    SIM_STEP_TAP_LAYOUT_STEP, // `layout_page prev|next`: one page of the open list
    SIM_STEP_TAP_LAYOUT_ROW,  // `layout_row N`: tap row N (0..UI_SETTINGS_PICK_ROWS-1) of the open page
    SIM_STEP_TAP_LAYOUT_ALL,    // ADR-039: `All layouts` on the picker's home screen
    SIM_STEP_TAP_LAYOUT_SEARCH, // `Search` on the home screen
    SIM_STEP_LAYOUT_TYPE,       // `layout_type <text>`: the search keys, one per character
    SIM_STEP_HOLD_QUICK,        // ADR-039 section 8: long press on the KEYBOARD layout strip
    SIM_STEP_TAP_QUICK,         // `quick_pick N`: tile N of the open quick switch
    SIM_STEP_TAP_LAYOUT_STAR,   // `layout_star N`: the star of row N
    SIM_STEP_TAP_LAYOUT_FAV,    // `layout_pick_fav N`: favorite tile N
    SIM_STEP_TAP_LAYOUT_RECENT, // `layout_pick_recent N`: recent chip N
    SIM_STEP_TAP_LAYOUT_INDEX,  // `layout_index N`: bucket N of the A-Z strip
    SIM_STEP_TAP_FLICK_MODE, // the Flick / QWERTY toggle on the JA layer (FR-746)
    SIM_STEP_FLICK,          // `flick Q:D`: kana key Q (0..11) slid in direction D (0 tap, 1 left, 2 up, 3 right, 4 down)
    SIM_STEP_WAIT,
} sim_step_kind_t;

typedef struct {
    sim_step_kind_t kind;
    int32_t arg;
    char text[SIM_FIX_TEXT_MAX];
} sim_step_t;

typedef struct {
    char name[SIM_FIX_NAME_MAX];   // file stem; also the golden file name
    char title[SIM_FIX_TITLE_MAX];
    ui_view_id_t view;
    sim_state_t st;
    bool confirm_alt_f4;
    uint16_t layout_chord;
    ui_pair_view_t pair;         // published through ui_app_set_pairing()
    ui_approval_view_t approval; // published through ui_app_set_approval()
    ui_settings_view_t settings; // published through ui_app_set_settings()
    bool settings_open;          // open the Settings card before the steps run
    bool zh_engine;              // install the canned pinyin decoder of sim_zh.c (the ZH layer)
    bool ja_engine;              // install the canned romaji / kana-kanji decoder of sim_ja.c (the JA layer)
    ui_settings_section_t section;
    sim_step_t steps[SIM_FIX_STEPS_MAX];
    uint8_t n_steps;
} sim_fixture_t;

/** Parse one fixture file. False on an I/O error, an unknown key or a bad value (the reason is printed). */
bool sim_fixture_load(const char *path, sim_fixture_t *out);

/** Publish the state, navigate to the view and replay the steps. A `hold_key` press is left held. */
bool sim_fixture_apply(sim_app_t *s, const sim_fixture_t *fx);

/** The widget of the keyboard cap whose legend is `legend`, or NULL. */
lv_obj_t *sim_keyboard_key(const sim_app_t *s, const char *legend);
