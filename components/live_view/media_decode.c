// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_decode.h"
#include "cinepak_tile.h"

#include <stdlib.h>
#include <string.h>
#if defined(ESP_PLATFORM) && defined(__XTENSA__)
#include <strings.h>
#endif

#ifdef ESP_PLATFORM
#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"
#define media_alloc(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define media_alloc(n) malloc(n)
#endif

void kvm_media_zeroize(void *buffer, size_t bytes)
{
#if defined(ESP_PLATFORM) && defined(__XTENSA__)
    explicit_bzero(buffer, bytes);
#elif defined(__GNUC__) || defined(__clang__)
    memset(buffer, 0, bytes);
    __asm__ __volatile__("" : : "r"(buffer) : "memory");
#else
    volatile unsigned char *p = buffer;
    while (bytes--)
        *p++ = 0;
#endif
}

static bool running(const kvm_media_target_t *t)
{
    return !t->cancelled || !t->cancelled(t->ctx);
}

static bool fits(const kvm_media_target_t *t, unsigned w, unsigned h)
{
    return t && t->pixels && w && h && w == t->width && h == t->height && t->stride >= w &&
           t->stride <= SIZE_MAX / sizeof(uint16_t) / h && running(t);
}

static bool copy_band(void *ctx, unsigned y, const uint16_t *pixels)
{
    const kvm_media_target_t *t = ctx;
    if (!running(t))
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        memcpy(t->pixels + (y + i) * t->stride, pixels + i * KVM_CVID_W, KVM_CVID_W * sizeof(*pixels));
    }
    return true;
}

static inline int box_sample(const uint8_t *p, int stride, unsigned right)
{
    return (p[0] + p[right] + p[stride] + p[stride + right] + 2) >> 2;
}

static inline unsigned clip_q8(int value)
{
    return value < 0 ? 0 : value > 65535 ? 65535 : (unsigned)value;
}

static inline uint16_t rgb565(int luma, int red, int green, int blue)
{
    return ((clip_q8(luma + red) >> 11) << 11) |
           ((clip_q8(luma + green) >> 10) << 5) | (clip_q8(luma + blue) >> 11);
}

static inline uint16_t gray565(int luma)
{
    unsigned green = clip_q8(luma) >> 10;
    unsigned red_blue = green >> 1;
    return (red_blue << 11) | (green << 5) | red_blue;
}

#if defined(__XTENSA__) && defined(__GNUC__)
__attribute__((always_inline))
#endif
static inline bool render_yuv_blocks(const kvm_media_yuv_t *f, const kvm_media_target_t *t,
                                     bool half, bool complete_blocks)
{
    unsigned scale = half ? 2 : 1;
    int cy = f->full_range ? 256 : 298;
    int rv = f->full_range ? (f->bt709 ? 403 : 359) : (f->bt709 ? 459 : 409);
    int gu = f->full_range ? (f->bt709 ? 48 : 88) : (f->bt709 ? 55 : 100);
    int gv = f->full_range ? (f->bt709 ? 120 : 183) : (f->bt709 ? 136 : 208);
    int bu = f->full_range ? (f->bt709 ? 475 : 454) : (f->bt709 ? 541 : 516);
    int bias = 128 - (f->full_range ? 0 : 16 * cy);
    unsigned cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    // Each 2x2 output block shares one chroma pair, including its Q8 color terms.
    for (unsigned y = 0; y < t->height; y += 2) {
        if (!running(t))
            return false;
        bool second_row = complete_blocks || y + 1 < t->height;
        uint16_t *out0 = t->pixels + y * t->stride, *out1 = second_row ? out0 + t->stride : out0;
        const uint8_t *y0 = f->plane[0] + y * scale * f->stride[0];
        const uint8_t *y1 = second_row ? y0 + scale * f->stride[0] : y0;
        const uint8_t *u_row = f->plane[1] + (y / 2) * scale * f->stride[1];
        const uint8_t *v_row = f->plane[2] + (y / 2) * scale * f->stride[2];
        bool second_chroma_row = complete_blocks || y + 1 < ch;
        int u_stride = second_chroma_row ? f->stride[1] : 0;
        int v_stride = second_chroma_row ? f->stride[2] : 0;
        for (unsigned x = 0; x < t->width; x += 2) {
            unsigned cx = (x / 2) * scale;
            unsigned right = complete_blocks || x + 1 < cw;
            int u = (half ? box_sample(u_row + cx, u_stride, right) : u_row[cx]) - 128;
            int v = (half ? box_sample(v_row + cx, v_stride, right) : v_row[cx]) - 128;
            if (!half && x + 1 < t->width && u == 0 && v == 0) {
                out0[x] = gray565(cy * y0[x] + bias);
                out1[x] = gray565(cy * y1[x] + bias);
                out0[x + 1] = gray565(cy * y0[x + 1] + bias);
                out1[x + 1] = gray565(cy * y1[x + 1] + bias);
                continue;
            }
            int red = bias + rv * v, green = bias - gu * u - gv * v, blue = bias + bu * u;
            unsigned i = x * scale;
            int a = half ? box_sample(y0 + i, f->stride[0], 1) : y0[i];
            int b = half ? box_sample(y1 + i, f->stride[0], 1) : y1[i];
            out0[x] = rgb565(cy * a, red, green, blue);
            out1[x] = rgb565(cy * b, red, green, blue);
            if (complete_blocks || x + 1 < t->width) {
                i += scale;
                a = half ? box_sample(y0 + i, f->stride[0], 1) : y0[i];
                b = half ? box_sample(y1 + i, f->stride[0], 1) : y1[i];
                out0[x + 1] = rgb565(cy * a, red, green, blue);
                out1[x + 1] = rgb565(cy * b, red, green, blue);
            }
        }
    }
    return true;
}

