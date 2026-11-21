// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "media_decode.h"

// The prior complete picture stays visible while the reader writes pixels. At 960 x 540,
// pixels can equal previous after LVGL releases the only available surface.
typedef bool (*media_rgb_read_t)(void *ctx, uint16_t *pixels, const uint16_t *previous, uint64_t *pts_us,
                                 uint32_t *duration_us, uint32_t *crc, bool (*cancelled)(void *), void *cancel_ctx);
kvm_play_result_t media_panel_play_rgb(uint32_t epoch, unsigned width, unsigned height, unsigned count,
                                      media_rgb_read_t read_frame, void *reader, bool (*present)(void));

// UI owner. A nonzero epoch admits work on this screen only. Physical Home/Stop
// calls close and then the supplied navigation callback. Programmatic close
// restores the previous screen without invoking that callback.
uint32_t media_panel_open(bool live, void (*stop)(void));
// Set the library filename and fit the picture inside the file player's viewport.
void media_panel_file_title(uint32_t epoch, const char *filename);
// UI owner. Override the local Stop destination for this file session. Home keeps its normal destination.
void media_panel_file_stop_action(uint32_t epoch, void (*stopped)(void));
// Open the capture settings with the profile's current crop mode selected.
uint32_t media_panel_open_magnifier(const char *profile_id, const char *mode, void (*stop)(void));
// UI owner, following a local Start live view action and profile handoff.
// The built-in magnifier may request source selection from its connected HOST.
bool media_panel_request_capture(uint32_t epoch);
// UI owner, for the composite profile whose telemetry grant accompanies this magnifier.
void media_panel_telemetry_status(const char *profile_id, const char *text);
// UI owner. Return live LCD refresh metrics without copying any displayed pixels.
bool media_panel_live_stats(unsigned *frames, unsigned *fps_tenths, unsigned *rx_to_lcd_ms);
void media_panel_close(void);

typedef struct {
    uint32_t epoch, frames;
    uint64_t window_us, media_window_us;  // Media span is zero without an increasing picture timeline.
    unsigned width, height;  // Last picture that completed a display refresh.
} media_panel_file_stats_t;
// LVGL owner, after successful playback and before close. Counts file-picture
// refresh completions. The first-to-last interval is zero for fewer than two.
// A stopped, unfinished or different session returns false and clears out.
bool media_panel_file_stats(uint32_t epoch, media_panel_file_stats_t *out);
// LVGL owner. Count completed file-picture refreshes for this session, including partial playback.
unsigned media_panel_file_frames(uint32_t epoch);
// Local LCD result. A NULL measurement retains the selected file and mode on failure.
void media_panel_file_summary(char *out, size_t capacity, const char *filename,
                              kvm_h264_reconstruction_t mode, const media_panel_file_stats_t *stats);

// Worker owns FILE and card lease until return. present() executes on this worker.
bool media_panel_play(uint32_t epoch, FILE *file, bool mp4, kvm_h264_reconstruction_t mode,
                      bool (*present)(void));
kvm_play_result_t media_panel_play_result(uint32_t epoch, FILE *file, bool mp4, kvm_h264_reconstruction_t mode,
                                          bool (*present)(void));
const char *media_panel_play_message(kvm_play_result_t result);

// Called by the authenticated bridge after both local consents.
bool media_panel_stream_start(uint32_t epoch, uint32_t id, kvm_live_codec_t codec);
bool media_panel_stream_push(uint32_t epoch, const uint8_t *message, size_t size);
void media_panel_stream_end(uint32_t epoch);

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
// Count zero-valued 8x6 grids only between two matching, visibly lit source grids.
// confirmed covers every available FB, inferred uses the calibrated DOUBLE_PARTIAL
// swap order. longest_us runs from the first black sample to the next nonblack one.
typedef struct {
    uint32_t samples, pairs, confirmed, inferred, ambiguous_samples, dark_at_picture, resyncs;
    uint64_t longest_us;
} media_blackout_stats_t;
// UI task. Observations taken from the video rectangle at flush and refresh events.
bool media_panel_blackout_stats(uint32_t epoch, media_blackout_stats_t *out);
// LVGL owner, for an open local file session before its worker starts.
void media_panel_test_pixel_size(uint32_t epoch);
void media_panel_time_without_scanout_crc(uint32_t epoch);
// UI owner, offline diagnostic. Two warm-up patterns precede the measured run.
// A successful start calls done after Stop/Home or automatic finish. A failed
// start returns false and leaves navigation recovery to the caller.
bool media_panel_synthetic_start(unsigned fps, unsigned frames, void (*done)(void));
bool media_panel_synthetic_h264_start(unsigned fps, unsigned frames, void (*done)(void));
bool media_panel_synthetic_h264_paced(unsigned fps, unsigned frames, void (*done)(void));
bool media_panel_synthetic_jpeg_paced(unsigned fps, unsigned frames, void (*done)(void));
bool media_panel_pixel_pattern(void (*done)(void));
bool media_panel_pixel_pattern_scan(void);
#endif
#ifdef KVM_MEDIA_PAGED_GUEST
// Native Annex B decode through the embedded decoder. Both files and the card
// lease are owned by the worker. Scratch must be fresh, private and seekable.
kvm_play_result_t media_panel_play_paged(uint32_t epoch, FILE *source, FILE *scratch,
                                        kvm_h264_reconstruction_t mode, bool (*present)(void));
#endif
