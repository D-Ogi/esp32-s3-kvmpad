// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// KEYBOARD view (TASK-070B; 01 FR-201..212, reference keyboard-view): a US ANSI key field of real tap targets -
// utility row, number row with shift sub-legends, three letter rows and a bottom row - built from UiActionKey
// (TASK-060D) inside the router's content slot.
//
// No HID here (AGENTS-UI §4): every cap hands the view a control id plus a down/up edge, and the view turns that
// into a typed action through ui_actions:
//   - ordinary key: down edge -> UI_ACT_KEY_DOWN(usage), up edge or lost press -> UI_ACT_KEY_UP(usage). Native
//     DOWN/UP, no auto-repeat in the firmware (FR-203, FR-208); at most one ordinary key is held (FR-211).
//   - modifier (Shift/Ctrl/Alt/Win/AltGr): down edge -> UI_ACT_MODIFIER_TOGGLE(usage). The view never sends a
//     modifier bit with a key - the one-shot combination is assembled atomically by the HID core (FR-205,
//     ADR-016) - and never toggles the cap itself: OFF / ARMED / LOCKED come from UI_SUBJ_MODS_ARMED and
//     UI_SUBJ_MODS_LOCKED alone (FR-204, AGENTS-UI §5).
//   - CapsLock: an ordinary key (usage 0x39); its indicator reads UI_SUBJ_CAPS_LED and shows "Unknown" while the
//     HOST has sent no LED output report (FR-206, never guessed).
//   - "F1-F12": a local UI layer that swaps the number row for F1..F12 and is never sent to the HOST (FR-207).
//     Switching the layer releases a held key first.
//   - the language key: one cap in the utility row that shows the code of the current input language (EN, KO, ZH,
//     JA) and cycles to the next one that is turned on in Settings and available right now (ADR-037). A long press
//     opens UI_OVERLAY_KBD_LANG, a blocking picker that lists the languages turned on, marks the current one and
//     shows why an unavailable one cannot be chosen; the long press is never also a tap. With English alone turned
//     on, or with nothing else available, a tap only toasts the reason and changes nothing. The set of languages
//     comes from the Settings record through ui_view_keyboard_set_languages(); a language turned off while it is
//     active falls back to English with the ordinary commit rule. Every switch below is reached through this one
//     cap, which is why the per-language caps of v1 are gone.
//   - Korean: a local layer for Korean composed on the DEVICE (ADR-024 FR-746, 2-set). While it is on, the letter
//     caps feed the Hangul composer (ime_engine) and the preedit is shown in a strip above the keys; committed
//     syllables leave as UI_ACT_TEXT_CHAR, every other key as UI_ACT_TEXT_KEY in the same order (the domain sends
//     each item as one cancellable burst, FR-748). Shift is a local jamo shift (ㄲ ㄸ ㅃ ㅆ ㅉ ㅒ ㅖ); Ctrl, Alt,
//     Win, AltGr and Caps are disabled. Switching the layer on submits UI_ACT_TEXT_CANCEL first (no modifier armed
//     before it can reach the text); switching it off commits the preedit; leaving the view discards the preedit
//     and submits UI_ACT_TEXT_CANCEL. In the host-ime delivery mode the language cannot be selected at all: the
//     HOST's own IME composes instead (ui_view_keyboard_set_text_mode()).
//   - Simplified Chinese: a local layer composed on the DEVICE (FR-746, pinyin). The letter caps (and the
//     syllable separator ') build a pinyin string that the decoder behind ui_kbd_conv_engine_t turns into hanzi
//     candidates; a strip above the keys shows the input (the fixed hanzi, then the letters still being decoded)
//     and up to UI_VIEW_KBD_CANDS candidates with paging. Tapping a candidate (or its digit 1..6) fixes it; when
//     the whole input is fixed the sentence is committed. Space commits the first candidate, Enter the fixed hanzi
//     and the remaining letters as typed, Esc discards, Backspace undoes the last choice or the last letter. Any
//     other key commits the first candidate before itself. With nothing composed, , . ; \ and Shift+; / 1 give
//     the full-width Chinese punctuation (，。；、：？！) and other keys pass through as UI_ACT_TEXT_KEY. Committed text
//     leaves as UI_ACT_TEXT_CHAR / UI_ACT_TEXT_KEY exactly like the Korean layer, with the same TEXT_CANCEL rules.
//     It can be selected only with a decoder installed (ui_view_keyboard_set_zh_engine()) and a Unicode delivery
//     mode; the languages exclude each other. The decoder runs in the LVGL owner context (~10 ms per search on
//     board #1).
//   - Japanese: the same layer (FR-746, romaji -> kana -> kanji) with its own decoder
//     (ui_view_keyboard_set_conv_engine(k, UI_KBD_LAYER_JA, ...)) and the UI_FONT_KANJI strip font. The decoder's
//     `preedit` gives the kana reading, shown after the fixed text and committed by Enter instead of the romaji;
//     '-' is part of the input (the long vowel mark ー). With nothing composed , . / [ ] Shift+/ Shift+1 give
//     、。・「」？！. KO, ZH and JA exclude each other; switching ZH <-> JA commits the text on screen first.
//     The strip key "Flick" swaps the number and letter rows for a 12-key flick pad (FR-746 "JP flick"): あかさ
//     たなはまやら わ, a tap gives the a-row kana, a flick left / up / right / down (at least UI_FLICK_MIN px) the
//     i / u / e / o kana, and the key shows the kana under the finger while it is held. ゛゜小 cycles the last kana
//     through its dakuten, handakuten and small forms; 、。？！ and the bracket flicks of や commit what is composed
//     first. The right column is Backspace (one kana), Space, Enter and "ABC" (back to QWERTY). Kana enter the same
//     input as romaji (か = "ka", っ = "xtu", ん = "nn"), so the decoder and the strip behave exactly as with romaji.
//
// HOST layout (FR-741/742/743, ADR-024). The field is drawn for the layout Settings selected: every cap that owns a
// key slot takes its legends (Base, Shift, AltGr) and its HID usage from the `kbd_layouts` tables, and the caps that
// exist only in one geometry family - LSGT (ISO, ABNT2), the Yen key AE13 and the three Japanese IME keys (JIS), the
// extra AB11 (JIS, ABNT2), Hangul and Hanja (KR) - are shown or hidden with the family. On ISO the `#` key moves from
// the end of the QWERTY row into the home row next to Enter, which is the ISO Enter shape. A dead level shows its
// accent, exactly as the table carries it, and the hint "Set the same layout on the HOST" stands over the field,
// because this device cannot read the HOST's own setting (FR-742).
//
// A legend is decoration and never a keymap (FR-743): what a cap sends is the usage of its slot in the selected
// layout - the one place where the layout changes behavior is that ISO moves `#` to usage 0x32 - and nothing on this
// path types a character or a code point. Without a layout (no selection yet, or a tree whose generated tables are
// absent) the field stays the US ANSI table compiled into this file, which is also what the KO / ZH / JA composition
// layers of ADR-037 use: those layers read a cap's position, so they keep working under every layout.
//
// While the HOST is not mounted every cap is disabled and nothing is submitted; a HOST lost while a key is held
// gives that key its one owed UP.
// LVGL owner context only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ime_hangul.h"
#include "kbd_layouts.h"
#include "lvgl.h"
#include "ui_action_key.h"
#include "ui_actions.h"
#include "ui_router.h"
#include "ui_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Control ids of this view: UI_CTRL_KBD_BASE + table index of the cap (07 §3 "UI-local stable id"). */
#define UI_CTRL_KBD_BASE 0x0200u