// This loop runs for every decoded H.264 pixel on the 7B.
#if defined(__XTENSA__) && defined(__OPTIMIZE_SIZE__) && !defined(__clang__)
__attribute__((optimize("O2")))
#endif
bool kvm_media_render_yuv(const kvm_media_yuv_t *f, bool half, const kvm_media_target_t *t)
{
    unsigned scale = half ? 2 : 1;
    if (!f || f->width > 1920 || f->height > 1088 || !fits(t, f->width / scale, f->height / scale))
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned w = i ? (f->width + 1) / 2 : f->width;
        if (!f->plane[i] || f->stride[i] < (int)w || f->stride[i] > 16384)
            return false;
    }
    // Four source rows and columns cover a complete 2x2 half-scale output block.
    if (half && f->width % 4 == 0 && f->height % 4 == 0)
        return render_yuv_blocks(f, t, true, true);
    return render_yuv_blocks(f, t, half, false);
}

#ifdef ESP_PLATFORM
typedef struct {
    const uint8_t *next;
    size_t left;
    const kvm_media_target_t *target;
    bool has_nonblack;
} jpeg_input_t;

static UINT jpeg_read(JDEC *dec, BYTE *dst, UINT count)
{
    jpeg_input_t *s = dec->device;
    if (!running(s->target))
        return 0;
    if (s->left < count)
        count = s->left;
    if (dst)
        memcpy(dst, s->next, count);
    s->next += count;
    s->left -= count;
    return count;
}

// Keep the conversion inline in the RGB565 store loop even under the firmware's -Os.
static inline __attribute__((always_inline)) uint16_t jpeg_rgb565(const uint8_t *p)
{
    return ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
}

static UINT jpeg_write(JDEC *dec, void *rgb, JRECT *r)
{
    jpeg_input_t *input = dec->device;
    const kvm_media_target_t *t = input->target;
    if (!running(t) || r->right < r->left || r->bottom < r->top ||
        r->right >= t->width || r->bottom >= t->height)
        return 0;
    const uint8_t *p = rgb;
    const unsigned width = r->right - r->left + 1;
    const unsigned height = r->bottom - r->top + 1;
    const unsigned pixels = width * height;
    const unsigned stride = t->stride;
    uint16_t *base = t->pixels + r->top * stride + r->left;
    // Compare the first four RGB triples, then compare the remaining rows
    // twelve bytes apart so both source addresses retain their word alignment.
    if (pixels >= 4 && memcmp(p, p + 3, 9) == 0 && memcmp(p, p + 12, 3 * (pixels - 4)) == 0) {
        const uint16_t color = jpeg_rgb565(p);
        if (color) input->has_nonblack = true;
        for (unsigned y = 0; y < height; ++y) {
            uint16_t *row = base + y * stride;
            for (unsigned x = 0; x < width; ++x) row[x] = color;
        }
        return 1;
    }
    // Accumulate the converted words as they are written. Once any block has
    // color, the remaining blocks use the ordinary conversion loop below.
    if (!input->has_nonblack) {
        uint16_t colors = 0;
        for (unsigned y = 0; y < height; ++y) {
            uint16_t *row = base + y * stride;
            for (unsigned x = 0; x < width; ++x, p += 3) {
                const uint16_t color = jpeg_rgb565(p);
                colors |= color;
                row[x] = color;
            }
        }
        input->has_nonblack = colors != 0;
        return 1;
    }
    for (unsigned y = 0; y < height; ++y) {
        uint16_t *row = base + y * stride;
        for (unsigned x = 0; x < width; ++x, p += 3)
            row[x] = jpeg_rgb565(p);
    }
    return 1;
}

