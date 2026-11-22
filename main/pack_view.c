// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "pack_view.h"
#include "pack_chart_view.h"
#include "pack_autopilot_view.h"
#include "pack_radio_view.h"
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "pack_desktop_view.h"
#include "pack_keyboard_surface.h"
#endif
#include "pack_panel.h"
#include "kvmbridge.h"
#include "hid_dispatch.h"
#include "ui_rate_slider.h"
#include "ui_segmented.h"
#include "hpe_reports.h"
#include "esp_heap_caps.h"
#include "ui_theme.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The parent screen owns LVGL objects. This table only owns their runtime references.
typedef struct {
    lv_obj_t *obj, *label, *gauge, *needle;
    lv_point_precise_t line[2];
    double value[2];
    int16_t origin_x, origin_y, last_x, last_y, selected;
    bool tracking, dragged, held;
    bool valid[2], seen[2];
    kvmbridge_quality_t quality[2];
} control_t;
static control_t *controls;
static lv_image_dsc_t *icon_images; // descriptors follow controls in the same allocation
static void *chart_storage;
static pack_scene_t *scene;
static lv_obj_t *viewport;
static lv_obj_t *screen_picker, *screen_handle;
static lv_point_t handle_start;
static bool handle_down, handle_moved;
static unsigned permissions, actions, events;
static uint32_t window_start, sample_tick;
#if KVM_HID_HAS_HPE
static uint32_t release_generation;
static int hat_owner=-1;
#endif
static bool xplane, subscribed, attempted, foreground, gate_input;
static hpe_joystick_t joystick;

static bool visible(void)
{
    return viewport && lv_screen_active() == lv_obj_get_screen(viewport);
}

void pack_view_logical_point(bool portrait,const lv_point_t *physical,lv_point_t *logical)
{
    *logical=portrait ? (lv_point_t){599-physical->y,physical->x} : *physical;
}

