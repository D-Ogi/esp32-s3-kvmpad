// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "screenshot_volume.h"
#include "ff.h"
#include "diskio.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static screenshot_volume_io_t volume;
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};
void *ff_memalloc(size_t n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }
int ff_mutex_create(int vol) { (void)vol; return 1; }
void ff_mutex_delete(int vol) { (void)vol; }
int ff_mutex_take(int vol) { (void)vol; return 1; }
void ff_mutex_give(int vol) { (void)vol; }
DWORD get_fattime(void) { return (46u << 25) | (1u << 21) | (1u << 16); }
DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t lba, UINT count)
{
    return !drive && lba < SCREENSHOT_SECTORS && screenshot_volume_read(&volume, (uint32_t)lba, count, data)
           ? RES_OK : RES_ERROR;
}
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t lba, UINT count)
{
    return !drive && lba < SCREENSHOT_SECTORS && screenshot_volume_write(&volume, (uint32_t)lba, count, data)
           ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE drive, BYTE cmd, void *p)
{
    if (drive) return RES_PARERR;
    switch (cmd) {
    case CTRL_SYNC: return screenshot_volume_sync(&volume) ? RES_OK : RES_ERROR;
    case GET_BLOCK_SIZE: *(DWORD *)p = 1; return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)p = SCREENSHOT_SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)p = SCREENSHOT_SECTOR_BYTES; return RES_OK;
    default: return RES_PARERR;
    }
}

static unsigned remaining;
static bool published;
static bool fail_write(void *ctx, uint32_t lba, uint32_t n, const void *data)
{
    (void)ctx;
    assert(lba < SCREENSHOT_SECTORS && n <= SCREENSHOT_SECTORS - lba);
    if (!remaining) return false;
    remaining--;
    if (!lba && ((const uint8_t *)data)[510] == 0x55) published = true;
    return true;
}
static bool fail_sync(void *ctx) { (void)ctx; return false; }

static void format_file(FILE *stream)
{
    uint8_t scratch[4096];
    assert(!screenshot_volume_open_file(stream, &volume) && !volume.read);
    screenshot_volume_io_t fault = { .write = fail_write, .sync = fail_sync };
    remaining = 0;
    assert(!screenshot_volume_format(&fault, 1, scratch, 511));
    remaining = 7;
    assert(!screenshot_volume_format(&fault, 1, scratch, sizeof(scratch)) && !published);
    remaining = 10000;
    assert(!screenshot_volume_format(&fault, 1, scratch, sizeof(scratch)) && !published);
    assert(screenshot_volume_create_file(stream, 0x12345678, scratch, sizeof(scratch)));
    assert(!screenshot_volume_create_file(stream, 0, scratch, sizeof(scratch)));
    assert(screenshot_volume_open_file(stream, &volume));
    assert(!screenshot_volume_write(&volume, UINT32_MAX, 1, scratch));
    assert(!screenshot_volume_write(&volume, SCREENSHOT_SECTORS - 1, 2, scratch));
    assert(!screenshot_volume_read(&volume, 1, UINT32_MAX, scratch));
    assert(!screenshot_volume_write(&volume, 0, 0, scratch));
    assert(!screenshot_volume_read(&volume, SCREENSHOT_SECTORS, 1, scratch));
    assert(screenshot_volume_read(&volume, SCREENSHOT_SECTORS - 1, 1, scratch));
    for (size_t i = 0; i < SCREENSHOT_SECTOR_BYTES; ++i) assert(!scratch[i]);
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    bool create = !strcmp(argv[1], "create");
    FILE *image = fopen(argv[2], create ? "rb+" : "rb");
    assert(image);
    if (create) format_file(image);
    else assert(!strcmp(argv[1], "verify") && screenshot_volume_open_file(image, &volume));

    FATFS fs = {0};
    assert(f_mount(&fs, "", 1) == FR_OK && fs.fs_type == FS_FAT16);
    assert(fs.n_fats == 2 && fs.csize == 4 && fs.n_fatent == 16345);
    FF_DIR dir = {0};
    FILINFO entry;
    assert(f_opendir(&dir, "/") == FR_OK && f_readdir(&dir, &entry) == FR_OK);
    if (create) {
        assert(!entry.fname[0]);
        puts("PASS initial empty FAT16, two FATs, 16343 clusters; bounds and interrupted format");
    } else {
        assert(!strcmp(entry.fname, "11221234.BMP"));
        FILE *reference = fopen(argv[3], "rb");
        FIL file = {0};
        assert(reference && f_open(&file, entry.fname, FA_READ) == FR_OK);
        uint8_t expected[775], actual[775];
        size_t count, total = 0;
        UINT got;
        while ((count = fread(expected, 1, sizeof(expected), reference))) {
            assert(f_read(&file, actual, (UINT)count, &got) == FR_OK && got == count);
            assert(!memcmp(expected, actual, count));
            total += count;
        }
        assert(!ferror(reference) && total == entry.fsize && total > 65536 && f_eof(&file));
        assert(!fclose(reference) && f_close(&file) == FR_OK);
        assert(f_readdir(&dir, &entry) == FR_OK && !entry.fname[0]);
        printf("PASS pinned FatFs reopened 11221234.BMP, %zu bytes identical across clusters\n", total);
    }
    assert(f_closedir(&dir) == FR_OK && f_mount(NULL, "", 0) == FR_OK && !fclose(image));
    return 0;
}
