// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_radio_view.h"
#include "host_lv_mem.h"
#include "ui_theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static lv_obj_t *parent;
static pack_radio_request_t sent;
static unsigned sends;
static bool accept_dispatch=true;
static bool send_request(const pack_radio_request_t *r,void *ctx)
{
    assert(ctx==&sends && r->id);
    sent=*r;sends++;return accept_dispatch;
}
static lv_obj_t *find(lv_obj_t *root,const char *text)
{
    if(lv_obj_is_hidden(root)) return NULL;
    if(lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text))return root;
    for(uint32_t i=0;i<lv_obj_get_child_count(root);i++){
        lv_obj_t *hit=find(lv_obj_get_child(root,(int)i),text);if(hit)return hit;
    }
    return NULL;
}
static lv_obj_t *control(const char *text)
{
    lv_obj_t *l=find(parent,text);
    if(!l)fprintf(stderr,"Missing control '%s'\n",text);
    assert(l && lv_obj_check_type(lv_obj_get_parent(l),&lv_button_class));
    return lv_obj_get_parent(l);
}
static void tap(const char *text)
{
    lv_obj_t *b=control(text);
    lv_obj_update_layout(parent);lv_area_t a;lv_obj_get_coords(b,&a);
    assert(lv_area_get_width(&a)>=44 && lv_area_get_height(&a)>=44 && lv_obj_is_visible(b));
    lv_test_mouse_move_to((a.x1+a.x2)/2,(a.y1+a.y2)/2);
    lv_test_mouse_press();lv_test_wait(50);lv_test_mouse_release();lv_test_wait(50);
}
static void digits(const char *s){while(*s){char key[2]={*s++,0};tap(key);}}
static void done(void){lv_test_wait(250);pack_radio_view_tick();}
static bool disabled(const char *label){return lv_obj_has_state(control(label),LV_STATE_DISABLED);}
static void show(const char *text){if(!find(parent,text))fprintf(stderr,"Missing '%s'\n",text);assert(find(parent,text));}

