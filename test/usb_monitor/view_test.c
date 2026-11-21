// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "test_port.h"
#include "usb_monitor.h"
#ifdef KVM_USB_PICTURE_CHECK
#include "usb_jpeg_check.h"
#endif
#include "usb_monitor_chart.h"
#include "media_decode.h"
#include "usb_jpeg_decode.h"
#include "ui_test_env.h"
#include "ui_theme.h"
#include "sim_image.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>

struct test_task { pthread_t id; StaticSemaphore_t wake; void (*entry)(void *); };
static struct test_task job;
static bool launched, joined;
static atomic_bool hold, inside;
static atomic_int sample;
enum { COLOR_BARS, BLACK_PICTURE, LAST_PIXEL_GRAY, DECODE_FAILURE, CURSOR_OLD, CURSOR_NEW };
static unsigned launches, stops, homes;
static const char *snapshot;
static FILE *wire_file;
static unsigned allocations, fail_at, total_allocations;
static size_t heap_limit = SIZE_MAX, heap_used, heap_peak;
static unsigned native_decodes;
static bool reset_during_native_decode;
static bool feed_during_native_decode;
static bool vendor_unavailable;
static void native_pattern_frame(unsigned pattern);
static void pattern_frame(bool native, unsigned pattern, unsigned encoding);
static unsigned surface_width = 640, surface_height = 480;

static void *thread(void *unused){job.entry(unused);return NULL;}
int xTaskCreatePinnedToCoreWithCaps(void (*entry)(void *),const char *name,unsigned bytes,void *arg,unsigned priority,
                                   TaskHandle_t *task,int core,unsigned caps)
{
    assert(name && bytes && !arg && priority && core==1 && caps);
    if(launched) { assert(!pthread_join(job.id,NULL)); } else { test_semaphore(&job.wake,0); }
    launched=true;joined=false;job.entry=entry;*task=&job;
    assert(!pthread_create(&job.id,NULL,thread,arg));return pdPASS;
}
void ulTaskNotifyTake(int clear,unsigned wait){assert(clear);xSemaphoreTake(&job.wake,wait);}
void xTaskNotifyGive(TaskHandle_t t){assert(t==&job);xSemaphoreGive(&job.wake);}
void *heap_caps_malloc(size_t n,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(++total_allocations==fail_at || n > heap_limit - heap_used) return NULL;
    size_t *allocation = malloc(16 + n);
    assert(allocation);
    *allocation=n; heap_used += n;
    if(heap_peak < heap_used) heap_peak=heap_used;
    ++allocations;return (uint8_t *)allocation+16;
}
void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps)
{
    void *p=heap_caps_malloc(size,caps);
    assert(!p || !((uintptr_t)p % alignment));
    return p;
}
void heap_caps_free(void *p)
{
    size_t *allocation=(size_t *)((uint8_t *)p-16);
    assert(allocations && heap_used>=*allocation);
    --allocations;heap_used-=*allocation;free(allocation);
}
void kvm_media_zeroize(void *p,size_t n){memset(p,0,n);}
kvm_media_decoder_t *kvm_media_decoder_open(kvm_live_codec_t codec)
{
    assert(codec==KVM_LIVE_JPEG);return (kvm_media_decoder_t *)malloc(1);
}
void kvm_media_decoder_close(kvm_media_decoder_t *d){free(d);}
static void delay(void){struct timespec t={0,1000000};nanosleep(&t,NULL);}

#ifdef KVM_USB_PICTURE_CHECK
static atomic_uint reference_calls;
usb_jpeg_result_t usb_jpeg_check(const uint8_t *p, size_t n)
{
    assert(n >= 4 && p[0] == 0xff && p[1] == 0xd8);
    atomic_fetch_add(&reference_calls, 1);
    int fixture = n == 6 ? p[2] : COLOR_BARS;
    return fixture == BLACK_PICTURE ? USB_JPEG_ALL_BLACK : USB_JPEG_HAS_NONBLACK;
}
static void checked_picture(const char *decoded, const char *reference, unsigned sequence)
{
    usb_monitor_picture_t pair = usb_monitor_view_picture_check();
    assert(!strcmp(pair.decoded, decoded) && !strcmp(pair.reference, reference) && pair.sequence == sequence);
}
#endif

// The parallel decoder fixture is controllable at an MCU boundary. The actual
// 640x480 decoder is qualified separately with the ESP32-S3 ROM on QEMU.
static bool decoded_nonblack;
bool kvm_media_usb_monitor_has_nonblack(const kvm_media_decoder_t *decoder)
{
    assert(decoder);
    return decoded_nonblack;
}

bool kvm_media_usb_monitor_jpeg_fit(kvm_media_decoder_t *d, const uint8_t *p, size_t n,
                                    const kvm_media_target_t *t, unsigned width, unsigned height)
{
    assert(width && height);
    return kvm_media_usb_monitor_jpeg(d, p, n, t);
}

