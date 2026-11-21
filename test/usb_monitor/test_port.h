// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
enum { MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2, pdPASS=1, pdTRUE=1 };
#define portMAX_DELAY UINT32_MAX
#define ESP_LOGI(tag,...) do { (void)(tag); printf(__VA_ARGS__); putchar('\n'); } while (0)
typedef struct { pthread_mutex_t lock; pthread_cond_t event; unsigned count; } StaticSemaphore_t, *SemaphoreHandle_t;
typedef struct test_task *TaskHandle_t;
static inline SemaphoreHandle_t test_semaphore(StaticSemaphore_t *s,unsigned count)
{
    assert(!pthread_mutex_init(&s->lock,NULL) && !pthread_cond_init(&s->event,NULL));
    s->count=count; return s;
}
#define xSemaphoreCreateMutexStatic(s) test_semaphore(s,1)
#define xSemaphoreCreateBinaryStatic(s) test_semaphore(s,0)
static inline void xSemaphoreGive(SemaphoreHandle_t s)
{
    pthread_mutex_lock(&s->lock); ++s->count; pthread_cond_signal(&s->event); pthread_mutex_unlock(&s->lock);
}
static inline void xSemaphoreTake(SemaphoreHandle_t s, unsigned wait)
{
    assert(wait==portMAX_DELAY); pthread_mutex_lock(&s->lock);
    while(!s->count) pthread_cond_wait(&s->event,&s->lock);
    --s->count; pthread_mutex_unlock(&s->lock);
}
int xTaskCreatePinnedToCoreWithCaps(void (*f)(void *), const char *name, unsigned bytes, void *arg, unsigned priority,
                                   TaskHandle_t *task, int core, unsigned caps);
void xTaskNotifyGive(TaskHandle_t task);
void ulTaskNotifyTake(int clear,unsigned wait);
static inline void vTaskDeleteWithCaps(void *self){assert(!self);}
void *heap_caps_malloc(size_t size,unsigned caps);
void heap_caps_free(void *p);

void *heap_caps_aligned_alloc(size_t alignment,size_t size,unsigned caps);