/** Capacity of the cap array; the live number of caps is ui_view_keyboard_key_count(). */
#define UI_VIEW_KBD_KEY_CAP 96u

/** `held` sentinel: no ordinary key is down. */
#define UI_VIEW_KBD_NO_KEY 0xFFu

/** Capacity of a HOST layout id, KVM_CONFIG_LAYOUT_ID_CAP ("us", "de(neo)", ...) mirrored; a test pins it. */
#define UI_VIEW_KBD_LAYOUT_CAP 16

/** ZH layer: visible candidates per page, the longest pinyin input, UTF-8 room per shown string. */
#define UI_VIEW_KBD_CANDS 6u
#define UI_VIEW_KBD_PINYIN_CAP 26u
#define UI_VIEW_KBD_CAND_UTF16 32u
#define UI_VIEW_KBD_CAND_UTF8 (3u * UI_VIEW_KBD_CAND_UTF16 + 4u)

/** Control ids of the candidate strip: slots 0..UI_VIEW_KBD_CANDS-1, then the previous / next page keys. */
#define UI_CTRL_KBD_CAND_BASE (UI_CTRL_KBD_BASE + 0x80u)
#define UI_CTRL_KBD_CAND_PREV (UI_CTRL_KBD_CAND_BASE + UI_VIEW_KBD_CANDS)
#define UI_CTRL_KBD_CAND_NEXT (UI_CTRL_KBD_CAND_PREV + 1u)
/** JA layer: the strip key that swaps the QWERTY field for the 12-key flick pad and back. */
#define UI_CTRL_KBD_FLICK (UI_CTRL_KBD_CAND_NEXT + 1u)

