// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ui_keyboard_surface.h"
#include "ui_action_key.h"
#include "ui_theme.h"
#include <string.h>

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
    lv_obj_t *button;
    ui_action_key_desc_t desc;
    uint8_t key, usage, weight;
} cap_t;

typedef struct {
    lv_obj_t *root;
    const kbd_layout_t *layout;
    ui_keyboard_surface_hooks_t hooks;
    ui_action_key_sink_t sink;
    cap_t keys[MAP_CAPACITY];
    unsigned count;
    int width, height, column_gap, row_gap;
    ui_font_role_t font;
    bool numeric, fn_open, has_ime, ime_open, home_row_letter, closing;
    uint8_t mods, held, caps_id, space_width, caps_width;
    caps_state_t caps;
} surface_t;

static const char *caps_caption(const surface_t *s)
{
    if (s->caps==CAPS_UNKNOWN) return s->caps_width==2 ? "Caps\nUnknown" : "Caps\n?";
    return s->caps==CAPS_ON ? "Caps\nOn" : "Caps\nOff";
}

static unsigned legend_level(const surface_t *s)
{
    unsigned shift=(s->mods & 2) ? 1u : 0u;
    bool altgr=(s->layout->flags & KBD_LAYOUT_ALTGR) && (s->mods & 64);
    return shift+(altgr ? 2u : 0u);
}

static bool has_caps_pair(const kbd_key_t *binding,unsigned level)
{
    unsigned pair=(level & 2u) ? KBD_KEY_CAPS_ALTGR : KBD_KEY_CAPS_BASE;
    return binding && (binding->flags & pair);
}

static const char *slot_legend(const kbd_key_t *binding,unsigned level,caps_state_t caps)
{
    // Caps reverses Shift within a letter pair, including the active AltGr pair.
    if (caps==CAPS_ON && has_caps_pair(binding,level)) level^=1u;
    const char *legend=kbd_layouts_legend(binding,level);
    return *legend ? legend : kbd_layouts_legend(binding,0);
}

static void set_legend(surface_t *s,const cap_t *cap,const char *label)
{
    const char *legend=cap->key==CAPS ? label : *label ? ui_theme_drawable(ui_theme_font(s->font),label) : " ";
    if (strcmp(lv_label_get_text(lv_obj_get_child(cap->button,-1)),legend)) ui_action_key_set_label(cap->button,legend);
}

static void layout_row(surface_t *s,unsigned start,unsigned end,unsigned row)
{
    int count=(int)(end-start), units=0, used=0;
    for (unsigned i=start;i<end;i++) units+=s->keys[i].weight;
    int space=s->width-(count-1)*s->column_gap;
    int y=(int)row*(s->height+s->row_gap)/4;
    int height=((int)row+1)*(s->height+s->row_gap)/4-s->row_gap-y;
    for (unsigned i=start;i<end;i++) {
        cap_t *cap=&s->keys[i];
        int previous=used;
        used+=cap->weight;
        int x=space*previous/units, next=space*used/units;
        if (space<units*44 && units>count) {
            // Reserve a full hit target for single-unit keys on dense IME rows.
            int col=(int)(i-start), extra=space-44*count;
            x=44*col+extra*(previous-col)/(units-count);
            next=44*(col+1)+extra*(used-col-1)/(units-count);
        }
        lv_obj_set_pos(cap->button,x+(int)(i-start)*s->column_gap,y);
        lv_obj_set_size(cap->button,next-x,height);
    }
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
    const unsigned level=legend_level(s);
    unsigned id=0, start=0, row=0;
    for (size_t i=0;i<n+last_n;i++) {
        uint8_t key=i<n ? slots[i] : last[i-n];
        if (key==LINE) { layout_row(s,start,id,row++); start=id; continue; }
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
            label=slot_legend(binding,level,s->caps);
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

        if (key==CAPS) { s->caps_id=(uint8_t)id; label=caps_caption(s); width=s->caps_width; }
        cap_t *cap=&s->keys[id++];
        cap->key=key; cap->usage=usage; cap->weight=(uint8_t)width;
        bool modifier=key>=SHIFT && key<=ALTGR;
        bool function=key>=LAYER && key!=SPACE;
        ui_key_variant_t variant=function ? UI_KEY_MODIFIER : UI_KEY_NORMAL;
        if (cap->desc.variant!=variant) {
            const lv_style_t *normal=ui_theme_style(UI_STYLE_KEY_NORMAL);
            const lv_style_t *special=ui_theme_style(UI_STYLE_KEY_MODIFIER);
            lv_obj_replace_style(cap->button,function ? normal : special,function ? special : normal,LV_PART_MAIN);
            cap->desc.variant=variant;
        }
        set_legend(s,cap,label);
        bool checked=(key==CAPS && s->caps==CAPS_ON) || (key==IME && s->ime_open) ||
                     ((modifier || key==WIN_MOD) && (s->mods & (1u<<(usage-0xe0))));
        ui_action_key_set_checked(cap->button,checked);
        ui_action_key_set_enabled(cap->button,key>=KBD_SLOT_COUNT || usage!=0);
        lv_obj_set_hidden(cap->button,false);
    }
    layout_row(s,start,id,row);
    while (id<s->count) lv_obj_set_hidden(s->keys[id++].button,true);
}

