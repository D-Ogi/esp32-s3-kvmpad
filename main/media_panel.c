// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_panel.h"
#include "magnifier_landscape.h"
#include "ui_theme.h"
#include "cinepak_tile.h"
#include "live_queue.h"
#include "kvmbridge.h"
#include "board_lcd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lvgl.h"
#include "src/draw/lv_draw_buf_private.h"
#include "src/image/lv_image_decoder_private.h"
#include <inttypes.h>
#include <limits.h>
#include "kvm_control_icons.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#include "esp_rom_crc.h"
#include "media_synthetic_h264.h"
#include "media_synthetic_jpeg.h"
#endif

// Only the UI owns LVGL. A worker writes the slot opposite the displayed
// descriptor. The UI releases its old slot only after the prior refresh.
static lv_obj_t *screen, *previous, *image, *status, *controls, *modes, *formats, *freeze_btn;
static lv_obj_t *settings_card, *review_card, *review_prompt, *approve_display;
static lv_obj_t *preview_frame, *preview_source, *preview_output;
static lv_obj_t *monitor_btn, *crop_info, *telemetry_status;
static lv_obj_t *file_title, *file_position, *file_bar, *pause_btn, *fit_btn, *native_btn;
static bool file_fit, holding_scanout;
static bool prefer_fit = true;
static unsigned position_frame;
static atomic_bool file_paused;
static atomic_uint file_total;
static int64_t paused_at, pause_total;
static char crop_text[144];
static lv_timer_t *timer;
static void (*navigate)(void), (*file_stopped)(void);
static unsigned generation;
static uint32_t changing_monitor;
static bool live_screen, flush_pending, did_flush, requested;
static int64_t first_refresh, last_refresh;
static unsigned refresh_count;
// Arrival timestamps follow the encoded and decoded slots. Only the LVGL task
// updates the live display measurements, after a completed picture refresh.
static int64_t packet_rx_us[2], picture_rx_us[2];
static uint32_t packet_sequence[2], picture_sequence[2];
static kvmbridge_crop_t packet_crop[2], picture_crop[2];
static struct {
    int64_t fresh_since, rate_start;
    unsigned rate_frames, fps_tenths, rx_to_lcd_ms;
    bool awaiting_frame;
    kvmbridge_crop_t crop; // Geometry of the last completed refresh. Zero width is a legacy frame.
} live_stats;
enum { LIVE_STALE_US = 1500000 };
static unsigned file_width, file_height;
static int64_t file_slot_pts[2], first_file_pts, last_file_pts;
#ifdef KVM_MEDIA_PAGED_GUEST
#include "paged_video.h"
extern const uint8_t _binary_kvm_paged_decoder_start[], _binary_kvm_paged_decoder_end[];
#endif
static atomic_bool retire_surface;
static atomic_bool prepared_playing;
static atomic_uint file_flushed;
static _Atomic uint32_t file_refresh_us;
static unsigned file_published;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static atomic_uint prepared_crc, prepared_verified;
static atomic_uint timed_epoch;
// UI-owned. The file_flushed handoff makes the preceding ready timestamp
// visible to the worker before it publishes another picture.
static int64_t timed_selected_at, timed_ready_at, timed_display_us;
static unsigned timed_display_frames;

static int64_t before_ready(int64_t start, int64_t end, int64_t ready)
{
    if (ready <= start) return 0;
    return (ready < end ? ready : end) - start;
}
#endif
enum { REFRESH_INTERVAL_CAPACITY = 256 };
static uint32_t intervals[REFRESH_INTERVAL_CAPACITY];
static int64_t longest_refresh_interval;
static unsigned widths[2], heights[2];
static unsigned picture_revision[2], packet_revision[2];
static size_t surface_bytes;
static uint32_t vsync_first;
static uint16_t *surfaces[2];
static kvm_h264_picture_t retained[2];
static lv_image_dsc_t descriptors[2];
static lv_image_decoder_t *banded_decoder;
static lv_draw_buf_handlers_t cinepak_band_handlers;
static atomic_uint epoch;
static atomic_uint freeze_revision;
static atomic_bool busy, frozen, started;
static atomic_int file_result;
static atomic_int shown, pending;
static SemaphoreHandle_t gate;
static StaticSemaphore_t gate_memory;
static kvm_live_queue_t queue;
static uint8_t *encoded[2];
static bool in_place;
static atomic_uint wire_epoch;
static kvm_live_codec_t stream_codec;
static const char *const crop_modes[] = {"OVERVIEW", "PIXEL_1X", "ZOOM_2X"};
static const char magnifier_profile[] = "org.kvmpad.utility.cursor-magnifier";
static unsigned magnifier_format;
static char capture_profile[101];
static kvm_media_decoder_t *decoder;
static bool (*card_present)(void);
static int64_t next_card_check, clock_origin, pts_origin, next_frame_us;
static uint32_t file_duration_us;
static bool file_half;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
enum { SYNTH_PAYLOAD = 32438, SYNTH_PACKET = KVM_LIVE_HEADER + SYNTH_PAYLOAD, SYNTH_ID = 0x53594e54 };
enum { PHASE_CAPACITY = 32, PHASE_COUNT = 6 };
typedef struct {
    uint32_t sequence;
    int64_t accepted, decode_start, decode_end;
} phase_picture_t;
// Accepted timestamps follow the queue slots under gate. The pending-slot handoff
// publishes the worker's timestamps to the UI together with the decoded picture.
static atomic_uint phase_epoch;
static int64_t phase_accepted[2];
static phase_picture_t phase_picture[2];
static struct {
    bool active, paced;
    kvm_live_codec_t codec;
    uint8_t *packet, *pressure;
    unsigned rate, frames, sent, verified, drawing, flushed;
    uint32_t vsync, bounce;
    int64_t next, deadline;
    phase_picture_t selected_picture;
    int64_t selected_at, last_flush;
    uint32_t last_sequence, valid, invalid, durations[PHASE_COUNT][PHASE_CAPACITY];
} synth;

static void record_phases(uint32_t sequence, int64_t refresh)
{
    if (sequence <= 2) return; // Framebuffer CRC warmup.
    const phase_picture_t *p = &synth.selected_picture;
    if (sequence != p->sequence || sequence != synth.drawing || sequence <= synth.last_sequence ||
        p->accepted <= 0 || p->decode_start < p->accepted || p->decode_end < p->decode_start ||
        synth.selected_at < p->decode_end || synth.last_flush < synth.selected_at || refresh < synth.last_flush ||
        refresh - p->accepted > UINT32_MAX) {
        ++synth.invalid;
        return;
    }
    const int64_t times[] = {p->accepted, p->decode_start, p->decode_end, synth.selected_at, synth.last_flush, refresh};
    unsigned write = synth.valid % PHASE_CAPACITY;
    for (unsigned i = 0; i < PHASE_COUNT - 1; ++i)
        synth.durations[i][write] = (uint32_t)(times[i + 1] - times[i]);
    synth.durations[PHASE_COUNT - 1][write] = (uint32_t)(refresh - p->accepted);
    synth.last_sequence = sequence;
    ++synth.valid;
}

static void admission(const char *stage)
{
    if (!synth.active)
        return;
    uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI("media", "MEDIA_SYNTHETIC admission=%s internal_free=%u internal_largest=%u psram_free=%u psram_largest=%u",
             stage, (unsigned)heap_caps_get_free_size(internal), (unsigned)heap_caps_get_largest_free_block(internal),
             (unsigned)heap_caps_get_free_size(external), (unsigned)heap_caps_get_largest_free_block(external));
}

#ifdef ESP_PLATFORM
static void file_heap_status(const char *stage)
{
    bool external_ok = heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true);
    bool internal_ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, true);
    ESP_LOGI("media", "CARD_HEAP stage=%s external_ok=%d internal_ok=%d worker_stack=%u",
             stage, external_ok, internal_ok, (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#else
#define file_heap_status(stage) ((void)0)
#endif
#else
#define admission(stage) ((void)0)
#define file_heap_status(stage) ((void)0)
#endif

static bool current(uint32_t value)
{
    return value && atomic_load(&epoch) == value;
}

static uint32_t new_epoch(void)
{
    if (!++generation) ++generation;
    atomic_store(&epoch, generation);
    return generation;
}

static bool cancelled(void *ctx)
{
    uint32_t value = (uint32_t)(uintptr_t)ctx;
    if (!current(value))
        return true;
    if (card_present) {
        int64_t now = esp_timer_get_time();
        if (now >= next_card_check) {
            next_card_check = now + 250000;
            if (!card_present()) {
                media_panel_stream_end(value);
                return true;
            }
        }
    }
    return false;
}

static void pause_worker(void)
{
    vTaskDelay(1);
}

// File presentation timestamps stop advancing while the local transport is paused.
static int64_t file_now(void)
{
    xSemaphoreTake(gate, portMAX_DELAY);
    int64_t now = (atomic_load(&file_paused) ? paused_at : esp_timer_get_time()) - pause_total;
    xSemaphoreGive(gate);
    return now;
}

// Once the LCD has presented the last picture, keep it visible for its
// recorded duration before the library replaces it with the playback summary.
static void hold_last_picture(void *ctx, uint32_t duration_us)
{
    // File durations are at most one second. Unsigned subtraction also
    // measures a final frame whose presentation crosses the 32-bit wrap.
    const uint32_t presented = atomic_load(&file_refresh_us);
    while ((atomic_load(&file_paused) || (uint32_t)file_now() - presented < duration_us) && !cancelled(ctx))
        pause_worker();
}

static bool claim(uint32_t value)
{
    if (!current(value))
        return false;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&busy, &idle, true))
        return false;
    if (!current(value) || atomic_exchange(&started, true)) {
        atomic_store(&busy, false);
        return false;
    }
    return true;
}

static bool reserve_surfaces(size_t bytes)
{
    surface_bytes = bytes;
    for (unsigned i = 0; i < 2; ++i) {
        surfaces[i] = heap_caps_malloc(surface_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!surfaces[i])
            return false;
    }
    return true;
}

static int next_slot(void *ctx)
{
    while (atomic_load(&pending) >= 0 && !cancelled(ctx))
        pause_worker();
    return cancelled(ctx) ? -1 : atomic_load(&shown) == 0 ? 1 : 0;
}

static void publish(int slot, unsigned w, unsigned h, unsigned revision)
{
    widths[slot] = w;
    heights[slot] = h;
    picture_revision[slot] = revision;
    atomic_store(&pending, slot);
}

static bool file_frame(void *ctx, const kvm_media_yuv_t *f)
{
    unsigned w = f->width >> file_half, h = f->height >> file_half;
    if (!w || !h || w > 960 || h > 540 || (size_t)w * h * 2 > surface_bytes)
        return false;
    int slot = next_slot(ctx);
    if (slot < 0)
        return false;
    kvm_media_target_t t = {surfaces[slot], w, h, w, cancelled, ctx};
    if (!kvm_media_render_yuv(f, file_half, &t))
        return false;
    int64_t now = file_now();
    if (!clock_origin) {
        clock_origin = now;
        pts_origin = f->pts_us;
        next_frame_us = now;
    }
    // Reject discontinuities before arithmetic over untrusted timestamps.
    if (f->pts_us != INT64_MIN && pts_origin != INT64_MIN) {
        const int64_t max_span = INT64_C(86400) * 1000000;
        if (f->pts_us < pts_origin || (uint64_t)f->pts_us - (uint64_t)pts_origin > (uint64_t)max_span)
            return false;
        next_frame_us = clock_origin + (f->pts_us - pts_origin);
    }
    while ((atomic_load(&file_paused) || file_now() < next_frame_us) && !cancelled(ctx))
        pause_worker();
    if (cancelled(ctx))
        return false;
    file_slot_pts[slot] = f->pts_us;
    publish(slot, w, h, 0);
    ++file_published;
    file_duration_us = f->duration_us > 0 && f->duration_us <= 1000000 ? f->duration_us : 41667;
    next_frame_us += file_duration_us;
    return true;
}

kvm_play_result_t media_panel_play_result(uint32_t value, FILE *file, bool mp4, kvm_h264_reconstruction_t mode,
                                          bool (*present)(void))
{
    if (!claim(value))
        return current(value) ? KVM_PLAY_UNAVAILABLE : KVM_PLAY_STOPPED;
    void *ctx = (void *)(uintptr_t)value;
    card_present = present;
    next_card_check = clock_origin = 0;
    file_published = 0;
    file_half = mode == KVM_H264_EXACT;
    kvm_media_playback_t sink = {file_frame, cancelled, ctx, mode};
    unsigned width = 0, height = 0;
    file_heap_status("open");
    kvm_play_result_t result = kvm_media_h264_probe(file, mp4, &width, &height, cancelled, ctx);
    file_heap_status("probed");
    if (result == KVM_PLAY_OK && !cancelled(ctx)) {
        if (!reserve_surfaces((size_t)(width / 2) * (height / 2) * 2))
            result = KVM_PLAY_NO_MEMORY;
        else
            result = kvm_media_play_h264_result(file, mp4, &sink);
    }
    file_heap_status("decoded");
    // A later decoding error can arrive while LVGL still owns the last submitted picture.
    while (atomic_load(&file_flushed) < file_published && !cancelled(ctx))
        pause_worker();
    if (result == KVM_PLAY_OK && file_published)
        hold_last_picture(ctx, file_duration_us);
    if (cancelled(ctx))
        result = KVM_PLAY_STOPPED;
    card_present = NULL;
    atomic_store(&file_result, result);
    atomic_store(&busy, false);
    return result;
}

