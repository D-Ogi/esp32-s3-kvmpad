// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "screenshot_volume.h"
#include <string.h>

enum { FAT_SECTORS = 64, ROOT_ENTRIES = 512, ROOT_START = 1 + 2 * FAT_SECTORS,
       CLUSTER_SECTORS = 4, DATA_START = ROOT_START + ROOT_ENTRIES * 32 / SCREENSHOT_SECTOR_BYTES,
       CLUSTERS = (SCREENSHOT_SECTORS - DATA_START) / CLUSTER_SECTORS };
_Static_assert(CLUSTERS >= 4085 && CLUSTERS < 65525, "FAT16 cluster count");
_Static_assert((CLUSTERS + 2) * 2 <= FAT_SECTORS * SCREENSHOT_SECTOR_BYTES, "FAT capacity");
static const char label[] = "KVMPADSHOT ";
_Static_assert(sizeof(label) - 1 == 11, "FAT volume label width");

static bool valid_range(uint32_t lba, uint32_t count)
{
    return count && lba < SCREENSHOT_SECTORS && count <= SCREENSHOT_SECTORS - lba;
}

bool screenshot_volume_read(const screenshot_volume_io_t *io, uint32_t lba, uint32_t count, void *data)
{
    return io && io->read && data && valid_range(lba, count) && io->read(io->context, lba, count, data);
}

bool screenshot_volume_write(const screenshot_volume_io_t *io, uint32_t lba, uint32_t count, const void *data)
{
    return io && io->write && data && valid_range(lba, count) && io->write(io->context, lba, count, data);
}

bool screenshot_volume_sync(const screenshot_volume_io_t *io)
{
    return io && io->sync && io->sync(io->context);
}

static void le16(uint8_t *p, unsigned n)
{
    p[0] = (uint8_t)n;
    p[1] = (uint8_t)(n >> 8);
}

static void le32(uint8_t *p, uint32_t n)
{
    le16(p, n);
    le16(p + 2, n >> 16);
}

bool screenshot_volume_format(const screenshot_volume_io_t *io, uint32_t serial,
                              void *scratch, size_t scratch_bytes)
{
    if (!io || !io->write || !io->sync || !scratch || scratch_bytes < SCREENSHOT_SECTOR_BYTES) return false;
    uint8_t *b = scratch;
    size_t capacity = scratch_bytes / SCREENSHOT_SECTOR_BYTES;
    uint32_t chunk = capacity < SCREENSHOT_SECTORS ? (uint32_t)capacity : SCREENSHOT_SECTORS;
    memset(b, 0, chunk * SCREENSHOT_SECTOR_BYTES);
    for (uint32_t lba = 0; lba < SCREENSHOT_SECTORS;) {
        uint32_t left = SCREENSHOT_SECTORS - lba;
        uint32_t count = left < chunk ? left : chunk;
        if (!screenshot_volume_write(io, lba, count, b)) return false;
        lba += count;
    }

    /* FAT[0] carries the media byte, FAT[1] is reserved, clean and error-free. */
    le16(b, 0xfff8);
    le16(b + 2, 0xffff);
    if (!screenshot_volume_write(io, 1, 1, b) ||
        !screenshot_volume_write(io, 1 + FAT_SECTORS, 1, b)) return false;
    memcpy(b, label, sizeof(label) - 1);
    b[11] = 0x08;
    if (!screenshot_volume_write(io, ROOT_START, 1, b) || !screenshot_volume_sync(io)) return false;

    memset(b, 0, SCREENSHOT_SECTOR_BYTES);
    b[0] = 0xeb; b[1] = 0x3c; b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    le16(b + 11, SCREENSHOT_SECTOR_BYTES);
    b[13] = CLUSTER_SECTORS;
    le16(b + 14, 1);
    b[16] = 2;
    le16(b + 17, ROOT_ENTRIES);
    b[21] = 0xf8;
    le16(b + 22, FAT_SECTORS);
    le16(b + 24, 63);
    le16(b + 26, 255);
    le32(b + 32, SCREENSHOT_SECTORS);
    b[36] = 0x80;
    b[38] = 0x29;
    le32(b + 39, serial);
    memcpy(b + 43, label, sizeof(label) - 1);
    memcpy(b + 54, "FAT16   ", 8);
    b[62] = 0xcd; b[63] = 0x18; /* Legacy BIOS: try the next boot option. */
    b[510] = 0x55; b[511] = 0xaa;
    return screenshot_volume_write(io, 0, 1, b) && screenshot_volume_sync(io);
}
