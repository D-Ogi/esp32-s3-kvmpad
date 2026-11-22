// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_radio_view.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

#if !LV_FONT_MONTSERRAT_36
LV_FONT_DECLARE(lv_font_montserrat_36)
#endif

typedef enum { OFFLINE, DEMO, LIVE } link_t;
typedef enum { NO_EDIT, FREQUENCY, CODE } edit_t;
typedef enum { LINK_BUTTON, COM1, COM2, STANDBY, SWAP, SQUAWK, MODE_STBY, MODE_ON, MODE_ALT,
               BACKSPACE, ACCEPT, CANCEL, DIGIT } key_t;
typedef struct { lv_obj_t *obj,*label; } face_t;
static const char *const modes[]={"STBY","ON","ALT"};
static uint32_t next_id;
static struct {
    lv_obj_t *parent,*body,*status,*notice,*active;
    face_t link,com[2],standby,swap,code,mode[3],digits[10],back,ok,cancel;
    int width,height;
    link_t source;
    unsigned com_index;
    bool package_selection;
    edit_t edit;
    char input[7],message[84];
    pack_radio_readback_t confirmed;
    pack_radio_request_t pending;
    uint32_t old_active,old_standby,sent_at;
    pack_radio_sender_t send;
    void *sender_ctx;
} panel;

static void project(void);

static bool channel(uint32_t khz)
{
    // The displayed 25 kHz and 8.33 kHz channel designators use integer kHz.
    return khz>=118000 && khz<=136990 && khz%5==0 && khz%25!=20;
}

static bool valid_sample(const pack_radio_readback_t *s)
{
    return s && channel(s->active_khz[0]) && channel(s->standby_khz[0]) &&
           channel(s->active_khz[1]) && channel(s->standby_khz[1]) && s->squawk<=07777 &&
           (unsigned)s->mode<=PACK_RADIO_ALT && !(s->mode_mask & ~7u) &&
           (s->mode_mask & (1u<<(unsigned)s->mode));
}

static void message(const char *text)
{
    snprintf(panel.message,sizeof(panel.message),"%s",text);
}

static void end_edit(void)
{
    panel.edit=NO_EDIT;
    panel.input[0]=0;
}

void pack_radio_view_select(unsigned index)
{
    if (!panel.parent || index>=2 || panel.com_index==index) return;
    panel.com_index=index;
    end_edit();
    if (panel.source!=OFFLINE && !panel.pending.id) message("");
    project();
}

void pack_radio_view_bind(pack_radio_sender_t sender,void *ctx)
{
    if (!panel.parent) return;
    panel.send=sender;panel.sender_ctx=ctx;
    panel.source=OFFLINE;panel.pending.id=0;end_edit();
    memset(&panel.confirmed,0,sizeof(panel.confirmed));
    message(sender ? "Waiting for radio readback." : "No simulator connection.");
    project();
}

static bool matched(void)
{
    const pack_radio_request_t *p=&panel.pending;
    const pack_radio_readback_t *s=&panel.confirmed;
    switch(p->action) {
    case PACK_RADIO_TUNE: return s->standby_khz[p->com]==p->value;
    case PACK_RADIO_SWAP: return s->active_khz[p->com]==panel.old_standby && s->standby_khz[p->com]==panel.old_active;
    case PACK_RADIO_SQUAWK: return s->squawk==p->value;
    case PACK_RADIO_MODE: return (unsigned)s->mode==p->value;
    }
    return false;
}

bool pack_radio_view_readback(const pack_radio_readback_t *snapshot,uint32_t reply_id,bool rejected)
{
    if (!panel.parent || !panel.send || (reply_id && reply_id!=panel.pending.id)) return false;
    if (!valid_sample(snapshot)) {
        pack_radio_view_bind(panel.send,panel.sender_ctx);
        message("Invalid simulator readback.");project();return false;
    }
    panel.confirmed=*snapshot;
    if (panel.source!=LIVE) { panel.source=LIVE;message("Simulator connected."); }
    if (reply_id) {
        message(rejected ? "Simulator rejected the request." :
                matched() ? "Confirmed by simulator." : "Simulator returned a different value.");
        panel.pending.id=0;
    }
    project();return true;
}

static void request(pack_radio_action_t action,uint32_t value)
{
    if (panel.source==OFFLINE || panel.pending.id) return;
    if (++next_id==0) ++next_id;
    unsigned c=panel.com_index;
    panel.old_active=panel.confirmed.active_khz[c];panel.old_standby=panel.confirmed.standby_khz[c];
    panel.pending=(pack_radio_request_t){.id=next_id,.action=action,.com=c,.value=value};
    panel.sent_at=lv_tick_get();end_edit();
    message(panel.source==DEMO ? "Applying local demo change." : "Awaiting simulator readback.");
    if (panel.source==LIVE && !panel.send(&panel.pending,panel.sender_ctx)) {
        panel.pending.id=0;message("Request could not be sent.");
    }
}

