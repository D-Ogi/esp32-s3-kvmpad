// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_autopilot_view.h"
#include "ui_theme.h"
#include <stdio.h>
#include <string.h>

typedef enum { HEADING, ALTITUDE, VERTICAL_SPEED, TARGET_COUNT } target_t;
typedef enum { VIEW_MAIN, VIEW_EDITOR, VIEW_NUMBER, VIEW_OPTIONS } page_t;
typedef enum { NONE, EDIT_HDG, EDIT_ALT, EDIT_VS, AP, HDG, NAV, VS, ALT, DEMO, OPTIONS, OFFLINE,
               STEP_0, STEP_1, STEP_2, STEP_3, CONFIRM, CANCEL, REJECT, ADVANCE, NAV_CAPTURE, ALT_CAPTURE,
               NUMBER, NUMBER_SIGN, NUMBER_BACKSPACE, NUMBER_RETURN, NUMBER_CANCEL, REPORT_ARMED, LATENCY,
               DIGIT_0 } command_t;
typedef enum { MODE_ROL, MODE_HDG, MODE_NAV, MODE_PIT, MODE_VS, MODE_ALT } ap_mode_t;
static const char *const mode_name[]={"ROL","HDG","NAV","PIT","VS","ALT"};
static const struct {
    const char *name,*unit;
    int min,max,small,big;
} fields[TARGET_COUNT]={
    {"HDG","deg",0,359,1,10},{"ALT","ft",0,45000,100,1000},{"VS","ft/min",-6000,6000,100,500}
};

typedef struct {
    bool ap,nav_armed,alt_armed;
    ap_mode_t lateral,vertical;
    int target[TARGET_COUNT],actual[TARGET_COUNT];
} aircraft_t;

static struct {
    lv_obj_t *parent,*body,*draft_label,*confirm,*number_error;
    int width,height,draft;
    target_t editing;
    aircraft_t model;
    page_t page;
    bool package_selection,demo,reject_next,decline,report_armed,slow_reply;
    char number[8]; // optional sign, six digits, terminator
    command_t pending;
    int requested;
    uint32_t sent;
    char notice[96];
} panel;

static void render(void);

static void update_alt_arm(void)
{
    aircraft_t *m=&panel.model;
    int delta=m->target[ALTITUDE]-m->actual[ALTITUDE], vs=m->target[VERTICAL_SPEED];
    m->alt_armed=m->ap && m->vertical==MODE_VS && ((delta>0 && vs>0) || (delta<0 && vs<0));
}

static void capture_alt(void)
{
    aircraft_t *m=&panel.model;
    m->actual[ALTITUDE]=m->target[ALTITUDE]; m->actual[VERTICAL_SPEED]=0;
    m->vertical=MODE_ALT; m->alt_armed=false;
}

static void advance(void)
{
    aircraft_t *m=&panel.model;
    if (m->lateral==MODE_HDG) {
        int delta=(m->target[HEADING]-m->actual[HEADING]+540)%360-180;
        m->actual[HEADING]=(m->actual[HEADING]+LV_CLAMP(-30,delta,30)+360)%360;
    }
    if (m->vertical==MODE_VS) {
        int old=m->actual[ALTITUDE];
        m->actual[VERTICAL_SPEED]=m->target[VERTICAL_SPEED];
        int next=old+m->actual[VERTICAL_SPEED]/2, alt=m->target[ALTITUDE];
        if (m->alt_armed && ((old<=alt && next>=alt) || (old>=alt && next<=alt))) capture_alt();
        else { m->actual[ALTITUDE]=LV_CLAMP(0,next,45000); update_alt_arm(); }
    }
}

static bool is_target(command_t c) { return c>=EDIT_HDG && c<=EDIT_VS; }

static void confirm_command(command_t command)
{
    aircraft_t *m=&panel.model;
    if (is_target(command)) {
        m->target[command-EDIT_HDG]=panel.requested;
        update_alt_arm();
    } else if (command==AP) {
        m->ap=!m->ap; m->lateral=MODE_ROL; m->vertical=MODE_PIT;
        m->nav_armed=false; m->alt_armed=false;
    } else if (command==HDG) { m->lateral=MODE_HDG; m->nav_armed=false; }
    else if (command==NAV && m->lateral!=MODE_NAV) m->nav_armed=!m->nav_armed;
    else if (command==VS) { m->vertical=MODE_VS; update_alt_arm(); }
    else if (command==ALT) { m->vertical=MODE_ALT; m->actual[VERTICAL_SPEED]=0; m->alt_armed=false; }
}

