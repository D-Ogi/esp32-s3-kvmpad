// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_data: the read-only dictionary image of the `ime_data` partition (ADR-024 TECH-081).
//
// The image is written by tools/ime/build_ime_data.py and flashed once with the rest of the firmware tooling; it
// is never downloaded at run time. Layout (little endian):
//
//   offset 0     header   "KVMIMEDT", u32 version (1), u32 count (1..IME_DATA_MAX_ENTRIES), u32 reserved[2] = 0
//   offset 24    count x entry: char name[24] (NUL-terminated, [a-z0-9_.-]), u32 offset, u32 size, u8 sha256[32]
//   offset 4096  the files, each at a 4096-aligned offset, inside the partition, never overlapping
//
// ime_data_parse() checks the directory only; the SHA-256 of every file is checked by the mount (ime_data_mount())
// before the file can be opened, so a torn or stale image is refused instead of being decoded.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IME_DATA_MAGIC "KVMIMEDT"
#define IME_DATA_VERSION 1u
#define IME_DATA_MAX_ENTRIES 8u
#define IME_DATA_NAME_CAP 24u
#define IME_DATA_ALIGN 4096u
#define IME_DATA_HEADER_LEN 24u
#define IME_DATA_ENTRY_LEN 64u
/** Bytes a parser needs to see: the header and the largest directory. */
#define IME_DATA_DIR_LEN (IME_DATA_HEADER_LEN + IME_DATA_MAX_ENTRIES * IME_DATA_ENTRY_LEN)

typedef struct {
    char name[IME_DATA_NAME_CAP];
    uint32_t offset;
    uint32_t size;
    uint8_t sha256[32];
} ime_data_entry_t;

typedef enum {
    IME_DATA_OK = 0,
    IME_DATA_ERR_ARG,     // NULL or a buffer shorter than IME_DATA_DIR_LEN
    IME_DATA_ERR_MAGIC,   // erased partition or not an image
    IME_DATA_ERR_VERSION,
    IME_DATA_ERR_COUNT,   // 0 or more than IME_DATA_MAX_ENTRIES
    IME_DATA_ERR_NAME,    // empty, not terminated, a character outside [a-z0-9_.-], or a duplicate
    IME_DATA_ERR_BOUNDS,  // unaligned, inside the directory page, past the partition end or overlapping
} ime_data_status_t;

/**
 * Parse the directory at the start of an image. `dir` holds the first `dir_len` bytes of the partition
 * (at least IME_DATA_DIR_LEN), `part_size` is the partition size. On IME_DATA_OK `out[0..*count)` is filled.
 * Pure function: no I/O, no allocation; compiled by the host tests.
 */
ime_data_status_t ime_data_parse(const uint8_t *dir, size_t dir_len, uint32_t part_size, ime_data_entry_t *out,
                                 uint32_t cap, uint32_t *count);

const char *ime_data_status_name(ime_data_status_t s);

#ifdef __cplusplus
}
#endif
