// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// The one owner of the shared LVGL styles (TASK-060D, 07 §7). Every number comes from ui_tokens.h.
// No shadows and no gradients: 07 §9 forbids effects that need repeated rasterization on the RGB
// panel, and the reference look survives without them (1 px hairlines carry the elevation).
#include "ui_theme.h"

#include "ui_tokens.h"

static lv_style_t s_style[UI_STYLE_COUNT];
static bool s_ready;

static void surface(lv_style_t *s, uint32_t bg, uint32_t border, int32_t radius, int32_t width)
{
    lv_style_set_bg_color(s, lv_color_hex(bg));
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, radius);
    lv_style_set_border_color(s, lv_color_hex(border));
    lv_style_set_border_width(s, width);
    lv_style_set_border_side(s, LV_BORDER_SIDE_FULL);
    lv_style_set_outline_width(s, 0);
}

static void text(lv_style_t *s, ui_font_role_t role, uint32_t color)
{
    lv_style_set_text_font(s, ui_theme_font(role));
    lv_style_set_text_color(s, lv_color_hex(color));
}

static void init_surfaces(void)
{
    lv_style_set_bg_color(&s_style[UI_STYLE_SCREEN], lv_color_hex(UI_COLOR_BG));
    lv_style_set_bg_opa(&s_style[UI_STYLE_SCREEN], LV_OPA_COVER);
    lv_style_set_pad_all(&s_style[UI_STYLE_SCREEN], 0);
    lv_style_set_radius(&s_style[UI_STYLE_SCREEN], 0);
    lv_style_set_border_width(&s_style[UI_STYLE_SCREEN], 0);

    // Topbar: fixed height, hairline only at the bottom; FR-003 wants it permanent and stable.
    surface(&s_style[UI_STYLE_TOPBAR], UI_COLOR_BG, UI_COLOR_RULE, 0, UI_DIVIDER_W);
    lv_style_set_border_side(&s_style[UI_STYLE_TOPBAR], LV_BORDER_SIDE_BOTTOM);
    lv_style_set_height(&s_style[UI_STYLE_TOPBAR], UI_TOPBAR_H);
    lv_style_set_min_height(&s_style[UI_STYLE_TOPBAR], UI_TOPBAR_H);
    lv_style_set_max_height(&s_style[UI_STYLE_TOPBAR], UI_TOPBAR_H);
    lv_style_set_pad_ver(&s_style[UI_STYLE_TOPBAR], 0);
    lv_style_set_pad_hor(&s_style[UI_STYLE_TOPBAR], UI_TOPBAR_PAD);
    lv_style_set_pad_gap(&s_style[UI_STYLE_TOPBAR], UI_TOPBAR_GAP);

    surface(&s_style[UI_STYLE_CARD], UI_COLOR_SURFACE, UI_COLOR_EDGE, UI_RADIUS_XL, UI_BORDER_W);
    lv_style_set_pad_all(&s_style[UI_STYLE_CARD], UI_SPACE_L);
    lv_style_set_pad_gap(&s_style[UI_STYLE_CARD], UI_SPACE_S);

    surface(&s_style[UI_STYLE_WELL], UI_COLOR_WELL, UI_COLOR_WELL, UI_RADIUS_L, 0);
    lv_style_set_pad_all(&s_style[UI_STYLE_WELL], UI_SPACE_XS);
    lv_style_set_pad_gap(&s_style[UI_STYLE_WELL], UI_SPACE_XS);

    surface(&s_style[UI_STYLE_DIVIDER], UI_COLOR_RULE, UI_COLOR_RULE, 0, 0);
    lv_style_set_height(&s_style[UI_STYLE_DIVIDER], UI_DIVIDER_W);
    lv_style_set_pad_all(&s_style[UI_STYLE_DIVIDER], 0);

    lv_style_set_outline_width(&s_style[UI_STYLE_FOCUS], UI_FOCUS_RING_W);
    lv_style_set_outline_color(&s_style[UI_STYLE_FOCUS], lv_color_hex(UI_COLOR_ACCENT));
    lv_style_set_outline_pad(&s_style[UI_STYLE_FOCUS], UI_SPACE_XS / 2);
}

