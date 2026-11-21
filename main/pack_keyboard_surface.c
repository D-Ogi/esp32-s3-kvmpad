// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_keyboard_surface.h"
#include "app_config.h"
#include "hid_dispatch.h"
#include "kbd_layouts.h"
#include "ui_theme.h"

// Number/symbol and letter layers each have four rows. Ordinary keys always
// carry the physical usage from the configured xkeyboard layout.
enum { LAYER = KBD_SLOT_COUNT, SHIFT, CTRL, ALT, ALTGR, BACK, ENTER, TAB, SPACE,
       LEFT, RIGHT, UP, DOWN, ESC, DEL, HOME, END, CAPS, IME, FN, FN_BACK, INS, PGUP, PGDN, WIN, WIN_MOD,
       F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12, HOME_ROW_END, EXTRA_BACK, LINE };
static const uint8_t letters[] = {
    KBD_SLOT_AD01,KBD_SLOT_AD02,KBD_SLOT_AD03,KBD_SLOT_AD04,KBD_SLOT_AD05,KBD_SLOT_AD06,KBD_SLOT_AD07,KBD_SLOT_AD08,KBD_SLOT_AD09,KBD_SLOT_AD10,LINE,
    KBD_SLOT_AC01,KBD_SLOT_AC02,KBD_SLOT_AC03,KBD_SLOT_AC04,KBD_SLOT_AC05,KBD_SLOT_AC06,KBD_SLOT_AC07,KBD_SLOT_AC08,KBD_SLOT_AC09,HOME_ROW_END,LINE,
    SHIFT,KBD_SLOT_AB01,KBD_SLOT_AB02,KBD_SLOT_AB03,KBD_SLOT_AB04,KBD_SLOT_AB05,KBD_SLOT_AB06,KBD_SLOT_AB07,EXTRA_BACK,ENTER,LINE
};
static const uint8_t numbers[] = {
    KBD_SLOT_AE01,KBD_SLOT_AE02,KBD_SLOT_AE03,KBD_SLOT_AE04,KBD_SLOT_AE05,KBD_SLOT_AE06,KBD_SLOT_AE07,KBD_SLOT_AE08,KBD_SLOT_AE09,KBD_SLOT_AE10,LINE,
    KBD_SLOT_TLDE,KBD_SLOT_AE11,KBD_SLOT_AE12,KBD_SLOT_AD11,KBD_SLOT_AD12,KBD_SLOT_BKSL,KBD_SLOT_AC10,KBD_SLOT_AC11,KBD_SLOT_AB08,BACK,LINE,
    SHIFT,KBD_SLOT_AB09,KBD_SLOT_AB10,KBD_SLOT_LSGT,KBD_SLOT_AE13,KBD_SLOT_AB11,ESC,DEL,UP,DOWN,ENTER,LINE
};
static const uint8_t bottom[]={IME,LAYER,CTRL,ALT,ALTGR,TAB,CAPS,SPACE,LEFT,RIGHT};
static const uint8_t functions[]={
    F1,F2,F3,F4,F5,F6,LINE,
    F7,F8,F9,F10,F11,F12,LINE,
    ESC,INS,DEL,HOME,END,PGUP,PGDN,LINE
};
static const uint8_t function_bottom[]={FN_BACK,SHIFT,CTRL,ALT,WIN_MOD,WIN,CAPS};
static const uint8_t ime_row[]={IME,KBD_SLOT_MUHE,KBD_SLOT_HENK,KBD_SLOT_HKTG,KBD_SLOT_HNGL,KBD_SLOT_HJCV,CAPS,SPACE};
typedef enum { CAPS_UNKNOWN, CAPS_OFF, CAPS_ON } caps_state_t;
enum { MAP_CAPACITY=sizeof(numbers)+sizeof(bottom) };
_Static_assert(sizeof(letters)<=sizeof(numbers) && sizeof(ime_row)<=sizeof(bottom),"keyboard map capacity");
_Static_assert(sizeof(functions)+sizeof(function_bottom)<=MAP_CAPACITY,"function map capacity");
static const struct { const char *label; uint8_t usage; } controls[] = {
    [LAYER-LAYER]={"123",0}, [SHIFT-LAYER]={"Shift",0xe1}, [CTRL-LAYER]={"Ctrl",0xe0},
    [ALT-LAYER]={"Alt",0xe2}, [ALTGR-LAYER]={"AltGr",0xe6}, [BACK-LAYER]={LV_SYMBOL_BACKSPACE,0x2a},
    [ENTER-LAYER]={"Enter",0x28}, [TAB-LAYER]={"Tab",0x2b}, [SPACE-LAYER]={"Space",0x2c},
    [LEFT-LAYER]={LV_SYMBOL_LEFT,0x50}, [RIGHT-LAYER]={LV_SYMBOL_RIGHT,0x4f},
    [UP-LAYER]={LV_SYMBOL_UP,0x52}, [DOWN-LAYER]={LV_SYMBOL_DOWN,0x51}, [ESC-LAYER]={"Esc",0x29},
    [DEL-LAYER]={"Del",0x4c}, [HOME-LAYER]={"Home",0x4a}, [END-LAYER]={"End",0x4d},
    [CAPS-LAYER]={"Caps",0x39}, [IME-LAYER]={"IME",0}, [FN-LAYER]={"Fn",0}, [FN_BACK-LAYER]={"Back",0},
    [INS-LAYER]={"Ins",0x49}, [PGUP-LAYER]={"PgUp",0x4b}, [PGDN-LAYER]={"PgDn",0x4e},
    [WIN-LAYER]={"Win",0xe3}, [WIN_MOD-LAYER]={"Win+",0xe3},
    [F1-LAYER]={"F1",0x3a}, [F2-LAYER]={"F2",0x3b}, [F3-LAYER]={"F3",0x3c}, [F4-LAYER]={"F4",0x3d},
    [F5-LAYER]={"F5",0x3e}, [F6-LAYER]={"F6",0x3f}, [F7-LAYER]={"F7",0x40}, [F8-LAYER]={"F8",0x41},
    [F9-LAYER]={"F9",0x42}, [F10-LAYER]={"F10",0x43}, [F11-LAYER]={"F11",0x44}, [F12-LAYER]={"F12",0x45}
};
typedef struct {
    lv_obj_t *matrix;
    const kbd_layout_t *layout;
    const char *labels[MAP_CAPACITY+1];
    uint8_t usages[MAP_CAPACITY], keys[MAP_CAPACITY], count;
    bool (*ready)(void);
    bool numeric, fn_open, has_ime, ime_open, home_row_letter;
    uint8_t mods, held;
    uint8_t caps_id, caps_entry, space_width, caps_width;
    caps_state_t caps;
} surface_t;