static void format_value(char *out,size_t cap,target_t t,int value)
{
    const char *format=t==HEADING ? "%03d %s" : t==VERTICAL_SPEED ? "%+d %s" : "%d %s";
    snprintf(out,cap,format,value,fields[t].unit);
}

static void describe(char *out,size_t size,command_t c)
{
    if (is_target(c)) {
        target_t t=(target_t)(c-EDIT_HDG);
        char value[28]; format_value(value,sizeof(value),t,panel.requested);
        snprintf(out,size,"%s %s",fields[t].name,value);
    } else {
        const char *desc=c==AP ? (panel.model.ap ? "AP off" : "AP on") :
                         c==HDG ? "HDG" : c==NAV ? "NAV" : c==VS ? "VS" : "ALT";
        snprintf(out,size,"%s",desc);
    }
}

static void submit(command_t c)
{
    if (!panel.demo || panel.pending || (c>=HDG && c<=ALT && !panel.model.ap)) return;
    panel.pending=c; panel.sent=lv_tick_get();
    panel.decline=panel.reject_next; panel.reject_next=false;
    char desc[40]; describe(desc,sizeof(desc),c);
    snprintf(panel.notice,sizeof(panel.notice),"Awaiting demo confirmation: %s",desc);
    panel.page=VIEW_MAIN;
}

static void draft_refresh(void)
{
    char value[28];format_value(value,sizeof(value),panel.editing,panel.draft);
    lv_label_set_text(panel.draft_label,value);
    lv_obj_set_disabled(panel.confirm,panel.draft==panel.model.target[panel.editing]);
}

// A digit key never sends a command. Return copies a valid number into the editor draft.
static bool read_number(int *out)
{
    const char *p=panel.number;
    bool negative=*p=='-';
    if (negative) p++;
    if (!*p) return false;
    int n=0;
    while (*p) n=n*10+(*p++-'0');
    if (negative) n=-n;
    target_t t=panel.editing;
    if (t==HEADING && n==360) n=0;
    if (n<fields[t].min || n>fields[t].max) return false;
    *out=n;
    return true;
}

static void keypad_refresh(void)
{
    int n;
    bool valid=read_number(&n);
    lv_label_set_text(panel.draft_label,*panel.number ? panel.number : "_");
    lv_label_set_text_static(panel.number_error, !*panel.number || !strcmp(panel.number,"-")
                             ? "Enter a number." : valid ? "" : "Value outside the allowed range.");
    lv_obj_set_disabled(panel.confirm,!valid);
}

static void keypad_key(command_t key)
{
    char *digits=panel.number+(panel.number[0]=='-');
    size_t n=strlen(digits);
    if (key==NUMBER_SIGN) {
        if (fields[panel.editing].min>=0) return;
        if (digits!=panel.number) memmove(panel.number,digits,n+1);
        else { memmove(digits+1,digits,n+1); panel.number[0]='-'; }
    } else if (key==NUMBER_BACKSPACE) {
        size_t len=strlen(panel.number);
        if (len) panel.number[len-1]=0;
    } else if (key>=DIGIT_0 && key<DIGIT_0+10) {
        if (n==1 && digits[0]=='0') n=0;
        if (n<6) { digits[n]=(char)('0'+key-DIGIT_0);digits[n+1]=0; }
    }
    keypad_refresh();
}

void pack_autopilot_view_set_demo(bool enabled)
{
    if (!panel.parent || panel.demo==enabled) return;
    panel.demo=enabled;
    panel.page=VIEW_MAIN;
    panel.pending=NONE;
    panel.reject_next=false;
    panel.notice[0]=0;
    if (enabled) {
        panel.report_armed=true;panel.slow_reply=false;
        panel.model=(aircraft_t){.ap=true,.alt_armed=true,.lateral=MODE_HDG,.vertical=MODE_VS,
                                 .target={275,6000,800},.actual={268,5400,700}};
    }
    render();
}

