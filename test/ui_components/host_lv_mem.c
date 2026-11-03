// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// LV_STDLIB_CUSTOM allocator for the host UI tests of TASK-060D: every lv_malloc() is a real
// malloc() (so the address sanitizer sees use-after-free inside LVGL) with a small header that
// keeps the live allocation count and byte total exact (AT-UIFW-02 / AT-UIFW-09 leak checks).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "host_lv_mem.h"
#include "lvgl.h"

#define HDR 16 // keeps 16-byte alignment of the returned block

static size_t s_live_cnt;
static size_t s_live_bytes;
static size_t s_max_bytes;

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
    uint8_t *p = malloc(size + HDR);
    if (!p) {
        return NULL;
    }
    memcpy(p, &size, sizeof(size));
    s_live_cnt++;
    s_live_bytes += size;
    if (s_live_bytes > s_max_bytes) {
        s_max_bytes = s_live_bytes;
    }
    return p + HDR;
}

void lv_free_core(void *ptr)
{
    if (!ptr) {
        return;
    }
    uint8_t *p = (uint8_t *)ptr - HDR;
    size_t size;
    memcpy(&size, p, sizeof(size));
    s_live_cnt--;
    s_live_bytes -= size;
    free(p);
}

void *lv_realloc_core(void *ptr, size_t new_size)
{
    if (!ptr) {
        return lv_malloc_core(new_size);
    }
    uint8_t *p = (uint8_t *)ptr - HDR;
    size_t old;
    memcpy(&old, p, sizeof(old));
    uint8_t *n = realloc(p, new_size + HDR);
    if (!n) {
        return NULL;
    }
    memcpy(n, &new_size, sizeof(new_size));
    s_live_bytes = s_live_bytes - old + new_size;
    if (s_live_bytes > s_max_bytes) {
        s_max_bytes = s_live_bytes;
    }
    return n + HDR;
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    memset(mon, 0, sizeof(*mon));
    mon->used_cnt = s_live_cnt;
    mon->max_used = s_max_bytes;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

size_t host_lv_live_count(void)
{
    return s_live_cnt;
}

size_t host_lv_live_bytes(void)
{
    return s_live_bytes;
}
