// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_decode.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"
#include "libavutil/pixfmt.h"

#if defined(ESP_PLATFORM) || defined(KVM_TEST_EXTERNAL_HEAP)
kvm_play_result_t kvm_h264_heap_open(bool live);
kvm_play_result_t kvm_h264_header_heap_open(bool mp4);
bool kvm_h264_heap_close(bool live);
#else
static atomic_flag active = ATOMIC_FLAG_INIT;
static kvm_play_result_t kvm_h264_heap_open(bool live)
{
    (void)live;
    return atomic_flag_test_and_set(&active) ? KVM_PLAY_UNAVAILABLE : KVM_PLAY_OK;
}
static bool kvm_h264_heap_close(bool live)
{
    (void)live;
    atomic_flag_clear(&active);
    return false;
}
static kvm_play_result_t kvm_h264_header_heap_open(bool mp4)
{
    (void)mp4;
    return kvm_h264_heap_open(false);
}
#endif

bool kvm_h264_live_open(void)
{
    return kvm_h264_heap_open(true) == KVM_PLAY_OK;
}
void kvm_h264_live_close(void)
{
    kvm_h264_heap_close(true);
}

static const char *reconstruction;
const char *kvm_media_rr_mode(void)
{
    return reconstruction;
}

// The reference player limits gaps between anchors to 600 CFR slots. A few
// additional slots accommodate H.264 coding-order packet reordering.
enum { TEMPORAL_GAP = 600, TEMPORAL_SLOTS = TEMPORAL_GAP + 32 };
extern int ff_kvm_rr_safe(int64_t pts, int loose);
extern void ff_kvm_rr_release_hint(void);
typedef struct {
    AVRational period;
    int64_t origin;
    unsigned next, packets, synthesized;
    struct {
        int64_t pts;
        bool occupied;
    } slots[TEMPORAL_SLOTS];
    AVBufferRef *previous_buffer;
    kvm_media_yuv_t previous;
    uint8_t *mixed;
} temporal_t;

typedef struct {
    AVCodecContext *codec;
    AVPacket *packet;
    AVFrame *frame;
    AVFrame *scratch;
    kvm_h264_picture_t *retain;
    const kvm_media_playback_t *sink;
    AVRational time_base;
    unsigned frames;
    bool live;
    temporal_t *temporal;
} decoder_t;

static void release_scratch_view(void *ctx, uint8_t *base)
{
    (void)base;
    AVBufferRef *reference = ctx;
    av_buffer_unref(&reference);
}

static bool cancelled(const kvm_media_playback_t *s)
{
    return s->cancelled && s->cancelled(s->ctx);
}

static enum AVPixelFormat choose_format(AVCodecContext *c, const enum AVPixelFormat *formats)
{
    decoder_t *d = c->opaque;
    // FFmpeg exposes parsed SPS geometry and references here, before allocating its
    // picture-sized H.264 tables. The independent get_buffer check also guards output.
    if (cancelled(d->sink) || c->coded_width < 1 || c->coded_width > 1920 ||
        c->coded_height < 1 || c->coded_height > 1088 || c->refs > (d->live ? 1 : 16))
        return AV_PIX_FMT_NONE;
    if (d->live && (c->coded_width != 512 || c->coded_height != 512 ||
                    c->width != 512 || c->height != 512))
        return AV_PIX_FMT_NONE;
    for (; *formats != AV_PIX_FMT_NONE; ++formats) {
        if (*formats == AV_PIX_FMT_YUV420P || *formats == AV_PIX_FMT_YUVJ420P)
            return *formats;
    }
    return AV_PIX_FMT_NONE;
}