static void clicked(lv_event_t *e)
{
    command_t c=(command_t)(uintptr_t)lv_event_get_user_data(e);
    if (!panel.parent || !lv_obj_is_visible(lv_event_get_current_target_obj(e))) return;
    if (c==DEMO || c==OFFLINE) { pack_autopilot_view_set_demo(c==DEMO); return; }
    if (!panel.demo || panel.pending) return;
    if (c==CANCEL) panel.page=VIEW_MAIN;
    else if (c==OPTIONS) panel.page=VIEW_OPTIONS;
    else if (c==REPORT_ARMED && panel.page==VIEW_OPTIONS) panel.report_armed=!panel.report_armed;
    else if (c==LATENCY && panel.page==VIEW_OPTIONS) panel.slow_reply=!panel.slow_reply;
    else if (c==NUMBER && panel.page==VIEW_EDITOR) { panel.number[0]=0; panel.page=VIEW_NUMBER; }
    else if (panel.page==VIEW_NUMBER) {
        if (c==NUMBER_RETURN) {
            if (!read_number(&panel.draft)) return;
        } else if (c!=NUMBER_CANCEL) { keypad_key(c); return; }
        panel.page=VIEW_EDITOR;
    }
    else if (is_target(c)) {
        panel.page=VIEW_EDITOR; panel.editing=(target_t)(c-EDIT_HDG);
        panel.draft=panel.model.target[panel.editing];
    } else if (c>=STEP_0 && c<=STEP_3 && panel.page==VIEW_EDITOR) {
        target_t t=panel.editing;
        int step=c==STEP_0 || c==STEP_3 ? fields[t].big : fields[t].small;
        int next=panel.draft+(c<=STEP_1 ? -step : step);
        panel.draft=t==HEADING ? (next+360)%360 : LV_CLAMP(fields[t].min,next,fields[t].max);
        draft_refresh();return;
    } else if (c==CONFIRM && panel.page==VIEW_EDITOR && panel.draft!=panel.model.target[panel.editing]) {
        panel.requested=panel.draft; submit((command_t)(EDIT_HDG+(int)panel.editing));
    } else if (c>=AP && c<=ALT) submit(c);
    else if (c==REJECT) {
        panel.reject_next=!panel.reject_next;
    } else if (c==ADVANCE && panel.model.ap) {
        advance();panel.page=VIEW_MAIN;
        snprintf(panel.notice,sizeof(panel.notice),"Demo advanced 30 seconds.");
    } else if (c==NAV_CAPTURE && panel.model.nav_armed) {
        panel.model.lateral=MODE_NAV;panel.model.nav_armed=false;panel.page=VIEW_MAIN;
        snprintf(panel.notice,sizeof(panel.notice),"Demo captured NAV.");
    } else if (c==ALT_CAPTURE && panel.model.alt_armed) {
        capture_alt();panel.page=VIEW_MAIN;
        snprintf(panel.notice,sizeof(panel.notice),"Demo captured altitude.");
    } else return;
    render();
}

static lv_obj_t *label(int x,int y,int w,int h,const char *title,bool heading)
{
    lv_obj_t *l=lv_label_create(panel.body);
    if (!l) return NULL;
    lv_obj_set_style_text_font(l,ui_theme_font(heading ? UI_FONT_HEADING : UI_FONT_BODY),0);
    lv_label_set_text(l,title);lv_obj_set_pos(l,x,y);lv_obj_set_size(l,w,h);
    return l;
}

static lv_obj_t *button(int x,int y,int w,int h,const char *title,command_t c,bool live,bool checked)
{
    lv_obj_t *b=lv_button_create(panel.body);
    if (!b) return NULL;
    ui_theme_control_button(b);
    lv_obj_set_pos(b,x,y);lv_obj_set_size(b,w,h);
    lv_obj_t *l=lv_label_create(b);
    if (!l || !lv_obj_add_event_cb(b,clicked,LV_EVENT_CLICKED,(void *)(uintptr_t)c)) return NULL;
    lv_obj_remove_style_all(l);
    lv_label_set_text(l,title);lv_obj_set_width(l,lv_pct(100));lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_center(l);lv_obj_set_disabled(b,!live);lv_obj_set_checked(b,checked);
    return b;
}

typedef struct {
    lv_layer_t *layer;
    int x,y;
    lv_color_t ink,scale,bug;
} gauge_t;

