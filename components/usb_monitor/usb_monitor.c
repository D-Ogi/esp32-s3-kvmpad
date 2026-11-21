// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "monitor_present.h"
#include "monitor_wire.h"
#include "usb_jpeg_decode.h"
#include "media_decode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <string.h>
#ifdef KVM_USB_PICTURE_CHECK
#include "usb_jpeg_check.h"
#endif

enum { JPEG_SLOTS = 3 };
static bool native_mode, windows_modes;
static monitor_size_t source_size[JPEG_SLOTS], last_native_geometry;
static size_t rgb_bytes;
static StaticSemaphore_t mutex_storage, exit_storage;
static SemaphoreHandle_t mutex, worker_exited;
static TaskHandle_t worker;
static atomic_bool active;
static _Atomic uint32_t revision;
static monitor_wire_t wire;
static uint8_t *compressed[JPEG_SLOTS];
static uint16_t *pixels[2];
// Native redraws share one RGB surface with the LVGL task. Keep the immutable
// source of its last good picture so a failed partial decode can be rolled back.
static uint8_t *last_native_jpeg;
static uint8_t *decode_input;
static uint16_t *native_stripe;
static uint16_t *native_cells;

static size_t native_cell_bytes(void)
{
    return monitor_width(true) * monitor_height(true) / USB_JPEG_CELL_PIXELS * sizeof(*native_cells);
}
#ifdef KVM_USB_MOTION_CHECK
bool usb_monitor_test_has_cells(void) { return native_cells != NULL; }
#endif
static bool saved_accelerated;
static monitor_damage_t pending_damage[2];
static unsigned pending_damage_count;
static bool pending_partial;
static size_t last_native_size;
// A free VGA input slot may still contain the JPEG for the displayed RGB surface.
// The receiver can reclaim it whenever the other slots are occupied.
static int cached_vga_input = -1;
static size_t cached_vga_size;
static bool has_nonblack[2];
static kvm_media_decoder_t *decoder;
// USB, the worker and the UI transfer ownership under mutex. Keep at most
// one complete JPEG waiting and never overwrite an in-flight input or RGB surface.
static int receiving_input = -1, queued_input = -1, decoding_input = -1;
static size_t queued_size;
static bool clear_screen;
static int pending = -1, shown = -1;
static atomic_uint received, decoded, busy_drops, decode_errors, presented, superseded, input_slots;
static atomic_bool last_decode_failed;
static atomic_uint accelerated_frames;
static atomic_uint duplicate_jpegs;

// The slot owners move these timestamps together with their JPEG or decoded frame.
typedef struct {
    unsigned sequence;
    int64_t header, complete, started, ready, selected;
} frame_clock_t;
static frame_clock_t input_clock[JPEG_SLOTS], pending_clock, display_clock;
static usb_monitor_latency_t latency;
static usb_monitor_stages_t cumulative;

usb_monitor_input_stats_t usb_monitor_input_stats(void)
{
    usb_monitor_input_stats_t sample = {0};
    if (!atomic_load(&active)) return sample;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (atomic_load(&active)) {
        sample = (usb_monitor_input_stats_t){
            .bytes = wire.bytes, .headers = wire.headers, .rejected = wire.rejected,
            .header_bytes = wire.used, .payload_left = (unsigned)wire.remaining,
        };
    }
    xSemaphoreGive(mutex);
    return sample;
}

usb_monitor_latency_t usb_monitor_latency(void)
{
    usb_monitor_latency_t copy = {0};
    if (!atomic_load(&active)) return copy;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (atomic_load(&active)) {
        copy = latency;
        if (copy.samples) {
            copy.mean = (usb_monitor_stages_t){cumulative.receive / copy.samples, cumulative.queue / copy.samples,
                cumulative.decode / copy.samples, cumulative.ready / copy.samples, cumulative.refresh / copy.samples};
        }
    }
    xSemaphoreGive(mutex);
    return copy;
}

bool monitor_last_decode_failed(void)
{
    return atomic_load(&last_decode_failed);
}

#ifdef KVM_USB_PICTURE_CHECK
static bool check_pictures;
static struct {
    usb_jpeg_result_t reference;
    unsigned sequence;
} checks[2];

void usb_monitor_enable_picture_check(void)
{
    if (!atomic_load(&active)) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    check_pictures = atomic_load(&active);
    xSemaphoreGive(mutex);
}

