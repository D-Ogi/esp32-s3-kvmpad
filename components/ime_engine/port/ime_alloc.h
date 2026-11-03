// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Keeps the IME engines' heap in PSRAM (TASK-072 footprint spike, board #1: PinyinIME took ~47 KiB of internal
// RAM through allocations below CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, leaving an 8 KiB minimum).
//
// - The vendored sources are compiled with malloc/calloc/realloc/free renamed to ime_engine_* (CMakeLists.txt),
//   so their C allocations prefer PSRAM; the sources themselves are unchanged.
// - Their `new` expressions use the global operator new, which ime_alloc.cpp replaces: it prefers PSRAM only on
//   the task that is inside an engine call (ImeAllocScope); every other task (NVS, LVGL) gets plain malloc(),
//   exactly as before. Memory from either path is released by free(), so delete is uniform.
// On a host build these are plain malloc() and the scope is empty.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *ime_engine_malloc(size_t n);
void *ime_engine_calloc(size_t n, size_t size);
void *ime_engine_realloc(void *p, size_t n);
void ime_engine_free(void *p);

/** Mark the calling task as inside an engine call (nesting allowed). */
void ime_alloc_enter(void);
void ime_alloc_leave(void);

#ifdef __cplusplus
}

struct ImeAllocScope {
    ImeAllocScope() { ime_alloc_enter(); }
    ~ImeAllocScope() { ime_alloc_leave(); }
    ImeAllocScope(const ImeAllocScope &) = delete;
    ImeAllocScope &operator=(const ImeAllocScope &) = delete;
};
#endif
