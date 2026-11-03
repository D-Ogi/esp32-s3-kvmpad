// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ui_font_blob: an LVGL bitmap font read in place from a KVMFONT1 blob (ADR-024 FR-746, TECH-081).
//
// The hanzi font of the pinyin candidate bar is ~4.6 MB, too large for the application image, so it ships in the
// read-only ime_data partition (tools/fonts/gen_hanzi_font_blob.py builds it, tools/ime/build_ime_data.py packs it).
// The loader builds a chain of lv_font_t (one per part, each part at most 1 MiB of bitmap because the LVGL glyph
// descriptor has a 20-bit bitmap index). Two ways to give it the blob:
// - in place (ui_font_blob_load): the blob is in memory; the glyph tables and bitmaps point into it, no copy, no
//   allocation. The host tests and the simulator use this.
// - streamed (ui_font_blob_open, ADR-034): the blob is read through a callback. The glyph tables and cmap lists are
//   copied once into memory from the source's allocator; each glyph bitmap is read on demand, when LVGL draws it,
//   through LVGL's own glyph loader (lv_font_fmt_txt_dsc_t.are_glyphs_dynamic_loaded). The firmware uses this, so no
//   flash region stays memory-mapped: a permanent mapping makes every flash write (OTA, NVS) disable the cache, and
//   the RGB panel's IRAM interrupt then reads the PSRAM frame buffer with the cache off (found on board #1).
// The descriptors live in the caller's ui_font_blob_t.
//
// Every offset, count, list and glyph is checked before LVGL sees it, so a damaged blob is refused, never read out
// of bounds. A glyph whose bitmap stream is longer than UI_FONT_BLOB_GLYPH_MAX is refused in both modes.
//
// Format (little endian; every table 4-byte aligned):
//   header (32 B): "KVMFONT1", u32 version (1 plain, 2 compressed), u32 part count (1..UI_FONT_BLOB_PART_CAP), u32 bpp (4),
//                  u32 total size (== blob length), 8 reserved bytes (0)
//   part table: part count x 48 B: i32 line_height, i32 base_line, i32 underline_position, i32 underline_thickness,
//                  u32 cmap offset, u32 cmap count, u32 glyph descriptor offset, u32 glyph count (id 0 reserved),
//                  u32 bitmap offset, u32 bitmap length, 8 reserved bytes (0)
//   cmap record (16 B): u32 range_start, u16 range_length, u16 glyph_id_start, u32 list offset (0 = none),
//                  u16 list length, u8 type (lv_font_fmt_txt_cmap_type_t: FORMAT0_FULL with a uint8 glyph id offset
//                  list, FORMAT0_TINY without a list, SPARSE_TINY with a uint16 code point offset list), u8 0
//   glyph descriptor (8 B): u32 bitmap_index | adv_w << 20, u8 box_w, u8 box_h, i8 ofs_x, i8 ofs_y - the in-memory
//                  layout of lv_font_fmt_txt_glyph_dsc_t (checked at run time)
//   bitmap: version 1 plain 4 bpp glyph bitmaps; version 2 LVGL's RLE stream with the line XOR prefilter
//                  (LV_FONT_FMT_TXT_COMPRESSED, about two thirds of the plain size). A version 2 blob needs
//                  LV_USE_FONT_COMPRESSED; the loader then runs a bounded copy of LVGL's decoder over every glyph, so
//                  no glyph stream can make LVGL read past its part (a walk over every pixel of every glyph: load it
//                  off the UI task)
// LVGL owner context only (the fonts are read while drawing). Loading calls no LVGL function except, in streamed
// mode, lv_mutex_init() for each part's glyph loader (an OS mutex, safe from any task).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FONT_BLOB_PART_CAP 8u
#define UI_FONT_BLOB_CMAP_CAP 256u
#define UI_FONT_BLOB_HEADER_LEN 32u
#define UI_FONT_BLOB_PART_LEN 48u
#define UI_FONT_BLOB_CMAP_LEN 16u
/** The longest glyph bitmap stream accepted (a 24 px 4 bpp glyph is at most 288 B plain). */
#define UI_FONT_BLOB_GLYPH_MAX 1024u