/** The input languages the language key cycles through, in cycle order (ADR-037). */
typedef enum {
    UI_KBD_LANG_EN = 0, // no composition layer: the caps type US ANSI usages
    UI_KBD_LANG_KO = 1, // the Hangul layer
    UI_KBD_LANG_ZH = 2, // the pinyin conversion layer
    UI_KBD_LANG_JA = 3, // the romaji / kana-kanji conversion layer
    UI_KBD_LANG_COUNT = 4,
} ui_kbd_lang_t;

/** Bit of `langs`: which languages Settings has turned on. English is always set. */
#define UI_KBD_LANG_BIT(l) ((uint8_t)(1u << (l)))

/** Language picker: one control id per language, then its Close key. */
#define UI_CTRL_KBD_LANG_PICK_BASE (UI_CTRL_KBD_FLICK + 1u)
#define UI_CTRL_KBD_LANG_CLOSE (UI_CTRL_KBD_LANG_PICK_BASE + UI_KBD_LANG_COUNT)

/** The blocking picker a long press on the language key opens. */
#define UI_OVERLAY_KBD_LANG 0x0601u
/** ADR-039 section 8: the quick switch of the HOST layout, opened by a long press on the layout strip. */
#define UI_OVERLAY_KBD_LAYOUT 0x0602u
#define UI_KBD_OVERLAYS 2

/** How many pinned and recently used layouts the quick switch offers (ADR-039 sections 3 and 4). */
#define UI_VIEW_KBD_FAV_MAX 8
#define UI_VIEW_KBD_RECENT_MAX 5
#define UI_VIEW_KBD_QUICK_MAX (UI_VIEW_KBD_FAV_MAX + UI_VIEW_KBD_RECENT_MAX)

/** Control ids of the quick switch, for the diagnostics dump and the tests. */
#define UI_CTRL_KBD_LAYOUT_PICK_BASE 0x0620u
#define UI_CTRL_KBD_LAYOUT_CLOSE 0x0630u

/**
 * What the view does with a layout the user chose in the quick switch: report it, as the table index of the
 * entry, exactly as the Settings picker reports `UI_SETTING_HOST_LAYOUT`. The view writes nothing itself and
 * nothing on screen moves until the domain publishes the record back (AGENTS-UI MUST 5).
 */
typedef void (*ui_view_keyboard_layout_fn)(void *ctx, int32_t table_index);

/** JA layer: the longest romaji input (flick kana take up to 3 letters each; ime_wnn takes 50). */
#define UI_VIEW_KBD_JA_CAP 48u
/** Room of the input buffer: the longer of the two layers' limits. */
#define UI_VIEW_KBD_INPUT_CAP UI_VIEW_KBD_JA_CAP
/** UTF-16 units of the JA reading the view shows and commits (one kana per letter at most). */
#define UI_VIEW_KBD_READING_UTF16 UI_VIEW_KBD_INPUT_CAP

