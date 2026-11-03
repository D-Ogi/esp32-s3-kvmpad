// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Design tokens (TASK-060D, 07 §7): the single place where colors, typography roles, spacing, radii,
// touch targets and the shared geometry of the shell live. "Do not copy 50 magic values across the
// views" (07 §7) means: a view reads a token, it does not invent a number.
//
// Geometry scale. The reference proportions come from `reference/kvm-essential-800x480.html` and
// 01 §2.1 (topbar 65 px, content inset 16 px at 800x480). The target panel is 1024x600 (ADR-026
// TECH-087), so lengths scale by 1024/800 = 1.28 horizontally and 600/480 = 1.25 vertically and are
// then rounded to a multiple of 4. Values that the specification states as absolute minimums -
// spacing 4/8/12/16 and the 44 px / 48 px touch targets of 07 §7 and FR-003 - are NOT scaled: they
// are accessibility floors, not proportions.
//
// Pure C plus LVGL types. No USB, no network, no pairing logic (07 §2 dependency rule).
#pragma once

#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------
// Panel geometry (ADR-026 TECH-087: 1024x600 RGB565)
// ---------------------------------------------------------------------------------------------
#define UI_SCREEN_W 1024
#define UI_SCREEN_H 600

#define UI_TOPBAR_H      80 // 65 * 600/480 = 81.25 -> 80 (FR-003: permanent, never grows)
#define UI_CONTENT_INSET 20 // 16 * 1024/800 = 20.48 -> 20 (01 §2.1 x=16..783)
#define UI_CONTENT_TOP   UI_TOPBAR_H
#define UI_CONTENT_H     (UI_SCREEN_H - UI_TOPBAR_H)

// ---------------------------------------------------------------------------------------------
// Spacing (07 §7: 4/8/12/16) and the two scaled steps the 1024x600 shell needs
// ---------------------------------------------------------------------------------------------
#define UI_SPACE_XS  4
#define UI_SPACE_S   8
#define UI_SPACE_M   12
#define UI_SPACE_L   16
#define UI_SPACE_XL  20 // = UI_CONTENT_INSET
#define UI_SPACE_XXL 24

// ---------------------------------------------------------------------------------------------
// Corner radii (reference: key 11, button 15, card/modal 20, pad 22; scaled and rounded)
// ---------------------------------------------------------------------------------------------
#define UI_RADIUS_S  8
#define UI_RADIUS_M  14 // key cap, chip, status pill
#define UI_RADIUS_L  18 // button, segmented tab
#define UI_RADIUS_XL 24 // card, modal, touch surface

// ---------------------------------------------------------------------------------------------
// Dividers and borders (reference: 1 px hairlines; no blur or shadow stacks, 07 §9)
// ---------------------------------------------------------------------------------------------
#define UI_DIVIDER_W     1
#define UI_BORDER_W      1
#define UI_BORDER_W_BOLD 2
#define UI_FOCUS_RING_W  3 // reference :focus-visible outline

// ---------------------------------------------------------------------------------------------
// Touch targets (07 §7 and FR-003: minimum 44x44 px, >= 48 px preferred). Never scaled.
// ---------------------------------------------------------------------------------------------
#define UI_TOUCH_MIN       44
#define UI_TOUCH_PREFERRED 48
#define UI_TOUCH_COMFORT   56 // settings button, modal buttons, segmented tabs
#define UI_FLICK_MIN       24 // JA flick pad: a shorter travel is a tap (about half a key)

// ---------------------------------------------------------------------------------------------
// Shell slot geometry. The topbar budget is fixed so that a long IP/SSID or status string can
// never push a slot out of the bar (07 §7: "the topbar must not grow depending on the length of
// the IP/SSID"). The slot proportions come from the reference (.brand 96, .modes 357 at 800 px).
//
//   inner width            1024 - 2*16                     = 992
//   brand + nav + keep-awake + 2*status + button           = 960
//   6 gaps of 4 px between the 7 slots                     =  24
//   spacer (absorbs the rest, keeps the statuses right)    =   8
//
// 07 §7 gives the bar "navigation + two independent HOST/CLIENT statuses". A third status does not
// fit beside a reference-width navigation; ownership and link detail belong in the UiStatusItem
// details overlay, as in the reference popover.
// ---------------------------------------------------------------------------------------------
#define UI_TOPBAR_PAD          UI_SPACE_L
#define UI_TOPBAR_GAP          UI_SPACE_XS
#define UI_TOPBAR_BRAND_W      96
#define UI_TOPBAR_NAV_W        392 // reference .modes 357 * 1.28 = 457, trimmed for the status slots
#define UI_TOPBAR_STATUS_W     152 // fits "Disconnected" at UI_FONT_BODY beside its glyph
#define UI_TOPBAR_KEEP_AWAKE_W 112 // FR-712 indicator: icon + text, hidden while the feature is off
#define UI_TOPBAR_BTN_W        UI_TOUCH_COMFORT