static bool entered(uint32_t *value)
{
    const size_t needed=panel.edit==CODE ? 4u : 6u;
    if (panel.edit==NO_EDIT || strlen(panel.input)!=needed) return false;
    uint32_t n=0;
    for(size_t i=0;i<needed;i++) n=n*(panel.edit==CODE ? 8u : 10u)+(unsigned)(panel.input[i]-'0');
    *value=n;
    return panel.edit==CODE || channel(n);
}

static void click(lv_event_t *e)
{
    if (!panel.body || !lv_obj_is_visible(lv_event_get_current_target_obj(e))) return;
    key_t key=(key_t)(uintptr_t)lv_event_get_user_data(e);
    if (key==LINK_BUTTON) {
        if (panel.send) return;
        if (panel.source==DEMO) { pack_radio_view_bind(NULL,NULL);return; }
        panel.source=DEMO;
        panel.confirmed=(pack_radio_readback_t){.active_khz={118100,123450},.standby_khz={121900,124850},
                                               .squawk=07000,.mode=PACK_RADIO_ALT,.mode_mask=7};
        message("Local demo. Tap Standby or the transponder code.");
    } else if (key==COM1 || key==COM2) {
        if (panel.pending.id) return;
        pack_radio_view_select((unsigned)(key-COM1));
        return;
    } else if (panel.source==OFFLINE || panel.pending.id) return;
    else if (key==STANDBY || key==SQUAWK) {
        edit_t next=key==STANDBY ? FREQUENCY : CODE;
        if (panel.edit!=next) {end_edit();panel.edit=next;}
    } else if (key==CANCEL) {end_edit();message("Edit cancelled.");}
    else if (key==BACKSPACE) {
        size_t n=strlen(panel.input);if (n) panel.input[n-1]=0;
    } else if (key>=DIGIT && key<DIGIT+10) {
        unsigned d=(unsigned)(key-DIGIT);
        if(panel.edit==NO_EDIT || (panel.edit==CODE && d>7)) return;
        size_t n=strlen(panel.input);
        if(n<(panel.edit==CODE ? 4u : 6u)){panel.input[n]=(char)('0'+d);panel.input[n+1]=0;}
    } else if (key==ACCEPT) {
        uint32_t v;if (!entered(&v)) return;
        request(panel.edit==CODE ? PACK_RADIO_SQUAWK : PACK_RADIO_TUNE,v);
    } else if (panel.edit!=NO_EDIT) return;
    else if (key==SWAP) request(PACK_RADIO_SWAP,0);
    else if (key>=MODE_STBY && key<=MODE_ALT) {
        unsigned m=(unsigned)(key-MODE_STBY);
        if (!(panel.confirmed.mode_mask & (1u<<m))) return;
        request(PACK_RADIO_MODE,m);
    }
    project();
}

static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,int w,const lv_font_t *font)
{
    lv_obj_t *l=lv_label_create(parent);
    if (!l) return NULL;
    lv_obj_remove_style_all(l);lv_label_set_text(l,text);
    lv_obj_set_style_text_font(l,font,0);lv_obj_set_pos(l,x,y);lv_obj_set_width(l,w);
    return l;
}