static void init_text(void)
{
    text(&s_style[UI_STYLE_TEXT_TITLE], UI_FONT_TITLE, UI_COLOR_INK);
    text(&s_style[UI_STYLE_TEXT_HEADING], UI_FONT_HEADING, UI_COLOR_INK);
    text(&s_style[UI_STYLE_TEXT_BODY], UI_FONT_BODY, UI_COLOR_INK);
    text(&s_style[UI_STYLE_TEXT_LABEL], UI_FONT_LABEL, UI_COLOR_INK);
    text(&s_style[UI_STYLE_TEXT_MUTED], UI_FONT_LABEL, UI_COLOR_MUTED);
    text(&s_style[UI_STYLE_TEXT_INVERSE], UI_FONT_BODY, UI_COLOR_ON_INVERSE);
}

static void init_controls(void)
{
    // Touch target and metrics shared by every tappable control (07 §7, FR-003).
    lv_style_t *b = &s_style[UI_STYLE_BTN_BASE];
    lv_style_set_min_width(b, UI_TOUCH_MIN);
    lv_style_set_min_height(b, UI_TOUCH_PREFERRED);
    lv_style_set_radius(b, UI_RADIUS_L);
    lv_style_set_pad_hor(b, UI_SPACE_M);
    lv_style_set_pad_ver(b, UI_SPACE_S);
    lv_style_set_pad_gap(b, UI_SPACE_S);
    lv_style_set_text_font(b, ui_theme_font(UI_FONT_BODY));
    lv_style_set_text_align(b, LV_TEXT_ALIGN_CENTER);
    lv_style_set_bg_opa(b, LV_OPA_COVER);
    lv_style_set_border_width(b, UI_BORDER_W);
    lv_style_set_border_side(b, LV_BORDER_SIDE_FULL);
    lv_style_set_outline_width(b, 0);

    surface(&s_style[UI_STYLE_BTN_PRIMARY], UI_COLOR_INVERSE, UI_COLOR_INVERSE, UI_RADIUS_L, UI_BORDER_W);
    lv_style_set_text_color(&s_style[UI_STYLE_BTN_PRIMARY], lv_color_hex(UI_COLOR_ON_INVERSE));

    surface(&s_style[UI_STYLE_BTN_SECONDARY], UI_COLOR_SURFACE, UI_COLOR_EDGE, UI_RADIUS_L, UI_BORDER_W);
    lv_style_set_text_color(&s_style[UI_STYLE_BTN_SECONDARY], lv_color_hex(UI_COLOR_INK));

    surface(&s_style[UI_STYLE_BTN_QUIET], UI_COLOR_BG, UI_COLOR_BG, UI_RADIUS_L, 0);
    lv_style_set_bg_opa(&s_style[UI_STYLE_BTN_QUIET], LV_OPA_TRANSP);
    lv_style_set_text_color(&s_style[UI_STYLE_BTN_QUIET], lv_color_hex(UI_COLOR_MUTED));

    lv_style_set_bg_color(&s_style[UI_STYLE_STATE_PRESSED], lv_color_hex(UI_COLOR_PRESSED_SURFACE));
    lv_style_set_bg_opa(&s_style[UI_STYLE_STATE_PRESSED], LV_OPA_COVER);
    lv_style_set_text_color(&s_style[UI_STYLE_STATE_PRESSED], lv_color_hex(UI_COLOR_ACTIVE_INK));
    lv_style_set_translate_y(&s_style[UI_STYLE_STATE_PRESSED], 1);

    lv_style_set_bg_color(&s_style[UI_STYLE_STATE_CHECKED], lv_color_hex(UI_COLOR_CHECKED_SURFACE));
    lv_style_set_bg_opa(&s_style[UI_STYLE_STATE_CHECKED], LV_OPA_COVER);
    lv_style_set_border_color(&s_style[UI_STYLE_STATE_CHECKED], lv_color_hex(UI_COLOR_CHECKED_EDGE));
    lv_style_set_text_color(&s_style[UI_STYLE_STATE_CHECKED], lv_color_hex(UI_COLOR_CHECKED_INK));

    // No blanket opacity here (see UI_COLOR_DISABLED_INK): a disabled control keeps its full coverage and is
    // read as disabled from its surface, its ink and its border, so the legend never fades into the page.
    lv_style_set_bg_color(&s_style[UI_STYLE_STATE_DISABLED], lv_color_hex(UI_COLOR_DISABLED_SURFACE));
    lv_style_set_bg_opa(&s_style[UI_STYLE_STATE_DISABLED], LV_OPA_COVER);
    lv_style_set_text_color(&s_style[UI_STYLE_STATE_DISABLED], lv_color_hex(UI_COLOR_DISABLED_INK));
    lv_style_set_border_color(&s_style[UI_STYLE_STATE_DISABLED], lv_color_hex(UI_COLOR_DISABLED_EDGE));
    lv_style_set_opa(&s_style[UI_STYLE_STATE_DISABLED], LV_OPA_COVER);
}