static int get_buffer(AVCodecContext *c, AVFrame *f, int flags)
{
    decoder_t *d = c->opaque;
    if (cancelled(d->sink) || c->refs > (d->live ? 1 : 16) || f->width < 1 || f->height < 1 ||
        f->width > 1920 || f->height > 1088 || (d->live && (f->width != 512 || f->height != 512)) ||
        (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P)) {
        return AVERROR_INVALIDDATA;
    }
    if (!reconstruction)
        return avcodec_default_get_buffer2(c, f, flags);
    // The reduced pictures own their references. Native intra prediction only
    // uses the current full-size scratch, with distinct buffer IDs for FFmpeg.
    if (!d->scratch) {
        d->scratch = av_frame_alloc();
        if (!d->scratch)
            return AVERROR(ENOMEM);
        d->scratch->width = f->width;
        d->scratch->height = f->height;
        d->scratch->format = f->format;
        int rc = avcodec_default_get_buffer2(c, d->scratch, flags);
        if (rc < 0)
            return rc;
    }
    if (d->scratch->width != f->width || d->scratch->height != f->height || d->scratch->format != f->format)
        return AVERROR_INVALIDDATA;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; ++i) {
        f->data[i] = d->scratch->data[i];
        f->linesize[i] = d->scratch->linesize[i];
        if (!d->scratch->buf[i])
            continue;
        AVBufferRef *ref = av_buffer_ref(d->scratch->buf[i]);
        if (!ref)
            return AVERROR(ENOMEM);
        f->buf[i] = av_buffer_create(ref->data, ref->size, release_scratch_view, ref, 0);
        if (!f->buf[i]) {
            av_buffer_unref(&ref);
            return AVERROR(ENOMEM);
        }
    }
    return 0;
}

static bool decoder_open(decoder_t *d, AVCodecParameters *params)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec)
        return false;
    d->codec = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    d->packet = av_packet_alloc();
    if (!d->codec || !d->frame || !d->packet)
        return false;
    if (params && avcodec_parameters_to_context(d->codec, params) < 0)
        return false;
    d->codec->opaque = d;
    d->codec->thread_count = 1;
    d->codec->thread_type = 0;
    d->codec->get_format = choose_format;
    d->codec->get_buffer2 = get_buffer;
    d->codec->max_pixels = d->live ? 576 * 576 : 2048 * 1152;
    d->codec->err_recognition = AV_EF_EXPLODE;
    if (reconstruction)
        d->codec->skip_loop_filter = AVDISCARD_ALL;
    if (d->temporal)
        d->codec->skip_frame = AVDISCARD_NONREF;
    return avcodec_open2(d->codec, codec, NULL) == 0;
}

static void decoder_close(decoder_t *d)
{
    av_frame_free(&d->frame);
    av_packet_free(&d->packet);
    avcodec_free_context(&d->codec);
    av_frame_free(&d->scratch);
}

static bool temporal_tick(decoder_t *d, int64_t pts, unsigned *slot)
{
    temporal_t *t = d->temporal;
    if (pts == AV_NOPTS_VALUE || pts < t->origin || (uint64_t)pts - (uint64_t)t->origin > INT64_MAX)
        return false;
    int64_t delta = pts - t->origin;
    int64_t tick = av_rescale_q(delta, d->time_base, t->period);
    if (tick < 0 || tick > INT_MAX - 1 || (uint64_t)tick < t->next || (uint64_t)tick - t->next >= TEMPORAL_SLOTS)
        return false;
    // A rational frame rate can require alternating integer timestamps in
    // container units. Allow that quantization, but reject variable-rate gaps.
    int64_t expected = av_rescale_q(tick, t->period, d->time_base);
    if (expected < 0 || (uint64_t)(delta > expected ? delta - expected : expected - delta) > 1)
        return false;
    *slot = (unsigned)tick;
    return true;
}

static bool temporal_packet(decoder_t *d, AVPacket *p)
{
    temporal_t *t = d->temporal;
    unsigned tick;
    if (!temporal_tick(d, p->pts, &tick))
        return false;
    unsigned i = tick % TEMPORAL_SLOTS;
    if (t->slots[i].occupied)
        return false;
    t->slots[i].pts = p->pts;
    t->slots[i].occupied = true;
    ++t->packets;
    if (d->sink->reconstruction == KVM_H264_RR_SMART_LOOSE)
        d->codec->skip_frame = ff_kvm_rr_safe(p->pts, 1) ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    return true;
}