static bool button(face_t *f,key_t key,const char *title,int x,int y,int w,int h)
{
    f->obj=lv_button_create(panel.body);
    if (!f->obj || !lv_obj_add_event_cb(f->obj,click,LV_EVENT_CLICKED,(void *)(uintptr_t)key)) return false;
    ui_theme_control_button(f->obj);lv_obj_set_style_pad_all(f->obj,0,0);
    lv_obj_set_pos(f->obj,x,y);lv_obj_set_size(f->obj,w,h);
    f->label=label(f->obj,title,0,0,w-8,ui_theme_font(UI_FONT_HEADING));
    if (!f->label) return false;
    lv_obj_set_style_text_align(f->label,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(f->label);
    return true;
}

static void large_digits(lv_obj_t *label)
{
    lv_obj_set_width(label,LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(label,&lv_font_montserrat_36,0);
}

static bool build(void)
{
    int w=panel.width,h=panel.height,pad=8,half=(w-24)/2;
    panel.body=lv_obj_create(panel.parent);
    if (!panel.body) return false;
    lv_obj_remove_style_all(panel.body);ui_theme_apply(panel.body,UI_STYLE_CONTROL_PAGE,0);
    lv_obj_set_pos(panel.body,0,0);lv_obj_set_size(panel.body,w,h);lv_obj_set_scrollable(panel.body,false);
    if (!label(panel.body,"Radio and transponder",pad,0,w-158,ui_theme_font(UI_FONT_TITLE))) return false;
    panel.status=label(panel.body,"",pad,32,w-158,ui_theme_font(UI_FONT_BODY));
    if (!panel.status || !button(&panel.link,LINK_BUTTON,"",w-148,3,140,48)) return false;
    if (!panel.package_selection && (!button(&panel.com[0],COM1,"COM 1",pad,59,half,50) ||
        !button(&panel.com[1],COM2,"COM 2",16+half,59,half,50))) return false;
    const int field=70+(h-680)/12, base=panel.package_selection ? 59 : 117, fw=w-122;
    lv_obj_t *active=lv_obj_create(panel.body);
    if (!active) return false;
    lv_obj_remove_style_all(active);ui_theme_apply(active,UI_STYLE_CONTROL_BUTTON,0);
    lv_obj_set_pos(active,pad,base);lv_obj_set_size(active,fw,field);lv_obj_set_scrollable(active,false);
    lv_obj_set_style_pad_all(active,0,0);
    if (!label(active,"ACTIVE",12,3,fw-24,ui_theme_font(UI_FONT_BODY))) return false;
    panel.active=label(active,"",12,0,fw-24,ui_theme_font(UI_FONT_TITLE));
    if (!panel.active) return false;
    large_digits(panel.active);lv_obj_align(panel.active,LV_ALIGN_LEFT_MID,12,11);
    if (!button(&panel.standby,STANDBY,"",pad,base+field+5,fw,field) ||
        !label(panel.standby.obj,"STANDBY",12,3,fw-24,ui_theme_font(UI_FONT_BODY)) ||
        !button(&panel.swap,SWAP,"Swap",w-106,base,98,2*field+5)) return false;
    lv_obj_set_style_text_align(panel.standby.label,LV_TEXT_ALIGN_LEFT,0);
    large_digits(panel.standby.label);
    lv_obj_align(panel.standby.label,LV_ALIGN_LEFT_MID,12,11);
    int info=base+2*field+12;
    panel.notice=label(panel.body,"",pad,info+9,w-128,ui_theme_font(UI_FONT_BODY));
    if (!panel.notice || !button(&panel.cancel,CANCEL,"Cancel",w-112,info,104,44)) return false;
    int top=info+52, step=(h-112-top)/4, col=(w-32)/3;
    for(unsigned i=0;i<12;i++) {
        unsigned d=(i+1)%10;
        face_t *f=i==9 ? &panel.back : i==11 ? &panel.ok : &panel.digits[i==10 ? 0 : d];
        key_t k=i==9 ? BACKSPACE : i==11 ? ACCEPT : (key_t)(DIGIT+(i==10 ? 0 : d));
        char s[2]={(char)('0'+(i==10 ? 0 : d)),0};
        if (!button(f,k,i==9 ? LV_SYMBOL_BACKSPACE : i==11 ? "OK" : s,
                    pad+(int)(i%3)*(col+8),top+(int)(i/3)*step,col,step-5)) return false;
        large_digits(f->label);
    }
    if (!label(panel.body,"TRANSPONDER",pad,h-101,w-16,ui_theme_font(UI_FONT_BODY))) return false;
    int code_width=w/3,mode_width=(w-code_width-40)/3;
    if (!button(&panel.code,SQUAWK,"",pad,h-70,code_width,60)) return false;
    large_digits(panel.code.label);
    for(unsigned i=0;i<3;i++)
        if (!button(&panel.mode[i],(key_t)(MODE_STBY+i),modes[i],16+code_width+(int)i*(mode_width+8),
                    h-70,mode_width,60)) return false;
    return true;
}

static void set(lv_obj_t *l,const char *text)
{
    if (strcmp(lv_label_get_text(l),text)) lv_label_set_text(l,text);
}

static void frequency(char *out,size_t cap,uint32_t khz)
{
    snprintf(out,cap,"%u.%03u MHz",(unsigned)(khz/1000),(unsigned)(khz%1000));
}

static void project(void)
{
    if (!panel.body) return;
    bool live=panel.source!=OFFLINE, ready=live && !panel.pending.id, editing=panel.edit!=NO_EDIT;
    unsigned c=panel.com_index;
    const pack_radio_readback_t *s=&panel.confirmed;
    set(panel.status,panel.source==DEMO ? "Demo" : panel.source==LIVE ? "Simulator online" : "Offline");
    set(panel.link.label,panel.source==DEMO ? "Go offline" : "Start demo");lv_obj_set_hidden(panel.link.obj,panel.send!=NULL);
    for(unsigned i=0;!panel.package_selection && i<2;i++){
        lv_obj_set_checked(panel.com[i].obj,c==i);lv_obj_set_disabled(panel.com[i].obj,panel.pending.id!=0);
    }
    char a[24]="---.--- MHz",b[24]="---.--- MHz",xpdr[12]="----",notice[84];
    if (live){frequency(a,sizeof(a),s->active_khz[c]);frequency(b,sizeof(b),s->standby_khz[c]);snprintf(xpdr,sizeof(xpdr),"%04o",(unsigned)s->squawk);}
    snprintf(notice,sizeof(notice),"%s",panel.message);
    uint32_t v=0;bool valid=entered(&v);
    if (editing) {
        bool code=panel.edit==CODE;char entry[7]="______";
        memcpy(entry,panel.input,strlen(panel.input));
        snprintf(notice,sizeof(notice),code ? "Edit code (0-7). Current %s" : "Edit standby. Current %s",code ? xpdr : b);
        if (code) snprintf(xpdr,sizeof(xpdr),"%.4s",entry);
        else {
            snprintf(b,sizeof(b),"%.3s.%.3s MHz",entry,entry+3);
            if (strlen(panel.input)==6 && !valid) snprintf(notice,sizeof(notice),"Invalid COM channel. Use a valid 118-136 MHz channel.");
        }
    }
    set(panel.active,a);set(panel.standby.label,b);set(panel.code.label,xpdr);set(panel.notice,notice);
    lv_obj_set_hidden(panel.cancel.obj,!editing);lv_obj_set_checked(panel.standby.obj,panel.edit==FREQUENCY);
    lv_obj_set_checked(panel.code.obj,panel.edit==CODE);lv_obj_set_disabled(panel.standby.obj,!ready);
    lv_obj_set_disabled(panel.code.obj,!ready);lv_obj_set_disabled(panel.swap.obj,!ready || editing);
    for(unsigned i=0;i<10;i++)lv_obj_set_disabled(panel.digits[i].obj,!editing || (panel.edit==CODE && i>7));
    lv_obj_set_disabled(panel.back.obj,!editing || !panel.input[0]);lv_obj_set_disabled(panel.ok.obj,!valid);
    for(unsigned i=0;i<3;i++){
        lv_obj_set_checked(panel.mode[i].obj,live && (unsigned)s->mode==i);
        lv_obj_set_disabled(panel.mode[i].obj,!ready || editing || !(s->mode_mask & (1u<<i)));
    }
}

static void deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e)==panel.parent) memset(&panel,0,sizeof(panel));
}

