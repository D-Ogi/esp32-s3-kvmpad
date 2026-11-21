// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_autopilot_view.h"
#include "ui_theme.h"
#include "host_lv_mem.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *parent;
static lv_obj_t *match(lv_obj_t *root,const char *caption,bool button)
{
    if (lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),caption) &&
        (!button || lv_obj_check_type(lv_obj_get_parent(root),&lv_button_class)))
        return button ? lv_obj_get_parent(root) : root;
    for (uint32_t i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *hit=match(lv_obj_get_child(root,(int32_t)i),caption,button);
        if (hit) return hit;
    }
    return NULL;
}
static void tap(const char *caption)
{
    lv_obj_t *b=match(parent,caption,true);
    if (!b) fprintf(stderr,"Missing button: %s\n",caption);
    assert(b);
    lv_obj_update_layout(parent);
    lv_area_t a;lv_obj_get_coords(b,&a);
    assert(lv_area_get_height(&a)>=44 && lv_area_get_width(&a)>=44);
    lv_test_mouse_move_to((a.x1+a.x2)/2,(a.y1+a.y2)/2);
    lv_test_mouse_press();lv_test_wait(50);lv_test_mouse_release();lv_test_wait(50);
}
static void shows(const char *caption)
{
    if (!match(parent,caption,false)) fprintf(stderr,"Missing label: %s\n",caption);
    assert(match(parent,caption,false));
}
static void acknowledge(void) {lv_test_wait(950);pack_autopilot_view_tick();}
static void fma(const char *row,const char *lateral,const char *vertical)
{
    lv_obj_t *legend=match(parent,row,false);
    assert(legend);lv_obj_update_layout(parent);
    lv_area_t a;lv_obj_get_coords(legend,&a);
    const int y=(a.y1+a.y2)/2;
    lv_obj_t *body=lv_obj_get_parent(legend);
    const char *cell[2]={NULL,NULL};
    unsigned n=0;
    for(uint32_t i=0;i<lv_obj_get_child_count(body);i++) {
        lv_obj_t *l=lv_obj_get_child(body,(int32_t)i);
        if (!lv_obj_check_type(l,&lv_label_class)) continue;
        lv_area_t b;lv_obj_get_coords(l,&b);
        if (b.x1>a.x2 && b.y1<=y && b.y2>=y) {
            assert(n<2);cell[n++]=lv_label_get_text(l);
        }
    }
    assert(n==2 && !strcmp(cell[0],lateral) && !strcmp(cell[1],vertical));
}
static bool disabled(const char *caption)
{
    lv_obj_t *obj=match(parent,caption,true);assert(obj);
    return lv_obj_has_state(obj,LV_STATE_DISABLED);
}
static void count_deletion(lv_event_t *e)
{
    unsigned *count=lv_event_get_user_data(e);
    ++*count;
}
int main(void)
{
    lv_init();
    lv_display_t *display=lv_test_display_create(600,1024);assert(display);
    lv_test_indev_create_all();assert(ui_theme_init());
    parent=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(parent);lv_obj_set_size(parent,584,864);
    lv_obj_set_pos(parent,8,68);lv_obj_set_hidden(parent,true);
    assert(pack_autopilot_view_open(parent,584,864));
    assert(!pack_autopilot_view_open(parent,584,864));
    pack_autopilot_view_tick();assert(!lv_obj_get_child_count(parent));
    lv_obj_set_hidden(parent,false);lv_obj_update_layout(parent);
    size_t idle=host_lv_live_bytes();
    pack_autopilot_view_tick();lv_test_wait(50);
    size_t offline=host_lv_live_bytes();
    shows("Autopilot  /  Offline");assert(disabled("AP --") && disabled("HDG   --"));
    fma("Active","--","--");fma("Armed","Unavailable","Unavailable");
    tap("ALT   --");assert(!match(parent,"Set ALT",false));
    assert(!match(parent,"275 deg",false));
    tap("Start demo");shows("HDG   275 deg");shows("ALT   6000 ft");shows("VS   +800 ft/min");
    fma("Active","HDG","VS");fma("Armed","--","ALT");
    size_t demo=host_lv_live_bytes();
    tap("HDG   275 deg");tap("+10");shows("285 deg");tap("Cancel");shows("HDG   275 deg");
    tap("HDG   275 deg");for(int i=0;i<9;i++)tap("+10");shows("005 deg");tap("-1");shows("004 deg");
    tap("Apply");assert(disabled("HDG   275 deg") && disabled("AP on"));tap("NAV");
    shows("Awaiting demo confirmation: HDG 004 deg");pack_autopilot_view_tick();shows("HDG   275 deg");
    acknowledge();shows("HDG   004 deg");shows("Demo confirmed: HDG 004 deg");
    tap("ALT   6000 ft");tap("+1000");tap("-100");tap("Apply");acknowledge();shows("ALT   6900 ft");
    tap("VS   +800 ft/min");tap("-500");tap("-100");tap("Apply");acknowledge();shows("VS   +200 ft/min");
    tap("Demo controls");tap("Reject next request: off");tap("Back");tap("AP on");acknowledge();
    shows("Demo rejected: AP off");shows("AP on");
    tap("NAV");fma("Armed","--","ALT");acknowledge();fma("Armed","NAV","ALT");
    tap("Demo controls");assert(!disabled("Capture NAV"));tap("Capture NAV");shows("Demo captured NAV.");
    fma("Active","NAV","VS");fma("Armed","--","ALT");
    tap("Demo controls");tap("Capture altitude");shows("Actual  6900 ft");shows("Actual  +0 ft/min");
    fma("Active","NAV","ALT");fma("Armed","--","--");
    tap("HDG");acknowledge();tap("VS");acknowledge();tap("Demo controls");tap("Advance 30 seconds");
    shows("Actual  7000 ft");tap("ALT");acknowledge();
    tap("AP on");acknowledge();shows("AP off");assert(disabled("NAV"));
    tap("AP off");acknowledge();shows("ROL");shows("PIT");
    tap("HDG   004 deg");tap("+1");tap("Apply");tap("Go offline");acknowledge();
    assert(!match(parent,"HDG   005 deg",false));shows("Autopilot  /  Offline");
    tap("Start demo");tap("ALT   6000 ft");tap("+1000");tap("Apply");
    lv_obj_set_hidden(parent,true);pack_autopilot_view_tick();assert(!lv_obj_get_child_count(parent));
    acknowledge();lv_obj_set_hidden(parent,false);pack_autopilot_view_tick();
    shows("ALT   6000 ft");shows("Demo request cancelled.");
    tap("HDG   275 deg");tap("+1");tap("Enter number");assert(disabled("Return") && disabled("+/-"));
    size_t keypad=host_lv_live_bytes();
    tap("3");tap("6");tap("1");assert(disabled("Return"));shows("Value outside the allowed range.");
    tap("Backspace");tap("0");tap("Return");shows("000 deg");tap("Apply");acknowledge();shows("HDG   000 deg");
    tap("VS   +800 ft/min");tap("-100");tap("Enter number");tap("+/-");assert(disabled("Return"));
    tap("6");tap("0");tap("0");tap("1");assert(disabled("Return"));
    tap("Backspace");tap("0");tap("Cancel");shows("+700 ft/min");
    tap("Enter number");tap("6");tap("0");tap("0");tap("0");tap("+/-");tap("Return");
    shows("-6000 ft/min");tap("Apply");acknowledge();shows("VS   -6000 ft/min");
    // Neither axis reports an armed mode. With reporting disabled, the same model supplies no data.
    fma("Armed","--","--");
    tap("Demo controls");tap("Report armed modes: on");tap("Response delay: 900 ms");tap("Back");
    fma("Armed","Not reported","Not reported");fma("Active","HDG","VS");
    tap("ALT   6000 ft");tap("Enter number");tap("4");tap("5");tap("0");tap("0");tap("1");
    assert(disabled("Return"));tap("Backspace");tap("0");tap("Return");shows("45000 ft");tap("Apply");
    lv_test_wait(1000);pack_autopilot_view_tick();shows("ALT   6000 ft");assert(disabled("AP on"));
    tap("NAV");shows("Awaiting demo confirmation: ALT 45000 ft");
    lv_test_wait(1500);pack_autopilot_view_tick();shows("ALT   45000 ft");shows("Demo confirmed: ALT 45000 ft");
    tap("Demo controls");tap("Report armed modes: off");tap("Response delay: 2500 ms");tap("Back");
    fma("Armed","--","--");
    tap("HDG   000 deg");tap("+10");tap("Enter number");tap("2");
    lv_obj_set_hidden(parent,true);pack_autopilot_view_tick();assert(!lv_obj_get_child_count(parent));
    lv_obj_set_hidden(parent,false);pack_autopilot_view_tick();
    shows("HDG   000 deg");assert(!match(parent,"Return",true));
    unsigned deleted=0;
    lv_obj_t *retained_button=match(parent,"HDG   000 deg",true);
    assert(lv_obj_add_event_cb(retained_button,count_deletion,LV_EVENT_DELETE,&deleted));
    lv_test_wait(170);
    lv_obj_set_hidden(parent,true);pack_autopilot_view_tick();size_t hidden=host_lv_live_bytes();
    for(int i=0;i<25;i++) {
        lv_obj_set_hidden(parent,false);pack_autopilot_view_tick();
        assert(deleted==0 && match(parent,"HDG   000 deg",true)==retained_button);
        shows("ALT   45000 ft");
        lv_obj_set_hidden(parent,true);pack_autopilot_view_tick();
    }
    assert(hidden==host_lv_live_bytes());
    assert(offline<idle+48*1024 && demo<idle+48*1024 && keypad<idle+48*1024 && hidden<idle+48*1024);
    printf("LVGL host live increments: offline=%zu B, demo=%zu B, numeric keypad=%zu B, retained demo=%zu B.\n",
           offline-idle,demo-idle,keypad-idle,hidden-idle);
    pack_autopilot_view_close();assert(deleted==1 && !lv_obj_get_child_count(parent));
    assert(pack_autopilot_view_open(parent,584,864));
    pack_autopilot_view_tick();assert(!lv_obj_get_child_count(parent));
    lv_obj_set_hidden(parent,false);pack_autopilot_view_tick();
    shows("Start demo");assert(!match(parent,"ALT   45000 ft",true));
    lv_obj_delete(parent);pack_autopilot_view_close();pack_autopilot_view_tick();
    lv_test_indev_delete_all();lv_display_delete(display);ui_theme_deinit();lv_deinit();
    puts("PASS Offline, Demo, editors, cancel, one request, delayed acceptance, rejection, modes, capture, 25 retained returns, close and fresh reopen.");
    puts("PASS Numeric HDG 360 to 000, signed VS, ALT bounds, Return/Cancel/Backspace, missing and empty armed modes, 900/2500 ms confirmation.");
    return 0;
}
