// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_alloc: see port/ime_alloc.h.
#include "ime_alloc.h"

#include <cstdlib>
#include <new>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static TaskHandle_t volatile s_owner; // the task inside an engine call, or NULL
static unsigned s_depth;              // touched only by the owner

static void *prefer_psram(size_t n)
{
    return heap_caps_malloc_prefer(n ? n : 1, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

extern "C" void ime_alloc_enter(void)
{
    if (s_depth++ == 0) {
        s_owner = xTaskGetCurrentTaskHandle();
    }
}

extern "C" void ime_alloc_leave(void)
{
    if (s_depth > 0 && --s_depth == 0) {
        s_owner = nullptr;
    }
}

static bool owner(void)
{
    const TaskHandle_t o = s_owner;
    return o != nullptr && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED && xTaskGetCurrentTaskHandle() == o;
}

extern "C" void *ime_engine_malloc(size_t n)
{
    return prefer_psram(n);
}

extern "C" void *ime_engine_calloc(size_t n, size_t size)
{
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

extern "C" void *ime_engine_realloc(void *p, size_t n)
{
    return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT);
}

extern "C" void ime_engine_free(void *p)
{
    std::free(p);
}

// Global replacement: plain malloc() on every task but the engine owner (the default behavior of libstdc++,
// which also aborts on exhaustion in this exception-free build).
void *operator new(std::size_t n)
{
    void *p = owner() ? prefer_psram(n) : std::malloc(n ? n : 1);
    if (p == nullptr) {
        std::abort();
    }
    return p;
}

void *operator new[](std::size_t n)
{
    return ::operator new(n);
}

void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return owner() ? prefer_psram(n) : std::malloc(n ? n : 1);
}

void *operator new[](std::size_t n, const std::nothrow_t &t) noexcept
{
    return ::operator new(n, t);
}

void operator delete(void *p) noexcept
{
    std::free(p);
}

void operator delete[](void *p) noexcept
{
    std::free(p);
}

void operator delete(void *p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void *p, std::size_t) noexcept
{
    std::free(p);
}

#else // host build: plain heap, empty scope

extern "C" void *ime_engine_malloc(size_t n)
{
    return std::malloc(n);
}

extern "C" void *ime_engine_calloc(size_t n, size_t size)
{
    return std::calloc(n, size);
}

extern "C" void *ime_engine_realloc(void *p, size_t n)
{
    return std::realloc(p, n);
}

extern "C" void ime_engine_free(void *p)
{
    std::free(p);
}

extern "C" void ime_alloc_enter(void) {}
extern "C" void ime_alloc_leave(void) {}

#endif