static bool temporal_emit(decoder_t *d, const kvm_media_yuv_t *picture, bool synthetic)
{
    temporal_t *t = d->temporal;
    unsigned i = t->next % TEMPORAL_SLOTS;
    if (!t->slots[i].occupied || cancelled(d->sink))
        return false;
    kvm_media_yuv_t view = *picture;
    view.pts_us = av_rescale_q(t->slots[i].pts, d->time_base, AV_TIME_BASE_Q);
    // Durations are display intervals. MP4 sample durations can be expressed
    // in coding order; a B picture can have a different following display PTS.
    unsigned next = (t->next + 1) % TEMPORAL_SLOTS;
    int64_t following = t->slots[next].pts;
    if (!t->slots[next].occupied) {
        int64_t offset = av_rescale_q(t->next + 1, t->period, d->time_base);
        if (offset < 0 || t->origin > INT64_MAX - offset)
            return false;
        following = t->origin + offset;
    }
    int64_t end = av_rescale_q(following, d->time_base, AV_TIME_BASE_Q);
    if (view.pts_us == INT64_MIN || end <= view.pts_us || (uint64_t)end - (uint64_t)view.pts_us > INT64_MAX)
        return false;
    view.duration_us = end - view.pts_us;
    if (!d->sink->frame(d->sink->ctx, &view))
        return false;
    t->slots[i].occupied = false;
    ++t->next;
    t->synthesized += synthetic;
    return true;
}

static bool temporal_anchor(decoder_t *d, const kvm_media_yuv_t *current, AVFrame *f)
{
    temporal_t *t = d->temporal;
    unsigned tick;
    if (!temporal_tick(d, f->pts, &tick) || tick - t->next >= TEMPORAL_GAP || !f->buf[0] ||
        !current->width || !current->height || ((current->width | current->height) & 1) ||
        current->width > 960 || current->height > 540)
        return false;
    for (unsigned p = 0; p < 3; ++p)
        if (!current->plane[p] || current->stride[p] < (int)(current->width >> (p != 0)))
            return false;
    if (t->previous_buffer && (t->previous.width != current->width || t->previous.height != current->height))
        return false;
    unsigned gap = tick - t->next + 1;
    for (unsigned j = 1; j < gap; j++) {
        kvm_media_yuv_t mixed = *current;
        if (t->previous_buffer) {
            if (!t->mixed)
                t->mixed = av_malloc((size_t)current->width * current->height * 3 / 2);
            if (!t->mixed)
                return false;
            uint8_t *dst = t->mixed;
            unsigned a = (j * 256 + gap / 2) / gap;
            for (unsigned p = 0; p < 3; p++) {
                unsigned w = current->width >> (p != 0), h = current->height >> (p != 0);
                mixed.plane[p] = dst;
                mixed.stride[p] = w;
                for (unsigned y = 0; y < h; y++) {
                    if (cancelled(d->sink))
                        return false;
                    const uint8_t *lhs = t->previous.plane[p] + y * t->previous.stride[p];
                    const uint8_t *rhs = current->plane[p] + y * current->stride[p];
                    for (unsigned x = 0; x < w; x++)
                        *dst++ = ((256 - a) * lhs[x] + a * rhs[x] + 128) >> 8;
                }
            }
        }
        if (!temporal_emit(d, &mixed, true))
            return false;
    }
    if (!temporal_emit(d, current, false))
        return false;
    av_buffer_unref(&t->previous_buffer);
    // The v2 overlay keeps the reduced Y/U/V in one reference-counted buffer.
    t->previous_buffer = av_buffer_ref(f->buf[0]);
    t->previous = *current;
    return t->previous_buffer != NULL;
}

static bool temporal_finish(decoder_t *d)
{
    temporal_t *t = d->temporal;
    if (!t->previous_buffer || t->next > t->packets || t->packets - t->next >= TEMPORAL_GAP)
        return false;
    while (t->next < t->packets)
        if (!temporal_emit(d, &t->previous, true))
            return false;
    return true;
}

