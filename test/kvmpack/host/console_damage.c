// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "console_damage.h"
#include "monitor_viewport.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { WIDTH=600, HEIGHT=1024, PIXELS=WIDTH*HEIGHT };
static uint16_t panel[PIXELS], partial[PIXELS], previous[PIXELS], frame[2][1024*600];
static uint8_t invalidated[PIXELS], transferred[PIXELS];
static LV_ATTRIBUTE_MEM_ALIGN uint16_t strip[12*1024];
static unsigned flushed_pixels;

static void cover(uint8_t *mask,const lv_area_t *a)
{
    assert(a->x1>=0 && a->y1>=0 && a->x2<WIDTH && a->y2<HEIGHT);
    for (int y=a->y1;y<=a->y2;y++) memset(mask+y*WIDTH+a->x1,1,(size_t)lv_area_get_width(a));
}

static unsigned area(const uint8_t *mask)
{
    unsigned result=0;
    for (int i=0;i<PIXELS;i++) result+=mask[i];
    return result;
}

static void invalidation(lv_event_t *event)
{
    cover(invalidated,lv_event_get_invalidated_area(event));
}

static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *data)
{
    cover(transferred,a);
    flushed_pixels+=lv_area_get_size(a);
    const lv_draw_buf_t *buf=lv_display_get_buf_active(d);
    const unsigned stride=buf->header.stride;
    const size_t bytes=2u*(unsigned)lv_area_get_width(a);
    assert(buf->header.cf==LV_COLOR_FORMAT_RGB565 && stride>=bytes);
    for (int y=a->y1;y<=a->y2;y++,data+=stride) memcpy(panel+y*WIDTH+a->x1,data,bytes);
    lv_display_flush_ready(d);
}

static void reset_coverage(void)
{
    flushed_pixels=0;
    memset(invalidated,0,sizeof(invalidated));
    memset(transferred,0,sizeof(transferred));
}

static lv_obj_t *visible_image(lv_obj_t *obj)
{
    if (!lv_obj_is_visible(obj)) return NULL;
    if (lv_obj_check_type(obj,&lv_image_class) && lv_image_get_src(obj)) return obj;
    for (uint32_t i=0;i<lv_obj_get_child_count(obj);i++) {
        lv_obj_t *image=visible_image(lv_obj_get_child(obj,(int)i));
        if (image) return image;
    }
    return NULL;
}

typedef struct {const char *name;monitor_damage_t box;} trial_t;
static const trial_t trials[]={
    {"NW",{0,0,5,4}},{"NE",{633,0,639,5}},{"SW",{0,474,5,479}},{"SE",{634,475,639,479}},
    {"center",{304,226,335,249}},{"left",{0,185,8,221}},{"right",{634,211,639,250}},
    {"top",{216,0,230,8}},{"bottom",{345,475,367,479}},{"first-pixel",{0,0,0,0}},
    {"last-pixel",{639,479,639,479}},{"one-pixel",{317,239,317,239}},
    {"under-stop",{574,23,581,31}},{"identical",{0,0,-1,-1}}
};

static void invert_box(uint16_t *pixels,int stride,monitor_damage_t b)
{
    for (int y=b.y1;y<=b.y2;y++)
        for (int x=b.x1;x<=b.x2;x++) pixels[y*stride+x]^=0xffff;
}

static void viewport_pointer(const lv_area_t *bounds,int x,int y)
{
    lv_test_mouse_move_to(bounds->y1+y,WIDTH-1-(bounds->x1+x));
    lv_test_wait(50);
}

static void viewport_drag(const lv_area_t *bounds,int x,int y,int end_x,int end_y)
{
    viewport_pointer(bounds,x,y);
    lv_test_mouse_press();lv_test_wait(50);
    // Supply several real indev samples, including an intermediate drag past the press threshold.
    for (int i=1;i<=4;i++)
        viewport_pointer(bounds,x+(end_x-x)*i/4,y+(end_y-y)*i/4);
    lv_test_mouse_release();lv_test_wait(100);
}

static void viewport_tap(const lv_area_t *bounds,int x,int y)
{
    viewport_pointer(bounds,x,y);
    lv_test_mouse_press();lv_test_wait(50);
    lv_test_mouse_release();lv_test_wait(100);
}

