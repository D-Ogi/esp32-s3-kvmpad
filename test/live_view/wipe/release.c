// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Compile each branch with the matching production translation unit's IDF flags.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
enum allocation { RGB0, RGB1, INPUT0, INPUT1, BAND, JPEG, H264, WIRE, WORKER, PACED_WIRE, CONTROL, COUNT };
void *fixture(enum allocation id, size_t size);
void __wrap_free(void *ptr);
void qualify_bridge_borrowed(uint8_t *wire);

#if defined(TEST_PANEL)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#undef xSemaphoreTake
#undef xSemaphoreGive
#define xSemaphoreTake(...) pdTRUE
#define xSemaphoreGive(...) ((void)0)
#include "../../../main/media_panel.c"

void qualify_panel(void)
{
    live_screen = true;
    surface_bytes = 512 * 512 * sizeof(uint16_t);
    for (int i = 0; i < 2; ++i) {
        surfaces[i] = fixture(RGB0 + i, surface_bytes);
        encoded[i] = fixture(INPUT0 + i, KVM_LIVE_MAX_PAYLOAD);
    }
    free_buffers();
    free_buffers(); // Repeated teardown after all pointers have been detached.
    in_place = true;
    encoded[0] = fixture(PACED_WIRE, KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD);
    qualify_bridge_borrowed(encoded[0]);
    free_buffers(); // The presenter owns and wipes both header and payload.
    lv_draw_buf_t band = {0};
    band.header.h = 4;
    band.header.stride = 960 * sizeof(uint16_t);
    band.data = fixture(BAND, band.header.stride * band.header.h);
    // LVGL can offset its aligned pixel pointer from the raw allocation.
    band.unaligned_data = band.data - 5;
    lv_image_decoder_dsc_t d = {.user_data = &band};
    banded_close(NULL, &d);
}
void lv_draw_buf_destroy(lv_draw_buf_t *band) { __wrap_free((uint8_t *)band->unaligned_data + 5); }
void lv_image_cache_drop(const void *src) { (void)src; }
void kvm_media_h264_release(kvm_h264_picture_t *picture) { (void)picture; }

#elif defined(TEST_HEAP)
#include "esp_log.h"
#undef ESP_LOGI
#define ESP_LOGI(...) ((void)0)
#include "../../../components/live_view/h264_heap.c"

void qualify_heap(void)
{
    reserved = 1792 * 1024;
    storage = fixture(H264, reserved);
    kvm_h264_heap_close(true);
}

#elif defined(TEST_BRIDGE)
// An empty synthetic trust store forces the ordinary worker exit. No network
// driver, credentials, scheduler or physical device are present in this image.
#include "peer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_transport.h"
#include "esp_timer.h"
#define kvb_peer_load(...) false
#define esp_timer_get_time() INT64_C(0)
#define vTaskDelay(...) __builtin_trap()
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define vTaskDelete(...) ((void)0)
#include "../../../components/kvmbridge/bridge.c"
esp_err_t esp_transport_close(esp_transport_handle_t t) { (void)t; return ESP_OK; }
esp_err_t esp_transport_destroy(esp_transport_handle_t t) { (void)t; return ESP_OK; }
int esp_transport_ws_send_raw(esp_transport_handle_t t, ws_transport_opcodes_t opcode,
                              const char *data, int len, int timeout)
{ (void)t; (void)opcode; (void)data; (void)len; (void)timeout; return -1; }

static void finish_worker(uint8_t *wire, bool borrowed)
{
    worker_t *w = fixture(WORKER, sizeof(*w));
    *w = (worker_t){0};
    memset(w->chunk, 0xa5, sizeof(w->chunk));
    w->picture = wire;
    w->borrowed = borrowed;
    worker(w);
}

void qualify_bridge(void) { finish_worker(fixture(WIRE, KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD), false); }
void qualify_bridge_borrowed(uint8_t *wire) { finish_worker(wire, true); }

#else
#include "media_decode.h"
#include <stdlib.h>
static uint8_t pool[3500 * 1024] __attribute__((aligned(16)));
static size_t next;
static struct { void *ptr; size_t size; } tracked[COUNT];
volatile struct { uint32_t magic, seen, dirty, guards, invalid, bytes[COUNT]; } result;

void *fixture(enum allocation id, size_t size)
{
    next = (next + 15u) & ~15u;
    if (size + 32 > sizeof(pool) - next || tracked[id].ptr)
        __builtin_trap();
    volatile uint8_t *base = pool + next;
    for (size_t i = 0; i < size + 32; ++i)
        base[i] = 0xa5;
    void *p = pool + next + 16;
    tracked[id].ptr = p;
    tracked[id].size = size;
    next += size + 32;
    return p;
}

void __wrap_free(void *ptr)
{
    if (!ptr)
        return;
    for (unsigned i = 0; i < COUNT; ++i) {
        if (tracked[i].ptr != ptr)
            continue;
        const volatile uint8_t *p = ptr;
        size_t size = tracked[i].size;
        for (size_t n = 0; n < size; ++n)
            if (p[n])
                result.dirty |= 1u << i;
        for (size_t n = 1; n <= 16; ++n)
            if (*(p - n) != 0xa5 || p[size + n - 1] != 0xa5)
                result.guards |= 1u << i;
        result.seen |= 1u << i;
        result.bytes[i] = size;
        tracked[i].ptr = NULL;
        return;
    }
    ++result.invalid;
}
void __wrap_heap_caps_free(void *ptr) { __wrap_free(ptr); }
void *heap_caps_malloc(size_t size, uint32_t caps) { (void)caps; return fixture(JPEG, size); }
bool kvm_h264_live_open(void) { return false; }
void kvm_h264_live_close(void) {}
void qualify_panel(void), qualify_heap(void), qualify_bridge(void);
void qualify(void)
{
    qualify_panel();
    qualify_heap();
    qualify_bridge();
    kvm_media_decoder_close(kvm_media_decoder_open(KVM_LIVE_JPEG));
    // Positive control for the observer: a release without erasure MUST be seen.
    __wrap_free(fixture(CONTROL, 513));
    result.magic = 0x57495045;
}
#endif
