// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "ui_rate_slider.h"
#include "ui_theme.h"

typedef struct {
    lv_obj_t *meter;
    lv_obj_t *up;
    lv_obj_t *down;
    void (*step)(int direction, void *ctx);
    void *ctx;
    uint32_t last_step;
    int direction;
    bool held;
    bool wheel;
    unsigned phase;
} rate_slider_t;

// A short drum with transverse ribs. Their spacing and width taper toward both ends.
static void draw_wheel(lv_event_t *event)
{
    rate_slider_t *s = lv_event_get_user_data(event);
    lv_obj_t *target = lv_obj_get_parent(s->meter);
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_area_t body;
    lv_obj_get_coords(s->meter, &body);
    int32_t half = lv_area_get_height(&body) / 2;
    if (half < 8 || lv_area_get_width(&body) < 12) return;
    int32_t mid = body.y1 + half;
    lv_color_t ink = lv_obj_get_style_text_color(target, LV_PART_MAIN);
    lv_color_t surface = lv_obj_get_style_bg_color(target, LV_PART_MAIN);
    lv_draw_rect_dsc_t drum;
    lv_draw_rect_dsc_init(&drum);
    drum.radius = 10;
    drum.bg_color = lv_color_mix(ink, surface, 24);
    drum.border_color = lv_color_mix(ink, surface, 96);
    drum.border_width = 1;
    lv_draw_rect(layer, &drum, &body);

    lv_draw_rect_dsc_t rib;
    lv_draw_rect_dsc_init(&rib);
    rib.radius = 2;
    for (int i = -8; i <= 8; ++i) {
        int t = i * 3 + (int)s->phase; // one third of a rib per detent
        if (t < -23 || t > 23) continue;
        int distance = t < 0 ? -t : t;
        int32_t y = mid + (half - 4) * t * (48 - distance) / (24 * 24);
        int inset = 4 + distance * distance / 100;
        lv_area_t stripe = {body.x1 + inset, y - 1, body.x2 - inset, y + (distance < 16 ? 1 : 0)};
        if (stripe.x2 < stripe.x1) continue;
        rib.bg_color = lv_color_mix(ink, surface, (lv_opa_t)(230 - distance * 4));
        lv_draw_rect(layer, &rib, &stripe);
    }
}

static void center(rate_slider_t *s)
{
    s->direction = 0;
    lv_slider_set_value(s->meter, 0, LV_ANIM_OFF);
}

static void track(lv_event_t *event, rate_slider_t *s, lv_obj_t *target)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    lv_obj_transform_point(target, &point, LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);
    lv_area_t area;
    // Press lock owns this contact until release. Only the logical vertical
    // position controls speed, with both ends saturated outside the track.
    lv_obj_get_coords(s->meter, &area);
    const int32_t travel = area.y2 - area.y1;
    if (travel <= 0) return;
    int32_t value = (area.y1 + area.y2 - 2 * point.y) * 100 / travel;
    value = LV_CLAMP(-100, value, 100);
    const int magnitude = value < 0 ? -value : value;
    if (magnitude <= 12) {
        center(s);
        return;
    }
    lv_slider_set_value(s->meter, value, LV_ANIM_OFF);
    const int direction = value > 0 ? 1 : -1;
    // One immediate detent on contact or reversal. No catch-up burst after a slow UI frame.
    const uint32_t period_ms = 480u - (uint32_t)(magnitude - 12) * 415u / 88u;
    if (direction != s->direction || lv_tick_elaps(s->last_step) >= period_ms) {
        s->last_step = lv_tick_get();
        s->direction = direction;
        if (s->wheel) {
            s->phase = (s->phase + (direction > 0 ? 2u : 1u)) % 3u;
            lv_obj_invalidate(s->meter);
        }
        s->step(direction, s->ctx);
    }
}

