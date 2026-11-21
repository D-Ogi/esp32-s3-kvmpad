// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "pack_chart_view.h"
#include "ui_theme.h"
#include "src/draw/lv_draw_buf_private.h"
#include "src/image/lv_image_decoder_private.h"
#include "src/misc/lv_area_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>

enum { STRIP_ROWS=4, MAX_VIEW_WIDTH=1024 };
static const uint16_t zoom_levels[]={100,125,150,200,300,400,500,600};

typedef struct {
    int zoom, dx, dy;  // offsets from a centered, fitted chart, in scaled output pixels
} camera_t;

typedef struct {
    const pack_scene_t *scene;
    const pack_chart_t *data;
    camera_t camera[PACK_CHARTS];
    unsigned active, slot[2], count;
    int width, height, scaled_w, scaled_h, left, top;
    uint16_t edge_x[PACK_CHART_WIDTH+1];  // exclusive output end of each source interval
    uint32_t rows[PACK_CHART_HEIGHT+1];
    uint16_t palette565[16];
    bool opaque;
    lv_point_t last;
    bool dragging;
    enum { PINCH_IDLE, PINCH_ACTIVE, PINCH_TAIL, PINCH_IGNORED } pinch;
    int span, base_zoom, base_w, base_h;
    lv_point_t anchor;  // source position expressed in initial scaled pixels
    lv_obj_t *image,*slots[2],*percent,*minus,*plus,*catalog,*heading;
    lv_image_dsc_t descriptor;
    lv_image_decoder_t *decoder;
} chart_view_t;
static chart_view_t *view;

size_t pack_chart_view_storage(void) { return sizeof(chart_view_t); }

size_t pack_chart_view_budget(unsigned width,unsigned count)
{
    // Includes the transient decoder strip and the slot, toolbar and catalog controls.
    return sizeof(chart_view_t)+4u*width*STRIP_ROWS+(10u+count)*1536u;
}

static bool index_chart(const pack_chart_t *data)
{
    if (!data || !data->width || !data->height || data->width>PACK_CHART_WIDTH ||
        data->height>PACK_CHART_HEIGHT || !data->runs) return false;
    size_t cursor=0;
    unsigned used=0;
    for (unsigned y=0; y<data->height; y++) {
        unsigned x=0;
        view->rows[y]=(uint32_t)cursor;
        while (x<data->width && cursor<data->run_count) {
            uint8_t run=data->runs[cursor++];
            used|=1u<<(run&15);
            x+=(run>>4)+1;
        }
        if (x!=data->width) return false;
    }
    view->rows[data->height]=(uint32_t)cursor;
    view->opaque=true;
    for (unsigned i=0;i<16;i++) {
        uint32_t c=data->palette[i];
        if ((used&(1u<<i)) && (c>>24)!=255u) view->opaque=false;
        view->palette565[i]=(uint16_t)(((c>>8)&0xf800u)|((c>>5)&0x07e0u)|((c>>3)&0x001fu));
    }
    return cursor==data->run_count;
}

static lv_result_t chart_info(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *d,lv_image_header_t *header)
{
    (void)decoder;
    if (!view || d->src!=&view->descriptor) return LV_RESULT_INVALID;
    *header=view->descriptor.header;
    header->cf=LV_COLOR_FORMAT_ARGB8888;
    header->stride=4u*header->w;
    return LV_RESULT_OK;
}

static lv_result_t chart_open(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *d)
{
    if (chart_info(decoder,d,&d->header)!=LV_RESULT_OK) return LV_RESULT_INVALID;
    d->args.no_cache=true;
    lv_color_format_t format=view->opaque ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_ARGB8888;
    d->user_data=lv_draw_buf_create(d->header.w,STRIP_ROWS,format,LV_STRIDE_AUTO);
    return d->user_data ? LV_RESULT_OK : LV_RESULT_INVALID;
}

static void draw_chart_row(void *out,int sy,unsigned first_sample,int start,int stop)
{
    uint16_t *rgb565=out;
    uint32_t *argb=out;
    const pack_chart_t *data=view->data;
    const uint8_t *run=data->runs+view->rows[sy];
    unsigned sx=0;
    uint8_t packed;
    do {
        packed=*run++;
        sx+=(packed>>4)+1u;
    } while (sx<=first_sample);
    for (;;) {
        int end=LV_MIN(view->edge_x[sx],stop);
        if (view->opaque) {
            uint16_t color=view->palette565[packed&15];
            while (start<end) { *rgb565++=color; start++; }
        } else {
            uint32_t color=data->palette[packed&15];
            while (start<end) { *argb++=color; start++; }
        }
        if (start==stop) return;
        packed=*run++;
        sx+=(packed>>4)+1u;
    }
}