static void control_point(lv_obj_t *obj,lv_indev_t *indev,lv_point_t *p)
{
    lv_indev_get_point(indev,p);
    if (scene->portrait)
        lv_obj_transform_point(obj,p,LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE | LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
}

static void reset_joystick(void)
{
    joystick = (hpe_joystick_t){.hat=8};
#if KVM_HID_HAS_HPE
    hat_owner=-1;
#endif
    gate_input=true;
}

static bool within_budget(void)
{
    if (lv_tick_elaps(window_start)>=1000) {
        window_start=lv_tick_get();
        actions=events=0;
    }
    if (++events>scene->events_per_second || actions>=32) return false;
    actions++;
    return true;
}

static void render_instrument(unsigned idx);
static void show_tabs(unsigned idx);

static void update_binding(pack_binding_t *b, const pack_value_t *input, kvmbridge_quality_t quality, uint32_t now)
{
    control_t *c=&controls[b->widget];
    pack_value_t value;
    bool valid=pack_binding_apply(scene,b,input,now,&value);
    if (b->field==PACK_BIND_SELECTED) {
        if (scene->widget[b->widget].kind==PACK_RADIO) {
            if (valid && value.integer>=0 && value.integer<2) pack_radio_view_select((unsigned)value.integer);
            return;
        }
        if (valid && value.integer>=0 && value.integer<scene->widget[b->widget].tab_count &&
            c->selected!=value.integer) {
            c->selected=(int16_t)value.integer;
            show_tabs(b->widget);
        }
        return;
    }
    char scratch[32];
    if (b->field==PACK_BIND_TEXT) {
        const char *text=valid ? pack_value_text(b->output,&value,scratch) : quality==KVB_ERROR ? "Data error" :
                         quality==KVB_STALE ? "Stale" : "Unavailable";
        if (strcmp(lv_label_get_text(c->label),text)) lv_label_set_text(c->label,text);
        return;
    }
    unsigned axis=b->field==PACK_BIND_ROLL ? 1 : 0;
    double number=valid ? (b->output==PACK_INTEGER ? (double)value.integer : value.real) : 0;
    bool changed=!c->seen[axis] || c->valid[axis]!=valid || c->quality[axis]!=quality || c->value[axis]!=number;
    c->seen[axis]=true; c->valid[axis]=valid; c->quality[axis]=quality; c->value[axis]=number;
    if (valid && b->local && scene->widget[b->widget].kind==PACK_VALUE) {
        const pack_widget_t *w=&scene->widget[b->widget];
        char text[160];
        snprintf(text,sizeof(text),"%s\n%s %s",w->text,pack_value_text(b->output,&value,scratch),w->unit);
        if (strcmp(lv_label_get_text(c->label),text)) lv_label_set_text(c->label,text);
    } else if (changed) render_instrument(b->widget);
}

static void local_bindings(int source)
{
    uint32_t now=lv_tick_get();
    for (unsigned i=0; i<scene->bindings; i++) {
        pack_binding_t *b=&scene->binding[i];
        if (b->local && (source<0 || source==b->source)) update_binding(b,&scene->state[b->source].value,KVB_VALID,now);
    }
}

#if KVM_HID_HAS_HPE
static int normalized(int point, int low, int high, bool sign)
{
    if (high<=low) return 0;
    if (point<low) point=low;
    if (point>high) point=high;
    if (sign && high-low>1 && abs(2*(point-low)-(high-low))<=1) return 0;
    int range=sign ? 65534 : 65535;
    return ((point-low)*range + (high-low)/2)/(high-low) - (sign ? 32767 : 0);
}

static int axis_response(int raw, const pack_widget_t *w)
{
    int scale=100-w->dead_zone_percent;
    int usable=abs(raw)*100-32767*w->dead_zone_percent;
    if (usable<=0) return 0;
    int mag=(usable+scale/2)/scale;
    if (w->cubic) {
        const int32_t square=32767*32767;
        mag=(int)(((int64_t)mag*mag*mag+square/2)/square);
    }
    return raw<0 ? -mag : mag;
}

static void joystick_input(unsigned idx, lv_event_code_t ev)
{
    if (!(permissions & PACK_PERM_JOYSTICK)) return;
    kvm_hid_snapshot_t snap;
    hid_dispatch_get_snapshot(&snap,NULL);
    if (snap.release_generation!=release_generation) {
        reset_joystick();
        release_generation=snap.release_generation;
    }
    if (snap.host_usb!=KVM_HOST_USB_MOUNTED || snap.neutralizing || snap.maintenance) {reset_joystick();return;}
    if (ev==LV_EVENT_PRESSED) gate_input=false;
    if (gate_input) return;

    const pack_widget_t *w=&scene->widget[idx];
    bool end=ev==LV_EVENT_RELEASED || ev==LV_EVENT_PRESS_LOST;
    bool motion=w->kind==PACK_AXIS || w->kind==PACK_SLIDER;
    if (motion && ev!=LV_EVENT_PRESSING && ev!=LV_EVENT_PRESSED && !end) return;
    if (!motion && ev!=LV_EVENT_PRESSED && !end) return;
    const pack_action_t *a=pack_scene_action(scene,idx,motion ? PACK_CHANGE : end ? PACK_RELEASE : PACK_PRESS);
    if (!a) return;
    hpe_joystick_t next=joystick;
    if (motion) {
        if (end && !w->spring_center) return;
        lv_area_t area; lv_point_t p={0};
        lv_obj_get_coords(controls[idx].obj,&area);
        if (!end) {
            lv_indev_t *indev=lv_indev_active();
            if (!indev) return;
            control_point(controls[idx].obj,indev,&p);
        }
        int x=end ? 0 : normalized(p.x,area.x1,area.x2,a->mask!=PACK_Z);
        if (a->mask & PACK_X) next.x=(int16_t)axis_response(x,w);
        if (a->mask & PACK_Y) {
            int y=end ? 0 : axis_response(normalized(p.y,area.y1,area.y2,true),w);
            next.y=(int16_t)(w->invert_y ? -y : y);
        }
        if (a->mask & PACK_RZ) next.rz=(int16_t)x;
        if (a->mask & PACK_Z) next.z=(uint16_t)x;
        if (w->kind==PACK_SLIDER) lv_bar_set_value(controls[idx].gauge,x,LV_ANIM_OFF);
        if (w->kind==PACK_AXIS) {
            lv_obj_align(controls[idx].gauge,LV_ALIGN_CENTER,(w->w/2-28)*next.x/32767,(w->h/2-28)*next.y/32767);
        }
    } else if (a->kind==PACK_HAT) {
        if ((!end && hat_owner>=0) || (end && hat_owner!=(int)idx)) return;
        next.hat=(uint8_t)a->value.integer;
    } else if (a->down) next.buttons|=a->mask;
    else next.buttons&=(uint16_t)~a->mask;
    if (!memcmp(&next,&joystick,sizeof(next))) return;
    if (!within_budget()) {
        if (end) {hid_dispatch_release_all(KVM_RA_QUEUE_OVERFLOW);reset_joystick();}
        return;
    }
    kvm_hid_intent_t intent={.type=KVM_HID_JOYSTICK,.data.joystick=next};
    if (hid_dispatch_local(&intent)==ESP_OK) {
        joystick=next;
        if (a->kind==PACK_HAT) hat_owner=end ? -1 : (int)idx;
    }
    else {hid_dispatch_release_all(KVM_RA_QUEUE_OVERFLOW);reset_joystick();}
}
#endif

static void local_event(unsigned idx,pack_event_t event,int value,int x,int y)
{
    const pack_action_t *a=pack_scene_action(scene,idx,event);
    if (a && within_budget() && pack_scene_set(scene,a,value,x,y)) local_bindings(a->state);
}

static void rate_step(int direction, void *ctx)
{
    unsigned idx=(unsigned)(uintptr_t)ctx;
    if (direction && scene && visible() && idx<scene->widgets && lv_obj_is_visible(controls[idx].obj))
        local_event(idx,direction>0 ? PACK_STEP_UP : PACK_STEP_DOWN,0,0,0);
}

static void cancel_rate_sliders(int page)
{
    for (unsigned i=0;i<scene->widgets;i++) {
        if (scene->widget[i].kind!=PACK_RATE_SLIDER || !controls[i].obj) continue;
        int ancestor=scene->widget[i].parent;
        while (ancestor>=0 && ancestor!=page) ancestor=scene->widget[ancestor].parent;
        if (ancestor==page) ui_rate_slider_cancel(controls[i].obj);
    }
}

static void touch_event(lv_event_t *e)
{
    unsigned idx=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (!scene || !visible() || idx>=scene->widgets) return;
    control_t *c=&controls[idx];
    if (lv_event_get_target_obj(e)!=c->obj) return;
    lv_event_code_t ev=lv_event_get_code(e);
    if (ev==LV_EVENT_PRESS_LOST || !lv_obj_is_visible(c->obj)) { c->tracking=false; return; }
    if (ev==LV_EVENT_SHORT_CLICKED) {
        if (c->tracking && !c->dragged && !c->held) local_event(idx,PACK_TAP,0,0,0);
        c->tracking=false;
    } else if (ev==LV_EVENT_LONG_PRESSED && c->tracking && !c->dragged && !c->held) {
        c->held=true;
        local_event(idx,PACK_LONG_PRESS,0,0,0);
    } else if (ev==LV_EVENT_PRESSED || (ev==LV_EVENT_PRESSING && c->tracking && !c->held)) {
        lv_indev_t *indev=lv_indev_active();
        if (!indev) return;
        lv_point_t point;
        lv_area_t area;
        control_point(c->obj,indev,&point);
        lv_obj_get_coords(c->obj,&area);
        int x=LV_CLAMP(area.x1,point.x,area.x2)-area.x1;
        int y=LV_CLAMP(area.y1,point.y,area.y2)-area.y1;
        if (ev==LV_EVENT_PRESSED) {
            c->tracking=true; c->dragged=c->held=false;
            c->origin_x=c->last_x=x; c->origin_y=c->last_y=y;
        } else if ((x!=c->last_x || y!=c->last_y) &&
                   (c->dragged || LV_ABS(x-c->origin_x)>=8 || LV_ABS(y-c->origin_y)>=8)) {
            c->dragged=true; c->last_x=x; c->last_y=y;
            local_event(idx,PACK_DRAG,0,x,y);
        }
    }
}

// LVGL's arc reads the native pointer directly even when an ancestor is rotated.
static void portrait_dial_point(lv_event_t *e)
{
    lv_indev_t *indev=lv_indev_active();
    if (!indev || lv_indev_get_type(indev)!=LV_INDEV_TYPE_POINTER) return;
    lv_event_stop_processing(e);
    lv_obj_t *arc=lv_event_get_current_target_obj(e);
    lv_point_t point;
    control_point(arc,indev,&point);
    lv_area_t area;
    lv_obj_get_content_coords(arc,&area);
    int radius=LV_MIN(lv_area_get_width(&area),lv_area_get_height(&area))/2;
    int x=point.x-area.x1-radius, y=point.y-area.y1-radius;
    if (!radius || x*x+y*y < radius*radius/4 || (!x && !y)) return;
    int angle=(lv_atan2(y,x)+360-135)%360;
    if (angle>270) return;
    int value=lv_map(angle,0,270,lv_arc_get_min_value(arc),lv_arc_get_max_value(arc));
    if (value!=lv_arc_get_value(arc)) {
        lv_arc_set_value(arc,value);
        lv_obj_send_event(arc,LV_EVENT_VALUE_CHANGED,NULL);
    }
}

static void dial_event(lv_event_t *e)
{
    unsigned idx=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (!scene || !visible() || idx>=scene->widgets || !lv_obj_is_visible(controls[idx].obj)) return;
    control_t *c=&controls[idx];
    if (lv_event_get_target_obj(e)!=c->gauge) return;
    int v=lv_arc_get_value(c->gauge);
    if (c->selected==v) return;
    c->selected=v;
    lv_label_set_text_fmt(c->label,"%s %d",scene->widget[idx].text,v);
    local_event(idx,PACK_ROTATE,v,0,0);
}

static void show_tabs(unsigned idx)
{
    const pack_widget_t *w=&scene->widget[idx];
    control_t *c=&controls[idx];
    ui_segmented_set_active(c->obj,c->selected);
    for (unsigned tab=0; tab<w->tab_count;tab++) {
        bool selected=(int)tab==c->selected;
        int page=scene->tab[w->tab_first+tab].page;
        if (page>=0 && controls[page].obj && lv_obj_has_flag(controls[page].obj,LV_OBJ_FLAG_HIDDEN)==selected) {
            if (!selected) cancel_rate_sliders(page);
            lv_obj_set_hidden(controls[page].obj,!selected);
        }
    }
}

static void tab_selected(uint16_t idx,uint16_t tab,void *ctx)
{
    (void)ctx;
    if (!scene || !visible() || idx>=scene->widgets || !lv_obj_is_visible(controls[idx].obj) ||
        tab>=scene->widget[idx].tab_count || controls[idx].selected==(int)tab) return;
    controls[idx].selected=tab;
    show_tabs(idx);
    local_event(idx,PACK_CHANGE,tab,0,0);
}

typedef struct {
    ui_segmented_desc_t desc;
    ui_segment_t tabs[];
} pack_tab_strip_t;

static void free_tab_strip(lv_event_t *event)
{
    lv_free(lv_event_get_user_data(event));
}

static lv_obj_t *create_tab_strip(lv_obj_t *parent,const pack_widget_t *w,unsigned idx)
{
    static ui_segmented_sink_t sink={.fn=tab_selected};
    pack_tab_strip_t *strip=lv_calloc(1,sizeof(*strip)+w->tab_count*sizeof(*strip->tabs));
    if (!strip) return NULL;
    for (unsigned j=0;j<w->tab_count;j++) {
        strip->tabs[j].label=scene->tab[w->tab_first+j].label;
        strip->tabs[j].control_id=(uint16_t)idx;
    }
    strip->desc=(ui_segmented_desc_t){.segments=strip->tabs,.count=w->tab_count,.width=w->w,.height=w->h,
                                     .control_palette=true};
    lv_obj_t *obj=ui_segmented_create(parent,&strip->desc,&sink);
    if (!obj || !lv_obj_add_event_cb(obj,free_tab_strip,LV_EVENT_DELETE,strip)) {
        if (obj) lv_obj_delete(obj);
        lv_free(strip);
        return NULL;
    }
    lv_obj_set_pos(obj,w->x,w->y);
    return obj;
}

static void widget_event(lv_event_t *e)
{
    if (!scene || !visible()) return;
    unsigned idx=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (idx>=scene->widgets || lv_event_get_target_obj(e)!=controls[idx].obj) return;
    lv_event_code_t ev=lv_event_get_code(e);
#if KVM_HID_HAS_HPE
    if (ev!=LV_EVENT_CLICKED) {
        if (ev==LV_EVENT_RELEASED || ev==LV_EVENT_PRESS_LOST || lv_obj_is_visible(controls[idx].obj))
            joystick_input(idx,ev);
        return;
    }
#endif
    if (ev!=LV_EVENT_CLICKED || !lv_obj_is_visible(controls[idx].obj)) return;
    const pack_action_t *a=pack_scene_action(scene,idx,PACK_TAP);
    if (!a) return;
    if (a->kind==PACK_STOP_STREAM) {if (permissions & PACK_PERM_CAPTURE) kvmbridge_stop_display(scene->id,0);return;}
    if (!within_budget()) return;
    if (a->kind==PACK_START_STREAM && (permissions & PACK_PERM_CAPTURE)) {
        const char *mode=pack_scene_capture_mode(scene,a);
        if (mode) pack_panel_magnify(mode);
        else {
            lv_obj_t *label=controls[idx].label;
            for (unsigned i=0;i<scene->widgets;i++) {
                if (scene->widget[i].kind==PACK_STREAM && lv_obj_is_visible(controls[i].obj)) {
                    label=controls[i].label;
                    break;
                }
            }
            lv_label_set_text_static(label,"Unknown capture mode.\nSelect Overview, 1:1 or 2x.");
        }
    }
    else if (a->kind==PACK_STATE && pack_scene_tap(scene,idx)) local_bindings(a->state);
}

static void plain(lv_obj_t *o)
{
    lv_obj_remove_style_all(o);
    lv_obj_set_scrollable(o,false);
}

#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
static void portrait_screen_event(lv_event_t *event)
{
    lv_display_t *display=lv_obj_get_display(lv_event_get_target_obj(event));
    lv_display_rotation_t rotation=lv_event_get_code(event)==LV_EVENT_SCREEN_LOAD_START
        ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_0;
    if (lv_display_get_rotation(display)!=rotation) lv_display_set_rotation(display,rotation);
}
#endif

lv_obj_t *pack_view_frame(lv_obj_t *screen,bool portrait)
{
    if (!portrait) return screen;
    lv_obj_t *frame=lv_obj_create(screen);
    if (!frame) return NULL;
    plain(frame);
    lv_obj_set_size(frame,600,1024);
#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
    if (!lv_obj_add_event_cb(screen,portrait_screen_event,LV_EVENT_SCREEN_LOAD_START,NULL) ||
        !lv_obj_add_event_cb(screen,portrait_screen_event,LV_EVENT_SCREEN_UNLOAD_START,NULL)) {
        lv_obj_delete(frame);
        return NULL;
    }
#else
    lv_obj_set_pos(frame,0,599);
    lv_obj_set_style_transform_pivot_x(frame,0,0);
    lv_obj_set_style_transform_pivot_y(frame,0,0);
    lv_obj_set_style_transform_rotation(frame,2700,0);
#endif
    return frame;
}

static void instrument(control_t *c, const pack_widget_t *w)
{
    // These native instrument faces have no author color properties. Keep the palette
    // scoped to the face so neighboring pack controls and their status colors are independent.
    ui_theme_apply(c->obj,UI_STYLE_SCREEN,LV_PART_MAIN);
    ui_theme_apply(c->obj,UI_STYLE_CARD,LV_PART_MAIN);
    ui_theme_apply(c->label,UI_STYLE_TEXT_HEADING,LV_PART_MAIN);
    lv_color_t accent=lv_color_hex(UI_COLOR_ACCENT_INK);
    lv_color_t ink=lv_color_hex(UI_COLOR_INK);
    int size=w->w<w->h ? w->w-40 : w->h-70;
    if (size<16) size=16;
    if (w->kind==PACK_GAUGE_ARC) {
        c->gauge=lv_arc_create(c->obj);
        lv_obj_set_style_arc_color(c->gauge,lv_color_hex(UI_COLOR_MUTED),LV_PART_MAIN);
        lv_obj_set_style_arc_width(c->gauge,6,LV_PART_MAIN);
        lv_obj_set_style_arc_color(c->gauge,accent,LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c->gauge,accent,LV_PART_KNOB);
        lv_arc_set_range(c->gauge,0,1000);
        lv_arc_set_value(c->gauge,0);
    } else if (w->kind==PACK_GAUGE_NEEDLE) {
        c->gauge=lv_scale_create(c->obj);
        lv_scale_set_mode(c->gauge,LV_SCALE_MODE_ROUND_INNER);
        lv_scale_set_range(c->gauge,w->min,w->max);
        lv_obj_set_style_arc_color(c->gauge,ink,LV_PART_MAIN);
        lv_obj_set_style_line_color(c->gauge,ink,LV_PART_ITEMS);
        lv_obj_set_style_line_color(c->gauge,ink,LV_PART_INDICATOR);
        lv_obj_set_style_text_color(c->gauge,ink,LV_PART_INDICATOR);
        c->needle=lv_line_create(c->gauge);
        lv_line_set_points_mutable(c->needle,c->line,2);
        lv_obj_set_style_line_width(c->needle,3,0);
        lv_obj_set_style_line_color(c->needle,accent,LV_PART_MAIN);
    } else if (w->kind==PACK_HORIZON) {
        c->gauge=lv_line_create(c->obj);
        lv_obj_set_style_line_width(c->gauge,5,0);
        lv_obj_set_style_line_color(c->gauge,accent,LV_PART_MAIN);
    }
    if (c->gauge) {
        lv_obj_set_size(c->gauge,size,size);
        lv_obj_align(c->gauge,LV_ALIGN_CENTER,0,-12);
        lv_obj_set_clickable(c->gauge,false);
        lv_obj_set_hidden(c->gauge,true);
    }
    lv_label_set_text_fmt(c->label,"%s\nUnavailable",w->text);
    lv_obj_align(c->label,LV_ALIGN_BOTTOM_MID,0,-8);
}

static void select_screen(unsigned page)
{
    for (unsigned i=1;i<scene->widgets;i++) {
        const pack_widget_t *w=&scene->widget[i];
        if (w->parent==0) lv_obj_set_hidden(controls[i].obj,(unsigned)w->initial!=page);
    }
    lv_obj_set_hidden(screen_picker,true);
    lv_obj_set_hidden(screen_handle,false);
    lv_obj_set_hidden(controls[0].obj,false);
    // Build a newly visible autopilot before LVGL presents the selected page.
    pack_autopilot_view_tick();
    pack_radio_view_tick();
}

static void choose_screen(lv_event_t *event)
{
    if (!visible() || !screen_picker || !lv_obj_is_visible(screen_picker)) return;
    unsigned page=(unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (page==scene->screen_count) pack_panel_leave();
    else select_screen(page);
}

static void show_screen_picker(void)
{
    cancel_rate_sliders(-1);
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (permissions & PACK_PERM_MONITOR) pack_desktop_select(NULL,NULL,scene->portrait);
#endif
    if (permissions & PACK_PERM_JOYSTICK) hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    reset_joystick();
    lv_obj_set_hidden(controls[0].obj,true);
    lv_obj_set_hidden(screen_handle,true);
    lv_obj_set_hidden(screen_picker,false);
    pack_autopilot_view_tick();
    pack_radio_view_tick();
}

#if CONFIG_KVM_UI_BENCH
bool pack_view_bench_navigate(int page)
{
    if (!scene || scene->permissions!=PACK_PERM_UI || !visible() || !screen_picker ||
        page < -1 || page>=scene->screen_count) return false;
    if (page<0 && lv_obj_is_visible(screen_handle)) show_screen_picker();
    else if (page>=0 && lv_obj_is_visible(screen_picker)) select_screen((unsigned)page);
    else return false;
    return true;
}
#endif

static void handle_swipe(lv_event_t *event)
{
    lv_event_code_t code=lv_event_get_code(event);
    if (!visible() || !screen_picker || !lv_obj_is_visible(screen_handle)) return;
    if (code==LV_EVENT_PRESS_LOST) { handle_down=false; return; }
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING && code!=LV_EVENT_RELEASED) return;
    lv_indev_t *indev=lv_indev_active();
    if (!indev) return;
    lv_point_t raw, point;
    lv_indev_get_point(indev,&raw);
    pack_view_logical_point(scene->portrait && lv_display_get_rotation(lv_obj_get_display(screen_handle))==
                            LV_DISPLAY_ROTATION_0,&raw,&point);
    if (code==LV_EVENT_PRESSED) {
        handle_start=point; handle_down=true; handle_moved=false;
        return;
    }
    if (!handle_down) return;
    int up=handle_start.y-point.y, side=LV_ABS(point.x-handle_start.x);
    if (side>8 || LV_ABS(up)>8) handle_moved=true;
    if (code!=LV_EVENT_RELEASED) return;
    handle_down=false;
    if (handle_moved && (up<48 || up<=side)) return;
    show_screen_picker();
}

static void switcher_button_style(lv_obj_t *button)
{
    // Keep the normal and pressed palette colors. Fading a navigation button
    // repeatedly flushes the same region immediately before its page disappears.
    static const lv_style_prop_t colors[]={LV_STYLE_BG_COLOR,LV_STYLE_RECOLOR_OPA,LV_STYLE_RECOLOR,0};
    static const lv_style_transition_dsc_t immediate={.props=colors,.path_xcb=lv_anim_path_linear};
    ui_theme_control_button(button);
    lv_obj_set_style_transition(button,&immediate,LV_STATE_DEFAULT);
    lv_obj_set_style_transition(button,&immediate,LV_STATE_PRESSED);
}

static bool create_switcher(void)
{
    int width=scene->portrait ? 600 : 1024, height=scene->portrait ? 992 : 568;
    screen_handle=lv_button_create(viewport);
    if (!screen_handle) return false;
    switcher_button_style(screen_handle);
    lv_obj_set_size(screen_handle,width,48);
    lv_obj_set_pos(screen_handle,0,height-48);
    lv_obj_set_press_lock(screen_handle,true);
    lv_obj_t *hint=lv_label_create(screen_handle);
    if (!hint) return false;
    lv_label_set_text_static(hint,LV_SYMBOL_UP "  Screens");
    lv_obj_center(hint);
    if (!lv_obj_add_event_cb(screen_handle,handle_swipe,LV_EVENT_ALL,NULL)) return false;
    screen_picker=lv_obj_create(viewport);
    if (!screen_picker) return false;
    plain(screen_picker);
    ui_theme_apply(screen_picker,UI_STYLE_CONTROL_PAGE,LV_STATE_DEFAULT);
    lv_obj_set_size(screen_picker,width,height);
    lv_obj_t *title=lv_label_create(screen_picker);
    if (!title) return false;
    lv_obj_set_style_text_font(title,ui_theme_font(UI_FONT_TITLE),0);
    lv_label_set_text_static(title,"Screens");
    lv_obj_set_pos(title,20,16);
    for (unsigned i=0;i<scene->widgets;i++) {
        const pack_widget_t *w=&scene->widget[i];
        if (i && w->parent!=0) continue;
        unsigned page=i ? (unsigned)w->initial : scene->screen_count;
        lv_obj_t *button=lv_button_create(screen_picker);
        if (!button) return false;
        switcher_button_style(button);
        int column=(width-52)/2;
        lv_obj_set_size(button,i ? column : width-40,i ? 64 : 52);
        lv_obj_set_pos(button,i ? 20+(page%2)*(column+12) : 20,i ? 64+(page/2)*76 : height-72);
        lv_obj_t *caption=lv_label_create(button);
        if (!caption) return false;
        lv_obj_remove_style_all(caption);
        lv_label_set_text(caption,i ? w->text : "Return to KVMpad");
        lv_obj_center(caption);
        if (!lv_obj_add_event_cb(button,choose_screen,LV_EVENT_CLICKED,(void *)(uintptr_t)page)) return false;
    }
    select_screen(0);
    return true;
}

static bool create_control(unsigned idx)
{
    const pack_widget_t *w=&scene->widget[idx];
    lv_obj_t *parent=w->parent<0 ? viewport : controls[w->parent].obj;
    control_t *c=&controls[idx];
    if (w->kind==PACK_TABS) {
        c->selected=w->initial;
        c->obj=create_tab_strip(parent,w,idx);
        return c->obj!=NULL;
    }
    if (w->kind==PACK_RATE_SLIDER) {
        c->obj=ui_rate_slider_create(parent,rate_step,(void *)(uintptr_t)idx);
        if (!c->obj) return false;
        lv_obj_set_pos(c->obj,w->x,w->y);
        lv_obj_set_size(c->obj,w->w,w->h);
        return true;
    }
    c->obj=w->kind==PACK_BUTTON ? lv_button_create(parent) : lv_obj_create(parent);
    if (!c->obj) return false;
    if (w->kind==PACK_BUTTON) ui_theme_control_button(c->obj);
    bool container=w->kind==PACK_CONTAINER || w->kind==PACK_SPACER;
    if (container || w->kind==PACK_TEXT) plain(c->obj);
    bool input_face=w->kind==PACK_TOUCH || w->kind==PACK_AXIS || w->kind==PACK_SLIDER || w->kind==PACK_DIAL;
    if (input_face) {
        ui_theme_apply(c->obj,UI_STYLE_SCREEN,LV_PART_MAIN);
        ui_theme_apply(c->obj,UI_STYLE_CARD,LV_PART_MAIN);
    }
    lv_obj_set_scrollable(c->obj,false);
    lv_obj_set_size(c->obj,w->w,w->h);
    lv_obj_set_pos(c->obj,w->x,w->y);
    if (container) return true;
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (w->kind==PACK_MONITOR) return pack_desktop_viewport(c->obj,w->w,&c->gauge);
    if (w->kind==PACK_TOUCHPAD) return pack_desktop_pad(c->obj,viewport);
    if (w->kind==PACK_KEYBOARD) {
        c->gauge=pack_keyboard_surface_open(c->obj,w->w,w->h,pack_desktop_ready);
        return c->gauge!=NULL;
    }
#endif
    if (w->kind==PACK_CHART) return pack_chart_view_open(c->obj,scene,w,chart_storage);
    if (w->kind==PACK_AUTOPILOT) return pack_autopilot_view_open(c->obj,w->w,w->h);
    if (w->kind==PACK_RADIO) return pack_radio_view_open(c->obj,w->w,w->h,w->tab_count!=0);
    c->label=lv_label_create(c->obj);
    if (!c->label) return false;
    if (w->kind==PACK_BUTTON || w->kind==PACK_TEXT) lv_obj_remove_style_all(c->label);
    if (input_face) ui_theme_apply(c->label,UI_STYLE_TEXT_HEADING,LV_PART_MAIN);
    lv_obj_set_style_text_font(c->label,ui_theme_font(UI_FONT_HEADING),0);
    lv_label_set_text(c->label,w->text);
    lv_obj_center(c->label);
    if (w->kind==PACK_TEXT) lv_obj_set_clickable(c->obj,false);
    if (w->kind==PACK_BUTTON && w->icon_member && w->w>=64 && w->h>=40) {
        unsigned slot=0;
        for (const pack_icon_t *icon=scene->icons;icon;icon=icon->next,slot++) {
            if (icon->member!=w->icon_member) continue;
            lv_obj_set_style_pad_all(c->obj,0,0);
            int border=lv_obj_get_style_border_width(c->obj,LV_PART_MAIN);
            c->gauge=lv_image_create(c->obj);
            if (!c->gauge) return false;
            lv_image_set_src(c->gauge,&icon_images[slot]);
            if (lv_image_get_src(c->gauge)!=&icon_images[slot]) return false;
            lv_obj_set_clickable(c->gauge,false);
            lv_obj_align(c->gauge,LV_ALIGN_LEFT_MID,6-border,0);
            lv_obj_set_clickable(c->label,false);
            lv_obj_set_width(c->label,w->w-18-icon->width);
            lv_label_set_long_mode(c->label,LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(c->label,LV_TEXT_ALIGN_LEFT,0);
            lv_obj_align(c->label,LV_ALIGN_LEFT_MID,12+icon->width-border,0);
            break;
        }
        if (!c->gauge) return false;
    } else if (w->kind==PACK_TOUCH) {
        lv_obj_set_clickable(c->obj,true);
        lv_obj_set_style_border_width(c->obj,2,0);
        lv_obj_set_style_border_color(c->obj,lv_color_hex(UI_COLOR_ACCENT),0);
        lv_obj_set_press_lock(c->obj,false);
        lv_obj_add_event_cb(c->obj,touch_event,LV_EVENT_ALL,(void *)(uintptr_t)idx);
    } else if (w->kind==PACK_DIAL) {
        lv_obj_set_style_pad_all(c->obj,0,0);
        c->gauge=lv_arc_create(c->obj);
        int size=LV_MIN(w->w-8,w->h-32);
        lv_obj_set_size(c->gauge,size,size);
        lv_obj_align(c->gauge,LV_ALIGN_TOP_MID,0,2);
        lv_arc_set_bg_angles(c->gauge,135,45);
        lv_obj_set_style_arc_color(c->gauge,lv_color_hex(UI_COLOR_EDGE),LV_PART_MAIN);
        lv_obj_set_style_arc_color(c->gauge,lv_color_hex(UI_COLOR_ACCENT_INK),LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c->gauge,lv_color_hex(UI_COLOR_ACCENT_INK),LV_PART_KNOB);
        lv_arc_set_range(c->gauge,w->min,w->max);
        lv_arc_set_value(c->gauge,w->initial);
        c->selected=w->initial;
        lv_label_set_text_fmt(c->label,"%s %d",w->text,(int)w->initial);
        lv_obj_align(c->label,LV_ALIGN_BOTTOM_MID,0,-2);
        lv_obj_add_event_cb(c->gauge,dial_event,LV_EVENT_VALUE_CHANGED,(void *)(uintptr_t)idx);
        if (scene->portrait && !lv_obj_add_event_cb(c->gauge,portrait_dial_point,
                LV_EVENT_PRESSING | LV_EVENT_PREPROCESS,NULL)) return false;
    } else if (w->kind==PACK_CHIP) {
        // Same order as the seven declarative StatusChip roles in pack_scene.
        static const struct { const char *symbol; uint32_t color; } roles[]={
            {LV_SYMBOL_MINUS,UI_COLOR_MUTED}, {LV_SYMBOL_SETTINGS,UI_COLOR_ACCENT_INK},
            {LV_SYMBOL_EYE_OPEN,UI_COLOR_WARN}, {"i",UI_COLOR_ACCENT_INK},
            {LV_SYMBOL_OK,UI_COLOR_OK}, {LV_SYMBOL_WARNING,UI_COLOR_WARN}, {LV_SYMBOL_CLOSE,UI_COLOR_DANGER}
        };
        lv_color_t ink=lv_color_hex(roles[w->chip_style].color);
        ui_theme_apply(c->obj,UI_STYLE_SCREEN,LV_PART_MAIN);
        ui_theme_apply(c->obj,UI_STYLE_CARD,LV_PART_MAIN);
        ui_theme_apply(c->label,UI_STYLE_TEXT_HEADING,LV_PART_MAIN);
        lv_obj_set_clickable(c->obj,false);
        lv_obj_set_style_radius(c->obj,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_border_width(c->obj,2,0);
        lv_obj_set_style_pad_all(c->obj,4,0);
        lv_obj_set_style_border_color(c->obj,ink,0);
        lv_obj_t *marker=lv_label_create(c->obj);
        lv_label_set_text_static(marker,roles[w->chip_style].symbol);
        lv_obj_set_style_text_font(marker,&lv_font_montserrat_16,0);
        lv_obj_set_style_text_color(marker,ink,0);
        lv_obj_set_style_text_align(marker,LV_TEXT_ALIGN_CENTER,0);
        lv_obj_set_width(marker,18);
        lv_obj_align(marker,LV_ALIGN_LEFT_MID,1,0);
        lv_obj_set_width(c->label,LV_MAX(1,w->w-36));
        lv_label_set_long_mode(c->label,LV_LABEL_LONG_DOT);
        lv_obj_align(c->label,LV_ALIGN_LEFT_MID,22,0);
    }
    if (w->kind==PACK_BUTTON || w->kind==PACK_AXIS || w->kind==PACK_SLIDER) {
        lv_obj_set_clickable(c->obj,true);
        if (!lv_obj_add_event_cb(c->obj,widget_event,LV_EVENT_ALL,(void *)(uintptr_t)idx)) return false;
    }
    if (w->kind==PACK_AXIS) {
        c->gauge=lv_obj_create(c->obj);
        plain(c->gauge);
        lv_obj_set_size(c->gauge,22,22);
        lv_obj_set_style_radius(c->gauge,LV_RADIUS_CIRCLE,0);
        lv_obj_set_style_bg_color(c->gauge,lv_color_hex(UI_COLOR_ACCENT),0);
        lv_obj_set_style_bg_opa(c->gauge,LV_OPA_COVER,0);
        lv_obj_set_clickable(c->gauge,false);
        lv_obj_center(c->gauge);
        lv_obj_align(c->label,LV_ALIGN_TOP_MID,0,8);
    }
    if (w->kind==PACK_SLIDER) {
        lv_obj_align(c->label,LV_ALIGN_TOP_MID,0,-4);
        c->gauge=lv_bar_create(c->obj);
        lv_obj_set_clickable(c->gauge,false); // route touches on the displayed track to the slider's input handler
        lv_obj_set_size(c->gauge,w->w-32,12);
        lv_obj_set_style_bg_color(c->gauge,lv_color_hex(UI_COLOR_WELL),LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c->gauge,LV_OPA_COVER,LV_PART_MAIN);
        lv_obj_set_style_border_color(c->gauge,lv_color_hex(UI_COLOR_EDGE),LV_PART_MAIN);
        lv_obj_set_style_border_width(c->gauge,1,LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->gauge,lv_color_hex(UI_COLOR_ACCENT_INK),LV_PART_INDICATOR);
        lv_bar_set_range(c->gauge,w->min<0 ? -32767 : 0,w->min<0 ? 32767 : 65535);
        lv_bar_set_value(c->gauge,0,LV_ANIM_OFF);
        lv_obj_align(c->gauge,LV_ALIGN_BOTTOM_MID,0,-2);
    } else if (w->kind>=PACK_GAUGE_ARC && w->kind<=PACK_VALUE) instrument(c,w);
    else if (w->kind==PACK_STREAM) {
        // The preview has no captured pixels. Keep its empty frame neutral in every palette.
        lv_obj_set_style_bg_color(c->obj,lv_color_black(),0);
        lv_obj_set_style_border_color(c->obj,lv_color_hex(UI_COLOR_MUTED),0);
        lv_obj_set_style_border_width(c->obj,1,0);
        lv_obj_set_style_text_color(c->label,lv_color_white(),0);
        lv_label_set_text_static(c->label,(permissions & PACK_PERM_CAPTURE) ? "Capture stopped" :
                                 "Screen sharing is off for this extension.\nYou can allow it in the permission review.");
    }
    return true;
}

static void render_instrument(unsigned idx)
{
    control_t *c=&controls[idx];
    const pack_widget_t *w=&scene->widget[idx];
    bool valid=c->valid[0] && (w->kind!=PACK_HORIZON || c->valid[1]);
    if (c->gauge) lv_obj_set_hidden(c->gauge,!valid);
    if (!valid) {
        const char *status=(c->quality[0]==KVB_ERROR || c->quality[1]==KVB_ERROR) ? "Data error" :
                           (c->quality[0]==KVB_STALE || c->quality[1]==KVB_STALE) ? "Stale" : "Unavailable";
        lv_label_set_text_fmt(c->label,"%s\n%s",w->text,status);
        return;
    }
    char text[144];
    if (w->kind==PACK_HORIZON) {
        double pitch=fmax(-90,fmin(90,c->value[0])), rad=fmod(c->value[1],360.0)*0.01745329252;
        int mid=(w->w<w->h ? w->w-40 : w->h-70)/2;
        int dx=(int)((mid-3)*cos(rad)), dy=(int)((mid-3)*sin(rad));
        int center=mid+(int)(pitch*mid/90);
        c->line[0]=(lv_point_precise_t){mid-dx,center-dy};
        c->line[1]=(lv_point_precise_t){mid+dx,center+dy};
        lv_line_set_points(c->gauge,c->line,2);
        snprintf(text,sizeof(text),"%s\nPitch %.1f   Roll %.1f",w->text,c->value[0],c->value[1]);
    } else {
        double value=fmax(w->min,fmin(w->max,c->value[0]));
        if (w->kind==PACK_GAUGE_ARC && w->max>w->min)
            lv_arc_set_value(c->gauge,(int)((value-w->min)*1000/(w->max-w->min)));
        if (w->kind==PACK_GAUGE_NEEDLE)
            lv_scale_set_line_needle_value(c->gauge,c->needle,lv_obj_get_width(c->gauge)/2-8,(int)value);
        snprintf(text,sizeof(text),"%s\n%.1f %s",w->text,c->value[0],w->unit);
    }
    lv_label_set_text(c->label,text);
}

void pack_view_provider(bool use_xplane) { xplane=use_xplane; }

#if CONFIG_KVM_USB_MONITOR_EVAL
bool pack_view_touchpad(unsigned count,const lv_point_t logical[2],const uint8_t ids[2],uint64_t timestamp_us)
{
    return scene && (permissions & PACK_PERM_MOUSE) && pack_desktop_contacts(count,logical,ids,timestamp_us);
}

static void update_desktop(void)
{
    lv_obj_t *image=NULL,*pad=NULL,*keyboard=NULL;
    for (unsigned i=0;i<scene->widgets;i++) {
        switch (scene->widget[i].kind) {
        case PACK_MONITOR: if (lv_obj_is_visible(controls[i].obj)) image=controls[i].gauge; break;
        case PACK_TOUCHPAD: if (lv_obj_is_visible(controls[i].obj)) pad=controls[i].obj; break;
        case PACK_KEYBOARD: if (lv_obj_is_visible(controls[i].obj)) keyboard=controls[i].gauge; break;
        default: break;
        }
    }
    pack_desktop_select(image,pad,scene->portrait);
    pack_desktop_tick(keyboard);
}
#endif

void pack_view_tick(void)
{
    if (!scene) return;
    if (!visible()) { if (foreground) pack_view_close(); return; }
    foreground=true;
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (permissions & PACK_PERM_MONITOR) update_desktop();
#endif
    pack_autopilot_view_tick();
    pack_radio_view_tick();
    if (scene->sources && (permissions & PACK_PERM_TELEMETRY) && !attempted) {
        attempted=true;
        unsigned channels=0;
        for (unsigned i=0;i<scene->sources;i++)
            channels |= 1u << scene->source[i].channel;
        bool composite=scene->composite && (permissions & PACK_PERM_CAPTURE);
        subscribed=kvmbridge_instruments_for_profile(scene->id,xplane,channels,composite);
    }
    if (lv_tick_elaps(sample_tick)<50) return;
    sample_tick=lv_tick_get();
    for (unsigned i=0; i<scene->bindings; i++) {
        pack_binding_t *b=&scene->binding[i];
        if (b->local) {
            if (b->transform==PACK_LOWPASS) update_binding(b,&scene->state[b->source].value,KVB_VALID,sample_tick);
            continue;
        }
        const pack_source_t *src=&scene->source[b->source];
        kvmbridge_sample_t sample={0};
        bool ok=subscribed && kvmbridge_sample(src->channel,&sample);
        kvmbridge_quality_t quality=ok ? sample.quality : KVB_UNAVAILABLE;
        if (quality==KVB_VALID && sample.age_ms>src->max_age_ms) quality=KVB_STALE;
        bool valid=quality==KVB_VALID && isfinite(sample.value);
        pack_value_t input={.real=sample.value};
        update_binding(b,valid ? &input : NULL,quality,sample_tick);
    }
}

static void deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e)==viewport) pack_view_close();
}

