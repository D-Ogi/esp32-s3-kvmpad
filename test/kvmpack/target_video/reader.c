// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_video.h"
#include <string.h>

extern const uint8_t clip[], clip_end[];
volatile uint32_t result[8];
static uint16_t pictures[2][960 * 540];
static pack_video_window_t window;
static unsigned heap[16384 / sizeof(unsigned)], allocated;
static size_t source_pos, companion_pos;
static FILE *const source = (FILE *)1, *const companion = (FILE *)2;

size_t __wrap_fread(void *data, size_t size, size_t count, FILE *file)
{
    if (size != 1) return 0;
    if (file == source) {
        memset(data, 0, count);
        source_pos += count;
    } else {
        size_t available = (size_t)(clip_end - clip) - companion_pos;
        if (count > available) count = available;
        memcpy(data, clip + companion_pos, count);
        companion_pos += count;
    }
    return count;
}

int __wrap_fseek(FILE *file, long offset, int origin)
{
    uint32_t length;
    memcpy(&length, clip + 16, sizeof(length));
    if (file != source) return -1;
    source_pos = origin == SEEK_END ? length : (size_t)offset;
    return 0;
}

long __wrap_ftell(FILE *file) { return source_pos; }
int __wrap_fgetc(FILE *file) { return companion_pos == (size_t)(clip_end - clip) ? EOF : clip[companion_pos++]; }
int __wrap_ferror(FILE *file) { return 0; }

void *heap_caps_malloc(size_t size, unsigned caps)
{
    if (size > sizeof(heap) - allocated) return NULL;
    uint8_t *data = (uint8_t *)heap + allocated;
    allocated += (size + 3) & ~(size_t)3;
    return data;
}

void heap_caps_free(void *data) {}
void *zcalloc(void *ctx, unsigned count, unsigned size) { ++result[7]; return NULL; }
void zcfree(void *ctx, void *data) {}

// The host checks the original SHA-256 before this run. Only pack_video_read
// is timed here. These stubs let the production open function initialize it.
int psa_crypto_init(void) { return 0; }
int psa_hash_setup(void *op, unsigned alg) { return 0; }
int psa_hash_update(void *op, const uint8_t *data, size_t n) { return 0; }
int psa_hash_finish(void *op, uint8_t *out, size_t size, size_t *used)
{
    memcpy(out, clip + 24, 32);
    *used = 32;
    return 0;
}
int psa_hash_abort(void *op) { return 0; }
unsigned __stack_chk_guard = 0x73c691b5;
void __stack_chk_fail(void) { result[0] = 0x1234; for (;;) {} }

static inline uint32_t clock_count(void)
{
    uint32_t value;
    __asm__ volatile ("rsr.ccount %0" : "=a"(value) :: "memory");
    return value;
}

void qualify(void)
{
    pack_video_t *video = NULL;
    pack_video_info_t info;
    unsigned mode;
    memcpy(&mode, clip + 8, sizeof(mode));
    if (pack_video_open_with_window(source, companion, mode, NULL, &window, &video, &info) != 0)
        goto done;

    uintptr_t sp;
    __asm__ volatile ("mov %0,a1" : "=a"(sp));
    volatile uint32_t *low = (uint32_t *)0x401e0000;
    for (volatile uint32_t *p = low; (uintptr_t)p < sp - 256; ++p)
        *p = 0x5a31c84e;

    uint32_t crc_hash = 0, pts_hash = 0;
    for (unsigned i = 0; i < info.frames; ++i) {
        uint32_t crc;
        uint64_t pts;
        uint32_t start = clock_count();
        if (!pack_video_read(video, pictures[i & 1], i ? pictures[(i & 1) ^ 1] : NULL, &pts, &crc, NULL, NULL))
            goto done;
        result[1] += clock_count() - start;
        crc_hash = (crc_hash << 1 | crc_hash >> 31) ^ crc;
        pts_hash = (pts_hash << 1 | pts_hash >> 31) ^ (uint32_t)pts;
        result[2] = i + 1;
    }
    while ((uintptr_t)low < sp - 256 && *low == 0x5a31c84e) ++low;
    result[3] = sp - (uintptr_t)low;
    result[4] = crc_hash;
    result[5] = pts_hash;
    result[6] = allocated;
done:
    result[0] = 0x1234;
}