usb_monitor_picture_t monitor_checked_picture(const uint16_t *selected, const char *decoded_picture)
{
    static const char *const names[] = {
        [USB_JPEG_ABSENT] = "absent", [USB_JPEG_ALL_BLACK] = "all_black",
        [USB_JPEG_HAS_NONBLACK] = "has_nonblack", [USB_JPEG_FAILED] = "failed",
    };
    usb_monitor_picture_t result = {"absent", "absent", 0};
    if (!selected || !atomic_load(&active)) return result;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (atomic_load(&active) && !clear_screen && shown >= 0 && pixels[shown] == selected) {
        result.decoded = decoded_picture;
        if (check_pictures) {
            result.reference = names[checks[shown].reference];
            result.sequence = checks[shown].sequence;
        }
    }
    xSemaphoreGive(mutex);
    return result;
}
#endif

usb_monitor_stats_t usb_monitor_stats(void)
{
    return (usb_monitor_stats_t){atomic_load(&received), atomic_load(&decoded), atomic_load(&busy_drops),
                                 atomic_load(&decode_errors), atomic_load(&presented), atomic_load(&superseded),
                                 atomic_load(&input_slots), atomic_load(&accelerated_frames), atomic_load(&duplicate_jpegs)};
}

void monitor_refresh_complete(void)
{
    const int64_t end = esp_timer_get_time();
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (display_clock.sequence) {
        const frame_clock_t c = display_clock;
        latency.last = (usb_monitor_stages_t){c.complete - c.header, c.started - c.complete, c.ready - c.started,
                                              c.selected - c.ready, end - c.selected};
        cumulative.receive += latency.last.receive;
        cumulative.queue += latency.last.queue;
        cumulative.decode += latency.last.decode;
        cumulative.ready += latency.last.ready;
        cumulative.refresh += latency.last.refresh;
        latency.samples++;
        latency.sequence = c.sequence;
        if (end - c.header > latency.peak_total) latency.peak_total = end - c.header;
        display_clock.sequence = 0;
    }
    xSemaphoreGive(mutex);
    if (atomic_fetch_add(&presented, 1) == 0)
        ESP_LOGI("usb_monitor", "USB_MONITOR first_refresh=1 width=%u height=%u",
                 monitor_width(native_mode), monitor_height(native_mode));
}

// A running decode keeps its input until it observes the new revision. The
// LVGL task retains the current RGB surface until it has replaced the image.
static void reset_locked(void)
{
    atomic_fetch_add(&revision, 1);
    atomic_store(&last_decode_failed, false);
    wire = (monitor_wire_t){.native = native_mode, .windows_modes = windows_modes};
    clear_screen = true;
    last_native_size = 0;
    cached_vga_input = -1;
    display_clock.sequence = 0;
    receiving_input = queued_input = -1;
    pending = -1;
#ifdef KVM_USB_PICTURE_CHECK
    for (unsigned i = 0; i < 2; ++i) {
        checks[i].reference = USB_JPEG_ABSENT;
        checks[i].sequence = 0;
    }
#endif
}

static bool cancelled(void *ctx)
{
    return atomic_load(&revision) != *(const uint32_t *)ctx;
}

// The vendor API takes mutable input. Preserve the queued and rollback JPEGs
// untouched even on error. The ROM decoder handles constrained-memory sessions.
static bool decode_picture(const uint8_t *input, size_t size, const kvm_media_target_t *target,
                           monitor_size_t source, bool *nonblack, bool *accelerated,
                           usb_jpeg_damage_t *tracking, bool allow_vendor)
{
    *accelerated = false;
    if (source.width != target->width || source.height != target->height) {
        if (!kvm_media_usb_monitor_jpeg_fit(decoder, input, size, target, source.width, source.height)) return false;
        *nonblack = kvm_media_usb_monitor_has_nonblack(decoder);
        return true;
    }
    if (allow_vendor && decode_input) {
        memcpy(decode_input, input, size);
        usb_jpeg_decode_result_t result = usb_jpeg_decode(decode_input, size, target, rgb_bytes, nonblack, tracking);
        if (result == USB_JPEG_OK) {
            *accelerated = true;
            return true;
        }
        if (result != USB_JPEG_UNAVAILABLE) return false;
    }
    if (!kvm_media_usb_monitor_jpeg(decoder, input, size, target)) return false;
    *nonblack = kvm_media_usb_monitor_has_nonblack(decoder);
    if (tracking && !native_mode) {
        // The ROM writes a complete VGA surface. The other surface is immutable.
        tracking->bounds = (monitor_damage_t){0, 0, -1, -1};
        tracking->gap_rows = 0;
        tracking->cut = 0;
        usb_jpeg_damage_band(tracking, target->pixels, NULL, target->width, 0, target->height);
    }
    return true;
}