// Only the active screen can receive a local keyboard press.
static surface_t *active;

static const char *caps_caption(const surface_t *s)
{
    if (s->caps==CAPS_UNKNOWN) return s->caps_width==2 ? "Caps\nUnknown" : "Caps\n?";
    return s->caps==CAPS_ON ? "Caps\nOn" : "Caps\nOff";
}

static void show_keys(surface_t *s)
{
    const uint8_t *slots=s->numeric ? numbers : letters;
    size_t n=s->numeric ? sizeof(numbers) : sizeof(letters);
    const uint8_t *last=s->ime_open ? ime_row : bottom;
    size_t last_n=s->ime_open ? sizeof(ime_row) : sizeof(bottom);
    if (s->fn_open) {
        slots=functions;
        n=sizeof(functions);
        last=function_bottom;
        last_n=sizeof(function_bottom);
    }
    const bool altgr=(s->layout->flags & KBD_LAYOUT_ALTGR)!=0;
    const unsigned level=((s->mods & 2) ? 1u : 0u) + (altgr && (s->mods & 64) ? 2u : 0u);
    lv_buttonmatrix_ctrl_t ctrl[MAP_CAPACITY]={0};
    size_t id=0, entry=0;
    for (size_t i=0;i<n+last_n;i++) {
        uint8_t key=i<n ? slots[i] : last[i-n];
        if (key==LINE) { s->labels[entry++]="\n"; continue; }
        if (key==HOME_ROW_END) key=s->home_row_letter ? KBD_SLOT_AC10 : BACK;
        if (key==EXTRA_BACK) key=s->home_row_letter ? BACK : FN;
        if (key==IME && !s->has_ime) continue;
        if (s->numeric && key==LEFT) key=HOME;
        if (s->numeric && key==RIGHT) key=END;
        if (s->numeric && key==TAB) key=FN;
        const char *label;
        uint8_t usage;
        unsigned width=1;
        if (key<KBD_SLOT_COUNT) {
            const kbd_key_t *binding=kbd_layouts_key(s->layout,(kbd_slot_t)key);
            if (!binding && (i>=n || key==KBD_SLOT_AE13 || key==KBD_SLOT_AB11 || key==KBD_SLOT_LSGT)) continue;
            usage=binding ? binding->usage : 0;
            label=binding ? kbd_layouts_legend(binding,level) : "";
            if (binding && !*label) label=kbd_layouts_legend(binding,0);
            if (key>=KBD_SLOT_MUHE && key<=KBD_SLOT_HJCV) {
                static const char *const names[]={"Muhenkan","Henkan","Kana","Han/Eng","Hanja"};
                label=names[key-KBD_SLOT_MUHE];
                width=(key==KBD_SLOT_MUHE || s->layout->geometry==KBD_GEOM_KR) ? 3 : 2;
            } else if (!*label && binding && (binding->flags & KBD_KEY_IME)) label="Mode";
        } else {
            usage=controls[key-LAYER].usage;
            label=key==LAYER && s->numeric ? "ABC" : controls[key-LAYER].label;
            if (key==ALTGR && !altgr) label="R Alt";
            if (key==SPACE) width=s->space_width;
        }
        s->keys[id]=key;
        s->usages[id]=usage;
        if (key==CAPS) {
            s->caps_entry=(uint8_t)entry;
            s->caps_id=(uint8_t)id;
            s->labels[entry++]=caps_caption(s);
            width=s->caps_width;
        } else {
            s->labels[entry++]=*label ? ui_theme_drawable(lv_obj_get_style_text_font(s->matrix,LV_PART_ITEMS),label) : " ";
        }
        ctrl[id]=LV_BUTTONMATRIX_CTRL_NO_REPEAT | width;
        if (key==CAPS && s->caps==CAPS_ON) ctrl[id]|=LV_BUTTONMATRIX_CTRL_CHECKED;
        if (key==IME && s->ime_open) ctrl[id]|=LV_BUTTONMATRIX_CTRL_CHECKED;
        if (key<KBD_SLOT_COUNT && !usage) ctrl[id]|=LV_BUTTONMATRIX_CTRL_DISABLED;
        if (((key>=SHIFT && key<=ALTGR) || key==WIN_MOD) && (s->mods & (1u<<(usage-0xe0))))
            ctrl[id]|=LV_BUTTONMATRIX_CTRL_CHECKED;
        id++;
    }
    s->labels[entry]="";
    s->count=(uint8_t)id;
    lv_buttonmatrix_set_map(s->matrix,s->labels);
    lv_buttonmatrix_set_ctrl_map(s->matrix,ctrl);
}