bool media_panel_play(uint32_t value, FILE *file, bool mp4, kvm_h264_reconstruction_t mode,
                      bool (*present)(void))
{
    return media_panel_play_result(value, file, mp4, mode, present) == KVM_PLAY_OK;
}

kvm_play_result_t media_panel_play_rgb(uint32_t value, unsigned w, unsigned h, unsigned count,
                                      media_rgb_read_t read_frame, void *reader, bool (*present)(void))
{
    if (!w || w > 960 || !h || h > 540 || !count || !read_frame)
        return KVM_PLAY_UNAVAILABLE;
    if (!claim(value))
        return KVM_PLAY_STOPPED;
    void *ctx = (void *)(uintptr_t)value;
    card_present = present;
    next_card_check = 0;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    atomic_store(&prepared_verified, 0);
#endif
    atomic_store(&prepared_playing, true);
    atomic_store(&file_total, count);
    // LVGL keeps drawing the previous complete picture while the SD worker
    // reconstructs the next one, including at the largest display geometry.
    size_t bytes = (size_t)w * h * sizeof(uint16_t);
    bool two_surfaces = reserve_surfaces(bytes);
    // At the largest card geometry both display sizes use identical pixels. If the
    // heap is fragmented, let scanout hold the completed frame while its source
    // is detached and reconstructed in place. Smaller clips retain both surfaces.
    bool single_surface = !two_surfaces && surfaces[0] && w == 960 && h == 540;
    kvm_play_result_t result = two_surfaces || single_surface ? KVM_PLAY_OK : KVM_PLAY_NO_MEMORY;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    ESP_LOGI("media", "CARD_SURFACES count=%u", two_surfaces ? 2u : single_surface ? 1u : 0u);
#endif
    int64_t first = 0;
    const uint16_t *prior = NULL;
    unsigned published = 0;
    uint32_t duration_us = 0;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    bool timed = atomic_load(&timed_epoch) == value;
    int64_t loop_start = timed ? esp_timer_get_time() : 0;
    int64_t slot_us = 0, read_us = 0, gate_us = 0, read_prev_ready_us = 0, gate_prev_ready_us = 0;
#endif
    for (unsigned i = 0; result == KVM_PLAY_OK && i < count; ++i) {
        // Once LVGL has selected the preceding descriptor its old surface is
        // available to the worker. Keep that selected picture intact while
        // reading and reconstructing the next one into the other surface.
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        int64_t slot_start = timed ? esp_timer_get_time() : 0;
#endif
        int slot = 0;
        if (!single_surface) {
            slot = next_slot(ctx);
        } else if (published) {
            atomic_store(&retire_surface, true);
            while (atomic_load(&retire_surface) && !cancelled(ctx))
                pause_worker();
            if (cancelled(ctx)) slot = -1;
        }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        int64_t read_start = timed ? esp_timer_get_time() : 0;
#endif
        uint64_t pts = 0;
        uint32_t crc = 0;
        if (slot < 0 || !read_frame(reader, surfaces[slot], prior, &pts, &duration_us, &crc, cancelled, ctx)) {
            result = KVM_PLAY_UNAVAILABLE;
            break;
        }
        if (!i)
            first = file_now();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        int64_t read_end = timed ? esp_timer_get_time() : 0;
#endif
        // If Pause arrived while the sole surface was being rewritten, finish
        // that already in-flight picture and paint the paused transport with it.
        while ((atomic_load(&file_flushed) < published ||
                (atomic_load(&file_paused) ? !(single_surface && published) : file_now() - first < (int64_t)pts)) &&
               !cancelled(ctx))
            pause_worker();
        if (cancelled(ctx))
            break;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (timed) {
            int64_t gate_end = esp_timer_get_time();
            slot_us += read_start - slot_start;
            read_us += read_end - read_start;
            gate_us += gate_end - read_end;
            if (published) {
                read_prev_ready_us += before_ready(read_start, read_end, timed_ready_at);
                gate_prev_ready_us += before_ready(read_end, gate_end, timed_ready_at);
            }
        }
        atomic_store(&prepared_crc, crc);
#endif
        file_slot_pts[slot] = pts <= INT64_MAX ? (int64_t)pts : INT64_MIN;
        publish(slot, w, h, 0);
        ++published;
        prior = surfaces[slot];
    }
    // Also preserve the last submitted picture if a later SD read failed.
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t drain_start = timed ? esp_timer_get_time() : 0;
#endif
    while (atomic_load(&file_flushed) < published && !cancelled(ctx))
        pause_worker();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t hold_start = timed ? esp_timer_get_time() : 0;
#endif
    if (result == KVM_PLAY_OK && published == count)
        hold_last_picture(ctx, duration_us);
    if (cancelled(ctx))
        result = KVM_PLAY_STOPPED;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (timed && result == KVM_PLAY_OK) {
        int64_t end = esp_timer_get_time();
        ESP_LOGI("media", "CARD_WORKER_STAGES epoch=%" PRIu32 " frames=%u wall_us=%" PRId64
                 " slot_us=%" PRId64 " read_us=%" PRId64 " gate_us=%" PRId64 " drain_us=%" PRId64
                 " hold_us=%" PRId64 " read_prev_ready_us=%" PRId64 " gate_prev_ready_us=%" PRId64,
                 value, published, end - loop_start, slot_us, read_us, gate_us, hold_start - drain_start,
                 end - hold_start, read_prev_ready_us, gate_prev_ready_us);
    }
    if (atomic_load(&timed_epoch) == value)
        ESP_LOGI("media", "CARD_VIDEO frames=%u lcd_verification=skipped total=%u", atomic_load(&file_flushed), count);
    else
        ESP_LOGI("media", "CARD_VIDEO frames=%u lcd_verified=%u total=%u", atomic_load(&file_flushed),
                 atomic_load(&prepared_verified), count);
#endif
    atomic_store(&prepared_playing, false);
    card_present = NULL;
    atomic_store(&file_result, result);
    atomic_store(&busy, false);
    return result;
}

#ifdef KVM_MEDIA_PAGED_GUEST
typedef struct {
    void *epoch_ctx;
    unsigned frames;
    unsigned width, height;
    int64_t last_yield;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    const kvm_paged_stats_t *stats;
    int64_t next_progress;
#endif
} paged_panel_t;

static bool paged_stop(void *ctx)
{
    paged_panel_t *p = ctx;
    int64_t now = esp_timer_get_time();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (now >= p->next_progress) {
        p->next_progress = now + INT64_C(15000000);
        ESP_LOGI("media", "PAGED_H264 progress frames=%u steps=%llu io_read_pages=%llu io_write_pages=%llu",
                 p->frames, (unsigned long long)p->stats->steps, (unsigned long long)p->stats->page_reads,
                 (unsigned long long)p->stats->page_writes);
    }
#endif
    if (now - p->last_yield >= 20000) {
        p->last_yield = now;
        pause_worker();
    }
    return cancelled(p->epoch_ctx);
}

static bool paged_frame(void *ctx, kvm_paged_vm_t *vm, const kvm_paged_picture_t *f)
{
    paged_panel_t *p = ctx;
    unsigned w = f->width, h = f->height;
    if (w > 960 || h > 540 || (w | h) & 1 || paged_stop(ctx))
        return false;
    if (p->width != w || p->height != h)
        return false;
    if (p->frames) {
        // The previous scanout already owns its pixels. Detach the old LVGL
        // descriptor on its owner thread before writing the single RGB surface.
        atomic_store(&retire_surface, true);
        while (atomic_load(&retire_surface) && !paged_stop(ctx))
            pause_worker();
    }
    uint8_t luma[2 * 960], u[480], v[480];
    kvm_media_yuv_t rows = {.plane = {luma, u, v},
                            .stride = {w, w / 2, w / 2},
                            .width = w,
                            .height = 2,
                            .full_range = f->full_range,
                            .bt709 = f->bt709};
    for (unsigned y = 0; y < h; y += 2) {
        if (paged_stop(ctx) || !kvm_paged_read(vm, f->plane[0] + y * f->stride[0], luma, w) ||
            !kvm_paged_read(vm, f->plane[0] + (y + 1) * f->stride[0], luma + w, w) ||
            !kvm_paged_read(vm, f->plane[1] + y / 2 * f->stride[1], u, w / 2) ||
            !kvm_paged_read(vm, f->plane[2] + y / 2 * f->stride[2], v, w / 2))
            return false;
        kvm_media_target_t target = {.pixels = surfaces[0] + y * w, .width = w, .height = 2, .stride = w};
        if (!kvm_media_render_yuv(&rows, false, &target))
            return false;
    }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    uint32_t crc = esp_rom_crc32_le(0, (const uint8_t *)surfaces[0], surface_bytes);
    atomic_store(&prepared_crc, crc);
#endif
    file_slot_pts[0] = INT64_MIN;
    publish(0, w, h, 0);
    while (atomic_load(&file_flushed) <= p->frames && !paged_stop(ctx))
        pause_worker();
    if (cancelled(p->epoch_ctx))
        return false;
    ++p->frames;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    ESP_LOGI("media", "PAGED_H264 frame=%u lcd_verified=%u rgb_crc=%08lx", p->frames,
             atomic_load(&prepared_verified), (unsigned long)crc);
#endif
    return true;
}

kvm_play_result_t media_panel_play_paged(uint32_t value, FILE *source, FILE *scratch,
                                         kvm_h264_reconstruction_t mode, bool (*present)(void))
{
    if (!source || !scratch || source == scratch || (mode != KVM_H264_REDUCED && mode != KVM_H264_LARGE_BANK))
        return KVM_PLAY_UNAVAILABLE;
    if (!claim(value))
        return current(value) ? KVM_PLAY_UNAVAILABLE : KVM_PLAY_STOPPED;
    paged_panel_t panel = {.epoch_ctx = (void *)(uintptr_t)value};
    card_present = present;
    next_card_check = 0;
    atomic_store(&retire_surface, false);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    atomic_store(&prepared_verified, 0);
#endif
    atomic_store(&prepared_playing, true);
    kvm_paged_sink_t sink = {&panel, paged_frame, paged_stop};
    kvm_paged_stats_t stats = {0};
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    panel.stats = &stats;
    panel.next_progress = esp_timer_get_time() + INT64_C(15000000);
#endif
    unsigned w = 0, h = 0;
    kvm_play_result_t result = kvm_media_h264_probe(source, false, &w, &h, cancelled, panel.epoch_ctx);
    int rc = -1;
    if (result != KVM_PLAY_OK)
        goto done;
    panel.width = w / 2;
    panel.height = h / 2;
    if (!panel.width || panel.width > 960 || !panel.height || panel.height > 540) {
        result = KVM_PLAY_UNAVAILABLE;
        goto done;
    }
    surface_bytes = (size_t)panel.width * panel.height * sizeof(*surfaces[0]);
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    surfaces[0] = heap_caps_malloc(surface_bytes, caps);
    // Reserve the virtual-page maps, the 96 KiB ROM cache and space for normal
    // card/UI allocations before selecting the number of dirty-cache slots.
    size_t largest = heap_caps_get_largest_free_block(caps);
    unsigned pages = largest > 192 * 1024 ? (unsigned)((largest - 192 * 1024) / 4112) : 0;
    if (pages > 192)
        pages = 192;
    ESP_LOGI("media", "PAGED_H264 start mode=%u cache_pages=%u psram_free=%u largest=%u", mode, pages,
             (unsigned)heap_caps_get_free_size(caps), (unsigned)largest);
    if (!surfaces[0] || pages < 16) {
        result = KVM_PLAY_NO_MEMORY;
        goto done;
    }
    rc = kvm_paged_run(_binary_kvm_paged_decoder_start,
                       (size_t)(_binary_kvm_paged_decoder_end - _binary_kvm_paged_decoder_start), source, scratch,
                       pages, mode == KVM_H264_LARGE_BANK, &sink, &stats);
    result = rc == 0 && stats.frames ? KVM_PLAY_OK : KVM_PLAY_UNAVAILABLE;
    if (rc == 1)
        result = KVM_PLAY_NO_MEMORY;
done:
    if (cancelled(panel.epoch_ctx))
        result = KVM_PLAY_STOPPED;
    ESP_LOGI(
        "media",
        "PAGED_H264 result=%d rc=%d guest=%d frames=%lu steps=%llu reads=%llu writes=%llu scratch=%lu rom_loads=%lu",
        (int)result, rc, stats.guest_status, (unsigned long)stats.frames, (unsigned long long)stats.steps,
        (unsigned long long)stats.page_reads, (unsigned long long)stats.page_writes,
        (unsigned long)stats.scratch_bytes, (unsigned long)stats.rom_loads);
    atomic_store(&retire_surface, false);
    atomic_store(&prepared_playing, false);
    card_present = NULL;
    atomic_store(&file_result, result);
    atomic_store(&busy, false);
    return result;
}
#endif