static void init_keys(void)
{
    surface(&s_style[UI_STYLE_KEY_NORMAL], UI_COLOR_SURFACE, UI_COLOR_EDGE, UI_RADIUS_M, UI_BORDER_W);
    lv_style_set_text_font(&s_style[UI_STYLE_KEY_NORMAL], ui_theme_font(UI_FONT_TITLE));
    lv_style_set_text_color(&s_style[UI_STYLE_KEY_NORMAL], lv_color_hex(UI_COLOR_INK));

    surface(&s_style[UI_STYLE_KEY_MODIFIER], UI_COLOR_SPECIAL_SURFACE, UI_COLOR_EDGE, UI_RADIUS_M, UI_BORDER_W);
    lv_style_set_text_font(&s_style[UI_STYLE_KEY_MODIFIER], ui_theme_font(UI_FONT_BODY));
    lv_style_set_text_color(&s_style[UI_STYLE_KEY_MODIFIER], lv_color_hex(UI_COLOR_MUTED));

    surface(&s_style[UI_STYLE_KEY_DESTRUCTIVE], UI_COLOR_SURFACE, UI_COLOR_DANGER, UI_RADIUS_M, UI_BORDER_W_BOLD);
    lv_style_set_text_font(&s_style[UI_STYLE_KEY_DESTRUCTIVE], ui_theme_font(UI_FONT_BODY));
    lv_style_set_text_color(&s_style[UI_STYLE_KEY_DESTRUCTIVE], lv_color_hex(UI_COLOR_DANGER));
}

static void init_overlays(void)
{
    surface(&s_style[UI_STYLE_SEG_TRACK], UI_COLOR_WELL, UI_COLOR_WELL, UI_RADIUS_L, 0);
    lv_style_set_pad_all(&s_style[UI_STYLE_SEG_TRACK], UI_SEGMENTED_PAD);
    lv_style_set_pad_gap(&s_style[UI_STYLE_SEG_TRACK], 2);
    lv_style_set_height(&s_style[UI_STYLE_SEG_TRACK], UI_SEGMENTED_H);

    surface(&s_style[UI_STYLE_SEG_TAB], UI_COLOR_WELL, UI_COLOR_WELL, UI_RADIUS_M, 0);
    lv_style_set_bg_opa(&s_style[UI_STYLE_SEG_TAB], LV_OPA_TRANSP);
    text(&s_style[UI_STYLE_SEG_TAB], UI_FONT_BODY, UI_COLOR_MUTED);
    lv_style_set_min_height(&s_style[UI_STYLE_SEG_TAB], UI_TOUCH_MIN);
    lv_style_set_min_width(&s_style[UI_STYLE_SEG_TAB], UI_TOUCH_MIN);
    lv_style_set_pad_hor(&s_style[UI_STYLE_SEG_TAB], UI_SPACE_S);
    lv_style_set_pad_ver(&s_style[UI_STYLE_SEG_TAB], 0);
    lv_style_set_pad_gap(&s_style[UI_STYLE_SEG_TAB], UI_SPACE_XS);

    surface(&s_style[UI_STYLE_SEG_TAB_CHECKED], UI_COLOR_SURFACE, UI_COLOR_SURFACE, UI_RADIUS_M, 0);
    lv_style_set_text_color(&s_style[UI_STYLE_SEG_TAB_CHECKED], lv_color_hex(UI_COLOR_INK));

    // Scrim: covers the whole parent and is the reason a modal blocks the view below (AT-UIFW-10).
    surface(&s_style[UI_STYLE_SCRIM], UI_COLOR_INK, UI_COLOR_INK, 0, 0);
    lv_style_set_bg_opa(&s_style[UI_STYLE_SCRIM], UI_SCRIM_OPA);
    lv_style_set_pad_all(&s_style[UI_STYLE_SCRIM], 0);

    surface(&s_style[UI_STYLE_TOAST], UI_COLOR_INVERSE, UI_COLOR_INVERSE, UI_RADIUS_L, 0);
    lv_style_set_text_font(&s_style[UI_STYLE_TOAST], ui_theme_font(UI_FONT_BODY));
    lv_style_set_text_color(&s_style[UI_STYLE_TOAST], lv_color_hex(UI_COLOR_ON_INVERSE));
    lv_style_set_pad_hor(&s_style[UI_STYLE_TOAST], UI_TOAST_PAD_HOR);
    lv_style_set_pad_ver(&s_style[UI_STYLE_TOAST], UI_TOAST_PAD_VER);
    lv_style_set_max_width(&s_style[UI_STYLE_TOAST], UI_TOAST_MAX_W);
}