#define UI_SEGMENTED_H   60 // 52 * 1.25 = 65 -> 60; tab height 52 >= UI_TOUCH_PREFERRED
#define UI_SEGMENTED_PAD UI_SPACE_XS

#define UI_STATUS_ITEM_H UI_TOUCH_PREFERRED // FR-003: tapping a status opens its details
#define UI_STATUS_DOT    10                 // 8 * 1.25 = 10

#define UI_MODAL_W        480
#define UI_MODAL_MIN_W    360
#define UI_MODAL_MAX_H    448
#define UI_MODAL_PAD      UI_SPACE_XXL
#define UI_MODAL_BTN_H    UI_TOUCH_COMFORT
#define UI_SCRIM_OPA      LV_OPA_50

#define UI_TOAST_BOTTOM      32 // 27 * 1.25 = 33.75 -> 32
#define UI_TOAST_PAD_HOR     UI_SPACE_XXL
#define UI_TOAST_PAD_VER     UI_SPACE_L
#define UI_TOAST_MAX_W       640
#define UI_TOAST_DEFAULT_MS  2400

// ---------------------------------------------------------------------------------------------
// Colors. Every value is taken from `reference/kvm-essential-800x480.html` (light theme, FR-008:
// "a simplified, calm appearance on a light background"), except UI_COLOR_WARN* - the reference
// has no amber although 07 §7 defines a `warning` status variant. See the README for that gap.
// FR-005: color is never the only carrier; every severity also has text and an icon.
// ---------------------------------------------------------------------------------------------
#define UI_COLOR_BG      0xF5F5F7u // --bg
#define UI_COLOR_SURFACE 0xFFFFFFu // --surface
#define UI_COLOR_WELL    0xEDEDF0u // --well: recessed track (segmented control, key wells)
#define UI_COLOR_RULE    0xE5E5E9u // --rule: divider / hairline border
#define UI_COLOR_EDGE    0xE2E3E8u // key and card border
#define UI_COLOR_SHADE   0xDCDDE1u // 2 px key under-edge

#define UI_COLOR_INK       0x1D1D1Fu // --ink: primary text
#define UI_COLOR_MUTED     0x777982u // --muted: secondary text
#define UI_COLOR_SUBTLE    0xA0A3ACu // --subtle: tertiary text
#define UI_COLOR_INVERSE   0x202126u // primary button / toast surface
#define UI_COLOR_ON_INVERSE 0xFFFFFFu
#define UI_COLOR_CHIP_FEATURED 0x424249u // glyph chip on an inverse surface (reference featured tile)

#define UI_COLOR_ACCENT      0x007AFFu // --accent
#define UI_COLOR_ACCENT_INK  0x006ACBu // accent text on a tinted surface (contrast on #D7EAFF)
#define UI_COLOR_ACCENT_TINT 0xE1F0FFu
#define UI_COLOR_ACCENT_EDGE 0xB8DCFFu

#define UI_COLOR_OK      0x158652u // popover .online, text grade
#define UI_COLOR_OK_DOT  0x27AF77u // --green, dot grade
#define UI_COLOR_WARN     0x8A5A00u // NOT in the reference: text-grade amber chosen for contrast
#define UI_COLOR_WARN_DOT 0xE8920Cu // NOT in the reference: dot-grade amber
#define UI_COLOR_DANGER     0xD3474Eu // popover .offline, text grade
#define UI_COLOR_DANGER_DOT 0xEF5B5Bu // --red, dot grade