/** JA flick pad: 4 x 4 keys - three columns of kana keys and a column of Backspace, Space, Enter, ABC. */
#define UI_VIEW_KBD_FLICK_KEYS 16u

/**
 * A letters-to-text decoder behind a conversion layer: pinyin -> hanzi for ZH (on the device ime_pinyin), romaji ->
 * kana -> kanji for JA (on the device ime_wnn). The view never includes an engine: the firmware or a test hands it
 * these calls. Candidate 0 is always the whole sentence with the fixed text first; candidates 1.. cover the first
 * part that is not fixed yet. Every member is required except `preedit`.
 */
typedef struct {
    void *ctx;
    size_t (*search)(void *ctx, const char *letters, size_t len); // a..z, '\'' (and '-' for JA); candidate count
    size_t (*candidate)(void *ctx, size_t i, uint16_t *out, size_t cap); // UTF-16, NUL-terminated; its length
    size_t (*choose)(void *ctx, size_t i);   // fix candidate i; returns the new count
    size_t (*cancel_last)(void *ctx);        // undo the last choice; returns the new count
    size_t (*fixed_len)(void *ctx);          // UTF-16 units fixed so far
    size_t (*consumed)(void *ctx);           // input letters covered by them
    size_t (*parsed)(void *ctx);             // input letters the decoder turned into its reading (syllables, kana)
    bool (*complete)(void *ctx);             // everything fixed: candidate 0 is the sentence to commit
    void (*reset)(void *ctx);
    // optional: the reading of the input that is parsed but not fixed (the kana for JA), shown and committed by
    // Enter instead of the letters; NULL shows the letters as typed (ZH)
    size_t (*preedit)(void *ctx, uint16_t *out, size_t cap);
} ui_kbd_conv_engine_t;

typedef struct {
    uint32_t key_down;        // UI_ACT_KEY_DOWN accepted by the sink
    uint32_t key_up;          // UI_ACT_KEY_UP submitted (accepted or not)
    uint32_t mod_toggles;     // UI_ACT_MODIFIER_TOGGLE submitted
    uint32_t fn_switches;     // F1-F12 layer switches (local, never HID)
    uint32_t refused;         // a submit the sink refused
    uint32_t leave_releases;  // a held key released by on_leave / destroy / HOST loss / layer switch
    uint32_t text_chars;      // UI_ACT_TEXT_CHAR accepted (KO layer)
    uint32_t text_keys;       // UI_ACT_TEXT_KEY accepted (KO layer)
    uint32_t text_cancels;    // UI_ACT_TEXT_CANCEL submitted
    uint32_t zh_choices;      // ZH layer: candidates chosen (taps, digits, Space)
    uint32_t zh_commits;      // ZH layer: compositions committed
    uint32_t flicks;          // JA flick pad: kana entered (taps and flicks)
    uint32_t lang_switches;   // the input language changed (tap, picker or Settings fallback)
    uint32_t lang_refused;    // a tap or a pick that changed nothing: nothing else on, or not available
    uint32_t picker_opens;    // the language card was built
    uint32_t picker_closes;   // ... and taken down (pick, Close, router dismiss or teardown)
    uint32_t toasts;          // a toast shown instead of a switch
    uint32_t layout_sets;     // ui_view_keyboard_set_host_layout() accepted an id (FR-741)
    uint32_t layout_unknown;  // ... and refused one the generated tables do not ship
    uint32_t relabels;        // the field was redrawn for a layout (build or change)
    uint32_t quick_opens;     // ADR-039 section 8: the quick-switch card was built
    uint32_t quick_closes;    // ... and taken down (pick, Close, router dismiss or teardown)
    uint32_t quick_picks;     // a layout reported from the quick switch
    uint32_t quick_refused;   // a long press that opened nothing: no sink, no list or the router said no
} ui_view_keyboard_counters_t;

