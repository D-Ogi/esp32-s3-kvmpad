// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "media_decode.h"
#include <stddef.h>
#include <stdint.h>

typedef enum {
    USB_JPEG_ABSENT = 0,
    USB_JPEG_ALL_BLACK,
    USB_JPEG_HAS_NONBLACK,
    USB_JPEG_FAILED,
} usb_jpeg_result_t;

// Classify a complete USB monitor JPEG at RGB565 precision. The caller owns the
// input and keeps it stable during this synchronous, task-context operation.
usb_jpeg_result_t usb_jpeg_check(const uint8_t *jpeg, size_t length);
// Independent mask-ROM classification for the accelerated display decoder.
usb_jpeg_result_t usb_jpeg_check_rom(const uint8_t *jpeg, size_t length, const kvm_media_target_t *target);
