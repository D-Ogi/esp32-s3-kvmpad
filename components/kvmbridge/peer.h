// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "kvm_srt_identity.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t port, length;
    char address[16];
    uint8_t leaf[1024];
} kvb_peer_t;
bool kvb_peer_load(kvb_peer_t *out);
bool kvb_identity(kvm_srt_identity_t *out,bool need_private);