bool kvm_media_usb_monitor_jpeg(kvm_media_decoder_t *d,const uint8_t *p,size_t n,const kvm_media_target_t *t)
{
    assert(d && t->width==surface_width && t->height==surface_height && t->stride==surface_width);
    atomic_store(&inside,true);
    while(atomic_load(&hold)&&!t->cancelled(t->ctx)) delay();
    atomic_store(&inside,false);
    if(t->cancelled(t->ctx)||n<4||p[0]!=0xff||p[1]!=0xd8||p[n-2]!=0xff||p[n-1]!=0xd9) return false;
    // Short fixture records carry the picture and encoding separately. The
    // full offline chart always decodes to bars in this controlled decoder.
    const int fixture = n == 6 ? p[2] : COLOR_BARS;
    if (t->width == 1024) {
        ++native_decodes;
        if (feed_during_native_decode) {
            feed_during_native_decode=false;
            native_pattern_frame(LAST_PIXEL_GRAY);
            native_pattern_frame(BLACK_PICTURE);
            assert(p[2]==fixture);
        }
        if (reset_during_native_decode) {
            reset_during_native_decode=false;
            usb_monitor_reset();
            assert(t->cancelled(t->ctx));
            t->pixels[0]=0xffff;
            return false;
        }
    }
    if (fixture == DECODE_FAILURE) {
        memset(t->pixels, 0x55, t->width * t->height); // Corrupt half the RGB surface before rejection.
        return false;
    }
    decoded_nonblack = fixture != BLACK_PICTURE;
    if (fixture != COLOR_BARS) {
        memset(t->pixels, 0, t->height * t->stride * sizeof(*t->pixels));
        if (fixture == LAST_PIXEL_GRAY) t->pixels[(t->height - 1) * t->stride + t->width - 1] = 0x8410;
        if (fixture == CURSOR_OLD || fixture == CURSOR_NEW) {
            unsigned left=fixture==CURSOR_OLD ? 32 : 480, top=fixture==CURSOR_OLD ? 32 : 288;
            for (unsigned y=top;y<top+16;y++)
                for (unsigned x=left;x<left+16;x++) t->pixels[y*t->stride+x]=0xffff;
        }
        return true;
    }
    for(unsigned y=0;y<t->height;++y)
        for(unsigned x=0;x<t->width;++x) t->pixels[y*t->stride+x]=y<t->height/2?0xf800:0x001f;
    return true;
}
usb_jpeg_decode_result_t usb_jpeg_decode(uint8_t *input, size_t size, const kvm_media_target_t *target,
                                        size_t capacity, bool *nonblack, usb_jpeg_damage_t *tracking)
{
    assert(capacity >= target->stride * target->height * sizeof(*target->pixels));
    if (vendor_unavailable) {
        memset(input, 0xa5, size);
        return USB_JPEG_UNAVAILABLE;
    }
    kvm_media_decoder_t *fixture=(kvm_media_decoder_t *)target;
    uint16_t *previous = NULL;
    if (tracking && tracking->previous == target->pixels) {
        const size_t bytes = target->width * target->height * sizeof(*previous);
        previous = malloc(bytes);
        assert(previous);
        memcpy(previous, target->pixels, bytes);
    }
    bool ok=kvm_media_usb_monitor_jpeg(fixture,input,size,target);
    if (ok && tracking) {
        const uint16_t *saved = tracking->previous;
        if (previous) tracking->previous = previous;
        for (unsigned top=0;top<target->height;top+=16)
            usb_jpeg_damage_band(tracking,target->pixels+top*target->stride,NULL,target->width,
                                 top,LV_MIN(16u,target->height-top));
        tracking->previous = saved;
    }
    free(previous);
    // The vendor's public input type is mutable, even on a failed decode.
    memset(input,0xa5,size);
    if (!ok) return USB_JPEG_INVALID;
    *nonblack=kvm_media_usb_monitor_has_nonblack(fixture);
    return USB_JPEG_OK;
}

#ifdef KVM_USB_PICTURE_CHECK
usb_jpeg_result_t usb_jpeg_check_rom(const uint8_t *input,size_t size,const kvm_media_target_t *target)
{
    assert(target);
    return usb_jpeg_check(input,size);
}
#endif

static bool personality(bool on){if(on)++launches;else ++stops;return true;}
static void home(void){++homes;}
static void pump(unsigned expected)
{
    for(unsigned t=0;t<2000;++t){lv_test_wait(20);if(usb_monitor_stats().presented==expected) return;delay();}
    assert(false);
}
static void wait_unchanged(unsigned expected)
{
    for (unsigned t=0; t<2000; ++t) {
        lv_test_wait(20);
        if (usb_monitor_view_redraw().unchanged == expected) return;
        delay();
    }
    assert(false);
}
static void wait_duplicate(unsigned expected)
{
    for (unsigned t = 0; t < 2000; ++t) {
        lv_test_wait(20);
        if (usb_monitor_stats().duplicate_jpegs == expected) return;
        delay();
    }
    assert(false);
}
static void hit(int x,int y){lv_test_mouse_click_at(x,y);lv_test_wait(60);}
static void save_bytes(const uint8_t *data,size_t n){assert(fwrite(data,1,n,wire_file)==n);}
static lv_obj_t *find_label(lv_obj_t *root, const char *value)
{
    for (unsigned i = 0; i < lv_obj_get_child_count(root); ++i) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (lv_obj_check_type(child, &lv_label_class) && !strcmp(lv_label_get_text(child), value))
            return child;
        lv_obj_t *nested = find_label(child, value);
        if (nested) return nested;
    }
    return NULL;
}
static lv_obj_t *screen_label(const char *value){return find_label(lv_screen_active(),value);}

static void check_guide(lv_obj_t *label)
{
    lv_area_t text, card;
    assert(label && lv_obj_is_visible(label));
    lv_obj_update_layout(label);
    lv_obj_get_coords(label,&text);
    lv_obj_get_content_coords(lv_obj_get_parent(label),&card);
    assert(text.x1 >= card.x1 && text.x2 <= card.x2 && text.y1 >= card.y1 && text.y2 <= card.y2);
}

