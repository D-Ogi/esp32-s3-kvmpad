// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// The test drives producer, timer and TinyUSB events in a fixed order on one
// thread. Only the real panel/decoder runs on its own worker thread.
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_ERR_NO_MEM, ESP_ERR_TIMEOUT, ESP_ERR_NOT_SUPPORTED, ESP_TIMER_TASK, pdTRUE = 1 };
#define ESP_RETURN_ON_FALSE(c,e,...) do { if (!(c)) return (e); } while (0)
#define ESP_RETURN_ON_ERROR(c,...) do { esp_err_t rc = (c); if (rc) return rc; } while (0)
#define ESP_LOGI(t,...) ((void)(t))
#define ESP_LOGD ESP_LOGI
#define ESP_LOGW ESP_LOGI
typedef unsigned TickType_t;
typedef unsigned portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) assert((*p)++ == 0)
#define portEXIT_CRITICAL(p) assert((*p)-- == 1)
#define pdMS_TO_TICKS(t) (t)

typedef struct { unsigned depth, width, head, count; uint8_t *storage; } StaticQueue_t, *QueueHandle_t;
static inline QueueHandle_t xQueueCreateStatic(unsigned depth, unsigned width, uint8_t *storage, StaticQueue_t *q)
{
    *q = (StaticQueue_t){.depth = depth, .width = width, .storage = storage};
    return q;
}
static inline bool xQueueSend(QueueHandle_t q, const void *item, unsigned wait)
{
    assert(!wait);
    if (q->count == q->depth) return false;
    unsigned tail = (q->head + q->count) % q->depth;
    memcpy(q->storage + tail * q->width, item, q->width);
    ++q->count;
    return true;
}
static inline bool xQueueReceive(QueueHandle_t q, void *out, unsigned wait)
{
    assert(!wait);
    if (!q->count) return false;
    memcpy(out, q->storage + q->head * q->width, q->width);
    q->head = (q->head + 1) % q->depth;
    --q->count;
    return true;
}
static inline unsigned uxQueueMessagesWaiting(QueueHandle_t q) { return q->count; }
typedef bool StaticSemaphore_t, *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { *s = true; return s; }
static inline SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *s) { *s = false; return s; }
static inline bool xSemaphoreTake(SemaphoreHandle_t s, unsigned wait)
{
    (void)wait;
    bool old = *s;
    *s = false;
    return old;
}
static inline void xSemaphoreGive(SemaphoreHandle_t s) { *s = true; }
typedef struct { void (*callback)(void *); int dispatch_method; const char *name; bool skip_unhandled_events; } esp_timer_create_args_t;
typedef void (*esp_timer_handle_t)(void *);
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period);
int64_t esp_timer_get_time(void);
static inline TickType_t xTaskGetTickCount(void) { return (TickType_t)(esp_timer_get_time() / 1000); }
