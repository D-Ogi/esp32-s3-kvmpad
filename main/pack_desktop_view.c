// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "sdkconfig.h"
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "pack_desktop_view.h"
#include "pack_keyboard_surface.h"
#include "pack_touchpad_drawer.h"
#include "monitor_viewport.h"
#include "gesture_engine.h"
#include "hid_dispatch.h"
#include "usb_hid_device.h"
#include "ui_theme.h"
#include "ui_segmented.h"

static lv_obj_t *current_image, *current_pad;
static bool running, mounted, portrait, tracking, ignore_contact, native_signal;
static uint32_t sequence;
static bool awaiting_mount;
static uint32_t previous_usb_epoch;
static uint8_t first_id, previous_count;
static kvm_gesture_t gesture;
static bool controls_hidden, image_dragged, actual_size;
static lv_point_t press_point, previous_point, pan;

static void release_inputs(void)
{
    pack_keyboard_surface_release();
    if (running) hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    if (tracking) ignore_contact=true;
    tracking=false;
    previous_count=0;
    kvm_gesture_cfg_t cfg=KVM_GESTURE_CFG_DEFAULT;
    cfg.h_res=portrait ? 600 : 1024;
    cfg.v_res=portrait ? 1024 : 600;
    kvm_gesture_init(&gesture,&cfg);
}

static void caption(const char *prompt)
{
    if (!current_image) return;
    lv_obj_t *button=lv_obj_get_sibling(current_image,1);
    lv_obj_t *resolution=lv_obj_get_sibling(current_image,2);
    lv_obj_t *zoom=lv_obj_get_sibling(current_image,3);
    lv_label_set_text_static(lv_obj_get_child(button,0),prompt);
    lv_obj_align(button,running ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_CENTER,running ? -6 : 0,running ? 6 : 26);
    lv_obj_set_hidden(button,running && mounted && controls_hidden);
    lv_obj_set_hidden(zoom,!running || !mounted || controls_hidden);
    lv_obj_set_hidden(resolution,running);
    if (!running) ui_segmented_set_active(resolution,native_signal ? 1 : 0);
}

static int pan_offset(int proposed,int viewport,int source)
{
    if (source<=viewport) return 0;
    int maximum=source/2-viewport/2;
    return LV_CLAMP(maximum+viewport-source,proposed,maximum);
}

static void apply_framing(void)
{
    if (!current_image) return;
    lv_image_set_inner_align(current_image,actual_size ? LV_IMAGE_ALIGN_CENTER : LV_IMAGE_ALIGN_CONTAIN);
    if (actual_size) lv_image_set_scale(current_image,LV_SCALE_NONE);
    const lv_image_dsc_t *src=lv_image_get_src(current_image);
    int x=0,y=0;
    if (actual_size && src) {
        lv_obj_update_layout(current_image);
        x=pan_offset(pan.x,lv_obj_get_width(current_image),src->header.w);
        y=pan_offset(pan.y,lv_obj_get_height(current_image),src->header.h);
    }
    if (x!=lv_image_get_offset_x(current_image)) lv_image_set_offset_x(current_image,x);
    if (y!=lv_image_get_offset_y(current_image)) lv_image_set_offset_y(current_image,y);
    lv_obj_t *button=lv_obj_get_sibling(current_image,3);
    lv_label_set_text_static(lv_obj_get_child(button,0),actual_size ? "Fit" : "1:1");
}

static void set_zoom(bool zoomed)
{
    actual_size=zoomed;
    pan=(lv_point_t){0,0};
    apply_framing();
}

static void reset_view(void)
{
    controls_hidden=false;
    set_zoom(false);
}

static void zoom_clicked(lv_event_t *event)
{
    lv_obj_t *button=lv_event_get_current_target_obj(event);
    if (current_image && button==lv_obj_get_sibling(current_image,3) && pack_desktop_ready())
        set_zoom(!actual_size);
}

static void draw_waiting_hint(lv_event_t *event)
{
    lv_obj_t *image=lv_event_get_current_target_obj(event);
    // A black desktop is a valid picture. The renderer sets the image source
    // on first presentation and clears it on receiver reset or mode change.
    if (!running || !mounted || image!=current_image || lv_image_get_src(image)) return;
    lv_area_t area;
    lv_obj_get_coords(image,&area);
    bool compact=lv_area_get_width(&area)<480;
    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.text=compact ? "Waiting for a picture\nWin+P: select Extend" :
                         "Waiting for the first picture\nIn Windows, press Win+P and choose Extend.";
    label.font=ui_theme_font(compact ? UI_FONT_LABEL : UI_FONT_BODY);
    label.color=lv_color_white(); // The viewport stays black in every palette.
    label.align=LV_TEXT_ALIGN_CENTER;
    label.text_static=1;
    label.line_space=2;
    int text_height=2*lv_font_get_line_height(label.font)+label.line_space;
    area.y1+=LV_MAX(56,(lv_area_get_height(&area)-text_height)/2);
    lv_draw_label(lv_event_get_layer(event),&label,&area);
}

