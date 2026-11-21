// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Position 1 has a white 16x16 marker at (32,32), position 2 at (480,288).
// RGB565 probes (40,40) and (488,296) are 0xffff inside and 0x0000 outside;
// (0,0) is always background. Native selects 1024x600, otherwise 640x480.
size_t usb_motion_jpeg_size(bool native, unsigned position);
// Sends one 16-byte USB JPEG header and its payload in chunks of at most 64 bytes.
// For a position other than 1 or 2, the size is zero and no bytes are sent.
void usb_motion_feed(bool native, unsigned position, void (*feed)(const uint8_t *, size_t));