bool ui_theme_init(void)
{
    if (s_ready) {
        return true;
    }
    if (!lv_is_initialized()) {
        return false;
    }
    for (int i = 0; i < UI_STYLE_COUNT; i++) {
        lv_style_init(&s_style[i]);
    }
    init_surfaces();
    init_text();
    init_controls();
    init_keys();
    init_overlays();
    s_ready = true;
    return true;
}

void ui_theme_deinit(void)
{
    if (!s_ready) {
        return;
    }
    s_ready = false;
    for (int i = 0; i < UI_STYLE_COUNT; i++) {
        lv_style_reset(&s_style[i]);
    }
}

bool ui_theme_is_ready(void)
{
    return s_ready;
}

const lv_style_t *ui_theme_style(ui_style_id_t id)
{
    if (!s_ready || (int)id < 0 || (int)id >= (int)UI_STYLE_COUNT) {
        return NULL;
    }
    return &s_style[id];
}

void ui_theme_apply(lv_obj_t *obj, ui_style_id_t id, lv_style_selector_t selector)
{
    const lv_style_t *st = ui_theme_style(id);
    if (obj && st) {
        lv_obj_add_style(obj, st, selector);
    }
}

static void apply_states(lv_obj_t *obj)
{
    ui_theme_apply(obj, UI_STYLE_STATE_PRESSED, LV_STATE_PRESSED);
    ui_theme_apply(obj, UI_STYLE_STATE_CHECKED, LV_STATE_CHECKED);
    ui_theme_apply(obj, UI_STYLE_STATE_DISABLED, LV_STATE_DISABLED);
    ui_theme_apply(obj, UI_STYLE_FOCUS, LV_STATE_FOCUS_KEY);
}

void ui_theme_apply_button(lv_obj_t *obj, ui_btn_variant_t variant)
{
    static const ui_style_id_t map[UI_BTN_VARIANT_COUNT] = {
        UI_STYLE_BTN_PRIMARY,
        UI_STYLE_BTN_SECONDARY,
        UI_STYLE_BTN_QUIET,
    };
    ui_theme_apply(obj, UI_STYLE_BTN_BASE, LV_STATE_DEFAULT);
    const int idx = ((int)variant >= 0 && (int)variant < (int)UI_BTN_VARIANT_COUNT) ? (int)variant : (int)UI_BTN_SECONDARY;
    ui_theme_apply(obj, map[idx], LV_STATE_DEFAULT);
    apply_states(obj);
}

