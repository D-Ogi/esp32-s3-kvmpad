// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "esp_heap_caps.h"
#include "hid_dispatch.h"
#include "host_lv_mem.h"
#include "hpe_panel.h"
#include "hpe_access_config.h"
#include "kvmbridge.h"
#include "pack_chart_view.h"
#include "pack_view.h"
#include "sim_image.h"
#include "sim_platform.h"
#include "ui_theme.h"
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "app_config.h"
#include "gesture_engine.h"
#include "console_damage.h"
#include "pack_desktop_view.h"
#include "pack_keyboard_surface.h"
#include "monitor_present.h"
#include "monitor_viewport.h"
#include "usb_hid_device.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static kvm_hid_snapshot_t snap = {.host_usb=KVM_HOST_USB_MOUNTED};
static kvm_hid_intent_t recorded[128];
static unsigned reports, subscriptions, stops, starts, keys;
static size_t control_bytes;
static kvmbridge_quality_t quality = KVB_VALID;
static pack_scene_t scene;
static char data[65536];
static kvm_json_tok_t tokens[8192];
static lv_obj_t *home_screen;

#if CONFIG_KVM_USB_MONITOR_EVAL
static unsigned monitor_opens,monitor_closes,monitor_flushes,usb_changes;
static bool usb_controls,usb_mounted,picture_waiting,memory_available=true;
static bool receiver_native,next_nonblack=true;
static bool scroll_enabled=true, scroll_natural=false;
static kvm_gesture_speed_t pointer_speed=KVM_GEST_SPEED_NORMAL;
static bool hold_right=true;
void app_config_apply_mouse(kvm_gesture_cfg_t *gesture)
{
    gesture->scroll_enabled=scroll_enabled;
    gesture->scroll_natural=scroll_natural;
    gesture->speed=pointer_speed;
    gesture->longpress_right_click=hold_right;
}
static bool led_known;
static uint8_t host_leds;
static unsigned mount_epoch, failed_allocations, failed_profiles;
static uint16_t monitor_pixels[1024*600];
static const uint16_t *next_pixels=monitor_pixels;
static const monitor_damage_t *next_damage;
static unsigned next_damage_count;
bool monitor_start(bool native)
{
    if (failed_allocations) { failed_allocations--;return false; }
    if (!memory_available) return false;
    receiver_native=native;monitor_opens++;return true;
}
void monitor_stop(void) { monitor_closes++; }
void monitor_refresh_complete(void) { monitor_flushes++; }
void monitor_present(void (*select)(const uint16_t *,bool,const monitor_damage_t *,unsigned))
{
    if (picture_waiting) { picture_waiting=false;select(next_pixels,next_nonblack,next_damage,next_damage_count); }
}
static void offer_picture(const uint16_t *pixels,const monitor_damage_t *damage,unsigned count)
{
    assert(!picture_waiting);
    next_pixels=pixels;next_damage=damage;next_damage_count=count;picture_waiting=true;
    // Give the visible viewport one presentation period before the fixture forces a refresh.
    for (unsigned i=0;i<8;i++) { lv_tick_inc(1);lv_timer_handler(); }
    assert(!picture_waiting);
    lv_test_wait(92);
    next_damage=NULL;
}
esp_err_t usb_hid_device_set_monitor_controls(bool enable,bool native)
{
    assert(!enable || native==receiver_native);
    if (failed_profiles) { failed_profiles--; return -1; }
    if (usb_controls!=enable) usb_mounted=false;
    usb_changes++;usb_controls=enable;return ESP_OK;
}
bool usb_hid_device_monitor_mounted(void) { return usb_mounted; }
void usb_hid_device_get_status(usb_hid_status_t *out)
{
    *out=(usb_hid_status_t){.usb_epoch=mount_epoch,.led_known=led_known,.led_state=host_leds};
}
#endif

// The test captures typed intents before the arbiter. All time and provider readings are synthetic.
static uint64_t timing_word;
static unsigned saves;
static uint8_t last_key;
bool app_config_access_read(hpe_access_timing_t *out) { *out=hpe_access_unpack(timing_word); return true; }
bool app_config_access_save(hpe_access_timing_t v)
{
    if (!hpe_access_valid(v)) return false;
    timing_word=hpe_access_pack(v);
    saves++;
    return true;
}
static hpe_studio_status_t studio;
static uint8_t profile;
void hid_dispatch_set_profile(uint8_t active) { profile=active; }
void hid_dispatch_studio_status(hpe_studio_status_t *out) { *out=studio; }
void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *out, uint32_t *seq)
{
    *out=snap;
    if (seq) *seq=0;
}
void hid_dispatch_release_all(kvm_release_reason_t reason)
{
    (void)reason;
    snap.release_generation++;
}
esp_err_t hid_dispatch_local(const kvm_hid_intent_t *intent)
{
    assert(reports<128);
    recorded[reports++]=*intent;
    return ESP_OK;
}
esp_err_t hid_dispatch_burst_start(const kvm_hid_burst_step_t *steps, uint16_t n,
                                   const kvm_hid_burst_step_t *cancel, uint8_t nc)
{
    (void)cancel;
    assert(!nc && n>=2 && steps[0].down && !steps[n-1].down);
    keys++;
    last_key=steps[0].usage;
    return ESP_OK;
}
void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    assert(!control_bytes && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    control_bytes=count*size;
    return calloc(count,size);
}
void heap_caps_free(void *p) { control_bytes=0; free(p); }
static const char *started_mode;
static unsigned system_exits;
void pack_panel_leave(void) { system_exits++; lv_screen_load(home_screen); }
void pack_panel_magnify(const char *mode) { started_mode=mode; starts++; }
void kvmbridge_stop(void) { stops++; }
static bool use_test_sample;
static bool joint_session;
bool kvmbridge_transfer_telemetry(const char *p) { (void)p; return joint_session; }
void kvmbridge_stop_display(const char *p,uint32_t epoch) { assert(p && !epoch); }
bool kvmbridge_instruments_for_profile(const char *id, bool xplane, unsigned channel_mask, bool composite)
{
    joint_session=composite;
    assert(!strcmp(id,use_test_sample ? "net.example.instrument.single" : "org.kvmpad.flight.instruments"));
    assert(channel_mask==(use_test_sample ? 1u : 63u));
    subscriptions++;
    return !xplane;
}
static kvmbridge_sample_t test_sample;
bool kvmbridge_sample(unsigned channel, kvmbridge_sample_t *out)
{
    if (use_test_sample) {
        *out=test_sample;
        return out->quality!=KVB_UNAVAILABLE;
    }
    const double values[]={115,12000,258,-80,4,13};
    assert(channel<6);
    *out=(kvmbridge_sample_t){values[channel],quality==KVB_VALID ? 5u : 2000u,quality};
    return true;
}

static void load_at(const char *folder)
{
    const char *const files[]={"manifest","ui","bindings"};
    for (unsigned i=0;i<3;i++) {
        char path[768];
        snprintf(path,sizeof(path),"%s/%s.json",folder,files[i]);
        FILE *fp=fopen(path,"rb");
        assert(fp);
        size_t len=fread(data,1,sizeof(data),fp);
        fclose(fp);
        kvm_json_doc_t doc;
        assert(kvm_json_parse_local(&doc,data,len,tokens,8192,24)==KVM_JSON_OK);
        assert(i==0 ? pack_scene_manifest(&scene,&doc) : i==1 ? pack_scene_ui(&scene,&doc) : pack_scene_bindings(&scene,&doc));
    }
}

static void home(void) { lv_screen_load(home_screen); }
static void picture(const char *name)
{
    char path[768];
    snprintf(path,sizeof(path),"%s/%s.png",KVM_SCREENSHOTS,name);
    sim_image_t img;
    assert(sim_image_render(lv_screen_active(),&img));
    assert(img.w==1024 && img.h==600);
    assert(sim_image_write_png(path,&img));
    sim_image_free(&img);
}

static void tap(int x,int y)
{
    sim_press_point(x,y);
    sim_release();
}

static bool shows(lv_obj_t *root, const char *text);

static unsigned instrument_pixels(const sim_image_t *img, lv_area_t area, bool accent, bool day)
{
    unsigned count=0;
    for (int y=area.y1; y<=area.y2; y++) {
        for (int x=area.x1; x<=area.x2; x++) {
            const uint8_t *rgb=&img->rgb[((size_t)y*img->w+x)*3];
            bool dark_ink=LV_MAX3(rgb[0],rgb[1],rgb[2])<64;
            bool light_ink=LV_MIN3(rgb[0],rgb[1],rgb[2])>190;
            bool active_mark=rgb[2]>130 && rgb[2]-rgb[0]>32;
            if (accent ? active_mark : (day ? dark_ink : light_ink)) count++;
        }
    }
    return count;
}

static void instrument_palettes(lv_obj_t *content)
{
    const char *const names[]={"day","night","high-contrast"};
    lv_obj_t *dashboard=lv_obj_get_child(content,0);
    for (unsigned p=0; p<UI_PALETTE_COUNT; p++) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        sim_settle();
        sim_image_t img;
        assert(sim_image_render(lv_screen_active(),&img));
        char path[768];
        snprintf(path,sizeof(path),"%s/flight-instruments-%s.png",KVM_SCREENSHOTS,names[p]);
        assert(sim_image_write_png(path,&img));
        bool day=p==UI_PALETTE_DAY;
        for (unsigned j=0; j<6; j++) {
            lv_obj_t *card=lv_obj_get_child(dashboard,j);
            lv_area_t area, label;
            lv_obj_get_coords(card,&area);
            lv_obj_get_coords(lv_obj_get_child(card,0),&label);
            const uint8_t *fill=&img.rgb[((size_t)(area.y1+24)*img.w+area.x1+24)*3];
            if (day) assert(LV_MIN3(fill[0],fill[1],fill[2])>220);
            if (p==UI_PALETTE_NIGHT) assert(LV_MAX3(fill[0],fill[1],fill[2])<80);
            if (p==UI_PALETTE_HIGH_CONTRAST) assert(!(fill[0]|fill[1]|fill[2]));
            assert(instrument_pixels(&img,label,false,day)>30);
        }
        // Each region isolates the active IAS arc, horizon line or altitude needle for the fixture's readings.
        const lv_area_t marks[]={{25,140,100,300},{370,165,650,250},{790,145,858,198}};
        for (unsigned i=0; i<3; i++)
            assert(instrument_pixels(&img,marks[i],true,day)>60);
        sim_image_free(&img);
    }
    ui_theme_set_control_palette(UI_PALETTE_DAY);
}

static void pack(unsigned i, const char *name)
{
    char folder[768];
    snprintf(folder,sizeof(folder),"%s/%s",KVM_EXAMPLES,name);
    load_at(folder);
    lv_obj_t *scr=lv_obj_create(NULL), *content=lv_obj_create(scr);
    if (i==1) {
        assert(ui_theme_init());
        ui_theme_apply(scr,UI_STYLE_CONTROL_PAGE,LV_STATE_DEFAULT);
    }
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content,0,32);
    lv_obj_set_size(content,1024,568);
    unsigned limit=scene.max_memory_kib;
    scene.max_memory_kib=16;
    assert(!pack_view_open(content,&scene,scene.permissions));
    scene.max_memory_kib=limit;
    assert(!pack_view_open(content,&scene,0));
    size_t before=host_lv_live_bytes();
    assert(pack_view_open(content,&scene,scene.permissions));
    pack_view_tick();
    assert(subscriptions==(i>1));
    lv_screen_load(scr);
    sim_settle();
    pack_view_tick();
    sim_settle();
    if (i==0) {
        unsigned n=reports;
        tap(615,250);
        assert(reports>=n+2 && recorded[n].data.joystick.x>10000 && !recorded[reports-1].data.joystick.x);
        n=reports;
        tap(700,360);
        assert(reports>=n+2 && recorded[n].data.joystick.buttons==1 && !recorded[reports-1].data.joystick.buttons);
    } else if (i==1) {
        assert(subscriptions==1);
        quality=KVB_STALE;
        lv_test_wait(70); pack_view_tick();
        picture("instruments-stale");
        quality=KVB_ERROR;
        lv_test_wait(70); pack_view_tick();
        assert(shows(content,"IAS\nData error") && shows(content,"Artificial Horizon\nData error") &&
               shows(content,"Altitude\nData error"));
        picture("instruments-error");
        quality=KVB_UNAVAILABLE;
        lv_test_wait(70); pack_view_tick();
        assert(shows(content,"IAS\nUnavailable") && !shows(content,"IAS\nData error"));
        quality=KVB_VALID;
        lv_test_wait(70); pack_view_tick();
        instrument_palettes(content);
    } else if (i==2) {
        assert(!starts);
        tap(655,320);
        assert(starts==1 && !strcmp(started_mode,"OVERVIEW"));
    } else {
        const pack_action_t *a=&scene.action[0];
        const pack_widget_t *w=&scene.widget[a->widget];
        tap(w->x+w->w/2,32+w->y+w->h/2);
        assert(!strcmp(scene.state[a->state].value.text,a->value.text));
    }
    size_t used=sizeof(scene)+control_bytes+host_lv_live_bytes()-before;
    printf("%s: %zu resident bytes of %u\n",name,used,limit*1024);
    assert(used<=limit*1024);
    picture(name);
    pack_view_close();
    assert(control_bytes==0);
    lv_obj_invalidate(scr);
    lv_refr_now(lv_display_get_default());
    lv_screen_load(home_screen);
    lv_obj_delete(scr);
    if (i==1) ui_theme_deinit();
}