typedef struct {
    // bound at init
    ui_store_t *store;
    ui_actions_t *actions;
    uint64_t (*now_us)(void);
    ui_view_descriptor_t desc;      // id UI_VIEW_KEYBOARD; hand &desc to ui_router_init()
    ui_action_key_sink_t sink;      // caller-owned sink the caps report their edges to
    ui_overlay_descriptor_t ov[UI_KBD_OVERLAYS]; // UI_OVERLAY_KBD_LANG; hand copies to ui_router_init()
    ui_router_t *router;            // bound by ui_view_keyboard_bind_router(); NULL: no picker, no toast

    // created by desc.create (NULL before / after)
    lv_obj_t *root;
    lv_obj_t *row_util;
    lv_obj_t *row_number;           // visible while the F1-F12 layer is off
    lv_obj_t *row_fn;               // visible while it is on
    lv_obj_t *row_top;
    lv_obj_t *row_mid;
    lv_obj_t *row_low;
    lv_obj_t *row_bottom;
    lv_obj_t *caps_sub;             // "On" / "Off" / "Unknown" sub-legend of the CapsLock cap
    lv_obj_t *cap_label[UI_VIEW_KBD_KEY_CAP]; // the main legend of a cap, NULL for an icon-only cap
    lv_obj_t *sub[UI_VIEW_KBD_KEY_CAP];  // FR-742 Shift legend of a cap that owns a key slot, else NULL
    lv_obj_t *alt[UI_VIEW_KBD_KEY_CAP];  // FR-742 AltGr legend of such a cap (hidden when the level is empty)
    lv_obj_t *hint;                 // "Set the same layout on the HOST" (FR-742), hidden without a layout
    char hint_text[96];             // the text that label renders: the layout's name plus the hint
    lv_obj_t *offline;              // "Host disconnected" notice over the field, hidden while HOST is mounted
    lv_obj_t *row_compose;          // KO layer: preedit strip, hidden while the layer is off
    lv_obj_t *preedit;              // the syllable being composed (UTF-8)
    lv_obj_t *keys[UI_VIEW_KBD_KEY_CAP];
    lv_obj_t *row_zh;               // ZH layer: input + candidate strip, hidden while the layer is off
    lv_obj_t *zh_input;             // the fixed hanzi, then the letters still being decoded
    lv_obj_t *zh_cand[UI_VIEW_KBD_CANDS];
    lv_obj_t *zh_prev;
    lv_obj_t *zh_next;
    lv_obj_t *lang_key;             // the one language cap of the utility row (ADR-037); its legend is the code

    // created by the overlay / the toast layer (NULL while they are down)
    lv_obj_t *lang_card;
    lv_obj_t *pick_btn[UI_KBD_LANG_COUNT];
    lv_obj_t *strip_code;                             // ADR-039 section 8: the short id on the strip ("DE")
    char strip_code_text[8];
    char strip_name_text[40];
    lv_obj_t *layout_card;                            // ADR-039 section 8, NULL while the overlay is down
    lv_obj_t *layout_pick[UI_VIEW_KBD_QUICK_MAX];     // its tiles, favorites first, then the recents
    char layout_pick_text[UI_VIEW_KBD_QUICK_MAX][48]; // each tile's caption, owned here (static labels)
    uint8_t layout_pick_count;
    lv_obj_t *toast;
    char toast_text[96];

    bool fn_layer;                  // local F1-F12 layer (FR-207)
    bool hangul;                    // local KO composition layer (FR-746)
    bool jamo_shift;                // KO layer: the next letter takes its Shift jamo
    bool text_mode_ok;              // a delivery mode other than host-ime is selected (FR-747)
    kvm_hangul_t ime;
    char preedit_text[8];
    bool converting;                // a conversion layer (ZH or JA) is on (FR-746)
    uint8_t conv_layer;             // which one: UI_KBD_LAYER_ZH / UI_KBD_LAYER_JA
    bool zh_shift;                  // conversion layer: one-shot local Shift for the next key
    const ui_kbd_conv_engine_t *zh; // pinyin decoder, NULL until installed
    const ui_kbd_conv_engine_t *ja; // romaji / kana-kanji decoder, NULL until installed
    const ui_kbd_conv_engine_t *conv; // the decoder of the layer that is on (zh or ja), else NULL
    char py[UI_VIEW_KBD_INPUT_CAP + 1u];
    uint8_t py_len;
    uint16_t zh_count;              // candidates of the last decode
    uint16_t zh_page;               // first candidate shown
    char zh_input_text[UI_VIEW_KBD_CAND_UTF8 + 4u * UI_VIEW_KBD_INPUT_CAP]; // fixed text, reading, letters
    char zh_cand_text[UI_VIEW_KBD_CANDS][UI_VIEW_KBD_CAND_UTF8 + 4u];
    ui_action_key_desc_t zh_desc[UI_VIEW_KBD_CANDS + 3u]; // candidate slots, previous, next, flick toggle

    // JA flick pad (FR-746): shown instead of the number and letter rows while the JA layer and `flick` are on
    lv_obj_t *flick_toggle;         // strip key "Flick" / "QWERTY", shown only on the JA layer
    lv_obj_t *flick_pad;
    lv_obj_t *flick_key[UI_VIEW_KBD_FLICK_KEYS];
    lv_obj_t *flick_label[UI_VIEW_KBD_FLICK_KEYS];
    ui_action_key_desc_t flick_desc[4]; // the right column: Backspace, Space, Enter, ABC
    bool flick;                     // the pad replaces the QWERTY field on the JA layer
    int8_t flick_down;              // kana key under the finger, -1 when none
    lv_point_t flick_start;         // where that press began
    uint8_t flick_n;                // kana typed on the pad that are still in `py`, newest last
    uint8_t flick_seg[UI_VIEW_KBD_INPUT_CAP]; // per kana: key (bits 0-3), direction (4-6)
    uint8_t flick_var[UI_VIEW_KBD_INPUT_CAP]; // per kana: 0 plain, 1 dakuten, 2 handakuten, 3 small
    uint8_t flick_len[UI_VIEW_KBD_INPUT_CAP]; // per kana: its romaji letters in `py`
    /** FR-741/742: the selected HOST layout, NULL for the compiled-in US ANSI field. */
    const kbd_layout_t *layout;
    char layout_id[UI_VIEW_KBD_LAYOUT_CAP]; // the id as it was set, even when it is not shipped ("" = none)
    // ADR-039: what the record holds, for the quick switch. Ids, so a regenerated table does not renumber them.
    char host_fav[UI_VIEW_KBD_FAV_MAX][UI_VIEW_KBD_LAYOUT_CAP];
    uint8_t host_fav_count;
    char host_recent[UI_VIEW_KBD_RECENT_MAX][UI_VIEW_KBD_LAYOUT_CAP];
    uint8_t host_recent_count;
    ui_view_keyboard_layout_fn layout_cb; // where a chosen layout is reported; NULL: the strip offers nothing
    void *layout_ctx;
    uint8_t usage_live[UI_VIEW_KBD_KEY_CAP]; // usage of each cap under `layout`; 0 = the table's own usage
    uint8_t langs;                  // UI_KBD_LANG_BIT mask from Settings; the EN bit is always set
    bool keys_enabled;              // mirror of the last applied enable state
    uint8_t held;                   // table index of the held ordinary key, or UI_VIEW_KBD_NO_KEY
    ui_view_keyboard_counters_t c;
} ui_view_keyboard_t;

