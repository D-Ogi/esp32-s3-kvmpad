// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "esp_heap_caps.h"
#include "hid_dispatch.h"
#include "kvmbridge.h"
#include "pack_view.h"
#include "sim_platform.h"
#include "usb_hid_reports.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pack_scene_t scene;
static kvm_hid_core_t core;
static bool pending[KVM_HID_ITF_COUNT];
static uint8_t wire[12];
static unsigned frames;
static uint64_t now(void) { return (uint64_t)lv_tick_get()*1000; }

// The USB sink exists only in process memory. Retire reports on the next tick.
static bool send_report(void *ctx, uint8_t itf, const uint8_t *p, uint8_t n)
{
    (void)ctx;
    assert(itf<KVM_HID_ITF_COUNT && !pending[itf]);
    pending[itf]=true;
    if (itf==3 && p[0]==0x30) {
        assert(n==sizeof(wire));
        memcpy(wire,p,n);
        frames++;
    }
    return true;
}
static void poll(lv_timer_t *t)
{
    (void)t;
    for (uint8_t i=0;i<KVM_HID_ITF_COUNT;i++) if (pending[i]) {
        pending[i]=false;
        kvm_hid_core_on_report_complete(&core,i);
    }
    kvm_hid_core_poll(&core,now());
}
esp_err_t hid_dispatch_local(const kvm_hid_intent_t *i) { return kvm_hid_core_local(&core,i,now()); }
void hid_dispatch_get_snapshot(kvm_hid_snapshot_t *out,uint32_t *seq)
{
    kvm_hid_core_snapshot(&core,out);
    if (seq) *seq=0;
}
void hid_dispatch_release_all(kvm_release_reason_t r) { kvm_hid_core_release_all(&core,r); }
void *heap_caps_calloc(size_t n,size_t z,unsigned caps) { (void)caps; return calloc(n,z); }
void heap_caps_free(void *p) { free(p); }
void pack_panel_magnify(const char *mode) { (void)mode; assert(0); }
void pack_panel_leave(void) { assert(0); }
void kvmbridge_stop(void) { assert(0); }
void kvmbridge_stop_display(const char *p,uint32_t epoch) { (void)p; (void)epoch; assert(0); }
bool kvmbridge_transfer_telemetry(const char *p) { (void)p; return false; }
bool kvmbridge_instruments_for_profile(const char *id,bool xp,unsigned channels,bool composite)
{ (void)id; (void)xp; (void)channels; (void)composite; assert(0); return false; }
bool kvmbridge_sample(unsigned ch,kvmbridge_sample_t *out) { (void)ch; (void)out; assert(0); return false; }

static bool load(const char *folder)
{
    static char buf[65536];
    static kvm_json_tok_t tokens[8192];
    const char *const docs[]={"manifest","ui","bindings"};
    for (unsigned i=0;i<3;i++) {
        char path[1024];
        snprintf(path,sizeof(path),"%s/%s.json",folder,docs[i]);
        FILE *f=fopen(path,"rb");
        assert(f);
        size_t n=fread(buf,1,sizeof(buf),f);
        assert(!ferror(f) && feof(f)); fclose(f);
        kvm_json_doc_t d;
        if (kvm_json_parse_local(&d,buf,n,tokens,8192,24)!=KVM_JSON_OK ||
            !(i==0 ? pack_scene_manifest(&scene,&d) : i==1 ? pack_scene_ui(&scene,&d) : pack_scene_bindings(&scene,&d)))
            return false;
    }
    return true;
}
static int16_t axis(unsigned byte) { return (int16_t)(wire[byte] | wire[byte+1]<<8); }
static void neutral(void)
{
    assert(!memcmp(wire,(uint8_t[12]){0x30,0,0,0,0,0,0,0,0,0,0,8},12));
}