static lv_result_t chart_area(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *d,
                              const lv_area_t *request,lv_area_t *area)
{
    (void)decoder;
    const pack_chart_t *data=view->data;
    bool first=area->y1==LV_COORD_MIN;
    int x1=LV_MAX(request->x1,view->left);
    int x2=LV_MIN(request->x2,view->left+view->scaled_w-1);
    int y=first ? LV_MAX(request->y1,view->top) : area->y2+1;
    int last=LV_MIN(request->y2,view->top+view->scaled_h-1);
    // Unreturned margins are transparent; LVGL already painted their background.
    if (x1>x2 || y>last) return LV_RESULT_INVALID;
    lv_draw_buf_t *strip=d->user_data;
    unsigned width=(unsigned)(x2-x1+1);
    unsigned row_bytes=width*(view->opaque ? sizeof(uint16_t) : sizeof(uint32_t));
    if (first) {
        // A rotated or partially covered view can request only a few columns.
        // Use the same strip allocation for more rows in this case.
        unsigned stride=lv_draw_buf_width_to_stride(width,strip->header.cf);
        unsigned rows=LV_MIN((unsigned)(last-y+1),strip->data_size/stride);
        if (!lv_draw_buf_reshape(strip,strip->header.cf,width,rows,stride))
            return LV_RESULT_INVALID;
    }
    last=LV_MIN(last,y+(int)strip->header.h-1);
    int previous_y=-1;
    uint8_t *previous=NULL;
    if (!first) {
        previous_y=(area->y2-view->top)*(int)data->height/view->scaled_h;
        previous=strip->data+(area->y2-area->y1)*strip->header.stride;
    }
    int start=x1-view->left, stop=x2-view->left+1;
    unsigned first_sample=(unsigned)(start*data->width/view->scaled_w);
    for (int row=y;row<=last;row++) {
        uint8_t *out=strip->data+(row-y)*strip->header.stride;
        int sy=(row-view->top)*(int)data->height/view->scaled_h;
        if (sy==previous_y) {
            if (out!=previous) memcpy(out,previous,row_bytes);
        } else {
            draw_chart_row(out,sy,first_sample,start,stop);
        }
        previous=out;
        previous_y=sy;
    }
    *area=(lv_area_t){x1,y,x2,last};
    d->decoded=strip;
    return LV_RESULT_OK;
}

static void chart_close(lv_image_decoder_t *decoder,lv_image_decoder_dsc_t *d)
{
    (void)decoder;
    if (d->user_data) lv_draw_buf_destroy(d->user_data);
}

static camera_t *camera(void) { return &view->camera[view->slot[view->active]]; }

unsigned pack_chart_view_visible_zoom(void)
{
    if (!view || !view->image || lv_obj_get_screen(view->image)!=lv_screen_active() ||
        !lv_obj_is_visible(view->image) || (view->catalog && lv_obj_is_visible(view->catalog))) return 0;
    return (unsigned)camera()->zoom;
}

static void chart_dimensions(int zoom,int *scaled_w,int *scaled_h)
{
    int w=view->width,h=view->height;
    int fw=w,fh=h,sw=view->data->width,sh=view->data->height;
    if (w*sh<=h*sw) fh=LV_MAX(1,(w*sh+sw/2)/sw);
    else fw=LV_MAX(1,(h*sw+sh/2)/sh);
    *scaled_w=LV_MAX(1,(fw*zoom+50)/100);
    *scaled_h=LV_MAX(1,(fh*zoom+50)/100);
}

static void invalidate_camera_change(const lv_area_t *old_chart)
{
    lv_area_t current={view->left,view->top,view->left+view->scaled_w-1,view->top+view->scaled_h-1};
    lv_area_t bounds={0,0,view->width-1,view->height-1};
    lv_area_t damage;
    lv_area_join(&damage,old_chart,&current);
    if (!lv_area_intersect(&damage,&damage,&bounds)) return;
    lv_area_t image;
    lv_obj_get_coords(view->image,&image);
    lv_area_move(&damage,image.x1,image.y1);
    lv_obj_invalidate_area(view->image,&damage);
}

