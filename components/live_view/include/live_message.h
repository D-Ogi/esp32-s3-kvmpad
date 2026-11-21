// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    KVM_LIVE_JPEG = 1,
    KVM_LIVE_H264_INTRA,
    KVM_LIVE_CINEPAK_INTRA,
} kvm_live_codec_t;

enum { KVM_LIVE_HEADER = 32, KVM_LIVE_MAX_PAYLOAD = 96 * 1024 };

typedef struct {
    unsigned source_width, source_height;
    unsigned image_width, image_height;
} kvm_live_geometry_t;

// Requested physical pixels before clipping to the selected monitor. Unknown settings return zeroes.
kvm_live_geometry_t kvm_live_geometry(kvm_live_codec_t codec, const char *mode);

typedef struct {
    uint32_t stream_id;
    uint32_t sequence;
    kvm_live_codec_t codec;
    bool received;
} kvm_live_receiver_t;

// The owner serializes these operations. Start follows authorization and codec negotiation.
bool kvm_live_start(kvm_live_receiver_t *rx, uint32_t stream_id, kvm_live_codec_t codec);
void kvm_live_stop(kvm_live_receiver_t *rx);

// Accept one complete WSS binary message, including its CRC. The successful result borrows
// size - KVM_LIVE_HEADER encoded bytes from the immutable message until decoding returns.
// Each codec decoder validates its syntax and dimensions before presenting pixels.
const uint8_t *kvm_live_receive(kvm_live_receiver_t *rx, const uint8_t *message, size_t size);