unsigned pack_view_available_permissions(void)
{
    unsigned mask = PACK_PERM_UI | PACK_PERM_TELEMETRY | PACK_PERM_CAPTURE;
#if CONFIG_KVM_USB_MONITOR_EVAL
    mask |= PACK_PERM_MONITOR | PACK_PERM_KEYBOARD | PACK_PERM_MOUSE;
#endif
#if KVM_HID_HAS_HPE && CONFIG_KVM_HPE_JOYSTICK_QUALIFIED
    mask |= PACK_PERM_JOYSTICK;
#endif
    return mask;
}

bool pack_view_open(lv_obj_t *content, pack_scene_t *s, unsigned granted)
{
    if (!content || !s || scene || (s->required_permissions & ~granted)) return false;
    if (s->required_permissions & ~pack_view_available_permissions()) return false;
    // Charge resident IR, renderer state and a per-widget LVGL allowance. Screens and
    // font caches shared with the system stay outside the pack's declared scene budget.
    size_t charge=sizeof(*s)+(size_t)s->widgets*sizeof(*controls);
    unsigned icons=0;
    for (const pack_icon_t *icon=s->icons;icon;icon=icon->next) {
        charge+=sizeof(*icon)+4u*icon->width*icon->height+sizeof(*icon_images);
        icons++;
    }
    size_t chart_size=0;
    for (unsigned i=0;i<s->widgets;i++) {
        const pack_widget_t *w=&s->widget[i];
        if (w->kind==PACK_AUTOPILOT) charge+=48u*1024u;
        if (w->kind==PACK_RADIO) charge+=64u*1024u;
        if (w->kind==PACK_CHART) {
            if (chart_size) return false;
            chart_size=pack_chart_view_storage();
            charge+=pack_chart_view_budget(w->w,w->tab_count);
            for (unsigned j=0;j<w->tab_count;j++) {
                const pack_chart_t *chart=pack_chart_at(s,j);
                if (!chart) return false;
                charge+=chart->run_count;
            }
        } else charge += 1536u*w->tab_count + (w->kind==PACK_DIAL ? 1024u : 0u);
        if (w->icon_member && w->w>=64 && w->h>=40) charge+=1024;
        // A plain text face owns a container and one label. A tab strip's buttons
        // and captions are already budgeted individually above.
        charge += (w->kind==PACK_CONTAINER || w->kind==PACK_SPACER || w->kind==PACK_TABS) ? 1024 :
                  w->kind==PACK_TEXT ? 1536 : w->kind==PACK_KEYBOARD ? 48u*1024u :
                  (w->kind>=PACK_GAUGE_ARC && w->kind<=PACK_HORIZON) ? 6144 : 3072;
    }
    if (s->screen_count) charge+=(s->screen_count+4u)*1536u;
    if (charge>(size_t)s->max_memory_kib*1024u) return false;
    controls=heap_caps_calloc(1,(size_t)s->widgets*sizeof(*controls)+icons*sizeof(*icon_images)+chart_size,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!controls) return false;
    icon_images=(lv_image_dsc_t *)(controls+s->widgets);
    chart_storage=chart_size ? icon_images+icons : NULL;
    unsigned i=0;
    for (const pack_icon_t *icon=s->icons;icon;icon=icon->next,i++) {
        icon_images[i]=(lv_image_dsc_t){
            .header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_ARGB8888,
                     .w=icon->width,.h=icon->height,.stride=4u*icon->width},
            .data_size=4u*icon->width*icon->height,.data=icon->bgra
        };
    }
    viewport=content; scene=s; permissions=s->permissions & granted & pack_view_available_permissions();
    attempted=subscribed=foreground=false; events=actions=0; window_start=lv_tick_get(); reset_joystick();
    if (!lv_obj_add_event_cb(viewport,deleted,LV_EVENT_DELETE,NULL)) {
        pack_view_close();
        return false;
    }
    for (unsigned i=0;i<s->widgets;i++) {
        if (!create_control(i)) {
            // A preceding button may already borrow a thumbnail; detach before freeing its descriptor.
            lv_obj_t *root=controls[0].obj;
            pack_view_close();
            if (root) lv_obj_delete(root);
            return false;
        }
    }
    for (unsigned i=0;i<s->widgets;i++) if (s->widget[i].kind==PACK_TABS) show_tabs(i);
    if (s->screen_count && !create_switcher()) {
        lv_obj_t *root=controls[0].obj;
        pack_view_close();
        lv_obj_delete(root);
        return false;
    }
    for (unsigned i=0;i<s->bindings;i++) {
        pack_binding_t *b=&s->binding[i];
        b->seeded=false;
        if (!b->local) update_binding(b,NULL,KVB_UNAVAILABLE,lv_tick_get());
    }
    sample_tick=lv_tick_get()-50;
    local_bindings(-1);
    return true;
}

