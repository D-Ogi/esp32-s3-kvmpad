// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_data_vfs (firmware only): mounts the `ime_data` partition read-only at IME_DATA_MOUNT so the vendored
// engines can fopen() their dictionaries unchanged (TECH-081). Every file's SHA-256 is checked once, at mount; a
// file that fails the check cannot be opened. Writes are refused (EROFS); there is no runtime download path.
// Mounting is explicit and off the boot path: the low-priority ime_glue worker after the UI is up, or the `ime`
// console command.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ime_data.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IME_DATA_MOUNT "/ime"

typedef struct {
    bool partition_found;
    uint32_t partition_size;
    ime_data_status_t parse;
    bool mounted;
    uint32_t count;
    ime_data_entry_t entry[IME_DATA_MAX_ENTRIES];
    bool verified[IME_DATA_MAX_ENTRIES];
    uint32_t verify_ms; // time spent hashing at mount
    uint32_t opens, open_refused, reads;
} ime_data_vfs_status_t;

/** Find, parse, verify and register. Idempotent: a second call returns the first result. */
esp_err_t ime_data_mount(void);
void ime_data_get_status(ime_data_vfs_status_t *out);

// ADR-034: nothing of ime_data is memory-mapped. With the application executing from PSRAM, ESP-IDF keeps the cache
// enabled during a flash write only while no flash region is mapped (spi_flash_os_func_app.c, spi1_start()); a
// permanent mapping makes every OTA and NVS write disable the cache, and the RGB panel's IRAM interrupt then faults on
// the PSRAM frame buffer. A file is read through ime_data_file_read() (a flash read keeps the cache on) or copied into
// PSRAM with ime_data_load().

/** A verified file of the mounted image (from ime_data_file()). */
typedef struct {
    uint32_t offset; // in the partition
    uint32_t size;
} ime_data_file_t;

/**
 * Look up a verified file. ESP_ERR_INVALID_STATE before a successful mount, ESP_ERR_NOT_FOUND for a missing, empty
 * or unverified file (an unverified file is never handed out).
 */
esp_err_t ime_data_file(const char *name, ime_data_file_t *out);

/**
 * Read [off, off + n) of `file`; false when the range is not inside the file or the flash read fails. Safe from any
 * task (the flash driver serializes); used by the LVGL owner to read one glyph bitmap per call.
 */
bool ime_data_file_read(const ime_data_file_t *file, uint32_t off, void *dst, uint32_t n);

/**
 * Copy a verified file into a new PSRAM buffer (the caller owns it; heap_caps_free()). The engines that read their
 * data in place (OpenWnn) use this. ESP_ERR_NO_MEM when PSRAM cannot hold it.
 */
esp_err_t ime_data_load(const char *name, void **data, size_t *len);

#ifdef __cplusplus
}
#endif
