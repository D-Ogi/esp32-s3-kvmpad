// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef struct pack_video pack_video_t;
typedef union {
    uint32_t alignment;
    uint8_t bytes[16 * 1024];
} pack_video_window_t;
typedef struct {
    unsigned width, height, frames;
} pack_video_info_t;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
// Worker-owned wall times. Binding includes source reads, SHA-256 and seeks.
// Prefill follows the 64-byte KVC1 header. Refills cover only fread in take_bytes.
typedef struct {
    int64_t bind_us, prefill_us, refill_us;
    uint64_t bind_bytes, refill_bytes;
    unsigned prefill_bytes, refill_calls;
} pack_video_io_t;
void pack_video_get_io(const pack_video_t *video, pack_video_io_t *out);
#endif
// Source and companion remain owned by the caller. The source position is reset on return.
esp_err_t pack_video_open(FILE *source, FILE *companion, unsigned mode, bool (*stopped)(void),
                          pack_video_t **out, pack_video_info_t *info);
// A supplied window remains owned by the caller and must live until pack_video_close().
esp_err_t pack_video_open_with_window(FILE *source, FILE *companion, unsigned mode, bool (*stopped)(void),
                                      pack_video_window_t *window, pack_video_t **out, pack_video_info_t *info);
void pack_video_close(pack_video_t *video);
// pixels may equal previous when the presenter has released its single surface.
bool pack_video_read(void *video, uint16_t *pixels, const uint16_t *previous, uint64_t *pts_us,
                     uint32_t *crc, bool (*cancelled)(void *), void *cancel_ctx);
// On success, duration_us is the duration stored for this picture (1 to 1000000 us).
bool pack_video_read_timed(void *video, uint16_t *pixels, const uint16_t *previous, uint64_t *pts_us,
                           uint32_t *duration_us, uint32_t *crc, bool (*cancelled)(void *), void *cancel_ctx);