// Called with the ownership mutex held. Release it for the entire decoder run
// so TinyUSB can receive a newer picture or cancel this generation.
static void decode_queued_locked(void)
{
    if (queued_input < 0 || pending >= 0) return;
    decoding_input = queued_input;
    queued_input = -1;
    const uint8_t *input = compressed[decoding_input];
    size_t size = queued_size;
    monitor_size_t source = source_size[decoding_input], restore_geometry = last_native_geometry;
    const uint8_t *cached = native_mode ? last_native_jpeg :
                            cached_vga_input >= 0 ? compressed[cached_vga_input] : NULL;
    bool same_geometry = !native_mode || (source.width == restore_geometry.width && source.height == restore_geometry.height);
    bool can_reuse = same_geometry && shown >= 0 && !clear_screen && cached &&
                     size == (native_mode ? last_native_size : cached_vga_size);
#ifdef KVM_USB_PICTURE_CHECK
    if (can_reuse && check_pictures && checks[shown].reference == USB_JPEG_ABSENT) can_reuse = false;
#endif
    if (can_reuse && !memcmp(input, cached, size)) {
        decoding_input = -1;
        atomic_store(&last_decode_failed, false);
        atomic_fetch_add(&duplicate_jpegs, 1);
        return;
    }
    frame_clock_t clock = input_clock[decoding_input];
    clock.started = esp_timer_get_time();
    int slot = !native_mode && shown == 0 ? 1 : 0;
    size_t restore_size = last_native_size;
    bool restore_accelerated = saved_accelerated;
    usb_jpeg_damage_t damage = {.bounds = {0, 0, -1, -1}};
    bool track = shown >= 0 && !clear_screen && (!native_mode || native_stripe);
    if (track) {
        damage.previous = pixels[shown];
        damage.stripe = native_stripe;
        damage.cells = native_cells;
#ifdef KVM_USB_MOTION_CHECK
        if (!usb_jpeg_test_cells) damage.cells = NULL;
#endif
    }
    uint32_t generation = atomic_load(&revision);
#ifdef KVM_USB_PICTURE_CHECK
    bool check = check_pictures;
#endif
    xSemaphoreGive(mutex);

    kvm_media_target_t target = {pixels[slot], monitor_width(native_mode), monitor_height(native_mode),
                                 monitor_width(native_mode),
                                 cancelled, &generation};
    bool nonblack = false, accelerated;
    bool ok = decode_picture(input, size, &target, source, &nonblack, &accelerated, track ? &damage : NULL, true);
#ifdef KVM_USB_PICTURE_CHECK
    usb_jpeg_result_t reference = USB_JPEG_ABSENT;
    if (check && ok && !cancelled(&generation))
        reference = accelerated ? usb_jpeg_check_rom(input, size, &target) : usb_jpeg_check(input, size);
#endif
    bool lost_picture = !ok && restore_size && !cancelled(&generation) &&
                        (!decode_picture(last_native_jpeg, restore_size, &target, restore_geometry, &nonblack, &accelerated, NULL,
                                        restore_accelerated) || accelerated != restore_accelerated);
    // First frames, ROM fallback and rollback can all replace pixels without
    // passing through the tracked bands, including the padded final MCU row.
    if (native_cells && (!track || !ok || !accelerated)) memset(native_cells, 0, native_cell_bytes());
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!cancelled(&generation)) {
        atomic_store(&last_decode_failed, !ok);
        if (ok) {
            if (accelerated) atomic_fetch_add(&accelerated_frames, 1);
            has_nonblack[slot] = nonblack;
#ifdef KVM_USB_PICTURE_CHECK
            if (check) {
                checks[slot].reference = reference;
                checks[slot].sequence = atomic_load(&decoded) + 1;
            }
#endif
            if (native_mode) {
                uint8_t *reusable = last_native_jpeg;
                last_native_jpeg = compressed[decoding_input];
                compressed[decoding_input] = reusable;
                last_native_size = size;
                last_native_geometry = source;
                saved_accelerated = accelerated;
            } else {
                cached_vga_input = decoding_input;
                cached_vga_size = size;
            }
            pending_partial = track && (!native_mode || accelerated);
            pending_damage_count = usb_jpeg_damage_regions(&damage, pending_damage);
            pending = slot;
            clock.ready = esp_timer_get_time();
            pending_clock = clock;
            atomic_fetch_add(&decoded, 1);
        } else {
            atomic_fetch_add(&decode_errors, 1);
            if (lost_picture) {
                last_native_size = 0;
                clear_screen = true;
            }
        }
    }
    decoding_input = -1;
}

