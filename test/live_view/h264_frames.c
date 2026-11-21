// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_decode.h"
#include "libavutil/sha.h"
#include "libavutil/mem.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef KVM_TEST_BOUNDED_HEAP
int kvm_test_pool_open(size_t bytes);
int kvm_test_pool_close(void);
#endif

typedef struct {
    struct AVSHA *sha;
    bool reduced, temporal, native_planes, report_timing;
    unsigned count, cancel_after;
    unsigned width, height;
} digest_t;

static bool cancel_digest(void *ctx)
{
    digest_t *s = ctx;
    return s->cancel_after && s->count >= s->cancel_after;
}
static bool frame_digest(void *ctx, const kvm_media_yuv_t *f)
{
    digest_t *s = ctx;
    struct AVSHA *sha = s->sha;
    if (s->width != (f->width << s->reduced) || s->height != (f->height << s->reduced))
        return false;
    uint8_t row[960], digest[32];
    if ((f->width | f->height) & (s->reduced ? 1 : 3))
        return false;
    for (int plane = 0; plane < 3; ++plane) {
        unsigned shift = (!s->reduced && !s->native_planes) + !!plane;
        unsigned w = f->width >> shift, h = f->height >> shift;
        if (!s->native_planes && w > sizeof(row))
            return false;
        av_sha_init(sha, 256);
        for (unsigned y = 0; y < h; ++y) {
            if (s->reduced || s->native_planes) {
                av_sha_update(sha, f->plane[plane] + y * f->stride[plane], w);
                continue;
            }
            const uint8_t *a = f->plane[plane] + 2 * y * f->stride[plane], *b = a + f->stride[plane];
            for (unsigned x = 0; x < w; ++x) {
                row[x] = (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) / 4;
            }
            av_sha_update(sha, row, w);
        }
        av_sha_final(sha, digest);
        for (unsigned i = 0; i < sizeof(digest); ++i)
            printf("%02x", digest[i]);
        putchar(plane == 2 ? '\n' : ' ');
    }
    if (s->temporal || s->report_timing)
        fprintf(stderr, "timing %" PRId64 " %" PRId64 "\n", f->pts_us, f->duration_us);
    ++s->count;
    return true;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    FILE *file = fopen(argv[3], "rb");
    if (!file)
        return 2;
#ifdef KVM_TEST_BOUNDED_HEAP
    const char *capacity = getenv("KVM_TEST_ARENA_BYTES");
    if (!capacity || !kvm_test_pool_open(strtoul(capacity, NULL, 10))) {
        fclose(file);
        return 2;
    }
#endif
    struct AVSHA *sha = av_sha_alloc();
    bool ok = false;
    if (sha) {
        digest_t digest = {.sha = sha, .reduced = strcmp(argv[2], "exact") != 0,
                           .native_planes = getenv("KVM_TEST_NATIVE_PLANES") != NULL,
                           .report_timing = getenv("KVM_TEST_REPORT_TIMING") != NULL};
        kvm_h264_reconstruction_t mode;
        if (!strcmp(argv[2], "exact"))
            mode = KVM_H264_EXACT;
        else if (!strcmp(argv[2], "rr"))
            mode = KVM_H264_REDUCED;
        else if (!strcmp(argv[2], "rr_bank_large"))
            mode = KVM_H264_LARGE_BANK;
        else if (!strcmp(argv[2], "rr_blend"))
            mode = KVM_H264_RR_BLEND;
        else if (!strcmp(argv[2], "rr_smart_loose"))
            mode = KVM_H264_RR_SMART_LOOSE;
        else
            return 2;
        digest.temporal = mode >= KVM_H264_RR_BLEND;
        const char *stop = getenv("KVM_TEMPORAL_TEST_STOP");
        if (stop)
            digest.cancel_after = (unsigned)strtoul(stop, NULL, 10);
        kvm_media_playback_t callbacks = {
            .frame = frame_digest, .cancelled = cancel_digest, .ctx = &digest, .reconstruction = mode};
        bool mp4 = !strcmp(argv[1], "mp4");
        kvm_h264_play_stats_t stats = {0};
        kvm_play_result_t result = KVM_PLAY_UNAVAILABLE;
        if (kvm_media_h264_size(file, mp4, &digest.width, &digest.height, NULL, NULL))
            result = kvm_media_play_h264_stats(file, mp4, &callbacks, &stats);
        ok = result == KVM_PLAY_OK;
        if (digest.temporal)
            fprintf(stderr, "temporal decoded=%u synthesized=%u result=%d\n", stats.decoded, stats.synthesized,
                    (int)result);
    }
    av_free(sha);
#ifdef KVM_TEST_BOUNDED_HEAP
    if (!kvm_test_pool_close())
        return 2;
#endif
#ifdef KVM_MEASURE_ALLOCATIONS
    extern void kvm_probe_report(void);
    kvm_probe_report();
#endif
    fclose(file);
    return ok ? 0 : 1;
}