/** Bind the view to the store and the action gateway and fill k->desc. Builds no object. */
void ui_view_keyboard_init(ui_view_keyboard_t *k, ui_store_t *store, ui_actions_t *actions,
                           uint64_t (*now_us)(void));

/**
 * FR-747: whether a Unicode delivery mode other than host-ime is selected. False turns the KO layer off (the
 * preedit is discarded, UI_ACT_TEXT_CANCEL submitted) and disables its cap. Defaults to false.
 */
void ui_view_keyboard_set_text_mode(ui_view_keyboard_t *k, bool unicode_delivery);

/** The two conversion layers. */
#define UI_KBD_LAYER_ZH 1u
#define UI_KBD_LAYER_JA 2u

/**
 * Install the decoder of the ZH or JA layer (the struct must outlive the view), or NULL to remove it; an incomplete
 * table counts as NULL. Removing or replacing it while that layer is on discards the composition and submits
 * UI_ACT_TEXT_CANCEL. The strip takes the layer's font when the layer turns on (UI_FONT_HANZI / UI_FONT_KANJI), so a
 * font installed after the view was built is picked up.
 */
void ui_view_keyboard_set_conv_engine(ui_view_keyboard_t *k, uint8_t layer, const ui_kbd_conv_engine_t *engine);

/** ui_view_keyboard_set_conv_engine(k, UI_KBD_LAYER_ZH, engine). */
void ui_view_keyboard_set_zh_engine(ui_view_keyboard_t *k, const ui_kbd_conv_engine_t *engine);