void ui_theme_apply_key(lv_obj_t *obj, ui_key_variant_t variant)
{
    static const ui_style_id_t map[UI_KEY_VARIANT_COUNT] = {
        UI_STYLE_KEY_NORMAL,
        UI_STYLE_KEY_MODIFIER,
        UI_STYLE_KEY_DESTRUCTIVE,
    };
    ui_theme_apply(obj, UI_STYLE_BTN_BASE, LV_STATE_DEFAULT);
    const int idx = ((int)variant >= 0 && (int)variant < (int)UI_KEY_VARIANT_COUNT) ? (int)variant : (int)UI_KEY_NORMAL;
    ui_theme_apply(obj, map[idx], LV_STATE_DEFAULT);
    apply_states(obj);
}

// ADR-024 TECH-078: the Hangul legend subset (fonts/kvm_font_hangul_24.c, generated by
// tools/fonts/gen_hangul_font.py; Latin falls back to Montserrat 24 inside the font). A build that does not link it
// (KVM_HAVE_FONT_HANGUL_24 undefined: the lean host test builds) uses Montserrat 24, which shows no Hangul glyph
// rather than a wrong one.
#if defined(KVM_HAVE_FONT_HANGUL_24)
LV_FONT_DECLARE(kvm_font_hangul_24)
static const lv_font_t *ui_theme_font_hangul(void)
{
    return &kvm_font_hangul_24;
}
#else
static const lv_font_t *ui_theme_font_hangul(void)
{
    return &lv_font_montserrat_24;
}
#endif

// ADR-024 TECH-078 / FR-742: the keyboard legend subset (fonts/kvm_font_legend_{24,16,14}.c, generated by
// tools/fonts/gen_legend_font.py from Noto Sans). Montserrat stays the font of the UI: TITLE, BODY and LABEL hand
// out a copy of the Montserrat of that size whose LVGL `fallback` is the legend font, so every glyph Montserrat
// has is drawn exactly as before and only the Latin-extended, Greek and Cyrillic legends of the HOST layouts come
// from Noto Sans. A build without them (KVM_HAVE_FONT_LEGEND undefined: the lean host test builds) hands out
// Montserrat itself, which shows no glyph for those code points rather than a wrong one.
#if defined(KVM_HAVE_FONT_LEGEND)
LV_FONT_DECLARE(kvm_font_legend_24)
LV_FONT_DECLARE(kvm_font_legend_16)
LV_FONT_DECLARE(kvm_font_legend_14)

/**
 * Montserrat with the legend font behind it. The three structs are file-scope storage, so the pointers a widget
 * keeps stay valid for the life of the program; `chain()` is idempotent and needs no LVGL call.
 */
static lv_font_t s_text[3];

static const lv_font_t *chain(uint8_t i, const lv_font_t *base, const lv_font_t *legend)
{
    if (s_text[i].get_glyph_dsc == NULL) {
        s_text[i] = *base;
        s_text[i].fallback = legend;
    }
    return &s_text[i];
}

#define UI_FONT_TEXT_24 chain(0, &lv_font_montserrat_24, &kvm_font_legend_24)
#define UI_FONT_TEXT_16 chain(1, &lv_font_montserrat_16, &kvm_font_legend_16)
#define UI_FONT_TEXT_14 chain(2, &lv_font_montserrat_14, &kvm_font_legend_14)
#else
#define UI_FONT_TEXT_24 (&lv_font_montserrat_24)
#define UI_FONT_TEXT_16 (&lv_font_montserrat_16)
#define UI_FONT_TEXT_14 (&lv_font_montserrat_14)
#endif

/** The code point the legend fonts carry for a legend they have no glyph for (TECH-078). */
const char *ui_theme_legend_fallback(void)
{
    return "\xEF\xBF\xBD"; // U+FFFD REPLACEMENT CHARACTER
}

/**
 * The code point at `*i` of a UTF-8 string, advancing `*i` past it; 0 at the end and on a malformed byte.
 * LVGL's own iterator lives in a private header, so the three lines it would save are written out here.
 */