typedef enum {
    UI_FONT_BLOB_OK = 0,
    UI_FONT_BLOB_ERR_ARG,     // NULL, misaligned or a blob shorter than the header
    UI_FONT_BLOB_ERR_MAGIC,
    UI_FONT_BLOB_ERR_VERSION, // version, bpp, reserved bytes or the total size
    UI_FONT_BLOB_ERR_PART,    // part count, metrics or a table outside the blob
    UI_FONT_BLOB_ERR_CMAP,    // cmap type, range, list or glyph id
    UI_FONT_BLOB_ERR_GLYPH,   // a glyph's bitmap outside its part
    UI_FONT_BLOB_ERR_ABI,     // lv_font_fmt_txt_glyph_dsc_t is not the 8-byte layout of the blob
    UI_FONT_BLOB_ERR_IO,      // streamed: the source's read failed
    UI_FONT_BLOB_ERR_NOMEM,   // streamed: the source's allocator failed
} ui_font_blob_status_t;

/** A streamed blob (ui_font_blob_open). */
typedef struct {
    /**
     * Copy `n` bytes at offset `off` of the blob to `dst`; false on any error. [off, off + n) always lies inside
     * [0, len). Called by the loading task, then by the LVGL owner while it draws (one glyph per call, serialized by
     * the part's loader mutex), so it must be safe from both.
     */
    bool (*read)(void *ctx, uint32_t off, void *dst, uint32_t n);
    void *(*alloc)(size_t n); // the tables and loaders, one block; NULL on failure
    void (*free)(void *p);
    void *ctx;
    uint32_t len; // blob length
} ui_font_blob_src_t;

typedef struct {
    lv_font_t font[UI_FONT_BLOB_PART_CAP];
    lv_font_fmt_txt_dsc_t dsc[UI_FONT_BLOB_PART_CAP];
    lv_font_fmt_txt_cmap_t cmap[UI_FONT_BLOB_CMAP_CAP];
    uint32_t parts;  // 0 while nothing is loaded
    uint32_t glyphs; // glyphs over every part, the reserved id 0 of each part not counted
    ui_font_blob_src_t src; // streamed: a copy of the source (the loaders point here); zero in place
    void *arena;            // streamed: the block from src.alloc; NULL in place
    size_t arena_len;
    uint32_t glyph_reads;   // streamed: glyph bitmaps read since the open (diagnostics; LVGL owner)
} ui_font_blob_t;

/**
 * Validate `blob[0..len)` and build its font chain in `f`; the last part falls back to `fallback` (may be NULL).
 * The blob must stay mapped and unchanged while the fonts are in use. On any error `f` holds no font.
 */
ui_font_blob_status_t ui_font_blob_load(ui_font_blob_t *f, const void *blob, size_t len, const lv_font_t *fallback);

/** How many glyphs ui_font_blob_load_ex() checks between two calls of its `yield`. */
#define UI_FONT_BLOB_YIELD_GLYPHS 256u

/**
 * ui_font_blob_load() that calls `yield(ctx)` (if not NULL) after every UI_FONT_BLOB_YIELD_GLYPHS glyphs it checks.
 * Checking a compressed blob walks every pixel - seconds on the ESP32-S3 - so a caller in a low-priority task passes
 * a yield that blocks for a tick: the idle task of its core must run, or the task watchdog resets the chip.
 */
ui_font_blob_status_t ui_font_blob_load_ex(ui_font_blob_t *f, const void *blob, size_t len, const lv_font_t *fallback,
                                           void (*yield)(void *ctx), void *ctx);

/**
 * Streamed load (ADR-034): validate the blob read through `src` exactly as ui_font_blob_load_ex() does, copy the
 * glyph tables and cmap lists into one block from `src->alloc`, and give every part a glyph loader that reads one
 * glyph bitmap through `src->read` when LVGL draws it. `f` must not move while the fonts are in use (the loaders
 * point into it). On any error `f` holds no font and nothing stays allocated.
 */
ui_font_blob_status_t ui_font_blob_open(ui_font_blob_t *f, const ui_font_blob_src_t *src, const lv_font_t *fallback,
                                        void (*yield)(void *ctx), void *ctx);

/**
 * Release what ui_font_blob_open() allocated and clear `f` (an in-place font is only cleared). LVGL owner context,
 * after no widget uses the fonts any more.
 */
void ui_font_blob_close(ui_font_blob_t *f);

/** The head of the chain, or NULL when nothing is loaded. */
const lv_font_t *ui_font_blob_font(const ui_font_blob_t *f);

const char *ui_font_blob_status_name(ui_font_blob_status_t s);

#ifdef __cplusplus
}
#endif