/** The router that shows the language picker and owns the toast layer. Call after ui_view_keyboard_init(). */
void ui_view_keyboard_bind_router(ui_view_keyboard_t *k, ui_router_t *router);

/** The view's overlay descriptors (UI_KBD_OVERLAYS of them) for ui_router_init(). */
const ui_overlay_descriptor_t *ui_view_keyboard_overlays(ui_view_keyboard_t *k, uint8_t *n);

/**
 * ADR-037: which composed languages Settings has turned on. English is always on and has no flag. Turning off the
 * language that is active falls back to English the way the cap does: the composition on screen is committed first.
 * Defaults to English alone.
 */
void ui_view_keyboard_set_languages(ui_view_keyboard_t *k, bool ko, bool zh, bool ja);

/** The mask of UI_KBD_LANG_BIT() that ui_view_keyboard_set_languages() left (the EN bit is always set). */
uint8_t ui_view_keyboard_languages(const ui_view_keyboard_t *k);

/** The language in effect right now, derived from the live layers - never a second copy of that state. */
ui_kbd_lang_t ui_view_keyboard_lang(const ui_view_keyboard_t *k);

/** The two-letter code on the cap ("EN") and the name in the picker ("English"); never NULL. */
const char *ui_view_keyboard_lang_code(ui_kbd_lang_t lang);
const char *ui_view_keyboard_lang_name(ui_kbd_lang_t lang);

/** Whether `lang` can be selected right now (turned on, HOST mounted, delivery mode, decoder installed). */
bool ui_view_keyboard_lang_available(const ui_view_keyboard_t *k, ui_kbd_lang_t lang);

/** Why it cannot be, for the picker and the toast; NULL when it can. */
const char *ui_view_keyboard_lang_reason(const ui_view_keyboard_t *k, ui_kbd_lang_t lang);

/** The language cap, the picker's row for `lang` and the text of the toast on screen (NULL / "" when down). */
lv_obj_t *ui_view_keyboard_lang_key(const ui_view_keyboard_t *k);
lv_obj_t *ui_view_keyboard_lang_pick_button(const ui_view_keyboard_t *k, ui_kbd_lang_t lang);
bool ui_view_keyboard_picker_open(const ui_view_keyboard_t *k);
const char *ui_view_keyboard_toast_text(const ui_view_keyboard_t *k);

/**
 * FR-741: draw the field for the HOST layout `id` ("us", "de(neo)"); NULL or "" goes back to the compiled-in US
 * ANSI field. An id the generated tables do not ship is remembered but changes nothing on screen (counter
 * `layout_unknown`), so a record written by a build with a larger table never produces invented legends. Safe
 * before the view is built: the field is drawn for the layout when it is.
 */
void ui_view_keyboard_set_host_layout(ui_view_keyboard_t *k, const char *id);

/** The id that was set, "" when none. Never NULL. */
const char *ui_view_keyboard_host_layout(const ui_view_keyboard_t *k);

/** The selected layout, or NULL when none is in force (the US ANSI field). */
const kbd_layout_t *ui_view_keyboard_layout(const ui_view_keyboard_t *k);

