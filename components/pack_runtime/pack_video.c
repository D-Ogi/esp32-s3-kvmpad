// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_video.h"
#include "pack_port.h"
#include <string.h>
#include <zlib.h>
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#include "esp_timer.h"
#endif
#ifdef ESP_PLATFORM
#include "psa/crypto.h"
#else
#include <openssl/evp.h>
#endif

enum { MAX_WIDTH = 960, MAX_HEIGHT = 540, BAND_ROWS = 8, BAND_BYTES = MAX_WIDTH * BAND_ROWS * 2 };
enum { SECTOR_BYTES = 512, WINDOW_BYTES = sizeof(pack_video_window_t) };
_Static_assert(WINDOW_BYTES >= BAND_BYTES + SECTOR_BYTES, "The window must fit a band and a sector.");
struct pack_video {
    uint8_t *window;
    uint16_t *delta;
    size_t begin, end;
    FILE *file;
    pack_video_info_t info;
    unsigned frame;
    uint64_t previous_pts;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    pack_video_io_t io;
#endif
};

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
void pack_video_get_io(const pack_video_t *video, pack_video_io_t *out) { *out = video->io; }
#endif

static unsigned u16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }

static bool source_matches(FILE *source, const uint8_t *header, uint8_t *buf, bool (*stopped)(void))
{
    bool ok = false;
    if (fseek(source, 0, SEEK_END) != 0)
        return false;
    long length = ftell(source);
    if (fseek(source, 0, SEEK_SET) || length < 1 || (uint64_t)length != u64(header + 16))
        return false;
    uint8_t hash[32];
#ifdef ESP_PLATFORM
    psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
    ok = psa_crypto_init() == PSA_SUCCESS && psa_hash_setup(&sha, PSA_ALG_SHA_256) == PSA_SUCCESS;
#else
    EVP_MD_CTX *sha = EVP_MD_CTX_new();
    ok = sha && EVP_DigestInit_ex(sha, EVP_sha256(), NULL) == 1;
#endif
    while (ok && length > 0 && !(stopped && stopped())) {
        size_t n = length > 8192 ? 8192 : (size_t)length;
        ok = fread(buf, 1, n, source) == n;
#ifdef ESP_PLATFORM
        ok = ok && psa_hash_update(&sha, buf, n) == PSA_SUCCESS;
#else
        ok = ok && EVP_DigestUpdate(sha, buf, n) == 1;
#endif
        length -= (long)n;
    }
#ifdef ESP_PLATFORM
    size_t used = 0;
    ok = ok && !length && psa_hash_finish(&sha, hash, sizeof(hash), &used) == PSA_SUCCESS && used == sizeof(hash);
    psa_hash_abort(&sha);
#else
    unsigned used = 0;
    ok = ok && !length && EVP_DigestFinal_ex(sha, hash, &used) == 1 && used == sizeof(hash);
    EVP_MD_CTX_free(sha);
#endif
    return fseek(source, 0, SEEK_SET) == 0 && ok && !memcmp(hash, header + 24, sizeof(hash));
}

esp_err_t pack_video_open(FILE *source, FILE *file, unsigned mode, bool (*stopped)(void),
                          pack_video_t **out, pack_video_info_t *info)
{
    return pack_video_open_with_window(source, file, mode, stopped, NULL, out, info);
}

esp_err_t pack_video_open_with_window(FILE *source, FILE *file, unsigned mode, bool (*stopped)(void),
                                      pack_video_window_t *window, pack_video_t **out, pack_video_info_t *info)
{
    *out = NULL;
    uint8_t header[64];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "KVC1", 4) ||
        pack_crc(header, 60) != u32(header + 60) || u32(header + 8) != mode || u32(header + 56) != BAND_ROWS)
        return ESP_ERR_INVALID_RESPONSE;
    pack_video_info_t dimensions = {u16(header + 4), u16(header + 6), u32(header + 12)};
    if (!dimensions.width || dimensions.width > MAX_WIDTH || !dimensions.height || dimensions.height > MAX_HEIGHT ||
        !dimensions.frames || dimensions.frames > 2073600)
        return ESP_ERR_INVALID_SIZE;
    pack_video_t *video = pack_alloc(sizeof(*video) + (window ? 0 : WINDOW_BYTES));
    if (!video)
        return ESP_ERR_NO_MEM;
    video->window = window ? window->bytes : (uint8_t *)(video + 1);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t bind_start = esp_timer_get_time();