// The software renderer calls the card's draw event once per intersecting tile.
// Do not allocate draw tasks for gauge marks outside that tile.
static bool gauge_intersects(const gauge_t *g,int left,int top,int right,int bottom)
{
#if LV_DRAW_TRANSFORM_USE_MATRIX
    return true; // The layer's clip and the primitive may be in different spaces.
#else
    const lv_area_t *clip=&g->layer->_clip_area;
    return g->x+right>=clip->x1 && g->x+left<=clip->x2 &&
           g->y+bottom>=clip->y1 && g->y+top<=clip->y2;
#endif
}

static void line(const gauge_t *g,int x1,int y1,int x2,int y2,int width,lv_color_t color)
{
    if (!gauge_intersects(g,LV_MIN(x1,x2)-width,LV_MIN(y1,y2)-width,
                          LV_MAX(x1,x2)+width,LV_MAX(y1,y2)+width)) return;
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1=(lv_point_precise_t){g->x+x1,g->y+y1};d.p2=(lv_point_precise_t){g->x+x2,g->y+y2};
    d.width=width;d.color=color;
    lv_draw_line(g->layer,&d);
}

static void marker(const gauge_t *g,int x1,int y1,int x2,int y2,int x3,int y3)
{
    lv_draw_triangle_dsc_t d;
    lv_draw_triangle_dsc_init(&d);
    d.p[0]=(lv_point_precise_t){g->x+x1,g->y+y1};d.p[1]=(lv_point_precise_t){g->x+x2,g->y+y2};
    d.p[2]=(lv_point_precise_t){g->x+x3,g->y+y3};d.color=g->bug;
    lv_draw_triangle(g->layer,&d);
}

static lv_point_t radial(int a,int radius)
{
    return (lv_point_t){82+radius*lv_trigo_sin((int16_t)a)/32768,
                        108-radius*lv_trigo_sin((int16_t)(a+90))/32768};
}

static void compass(const gauge_t *g)
{
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);arc.center=(lv_point_t){g->x+82,g->y+108};
    arc.radius=88;arc.start_angle=225;arc.end_angle=315;arc.color=g->scale;arc.width=2;
    lv_draw_arc(g->layer,&arc);
    if (!panel.demo) return;
    int actual=panel.model.actual[HEADING];
    for (int a=-42;a<=42;a++) {
        int heading=(actual+a+360)%360;
        if (heading%5) continue;
        lv_point_t p=radial(a,88),q=radial(a,(heading%10) ? 83 : 77);
        line(g,p.x,p.y,q.x,q.y,2,g->ink);
    }
    int delta=(panel.model.target[HEADING]-actual+540)%360-180;
    if (delta<-42) marker(g,5,38,15,31,15,45);
    else if (delta>42) marker(g,159,38,149,31,149,45);
    else {
        lv_point_t p=radial(delta,92);
        marker(g,p.x-6,p.y-6,p.x+6,p.y-6,p.x,p.y+3);
    }
    // Aircraft seen from above; its nose indicates the actual heading.
    line(g,82,34,82,54,3,g->ink);line(g,69,48,82,41,3,g->ink);line(g,82,41,95,48,3,g->ink);
}

static void altitude_tape(const gauge_t *g)
{
    line(g,110,6,110,59,2,g->scale);
    if (!panel.demo) return;
    int actual=panel.model.actual[ALTITUDE];
    for (int feet=LV_MAX(0,(actual-600)/100*100);feet<=actual+600;feet+=100) {
        int y=32-(feet-actual)*25/600;
        if (y>=6 && y<=59) line(g,111,y,feet%500 ? 118 : 133,y,2,g->ink);
    }
    int target_y=LV_CLAMP(7,32-(panel.model.target[ALTITUDE]-actual)*25/600,57);
    for (int x=33;x<89;x+=10) line(g,x,target_y,x+5,target_y,2,g->bug);
    marker(g,94,target_y-5,105,target_y,94,target_y+5);
    line(g,33,32,87,32,3,g->ink);
    line(g,48,32,61,22,3,g->ink);line(g,49,32,61,42,3,g->ink);
}

static void vs_scale(const gauge_t *g)
{
    line(g,16,34,148,34,2,g->scale);
    for (int i=-6;i<=6;i++) line(g,82+11*i,31,82+11*i,i%2 ? 37 : 42,2,g->scale);
    if (!panel.demo) return;
    int target=panel.model.target[VERTICAL_SPEED], actual=panel.model.actual[VERTICAL_SPEED];
    int span=LV_MAX(LV_ABS(target),LV_ABS(actual))>3000 ? 6000 : 3000;
    int x=82+66*actual/span;
    line(g,82,34,x,34,3,g->ink);line(g,x,28,x,42,3,g->ink);
    x=82+66*target/span;
    marker(g,x-6,15,x+6,15,x,25);
}