static void check_overlays(const lv_area_t *b,const uint16_t *source,int stride,int left,int top,bool shown)
{
    const int centers[]={38,lv_area_get_width(b)-45};
    for (unsigned i=0;i<sizeof(centers)/sizeof(centers[0]);i++) {
        unsigned covered=0;
        for (int y=20;y<36;y++) for (int x=centers[i]-8;x<centers[i]+8;x++) {
            covered+=panel[(b->y1+y)*WIDTH+b->x1+x]!=source[(top+y)*stride+left+x];
        }
        assert(shown ? covered>32 : !covered);
    }
}

static void check_crop(const lv_area_t *b,const uint16_t *source,int stride,int left,int top)
{
    // Avoid both overlay buttons while sampling near all four corners and in the center.
    const int points[][2]={{2,2},{581,2},{2,435},{581,435},{292,219},{48,120}};
    for (unsigned i=0;i<sizeof(points)/sizeof(points[0]);i++) {
        int x=points[i][0],y=points[i][1];
        uint16_t actual=panel[(b->y1+y)*WIDTH+b->x1+x],expected=source[(top+y)*stride+left+x];
        if (actual!=expected) fprintf(stderr,"1:1 %dx%d crop (%d,%d) local (%d,%d): %04x != %04x\n",
                                     stride,lv_area_get_height(b),left,top,x,y,actual,expected);
        assert(actual==expected);
    }
}

static void check_zoom(lv_obj_t *screen,lv_obj_t *image,const uint16_t *initial,int sw,int sh,
                       void (*offer)(const uint16_t *,const monitor_damage_t *,unsigned))
{
    lv_area_t bounds;
    lv_obj_get_coords(lv_obj_get_parent(image),&bounds);
    const int w=lv_area_get_width(&bounds),h=lv_area_get_height(&bounds);
    assert(w==584 && h==438);
    const size_t bytes=(size_t)sw*sh*sizeof(uint16_t);
    for (int y=0;y<sh;y++) for (int x=0;x<sw;x++) frame[0][y*sw+x]=(uint16_t)(257*x+53*y);
    offer(frame[0],NULL,0);
    viewport_tap(&bounds,245,116);
    memcpy(previous,panel,sizeof(panel));
    viewport_tap(&bounds,245,116);
    viewport_tap(&bounds,38,28);
    check_crop(&bounds,frame[0],sw,(sw-w)/2,(sh-h)/2);
    viewport_tap(&bounds,245,116);
    check_overlays(&bounds,frame[0],sw,(sw-w)/2,(sh-h)/2,false);
    viewport_tap(&bounds,245,116);
    check_overlays(&bounds,frame[0],sw,(sw-w)/2,(sh-h)/2,true);
    viewport_drag(&bounds,62,78,552,374);
    check_crop(&bounds,frame[0],sw,0,0);
    check_overlays(&bounds,frame[0],sw,0,0,true);
    viewport_drag(&bounds,552,374,62,78);
    const int left=sw-w,top=sh-h;
    check_crop(&bounds,frame[0],sw,left,top);
    check_overlays(&bounds,frame[0],sw,left,top,true);

    uint16_t *next=sw==1024 ? frame[0] : frame[1];
    if (next!=frame[0]) memcpy(next,frame[0],bytes);
    const monitor_damage_t hidden={1,1,6,6},edge={left-4,top+140,left+7,top+148};
    invert_box(next,sw,hidden);
    reset_coverage();
    offer(next,&hidden,1);
    assert(!flushed_pixels);
    memcpy(partial,panel,sizeof(panel));
    invert_box(next,sw,edge);
    reset_coverage();
    offer(next,&edge,1);
    const unsigned sent=flushed_pixels;
    assert(sent>=8*9 && sent<24*24);
    for (int y=0;y<HEIGHT;y++) for (int x=0;x<WIDTH;x++) {
        int idx=y*WIDTH+x;
        bool edited=x>=bounds.x1 && x<bounds.x1+8 && y>=bounds.y1+140 && y<=bounds.y1+148;
        assert(panel[idx]==(edited ? (uint16_t)~partial[idx] : partial[idx]));
    }
    memcpy(partial,panel,sizeof(panel));
    reset_coverage();
    lv_obj_invalidate(screen);lv_refr_now(lv_obj_get_display(screen));
    assert(flushed_pixels==PIXELS && !memcmp(panel,partial,sizeof(panel)));
    invert_box(next,sw,hidden);invert_box(next,sw,edge);
    offer(next,NULL,0);
    viewport_tap(&bounds,38,28);
    viewport_tap(&bounds,245,116);
    assert(!memcmp(panel,previous,sizeof(panel)));
    viewport_tap(&bounds,245,116);
    offer(initial,NULL,0);
    printf("PASS 1:1 %dx%d center, opposite pan limits, clipped 72 changed pixels (%u flushed), full/Fit RGB565 equal\n",
           sw,sh,sent);
}

