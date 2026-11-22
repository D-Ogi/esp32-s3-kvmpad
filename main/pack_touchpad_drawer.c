// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_touchpad_drawer.h"
#include "app_config.h"
#include "gesture_engine.h"
#include "pack_desktop_view.h"
#include "hid_dispatch.h"
#include "ui_theme.h"

static lv_obj_t *scrim, *actions[3], *status;

bool pack_touchpad_drawer_active(void) { return scrim!=NULL; }

void pack_touchpad_drawer_close(void)
{
    if (scrim) lv_obj_delete(scrim);
}

static void scrim_event(lv_event_t *e)
{
    if (lv_event_get_target_obj(e)!=scrim) return;
    if (lv_event_get_code(e)==LV_EVENT_DELETE) scrim=NULL;
    else if (lv_event_get_code(e)==LV_EVENT_CLICKED) pack_touchpad_drawer_close();
}

static void close_clicked(lv_event_t *e)
{
    (void)e;
    pack_touchpad_drawer_close();
}

void pack_touchpad_drawer_update(bool ready)
{
    if (!scrim) return;
    for (unsigned i=0;i<3;i++) lv_obj_set_disabled(actions[i],!ready);
    lv_label_set_text_static(status,"Connect the USB monitor to use the mouse buttons.");
    lv_obj_set_hidden(status,ready);
}

static void mouse_clicked(lv_event_t *e)
{
    if (!scrim || !lv_obj_is_visible(scrim) || !pack_desktop_ready()) return;
    kvm_hid_intent_t intent={.type=KVM_HID_MOUSE_BTN_DOWN,
        .data.mouse_button.button_id=(uint8_t)(uintptr_t)lv_event_get_user_data(e)};
    if (hid_dispatch_local(&intent)==ESP_OK) {
        intent.type=KVM_HID_MOUSE_BTN_UP;
        if (hid_dispatch_local(&intent)==ESP_OK) {
            pack_touchpad_drawer_close();
            return;
        }
        hid_dispatch_release_all(KVM_RA_GESTURE_CANCEL);
    }
    lv_label_set_text_static(status,"Mouse input unavailable.");
    lv_obj_set_hidden(status,false);
}

static lv_obj_t *text(lv_obj_t *parent,const char *caption,ui_font_role_t font)
{
    lv_obj_t *label=lv_label_create(parent);
    if (label) {
        lv_label_set_text_static(label,caption);
        lv_obj_set_style_text_font(label,ui_theme_font(font),0);
    }
    return label;
}

static lv_obj_t *button(lv_obj_t *parent,const char *caption,int width,lv_event_cb_t cb,uintptr_t value)
{
    lv_obj_t *b=lv_button_create(parent);
    if (!b) return NULL;
    ui_theme_control_button(b);
    lv_obj_set_size(b,width,48);
    lv_obj_t *label=text(b,caption,UI_FONT_BODY);
    if (!label || !lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,(void *)value)) return NULL;
    lv_obj_center(label);
    return b;
}

static void slide(void *panel,int32_t x) { lv_obj_set_x(panel,x); }

void pack_touchpad_drawer_open(lv_obj_t *content)
{
    if (scrim) return;
    scrim=lv_obj_create(content);
    if (!scrim) return;
    if (!lv_obj_add_event_cb(scrim,scrim_event,LV_EVENT_ALL,NULL)) {
        lv_obj_delete(scrim); scrim=NULL; return;
    }
    lv_obj_remove_style_all(scrim);
    ui_theme_apply(scrim,UI_STYLE_SCRIM,0);
    lv_obj_set_pos(scrim,0,0);
    lv_obj_set_size(scrim,lv_pct(100),lv_pct(100));
    lv_obj_set_scrollable(scrim,false);
    lv_obj_set_clickable(scrim,true);
    lv_obj_t *panel=lv_obj_create(scrim);
    if (!panel) goto failed;
    lv_obj_remove_style_all(panel);
    ui_theme_apply(panel,UI_STYLE_CONTROL_PAGE,0);
    ui_theme_apply(panel,UI_STYLE_DRAWER_SHELL,0);
    lv_obj_set_size(panel,LV_MIN(360,lv_obj_get_content_width(content)),lv_pct(100));
    lv_obj_set_style_pad_all(panel,16,0);
    lv_obj_set_style_pad_row(panel,12,0);
    lv_obj_set_flex_flow(panel,LV_FLEX_FLOW_COLUMN);
    lv_obj_t *header=lv_obj_create(panel);
    if (!header) goto failed;
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header,lv_pct(100),48);
    lv_obj_set_scrollable(header,false);
    lv_obj_t *title=text(header,"Touchpad gestures",UI_FONT_HEADING);
    lv_obj_t *close=button(header,"Close",72,close_clicked,0);
    if (!title || !close) goto failed;
    lv_obj_align(title,LV_ALIGN_LEFT_MID,0,0);
    lv_obj_align(close,LV_ALIGN_RIGHT_MID,0,0);
    kvm_gesture_cfg_t prefs = KVM_GESTURE_CFG_DEFAULT;
    app_config_apply_mouse(&prefs);
    const char *const gestures[]={
        "Slide to move the pointer.", "Tap to click. Tap with two fingers to right-click.",
        prefs.scroll_enabled ? "Slide two fingers to scroll." : "Two-finger scrolling is off in Settings.",
        "Tap twice and drag on the second touch. Lift to drop."
    };
    for (unsigned i=0;i<4;i++) {
        lv_obj_t *line=text(panel,gestures[i],UI_FONT_BODY);
        if (!line) goto failed;
        lv_obj_set_width(line,lv_pct(100));
    }
    static const char *const names[]={"Left click","Right click","Middle click"};
    static const uint8_t ids[]={KVM_MOUSE_BTN_LEFT,KVM_MOUSE_BTN_RIGHT,KVM_MOUSE_BTN_MIDDLE};
    for (unsigned i=0;i<3;i++) {
        actions[i]=button(panel,names[i],lv_pct(100),mouse_clicked,ids[i]);
        if (!actions[i]) goto failed;
        ui_theme_apply(actions[i],UI_STYLE_STATE_DISABLED,LV_STATE_DISABLED);
    }
    status=text(panel,"",UI_FONT_LABEL);
    if (!status) goto failed;
    lv_obj_set_width(status,lv_pct(100));
    pack_touchpad_drawer_update(pack_desktop_ready());
    lv_obj_update_layout(content);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a,panel);
    lv_anim_set_exec_cb(&a,slide);
    int w=lv_obj_get_width(scrim);
    lv_anim_set_values(&a,w,w-lv_obj_get_width(panel));
    lv_anim_set_duration(&a,150);
    lv_anim_set_path_cb(&a,lv_anim_path_ease_out);
    if (!lv_anim_start(&a)) slide(panel,w-lv_obj_get_width(panel));
    return;
failed:
    pack_touchpad_drawer_close();
}