const char *media_panel_play_message(kvm_play_result_t result)
{
    switch (result) {
    case KVM_PLAY_OK: return "Playback complete.";
    case KVM_PLAY_NO_MEMORY: return "Not enough memory to play this video.";
    case KVM_PLAY_STOPPED: return "Playback stopped.";
    default: return "Could not play this video.";
    }
}

static void live_worker(void *arg)
{
    unsigned w = stream_codec == KVM_LIVE_CINEPAK_INTRA ? 960 : 512;
    unsigned h = stream_codec == KVM_LIVE_CINEPAK_INTRA ? 540 : 512;
    while (!cancelled(arg)) {
        size_t size = 0;
        xSemaphoreTake(gate, portMAX_DELAY);
        const uint8_t *data = kvm_live_queue_take(&queue, &size);
        unsigned revision = data ? packet_revision[queue.decoding] : 0;
        int64_t received = data ? packet_rx_us[queue.decoding] : 0;
        kvmbridge_crop_t crop = data ? packet_crop[queue.decoding] : (kvmbridge_crop_t){0};
        uint32_t seq = data ? packet_sequence[queue.decoding] : 0;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        bool measure = seq > 2 && atomic_load(&phase_epoch) == (uint32_t)(uintptr_t)arg;
        int64_t accepted = measure ? phase_accepted[queue.decoding] : 0;
        bool chart_stack = data && stream_codec == KVM_LIVE_H264_INTRA && queue.receiver.stream_id == SYNTH_ID;
#endif
        xSemaphoreGive(gate);
        if (!data) {
            pause_worker();
            continue;
        }
        int slot = next_slot(arg);
        bool published = false;
        if (slot >= 0 && revision == atomic_load(&freeze_revision) && !atomic_load(&frozen)) {
            kvm_media_target_t t = {surfaces[slot], w, h, w, cancelled, arg};
            bool ok;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            int64_t decode_start = measure ? esp_timer_get_time() : 0;
#endif
            if (stream_codec == KVM_LIVE_CINEPAK_INTRA)
                ok = kvm_cvid_index((void *)surfaces[slot], data, size);
            else if (stream_codec == KVM_LIVE_H264_INTRA)
                ok = kvm_media_h264_picture(decoder, &retained[slot], data, size, cancelled, arg);
            else
                ok = kvm_media_decoder_frame(decoder, data, size, &t);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (measure)
                phase_picture[slot] = (phase_picture_t){seq, accepted, decode_start, esp_timer_get_time()};
            if (chart_stack && seq <= 3)
                ESP_LOGI("media", "MEDIA_SYNTHETIC frame=%lu decoder_stack_free=%u",
                         (unsigned long)seq, (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
            if (ok && !cancelled(arg) && revision == atomic_load(&freeze_revision) && !atomic_load(&frozen)) {
                picture_rx_us[slot] = received;
                picture_crop[slot] = crop;
                picture_sequence[slot] = seq;
                publish(slot, w, h, revision);
                published = true;
            }
        }
        if (!published)
            kvmbridge_frame_done((uint32_t)(uintptr_t)arg, seq, false);
        xSemaphoreTake(gate, portMAX_DELAY);
        kvm_live_queue_release(&queue);
        xSemaphoreGive(gate);
    }
    xSemaphoreTake(gate, portMAX_DELAY);
    kvm_live_queue_stop(&queue);
    xSemaphoreGive(gate);
    atomic_store(&busy, false);
    vTaskDeleteWithCaps(NULL);
}

static bool start_stream(uint32_t value, uint32_t id, kvm_live_codec_t codec, bool paced)
{
    if (!claim(value))
        return false;
    bool cvid = codec == KVM_LIVE_CINEPAK_INTRA;
    bool ok = codec != KVM_LIVE_H264_INTRA || kvm_media_has_h264();
    if (!ok)
        kvmbridge_capture_decoder_failed(value);
    admission("resources");
    if (ok && codec != KVM_LIVE_H264_INTRA)
        ok = reserve_surfaces(cvid ? sizeof(kvm_cvid_index_t) : 512u * 512u * 2);
    if (!ok)
        admission("failed_surface_or_codec");
    xSemaphoreTake(gate, portMAX_DELAY);
    in_place = paced;
    if (ok) {
        if (paced) {
            encoded[0] = heap_caps_malloc(KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ok = kvm_live_queue_init_in_place(&queue, encoded[0], KVM_LIVE_MAX_PAYLOAD, id, codec);
        } else {
            for (int i = 0; i < 2; ++i)
                encoded[i] = heap_caps_malloc(KVM_LIVE_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ok = kvm_live_queue_init(&queue, encoded[0], encoded[1], KVM_LIVE_MAX_PAYLOAD, id, codec);
        }
        if (!ok)
            admission("failed_queue");
    }
    stream_codec = codec;
    xSemaphoreGive(gate);
    if (ok && !cvid) {
        admission("before_decoder_arena");
        ok = (decoder = kvm_media_decoder_open(codec)) != NULL;
        if (!ok) {
            kvmbridge_capture_decoder_failed(value);
            admission("failed_decoder_arena");
        }
    }
    uint32_t stack_caps = MALLOC_CAP_8BIT;
#if CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    stack_caps |= MALLOC_CAP_SPIRAM;
#else
    stack_caps |= MALLOC_CAP_INTERNAL;
#endif
    if (ok && current(value)) {
        if (paced)
            atomic_store(&wire_epoch, value);
        unsigned stack_bytes = codec == KVM_LIVE_H264_INTRA ? 65536 : cvid ? 6144 : 32768;
        // Keep sustained H.264 decoder traffic on the display core, where the
        // bounce-buffer interrupt can preempt it before starting a PSRAM read.
        int core = codec == KVM_LIVE_H264_INTRA ? CONFIG_KVM_UI_TASK_CORE : tskNO_AFFINITY;
        if (xTaskCreatePinnedToCoreWithCaps(live_worker, "media_decode", stack_bytes, (void *)(uintptr_t)value,
                                          2, NULL, core, stack_caps) == pdPASS) {
            admission("ready");
            return true;
        }
        admission("failed_worker_stack");
    }
    media_panel_stream_end(value);
    kvm_media_decoder_close(decoder);
    decoder = NULL;
    atomic_store(&wire_epoch, 0);
    atomic_store(&busy, false);
    return false;
}

bool media_panel_stream_start(uint32_t value, uint32_t id, kvm_live_codec_t codec)
{
    return start_stream(value, id, codec, false);
}

static uint8_t *start_buffered_capture(uint32_t value, uint32_t id, kvm_live_codec_t codec)
{
    return start_stream(value, id, codec, true) ? encoded[0] : NULL;
}

static bool packet_writable(uint32_t value)
{
    if (!current(value) || atomic_load(&wire_epoch) != value || xSemaphoreTake(gate, 0) != pdTRUE)
        return false;
    bool writable = current(value) && kvm_live_queue_writable(&queue);
    xSemaphoreGive(gate);
    return writable;
}

// Stop/navigation invalidates the epoch immediately. Only the transport releases
// its wire-buffer reference after the last socket read has returned.
static void finish_capture(uint32_t value)
{
    media_panel_stream_end(value);
    if (value)
        atomic_compare_exchange_strong(&wire_epoch, &value, 0);
}

static bool receiver_released(void)
{
    return !atomic_load(&busy) && !atomic_load(&wire_epoch);
}

static bool stream_push_with_crop(uint32_t value, const uint8_t *message, size_t size, const kvmbridge_crop_t *crop)
{
    if (!current(value) || atomic_load(&frozen) || !gate || xSemaphoreTake(gate, 0) != pdTRUE)
        return false;
    int64_t received = esp_timer_get_time();
    bool ok = current(value) && kvm_live_queue_push(&queue, message, size);
    if (ok) {
        packet_crop[queue.waiting] = crop ? *crop : (kvmbridge_crop_t){0};
        packet_rx_us[queue.waiting] = received;
        packet_revision[queue.waiting] = atomic_load(&freeze_revision);
        packet_sequence[queue.waiting] = queue.receiver.sequence;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (queue.receiver.sequence > 2 && value == atomic_load(&phase_epoch))
            phase_accepted[queue.waiting] = esp_timer_get_time();
#endif
    }
    xSemaphoreGive(gate);
    return ok;
}

bool media_panel_stream_push(uint32_t value, const uint8_t *message, size_t size)
{
    return stream_push_with_crop(value, message, size, NULL);
}

void media_panel_stream_end(uint32_t value)
{
    if (value)
        atomic_compare_exchange_strong(&epoch, &value, 0);
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
typedef struct {
    uint32_t hash;
    unsigned lit;
    bool black;
} grid_t;

static struct {
    media_blackout_stats_t stats;
    unsigned width, height, displayed_w, displayed_h, origin, pending_confirmed, pending_inferred;
    uint32_t previous_hash;
    int front;
    bool anchored, dark, all_black;
    int64_t since;
    uint64_t pending_longest;
} blackout;

// Nearest-neighbor display coordinates of the first displayed copy of each source pixel.
static unsigned projected(unsigned pixel, unsigned native, unsigned displayed)
{
    return (pixel * displayed + native - 1) / native;
}

static grid_t sample_grid(const uint16_t *pixels, unsigned stride, unsigned w, unsigned h, unsigned dw, unsigned dh)
{
    grid_t g = {.hash = UINT32_C(2166136261), .black = true};
    for (unsigned row = 0; row < 6; ++row) {
        const uint16_t *line = pixels + projected((2 * row + 1) * h / 12, h, dh) * stride;
        for (unsigned col = 0; col < 8; ++col) {
            uint16_t p = line[projected((2 * col + 1) * w / 16, w, dw)];
            g.hash = (g.hash ^ p) * UINT32_C(16777619);
            g.black = g.black && p == 0;
            if ((p >> 11) >= 4 || ((p >> 5) & 63) >= 8 || (p & 31) >= 4)
                ++g.lit;
        }
    }
    return g;
}

static void blackout_sample(bool last_flush, bool picture)
{
    int shown_slot = atomic_load(&shown);
    grid_t expected = {0};
    bool has_source = flush_pending && shown_slot >= 0 && surfaces[shown_slot];
    if (has_source) {
        blackout.width = widths[shown_slot];
        blackout.height = heights[shown_slot];
        if (last_flush || picture)
            expected = sample_grid(surfaces[shown_slot], blackout.width, blackout.width, blackout.height,
                                   blackout.width, blackout.height);
    }
    unsigned w = blackout.width, h = blackout.height;
    if (!w || !h)
        return;
    const lv_image_dsc_t *source = lv_image_get_src(image);
    if (source) {
        blackout.displayed_w = source->header.w;
        blackout.displayed_h = source->header.h;
        blackout.origin = lv_obj_get_y(image) * 1024 + lv_obj_get_x(image);
    }
    if (!blackout.displayed_w || !blackout.displayed_h) return;
    grid_t fb[2];
    unsigned count = 0;
    for (; count < 2; ++count) {
        const uint16_t *p = board_lcd_fb(count);
        if (!p)
            break;
        fb[count] = sample_grid(p + blackout.origin, 1024, w, h, blackout.displayed_w, blackout.displayed_h);
    }
    if (!count || board_lcd_fb(2))
        return;
    ++blackout.stats.samples;
    int front = count == 1 ? 0 : -1;
#if CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL || !defined(ESP_PLATFORM)
    // DOUBLE_PARTIAL submits the full draw_fb only on the final flush, waits for the
    // panel's free-buffer notification and swaps its two private buffers. Calibrate
    // using a new source signature unique to one FB. LVGL's partial tile is not a FB.
    if (count == 2) {
        if (last_flush && blackout.front >= 0)
            blackout.front ^= 1;
        if (last_flush && has_source && expected.lit >= 4 && expected.hash != blackout.previous_hash) {
            bool a = fb[0].hash == expected.hash, b = fb[1].hash == expected.hash;
            if (a != b) {
                int unique = a ? 0 : 1;
                if (blackout.front >= 0 && blackout.front != unique)
                    ++blackout.stats.resyncs;
                blackout.front = unique;
            }
        }
        front = blackout.front;
    }
#endif
    bool all_black = fb[0].black && (count == 1 || fb[1].black);
    bool dark = all_black || (front >= 0 && fb[front].black);
    bool unknown = front < 0 && fb[0].black != fb[1].black;
    if (unknown)
        ++blackout.stats.ambiguous_samples;
    int64_t now = esp_timer_get_time();
    if (blackout.anchored && !unknown) {
        if (dark) {
            if (!blackout.dark) {
                blackout.since = now;
                blackout.all_black = false;
            }
            blackout.dark = true;
            blackout.all_black |= all_black;
        } else if (blackout.dark) {
            uint64_t elapsed = (uint64_t)(now - blackout.since);
            if (elapsed > blackout.pending_longest)
                blackout.pending_longest = elapsed;
            if (blackout.all_black) ++blackout.pending_confirmed;
            else ++blackout.pending_inferred;
            blackout.dark = false;
        }
    }
    if (!picture || !has_source)
        return;
    bool bright = expected.lit >= 4;
    bool matched = front >= 0 ? fb[front].hash == expected.hash
                              : fb[0].hash == expected.hash && fb[1].hash == expected.hash;
    if (bright && dark)
        ++blackout.stats.dark_at_picture;
    if (bright && matched && blackout.anchored) {
        ++blackout.stats.pairs;
        blackout.stats.confirmed += blackout.pending_confirmed;
        blackout.stats.inferred += blackout.pending_inferred;
        if (blackout.pending_longest > blackout.stats.longest_us)
            blackout.stats.longest_us = blackout.pending_longest;
    }
    blackout.anchored = bright && matched;
    blackout.previous_hash = expected.hash;
    blackout.dark = false;
    blackout.pending_confirmed = blackout.pending_inferred = 0;
    blackout.pending_longest = 0;
}

bool media_panel_blackout_stats(uint32_t value, media_blackout_stats_t *out)
{
    if (!out || !current(value) || live_screen || atomic_load(&timed_epoch))
        return false;
    *out = blackout.stats;
    return true;
}
#endif

static void reset_live_rate(void)
{
    live_stats.rate_start = 0;
    live_stats.rate_frames = 0;
    live_stats.fps_tenths = 0;
}

static void refreshed(lv_event_t *event)
{
    if (!screen || lv_screen_active() != screen)
        return;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (atomic_load(&prepared_playing) && !atomic_load(&timed_epoch)) {
        bool flush = lv_event_get_code(event) == LV_EVENT_FLUSH_FINISH;
        blackout_sample(flush && lv_display_flush_is_last(lv_event_get_target(event)),
                        !flush && did_flush && flush_pending);
    }
#endif
    if (!flush_pending)
        return;
    if (lv_event_get_code(event) == LV_EVENT_FLUSH_FINISH) {
        // A picture can span many partial flushes. The final part completes
        // the display transaction before REFR_READY timestamps this picture.
        did_flush = lv_display_flush_is_last(lv_event_get_target(event));
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (synth.packet && synth.drawing > 2 && did_flush)
            synth.last_flush = esp_timer_get_time();
#endif
    } else if (did_flush) {
        int64_t now = esp_timer_get_time();
        if (!refresh_count) {
            first_refresh = now;
            longest_refresh_interval = 0;
        } else {
            int64_t gap = now - last_refresh;
            intervals[(refresh_count - 1) % REFRESH_INTERVAL_CAPACITY] =
                gap > UINT32_MAX ? UINT32_MAX : (uint32_t)gap;
            if (gap > longest_refresh_interval)
                longest_refresh_interval = gap;
        }
        last_refresh = now;
        ++refresh_count;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (atomic_load(&prepared_playing) && !atomic_load(&timed_epoch)) {
            int slot = atomic_load(&shown);
            unsigned w = widths[slot], h = heights[slot];
            unsigned offset = lv_obj_get_y(image) * 1024 + lv_obj_get_x(image);
            const lv_image_header_t *geometry = &descriptors[slot].header;
            uint32_t expected = atomic_load(&prepared_crc);
            for (unsigned index = 0; index < 2; ++index) {
                const uint16_t *fb = board_lcd_fb(index);
                if (!fb) continue;
                uint32_t actual = 0;
                for (unsigned y = 0; y < h; ++y) {
                    const uint16_t *row = fb + offset + projected(y, h, geometry->h) * 1024;
                    if (geometry->w == w) {
                        actual = esp_rom_crc32_le(actual, (const uint8_t *)row, w * 2);
                    } else {
                        uint16_t samples[32];
                        for (unsigned x = 0; x < w; x += 32) {
                            unsigned n = LV_MIN(32, w - x);
                            for (unsigned j = 0; j < n; ++j)
                                samples[j] = row[projected(x + j, w, geometry->w)];
                            actual = esp_rom_crc32_le(actual, (const uint8_t *)samples, n * 2);
                        }
                    }
                }
                if (actual == expected) {
                    atomic_fetch_add(&prepared_verified, 1);
                    break;
                }
            }
        }
#endif
        int slot = atomic_load(&shown);
        bool current_picture = live_screen && !atomic_load(&frozen) && picture_revision[slot] == atomic_load(&freeze_revision);
        if (live_screen)
            kvmbridge_frame_done(atomic_load(&epoch), picture_sequence[slot], current_picture);
        if (current_picture) {
            int64_t received = picture_rx_us[slot];
            if (live_stats.fresh_since && received - live_stats.fresh_since >= LIVE_STALE_US)
                reset_live_rate();
            live_stats.crop = picture_crop[slot];
            live_stats.rx_to_lcd_ms = (unsigned)((now - received + 500) / 1000);
            live_stats.fresh_since = received;
            live_stats.awaiting_frame = false;
            if (!live_stats.rate_start) {
                live_stats.rate_start = now;
            } else {
                ++live_stats.rate_frames;
                int64_t elapsed = now - live_stats.rate_start;
                if (elapsed >= 1000000) {
                    live_stats.fps_tenths = (unsigned)((live_stats.rate_frames * INT64_C(10000000) + elapsed / 2) / elapsed);
                    live_stats.rate_frames = 0;
                    live_stats.rate_start = now;
                }
            }
        } else if (!live_screen) {
            const lv_image_header_t *header = &descriptors[slot].header;
            file_width = header->w;
            file_height = header->h;
            int64_t pts = file_slot_pts[slot];
            if (refresh_count == 1)
                first_file_pts = pts;
            else if (pts <= last_file_pts)
                first_file_pts = INT64_MIN;
            last_file_pts = pts;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (timed_selected_at) {
                timed_ready_at = now;
                timed_display_us += now - timed_selected_at;
                ++timed_display_frames;
                timed_selected_at = 0;
            }
#endif
            atomic_store(&file_refresh_us, (uint32_t)file_now());
            atomic_fetch_add(&file_flushed, 1);
        }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        if (synth.packet && slot >= 0)
            record_phases(picture_sequence[slot], now);
        synth.flushed = synth.drawing;
#endif
        flush_pending = false;
        did_flush = false;
    }
}

bool media_panel_live_stats(unsigned *frames, unsigned *fps_tenths, unsigned *rx_to_lcd_ms)
{
    bool live = screen && live_screen;
    *frames = live ? refresh_count : 0;
    *fps_tenths = live && atomic_load(&epoch) ? live_stats.fps_tenths : 0;
    *rx_to_lcd_ms = live ? live_stats.rx_to_lcd_ms : 0;
    return live;
}

unsigned media_panel_file_frames(uint32_t value)
{
    return !live_screen && current(value) ? refresh_count : 0;
}

bool media_panel_file_stats(uint32_t value, media_panel_file_stats_t *out)
{
    if (!out)
        return false;
    *out = (media_panel_file_stats_t){0};
    if (live_screen || !current(value) || atomic_load(&busy) || atomic_load(&file_result) != KVM_PLAY_OK)
        return false;
    out->epoch = value;
    out->frames = refresh_count;
    out->width = file_width;
    out->height = file_height;
    if (refresh_count > 1) {
        out->window_us = (uint64_t)(last_refresh - first_refresh);
        if (first_file_pts != INT64_MIN)
            out->media_window_us = (uint64_t)last_file_pts - (uint64_t)first_file_pts;
    }
    return true;
}

static uint64_t average_fps_hundredths(uint32_t frames, uint64_t span_us)
{
    uint64_t intervals = (uint64_t)(frames - 1) * 100000000;
    return (intervals + span_us / 2) / span_us;
}

void media_panel_file_summary(char *out, size_t capacity, const char *filename,
                              kvm_h264_reconstruction_t mode, const media_panel_file_stats_t *stats)
{
    static const char *const names[] = {
        [KVM_H264_EXACT] = "Exact", [KVM_H264_REDUCED] = "Reduced",
        [KVM_H264_LARGE_BANK] = "Large bank", [KVM_H264_RR_BLEND] = "Temporal blend",
        [KVM_H264_RR_SMART_LOOSE] = "Smart loose"
    };
    const char *name = (unsigned)mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "Unknown mode";
    if (!stats || !stats->frames) {
        snprintf(out, capacity, "%s\n%s", filename, name);
        return;
    }
    char timing[112];
    if (stats->frames >= 2 && stats->window_us) {
        uint64_t rate = average_fps_hundredths(stats->frames, stats->window_us);
        uint64_t ms = (stats->window_us + 500) / 1000;
        int n = snprintf(timing, sizeof(timing), "%u frames displayed in %" PRIu64 ".%03u s\nLCD %" PRIu64 ".%02u fps",
                 (unsigned)stats->frames, ms / 1000, (unsigned)(ms % 1000), rate / 100, (unsigned)(rate % 100));
        if (stats->media_window_us && n > 0 && (size_t)n < sizeof(timing)) {
            rate = average_fps_hundredths(stats->frames, stats->media_window_us);
            snprintf(timing + n, sizeof(timing) - (size_t)n, "   |   Source %" PRIu64 ".%02u fps",
                     rate / 100, (unsigned)(rate % 100));
        }
    } else {
        snprintf(timing, sizeof(timing), "%u frame%s displayed   |   fps unavailable", (unsigned)stats->frames,
                 stats->frames == 1 ? "" : "s");
    }
    snprintf(out, capacity, "%s\n%s   |   Displayed at %u x %u\n%s",
             filename, name, stats->width, stats->height, timing);
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static void report_phases(void)
{
    unsigned samples = synth.valid < PHASE_CAPACITY ? synth.valid : PHASE_CAPACITY;
    const char *codec = synth.codec == KVM_LIVE_JPEG ? "JPEG" : synth.codec == KVM_LIVE_H264_INTRA ? "H264" : "Cinepak";
    ESP_LOGI("media", "MEDIA_SYNTHETIC_PHASES codec=%s accepted=%u valid=%u invalid=%u samples=%u capacity=%u",
             codec, synth.sent > 2 ? synth.sent - 2 : 0, (unsigned)synth.valid, (unsigned)synth.invalid,
             samples, (unsigned)PHASE_CAPACITY);
    if (!samples) return;
    unsigned medians[PHASE_COUNT];
    for (unsigned i = 0; i < PHASE_COUNT; ++i) {
        qsort(synth.durations[i], samples, sizeof(uint32_t), compare_u32);
        medians[i] = synth.durations[i][(samples - 1) / 2];
    }
    ESP_LOGI("media", "MEDIA_SYNTHETIC_PHASE_P50_US queue=%u decode=%u handoff=%u select_to_flush=%u "
             "flush_to_refresh=%u total=%u", medians[0], medians[1], medians[2], medians[3], medians[4], medians[5]);
}
#endif

static void free_buffers(void)
{
    // The UI has detached both image descriptors and the producer has exited.
    xSemaphoreTake(gate, portMAX_DELAY);
    memset(&queue, 0, sizeof(queue));
    memset(packet_crop, 0, sizeof(packet_crop));
    memset(picture_crop, 0, sizeof(picture_crop));
    for (int i = 0; i < 2; ++i) {
        lv_image_cache_drop(&descriptors[i]);
        descriptors[i].data = NULL;
        kvm_media_h264_release(&retained[i]);
        if (live_screen && surfaces[i])
            kvm_media_zeroize(surfaces[i], surface_bytes);
        if (encoded[i])
            kvm_media_zeroize(encoded[i], KVM_LIVE_MAX_PAYLOAD + (in_place ? KVM_LIVE_HEADER : 0));
        free(surfaces[i]);
        surfaces[i] = NULL;
        free(encoded[i]);
        encoded[i] = NULL;
    }
    kvm_media_decoder_close(decoder);
    decoder = NULL;
    in_place = false;
    xSemaphoreGive(gate);
}

static void dispose(void)
{
    free_buffers();
    lv_timer_delete(timer);
    timer = NULL;
}

void media_panel_close(void)
{
    if (!screen) {
        if (timer && receiver_released())
            dispose();
        return;
    }
    atomic_store(&epoch, 0);
    if (holding_scanout) {
        lv_display_enable_invalidation(lv_obj_get_display(screen), true);
        holding_scanout = false;
    }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    bool offline = synth.active;
    if (timed_display_frames)
        ESP_LOGI("media", "CARD_DISPLAY_STAGES epoch=%u frames=%u selected_to_ready_us=%" PRId64,
                 atomic_load(&timed_epoch), timed_display_frames, timed_display_us);
    if (atomic_exchange(&phase_epoch, 0))
        report_phases();
    if (blackout.stats.samples) {
        ESP_LOGI("media", "CARD_BLACKOUT samples=%u pairs=%u confirmed=%u inferred=%u ambiguous_samples=%u "
                 "dark_at_picture=%u resyncs=%u longest_us=%" PRIu64,
                 (unsigned)blackout.stats.samples, (unsigned)blackout.stats.pairs, (unsigned)blackout.stats.confirmed,
                 (unsigned)blackout.stats.inferred, (unsigned)blackout.stats.ambiguous_samples,
                 (unsigned)blackout.stats.dark_at_picture, (unsigned)blackout.stats.resyncs, blackout.stats.longest_us);
    }
    synth.active = false;
    if (synth.paced)
        finish_capture(atomic_load(&wire_epoch));
    else
        free(synth.packet);
    free(synth.pressure);
    synth.pressure = NULL;
    synth.paced = false;
    synth.packet = NULL;
#else
    bool offline = false;
#endif
    if (live_screen && !offline)
        kvmbridge_stop();
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refreshed, NULL);
    if (previous && lv_screen_active() == screen)
        lv_screen_load(previous);
    lv_obj_delete(screen);
    screen = previous = image = status = controls = freeze_btn = monitor_btn = crop_info = telemetry_status = NULL;
    settings_card = review_card = review_prompt = approve_display = modes = formats = NULL;
    preview_frame = preview_source = preview_output = NULL;
    file_stopped = NULL;
    file_title = file_position = file_bar = pause_btn = fit_btn = native_btn = NULL;
    memset(crop_text, 0, sizeof(crop_text));
    live_stats.crop = (kvmbridge_crop_t){0};
    board_lcd_stats_t stats;
    board_lcd_get_stats(&stats);
    unsigned total = refresh_count ? refresh_count - 1 : 0;
    unsigned n = total < REFRESH_INTERVAL_CAPACITY ? total : REFRESH_INTERVAL_CAPACITY;
    qsort(intervals, n, sizeof(intervals[0]), compare_u32);
    ESP_LOGI("media", "refresh_frames=%u window_us=%lld p50_us=%lu p95_us=%lu p99_us=%lu vsync=%lu"
                      " interval_samples=%u interval_total=%u max_gap_us=%lld",
             refresh_count, (long long)(last_refresh - first_refresh),
             (unsigned long)(n ? intervals[(n * 50 + 99) / 100 - 1] : 0),
             (unsigned long)(n ? intervals[(n * 95 + 99) / 100 - 1] : 0),
             (unsigned long)(n ? intervals[(n * 99 + 99) / 100 - 1] : 0),
             (unsigned long)(stats.vsync_count - vsync_first), n, total, (long long)(total ? longest_refresh_interval : 0));
    flush_pending = false;
    if (receiver_released())
        dispose();
}

static void leave(void)
{
    void (*back)(void) = navigate;
    media_panel_close();
    if (back)
        back();
}

static void leave_button(lv_event_t *e)
{
    (void)e;
    leave();
}

static void stop_file_button(lv_event_t *event)
{
    (void)event;
    void (*after)(void) = file_stopped ? file_stopped : navigate;
    media_panel_close();
    if (after) after();
}

void media_panel_file_stop_action(uint32_t value, void (*stopped)(void))
{
    if (current(value) && !live_screen)
        file_stopped = stopped;
}

static lv_result_t banded_info(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *d, lv_image_header_t *header)
{
    (void)dec;
    if (d->src != &descriptors[0] && d->src != &descriptors[1])
        return LV_RESULT_INVALID;
    const lv_image_dsc_t *source = d->src;
    if (source->header.cf != LV_COLOR_FORMAT_RAW ||
        (live_screen && source->data_size != sizeof(kvm_cvid_index_t) && source->data_size != sizeof(kvm_h264_picture_t)))
        return LV_RESULT_INVALID;
    *header = source->header;
    header->cf = LV_COLOR_FORMAT_RGB565;
    header->stride = header->w * 2;
    return LV_RESULT_OK;
}

static void *alloc_cinepak_band(size_t size, lv_color_format_t format)
{
    (void)format;
    size += LV_DRAW_BUF_ALIGN - 1;
#if CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
    // Leave the configured DMA/driver reserve for the active RGB, Wi-Fi and USB drivers.
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < size + CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL) {
        void *external = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (external) return external;
    }
#endif
    // At this size the product's IDF malloc prefers internal memory and can fall back to PSRAM.
    return malloc(size);
}

static lv_result_t banded_open(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *d)
{
    if (banded_info(dec, d, &d->header) != LV_RESULT_OK)
        return LV_RESULT_INVALID;
    d->args.no_cache = true;
    // Each LVGL software draw unit owns its own strip. No writes reach a
    // scanout buffer until the normal display adapter flushes its draw buffer.
    const lv_draw_buf_handlers_t *handlers = !live_screen || d->header.w == KVM_CVID_W ? &cinepak_band_handlers : lv_draw_buf_get_handlers();
    d->user_data = lv_draw_buf_create_ex(handlers, d->header.w, 4, LV_COLOR_FORMAT_RGB565, d->header.stride);
    return d->user_data ? LV_RESULT_OK : LV_RESULT_INVALID;
}

static void scale_file_row(uint16_t *dst, const uint16_t *src, unsigned width, unsigned output_width)
{
    if (output_width == 3 * width) {
        for (unsigned x = 0; x < width; ++x, dst += 3)
            dst[0] = dst[1] = dst[2] = src[x];
    } else if (output_width == 2 * width) {
        for (unsigned x = 0; x < width; ++x, dst += 2)
            dst[0] = dst[1] = src[x];
    } else if (output_width == 6 * width) {
        for (unsigned x = 0; x < width; ++x, dst += 6)
            dst[0] = dst[1] = dst[2] = dst[3] = dst[4] = dst[5] = src[x];
    } else {
        unsigned phase = 0;
        for (unsigned x = 0; x < output_width; ++x) {
            dst[x] = *src;
            phase += width;
            if (phase >= output_width) { phase -= output_width; ++src; }
        }
    }
}

static lv_result_t banded_area(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *d,
                             const lv_area_t *request, lv_area_t *area)
{
    (void)dec;
    const lv_image_dsc_t *source = d->src;
    lv_draw_buf_t *band = d->user_data;
    if (!live_screen) {
        unsigned slot = source == &descriptors[1], w = widths[slot], h = heights[slot];
        unsigned dw = d->header.w, dh = d->header.h;
        bool first = area->y1 == LV_COORD_MIN;
        int y = first ? LV_MAX(0, request->y1) : area->y2 + 1;
        int last = LV_MIN(request->y2, (int)dh - 1);
        if (y > last) return LV_RESULT_INVALID;
        last = LV_MIN(last, y + 3);
        const uint16_t *pixels = (const uint16_t *)source->data;
        unsigned previous_y = UINT_MAX;
        const uint16_t *previous = NULL;
        if (!first) {
            // LVGL returns this decoder's previous area with the corresponding pixels still in its strip.
            previous_y = (unsigned)area->y2 * h / dh;
            previous = (const uint16_t *)(band->data + (area->y2 - area->y1) * band->header.stride);
        }
        for (int row = y; row <= last; ++row) {
            unsigned sy = (unsigned)row * h / dh;
            uint16_t *dst = (uint16_t *)(band->data + (row - y) * band->header.stride);
            if (sy == previous_y) {
                if (previous != dst) memcpy(dst, previous, dw * sizeof(*dst));
            } else {
                scale_file_row(dst, pixels + sy * w, w, dw);
            }
            previous = dst;
            previous_y = sy;
        }
        *area = (lv_area_t){0, y, dw - 1, last};
        d->decoded = band;
        return LV_RESULT_OK;
    }
    int y = area->y1 == LV_COORD_MIN ? request->y1 / 4 * 4 : area->y1 + 4;
    if (y > request->y2 || y >= d->header.h || y < 0)
        return LV_RESULT_INVALID;
    bool ok = source->data_size == sizeof(kvm_h264_picture_t)
                  ? kvm_media_h264_band((const void *)source->data, y, (void *)band->data)
                  : kvm_cvid_index_band((const void *)source->data, y, (void *)band->data);
    if (!ok)
        return LV_RESULT_INVALID;
    *area = (lv_area_t){0, y, d->header.w - 1, y + 3};
    d->decoded = band;
    return LV_RESULT_OK;
}

static void banded_close(lv_image_decoder_t *dec, lv_image_decoder_dsc_t *d)
{
    (void)dec;
    lv_draw_buf_t *band = d->user_data;
    if (band) {
        kvm_media_zeroize(band->data, (size_t)band->header.stride * band->header.h);
        lv_draw_buf_destroy(band);
    }
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static uint32_t framebuffer_crc(unsigned index)
{
    const uint16_t *fb = board_lcd_fb(index);
    uint32_t crc = 0;
    if (fb) {
        bool wide = synth.codec == KVM_LIVE_CINEPAK_INTRA;
        unsigned height = wide ? KVM_CVID_H : 512;
        unsigned width = wide ? KVM_CVID_W : 512;
        fb += wide ? 46 * 1024 + 32 : 60 * 1024 + 24;
        for (unsigned y = 0; y < height; ++y, fb += 1024)
            crc = esp_rom_crc32_le(crc, (const uint8_t *)fb, width * 2);
    }
    return crc;
}

static void synthetic_tick(void)
{
    static const uint32_t cvid_crc[] = {0xb3d32c6e, 0x1b25dd0e};
    bool jpeg = synth.codec == KVM_LIVE_JPEG;
    bool h264 = synth.codec == KVM_LIVE_H264_INTRA;
    const uint32_t *expected = jpeg ? synthetic_jpeg_crc : h264 ? synthetic_idr_crc : cvid_crc;
    int64_t now = esp_timer_get_time();
    if (now < synth.next)
        return;
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    if (now >= synth.deadline || !atomic_load(&epoch) ||
        (synth.flushed == synth.frames + 2 && synth.verified == 2)) {
        bool complete = synth.verified == 2 && synth.flushed == synth.frames + 2 && refresh_count &&
                        st.vsync_count != synth.vsync && st.bounce_frames != synth.bounce;
        ESP_LOGI("media", "MEDIA_SYNTHETIC result=%s verified=%u sent=%u measured_refresh=%u vsync=%lu bounce=%lu",
                 complete ? "PASS" : "INCONCLUSIVE", synth.verified, synth.sent, refresh_count,
                 (unsigned long)(st.vsync_count - synth.vsync), (unsigned long)(st.bounce_frames - synth.bounce));
        leave();
        return;
    }
    if (synth.sent > synth.verified && synth.verified < 2) {
        if (synth.flushed != synth.sent || st.vsync_count == synth.vsync || st.bounce_frames == synth.bounce)
            return;
        unsigned phase = synth.verified;
        uint32_t a = framebuffer_crc(0), b = framebuffer_crc(1);
        ESP_LOGI("media", "MEDIA_SYNTHETIC phase=%u expected=%08lx fb0=%08lx fb1=%08lx",
                 phase, (unsigned long)expected[phase], (unsigned long)a, (unsigned long)b);
        synth.next = now + 250000;
        if (a != expected[phase] && b != expected[phase])
            return;
        ++synth.verified;
        synth.vsync = st.vsync_count;
        synth.bounce = st.bounce_frames;
        if (synth.verified == 2) {
            first_refresh = last_refresh = 0;
            refresh_count = 0;
            synth.deadline = now + INT64_C(5000000) + (int64_t)synth.frames * 1000000 / synth.rate;
        }
        return;
    }
    if (synth.sent >= synth.frames + 2)
        return;
    if (synth.paced && (synth.sent != synth.flushed || !packet_writable(atomic_load(&epoch))))
        return;
    uint8_t *packet = synth.packet;
    uint32_t seq = synth.sent + 1;
    uint32_t timestamp = (seq - 1) * 1000 / synth.rate;
    memcpy(packet + 12, &seq, 4);
    memcpy(packet + 16, &timestamp, 4);
    uint32_t length = SYNTH_PAYLOAD;
    if (jpeg || h264) {
        const uint8_t *source;
        if (jpeg) {
            source = seq % 2 ? synthetic_jpeg_0 : synthetic_jpeg_1;
            length = seq % 2 ? sizeof(synthetic_jpeg_0) : sizeof(synthetic_jpeg_1);
        } else {
            source = seq % 2 ? synthetic_idr_0 : synthetic_idr_1;
            length = seq % 2 ? sizeof(synthetic_idr_0) : sizeof(synthetic_idr_1);
        }
        memcpy(packet + KVM_LIVE_HEADER, source, length);
        memcpy(packet + 24, &length, 4);
    } else {
        for (unsigned i = 0; i < 8; ++i) {
            unsigned level = ((i / 4 + seq) & 1) ? i % 4 : 3 - i % 4;
            packet[KVM_LIVE_HEADER + 26 + i] = (uint8_t)(32 + 64 * level);
        }
    }
    uint32_t crc = esp_rom_crc32_le(esp_rom_crc32_le(0, packet, 28), packet + KVM_LIVE_HEADER, length);
    memcpy(packet + 28, &crc, 4);
    if (media_panel_stream_push(atomic_load(&epoch), packet, KVM_LIVE_HEADER + length)) {
        synth.sent = seq;
        int64_t period = 1000000 / synth.rate;
        // Keep the synthetic producer's clock independent of the LVGL timer's
        // wake phase. Advance past a missed deadline without enqueuing a burst.
        if (!synth.next)
            synth.next = now;
        synth.next += ((now - synth.next) / period + 1) * period;
    }
}
#endif

static void show_crop_details(bool visible)
{
    const kvmbridge_crop_t *c = &live_stats.crop;
    // The 512-square view has a separate information column; the wide video fills the panel.
    bool show = visible && c->width && stream_codec != KVM_LIVE_CINEPAK_INTRA;
    lv_obj_set_hidden(crop_info, !show);
    if (!show) return;
    char text[sizeof(crop_text)];
    const char *const labels[] = {"Overview", "Pixel 1:1", "Zoom 2x"};
    unsigned mode = lv_dropdown_get_selected(modes);
    snprintf(text, sizeof(text), "%s\nCursor (%" PRId32 ", %" PRId32 ")\nSource (%" PRId32 ", %" PRId32
             ")  %u x %u px\nPadding left %u, top %u px",
             mode < 3 ? labels[mode] : "", c->cursor_x, c->cursor_y, c->left, c->top,
             c->width, c->height, c->pad_x, c->pad_y);
    if (strcmp(crop_text, text)) {
        strcpy(crop_text, text);
        lv_label_set_text_static(crop_info, crop_text);
    }
}

enum { FILE_VIEW_WIDTH = 1024, FILE_VIEW_HEIGHT = 540, FILE_VIEW_TOP = 46 };

static void place_file_picture(int slot)
{
    unsigned w = widths[slot], h = heights[slot], dw = w, dh = h;
    if (file_fit) {
        if (FILE_VIEW_WIDTH * h <= FILE_VIEW_HEIGHT * w) {
            dw = FILE_VIEW_WIDTH;
            dh = LV_MAX(1u, h * dw / w);
        } else {
            dh = FILE_VIEW_HEIGHT;
            dw = LV_MAX(1u, w * dh / h);
        }
    }
    bool scaled = dw != w || dh != h;
    lv_image_dsc_t *src = &descriptors[slot];
    lv_image_cache_drop(src);
    *src = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .w = dw, .h = dh,
                   .cf = scaled ? LV_COLOR_FORMAT_RAW : LV_COLOR_FORMAT_RGB565, .stride = scaled ? 0 : w * 2},
        .data_size = surface_bytes, .data = (const uint8_t *)surfaces[slot]};
    lv_image_set_src(image, src);
    lv_obj_set_pos(image, (FILE_VIEW_WIDTH - dw) / 2, FILE_VIEW_TOP + (FILE_VIEW_HEIGHT - dh) / 2);
    lv_obj_invalidate(image);
}

static void update_file_position(void)
{
    if (position_frame == refresh_count) return;
    position_frame = refresh_count;
    unsigned total = atomic_load(&file_total);
    char text[64];
    int64_t elapsed = first_file_pts != INT64_MIN && last_file_pts >= first_file_pts ?
                      (last_file_pts - first_file_pts) / 1000000 : 0;
    if (total)
        snprintf(text, sizeof(text), "%lld:%02lld   %u / %u", (long long)(elapsed / 60),
                 (long long)(elapsed % 60), refresh_count, total);
    else if (elapsed)
        snprintf(text, sizeof(text), "%lld:%02lld   Frame %u", (long long)(elapsed / 60),
                 (long long)(elapsed % 60), refresh_count);
    else
        snprintf(text, sizeof(text), "Frame %u", refresh_count);
    if (strcmp(lv_label_get_text(file_position), text))
        lv_label_set_text(file_position, text);
    lv_obj_set_hidden(file_bar, !total);
    if (total)
        lv_bar_set_value(file_bar, (int32_t)((uint64_t)refresh_count * 1000 / total), LV_ANIM_OFF);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (!screen) {
        if (receiver_released())
            dispose();
        return;
    }
    if (lv_screen_active() != screen || (!atomic_load(&epoch) && !live_screen)) {
        leave();
        return;
    }
    uint32_t value = atomic_load(&epoch);
    char display[64], message[160];
    kvmbridge_display_state_t display_state = KVB_DISPLAY_NONE;
    int64_t age = 0;
    if (value && live_screen && requested) {
        display_state = kvmbridge_display(value, display);
        if (display_state == KVB_DISPLAY_SHARING && !atomic_load(&frozen)) {
            int64_t now = esp_timer_get_time();
            if (!live_stats.fresh_since) live_stats.fresh_since = now;
            age = now - live_stats.fresh_since;
            if (age >= LIVE_STALE_US && live_stats.rate_start)
                reset_live_rate();
            if (age >= KVB_STREAM_TIMEOUT_US) {
                kvmbridge_capture_frame_timeout(value);
                media_panel_stream_end(value);
                kvmbridge_stop_display(capture_profile,value);
            }
        }
    }
    if (!atomic_load(&epoch)) {
        if (image) {
            lv_obj_delete(image);
            image = NULL;
            flush_pending = false;
            lv_obj_set_hidden(crop_info, true);
            memset(crop_text, 0, sizeof(crop_text));
            live_stats.crop = (kvmbridge_crop_t){0};
            lv_obj_set_hidden(freeze_btn, true);
            lv_obj_set_hidden(controls, true);
            lv_obj_set_hidden(monitor_btn, true);
            if (changing_monitor || !requested)
                lv_label_set_text_static(status, changing_monitor ? "Closing the previous display" : "Screen capture unavailable");
        }
        // The decoder can invalidate the panel epoch just before its worker publishes the final reason.
        if (requested && !changing_monitor) {
            const char *reason = kvmbridge_activity().capture_text;
            if (strcmp(lv_label_get_text(status), reason))
                lv_label_set_text_static(status, reason);
        }
        if (receiver_released() && (surfaces[0] || encoded[0]))
            free_buffers();
        if ((requested || changing_monitor) && receiver_released() &&
            kvmbridge_display(generation, display) == KVB_DISPLAY_NONE) {
            if (changing_monitor) {
                requested = false;
                changing_monitor = 0;
                lv_label_set_text_static(status, "Select Start sharing to choose a display");
            }
            lv_obj_set_hidden(review_card, true);
            lv_obj_set_hidden(settings_card, false);
            lv_obj_set_hidden(controls, false);
        }
        return;
    }
    if (atomic_load(&retire_surface) && !atomic_load(&file_paused) && !flush_pending && atomic_load(&pending) < 0) {
        // Keep the previous hardware scanout visible until the next complete
        // frame. Unrelated UI invalidations still follow the normal LVGL path.
        lv_display_t *display = lv_obj_get_display(image);
        holding_scanout = atomic_load(&file_total) != 0;
        // Flush pending transport changes with the old intact picture. A full
        // redraw during an in-place card read would paint the empty viewport.
        if (holding_scanout) lv_refr_now(display);
        lv_display_enable_invalidation(display, false);
        lv_image_set_src(image, NULL);
        if (!holding_scanout) lv_display_enable_invalidation(display, true);
        lv_image_cache_drop(&descriptors[0]);
        atomic_store(&shown, -1);
        atomic_store(&retire_surface, false);
    }
    int slot = atomic_load(&pending);
    if (slot >= 0 && !flush_pending && (live_screen || holding_scanout || !atomic_load(&file_paused))) {
        if (holding_scanout) {
            lv_display_enable_invalidation(lv_obj_get_display(screen), true);
            holding_scanout = false;
            lv_obj_invalidate(screen);
        }
        if (!atomic_load(&frozen) &&
            (!live_screen || picture_revision[slot] == atomic_load(&freeze_revision))) {
            int w = widths[slot], h = heights[slot];
            bool h264 = live_screen && stream_codec == KVM_LIVE_H264_INTRA;
            bool banded = h264 || (live_screen && stream_codec == KVM_LIVE_CINEPAK_INTRA);
            lv_image_dsc_t *source = &descriptors[slot];
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (atomic_load(&timed_epoch) == value && atomic_load(&prepared_playing))
                timed_selected_at = esp_timer_get_time();
#endif
            if (live_screen) {
                lv_image_cache_drop(source);
                *source = (lv_image_dsc_t){
                    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .w = w, .h = h,
                               .cf = banded ? LV_COLOR_FORMAT_RAW : LV_COLOR_FORMAT_RGB565,
                               .stride = banded ? 0 : w * 2},
                    .data_size = h264 ? sizeof(retained[slot]) : surface_bytes,
                    .data = h264 ? (const uint8_t *)&retained[slot] : (const uint8_t *)surfaces[slot]};
                lv_image_set_src(image, source);
                lv_obj_invalidate(image);
                lv_obj_set_pos(image, w == 512 ? 24 : (1024 - w) / 2, 32 + (568 - h) / 2);
            } else {
                place_file_picture(slot);
            }
            atomic_store(&shown, slot);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            synth.drawing = picture_sequence[slot];
            if (synth.packet && synth.drawing > 2) {
                synth.selected_picture = phase_picture[slot];
                synth.selected_at = esp_timer_get_time();
                synth.last_flush = 0;
            }
#endif
            flush_pending = true;
        } else if (live_screen) {
            kvmbridge_frame_done(value, picture_sequence[slot], false);
        }
        atomic_store(&pending, -1);
    }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (synth.packet) {
        synthetic_tick();
        return;
    }
#endif
    if (live_screen && requested) {
        bool review = display_state == KVB_DISPLAY_REVIEW;
        kvmbridge_activity_t activity = {0};
        if (display_state != KVB_DISPLAY_SHARING && !review)
            activity = kvmbridge_activity();
        bool waiting = display_state == KVB_DISPLAY_NONE && activity.capture_code &&
                       (!strcmp(activity.capture_code, "awaiting_host") || !strcmp(activity.capture_code, "awaiting_picker") ||
                        !strcmp(activity.capture_code, "awaiting_pointer"));
        lv_obj_set_hidden(controls, !(review || waiting));
        bool sharing = display_state == KVB_DISPLAY_SHARING;
        lv_obj_t *monitor_label = lv_obj_get_child(monitor_btn, 0);
        const char *monitor_action = sharing ? "Monitor" : "Settings";
        if (strcmp(lv_label_get_text(monitor_label), monitor_action))
            lv_label_set_text_static(monitor_label, monitor_action);
        lv_obj_set_hidden(monitor_btn, false);
        show_crop_details(display_state == KVB_DISPLAY_SHARING && !live_stats.awaiting_frame);
        if (review || waiting) {
            if (review)
                snprintf(message, sizeof(message), "Allow capture of %.63s?", display);
            else if (display[0] && !strcmp(activity.capture_code, "awaiting_pointer"))
                snprintf(message, sizeof(message), "Move the pointer onto %s", display + sizeof("\\\\.\\") - 1);
            else
                snprintf(message, sizeof(message), "%s", activity.capture_text);
            if (strcmp(lv_label_get_text(review_prompt), message)) {
                lv_label_set_text(review_prompt, message);
                lv_obj_align(review_prompt, review ? LV_ALIGN_TOP_LEFT : LV_ALIGN_LEFT_MID, 0, 0);
            }
            lv_obj_set_hidden(approve_display, !review);
            lv_obj_set_hidden(settings_card, true);
            lv_obj_set_hidden(review_card, false);
            lv_obj_set_hidden(freeze_btn, true);
            strcpy(message, "Cursor magnifier");
        } else if (display_state == KVB_DISPLAY_SHARING) {
            bool visible = atomic_load(&shown) >= 0;
            const char *edge = visible && live_stats.crop.clamped ? "Edge clamped | " : "";
            lv_obj_set_hidden(freeze_btn, !visible);
            if (atomic_load(&frozen))
                snprintf(message, sizeof(message), "CAPTURE  |  Frozen  |  %s%s", edge, display);
            else if (visible && age >= LIVE_STALE_US)
                snprintf(message, sizeof(message), "CAPTURE  |  Last frame stale  |  %s%s", edge, display);
            else if (!visible || live_stats.awaiting_frame || !refresh_count)
                snprintf(message, sizeof(message), "CAPTURE  |  Waiting for a frame  |  %s", display);
            else if (live_stats.fps_tenths)
                snprintf(message, sizeof(message), "CAPTURE | %s%s | %u.%u fps | RX to LCD %u ms",
                         edge, display, live_stats.fps_tenths / 10, live_stats.fps_tenths % 10, live_stats.rx_to_lcd_ms);
            else
                snprintf(message, sizeof(message), "CAPTURE | %s%s | RX to LCD %u ms", edge, display, live_stats.rx_to_lcd_ms);
        } else {
            lv_obj_set_hidden(freeze_btn, true);
            snprintf(message, sizeof(message), "%s", activity.capture_text);
        }
        if (strcmp(lv_label_get_text(status), message))
            lv_label_set_text(status, message);
    } else if (!live_screen && atomic_load(&started)) {
        lv_obj_set_state(pause_btn, LV_STATE_DISABLED, !atomic_load(&busy));
        const char *caption = atomic_load(&file_paused) ? "Paused" : "Playing video";
        if (!atomic_load(&busy))
            caption = media_panel_play_message(atomic_load(&file_result));
        else if (!refresh_count)
            return;
        if (strcmp(lv_label_get_text(status), caption))
            lv_label_set_text_static(status, caption);
        update_file_position();
    }
}

static lv_obj_t *button(lv_obj_t *p, const char *title, int x, int y, int w, int h, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    ui_theme_control_button(b);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_font(b, ui_theme_font(UI_FONT_HEADING), 0);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text_static(label, title);
    lv_obj_center(label);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void toggle_file_pause(lv_event_t *event)
{
    (void)event;
    if (!screen || !atomic_load(&busy)) return;
    xSemaphoreTake(gate, portMAX_DELAY);
    bool pause = !atomic_load(&file_paused);
    int64_t now = esp_timer_get_time();
    if (pause) paused_at = now;
    else pause_total += now - paused_at;
    atomic_store(&file_paused, pause);
    xSemaphoreGive(gate);
    lv_label_set_text_static(lv_obj_get_child(pause_btn, 0), pause ? "Resume" : "Pause");
}

static void set_file_scale(bool fit)
{
    file_fit = fit;
    lv_obj_set_checked(fit_btn, fit);
    lv_obj_set_checked(native_btn, !fit);
    int slot = atomic_load(&shown);
    if (slot >= 0)
        place_file_picture(slot);
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
void media_panel_test_pixel_size(uint32_t value)
{
    if (current(value) && !live_screen && native_btn)
        set_file_scale(false);
}
#endif

static void choose_file_scale(lv_event_t *event)
{
    prefer_fit = lv_event_get_current_target(event) == fit_btn;
    set_file_scale(prefer_fit);
}

static void file_controls(void)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    ui_theme_apply(bar, UI_STYLE_CONTROL_PAGE, LV_PART_MAIN);
    lv_obj_set_size(bar, FILE_VIEW_WIDTH, FILE_VIEW_TOP);
    lv_obj_set_clickable(bar, false);
    lv_obj_set_scrollable(bar, false);
    lv_obj_move_to_index(bar, 1);
    lv_color_t ink = lv_obj_get_style_text_color(bar, LV_PART_MAIN);
    lv_obj_t *icon = ui_theme_control_mask(bar, &kvm_icon_video);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 9, 0);
    file_title = lv_label_create(screen);
    lv_obj_set_pos(file_title, 55, 1);
    lv_obj_set_width(file_title, 262);
    lv_label_set_long_mode(file_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(file_title, ink, 0);
    lv_obj_set_style_text_font(file_title, ui_theme_font(UI_FONT_HEADING), 0);
    lv_label_set_text_static(file_title, "Video player");
    lv_obj_set_pos(status, 55, 25);
    lv_obj_set_width(status, 262);
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_color(status, ink, 0);
    lv_obj_set_style_text_font(status, ui_theme_font(UI_FONT_LABEL), 0);
    file_position = lv_label_create(screen);
    lv_obj_set_pos(file_position, 326, 14);
    lv_obj_set_style_text_font(file_position, ui_theme_font(UI_FONT_BODY), 0);
    lv_obj_set_style_text_color(file_position, ink, 0);
    lv_label_set_text_static(file_position, "");
    pause_btn = button(screen, "Pause", 504, 0, 105, 44, toggle_file_pause);
    lv_obj_add_state(pause_btn, LV_STATE_DISABLED);
    fit_btn = button(screen, "Fit", 616, 0, 82, 44, choose_file_scale);
    native_btn = button(screen, "1:1", 700, 0, 82, 44, choose_file_scale);
    file_bar = lv_bar_create(screen);
    lv_obj_set_pos(file_bar, 12, 590);
    lv_obj_set_size(file_bar, 1000, 6);
    lv_obj_set_style_bg_color(file_bar, lv_color_hex(0x35424a), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(file_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(file_bar, ui_theme_control_palette() == UI_PALETTE_HIGH_CONTRAST ?
                              lv_color_white() : lv_color_hex(0x7de1d6), LV_PART_INDICATOR);
    lv_obj_set_clickable(file_bar, false);
    lv_bar_set_range(file_bar, 0, 1000);
    lv_obj_set_hidden(file_bar, true);
    file_fit = false;
    lv_obj_set_checked(native_btn, true);
}

void media_panel_file_title(uint32_t value, const char *filename)
{
    if (current(value) && !live_screen && file_title && filename) {
        lv_label_set_text(file_title, filename);
        set_file_scale(prefer_fit);
    }
}

static kvm_live_codec_t selected_codec(void)
{
    unsigned choice = lv_dropdown_get_selected(formats);
    if (choice == 0) return KVM_LIVE_JPEG;
    if (choice == 1 && kvm_media_has_h264()) return KVM_LIVE_H264_INTRA;
    return KVM_LIVE_CINEPAK_INTRA;
}

static void update_preview(void)
{
    unsigned mode = lv_dropdown_get_selected(modes);
    if (mode >= sizeof(crop_modes) / sizeof(crop_modes[0])) return;
    kvm_live_geometry_t g = kvm_live_geometry(selected_codec(), crop_modes[mode]);
    lv_label_set_text_fmt(preview_source, "%u x %u px", g.source_width, g.source_height);
    lv_label_set_text_fmt(preview_output, "KVMpad image   %u x %u px", g.image_width, g.image_height);
    // The 240x135 sample represents a 1920x1080 monitor.
    lv_obj_set_size(preview_frame, (g.source_width + 4) / 8, (g.source_height + 4) / 8);
    lv_obj_center(preview_frame);
}

static void preview_changed(lv_event_t *e)
{
    (void)e;
    update_preview();
}

static bool start_capture(void)
{
    unsigned mode = lv_dropdown_get_selected(modes);
    kvm_live_codec_t codec = selected_codec();
    if (mode >= sizeof(crop_modes) / sizeof(crop_modes[0]) || changing_monitor ||
        (requested && atomic_load(&epoch)))
        return false;
    if (!atomic_load(&epoch)) {
        // The controls reappear after teardown. Only a new local Start creates a fresh capture.
        requested = false;
        image = lv_image_create(screen);
        if (!image) {
            lv_label_set_text_static(status, "Insufficient memory for the magnifier");
            return false;
        }
        lv_obj_move_to_index(image, 0);
        atomic_store(&shown, -1);
        atomic_store(&pending, -1);
        atomic_store(&started, false);
        atomic_store(&frozen, false);
        atomic_store(&freeze_revision, 0);
        memset(&live_stats, 0, sizeof(live_stats));
        first_refresh = last_refresh = 0;
        refresh_count = 0;
        did_flush = false;
        new_epoch();
        lv_label_set_text_static(lv_obj_get_child(freeze_btn, 0), "Freeze");
    }
    kvmbridge_media_with_buffer(media_panel_stream_start, stream_push_with_crop, finish_capture,
                                (kvmbridge_receive_buffer_t){start_buffered_capture, packet_writable});
    if (kvmbridge_capture_paced(capture_profile, atomic_load(&epoch), codec, crop_modes[mode], 10)) {
        requested = true;
        if (!strcmp(capture_profile, magnifier_profile))
            magnifier_format = lv_dropdown_get_selected(formats);
        lv_obj_set_hidden(controls, true);
        return true;
    }
    lv_label_set_text_static(status, kvmbridge_activity().capture_text);
    return false;
}

static void begin_capture(lv_event_t *e)
{
    (void)e;
    start_capture();
}

bool media_panel_request_capture(uint32_t value)
{
    char endpoint[24];
    return value && value == atomic_load(&epoch) && screen && live_screen && lv_screen_active() == screen &&
           !strcmp(capture_profile, magnifier_profile) && kvmbridge_host_endpoint(endpoint) && start_capture();
}

static void permit(lv_event_t *e)
{
    (void)e;
    kvmbridge_allow_display(atomic_load(&epoch));
    lv_obj_set_hidden(controls, true);
}

static void freeze(lv_event_t *e)
{
    (void)e;
    bool pause = !atomic_load(&frozen);
    if (kvmbridge_freeze(atomic_load(&epoch), pause)) {
        atomic_store(&frozen, pause);
        atomic_fetch_add(&freeze_revision, 1);
        reset_live_rate();
        if (!pause) {
            live_stats.fresh_since = esp_timer_get_time();
            live_stats.awaiting_frame = true;
        }
        lv_label_set_text_static(lv_obj_get_child(freeze_btn, 0), pause ? "Resume" : "Freeze");
    }
}

static void change_monitor(lv_event_t *e)
{
    (void)e;
    changing_monitor = generation;
    media_panel_stream_end(generation);
    kvmbridge_stop_display(capture_profile,generation);
    lv_obj_set_hidden(monitor_btn, true);
    lv_label_set_text_static(status, "Closing the previous display");
}

static lv_obj_t *dropdown(lv_obj_t *p, const char *items, int y)
{
    lv_obj_t *d = lv_dropdown_create(p);
    if (ui_theme_control_palette() == UI_PALETTE_HIGH_CONTRAST) {
        // Change ink and fill together on touch. The base theme animates only the fill.
        lv_obj_remove_style_all(d);
        lv_obj_set_style_pad_all(d, 12, 0);
        lv_obj_set_style_radius(d, 10, 0);
        lv_obj_set_style_outline_width(d, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_color(d, lv_color_white(), LV_STATE_FOCUS_KEY);
    }
    lv_dropdown_set_options_static(d, items);
    lv_obj_set_pos(d, 0, y);
    lv_obj_set_size(d, lv_pct(100), 52);
    ui_theme_control_button(d);
    lv_obj_t *list = lv_dropdown_get_list(d);
    ui_theme_apply(list, UI_STYLE_CONTROL_BUTTON, LV_PART_MAIN);
    ui_theme_apply(list, UI_STYLE_CONTROL_SELECTED, LV_PART_SELECTED | LV_STATE_CHECKED);
    ui_theme_apply(list, UI_STYLE_CONTROL_SELECTED, LV_PART_SELECTED | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(list, ui_theme_font(UI_FONT_BODY), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(list, 28, LV_PART_MAIN);
    return d;
}

static lv_obj_t *magnifier_card(int width, int height)
{
    lv_obj_t *card = lv_obj_create(controls);
    ui_theme_apply(card, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    ui_theme_apply(card, UI_STYLE_LIBRARY_TILE, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_scrollable(card, false);
    lv_obj_set_size(card, width, height);
    lv_obj_center(card);
    return card;
}

static lv_obj_t *field_label(lv_obj_t *parent, const char *title, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text_static(label, title);
    lv_obj_set_style_text_font(label, ui_theme_font(UI_FONT_BODY), 0);
    lv_obj_set_pos(label, 0, y);
    return label;
}

static void create_preview(lv_obj_t *card)
{
    field_label(card, "Requested monitor area", 0);
    preview_source = field_label(card, "", 26);
    lv_obj_set_style_text_font(preview_source, ui_theme_font(UI_FONT_TITLE), 0);
    lv_obj_t *sample = lv_image_create(card);
    lv_image_set_src(sample, &magnifier_landscape);
    lv_obj_set_pos(sample, 0, 66);
    ui_control_palette_t palette = ui_theme_control_palette();
    lv_obj_set_style_image_recolor(sample, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(sample, palette == UI_PALETTE_HIGH_CONTRAST ? LV_OPA_70 :
                                              palette == UI_PALETTE_NIGHT ? LV_OPA_30 : LV_OPA_TRANSP, 0);
    preview_frame = lv_obj_create(sample);
    lv_obj_remove_style_all(preview_frame);
    lv_obj_set_scrollable(preview_frame, false);
    lv_obj_set_style_border_color(preview_frame, lv_color_white(), 0);
    lv_obj_set_style_border_width(preview_frame, 2, 0);
    lv_obj_set_style_outline_color(preview_frame, lv_color_black(), 0);
    lv_obj_set_style_outline_width(preview_frame, 1, 0);
    lv_obj_t *center = lv_label_create(preview_frame);
    lv_label_set_text_static(center, "+");
    lv_obj_set_style_text_color(center, lv_color_white(), 0);
    lv_obj_set_style_bg_color(center, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(center, 3, 0);
    lv_obj_center(center);
    lv_obj_t *caption = field_label(card, "1920 x 1080 monitor example", 210);
    lv_obj_set_style_text_font(caption, ui_theme_font(UI_FONT_LABEL), 0);
    preview_output = field_label(card, "", 248);
}

uint32_t media_panel_open(bool live, void (*stop)(void))
{
    if (screen || timer || atomic_load(&busy))
        return 0;
    strcpy(capture_profile, magnifier_profile);
    if (!gate)
        gate = xSemaphoreCreateMutexStatic(&gate_memory);
    if (!gate)
        return 0;
    previous = lv_screen_active();
    screen = lv_obj_create(NULL);
    if (!screen)
        return 0;
    lv_obj_set_scrollable(screen, false);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    image = lv_image_create(screen);
    status = lv_label_create(screen);
    lv_obj_set_pos(status, 16, live ? 5 : 9);
    if (!live)
        lv_obj_set_style_text_font(status, ui_theme_font(UI_FONT_HEADING), 0);
    lv_label_set_text_static(status, live ? "Cursor magnifier" : "Opening video");
    // The 960x540 picture starts at y=46, leaving room for 44 px header targets.
    button(screen, "Stop", 790, 0, 106, 44, live ? leave_button : stop_file_button);
    button(screen, "Home", 904, 0, 112, 44, leave_button);
    if (!live)
        file_controls();
    if (!banded_decoder) {
        banded_decoder = lv_image_decoder_create();
        if (!banded_decoder) {
            lv_obj_delete(screen);
            screen = previous = NULL;
            return 0;
        }
        cinepak_band_handlers = *lv_draw_buf_get_handlers();
        cinepak_band_handlers.buf_malloc_cb = alloc_cinepak_band;
        cinepak_band_handlers.buf_free_cb = free;
        lv_image_decoder_set_info_cb(banded_decoder, banded_info);
        lv_image_decoder_set_open_cb(banded_decoder, banded_open);
        lv_image_decoder_set_get_area_cb(banded_decoder, banded_area);
        lv_image_decoder_set_close_cb(banded_decoder, banded_close);
    }
    if (live) {
        lv_obj_set_size(status, 542, 25);
        lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_CLIP);
        monitor_btn = button(screen, "Monitor", 566, 0, 109, 44, change_monitor);
        freeze_btn = button(screen, "Freeze", 680, 0, 101, 44, freeze);
        lv_obj_set_hidden(freeze_btn, true);
        lv_obj_set_hidden(monitor_btn, true);
        controls = lv_obj_create(screen);
        lv_obj_remove_style_all(controls);
        ui_theme_apply(controls, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
        lv_obj_set_scrollable(controls, false);
        lv_obj_set_pos(controls, 0, 46);
        lv_obj_set_size(controls, lv_pct(100), 554);
        settings_card = magnifier_card(744, 332);
        create_preview(settings_card);
        lv_obj_t *fields = lv_obj_create(settings_card);
        lv_obj_remove_style_all(fields);
        lv_obj_set_scrollable(fields, false);
        lv_obj_set_size(fields, 378, 272);
        lv_obj_align(fields, LV_ALIGN_TOP_RIGHT, 0, 0);
        modes = dropdown(fields, "Overview\nPixel 1x\nZoom 2x", 28);
        lv_dropdown_set_selected(modes, 1);
        formats = dropdown(fields, kvm_media_has_h264() ? "JPEG\nH.264\nCinepak" : "JPEG\nCinepak", 124);
        lv_dropdown_set_selected(formats, kvm_media_has_h264() ? 2 : 1);
        field_label(fields, "Magnification", 0);
        field_label(fields, "Video format", 96);
        lv_obj_add_event_cb(modes, preview_changed, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(formats, preview_changed, LV_EVENT_VALUE_CHANGED, NULL);
        update_preview();
        lv_obj_t *start = button(fields, "Start sharing", 0, 200, lv_pct(100), 56, begin_capture);
        ui_theme_apply(start, UI_STYLE_CONTROL_SELECTED, LV_STATE_DEFAULT);
        review_card = magnifier_card(600, 252);
        review_prompt = lv_label_create(review_card);
        lv_obj_set_width(review_prompt, lv_pct(100));
        lv_obj_set_style_text_font(review_prompt, ui_theme_font(UI_FONT_HEADING), 0);
        lv_label_set_long_mode(review_prompt, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text_static(review_prompt, "");
        approve_display = button(review_card, "Allow display", 0, 0, lv_pct(100), 56, permit);
        ui_theme_apply(approve_display, UI_STYLE_CONTROL_SELECTED, LV_STATE_DEFAULT);
        lv_obj_align(approve_display, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_hidden(review_card, true);
        crop_info = lv_label_create(screen);
        lv_obj_set_pos(crop_info, 564, 112);
        lv_obj_set_width(crop_info, 438);
        lv_label_set_text_static(crop_info, "");
        lv_obj_set_hidden(crop_info, true);
    }
    timer = lv_timer_create(tick, 16, NULL);
    if (!timer) {
        lv_obj_delete(screen);
        screen = previous = NULL;
        return 0;
    }
    lv_display_add_event_cb(lv_display_get_default(), refreshed, LV_EVENT_REFR_READY, NULL);
    lv_display_add_event_cb(lv_display_get_default(), refreshed, LV_EVENT_FLUSH_FINISH, NULL);
    live_screen = live;
    navigate = stop;
    requested = flush_pending = did_flush = false;
    changing_monitor = 0;
    memset(&live_stats, 0, sizeof(live_stats));
    first_refresh = last_refresh = 0;
    refresh_count = 0;
    file_width = file_height = 0;
    position_frame = 0;
    first_file_pts = last_file_pts = INT64_MIN;
    atomic_store(&file_total, 0);
    atomic_store(&file_paused, false);
    atomic_store(&retire_surface, false);
    paused_at = pause_total = 0;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    atomic_store(&timed_epoch, 0);
    timed_display_frames = 0;
    timed_selected_at = timed_display_us = 0;
    memset(&blackout, 0, sizeof(blackout));
    blackout.front = -1;
#endif
    atomic_store(&file_flushed, 0);
    board_lcd_stats_t stats;
    board_lcd_get_stats(&stats);
    vsync_first = stats.vsync_count;
    atomic_store(&shown, -1);
    atomic_store(&pending, -1);
    atomic_store(&started, false);
    atomic_store(&file_result, KVM_PLAY_UNAVAILABLE);
    atomic_store(&frozen, false);
    atomic_store(&freeze_revision, 0);
    new_epoch();
    lv_screen_load(screen);
    return generation;
}

uint32_t media_panel_open_magnifier(const char *profile_id, const char *mode, void (*stop)(void))
{
    if (!mode || !profile_id || !*profile_id || strlen(profile_id) >= sizeof(capture_profile)) return 0;
    for (unsigned i = 0; i < sizeof(crop_modes) / sizeof(crop_modes[0]); ++i) {
        if (strcmp(mode, crop_modes[i])) continue;
        uint32_t value = media_panel_open(true, stop);
        if (value) {
            strcpy(capture_profile, profile_id);
            lv_dropdown_set_selected(modes, i);
            lv_dropdown_set_selected(formats, !strcmp(profile_id, magnifier_profile) ? magnifier_format : 0);
            update_preview();
        }
        return value;
    }
    return 0;
}

void media_panel_telemetry_status(const char *profile_id, const char *text)
{
    if (!screen || !live_screen || lv_screen_active() != screen || !profile_id || !text ||
        strcmp(capture_profile, profile_id))
        return;
    if (!telemetry_status) {
        telemetry_status = lv_label_create(screen);
        if (!telemetry_status) return;
        // Two status lines fit above even the 960x540 image, which starts at y=46.
        lv_obj_set_pos(status, 16, 0);
        lv_obj_set_height(status, 22);
        lv_obj_set_pos(telemetry_status, 16, 22);
        lv_obj_set_size(telemetry_status, 542, 23);
        lv_label_set_long_mode(telemetry_status, LV_LABEL_LONG_MODE_CLIP);
    }
    if (strcmp(lv_label_get_text(telemetry_status), text))
        lv_label_set_text(telemetry_status, text);
}

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
// The lossless reference uses the same RGB565 descriptor and placement as Pixel 1x.
// A single immutable source surface is enough until Stop/Home destroys the panel.
static uint32_t pattern_epoch;
static uint32_t pattern_vsync, pattern_bounce;

static uint16_t pattern_pixel(unsigned x, unsigned y)
{
    if (!y) return 0xf800;
    if (y == 511) return 0x07e0;
    if (!x) return 0x001f;
    if (x == 511) return 0xffe0;
    uint16_t value = (uint16_t)(((x + 3*y) % 32) * 2048 + ((x ^ y) % 64) * 32 + (5*x + y) % 32);
    return (x ^ y) & 1 ? (uint16_t)~value : value;
}

bool media_panel_pixel_pattern(void (*done)(void))
{
    uint32_t value = media_panel_open(true, done);
    if (!value) return false;
    memset(&synth, 0, sizeof(synth));
    synth.active = true; // Offline Stop also bypasses the bridge.
    surface_bytes = 512u * 512u * sizeof(uint16_t);
    surfaces[0] = heap_caps_malloc(surface_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!surfaces[0]) {
        media_panel_close();
        return false;
    }
    for (unsigned y = 0; y < 512; ++y)
        for (unsigned x = 0; x < 512; ++x)
            surfaces[0][512*y+x] = pattern_pixel(x, y);
    stream_codec = KVM_LIVE_JPEG; // The presenter's native 512-square RGB565 surface format.
    lv_obj_set_hidden(controls, true);
    lv_label_set_text_static(status, "CAPTURE OFF   |   Test pattern");
    board_lcd_stats_t stats;
    board_lcd_get_stats(&stats);
    pattern_vsync = stats.vsync_count;
    pattern_bounce = stats.bounce_frames;
    pattern_epoch = value;
    publish(0, 512, 512, 0);
    return true;
}

bool media_panel_pixel_pattern_scan(void)
{
    if (!pattern_epoch || !synth.active || pattern_epoch != atomic_load(&epoch) || !refresh_count ||
        !screen || lv_screen_active() != screen || atomic_load(&shown) != 0 || flush_pending)
        return false;
    board_lcd_stats_t stats;
    board_lcd_get_stats(&stats);
    uint32_t vsync = stats.vsync_count - pattern_vsync, bounce = stats.bounce_frames - pattern_bounce;
    unsigned available = 0, matching = 0;
    for (unsigned i = 0; i < 2; ++i) {
        const uint16_t *fb = board_lcd_fb(i);
        if (!fb) continue;
        available |= 1u << i;
        bool match = true;
        for (unsigned y = 0; y < 512 && match; ++y)
            match = !memcmp(fb + (60+y)*1024+24, surfaces[0]+512*y, 512*sizeof(uint16_t));
        if (match) matching |= 1u << i;
    }
    ESP_LOGI("media", "MAG13_SCAN fb_available=%u fb_equal=%u vsync_delta=%lu bounce_delta=%lu",
             available, matching, (unsigned long)vsync, (unsigned long)bounce);
    return matching && vsync && bounce;
}

void media_panel_time_without_scanout_crc(uint32_t value)
{
    if (value && value == atomic_load(&epoch) && !live_screen)
        atomic_store(&timed_epoch, value);
}

static bool synthetic_start(kvm_live_codec_t codec, unsigned fps, unsigned frames, void (*done)(void), bool paced)
{
    if (!fps || fps > 60 || !frames || frames > 3600)
        return false;
    uint32_t value = media_panel_open(true, done);
    if (!value)
        return false;
    memset(&synth, 0, sizeof(synth));
    synth.active = true;
    synth.codec = codec;
    synth.paced = paced;
    bool h264 = codec == KVM_LIVE_H264_INTRA;
    admission("begin");
    bool ready = false;
    if (paced) {
        enum { NETWORK_PRESSURE = 128 * 1024 };
        synth.pressure = heap_caps_malloc(NETWORK_PRESSURE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (synth.pressure) {
            ESP_LOGI("media", "MEDIA_SYNTHETIC wire=in_place network_pressure=%u", (unsigned)NETWORK_PRESSURE);
            synth.packet = start_buffered_capture(value, SYNTH_ID, codec);
            ready = synth.packet != NULL;
        } else {
            admission("failed_network_pressure");
        }
    } else {
        size_t max_idr = sizeof(synthetic_idr_0) > sizeof(synthetic_idr_1) ? sizeof(synthetic_idr_0) : sizeof(synthetic_idr_1);
        synth.packet = heap_caps_malloc(h264 ? KVM_LIVE_HEADER + max_idr : SYNTH_PACKET, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!synth.packet)
            admission("failed_fixture");
        ready = synth.packet && media_panel_stream_start(value, SYNTH_ID, codec);
    }
    if (!ready) {
        media_panel_close();
        return false;
    }
    static const uint8_t header[KVM_LIVE_HEADER] = {
        'K', 'V', 'S', '2', 2, 3, 1, 0, 'T', 'N', 'Y', 'S',
        0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 3, 0x1c, 2, 0xb6, 0x7e, 0, 0};
    // Single 960x540 intra strip, two four-luma grayscale V1 entries, 32400 blocks.
    static const uint8_t cvid[] = {
        0, 0, 0x7e, 0xb6, 3, 0xc0, 2, 0x1c, 0, 1, 0x10, 0, 0x7e, 0xac, 0, 0, 0, 0, 2, 0x1c, 3, 0xc0,
        0x26, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0x7e, 0x94};
    memcpy(synth.packet, header, sizeof(header));
    bool jpeg = codec == KVM_LIVE_JPEG;
    if (h264 || jpeg) {
        if (jpeg) {
            synth.packet[3] = '1';
            synth.packet[4] = 1;
        }
        synth.packet[5] = codec;
        synth.packet[20] = synth.packet[22] = 0;
        synth.packet[21] = synth.packet[23] = 2;
    } else {
        uint8_t *payload = synth.packet + sizeof(header);
        memcpy(payload, cvid, sizeof(cvid));
        uint8_t *indices = payload + sizeof(cvid);
        for (unsigned y = 0; y < KVM_CVID_H / 4; ++y)
            for (unsigned x = 0; x < KVM_CVID_W / 4; ++x)
                *indices++ = (uint8_t)((x / 12 + y / 12) & 1);
    }
    synth.rate = fps;
    synth.frames = frames;
    atomic_store(&phase_epoch, value);
    synth.deadline = esp_timer_get_time() + 10000000;
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    synth.vsync = st.vsync_count;
    synth.bounce = st.bounce_frames;
    lv_obj_set_hidden(controls, true);
    lv_label_set_text_static(status, jpeg ? "Synthetic JPEG test" : h264 ? "Synthetic H.264 test" : "Synthetic Cinepak test");
    return true;
}

bool media_panel_synthetic_start(unsigned fps, unsigned frames, void (*done)(void))
{
    return synthetic_start(KVM_LIVE_CINEPAK_INTRA, fps, frames, done, false);
}

bool media_panel_synthetic_h264_start(unsigned fps, unsigned frames, void (*done)(void))
{
    return synthetic_start(KVM_LIVE_H264_INTRA, fps, frames, done, false);
}

bool media_panel_synthetic_h264_paced(unsigned fps, unsigned frames, void (*done)(void))
{
    return synthetic_start(KVM_LIVE_H264_INTRA, fps, frames, done, true);
}

bool media_panel_synthetic_jpeg_paced(unsigned fps, unsigned frames, void (*done)(void))
{
    return synthetic_start(KVM_LIVE_JPEG, fps, frames, done, true);
}
#endif
