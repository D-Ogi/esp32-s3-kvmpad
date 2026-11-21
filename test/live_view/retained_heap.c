// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "tlsf.h"
#include <stdint.h>
#include <stdlib.h>

// Exercise the same ESP-IDF TLSF policy on a real bounded pool on the host.
// The FFmpeg object layout is that of the host ABI. Only a target run measures
// Xtensa and the outer firmware/WSS heaps.
static tlsf_t pool;
static pool_t blocks;
static void *memory;
static _Alignas(16) uint8_t control[32768];

int kvm_test_pool_open(size_t bytes)
{
    if (pool || !(memory = malloc(bytes)))
        return 0;
    // ESP-IDF uses 32-bit pointers on target. On this 64-bit host the last
    // control-table pointer may need additional alignment. Place the pool at a
    // separately aligned address and charge its control bytes against admission.
    pool = tlsf_create(control, bytes);
    size_t overhead = pool ? tlsf_size(pool) + 16 : 0;
    if (pool && overhead <= sizeof(control) && bytes > overhead)
        blocks = tlsf_add_pool(pool, memory, bytes - overhead);
    return blocks != NULL;
}

int kvm_test_pool_close(void)
{
    int valid = blocks && !tlsf_check(pool) && !tlsf_check_pool(blocks);
    tlsf_destroy(pool);
    free(memory);
    pool = blocks = memory = NULL;
    return valid;
}

void *kvm_probe_malloc(size_t bytes)
{
    return pool ? tlsf_memalign(pool, 16, bytes) : NULL;
}

void *kvm_probe_realloc(void *p, size_t bytes)
{
    return p ? tlsf_realloc(pool, p, bytes) : kvm_probe_malloc(bytes);
}

void kvm_probe_free(void *p)
{
    if (p)
        tlsf_free(pool, p);
}
