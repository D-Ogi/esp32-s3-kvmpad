// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Shared LVGL styles (TASK-060D, 07 §7). Opaque colors, thin outlines and short edge reflections
// keep the main surfaces inexpensive to paint on the 7B RGB panel.
#include "ui_theme.h"

#include "ui_tokens.h"

static lv_style_t s_style[UI_STYLE_COUNT];
static bool s_ready;
static ui_control_palette_t s_control_palette;

typedef struct {
    uint32_t page, surface, ink, border, selected, selected_ink;
} control_colors_t;

static const control_colors_t control_colors[UI_PALETTE_COUNT] = {
    [UI_PALETTE_DAY] = {UI_COLOR_BG, UI_COLOR_SURFACE, UI_COLOR_INK, 0x7A8896, UI_COLOR_ACCENT_TINT, 0x005497},
    [UI_PALETTE_NIGHT] = {0x111820, 0x202C39, 0xF0F5FA, 0x8493A2, 0x263D50, 0xA8DAFF},
    [UI_PALETTE_HIGH_CONTRAST] = {0x000000, 0x000000, 0xFFFFFF, 0xFFFFFF, 0xFFFF00, 0x000000},
};

// Opaque RGB565 surfaces. Opposite edges of the touchpad pick up the light without a second fill.
typedef struct {
    uint32_t canvas, chrome, track, pad, rim, edge, contact, contact_ink, legend;
} glass_colors_t;

static const glass_colors_t glass_colors[UI_PALETTE_COUNT] = {
    [UI_PALETTE_DAY] = {
        0xE1EAF0, 0xF1F7FA, 0xDCE7ED, 0xD2E6EA, 0xF4FEFF, 0x819EAB, 0xEDF8F9, 0x21586C, 0x405A68,
    },
    [UI_PALETTE_NIGHT] = {
        0x101B24, 0x1D2C3A, 0x132431, 0x193849, 0x738C9D, 0x355367, 0x27495C, 0xB4E5EA, 0xC5D8DF,
    },
    [UI_PALETTE_HIGH_CONTRAST] = {
        0x000000, 0x000000, 0x000000, 0x000000, UI_COLOR_EDGE, UI_COLOR_EDGE, 0x000000, UI_COLOR_INK, UI_COLOR_MUTED,
    },
};

// The product shell also paints some token colors locally (pad, key legends and status dots).
// LVGL applies this inherited filter to draw styles, once per primitive. Media and pack
// scenes have their own screens and retain their original pixels and authored colors.
static lv_color_t shell_color(const lv_color_filter_dsc_t *dsc, lv_color_t original, lv_opa_t opa)
{
    (void)dsc;
    (void)opa;
    const control_colors_t *p = &control_colors[s_control_palette];
    bool high = s_control_palette == UI_PALETTE_HIGH_CONTRAST;
    uint32_t result;
    switch (lv_color_to_u32(original) & 0xffffff) {
    case UI_COLOR_BG: result = p->page; break;
    case UI_COLOR_SURFACE: result = p->surface; break;
    case UI_COLOR_WELL: case UI_COLOR_SPECIAL_SURFACE: case UI_COLOR_DISABLED_SURFACE:
        result = high ? 0x121212 : 0x283440; break;
    case UI_COLOR_INK: result = p->ink; break;
    case UI_COLOR_MUTED: case UI_COLOR_SUBTLE: case UI_COLOR_DISABLED_INK:
        result = high ? 0xe2e2e2 : 0xc1cedb; break;
    case UI_COLOR_RULE: case UI_COLOR_EDGE: case UI_COLOR_SHADE: result = p->border; break;
    case UI_COLOR_INVERSE: case UI_COLOR_CHIP_FEATURED: case UI_COLOR_INVERSE_PRESSED:
        result = high ? 0xffffff : 0xc8e5ff; break;
    case UI_COLOR_INVERSE_SUBTLE: result = high ? 0x191919 : 0x273645; break;
    case UI_COLOR_ACCENT: case UI_COLOR_ACCENT_INK: case UI_COLOR_ACTIVE_INK:
        result = high ? 0x00ffff : 0xa8daff; break;
    case UI_COLOR_ACCENT_TINT: case UI_COLOR_CHECKED_SURFACE: case UI_COLOR_PRESSED_SURFACE:
    case UI_COLOR_PRESSED_QUIET: result = high ? 0x1a3333 : 0x263d50; break;
    case UI_COLOR_ACCENT_EDGE: case UI_COLOR_CHECKED_EDGE: result = high ? 0x00ffff : 0x8493a2; break;
    case UI_COLOR_OK: case UI_COLOR_OK_DOT: result = high ? 0x74ff90 : 0x91eab8; break;
    case UI_COLOR_WARN: case UI_COLOR_WARN_DOT: result = high ? 0xffff00 : 0xffda8c; break;
    case UI_COLOR_DANGER: case UI_COLOR_DANGER_DOT: result = high ? 0xff98a9 : 0xffa4aa; break;
    default: return original;
    }
    return lv_color_hex(result);
}

