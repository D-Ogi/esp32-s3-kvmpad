// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "monitor_viewport.h"
#include "monitor_present.h"
#include "monitor_wire.h"
#include "src/misc/cache/instance/lv_image_cache.h"

static lv_display_t *display;
static lv_obj_t *target;
static lv_timer_t *timer;
static bool pending, flushed;
static usb_monitor_redraw_t redraw;
static bool full_redraw, split_redraw;
static uint32_t selected_tick;
static lv_image_dsc_t picture;

// Match the active LVGL alignment, including the rounding of odd viewport sizes.
// Expand the source rectangle for bilinear sampling before clipping.
static lv_area_t viewport_damage(const lv_area_t *bounds, const monitor_damage_t *d)
{
    const int scale=lv_image_get_scale(target);
    int w=lv_area_get_width(bounds), h=lv_area_get_height(bounds);
    int x=bounds->x1+lv_image_get_offset_x(target), y=bounds->y1+lv_image_get_offset_y(target);
    lv_area_t area;
    if (lv_image_get_inner_align(target)==LV_IMAGE_ALIGN_CENTER) {
        x+=w/2-(int)picture.header.w/2;
        y+=h/2-(int)picture.header.h/2;
        area=(lv_area_t){x+d->x1,y+d->y1,x+d->x2,y+d->y2};
    } else {
        x+=(w-(int)picture.header.w*scale/LV_SCALE_NONE)/2;
        y+=(h-(int)picture.header.h*scale/LV_SCALE_NONE)/2;
        area=(lv_area_t){
            x+LV_MAX(0,d->x1-1)*scale/LV_SCALE_NONE-1,
            y+LV_MAX(0,d->y1-1)*scale/LV_SCALE_NONE-1,
            x+((d->x2+2)*scale+LV_SCALE_NONE-1)/LV_SCALE_NONE,
            y+((d->y2+2)*scale+LV_SCALE_NONE-1)/LV_SCALE_NONE
        };
    }
    area.x1=LV_MAX(area.x1,bounds->x1);
    area.y1=LV_MAX(area.y1,bounds->y1);
    area.x2=LV_MIN(area.x2,bounds->x2);
    area.y2=LV_MIN(area.y2,bounds->y2);
    return area;
}

static void present(const uint16_t *pixels, bool nonblack, const monitor_damage_t *damage, unsigned count)
{
    (void)nonblack;
    bool first = !picture.data;
    lv_image_cache_drop(&picture);
    picture.data=(const uint8_t *)pixels;
    if (!pixels) {
        lv_image_set_src(target,NULL);
        return;
    }
    if (first) lv_image_set_src(target,&picture);
    if (!first && damage && !count) {
        ++redraw.unchanged;
        redraw.last_pixels=0;
        return;
    }
    lv_area_t bounds;
    lv_obj_get_coords(target,&bounds);
    full_redraw=first || !damage || (count==1 && damage->x1==0 && damage->y1==0 &&
                damage->x2==(int)picture.header.w-1 && damage->y2==(int)picture.header.h-1);
    redraw.last_pixels=0;
    unsigned submitted=0;
    lv_area_t previous_area={0,0,-1,-1};
    for (unsigned i=0;i<(full_redraw ? 1u : count);i++) {
        lv_area_t area=full_redraw ? bounds : viewport_damage(&bounds,&damage[i]);
        if (area.x2<area.x1 || area.y2<area.y1 || lv_obj_invalidate_area(target,&area)!=LV_RESULT_OK) continue;
        submitted++;
        redraw.last_pixels+=lv_area_get_size(&area);
        // Bilinear margins can overlap after downscaling two disjoint source rectangles.
        if (i && previous_area.x2>=previous_area.x1) {
            int width=LV_MIN(previous_area.x2,area.x2)-LV_MAX(previous_area.x1,area.x1)+1;
            int height=LV_MIN(previous_area.y2,area.y2)-LV_MAX(previous_area.y1,area.y1)+1;
            if (width>0 && height>0) redraw.last_pixels-=(unsigned)(width*height);
        }
        previous_area=area;
    }
    if (!redraw.last_pixels) return;
    split_redraw=submitted==2;
    pending=true;
    flushed=false;
    selected_tick=lv_tick_get();
}