static bool submit(bool down,uint8_t usage)
{
    kvm_hid_intent_t intent={.type=down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP,.data.key={.usage_id=usage}};
    return hid_dispatch_local(&intent)==ESP_OK;
}

static void clear(surface_t *s,bool cancel)
{
    if (s->held) {
        bool ok=!cancel && submit(false,s->held);
        for (unsigned bit=0;ok && bit<8;bit++)
            if (s->mods & (1u<<bit)) ok=submit(false,(uint8_t)(0xe0+bit));
        if (!ok) hid_dispatch_release_all(KVM_RA_GESTURE_CANCEL);
        s->held=0;
    }
    if (s->mods) { s->mods=0; show_keys(s); }
}

void pack_keyboard_surface_release(void)
{
    if (active) { clear(active,true); active=NULL; }
}

static void on_event(lv_event_t *e)
{
    surface_t *s=lv_event_get_user_data(e);
    lv_event_code_t code=lv_event_get_code(e);
    if (code==LV_EVENT_DELETE) {
        if (active==s) {
            if (s->held) hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
            active=NULL;
        }
        // The input device can defocus a deleting matrix and trigger another
        // style/layout pass after DELETE. Detach its borrowed map first.
        static const char *const empty_map[]={""};
        lv_buttonmatrix_set_map(s->matrix,empty_map);
        lv_free(s);
        return;
    }
    if (code==LV_EVENT_INDEV_RESET) {
        if (active==s) clear(s,true);
        return;
    }
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING &&
        code!=LV_EVENT_RELEASED && code!=LV_EVENT_PRESS_LOST) return;
    uint32_t id=lv_buttonmatrix_get_selected_button(s->matrix);
    if (code!=LV_EVENT_PRESSED) {
        if (active==s && s->held && (code!=LV_EVENT_PRESSING || !s->ready() || id==LV_BUTTONMATRIX_BUTTON_NONE))
            clear(s,code==LV_EVENT_PRESS_LOST || !s->ready());
        return;
    }
    if (!s->ready() || id>=s->count) return;
    if (active!=s) pack_keyboard_surface_release();
    active=s;
    uint8_t key=s->keys[id], usage=s->usages[id];
    if (key==LAYER) { s->numeric=!s->numeric; show_keys(s); return; }
    if (key==IME) { s->ime_open=!s->ime_open; show_keys(s); return; }
    if (key==FN || key==FN_BACK) { s->fn_open=key==FN; show_keys(s); return; }
    if ((key>=SHIFT && key<=ALTGR) || key==WIN_MOD) { s->mods^=(uint8_t)(1u<<(usage-0xe0)); show_keys(s); return; }
    if (!usage || s->held) return;
    if (key==WIN && (s->mods & (1u<<3))) {
        s->mods &= (uint8_t)~(1u<<3);
        show_keys(s);
    }
    s->held=usage;
    for (unsigned bit=0;bit<8;bit++)
        if ((s->mods & (1u<<bit)) && !submit(true,(uint8_t)(0xe0+bit))) { clear(s,true); return; }
    if (!submit(true,usage)) clear(s,true);
}

