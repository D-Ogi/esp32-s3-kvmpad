// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "preboot_capture.h"
#include "screenshot_volume.h"
#include "pack_card.h"
#include "diskio_impl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define IMAGE_DIR PACK_MOUNT "/KVM-CAP"
#define IMAGE_PATH IMAGE_DIR "/CAPTURE.IMG"

esp_err_t preboot_volume_open(FILE **image)
{
    *image = NULL;
    if (mkdir(IMAGE_DIR, 0770) && errno != EEXIST) return ESP_FAIL;
    FILE *file = fopen(IMAGE_PATH, "r+b");
    if (!file) {
        if (errno != ENOENT) return ESP_FAIL;
        // Exclusive creation prevents a partial or unrelated file becoming an export.
        char pending[sizeof(IMAGE_DIR) + 16];
        snprintf(pending, sizeof(pending), "%s/%08lx.TMP", IMAGE_DIR, (unsigned long)esp_random());
        int fd = open(pending, O_CREAT | O_EXCL | O_RDWR, 0660);
        if (fd < 0) return ESP_FAIL;
        file = fdopen(fd, "w+b");
        if (!file) { close(fd); unlink(pending); return ESP_FAIL; }
        uint8_t scratch[4096];
        bool ok = screenshot_volume_create_file(file, esp_random(), scratch, sizeof(scratch));
        if (fclose(file)) ok = false;
        if (!ok || rename(pending, IMAGE_PATH)) {
            unlink(pending);
            return ESP_FAIL;
        }
        file = fopen(IMAGE_PATH, "r+b");
        if (!file) return ESP_FAIL;
    }
    screenshot_volume_io_t io;
    uint8_t boot[SCREENSHOT_SECTOR_BYTES];
    if (!screenshot_volume_open_file(file, &io) || !screenshot_volume_read(&io, 0, 1, boot) ||
        memcmp(boot + 43, "KVMPADSHOT FAT16   ", 19) || boot[510] != 0x55 || boot[511] != 0xaa) {
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }
    *image = file;
    return ESP_OK;
}

// A temporary, read-only FatFs drive owned by this worker. USB releases
// the private volume before we register it. The outer SD remains mounted.
static BYTE drive = FF_DRV_NOT_USED;
static screenshot_volume_io_t reader;
static DSTATUS ready(BYTE p) { return p == drive ? 0 : STA_NOINIT; }
static DRESULT read_disk(BYTE p, BYTE *dest, uint32_t lba, UINT count)
{
    return p == drive && screenshot_volume_read(&reader, lba, count, dest) ? RES_OK : RES_ERROR;
}
static DRESULT protect(BYTE p, const BYTE *src, uint32_t lba, UINT count)
{
    (void)p; (void)src; (void)lba; (void)count;
    return RES_WRPRT;
}
static DRESULT query(BYTE p, BYTE cmd, void *arg)
{
    if (p != drive) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_SIZE: *(WORD *)arg = SCREENSHOT_SECTOR_BYTES; return RES_OK;
    case GET_SECTOR_COUNT: *(DWORD *)arg = SCREENSHOT_SECTORS; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)arg = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

static uint16_t u16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }

static esp_err_t decode(FIL *file, preboot_picture_t *out)
{
    uint8_t header[54];
    UINT got;
    if (f_read(file, header, sizeof(header), &got) || got != sizeof(header)) return ESP_FAIL;
    const uint32_t start = u32(header + 10), dib = u32(header + 14), w = u32(header + 18);
    const int32_t signed_h = (int32_t)u32(header + 22);
    unsigned bpp = u16(header + 28);
    if (u16(header) != 0x4d42 || dib < 40 || start < 54 || dib > start - 14 ||
        !w || w > 8192 || !signed_h || signed_h < -8192 || signed_h > 8192 ||
        u16(header + 26) != 1 || (bpp != 24 && bpp != 32) || u32(header + 30))
        return ESP_ERR_NOT_SUPPORTED;
    uint32_t h = signed_h < 0 ? -signed_h : signed_h;
    unsigned bytes = bpp / 8, stride = (w * bytes + 3u) & ~3u;
    if ((uint64_t)start + (uint64_t)h * stride > f_size(file)) return ESP_ERR_INVALID_SIZE;
    unsigned width = w, height = h;
    if (w > 960 || h > 432) {
        /* Compare the two scale factors before rounding either dimension. */
        if (w * 432u >= h * 960u) {
            width = 960;
            height = h * 960u / w;
        } else {
            height = 432;
            width = w * 432u / h;
        }
    }
    if (!width) width = 1;
    if (!height) height = 1;
    uint16_t *pixels = heap_caps_malloc(width * height * sizeof(*pixels), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *row = heap_caps_malloc(stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_ERR_NO_MEM;
    if (pixels && row) {
        err = ESP_OK;
        // Read in file order, including bottom-up BMPs, to keep SD I/O sequential.
        for (unsigned i = 0; i < height; ++i) {
            unsigned y = signed_h < 0 ? i : height - 1 - i;
            unsigned sy = y * h / height;
            unsigned file_row = signed_h < 0 ? sy : h - 1 - sy;
            if (f_lseek(file, start + file_row * stride) || f_read(file, row, stride, &got) || got != stride) {
                err = ESP_FAIL;
                break;
            }
            for (unsigned x = 0; x < width; ++x) {
                const uint8_t *bgr = row + bytes * (x * w / width);
                pixels[y * width + x] = (uint16_t)((bgr[2] >> 3) << 11 | (bgr[1] >> 2) << 5 | bgr[0] >> 3);
            }
        }
    }
    heap_caps_free(row);
    if (err == ESP_OK) { out->pixels = pixels; out->width = width; out->height = height; }
    else heap_caps_free(pixels);
    return err;
}

static esp_err_t latest(char *root, preboot_picture_t *out)
{
    FF_DIR dir = {0};
    if (f_opendir(&dir, root) != FR_OK) return ESP_FAIL;
    FILINFO info;
    uint32_t stamp = 0;
    FRESULT r;
    while ((r = f_readdir(&dir, &info)) == FR_OK && *info.fname) {
        size_t n = strlen(info.fname);
        if ((info.fattrib & (AM_DIR | AM_HID | AM_SYS)) || n < 5 || n >= sizeof(out->name) ||
            strcasecmp(info.fname + n - 4, ".bmp")) continue;
        uint32_t next = (uint32_t)info.fdate << 16 | info.ftime;
        if (!*out->name || next > stamp || (next == stamp && strcmp(info.fname, out->name) > 0)) {
            memcpy(out->name, info.fname, n + 1);
            stamp = next;
        }
    }
    f_closedir(&dir);
    if (r != FR_OK) return ESP_FAIL;
    if (!*out->name) return ESP_ERR_NOT_FOUND;
    char name[20];
    snprintf(name, sizeof(name), "%s/%s", root, out->name);
    FIL file = {0};
    if (f_open(&file, name, FA_READ) != FR_OK) return ESP_FAIL;
    esp_err_t err = decode(&file, out);
    f_close(&file);
    return err;
}

esp_err_t preboot_read_latest(FILE *image, preboot_picture_t *out)
{
    *out = (preboot_picture_t){0};
    if (drive != FF_DRV_NOT_USED || !screenshot_volume_open_file(image, &reader) ||
        ff_diskio_get_drive(&drive) != ESP_OK) return ESP_ERR_INVALID_STATE;
    const ff_diskio_impl_t io = {.init = ready, .status = ready, .read = read_disk, .write = protect, .ioctl = query};
    ff_diskio_register(drive, &io);
    FATFS fs = {0};
    char root[] = {(char)('0' + drive), ':', 0};
    esp_err_t result = f_mount(&fs, root, 1) == FR_OK ? latest(root, out) : ESP_FAIL;
    f_mount(NULL, root, 0);
    ff_diskio_unregister(drive);
    drive = FF_DRV_NOT_USED;
    reader = (screenshot_volume_io_t){0};
    return result;
}