#endif
    if (!source_matches(source, header, video->window, stopped)) {
        pack_free(video);
        return ESP_ERR_INVALID_RESPONSE;
    }
    video->file = file;
    video->delta = NULL;
    video->begin = sizeof(header);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t prefill_start = esp_timer_get_time();
    video->io = (pack_video_io_t){.bind_us = prefill_start - bind_start, .bind_bytes = u64(header + 16)};
#endif
    video->end = video->begin + fread(video->window + video->begin, 1, WINDOW_BYTES - video->begin, file);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    video->io.prefill_us = esp_timer_get_time() - prefill_start;
    video->io.prefill_bytes = video->end - video->begin;
#endif
    video->info = dimensions;
    video->frame = 0;
    video->previous_pts = 0;
    *out = video;
    *info = dimensions;
    return ESP_OK;
}

static bool reserve_delta_band(pack_video_t *video)
{
    if (!video->delta) {
        unsigned rows = video->info.height < BAND_ROWS ? video->info.height : BAND_ROWS;
        video->delta = pack_alloc((size_t)video->info.width * rows * sizeof(*video->delta));
    }
    return video->delta != NULL;
}

void pack_video_close(pack_video_t *video)
{
    if (video) {
        pack_free(video->delta);
        pack_free(video);
    }
}

// A returned span stays valid until the next read. Keep full-sector reads contiguous on the card.
static const uint8_t *take_bytes(pack_video_t *v, size_t size)
{
    size_t available = v->end - v->begin;
    if (available < size) {
        size_t room = WINDOW_BYTES - available;
        size_t start = room % SECTOR_BYTES;
        memmove(v->window + start, v->window + v->begin, available);
        v->begin = start;
        v->end = start + available;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        int64_t begin = esp_timer_get_time();
#endif
        size_t count = fread(v->window + v->end, 1, room - start, v->file);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        v->io.refill_us += esp_timer_get_time() - begin;
        v->io.refill_bytes += count;
        ++v->io.refill_calls;
#endif
        v->end += count;
        if (available + count < size)
            return NULL;
    }
    const uint8_t *span = v->window + v->begin;
    v->begin += size;
    return span;
}

// Derive the other three byte positions of a four-byte CRC from zlib's
// existing polynomial table. The tables live on the card worker's stack.
typedef struct {
    const z_crc_t *slice[4];
} picture_crc_t;

static void picture_crc_init(picture_crc_t *t, z_crc_t derived[3][256])
{
    t->slice[0] = get_crc_table();
    for (unsigned k = 0; k < 3; ++k)
        t->slice[k + 1] = derived[k];
    for (unsigned i = 0; i < 256; ++i) {
        uint32_t v = t->slice[0][i];
        for (unsigned k = 0; k < 3; ++k) {
            v = (v >> 8) ^ t->slice[0][v & 255];
            derived[k][i] = v;
        }
    }
}

static inline uint32_t picture_crc_word(const picture_crc_t *t, uint32_t crc, uint32_t word)
{
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    word = __builtin_bswap32(word);
#endif
    uint32_t v = word ^ crc;
    return t->slice[3][v & 255] ^ t->slice[2][(v >> 8) & 255] ^
           t->slice[1][(v >> 16) & 255] ^ t->slice[0][v >> 24];
}

