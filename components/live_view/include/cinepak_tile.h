// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { KVM_CVID_W = 960, KVM_CVID_H = 540, KVM_CVID_MAX_BYTES = 96 * 1024 };

typedef struct {
    uint16_t books[2][256][4];
    uint16_t rows[4 * KVM_CVID_W];
} kvm_cvid_workspace_t;

// Four rows of native RGB565 words. The sink consumes them before returning.
// Return false to cancel. Each in-flight decode needs its own caller-owned workspace.
typedef bool (*kvm_cvid_sink_t)(void *ctx, unsigned y, const uint16_t *pixels);

// One independent 960x540 Cinepak frame with a single intra strip. Input must remain
// immutable until return. Invalid packets never reach the sink. No heap allocation.
bool kvm_cvid_decode(kvm_cvid_workspace_t *work, const uint8_t *packet, size_t size,
                     kvm_cvid_sink_t sink, void *ctx);

// Validated immutable picture for clipped LVGL refreshes. It owns its indices
// and RGB565 codebooks; the transport packet may be released after preparation.
typedef struct {
    uint16_t books[2][256][4];
    bool coarse_only;
    union {
        uint8_t coarse[KVM_CVID_W * KVM_CVID_H / 16];
        struct {
            uint8_t fine[KVM_CVID_W * KVM_CVID_H / 16 / 8];
            uint8_t indices[KVM_CVID_W * KVM_CVID_H / 16][4];
        } mixed;
    };
} kvm_cvid_index_t;

bool kvm_cvid_index(kvm_cvid_index_t *index, const uint8_t *packet, size_t size);
// Render four adjacent rows into caller-owned RGB565 memory, stride 960 pixels.
bool kvm_cvid_index_band(const kvm_cvid_index_t *index, unsigned y, uint16_t *rows);
