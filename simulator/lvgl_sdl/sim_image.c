// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// See sim_image.h.
#include "sim_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/libs/lodepng/lodepng.h"

bool sim_image_render(lv_obj_t *obj, sim_image_t *out)
{
    if (!obj || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    lv_obj_update_layout(obj);
    lv_refr_now(NULL);
    lv_draw_buf_t *buf = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565);
    if (!buf) {
        printf("    render: lv_snapshot_take failed\n");
        return false;
    }
    const uint32_t w = buf->header.w;
    const uint32_t h = buf->header.h;
    uint8_t *rgb = malloc((size_t)w * h * 3u);
    if (!rgb) {
        lv_draw_buf_destroy(buf);
        printf("    render: out of memory for %ux%u RGB888\n", w, h);
        return false;
    }
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *src = buf->data + (size_t)row * buf->header.stride;
        uint8_t *dst = rgb + (size_t)row * w * 3u;
        for (uint32_t col = 0; col < w; col++) {
            const uint16_t px = (uint16_t)(src[col * 2] | (uint16_t)(src[col * 2 + 1] << 8));
            dst[col * 3 + 0] = (uint8_t)(((px >> 11) & 0x1Fu) * 255u / 31u);
            dst[col * 3 + 1] = (uint8_t)(((px >> 5) & 0x3Fu) * 255u / 63u);
            dst[col * 3 + 2] = (uint8_t)((px & 0x1Fu) * 255u / 31u);
        }
    }
    lv_draw_buf_destroy(buf);
    out->w = w;
    out->h = h;
    out->rgb = rgb;
    return true;
}

// lodepng is the copy LVGL vendors: it allocates through lv_malloc and its *_file helpers go through lv_fs, so
// the memory entry points are used here and the file bytes are moved with plain stdio. Everything lodepng
// returns is released with lv_free and the pixels are copied into a malloc'ed buffer, so one sim_image_t always
// has the same ownership whether it was rendered or decoded.
static uint8_t *read_whole_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    *size = got;
    return buf;
}

bool sim_image_read_png(const char *path, sim_image_t *out)
{
    if (!path || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    size_t size = 0;
    uint8_t *file = read_whole_file(path, &size);
    if (!file) {
        printf("    png read %s: cannot read the file\n", path);
        return false;
    }
    // LVGL's lodepng copy hands the decoder's result back as an lv_draw_buf_t (32 bpp, own stride), not as a
    // plain pixel array, and it must be released with lv_draw_buf_destroy - this is the same call
    // lv_test_screenshot_compare.c makes.
    lv_draw_buf_t *decoded = NULL;
    unsigned w = 0, h = 0;
    const unsigned err = lodepng_decode32((unsigned char **)&decoded, &w, &h, file, size);
    free(file);
    if (err || !decoded) {
        printf("    png read %s: lodepng error %u\n", path, err);
        if (decoded) {
            lv_draw_buf_destroy(decoded);
        }
        return false;
    }
    uint8_t *rgb = malloc((size_t)w * h * 3u);
    if (!rgb) {
        lv_draw_buf_destroy(decoded);
        printf("    png read %s: out of memory\n", path);
        return false;
    }
    for (uint32_t row = 0; row < h; row++) {
        const uint8_t *src = decoded->data + (size_t)row * decoded->header.stride;
        uint8_t *dst = rgb + (size_t)row * w * 3u;
        for (uint32_t col = 0; col < w; col++) {
            dst[col * 3 + 0] = src[col * 4 + 0];
            dst[col * 3 + 1] = src[col * 4 + 1];
            dst[col * 3 + 2] = src[col * 4 + 2];
        }
    }
    lv_draw_buf_destroy(decoded);
    out->w = w;
    out->h = h;
    out->rgb = rgb;
    return true;
}

bool sim_image_write_png(const char *path, const sim_image_t *img)
{
    if (!path || !img || !img->rgb) {
        return false;
    }
    unsigned char *png = NULL;
    size_t size = 0;
    const unsigned err = lodepng_encode24(&png, &size, img->rgb, img->w, img->h);
    if (err) {
        printf("    png write %s: lodepng error %u\n", path, err);
        lv_free(png);
        return false;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        printf("    png write %s: cannot create the file\n", path);
        lv_free(png);
        return false;
    }
    const size_t wrote = fwrite(png, 1, size, f);
    fclose(f);
    lv_free(png);
    if (wrote != size) {
        printf("    png write %s: short write (%zu of %zu bytes)\n", path, wrote, size);
        return false;
    }
    return true;
}

void sim_image_free(sim_image_t *img)
{
    if (img) {
        free(img->rgb);
        img->rgb = NULL;
        img->w = img->h = 0;
    }
}

bool sim_image_compare(const sim_image_t *a, const sim_image_t *b, uint8_t tolerance, uint32_t budget,
                       sim_diff_t *out)
{
    sim_diff_t d;
    memset(&d, 0, sizeof(d));
    d.first_x = -1;
    d.first_y = -1;
    if (!a || !b || !a->rgb || !b->rgb) {
        if (out) {
            *out = d;
        }
        return false;
    }
    if (a->w != b->w || a->h != b->h) {
        printf("    compare: geometry %ux%u vs %ux%u\n", a->w, a->h, b->w, b->h);
        if (out) {
            *out = d;
        }
        return false;
    }
    d.pixels_compared = a->w * a->h;
    for (uint32_t y = 0; y < a->h; y++) {
        for (uint32_t x = 0; x < a->w; x++) {
            const size_t i = ((size_t)y * a->w + x) * 3u;
            uint32_t worst = 0;
            for (int ch = 0; ch < 3; ch++) {
                const int delta = (int)a->rgb[i + (size_t)ch] - (int)b->rgb[i + (size_t)ch];
                const uint32_t mag = (uint32_t)(delta < 0 ? -delta : delta);
                if (mag > worst) {
                    worst = mag;
                }
            }
            if (worst > d.max_channel_delta) {
                d.max_channel_delta = worst;
            }
            if (worst > tolerance) {
                d.pixels_differing++;
                if (d.first_x < 0) {
                    d.first_x = (int32_t)x;
                    d.first_y = (int32_t)y;
                }
            }
        }
    }
    if (out) {
        *out = d;
    }
    return d.pixels_differing <= budget;
}