int main(int argc,char **argv)
{
    const char *folder=argc>1 ? argv[1] : KVM_EXAMPLES "/flight-yoke";
    bool ok=load(folder);
    if (argc==3 && !strcmp(argv[2],"--validate")) return ok ? 0 : 1;
    assert(ok);
    bool calibrated=argc==3 && !strcmp(argv[2],"--calibrated");
    size_t baseline=sim_env_start();
    lv_obj_t *base=lv_screen_active(), *scr=lv_obj_create(NULL), *content=lv_obj_create(scr);
    lv_obj_remove_style_all(content); lv_obj_set_pos(content,0,32); lv_obj_set_size(content,1024,568);
    kvm_hid_port_t port={.send=send_report};
    kvm_hid_core_init(&core,&port,NULL);
    lv_timer_t *timer=lv_timer_create(poll,2,NULL);
    kvm_hid_core_set_usb(&core,KVM_HOST_USB_MOUNTED,1,now()); lv_test_wait(100);
    neutral();
    unsigned start=frames;
    assert(!pack_view_open(content,&scene,PACK_PERM_UI));
    assert(pack_view_open(content,&scene,scene.permissions));
    lv_screen_load(scr); pack_view_tick(); sim_settle();
    assert(frames==start); // No throttle or initial joystick action on activation.
    const int cols[]={1,2,2,2,1,0,0,0}, rows[]={0,0,1,2,2,2,1,0};
    for (int direction=0;direction<8;direction++) {
        unsigned before=frames;
        lv_point_t point={719+114*cols[direction],439+56*rows[direction]};
        sim_press_point(point.x,point.y);
        assert(frames==before+1 && wire[11]==direction);
        if (direction==3) {
            lv_obj_t *cancel_target=lv_indev_search_obj(scr,&point);
            assert(cancel_target && lv_obj_has_state(cancel_target,LV_STATE_PRESSED));
            lv_obj_send_event(cancel_target,LV_EVENT_PRESS_LOST,NULL);
            lv_test_wait(20);
        }
        sim_release();
        assert(frames==before+2);
        neutral();
    }
    // Physical extents and midpoint. Positive screen Y points down unless explicitly inverted.
    sim_press_point(320,316); neutral(); sim_release();
    sim_press_point(0,33);
    assert(axis(1)==-32767 && (calibrated ? axis(3)>32000 : axis(3)<-32000));
    sim_release(); neutral();
    sim_press_point(638,598);
    assert(axis(1)>32000 && (calibrated ? axis(3)<-32000 : axis(3)>32000));
    sim_release(); neutral();
    sim_press_point(355,316);
    if (calibrated) neutral(); else assert(axis(1)>3000 && !axis(3));
    sim_release(); neutral();
    sim_press_point(480,175);
    if (calibrated) assert(axis(1)>1680 && axis(1)<1810 && axis(3)>1520 && axis(3)<1810);
    else assert(axis(1)>16300 && axis(1)<16600 && axis(3)<-16000);
    sim_release(); neutral();
    // Throttle stays absolute across a hat press/release and clears on teardown.
    sim_press_point(915,110); sim_release();
    unsigned throttle=wire[7] | wire[8]<<8;
    assert(throttle>47000 && throttle<51000);
    sim_press_point(833,439); assert(wire[11]==0); sim_release();
    assert(wire[11]==8 && (unsigned)(wire[7] | wire[8]<<8)==throttle);
    sim_press_point(719,495); assert(wire[11]==6);
    kvm_hid_core_set_usb(&core,KVM_HOST_USB_DETACHED,1,now()); lv_test_wait(70);
    kvm_hid_core_set_usb(&core,KVM_HOST_USB_MOUNTED,2,now()); lv_test_wait(70);
    sim_release(); neutral();
    sim_press_point(947,495); assert(wire[11]==2);
    pack_view_close(); lv_screen_load(base); lv_test_wait(80); sim_release(); neutral();
    assert(core.owner.kind==OWNER_LOCAL && !core.joystick_active);
    lv_obj_delete(scr); lv_timer_delete(timer); sim_env_stop(baseline);
    printf("PASS report 0x30, eight POV directions, lift/cancel/detach/exit neutral, "
           "throttle=%u, %s pitch/roll, %u frames\n",throttle,calibrated?"inverted cubic 20% dead zone":"linear",frames);
    return 0;
}
