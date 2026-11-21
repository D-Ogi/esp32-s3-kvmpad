// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "chart_bench.h"
#include "pack_chart_view.h"
#include "pack_autopilot_view.h"
#include "pack_store.h"
#include "pack_view.h"
#include "ui_platform.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { FIT_FRAMES=5, PAN_FRAMES=12, PINCH_FRAMES=12,
       MAX_ZOOM_SETUP=FIT_FRAMES+1+PAN_FRAMES+PINCH_FRAMES, FRAME_COUNT=MAX_ZOOM_SETUP+1+PAN_FRAMES };
enum { PAGE_CYCLES=4, PAGE_STEPS=4, PAGE_FRAMES=1+PAGE_CYCLES*PAGE_STEPS };
typedef struct {
    char path[PACK_PATH_CAP];
    SemaphoreHandle_t done;
    esp_err_t result;
    lv_obj_t *screen;
    unsigned frame, frame_count, partial_frames, target_page, pan_zoom, band_width;
    bool navigation;
    bool flushed, failed;
    atomic_bool complete;
    int64_t started, flush_started;
    uint32_t elapsed[FRAME_COUNT];
    uint32_t flush_us[FRAME_COUNT];
    uint32_t page_action_us[PAGE_FRAMES];
} chart_bench_t;

static void draw_next(lv_timer_t *timer)
{
    chart_bench_t *b=lv_timer_get_user_data(timer);
    if (b->started || b->frame==b->frame_count || b->failed) return;
    if (lv_screen_active()!=b->screen) { b->failed=true; atomic_store(&b->complete,true); return; }
    b->flushed=false;
    b->started=esp_timer_get_time();
    if (b->navigation) {
        const int page_order[PAGE_STEPS]={-1,(int)b->target_page,-1,0};
        if (!b->frame) lv_obj_invalidate(b->screen);
        else if (!pack_view_bench_navigate(page_order[(b->frame-1)%PAGE_STEPS])) {
            b->failed=true;
            atomic_store(&b->complete,true);
        }
        b->page_action_us[b->frame]=(uint32_t)(esp_timer_get_time()-b->started);
        return;
    }
    if (b->frame<FIT_FRAMES) {
        lv_obj_invalidate(b->screen);
        return;
    }
    // These positions go only to the benchmark's ChartViewer, on its own screen.
    int middle=300, span=200;
    if (b->frame==FIT_FRAMES || b->frame==MAX_ZOOM_SETUP) {
        pack_chart_view_touch(0,NULL);
        lv_point_t initial[2]={{250,480},{350,480}};
        if (!pack_chart_view_touch(2,initial)) { b->failed=true; atomic_store(&b->complete,true); return; }
        if (b->frame==MAX_ZOOM_SETUP) span=210;
    } else if (b->frame<=FIT_FRAMES+PAN_FRAMES) {
        middle+=(int)(b->frame-FIT_FRAMES)*4;
    } else if (b->frame<MAX_ZOOM_SETUP) {
        middle+=PAN_FRAMES*4;
        span+=8*(int)(b->frame-FIT_FRAMES-PAN_FRAMES);
    } else {
        span=210;
        middle+=(int)(b->frame-MAX_ZOOM_SETUP)*4;
    }
    lv_point_t pair[2]={{middle-span/2,480},{middle+span/2,480}};
    bool handled=pack_chart_view_touch(2,pair);
    if (b->frame==MAX_ZOOM_SETUP) b->pan_zoom=pack_chart_view_visible_zoom();
    if (!handled || (b->frame>=MAX_ZOOM_SETUP && b->pan_zoom!=600)) {
        b->failed=true;
        atomic_store(&b->complete,true);
    }
}

static void presented(lv_event_t *e)
{
    chart_bench_t *b=lv_event_get_user_data(e);
    if (!b->started || b->frame==b->frame_count || lv_screen_active()!=b->screen) return;
    if (lv_event_get_code(e)==LV_EVENT_FLUSH_START) b->flush_started=esp_timer_get_time();
    if (lv_event_get_code(e)==LV_EVENT_FLUSH_FINISH && b->flush_started) {
        b->flushed=true;
        b->flush_us[b->frame]+=(uint32_t)(esp_timer_get_time()-b->flush_started);
        b->flush_started=0;
    }
    if (lv_event_get_code(e)==LV_EVENT_REFR_READY && b->flushed) {
        b->flushed=false;
        if (b->navigation && pack_autopilot_view_waiting_for_body()) {
            b->partial_frames++;
            return;
        }
        b->elapsed[b->frame++]=(uint32_t)(esp_timer_get_time()-b->started);
        b->started=0;
        if (b->frame==b->frame_count) atomic_store(&b->complete,true);
    }
}

