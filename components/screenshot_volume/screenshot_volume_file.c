// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "screenshot_volume.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

static bool regular_size(FILE *file, long expected)
{
    if (!file) return false;
#ifdef _WIN32
    struct _stat64 st;
    return _fstat64(_fileno(file), &st) == 0 && (st.st_mode & _S_IFMT) == _S_IFREG && st.st_size == expected;
#else
    struct stat st;
    return fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == expected;
#endif
}

static bool read_sectors(void *context, uint32_t lba, uint32_t sectors, void *data)
{
    FILE *file = context;
    return !fseek(file, (long)(lba * SCREENSHOT_SECTOR_BYTES), SEEK_SET) &&
           fread(data, SCREENSHOT_SECTOR_BYTES, sectors, file) == sectors;
}

static bool write_sectors(void *context, uint32_t lba, uint32_t sectors, const void *data)
{
    FILE *file = context;
    return !fseek(file, (long)(lba * SCREENSHOT_SECTOR_BYTES), SEEK_SET) &&
           fwrite(data, SCREENSHOT_SECTOR_BYTES, sectors, file) == sectors;
}

static bool sync_file(void *context)
{
    FILE *file = context;
    if (fflush(file)) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

static screenshot_volume_io_t file_io(FILE *file)
{
    return (screenshot_volume_io_t){ .context = file, .read = read_sectors,
                                     .write = write_sectors, .sync = sync_file };
}

bool screenshot_volume_create_file(FILE *file, uint32_t serial, void *scratch, size_t scratch_bytes)
{
    if (!regular_size(file, 0)) return false;
    screenshot_volume_io_t io = file_io(file);
    return screenshot_volume_format(&io, serial, scratch, scratch_bytes) &&
           regular_size(file, SCREENSHOT_VOLUME_BYTES);
}

bool screenshot_volume_open_file(FILE *file, screenshot_volume_io_t *out)
{
    if (!out) return false;
    *out = (screenshot_volume_io_t){0};
    if (!regular_size(file, SCREENSHOT_VOLUME_BYTES)) return false;
    *out = file_io(file);
    return true;
}
