// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_keyboard_surface.h"
#include "app_config.h"
#include "hid_dispatch.h"
#include "host_lv_mem.h"
#include "sim_image.h"
#include "ui_theme.h"
#include "src/misc/lv_text_private.h"
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *layout="fr";
static bool mounted, refuse;
static unsigned count, releases;
static kvm_hid_intent_t input[128];
static bool ready(void) { return mounted; }
void app_config_host_layout(char *id,size_t cap) { snprintf(id,cap,"%s",layout); }
esp_err_t hid_dispatch_local(const kvm_hid_intent_t *intent)
{
    assert(count<sizeof(input)/sizeof(input[0]));
    input[count++]=*intent;
    if (refuse) { refuse=false; return -1; }
    return ESP_OK;
}
void hid_dispatch_release_all(kvm_release_reason_t reason)
{
    assert(reason==KVM_RA_VIEW_CHANGE || reason==KVM_RA_GESTURE_CANCEL);
    releases++;
}
static void check(unsigned i,bool down,unsigned usage)
{
    assert(i<count && input[i].type==(down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP));
    assert(input[i].data.key.usage_id==usage);
}
static void move(int x,int y) { lv_test_mouse_move_to(x+8,y+8);lv_test_wait(50); }
static void press(int x,int y) { move(x,y);lv_test_mouse_press();lv_test_wait(50); }
static void up(void) { lv_test_mouse_release();lv_test_wait(50); }
static void tap(int x,int y) { press(x,y);up(); }

// The matrix API exposes key captions, but only its layout knows the hit rectangles.
static uint32_t find_key(lv_obj_t *matrix, const char *caption)
{
    lv_buttonmatrix_t *buttons=(lv_buttonmatrix_t *)matrix;
    for (uint32_t id=0; id<buttons->btn_cnt; ++id) {
        if (!lv_buttonmatrix_has_button_ctrl(matrix,id,LV_BUTTONMATRIX_CTRL_HIDDEN) &&
            !lv_buttonmatrix_has_button_ctrl(matrix,id,LV_BUTTONMATRIX_CTRL_DISABLED) &&
            !strcmp(lv_buttonmatrix_get_button_text(matrix,id),caption)) return id;
    }
    return LV_BUTTONMATRIX_BUTTON_NONE;
}

static void press_id(lv_obj_t *matrix, uint32_t id)
{
    lv_obj_update_layout(matrix);
    assert(id<((lv_buttonmatrix_t *)matrix)->btn_cnt);
    lv_area_t origin;
    lv_obj_get_coords(matrix,&origin);
    const lv_area_t *area=&((lv_buttonmatrix_t *)matrix)->button_areas[id];
    lv_test_mouse_move_to(origin.x1+(area->x1+area->x2)/2,origin.y1+(area->y1+area->y2)/2);
    lv_test_wait(50);
    lv_test_mouse_press();
    lv_test_wait(50);
}

static void tap_id(lv_obj_t *matrix, uint32_t id)
{
    press_id(matrix,id);
    up();
}

static void tap_key(lv_obj_t *matrix, const char *caption)
{
    uint32_t id=find_key(matrix,caption);
    if (id==LV_BUTTONMATRIX_BUTTON_NONE) fprintf(stderr,"%s: missing key '%s'\n",layout,caption);
    assert(id!=LV_BUTTONMATRIX_BUTTON_NONE);
    tap_id(matrix,id);
}