/** Geometry family on screen: the layout's, or KBD_GEOM_ANSI without one (FR-742). */
kbd_geometry_t ui_view_keyboard_geometry(const ui_view_keyboard_t *k);

/** The key slot of cap `idx`, or KBD_SLOT_COUNT for a cap that owns none (Esc, Shift, the arrows, F1-F12...). */
kbd_slot_t ui_view_keyboard_key_slot(uint32_t idx);

/**
 * What cap `idx` sends right now: the usage of its slot in the selected layout, else the table's own usage
 * (FR-743 - it is never derived from a legend). 0 for the local keys.
 */
uint8_t ui_view_keyboard_key_live_usage(const ui_view_keyboard_t *k, uint32_t idx);

/** Whether cap `idx` is drawn under the geometry family on screen (false for a hidden LSGT, Yen, IME key...). */
bool ui_view_keyboard_key_shown(const ui_view_keyboard_t *k, uint32_t idx);

/** The legend of cap `idx`: `level` 0 the cap's own label, 1 its Shift corner, 2 its AltGr corner. "" when
 *  that corner is empty, NULL for an unknown cap or a closed view. */
const char *ui_view_keyboard_key_legend(const ui_view_keyboard_t *k, uint32_t idx, unsigned level);

/**
 * ADR-039 section 8. The two lists of the record, so the quick switch can offer them, and the sink a chosen
 * layout is reported to. Without the sink the strip is inert: a screen that cannot change a value must not
 * offer to (07 section 12).
 */
void ui_view_keyboard_set_host_lists(ui_view_keyboard_t *k, const char fav[][UI_VIEW_KBD_LAYOUT_CAP],
                                     uint8_t fav_count, const char recent[][UI_VIEW_KBD_LAYOUT_CAP],
                                     uint8_t recent_count);
void ui_view_keyboard_set_layout_action(ui_view_keyboard_t *k, ui_view_keyboard_layout_fn cb, void *ctx);

/**
 * The layout strip over the field: a control (keyboard icon, short id, name, chevron) that a tap or a long
 * press opens the quick switch with. NULL before the view is built; hidden while no layout is in force.
 */
lv_obj_t *ui_view_keyboard_layout_strip(const ui_view_keyboard_t *k);

/** What the strip shows: the short id ("DE", the base of the xkb id upper case) and the layout's name. */
const char *ui_view_keyboard_layout_code(const ui_view_keyboard_t *k);
const char *ui_view_keyboard_layout_name(const ui_view_keyboard_t *k);

/** The quick-switch card while it is up, else NULL, and tile `i` of it (favorites first, then recents). */
lv_obj_t *ui_view_keyboard_layout_card(const ui_view_keyboard_t *k);
lv_obj_t *ui_view_keyboard_layout_pick_button(const ui_view_keyboard_t *k, uint8_t i);
const char *ui_view_keyboard_layout_pick_text(const ui_view_keyboard_t *k, uint8_t i);

/** The FR-742 hint over the field while a layout is in force, else "". Never NULL. */
const char *ui_view_keyboard_hint_text(const ui_view_keyboard_t *k);

/** Number of caps in the layout table (<= UI_VIEW_KBD_KEY_CAP). */
uint32_t ui_view_keyboard_key_count(void);

/** HID keyboard usage of cap `idx` (page 0x07), or 0 for the "F1-F12" and language keys, which have none. */
uint8_t ui_view_keyboard_key_usage(uint32_t idx);

/**
 * Legend of cap `idx` for diagnostics: its label, else its icon, never NULL. The language cap keeps the stable name
 * "Lang" - its legend changes with the language, so a lookup by name would not be reproducible.
 */
const char *ui_view_keyboard_key_name(uint32_t idx);

/** The widget of cap `idx` while the view exists, else NULL. */
lv_obj_t *ui_view_keyboard_key_obj(const ui_view_keyboard_t *k, uint32_t idx);

#ifdef __cplusplus
}
#endif