static bool jpeg_envelope(const uint8_t *data, size_t n)
{
    return n >= 4 && data[0] == 0xff && data[1] == 0xd8 && data[n - 2] == 0xff && data[n - 1] == 0xd9;
}

static bool jpeg_decode(void *work, const uint8_t *data, size_t n, const kvm_media_target_t *t,
                        bool *has_nonblack)
{
    if (!jpeg_envelope(data, n))
        return false;
    JDEC dec;
    jpeg_input_t input = {data, n, t, has_nonblack == NULL};
    bool ok = jd_prepare(&dec, jpeg_read, work, 4096, &input) == JDR_OK && dec.width == t->width &&
              dec.height == t->height && jd_decomp(&dec, jpeg_write, 0) == JDR_OK;
    if (ok && has_nonblack) *has_nonblack = input.has_nonblack;
    return ok;
}

typedef struct {
    jpeg_input_t input; // jpeg_read also receives this structure through JDEC.device.
    unsigned width, height, left, top, x_step, x_remainder;
} jpeg_fit_t;

// Pixels in [ceil(first*dest/src), ceil(end*dest/src)) sample this MCU.
// Advance the inverse map with its quotient and remainder, avoiding a division per pixel.
static UINT jpeg_write_fit(JDEC *dec, void *rgb, JRECT *r)
{
    jpeg_fit_t *fit = dec->device;
    const kvm_media_target_t *t = fit->input.target;
    if (!running(t) || r->left > r->right || r->top > r->bottom ||
        r->right >= dec->width || r->bottom >= dec->height)
        return 0;
    unsigned x0 = (r->left * fit->width + dec->width - 1) / dec->width;
    unsigned x1 = ((r->right + 1u) * fit->width + dec->width - 1) / dec->width;
    unsigned y0 = (r->top * fit->height + dec->height - 1) / dec->height;
    unsigned y1 = ((r->bottom + 1u) * fit->height + dec->height - 1) / dec->height;
    unsigned phase = x0 * dec->width - r->left * fit->width;
    unsigned first_x = phase / fit->width, first_remainder = phase % fit->width;
    unsigned block_width = r->right - r->left + 1u;
    uint16_t colors = 0;
    for (unsigned y = y0; y < y1; ++y) {
        unsigned source_y = y * dec->height / fit->height - r->top;
        const uint8_t *row = (const uint8_t *)rgb + source_y * block_width * 3;
        uint16_t *out = t->pixels + (fit->top + y) * t->stride + fit->left;
        unsigned sx = first_x, remainder = first_remainder;
        for (unsigned x = x0; x < x1; ++x) {
            uint16_t color = jpeg_rgb565(row + sx * 3);
            colors |= color;
            out[x] = color;
            sx += fit->x_step;
            remainder += fit->x_remainder;
            if (remainder >= fit->width) {
                remainder -= fit->width;
                ++sx;
            }
        }
    }
    if (colors) fit->input.has_nonblack = true;
    return 1;
}
#endif

#ifdef KVM_HAVE_FFMPEG
bool kvm_h264_idr(const uint8_t *data, size_t size, const kvm_media_target_t *target);
bool kvm_h264_live_open(void);
void kvm_h264_live_close(void);
#else
kvm_play_result_t kvm_media_h264_inspect(FILE *file, bool mp4, kvm_h264_source_info_t *info,
                                         kvm_media_cancel_t cancel, void *ctx)
{
    (void)file;
    (void)mp4;
    (void)info;
    (void)cancel;
    (void)ctx;
    return KVM_PLAY_UNAVAILABLE;
}
kvm_play_result_t kvm_media_h264_probe(FILE *file, bool mp4, unsigned *width, unsigned *height,
                                       kvm_media_cancel_t cancel, void *ctx)
{
    (void)file;
    (void)mp4;
    (void)width;
    (void)height;
    (void)cancel;
    (void)ctx;
    return KVM_PLAY_UNAVAILABLE;
}
kvm_play_result_t kvm_media_play_h264_stats(FILE *file, bool mp4, const kvm_media_playback_t *sink,
                                            kvm_h264_play_stats_t *stats)
{
    if (stats)
        *stats = (kvm_h264_play_stats_t){0};
    (void)file;
    (void)mp4;
    (void)sink;
    return KVM_PLAY_UNAVAILABLE;
}
#endif