static void clear(surface_t *s,bool cancel)
{
    uint8_t held=s->held, mods=s->mods;
    s->held=s->mods=0;
    if (held) {
        bool ok=!cancel && s->hooks.edge(s->hooks.ctx,false,held);
        for (unsigned bit=0;ok && bit<8;bit++)
            if (mods & (1u<<bit)) ok=s->hooks.edge(s->hooks.ctx,false,(uint8_t)(0xe0+bit));
        if (!ok) s->hooks.release_all(s->hooks.ctx);
    }
    if (mods && !s->closing) show_keys(s);
}

void ui_keyboard_surface_cancel(lv_obj_t *surface)
{
    if (surface) clear(lv_obj_get_user_data(surface),true);
}

static void key_edge(uint16_t id,bool down,void *ctx)
{
    surface_t *s=ctx;
    if (s->closing || id>=s->count) return;
    cap_t *cap=&s->keys[id];
    uint8_t key=cap->key, usage=cap->usage;
    if (!down) {
        if (s->held==usage && usage) clear(s,!s->hooks.ready(s->hooks.ctx));
        return;
    }
    if (!s->hooks.ready(s->hooks.ctx) || s->held) return;
    if (key==LAYER) { s->numeric=!s->numeric; show_keys(s); return; }
    if (key==IME) { s->ime_open=!s->ime_open; show_keys(s); return; }
    if (key==FN || key==FN_BACK) { s->fn_open=key==FN; show_keys(s); return; }
    if ((key>=SHIFT && key<=ALTGR) || key==WIN_MOD) {
        s->mods^=(uint8_t)(1u<<(usage-0xe0)); show_keys(s); return;
    }
    if (!usage) return;
    if (key==WIN && (s->mods & (1u<<3))) { s->mods &= (uint8_t)~(1u<<3); show_keys(s); }
    s->held=usage;
    for (unsigned bit=0;bit<8;bit++)
        if ((s->mods & (1u<<bit)) && !s->hooks.edge(s->hooks.ctx,true,(uint8_t)(0xe0+bit))) { clear(s,true); return; }
    if (!s->hooks.edge(s->hooks.ctx,true,usage)) clear(s,true);
}

static void cancel_edge(lv_event_t *event)
{
    surface_t *s=lv_event_get_user_data(event);
    if (!s->closing && s->held) clear(s,true);
}