// For a delta band, dst already holds one XOR operand. Checksum the completed
// word as it is written so the reader only traverses two PSRAM ranges.
// Keep the CRC table addresses in registers in the S3 loop, including size-optimized builds.
#if defined(__XTENSA__) && defined(__OPTIMIZE_SIZE__) && !defined(__clang__)
__attribute__((optimize("O2")))
#endif
static uint32_t finish_band(const picture_crc_t *restrict t, uint32_t crc, uint16_t *dst,
                            const uint16_t *xor_band, size_t bytes)
{
    typedef uint32_t word_t __attribute__((may_alias));
    if (!(((uintptr_t)dst | (uintptr_t)xor_band) & 3)) {
        word_t *out = (word_t *)dst;
        size_t words = bytes / sizeof(*out);
        if (xor_band) {
            const word_t *operand = (const word_t *)xor_band;
            const word_t *end = out + words;
#if defined(__XTENSA__) && defined(__GNUC__) && !defined(__clang__)
            // Process a 64-byte line per group in the board's aligned XOR path.
#pragma GCC unroll 16
#endif
            while (out < end) {
                uint32_t v = *out ^ *operand++;
                *out++ = v;
                crc = picture_crc_word(t, crc, v);
            }
        } else {
            for (size_t i = 0; i < words; ++i)
                crc = picture_crc_word(t, crc, out[i]);
        }
        size_t pixels = words * 2;
        dst += pixels;
        if (xor_band) xor_band += pixels;
        bytes %= sizeof(*out);
    }
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *operand = (const uint8_t *)xor_band;
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t v = out[i];
        if (operand) {
            v ^= operand[i];
            out[i] = v;
        }
        crc = (crc >> 8) ^ t->slice[0][(crc ^ v) & 255];
    }
    return crc;
}

bool pack_video_read_timed(void *ctx, uint16_t *rgb, const uint16_t *previous, uint64_t *pts,
                           uint32_t *duration_us, uint32_t *crc, bool (*cancelled)(void *), void *cancel_ctx)
{
    pack_video_t *v = ctx;
    const uint8_t *record = v->frame < v->info.frames ? take_bytes(v, 16) : NULL;
    if (!record)
        return false;
    *pts = u64(record);
    *duration_us = u32(record + 8);
    *crc = u32(record + 12);
    if (*pts > UINT64_C(86400000000) || *pts < v->previous_pts || (!v->frame && *pts != 0) ||
        !*duration_us || *duration_us > 1000000)
        return false;
    picture_crc_t table;
    z_crc_t derived[3][256];
    picture_crc_init(&table, derived);
    uint32_t running_crc = UINT32_MAX;
    for (unsigned y = 0; y < v->info.height; y += BAND_ROWS) {
        if (cancelled && cancelled(cancel_ctx))
            return false;
        const uint8_t *band = take_bytes(v, 4);
        if (!band)
            return false;
        unsigned n = u16(band), coding = band[2];
        size_t rows = v->info.height - y < BAND_ROWS ? v->info.height - y : BAND_ROWS;
        size_t words = rows * v->info.width, offset = (size_t)y * v->info.width, bytes = words * 2;
        uint16_t *dst = rgb + offset;
        if (band[3] || coding > 3 || n > bytes || (coding >= 2 && !previous))
            return false;
        const uint16_t *xor_band = coding == 2 ? previous + offset : NULL;
        if (coding == 3) {
            if (n) return false;
            if (rgb != previous)
                memcpy(dst, previous + offset, bytes);
        } else {
            if (!n || (coding == 0 && n != bytes))
                return false;
            const uint8_t *payload = take_bytes(v, n);
            if (!payload)
                return false;
            if (coding == 0)
                memcpy(dst, payload, bytes);
            else {
                uint16_t *decoded = dst;
                if (coding == 2 && rgb == previous) {
                    if (!reserve_delta_band(v))
                        return false;
                    decoded = v->delta;
                    xor_band = decoded;
                }
                if (pack_inflate_band(payload, n, (uint8_t *)decoded, bytes) != ESP_OK)
                    return false;
            }
        }
        running_crc = finish_band(&table, running_crc, dst, xor_band, bytes);
    }
    if (~running_crc != *crc)
        return false;
    v->previous_pts = *pts;
    ++v->frame;
    return v->frame != v->info.frames || (v->begin == v->end && fgetc(v->file) == EOF && !ferror(v->file));
}

bool pack_video_read(void *ctx, uint16_t *rgb, const uint16_t *previous, uint64_t *pts,
                     uint32_t *crc, bool (*cancelled)(void *), void *cancel_ctx)
{
    uint32_t duration_us;
    return pack_video_read_timed(ctx, rgb, previous, pts, &duration_us, crc, cancelled, cancel_ctx);
}