static void observe_flush(lv_timer_t *timer)
{
    int *observed=lv_timer_get_user_data(timer);
    *observed=flushed_pixels!=0;
}

static void check_refresh_scheduling(lv_obj_t *screen,const uint16_t *initial,bool native,int source_w,int source_h,
                                     void (*offer)(const uint16_t *,const monitor_damage_t *,unsigned))
{
    lv_display_t *d=lv_obj_get_display(screen);
    lv_timer_t *timer=lv_display_get_refr_timer(d);
    lv_timer_set_period(timer,1000);
    const monitor_damage_t region={310,225,335,238};
    memcpy(frame[0],initial,(size_t)source_w*source_h*sizeof(uint16_t));
    invert_box(frame[0],source_w,region);
    usb_monitor_redraw_t before,after;
    assert(monitor_viewport_redraw(&before));
    lv_timer_reset(timer);
    reset_coverage();
    offer(frame[0],&region,1);
    assert(monitor_viewport_redraw(&after) && after.partial==before.partial+1 && flushed_pixels);
    unsigned elapsed=(unsigned)(after.partial_ms-before.partial_ms);
    assert(elapsed<=1);

    // Leave shell damage pending. Idle or unchanged monitor data must not wake it.
    lv_timer_reset(timer);
    reset_coverage();
    lv_obj_invalidate(screen);
    for (unsigned i=0;i<75;i++) {lv_tick_inc(1);lv_timer_handler();}
    assert(!flushed_pixels);
    int observed=-1;
    // The fixture advances 100 ms and then forces a refresh with lv_test_wait.
    // Observe halfway through, after at least one regular receiver poll.
    lv_timer_t *probe=lv_timer_create(observe_flush,50,&observed);
    assert(probe);
    lv_timer_set_repeat_count(probe,1);
    offer(frame[0],&region,0);
    assert(observed==0 && monitor_viewport_redraw(&before) && before.unchanged==after.unchanged+1);
    lv_timer_set_period(timer,LV_DEF_REFR_PERIOD);
    offer(initial,NULL,0);
    printf("PASS %s Fit refresh scheduling: partial=%u ms from selection; idle/equal left shell damage pending\n",
           native ? "native" : "VGA",elapsed);
}

static void check_motion(lv_obj_t *screen,const uint16_t *initial,int w,int h,int source_w,int source_h,int second_y,
                         void (*offer)(const uint16_t *,const monitor_damage_t *,unsigned))
{
    const monitor_damage_t boxes[]={{32,32,47,47},{480,second_y,495,second_y+15}};
    const monitor_damage_t combined={32,32,495,second_y+15};
    size_t frame_bytes=(size_t)source_w*source_h*sizeof(uint16_t);
    memcpy(frame[0],initial,frame_bytes);
    invert_box(frame[0],source_w,boxes[0]);
    offer(frame[0],NULL,0);
    memcpy(previous,panel,sizeof(panel));
    // Native mode updates the selected RGB surface in place on the LVGL thread.
    uint16_t *next=source_w==1024 ? frame[0] : frame[1];
    memcpy(next,initial,frame_bytes);
    invert_box(next,source_w,boxes[1]);
    reset_coverage();
    usb_monitor_redraw_t before,after;
    assert(monitor_viewport_redraw(&before));
    offer(next,boxes,2);
    assert(monitor_viewport_redraw(&after));
    assert(after.split==before.split+1 && after.partial==before.partial+1 && after.full==before.full);
    unsigned split=flushed_pixels;
    assert(split && split==area(transferred));
    assert(memcmp(panel,previous,sizeof(panel)));
    memcpy(partial,panel,sizeof(partial));
    reset_coverage();
    offer(next,&combined,1);
    unsigned bounding=flushed_pixels;
    assert(bounding>split*(second_y==32 ? 3u : 10u) && !memcmp(panel,partial,sizeof(panel)));
    reset_coverage();
    lv_obj_invalidate(screen);lv_refr_now(lv_obj_get_display(screen));
    assert(flushed_pixels==PIXELS && !memcmp(panel,partial,sizeof(panel)));
    printf("PASS MOTION Fit %dx%d source=%dx%d cursor jump: split=%u bounding=%u full=%u RGB565 mismatch=0\n",
           w,h,source_w,source_h,split,bounding,flushed_pixels);

    // The two disjoint source pixels share filter support after fitting. Count
    // their output union once, including when LVGL combines the two redraws.
    const monitor_damage_t nearby[]={{312,235,312,235},{313,236,313,236}};
    unsigned singles=0;
    for (unsigned i=0;i<2;i++) {
        offer(next,&nearby[i],1);
        assert(monitor_viewport_redraw(&before));
        singles+=before.last_pixels;
    }
    if (next!=frame[0]) memcpy(frame[0],next,frame_bytes);
    invert_box(frame[0],source_w,nearby[0]); invert_box(frame[0],source_w,nearby[1]);
    reset_coverage();
    offer(frame[0],nearby,2);
    assert(monitor_viewport_redraw(&after));
    assert(after.split==before.split+1 && after.last_pixels>0 && after.last_pixels<singles);
    assert(after.last_pixels<=area(invalidated)); // LVGL can expand the requested area by a pixel.
    memcpy(partial,panel,sizeof(partial));
    lv_obj_invalidate(screen);lv_refr_now(lv_obj_get_display(screen));
    assert(!memcmp(panel,partial,sizeof(panel)));
}