static void track_held_key(lv_event_t *event)
{
    surface_t *s=lv_event_get_user_data(event);
    if (!s->held) return;
    if (!s->hooks.ready(s->hooks.ctx)) { clear(s,true); return; }
    lv_obj_t *key=lv_event_get_current_target_obj(event);
    lv_indev_t *indev=lv_event_get_indev(event);
    if (!indev || lv_indev_get_type(indev)!=LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(indev,&point);
    lv_obj_transform_point(key,&point,LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE_RECURSIVE);
    if (!lv_obj_hit_test(key,&point)) {
        clear(s,false);
        lv_obj_set_pressed(key,false);
    }
}

static void destroy(lv_event_t *event)
{
    surface_t *s=lv_event_get_user_data(event);
    s->closing=true;
    clear(s,true);
    // The action buttons borrow descriptors and a sink from this instance.
    lv_obj_clean(s->root);
    lv_free(s);
}

bool ui_keyboard_surface_buffered_target(lv_obj_t *target)
{
    if (!target || !lv_obj_check_type(target,&lv_button_class) || lv_obj_is_disabled(target)) return false;
    // Only keys created by this instance get the registered cancel handler.
    for (uint32_t i=0;i<lv_obj_get_event_count(target);i++) {
        lv_event_dsc_t *event=lv_obj_get_event_dsc(target,i);
        if (lv_event_dsc_get_cb(event)==cancel_edge) {
            surface_t *s=lv_event_dsc_get_user_data(event);
            return !s->closing && lv_obj_is_visible(target) && s->hooks.ready(s->hooks.ctx);
        }
    }
    return false;
}

void ui_keyboard_surface_set_caps(lv_obj_t *surface,bool known,bool on)
{
    if (!surface) return;
    surface_t *s=lv_obj_get_user_data(surface);
    caps_state_t next=known ? (on ? CAPS_ON : CAPS_OFF) : CAPS_UNKNOWN;
    if (s->caps==next) return;
    bool letters_change=s->caps==CAPS_ON || next==CAPS_ON;
    s->caps=next;
    lv_obj_t *key=s->keys[s->caps_id].button;
    ui_action_key_set_label(key,caps_caption(s));
    ui_action_key_set_checked(key,next==CAPS_ON);
    if (letters_change) {
        unsigned level=legend_level(s);
        for (unsigned i=0;i<s->count;i++) {
            cap_t *cap=&s->keys[i];
            if (cap->key>=KBD_SLOT_COUNT || lv_obj_is_hidden(cap->button)) continue;
            const kbd_key_t *binding=kbd_layouts_key(s->layout,(kbd_slot_t)cap->key);
            if (has_caps_pair(binding,level)) set_legend(s,cap,slot_legend(binding,level,next));
        }
    }
}

lv_obj_t *ui_keyboard_surface_create(lv_obj_t *parent,int width,int height,const kbd_layout_t *layout,
                                     const ui_keyboard_surface_hooks_t *hooks)
{
    if (!parent || !layout || !hooks || !hooks->ready || !hooks->edge || !hooks->release_all ||
        width<440 || height<176) return NULL;
    surface_t *s=lv_malloc_zeroed(sizeof(*s));
    if (!s) return NULL;
    s->hooks=*hooks;
    s->sink=(ui_action_key_sink_t){.fn=key_edge,.user_data=s};
    s->layout=layout; s->width=width; s->height=height;
    const kbd_key_t *home_key=kbd_layouts_key(layout,KBD_SLOT_AC10);
    s->home_row_letter=kbd_layouts_case_pair(kbd_layouts_legend(home_key,0),kbd_layouts_legend(home_key,1));
    for (unsigned k=KBD_SLOT_MUHE;k<=KBD_SLOT_HJCV;k++)
        if (kbd_layouts_key(layout,(kbd_slot_t)k)) s->has_ime=true;
    s->column_gap=width>=458 ? 2 : 0; s->row_gap=height>=182 ? 2 : 0;
    int usable_width=width-9*s->column_gap;
    int fixed_units=s->has_ime ? 9 : 8;
    s->caps_width=usable_width>=44*(fixed_units+3) ? 2 : 1;
    if (s->caps_width==2) fixed_units++;
    bool body=usable_width>=50*LV_MAX(10,fixed_units+2);
    s->font=body ? UI_FONT_BODY : UI_FONT_LABEL;
    s->space_width=(uint8_t)LV_MIN(3,usable_width/(body ? 50 : 44)-fixed_units);
    s->root=lv_obj_create(parent);
    if (!s->root || !lv_obj_add_event_cb(s->root,destroy,LV_EVENT_DELETE,s)) {
        if (s->root) lv_obj_delete(s->root);
        lv_free(s); return NULL;
    }
    lv_obj_set_user_data(s->root,s);
    lv_obj_remove_style_all(s->root);
    ui_theme_apply(s->root,UI_STYLE_CONTROL_PAGE,LV_PART_MAIN);
    lv_obj_set_style_pad_all(s->root,0,LV_PART_MAIN);
    lv_obj_set_style_border_width(s->root,0,LV_PART_MAIN);
    lv_obj_set_scrollable(s->root,false);
    lv_obj_set_size(s->root,width,height);
    // Four rows of at most ten keys. Keep every button stable across layer changes.
    for (unsigned i=0;i<40;i++) {
        cap_t *cap=&s->keys[i];
        cap->desc=(ui_action_key_desc_t){.label=" ",.width=44,.height=44,.control_id=(uint16_t)i,.font=s->font};
        cap->button=ui_action_key_create(s->root,&cap->desc,&s->sink);
        if (!cap->button || !lv_obj_add_event_cb(cap->button,cancel_edge,LV_EVENT_INDEV_RESET|LV_EVENT_PREPROCESS,s)
            || !lv_obj_add_event_cb(cap->button,cancel_edge,LV_EVENT_PRESS_LOST|LV_EVENT_PREPROCESS,s)
            || !lv_obj_add_event_cb(cap->button,track_held_key,LV_EVENT_PRESSING,s)) {
            lv_obj_delete(s->root); return NULL;
        }
        lv_obj_set_style_pad_all(cap->button,0,LV_PART_MAIN);
        lv_obj_set_style_min_height(cap->button,UI_TOUCH_MIN,LV_PART_MAIN);
        // Leaving the cap releases its usage; the same contact must never type its neighbour.
        lv_obj_set_press_lock(cap->button,true);
        lv_obj_t *label=lv_obj_get_child(cap->button,-1);
        lv_obj_set_style_max_width(label,LV_COORD_MAX,LV_PART_MAIN);
        lv_label_set_long_mode(label,LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,LV_PART_MAIN);
        s->count++;
    }
    show_keys(s);
    return s->root;
}
