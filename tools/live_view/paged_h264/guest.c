// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Freestanding RV32 guest. Its only host operations are bounded reads of the
// already-open original Annex B stream, borrowed YUV frames and final status.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "libavcodec/avcodec.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
static int rpc(unsigned op, void *p, unsigned bytes)
{
    register unsigned a0 __asm__("a0") = (uintptr_t)p;
    register unsigned a1 __asm__("a1") = bytes;
    register unsigned a7 __asm__("a7") = op;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return (int)a0;
}
extern unsigned char guest_heap[];
void *_sbrk(ptrdiff_t n)
{
    static uintptr_t end;
    if (!end)
        end = (uintptr_t)guest_heap;
    if (n < 0 || (uintptr_t)n > 0x03f00000u - end) {
        errno = ENOMEM;
        return (void *)-1;
    }
    void *p = (void *)end;
    end += n;
    return p;
}
static const char *mode;
const char *kvm_media_rr_mode(void)
{
    return mode;
}
static int submit(AVCodecContext *c, AVFrame *f, AVPacket *pkt)
{
    if (avcodec_send_packet(c, pkt) < 0)
        return -4;
    int rc;
    while ((rc = avcodec_receive_frame(c, f)) == 0) {
        if (f->decode_error_flags || f->width < 1 || f->width > 960 || f->height < 1 || f->height > 544 ||
            (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P))
            return -5;
        uint32_t picture[] = {f->width,
                              f->height,
                              (uintptr_t)f->data[0],
                              (uintptr_t)f->data[1],
                              (uintptr_t)f->data[2],
                              f->linesize[0],
                              f->linesize[1],
                              f->linesize[2],
                              f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P,
                              f->colorspace == AVCOL_SPC_BT709};
        if (rpc(2, picture, sizeof(picture)))
            return -6;
        av_frame_unref(f);
    }
    return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : -7;
}
static int run(void)
{
    av_log_set_level(AV_LOG_QUIET);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *c = avcodec_alloc_context3(codec);
    AVCodecParserContext *parser = av_parser_init(AV_CODEC_ID_H264);
    AVFrame *f = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    if (!codec || !c || !parser || !f || !pkt)
        return -1;
    c->thread_count = 1;
    c->thread_type = 0;
    c->max_pixels = 1920 * 1088;
    c->err_recognition = AV_EF_EXPLODE;
    c->skip_loop_filter = AVDISCARD_ALL;
    if (avcodec_open2(c, codec, NULL) < 0)
        return -2;
    static uint8_t block[8192 + AV_INPUT_BUFFER_PADDING_SIZE];
    int rc = 0, bytes = 0;
    do {
        bytes = rpc(1, block, 8192);
        if (bytes < 0 || bytes > 8192)
            return -8;
        memset(block + bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
        int off = 0;
        do {
            int used = av_parser_parse2(parser, c, &pkt->data, &pkt->size, block + off, bytes - off, AV_NOPTS_VALUE,
                                        AV_NOPTS_VALUE, -1);
            if (used < 0 || used > bytes - off || (!used && bytes && !pkt->size))
                return -3;
            off += used;
            if (pkt->size && (rc = submit(c, f, pkt)))
                return rc;
        } while (off < bytes);
    } while (bytes);
    return submit(c, f, NULL);
}
void guest_entry(unsigned large)
{
    mode = large ? "rr_bank_large" : "rr";
    int rc = run();
    rpc(0, (void *)(uintptr_t)rc, 0);
    for (;;) {
    }
}
