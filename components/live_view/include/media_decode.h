// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include "live_message.h"
#include <stdio.h>

// Erase live image storage before returning it to its allocator.
void kvm_media_zeroize(void *buffer, size_t bytes);

typedef bool (*kvm_media_cancel_t)(void *ctx);
typedef struct {
    uint16_t *pixels;
    unsigned width, height, stride; // stride in RGB565 words; allocation >= stride*height words
    kvm_media_cancel_t cancelled;
    void *ctx;
} kvm_media_target_t;

typedef struct {
    const uint8_t *plane[3]; // Y, U, V; pointers valid only during the callback
    int stride[3];
    unsigned width, height;
    int64_t pts_us;      // INT64_MIN when timing is unavailable
    int64_t duration_us; // 0 when unavailable
    bool full_range, bt709;
} kvm_media_yuv_t;

typedef enum {
    KVM_H264_EXACT = 0,
    KVM_H264_REDUCED,
    KVM_H264_LARGE_BANK,
    KVM_H264_RR_BLEND,
    KVM_H264_RR_SMART_LOOSE,
} kvm_h264_reconstruction_t;

typedef struct {
    bool (*frame)(void *ctx, const kvm_media_yuv_t *picture);
    kvm_media_cancel_t cancelled;
    void *ctx;
    kvm_h264_reconstruction_t reconstruction; // reduced modes already deliver half-size planes
} kvm_media_playback_t;

// Synchronous, serialized worker calls. Only publish the staging surface after success.
// Never pass the currently visible surface while the UI can read it.
bool kvm_media_decode_live(kvm_live_codec_t codec, const uint8_t *data, size_t size,
                           const kvm_media_target_t *target);
typedef struct kvm_media_decoder kvm_media_decoder_t;
// Zero-initialize before first use. A picture retains the decoder's YUV allocations
// until release. Release both retired slots before closing their decoder session.
typedef struct {
    kvm_media_yuv_t yuv;
    void *reference;
} kvm_h264_picture_t;
bool kvm_media_h264_picture(kvm_media_decoder_t *decoder, kvm_h264_picture_t *slot,
                            const uint8_t *data, size_t size, kvm_media_cancel_t cancelled, void *ctx);
void kvm_media_h264_release(kvm_h264_picture_t *slot);
bool kvm_media_h264_band(const kvm_h264_picture_t *slot, unsigned y, uint16_t *rows);
// Reserve the selected codec's workspace at negotiation, release it at Stop.
kvm_media_decoder_t *kvm_media_decoder_open(kvm_live_codec_t codec);
bool kvm_media_decoder_frame(kvm_media_decoder_t *decoder, const uint8_t *data, size_t size,
                             const kvm_media_target_t *target);
// Full JPEG image matching the selected USB display surface (640x480 or 1024x600 RGB565).
bool kvm_media_usb_monitor_jpeg(kvm_media_decoder_t *decoder, const uint8_t *data, size_t size,
                                const kvm_media_target_t *target);
// Decode directly into a 1024x600 monitor canvas using centered nearest-neighbor fit and black bars.
// The JPEG header must match the declared source geometry (up to 1920x1200).
bool kvm_media_usb_monitor_jpeg_fit(kvm_media_decoder_t *decoder, const uint8_t *data, size_t size,
                                    const kvm_media_target_t *target, unsigned source_width, unsigned source_height);
// Visible RGB565 pixels from the last successful USB monitor decode on this worker.
bool kvm_media_usb_monitor_has_nonblack(const kvm_media_decoder_t *decoder);
void kvm_media_decoder_close(kvm_media_decoder_t *decoder);
bool kvm_media_has_h264(void);
// Read coded stream metadata before sizing RGB565 surfaces. Restores file position to zero.
bool kvm_media_h264_size(FILE *file, bool mp4, unsigned *width, unsigned *height, kvm_media_cancel_t cancel,
                         void *ctx);
typedef enum {
    KVM_PLAY_UNAVAILABLE,
    KVM_PLAY_OK,
    KVM_PLAY_NO_MEMORY,
    KVM_PLAY_STOPPED,
} kvm_play_result_t;
typedef struct {
    unsigned width, height;
    unsigned fps_milli; // MP4 average rate in thousandths of a frame per second, or zero if unknown.
} kvm_h264_source_info_t;
kvm_play_result_t kvm_media_h264_inspect(FILE *file, bool mp4, kvm_h264_source_info_t *info,
                                         kvm_media_cancel_t cancel, void *ctx);
kvm_play_result_t kvm_media_h264_probe(FILE *file, bool mp4, unsigned *width, unsigned *height,
                                       kvm_media_cancel_t cancel, void *ctx);
// Caller owns the open binary seekable file, positioned at zero, and the card lease.
// One H.264 decoder at a time. Cancellation, I/O failure and insufficient memory return false.
bool kvm_media_play_h264(FILE *file, bool mp4, const kvm_media_playback_t *sink);
kvm_play_result_t kvm_media_play_h264_result(FILE *file, bool mp4, const kvm_media_playback_t *sink);
// Temporal profiles accept timed, constant-rate MP4 with one video packet per display slot.
typedef struct {
    unsigned decoded, synthesized;
} kvm_h264_play_stats_t;
kvm_play_result_t kvm_media_play_h264_stats(FILE *file, bool mp4, const kvm_media_playback_t *sink,
                                          kvm_h264_play_stats_t *stats);
// Render the visible YUV frame at native or half size (half-up 2x2 plane average).
bool kvm_media_render_yuv(const kvm_media_yuv_t *picture, bool half, const kvm_media_target_t *target);
