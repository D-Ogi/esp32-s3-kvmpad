// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "preboot_capture.h"
#include "screenshot_volume.h"
#include "diskio_impl.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void *ff_memalloc(size_t bytes) { return malloc(bytes); }
void ff_memfree(void *p) { free(p); }
int ff_mutex_create(int volume) { (void)volume; return 1; }
void ff_mutex_delete(int volume) { (void)volume; }
int ff_mutex_take(int volume) { (void)volume; return 1; }
void ff_mutex_give(int volume) { (void)volume; }
void *heap_caps_malloc(size_t n, unsigned caps) { (void)caps; return malloc(n); }
void heap_caps_free(void *p) { free(p); }
uint32_t esp_random(void) { return 7; }

static screenshot_volume_io_t writable;
static DSTATUS present(BYTE drive) { return drive == 1 ? 0 : STA_NOINIT; }
static DRESULT read_image(BYTE drive, BYTE *out, uint32_t sector, UINT count)
{
    return drive == 1 && screenshot_volume_read(&writable, sector, count, out) ? RES_OK : RES_ERROR;
}
static DRESULT write_image(BYTE drive, const BYTE *in, uint32_t sector, UINT count)
{
    return drive == 1 && screenshot_volume_write(&writable, sector, count, in) ? RES_OK : RES_ERROR;
}
static DRESULT image_ioctl(BYTE drive, BYTE cmd, void *data)
{
    assert(drive == 1);
    switch (cmd) {
    case GET_BLOCK_SIZE: *(DWORD *)data = 1; return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t *)data = SCREENSHOT_SECTORS; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)data = SCREENSHOT_SECTOR_BYTES; return RES_OK;
    case CTRL_SYNC: return screenshot_volume_sync(&writable) ? RES_OK : RES_ERROR;
    default: return RES_PARERR;
    }
}

static void replace_bmp(FILE *image, const char *source)
{
    assert(screenshot_volume_open_file(image, &writable));
    const ff_diskio_impl_t ops = {.init=present, .status=present, .read=read_image,
                                  .write=write_image, .ioctl=image_ioctl};
    ff_diskio_register(1, &ops);
    FATFS fs = {0};
    FIL file = {0};
    assert(f_mount(&fs, "1:", 1) == FR_OK && fs.fs_type == FS_FAT16);
    assert(f_open(&file, "1:/11221234.BMP", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    FILE *seed = fopen(source, "rb");
    assert(seed);
    BYTE buf[4096];
    size_t n;
    UINT written;
    while ((n = fread(buf, 1, sizeof(buf), seed)) != 0) {
        assert(f_write(&file, buf, n, &written) == FR_OK && n == written);
    }
    assert(!ferror(seed) && !fclose(seed) && f_close(&file) == FR_OK);
    assert(f_mount(NULL, "1:", 0) == FR_OK);
    ff_diskio_unregister(1);
}

int main(int argc, char **argv)
{
    assert(argc >= 4);
    bool replace = !strcmp(argv[1], "replace");
    FILE *image = fopen(argv[2], replace ? "rb+" : "rb");
    assert(image);
    /* The outer card owns logical drive zero for the whole transaction. Its
     * registered sentinel operations must never be called by the inner FS. */
    const ff_diskio_impl_t outer = {0};
    ff_diskio_register(0, &outer);
    if (replace) {
        replace_bmp(image, argv[3]);
    } else {
        assert(!strcmp(argv[1], "read"));
        for (unsigned i = 0; i < 2; ++i) {
            preboot_picture_t picture;
            int expected_error = atoi(argv[3]);
            esp_err_t result = preboot_read_latest(image, &picture);
            assert(result == expected_error);
            BYTE next = FF_DRV_NOT_USED;
            assert(ff_diskio_get_drive(&next) == ESP_OK && next == 1);
            if (result == ESP_OK) {
                assert(argc == 7 && !strcmp(picture.name, "11221234.BMP"));
                unsigned width = (unsigned)atoi(argv[4]), height = (unsigned)atoi(argv[5]);
                assert(picture.width == width && picture.height == height && picture.pixels);
                FILE *ref = fopen(argv[6], "rb");
                assert(ref);
                for (unsigned y = 0; y < height; ++y) {
                    uint16_t line[960];
                    assert(width <= 960 && fread(line, sizeof(*line), width, ref) == width);
                    assert(!memcmp(line, picture.pixels + y * width, width * sizeof(*line)));
                }
                assert(fgetc(ref) == EOF && !ferror(ref) && !fclose(ref));
                free(picture.pixels);
            } else assert(!picture.width && !picture.height && !picture.pixels);
        }
    }
    ff_diskio_unregister(0);
    assert(!fclose(image));
    return 0;
}
