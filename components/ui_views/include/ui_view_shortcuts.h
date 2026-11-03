// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// SHORTCUTS view (TASK-070C; 01 §5 FR-301..FR-310, FR-209, ADR-018 FR-311, ADR-023 FR-734, ADR-024 FR-744,
// reference commands-grid): the six reference command cards, a Media row and the "Switch HOST layout" command.
//
// No HID here (AGENTS-UI §4). Every tile submits exactly one UI_ACT_SHORTCUT_REQUEST whose value is a chord id
// of the compiled allowlist (hid_keymap.h) - never a usage, never a modifier bitmap. FR-209: the view holds no
// modifier state of its own and never reads the KEYBOARD screen's, so a shortcut cannot inherit a sticky
// modifier; the chord is atomic in the HID core.
//
// The view claims nothing about the HOST (AGENTS-UI §5): the toast after a tap says "Requested", and a refused
// submit says so instead of being retried. Commands are enabled only while the authoritative projection allows
// local input - HOST mounted, no maintenance, the control owner not REMOTE (FR-006, FR-308).
//
// FR-734 / ADR-023: Close window (Alt+F4) asks for confirmation through a blocking ui_modal when the policy
// callback given at init returns true. The view reads that policy, it never changes it (Settings owns it).
//
// LVGL owner context only; ui_theme_init() must have run.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "ui_actions.h"
#include "ui_modal.h"
#include "ui_router.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Control id of a tile: UI_CTRL_SHORTCUT_BASE + chord id, so the id of a command is stable and distinct for
 * every chord without a second table (07 §3: a UI-local id, never a keycode).
 */
#define UI_CTRL_SHORTCUT_BASE 0x0300u
#define UI_CTRL_SHORTCUT_CONFIRM 0x03F1u // FR-734 modal: "Close window"
#define UI_CTRL_SHORTCUT_CANCEL 0x03F2u  // FR-734 modal: "Keep it open"

/** The six reference commands, in grid order (card 0 is the featured Ctrl+Alt+Del tile). */
typedef enum {
    UI_SHORTCUT_SECURE_ATTENTION = 0, // FR-301 Ctrl + Alt + Del
    UI_SHORTCUT_SWITCH_WINDOW,        // FR-302 Alt + Tab
    UI_SHORTCUT_TASK_MANAGER,         // FR-303 Ctrl + Shift + Esc
    UI_SHORTCUT_DESKTOP,              // FR-304 Win + D
    UI_SHORTCUT_CLOSE_WINDOW,         // FR-305 Alt + F4 (FR-734 confirmation)
    UI_SHORTCUT_SCREENSHOT,           // FR-306 Print Screen
    UI_SHORTCUT_COMMAND_COUNT,
} ui_shortcut_command_t;

/** The Media group of ADR-018 / FR-311, in row order. */
typedef enum {
    UI_SHORTCUT_MEDIA_PLAY_PAUSE = 0,
    UI_SHORTCUT_MEDIA_PREV,
    UI_SHORTCUT_MEDIA_NEXT,
    UI_SHORTCUT_MEDIA_MUTE,
    UI_SHORTCUT_MEDIA_VOLUME_DOWN,
    UI_SHORTCUT_MEDIA_VOLUME_UP,
    UI_SHORTCUT_MEDIA_COUNT,
} ui_shortcut_media_t;

typedef struct {
    uint32_t requests;        // SHORTCUT_REQUEST the sink accepted
    uint32_t refused;         // submitted and refused (sink, no sink or invalid value)
    uint32_t blocked;         // taps dropped because local input is not allowed right now
    uint32_t confirms_opened; // FR-734 confirmation modals opened
    uint32_t confirms_taken;  // confirmed (the chord was then submitted once)
    uint32_t confirms_closed; // closed without confirming: Cancel, scrim, leave or destroy
} ui_view_shortcuts_counters_t;