bool pack_keyboard_surface_buffered_target(lv_obj_t *target)
{
    if (!lv_obj_check_type(target,&lv_buttonmatrix_class)) return false;
    for (uint32_t i=0;i<lv_obj_get_event_count(target);i++) {
        lv_event_dsc_t *event=lv_obj_get_event_dsc(target,i);
        if (lv_event_dsc_get_cb(event)==on_event) {
            surface_t *s=lv_event_dsc_get_user_data(event);
            return s->ready();
        }
    }
    return false;
}

void pack_keyboard_surface_set_caps(lv_obj_t *matrix,bool known,bool on)
{
    surface_t *s=lv_obj_get_user_data(matrix);
    caps_state_t next=known ? (on ? CAPS_ON : CAPS_OFF) : CAPS_UNKNOWN;
    if (s->caps==next) return;
    s->caps=next;
    // The matrix borrows our labels. Invalidate just this cap without rebuilding
    // the map or disturbing a key that the user is currently holding.
    s->labels[s->caps_entry]=caps_caption(s);
    if (next==CAPS_ON) lv_buttonmatrix_set_button_ctrl(matrix,s->caps_id,LV_BUTTONMATRIX_CTRL_CHECKED);
    else lv_buttonmatrix_clear_button_ctrl(matrix,s->caps_id,LV_BUTTONMATRIX_CTRL_CHECKED);
}