bool kvm_media_h264_size(FILE *file, bool mp4, unsigned *width, unsigned *height, kvm_media_cancel_t cancel,
                         void *ctx)
{
    return kvm_media_h264_probe(file, mp4, width, height, cancel, ctx) == KVM_PLAY_OK;
}

kvm_play_result_t kvm_media_play_h264_result(FILE *file, bool mp4, const kvm_media_playback_t *sink)
{
    return kvm_media_play_h264_stats(file, mp4, sink, NULL);
}

bool kvm_media_play_h264(FILE *file, bool mp4, const kvm_media_playback_t *sink)
{
    return kvm_media_play_h264_result(file, mp4, sink) == KVM_PLAY_OK;
}

bool kvm_media_has_h264(void)
{
#ifdef KVM_HAVE_FFMPEG
    return true;
#else
    return false;
#endif
}

struct kvm_media_decoder {
    kvm_live_codec_t codec;
    size_t workspace_bytes;
    bool monitor_has_nonblack;
    uint32_t workspace[];
};

bool kvm_media_h264_picture(kvm_media_decoder_t *d, kvm_h264_picture_t *slot, const uint8_t *data,
                            size_t size, kvm_media_cancel_t cancelled, void *ctx)
{
#ifdef KVM_HAVE_FFMPEG
    extern bool kvm_h264_retain(kvm_h264_picture_t *, const uint8_t *, size_t, kvm_media_cancel_t, void *);
    if (d && d->codec == KVM_LIVE_H264_INTRA && slot && data && size && size <= KVM_LIVE_MAX_PAYLOAD)
        return kvm_h264_retain(slot, data, size, cancelled, ctx);
#else
    (void)d; (void)slot; (void)data; (void)size; (void)cancelled; (void)ctx;
#endif
    return false;
}

#ifndef KVM_HAVE_FFMPEG
void kvm_media_h264_release(kvm_h264_picture_t *slot)
{
    (void)slot;
}
#endif

// Inline the converter here so the S3 compiler can specialize its 512 x 4 live strip.
#if defined(__XTENSA__) && defined(__OPTIMIZE_SIZE__) && !defined(__clang__)
__attribute__((flatten, optimize("O2")))
#endif
bool kvm_media_h264_band(const kvm_h264_picture_t *slot, unsigned y, uint16_t *rows)
{
    if (!slot || !slot->reference || y % 4 || y >= 512 || !rows)
        return false;
    kvm_media_yuv_t f = slot->yuv;
    if (f.width != 512 || f.height != 512)
        return false;
    f.height = 4;
    for (unsigned i = 0; i < 3; ++i)
        f.plane[i] += (i ? y / 2 : y) * f.stride[i];
    kvm_media_target_t target = {.pixels = rows, .width = 512, .height = 4, .stride = 512};
    return kvm_media_render_yuv(&f, false, &target);
}

kvm_media_decoder_t *kvm_media_decoder_open(kvm_live_codec_t codec)
{
    size_t bytes;
    switch (codec) {
    case KVM_LIVE_CINEPAK_INTRA:
        bytes = sizeof(kvm_cvid_workspace_t);
        break;
#ifdef ESP_PLATFORM
    case KVM_LIVE_JPEG:
        bytes = 4096;
        break;
#endif
#ifdef KVM_HAVE_FFMPEG
    case KVM_LIVE_H264_INTRA:
        bytes = 0;
        break;
#endif
    default:
        return NULL;
    }
    kvm_media_decoder_t *d = media_alloc(sizeof(*d) + bytes);
    if (!d)
        return NULL;
#ifdef KVM_HAVE_FFMPEG
    if (codec == KVM_LIVE_H264_INTRA && !kvm_h264_live_open()) {
        free(d);
        return NULL;
    }
#endif
    d->codec = codec;
    d->workspace_bytes = bytes;
    d->monitor_has_nonblack = false;
    return d;
}

void kvm_media_decoder_close(kvm_media_decoder_t *d)
{
    if (!d)
        return;
#ifdef KVM_HAVE_FFMPEG
    if (d->codec == KVM_LIVE_H264_INTRA)
        kvm_h264_live_close();
#endif
    kvm_media_zeroize(d, sizeof(*d) + d->workspace_bytes);
    free(d);
}