typedef struct {
    // bound at init
    ui_store_t *store;
    ui_actions_t *actions;
    uint64_t (*now_us)(void);
    uint16_t layout_chord;             // FR-744: KVM_CHORD_LAYOUT_WIN or KVM_CHORD_LAYOUT_CTRL
    bool (*confirm_alt_f4)(void *ctx); // FR-734 policy, read on every tap; NULL = no confirmation
    void *policy_ctx;
    ui_view_descriptor_t desc; // id UI_VIEW_SHORTCUTS; hand a copy to ui_router_init()
    ui_modal_sink_t modal_sink; // the FR-734 modal's button sink (points back at this struct)

    // optional parents for the transient layers; NULL = the view root's screen (see set_layers)
    lv_obj_t *modal_parent;
    lv_obj_t *toast_parent;

    // created by desc.create (NULL before / after)
    lv_obj_t *root;
    lv_obj_t *grid;
    lv_obj_t *media_row;
    lv_obj_t *foot_row;
    lv_obj_t *card[UI_SHORTCUT_COMMAND_COUNT];
    // Children of each command card. A label that carries its own text color is shielded from the card's
    // LV_STATE_DISABLED - LVGL hands no state down the tree - so the view repaints these itself.
    lv_obj_t *card_chip[UI_SHORTCUT_COMMAND_COUNT];
    lv_obj_t *card_glyph[UI_SHORTCUT_COMMAND_COUNT];
    lv_obj_t *card_name[UI_SHORTCUT_COMMAND_COUNT];
    lv_obj_t *card_sub[UI_SHORTCUT_COMMAND_COUNT];
    lv_obj_t *media[UI_SHORTCUT_MEDIA_COUNT];
    lv_obj_t *layout_key;
    lv_obj_t *layout_label; // "Switch HOST layout"; repainted with the tile's enabled state, see card_name
    lv_obj_t *layout_sub;   // "Win + Space" / "Ctrl + Space", follows layout_chord
    lv_obj_t *hint;

    // transient, parented outside the view root (cleared by their own delete events)
    lv_obj_t *modal;
    lv_obj_t *toast;

    uint16_t pending_chord; // the chord the open confirmation would submit, else KVM_CHORD_NONE (0)
    char toast_text[64];
    ui_view_shortcuts_counters_t c;
} ui_view_shortcuts_t;

/**
 * Bind the view to the store and the action gateway and fill s->desc. Builds no object.
 * `layout_chord` is the FR-744 chord for the HOST OS selected in Settings (the view never hard-codes it);
 * an unknown value disables the layout tile. `confirm_alt_f4` is the FR-734 policy: it is called on every
 * Close window tap and may be NULL (no confirmation).
 */
void ui_view_shortcuts_init(ui_view_shortcuts_t *s, ui_store_t *store, ui_actions_t *actions,
                            uint64_t (*now_us)(void), uint16_t layout_chord, bool (*confirm_alt_f4)(void *ctx),
                            void *policy_ctx);

/**
 * Where the FR-734 confirmation modal and the feedback toasts are created. Both default to the view root's
 * screen, which is correct for the standard shell. Pass the shell's toast layer as `toast_parent` to keep
 * toasts in their layer. Do NOT pass the router's overlay layer as `modal_parent`: the router hides that
 * layer while its own overlay stack is empty, so the modal would be invisible. Either argument may be NULL
 * to keep the default. Call it after ui_router_init() and before the first show.
 */
void ui_view_shortcuts_set_layers(ui_view_shortcuts_t *s, lv_obj_t *modal_parent, lv_obj_t *toast_parent);

/**
 * Abandon the FR-734 confirmation if one is open (nothing is submitted; the pending chord is dropped and
 * counted as closed). The shell calls it when another blocking decision must own the screen - the
 * confirmation lives on its own scrim, outside the router's overlay stack, so it would otherwise stay on
 * top of a prompt the user has to answer.
 */
void ui_view_shortcuts_dismiss_modal(ui_view_shortcuts_t *s);

/** True while local input is allowed (HOST mounted, no maintenance, owner not REMOTE): FR-006, FR-308. */
bool ui_view_shortcuts_input_allowed(const ui_view_shortcuts_t *s);

/** Chord id (kvm_chord_id_t) a command card submits; 0 for an out-of-range index. */
uint16_t ui_view_shortcuts_command_chord(ui_shortcut_command_t cmd);

/** Chord id (kvm_chord_id_t) a Media tile submits; 0 for an out-of-range index. */
uint16_t ui_view_shortcuts_media_chord(ui_shortcut_media_t media);

/** Label of a command card, for logs and tests ("Close window"); NULL for an out-of-range index. */
const char *ui_view_shortcuts_command_name(ui_shortcut_command_t cmd);

#ifdef __cplusplus
}
#endif
