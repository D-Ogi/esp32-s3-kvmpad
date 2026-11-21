// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// TinyUSB task. Only bytes from the dedicated display OUT pipe are accepted.
// Reset on configuration loss or USB reset. Suspend pauses the same configured stream.
void usb_monitor_feed(const uint8_t *bytes, size_t size);
void usb_monitor_reset(void);
bool usb_monitor_enable_windows_modes(void);

typedef struct {
    uint64_t bytes;
    uint32_t headers, rejected;
    unsigned header_bytes, payload_left;
} usb_monitor_input_stats_t;
usb_monitor_input_stats_t usb_monitor_input_stats(void);

typedef struct {
    unsigned queued, decoded, busy_drops, decode_errors, presented;
    unsigned superseded; // Complete waiting inputs replaced by a newer complete JPEG.
    unsigned input_slots; // JPEG slots allocated at the start of the most recent session.
    unsigned accelerated; // Successful frames using the S3 RGB565 block decoder.
    unsigned duplicate_jpegs; // Byte-identical to the current good picture; skipped before decoding.
} usb_monitor_stats_t;
usb_monitor_stats_t usb_monitor_stats(void);

// On-board microseconds, from reserving the first JPEG payload bytes to LVGL's completed refresh.
typedef struct {
    int64_t receive, queue, decode, ready, refresh;
} usb_monitor_stages_t;
typedef struct {
    unsigned samples;
    unsigned sequence; // Received JPEG sequence of the most recent measured presentation.
    usb_monitor_stages_t last, mean;
    int64_t peak_total;
} usb_monitor_latency_t;
usb_monitor_latency_t usb_monitor_latency(void);

// LVGL owner. Completed refreshes grouped by image damage before display-driver rounding.
typedef struct {
    unsigned full, partial, unchanged, last_pixels;
    uint64_t full_ms, partial_ms, partial_pixels;
    unsigned split;
} usb_monitor_redraw_t;
usb_monitor_redraw_t usb_monitor_view_redraw(void);

// LVGL owner. Prepare the foreground screen before enabling the display-only
// USB personality. Home disables it and returns through the shell callback.
bool usb_monitor_view_open(void (*home)(void), bool (*set_monitor)(bool));
bool usb_monitor_view_open_mode(void (*home)(void), bool (*set_monitor)(bool), bool native);
typedef enum { USB_MONITOR_MODE_RESTORED, USB_MONITOR_MODE_CHANGED, USB_MONITOR_MODE_STOPPED } usb_monitor_mode_result_t;
// LVGL owner. Only the foreground view opened with the matching USB callback can change.
bool usb_monitor_view_has_link(bool (*set_monitor)(bool));
bool usb_monitor_view_change_mode(bool native, bool (*set_profile)(bool native));
void usb_monitor_view_close(void);
bool usb_monitor_view_active(void);
#ifdef KVM_USB_MOTION_CHECK
void usb_monitor_view_merge_test_regions(bool merge);
void usb_monitor_view_test_wait_period(bool wait);
unsigned usb_monitor_view_test_poll(bool slow);
void usb_monitor_view_test_refresh_times(unsigned *wait, unsigned *draw);
void usb_monitor_test_pixel_path(bool scalar, bool rows_only, bool vector, bool cells);
bool usb_monitor_test_equal_blocks(void);
bool usb_monitor_test_has_cells(void);
void usb_monitor_test_decode_times(unsigned *jpeg_us, unsigned *black_us, unsigned *damage_us);
#endif
// Called on the LVGL owner with the current configured/suspended state of the monitor port.
void usb_monitor_view_set_link(bool mounted, bool suspended);
// With the LVGL lock held, classify the selected RGB565 image for a local UART request.
const char *usb_monitor_view_decoded_picture(void);
#ifdef KVM_USB_PICTURE_CHECK
typedef struct {
    const char *decoded, *reference;
    unsigned sequence;
} usb_monitor_picture_t;

// Engineering trial only: enable after opening the receiver and before supplying its first JPEG.
void usb_monitor_enable_picture_check(void);
// With the LVGL lock held, pair the selected RGB565 surface with its independent JPEG check.
usb_monitor_picture_t usb_monitor_view_picture_check(void);
#endif