static bool shows(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root,&lv_label_class) && !strcmp(lv_label_get_text(root),text)) return true;
    for (unsigned i=0; i<lv_obj_get_child_count(root); i++)
        if (shows(lv_obj_get_child(root,(int)i),text)) return true;
    return false;
}

static void magnifier_modes(const char *folder, char **expected)
{
    load_at(folder);
    lv_obj_t *screen=lv_obj_create(NULL), *content=lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content,1024,568);
    lv_obj_set_pos(content,0,32);
    assert(!pack_view_open(content,&scene,PACK_PERM_UI));
    assert(pack_view_open(content,&scene,scene.permissions));
    lv_screen_load(screen); sim_settle();
    static const int select_x[]={0,910,770,620};
    for (unsigned i=0;i<4;i++) {
        if (i) tap(select_x[i],110);
        unsigned before=starts;
        tap(650,330);
        if (!strcmp(expected[i],"invalid")) {
            assert(starts==before && shows(content,"Unknown capture mode.\nSelect Overview, 1:1 or 2x."));
        } else assert(starts==before+1 && !strcmp(started_mode,expected[i]));
    }
    assert(!reports && !subscriptions && !stops && lv_screen_active()==screen);
    home(); pack_view_close(); lv_obj_delete(screen);
}

static void chip_palettes(const char *folder)
{
    static const char *const symbols[]={LV_SYMBOL_MINUS,LV_SYMBOL_SETTINGS,LV_SYMBOL_EYE_OPEN,"i",
                                        LV_SYMBOL_OK,LV_SYMBOL_WARNING,LV_SYMBOL_CLOSE,LV_SYMBOL_MINUS};
    static const char *const pictures[]={"chips-day","chips-night","chips-high-contrast"};
    load_at(folder);
    assert(ui_theme_init());
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    ui_theme_apply(screen,UI_STYLE_CONTROL_PAGE,LV_STATE_DEFAULT);
    assert(pack_view_open(screen,&scene,PACK_PERM_UI));
    lv_screen_load(screen);
    lv_obj_t *root=lv_obj_get_child(screen,0);
    assert(lv_obj_get_child_count(root)==8);
    for (unsigned p=0;p<UI_PALETTE_COUNT;p++) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        sim_settle();
        sim_image_t img;
        assert(sim_image_render(screen,&img));
        uint32_t masks[8];
        for (unsigned i=0;i<8;i++) {
            lv_obj_t *chip=lv_obj_get_child(root,i);
            lv_obj_t *label=lv_obj_get_child(chip,0), *marker=lv_obj_get_child(chip,1);
            assert(!strcmp(lv_label_get_text(marker),symbols[i]));
            lv_area_t mark, text, area;
            lv_obj_get_coords(marker,&mark);
            lv_obj_get_coords(label,&text);
            lv_obj_get_coords(chip,&area);
            assert(mark.x2<text.x1 && instrument_pixels(&img,text,false,p==UI_PALETTE_DAY)>10);
            const uint8_t *fill=&img.rgb[((size_t)(area.y1+6)*img.w+area.x1+50)*3];
            if (p==UI_PALETTE_DAY) assert(LV_MIN3(fill[0],fill[1],fill[2])>220);
            else if (p==UI_PALETTE_NIGHT) assert(LV_MAX3(fill[0],fill[1],fill[2])<80);
            else assert(!(fill[0]|fill[1]|fill[2]));
            uint32_t shape=2166136261u;
            unsigned painted=0;
            for (int y=mark.y1;y<=mark.y2;y++) for (int x=mark.x1;x<=mark.x2;x++) {
                const uint8_t *rgb=&img.rgb[((size_t)y*img.w+x)*3];
                bool ink=abs(rgb[0]-fill[0])+abs(rgb[1]-fill[1])+abs(rgb[2]-fill[2])>120;
                shape=(shape^(unsigned)ink)*16777619u;
                painted+=ink;
            }
            assert(painted>=5);
            masks[i]=shape;
            if (i==7) assert(shape==masks[0]);
            else for (unsigned j=0;j<i;j++) assert(shape!=masks[j]);
        }
        sim_image_free(&img);
        picture(pictures[p]);
    }
    home(); pack_view_close(); lv_obj_delete(screen);
    ui_theme_set_control_palette(UI_PALETTE_DAY); ui_theme_deinit();
    puts("PASS seven visible status marker shapes and default neutral in day, night and high contrast");
}

static void local_controls_view(const char *folder)
{
    load_at(folder);
    lv_obj_t *screen=lv_obj_create(NULL), *content=lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content,0,32);
    lv_obj_set_size(content,1024,568);
    size_t before=host_lv_live_bytes();
    assert(pack_view_open(content,&scene,PACK_PERM_UI));
    lv_screen_load(screen);
    sim_settle();
    lv_obj_t *root=lv_obj_get_child(content,0), *chip=lv_obj_get_child(root,3);
    lv_obj_t *first=lv_obj_get_child(root,4), *second=lv_obj_get_child(root,5);
    assert(!lv_obj_is_hidden(first) && lv_obj_is_hidden(second));
    assert(lv_obj_get_style_radius(chip,0)==LV_RADIUS_CIRCLE && lv_obj_get_child_count(chip)==2);
    tap(42,64);
    assert(shows(chip,"1"));
    sim_press_point(42,64);
    lv_test_wait(650);
    assert(shows(chip,"2"));
    sim_release();
    assert(shows(chip,"2"));
    sim_press_point(42,64);
    lv_test_mouse_move_to(100,70);
    lv_test_wait(100);
    assert(shows(chip,"90"));
    sim_release();
    assert(shows(chip,"90"));
    sim_press_point(42,64);
    lv_test_mouse_move_to(480,200);
    lv_test_wait(70);
    sim_release();
    assert(shows(chip,"90"));
    lv_obj_t *dial=lv_obj_get_child(root,1), *arc=lv_obj_get_child(dial,1);
    assert(lv_obj_check_type(arc,&lv_arc_class) && lv_arc_get_value(arc)==0);
    lv_area_t rect;
    lv_obj_get_coords(arc,&rect);
    int x=(rect.x1+rect.x2)/2,y=(rect.y1+rect.y2)/2,r=(rect.x2-rect.x1)/2-5;
    sim_press_point(x,y-r);
    lv_test_wait(100);
    lv_test_mouse_move_to(x+r,y);
    lv_test_wait(100);
    sim_release();
    int value=lv_arc_get_value(arc);
    assert(value==66 && scene.state[0].value.integer==value);
    tap(155,270);
    assert(lv_obj_is_hidden(first) && !lv_obj_is_hidden(second) && shows(chip,"1"));
    tap(45,270);
    assert(!lv_obj_is_hidden(first) && lv_obj_is_hidden(second) && shows(chip,"0"));
    assert(!reports && !subscriptions && !starts && !stops);
    size_t used=sizeof(scene)+control_bytes+host_lv_live_bytes()-before;
    assert(used<=scene.max_memory_kib*1024u);
    printf("Local controls: %u scene nodes, %zu resident bytes, real pointer tap/hold/drag/cancel/rotate/tabs\n",scene.widgets,used);
    picture("local-controls");
    home(); sim_settle(); pack_view_tick(); pack_view_close(); lv_obj_delete(screen);
    assert(!control_bytes && lv_screen_active()==home_screen);
}

static bool between(lv_obj_t *obj, double low, double high)
{
    if (lv_obj_check_type(obj,&lv_label_class)) {
        const char *label=lv_label_get_text(obj);
        char *end;
        double value=strtod(label,&end);
        if (end!=label && !*end && value>low && value<high) return true;
    }
    for (unsigned i=0;i<lv_obj_get_child_count(obj);i++)
        if (between(lv_obj_get_child(obj,(int)i),low,high)) return true;
    return false;
}

static void telemetry_view(const char *folder)
{
    load_at(folder);
    use_test_sample=true;
    lv_obj_t *screen=lv_obj_create(NULL), *content=lv_obj_create(screen);
    lv_obj_set_size(content,1024,568);
    test_sample=(kvmbridge_sample_t){.value=0,.quality=KVB_VALID};
    assert(pack_view_open(content,&scene,scene.permissions));
    assert(shows(content,"Unavailable"));
    lv_screen_load(screen);
    sim_settle(); pack_view_tick();
    assert(subscriptions==1 && shows(content,"0") && shows(content,"false"));
    test_sample.value=3;
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"true") && between(content,0,3));
    test_sample.age_ms=101; // still valid at the provider, stale at this binding's 100 ms deadline
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"Stale") && !shows(content,"true") && !between(content,0,3));
    test_sample=(kvmbridge_sample_t){.value=1.5,.quality=KVB_VALID};
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"1.5") && shows(content,"false"));
    test_sample.quality=KVB_ERROR;
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"Data error") && !shows(content,"1.5"));
    test_sample.quality=KVB_UNAVAILABLE;
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"Unavailable") && !shows(content,"1.5"));
    test_sample.value=13; test_sample.quality=KVB_VALID;
    lv_test_wait(60); pack_view_tick();
    assert(shows(content,"13") && shows(content,"true"));
    pack_view_close();
    lv_screen_load(home_screen);
    lv_obj_delete(screen);
    screen=lv_obj_create(NULL); content=lv_obj_create(screen);
    lv_obj_set_size(content,1024,568);
    test_sample.value=1.5;
    assert(pack_view_open(content,&scene,scene.permissions));
    lv_screen_load(screen);
    sim_settle(); pack_view_tick();
    assert(shows(content,"1.5") && shows(content,"false"));
    pack_view_close();
    lv_screen_load(home_screen);
    lv_obj_delete(screen);

    // Only a negotiated composite with both grants transfers the subscription to the magnifier.
    scene.composite=true;
    scene.permissions|=PACK_PERM_CAPTURE;
    for (unsigned allow=0;allow<2;allow++) {
        screen=lv_obj_create(NULL); content=lv_obj_create(screen);
        unsigned previous_stops=stops;
        unsigned granted=allow ? scene.permissions : scene.permissions & ~PACK_PERM_CAPTURE;
        assert(pack_view_open(content,&scene,granted));
        lv_screen_load(screen);
        sim_settle(); pack_view_tick();
        assert(joint_session==(bool)allow);
        pack_view_enter_magnifier();
        assert(stops==previous_stops+(allow ? 0u : 1u));
        lv_screen_load(home_screen);
        lv_obj_delete(screen);
    }
}

static void local_state_view(const char *folder)
{
    load_at(folder);
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_t *content=lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_pos(content,0,32);
    lv_obj_set_size(content,1024,568);
    assert(scene.states==1 && scene.actions==2 && pack_view_open(content,&scene,PACK_PERM_UI));
    lv_screen_load(screen);
    for (unsigned i=0; i<=scene.actions; i++) {
        sim_settle();
        pack_view_tick();
        char scratch[32], tile[160];
        const char *expected=pack_state_text(&scene.state[0],scratch);
        assert(shows(content,expected));
        if (scene.bindings==2) {
            snprintf(tile,sizeof(tile),"Local\n%s kt",expected);
            assert(shows(content,tile));
        }
        puts(expected);
        if (i<scene.actions) {
            const pack_widget_t *w=&scene.widget[scene.action[i].widget];
            tap(w->x+w->w/2,32+w->y+w->h/2);
        }
    }
    assert(!subscriptions && !reports && !starts);
    pack_view_close();
    lv_screen_load(home_screen);
    lv_obj_delete(screen);
}

static lv_obj_tree_walk_res_t count_object(lv_obj_t *obj, void *user)
{
    (void)obj;
    ++*(unsigned *)user;
    return LV_OBJ_TREE_WALK_NEXT;
}

static void count_flush(lv_event_t *event)
{
    ++*(unsigned *)lv_event_get_user_data(event);
}

