// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_scene.h"

#include <stdio.h>
#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef kvm_json_doc_t doc_t;
#define KEYS(d, obj, ...) kvm_json_keys_allowed(d, obj, (const char *const[]){__VA_ARGS__, NULL})

static int member(const doc_t *d, int object, const char *name)
{
    return kvm_json_member(d, object, name);
}

static bool equals(const doc_t *d, int obj, const char *key, const char *value)
{
    return kvm_json_str_eq(d, member(d, obj, key), value);
}

static bool string(const doc_t *d, int obj, const char *key, char *out, size_t cap)
{
    size_t n = kvm_json_str_copy(d, member(d, obj, key), out, cap);
    return n != (size_t)-1 && n > 0 && strlen(out)==n;
}

static int number(const doc_t *d, int obj, const char *key, int low, int high)
{
    int64_t value;
    if (!kvm_json_get_i64(d, member(d, obj, key), &value) || value < low || value > high) {
        return -1;
    }
    return (int)value;
}

static int array_count(const doc_t *d, int arr)
{
    return kvm_json_type(d, arr) == KVM_JSON_ARRAY ? (int)kvm_json_count(d, arr) : -1;
}

static bool letter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool name_valid(const char *s, bool dots)
{
    if (!letter(*s)) return false;
    for (; *s; ++s) {
        if (!letter(*s) && (*s < '0' || *s > '9') && *s != '-' && *s != '_' && !(dots && *s == '.')) return false;
    }
    return true;
}

bool pack_profile_id_valid(const char *s)
{
    if (!s) return false;
    unsigned dots = 0;
    bool first = true;
    for (unsigned length=0; *s; ++s, ++length) {
        if (length == 100) return false;
        if (*s >= 'a' && *s <= 'z') first = false;
        else if (!first && *s == '.') { ++dots; first = true; }
        else if (first || ((*s < '0' || *s > '9') && *s != '-')) return false;
    }
    return dots >= 2 && !first;
}