int main(void)
{
    lv_init();lv_display_t *d=lv_test_display_create(600,1024);assert(d);
    lv_test_indev_create_all();assert(ui_theme_init());
    parent=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(parent);
    lv_obj_set_pos(parent,8,8);lv_obj_set_size(parent,584,928);lv_obj_set_hidden(parent,true);
    assert(!pack_radio_view_open(parent,539,680));assert(pack_radio_view_open(parent,584,928));
    pack_radio_view_tick();assert(lv_obj_get_child_count(parent)==0);
    lv_obj_set_hidden(parent,false);lv_obj_update_layout(parent);size_t idle=host_lv_live_bytes();
    pack_radio_view_tick();lv_test_wait(100);show("Offline");assert(disabled("Swap") && disabled("OK"));
    tap("Swap");assert(!sends);tap("Start demo");show("Demo");show("118.100 MHz");show("121.900 MHz");
    const char *const readings[]={"118.100 MHz","121.900 MHz","7000","9","OK",LV_SYMBOL_BACKSPACE};
    for (unsigned i=0;i<sizeof(readings)/sizeof(readings[0]);i++) {
        lv_obj_t *label=find(parent,readings[i]);
        assert(label && lv_font_get_line_height(lv_obj_get_style_text_font(label,LV_PART_MAIN))>=36);
        assert(lv_obj_get_style_transform_scale_x(label,LV_PART_MAIN)==LV_SCALE_NONE);
        assert(lv_obj_get_style_transform_scale_y(label,LV_PART_MAIN)==LV_SCALE_NONE);
    }
    tap("STANDBY");digits("118020");show("Invalid COM channel. Use a valid 118-136 MHz channel.");assert(disabled("OK"));
    tap(LV_SYMBOL_BACKSPACE);tap(LV_SYMBOL_BACKSPACE);digits("05");show("118.005 MHz");tap("OK");
    show("Applying local demo change.");show("121.900 MHz");done();show("118.005 MHz");
    tap("Swap");done();show("118.100 MHz");assert(control("118.100 MHz")==control("STANDBY"));
    tap("COM 2");show("123.450 MHz");show("124.850 MHz");tap("STANDBY");digits("136990");tap("OK");done();
    tap("COM 1");show("118.005 MHz");show("118.100 MHz");
    tap("7000");assert(disabled("8") && disabled("9") && disabled("OK"));digits("007");assert(disabled("OK"));
    tap("3");tap("OK");done();show("0073");tap("STBY");done();assert(lv_obj_has_state(control("STBY"),LV_STATE_CHECKED));
    tap("STANDBY");digits("123");lv_obj_set_hidden(parent,true);pack_radio_view_tick();
    lv_obj_set_hidden(parent,false);pack_radio_view_tick();show("118.100 MHz");show("0073");assert(disabled("OK"));
    tap("COM 2");show("136.990 MHz");tap("Go offline");show("Offline");show("----");

    pack_radio_readback_t s={.active_khz={120000,125000},.standby_khz={121000,126000},
                             .squawk=00421,.mode=PACK_RADIO_ON,.mode_mask=3};
    pack_radio_view_bind(send_request,&sends);show("Waiting for radio readback.");assert(disabled("Swap"));
    assert(pack_radio_view_readback(&s,0,false));show("Simulator online");show("0421");assert(disabled("ALT"));
    tap("COM 1");tap("Swap");assert(sends==1 && sent.action==PACK_RADIO_SWAP && sent.com==0);
    assert(disabled("Swap"));tap("Swap");assert(sends==1);
    assert(!pack_radio_view_readback(&s,sent.id+1,false));show("Awaiting simulator readback.");
    assert(pack_radio_view_readback(&s,sent.id,true));show("Simulator rejected the request.");show("120.000 MHz");
    tap("Swap");assert(pack_radio_view_readback(&s,sent.id,false));show("Simulator returned a different value.");
    tap("0421");digits("0123");tap("OK");assert(sent.action==PACK_RADIO_SQUAWK && sent.value==0123);
    show("0421");lv_obj_set_hidden(parent,true);pack_radio_view_tick();
    s.squawk=0123;assert(pack_radio_view_readback(&s,sent.id,false));
    lv_obj_set_hidden(parent,false);pack_radio_view_tick();show("0123");show("Confirmed by simulator.");
    tap("STANDBY");digits("118000");tap("OK");lv_test_wait(3100);pack_radio_view_tick();show("No simulator confirmation.");
    assert(!pack_radio_view_readback(&s,sent.id,false));s.standby_khz[0]=135975;assert(pack_radio_view_readback(&s,0,false));show("135.975 MHz");
    accept_dispatch=false;tap("Swap");show("Request could not be sent.");accept_dispatch=true;
    tap("Swap");uint32_t stale=sent.id;pack_radio_view_bind(NULL,NULL);assert(!pack_radio_view_readback(&s,stale,false));show("Offline");
    pack_radio_view_bind(send_request,&sends);s.standby_khz[1]=127020;
    assert(!pack_radio_view_readback(&s,0,false));show("Invalid simulator readback.");assert(disabled("Swap"));
    s.standby_khz[1]=126000;s.squawk=010000;assert(!pack_radio_view_readback(&s,0,false));
    s.squawk=0;s.mode_mask=1;assert(!pack_radio_view_readback(&s,0,false));s.mode_mask=3;
    assert(pack_radio_view_readback(&s,0,false));show("0000");
    lv_test_wait(170);size_t used=host_lv_live_bytes()-idle;assert(used<48*1024);
    pack_radio_view_close();assert(!lv_obj_get_child_count(parent));
    lv_obj_set_size(parent,540,680);assert(pack_radio_view_open(parent,540,680));pack_radio_view_tick();
    tap("Start demo");tap("STANDBY");digits("117995");assert(disabled("OK"));tap("Cancel");show("121.900 MHz");
    tap("7000");digits("0000");tap("OK");done();show("0000");tap("ON");done();assert(lv_obj_has_state(control("ON"),LV_STATE_CHECKED));
    lv_obj_delete(parent);pack_radio_view_close();lv_test_indev_delete_all();lv_display_delete(d);ui_theme_deinit();lv_deinit();
    printf("PASS portrait radio, two COM banks, octal transponder, draft, gates, correlated/missing/rejected readback, teardown and minimum-size touch. Host widget live: %zu bytes.\n",used);
    return 0;
}
