// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

typedef pthread_mutex_t StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
enum { MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_INTERNAL = 2, MALLOC_CAP_8BIT = 4, pdTRUE = 1, pdPASS = 1 };
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(tag, fmt, ...) printf(fmt "\n", __VA_ARGS__)
#define ESP_LOGW ESP_LOGI
extern size_t test_largest_psram, test_refuse_size, test_shrink_size;
extern unsigned test_surfaces_until_failure;
static inline void *heap_caps_malloc(size_t n, unsigned caps)
{
    (void)caps;
    if (n == 960u * 540 * 2 && test_surfaces_until_failure && !--test_surfaces_until_failure)
        return NULL;
    if (n == test_refuse_size)
        return NULL;
    if (n == test_shrink_size)
        test_largest_psram = 131072;
    return malloc(n);
}
static inline void heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned caps)
{
    return (caps & MALLOC_CAP_SPIRAM) ? test_largest_psram : 65536;
}
static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{
    return heap_caps_get_free_size(caps);
}
// The presenter links the real heap admission code, but the desktop FFmpeg
// archive uses its native allocator. Separate bounded-codecs tests exercise TLSF.
typedef void *multi_heap_handle_t;
typedef struct { size_t total_free_bytes, largest_free_block; } multi_heap_info_t;
#define MALLOC_CAP_DMA 8
static inline multi_heap_handle_t multi_heap_register(void *p, size_t n) { (void)n; return p; }
static inline size_t multi_heap_minimum_free_size(multi_heap_handle_t h) { (void)h; return 0; }
static inline size_t multi_heap_get_allocated_size(multi_heap_handle_t h, void *p) { (void)h; (void)p; return 0; }
static inline void multi_heap_get_info(multi_heap_handle_t h, multi_heap_info_t *out)
{ (void)h; *out = (multi_heap_info_t){0}; }
static inline void *multi_heap_aligned_alloc(multi_heap_handle_t h, size_t n, size_t align)
{ (void)h; (void)n; (void)align; return NULL; }
static inline void *multi_heap_realloc(multi_heap_handle_t h, void *p, size_t n)
{ (void)h; (void)p; (void)n; return NULL; }
static inline void multi_heap_free(multi_heap_handle_t h, void *p) { (void)h; (void)p; }
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m)
{
    return pthread_mutex_init(m, NULL) ? NULL : m;
}
static inline int xSemaphoreTake(SemaphoreHandle_t m, unsigned t)
{
    return !(t ? pthread_mutex_lock(m) : pthread_mutex_trylock(m));
}
static inline void xSemaphoreGive(SemaphoreHandle_t m)
{
    pthread_mutex_unlock(m);
}
static inline void vTaskDelay(unsigned ticks)
{
    struct timespec delay = {.tv_nsec = (long)ticks * 1000000L};
    nanosleep(&delay, NULL);
}
// Host threads do not expose the FreeRTOS decoder stack watermark.
static inline unsigned uxTaskGetStackHighWaterMark(void *task) { (void)task; return 0; }
extern _Atomic int64_t simulated_time_offset;
static inline int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return simulated_time_offset + (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
typedef struct {
    uint32_t vsync_count, bounce_frames;
} board_lcd_stats_t;
void board_lcd_get_stats(board_lcd_stats_t *s);
uint16_t *board_lcd_fb(int index);
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *p, uint32_t size);
#define CONFIG_KVM_UI_TASK_CORE 1
#define tskNO_AFFINITY (-1)
int xTaskCreatePinnedToCoreWithCaps(void (*f)(void *), const char *name, unsigned bytes, void *arg, unsigned priority,
                                   void *handle, int core, unsigned caps);
static inline void vTaskDeleteWithCaps(void *self)
{
    (void)self;
}