static void draw_gauge(lv_event_t *e)
{
    lv_obj_t *b=lv_event_get_current_target_obj(e);
    lv_area_t area;lv_obj_get_coords(b,&area);
    target_t which=(target_t)(uintptr_t)lv_event_get_user_data(e);
    gauge_t g={.layer=lv_event_get_layer(e),.x=area.x1+6,.y=(area.y1+area.y2)/2-32};
    // Includes line widths, the whole compass arc and both off-scale markers.
    if (!gauge_intersects(&g,-8,0,172,which==HEADING ? 198 : 64)) return;
    lv_color_t ink=lv_obj_get_style_text_color(b,LV_PART_MAIN);
    ui_control_palette_t palette=ui_theme_control_palette();
    g.ink=ink;
    g.scale=lv_color_mix(ink,lv_obj_get_style_bg_color(b,LV_PART_MAIN),150);
    g.bug=lv_color_hex(palette==UI_PALETTE_DAY ? 0x0064a8 : palette==UI_PALETTE_NIGHT ? 0x93d7fc : 0xffdf67);
    if (which==HEADING) compass(&g);
    else if (which==ALTITUDE) altitude_tape(&g);
    else vs_scale(&g);
}

static bool main_page(void)
{
    int w=panel.width;
    const aircraft_t *m=&panel.model;
    bool on=panel.demo,free=on && !panel.pending;
    const char *title=panel.package_selection ? "Autopilot" : on ? "Autopilot  /  Demo" : "Autopilot  /  Offline";
    if (!label(8,6,w-190,36,title,true) ||
        (!panel.package_selection && !button(w-172,0,164,48,on ? "Go offline" : "Start demo",on ? OFFLINE : DEMO,true,false))) return false;
    if (!label(8,60,100,30,"FMA",true) ||
        !label(110,62,(w-130)/2,28,"Lateral",false) ||
        !label((w+102)/2,62,(w-130)/2,28,"Vertical",false)) return false;
    char row[96];
    const char *l=on && m->ap ? mode_name[m->lateral] : "--";
    const char *v=on && m->ap ? mode_name[m->vertical] : "--";
    const char *la=on && m->nav_armed ? "NAV" : "--", *va=on && m->alt_armed ? "ALT" : "--";
    if (!on) la=va="Unavailable";
    else if (!panel.report_armed) la=va="Not reported";
    if (!label(8,103,100,28,"Active",false) || !label(8,138,100,28,"Armed",false) ||
        !label(110,100,(w-130)/2,32,l,true) || !label((w+102)/2,100,(w-130)/2,32,v,true) ||
        !label(110,136,(w-130)/2,32,la,true) || !label((w+102)/2,136,(w-130)/2,32,va,true)) return false;
    int grow=LV_CLAMP(0,(panel.height-680)/4,48);
    int stride=84+grow, modes_y=182+3*stride+13;
    for (unsigned i=0;i<TARGET_COUNT;i++) {
        char set[28]="--",actual[28]="--";
        if (on) {format_value(set,sizeof(set),(target_t)i,m->target[i]);format_value(actual,sizeof(actual),(target_t)i,m->actual[i]);}
        int y=182+(int)i*stride;
        snprintf(row,sizeof(row),"%s   %s",fields[i].name,set);
        lv_obj_t *card=button(8,y,w-16,stride-9,row,(command_t)(EDIT_HDG+i),free,false);
        if (!card || !lv_obj_add_event_cb(card,draw_gauge,LV_EVENT_DRAW_MAIN_END,(void *)(uintptr_t)i)) return false;
        lv_obj_set_style_pad_all(card,0,0);
        lv_obj_t *value=lv_obj_get_child(card,0);
        lv_obj_set_size(value,w-204,34);lv_obj_set_style_text_font(value,ui_theme_font(UI_FONT_TITLE),0);
        lv_obj_set_style_text_align(value,LV_TEXT_ALIGN_LEFT,0);lv_obj_align(value,LV_ALIGN_LEFT_MID,174,-13);
        lv_obj_t *readback=lv_label_create(card);
        if (!readback) return false;
        lv_obj_remove_style_all(readback);
        lv_label_set_text_fmt(readback,"Actual  %s",actual);
        lv_obj_set_style_text_font(readback,ui_theme_font(UI_FONT_BODY),0);
        lv_obj_align(readback,LV_ALIGN_LEFT_MID,174,20);
    }
    const char *ap=on ? (m->ap ? "AP on" : "AP off") : "AP --";
    if (!button(8,modes_y,106,50,ap,AP,free,on && m->ap)) return false;
    static const char *const modes[]={"HDG","NAV","VS","ALT"};
    int step=(w-126)/4;
    for (unsigned i=0;i<4;i++) {
        bool active=on && m->ap && (!strcmp(l,modes[i]) || !strcmp(v,modes[i]));
        if (!button(120+(int)i*step,modes_y,step-5,50,modes[i],(command_t)(HDG+i),free && m->ap,active)) return false;
    }
    const char *notice=on ? (panel.notice[0] ? panel.notice : "Local demo. Tap a target to edit it.")
                          : panel.package_selection ? "No simulator connection." : "No simulator connection. Start demo to try the controls.";
    if (!label(8,modes_y+68,w-16,62,notice,false)) return false;
    return !on || button(8,panel.height-64,w-16,52,"Demo controls",OPTIONS,free,false);
}