// FFmpeg 6 MOV leaves duration unset for samples with composition offsets.
// The sample index already contains their decoding timestamps, in track units.
// Copy the duration onto the packet so it follows its picture through B-frame reordering.
static void mp4_sample_duration(AVStream *st, AVPacket *pkt, bool edited, int64_t visible_ticks)
{
    if (pkt->duration > 0 || pkt->dts == AV_NOPTS_VALUE)
        return;
    int i = av_index_search_timestamp(st, pkt->dts, AVSEEK_FLAG_ANY);
    const AVIndexEntry *entry = avformat_index_get_entry(st, i);
    if (!entry || entry->timestamp != pkt->dts || entry->pos != pkt->pos)
        return;
    int64_t end;
    if (i + 1 < avformat_index_get_entries_count(st)) {
        end = avformat_index_get_entry(st, i + 1)->timestamp;
    } else {
        const AVIndexEntry *first = avformat_index_get_entry(st, 0);
        if (st->nb_frames != (int64_t)i + 1 || st->duration <= 0)
            return;
        // The edit list excludes preroll from the stream duration even though
        // the sample index starts at the preroll DTS. Only charge visible samples.
        if (edited) {
            if (visible_ticks >= 0 && visible_ticks < st->duration)
                pkt->duration = st->duration - visible_ticks;
            return;
        }
        if (first->timestamp > INT64_MAX - st->duration)
            return;
        end = first->timestamp + st->duration;
    }
    if (end > pkt->dts && (uint64_t)end - (uint64_t)pkt->dts <= INT64_MAX)
        pkt->duration = end - pkt->dts;
}

static bool submit(decoder_t *d, AVPacket *packet)
{
    if (cancelled(d->sink) || avcodec_send_packet(d->codec, packet) < 0)
        return false;
    int rc;
    while ((rc = avcodec_receive_frame(d->codec, d->frame)) == 0) {
        AVFrame *f = d->frame;
        if (cancelled(d->sink) || f->decode_error_flags || (f->flags & AV_FRAME_FLAG_CORRUPT) ||
            (d->live && (d->frames || f->width != 512 || f->height != 512 || !(f->flags & AV_FRAME_FLAG_KEY) ||
                         f->pict_type != AV_PICTURE_TYPE_I)))
            return false;
        kvm_media_yuv_t view = {.plane = {f->data[0], f->data[1], f->data[2]},
                                .stride = {f->linesize[0], f->linesize[1], f->linesize[2]},
                                .width = f->width,
                                .height = f->height,
                                .pts_us = INT64_MIN,
                                .full_range =
                                    f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P,
                                .bt709 = f->colorspace == AVCOL_SPC_BT709};
        if (d->time_base.num && f->best_effort_timestamp != AV_NOPTS_VALUE)
            view.pts_us = av_rescale_q(f->best_effort_timestamp, d->time_base, AV_TIME_BASE_Q);
        if (d->time_base.num && f->duration > 0)
            view.duration_us = av_rescale_q(f->duration, d->time_base, AV_TIME_BASE_Q);
        if (d->retain) {
            d->retain->reference = av_frame_clone(f);
            if (!d->retain->reference)
                return false;
            d->retain->yuv = view;
        } else if (d->temporal) {
            if (!temporal_anchor(d, &view, f))
                return false;
        } else if (!d->sink->frame(d->sink->ctx, &view))
            return false;
        ++d->frames;
        av_frame_unref(f);
    }
    return packet ? rc == AVERROR(EAGAIN) : rc == AVERROR_EOF;
}

typedef struct {
    FILE *file;
    long size;
    const kvm_media_playback_t *sink;
} card_input_t;

