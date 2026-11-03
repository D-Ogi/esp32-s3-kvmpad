// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shared LVGL styles (TASK-060D, 07 §7): "A single owner, `ui_theme`, creates the shared LVGL
// styles *once* at start-up." The styles are static storage; the accessors hand out `const`
// pointers for `lv_obj_add_style()`, which keeps the pointer, so a style must outlive every widget
// that uses it - call ui_theme_deinit() only after the widget tree is gone.
//
// Threading: every entry point here touches LVGL state and may therefore run only in the LVGL
// owner context (the ui_platform task, or under ui_platform_lock()) - 07 §3 concurrency flow.
#pragma once

#include <stdbool.h>

#include "lvgl.h"
#include "ui_tokens.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Identifiers of the shared styles. One style per visual role, not one per widget. */
typedef enum {
    UI_STYLE_SCREEN = 0,  // page background (FR-008 calm light background)
    UI_STYLE_TOPBAR,      // fixed-height chrome bar with a bottom hairline
    UI_STYLE_CARD,        // raised surface: command tile, modal, popover
    UI_STYLE_WELL,        // recessed track: segmented control, key well
    UI_STYLE_DIVIDER,     // 1 px rule
    UI_STYLE_FOCUS,       // focus ring, applied with LV_STATE_FOCUS_KEY

    UI_STYLE_TEXT_TITLE,  // Montserrat 24 ink
    UI_STYLE_TEXT_HEADING,// Montserrat 20 ink
    UI_STYLE_TEXT_BODY,   // Montserrat 16 ink
    UI_STYLE_TEXT_LABEL,  // Montserrat 14 ink
    UI_STYLE_TEXT_MUTED,  // Montserrat 14 muted
    UI_STYLE_TEXT_INVERSE,// Montserrat 16 on an inverse surface

    UI_STYLE_BTN_BASE,      // geometry and touch target of every tappable control
    UI_STYLE_BTN_PRIMARY,
    UI_STYLE_BTN_SECONDARY,
    UI_STYLE_BTN_QUIET,
    UI_STYLE_STATE_PRESSED, // add with the LV_STATE_PRESSED selector
    UI_STYLE_STATE_CHECKED, // add with the LV_STATE_CHECKED selector
    UI_STYLE_STATE_DISABLED,// add with the LV_STATE_DISABLED selector

    UI_STYLE_KEY_NORMAL,
    UI_STYLE_KEY_MODIFIER,
    UI_STYLE_KEY_DESTRUCTIVE,

    UI_STYLE_SEG_TRACK,     // segmented control container
    UI_STYLE_SEG_TAB,       // one tab, unselected
    UI_STYLE_SEG_TAB_CHECKED,

    UI_STYLE_SCRIM,         // full-screen input blocker behind a modal
    UI_STYLE_TOAST,         // inverse pill, not clickable

    UI_STYLE_COUNT,
} ui_style_id_t;

/**
 * Create every shared style. Idempotent: a second call while the theme is ready is a no-op and
 * returns true, so a view may call it defensively without resetting the styles widgets point at.
 * Returns false only if LVGL is not initialized.
 */
bool ui_theme_init(void);

/**
 * Reset every shared style. Call it only after the widget tree that used the styles is deleted;
 * LVGL keeps the style pointers in the objects, so resetting a style still in use leaves a widget
 * reading a cleared property list. After this call ui_theme_style() returns NULL.
 */
void ui_theme_deinit(void);

bool ui_theme_is_ready(void);

/** The shared style for `id`, or NULL when `id` is out of range or the theme is not initialized. */
const lv_style_t *ui_theme_style(ui_style_id_t id);

/** Add a shared style to `obj`; a no-op when the style is unavailable. */
void ui_theme_apply(lv_obj_t *obj, ui_style_id_t id, lv_style_selector_t selector);

/** Add UI_STYLE_BTN_BASE, the variant style and the three state styles in the documented order. */
void ui_theme_apply_button(lv_obj_t *obj, ui_btn_variant_t variant);

/** Add UI_STYLE_BTN_BASE, the key variant style and the three state styles. */
void ui_theme_apply_key(lv_obj_t *obj, ui_key_variant_t variant);

/** Font for a typography role; UI_FONT_BODY for an out-of-range role, never NULL. */
const lv_font_t *ui_theme_font(ui_font_role_t role);

/**
 * ADR-024 TECH-078: whether this build links the keyboard legend fonts, which add the Latin-extended, Greek and
 * Cyrillic code points of the HOST layouts to UI_FONT_TITLE, UI_FONT_BODY and UI_FONT_LABEL. Without them those
 * legends have no glyph and the caller shows ui_theme_legend_fallback() instead.
 */
bool ui_theme_has_legend_font(void);

/** The one-code-point string a cap shows in place of a legend no shipped font has (U+FFFD). Never NULL. */
const char *ui_theme_legend_fallback(void);

/**
 * TECH-078: `text` when every one of its code points has a glyph in `font` (following the font's fallback
 * chain), else ui_theme_legend_fallback(). A legend from a script the shipped fonts leave out then shows a
 * visible placeholder instead of an empty cap, and the key still sends its usage (FR-743). A space is accepted
 * whatever the font says. `text` is returned unchanged when it is NULL or empty, or when `font` is NULL.
 */
const char *ui_theme_drawable(const lv_font_t *font, const char *text);

/**
 * Install the font of UI_FONT_HANZI (the head of a ui_font_blob chain whose last part falls back to Montserrat 24),
 * or NULL to go back to Montserrat 24. The font must outlive every widget that uses it; a widget styled before the
 * call keeps the font it was given. LVGL owner context.
 */
void ui_theme_set_hanzi_font(const lv_font_t *font);

/** The same for UI_FONT_KANJI (the Japanese candidate bar, Noto Sans JP). */
void ui_theme_set_kanji_font(const lv_font_t *font);

/** Text-grade color of a severity (legible on UI_COLOR_BG and UI_COLOR_SURFACE). */
lv_color_t ui_theme_severity_color(ui_severity_t severity);

/** Dot-grade color of a severity (saturated, used for the 10 px status dot only). */
lv_color_t ui_theme_severity_dot(ui_severity_t severity);

/**
 * Icon glyph of a severity - FR-005: the status must not be carried by color alone. The glyphs are
 * LV_SYMBOL_* entries present in every built-in Montserrat font. Never NULL.
 */
const char *ui_theme_severity_icon(ui_severity_t severity);

#ifdef __cplusplus
}
#endif