static void position_chart(bool force)
{
    camera_t *c=camera();
    int w=view->width,h=view->height,sw=view->data->width;
    int old_width=view->scaled_w,old_height=view->scaled_h;
    lv_area_t old_chart={view->left,view->top,view->left+old_width-1,view->top+old_height-1};
    chart_dimensions(c->zoom,&view->scaled_w,&view->scaled_h);
    // Retain the odd final pixel at both limits of travel.
    int left=(w-view->scaled_w)/2, top=(h-view->scaled_h)/2;
    c->dx=view->scaled_w>w ? LV_CLAMP(w-view->scaled_w-left,c->dx,-left) : 0;
    c->dy=view->scaled_h>h ? LV_CLAMP(h-view->scaled_h-top,c->dy,-top) : 0;
    left+=c->dx;
    top+=c->dy;
    if (!force && old_width==view->scaled_w && old_height==view->scaled_h &&
        left==view->left && top==view->top) return;
    view->left=left;
    view->top=top;
    if (force || old_width!=view->scaled_w) {
        // Nearest-neighbor samples use floor(output * source / scaled).
        // Precompute the corresponding exclusive ends once per chart or zoom.
        for (int x=0;x<=sw;x++)
            view->edge_x[x]=(uint16_t)((x*view->scaled_w+sw-1)/sw);
    }
    if (force) lv_obj_invalidate(view->image);
    else invalidate_camera_change(&old_chart);
}

static void zoom_controls(void)
{
    int zoom=camera()->zoom;
    lv_label_set_text_fmt(view->percent,"%u%%",(unsigned)zoom);
    lv_obj_set_disabled(view->minus,zoom==100);
    lv_obj_set_disabled(view->plus,zoom==600);
}

static void refresh_controls(void)
{
    for (unsigned i=0;i<2;i++) {
        const pack_chart_t *data=pack_chart_at(view->scene,view->slot[i]);
        lv_label_set_text_fmt(lv_obj_get_child(view->slots[i],0),"%c  %s",'A'+(int)i,data->title);
        lv_obj_set_checked(view->slots[i],i==view->active);
    }
    zoom_controls();
    position_chart(true);
}

enum { SLOT_A, SLOT_B, MINUS, PLUS, FIT, CHOOSE, CANCEL, PIN_FIRST };

static void clicked(lv_event_t *e)
{
    if (!view || !lv_obj_is_visible(lv_event_get_current_target_obj(e))) return;
    unsigned op=(unsigned)(uintptr_t)lv_event_get_user_data(e);
    camera_t *c=camera();
    if (view->pinch==PINCH_ACTIVE) return;
    if (op==CHOOSE) {
        lv_label_set_text_fmt(view->heading,"Choose chart for slot %c",'A'+(int)view->active);
        lv_obj_set_hidden(view->catalog,false);
        return;
    }
    if (op==CANCEL) { lv_obj_set_hidden(view->catalog,true); return; }
    if (op==FIT) *c=(camera_t){.zoom=100};
    else if (op==PLUS || op==MINUS) {
        int previous=c->zoom;
        for (unsigned i=1;i<sizeof(zoom_levels)/sizeof(*zoom_levels);i++) {
            if (op==PLUS && previous<zoom_levels[i]) { c->zoom=zoom_levels[i]; break; }
            if (op==MINUS && previous<=zoom_levels[i]) { c->zoom=zoom_levels[i-1]; break; }
        }
        c->dx=c->dx*c->zoom/previous;
        c->dy=c->dy*c->zoom/previous;
    } else {
        unsigned active=op<=SLOT_B ? op : view->active;
        unsigned card=op<=SLOT_B ? view->slot[active] : op-PIN_FIRST;
        if (card>=view->count) return;
        const pack_chart_t *data=pack_chart_at(view->scene,card);
        if (data!=view->data && !index_chart(data)) return;
        if (op>=PIN_FIRST && view->slot[1-active]==card)
            view->slot[1-active]=view->slot[active];
        view->slot[active]=card;
        view->active=active; view->data=data;
        lv_obj_set_hidden(view->catalog,true);
    }
    view->dragging=false;
    refresh_controls();
}

