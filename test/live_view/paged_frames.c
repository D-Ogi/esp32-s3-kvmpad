// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "paged_video.h"
#include "libavutil/sha.h"
#include "libavutil/mem.h"
#include <inttypes.h>
#include <stdlib.h>
typedef struct {
    struct AVSHA *sha;
    unsigned frames, limit;
} test_t;
static bool digest(void *ctx, kvm_paged_vm_t *v, const kvm_paged_picture_t *f)
{
    test_t *t = ctx;
    for (unsigned p = 0; p < 3; ++p) {
        uint8_t row[960], d[32];
        unsigned w = f->width >> (!!p), h = f->height >> (!!p);
        av_sha_init(t->sha, 256);
        for (unsigned y = 0; y < h; ++y) {
            if (!kvm_paged_read(v, f->plane[p] + y * f->stride[p], row, w))
                return false;
            av_sha_update(t->sha, row, w);
        }
        av_sha_final(t->sha, d);
        for (unsigned i = 0; i < 32; i++)
            printf("%02x", d[i]);
        putchar(p == 2 ? '\n' : ' ');
    }
    fflush(stdout);
    t->frames++;
    return true;
}
static bool stop(void *ctx)
{
    test_t *t = ctx;
    return t->limit && t->frames >= t->limit;
}
int main(int argc, char **argv)
{
    if (argc != 6)
        return 1;
    FILE *elf = fopen(argv[1], "rb"), *src = fopen(argv[2], "rb"), *scratch = tmpfile();
    if (!elf || !src || !scratch || fseek(elf, 0, SEEK_END))
        return 1;
    long n = ftell(elf);
    if (n < 52 || n > 8000000 || fseek(elf, 0, SEEK_SET))
        return 1;
    uint8_t *code = malloc(n);
    if (!code || fread(code, 1, n, elf) != (size_t)n)
        return 1;
    test_t t = {.sha = av_sha_alloc(), .limit = (unsigned)atoi(argv[4])};
    kvm_paged_sink_t sink = {&t, digest, stop};
    if (!t.sha) return 1;
    kvm_paged_stats_t stats = {0};
    int rc = kvm_paged_run(code, n, src, scratch, atoi(argv[3]), atoi(argv[5]) != 0, &sink, &stats);
    fprintf(stderr,
            "rc=%d guest=%d last_pc=%08" PRIx32 " frames=%u steps=%" PRIu64 " read_bytes=%" PRIu64
            " write_bytes=%" PRIu64 " scratch_bytes=%u rom_loads=%u\n",
            rc, stats.guest_status, stats.last_pc, stats.frames, stats.steps, stats.page_reads * 4096,
            stats.page_writes * 4096, stats.scratch_bytes, stats.rom_loads);
    free(code);
    av_free(t.sha);
    fclose(scratch);
    fclose(src);
    fclose(elf);
    return (rc == 2 && t.limit && t.frames == t.limit) ? 0 : rc;
}
