// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// LVGL heap in PSRAM (ADR-027 TECH-091, delivered with TASK-060B). With LV_USE_CUSTOM_MALLOC every
// lv_malloc() lands here: objects, styles, observers, subjects and LVGL's own layer buffers go to
// PSRAM, with internal RAM only as a fallback, so the internal heap stays for TinyUSB, GT911 and the
// Wi-Fi/TLS stack of M7/M8. The display frame buffers and the partial strip are allocated by the
// adapter itself with heap_caps and are not affected. Live allocation count and bytes are tracked
// (atomics: the two software draw units allocate concurrently) for `ui` diagnostics.
#include "sdkconfig.h"

#if CONFIG_LV_USE_CUSTOM_MALLOC

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "ui_platform.h"

#define CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static atomic_size_t s_live_cnt;
static atomic_size_t s_live_bytes;
static atomic_size_t s_fallbacks;

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, CAPS_PSRAM);
    if (p == NULL) {
        p = heap_caps_malloc(size, CAPS_INTERNAL);
        if (p != NULL) {
            atomic_fetch_add(&s_fallbacks, 1);
        }
    }
    if (p != NULL) {
        atomic_fetch_add(&s_live_cnt, 1);
        atomic_fetch_add(&s_live_bytes, heap_caps_get_allocated_size(p));
    }
    return p;
}

void lv_free_core(void *p)
{
    if (p == NULL) {
        return;
    }
    atomic_fetch_sub(&s_live_bytes, heap_caps_get_allocated_size(p));
    atomic_fetch_sub(&s_live_cnt, 1);
    heap_caps_free(p);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == NULL) {
        return lv_malloc_core(new_size);
    }
    size_t old = heap_caps_get_allocated_size(p);
    void *n = heap_caps_realloc_prefer(p, new_size, 2, CAPS_PSRAM, CAPS_INTERNAL);
    if (n != NULL) {
        atomic_fetch_sub(&s_live_bytes, old);
        atomic_fetch_add(&s_live_bytes, heap_caps_get_allocated_size(n));
    }
    return n;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, CAPS_PSRAM);
    memset(mon, 0, sizeof(*mon));
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->free_cnt = info.free_blocks;
    mon->used_cnt = atomic_load(&s_live_cnt);
    mon->max_used = mon->total_size - info.minimum_free_bytes;
    mon->used_pct = mon->total_size ? (uint8_t)(100u - (100u * mon->free_size) / mon->total_size) : 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(CAPS_PSRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}

void ui_platform_lv_heap(size_t *live_cnt, size_t *live_bytes, size_t *internal_fallbacks)
{
    if (live_cnt) {
        *live_cnt = atomic_load(&s_live_cnt);
    }
    if (live_bytes) {
        *live_bytes = atomic_load(&s_live_bytes);
    }
    if (internal_fallbacks) {
        *internal_fallbacks = atomic_load(&s_fallbacks);
    }
}

#else // C library allocator: nothing tracked

#include <stddef.h>

#include "ui_platform.h"

void ui_platform_lv_heap(size_t *live_cnt, size_t *live_bytes, size_t *internal_fallbacks)
{
    if (live_cnt) {
        *live_cnt = 0;
    }
    if (live_bytes) {
        *live_bytes = 0;
    }
    if (internal_fallbacks) {
        *internal_fallbacks = 0;
    }
}

#endif // CONFIG_LV_USE_CUSTOM_MALLOC