static void audit_keys(lv_obj_t *matrix)
{
    lv_obj_update_layout(matrix);
    const lv_buttonmatrix_t *buttons=(const lv_buttonmatrix_t *)matrix;
    const lv_font_t *font=lv_obj_get_style_text_font(matrix,LV_PART_ITEMS);
    lv_text_attributes_t attributes={
        .letter_space=lv_obj_get_style_text_letter_space(matrix,LV_PART_ITEMS),
        .max_width=LV_COORD_MAX,
    };
    for (uint32_t id=0; id<buttons->btn_cnt; ++id) {
        if (lv_buttonmatrix_has_button_ctrl(matrix,id,LV_BUTTONMATRIX_CTRL_HIDDEN) ||
            lv_buttonmatrix_has_button_ctrl(matrix,id,LV_BUTTONMATRIX_CTRL_DISABLED)) continue;
        const lv_area_t *area=&buttons->button_areas[id];
        const char *caption=lv_buttonmatrix_get_button_text(matrix,id);
        int32_t width=lv_area_get_width(area), height=lv_area_get_height(area);
        lv_point_t text;
        lv_text_get_size_attributes(&text,caption,font,&attributes);
        bool fits=width>=44 && height>=44 && area->x1>=0 && area->y1>=0 &&
                  area->x2<lv_obj_get_width(matrix) && area->y2<lv_obj_get_height(matrix) &&
                  text.x<=width && text.y<=height;
        if (!fits) fprintf(stderr,"%s '%s' key %ldx%ld, text %ldx%ld\n",layout,caption,
                           (long)width,(long)height,(long)text.x,(long)text.y);
        assert(fits);
    }
}

static void tap_control(lv_obj_t *matrix, const char *caption)
{
    unsigned at=count, previous_releases=releases;
    tap_key(matrix,caption);
    assert(count==at && releases==previous_releases);
    audit_keys(matrix);
}

// One-shot modifiers precede key-down and are released only after the key itself.
static void type_key(lv_obj_t *matrix, const char *caption, unsigned usage, bool shift, bool ralt)
{
    unsigned at=count;
    if (shift) tap_control(matrix,"Shift");
    if (ralt) tap_control(matrix,(!strcmp(layout,"jp") || !strcmp(layout,"us")) ? "R Alt" : "AltGr");
    tap_key(matrix,caption);
    if (shift) check(at++,true,0xe1);
    if (ralt) check(at++,true,0xe6);
    check(at++,true,usage);
    check(at++,false,usage);
    if (shift) check(at++,false,0xe1);
    if (ralt) check(at++,false,0xe6);
    assert(count==at);
}

static void toggle_ime(lv_obj_t *matrix)
{
    lv_obj_update_layout(matrix);
    uint32_t id=find_key(matrix,"IME");
    assert(id!=LV_BUTTONMATRIX_BUTTON_NONE);
    const lv_area_t *area=&((lv_buttonmatrix_t *)matrix)->button_areas[id];
    assert(area->x1==0 && area->y2==lv_obj_get_height(matrix)-1);
    tap_control(matrix,"IME");
}

static void snapshot(lv_obj_t *matrix, const char *directory, const char *name)
{
    if (!directory) return;
    char path[1024];
    int n=snprintf(path,sizeof(path),"%s/%s.png",directory,name);
    assert(n>0 && (size_t)n<sizeof(path));
    sim_image_t image;
    assert(sim_image_render(matrix,&image));
    assert(sim_image_write_png(path,&image));
    sim_image_free(&image);
}

static void expect_chord(lv_obj_t *matrix, const char *key, unsigned usage, unsigned modifier)
{
    unsigned at=count;
    tap_key(matrix,key);
    assert(count==at+4);
    check(at,true,modifier);check(at+1,true,usage);
    check(at+2,false,usage);check(at+3,false,modifier);
}