static const lv_color_filter_dsc_t shell_filter = {.filter_cb = shell_color};

static void set_control_colors(void)
{
    const control_colors_t *p = &control_colors[s_control_palette];
    const glass_colors_t *g = &glass_colors[s_control_palette];
    const bool high = s_control_palette == UI_PALETTE_HIGH_CONTRAST;
    lv_style_t *page = &s_style[UI_STYLE_CONTROL_PAGE], *button = &s_style[UI_STYLE_CONTROL_BUTTON];
    lv_style_t *selected = &s_style[UI_STYLE_CONTROL_SELECTED];
    lv_style_set_color_filter_dsc(&s_style[UI_STYLE_SCREEN], &shell_filter);
    lv_style_set_color_filter_opa(&s_style[UI_STYLE_SCREEN], s_control_palette == UI_PALETTE_DAY ? LV_OPA_TRANSP : LV_OPA_COVER);
    // These controls already use the selected colors directly.
    lv_style_set_color_filter_opa(button, LV_OPA_TRANSP);
    lv_style_set_bg_color(page, lv_color_hex(p->page));
    lv_style_set_bg_opa(page, LV_OPA_COVER);
    lv_style_set_text_color(page, lv_color_hex(p->ink));
    lv_style_set_bg_color(button, lv_color_hex(p->surface));
    lv_style_set_bg_opa(button, LV_OPA_COVER);
    lv_style_set_text_color(button, lv_color_hex(p->ink));
    lv_style_set_border_width(button, s_control_palette == UI_PALETTE_HIGH_CONTRAST ? 2 : 1);
    lv_style_set_border_color(button, lv_color_hex(p->border));
    lv_style_set_bg_color(selected, lv_color_hex(p->selected));
    lv_style_set_text_color(selected, lv_color_hex(p->selected_ink));
    lv_style_set_border_color(selected, lv_color_hex(p->selected_ink));
    lv_style_set_image_recolor(&s_style[UI_STYLE_CONTROL_MASK], lv_color_hex(p->ink));
    lv_style_set_image_recolor_opa(&s_style[UI_STYLE_CONTROL_MASK], LV_OPA_COVER);
    lv_style_set_image_recolor(&s_style[UI_STYLE_CONTROL_MASK_SELECTED], lv_color_hex(p->selected_ink));

    lv_style_t *tile = &s_style[UI_STYLE_LIBRARY_TILE];
    lv_style_set_bg_color(tile, lv_color_hex(p->surface));
    lv_style_set_bg_grad_color(tile, lv_color_hex(g->chrome));
    lv_style_set_bg_grad_dir(tile, high ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER);
    lv_style_set_radius(tile, UI_RADIUS_XL);
    lv_style_set_border_color(tile, lv_color_hex(p->border));
    lv_style_set_shadow_width(tile, 0);
    lv_style_set_pad_all(tile, UI_SPACE_L);

    lv_style_t *badge = &s_style[UI_STYLE_LIBRARY_ICON];
    lv_style_set_radius(badge, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(badge, LV_OPA_COVER);
    lv_style_set_bg_color(badge, lv_color_hex(high ? p->ink : g->pad));
    lv_style_set_image_recolor(badge, lv_color_hex(high ? p->surface : g->contact_ink));

    lv_style_set_bg_color(&s_style[UI_STYLE_SCREEN], lv_color_hex(g->canvas));
    lv_style_set_bg_color(&s_style[UI_STYLE_TOPBAR], lv_color_hex(g->chrome));
    lv_style_set_border_color(&s_style[UI_STYLE_TOPBAR], lv_color_hex(g->edge));
    lv_style_t *track = &s_style[UI_STYLE_SEG_TRACK];
    lv_style_set_bg_color(track, lv_color_hex(g->track));
    lv_style_set_radius(track, LV_RADIUS_CIRCLE);

    // Unselected navigation tabs share one track. A held or selected tab has an opaque fill.
    lv_style_t *tab = &s_style[UI_STYLE_NAV_TAB];
    lv_style_set_radius(tab, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(tab, high ? 2 : 0);
    lv_style_set_bg_opa(tab, high ? LV_OPA_COVER : LV_OPA_TRANSP);
    lv_style_set_radius(&s_style[UI_STYLE_SEG_TAB_CHECKED], LV_RADIUS_CIRCLE);
    lv_style_set_border_width(selected, high ? 2 : 1);
    lv_style_set_border_color(selected, lv_color_hex(high ? p->selected_ink : g->edge));
    lv_style_set_bg_opa(selected, LV_OPA_COVER);

    lv_style_t *pad = &s_style[UI_STYLE_TOUCH_SURFACE];
    lv_style_set_bg_color(pad, lv_color_hex(g->pad));
    lv_style_set_radius(pad, 32);
    lv_style_set_border_color(pad, lv_color_hex(g->rim));
    lv_style_set_border_side(pad, high ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT);
    lv_style_set_outline_width(pad, high ? 0 : 1);
    lv_style_set_outline_pad(pad, 0);
    lv_style_set_outline_color(pad, lv_color_hex(g->edge));

    lv_style_t *contact = &s_style[UI_STYLE_TOUCH_CONTACT];
    lv_style_set_bg_opa(contact, LV_OPA_COVER);
    lv_style_set_bg_color(contact, lv_color_hex(g->contact));
    lv_style_set_border_width(contact, high ? 2 : 1);
    lv_style_set_border_color(contact, lv_color_hex(g->rim));
    lv_style_set_radius(contact, LV_RADIUS_CIRCLE);
    lv_style_set_outline_color(contact, lv_color_hex(g->edge));
    lv_style_set_outline_width(contact, high ? 0 : 1);
    lv_style_set_outline_pad(contact, 0);
    lv_style_set_image_recolor(contact, lv_color_hex(g->contact_ink));
    lv_style_set_image_recolor_opa(contact, LV_OPA_COVER);

    lv_style_t *status = &s_style[UI_STYLE_TOUCH_STATUS];
    lv_style_set_bg_color(status, lv_color_hex(g->contact));
    lv_style_set_bg_opa(status, LV_OPA_COVER);
    lv_style_set_radius(status, LV_RADIUS_CIRCLE);
    lv_style_set_border_width(status, 1);
    lv_style_set_border_color(status, lv_color_hex(g->edge));
    lv_style_set_text_color(status, lv_color_hex(g->contact_ink));
    lv_style_set_pad_hor(status, UI_SPACE_M);
    lv_style_set_pad_ver(status, UI_SPACE_XS);

    lv_style_t *key = &s_style[UI_STYLE_MOUSE_KEY];
    lv_style_set_radius(key, UI_RADIUS_XL);
    lv_style_set_border_color(key, lv_color_hex(g->edge));
    lv_style_set_text_color(&s_style[UI_STYLE_TEXT_MUTED], lv_color_hex(g->legend));

    lv_style_t *drawer = &s_style[UI_STYLE_DRAWER_SHELL];
    lv_style_set_bg_opa(drawer, LV_OPA_COVER);
    lv_style_set_bg_color(drawer, lv_color_hex(g->track));
    lv_style_set_border_width(drawer, high ? 2 : 1);
    lv_style_set_border_color(drawer, lv_color_hex(g->edge));
    lv_style_set_radius(drawer, UI_RADIUS_L);
    lv_style_set_pad_all(drawer, UI_SPACE_XS / 2);

    lv_style_t *shortcut = &s_style[UI_STYLE_DRAWER_TILE];
    lv_style_set_bg_color(shortcut, lv_color_hex(p->surface));
    lv_style_set_bg_grad_color(shortcut, lv_color_hex(g->chrome));
    lv_style_set_bg_grad_dir(shortcut, high ? LV_GRAD_DIR_NONE : LV_GRAD_DIR_VER);
    lv_style_set_radius(shortcut, UI_RADIUS_M);
    lv_style_set_border_width(shortcut, high ? 2 : 1);
    lv_style_set_border_color(shortcut, lv_color_hex(high ? UI_COLOR_EDGE : g->rim));
    lv_style_set_pad_all(shortcut, 0);

    lv_style_t *link = &s_style[UI_STYLE_DRAWER_STATUS];
    lv_style_set_bg_color(link, lv_color_hex(g->track));
    lv_style_set_bg_grad_dir(link, LV_GRAD_DIR_NONE);
    lv_style_set_border_width(link, high ? 2 : 0);

    lv_style_t *icon = &s_style[UI_STYLE_DRAWER_MASK];
    lv_style_set_image_recolor(icon, lv_color_hex(high ? p->ink : g->contact_ink));
    lv_style_set_color_filter_opa(icon, LV_OPA_TRANSP);

    lv_style_t *grip = &s_style[UI_STYLE_DRAWER_GRIP];
    lv_style_set_bg_color(grip, lv_color_hex(high ? UI_COLOR_INVERSE : g->contact_ink));
    lv_style_set_text_color(grip, lv_color_hex(high ? UI_COLOR_ON_INVERSE : g->contact));
    lv_style_set_radius(grip, UI_RADIUS_M);
    lv_style_set_pad_all(grip, 0);
}

ui_control_palette_t ui_theme_control_palette(void)
{
    return s_control_palette;
}

void ui_theme_set_control_palette(ui_control_palette_t palette)
{
    if ((unsigned)palette >= UI_PALETTE_COUNT || palette == s_control_palette) return;
    s_control_palette = palette;
    if (s_ready) {
        set_control_colors();
        lv_obj_report_style_change(NULL);
    }
}

void ui_theme_control_button(lv_obj_t *button)
{
    ui_theme_apply(button, UI_STYLE_CONTROL_BUTTON, LV_STATE_DEFAULT);
    ui_theme_apply(button, UI_STYLE_CONTROL_SELECTED, LV_STATE_CHECKED);
    ui_theme_apply(button, UI_STYLE_CONTROL_SELECTED, LV_STATE_PRESSED);
}

// A short inset highlight follows each rounded corner. Its three primitives touch only the rim.
static void edge_reflection(lv_layer_t *layer, lv_point_t center, int32_t direction, lv_color_t color)
{
    const uint16_t radius = 26;
    const int32_t span = direction > 0 ? 208 : 132;
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center = center;
    arc.radius = radius;
    arc.start_angle = direction > 0 ? 180 : 0;
    arc.end_angle = direction > 0 ? 270 : 90;
    arc.width = 2;
    arc.color = color;
    lv_draw_arc(layer, &arc);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = color;
    line.width = 2;
    line.round_end = 1;
    line.p1 = (lv_point_precise_t){center.x, center.y - direction * (radius - 1)};
    line.p2 = (lv_point_precise_t){center.x + direction * span, line.p1.y};
    lv_draw_line(layer, &line);
    line.p1 = (lv_point_precise_t){center.x - direction * (radius - 1), center.y};
    line.p2 = (lv_point_precise_t){line.p1.x, center.y + direction * UI_SPACE_XL};
    lv_draw_line(layer, &line);
}

static void touch_surface_draw(lv_event_t *e)
{
    if (s_control_palette == UI_PALETTE_HIGH_CONTRAST) return;
    lv_area_t area;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &area);
    lv_layer_t *layer = lv_event_get_layer(e);
    const glass_colors_t *g = &glass_colors[s_control_palette];
    const int32_t inset = 31;
    edge_reflection(layer, (lv_point_t){area.x1 + inset, area.y1 + inset}, 1, lv_color_hex(g->rim));
    edge_reflection(layer, (lv_point_t){area.x2 - inset, area.y2 - inset}, -1, lv_color_hex(g->edge));
}

void ui_theme_touch_surface(lv_obj_t *pad)
{
    ui_theme_apply(pad, UI_STYLE_TOUCH_SURFACE, LV_STATE_DEFAULT);
    lv_obj_add_event_cb(pad, touch_surface_draw, LV_EVENT_DRAW_MAIN_END, NULL);
}

static void sync_mask(lv_event_t *event)
{
    lv_obj_t *button = lv_event_get_current_target_obj(event);
    bool selected = (lv_obj_get_state(button) & (LV_STATE_CHECKED | LV_STATE_PRESSED)) != 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(button); ++i) {
        lv_obj_t *child = lv_obj_get_child(button, (int32_t)i);
        if (lv_obj_check_type(child, &lv_image_class)) lv_obj_set_checked(child, selected);
    }
}