void check_console_damage(lv_obj_t *screen,const uint16_t *initial,bool native,
                          void (*offer)(const uint16_t *,const monitor_damage_t *,unsigned))
{
    lv_obj_t *image=visible_image(screen);
    assert(image);
    const int source_w=native ? 1024 : 640, source_h=native ? 600 : 480;
    size_t frame_bytes=(size_t)source_w*source_h*sizeof(uint16_t);
    const lv_image_dsc_t *descriptor=lv_image_get_src(image);
    assert(descriptor->header.w==source_w && descriptor->header.h==source_h &&
           descriptor->header.cf==LV_COLOR_FORMAT_RGB565 && descriptor->header.stride==2*source_w &&
           descriptor->data_size==frame_bytes);
    lv_obj_t *container=lv_obj_get_parent(image);
    const int original_w=lv_obj_get_width(container), original_h=lv_obj_get_height(container);
    lv_display_t *d=lv_obj_get_display(image);
    assert(lv_display_get_rotation(d)==LV_DISPLAY_ROTATION_90);
    assert(lv_display_get_render_mode(d)==LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_draw_buf_t *saved=lv_display_get_buf_active(d);
    lv_display_flush_cb_t original=lv_display_get_flush_cb(d);
    lv_display_set_buffers(d,strip,NULL,sizeof(strip),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    assert(lv_display_add_event_cb(d,invalidation,LV_EVENT_INVALIDATE_AREA,NULL));
    lv_obj_invalidate(screen);lv_refr_now(d);
    check_refresh_scheduling(screen,initial,native,source_w,source_h,offer);
    check_zoom(screen,image,initial,source_w,source_h,offer);
    // A tall community viewport fits the full source height at 1:1. Panning
    // moves only the clipped axis, including at odd sizes where centering rounds.
    lv_obj_set_size(container,173,source_h+23);
    lv_obj_update_layout(screen);
    lv_test_wait(80);
    lv_area_t tall;
    lv_obj_get_coords(container,&tall);
    viewport_tap(&tall,38,28);
    assert(lv_image_get_scale(image)==LV_SCALE_NONE);
    viewport_drag(&tall,50,110,90,160);
    assert(lv_image_get_offset_x(image)==40 && lv_image_get_offset_y(image)==0);
    const int source_x=source_w/2-173/2-40+83;
    assert(panel[(tall.y1+179)*WIDTH+tall.x1+83]==initial[(179-11)*source_w+source_x]);
    viewport_tap(&tall,38,28);
    const int sizes[][2]={{584,438},{337,189},{173,241}};
    unsigned index=0;
    for (unsigned mode=0;mode<sizeof(sizes)/sizeof(sizes[0]);mode++) {
        int w=sizes[mode][0],h=sizes[mode][1];
        lv_obj_set_size(container,w,h);
        lv_obj_update_layout(screen);
        lv_obj_invalidate(screen);
        lv_test_wait(80);
        assert(lv_image_get_scale(image)==LV_MIN(w*LV_SCALE_NONE/source_w,h*LV_SCALE_NONE/source_h));
        memcpy(frame[index],initial,frame_bytes);
        reset_coverage();
        offer(frame[index],NULL,0);
        unsigned full=area(transferred);
        assert(full>=(unsigned)(w*h));
        const unsigned named=sizeof(trials)/sizeof(trials[0]);
        for (unsigned t=0;t<named+24;t++) {
            // Different sampling phases, including narrow vertical and horizontal edits.
            int x=(int)((t*197u+43u)%(unsigned)(source_w-12)),y=(int)((t*113u+29u)%(unsigned)(source_h-12));
            trial_t extra={"phase",{x,y,x+(int)(t%12u),y+(int)((t*5u)%12u)}};
            const trial_t *trial=t<named ? &trials[t] : &extra;
            monitor_damage_t region=trial->box;
            if (native && t<named && region.x2>=region.x1) {
                // Map named VGA probes to the same edges/corners of the larger source.
                region.x1=region.x1*(source_w-1)/639; region.x2=region.x2*(source_w-1)/639;
                region.y1=region.y1*(source_h-1)/479; region.y2=region.y2*(source_h-1)/479;
            }
            const monitor_damage_t *b=&region;
            unsigned next=native ? index : index^1u;
            if (next!=index) memcpy(frame[next],frame[index],frame_bytes);
            invert_box(frame[next],source_w,*b);
            memcpy(previous,panel,sizeof(panel));
            reset_coverage();
            usb_monitor_redraw_t before,after;
            assert(monitor_viewport_redraw(&before));
            offer(frame[next],b,b->x2>=b->x1 ? 1u : 0u);
            assert(monitor_viewport_redraw(&after));
            unsigned inv=area(invalidated),sent=area(transferred);
            memcpy(partial,panel,sizeof(panel));
            // Repaint every physical output pixel through the same LVGL flush callback.
            reset_coverage();
            lv_obj_invalidate(screen);
            lv_refr_now(d);
            assert(area(transferred)==PIXELS);
            unsigned different=0, changed=0;
            for (int i=0;i<PIXELS;i++) {
                different+=panel[i]!=partial[i];
                changed+=panel[i]!=previous[i];
            }
            printf("DAMAGE %dx%d %-12s changed=%u invalidated=%u flushed=%u full=%u mismatch=%u source=%dx%d\n",
                   w,h,trial->name,changed,inv,sent,full,different,source_w,source_h);
            fflush(stdout);
            assert(!memcmp(partial,panel,sizeof(panel)));
            assert(after.full==before.full);
            if (b->x2<b->x1) {
                assert(!changed && !inv && !sent && !after.last_pixels);
                assert(after.unchanged==before.unchanged+1 && after.partial==before.partial);
            }
            else {
                if (!native && mode==0 && !strcmp(trial->name,"under-stop")) assert(!changed);
                if (!strcmp(trial->name,"center")) assert(changed);
                assert(after.partial==before.partial+1 && after.unchanged==before.unchanged);
                assert(after.partial_pixels-before.partial_pixels==after.last_pixels);
                // Outward rounding, filter taps and LVGL's invalidation margin are bounded.
                unsigned sw=(unsigned)(b->x2-b->x1+1),sh=(unsigned)(b->y2-b->y1+1);
                unsigned fit_width=LV_MIN((unsigned)w,(unsigned)h*source_w/source_h);
                unsigned max_area=((sw*fit_width+source_w-1)/source_w+8)*((sh*fit_width+source_w-1)/source_w+8);
                assert(inv<=max_area && sent<=max_area && after.last_pixels<=max_area);
            }
            index=next;
        }
        check_motion(screen,initial,w,h,source_w,source_h,288,offer);
        check_motion(screen,initial,w,h,source_w,source_h,32,offer);
    }
    lv_obj_set_size(container,original_w,original_h);
    lv_obj_update_layout(screen);
    offer(initial,NULL,0);
    lv_display_remove_event_cb_with_user_data(d,invalidation,NULL);
    lv_display_set_flush_cb(d,original);
    lv_display_set_draw_buffers(d,saved,NULL);
    lv_display_set_render_mode(d,LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_invalidate(screen);
    lv_refr_now(d);
}