static void function_layout(lv_obj_t *matrix, const char *id, const char *directory)
{
    static const struct { const char *label; unsigned usage; } extra[]={
        {"Ins",0x49},{"Home",0x4a},{"PgUp",0x4b},{"Del",0x4c},{"End",0x4d},{"PgDn",0x4e},
        {"Esc",0x29}
    };
    count=0;
    // Every layout retains the way in from the symbol page, even a full AZERTY letter row.
    tap_control(matrix,"123");
    assert(find_key(matrix,"Tab")==LV_BUTTONMATRIX_BUTTON_NONE);
    type_key(matrix,"Home",0x4a,false,false);
    type_key(matrix,"End",0x4d,false,false);
    tap_control(matrix,"Fn");
    if (!strcmp(id,"us")) snapshot(matrix,directory,"us-fn");
    for (unsigned f=1; f<=12; ++f) {
        char name[5];
        snprintf(name,sizeof(name),"F%u",f);
        type_key(matrix,name,0x3a+f-1,false,false);
    }
    for (size_t i=0; i<sizeof(extra)/sizeof(extra[0]); ++i)
        type_key(matrix,extra[i].label,extra[i].usage,false,false);
    unsigned win=count;
    press_id(matrix,find_key(matrix,"Win"));
    assert(count==win+1);check(win,true,0xe3);
    up();assert(count==win+2);check(win+1,false,0xe3);
    tap_control(matrix,"Alt");expect_chord(matrix,"F4",0x3d,0xe2);
    tap_control(matrix,"Ctrl");expect_chord(matrix,"Home",0x4a,0xe0);
    tap_control(matrix,"Shift");expect_chord(matrix,"F10",0x43,0xe1);

    // A host LED report during a held function key changes the Caps label only.
    assert(find_key(matrix,"Caps\n?")!=LV_BUTTONMATRIX_BUTTON_NONE ||
           find_key(matrix,"Caps\nUnknown")!=LV_BUTTONMATRIX_BUTTON_NONE);
    unsigned at=count;
    press_id(matrix,find_key(matrix,"F12"));
    check(at,true,0x45);
    pack_keyboard_surface_set_caps(matrix,true,true);
    uint32_t caps=find_key(matrix,"Caps\nOn");
    assert(caps!=LV_BUTTONMATRIX_BUTTON_NONE &&
           lv_buttonmatrix_has_button_ctrl(matrix,caps,LV_BUTTONMATRIX_CTRL_CHECKED));
    assert(count==at+1);
    up();check(at+1,false,0x45);assert(count==at+2);
    type_key(matrix,"Caps\nOn",0x39,false,false);
    assert(find_key(matrix,"Caps\nOn")!=LV_BUTTONMATRIX_BUTTON_NONE);
    pack_keyboard_surface_set_caps(matrix,true,false);
    caps=find_key(matrix,"Caps\nOff");
    assert(caps!=LV_BUTTONMATRIX_BUTTON_NONE &&
           !lv_buttonmatrix_has_button_ctrl(matrix,caps,LV_BUTTONMATRIX_CTRL_CHECKED));

    tap_control(matrix,"Win+");
    tap_control(matrix,"Back");
    assert(find_key(matrix,"ABC")!=LV_BUTTONMATRIX_BUTTON_NONE);
    expect_chord(matrix,"Home",0x4a,0xe3);
    type_key(matrix,"Home",0x4a,false,false);
    tap_control(matrix,"ABC");
    type_key(matrix,"Tab",0x2b,false,false);
    // FR and BR need their spare cell for Backspace because their home row ends with a letter.
    if (!strcmp(id,"fr") || !strcmp(id,"br")) {
        assert(find_key(matrix,"Fn")==LV_BUTTONMATRIX_BUTTON_NONE);
    } else {
        tap_control(matrix,"Fn");
        tap_control(matrix,"Win+");
        tap_control(matrix,"Back");
        assert(find_key(matrix,"123")!=LV_BUTTONMATRIX_BUTTON_NONE);
        expect_chord(matrix,"r",0x15,0xe3);
        type_key(matrix,"r",0x15,false,false);
    }
    pack_keyboard_surface_set_caps(matrix,false,false);
    printf("PASS %s Fn: F1-F12, editing, Win, Alt+F4, Ctrl/Shift, Win+ across Back, host Caps\n",id);
}