static void decode_task(void *unused)
{
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(mutex, portMAX_DELAY);
        if (!atomic_load(&active)) {
            xSemaphoreGive(mutex);
            break;
        }
        decode_queued_locked();
        xSemaphoreGive(mutex);
    }
    xSemaphoreGive(worker_exited);
    vTaskDeleteWithCaps(NULL);
}

static void release_buffers(void)
{
    native_cells = NULL; // Owned and cleared with pixels[0].
    kvm_media_decoder_close(decoder);
    decoder = NULL;
    if (decode_input) {
        kvm_media_zeroize(decode_input, MONITOR_JPEG_LIMIT);
        heap_caps_free(decode_input);
        decode_input = NULL;
    }
    if (native_stripe) {
        kvm_media_zeroize(native_stripe, monitor_width(true) * 16 * sizeof(*native_stripe));
        heap_caps_free(native_stripe);
        native_stripe = NULL;
    }
    if (last_native_jpeg) {
        kvm_media_zeroize(last_native_jpeg, MONITOR_JPEG_LIMIT);
        heap_caps_free(last_native_jpeg);
        last_native_jpeg = NULL;
    }
    for (unsigned i = 0; i < JPEG_SLOTS; ++i) {
        if (compressed[i]) {
            kvm_media_zeroize(compressed[i], MONITOR_JPEG_LIMIT);
            heap_caps_free(compressed[i]);
            compressed[i] = NULL;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (pixels[i]) {
            kvm_media_zeroize(pixels[i], rgb_bytes);
            heap_caps_free(pixels[i]);
            pixels[i] = NULL;
        }
    }
}

bool monitor_start(bool native)
{
    if (atomic_load(&active)) return false;
    if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    if (!worker_exited) worker_exited = xSemaphoreCreateBinaryStatic(&exit_storage);
    if (!mutex || !worker_exited) return false;
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    native_mode = native;
    windows_modes = false;
    rgb_bytes = monitor_width(native) * ((monitor_height(native) + 15u) & ~15u) * sizeof(uint16_t);
    pixels[0] = heap_caps_aligned_alloc(16, rgb_bytes, caps);
    if (native) last_native_jpeg = heap_caps_malloc(MONITOR_JPEG_LIMIT, caps);
    else pixels[1] = heap_caps_aligned_alloc(16, rgb_bytes, caps);
    compressed[0] = heap_caps_malloc(MONITOR_JPEG_LIMIT, caps);
    decoder = kvm_media_decoder_open(KVM_LIVE_JPEG);
    if (!compressed[0] || !pixels[0] || (native ? !last_native_jpeg : !pixels[1]) || !decoder ||
        (!native && xTaskCreatePinnedToCoreWithCaps(decode_task, "usb_jpeg", 8192, NULL, 2, &worker, 1, caps) != pdPASS)) {
        release_buffers();
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    // An in-flight decode and a complete queued picture each keep their slot.
    // The third slot can receive the final desktop change even if Windows then goes quiet.
    unsigned slots = 1;
    while (slots < JPEG_SLOTS) {
        compressed[slots] = heap_caps_malloc(MONITOR_JPEG_LIMIT, caps);
        if (!compressed[slots]) break;
        ++slots;
    }
    decode_input = heap_caps_malloc(MONITOR_JPEG_LIMIT, caps);
    if (native && decode_input)
        native_stripe = heap_caps_aligned_alloc(16, monitor_width(true) * 16 * sizeof(*native_stripe), caps);
    if (native_stripe) {
        // Tracked native decodes use the stripe for the entire last MCU row.
        // Its eight invisible rows in the main surface can hold the cell tags.
        size_t visible = (size_t)monitor_width(true) * monitor_height(true);
        if (rgb_bytes - visible * sizeof(*pixels[0]) >= native_cell_bytes()) native_cells = pixels[0] + visible;
    }
    atomic_store(&input_slots, slots);
#ifdef KVM_USB_PICTURE_CHECK
    check_pictures = false;
#endif
    decoding_input = -1;
    shown = -1;
    reset_locked();
    latency = (usb_monitor_latency_t){0};
    cumulative = (usb_monitor_stages_t){0};
    atomic_store(&received, 0);
    atomic_store(&decoded, 0);
    atomic_store(&accelerated_frames, 0);
    atomic_store(&duplicate_jpegs, 0);
    atomic_store(&busy_drops, 0);
    atomic_store(&decode_errors, 0);
    atomic_store(&presented, 0);
    atomic_store(&superseded, 0);
    atomic_store(&active, true);
    xSemaphoreGive(mutex);
    return true;
}

void monitor_stop(void)
{
    if (!atomic_load(&active)) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    atomic_store(&active, false);
    reset_locked();
    if (!native_mode) xTaskNotifyGive(worker);
    xSemaphoreGive(mutex);
    if (!native_mode) xSemaphoreTake(worker_exited, portMAX_DELAY);
    release_buffers();
    usb_monitor_stats_t stats = usb_monitor_stats();
    ESP_LOGI("usb_monitor", "USB_MONITOR stopped queued=%u decoded=%u busy_drops=%u decode_errors=%u presented=%u superseded=%u",
             stats.queued, stats.decoded, stats.busy_drops, stats.decode_errors, stats.presented, stats.superseded);
}

void usb_monitor_reset(void)
{
    if (!atomic_load(&active)) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (atomic_load(&active)) reset_locked();
    xSemaphoreGive(mutex);
}

bool usb_monitor_enable_windows_modes(void)
{
    if (!atomic_load(&active)) return false;
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool ready = atomic_load(&active) && native_mode && !wire.bytes;
    if (ready) wire.windows_modes = windows_modes = true;
    xSemaphoreGive(mutex);
    return ready;
}

static uint8_t *reserve(void)
{
    int slot = -1;
    for (int i = 0; i < JPEG_SLOTS; ++i) {
        if (compressed[i] && i != decoding_input && i != queued_input) {
            slot = i;
            if (i != cached_vga_input) break;
        }
    }
    if (slot >= 0) {
        if (slot == cached_vga_input) cached_vga_input = -1;
        receiving_input = slot;
        source_size[slot] = wire.source;
        input_clock[slot].header = esp_timer_get_time();
        return compressed[slot];
    }
    atomic_fetch_add(&busy_drops, 1);
    return NULL;
}

static void complete(size_t size)
{
    // While the UI is busy, replace a waiting JPEG only after its successor
    // is complete. A truncated or slow transfer never blocks a ready frame.
    if (queued_input >= 0) atomic_fetch_add(&superseded, 1);
    queued_input = receiving_input;
    receiving_input = -1;
    queued_size = size;
    input_clock[queued_input].sequence = atomic_fetch_add(&received, 1) + 1;
    input_clock[queued_input].complete = esp_timer_get_time();
    if (!native_mode) xTaskNotifyGive(worker);
}

void usb_monitor_feed(const uint8_t *bytes, size_t size)
{
    uint32_t generation = atomic_load(&revision);
    if (!bytes || !size || !atomic_load(&active)) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (atomic_load(&active) && generation == atomic_load(&revision))
        monitor_wire_feed(&wire, bytes, size, reserve, complete);
    xSemaphoreGive(mutex);
}

void monitor_present(void (*select)(const uint16_t *, bool, const monitor_damage_t *, unsigned))
{
    if (!atomic_load(&active)) return;
    xSemaphoreTake(mutex, portMAX_DELAY);
    // LVGL has completed the previous render before it runs this timer. A
    // native surface can therefore be updated or restored here, atomically
    // with respect to rendering, snapshots and the on-screen Home overlay.
    if (native_mode) decode_queued_locked();
    if (pending >= 0 || clear_screen) {
        shown = pending;
        pending = -1;
        clear_screen = false;
        if (shown >= 0) {
            display_clock = pending_clock;
            display_clock.selected = esp_timer_get_time();
        } else {
            display_clock.sequence = 0;
        }
        select(shown < 0 ? NULL : pixels[shown], shown >= 0 && has_nonblack[shown],
               shown >= 0 && pending_partial ? pending_damage : NULL,
               shown >= 0 && pending_partial ? pending_damage_count : 0);
        if (!native_mode && queued_input >= 0) xTaskNotifyGive(worker);
    }
    xSemaphoreGive(mutex);
}
