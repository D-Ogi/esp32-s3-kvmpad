// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Run the IDF FatFs library against a memory disk. The temporary image is the
// only host file used; every named video and companion lives inside exFAT.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"

enum { SECTORS = 16384, SECTOR_BYTES = 512 };
static BYTE disk[SECTORS][SECTOR_BYTES];
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};
void *ff_memalloc(size_t n) { return malloc(n); }
void ff_memfree(void *p) { free(p); }
int ff_mutex_create(int vol) { (void)vol; return 1; }
void ff_mutex_delete(int vol) { (void)vol; }
int ff_mutex_take(int vol) { (void)vol; return 1; }
void ff_mutex_give(int vol) { (void)vol; }
static DWORD format_time = (46u << 25) | (1u << 21) | (1u << 16);
DWORD get_fattime(void) { return format_time; }
DSTATUS disk_initialize(BYTE drive) { return drive ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count)
{
    if (drive || !count || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(data, disk[sector], count * SECTOR_BYTES);
    return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count)
{
    if (drive || !count || sector >= SECTORS || count > SECTORS - sector) return RES_PARERR;
    memcpy(disk[sector], data, count * SECTOR_BYTES);
    return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *data)
{
    if (drive) return RES_PARERR;
    switch (command) {
    case GET_SECTOR_COUNT: *(LBA_t *)data = SECTORS; break;
    case GET_SECTOR_SIZE: *(WORD *)data = SECTOR_BYTES; break;
    case GET_BLOCK_SIZE: *(DWORD *)data = 1; break;
    case CTRL_SYNC: case CTRL_TRIM: break;
    default: return RES_PARERR;
    }
    return RES_OK;
}

static const char *const names[] = {
    "Flight.mp4", "Zażółć gęślą jaźń.mp4", "東京-漢字.h264",
    "飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行.mp4",
    "東京-漢字.h264.kvc",
    "飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行飛行Z.mp4"
};
static const unsigned sizes[] = {7, 70123, 8049, 102113, 128, 149};

static void check_contents(unsigned index, const char *name, int write)
{
    FIL file = {0};
    FILINFO info;
    assert(f_open(&file, name, write ? FA_CREATE_NEW | FA_WRITE : FA_READ) == FR_OK);
    if (!write) {
        assert(f_stat(name, &info) == FR_OK && info.fsize == sizes[index]);
        assert(f_size(&file) == sizes[index]);
    }
    for (unsigned offset = 0; offset < sizes[index];) {
        BYTE expected[513], actual[sizeof(expected)];
        UINT count = sizes[index] - offset, transferred = 0;
        if (count > sizeof(expected)) count = sizeof(expected);
        for (unsigned i = 0; i < count; ++i) expected[i] = (BYTE)(index * 43 + offset + i);
        if (write) {
            assert(f_write(&file, expected, count, &transferred) == FR_OK);
        } else {
            assert(f_read(&file, actual, count, &transferred) == FR_OK);
            assert(!memcmp(actual, expected, count));
        }
        assert(transferred == count);
        offset += count;
    }
    assert(f_close(&file) == FR_OK);
}

int main(int argc, char **argv)
{
#ifdef KVM_TEST_CARD_MOUNT
    if (argc == 2 && !strcmp(argv[1], "remount")) {
        void check_card_mount(void *medium, size_t bytes, DWORD *clock);
        check_card_mount(disk, sizeof(disk), &format_time);
        return 0;
    }
#endif
    assert(argc == 3 && sizeof(FSIZE_t) == 8 && strlen(names[3]) == 100 && strlen(names[5]) == 101);
    const int seed = !strcmp(argv[1], "seed");
    FATFS fs = {0};
    if (seed) {
        assert(FF_LFN_UNICODE == 2);
        BYTE work[FF_MAX_SS];
        MKFS_PARM opt = {.fmt = FM_EXFAT | FM_SFD};
        assert(f_mkfs("", &opt, work, sizeof(work)) == FR_OK);
    } else {
        assert(!strcmp(argv[1], "check"));
        FILE *source = fopen(argv[2], "rb");
        assert(source && fread(disk, sizeof(disk), 1, source) == 1 && !fclose(source));
    }
    assert(f_mount(&fs, "", 1) == FR_OK && fs.fs_type == FS_EXFAT);
    if (seed) {
        for (unsigned i = 0; i < sizeof(names)/sizeof(*names); ++i) check_contents(i, names[i], 1);
    } else {
        FF_DIR dir = {0};
        FILINFO info;
        unsigned entries = 0, verified = 0, files = 0, payload_bytes = 0;
        assert(f_opendir(&dir, "/") == FR_OK);
        for (;;) {
            assert(f_readdir(&dir, &info) == FR_OK);
            if (!info.fname[0]) break;
            ++entries;
            for (unsigned i = 0; i < sizeof(names)/sizeof(*names); ++i) {
                if (!strcmp(info.fname, names[i])) {
                    assert(!(verified & (1u << i)));
                    check_contents(i, info.fname, 0);
                    verified |= 1u << i;
                    ++files;
                    payload_bytes += sizes[i];
                }
            }
        }
        assert(f_closedir(&dir) == FR_OK);
        assert(entries == 6 && verified == (FF_LFN_UNICODE == 2 ? 63u : 1u));
        printf("exFAT encoding=%u directory_entries=%u exact_names_reopened=%u valid_payload_bytes=%u\n",
               FF_LFN_UNICODE, entries, files, payload_bytes);
    }
    assert(f_mount(NULL, "", 0) == FR_OK);
    if (seed) {
        FILE *dest = fopen(argv[2], "wb");
        assert(dest && fwrite(disk, sizeof(disk), 1, dest) == 1 && !fclose(dest));
    }
    return 0;
}