lv_obj_t *pack_keyboard_surface_open(lv_obj_t *parent,int width,int height,bool (*ready)(void))
{
    if (!parent || !ready || width<440 || height<176) return NULL;
    char id[KVM_CONFIG_LAYOUT_ID_CAP];
    app_config_host_layout(id,sizeof(id));
    const kbd_layout_t *layout=kbd_layouts_find(id);
    if (!layout) layout=kbd_layouts_find("us");
    if (!layout) return NULL;
    surface_t *s=lv_malloc_zeroed(sizeof(*s));
    if (!s) return NULL;
    s->ready=ready;
    s->layout=layout;
    const kbd_key_t *home_key=kbd_layouts_key(layout,KBD_SLOT_AC10);
    s->home_row_letter=kbd_layouts_case_pair(kbd_layouts_legend(home_key,0),kbd_layouts_legend(home_key,1));
    for (unsigned k=KBD_SLOT_MUHE;k<=KBD_SLOT_HJCV;k++)
        if (kbd_layouts_key(layout,(kbd_slot_t)k)) s->has_ime=true;
    s->matrix=lv_buttonmatrix_create(parent);
    if (!s->matrix || !lv_obj_add_event_cb(s->matrix,on_event,LV_EVENT_ALL,s)) {
        if (s->matrix) lv_obj_delete(s->matrix);
        lv_free(s);
        return NULL;
    }
    lv_obj_set_user_data(s->matrix,s);
    // The matrix fills the authored bounds, including the outer keys.
    lv_obj_set_style_pad_all(parent,0,LV_PART_MAIN);
    lv_obj_set_style_border_width(parent,0,LV_PART_MAIN);
    lv_obj_set_scrollable(parent,false);
    lv_obj_remove_style_all(s->matrix);
    ui_theme_apply(s->matrix,UI_STYLE_CONTROL_PAGE,LV_PART_MAIN);
    ui_theme_apply(s->matrix,UI_STYLE_CONTROL_BUTTON,LV_PART_ITEMS);
    ui_theme_apply(s->matrix,UI_STYLE_CONTROL_SELECTED,(lv_style_selector_t)LV_PART_ITEMS | LV_STATE_PRESSED);
    ui_theme_apply(s->matrix,UI_STYLE_CONTROL_SELECTED,(lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    int column_gap=width>=458 ? 2 : 0;
    // The longest ordinary control caption, Home, uses 50 pixels at body size.
    int usable_width=width-9*column_gap;
    int fixed_units=s->has_ime ? 9 : 8; // Includes a single-unit Caps key, excludes Space.
    s->caps_width=usable_width>=44*(fixed_units+3) ? 2 : 1;
    if (s->caps_width==2) fixed_units++;
    bool body_font=usable_width>=50*LV_MAX(10,fixed_units+2);
    int units=usable_width/(body_font ? 50 : 44);
    s->space_width=(uint8_t)LV_MIN(3,units-fixed_units);
    lv_obj_set_style_text_align(s->matrix,LV_TEXT_ALIGN_CENTER,LV_PART_ITEMS);
    lv_obj_set_style_text_font(s->matrix,ui_theme_font(body_font ? UI_FONT_BODY : UI_FONT_LABEL),LV_PART_ITEMS);
    lv_obj_set_style_pad_all(s->matrix,0,LV_PART_MAIN);
    lv_obj_set_style_pad_column(s->matrix,column_gap,LV_PART_MAIN);
    lv_obj_set_style_pad_row(s->matrix,height>=182 ? 2 : 0,LV_PART_MAIN);
    lv_obj_set_style_border_width(s->matrix,0,LV_PART_MAIN);
    lv_obj_set_size(s->matrix,width,height);
    lv_obj_set_scrollable(s->matrix,false);
    show_keys(s);
    return s->matrix;
}