// State colors (07 §7: disabled / pressed / active / checked)
//
// A disabled control is dimmed by these two tokens alone, never by a blanket opacity: the reference's
// `opacity: .45` on top of #B4B6BC leaves roughly 1.1:1 against the page, which FR-604 (legible at low
// ambient contrast) and FR-005 (a state is never carried by one channel) both rule out. #9A9CA4 on the
// disabled surface keeps about 2.5:1 - clearly weaker than the 4:1 of an enabled muted legend, and still
// readable on the panel.
#define UI_COLOR_DISABLED_INK     0x9A9CA4u // reference .disabled-key #B4B6BC, darkened for contrast
#define UI_COLOR_DISABLED_SURFACE 0xF1F1F4u
#define UI_COLOR_PRESSED_SURFACE  0xCFE6FFu // .key.just-pressed
#define UI_COLOR_PRESSED_QUIET    0xF3F6FAu // .command:active
#define UI_COLOR_ACTIVE_SURFACE   UI_COLOR_ACCENT_TINT // .foot-btn.is-active
#define UI_COLOR_ACTIVE_INK       0x006DE4u
#define UI_COLOR_CHECKED_SURFACE  0xD7EAFFu // .key.selected
#define UI_COLOR_CHECKED_EDGE     0xBEDCF8u
#define UI_COLOR_CHECKED_INK      UI_COLOR_ACCENT_INK
#define UI_COLOR_SPECIAL_SURFACE  0xE6E7EBu // .key.special (modifier keys)

#define UI_COLOR_DISABLED_EDGE    UI_COLOR_RULE // border of a disabled control

// ---------------------------------------------------------------------------------------------
// Typography roles. Only the Montserrat sizes compiled into the firmware may be named here:
// sdkconfig.defaults enables CONFIG_LV_FONT_MONTSERRAT_14/16/20/24 and nothing else.
// ---------------------------------------------------------------------------------------------
typedef enum {
    UI_FONT_TITLE = 0, // Montserrat 24: screen title, key cap, brand
    UI_FONT_HEADING,   // Montserrat 20: modal title, card name
    UI_FONT_BODY,      // Montserrat 16: status text, tab label, button label
    UI_FONT_LABEL,     // Montserrat 14: caption, secondary line, utility key
    UI_FONT_HANGUL,    // 24 px Hangul subset (ADR-024 TECH-078): the KO preedit; Latin from Montserrat 24
    UI_FONT_HANZI,     // 24 px hanzi of the pinyin candidate bar, read in place from ime_data (ui_font_blob.h)
    UI_FONT_KANJI,     // 24 px kana and kanji of the Japanese candidate bar (Noto Sans JP), from ime_data likewise
                       // once ui_theme_set_hanzi_font() installed it; Montserrat 24 until then and for Latin
    UI_FONT_COUNT,
} ui_font_role_t;

// ---------------------------------------------------------------------------------------------
// Variants (07 §7: "Avoid 30 parameters in a single generic factory")
// ---------------------------------------------------------------------------------------------

/**
 * Status severity (07 §7 variants `ok`, `warning`, `offline`, `unknown`).
 * UNKNOWN is deliberately the zero value: a zero-initialized descriptor must read "unknown",
 * never "ok" (07 §3 security note, 07 §12 item 2 - the UI never upgrades a status it was not told).
 */
typedef enum {
    UI_SEVERITY_UNKNOWN = 0,
    UI_SEVERITY_OK,
    UI_SEVERITY_WARNING,
    UI_SEVERITY_OFFLINE,
    UI_SEVERITY_COUNT,
} ui_severity_t;

/** Key variants (07 §7: `normal`, `modifier`, `destructive`). */
typedef enum {
    UI_KEY_NORMAL = 0,
    UI_KEY_MODIFIER,
    UI_KEY_DESTRUCTIVE,
    UI_KEY_VARIANT_COUNT,
} ui_key_variant_t;

/** Button variants (07 §7: `primary`, `secondary`, `quiet`). */
typedef enum {
    UI_BTN_PRIMARY = 0,
    UI_BTN_SECONDARY,
    UI_BTN_QUIET,
    UI_BTN_VARIANT_COUNT,
} ui_btn_variant_t;

#ifdef __cplusplus
}
#endif