static void activation_cycles(const char *const examples[4])
{
    sim_app_t shell;
    assert(sim_app_start(&shell));
    lv_display_t *display=lv_display_get_default();
    unsigned flushes=0, warm_objects=0, saved_subs=subscriptions, saved_stops=stops;
    size_t warm_bytes=0, warm_blocks=0;
    lv_display_add_event_cb(display,count_flush,LV_EVENT_FLUSH_FINISH,&flushes);
    for (unsigned i=0; i<1004; i++) {
        char folder[768];
        snprintf(folder,sizeof(folder),"%s/%s",KVM_EXAMPLES,examples[i%4]);
        load_at(folder);
        ui_app_leave(&shell.app);
        lv_obj_t *scr=lv_obj_create(NULL), *content=lv_obj_create(scr);
        lv_obj_remove_style_all(content);
        lv_obj_set_pos(content,0,32);
        lv_obj_set_size(content,1024,568);
        assert(pack_view_open(content,&scene,scene.permissions) && control_bytes>0);
        lv_screen_load(scr);
        pack_view_tick();
        unsigned before=flushes;
        lv_obj_invalidate(scr);
        lv_refr_now(display);
        assert(lv_screen_active()==scr && flushes>before);

        pack_view_close();
        lv_screen_load(shell.app.screen);
        ui_app_show(&shell.app);
        lv_obj_delete_async(scr);
        sim_settle();
        assert(lv_screen_active()==shell.app.screen && !control_bytes);
        assert(subscriptions-saved_subs==stops-saved_stops);
        unsigned objects=0;
        lv_obj_tree_walk(NULL,count_object,&objects); // Includes inactive screens and display layers.
        if (i==3) {
            warm_objects=objects; warm_bytes=host_lv_live_bytes(); warm_blocks=host_lv_live_count();
        } else if (i>3) {
            assert(objects==warm_objects && host_lv_live_bytes()==warm_bytes && host_lv_live_count()==warm_blocks);
        }
    }
    assert(subscriptions-saved_subs==251);
    printf("PACK-014: 1000 scene activations after 4 warmups, Home objects=%u blocks=%zu bytes=%zu controls=0\n",
           warm_objects,warm_blocks,warm_bytes);
    lv_display_remove_event_cb_with_user_data(display,count_flush,&flushes);
    sim_app_stop(&shell);
}

static void reject_second_icon(lv_event_t *event)
{
    lv_obj_t *child=lv_event_get_param(event);
    if (!lv_obj_check_type(child,&lv_image_class)) return;
    lv_obj_t **first_image=lv_event_get_user_data(event);
    if (!*first_image) {
        *first_image=child;
        return;
    }
    // The first button already holds this shared, renderer-owned descriptor. Exercise
    // LVGL's real failure to identify the source when the second button sets its image.
    lv_image_dsc_t *src=(lv_image_dsc_t *)lv_image_get_src(*first_image);
    assert(src && src->data);
    src->data=NULL;
}

static void palette_ink(lv_obj_t *button,unsigned palette,bool active)
{
    static const uint32_t colors[][4]={
        {0xffffff,0x1d1d1f,0xe1f0ff,0x005497},
        {0x202c39,0xf0f5fa,0x263d50,0xa8daff},
        {0x000000,0xffffff,0xffff00,0x000000},
    };
    unsigned i=active ? 2 : 0;
    lv_obj_t *label=lv_obj_get_child(button,0);
    assert((lv_color_to_u32(lv_obj_get_style_bg_color(button,0))&0xffffff)==colors[palette][i]);
    assert((lv_color_to_u32(lv_obj_get_style_text_color(label,0))&0xffffff)==colors[palette][i+1]);
}

static void plain_button_palettes(void)
{
    load_at(KVM_EXAMPLES "/flight-checklist");
    unsigned strip=scene.widgets++;
    scene.widget[strip]=(pack_widget_t){.parent=0,.kind=PACK_TABS,.x=32,.y=358,.w=420,.h=60,.tab_count=2};
    scene.tabs=2;
    strcpy(scene.tab[0].label,"First"); strcpy(scene.tab[1].label,"Second");
    scene.tab[0].page=scene.tab[1].page=-1;
    assert(ui_theme_init());
    const char *const pictures[]={"buttons-day","buttons-night","buttons-high-contrast"};
    for (unsigned p=0;p<UI_PALETTE_COUNT;p++) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        lv_obj_t *screen=lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        assert(pack_view_open(screen,&scene,PACK_PERM_UI));
        lv_screen_load(screen); sim_settle();
        lv_obj_t *root=lv_obj_get_child(screen,0);
        lv_obj_t *button=lv_obj_get_child(root,scene.action[0].widget-1);
        lv_obj_t *tabs=lv_obj_get_child(root,strip-1);
        lv_obj_t *one=lv_obj_get_child(tabs,0), *two=lv_obj_get_child(tabs,1);
        lv_test_wait(400);
        palette_ink(button,p,false); palette_ink(one,p,true); palette_ink(two,p,false);
        sim_press_point(100,290);
        lv_test_wait(400);
        palette_ink(button,p,true);
        char path[64];
        snprintf(path,sizeof(path),"%s-pressed",pictures[p]); picture(path);
        sim_release();
        assert(shows(root,"Complete"));
        tap(420,290); assert(shows(root,"Pending"));
        tap(335,387);
        lv_test_wait(400);
        assert(!lv_obj_has_state(one,LV_STATE_CHECKED) && lv_obj_has_state(two,LV_STATE_CHECKED));
        palette_ink(button,p,false); palette_ink(one,p,false); palette_ink(two,p,true);
        picture(pictures[p]);
        pack_view_close(); home(); lv_obj_delete(screen);
    }
    ui_theme_deinit();
    puts("PASS palette fills and caption colors for plain Button, pressed Button, and both TabStrip states; real touch in three palettes.");
}

static void input_palettes(const char *local_fixture)
{
    const char *const themes[]={"day","night","high-contrast"};
    const char *const folders[]={KVM_EXAMPLES "/flight-checklist",local_fixture,
                                 KVM_EXAMPLES "/flight-yoke",KVM_EXAMPLES "/cursor-magnifier"};
    const char *const names[]={"text","local","yoke","stream"};
    assert(ui_theme_init());
    for (unsigned p=0;p<UI_PALETTE_COUNT;p++) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        for (unsigned k=0;k<4;k++) {
            load_at(folders[k]);
            lv_obj_t *screen=lv_obj_create(NULL), *content=lv_obj_create(screen);
            ui_theme_apply(screen,UI_STYLE_CONTROL_PAGE,0);
            lv_obj_remove_style_all(content);
            lv_obj_set_size(content,1024,568); lv_obj_set_pos(content,0,32);
            assert(pack_view_open(content,&scene,scene.permissions));
            lv_screen_load(screen); sim_settle(); pack_view_tick();
            lv_obj_t *root=lv_obj_get_child(content,0);
            if (k==0) {
                for (unsigned i=0;i<2;i++) {
                    lv_obj_t *row=lv_obj_get_child(root,i);
                    assert(lv_obj_get_style_bg_opa(row,0)==LV_OPA_TRANSP && !lv_obj_is_clickable(row));
                }
            } else if (k==1) {
                tap(42,64); assert(shows(root,"1"));
                lv_obj_t *arc=lv_obj_get_child(lv_obj_get_child(root,1),1);
                lv_area_t a; lv_obj_get_coords(arc,&a);
                int x=(a.x1+a.x2)/2,y=(a.y1+a.y2)/2,r=(a.x2-a.x1)/2-5;
                sim_press_point(x,y-r); lv_test_wait(100);
                lv_test_mouse_move_to(x+r,y); lv_test_wait(100); sim_release();
                if (lv_arc_get_value(arc)!=66)
                    fprintf(stderr,"palette %u dial value %d, touch at (%d,%d), radius %d\n",p,lv_arc_get_value(arc),x,y,r);
                assert(lv_arc_get_value(arc)==66 && scene.state[0].value.integer==66);
            } else if (k==2) {
                unsigned n=reports;
                tap(615,250);
                assert(reports>=n+2 && recorded[n].data.joystick.x>10000 && !recorded[reports-1].data.joystick.x);
                tap(911,122); assert(recorded[reports-1].data.joystick.z>30000);
                lv_obj_t *bar=lv_obj_get_child(lv_obj_get_child(root,1),1);
                lv_area_t a; lv_obj_get_coords(bar,&a);
                n=reports;
                tap(a.x1+10,(a.y1+a.y2)/2);
                assert(reports>n && recorded[reports-1].data.joystick.z<10000);
                n=reports;
                tap((a.x1+3*a.x2)/4,(a.y1+a.y2)/2);
                assert(reports>n && recorded[reports-1].data.joystick.z>40000);
            } else {
                assert(shows(root,"Capture stopped"));
            }
            char picture_name[64]; snprintf(picture_name,sizeof(picture_name),"%s-%s",names[k],themes[p]);
            sim_settle(); picture(picture_name);
            pack_view_close(); home(); lv_obj_delete(screen);
            if (k==3) {
                // A profile may make screen sharing optional. Keep its empty state readable in both cases.
                scene.required_permissions&=~PACK_PERM_CAPTURE;
                screen=lv_obj_create(NULL);
                ui_theme_apply(screen,UI_STYLE_CONTROL_PAGE,0);
                assert(pack_view_open(screen,&scene,PACK_PERM_UI));
                lv_screen_load(screen); sim_settle();
                assert(shows(screen,"Screen sharing is off for this extension.\nYou can allow it in the permission review."));
                snprintf(picture_name,sizeof(picture_name),"stream-off-%s",themes[p]); picture(picture_name);
                pack_view_close(); home(); lv_obj_delete(screen);
            }
        }
    }
    ui_theme_deinit();
    puts("PASS themed Text, TouchArea, Dial, AxisPad, Slider and idle StreamViewport in three palettes, real pointer events.");
}

static void button_icons(void)
{
    load_at(KVM_EXAMPLES "/flight-checklist");
    const unsigned width=24, height=12;
    pack_icon_t *icon=calloc(1,sizeof(*icon)+4*width*height);
    assert(icon);
    icon->width=width; icon->height=height; icon->member=PACK_ASSETS;
    for (unsigned y=0;y<height;y++) for (unsigned x=0;x<width;x++) {
        uint8_t *p=icon->bgra+4*(y*width+x);
        p[2]=255; p[3]=x<8 ? 255 : x<16 ? 128 : 0;
    }
    scene.icons=icon;
    unsigned first=scene.action[0].widget, second=scene.action[1].widget;
    scene.widget[first].icon_member=scene.widget[second].icon_member=icon->member;
    assert(ui_theme_init());
    lv_obj_t *failed_screen=lv_obj_create(NULL), *sibling=lv_label_create(failed_screen), *first_image=NULL;
    lv_obj_add_event_cb(failed_screen,reject_second_icon,LV_EVENT_CHILD_CREATED,&first_image);
    assert(!pack_view_open(failed_screen,&scene,PACK_PERM_UI));
    assert(first_image && !control_bytes && lv_obj_get_child_count(failed_screen)==1 &&
           lv_obj_get_child(failed_screen,0)==sibling);
    lv_obj_remove_event_cb_with_user_data(failed_screen,reject_second_icon,&first_image);
    assert(pack_view_open(failed_screen,&scene,PACK_PERM_UI));
    lv_obj_delete(failed_screen);
    assert(!control_bytes);
    const char *const pictures[]={"icons-day","icons-night","icons-high-contrast"};
    for (unsigned palette=0;palette<UI_PALETTE_COUNT;palette++) {
        ui_theme_set_control_palette((ui_control_palette_t)palette);
        lv_obj_t *screen=lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        assert(pack_view_open(screen,&scene,PACK_PERM_UI));
        lv_screen_load(screen); sim_settle();
        lv_obj_t *root=lv_obj_get_child(screen,0), *button=lv_obj_get_child(root,first-1);
        lv_obj_t *label=lv_obj_get_child(button,0), *image=lv_obj_get_child(button,1);
        lv_obj_t *reset=lv_obj_get_child(root,second-1), *reset_image=lv_obj_get_child(reset,1);
        palette_ink(button,palette,false);
        assert(lv_image_get_src(image)==lv_image_get_src(reset_image));
        lv_area_t box, mark, caption;
        lv_obj_get_coords(button,&box); lv_obj_get_coords(image,&mark); lv_obj_get_coords(label,&caption);
        assert(mark.x1==box.x1+6 && caption.x1==mark.x2+7 && caption.x2==box.x2-6);
        assert(lv_area_get_width(&mark)==24 && lv_area_get_height(&mark)==12);
        assert(abs((mark.y1+mark.y2)-(box.y1+box.y2))<=1);
        sim_image_t img;
        assert(sim_image_render(screen,&img));
        const uint8_t *row=img.rgb+3*((size_t)(mark.y1+6)*img.w+mark.x1);
        const uint8_t *opaque=row+3*3, *half=row+3*11, *clear=row+3*20, *background=row+3*25;
        assert(opaque[0]>240 && opaque[1]<12 && opaque[2]<12 && !memcmp(clear,background,3));
        for (unsigned j=0;j<3;j++) assert(abs(2*half[j]-opaque[j]-clear[j])<25);
        assert(!strcmp(lv_label_get_text(label),"Mark complete"));
        unsigned ink=0;
        for (int y=caption.y1;y<=caption.y2;y++) for (int x=caption.x1;x<=caption.x2;x++) {
            const uint8_t *p=img.rgb+3*((size_t)y*img.w+x);
            if (abs(p[0]-background[0])+abs(p[1]-background[1])+abs(p[2]-background[2])>120) ink++;
        }
        assert(ink>80);
        sim_image_free(&img);
        tap(mark.x1+3,mark.y1+6);
        const pack_action_t *action=&scene.action[0];
        assert(!strcmp(scene.state[action->state].value.text,action->value.text));
        lv_obj_get_coords(reset_image,&mark); tap(mark.x1+4,mark.y1+5);
        action=&scene.action[1];
        assert(!strcmp(scene.state[action->state].value.text,action->value.text));
        tap(caption.x1+12,(caption.y1+caption.y2)/2);
        action=&scene.action[0];
        assert(!strcmp(scene.state[action->state].value.text,action->value.text));
        lv_obj_get_coords(lv_obj_get_child(reset,0),&caption);
        tap(caption.x1+12,(caption.y1+caption.y2)/2);
        action=&scene.action[1];
        assert(!strcmp(scene.state[action->state].value.text,action->value.text));
        picture(pictures[palette]);
        pack_view_close();
        assert(!lv_image_get_src(image) && !lv_image_get_src(reset_image) && control_bytes==0);
        home(); lv_obj_delete(screen);
    }
    for (unsigned i=0;i<2;i++) {
        scene.widget[first].w=i ? 64 : 63;
        scene.widget[first].h=i ? 39 : 40;
        lv_obj_t *screen=lv_obj_create(NULL);
        assert(pack_view_open(screen,&scene,PACK_PERM_UI));
        lv_obj_t *button=lv_obj_get_child(lv_obj_get_child(screen,0),first-1);
        assert(lv_obj_get_child_count(button)==1);
        lv_obj_delete(screen);
        assert(!control_bytes);
    }
    free(icon); scene.icons=NULL; ui_theme_deinit();
    puts("PASS authored Button icons: shared source, RGB and three alpha levels in three palettes, real touch, small bounds and teardown.");
}