static bool editor(void)
{
    target_t t=panel.editing;
    int w=panel.width;
    char text[80],value[28];
    snprintf(text,sizeof(text),"Set %s",fields[t].name);
    if (!label(12,18,w-24,50,text,true)) return false;
    format_value(value,sizeof(value),t,panel.model.target[t]);
    snprintf(text,sizeof(text),"Confirmed target: %s",value);
    if (!label(12,88,w-24,40,text,false)) return false;
    panel.draft_label=label(12,150,w-204,64,"",true);
    if (!button(w-182,139,170,54,"Enter number",NUMBER,true,false)) return false;
    int step=(w-20)/4;
    for (int i=0;i<4;i++) {
        int n=i==0 || i==3 ? fields[t].big : fields[t].small;
        snprintf(text,sizeof(text),"%+d",i<2 ? -n : n);
        if (!button(10+i*step,242,step-6,60,text,(command_t)(STEP_0+i),true,false)) return false;
    }
    if (t==HEADING) snprintf(text,sizeof(text),"000 to 359 degrees");
    else snprintf(text,sizeof(text),"%d to %d %s",fields[t].min,fields[t].max,fields[t].unit);
    if (!label(12,326,w-24,42,text,false)) return false;
    if (!button(10,410,(w-28)/2,56,"Cancel",CANCEL,true,false)) return false;
    panel.confirm=button(18+(w-28)/2,410,(w-28)/2,56,"Apply",CONFIRM,true,true);
    if (!panel.draft_label || !panel.confirm) return false;
    draft_refresh();return true;
}

static bool keypad(void)
{
    const int w=panel.width,step=(w-16)/3;
    const target_t t=panel.editing;
    char text[64];snprintf(text,sizeof(text),"Enter %s  /  %s",fields[t].name,fields[t].unit);
    if (!label(12,12,w-24,44,text,true)) return false;
    snprintf(text,sizeof(text),"%d to %d %s",fields[t].min,t==HEADING ? 360 : fields[t].max,fields[t].unit);
    if (!label(12,65,w-24,32,t==HEADING ? "000 to 360 deg (360 = 000)" : text,false)) return false;
    panel.draft_label=label(12,107,w-24,44,"",true);
    panel.number_error=label(12,155,w-24,32,"",false);
    if (!panel.draft_label || !panel.number_error) return false;
    for (unsigned i=0;i<9;i++) {
        char digit[2]={(char)('1'+i),0};
        if (!button(8+(int)(i%3)*step,211+(int)(i/3)*68,step-6,60,digit,
                    (command_t)(DIGIT_0+i+1),true,false)) return false;
    }
    if (!button(8,415,step-6,60,"+/-",NUMBER_SIGN,t==VERTICAL_SPEED,false) ||
        !button(8+step,415,step-6,60,"0",DIGIT_0,true,false) ||
        !button(8+2*step,415,step-6,60,"Backspace",NUMBER_BACKSPACE,true,false) ||
        !button(8,510,(w-22)/2,56,"Cancel",NUMBER_CANCEL,true,false)) return false;
    panel.confirm=button(14+(w-22)/2,510,(w-22)/2,56,"Return",NUMBER_RETURN,true,true);
    if (!panel.confirm) return false;
    keypad_refresh();return true;
}