static void rate_event(lv_event_t *event)
{
    rate_slider_t *s = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target_obj(event);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_SIZE_CHANGED: {
        bool compact = lv_obj_get_height(target) < 150;
        lv_obj_set_hidden(s->up, compact);
        lv_obj_set_hidden(s->down, compact);
        break;
    }
    case LV_EVENT_PRESSED:
        s->held = !lv_obj_has_state(target, LV_STATE_DISABLED);
        if (s->held) track(event, s, target);
        break;
    case LV_EVENT_PRESSING:
        if (s->held && !lv_obj_has_state(target, LV_STATE_DISABLED)) track(event, s, target);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
    case LV_EVENT_INDEV_RESET:
        s->held = false;
        center(s);
        break;
    case LV_EVENT_DELETE:
        lv_free(s);
        break;
    default:
        break;
    }
}

static lv_obj_t *direction_label(lv_obj_t *target, bool up)
{
    lv_obj_t *arrow = lv_label_create(target);
    if (arrow) {
        lv_label_set_text_static(arrow, up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN);
        ui_theme_apply(arrow, UI_STYLE_TEXT_MUTED, LV_PART_MAIN);
        lv_obj_set_clickable(arrow, false);
        lv_obj_align(arrow, up ? LV_ALIGN_TOP_MID : LV_ALIGN_BOTTOM_MID, 0, up ? 3 : -3);
    }
    return arrow;
}

static lv_obj_t *create(lv_obj_t *parent, void (*step)(int, void *), void *ctx, bool wheel)
{
    if (!parent || !step) return NULL;
    lv_obj_t *target = lv_obj_create(parent);
    if (!target) return NULL;
    lv_obj_remove_style_all(target);
    ui_theme_apply(target, UI_STYLE_CONTROL_BUTTON, LV_PART_MAIN);
    ui_theme_apply(target, UI_STYLE_STATE_DISABLED, LV_STATE_DISABLED);
    lv_obj_set_size(target, 64, 160);
    lv_obj_set_style_radius(target, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(target, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(target, false);
    lv_obj_set_press_lock(target, true);

    lv_obj_t *meter = lv_slider_create(target);
    rate_slider_t *s = lv_calloc(1, sizeof(*s));
    if (!s || !meter) {
        lv_free(s);
        lv_obj_delete(target);
        return NULL;
    }
    lv_obj_set_clickable(meter, false);
    lv_slider_set_range(meter, -100, 100);
    lv_slider_set_mode(meter, LV_SLIDER_MODE_SYMMETRICAL);
    lv_slider_set_orientation(meter, LV_SLIDER_ORIENTATION_VERTICAL);
    if (wheel) {
        lv_obj_remove_style_all(meter);
        if (!lv_obj_add_event_cb(meter, draw_wheel, LV_EVENT_DRAW_MAIN_END, s)) {
            lv_obj_delete(target);
            lv_free(s);
            return NULL;
        }
    } else {
        ui_theme_apply(meter, UI_STYLE_DRAWER_GRIP, LV_PART_MAIN);
        ui_theme_apply(meter, UI_STYLE_CONTROL_SELECTED, LV_PART_INDICATOR);
        ui_theme_apply(meter, UI_STYLE_TOUCH_CONTACT, LV_PART_KNOB);
        lv_obj_set_style_pad_hor(meter, 7, LV_PART_KNOB);
    }
    lv_obj_set_size(meter, wheel ? lv_pct(58) : 12, lv_pct(72));
    lv_obj_center(meter);
    *s = (rate_slider_t){.meter = meter, .step = step, .ctx = ctx, .wheel = wheel};
    s->up = direction_label(target, true);
    s->down = direction_label(target, false);
    if (!s->up || !s->down || !lv_obj_add_event_cb(target, rate_event, LV_EVENT_ALL, s)) {
        lv_free(s);
        lv_obj_delete(target);
        return NULL;
    }
    return target;
}

lv_obj_t *ui_rate_slider_create(lv_obj_t *parent, void (*step)(int, void *), void *ctx)
{
    return create(parent, step, ctx, false);
}

lv_obj_t *ui_rate_wheel_create(lv_obj_t *parent, void (*step)(int, void *), void *ctx)
{
    return create(parent, step, ctx, true);
}

void ui_rate_slider_cancel(lv_obj_t *slider)
{
    if (slider) lv_obj_send_event(slider, LV_EVENT_PRESS_LOST, NULL);
}
