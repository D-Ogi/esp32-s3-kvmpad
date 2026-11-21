// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <malloc.h>
#define probe_alloc(n) _aligned_malloc(n, 16)
#define probe_resize(p, n) _aligned_realloc(p, n, 16)
#define probe_dispose(p) _aligned_free(p)
#else
#define probe_alloc(n) malloc(n)
#define probe_resize(p, n) realloc(p, n)
#define probe_dispose(p) free(p)
#endif
#ifdef KVM_TRACE_ALLOCATIONS
#include <windows.h>
#endif

// Native qualification only. Build libavutil/mem.c with MALLOC_PREFIX=kvm_probe_
// and aligned allocation probes disabled, then link it before the archive.
typedef union {
    max_align_t alignment;
    _Alignas(16) unsigned char simd_alignment;
    struct {
        size_t size;
#ifdef KVM_TRACE_ALLOCATIONS
        unsigned slot;
#endif
    };
} block_t;

static size_t live, peak;

#ifdef KVM_TRACE_ALLOCATIONS
// A snapshot of large allocations still live at the measured high-water mark.
typedef struct {
    size_t size;
    void *stack[16];
} allocation_t;
static allocation_t blocks[256], peak_blocks[256];

static void record(block_t *p)
{
    p->slot = 0;
    if (p->size >= 32768) {
        for (unsigned i = 1; i < 256; ++i) {
            if (blocks[i].size)
                continue;
            blocks[i].size = p->size;
            CaptureStackBackTrace(1, 16, blocks[i].stack, NULL);
            p->slot = i;
            break;
        }
    }
    if (live > peak)
        memcpy(peak_blocks, blocks, sizeof(blocks));
}
#else
#define record(p) ((void)0)
#endif

void *kvm_probe_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(block_t))
        return NULL;
    block_t *p = probe_alloc(sizeof(*p) + size);
    if (!p)
        return NULL;
    p->size = size;
    live += size;
    record(p);
    if (live > peak)
        peak = live;
    return p + 1;
}

void kvm_probe_free(void *p)
{
    if (p) {
        block_t *block = (block_t *)p - 1;
        live -= block->size;
#ifdef KVM_TRACE_ALLOCATIONS
        if (block->slot)
            blocks[block->slot] = (allocation_t){0};
#endif
        probe_dispose(block);
    }
}

void *kvm_probe_realloc(void *p, size_t size)
{
    if (!p)
        return kvm_probe_malloc(size);
    if (size > SIZE_MAX - sizeof(block_t))
        return NULL;
    block_t *block = (block_t *)p - 1;
    size_t old = block->size;
    block = probe_resize(block, sizeof(*block) + size);
    if (!block)
        return NULL;
    block->size = size;
    live = live - old + size;
#ifdef KVM_TRACE_ALLOCATIONS
    if (block->slot)
        blocks[block->slot] = (allocation_t){0};
#endif
    record(block);
    if (live > peak)
        peak = live;
    return block + 1;
}

void kvm_probe_report(void)
{
    fprintf(stderr, "ffmpeg_requested_peak=%zu remaining=%zu\n", peak, live);
#ifdef KVM_TRACE_ALLOCATIONS
    uintptr_t base = (uintptr_t)GetModuleHandle(NULL);
    for (unsigned i = 1; i < 256; ++i) {
        if (!peak_blocks[i].size)
            continue;
        fprintf(stderr, "block %zu", peak_blocks[i].size);
        for (unsigned n = 0; n < 16 && peak_blocks[i].stack[n]; ++n)
            fprintf(stderr, " %zx", (size_t)((uintptr_t)peak_blocks[i].stack[n] - base));
        fputc('\n', stderr);
    }
#endif
}