static bool options(void)
{
    int w=panel.width;
    if (!label(12,12,w-24,50,"Demo controls",true)) return false;
    bool ap=panel.model.ap;
    return button(12,84,w-24,56,"Advance 30 seconds",ADVANCE,ap,false) &&
           button(12,156,w-24,56,"Capture NAV",NAV_CAPTURE,panel.model.nav_armed,false) &&
           button(12,228,w-24,56,"Capture altitude",ALT_CAPTURE,panel.model.alt_armed,false) &&
           button(12,300,w-24,56,panel.reject_next ? "Reject next request: on" : "Reject next request: off",
                  REJECT,true,panel.reject_next) &&
           button(12,372,w-24,56,panel.report_armed ? "Report armed modes: on" : "Report armed modes: off",
                  REPORT_ARMED,true,panel.report_armed) &&
           button(12,444,w-24,56,panel.slow_reply ? "Response delay: 2500 ms" : "Response delay: 900 ms",
                  LATENCY,true,false) &&
           button(12,532,w-24,56,"Back",CANCEL,true,false);
}

static void clear_body(void)
{
    if (panel.body) lv_obj_delete(panel.body);
    panel.body=panel.draft_label=panel.confirm=panel.number_error=NULL;
}

static void render(void)
{
    if (!panel.body) return;
    lv_obj_clean(panel.body);panel.draft_label=panel.confirm=panel.number_error=NULL;
    bool ok=panel.page==VIEW_MAIN ? main_page() : panel.page==VIEW_EDITOR ? editor() : panel.page==VIEW_NUMBER ? keypad() : options();
    if (!ok) clear_body();
}

static void deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e)==panel.parent) memset(&panel,0,sizeof(panel));
}

bool pack_autopilot_view_open(lv_obj_t *parent,int width,int height,bool package_selection)
{
    if (!parent || panel.parent || width<540 || height<680) return false;
    if (!lv_obj_add_event_cb(parent,deleted,LV_EVENT_DELETE,NULL)) return false;
    lv_obj_set_style_pad_all(parent,0,0);lv_obj_set_style_border_width(parent,0,0);lv_obj_set_scrollable(parent,false);
    panel.parent=parent;panel.width=width;panel.height=height;panel.package_selection=package_selection;
    return true;
}

void pack_autopilot_view_tick(void)
{
    if (!panel.parent) return;
    if (lv_obj_get_screen(panel.parent)!=lv_screen_active() || !lv_obj_is_visible(panel.parent)) {
        // A settled main page is safe to reuse. Discard drafts and request controls
        // so returning shows the confirmed model and the cancellation notice.
        if (panel.page!=VIEW_MAIN || panel.pending) clear_body();
        panel.page=VIEW_MAIN;
        if (panel.pending) snprintf(panel.notice,sizeof(panel.notice),"Demo request cancelled.");
        panel.pending=NONE;return;
    }
    if (!panel.body) {
        panel.body=lv_obj_create(panel.parent);
        if (!panel.body) return;
        lv_obj_remove_style_all(panel.body);ui_theme_apply(panel.body,UI_STYLE_CONTROL_PAGE,0);
        lv_obj_set_pos(panel.body,0,0);lv_obj_set_size(panel.body,panel.width,panel.height);
        lv_obj_set_scrollable(panel.body,false);render();
    }
    if (panel.pending && lv_tick_elaps(panel.sent)>=(panel.slow_reply ? 2500u : 900u)) {
        command_t c=panel.pending;
        char desc[40];describe(desc,sizeof(desc),c);
        panel.pending=NONE;
        snprintf(panel.notice,sizeof(panel.notice),panel.decline ? "Demo rejected: %s" : "Demo confirmed: %s",desc);
        if (!panel.decline) confirm_command(c);
        render();
    }
}

#if CONFIG_KVM_UI_BENCH
bool pack_autopilot_view_waiting_for_body(void)
{
    return panel.parent && !panel.body && lv_obj_is_visible(panel.parent);
}
#endif

void pack_autopilot_view_close(void)
{
    if (!panel.parent) return;
    lv_obj_remove_event_cb_with_user_data(panel.parent,deleted,NULL);
    clear_body();memset(&panel,0,sizeof(panel));
}