static void regional_layout(lv_obj_t *parent, const char *id, int width, int height, const char *directory)
{
    layout=id;
    count=0;
    assert(pack_keyboard_surface_open(parent,width,height,ready));
    lv_obj_t *matrix=lv_obj_get_child(parent,0);
    audit_keys(matrix);
    function_layout(matrix,id,directory);
    count=0;
    if (strcmp(id,"jp") && strcmp(id,"kr"))
        assert(find_key(matrix,"IME")==LV_BUTTONMATRIX_BUTTON_NONE);
    if (!strcmp(id,"fr")) {
        snapshot(matrix,directory,"fr-letters");
        type_key(matrix,"m",0x33,false,false);
        type_key(matrix,"M",0x33,true,false);
        tap_control(matrix,"123");
        type_key(matrix,"<",0x64,false,false);
    } else if (!strcmp(id,"pl")) {
        type_key(matrix,"ą",0x04,false,true);
        type_key(matrix,"Ą",0x04,true,true);
    } else if (!strcmp(id,"br")) {
        tap_control(matrix,"123");
        type_key(matrix,"\\",0x64,false,false);
        type_key(matrix,"/",0x87,false,false);
        type_key(matrix,"?",0x87,true,false);
    } else if (!strcmp(id,"jp") || !strcmp(id,"kr")) {
        bool jis=!strcmp(id,"jp");
        assert(find_key(matrix,jis ? "Henkan" : "Hanja")==LV_BUTTONMATRIX_BUTTON_NONE);
        toggle_ime(matrix);
        if (jis && width==440) snapshot(matrix,directory,"jis-ime");
        type_key(matrix,"q",0x14,false,false);
        if (jis) {
            type_key(matrix,"Muhenkan",0x8b,false,false);
            type_key(matrix,"Henkan",0x8a,false,false);
            type_key(matrix,"Kana",0x88,false,false);
        } else {
            type_key(matrix,"Han/Eng",0x90,false,false);
            type_key(matrix,"Hanja",0x91,false,false);
        }
        toggle_ime(matrix);
        assert(find_key(matrix,jis ? "Henkan" : "Hanja")==LV_BUTTONMATRIX_BUTTON_NONE);
        type_key(matrix,"Tab",0x2b,false,false);
        tap_control(matrix,"123");
        toggle_ime(matrix);
        type_key(matrix,"1",0x1e,false,false);
        type_key(matrix,jis ? "Henkan" : "Hanja",jis ? 0x8a : 0x91,false,false);
        toggle_ime(matrix);
        assert(find_key(matrix,jis ? "Henkan" : "Hanja")==LV_BUTTONMATRIX_BUTTON_NONE);
        if (jis) {
            type_key(matrix,"Mode",0x35,false,false);
            // Both keys have the same base legend. Locate each by its distinct shifted legend.
            tap_control(matrix,"Shift");
            uint32_t yen=find_key(matrix,"|"), ro=find_key(matrix,"_");
            assert(yen!=LV_BUTTONMATRIX_BUTTON_NONE && ro!=LV_BUTTONMATRIX_BUTTON_NONE && yen!=ro);
            tap_control(matrix,"Shift");
            assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,yen),"\\") &&
                   !strcmp(lv_buttonmatrix_get_button_text(matrix,ro),"\\"));
            unsigned at=count;
            tap_id(matrix,yen);tap_id(matrix,ro);
            check(at,true,0x89);check(at+1,false,0x89);
            check(at+2,true,0x87);check(at+3,false,0x87);
            assert(count==at+4);
            type_key(matrix,"|",0x89,true,false);
            type_key(matrix,"_",0x87,true,false);
            type_key(matrix,"!",0x1e,true,true);
        }
    } else {
        assert(!strcmp(id,"us"));
        tap_control(matrix,"123");
        type_key(matrix,"!",0x1e,true,true);
    }
    printf("PASS %s: physical key reports, labels and 44x44 targets at %dx%d\n",id,width,height);
    lv_obj_delete(matrix);
}