bool pack_radio_view_open(lv_obj_t *parent,int width,int height,bool package_selection)
{
    if (!parent || panel.parent || width<540 || height<680) return false;
    if (!lv_obj_add_event_cb(parent,deleted,LV_EVENT_DELETE,NULL)) return false;
    lv_obj_set_style_pad_all(parent,0,0);lv_obj_set_style_border_width(parent,0,0);lv_obj_set_scrollable(parent,false);
    panel.parent=parent;panel.width=width;panel.height=height;panel.package_selection=package_selection;
    message("No simulator connection.");
    return true;
}

void pack_radio_view_tick(void)
{
    if (!panel.parent) return;
    bool visible=lv_obj_get_screen(panel.parent)==lv_screen_active() && lv_obj_is_visible(panel.parent);
    if (!visible) {
        // A delivered simulator request can still change the cockpit while this page is hidden.
        if (panel.edit || (panel.pending.id && panel.source==DEMO)) {
            end_edit();panel.pending.id=0;message("Edit closed.");project();
        }
    } else if (!panel.body) {
        if (!build()){if (panel.body) lv_obj_delete(panel.body);panel.body=NULL;return;}
        project();
    }
    if (!panel.pending.id) return;
    uint32_t elapsed=lv_tick_elaps(panel.sent_at);
    pack_radio_request_t p=panel.pending;
    if (panel.source==DEMO && elapsed>=200) {
        switch(p.action) {
        case PACK_RADIO_TUNE: panel.confirmed.standby_khz[p.com]=p.value;break;
        case PACK_RADIO_SWAP: panel.confirmed.active_khz[p.com]=panel.old_standby;panel.confirmed.standby_khz[p.com]=panel.old_active;break;
        case PACK_RADIO_MODE: panel.confirmed.mode=(pack_radio_mode_t)p.value;break;
        case PACK_RADIO_SQUAWK: panel.confirmed.squawk=(uint16_t)p.value;break;
        }
        panel.pending.id=0;message("Applied in local demo.");project();
    } else if (elapsed>=3000) {
        panel.pending.id=0;message("No simulator confirmation.");project();
    }
}

void pack_radio_view_close(void)
{
    if (!panel.parent) return;
    lv_obj_remove_event_cb_with_user_data(panel.parent,deleted,NULL);
    if (panel.body) lv_obj_delete(panel.body);
    memset(&panel,0,sizeof(panel));
}