static bool version(const char *s, unsigned out[3])
{
    for (unsigned i = 0; i < 3; i++) {
        unsigned n = 0;
        if (*s < '0' || *s > '9') return false;
        while (*s >= '0' && *s <= '9') {
            if (n > 6553 || (n == 6553 && *s > '5')) return false;
            n = n * 10 + (*s++ - '0');
        }
        out[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return !*s;
}

static int permission_mask(const doc_t *d, int arr)
{
    const char *const names[] = {"ui.basic", "hid.joystick", "host.telemetry.read", "host.screen.region",
                                 "usb.monitor", "hid.keyboard", "hid.mouse"};
    int count = array_count(d, arr), mask = 0;
    const unsigned names_count = sizeof(names)/sizeof(names[0]);
    if (count < 0 || (unsigned)count > names_count) return -1;
    for (int i = 0; i < count; i++) {
        int t = kvm_json_element(d, arr, (uint16_t)i), bit = 0;
        for (unsigned j = 0; j < names_count; j++) if (kvm_json_str_eq(d, t, names[j])) bit = 1 << j;
        if (!bit || (mask & bit)) return -1;
        mask |= bit;
    }
    return mask;
}

bool pack_scene_manifest(pack_scene_t *s, const doc_t *d)
{
    memset(s, 0, sizeof(*s));
    char minimum[32], description[721];
    unsigned ver[3];
    int usb = member(d, 0, "usb"), host = member(d, 0, "host"), limits = member(d, 0, "limits");
    int permissions = member(d, 0, "permissions"), required = member(d, 0, "required_permissions");
    int grants = permission_mask(d, permissions), mandatory = permission_mask(d, required);
    int usb_caps = permission_mask(d, member(d, usb, "requires"));
    int rate = number(d, limits, "max_events_per_sec", 1, 100);
    int hc = member(d, host, "requires");
    const int controls = PACK_PERM_MONITOR | PACK_PERM_KEYBOARD | PACK_PERM_MOUSE;
    bool combined_usb = equals(d,usb,"mode","monitor_controls");
    if (combined_usb) {
        if (!(grants & PACK_PERM_MONITOR) || (grants & PACK_PERM_JOYSTICK) ||
            usb_caps != (grants & controls) || (usb_caps & ~mandatory)) return false;
    } else if (!equals(d,usb,"mode","static_superset") || (grants & controls) ||
               usb_caps != (grants & PACK_PERM_JOYSTICK)) return false;
    if (!KEYS(d, 0, "schema_version","id","version","min_firmware","title","description","category","entry_view",
              "permissions","required_permissions","usb","host","limits") ||
        number(d, 0, "schema_version", 1, 1) != 1 || !string(d, 0, "id", s->id, sizeof(s->id)) || !pack_profile_id_valid(s->id) ||
        !string(d, 0, "version", s->version, sizeof(s->version)) || !version(s->version, ver) ||
        !string(d, 0, "min_firmware", minimum, sizeof(minimum)) || !version(minimum, ver) ||
        ver[0] > 1 || (ver[0] == 1 && (ver[1] > 5 || (ver[1] == 5 && ver[2] > 0))) ||
        !string(d, 0, "title", s->title, sizeof(s->title)) || !string(d, 0, "description", description, sizeof(description)) ||
        !string(d, 0, "entry_view", s->entry, sizeof(s->entry)) || !name_valid(s->entry, false) ||
        !(equals(d,0,"category","CONTROL") || equals(d,0,"category","COMPOSITE") || equals(d,0,"category","TELEMETRY") || equals(d,0,"category","LIVE_VIEW")) ||
        grants < 0 || !(grants & PACK_PERM_UI) || mandatory < 0 || (mandatory & ~grants) ||
        !KEYS(d,usb,"mode","requires") || !KEYS(d,host,"requires") ||
        !KEYS(d,limits,"max_events_per_sec","max_memory_kib") || rate < 1 ||
        number(d,limits,"max_memory_kib",16,512) < 0) return false;
    s->min_api_minor=ver[0]==1 ? ver[1] : 0;
    int hn = array_count(d, hc);
    unsigned host_caps = 0;
    if (hn < 0 || hn > 2) return false;
    for (int i = 0; i < hn; i++) {
        int t = kvm_json_element(d, hc, (uint16_t)i);
        unsigned bit;
        if (kvm_json_str_eq(d,t,"none") && hn == 1) break;
        if (kvm_json_str_eq(d,t,"telemetry.sim")) bit = PACK_PERM_TELEMETRY;
        else if (kvm_json_str_eq(d,t,"capture.windows")) bit = PACK_PERM_CAPTURE;
        else return false;
        if (host_caps & bit) return false;
        host_caps |= bit;
    }
    if (host_caps != ((unsigned)grants & (PACK_PERM_TELEMETRY | PACK_PERM_CAPTURE))) return false;
    s->permissions = (unsigned)grants;
    s->composite = equals(d,0,"category","COMPOSITE");
    s->required_permissions = (unsigned)mandatory;
    s->max_memory_kib = (unsigned)number(d, limits, "max_memory_kib", 16, 512);
    if (sizeof(*s) > (size_t)s->max_memory_kib * 1024u) return false;
    s->events_per_second = (unsigned)rate;
    return true;
}

static int widget_id(const pack_scene_t *s, const char *id)
{
    for (unsigned i=0; i<s->widgets; ++i) if (!strcmp(id,s->widget[i].id)) return (int)i;
    return -1;
}

static const char *const event_names[]={"tap","change","press","release","drag","rotate","long_press","step_up","step_down"};

static bool supports_event(pack_widget_kind_t k, unsigned e)
{
    switch (k) {
    case PACK_BUTTON: return e==PACK_TAP || e==PACK_PRESS || e==PACK_RELEASE;
    case PACK_AXIS: case PACK_SLIDER: case PACK_TABS: return e==PACK_CHANGE;
    case PACK_TOUCH: return e==PACK_TAP || e==PACK_DRAG || e==PACK_LONG_PRESS;
    case PACK_RATE_SLIDER: return e==PACK_STEP_UP || e==PACK_STEP_DOWN;
    case PACK_DIAL: return e==PACK_ROTATE;
    default: return false;
    }
}

static bool local_props(pack_scene_t *s, pack_widget_t *w, const doc_t *d, int p)
{
    if (p>=0 && kvm_json_type(d,p)!=KVM_JSON_OBJECT) return false;
    if (w->kind==PACK_RATE_SLIDER) return (p<0 || !kvm_json_count(d,p)) && w->w>=48 && w->h>=96;
    if (w->kind==PACK_TABS) {
        if (p>=0 && !KEYS(d,p,"tabs","pages","selected")) return false;
        int tabs=member(d,p,"tabs"), pages=member(d,p,"pages");
        int n=tabs>=0 ? array_count(d,tabs) : 2;
        if (n<2 || n>4 || w->w<48*n || w->h<42 ||
            (pages>=0 && array_count(d,pages)!=n)) return false;
        w->tab_first=s->tabs; w->tab_count=n;
        w->initial=member(d,p,"selected")>=0 ? number(d,p,"selected",0,n-1) : 0;
        if (w->initial<0) return false;
        char captions[4][25];
        for (int j=0;j<n;j++) {
            char *label=captions[j];
            if (tabs<0) snprintf(label,25,"Tab %d",j+1);
            else {
                size_t len=kvm_json_str_copy(d,kvm_json_element(d,tabs,j),label,25);
                if (!len || len==(size_t)-1 || len!=strlen(label)) return false;
            }
            for (int k=0;k<j;k++) if (!strcmp(label,captions[k])) return false;
        }
        // Identical caption-only strips share immutable labels. Each widget keeps its own selection.
        if (pages<0) for (unsigned i=0;i<s->widgets;i++) {
            const pack_widget_t *prior=&s->widget[i];
            if (prior->kind!=PACK_TABS || prior->tab_count!=n) continue;
            int j=0;
            for (;j<n;j++) {
                unsigned slot=prior->tab_first+j;
                if (s->tab[slot].page>=0 || strcmp(captions[j],s->tab[slot].label)) break;
            }
            if (j==n) { w->tab_first=prior->tab_first; return true; }
        }
        if (s->tabs+n>32) return false;
        for (int j=0;j<n;j++,s->tabs++) {
            strcpy(s->tab[s->tabs].label,captions[j]);
            s->tab[s->tabs].page=pages<0 ? -1 : kvm_json_element(d,pages,j);
        }
        return true;
    }
    if (w->kind==PACK_DIAL) {
        if ((p>=0 && !KEYS(d,p,"label","min","max","value")) || w->w<72 || w->h<80) return false;
        int64_t lo=0,hi=100,v;
        if ((member(d,p,"min")>=0 && !kvm_json_get_i64(d,member(d,p,"min"),&lo)) ||
            (member(d,p,"max")>=0 && !kvm_json_get_i64(d,member(d,p,"max"),&hi)) ||
            lo < -32768 || hi>32767 || lo>=hi) return false;
        v=lo;
        if ((member(d,p,"value")>=0 && !kvm_json_get_i64(d,member(d,p,"value"),&v)) || v<lo || v>hi) return false;
        w->min=(int)lo; w->max=(int)hi; w->initial=(int)v;
        strcpy(w->text,"Dial");
    } else if (w->kind==PACK_CHIP) {
        if (p>=0 && !KEYS(d,p,"label","text","status")) return false;
        const char *const styles[]={"neutral","capability","permission","info","success","warning","error"};
        if (member(d,p,"status")>=0) {
            for (;w->chip_style<7;w->chip_style++) if (equals(d,p,"status",styles[w->chip_style])) break;
            if (w->chip_style==7) return false;
        }
        strcpy(w->text,"Status");
    } else {
        if ((p>=0 && !KEYS(d,p,"label")) || w->w<48 || w->h<48) return false;
        strcpy(w->text,"Touch area");
    }
    if (member(d,p,"label")>=0 && !string(d,p,"label",w->text,sizeof(w->text))) return false;
    return member(d,p,"text")<0 || string(d,p,"text",w->text,sizeof(w->text));
}

static uint8_t asset_member(const doc_t *d, int props, const char *key, const pack_archive_t *archive)
{
    char path[PACK_NAME_MAX+1];
    if (!archive || !string(d,props,key,path,sizeof(path))) return 0;
    for (unsigned i=PACK_ASSETS;i<archive->end;i++)
        if (!strcmp(path,archive->member[i].name)) return (uint8_t)i;
    return 0;
}

static bool node(pack_scene_t *s, const doc_t *d, int obj, int parent, unsigned depth, const pack_archive_t *archive)
{
    if (depth > 8 || s->widgets >= PACK_WIDGETS || !KEYS(d,obj,"id","type","props","events","children","bounds")) return false;
    unsigned idx = s->widgets;
    pack_widget_t *w = &s->widget[idx];
    if (!string(d,obj,"id",w->id,sizeof(w->id)) || !name_valid(w->id,false) || widget_id(s,w->id) >= 0) return false;
    if (equals(d,obj,"type","Column") || equals(d,obj,"type","Row") || equals(d,obj,"type","Grid")) w->kind=PACK_CONTAINER;
    else if (equals(d,obj,"type","Text")) w->kind=PACK_TEXT;
    else if (equals(d,obj,"type","Button")) w->kind=PACK_BUTTON;
    else if (equals(d,obj,"type","Spacer")) w->kind=PACK_SPACER;
    else if (equals(d,obj,"type","AxisPad")) w->kind=PACK_AXIS;
    else if (equals(d,obj,"type","Slider")) w->kind=PACK_SLIDER;
    else if (equals(d,obj,"type","GaugeArc")) w->kind=PACK_GAUGE_ARC;
    else if (equals(d,obj,"type","GaugeNeedle")) w->kind=PACK_GAUGE_NEEDLE;
    else if (equals(d,obj,"type","Horizon")) w->kind=PACK_HORIZON;
    else if (equals(d,obj,"type","ValueTile")) w->kind=PACK_VALUE;
    else if (equals(d,obj,"type","StreamViewport")) w->kind=PACK_STREAM;
    else if (equals(d,obj,"type","TouchArea")) w->kind=PACK_TOUCH;
    else if (equals(d,obj,"type","RateSlider")) w->kind=PACK_RATE_SLIDER;
    else if (equals(d,obj,"type","Dial")) w->kind=PACK_DIAL;
    else if (equals(d,obj,"type","TabStrip")) w->kind=PACK_TABS;
    else if (equals(d,obj,"type","StatusChip")) w->kind=PACK_CHIP;
    else if (equals(d,obj,"type","ChartViewer")) w->kind=PACK_CHART;
    else if (equals(d,obj,"type","AutopilotPanel")) w->kind=PACK_AUTOPILOT;
    else if (equals(d,obj,"type","RadioPanel")) w->kind=PACK_RADIO;
    else if (equals(d,obj,"type","MonitorViewport")) w->kind=PACK_MONITOR;
    else if (equals(d,obj,"type","KeyboardSurface")) w->kind=PACK_KEYBOARD;
    else if (equals(d,obj,"type","TouchpadSurface")) w->kind=PACK_TOUCHPAD;
    else return false;
    w->spring_center=w->kind==PACK_AXIS;
    int b=member(d,obj,"bounds");
    int width=parent < 0 ? (s->portrait ? 600 : 1024) : s->widget[parent].w;
    int height=parent < 0 ? (s->portrait ? 992 : 568) : s->widget[parent].h;
    if (parent < 0 && s->screen_count) height -= 48;
    w->parent=parent;
    w->x=number(d,b,"x",0,width-1); w->y=number(d,b,"y",0,height-1);
    w->w=number(d,b,"w",1,width); w->h=number(d,b,"h",1,height);
    if (!KEYS(d,b,"x","y","w","h") || w->x<0 || w->y<0 || w->w<1 || w->h<1 ||
        w->x+w->w>width || w->y+w->h>height) return false;
    int p=member(d,obj,"props"), e=member(d,obj,"events"), children=member(d,obj,"children");
    if (parent == 0 && s->screen_count) {
        char title[49];
        if (w->kind != PACK_CONTAINER || member(d,p,"label") >= 0 || member(d,p,"text") >= 0 ||
            !string(d,p,"title",title,sizeof(title))) return false;
    }
    if ((w->kind == PACK_AXIS || w->kind == PACK_SLIDER) && !(s->permissions & PACK_PERM_JOYSTICK)) return false;
    if (w->kind == PACK_STREAM && !(s->permissions & PACK_PERM_CAPTURE)) return false;
    if (w->kind>=PACK_MONITOR) {
        const int widths[]={160,440,132}, heights[]={120,176,88};
        const unsigned bits[]={PACK_PERM_MONITOR,PACK_PERM_KEYBOARD,PACK_PERM_MOUSE};
        unsigned k=(unsigned)(w->kind-PACK_MONITOR);
        if (!(s->required_permissions & bits[k]) || w->w<widths[k] || w->h<heights[k] ||
            (p>=0 && (kvm_json_type(d,p)!=KVM_JSON_OBJECT || kvm_json_count(d,p)))) return false;
        int page=parent;
        while (page>0 && s->widget[page].parent>0) page=s->widget[page].parent;
        for (unsigned i=0;i<idx;i++) {
            if (s->widget[i].kind!=w->kind) continue;
            int other=s->widget[i].parent;
            while (other>0 && s->widget[other].parent>0) other=s->widget[other].parent;
            if (!s->screen_count || other==page) return false;
        }
    } else if (w->kind==PACK_AUTOPILOT || w->kind==PACK_RADIO) {
        if ((p>=0 && (kvm_json_type(d,p)!=KVM_JSON_OBJECT || kvm_json_count(d,p))) || w->w<540 || w->h<680) return false;
        for (unsigned i=0;i<idx;i++) if (s->widget[i].kind==w->kind) return false;
    } else if (w->kind==PACK_CHART) {
        int charts=member(d,p,"charts");
        int count=array_count(d,charts);
        if (!KEYS(d,p,"charts") || count<2 || count>PACK_CHARTS || s->charts[0].member ||
            w->w<540 || w->h<420) return false;
        w->tab_count=(uint8_t)count;
        for (int i=0;i<count;i++) {
            int row=kvm_json_element(d,charts,i);
            pack_chart_t *chart=&s->charts[i];
            if (!KEYS(d,row,"asset","title") || !string(d,row,"title",chart->title,sizeof(chart->title))) return false;
            chart->member=asset_member(d,row,"asset",archive);
            if (!chart->member) return false;
            for (int j=0;j<i;j++)
                if (chart->member==s->charts[j].member || !strcmp(chart->title,s->charts[j].title)) return false;
        }
    } else if (w->kind>=PACK_TOUCH) {
        if (!local_props(s,w,d,p)) return false;
    } else if (p>=0) {
        if (!KEYS(d,p,"layout","text","label","title","axes","spring_center","min","max","orientation",
                  "status","unit","stream","fit","source_size","mode","invert_y","dead_zone_percent","curve","icon")) return false;
        if (member(d,p,"icon")>=0) {
            if (w->kind!=PACK_BUTTON) return false;
            w->icon_member=asset_member(d,p,"icon",archive);
            if (!w->icon_member) return false;
        }
        if (member(d,p,"invert_y")>=0 &&
            (w->kind!=PACK_AXIS || !kvm_json_get_bool(d,member(d,p,"invert_y"),&w->invert_y))) return false;
        if (member(d,p,"dead_zone_percent")>=0) {
            int zone=number(d,p,"dead_zone_percent",0,99);
            if (w->kind!=PACK_AXIS || zone<0) return false;
            w->dead_zone_percent=(uint8_t)zone;
        }
        if (member(d,p,"curve")>=0) {
            w->cubic=equals(d,p,"curve","cubic");
            if (w->kind!=PACK_AXIS || (!w->cubic && !equals(d,p,"curve","linear"))) return false;
        }
        if (member(d,p,"axes")>=0) {
            int axes = member(d,p,"axes");
            if (w->kind != PACK_AXIS || array_count(d,axes)!=2 || !kvm_json_str_eq(d,kvm_json_element(d,axes,0),"X") ||
                !kvm_json_str_eq(d,kvm_json_element(d,axes,1),"Y")) return false;
        }
        if (member(d,p,"spring_center")>=0 && !kvm_json_get_bool(d,member(d,p,"spring_center"),&w->spring_center)) return false;
        if (member(d,p,"orientation")>=0 && !equals(d,p,"orientation","horizontal")) return false;
        if (member(d,p,"unit")>=0 && !string(d,p,"unit",w->unit,sizeof(w->unit))) return false;
        if (member(d,p,"min")>=0 || member(d,p,"max")>=0) {
            int64_t low, high;
            if (!kvm_json_get_i64(d,member(d,p,"min"),&low) || !kvm_json_get_i64(d,member(d,p,"max"),&high) ||
                low < -100000 || high > 100000 || low >= high) return false;
            w->min = (int32_t)low; w->max = (int32_t)high;
        }
        if (w->kind == PACK_STREAM) {
            int a = member(d,p,"source_size"); int64_t x, y;
            if (!equals(d,p,"stream","cursor_region") || !equals(d,p,"fit","native") || !equals(d,p,"mode","PIXEL_1X") ||
                array_count(d,a)!=2 || !kvm_json_get_i64(d,kvm_json_element(d,a,0),&x) ||
                !kvm_json_get_i64(d,kvm_json_element(d,a,1),&y) || x!=512 || y!=512) return false;
        }
        if (member(d,p,"layout")>=0 && !equals(d,p,"layout","absolute")) return false;
        const char *key = member(d,p,"text")>=0 ? "text" : member(d,p,"label")>=0 ? "label" : "title";
        if (member(d,p,key)>=0 && !string(d,p,key,w->text,sizeof(w->text))) return false;
    }
    if (e >= 0) {
        if (!KEYS(d,e,"tap","change","press","release","drag","rotate","long_press","step_up","step_down")) return false;
        for (unsigned ev=0; ev<sizeof(event_names)/sizeof(event_names[0]); ev++) {
            if (member(d,e,event_names[ev]) < 0) continue;
            if (s->actions >= PACK_ACTIONS || !supports_event(w->kind,ev)) return false;
            pack_action_t *a = &s->action[s->actions];
            if (!string(d,e,event_names[ev],a->id,sizeof(a->id)) || !name_valid(a->id,true)) return false;
            for (unsigned j=0;j<s->actions;j++) if (!strcmp(s->action[j].id,a->id)) return false;
            a->widget = (uint16_t)idx; a->event = (pack_event_t)ev; s->actions++;
        }
    }
    ++s->widgets;
    int count=children<0 ? 0 : array_count(d,children);
    if (count<0 || (count && w->kind!=PACK_CONTAINER)) return false;
    for (int i=0; i<count; ++i)
        if (!node(s,d,kvm_json_element(d,children,i),(int)idx,depth+1,archive)) return false;
    return true;
}

bool pack_scene_ui(pack_scene_t *s, const doc_t *d)
{
    return pack_scene_ui_assets(s,d,NULL);
}

bool pack_scene_ui_assets(pack_scene_t *s, const doc_t *d, const pack_archive_t *archive)
{
    s->portrait=equals(d,0,"orientation","portrait");
    int screens=member(d,0,"screens");
    int count=screens < 0 ? 0 : array_count(d,screens);
    if (screens >= 0 && (count < 2 || count > 10)) return false;
    s->screen_count=(uint8_t)count;
    if ((member(d,0,"orientation")>=0 && !s->portrait && !equals(d,0,"orientation","landscape")) ||
        !KEYS(d,0,"version","root","orientation","screens") || number(d,0,"version",1,1)!=1 ||
        !node(s,d,member(d,0,"root"),-1,1,archive) || s->widget[0].kind!=PACK_CONTAINER || strcmp(s->widget[0].id,s->entry)) return false;
    bool owned[PACK_WIDGETS]={false};
    if (s->permissions & PACK_PERM_MONITOR) {
        bool viewport=false;
        for (unsigned i=0;i<s->widgets;i++) if (s->widget[i].kind==PACK_MONITOR) viewport=true;
        if (!viewport) return false;
    }
    if (count) {
        unsigned children=0;
        for (unsigned i=1;i<s->widgets;i++) if (s->widget[i].parent==0) children++;
        if (children!=(unsigned)count) return false;
        for (int i=0;i<count;i++) {
            char name[49];
            size_t n=kvm_json_str_copy(d,kvm_json_element(d,screens,i),name,sizeof(name));
            if (!n || n==(size_t)-1 || n!=strlen(name)) return false;
            int page=widget_id(s,name);
            if (page<0 || s->widget[page].parent!=0 || owned[page]) return false;
            owned[page]=true;
            s->widget[page].initial=i;
        }
    }
    for (unsigned i=0;i<s->widgets;i++) {
        pack_widget_t *w=&s->widget[i];
        if (w->kind!=PACK_TABS) continue;
        for (unsigned j=w->tab_first;j<w->tab_first+w->tab_count;j++) {
            if (s->tab[j].page<0) continue;
            char id[49];
            size_t len=kvm_json_str_copy(d,s->tab[j].page,id,sizeof(id));
            if (!len || len==(size_t)-1 || len!=strlen(id)) return false;
            int page=widget_id(s,id);
            if (page<0 || owned[page] || s->widget[page].kind!=PACK_CONTAINER || s->widget[page].parent!=w->parent) return false;
            s->tab[j].page=page; owned[page]=true;
        }
    }
    return true;
}

static int state_id(const pack_scene_t *s, const char *key)
{
    for (unsigned i=0; i<s->states; ++i) if (!strcmp(key,s->state[i].id)) return i;
    return -1;
}

static int topic(const doc_t *d, int tok)
{
    static const char *const topics[] = {"sim.airspeed_kt","sim.altitude_ft","sim.heading_deg",
                                        "sim.vertical_speed_fpm","sim.attitude.pitch_deg","sim.attitude.roll_deg"};
    for (int i=0; i<6; i++) if (kvm_json_str_eq(d,tok,topics[i])) return i;
    return -1;
}

static int source_id(const pack_scene_t *s, const char *id)
{
    for (unsigned i=0; i<s->sources; ++i) if (!strcmp(id,s->source[i].id)) return (int)i;
    return -1;
}

static bool finite_number(const doc_t *d, int t, double *out)
{
    if (kvm_json_type(d,t)!=KVM_JSON_NUMBER) return false;
    const kvm_json_tok_t *token=&kvm_json_tokens(d)[t];
    char short_token[64];
    size_t bytes=token->len;
    char *text=bytes<sizeof(short_token) ? short_token : malloc(bytes+1);
    if (!text) return false;
    memcpy(text,d->text+token->start,bytes);
    text[bytes]=0;
    char *end;
    *out=strtod(text,&end);
    bool ok=end==text+bytes && isfinite(*out);
    if (text!=short_token) free(text);
    return ok;
}

static bool integer_bound(const doc_t *d, int token, bool lower, int64_t *out)
{
    *out=lower ? INT64_MIN : INT64_MAX;
    if (token<0) return true;
    if (kvm_json_type(d,token)!=KVM_JSON_NUMBER) return false;
    const kvm_json_tok_t *t=&kvm_json_tokens(d)[token];
    if (t->flags & KVM_JSON_FLAG_INT) {
        if (kvm_json_get_i64(d,token,out)) return true;
        // An integer beyond the representable domain can only be an outer bound.
        return (d->text[t->start]=='-')==lower;
    }
    double n;
    if (!finite_number(d,token,&n)) return false;
    if (n>=0x1p63) return !lower;
    if (n< -0x1p63) return lower;
    *out=(int64_t)(lower ? ceil(n) : floor(n));
    return true;
}

static bool state_value(const doc_t *d, int t, const pack_state_t *s, pack_value_t *value)
{
    switch (s->kind) {
    case PACK_STRING: return kvm_json_str_copy(d,t,value->text,sizeof(value->text))!=(size_t)-1;
    case PACK_BOOLEAN: return kvm_json_get_bool(d,t,&value->boolean);
    case PACK_INTEGER:
        return kvm_json_get_i64(d,t,&value->integer) &&
               value->integer>=s->min.integer && value->integer<=s->max.integer;
    case PACK_FLOAT:
        return finite_number(d,t,&value->real) && value->real>=s->min.real && value->real<=s->max.real;
    }
    return false;
}

static bool state_definition(const doc_t *d, int o, pack_state_t *s)
{
    int min=member(d,o,"min"), max=member(d,o,"max");
    if (equals(d,o,"type","int")) {
        s->kind=PACK_INTEGER;
        if (!integer_bound(d,min,true,&s->min.integer) || !integer_bound(d,max,false,&s->max.integer)) return false;
    } else if (equals(d,o,"type","float")) {
        s->kind=PACK_FLOAT;
        s->min.real=-DBL_MAX; s->max.real=DBL_MAX;
        if ((min>=0 && !finite_number(d,min,&s->min.real)) || (max>=0 && !finite_number(d,max,&s->max.real)))
            return false;
    } else {
        if (min>=0 || max>=0) return false;
        if (equals(d,o,"type","bool")) s->kind=PACK_BOOLEAN;
        else if (equals(d,o,"type","string")) s->kind=PACK_STRING;
        else return false;
    }
    return state_value(d,member(d,o,"initial"),s,&s->value);
}

const char *pack_value_text(pack_state_kind_t kind, const pack_value_t *value, char scratch[32])
{
    switch (kind) {
    case PACK_STRING: return value->text;
    case PACK_BOOLEAN: return value->boolean ? "true" : "false";
    case PACK_INTEGER: snprintf(scratch,32,"%" PRId64,value->integer); break;
    case PACK_FLOAT: snprintf(scratch,32,"%.15g",value->real==0 ? 0.0 : value->real); break;
    }
    return scratch;
}

const char *pack_state_text(const pack_state_t *s, char scratch[32])
{
    return pack_value_text(s->kind,&s->value,scratch);
}

static bool parameter(const doc_t *d, int object, const char *key, double fallback, double *out)
{
    int token=member(d,object,key);
    *out=fallback;
    return token<0 || finite_number(d,token,out);
}

static bool compile_transform(pack_scene_t *s, const doc_t *d, int o, pack_binding_t *b)
{
    static const char *const names[]={"identity","clamp","scale","offset","round","enum_map","hysteresis",
                                      "lowpass","format_number","boolean_threshold"};
    unsigned i=0;
    while (i<sizeof(names)/sizeof(*names) && !equals(d,o,"transform",names[i])) i++;
    if (i==sizeof(names)/sizeof(*names)) return false;
    b->transform=(pack_transform_t)i;
    int p=member(d,o,"params");
    if (p>=0 && kvm_json_type(d,p)!=KVM_JSON_OBJECT) return false;
    b->output=b->input;
    if (b->transform==PACK_IDENTITY) return p<0 || !kvm_json_count(d,p);
    if (b->input!=PACK_INTEGER && b->input!=PACK_FLOAT) return false;
    b->output=PACK_FLOAT;
    switch (b->transform) {
    case PACK_ROUND: return p<0 || !kvm_json_count(d,p);
    case PACK_CLAMP:
        return (p<0 || KEYS(d,p,"min","max")) && parameter(d,p,"min",0,&b->a) &&
               parameter(d,p,"max",1,&b->b) && b->a<=b->b;
    case PACK_SCALE: return (p<0 || KEYS(d,p,"factor")) && parameter(d,p,"factor",1,&b->a);
    case PACK_OFFSET: return (p<0 || KEYS(d,p,"amount")) && parameter(d,p,"amount",0,&b->a);
    case PACK_BOOLEAN_THRESHOLD:
        b->output=PACK_BOOLEAN;
        return (p<0 || KEYS(d,p,"threshold")) && parameter(d,p,"threshold",0.5,&b->a);
    case PACK_HYSTERESIS:
        b->output=PACK_BOOLEAN;
        return (p<0 || KEYS(d,p,"low","high")) && parameter(d,p,"low",0.25,&b->a) &&
               parameter(d,p,"high",0.75,&b->b) && b->a<b->b;
    case PACK_LOWPASS:
        if (p>=0 && !KEYS(d,p,"tau_ms")) return false;
        b->a=member(d,p,"tau_ms")<0 ? 250 : number(d,p,"tau_ms",1,60000);
        return b->a>=1;
    case PACK_FORMAT_NUMBER:
        b->output=PACK_STRING;
        if (p>=0 && !KEYS(d,p,"digits")) return false;
        b->a=member(d,p,"digits")<0 ? 2 : number(d,p,"digits",0,6);
        return b->a>=0;
    case PACK_ENUM_MAP: {
        b->output=PACK_STRING;
        if (p>=0 && !KEYS(d,p,"entries")) return false;
        int arr=member(d,p,"entries");
        if (arr<0) return true;
        int n=array_count(d,arr);
        if (n<1 || n>8 || s->enum_entries+(unsigned)n>PACK_ENUM_ENTRIES) return false;
        b->enum_start=(uint8_t)s->enum_entries;
        b->enum_count=(uint8_t)n;
        for (int j=0; j<n; j++) {
            int e=kvm_json_element(d,arr,(uint16_t)j);
            pack_enum_entry_t *entry=&s->enum_table[s->enum_entries];
            int64_t code;
            if (!KEYS(d,e,"code","label") || !kvm_json_get_i64(d,member(d,e,"code"),&code) ||
                code<INT32_MIN || code>INT32_MAX || !string(d,e,"label",entry->label,sizeof(entry->label))) return false;
            for (unsigned k=b->enum_start; k<s->enum_entries; k++)
                if (s->enum_table[k].code==code) return false;
            entry->code=(int32_t)code;
            s->enum_entries++;
        }
        return true;
    }
    default: return false;
    }
}

bool pack_binding_apply(pack_scene_t *s, pack_binding_t *b, const pack_value_t *in, uint32_t tick, pack_value_t *out)
{
    if (!in || (b->input==PACK_FLOAT && !isfinite(in->real))) { b->seeded=false; return false; }
    if (b->transform==PACK_IDENTITY) { *out=*in; return true; }
    double x=b->input==PACK_INTEGER ? (double)in->integer : in->real;
    switch (b->transform) {
    case PACK_CLAMP: out->real=fmin(b->b,fmax(b->a,x)); break;
    case PACK_SCALE: out->real=x*b->a; break;
    case PACK_OFFSET: out->real=x+b->a; break;
    case PACK_ROUND: out->real=round(x); break;
    case PACK_BOOLEAN_THRESHOLD: out->boolean=x>=b->a; return true;
    case PACK_HYSTERESIS:
        out->boolean=x>=b->b || (x>b->a && b->seeded && b->previous!=0);
        b->previous=out->boolean; b->seeded=true;
        return true;
    case PACK_LOWPASS: {
        if (!b->seeded) b->previous=x;
        else if (x!=b->previous) {
            double weight=-expm1(-(double)(uint32_t)(tick-b->tick)/b->a);
            // Convex form also works when endpoints have opposite signs near DBL_MAX.
            b->previous=(1-weight)*b->previous + weight*x;
        }
        b->tick=tick; b->seeded=true; out->real=b->previous;
        break;
    }
    case PACK_ENUM_MAP: {
        const char *label=NULL;
        if (!b->enum_count) label=x==0 ? "Off" : x==1 ? "On" : NULL;
        for (unsigned i=0; i<b->enum_count; i++) {
            const pack_enum_entry_t *e=&s->enum_table[b->enum_start+i];
            if (x==e->code) { label=e->label; break; }
        }
        if (!label) return false;
        strcpy(out->text,label);
        return true;
    }
    case PACK_FORMAT_NUMBER: {
        static const unsigned powers[]={1,10,100,1000,10000,100000,1000000};
        unsigned digits=(unsigned)b->a, factor=powers[digits];
        double scaled=round(fabs(x)*factor);
        if (scaled>9007199254740991.0) return false;
        uint64_t magnitude=(uint64_t)scaled;
        const char *sign=x<0 && magnitude ? "-" : "";
        if (digits) snprintf(out->text,sizeof(out->text),"%s%" PRIu64 ".%0*u",sign,magnitude/factor,
                             (int)digits,(unsigned)(magnitude%factor));
        else snprintf(out->text,sizeof(out->text),"%s%" PRIu64,sign,magnitude);
        return true;
    }
    default: return false;
    }
    if (!isfinite(out->real)) { b->seeded=false; return false; }
    return true;
}

static bool compile_connection(pack_scene_t *s, const doc_t *d, int o)
{
    char from[97], to[97];
    if (!KEYS(d,o,"from","to","transform","params") ||
        !string(d,o,"from",from,sizeof(from)) || !string(d,o,"to",to,sizeof(to))) return false;
    char *dot=strchr(to,'.');
    if (!dot) return false;
    *dot++=0;
    int wi=widget_id(s,to);
    if (wi<0) return false;
    pack_widget_kind_t k=s->widget[wi].kind;
    pack_field_t field;
    if (!strcmp(dot,"text") && (k==PACK_TEXT || k==PACK_CHIP)) field=PACK_BIND_TEXT;
    else if (!strcmp(dot,"value") && (k==PACK_GAUGE_ARC || k==PACK_GAUGE_NEEDLE || k==PACK_VALUE)) field=PACK_BIND_VALUE;
    else if (!strcmp(dot,"pitch") && k==PACK_HORIZON) field=PACK_BIND_PITCH;
    else if (!strcmp(dot,"roll") && k==PACK_HORIZON) field=PACK_BIND_ROLL;
    else if (!strcmp(dot,"selected") && (k==PACK_TABS || (k==PACK_RADIO && s->min_api_minor>=4) ||
                                        (k==PACK_AUTOPILOT && s->min_api_minor>=5))) field=PACK_BIND_SELECTED;
    else return false;
    int si=state_id(s,from);
    bool local=si>=0;
    if (!local) si=source_id(s,from);
    if (si<0) return false;
    for (unsigned j=0; j<s->bindings; j++)
        if (s->binding[j].widget==wi && s->binding[j].field==field) return false;
    pack_binding_t *b=&s->binding[s->bindings];
    *b=(pack_binding_t){.source=(uint16_t)si,.widget=(uint16_t)wi,.field=field,.local=local,
                        .input=local ? s->state[si].kind : PACK_FLOAT};
    if (!compile_transform(s,d,o,b) || (field!=PACK_BIND_TEXT && b->output!=PACK_INTEGER && b->output!=PACK_FLOAT))
        return false;
    if (field==PACK_BIND_SELECTED) {
        unsigned options=k==PACK_TABS ? s->widget[wi].tab_count : 2u;
        if (!local || b->input!=PACK_INTEGER || b->transform!=PACK_IDENTITY ||
            s->state[si].min.integer<0 || s->state[si].max.integer>=(int32_t)options) return false;
        // A bound panel gets its selector from the package.
        if (k!=PACK_TABS) s->widget[wi].package_selection=true;
    }
    s->bindings++;
    return true;
}

static bool acyclic_bindings(const pack_scene_t *s)
{
    static const char *const fields[]={"text","value","pitch","roll","selected"};
    int predecessor[PACK_BINDS];
    for (unsigned i=0;i<s->bindings;i++) predecessor[i]=-1;
    for (unsigned i=0;i<s->bindings;i++) {
        const pack_binding_t *b=&s->binding[i];
        char endpoint[64];
        snprintf(endpoint,sizeof(endpoint),"%s.%s",s->widget[b->widget].id,fields[b->field]);
        for (unsigned j=0;j<s->bindings;j++) {
            const pack_binding_t *next=&s->binding[j];
            const char *from=next->local ? s->state[next->source].id : s->source[next->source].id;
            if (!strcmp(from,endpoint)) predecessor[j]=(int)i;
        }
    }
    // Each distinct widget field has one writer, so every chain has at most one predecessor.
    for (unsigned i=0;i<s->bindings;i++) {
        int at=(int)i;
        for (unsigned n=0;at>=0 && n<s->bindings;n++) at=predecessor[at];
        if (at>=0) return false;
    }
    return true;
}

static bool joystick_action(pack_scene_t *s, const doc_t *d, int args, pack_action_t *a)
{
    if (!(s->permissions & PACK_PERM_JOYSTICK) || !KEYS(d,args,"axes","buttons","hat") || kvm_json_count(d,args)!=1) return false;
    int axes = member(d,args,"axes"), btn = member(d,args,"buttons");
    pack_widget_t *w = &s->widget[a->widget];
    if (axes >= 0 && a->event == PACK_CHANGE) {
        if (w->kind == PACK_AXIS && KEYS(d,axes,"X","Y") && equals(d,axes,"X","$event.x") && equals(d,axes,"Y","$event.y")) {
            a->mask = PACK_X | PACK_Y;
        } else if (w->kind == PACK_SLIDER && w->min==0 && w->max==1 && KEYS(d,axes,"Z") && equals(d,axes,"Z","$event.value")) {
            a->mask = PACK_Z;
        } else if (w->kind == PACK_SLIDER && w->min==-1 && w->max==1 && KEYS(d,axes,"Rz") && equals(d,axes,"Rz","$event.value")) {
            a->mask = PACK_RZ;
        } else return false;
        a->kind = PACK_AXES;
        return true;
    }
    if (w->kind!=PACK_BUTTON || (a->event!=PACK_PRESS && a->event!=PACK_RELEASE)) return false;
    if (member(d,args,"hat")>=0) {
        int pos=number(d,args,"hat",0,8);
        if (pos<0 || (a->event==PACK_RELEASE)!=(pos==8)) return false;
        a->kind=PACK_HAT; a->value.integer=pos;
        return true;
    }
    if (kvm_json_type(d,btn)!=KVM_JSON_OBJECT || kvm_json_count(d,btn)!=1) return false;
    char key[4];
    for (unsigned i=0; i<16; i++) {
        snprintf(key,sizeof(key),"%u",i+1);
        if (member(d,btn,key)<0) continue;
        if (!kvm_json_get_bool(d,member(d,btn,key),&a->down) || a->down!=(a->event==PACK_PRESS)) return false;
        a->mask=(uint16_t)(1u<<i); a->kind=PACK_BUTTON_BIT;
        return true;
    }
    return false;
}

static const char *capture_mode(const char *text)
{
    static const char *const modes[]={"OVERVIEW","PIXEL_1X","ZOOM_2X"};
    for (unsigned i=0;i<sizeof(modes)/sizeof(modes[0]);i++)
        if (!strcmp(text,modes[i])) return modes[i];
    return NULL;
}

const char *pack_scene_capture_mode(const pack_scene_t *s, const pack_action_t *a)
{
    if (!s || !a || a->kind!=PACK_START_STREAM) return NULL;
    if (a->argument==PACK_LITERAL) return capture_mode(a->value.text);
    if (a->argument!=PACK_MODE_STATE || a->state>=s->states || s->state[a->state].kind!=PACK_STRING) return NULL;
    return capture_mode(s->state[a->state].value.text);
}

static bool compile_action(pack_scene_t *s, const doc_t *d, int o)
{
    char trigger[97], id[97];
    if (!KEYS(d,o,"id","trigger","type","capability","args") || !string(d,o,"id",id,sizeof(id)) ||
        !string(d,o,"trigger",trigger,sizeof(trigger))) return false;
    int args=member(d,o,"args");
    for (unsigned j=0; j<s->actions; j++) {
        pack_action_t *a=&s->action[j];
        if (strcmp(a->id,id)) continue;
        char expected[64];
        snprintf(expected,sizeof(expected),"%s.%s",s->widget[a->widget].id,event_names[a->event]);
        if (a->defined || strcmp(expected,trigger)) return false;
        a->defined=true;
        const pack_widget_t *w=&s->widget[a->widget];
        if (w->kind<PACK_TOUCH && equals(d,o,"type","hid.joystick.set_axes") && equals(d,o,"capability","hid.joystick"))
            return joystick_action(s,d,args,a);
        if (a->event!=PACK_TAP && w->kind<PACK_TOUCH) return false;
        if ((w->kind==PACK_RATE_SLIDER || (s->min_api_minor>=4 && w->kind==PACK_BUTTON && a->event==PACK_TAP)) &&
            equals(d,o,"type","state.adjust") && equals(d,o,"capability","ui.basic")) {
            char name[97];
            int64_t delta;
            if (!KEYS(d,args,"name","delta") || !string(d,args,"name",name,sizeof(name)) ||
                !kvm_json_get_i64(d,member(d,args,"delta"),&delta) || !delta ||
                delta<INT32_MIN || delta>INT32_MAX) return false;
            int state=state_id(s,name);
            if (state<0 || s->state[state].kind!=PACK_INTEGER) return false;
            a->state=(uint16_t)state; a->kind=PACK_STATE; a->argument=PACK_ADJUST; a->value.integer=delta;
            return true;
        }
        if (equals(d,o,"type","state.set") && equals(d,o,"capability","ui.basic") && KEYS(d,args,"name","value")) {
            char name[97];
            if (!string(d,args,"name",name,sizeof(name))) return false;
            int state=state_id(s,name);
            if (state<0) return false;
            int v=member(d,args,"value");
            if (kvm_json_type(d,v)==KVM_JSON_OBJECT) {
                int lo=0, hi;
                if (s->state[state].kind!=PACK_INTEGER || !KEYS(d,v,"event")) return false;
                if (equals(d,v,"event","value") && (w->kind==PACK_DIAL || w->kind==PACK_TABS)) {
                    a->argument=PACK_EVENT_VALUE; lo=w->kind==PACK_DIAL?w->min:0; hi=w->kind==PACK_DIAL?w->max:w->tab_count-1;
                } else if (w->kind==PACK_TOUCH && a->event==PACK_DRAG && equals(d,v,"event","x")) {
                    a->argument=PACK_EVENT_X; hi=w->w-1;
                } else if (w->kind==PACK_TOUCH && a->event==PACK_DRAG && equals(d,v,"event","y")) {
                    a->argument=PACK_EVENT_Y; hi=w->h-1;
                } else return false;
                if (lo<s->state[state].min.integer || hi>s->state[state].max.integer) return false;
            } else if (!state_value(d,v,&s->state[state],&a->value)) return false;
            a->state=(uint16_t)state; a->kind=PACK_STATE;
            return true;
        }
        if (w->kind>=PACK_TOUCH || !equals(d,o,"capability","host.screen.region") || !(s->permissions & PACK_PERM_CAPTURE) ||
            !equals(d,args,"stream","cursor_region")) return false;
        if (equals(d,o,"type","stream.stop") && KEYS(d,args,"stream")) {
            a->kind=PACK_STOP_STREAM;
            return true;
        }
        bool consent=false;
        char requested[sizeof(s->state[0].id)+7];
        if (!equals(d,o,"type","stream.start") || !KEYS(d,args,"stream","mode","requires_foreground_consent") ||
            !string(d,args,"mode",requested,sizeof(requested)) ||
            !kvm_json_get_bool(d,member(d,args,"requires_foreground_consent"),&consent) || !consent) return false;
        if (!strncmp(requested,"$state.",7)) {
            int state=state_id(s,requested+7);
            if (state<0 || s->state[state].kind!=PACK_STRING) return false;
            a->argument=PACK_MODE_STATE;
            a->state=(uint16_t)state;
        } else {
            const char *mode=capture_mode(requested);
            if (!mode) return false;
            strcpy(a->value.text,mode);
        }
        a->kind=PACK_START_STREAM;
        return true;
    }
    return false;
}

bool pack_scene_bindings(pack_scene_t *s, const doc_t *d)
{
    int states=member(d,0,"state"), actions=member(d,0,"actions"), binds=member(d,0,"connections"), src=member(d,0,"sources");
    int ns=array_count(d,states), na=array_count(d,actions), nb=array_count(d,binds), nt=array_count(d,src);
    if (!KEYS(d,0,"version","sources","state","connections","actions") || number(d,0,"version",1,1)!=1 ||
        nt<0 || nt>PACK_SOURCES || ns<0 || ns>PACK_STATES || na!=(int)s->actions || nb<0 || nb>PACK_BINDS ||
        (nt && !(s->permissions & PACK_PERM_TELEMETRY))) return false;
    for (int i=0; i<nt; i++) {
        int o=kvm_json_element(d,src,(uint16_t)i);
        pack_source_t *v=&s->source[i];
        int ch=topic(d,member(d,o,"topic")), age=number(d,o,"max_age_ms",1,60000);
        if (!KEYS(d,o,"id","topic","type","max_age_ms") || !equals(d,o,"type","float") || ch<0 || age<1 ||
            !string(d,o,"id",v->id,sizeof(v->id)) || !name_valid(v->id,true) || source_id(s,v->id)>=0) return false;
        v->max_age_ms=(unsigned)age; v->channel=(unsigned)ch; s->sources++;
    }
    for (int i=0; i<ns; ++i) {
        int o=kvm_json_element(d,states,(uint16_t)i);
        pack_state_t *v=&s->state[i];
        if (!KEYS(d,o,"id","type","initial","min","max") ||
            !string(d,o,"id",v->id,sizeof(v->id)) || !name_valid(v->id,true) || state_id(s,v->id)>=0 ||
            source_id(s,v->id)>=0 || !state_definition(d,o,v)) return false;
        ++s->states;
    }
    for (int i=0; i<nb; ++i) if (!compile_connection(s,d,kvm_json_element(d,binds,(uint16_t)i))) return false;
    if (!acyclic_bindings(s)) return false;
    for (int i=0; i<na; ++i) if (!compile_action(s,d,kvm_json_element(d,actions,(uint16_t)i))) return false;
    for (unsigned i=0; i<s->actions; i++) {
        const pack_action_t *a=&s->action[i];
        if (a->kind!=PACK_BUTTON_BIT && a->kind!=PACK_HAT) continue;
        const pack_action_t *pair=pack_scene_action(s,a->widget,a->event==PACK_PRESS ? PACK_RELEASE : PACK_PRESS);
        if (!pair || pair->kind!=a->kind || (a->kind==PACK_BUTTON_BIT && pair->mask!=a->mask)) return false;
    }
    return true;
}

const pack_action_t *pack_scene_action(const pack_scene_t *s, unsigned widget, pack_event_t event)
{
    for (unsigned i=0; i<s->actions; ++i) {
        const pack_action_t *a=&s->action[i];
        if (a->defined && a->widget==widget && a->event==event) return a;
    }
    return NULL;
}

bool pack_scene_set(pack_scene_t *s, const pack_action_t *a, int value, int x, int y)
{
    if (!a || a->kind!=PACK_STATE) return false;
    pack_state_t *state=&s->state[a->state];
    if (a->argument==PACK_ADJUST) {
        int64_t current=state->value.integer, delta=a->value.integer;
        // Saturate before addition, including the ends of the signed 64-bit range.
        uint64_t room=delta>0 ? (uint64_t)state->max.integer-(uint64_t)current
                              : (uint64_t)current-(uint64_t)state->min.integer;
        state->value.integer=(uint64_t)(delta>0 ? delta : -delta)>room
            ? (delta>0 ? state->max.integer : state->min.integer) : current+delta;
        return current!=state->value.integer;
    }
    if (a->argument==PACK_LITERAL) state->value=a->value;
    else {
        int v=a->argument==PACK_EVENT_X ? x : a->argument==PACK_EVENT_Y ? y : value;
        if (state->kind!=PACK_INTEGER || v<state->min.integer || v>state->max.integer) return false;
        state->value.integer=v;
    }
    return true;
}

bool pack_scene_tap(pack_scene_t *s, unsigned widget)
{
    return pack_scene_set(s,pack_scene_action(s,widget,PACK_TAP),0,0,0);
}