static void summarize(const chart_bench_t *b, const char *name, unsigned first, unsigned count, unsigned step)
{
    uint64_t total=0, flush=0;
    uint32_t peak=0;
    for (unsigned i=first;i<first+count*step;i+=step) {
        total+=b->elapsed[i];
        flush+=b->flush_us[i];
        if (b->elapsed[i]>peak) peak=b->elapsed[i];
    }
    printf("UI_CHART_BENCH motion=%s frames=%u mean_us=%" PRIu32 " max_us=%" PRIu32
           " mean_flush_us=%" PRIu32 "\n",name,count,(uint32_t)(total/count),peak,(uint32_t)(flush/count));
}

static void poll_view(lv_timer_t *timer)
{
    (void)timer;
    pack_view_tick();
}

static esp_err_t render(chart_bench_t *b,pack_scene_t *scene)
{
    void *storage=NULL;
    if (b->navigation) {
        if (scene->permissions!=PACK_PERM_UI || b->target_page>=scene->screen_count) return ESP_ERR_NOT_SUPPORTED;
    } else {
        if (!scene->portrait || !pack_chart_at(scene,0) || !pack_chart_at(scene,1)) return ESP_ERR_NOT_SUPPORTED;
        storage=heap_caps_calloc(1,pack_chart_view_storage(),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if (!storage) return ESP_ERR_NO_MEM;
    }
    if (!ui_platform_lock(2000)) { heap_caps_free(storage); return ESP_ERR_TIMEOUT; }
    if (!ui_shell_active()) { ui_platform_unlock(); heap_caps_free(storage); return ESP_ERR_INVALID_STATE; }
#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
    ui_platform_set_benchmark_band(b->band_width);
#endif
    ui_shell_suspend();
    lv_display_t *display=ui_platform_display();
    b->screen=lv_obj_create(NULL);
    if (b->screen) lv_obj_set_scrollable(b->screen,false);
    lv_obj_t *frame=b->screen ? pack_view_frame(b->screen,scene->portrait) : NULL;
    lv_obj_t *area=frame ? lv_obj_create(frame) : NULL;
    const pack_widget_t widget={.kind=PACK_CHART,.x=8,.y=100,.w=584,.h=864,.tab_count=2};
    lv_timer_t *timer=NULL,*view_timer=NULL;
    esp_err_t result=ESP_ERR_NO_MEM;
    if (area && b->navigation) {
        lv_obj_remove_style_all(area);
        lv_obj_set_scrollable(area,false);
        lv_obj_set_pos(area,0,32);
        lv_obj_set_size(area,scene->portrait ? 600 : 1024,scene->portrait ? 992 : 568);
        if (pack_view_open(area,scene,PACK_PERM_UI)) view_timer=lv_timer_create(poll_view,50,NULL);
    }
    if (area && (b->navigation ? view_timer!=NULL : pack_chart_view_open(area,scene,&widget,storage))) {
        ui_theme_apply(frame,UI_STYLE_CONTROL_PAGE,0);
        lv_obj_t *label=lv_label_create(frame);
        if (label) {
            lv_label_set_text(label,b->navigation ? scene->title : "Chart benchmark");
            lv_obj_set_pos(label,12,6);
        }
        if (lv_display_add_event_cb(display,presented,LV_EVENT_FLUSH_START,b) &&
            lv_display_add_event_cb(display,presented,LV_EVENT_FLUSH_FINISH,b) &&
            lv_display_add_event_cb(display,presented,LV_EVENT_REFR_READY,b)) {
            lv_screen_load(b->screen);
            timer=lv_timer_create(draw_next,16,b);
        }
    }
    ui_platform_unlock();
    if (timer) {
        int64_t deadline=esp_timer_get_time()+45000000;
        result=ESP_ERR_TIMEOUT;
        while (esp_timer_get_time()<deadline) {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (!pack_card_present()) { result=ESP_ERR_NOT_FOUND; break; }
            if (atomic_load(&b->complete)) { result=b->failed ? ESP_FAIL : ESP_OK; break; }
        }
    }
    ui_platform_lock(-1);
    if (timer) lv_timer_delete(timer);
    if (view_timer) lv_timer_delete(view_timer);
    lv_display_remove_event_cb_with_user_data(display,presented,b);
    if (b->navigation) pack_view_close();
    else {
        pack_chart_view_touch(0,NULL);
        pack_chart_view_close();
    }
#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
    ui_platform_set_benchmark_band(0);
#endif
    ui_shell_show();
    if (b->screen) lv_obj_delete(b->screen);
    ui_platform_unlock();
    heap_caps_free(storage);
    if (result==ESP_OK && b->navigation) {
        printf("UI_PAGE_BENCH target_page=%u\n",b->target_page);
        static const char *const steps[]={"first_to_picker","picker_to_second","second_to_picker","picker_to_first"};
        for (unsigned i=0;i<PAGE_STEPS;i++) summarize(b,steps[i],1+i,PAGE_CYCLES,PAGE_STEPS);
        // Frame zero warms the first page. The second page is cold on the first visit.
        for (unsigned visit=0;visit<PAGE_CYCLES;visit++) {
            unsigned i=2+visit*PAGE_STEPS;
            printf("UI_PAGE_BENCH second_page_visit=%u action_us=%" PRIu32
                   " total_us=%" PRIu32 " flush_us=%" PRIu32 "\n",
                   visit+1,b->page_action_us[i],b->elapsed[i],b->flush_us[i]);
        }
        printf("UI_CHART_BENCH incomplete_presentations=%u\n",b->partial_frames);
    } else if (result==ESP_OK) {
        if (b->band_width) printf("UI_CHART_BENCH band_width=%u\n",b->band_width);
        summarize(b,"fit",0,FIT_FRAMES,1);
        summarize(b,"pan",FIT_FRAMES+1,PAN_FRAMES,1);
        summarize(b,"pinch",FIT_FRAMES+1+PAN_FRAMES,PINCH_FRAMES,1);
        printf("UI_CHART_BENCH pan_zoom_percent=%u\n",b->pan_zoom);
        summarize(b,"pan_600",MAX_ZOOM_SETUP+1,PAN_FRAMES,1);
    } else printf("UI_CHART_BENCH stopped_at_frame=%u\n",b->frame);
    return result;
}

static void worker(void *arg)
{
    chart_bench_t *b=arg;
    pack_candidate_t candidate={0};
    b->result=pack_card_mount();
    if (b->result==ESP_OK) {
        b->result=pack_load(b->path,&candidate);
        if (b->result==ESP_OK) b->result=render(b,candidate.scene);
        pack_dispose(&candidate);
        pack_card_close();
    }
    xSemaphoreGive(b->done);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t chart_bench_run(const char *filename, bool navigation, unsigned target_page, unsigned band_width)
{
    if (band_width && (navigation || band_width<32 || band_width>1024)) return ESP_ERR_INVALID_ARG;
#if !CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL
    if (band_width) return ESP_ERR_NOT_SUPPORTED;
#endif
    size_t len=strlen(filename);
    if (len<9 || strcmp(filename+len-8,".kvmpack") || strpbrk(filename,"/\\:") ||
        len+sizeof(PACK_MOUNT "/")>PACK_PATH_CAP) return ESP_ERR_INVALID_ARG;
    if (!ui_shell_active()) return ESP_ERR_INVALID_STATE;
    chart_bench_t b={.navigation=navigation,.target_page=target_page,.band_width=band_width,.frame_count=navigation ? PAGE_FRAMES : FRAME_COUNT};
    snprintf(b.path,sizeof(b.path),"%s/%s",PACK_MOUNT,filename);
    b.done=xSemaphoreCreateBinary();
    if (!b.done) return ESP_ERR_NO_MEM;
    b.result=ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCoreWithCaps(worker,"chart_bench",48*1024,&b,2,NULL,1,
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)==pdPASS) xSemaphoreTake(b.done,portMAX_DELAY);
    vSemaphoreDelete(b.done);
    return b.result;
}