bool kvm_media_usb_monitor_jpeg(kvm_media_decoder_t *d, const uint8_t *data, size_t size,
                                const kvm_media_target_t *t)
{
#ifdef ESP_PLATFORM
    return d && d->codec == KVM_LIVE_JPEG && data && size <= KVM_LIVE_MAX_PAYLOAD &&
           (fits(t, 640, 480) || fits(t, 1024, 600)) &&
           jpeg_decode(d->workspace, data, size, t, &d->monitor_has_nonblack);
#else
    (void)d; (void)data; (void)size; (void)t;
    return false;
#endif
}

bool kvm_media_usb_monitor_jpeg_fit(kvm_media_decoder_t *d, const uint8_t *data, size_t size,
                                    const kvm_media_target_t *t, unsigned source_width, unsigned source_height)
{
#ifdef ESP_PLATFORM
    if (!d || d->codec != KVM_LIVE_JPEG || !data || size > KVM_LIVE_MAX_PAYLOAD ||
        !source_width || source_width > 1920 || !source_height || source_height > 1200 ||
        !fits(t, 1024, 600) || !jpeg_envelope(data, size))
        return false;
    JDEC dec;
    jpeg_fit_t fit = {.input = {data, size, t, false}, .width = t->width, .height = t->height};
    if (jd_prepare(&dec, jpeg_read, d->workspace, 4096, &fit) != JDR_OK ||
        dec.width != source_width || dec.height != source_height)
        return false;
    if (source_width * t->height > source_height * t->width) {
        fit.height = t->width * source_height / source_width;
        if (!fit.height) fit.height = 1;
    } else {
        fit.width = t->height * source_width / source_height;
        if (!fit.width) fit.width = 1;
    }
    fit.left = (t->width - fit.width) / 2;
    fit.top = (t->height - fit.height) / 2;
    fit.x_step = source_width / fit.width;
    fit.x_remainder = source_width % fit.width;
    if (jd_decomp(&dec, jpeg_write_fit, 0) != JDR_OK) return false;
    if (fit.width != t->width || fit.height != t->height) {
        unsigned right = fit.left + fit.width;
        for (unsigned y = 0; y < t->height; ++y) {
            if (!running(t)) return false;
            uint16_t *row = t->pixels + (size_t)y * t->stride;
            if (y < fit.top || y >= fit.top + fit.height)
                memset(row, 0, t->width * sizeof(*row));
            else if (fit.width < t->width) {
                memset(row, 0, fit.left * sizeof(*row));
                memset(row + right, 0, (t->width - right) * sizeof(*row));
            }
        }
    }
    if (!running(t)) return false;
    d->monitor_has_nonblack = fit.input.has_nonblack;
    return true;
#else
    (void)d; (void)data; (void)size; (void)t; (void)source_width; (void)source_height;
    return false;
#endif
}

bool kvm_media_usb_monitor_has_nonblack(const kvm_media_decoder_t *d)
{
    return d && d->monitor_has_nonblack;
}

bool kvm_media_decoder_frame(kvm_media_decoder_t *d, const uint8_t *data, size_t size,
                             const kvm_media_target_t *t)
{
    if (!d || !data || !size || size > KVM_LIVE_MAX_PAYLOAD)
        return false;
    if (d->codec == KVM_LIVE_CINEPAK_INTRA) {
        if (!fits(t, KVM_CVID_W, KVM_CVID_H))
            return false;
        return kvm_cvid_decode((void *)d->workspace, data, size, copy_band, (void *)t);
    }
    if (!fits(t, 512, 512))
        return false;
#ifdef ESP_PLATFORM
    if (d->codec == KVM_LIVE_JPEG)
        return jpeg_decode(d->workspace, data, size, t, NULL);
#endif
#ifdef KVM_HAVE_FFMPEG
    if (d->codec == KVM_LIVE_H264_INTRA)
        return kvm_h264_idr(data, size, t);
#endif
    return false;
}

bool kvm_media_decode_live(kvm_live_codec_t codec, const uint8_t *data, size_t size,
                           const kvm_media_target_t *t)
{
    kvm_media_decoder_t *d = kvm_media_decoder_open(codec);
    bool ok = kvm_media_decoder_frame(d, data, size, t);
    kvm_media_decoder_close(d);
    return ok;
}