static int card_read(void *ctx, uint8_t *dest, int bytes)
{
    card_input_t *in = ctx;
    if (cancelled(in->sink))
        return AVERROR_EXIT;
    if (bytes <= 0)
        return AVERROR(EINVAL);
    // avio may bypass its own buffer for large packet reads. Keep every card
    // operation interruptible even when a single compressed sample is large.
    size_t count = fread(dest, 1, bytes > 8192 ? 8192 : (size_t)bytes, in->file);
    if (ferror(in->file))
        return AVERROR(EIO);
    return count ? (int)count : AVERROR_EOF;
}

static int64_t card_seek(void *ctx, int64_t offset, int whence)
{
    card_input_t *in = ctx;
    if (cancelled(in->sink))
        return AVERROR_EXIT;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return in->size;
    if (whence != SEEK_SET && whence != SEEK_END && whence != SEEK_CUR)
        return AVERROR(EINVAL);
    long base = whence == SEEK_CUR ? ftell(in->file) : whence == SEEK_END ? in->size : 0;
    if (base < 0 || offset < -base || offset > in->size - base)
        return AVERROR(EINVAL);
    long pos = base + (long)offset;
    return fseek(in->file, pos, SEEK_SET) == 0 ? pos : AVERROR(EIO);
}

static int deny_external(AVFormatContext *f, AVIOContext **pb, const char *url, int flags,
                         AVDictionary **options)
{
    (void)f;
    (void)pb;
    (void)url;
    (void)flags;
    (void)options;
    return AVERROR(EACCES);
}

static int open_card(card_input_t *input, bool mp4, AVFormatContext **out, AVIOContext **io)
{
    AVFormatContext *fmt = *out = avformat_alloc_context();
    uint8_t *buf = av_malloc(8192);
    *io = buf ? avio_alloc_context(buf, 8192, 0, input, card_read, NULL, card_seek) : NULL;
    if (!*io)
        av_free(buf);
    if (!fmt || !*io)
        return -1;
    fmt->pb = *io;
    fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    // MOV supplies complete samples and their decoding/presentation timestamps.
    // The decoder reads SPS and slice headers itself. A second header parser
    // would retain another parameter-set and RBSP workspace across samples.
    if (mp4)
        fmt->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
    fmt->io_open = deny_external;
    fmt->max_streams = 8;
    fmt->max_index_size = 512 * 1024;
    fmt->probesize = 128 * 1024;
    const AVInputFormat *type = av_find_input_format(mp4 ? "mov" : "h264");
    if (!type || avformat_open_input(out, NULL, type, NULL) != 0)
        return -1;
    int video = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        AVStream *st = fmt->streams[i];
        if (video < 0 && st->codecpar->codec_id == AV_CODEC_ID_H264)
            video = (int)i;
        else
            st->discard = AVDISCARD_ALL;
    }
    return video;
}

static void close_card(AVFormatContext **fmt, AVIOContext **io)
{
    avformat_close_input(fmt);
    if (*io)
        av_freep(&(*io)->buffer);
    avio_context_free(io);
}

