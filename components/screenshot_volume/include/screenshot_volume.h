// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

enum { SCREENSHOT_SECTOR_BYTES = 512, SCREENSHOT_SECTORS = 65536,
       SCREENSHOT_VOLUME_BYTES = SCREENSHOT_SECTORS * SCREENSHOT_SECTOR_BYTES };

/* One private image, one serialized owner. LBA zero is the start of the image.
 * Do not mount its filesystem locally while another writer owns the image. */
typedef struct {
    void *context;
    bool (*read)(void *context, uint32_t lba, uint32_t sectors, void *data);
    bool (*write)(void *context, uint32_t lba, uint32_t sectors, const void *data);
    bool (*sync)(void *context);
} screenshot_volume_io_t;

bool screenshot_volume_read(const screenshot_volume_io_t *io, uint32_t lba, uint32_t sectors, void *data);
bool screenshot_volume_write(const screenshot_volume_io_t *io, uint32_t lba, uint32_t sectors, const void *data);
bool screenshot_volume_sync(const screenshot_volume_io_t *io);

/* Format a NEW private image. Zero every sector including free data, flush
 * metadata, then publish the BPB. Scratch is at least one sector; 4 KiB or
 * more reduces SD calls. A failed construction must not be exported. */
bool screenshot_volume_format(const screenshot_volume_io_t *io, uint32_t serial,
                              void *scratch, size_t scratch_bytes);

/* File stream remains caller-owned. Create requires an empty, exclusively
 * created binary update stream. Open accepts only an exact-size regular file
 * and never formats it. The caller publishes the file only after creation. */
bool screenshot_volume_create_file(FILE *file, uint32_t serial, void *scratch, size_t scratch_bytes);
bool screenshot_volume_open_file(FILE *file, screenshot_volume_io_t *out);