void pack_view_enter_magnifier(void)
{
    // The full-screen magnifier owns the same profile until Home, Stop or card removal.
    // Its close path stops both transports. The controls release their HID state now.
    if (scene && subscribed && kvmbridge_transfer_telemetry(scene->id)) subscribed=false;
    pack_view_close();
}

void pack_view_close(void)
{
    if (!scene) return;
    cancel_rate_sliders(-1);
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (permissions & PACK_PERM_MONITOR) pack_desktop_close();
#endif
    lv_obj_remove_event_cb_with_user_data(viewport,deleted,NULL);
    if (screen_handle) lv_obj_delete(screen_handle);
    if (screen_picker) lv_obj_delete(screen_picker);
    screen_handle=screen_picker=NULL;
    handle_down=false;
    if (subscribed) kvmbridge_stop();
    if (permissions & PACK_PERM_JOYSTICK) hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    reset_joystick();
    // LVGL borrows the point arrays and the parent may still draw before deletion.
    pack_chart_view_close();
    pack_autopilot_view_close();
    pack_radio_view_close();
    chart_storage=NULL;
    for (unsigned i=0;i<scene->widgets;i++) {
        control_t *c=&controls[i];
        if (c->needle) lv_line_set_points(c->needle,NULL,0);
        if (scene->widget[i].kind==PACK_BUTTON && c->gauge) lv_image_set_src(c->gauge,NULL);
        if (scene->widget[i].kind==PACK_HORIZON && c->gauge) lv_line_set_points(c->gauge,NULL,0);
    }
    unsigned i=0;
    for (const pack_icon_t *icon=scene->icons;icon;icon=icon->next,i++)
        lv_image_cache_drop(&icon_images[i]);
    icon_images=NULL;
    scene=NULL; viewport=NULL; permissions=0; subscribed=foreground=false;
    heap_caps_free(controls); controls=NULL;
}