static void image_touch(lv_event_t *event)
{
    lv_obj_t *image=lv_event_get_current_target_obj(event);
    lv_event_code_t code=lv_event_get_code(event);
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING && code!=LV_EVENT_CLICKED) return;
    if (image!=current_image || !mounted || !pack_desktop_ready()) return;
    lv_indev_t *indev=lv_event_get_indev(event);
    if (!indev || lv_indev_get_type(indev)!=LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(indev,&point);
    lv_obj_transform_point(image,&point,LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE | LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
    if (code==LV_EVENT_PRESSED) {
        press_point=previous_point=point;
        image_dragged=false;
        return;
    }
    if (LV_ABS(point.x-press_point.x)>4 || LV_ABS(point.y-press_point.y)>4) image_dragged=true;
    if (code==LV_EVENT_CLICKED && !image_dragged) {
        controls_hidden=!controls_hidden;
        caption("Stop");
    }
    const lv_image_dsc_t *src=lv_image_get_src(image);
    if (!image_dragged || !src || lv_image_get_inner_align(image)!=LV_IMAGE_ALIGN_CENTER) return;
    int x=lv_image_get_offset_x(image), y=lv_image_get_offset_y(image);
    int next_x=pan_offset(x+point.x-previous_point.x,lv_obj_get_width(image),src->header.w);
    int next_y=pan_offset(y+point.y-previous_point.y,lv_obj_get_height(image),src->header.h);
    if (x!=next_x) { pan.x=next_x; lv_image_set_offset_x(image,next_x); }
    if (y!=next_y) { pan.y=next_y; lv_image_set_offset_y(image,next_y); }
    previous_point=point;
}

bool pack_desktop_ready(void)
{
    if (!running || !usb_hid_device_monitor_mounted()) return false;
    if (awaiting_mount) {
        usb_hid_status_t usb;
        usb_hid_device_get_status(&usb);
        if (usb.usb_epoch==previous_usb_epoch) return false;
        awaiting_mount=false;
    }
    return true;
}

static bool select_controls(bool native)
{
    return usb_hid_device_set_monitor_controls(true,native)==ESP_OK;
}

bool pack_desktop_change_mode(bool native)
{
    if (!running || !current_image || !lv_obj_is_visible(current_image)) return false;
    if (native==native_signal) return true;
    if (awaiting_mount && usb_hid_device_monitor_mounted() && !pack_desktop_ready()) return false;
    release_inputs();
    reset_view();
    usb_hid_status_t usb;
    usb_hid_device_get_status(&usb);
    usb_monitor_mode_result_t result=monitor_viewport_change_mode(native,select_controls);
    if (result==USB_MONITOR_MODE_CHANGED) {
        previous_usb_epoch=usb.usb_epoch;
        awaiting_mount=true;
        native_signal=native;
        mounted=false;
        pack_touchpad_drawer_update(false);
        caption("Connecting  /  Stop");
        return true;
    }
    if (result==USB_MONITOR_MODE_STOPPED) {
        usb_hid_device_set_monitor_controls(false,false);
        running=mounted=awaiting_mount=false;
        pack_touchpad_drawer_update(false);
        caption("USB monitor unavailable  /  Retry");
    } else caption(mounted ? "Stop" : "Connecting  /  Stop");
    return false;
}

static void toggle(lv_event_t *event)
{
    lv_obj_t *button=lv_event_get_current_target_obj(event);
    if (!current_image || lv_obj_get_parent(button)!=lv_obj_get_parent(current_image) || !lv_obj_is_visible(button)) return;
    if (running) {
        release_inputs();
        reset_view();
        usb_hid_device_set_monitor_controls(false,false);
        monitor_viewport_stop();
        running=mounted=awaiting_mount=false;
        caption("Start USB monitor");
        return;
    }
    if (!monitor_viewport_start(current_image,native_signal)) {
        caption("Memory unavailable  /  Retry");
        return;
    }
    if (usb_hid_device_set_monitor_controls(true,native_signal)!=ESP_OK) {
        monitor_viewport_stop();
        caption("USB unavailable  /  Retry");
        return;
    }
    running=true;
    caption("Connecting  /  Stop");
}

static void choose_resolution(uint16_t id,uint16_t index,void *context)
{
    (void)id;
    (void)context;
    if (!running && current_image) {
        native_signal=index==1;
        caption("Start USB monitor");
    }
}

bool pack_desktop_viewport(lv_obj_t *parent,int width,lv_obj_t **image)
{
    lv_obj_set_style_pad_all(parent,0,0);
    lv_obj_set_style_border_width(parent,0,0);
    lv_obj_set_style_bg_color(parent,lv_color_black(),0);
    lv_obj_set_style_bg_opa(parent,LV_OPA_COVER,0);
    lv_obj_set_scrollable(parent,false);
    *image=lv_image_create(parent);
    if (!*image) return false;
    lv_obj_set_size(*image,lv_pct(100),lv_pct(100));
    lv_obj_set_clickable(*image,true);
    lv_obj_set_scrollable(*image,false);
    lv_obj_set_adv_hittest(*image,false);
    lv_obj_set_press_lock(*image,true);
    lv_image_set_inner_align(*image,LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_t *button=lv_button_create(parent);
    if (!button) return false;
    ui_theme_control_button(button);
    lv_obj_set_height(button,48);
    lv_obj_t *label=lv_label_create(button);
    if (!label) return false;
    if (width<256) lv_obj_set_style_pad_hor(button,4,0);
    int inset=lv_obj_get_style_pad_left(button,0)+lv_obj_get_style_pad_right(button,0);
    lv_obj_set_style_max_width(label,width-inset-16,0);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_text_static(label,"Start USB monitor");
    lv_obj_center(label);
    lv_obj_align(button,LV_ALIGN_CENTER,0,26);
    static const ui_segment_t modes[]={{.label="640×480"},{.label="1024×600"}};
    static const ui_segmented_desc_t desc={.segments=modes,.count=2,.height=44};
    static ui_segmented_sink_t sink={.fn=choose_resolution};
    lv_obj_t *resolution=ui_segmented_create(parent,&desc,&sink);
    if (!resolution) return false;
    lv_obj_set_style_max_width(resolution,256,0);
    if (width<256) {
        for (unsigned i=0;i<desc.count;i++) {
            lv_obj_t *tab=ui_segmented_tab(resolution,i);
            if (!tab) return false;
            lv_obj_set_style_pad_hor(tab,0,0);
            lv_obj_t *mode_label=lv_obj_get_child(tab,0);
            if (!mode_label) return false;
            lv_obj_set_style_text_font(mode_label,ui_theme_font(UI_FONT_LABEL),0);
        }
    }
    lv_obj_align(resolution,LV_ALIGN_CENTER,0,-26);
    lv_obj_t *zoom=lv_button_create(parent);
    if (!zoom) return false;
    ui_theme_control_button(zoom);
    lv_obj_set_size(zoom,64,44);
    lv_obj_align(zoom,LV_ALIGN_TOP_LEFT,6,6);
    lv_obj_set_hidden(zoom,true);
    lv_obj_t *zoom_label=lv_label_create(zoom);
    if (!zoom_label) return false;
    lv_label_set_text_static(zoom_label,"1:1");
    lv_obj_center(zoom_label);
    return lv_obj_add_event_cb(button,toggle,LV_EVENT_CLICKED,NULL) &&
           lv_obj_add_event_cb(zoom,zoom_clicked,LV_EVENT_CLICKED,NULL) &&
           lv_obj_add_event_cb(*image,draw_waiting_hint,LV_EVENT_DRAW_MAIN_END,NULL) &&
           lv_obj_add_event_cb(*image,image_touch,LV_EVENT_ALL,NULL);
}

static void open_gestures(lv_event_t *event)
{
    lv_obj_t *entry=lv_event_get_current_target_obj(event);
    if (lv_obj_get_parent(entry)!=current_pad || !lv_obj_is_visible(entry) || pack_touchpad_drawer_active()) return;
    release_inputs();
    pack_touchpad_drawer_open(lv_event_get_user_data(event));
}

bool pack_desktop_pad(lv_obj_t *parent,lv_obj_t *content)
{
    ui_theme_touch_surface(parent);
    lv_obj_set_style_pad_all(parent,0,0);
    lv_obj_set_clickable(parent,true);
    lv_obj_t *label=lv_label_create(parent);
    if (!label) return false;
    lv_label_set_text_static(label,"Touchpad");
    lv_obj_set_clickable(label,false);
    lv_obj_align(label,LV_ALIGN_BOTTOM_LEFT,5,-3);
    lv_obj_t *entry=lv_button_create(parent);
    if (!entry) return false;
    ui_theme_control_button(entry);
    lv_obj_set_size(entry,96,44);
    lv_obj_align(entry,LV_ALIGN_TOP_RIGHT,-6,6);
    lv_obj_t *name=lv_label_create(entry);
    if (!name) return false;
    lv_label_set_text_static(name,"Gestures");
    lv_obj_center(name);
    return lv_obj_add_event_cb(entry,open_gestures,LV_EVENT_CLICKED,content)!=NULL;
}

void pack_desktop_select(lv_obj_t *image,lv_obj_t *pad,bool rotated)
{
    if (current_image==image && current_pad==pad && rotated==portrait) return;
    pack_touchpad_drawer_close();
    portrait=rotated;
    release_inputs();
    current_image=image;
    current_pad=pad;
    if (running) monitor_viewport_target(image);
    apply_framing();
    caption(running ? (mounted ? "Stop" : "Connecting  /  Stop") : "Start USB monitor");
}

void pack_desktop_tick(lv_obj_t *keyboard)
{
    bool ready=pack_desktop_ready();
    if (keyboard) {
        usb_hid_status_t usb={0};
        if (ready) usb_hid_device_get_status(&usb);
        pack_keyboard_surface_set_caps(keyboard,usb.led_known,(usb.led_state & KVM_HID_LED_CAPS_LOCK)!=0);
    }
    if (ready==mounted) return;
    if (!ready) { release_inputs(); reset_view(); }
    mounted=ready;
    if (current_image && !lv_image_get_src(current_image)) lv_obj_invalidate(current_image);
    pack_touchpad_drawer_update(ready);
    caption(ready ? "Stop" : "Waiting for USB  /  Stop");
}

static bool hits_pad(lv_point_t point)
{
    if (!current_pad || !lv_obj_is_visible(current_pad)) return false;
    lv_display_t *d=lv_obj_get_display(current_pad);
    if (portrait && lv_display_get_rotation(d)==LV_DISPLAY_ROTATION_0)
        point=(lv_point_t){point.y,599-point.x};
    return !lv_indev_search_obj(lv_display_get_layer_sys(d),&point) &&
           !lv_indev_search_obj(lv_display_get_layer_top(d),&point) &&
           lv_indev_search_obj(lv_display_get_screen_active(d),&point)==current_pad;
}

bool pack_desktop_contacts(unsigned count,const lv_point_t points[2],const uint8_t ids[2],uint64_t timestamp)
{
    if (pack_touchpad_drawer_active()) {
        ignore_contact=count!=0;
        return false;
    }
    if (ignore_contact) {
        if (!count) ignore_contact=false;
        return false;
    }
    if (!tracking) {
        if (!count) return false;
        if (count>2 || !pack_desktop_ready() || !hits_pad(points[0]) || (count==2 && !hits_pad(points[1]))) {
            ignore_contact=true;
            return false;
        }
        tracking=true;
    } else if (count==2 && previous_count==1) {
        unsigned added=ids[0]==first_id ? 1 : 0;
        if (!hits_pad(points[added])) {
            release_inputs();
            return true;
        }
    }
    if (count==1) first_id=ids[0];
    previous_count=(uint8_t)count;
    kvm_gesture_out_t out;
    kvm_touch_sample_t sample={.valid=count<=2,.point_count=count<=2 ? (uint8_t)count : 0,
                               .timestamp_us=timestamp,.sample_seq=++sequence};
    for (unsigned i=0;i<sample.point_count;i++)
        sample.point[i]=(kvm_touch_point_t){.x_px=points[i].x,.y_px=points[i].y,.track_id=ids[i]};
    kvm_gesture_feed(&gesture,&sample,&out);
    for (unsigned i=0;i<out.n;i++) hid_dispatch_local(&out.ev[i]);
    if (!count) tracking=false;
    return true;
}

void pack_desktop_close(void)
{
    pack_touchpad_drawer_close();
    release_inputs();
    if (running) {
        usb_hid_device_set_monitor_controls(false,false);
        monitor_viewport_stop();
    }
    running=mounted=ignore_contact=native_signal=awaiting_mount=false;
    current_pad=current_image=NULL;
    reset_view();
}
#endif