static void pan(lv_event_t *e)
{
    if (!view || view->pinch!=PINCH_IDLE) return;
    lv_event_code_t code=lv_event_get_code(e);
    if (code==LV_EVENT_RELEASED || code==LV_EVENT_PRESS_LOST) { view->dragging=false; return; }
    if (code!=LV_EVENT_PRESSED && code!=LV_EVENT_PRESSING) return;
    lv_indev_t *indev=lv_indev_active();
    if (!indev || lv_indev_get_type(indev)!=LV_INDEV_TYPE_POINTER) return;
    lv_point_t point;
    lv_indev_get_point(indev,&point);
    lv_obj_transform_point(view->image,&point,LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE | LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
    if (code==LV_EVENT_PRESSED) view->dragging=true;
    else if (view->dragging && (point.x!=view->last.x || point.y!=view->last.y)) {
        camera_t *c=camera();
        c->dx+=point.x-view->last.x; c->dy+=point.y-view->last.y;
        position_chart(false);
    }
    view->last=point;
}

static lv_point_t chart_point(lv_point_t logical)
{
    // With software rotation this chart lives inside a transformed frame.
    // With display rotation LVGL already uses the logical screen coordinates.
    if (view->scene->portrait && lv_display_get_rotation(lv_obj_get_display(view->image))==LV_DISPLAY_ROTATION_0)
        return (lv_point_t){logical.y,599-logical.x};
    return logical;
}

static bool hits_chart(lv_point_t logical)
{
    lv_point_t point=chart_point(logical);
    lv_display_t *display=lv_obj_get_display(view->image);
    return !lv_indev_search_obj(lv_display_get_layer_sys(display),&point) &&
           !lv_indev_search_obj(lv_display_get_layer_top(display),&point) &&
           lv_indev_search_obj(lv_display_get_screen_active(display),&point)==view->image;
}

bool pack_chart_view_touch(unsigned count,const lv_point_t points[2])
{
    if (!view || !view->image) return false;
    if (!count) {
        view->pinch=PINCH_IDLE;
        return false;
    }
    if (view->pinch==PINCH_TAIL || view->pinch==PINCH_IGNORED) return view->pinch==PINCH_TAIL;
    if (count!=2 || !points || !lv_obj_is_visible(view->image)) {
        if (view->pinch==PINCH_ACTIVE) view->pinch=PINCH_TAIL;
        else if (count>1) view->pinch=PINCH_IGNORED;
        return view->pinch==PINCH_TAIL;
    }
    if (view->pinch==PINCH_IDLE) {
        if (!hits_chart(points[0]) || !hits_chart(points[1])) {
            view->pinch=PINCH_IGNORED;
            return false;
        }
        view->dragging=false;
        view->pinch=PINCH_ACTIVE;
        view->span=0;
    }

    int dx=points[1].x-points[0].x, dy=points[1].y-points[0].y;
    int span=lv_sqrt32((uint32_t)(dx*dx+dy*dy));
    lv_point_t middle={(points[0].x+points[1].x)/2,(points[0].y+points[1].y)/2};
    middle=chart_point(middle);
    lv_obj_transform_point(view->image,&middle,LV_OBJ_POINT_TRANSFORM_FLAG_RECURSIVE | LV_OBJ_POINT_TRANSFORM_FLAG_INVERSE);
    lv_area_t bounds;
    lv_obj_get_coords(view->image,&bounds);
    middle.x-=bounds.x1; middle.y-=bounds.y1;
    camera_t *c=camera();
    if (!view->span) {
        if (span<8) return true;  // Establish a stable baseline once the contacts separate.
        view->span=span; view->base_zoom=c->zoom;
        view->base_w=view->scaled_w; view->base_h=view->scaled_h;
        view->anchor=(lv_point_t){middle.x-view->left,middle.y-view->top};
        return true;
    }
    int zoom=LV_CLAMP(100,(view->base_zoom*span+view->span/2)/view->span,600);
    int sw,sh;
    chart_dimensions(zoom,&sw,&sh);
    c->dx=middle.x-view->anchor.x*sw/view->base_w-(view->width-sw)/2;
    c->dy=middle.y-view->anchor.y*sh/view->base_h-(view->height-sh)/2;
    if (c->zoom!=zoom) { c->zoom=zoom; zoom_controls(); }
    position_chart(false);
    return true;
}

static lv_obj_t *button(lv_obj_t *parent,int x,int y,int w,int h,const char *title,unsigned op)
{
    lv_obj_t *b=lv_button_create(parent);
    if (!b) return NULL;
    ui_theme_control_button(b); lv_obj_set_pos(b,x,y); lv_obj_set_size(b,w,h);
    lv_obj_t *label=lv_label_create(b);
    if (!label) return NULL;
    lv_obj_remove_style_all(label);
    lv_obj_set_width(label,lv_pct(100)); lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_label_set_long_mode(label,LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label,title); lv_obj_center(label);
    if (!lv_obj_add_event_cb(b,clicked,LV_EVENT_CLICKED,(void *)(uintptr_t)op)) return NULL;
    return b;
}

bool pack_chart_view_open(lv_obj_t *parent,const pack_scene_t *scene,const pack_widget_t *w,void *storage)
{
    if (view || !storage || w->tab_count<2 || w->tab_count>PACK_CHARTS || w->w>MAX_VIEW_WIDTH ||
        w->w<540 || w->h<420) return false;
    view=storage;
    view->scene=scene; view->count=w->tab_count; view->slot[1]=1;
    for (unsigned i=0;i<view->count;i++) {
        view->camera[i].zoom=100;
        if (!index_chart(pack_chart_at(scene,i))) return false;
    }
    view->data=pack_chart_at(scene,0);
    if (!index_chart(view->data)) return false;
    view->width=w->w; view->height=w->h-188;
    lv_obj_remove_style_all(parent);
    lv_obj_set_scrollable(parent,false);
    // Removing the parent's styles also clears its authored coordinates.
    lv_obj_set_pos(parent,w->x,w->y); lv_obj_set_size(parent,w->w,w->h);
    int half=(w->w-8)/2;
    view->slots[0]=button(parent,0,0,half,52,"",SLOT_A);
    view->slots[1]=button(parent,half+8,0,w->w-half-8,52,"",SLOT_B);
    view->decoder=lv_image_decoder_create();
    if (!view->slots[0] || !view->slots[1] || !view->decoder) return false;
    lv_image_decoder_set_info_cb(view->decoder,chart_info);
    lv_image_decoder_set_open_cb(view->decoder,chart_open);
    lv_image_decoder_set_get_area_cb(view->decoder,chart_area);
    lv_image_decoder_set_close_cb(view->decoder,chart_close);
    view->descriptor=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RAW,
        .w=view->width,.h=view->height},.data=(const uint8_t *)view,.data_size=sizeof(*view)};
    view->image=lv_image_create(parent);
    if (!view->image) return false;
    lv_image_set_src(view->image,&view->descriptor);
    lv_obj_set_pos(view->image,0,60);
    lv_obj_set_clickable(view->image,true); lv_obj_set_press_lock(view->image,true);
    if (!lv_obj_add_event_cb(view->image,pan,LV_EVENT_ALL,NULL)) return false;
    int y=w->h-116;
    view->minus=button(parent,0,y,72,50,LV_SYMBOL_MINUS,MINUS);
    view->plus=button(parent,194,y,72,50,LV_SYMBOL_PLUS,PLUS);
    if (!view->minus || !view->plus || !button(parent,w->w-132,y,132,50,"Fit",FIT)) return false;
    view->percent=lv_label_create(parent);
    if (!view->percent || !button(parent,0,w->h-56,w->w,56,"Choose chart",CHOOSE)) return false;
    lv_obj_set_size(view->percent,108,30); lv_obj_set_pos(view->percent,79,y+13);
    lv_obj_set_style_text_align(view->percent,LV_TEXT_ALIGN_CENTER,0);
    view->catalog=lv_obj_create(parent);
    if (!view->catalog) return false;
    lv_obj_remove_style_all(view->catalog); lv_obj_set_scrollable(view->catalog,false);
    ui_theme_apply(view->catalog,UI_STYLE_CONTROL_PAGE,0); lv_obj_set_size(view->catalog,w->w,w->h);
    view->heading=lv_label_create(view->catalog);
    if (!view->heading) return false;
    lv_obj_set_pos(view->heading,12,12);
    for (unsigned i=0;i<view->count;i++)
        if (!button(view->catalog,12,62+64*(int)i,w->w-24,56,pack_chart_at(scene,i)->title,PIN_FIRST+i)) return false;
    if (!button(view->catalog,12,68+64*(int)view->count,w->w-24,52,"Cancel",CANCEL)) return false;
    lv_obj_set_hidden(view->catalog,true);
    refresh_controls();
    return true;
}

void pack_chart_view_close(void)
{
    if (!view) return;
    if (view->image) lv_image_set_src(view->image,NULL);
    lv_image_cache_drop(&view->descriptor);
    if (view->decoder) lv_image_decoder_delete(view->decoder);
    view=NULL;
}