static lv_obj_t *visible_caption(lv_obj_t *obj,const char *text)
{
    if (lv_obj_is_hidden(obj)) return NULL;
    if (lv_obj_check_type(obj,&lv_label_class) && !strcmp(lv_label_get_text(obj),text))
        return lv_obj_is_visible(obj) ? obj : NULL;
    for (unsigned i=0;i<lv_obj_get_child_count(obj);i++) {
        lv_obj_t *label=visible_caption(lv_obj_get_child(obj,(int)i),text);
        if (label) return label;
    }
    return NULL;
}

static void screen_pointer(bool portrait,int x,int y)
{
    lv_test_mouse_move_to(portrait ? y : x,portrait ? 599-x : y);
    lv_test_wait(50);
}

static void screen_press(bool portrait,int x,int y)
{
    screen_pointer(portrait,x,y);
    lv_test_mouse_press(); lv_test_wait(50);
}

static void screen_tap(bool portrait,int x,int y)
{
    screen_press(portrait,x,y);
    sim_release();
}

static void rate_slider_view(const char *folder)
{
    assert(ui_theme_init());
    for (unsigned portrait=0;portrait<2;portrait++) {
        memset(&scene,0,sizeof(scene));
        load_at(folder);
        scene.portrait=portrait; // Identical local coordinates in both orientations.
        lv_obj_t *screen=lv_obj_create(NULL);
        lv_obj_remove_style_all(screen);
        lv_obj_t *frame=pack_view_frame(screen,portrait);
        lv_obj_t *content=lv_obj_create(frame);
        lv_obj_remove_style_all(content);
        lv_obj_set_pos(content,0,32);
        lv_obj_set_size(content,portrait ? 600 : 1024,portrait ? 992 : 568);
        assert(pack_view_open(content,&scene,PACK_PERM_UI));
        lv_screen_load(screen); sim_settle();
        lv_obj_t *up=visible_caption(content,LV_SYMBOL_UP);
        assert(up && visible_caption(lv_obj_get_parent(up),LV_SYMBOL_DOWN));
        lv_obj_t *meter=lv_obj_get_child_by_type(lv_obj_get_parent(up),0,&lv_slider_class);
        assert(meter && lv_obj_is_visible(meter) && lv_slider_get_value(meter)==0);
        pack_state_t *value=&scene.state[0];
        screen_tap(portrait,44,124);
        assert(value->value.integer==1000);
        screen_press(portrait,44,54);
        assert(value->value.integer==1100 && shows(content,"1100"));
        lv_test_wait(340);
        int64_t increasing=value->value.integer;
        assert(increasing>=1400);
        picture(portrait ? "rate-slider-portrait" : "rate-slider-landscape");
        screen_pointer(portrait,44,124);
        lv_test_wait(300); assert(value->value.integer==increasing);
        screen_pointer(portrait,44,193);
        sim_release();
        assert(value->value.integer<increasing);
        int64_t released=value->value.integer;
        lv_test_wait(550); assert(value->value.integer==released);

        // The authored wheel is x=12..75, y=44..203 below the system header.
        // A finger that starts on the adjacent area cannot acquire the wheel.
        screen_press(portrait,110,124);
        screen_pointer(portrait,44,54);
        lv_test_wait(200); sim_release();
        assert(value->value.integer==released);
        screen_press(portrait,44,124);
        screen_pointer(portrait,44,24);
        int64_t above=value->value.integer;
        lv_test_wait(300);
        assert(above>released && value->value.integer>=above+300);
        screen_pointer(portrait,110,24); // Diagonal drift beyond the width keeps scrolling.
        int64_t to_side=value->value.integer;
        lv_test_wait(300);
        assert(value->value.integer>=to_side+300);
        screen_pointer(portrait,110,124); // The center is neutral even outside the width.
        int64_t center=value->value.integer;
        lv_test_wait(300); assert(value->value.integer==center);
        screen_pointer(portrait,44,244);
        int64_t below=value->value.integer;
        lv_test_wait(300);
        assert(below<center && value->value.integer<=below-300);
        sim_release();
        released=value->value.integer;
        lv_test_wait(550); assert(value->value.integer==released);
        value->value.integer=19950; screen_tap(portrait,44,54);
        assert(value->value.integer==20000 && shows(content,"20000"));
        value->value.integer=50; screen_tap(portrait,44,193);
        assert(value->value.integer==0 && shows(content,"0"));
        // Full int64 state bounds still saturate safely with the authored delta.
        value->value.integer=INT64_MAX-1; value->max.integer=INT64_MAX;
        screen_tap(portrait,44,54); assert(value->value.integer==INT64_MAX);
        value->value.integer=INT64_MIN+1; value->min.integer=INT64_MIN;
        screen_press(portrait,44,193); assert(value->value.integer==INT64_MIN);
        pack_view_close(); home(); sim_release(); lv_test_wait(150); lv_obj_delete(screen);
    }
    assert(!reports && !subscriptions && !starts && !stops);
    ui_theme_deinit();
    puts("PASS RateSlider package, landscape and portrait, captured drag beyond both ends and sides, neutral, release, bounded int64 and close");
}

#if CONFIG_KVM_USB_MONITOR_EVAL
static bool contacts(unsigned n,int x,int y,int x2,int y2)
{
    static uint64_t clock;
    const uint8_t ids[]={3,7};
    const lv_point_t p[]={{x,y},{x2,y2}};
    clock+=10000;
    return pack_view_touchpad(n,p,ids,clock);
}

static void check_console_edge_motion(void)
{
    unsigned before=reports;
    assert(contacts(1,110,530,0,0));
    assert(contacts(1,110,712,0,0)); // Cross from the pad onto the keyboard.
    for (unsigned i=0;i<3;i++) {
        assert(contacts(1,110,712,0,0)); // Stay still outside.
        assert(reports==before+2+i);
        assert(recorded[reports-1].type==KVM_HID_MOUSE_MOVE);
        assert(recorded[reports-1].data.mouse_move.dy>0 && recorded[reports-1].data.mouse_move.dx==0);
    }
    assert(contacts(1,110,530,0,0)); // Return to the pad and stay still.
    assert(recorded[reports-1].type==KVM_HID_MOUSE_MOVE && recorded[reports-1].data.mouse_move.dy<0);
    before=reports;
    assert(contacts(1,110,530,0,0));
    assert(contacts(0,0,0,0,0));
    assert(reports==before);

    assert(contacts(1,110,530,0,0));
    assert(contacts(1,110,420,0,0)); // Release over the monitor.
    assert(contacts(1,110,420,0,0));
    assert(reports==before+2 && recorded[reports-1].type==KVM_HID_MOUSE_MOVE);
    assert(recorded[reports-1].data.mouse_move.dy<0);
    before=reports;
    assert(contacts(0,0,0,0,0));
    // A new press on the keyboard cannot acquire the pad by sliding into it.
    assert(!contacts(1,110,712,0,0));
    assert(!contacts(1,110,530,0,0));
    assert(!contacts(0,0,0,0,0));
    assert(reports==before);
}

static lv_obj_t *drawer_label(lv_obj_t *screen,const char *text)
{
    lv_display_t *display=lv_obj_get_display(screen);
    lv_obj_t *label=visible_caption(lv_display_get_layer_sys(display),text);
    if (!label) label=visible_caption(lv_display_get_layer_top(display),text);
    return label ? label : visible_caption(screen,text);
}

