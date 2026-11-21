// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"
#include "media_decode.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// FFmpeg is single-threaded. Files use one arena; live IDRs can use a second
// PSRAM region when the running display and transport divide the outer heap.
static atomic_flag active = ATOMIC_FLAG_INIT;
static multi_heap_handle_t heap;
static void *storage;
static size_t reserved;
static multi_heap_handle_t extra_heap;
static void *extra_storage;
static size_t extra_bytes;
static bool failure_reported;

static kvm_play_result_t heap_open(bool live, size_t minimum, size_t limit)
{
    if (atomic_flag_test_and_set(&active))
        return KVM_PLAY_UNAVAILABLE;
    failure_reported = false;
    uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    size_t available = heap_caps_get_largest_free_block(caps);
    ESP_LOGI("media", "largest_psram=%u largest_internal=%u largest_dma=%u", (unsigned)available,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    size_t bytes = available > 65536 ? available - 65536 : 0;
    if (bytes > limit)
        bytes = limit;
    if (bytes >= minimum)
        storage = heap_caps_malloc(bytes, caps);
    reserved = bytes;
    if (!storage && live && available < limit + 65536 && available > 0) {
        size_t total = heap_caps_get_free_size(caps);
        if (total >= limit + 65536) {
            reserved = available < limit ? available : limit;
            extra_bytes = limit - reserved;
            storage = heap_caps_malloc(reserved, caps);
            if (storage && extra_bytes) {
                extra_storage = heap_caps_malloc(extra_bytes, caps);
                if (extra_storage)
                    extra_heap = multi_heap_register(extra_storage, extra_bytes);
            }
        }
    }
    if (storage && (!extra_bytes || extra_heap))
        heap = multi_heap_register(storage, reserved);
    if (heap)
        return KVM_PLAY_OK;
    heap_caps_free(storage);
    heap_caps_free(extra_storage);
    storage = NULL;
    extra_heap = NULL;
    extra_storage = NULL;
    extra_bytes = 0;
    atomic_flag_clear(&active);
    return KVM_PLAY_NO_MEMORY;
}

kvm_play_result_t kvm_h264_heap_open(bool live)
{
    // Two retained pictures and a 96 KiB input passed at 1760 KiB; leave 32 KiB of headroom.
    const size_t live_bytes = 1792u * 1024;
    return heap_open(live, live ? live_bytes : 512u * 1024, live ? live_bytes : 6u * 1024 * 1024);
}

kvm_play_result_t kvm_h264_header_heap_open(bool mp4)
{
    // MP4 has dimensions in its header. The Annex B parser may assemble a complete access unit.
    return heap_open(false, 64u * 1024, (mp4 ? 256u : 512u) * 1024);
}

bool kvm_h264_heap_close(bool live)
{
    bool exhausted = failure_reported;
    ESP_LOGI("media", "decoder_arena=%u minimum_free=%u extra_arena=%u", (unsigned)(reserved + extra_bytes),
             (unsigned)(multi_heap_minimum_free_size(heap) + (extra_heap ? multi_heap_minimum_free_size(extra_heap) : 0)),
             (unsigned)extra_bytes);
    heap = NULL;
    if (live)
        kvm_media_zeroize(storage, reserved);
    heap_caps_free(storage);
    storage = NULL;
    if (extra_storage) {
        kvm_media_zeroize(extra_storage, extra_bytes);
        heap_caps_free(extra_storage);
        extra_heap = NULL;
        extra_storage = NULL;
        extra_bytes = 0;
    }
    atomic_flag_clear(&active);
    return exhausted;
}

static void *checked(void *p, size_t size)
{
    if (!p && heap && size && !failure_reported) {
        failure_reported = true;
        multi_heap_info_t info;
        multi_heap_get_info(heap, &info);
        ESP_LOGW("media", "decoder_allocation_failed requested=%u arena_free=%u arena_largest=%u",
                 (unsigned)size, (unsigned)info.total_free_bytes, (unsigned)info.largest_free_block);
    }
    return p;
}

void *kvm_av_memalign(size_t alignment, size_t size)
{
    if (extra_heap) {
        void *p = multi_heap_aligned_alloc(extra_heap, size, alignment);
        if (p)
            return p;
    }
    return heap ? checked(multi_heap_aligned_alloc(heap, size, alignment), size) : NULL;
}

int kvm_av_posix_memalign(void **out, size_t alignment, size_t size)
{
    void *p = kvm_av_memalign(alignment, size);
    if (!p)
        return 12;
    *out = p;
    return 0;
}

void *kvm_av_malloc(size_t size)
{
    return kvm_av_memalign(16, size);
}

static multi_heap_handle_t owner(void *p)
{
    return extra_heap && (uintptr_t)p - (uintptr_t)extra_storage < extra_bytes ? extra_heap : heap;
}

void kvm_av_free(void *p)
{
    if (p)
        multi_heap_free(owner(p), p);
}

void *kvm_av_realloc(void *p, size_t size)
{
    if (!p)
        return kvm_av_malloc(size);
    // FFmpeg permits realloc to return ordinary malloc alignment.
    multi_heap_handle_t from = owner(p);
    void *result = multi_heap_realloc(from, p, size);
    if (!result && size && extra_heap) {
        size_t previous = multi_heap_get_allocated_size(from, p);
        result = multi_heap_aligned_alloc(from == heap ? extra_heap : heap, size, 16);
        if (result) {
            memcpy(result, p, size < previous ? size : previous);
            multi_heap_free(from, p);
        }
    }
    return checked(result, size);
}