int main(int argc,char **argv)
{
    lv_init(); assert(lv_test_display_create(600,1024)); lv_test_indev_create_all(); assert(ui_theme_init());
    lv_obj_t *parent=lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);lv_obj_set_size(parent,584,288);lv_obj_set_pos(parent,8,8);
    assert(!pack_keyboard_surface_open(parent,439,176,ready));
    assert(!pack_keyboard_surface_open(parent,440,175,ready));
    size_t baseline=host_lv_live_bytes();
    assert(pack_keyboard_surface_open(parent,440,176,ready));
    size_t surface=host_lv_live_bytes()-baseline;
    lv_obj_t *matrix=lv_obj_get_child(parent,0);
    assert(matrix && lv_obj_check_type(matrix,&lv_buttonmatrix_class));
    assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,0),"a")); // French AD01, physical US Q.
    tap(20,20);tap(20,110);assert(!count && !releases);
    mounted=true;
    press(20,20);lv_test_wait(600);assert(count==1);check(0,true,0x14);
    move(70,20);assert(count==2);check(1,false,0x14);up();assert(count==2);
    tap(20,110);assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,0),"A"));
    tap(20,20);assert(count==6);check(2,true,0xe1);check(3,true,0x14);check(4,false,0x14);check(5,false,0xe1);
    assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,0),"a"));
    tap(20,154);assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,0),"&"));
    tap(20,20);check(6,true,0x1e);check(7,false,0x1e);
    tap(20,154);tap(66,154);assert(count==8); // Ctrl is pending locally.
    pack_keyboard_surface_release();tap(20,20);check(8,true,0x14);check(9,false,0x14);
    press(20,20);mounted=false;lv_test_wait(70);assert(releases==1);up();assert(count==11);
    mounted=true;up();assert(count==11); // No replay at reattachment.
    refuse=true;press(20,20);assert(releases==2);up();
    press(20,20);pack_keyboard_surface_release();up();assert(releases==3);
    lv_obj_delete(matrix);assert(releases==3);
    const char *layouts[]={"fr","jp","br","pl","kr","us"};
    const char *directory=argc>=3 ? argv[2] : NULL;
    for (size_t i=0; i<sizeof(layouts)/sizeof(layouts[0]); ++i)
        regional_layout(parent,layouts[i],440,176,directory);
    regional_layout(parent,"jp",458,182,NULL); // First size with 2 px gaps between targets.
    regional_layout(parent,"jp",584,288,NULL); // Installed console bounds.
    layout="us";assert(pack_keyboard_surface_open(parent,584,288,ready));
    matrix=lv_obj_get_child(parent,0);assert(!strcmp(lv_buttonmatrix_get_button_text(matrix,0),"q"));
    count=0;
    type_key(matrix,"Caps\nUnknown",0x39,false,false);
    assert(find_key(matrix,"Caps\nUnknown")!=LV_BUTTONMATRIX_BUTTON_NONE); // Wait for the host.
    snapshot(matrix,argc>=3 ? argv[2] : NULL,"caps-unknown");
    pack_keyboard_surface_set_caps(matrix,true,false);
    type_key(matrix,"Caps\nOff",0x39,false,false);
    unsigned prior=count;
    press(20,20);
    check(prior,true,0x14);
    pack_keyboard_surface_set_caps(matrix,true,true);
    assert(lv_buttonmatrix_has_button_ctrl(matrix,find_key(matrix,"Caps\nOn"),LV_BUTTONMATRIX_CTRL_CHECKED));
    assert(count==prior+1);
    up();check(prior+1,false,0x14);
    assert(count==prior+2);
    tap_control(matrix,"123");
    type_key(matrix,"Caps\nOn",0x39,false,false);
    tap_control(matrix,"ABC");
    assert(find_key(matrix,"Caps\nOn")!=LV_BUTTONMATRIX_BUTTON_NONE);
    audit_keys(matrix);
    if (argc>=2) {
        sim_image_t image;
        assert(sim_image_render(matrix,&image));
        assert(sim_image_write_png(argv[1],&image));
        sim_image_free(&image);
    }
    press(20,20);lv_obj_delete(parent);up();assert(releases==4);
    pack_keyboard_surface_release();assert(releases==4);
    printf("PASS real LVGL keyboard surfaces, minimum and authored sizes, FR/US usage mapping, layers, "
           "one-shot modifiers, hold, slide cancel, offline, queue refusal, view switch and teardown; "
           "host widget allocation %zu bytes\n",surface);
    lv_deinit();
}