static void fail_picture(const char *caption)
{
    unsigned expected=usb_monitor_stats().decode_errors+1;
    pattern_frame(false, DECODE_FAILURE, 0);
    for(unsigned i=0;i<2000 && !screen_label(caption);++i) {lv_test_wait(20);delay();}
    assert(usb_monitor_stats().decode_errors==expected && screen_label(caption));
}
static void check_first_flush(lv_event_t *event)
{
    lv_obj_t *image = lv_obj_get_child(lv_screen_active(), -1);
    if (!lv_obj_is_hidden(image) && !usb_monitor_stats().presented) {
        assert(screen_label("Waiting for the first picture"));
        bool *seen = lv_event_get_user_data(event);
        *seen = true;
    }
}

static void check_picture_diagnostic(void)
{
    assert(!strcmp(usb_monitor_view_decoded_picture(), "absent"));
    assert(usb_monitor_view_open(home, personality));
    assert(!strcmp(usb_monitor_view_decoded_picture(), "absent"));
    usb_monitor_view_set_link(true,false);
    fail_picture("USB picture could not be decoded");
    check_guide(screen_label("Waiting for a new picture from Windows.\nTry moving a window on the KVMpad display."));
    assert(usb_monitor_stats().presented==0);
    usb_monitor_reset();lv_test_wait(80);
    assert(screen_label("USB connected  /  Waiting for a picture"));
#ifdef KVM_USB_PICTURE_CHECK
    assert(!atomic_load(&reference_calls));
    usb_monitor_enable_picture_check();
    checked_picture("absent", "absent", 0);
#endif
    pattern_frame(false, BLACK_PICTURE, 0);
    pump(1);
    assert(!strcmp(usb_monitor_view_decoded_picture(), "all_black"));
    assert(screen_label("Windows desktop  /  640 x 480"));
    usb_monitor_view_set_link(false,true);
    lv_test_wait(1100);
    assert(screen_label("USB suspended  /  Last picture held"));
    usb_monitor_view_set_link(true,false);
    assert(screen_label("Last USB picture is black") && usb_monitor_stats().presented == 1);
    lv_obj_t *hint=screen_label("Move a window onto the KVMpad display to check the picture.");
    lv_area_t hint_area, movie_area;
    assert(hint && lv_obj_is_visible(hint));
    lv_obj_update_layout(hint);
    lv_obj_get_coords(hint,&hint_area);
    lv_obj_get_coords(lv_obj_get_child(lv_screen_active(),-1),&movie_area);
    assert(hint_area.y1>movie_area.y2 && hint_area.y2<600);
    sim_image_t black;
    assert(sim_image_render(lv_screen_active(),&black));
    assert(sim_image_write_png("usb-monitor-black.png",&black));
    for (unsigned y=84;y<564;++y) for (unsigned x=192;x<832;++x) {
        const uint8_t *pixel=black.rgb+3*(y*black.w+x);
        assert(!(pixel[0]|pixel[1]|pixel[2]));
    }
    sim_image_free(&black);
    usb_monitor_view_set_link(false,true);
    assert(!lv_obj_is_visible(hint));
    usb_monitor_view_set_link(true,false);
    assert(lv_obj_is_visible(hint));
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("all_black", "all_black", 1);
#endif
    pattern_frame(false, LAST_PIXEL_GRAY, 0);
    pump(2);
    assert(!lv_obj_is_visible(hint));
    assert(!strcmp(usb_monitor_view_decoded_picture(), "has_nonblack"));
    assert(screen_label("Windows desktop  /  640 x 480"));
    lv_test_wait(1100);
    assert(screen_label("Windows desktop  /  640 x 480") && usb_monitor_stats().presented == 2);
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("has_nonblack", "has_nonblack", 2);
#endif
    assert(usb_monitor_stats().decoded == 2 && usb_monitor_stats().presented == 2);
    fail_picture("USB picture error  /  Last picture held");
    assert(usb_monitor_stats().presented==2 && !strcmp(usb_monitor_view_decoded_picture(),"has_nonblack"));
    usb_monitor_view_set_link(false,true);
    assert(screen_label("USB suspended  /  Last picture held"));
    usb_monitor_view_set_link(true,false);
    assert(screen_label("USB picture error  /  Last picture held"));
    pattern_frame(false, BLACK_PICTURE, 0);
    pump(3);
    assert(screen_label("Windows desktop  /  640 x 480"));
    lv_test_wait(1100);
    assert(screen_label("Last USB picture is black"));
    usb_monitor_view_set_link(false,false);
    usb_monitor_reset();
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("absent", "absent", 0);
#endif
    lv_test_wait(80);
    assert(!strcmp(usb_monitor_view_decoded_picture(), "absent"));
    assert(screen_label("USB video stopped"));
    usb_monitor_view_close();
    assert(!strcmp(usb_monitor_view_decoded_picture(), "absent"));
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("absent", "absent", 0);
    assert(atomic_load(&reference_calls) == 3);
#endif
}