static kvm_play_result_t read_source_info(FILE *file, bool mp4, kvm_h264_source_info_t *info,
                                          kvm_media_cancel_t cancel, void *ctx, bool preview)
{
    if (!file || !info || fseek(file, 0, SEEK_END))
        return KVM_PLAY_UNAVAILABLE;
    long length = ftell(file);
    if (length < 1 || fseek(file, 0, SEEK_SET))
        return KVM_PLAY_UNAVAILABLE;
    if (cancel && cancel(ctx))
        return KVM_PLAY_STOPPED;
    kvm_play_result_t admission = preview ? kvm_h264_header_heap_open(mp4) : kvm_h264_heap_open(false);
    if (admission != KVM_PLAY_OK)
        return admission;
    int w = 0, h = 0;
    unsigned fps_milli = 0;
    if (mp4) {
        kvm_media_playback_t sink = {.cancelled = cancel, .ctx = ctx};
        card_input_t input = {file, length, &sink};
        AVFormatContext *fmt = NULL;
        AVIOContext *io = NULL;
        int video = open_card(&input, true, &fmt, &io);
        if (video >= 0) {
            const AVStream *stream = fmt->streams[video];
            w = stream->codecpar->width;
            h = stream->codecpar->height;
            AVRational rate = stream->avg_frame_rate;
            if (rate.num > 0 && rate.den > 0) {
                int64_t value = av_rescale(rate.num, 1000, rate.den);
                if (value > 0 && value <= UINT_MAX)
                    fps_milli = (unsigned)value;
            }
        }
        close_card(&fmt, &io);
    } else {
        // Read up to one MiB to find the first access unit and its SPS. The
        // parser consumes syntax only and never allocates full reference pictures.
        AVCodecParserContext *p = av_parser_init(AV_CODEC_ID_H264);
        AVCodecContext *c = avcodec_alloc_context3(NULL);
        uint8_t *buf = av_malloc(4096 + AV_INPUT_BUFFER_PADDING_SIZE);
        if (p && c && buf) {
            c->codec_id = AV_CODEC_ID_H264;
            for (unsigned n = 0; n < 256 && !w && !(cancel && cancel(ctx)); ++n) {
                int count = (int)fread(buf, 1, 4096, file);
                if (ferror(file))
                    break;
                memset(buf + count, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                uint8_t *out;
                int bytes;
                int used = av_parser_parse2(p, c, &out, &bytes, buf, count, AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);
                if (used < 0)
                    break;
                if (bytes) {
                    w = p->width;
                    h = p->height;
                }
                if (!count || (used < count && !bytes))
                    break;
            }
        }
        av_free(buf);
        avcodec_free_context(&c);
        if (p)
            av_parser_close(p);
    }
    bool exhausted = kvm_h264_heap_close(false);
    int reset = fseek(file, 0, SEEK_SET);
    if (cancel && cancel(ctx))
        return KVM_PLAY_STOPPED;
    if (reset || w < 1 || w > 1920 || h < 1 || h > 1080)
        return exhausted ? KVM_PLAY_NO_MEMORY : KVM_PLAY_UNAVAILABLE;
    *info = (kvm_h264_source_info_t){(unsigned)w, (unsigned)h, fps_milli};
    return KVM_PLAY_OK;
}

kvm_play_result_t kvm_media_h264_inspect(FILE *file, bool mp4, kvm_h264_source_info_t *info,
                                         kvm_media_cancel_t cancel, void *ctx)
{
    return read_source_info(file, mp4, info, cancel, ctx, true);
}

kvm_play_result_t kvm_media_h264_probe(FILE *file, bool mp4, unsigned *width, unsigned *height,
                                       kvm_media_cancel_t cancel, void *ctx)
{
    if (!width || !height)
        return KVM_PLAY_UNAVAILABLE;
    kvm_h264_source_info_t info;
    kvm_play_result_t result = read_source_info(file, mp4, &info, cancel, ctx, false);
    if (result == KVM_PLAY_OK) {
        *width = info.width;
        *height = info.height;
    }
    return result;
}

kvm_play_result_t kvm_media_play_h264_stats(FILE *file, bool mp4, const kvm_media_playback_t *sink,
                                          kvm_h264_play_stats_t *stats)
{
    if (stats)
        *stats = (kvm_h264_play_stats_t){0};
    if (!file || !sink || !sink->frame || (unsigned)sink->reconstruction > KVM_H264_RR_SMART_LOOSE ||
        (!mp4 && sink->reconstruction >= KVM_H264_RR_BLEND))
        return KVM_PLAY_UNAVAILABLE;
    if (cancelled(sink))
        return KVM_PLAY_STOPPED;
    if (fseek(file, 0, SEEK_END) != 0)
        return KVM_PLAY_UNAVAILABLE;
    long length = ftell(file);
    if (length < 1 || fseek(file, 0, SEEK_SET))
        return KVM_PLAY_UNAVAILABLE;
    kvm_play_result_t admission = kvm_h264_heap_open(false);
    if (admission != KVM_PLAY_OK)
        return admission;
    reconstruction = sink->reconstruction == KVM_H264_REDUCED          ? "rr"
                     : sink->reconstruction == KVM_H264_RR_BLEND       ? "rr_blend"
                     : sink->reconstruction == KVM_H264_RR_SMART_LOOSE ? "rr_smart_loose"
                     : sink->reconstruction == KVM_H264_LARGE_BANK     ? "rr_bank_large"
                                                                       : NULL;
    bool ok = false;
    decoder_t d = {.sink = sink};
    card_input_t input = {file, length, sink};
    AVFormatContext *fmt = NULL;
    AVIOContext *io = NULL;
    int video = open_card(&input, mp4, &fmt, &io);
    if (video < 0)
        goto done;
    AVStream *st = fmt->streams[video];
    d.time_base = st->time_base;
    if (sink->reconstruction >= KVM_H264_RR_BLEND) {
        AVRational rate = av_guess_frame_rate(fmt, st, NULL);
        if (rate.num <= 0 || rate.den <= 0)
            rate = st->avg_frame_rate;
        if (rate.num <= 0 || rate.den <= 0 || d.time_base.num <= 0 || d.time_base.den <= 0 ||
            st->start_time == AV_NOPTS_VALUE || ((st->codecpar->width | st->codecpar->height) & 3))
            goto done;
        d.temporal = av_mallocz(sizeof(*d.temporal));
        if (!d.temporal)
            goto done;
        d.temporal->origin = st->start_time;
        d.temporal->period = av_inv_q(rate);
    }
    if (!decoder_open(&d, st->codecpar))
        goto done;
    int rc = AVERROR_EXIT;
    bool edited = false;
    int64_t visible_ticks = 0;
    while (!cancelled(sink) && (rc = av_read_frame(fmt, d.packet)) == 0) {
        if (d.packet->stream_index == video) {
            // Discarded MOV samples still feed the decoder's reference pictures,
            // but they have no slot or duration on the visible timeline.
            bool preroll = mp4 && (d.packet->flags & AV_PKT_FLAG_DISCARD);
            if (preroll)
                edited = true;
            else if (mp4) {
                mp4_sample_duration(st, d.packet, edited, visible_ticks);
                if (visible_ticks >= 0 && d.packet->duration > 0 && d.packet->duration <= INT64_MAX - visible_ticks)
                    visible_ticks += d.packet->duration;
                else
                    visible_ticks = -1;
            }
            if ((d.temporal && !preroll && !temporal_packet(&d, d.packet)) || !submit(&d, d.packet))
                goto done;
        }
        av_packet_unref(d.packet);
    }
    ok = !cancelled(sink) && rc == AVERROR_EOF && !ferror(file) && submit(&d, NULL) && d.frames;
done:
    if (d.temporal) {
        if (ok)
            ok = temporal_finish(&d);
        if (stats)
            stats->synthesized = d.temporal->synthesized;
        av_buffer_unref(&d.temporal->previous_buffer);
        av_free(d.temporal->mixed);
        av_freep(&d.temporal);
        ff_kvm_rr_release_hint();
    }
    if (stats)
        stats->decoded = d.frames;
    decoder_close(&d);
    close_card(&fmt, &io);
    reconstruction = NULL;
    bool exhausted = kvm_h264_heap_close(false);
    if (cancelled(sink))
        return KVM_PLAY_STOPPED;
    // The supplied smart selector retains thumbnails. Its upstream OOM
    // fallback decodes extra frames and cannot guarantee the selected profile.
    if (exhausted && sink->reconstruction == KVM_H264_RR_SMART_LOOSE)
        return KVM_PLAY_NO_MEMORY;
    return ok ? KVM_PLAY_OK : exhausted ? KVM_PLAY_NO_MEMORY : KVM_PLAY_UNAVAILABLE;
}

// Restrict live syntax before giving a padded packet to libavcodec. first_mb_in_slice
// is unsigned Exp-Golomb coded; its first bit is one iff this is macroblock zero.
static bool unregistered_sei_only(const uint8_t *p, size_t len)
{
    if (len < 3 || p[0] != 5 || p[len - 1] != 0x80)
        return false;
    size_t n = 0, i = 1;
    while (i < len - 1 && p[i] == 255) {
        n += 255;
        ++i;
    }
    // Match a single byte-aligned user-data payload with no escaped bytes.
    // A combined SEI or a different payload stays intact for the decoder.
    if (i >= len - 1)
        return false;
    n += p[i++];
    return n >= 16 && n == len - i - 1;
}

static bool live_syntax(const uint8_t *p, size_t len, uint8_t *output, size_t *used)
{
    bool sps = false, pps = false, idr = false;
    size_t pos = 0;
    *used = 0;
    while (pos < len) {
        size_t start = pos;
        while (pos < len && !p[pos])
            ++pos;
        if (pos - start < 2 || pos == len || p[pos++] != 1 || pos == len)
            return false;
        uint8_t header = p[pos++];
        unsigned type = header & 31;
        if (header & 0x80)
            return false;
        size_t end = pos;
        while (end + 2 < len && (p[end] || p[end + 1] || p[end + 2] != 1))
            ++end;
        if (end + 2 >= len)
            end = len;
        size_t next = end;
        while (end > pos && !p[end - 1])
            --end;
        if (end == pos)
            return false;
        switch (type) {
        case 7:
            if (sps || pps || idr || end - pos < 4 || p[pos] != 66)
                return false;
            sps = true;
            break;
        case 8:
            if (!sps || pps || idr)
                return false;
            pps = true;
            break;
        case 5:
            if (!pps || !(header & 0x60) || (bool)(p[pos] & 0x80) == idr)
                return false;
            idr = true;
            break;
        case 6:
        case 9:
            if (idr)
                return false;
            break;
        default:
            return false;
        }
        // These NALs carry no picture samples. In particular, an encoder's
        // large private user-data string need not occupy AVPacket, RBSP and SEI
        // side-data storage simultaneously. Preserve all other supplemental data.
        if (type != 9 && !(type == 6 && unregistered_sei_only(p + pos, end - pos))) {
            if (output)
                memcpy(output + *used, p + start, next - start);
            *used += next - start;
        }
        pos = next;
    }
    return idr;
}

static bool live_frame(void *ctx, const kvm_media_yuv_t *f)
{
    return kvm_media_render_yuv(f, false, ctx);
}

static bool live_cancelled(void *ctx)
{
    const kvm_media_target_t *t = ctx;
    return t->cancelled && t->cancelled(t->ctx);
}

static bool decode_idr(const uint8_t *data, size_t size, const kvm_media_playback_t *sink,
                       kvm_h264_picture_t *retain)
{
    size_t useful;
    if (!live_syntax(data, size, NULL, &useful))
        return false;
    decoder_t d = {.live = true, .sink = sink, .retain = retain};
    bool ok = false;
    if (decoder_open(&d, NULL) && av_new_packet(d.packet, (int)useful) == 0) {
        size_t copied;
        ok = live_syntax(data, size, d.packet->data, &copied) && copied == useful &&
             submit(&d, d.packet) && submit(&d, NULL) && d.frames == 1;
    }
    decoder_close(&d);
    return ok;
}

bool kvm_h264_idr(const uint8_t *data, size_t size, const kvm_media_target_t *target)
{
    kvm_media_playback_t sink = {.frame = live_frame, .cancelled = live_cancelled, .ctx = (void *)target};
    return decode_idr(data, size, &sink, NULL);
}

void kvm_media_h264_release(kvm_h264_picture_t *slot)
{
    if (slot) {
        AVFrame *f = slot->reference;
        av_frame_free(&f);
        *slot = (kvm_h264_picture_t){0};
    }
}

bool kvm_h264_retain(kvm_h264_picture_t *slot, const uint8_t *data, size_t size,
                     kvm_media_cancel_t cancel, void *ctx)
{
    kvm_media_h264_release(slot);
    kvm_media_playback_t sink = {.cancelled = cancel, .ctx = ctx};
    if (decode_idr(data, size, &sink, slot))
        return true;
    kvm_media_h264_release(slot);
    return false;
}