static uint32_t next_cp(const char *s, size_t *i)
{
    const unsigned char *p = (const unsigned char *)s + *i;
    uint32_t cp;
    size_t n;
    if (p[0] == 0u) {
        return 0u;
    }
    if (p[0] < 0x80u) {
        cp = p[0];
        n = 1u;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        cp = (uint32_t)(p[0] & 0x1Fu) << 6;
        n = 2u;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        cp = (uint32_t)(p[0] & 0x0Fu) << 12;
        n = 3u;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        cp = (uint32_t)(p[0] & 0x07u) << 18;
        n = 4u;
    } else {
        return 0u;
    }
    for (size_t k = 1; k < n; k++) {
        if ((p[k] & 0xC0u) != 0x80u) {
            return 0u;
        }
        cp |= (uint32_t)(p[k] & 0x3Fu) << (6u * (n - 1u - k));
    }
    *i += n;
    return cp;
}

const char *ui_theme_drawable(const lv_font_t *font, const char *text)
{
    if (font == NULL || text == NULL || text[0] == '\0') {
        return text;
    }
    size_t i = 0;
    uint32_t cp;
    while ((cp = next_cp(text, &i)) != 0u) {
        lv_font_glyph_dsc_t dsc;
        if (cp != (uint32_t)' ' && !lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
            return ui_theme_legend_fallback();
        }
    }
    return text;
}

bool ui_theme_has_legend_font(void)
{
#if defined(KVM_HAVE_FONT_LEGEND)
    return true;
#else
    return false;
#endif
}

static const lv_font_t *s_hanzi; // ui_theme_set_hanzi_font(); NULL until the ime_data font is mapped
static const lv_font_t *s_kanji; // ui_theme_set_kanji_font(); likewise

void ui_theme_set_hanzi_font(const lv_font_t *font)
{
    s_hanzi = font;
}

void ui_theme_set_kanji_font(const lv_font_t *font)
{
    s_kanji = font;
}

const lv_font_t *ui_theme_font(ui_font_role_t role)
{
    switch (role) {
    case UI_FONT_HANZI: return s_hanzi != NULL ? s_hanzi : &lv_font_montserrat_24;
    case UI_FONT_KANJI: return s_kanji != NULL ? s_kanji : &lv_font_montserrat_24;
    case UI_FONT_TITLE: return UI_FONT_TEXT_24;
    case UI_FONT_HEADING: return &lv_font_montserrat_20;
    case UI_FONT_LABEL: return UI_FONT_TEXT_14;
    case UI_FONT_HANGUL: return ui_theme_font_hangul();
    case UI_FONT_BODY:
    default: return UI_FONT_TEXT_16;
    }
}

lv_color_t ui_theme_severity_color(ui_severity_t severity)
{
    switch (severity) {
    case UI_SEVERITY_OK: return lv_color_hex(UI_COLOR_OK);
    case UI_SEVERITY_WARNING: return lv_color_hex(UI_COLOR_WARN);
    case UI_SEVERITY_OFFLINE: return lv_color_hex(UI_COLOR_DANGER);
    case UI_SEVERITY_UNKNOWN:
    default: return lv_color_hex(UI_COLOR_MUTED);
    }
}

lv_color_t ui_theme_severity_dot(ui_severity_t severity)
{
    switch (severity) {
    case UI_SEVERITY_OK: return lv_color_hex(UI_COLOR_OK_DOT);
    case UI_SEVERITY_WARNING: return lv_color_hex(UI_COLOR_WARN_DOT);
    case UI_SEVERITY_OFFLINE: return lv_color_hex(UI_COLOR_DANGER_DOT);
    case UI_SEVERITY_UNKNOWN:
    default: return lv_color_hex(UI_COLOR_SUBTLE);
    }
}

const char *ui_theme_severity_icon(ui_severity_t severity)
{
    switch (severity) {
    case UI_SEVERITY_OK: return LV_SYMBOL_OK;
    case UI_SEVERITY_WARNING: return LV_SYMBOL_WARNING;
    case UI_SEVERITY_OFFLINE: return LV_SYMBOL_CLOSE;
    case UI_SEVERITY_UNKNOWN:
    default: return LV_SYMBOL_MINUS;
    }
}