static void tap_drawer_label(lv_obj_t *screen,const char *text)
{
    lv_obj_t *label=drawer_label(screen,text);
    if (!label) fprintf(stderr,"Missing visible console drawer label '%s'\n",text);
    assert(label);
    lv_area_t bounds;
    lv_obj_get_coords(label,&bounds);
    screen_tap(true,(bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2);
    sim_settle();
}

static void open_gestures(lv_obj_t *screen)
{
    assert(!drawer_label(screen,"Touchpad gestures"));
    lv_obj_t *entry=drawer_label(screen,"Gestures");
    assert(entry);
    lv_area_t bounds;
    lv_obj_get_coords(entry,&bounds);
    // Both authored pages position their touchpad immediately below the monitor.
    assert(bounds.x1>300 && bounds.y1>=470 && bounds.y2<560);
    tap_drawer_label(screen,"Gestures");
    lv_obj_t *title=drawer_label(screen,"Touchpad gestures");
    assert(title);
    lv_obj_get_coords(title,&bounds);
    assert((bounds.x1+bounds.x2)/2>300);
}

static void check_gesture_drawer(lv_obj_t *screen,bool online,const char *output)
{
    static const char *const labels[]={"Left click","Right click","Middle click"};
    static const unsigned buttons[]={KVM_MOUSE_BTN_LEFT,KVM_MOUSE_BTN_RIGHT,KVM_MOUSE_BTN_MIDDLE};
    const unsigned changes=usb_changes,opens=monitor_opens,closes=monitor_closes,epoch=mount_epoch;
    const bool mounted=usb_mounted,controls=usb_controls,native=receiver_native;
    unsigned baseline=reports;
    open_gestures(screen);
    if (online) {
        sim_image_t image;
        assert(sim_image_render(screen,&image) && image.w==600 && image.h==1024);
        char path[1024];
        snprintf(path,sizeof(path),"%s/touchpad-gestures.png",output);
        assert(sim_image_write_png(path,&image));
        sim_image_free(&image);
    }
    // Route raw contacts from the otherwise active pad, including drag and wheel.
    contacts(1,110,530,0,0);contacts(1,180,530,0,0);contacts(0,0,0,0,0);
    contacts(2,100,530,190,530);contacts(2,100,570,190,570);contacts(0,0,0,0,0);
    assert(drawer_label(screen,"Touchpad gestures") && reports==baseline);
    if (!online) {
        for (unsigned i=0;i<3;i++) {
            lv_obj_t *button=drawer_label(screen,labels[i]);
            assert(button);
            while (button && !lv_obj_has_state(button,LV_STATE_DISABLED)) button=lv_obj_get_parent(button);
            assert(button);
            tap_drawer_label(screen,labels[i]);
            assert(drawer_label(screen,"Touchpad gestures") && reports==baseline);
        }
    }
    tap_drawer_label(screen,"Close");
    assert(!drawer_label(screen,"Touchpad gestures") && reports==baseline);
    open_gestures(screen);
    screen_tap(true,35,530);sim_settle();
    assert(!drawer_label(screen,"Touchpad gestures") && reports==baseline);
    if (online) {
        for (unsigned i=0;i<3;i++) {
            open_gestures(screen);
            assert(reports==baseline);
            tap_drawer_label(screen,labels[i]);
            assert(!drawer_label(screen,"Touchpad gestures") && reports==baseline+2);
            assert(recorded[baseline].type==KVM_HID_MOUSE_BTN_DOWN &&
                   recorded[baseline].data.mouse_button.button_id==buttons[i]);
            assert(recorded[baseline+1].type==KVM_HID_MOUSE_BTN_UP &&
                   recorded[baseline+1].data.mouse_button.button_id==buttons[i]);
            baseline=reports;
        }
    }
    assert(usb_changes==changes && monitor_opens==opens && monitor_closes==closes && mount_epoch==epoch);
    assert(usb_mounted==mounted && usb_controls==controls && receiver_native==native);
}

static void console_image(const char *folder,const char *filename)
{
    sim_image_t portrait;
    assert(sim_image_render(lv_screen_active(),&portrait));
    assert(portrait.w==600 && portrait.h==1024);
    // The inset borders distinguish a fitted 640x480 frame from a centered crop.
    const unsigned xy[][2]={{96,112},{550,110},{46,442},{550,442},{26,200},{574,200},{150,56},{150,462}};
    const uint8_t rgb[][3]={{255,0,0},{0,255,0},{0,0,255},{255,255,255},
                            {255,255,0},{255,255,0},{0,255,255},{0,255,255}};
    char path[1024];snprintf(path,sizeof(path),"%s/%s.png",folder,filename);
    assert(sim_image_write_png(path,&portrait));
    for (unsigned i=0;i<sizeof(xy)/sizeof(xy[0]);i++) {
        const uint8_t *actual=portrait.rgb+3*(xy[i][1]*600+xy[i][0]);
        if (memcmp(actual,rgb[i],3)) fprintf(stderr,"%s sample %u = %u,%u,%u\n",
                                              filename,i,actual[0],actual[1],actual[2]);
        assert(!memcmp(actual,rgb[i],3));
    }
    sim_image_free(&portrait);
}

static bool has_cap(lv_obj_t *surface,const char *text)
{
    lv_obj_t *caption=visible_caption(surface,text);
    if (!caption) return false;
    lv_obj_t *button=lv_obj_get_parent(caption);
    return lv_obj_check_type(button,&lv_button_class) && lv_obj_get_parent(button)==surface &&
           lv_obj_has_state(button,LV_STATE_CHECKED)==(!strcmp(text,"Caps\nOn"));
}

static void zoom_console(lv_obj_t *screen)
{
    unsigned hid=reports,changes=usb_changes,opens=monitor_opens,closes=monitor_closes;
    assert(visible_caption(screen,"1:1") && visible_caption(screen,"Stop"));
    screen_tap(true,45,67);
    assert(visible_caption(screen,"Fit"));
    screen_tap(true,300,160);
    assert(!visible_caption(screen,"Fit") && !visible_caption(screen,"Stop"));
    screen_tap(true,300,160);
    assert(visible_caption(screen,"Fit") && visible_caption(screen,"Stop"));
    assert(reports==hid && usb_changes==changes && monitor_opens==opens && monitor_closes==closes);
}

static lv_obj_t *visible_frame(lv_obj_t *root,const lv_image_dsc_t *source)
{
    if (!lv_obj_is_visible(root)) return NULL;
    if (lv_obj_check_type(root,&lv_image_class) && lv_image_get_src(root)==source) return root;
    for (unsigned i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *found=visible_frame(lv_obj_get_child(root,(int)i),source);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *console_image_widget(lv_obj_t *screen)
{
    lv_obj_t *stop=visible_caption(screen,"Stop");
    assert(stop);
    lv_obj_t *image=lv_obj_get_child(lv_obj_get_parent(lv_obj_get_parent(stop)),0);
    assert(lv_obj_check_type(image,&lv_image_class));
    return image;
}

static void check_pan_navigation(lv_obj_t *screen,bool from_console)
{
    const unsigned changes=usb_changes,opens=monitor_opens,closes=monitor_closes,epoch=mount_epoch;
    lv_obj_t *image=console_image_widget(screen);
    const lv_image_dsc_t *source=lv_image_get_src(image);
    assert(source && source->data==(const uint8_t *)monitor_pixels);
    zoom_console(screen);
    screen_press(true,240,160);screen_pointer(true,250,166);screen_pointer(true,262,174);sim_release();
    int x=lv_image_get_offset_x(image),y=lv_image_get_offset_y(image);
    assert(x==22 && y==14);
    screen_tap(true,300,160);
    assert(!visible_caption(screen,"Fit") && !visible_caption(screen,"Stop"));
    sim_image_t original;
    assert(sim_image_render(image,&original));
    assert(contacts(1,110,530,0,0));assert(contacts(1,160,530,0,0));
    const unsigned hid=reports;
    for (unsigned pick=0;pick<2;pick++) {
        unsigned generation=snap.release_generation;
        screen_tap(true,300,1000);pack_view_tick();sim_settle();
        assert(visible_caption(screen,"Return to KVMpad") && snap.release_generation>generation);
        assert(!contacts(1,170,535,0,0));
        bool console=pick==0 ? from_console : !from_console;
        screen_tap(true,console ? 160 : 440,125);pack_view_tick();sim_settle();
        assert(!visible_caption(screen,"Return to KVMpad") && !visible_caption(screen,"Stop") &&
               !visible_caption(screen,"Fit") && !visible_caption(screen,"1:1"));
        assert(!contacts(1,180,545,0,0));
        image=visible_frame(screen,source);
        assert(image && source->data==(const uint8_t *)monitor_pixels);
        assert(lv_image_get_inner_align(image)==LV_IMAGE_ALIGN_CENTER && lv_image_get_scale(image)==LV_SCALE_NONE);
        assert(lv_image_get_offset_x(image)==x && lv_image_get_offset_y(image)==y);
        sim_image_t after;
        assert(sim_image_render(image,&after));
        assert(after.w==original.w && after.h==original.h &&
               !memcmp(after.rgb,original.rgb,(size_t)after.w*after.h*3));
        sim_image_free(&after);
    }
    sim_image_free(&original);
    contacts(0,0,0,0,0);
    screen_tap(true,300,160);
    assert(visible_caption(screen,"Fit") && visible_caption(screen,"Stop"));
    screen_tap(true,45,67);
    assert(visible_caption(screen,"1:1") && !lv_image_get_offset_x(image) && !lv_image_get_offset_y(image));
    assert(reports==hid && changes==usb_changes && opens==monitor_opens && closes==monitor_closes && epoch==mount_epoch);
}

static void check_local_console_damage(lv_obj_t *screen,bool native)
{
    unsigned hid=reports,changes=usb_changes,opens=monitor_opens,closes=monitor_closes;
    assert(visible_caption(screen,"1:1") && visible_caption(screen,"Stop"));
    check_console_damage(screen,monitor_pixels,native,offer_picture);
    assert(visible_caption(screen,"1:1") && visible_caption(screen,"Stop"));
    assert(reports==hid && usb_changes==changes && monitor_opens==opens && monitor_closes==closes);
}

static void check_waiting_pixels(lv_obj_t *image,bool waiting)
{
    sim_image_t snap;
    assert(sim_image_render(image,&snap));
    unsigned bright=0,lines=0;
    bool previous_line=false;
    for (unsigned y=55;y<snap.h;y++) {
        bool line=false;
        for (unsigned x=0;x<snap.w;x++) {
            const uint8_t *pixel=snap.rgb+3*(y*snap.w+x);
            if (pixel[0]<130 || pixel[1]<130 || pixel[2]<130) continue;
            assert(x>0 && x+1<snap.w && y+1<snap.h);
            bright++;
            line=true;
        }
        if (line && !previous_line) lines++;
        previous_line=line;
    }
    assert(waiting ? (bright>160 && lines==2) : !bright);
    sim_image_free(&snap);
}

static void count_draw(lv_event_t *event)
{
    unsigned *draws=lv_event_get_user_data(event);
    ++*draws;
}

static void drag_viewport(lv_obj_t *image,int dx,int dy)
{
    lv_area_t bounds;
    lv_obj_get_coords(image,&bounds);
    int x=bounds.x1+80,y=bounds.y1+74;
    sim_press_point(x,y);
    screen_pointer(false,x+dx/2,y+dy/2);
    screen_pointer(false,x+dx,y+dy);
    sim_release();
}

static void check_viewport_size_change(lv_obj_t *screen,lv_obj_t *small)
{
    lv_obj_t *parent=lv_obj_create(screen), *large=NULL;
    lv_obj_set_pos(parent,200,0);
    lv_obj_set_size(parent,600,472);
    assert(pack_desktop_viewport(parent,600,&large));
    lv_obj_set_hidden(parent,true);
    sim_click_obj(lv_obj_get_parent(visible_caption(screen,"Start USB monitor")));
    usb_mounted=true;pack_desktop_tick(NULL);sim_settle();
    offer_picture(monitor_pixels,NULL,0);
    const lv_image_dsc_t *source=lv_image_get_src(small);
    assert(source && source->header.w==640 && source->header.h==480);
    unsigned changes=usb_changes,opens=monitor_opens;
    sim_click_obj(lv_obj_get_parent(visible_caption(screen,"1:1")));
    drag_viewport(small,52,30);
    assert(lv_image_get_offset_x(small)==52 && lv_image_get_offset_y(small)==30);
    for (unsigned change=0;change<2;change++) {
        lv_obj_set_hidden(lv_obj_get_parent(small),true);
        lv_obj_set_hidden(parent,false);
        pack_desktop_select(large,NULL,false);sim_settle();
        assert(lv_image_get_src(large)==source && lv_image_get_scale(large)==LV_SCALE_NONE);
        assert(lv_image_get_offset_x(large)==20 && lv_image_get_offset_y(large)==4);
        if (change) {
            drag_viewport(large,-14,-6);
            assert(lv_image_get_offset_x(large)==6 && lv_image_get_offset_y(large)==-2);
        }
        lv_obj_set_hidden(parent,true);
        lv_obj_set_hidden(lv_obj_get_parent(small),false);
        pack_desktop_select(small,NULL,false);sim_settle();
        assert(lv_image_get_src(small)==source && lv_image_get_scale(small)==LV_SCALE_NONE);
        assert(lv_image_get_offset_x(small)==(change ? 6 : 52) &&
               lv_image_get_offset_y(small)==(change ? -2 : 30));
    }
    assert(usb_changes==changes && monitor_opens==opens);
    sim_click_obj(lv_obj_get_parent(visible_caption(screen,"Stop")));
}

static void minimum_monitor_view(void)
{
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_t *parent=lv_obj_create(screen), *image=NULL;
    lv_obj_set_size(parent,160,120);
    assert(pack_desktop_viewport(parent,160,&image));
    lv_screen_load(screen);
    pack_desktop_select(image,NULL,false);
    ui_control_palette_t original=ui_theme_control_palette();
    for (unsigned p=0;p<UI_PALETTE_COUNT;p++) {
        ui_theme_set_control_palette((ui_control_palette_t)p);
        sim_settle();
        assert(visible_caption(screen,"640×480") && visible_caption(screen,"1024×600"));
        lv_obj_t *label=visible_caption(screen,"Start USB monitor");
        assert(label);
        lv_area_t button,view;
        lv_obj_get_coords(lv_obj_get_parent(label),&button);
        lv_obj_get_coords(parent,&view);
        assert(button.x1>=view.x1 && button.x2<=view.x2 && button.y1>=view.y1 && button.y2<=view.y2);
        sim_click_obj(lv_obj_get_parent(label));
        assert(usb_controls && !usb_mounted);
        check_waiting_pixels(image,false);
        usb_mounted=true;pack_desktop_tick(NULL);sim_settle();
        check_waiting_pixels(image,true);
        lv_point_t point={view.x1+80,view.y1+76};
        assert(lv_indev_search_obj(screen,&point)==image);
        usb_mounted=false;pack_desktop_tick(NULL);sim_settle();
        check_waiting_pixels(image,false);
        usb_mounted=true;pack_desktop_tick(NULL);sim_settle();
        check_waiting_pixels(image,true);
        memset(monitor_pixels,0,sizeof(monitor_pixels));
        next_nonblack=false;
        offer_picture(monitor_pixels,NULL,0);
        next_nonblack=true;
        assert(lv_image_get_src(image));
        check_waiting_pixels(image,false);
        unsigned draws=0;
        assert(lv_obj_add_event_cb(image,count_draw,LV_EVENT_DRAW_MAIN,&draws));
        for (unsigned i=0;i<10;i++) { pack_desktop_tick(NULL);lv_test_wait(50); }
        assert(!draws);
        assert(lv_obj_remove_event_cb_with_user_data(image,count_draw,&draws));
        sim_click_obj(lv_obj_get_parent(visible_caption(screen,"Stop")));
        assert(!usb_controls && !lv_image_get_src(image));
    }
    check_viewport_size_change(screen,image);
    pack_desktop_close();
    ui_theme_set_control_palette(original);
    home();
    lv_obj_delete(screen);
}

static void console_view(const char *output)
{
    const unsigned caps=PACK_PERM_UI|PACK_PERM_MONITOR|PACK_PERM_MOUSE|PACK_PERM_KEYBOARD;
    assert(scene.permissions==caps && scene.screen_count==2 && scene.portrait);
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_set_scrollable(screen,false);
    lv_obj_t *frame=pack_view_frame(screen,true);
    ui_theme_apply(frame,UI_STYLE_CONTROL_PAGE,0);
    lv_obj_t *title=lv_label_create(frame);
    lv_label_set_text(title,scene.title);lv_obj_set_pos(title,12,4);
    lv_obj_t *content=lv_obj_create(frame);
    lv_obj_remove_style_all(content);lv_obj_set_scrollable(content,false);
    lv_obj_set_pos(content,0,32);lv_obj_set_size(content,600,992);
    assert(!pack_view_open(content,&scene,caps & ~PACK_PERM_MOUSE));
    assert(pack_view_open(content,&scene,caps));
    lv_screen_load(screen);sim_settle();pack_view_tick();sim_settle();
    assert(visible_caption(screen,"Start USB monitor") && !usb_changes && !monitor_opens);
    check_gesture_drawer(screen,false,output);
    assert(monitor_viewport_change_mode(true,NULL)==USB_MONITOR_MODE_STOPPED);
    assert(!pack_desktop_change_mode(true));
    screen_tap(true,36,712);assert(!reports);
    assert(!contacts(1,110,530,0,0));contacts(0,0,0,0,0);
    memory_available=false;screen_tap(true,300,285);
    assert(visible_caption(screen,"Memory unavailable  /  Retry") && !usb_changes);
    memory_available=true;screen_tap(true,300,285);
    assert(usb_controls && !receiver_native && monitor_opens==1 && usb_changes==1 && !usb_mounted);
    screen_tap(true,36,712);assert(!reports);
    usb_mounted=true;pack_view_tick();sim_settle();
    assert(visible_caption(screen,"Stop"));
    lv_obj_t *viewport=console_image_widget(screen);
    check_waiting_pixels(viewport,true);
    screen_tap(true,36,712);
    assert(reports==2 && recorded[0].type==KVM_HID_KEY_DOWN && recorded[1].type==KVM_HID_KEY_UP);
    screen_tap(true,590,966); // Last pixel columns/rows of the authored bottom-right key.
    assert(reports==4 && recorded[2].data.key.usage_id==0x4f && recorded[3].type==KVM_HID_KEY_UP);
    check_console_edge_motion();
    // Exercise the alternate adapter's portrait frame on a landscape LVGL display.
    lv_display_t *display=lv_obj_get_display(screen);
    lv_display_set_rotation(display,LV_DISPLAY_ROTATION_0);
    lv_obj_set_pos(frame,0,599);
    lv_obj_set_style_transform_pivot_x(frame,0,0);
    lv_obj_set_style_transform_pivot_y(frame,0,0);
    lv_obj_set_style_transform_rotation(frame,2700,0);
    sim_settle();
    check_console_edge_motion();
    lv_obj_set_style_transform_rotation(frame,0,0);
    lv_obj_set_pos(frame,0,0);
    lv_display_set_rotation(display,LV_DISPLAY_ROTATION_90);
    sim_settle();
    for (unsigned y=0;y<480;y++) for (unsigned x=0;x<640;x++) {
        uint16_t color=y<240 ? (x<320 ? 0xf800 : 0x07e0) : (x<320 ? 0x001f : 0xffff);
        if ((x>=12 && x<28) || (x>=612 && x<628)) color=0xffe0;
        if ((y>=10 && y<26) || (y>=454 && y<470)) color=0x07ff;
        monitor_pixels[y*640+x]=color;
    }
    picture_waiting=true;lv_test_wait(160);
    assert(!picture_waiting && monitor_flushes==1);
    console_image(output,"console");
    check_gesture_drawer(screen,true,output);
    assert(contacts(1,110,530,0,0));assert(contacts(1,150,530,0,0));assert(contacts(1,200,530,0,0));
    assert(recorded[reports-1].type==KVM_HID_MOUSE_MOVE && recorded[reports-1].data.mouse_move.dx>0);
    assert(contacts(0,0,0,0,0));

    unsigned n=reports;
    assert(contacts(2,160,530,240,530));assert(contacts(2,160,557,240,557));
    assert(contacts(2,160,590,240,590));assert(contacts(0,0,0,0,0));
    assert(reports>n && recorded[reports-1].type==KVM_HID_MOUSE_WHEEL && recorded[reports-1].data.wheel.vertical<0);
    n=reports;
    scroll_natural=true;
    assert(contacts(2,160,530,240,530));
    scroll_enabled=false; // A change during a gesture applies on the next contact.
    assert(contacts(2,160,569,240,569));assert(contacts(0,0,0,0,0));
    assert(reports>n && recorded[reports-1].type==KVM_HID_MOUSE_WHEEL && recorded[reports-1].data.wheel.vertical>0);
    n=reports;
    assert(contacts(2,160,530,240,530));assert(contacts(2,160,569,240,569));assert(contacts(0,0,0,0,0));
    assert(reports==n);
    scroll_enabled=true;scroll_natural=false;
    int delta[3];
    for (unsigned speed=0;speed<3;++speed) {
        pointer_speed=(kvm_gesture_speed_t)speed;
        assert(contacts(1,120,530,0,0));
        pointer_speed=KVM_GEST_SPEED_NORMAL;
        assert(contacts(1,132,530,0,0));
        assert(recorded[reports-1].type==KVM_HID_MOUSE_MOVE);
        delta[speed]=recorded[reports-1].data.mouse_move.dx;
        assert(contacts(0,0,0,0,0));
    }
    assert(delta[0]>0 && delta[0]<delta[1] && delta[1]<delta[2]);
    for (unsigned enabled=0;enabled<2;++enabled) {
        n=reports;
        hold_right=enabled!=0;
        assert(contacts(1,120,530,0,0));
        hold_right=!hold_right;
        for (unsigned i=0;i<60;++i) assert(contacts(1,120,530,0,0));
        assert(contacts(0,0,0,0,0));
        assert(reports==n+2*enabled);
        if (enabled) {
            assert(recorded[n].type==KVM_HID_MOUSE_BTN_DOWN && recorded[n].data.mouse_button.button_id==KVM_MOUSE_BTN_RIGHT);
            assert(recorded[n+1].type==KVM_HID_MOUSE_BTN_UP && recorded[n+1].data.mouse_button.button_id==KVM_MOUSE_BTN_RIGHT);
        }
    }
    hold_right=true;
    n=reports;
    assert(contacts(1,110,530,0,0));assert(contacts(2,110,530,200,710));
    assert(!contacts(2,110,570,200,610));contacts(0,0,0,0,0);
    assert(reports==n);
    check_pan_navigation(screen,true);
    n=reports;
    assert(usb_changes==1 && monitor_opens==1 && !monitor_closes);
    console_image(output,"pointer");
    check_local_console_damage(screen,false);
    assert(contacts(1,120,800,0,0));assert(contacts(1,180,800,0,0));assert(contacts(1,220,800,0,0));
    assert(reports>n && recorded[reports-1].type==KVM_HID_MOUSE_MOVE);
    assert(contacts(0,0,0,0,0));
    zoom_console(screen);screen_press(true,270,180);screen_pointer(true,288,192);sim_release();
    screen_tap(true,300,160);
    assert(!visible_caption(screen,"Stop"));
    usb_mounted=false;pack_view_tick();sim_settle();
    assert(visible_caption(screen,"Waiting for USB  /  Stop"));
    n=reports;
    assert(!contacts(1,120,800,0,0));usb_mounted=true;pack_view_tick();sim_settle();
    assert(!contacts(1,130,800,0,0));contacts(0,0,0,0,0);
    assert(visible_caption(screen,"Stop") && reports==n);
    lv_obj_t *restored=console_image_widget(screen);
    assert(visible_caption(screen,"1:1") && lv_image_get_inner_align(restored)==LV_IMAGE_ALIGN_CONTAIN &&
           !lv_image_get_offset_x(restored) && !lv_image_get_offset_y(restored));
    zoom_console(screen);
    screen_tap(true,545,62);
    assert(!usb_controls && monitor_closes==1 && usb_changes==2);
    screen_tap(true,370,233);
    memory_available=false;screen_tap(true,300,285);
    assert(visible_caption(screen,"Memory unavailable  /  Retry") && usb_changes==2);
    memory_available=true;screen_tap(true,300,285);
    assert(usb_controls && receiver_native && monitor_opens==2 && usb_changes==3);
    usb_mounted=true;pack_view_tick();sim_settle();
    assert(visible_caption(screen,"Stop") && visible_caption(screen,"1:1") &&
           lv_image_get_inner_align(restored)==LV_IMAGE_ALIGN_CONTAIN &&
           !lv_image_get_offset_x(restored) && !lv_image_get_offset_y(restored));
    for (unsigned i=0;i<1024*600;i++) monitor_pixels[i]=(uint16_t)(i^(i>>13));
    offer_picture(monitor_pixels,NULL,0);
    check_pan_navigation(screen,false);
    assert(visible_caption(screen,"Stop") && usb_changes==3);
    check_local_console_damage(screen,true);
    n=reports;
    screen_tap(true,590,966);
    assert(reports==n+2 && recorded[n].data.key.usage_id==0x4f);
    unsigned changes=usb_changes,opens=monitor_opens,closes=monitor_closes;
    assert(pack_desktop_change_mode(true) && usb_changes==changes && monitor_opens==opens);
    screen_press(true,36,712);
    assert(recorded[reports-1].type==KVM_HID_KEY_DOWN);
    lv_indev_t *pointer=lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER);
    lv_point_t point;
    lv_indev_get_point(pointer,&point);
    lv_obj_t *key_button=lv_indev_search_obj(screen,&point);
    assert(key_button && lv_obj_check_type(key_button,&lv_button_class));
    assert(pack_keyboard_surface_buffered_target(key_button));
    lv_obj_t *key_surface=lv_obj_get_parent(key_button);
    assert(!pack_keyboard_surface_buffered_target(key_surface));
    assert(lv_obj_get_width(key_surface)==584 && lv_obj_get_height(key_surface)==288);
    assert(lv_obj_get_width(key_button)>=44 && lv_obj_get_height(key_button)>=44);
    assert(has_cap(key_surface,"Caps\nUnknown"));
    led_known=true;host_leds=2;pack_view_tick();
    assert(has_cap(key_surface,"Caps\nOn"));
    assert(!pack_keyboard_surface_buffered_target(screen));
    unsigned generation=snap.release_generation;
    lv_indev_reset(pointer,NULL);
    assert(snap.release_generation>generation);
    sim_release();
    n=reports;
    screen_press(true,36,712);
    assert(reports==n+1 && recorded[n].type==KVM_HID_KEY_DOWN);
    generation=snap.release_generation;
    assert(pack_desktop_change_mode(false) && !receiver_native && snap.release_generation>generation);
    assert(usb_mounted && !pack_desktop_ready()); // The previous USB mount is still reported.
    pack_view_tick();
    assert(has_cap(key_surface,"Caps\nUnknown"));
    n=reports;
    sim_release();screen_tap(true,36,712);
    assert(reports==n && !pack_desktop_change_mode(true));
    // No poll saw the intermediate detach. The new mount still unblocks local input.
    led_known=false;host_leds=0;mount_epoch++;pack_view_tick();sim_settle();
    assert(pack_desktop_ready() && visible_caption(screen,"Stop") && usb_changes==changes+1);
    check_waiting_pixels(viewport,true);
    assert(has_cap(key_surface,"Caps\nUnknown"));
    led_known=true;pack_view_tick();
    assert(has_cap(key_surface,"Caps\nOff"));
    assert(contacts(1,120,530,0,0));assert(contacts(1,155,530,0,0));
    failed_allocations=1;
    assert(!pack_desktop_change_mode(true) && !receiver_native && usb_controls && usb_changes==changes+1);
    assert(!contacts(1,200,530,0,0));contacts(0,0,0,0,0);
    failed_profiles=1;
    assert(!pack_desktop_change_mode(true) && !receiver_native && pack_desktop_ready());
    assert(pack_desktop_change_mode(true) && receiver_native);
    mount_epoch++;pack_view_tick();sim_settle();
    offer_picture(monitor_pixels,NULL,0);
    assert(visible_caption(screen,"Stop") && lv_screen_active()==screen);
    assert(pack_desktop_change_mode(false));
    usb_mounted=false;
    assert(pack_desktop_change_mode(true) && receiver_native && !pack_desktop_ready());
    usb_mounted=true;mount_epoch++;pack_view_tick();
    assert(pack_desktop_ready());
    failed_allocations=2;
    assert(!pack_desktop_change_mode(false) && !pack_desktop_ready() && !usb_controls);
    assert(visible_caption(screen,"USB monitor unavailable  /  Retry"));
    assert(!pack_desktop_change_mode(true));
    screen_tap(true,300,285);
    assert(usb_controls && receiver_native && monitor_closes>closes);
    changes=usb_changes;closes=monitor_closes;
    screen_tap(true,300,1000);pack_view_tick();screen_tap(true,300,978);
    assert(system_exits==1 && lv_screen_active()==home_screen);pack_view_tick();
    assert(!usb_controls && usb_changes==changes+1 && monitor_closes==closes+1);
    assert(!pack_desktop_change_mode(false));
    lv_obj_delete(screen);
    assert(!control_bytes);
    minimum_monitor_view();
    puts("PASS two portrait console pages, RGB565 corners and cached frame, local Start with memory failure, "
         "mount, keyboard, pointer, two-contact scroll, off-pad rejection, navigation release, unplug and Home.");
}
#endif

static void switcher_view(const char *folder,bool portrait)
{
    load_at(folder);
    assert(scene.portrait==portrait && scene.screen_count==2);
    const int w=portrait ? 600 : 1024, h=portrait ? 1024 : 600;
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_set_scrollable(screen,false);
    lv_obj_t *frame=pack_view_frame(screen,portrait);
    ui_theme_apply(frame,UI_STYLE_CONTROL_PAGE,0);
    lv_obj_t *content=lv_obj_create(frame);
    lv_obj_remove_style_all(content); lv_obj_set_scrollable(content,false);
    lv_obj_set_pos(content,0,32); lv_obj_set_size(content,w,h-32);
    assert(pack_view_open(content,&scene,scene.permissions));
    lv_screen_load(screen); sim_settle(); pack_view_tick();
    const unsigned previous_exits=system_exits;
    assert(visible_caption(content,"Standby") && !visible_caption(content,"Setup controls"));
    assert(!visible_caption(content,"Return to KVMpad"));
    screen_tap(portrait,110,102);
    assert(visible_caption(content,"121.500 MHz"));
    // A touch on the system exit location while a page is active cannot leave the extension.
    screen_tap(portrait,w/2,h-65);
    assert(lv_screen_active()==screen && visible_caption(content,"Tune") && system_exits==previous_exits);

    screen_press(portrait,w/2,h-49);
    screen_pointer(portrait,w/2,h-128);
    sim_release();
    assert(visible_caption(content,"Tune") && !visible_caption(content,"Return to KVMpad"));
    screen_press(portrait,w/2,h-3);
    screen_pointer(portrait,w/2+100,h-13);
    sim_release();
    assert(visible_caption(content,"Tune") && !visible_caption(content,"Return to KVMpad"));
    screen_press(portrait,w/2,h-2);
    screen_pointer(portrait,w/2,h-75);
    lv_indev_wait_release(lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER));
    sim_release();
    assert(visible_caption(content,"Tune") && !visible_caption(content,"Return to KVMpad"));
    screen_press(portrait,w/2,h-2);
    screen_pointer(portrait,w/2,h-38);
    sim_release();
    assert(visible_caption(content,"Tune") && !visible_caption(content,"Return to KVMpad"));

    screen_press(portrait,w/2,h-1);
    screen_pointer(portrait,w/2,h-100);
    assert(visible_caption(content,"Tune"));
    sim_release();
    assert(visible_caption(content,"Radio") && visible_caption(content,"Setup") && visible_caption(content,"Return to KVMpad"));
    assert(!visible_caption(content,"Tune") && !visible_caption(content,"Setup controls"));
    picture(portrait ? "screens-portrait-native" : "screens-landscape");
    screen_tap(portrait,3*w/4,126);
    assert(visible_caption(content,"Setup controls") && !visible_caption(content,"Tune"));
    assert(!visible_caption(content,"Return to KVMpad") && system_exits==previous_exits);
    screen_tap(portrait,w/2,h-1);
    assert(visible_caption(content,"Radio") && visible_caption(content,"Return to KVMpad"));
    screen_tap(portrait,w/4,126);
    assert(visible_caption(content,"121.500 MHz") && !visible_caption(content,"Setup controls"));
    screen_tap(portrait,w/2,h-1);
    screen_tap(portrait,w/2,h-48);
    assert(system_exits==previous_exits+1 && lv_screen_active()==home_screen);
    pack_view_close(); lv_obj_delete(screen);
    printf("PASS %s: edge swipe, tap, short/sideways/cancelled drag, two screens, retained state, system exit only in picker\n",
           portrait ? "portrait" : "landscape");
}

static void switch_to_autopilot(void)
{
    memset(&scene,0,sizeof(scene));
    scene.portrait=true; scene.screen_count=2; scene.widgets=4;
    scene.permissions=scene.required_permissions=PACK_PERM_UI;
    scene.max_memory_kib=256;
    scene.widget[0]=(pack_widget_t){.kind=PACK_CONTAINER,.parent=-1,.w=600,.h=944};
    scene.widget[1]=(pack_widget_t){.kind=PACK_CONTAINER,.parent=0,.w=600,.h=944,.text="Notes"};
    scene.widget[2]=(pack_widget_t){.kind=PACK_CONTAINER,.parent=0,.w=600,.h=944,.initial=1,.text="Autopilot"};
    scene.widget[3]=(pack_widget_t){.kind=PACK_AUTOPILOT,.parent=2,.x=8,.y=8,.w=584,.h=928};
    lv_obj_t *screen=lv_obj_create(NULL), *frame=pack_view_frame(screen,true);
    lv_obj_t *content=lv_obj_create(frame);
    lv_obj_set_scrollable(screen,false); lv_obj_set_scrollable(content,false);
    lv_obj_remove_style_all(content); lv_obj_set_pos(content,0,32); lv_obj_set_size(content,600,992);
    assert(pack_view_open(content,&scene,scene.permissions));
    lv_screen_load(screen); sim_settle(); pack_view_tick();
    screen_tap(true,300,1023);
    assert(visible_caption(content,"Return to KVMpad"));
    screen_tap(true,450,128);
    assert(!visible_caption(content,"Return to KVMpad"));
    // Navigation and the first refresh must work even before the next 50 ms pack poll.
    assert(visible_caption(content,"Start demo"));
    screen_tap(true,300,1023);
    assert(visible_caption(content,"Return to KVMpad") && !visible_caption(content,"Start demo"));
    screen_tap(true,150,128);
    assert(!visible_caption(content,"Return to KVMpad") && !visible_caption(content,"Start demo"));
    home(); pack_view_close(); lv_obj_delete(screen);
}

static void chart_controls(void)
{
    // Odd source widths, transparent letterbox, short and 16-pixel runs. Each chart's
    // two halves use distinct colors so a screenshot identifies the slot and its camera.
    static uint8_t runs[3][15];
    static const uint32_t colors[][2]={{0xffff0000,0xff00ff00},{0xff0000ff,0xffffff00},{0xffff00ff,0xff00ffff}};
    memset(&scene,0,sizeof(scene));
    scene.widgets=2; scene.permissions=scene.required_permissions=PACK_PERM_UI;
    scene.max_memory_kib=128; scene.events_per_second=20;
    scene.widget[0]=(pack_widget_t){.kind=PACK_CONTAINER,.parent=-1,.w=541,.h=421};
    scene.widget[1]=(pack_widget_t){.kind=PACK_CHART,.parent=0,.w=541,.h=421,.tab_count=3};
    for (unsigned i=0;i<3;i++) {
        pack_chart_t *c=&scene.charts[i];
        snprintf(c->title,sizeof(c->title),"Chart %u",i+1);
        c->width=33; c->height=5; c->runs=runs[i]; c->run_count=sizeof(runs[i]);
        memcpy(c->palette,colors[i],sizeof(colors[i]));
        c->palette[3]=0x8000ff00;
        for (int row=0;row<5;row++) {
            runs[i][3*row]=0xf0; runs[i][3*row+1]=0xf1; runs[i][3*row+2]=1;
        }
        runs[i][13]=0xf2;  // Transparent run and one half-alpha edge pixel on the last row.
        runs[i][14]=3;
    }
    assert(ui_theme_init());
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_remove_style_all(screen); ui_theme_apply(screen,UI_STYLE_CONTROL_PAGE,0);
    lv_obj_set_scrollable(screen,false); lv_screen_load(screen);
    --scene.charts[2].run_count;
    assert(!pack_view_open(screen,&scene,scene.permissions));
    assert(!control_bytes && lv_obj_get_child_count(screen)==0);
    ++scene.charts[2].run_count;
    assert(pack_view_open(screen,&scene,scene.permissions));
    sim_settle(); pack_view_tick();
    assert(visible_caption(screen,"100%"));
    sim_image_t image; assert(sim_image_render(screen,&image));
    const uint8_t *left=image.rgb+3*(175*image.w+1), *right=image.rgb+3*(175*image.w+540);
    assert(left[0]==255 && !left[1] && !left[2] && !right[0] && right[1]==255 && !right[2]);
    const uint8_t *background=image.rgb+3*(123*image.w+350), *clear=image.rgb+3*(211*image.w+350);
    const uint8_t *half=image.rgb+3*(211*image.w+540);
    assert(!memcmp(background,clear,3));
    for (unsigned j=0;j<3;j++) assert(abs(2*half[j]-right[j]-clear[j])<25);
    sim_image_free(&image);
    // At Fit the whole chart is visible. A drag cannot change a single pixel.
    unsigned flushes=0;
    lv_display_t *display=lv_obj_get_display(screen);
    lv_display_add_event_cb(display,count_flush,LV_EVENT_FLUSH_START,&flushes);
    sim_press_point(190,167);lv_test_wait(150);
    unsigned before=flushes;
    lv_test_mouse_move_to(420,170);lv_test_wait(100);
    assert(flushes==before);
    sim_release();
    lv_display_remove_event_cb_with_user_data(display,count_flush,&flushes);
    lv_point_t pair[2]={{355,24},{405,175}};
    assert(!pack_chart_view_touch(2,pair));  // One finger started on slot B.
    pair[0]=(lv_point_t){305,175};
    assert(!pack_chart_view_touch(2,pair) && !pack_chart_view_touch(0,NULL));
    assert(pack_chart_view_touch(2,pair));
    pair[0].x=255; pair[1].x=455;
    assert(pack_chart_view_touch(2,pair) && visible_caption(screen,"200%"));
    assert(sim_image_render(screen,&image));
    // Zoom around x=355 moves the color boundary from x=263 to x=171.
    left=image.rgb+3*(175*image.w+150); right=image.rgb+3*(175*image.w+210);
    assert(left[0]==255 && !left[1] && !right[0] && right[1]==255);
    sim_image_free(&image);
    assert(pack_chart_view_touch(1,pair));
    sim_press_point(190,167);lv_test_mouse_move_to(420,170);lv_test_wait(100);sim_release();
    assert(sim_image_render(screen,&image));
    left=image.rgb+3*(175*image.w+150);right=image.rgb+3*(175*image.w+210);
    assert(left[0]==255 && !left[1] && !right[0] && right[1]==255);
    sim_image_free(&image);
    pair[0].x=10; pair[1].x=520;
    assert(pack_chart_view_touch(2,pair) && visible_caption(screen,"200%"));
    assert(!pack_chart_view_touch(0,NULL));
    pair[0].x=300; pair[1].x=400;
    assert(pack_chart_view_touch(2,pair));
    pair[1].x=371;
    assert(pack_chart_view_touch(2,pair) && visible_caption(screen,"142%"));
    assert(pack_chart_view_touch(3,NULL) && pack_chart_view_touch(1,pair));
    assert(!pack_chart_view_touch(0,NULL));
    tap(230,329); assert(visible_caption(screen,"150%"));
    tap(35,329); assert(visible_caption(screen,"125%"));
    tap(470,328); assert(visible_caption(screen,"100%"));
    for (unsigned i=0;i<8;i++) tap(230,329);
    assert(visible_caption(screen,"600%"));
    sim_press_point(190,167);lv_test_mouse_move_to(420,170);lv_test_wait(80);sim_release();
    // Switch to B, assign C to B, then choose C for A, exchanging the two pinned cards.
    tap(400,24); assert(visible_caption(screen,"100%"));
    tap(260,395);assert(visible_caption(screen,"Choose chart for slot B"));
    tap(210,215);
    tap(123,24); assert(visible_caption(screen,"600%"));
    tap(260,395);tap(210,215);
    assert(visible_caption(screen,"A  Chart 3") && visible_caption(screen,"B  Chart 1"));
    tap(400,24);assert(visible_caption(screen,"600%"));
    tap(470,328);assert(visible_caption(screen,"100%"));
    tap(260,395);tap(240,284);assert(!visible_caption(screen,"Cancel"));
    pack_view_close(); home(); lv_obj_delete(screen); ui_theme_deinit();
    puts("PASS chart controls, slots, camera retention, alpha, Fit drag with zero flushes, anchored pinch, off-chart rejection, 2-to-1 and extra-contact cancellation");
}

static uint16_t *portrait_output;
static unsigned portrait_flushes;

static void portrait_flush(lv_display_t *display,const lv_area_t *a,uint8_t *pixels)
{
    unsigned width=lv_area_get_width(a);
    uint32_t stride=lv_display_get_buf_active(display)->header.stride;
    for (int y=a->y1;y<=a->y2;y++,pixels+=stride)
        memcpy(portrait_output+(size_t)y*1024+a->x1,pixels,width*sizeof(uint16_t));
    portrait_flushes++;
    lv_display_flush_ready(display);
}

static void portrait_strips(lv_obj_t *screen,const sim_image_t *reference)
{
    lv_display_t *display=lv_obj_get_display(screen);
    lv_draw_buf_t *original=lv_display_get_buf_active(display);
    lv_display_flush_cb_t flush=lv_display_get_flush_cb(display);
    lv_display_render_mode_t mode=lv_display_get_render_mode(display);
    uint8_t *strip=malloc(1024*12*2+LV_DRAW_BUF_ALIGN);
    portrait_output=malloc(1024*600*2);
    assert(strip && portrait_output);
    lv_display_set_flush_cb(display,portrait_flush);
    for (unsigned rows=8; rows<=12; rows+=4) {
        memset(portrait_output,0xda,1024*600*2);
        portrait_flushes=0;
        void *aligned=lv_draw_buf_align(strip,LV_COLOR_FORMAT_RGB565);
        lv_display_set_buffers(display,aligned,NULL,1024*rows*2,LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_obj_invalidate(screen);
        lv_refr_now(display);
        unsigned mismatch=0;
        for (size_t i=0;i<1024*600;i++) {
            const uint8_t *rgb=reference->rgb+3*i;
            uint16_t expected=((rgb[0]>>3)<<11)|((rgb[1]>>2)<<5)|(rgb[2]>>3);
            mismatch+=portrait_output[i]!=expected;
        }
        printf("Portrait partial RGB565, %u scanlines, %u flushes, %u differing pixels of 614400\n",
               rows,portrait_flushes,mismatch);
        fflush(stdout);
        assert(!mismatch && portrait_flushes==(600+rows-1)/rows);
    }
    lv_display_set_draw_buffers(display,original,NULL);
    lv_display_set_render_mode(display,mode);
    lv_display_set_flush_cb(display,flush);
    free(strip); free(portrait_output); portrait_output=NULL;
}

static void portrait_view(const char *folder)
{
    load_at(folder);
    assert(scene.portrait && ui_theme_init());
    lv_obj_t *screen=lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    lv_obj_t *frame=pack_view_frame(screen,true);
    ui_theme_apply(frame,UI_STYLE_CONTROL_PAGE,LV_STATE_DEFAULT);
    lv_obj_set_scrollable(screen,false);
    lv_obj_t *content=lv_obj_create(frame);
    lv_obj_remove_style_all(content);
    lv_obj_set_scrollable(content,false);
    lv_obj_set_size(content,600,992);
    lv_obj_set_pos(content,0,32);
    assert(pack_view_open(content,&scene,scene.permissions));
    const int lx[]={0,599,0,599}, ly[]={0,0,1023,1023};
    const uint32_t colors[]={0xff0000,0x00ff00,0x0000ff,0xffffff};
    for (int i=0;i<4;i++) {
        lv_obj_t *mark=lv_obj_create(frame);
        lv_obj_remove_style_all(mark);
        lv_obj_set_size(mark,4,4);
        lv_obj_set_pos(mark,lx[i] ? 596 : 0,ly[i] ? 1020 : 0);
        lv_obj_set_style_bg_color(mark,lv_color_hex(colors[i]),0);
        lv_obj_set_style_bg_opa(mark,LV_OPA_COVER,0);
        lv_obj_set_clickable(mark,false);
    }
    lv_screen_load(screen); sim_settle(); pack_view_tick();
    picture("portrait-native");
    const int px[]={0,0,1023,1023}, py[]={599,0,599,0};
    sim_image_t img;
    assert(sim_image_render(screen,&img));
    portrait_strips(screen,&img);
    for (int i=0;i<4;i++) {
        lv_point_t raw={px[i],py[i]}, logical;
        pack_view_logical_point(true,&raw,&logical);
        assert(logical.x==lx[i] && logical.y==ly[i]);
        const uint8_t *rgb=img.rgb+3*(py[i]*img.w+px[i]);
        unsigned color=((unsigned)rgb[0]<<16)|(rgb[1]<<8)|rgb[2];
        if (color!=colors[i]) fprintf(stderr,"corner %d pixel %06x expected %06x\n",i,color,(unsigned)colors[i]);
        assert(color==colors[i]);
    }
    sim_image_free(&img);
    const char *names[]={"NW","NE","SW","SE"};
    for (int i=0;i<4;i++) {
        tap(i<2 ? 52 : 1004,i%2 ? 19 : 579);
        if (strcmp(scene.state[0].value.text,names[i])) {
            lv_obj_t *b=lv_obj_get_child(lv_obj_get_child(content,0),i);
            lv_point_t p={i<2 ? 52 : 1004,i%2 ? 19 : 579};
            lv_obj_t *hit=lv_indev_search_obj(screen,&p);
            fprintf(stderr,"tap %s got '%s', visible=%d target=%p expected=%p\n",names[i],scene.state[0].value.text,lv_obj_is_visible(b),(void*)hit,(void*)b);
        }
        assert(!strcmp(scene.state[0].value.text,names[i]));
    }
    sim_press_point(260,539);
    lv_test_mouse_move_to(280,449); lv_test_wait(100); sim_release();
    assert(scene.state[1].value.integer==110);
    lv_obj_t *root=lv_obj_get_child(content,0);
    lv_obj_t *arc=lv_obj_get_child(lv_obj_get_child(root,6),1);
    lv_area_t a; lv_obj_get_coords(arc,&a);
    int x=(a.x1+a.x2)/2, y=(a.y1+a.y2)/2-599, r=(a.x2-a.x1)/2-5;
    sim_press_point(y-r,599-x); lv_test_wait(100);
    lv_test_mouse_move_to(y,599-x-r); lv_test_wait(100); sim_release();
    assert(lv_arc_get_value(arc)==66 && scene.state[1].value.integer==66);
    home(); pack_view_close(); lv_obj_delete(screen);
    lv_display_t *display=lv_display_get_default();
    assert(lv_display_get_horizontal_resolution(display)==1024 && lv_display_get_vertical_resolution(display)==600);
    assert(pack_view_frame(home_screen,false)==home_screen);
    ui_theme_deinit();
    puts("PASS clockwise portrait, physical corner pixels and four button actions, rotated drag and dial, original landscape restored");
}

int main(int argc, char **argv)
{
    size_t baseline=sim_env_start();
    home_screen=lv_screen_active();
    if (argc==1 || (argc==2 && !strcmp(argv[1],"--rate-slider"))) {
        rate_slider_view(KVM_EXAMPLES "/../samples/rate-slider");
        if (argc==2) { sim_env_stop(baseline); return 0; }
        memset(&scene,0,sizeof(scene));
    }
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (argc==4 && !strcmp(argv[1],"--usb-console")) {
        assert(ui_theme_init());load_at(argv[2]);console_view(argv[3]);
        ui_theme_deinit();sim_env_stop(baseline);return 0;
    }
#endif
    if (argc==2 && !strcmp(argv[1],"--charts")) {
        chart_controls();
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==4 && !strcmp(argv[1],"--screens")) {
        assert(ui_theme_init());
        switcher_view(argv[2],false);
        switcher_view(argv[3],true);
        switch_to_autopilot();
        ui_theme_deinit();
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--portrait")) {
        portrait_view(argv[2]);
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--input-palettes")) {
        input_palettes(argv[2]);
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--button-icons")) {
        button_icons();
        plain_button_palettes();
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==7 && !strcmp(argv[1],"--magnifier-modes")) {
        magnifier_modes(argv[2],&argv[3]);
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--controls")) {
        assert(ui_theme_init());
        local_controls_view(argv[2]);
        ui_theme_deinit();
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--chips")) {
        chip_palettes(argv[2]);
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--state")) {
        local_state_view(argv[2]);
        sim_env_stop(baseline);
        return 0;
    }
    if (argc==3 && !strcmp(argv[1],"--telemetry")) {
        telemetry_view(argv[2]);
        sim_env_stop(baseline);
        return 0;
    }
    const char *const examples[]={"flight-yoke","flight-instruments","cursor-magnifier","flight-checklist"};
    if (argc==2 && !strcmp(argv[1],"--instrument-quality")) {
        pack(1,examples[1]);
        sim_env_stop(baseline);
        puts("PASS real LVGL Flight Instruments: stale, data error, unavailable, recovery and all three palettes");
        return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--pack-cycles")) {
        activation_cycles(examples);
        sim_env_stop(baseline);
        return 0;
    }
    for (unsigned i=0;i<4;i++) pack(i,examples[i]);
    assert(stops==1 && starts==1);
    assert(hpe_panel_open(home));
    sim_settle();
    picture("dial");
    tap(510,286);
    assert(recorded[reports-2].type==KVM_HID_RADIAL && recorded[reports-2].data.radial.pressed &&
           !recorded[reports-1].data.radial.pressed);
    tap(630,568);
    sim_settle();
    picture("studio");
    tap(140,120);
    assert(recorded[reports-1].type==KVM_HID_CHORD);
    tap(900,20);
    sim_settle();
    unsigned n = reports;
    tap(120,240);
    assert(reports==n && profile==HPE_STUDIO_PROFILE);
    studio=(hpe_studio_status_t){.peer_nonce=49,.supported=7,.bank=7,.state={2,1,1},.label={"Studio capture","Stop recording","Mute input"}};
    lv_test_wait(75);
    tap(120,240);
    assert(reports==n+1 && recorded[n].type==KVM_HID_STUDIO_ACTION &&
           recorded[n].data.studio.action_id==0x101 && recorded[n].data.studio.peer_nonce==49);
    studio.phase=HPE_ACCEPTED;
    studio.tile_id=1;
    lv_test_wait(75);
    tap(440,240);
    assert(reports==n+1);
    studio.result=6;
    studio.phase=HPE_FINISHED;
    lv_test_wait(75);
    lv_test_wait(200);
    picture("studio-obs");
    tap(885,568);
    sim_settle();
    picture("access");
    tap(80,130);
    assert(keys==1);
    tap(365,415); // dwell on
    sim_press_point(80,130);
    lv_test_wait(2000);
    sim_release();
    assert(keys==2);
    tap(80,415); // enable scan; latch the first key even across the interval
    sim_press_point(221,415);
    lv_test_wait(1350);
    sim_release();
    assert(keys==3 && last_key==KVM_KEY_TAB);
    sim_press_point(221,415); // cooldown suppresses a bounce
    sim_release();
    assert(keys==3);
    lv_test_wait(400);
    sim_press_point(221,415);
    lv_test_wait(2120); // hold Stop never emits another key
    sim_release();
    assert(keys==3);
    tap(900,20);
    sim_settle();
    tap(820,90);  // scan +100
    tap(475,175); // dwell -100
    assert(!saves);
    tap(810,454); // Apply timing, via the config writer contract
    assert(saves==1 && hpe_access_unpack(timing_word).scan_ms==1300 && hpe_access_unpack(timing_word).dwell_ms==800);
    assert(hpe_access_unpack(UINT64_MAX).scan_ms==1200 && hpe_access_unpack(0).dwell_ms==900);
    picture("access-timing");
    tap(108,568);
    sim_settle();
    assert(profile==HPE_MOUSE_PROFILE);
    assert(hpe_panel_open(home));
    sim_settle();
    size_t warm_bytes = 0;
    unsigned saved_reports=reports, saved_keys=keys;
    for (unsigned i=0;i<100;i++) {
        tap(630,568); // Studio
        tap(885,568); // Access
        tap(385,568); // Dial
        sim_settle();
        if (i == 0) warm_bytes=host_lv_live_bytes();
    }
    sim_settle();
    assert(profile==HPE_DIAL_PROFILE && reports==saved_reports && keys==saved_keys);
    fprintf(stderr,"Profile-switch heap after one and 100 cycles: %zu / %zu bytes\n",warm_bytes,host_lv_live_bytes());
    assert(host_lv_live_bytes()==warm_bytes && saves==1);
    tap(108,568);
    sim_settle();
    activation_cycles(examples);
    sim_env_stop(baseline);
    puts("PASS profile views, memory limits, pointer playback, typed intents and foreground grants");
    return 0;
}