lv_obj_t *ui_theme_control_mask(lv_obj_t *button, const lv_image_dsc_t *mask)
{
    lv_obj_t *icon = lv_image_create(button);
    lv_image_set_src(icon, mask);
    lv_obj_set_clickable(icon, false);
    ui_theme_apply(icon, UI_STYLE_CONTROL_MASK, LV_STATE_DEFAULT);
    ui_theme_apply(icon, UI_STYLE_CONTROL_MASK_SELECTED, LV_STATE_CHECKED);
    lv_obj_set_checked(icon, (lv_obj_get_state(button) & (LV_STATE_CHECKED | LV_STATE_PRESSED)) != 0);
    lv_obj_add_event_cb(button, sync_mask, LV_EVENT_STATE_CHANGED, NULL);
    return icon;
}

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
    set_control_colors();
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
// tools/fonts/gen_hangul_font.py). Media builds can publish the same subset from ime_data.
#if defined(KVM_HAVE_FONT_HANGUL_24)
LV_FONT_DECLARE(kvm_font_hangul_24)
static const lv_font_t *s_hangul = &kvm_font_hangul_24;
#else
static const lv_font_t *s_hangul;
#endif

bool ui_theme_has_hangul_font(void)
{
    return s_hangul != NULL;
}

void ui_theme_set_hangul_font(const lv_font_t *font)
{
    s_hangul = font;
}

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
    case UI_FONT_HANGUL: return s_hangul ? s_hangul : &lv_font_montserrat_24;
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