static void check_continuous_black(void)
{
    assert(usb_monitor_view_open(home, personality));
    usb_monitor_view_set_link(true, false);
    const char *black = "Last USB picture is black";
    const char *desktop = "Windows desktop  /  640 x 480";
    uint32_t first_black = 0;
    for (unsigned i = 1; i <= 22; ++i) {
        // A single colored frame breaks the first black interval.
        pattern_frame(false, i == 5 ? LAST_PIXEL_GRAY : BLACK_PICTURE, 0);
        if (i == 1 || i == 5 || i == 6) pump(i == 1 ? 1 : i - 3);
        else wait_duplicate(i < 5 ? i - 1 : i - 3);
        if (i == 1 || i == 6) first_black = lv_tick_get();
        lv_test_wait(70);
        if (lv_tick_elaps(first_black) < 900) assert(screen_label(desktop));
        if (i > 6 && lv_tick_elaps(first_black) > 1050) assert(screen_label(black));
    }
    assert(screen_label(black));
    pattern_frame(false, LAST_PIXEL_GRAY, 0);
    pump(4);
    assert(usb_monitor_stats().decoded == 4 && usb_monitor_stats().duplicate_jpegs == 19);
    assert(screen_label(desktop));
    usb_monitor_view_close();
    puts("PASS continuous black USB pictures, transient black, and single colored pixel recovery");
}

static void pattern_frame(bool native, unsigned pattern, unsigned encoding)
{
    uint8_t record[] = {0,0,3,0,0,0,0,0,0x80,2,0xe0,1,0,24,0,0,0xff,0xd8,0,0,0xff,0xd9};
    if (native) {
        record[8] = 0; record[9] = 4;
        record[10] = 0x58; record[11] = 2;
    }
    record[18]=(uint8_t)pattern;
    record[19]=(uint8_t)encoding;
    usb_monitor_feed(record, 11);
    usb_monitor_feed(record + 11, sizeof(record) - 11);
}

static void native_pattern_frame(unsigned pattern) { pattern_frame(true, pattern, 0); }
static void native_frame(void) { native_pattern_frame((unsigned)atomic_load(&sample)); }

static void watch_corner(lv_event_t *event)
{
    const lv_area_t *a = lv_event_get_invalidated_area(event);
    int x = surface_width == 640 ? 831 : 1023;
    int y = surface_height == 480 ? 563 : 599;
    if (a->x1 == x && a->x2 == x && a->y1 == y && a->y2 == y)
        ++*(unsigned *)lv_event_get_user_data(event);
}

static void check_redraw(bool native)
{
    surface_width = native ? 1024 : 640;
    surface_height = native ? 600 : 480;
    assert(usb_monitor_view_open_mode(home, personality, native));
    unsigned corners = 0;
    lv_display_t *display = lv_display_get_default();
    lv_display_add_event_cb(display, watch_corner, LV_EVENT_INVALIDATE_AREA, &corners);
    for (unsigned i = 0; i < 4; ++i) {
        pattern_frame(native, (i == 1 || i == 2) ? LAST_PIXEL_GRAY : BLACK_PICTURE, i == 2);
        if (i == 2) wait_unchanged(1);
        else pump(i == 3 ? 3 : i + 1);
        usb_monitor_redraw_t r = usb_monitor_view_redraw();
        assert(r.full == 1 && r.last_pixels == (i == 0 ? surface_width*surface_height : (i == 2 ? 0u : 1u)));
    }
    usb_monitor_redraw_t r = usb_monitor_view_redraw();
    assert(corners == 2 && r.partial == 2 && r.unchanged == 1 && usb_monitor_stats().presented == 3);
    pattern_frame(native, BLACK_PICTURE, 0);
    wait_duplicate(1);
    assert(usb_monitor_stats().decoded == 4 && usb_monitor_stats().presented == 3 &&
           usb_monitor_view_redraw().unchanged == 1 && corners == 2);
    lv_display_remove_event_cb_with_user_data(display, watch_corner, &corners);
    usb_monitor_view_close();
    surface_width = 640; surface_height = 480;
}

enum { PANEL_WIDTH=1024, PANEL_HEIGHT=600 };
static LV_ATTRIBUTE_MEM_ALIGN uint16_t motion_strip[12*PANEL_WIDTH];
static uint16_t motion_screen[PANEL_WIDTH*PANEL_HEIGHT], motion_reference[PANEL_WIDTH*PANEL_HEIGHT];
static unsigned flushed_pixels;

static void motion_flush(lv_display_t *display,const lv_area_t *a,uint8_t *data)
{
    assert(a->x1>=0 && a->x2<PANEL_WIDTH && a->y1>=0 && a->y2<PANEL_HEIGHT);
    const lv_draw_buf_t *buf=lv_display_get_buf_active(display);
    assert(buf->header.cf==LV_COLOR_FORMAT_RGB565);
    unsigned bytes=2u*(unsigned)lv_area_get_width(a);
    assert(buf->header.stride>=bytes);
    for (int y=a->y1;y<=a->y2;y++,data+=buf->header.stride)
        memcpy(&motion_screen[y*PANEL_WIDTH+a->x1],data,bytes);
    flushed_pixels+=lv_area_get_size(a);
    lv_display_flush_ready(display);
}