static void refreshed(lv_event_t *event)
{
    if (!pending || !target || !lv_obj_is_visible(target)) return;
    if (lv_event_get_code(event)==LV_EVENT_FLUSH_FINISH) flushed=lv_display_flush_is_last(display);
    else if (flushed) {
        uint32_t elapsed=lv_tick_elaps(selected_tick);
        if (split_redraw) ++redraw.split;
        if (full_redraw) {
            ++redraw.full;
            redraw.full_ms+=elapsed;
        } else {
            ++redraw.partial;
            redraw.partial_ms+=elapsed;
            redraw.partial_pixels+=redraw.last_pixels;
        }
        monitor_refresh_complete();
        pending=false;
    }
}

static void poll(lv_timer_t *unused)
{
    (void)unused;
    if (!pending && target && lv_obj_is_visible(target)) {
        monitor_present(present);
        if (pending) {
            lv_timer_t *refresh = lv_display_get_refr_timer(display);
            if (refresh) lv_timer_ready(refresh);
        }
    }
}

void monitor_viewport_target(lv_obj_t *image)
{
    if (target==image) return;
    if (target) lv_image_set_src(target,NULL);
    target=image;
    pending=flushed=false;
    if (image && picture.data) lv_image_set_src(image,&picture);
}

void monitor_viewport_stop(void)
{
    if (!display) return;
    lv_display_remove_event_cb_with_user_data(display,refreshed,NULL);
    if (timer) lv_timer_delete(timer);
    timer=NULL;
    monitor_viewport_target(NULL);
    lv_image_cache_drop(&picture);
    picture.data=NULL;
    display=NULL;
    monitor_stop();
}

static void prepare_picture(bool native)
{
    unsigned width=monitor_width(native), height=monitor_height(native);
    picture=(lv_image_dsc_t){
        .header={.magic=LV_IMAGE_HEADER_MAGIC,.w=width,.h=height,.cf=LV_COLOR_FORMAT_RGB565,.stride=width*2},
        .data_size=width*height*2
    };
    redraw=(usb_monitor_redraw_t){0};
}

usb_monitor_mode_result_t monitor_viewport_change_mode(bool native, bool (*set_profile)(bool))
{
    if (!display) return USB_MONITOR_MODE_STOPPED;
    if (!set_profile) return USB_MONITOR_MODE_RESTORED;
    if (picture.header.w==monitor_width(native)) return USB_MONITOR_MODE_CHANGED;
    if (target) lv_image_set_src(target,NULL);
    lv_image_cache_drop(&picture);
    picture.data=NULL;
    pending=flushed=false;
    usb_monitor_mode_result_t result=monitor_change_receiver(native,set_profile);
    if (result==USB_MONITOR_MODE_STOPPED) monitor_viewport_stop();
    else prepare_picture(result==USB_MONITOR_MODE_CHANGED ? native : !native);
    return result;
}

bool monitor_viewport_start(lv_obj_t *image, bool native)
{
    if (display || !image || !monitor_start(native)) return false;
    prepare_picture(native);
    display=lv_obj_get_display(image);
    monitor_viewport_target(image);
    timer=lv_timer_create(poll,MONITOR_POLL_MS,NULL);
    if (timer && lv_display_add_event_cb(display,refreshed,LV_EVENT_FLUSH_FINISH,NULL) &&
        lv_display_add_event_cb(display,refreshed,LV_EVENT_REFR_READY,NULL)) return true;
    monitor_viewport_stop();
    return false;
}

bool monitor_viewport_redraw(usb_monitor_redraw_t *result)
{
    if (!display) return false;
    *result=redraw;
    return true;
}
