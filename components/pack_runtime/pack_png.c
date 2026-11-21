// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_png.h"
#include "pack_archive.h"
#include "pack_port.h"
#include <png.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

typedef struct {
    const uint8_t *next;
    size_t remaining, available, lowest;
    uint8_t *row;
    png_infop info;
    bool out_of_memory;
} reader_t;

typedef union {
    max_align_t align;
    size_t bytes;
} allocation_t;

static png_voidp allocate(png_structp png, png_alloc_size_t n)
{
    reader_t *r = png_get_mem_ptr(png);
    if (!r || r->available < sizeof(allocation_t) || n > r->available - sizeof(allocation_t))
        return NULL;
    allocation_t *a = pack_alloc(n + sizeof(*a));
    if (!a) {
        r->out_of_memory = true;
        return NULL;
    }
    a->bytes = n + sizeof(*a);
    r->available -= a->bytes;
    if (r->available < r->lowest) r->lowest = r->available;
    return a + 1;
}

static void release(png_structp png, png_voidp ptr)
{
    if (!ptr) return;
    reader_t *r = png_get_mem_ptr(png);
    allocation_t *a = (allocation_t *)ptr - 1;
    r->available += a->bytes;
    pack_free(a);
}

static void read_bytes(png_structp png, png_bytep dst, size_t n)
{
    reader_t *r = png_get_io_ptr(png);
    if (n > r->remaining) png_error(png, "truncated PNG");
    memcpy(dst, r->next, n);
    r->next += n;
    r->remaining -= n;
}

static void invalid(png_structp png, png_const_charp reason)
{
    (void)reason;
    png_longjmp(png, 1);
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool chunks(const uint8_t *p, size_t size)
{
    if (size < 45 || size > PACK_EXPANDED_MAX || memcmp(p, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16))
        return false;
    bool animation_header = false, frame = false, image_data = false;
    for (size_t pos = 8; size - pos >= 12;) {
        uint32_t n = be32(p+pos);
        const uint8_t *type = p + pos + 4;
        if (n > size-pos-12 || pack_crc(type, n+4) != be32(type+4+n))
            return false;
        for (unsigned k = 0; k < 4; ++k)
            if ((type[k] | 32u) < 'a' || (type[k] | 32u) > 'z') return false;
        if (type[2] & 32u) return false;
        const uint8_t *data = type + 4;
        if (!memcmp(type, "acTL", 4)) {
            if (animation_header || image_data || n != 8 || be32(data) != 1) return false;
            animation_header = true;
        } else if (!memcmp(type, "fcTL", 4)) {
            // A one-frame APNG is static only if its default IDAT image is that frame.
            if (!animation_header || image_data || frame || n != 26 || be32(data) ||
                memcmp(data + 4, p + 16, 8) || be32(data + 12) || be32(data + 16) ||
                data[24] > 2 || data[25] > 1) return false;
            frame = true;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (animation_header && !frame) return false;
            image_data = true;
        } else if (!memcmp(type, "fdAT", 4)) {
            return false;
        }
        if (!(type[0] & 32) && memcmp(type,"IHDR",4) && memcmp(type,"PLTE",4) &&
            memcmp(type,"IDAT",4) && memcmp(type,"IEND",4))
            return false;
        pos += (size_t)n + 12;
        if (!memcmp(type,"IEND",4)) return !n && pos == size;
    }
    return false;
}

static bool decode(reader_t *r, pack_png_span_t emit, void *ctx)
{
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, invalid, invalid, r, allocate, release);
    if (!png) return false;
    bool ok = false;
    if (setjmp(png_jmpbuf(png)) == 0) {
        r->info = png_create_info_struct(png);
        if (!r->info) png_error(png, "PNG allocation");
        png_set_read_fn(png, r, read_bytes);
        png_set_user_limits(png, 1024, 568);
        // Metadata is not used for rendering. Skip its decompression and retention.
        png_set_keep_unknown_chunks(png, PNG_HANDLE_CHUNK_NEVER, NULL, -1);
        png_read_info(png, r->info);
        unsigned w = png_get_image_width(png, r->info), h = png_get_image_height(png, r->info);
        int color = png_get_color_type(png, r->info), depth = png_get_bit_depth(png, r->info);
        bool inter = png_get_interlace_type(png, r->info) == PNG_INTERLACE_ADAM7;
        if (depth == 16) png_set_strip_16(png);
        if (color == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
        if (color == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
        if (png_get_valid(png, r->info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
        else if (!(color & PNG_COLOR_MASK_ALPHA)) png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
        if (!(color & PNG_COLOR_MASK_COLOR)) png_set_gray_to_rgb(png);
        png_read_update_info(png, r->info);
        if (png_get_rowbytes(png, r->info) != 4u*w) png_error(png, "PNG pixel format");
        r->row = png_malloc(png, 4u*w);
        for (int pass = 0; pass < (inter ? 7 : 1); ++pass) {
            unsigned cols = inter ? PNG_PASS_COLS(w,pass) : w;
            unsigned rows = inter ? PNG_PASS_ROWS(h,pass) : h;
            for (unsigned y = 0; cols && y < rows; ++y) {
                png_read_row(png, r->row, NULL);
                if (emit) emit(ctx, inter ? PNG_PASS_START_COL(pass) : 0,
                               inter ? PNG_ROW_FROM_PASS_ROW(y,pass) : y,
                               inter ? PNG_PASS_COL_OFFSET(pass) : 1, r->row, cols);
#ifdef ESP_PLATFORM
                if ((y & 63u) == 63u) vTaskDelay(1);
#endif
            }
        }
        png_read_end(png, NULL);
        ok = r->remaining == 0;
    }
    png_free(png, r->row);
    png_destroy_read_struct(&png, &r->info, NULL);
    return ok;
}

esp_err_t pack_png_decode(const uint8_t *data, size_t size, size_t budget, pack_png_span_t emit, void *ctx)
{
    return pack_png_decode_measured(data, size, budget, emit, ctx, NULL);
}

esp_err_t pack_png_decode_measured(const uint8_t *data, size_t size, size_t budget, pack_png_span_t emit, void *ctx, size_t *peak)
{
    if (!data || budget > 512u * 1024u || !chunks(data, size)) return ESP_ERR_NOT_SUPPORTED;
    reader_t reader = {.next = data, .remaining = size, .available = budget, .lowest = budget};
    if (decode(&reader, emit, ctx)) {
        if (peak && *peak < budget - reader.lowest) *peak = budget - reader.lowest;
        return ESP_OK;
    }
    return reader.out_of_memory ? ESP_ERR_NO_MEM : ESP_ERR_NOT_SUPPORTED;
}