static void check_cursor_move(bool native)
{
    lv_display_t *display=lv_display_get_default();
    lv_draw_buf_t *original=lv_display_get_buf_active(display);
    lv_display_flush_cb_t flush=lv_display_get_flush_cb(display);
    assert(lv_display_get_render_mode(display)==LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_buffers(display,motion_strip,NULL,sizeof(motion_strip),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,motion_flush);
    surface_width=native ? 1024 : 640;
    surface_height=native ? 600 : 480;
    assert(usb_monitor_view_open_mode(home,personality,native));
    pattern_frame(native,CURSOR_OLD,0);
    pump(1);
    lv_test_wait(100);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_invalidate(screen);lv_refr_now(display);
    const int left=native ? 0 : 192, top=native ? 0 : 84;
    assert(motion_screen[(top+32)*PANEL_WIDTH+left+32]==0xffff);
    assert(!motion_screen[(top+288)*PANEL_WIDTH+left+480]);
    flushed_pixels=0;
    pattern_frame(native,CURSOR_NEW,0);
    pump(2);
    unsigned split_pixels=flushed_pixels;
    usb_monitor_redraw_t stats=usb_monitor_view_redraw();
    assert(stats.split==1 && stats.last_pixels==512 && stats.partial==1);
    assert(split_pixels>=512 && split_pixels<=2*18*18);
    assert(!motion_screen[(top+32)*PANEL_WIDTH+left+32]);
    assert(motion_screen[(top+288)*PANEL_WIDTH+left+480]==0xffff);
    memcpy(motion_reference,motion_screen,sizeof(motion_reference));

    // Render the previous algorithm's single bounding rectangle through the same flush path.
    lv_area_t united={left+32,top+32,left+495,top+303};
    flushed_pixels=0;
    lv_obj_invalidate_area(screen,&united);lv_refr_now(display);
    unsigned single_pixels=flushed_pixels;
    assert(single_pixels>=464*272 && !memcmp(motion_screen,motion_reference,sizeof(motion_screen)));
    flushed_pixels=0;
    lv_obj_invalidate(screen);lv_refr_now(display);
    assert(flushed_pixels==PANEL_WIDTH*PANEL_HEIGHT && !memcmp(motion_screen,motion_reference,sizeof(motion_screen)));
    printf("PASS MOTION %ux%u cursor jump: split=%u bounding=%u full=%u RGB565 mismatch=0\n",
           surface_width,surface_height,split_pixels,single_pixels,flushed_pixels);
    usb_monitor_view_close();
    lv_display_set_flush_cb(display,flush);
    lv_display_set_draw_buffers(display,original,NULL);
    lv_display_set_render_mode(display,LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(display);
    surface_width=640;surface_height=480;
}

static void check_native(lv_obj_t *prior)
{
    for (unsigned allocation = 1; allocation <= 3; ++allocation) {
        const unsigned before = launches;
        fail_at = total_allocations + allocation;
        assert(!usb_monitor_view_open_mode(home, personality, true));
        assert(!usb_monitor_view_active() && lv_screen_active() == prior && !allocations && launches == before);
    }
    fail_at = 0;
    surface_width = 1024;
    surface_height = 600;
    // Free PSRAM observed before native startup on the 7B. The previous
    // two RGB surfaces alone (2,457,600 B) exceeded the entire budget.
    heap_limit=2220916;
    heap_peak=0;
    assert(usb_monitor_view_open_mode(home, personality, true));
    // The cache shares the padded RGB565 allocation. Charge no separate cache buffer.
    assert(usb_monitor_stats().input_slots == 3 && heap_peak==1024*608*2+5*96*1024+32768);
#ifdef KVM_USB_PICTURE_CHECK
    usb_monitor_enable_picture_check();
#endif
    assert(screen_label("Tap the desktop to show Home."));
    unsigned before = homes;
    atomic_store(&sample, COLOR_BARS);
    usb_monitor_test_chart(usb_monitor_feed);
    assert(usb_monitor_stats().queued == 0);
    native_frame();
    pump(1);
    assert(native_decodes==1 && usb_monitor_stats().accelerated==1);
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("has_nonblack","has_nonblack",1);
#endif
    sim_image_t shot;
    assert(sim_image_render(lv_screen_active(), &shot) && shot.w == 1024 && shot.h == 600);
    for (unsigned y = 0; y < shot.h; ++y) for (unsigned x = 0; x < shot.w; ++x) {
        const uint8_t *p = shot.rgb + 3 * (y * shot.w + x);
        assert(p[0] == (y < 300 ? 255 : 0) && !p[1] && p[2] == (y < 300 ? 0 : 255));
    }
    assert(sim_image_write_png("usb-monitor-native.png", &shot));
    atomic_store(&sample, DECODE_FAILURE);
    native_frame();
    lv_test_wait(120);
    assert(native_decodes==3 && usb_monitor_stats().decode_errors==1 &&
           usb_monitor_stats().decoded==1 && usb_monitor_stats().accelerated==1 && usb_monitor_stats().presented==1);
    sim_image_t restored;
    assert(sim_image_render(lv_screen_active(), &restored) && restored.w==shot.w && restored.h==shot.h);
    assert(!memcmp(shot.rgb, restored.rgb, (size_t)shot.w*shot.h*3));
    sim_image_free(&restored);
#ifdef KVM_USB_PICTURE_CHECK
    checked_picture("has_nonblack","has_nonblack",1);
#endif
    atomic_store(&sample, COLOR_BARS);
    sim_image_free(&shot);
    lv_obj_t *button = lv_obj_get_parent(screen_label("Home"));
    assert(!lv_obj_is_visible(button));
    hit(60,32); // The first tap at Home's location reveals it without leaving.
    assert(lv_obj_is_visible(button) && homes == before);
    native_frame(); wait_duplicate(1);
    assert(native_decodes == 3 && usb_monitor_stats().decoded == 1);
    assert(lv_obj_is_visible(button));
    lv_test_wait(3200);
    assert(!lv_obj_is_visible(button));
    reset_during_native_decode=true;
    pattern_frame(true, COLOR_BARS, 1); lv_test_wait(80);
    assert(!reset_during_native_decode && usb_monitor_stats().presented==1 && usb_monitor_stats().decode_errors==1);
    assert(lv_obj_is_visible(button) && !strcmp(usb_monitor_view_decoded_picture(),"absent"));
    atomic_store(&sample, LAST_PIXEL_GRAY);
    native_frame(); pump(2);
    assert(!strcmp(usb_monitor_view_decoded_picture(),"has_nonblack") && !lv_obj_is_visible(button));
    const usb_monitor_stats_t before_burst=usb_monitor_stats();
    feed_during_native_decode=true;
    native_pattern_frame(COLOR_BARS);
    for(unsigned attempt=0; attempt<20 && usb_monitor_stats().decoded<before_burst.decoded+2; ++attempt)
        lv_test_wait(40);
    const usb_monitor_stats_t after_burst=usb_monitor_stats();
    assert(!feed_during_native_decode && after_burst.queued==before_burst.queued+3 &&
           after_burst.decoded==before_burst.decoded+2 && after_burst.superseded==before_burst.superseded+1 &&
           after_burst.busy_drops==before_burst.busy_drops && !strcmp(usb_monitor_view_decoded_picture(),"all_black"));
    vendor_unavailable=true;
    native_pattern_frame(COLOR_BARS);
    lv_test_wait(100);
    vendor_unavailable=false;
    assert(usb_monitor_stats().decoded==after_burst.decoded+1 &&
           usb_monitor_stats().accelerated==after_burst.accelerated && !strcmp(usb_monitor_view_decoded_picture(),"has_nonblack"));
    hit(1020,595);
    assert(lv_obj_is_visible(button));
    hit(60,32);
    assert(homes == before + 1 && !usb_monitor_view_active() && lv_screen_active() == prior && !allocations);
    printf("PASS 7B budget %zu B, native RGB/JPEG heap peak %zu B, byte-identical rollback after partial decode\n",
           heap_limit,heap_peak);
    assert(!heap_used);
    heap_limit=1024*608*2+2*96*1024;
    heap_peak=0;
    assert(usb_monitor_view_open_mode(home,personality,true));
    assert(usb_monitor_stats().input_slots==1 && heap_peak==heap_limit);
    atomic_store(&sample,BLACK_PICTURE);
    native_frame(); pump(1);
    assert(usb_monitor_stats().accelerated==0); // Only the ROM workspace fits.
    assert(!strcmp(usb_monitor_view_decoded_picture(),"all_black"));
    atomic_store(&sample,DECODE_FAILURE);
    native_frame();lv_test_wait(80);
    assert(usb_monitor_stats().decode_errors==1 && !strcmp(usb_monitor_view_decoded_picture(),"all_black"));
    atomic_store(&sample,LAST_PIXEL_GRAY);
    native_frame(); pump(2);
    assert(!strcmp(usb_monitor_view_decoded_picture(),"has_nonblack"));
    usb_monitor_view_close();
    assert(!heap_used);
    --heap_limit;
    assert(!usb_monitor_view_open_mode(home,personality,true) && !allocations);
    heap_limit=SIZE_MAX;
    surface_width = 640;
    surface_height = 480;
    assert(usb_monitor_view_open(home, personality));
    native_frame(); assert(usb_monitor_stats().queued == 0);
    usb_monitor_view_close();
    puts("PASS native 614400 RGB565 pixels, exclusive wire dimensions across sessions and reset, allocation rollback, local Home reveal and timeout");
}

static unsigned profile_requests, reject_profiles;
static bool change_profile(bool native)
{
    ++profile_requests;
    if (reject_profiles) { --reject_profiles; return false; }
    surface_width=native ? 1024 : 640;
    surface_height=native ? 600 : 480;
    return true;
}

static void check_running_mode_change(void)
{
    assert(!usb_monitor_view_change_mode(true,change_profile));
    lv_obj_t *prior=lv_screen_active();
    assert(usb_monitor_view_open(home,personality));
    lv_obj_t *view=lv_screen_active();
    assert(usb_monitor_view_has_link(personality) && !usb_monitor_view_has_link(change_profile));
    assert(usb_monitor_view_change_mode(false,change_profile) && !profile_requests);
    pattern_frame(false,CURSOR_OLD,0);pump(1);
    assert(usb_monitor_view_change_mode(true,change_profile) && profile_requests==1);
    assert(lv_screen_active()==view && !strcmp(usb_monitor_view_decoded_picture(),"absent"));
    pattern_frame(false,CURSOR_OLD,0);
    assert(!usb_monitor_stats().queued);
    pattern_frame(true,CURSOR_NEW,0);pump(1);
    assert(screen_label("Windows desktop  /  1024 x 600"));
    assert(usb_monitor_view_change_mode(true,change_profile) && profile_requests==1 &&
           !strcmp(usb_monitor_view_decoded_picture(),"has_nonblack"));
    fail_at=total_allocations+1;
    assert(!usb_monitor_view_change_mode(false,change_profile) && profile_requests==1);
    pattern_frame(true,CURSOR_OLD,0);pump(1);
    reject_profiles=1;
    assert(!usb_monitor_view_change_mode(false,change_profile) && profile_requests==3);
    pattern_frame(true,CURSOR_NEW,0);pump(1);
    assert(usb_monitor_view_change_mode(false,change_profile));
    pattern_frame(false,CURSOR_OLD,0);pump(1);
    assert(screen_label("Windows desktop  /  640 x 480") && lv_screen_active()==view);
    reject_profiles=2;
    assert(!usb_monitor_view_change_mode(true,change_profile));
    assert(!usb_monitor_view_active() && !allocations && lv_screen_active()==prior);
    puts("PASS live standalone VGA/native change, same screen, matching wire sizes, idempotence, allocation and USB rollback, clean exit on failed restoration");
}

typedef struct {
    bool invalidated, flushed;
    uint32_t invalid_tick, flush_tick;
} refresh_probe_t;

static void watch_refresh(lv_event_t *event)
{
    refresh_probe_t *probe=lv_event_get_user_data(event);
    if (lv_event_get_code(event)==LV_EVENT_INVALIDATE_AREA && !probe->invalidated) {
        probe->invalidated=true;
        probe->invalid_tick=lv_tick_get();
    }
    if (lv_event_get_code(event)==LV_EVENT_FLUSH_START && !probe->flushed) {
        probe->flushed=true;
        probe->flush_tick=lv_tick_get();
    }
}

static void run_timers(unsigned ticks)
{
    // lv_test_wait also calls lv_refr_now, which would hide a missing display wake-up.
    while (ticks--) { lv_tick_inc(1);lv_timer_handler(); }
}

static void stage_pattern(bool native,unsigned pattern,unsigned encoding,unsigned decoded)
{
    pattern_frame(native,pattern,encoding);
    if (!native) { // Allow the VGA worker to complete without advancing the LVGL clock.
        for (unsigned i=0;i<2000 && usb_monitor_stats().decoded!=decoded;i++) delay();
        assert(usb_monitor_stats().decoded==decoded);
    }
}

static void check_refresh_scheduling(bool native)
{
    surface_width=native ? 1024 : 640;surface_height=native ? 600 : 480;
    assert(usb_monitor_view_open_mode(home,personality,native));
    lv_test_wait(80);
    lv_display_t *display=lv_display_get_default();
    lv_timer_t *timer=lv_display_get_refr_timer(display);
    lv_timer_set_period(timer,1000);
    refresh_probe_t probe;
    assert(lv_display_add_event_cb(display,watch_refresh,LV_EVENT_INVALIDATE_AREA,&probe));
    assert(lv_display_add_event_cb(display,watch_refresh,LV_EVENT_FLUSH_START,&probe));
    unsigned selection[2], lag[2];
    for (unsigned i=0;i<2;i++) {
        lv_timer_reset(timer);
        probe=(refresh_probe_t){0};
        stage_pattern(native,i ? CURSOR_NEW : CURSOR_OLD,0,i+1);
        uint32_t staged=lv_tick_get();
        // Stage the next movement just after this selection, near the worst timer phase.
        for (unsigned tick=0;tick<32 && !probe.flushed;tick++) run_timers(1);
        assert(probe.invalidated && probe.flushed && usb_monitor_stats().presented==i+1);
        selection[i]=probe.invalid_tick-staged;
        assert(selection[i]<=8);
        lag[i]=probe.flush_tick-probe.invalid_tick;
        assert(lag[i]<=1);
        lv_refr_now(display); // Finish unrelated caption changes before the next trial.
    }
    // An unrelated invalidation makes an incorrect wake on an idle poll or equal
    // picture observable as an early physical flush, even without monitor damage.
    lv_timer_reset(timer);
    probe=(refresh_probe_t){0};
    lv_obj_invalidate(lv_screen_active());
    run_timers(75);
    assert(probe.invalidated && !probe.flushed);
    stage_pattern(native,CURSOR_NEW,1,3); // Different JPEG bytes, identical RGB565.
    run_timers(75);
    assert(usb_monitor_view_redraw().unchanged==1 && usb_monitor_stats().presented==2 && !probe.flushed);
    run_timers(849);
    assert(!probe.flushed);
    run_timers(1);
    assert(probe.flushed && probe.flush_tick-probe.invalid_tick==1000);
    lv_display_remove_event_cb_with_user_data(display,watch_refresh,&probe);
    lv_timer_set_period(timer,LV_DEF_REFR_PERIOD);
    usb_monitor_view_close();
    surface_width=640;surface_height=480;
    printf("PASS %s refresh scheduling: selected in %u/%u ms, flushed in %u/%u ms from selection; idle/equal preserved 1000 ms display period\n",
           native ? "native" : "VGA",selection[0],selection[1],lag[0],lag[1]);
}

int main(int argc,char **argv)
{
    assert(argc==3); snapshot=argv[1];
    wire_file=fopen(argv[2],"wb");assert(wire_file);usb_monitor_test_chart(save_bytes);assert(!fclose(wire_file));
    size_t base=ui_test_env_start(); assert(ui_theme_init());lv_obj_t *prior=lv_screen_active();
    fail_at=2;assert(!usb_monitor_view_open(home,personality));assert(allocations==0 && launches==0);
    for(unsigned round=0;round<12;++round) {
        ui_theme_set_control_palette((ui_control_palette_t)(round % UI_PALETTE_COUNT));
        assert(usb_monitor_view_open(home,personality) && usb_monitor_view_active());
        assert(!usb_monitor_view_open(home,personality));lv_test_wait(60);
        lv_obj_t *guide = screen_label("Connect the PC to the USB data port.\nIn Windows, press Win+P and choose Extend.");
        check_guide(guide);
        assert(screen_label("Waiting for the first picture"));
        usb_monitor_view_set_link(true,false);
        assert(screen_label("USB connected  /  Waiting for a picture"));
        check_guide(screen_label("In Windows, press Win+P and choose Extend.\nMove a window onto the KVMpad display."));
        usb_monitor_view_set_link(false,true);
        assert(screen_label("USB suspended"));
        check_guide(screen_label("Windows has paused the USB link.\nVideo resumes when the link wakes."));
        usb_monitor_view_set_link(false,false);
        if (round < UI_PALETTE_COUNT) {
            char path[40];
            snprintf(path, sizeof(path), "usb-monitor-wait-%u.png", round);
            sim_image_t waiting;
            assert(sim_image_render(lv_screen_active(), &waiting));
            assert(sim_image_write_png(path, &waiting));
            sim_image_free(&waiting);
        }
        atomic_store(&hold,true);usb_monitor_test_chart(usb_monitor_feed);
        for(unsigned i=0;i<2000 && !atomic_load(&inside);++i) delay();
        assert(atomic_load(&inside));
        usb_monitor_test_chart(usb_monitor_feed); // One complete input can wait behind the worker.
        usb_monitor_test_chart(usb_monitor_feed); // The third JPEG slot supersedes the queued picture.
        usb_monitor_reset();atomic_store(&hold,false);
        for(unsigned i=0;i<2000 && atomic_load(&inside);++i) delay();
        assert(!atomic_load(&inside));lv_test_wait(60);assert(!usb_monitor_stats().presented);
        assert(screen_label("Waiting for the first picture") && lv_obj_is_visible(guide));
        bool observed_first_flush = false;
        lv_display_t *display = lv_obj_get_display(guide);
        lv_display_add_event_cb(display,check_first_flush,LV_EVENT_FLUSH_START,&observed_first_flush);
        usb_monitor_test_chart(usb_monitor_feed);
        pump(1);
        lv_display_remove_event_cb_with_user_data(display,check_first_flush,&observed_first_flush);
        usb_monitor_stats_t s=usb_monitor_stats();
        assert(observed_first_flush && s.decoded==1 && s.presented==1 && s.busy_drops==0 && s.superseded==1);
        assert(!lv_obj_is_visible(guide) && screen_label("Windows desktop  /  640 x 480"));
        if(round < UI_PALETTE_COUNT) {
            sim_image_t img;assert(sim_image_render(lv_screen_active(),&img));
            assert(img.w==1024 && img.h==600);
            for(unsigned y=0;y<480;y++) for(unsigned x=0;x<640;x++) {
                const uint8_t *rgb=img.rgb+3*((y+84)*img.w+x+192);
                assert(rgb[0]==(y<240?255:0)&&!rgb[1]&&rgb[2]==(y<240?0:255));
            }
            char path[40];
            snprintf(path, sizeof(path), "usb-monitor-active-%u.png", round);
            assert(sim_image_write_png(round ? path : snapshot,&img));sim_image_free(&img);
        }
        lv_obj_t *image=lv_obj_get_child(lv_screen_active(),-1);
        lv_test_fast_forward(120000);
        assert(screen_label("Windows desktop  /  640 x 480") && !lv_obj_is_hidden(image));
        assert(usb_monitor_stats().presented==1);
        usb_monitor_view_set_link(false,true);
        lv_test_fast_forward(120000);
        assert(screen_label("USB suspended  /  Last picture held") && !lv_obj_is_visible(guide));
        assert(usb_monitor_stats().presented==1 && !strcmp(usb_monitor_view_decoded_picture(),"has_nonblack"));
        if(round < UI_PALETTE_COUNT) {
            sim_image_t suspended;
            assert(sim_image_render(lv_screen_active(),&suspended));
            for (unsigned y=0;y<480;++y) for (unsigned x=0;x<640;++x) {
                const uint8_t *pixel=suspended.rgb+3*((y+84)*suspended.w+x+192);
                assert(pixel[0]==(y<240?255:0) && !pixel[1] && pixel[2]==(y<240?0:255));
            }
            char path[40];snprintf(path,sizeof(path),"usb-monitor-suspended-%u.png",round);
            assert(sim_image_write_png(path,&suspended));sim_image_free(&suspended);
        }
        usb_monitor_view_set_link(true,false);
        assert(screen_label("Windows desktop  /  640 x 480") && usb_monitor_stats().presented==1);
        usb_monitor_view_set_link(false,false);
        usb_monitor_reset();lv_test_wait(80);
        assert(lv_obj_is_hidden(image));
        assert(lv_obj_is_visible(guide) && screen_label("USB video stopped"));
        usb_monitor_reset();lv_test_wait(80);
        assert(screen_label("USB video stopped"));
        usb_monitor_test_chart(usb_monitor_feed);pump(2);
        assert(screen_label("Windows desktop  /  640 x 480") && !lv_obj_is_hidden(image));
        if (round & 1) usb_monitor_view_set_link(false,true);
        hit(60,32);assert(lv_screen_active()==prior && !usb_monitor_view_active());
        assert(homes==round+1 && stops==launches && allocations==0);
        usb_monitor_view_set_link(true,false);
        usb_monitor_test_chart(usb_monitor_feed);
    }
    check_picture_diagnostic();
    check_continuous_black();
    check_native(prior);
    check_redraw(false);
    check_redraw(true);
    check_cursor_move(false);
    check_cursor_move(true);
    check_running_mode_change();
    check_refresh_scheduling(false);
    check_refresh_scheduling(true);
    if(launched){assert(!pthread_join(job.id,NULL));joined=true;}
    lv_test_wait(80);ui_theme_deinit();ui_test_env_stop(base);
    assert(joined);
    puts("PASS 12 open/reset/Home cycles, first flush, two-minute static desktop, USB video stop and resume, queued frame replacement, 307200 rendered pixels, allocation rollback");
    puts("PASS on-demand decoded picture: absent, black, final grayscale pixel, USB reset, closed view");
    puts("PASS configured, suspended and resumed captions, guide bounds, retained 640x480 desktop in all three palettes");
}
