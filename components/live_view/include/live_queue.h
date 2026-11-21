// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "live_message.h"

// Owner serializes these short operations. Decode a borrowed frame outside the lock.
// Two caller-owned, nonoverlapping buffers must stay alive until the worker releases
// its frame. Stop invalidates pending work immediately. Reuse requires worker release.
typedef struct {
    kvm_live_receiver_t receiver;
    uint8_t *buffer[2];
    size_t capacity, length[2];
    int waiting, decoding;
} kvm_live_queue_t;

bool kvm_live_queue_init(kvm_live_queue_t *q, uint8_t *a, uint8_t *b, size_t capacity, uint32_t id,
                         kvm_live_codec_t codec);
// Paced producer writes a complete media packet only while writable. Its owner
// retains and finally wipes the wire buffer after both producer and decoder exit.
bool kvm_live_queue_init_in_place(kvm_live_queue_t *q, uint8_t *wire, size_t capacity, uint32_t id,
                                  kvm_live_codec_t codec);
bool kvm_live_queue_writable(const kvm_live_queue_t *q);
bool kvm_live_queue_push(kvm_live_queue_t *q, const uint8_t *message, size_t size);
const uint8_t *kvm_live_queue_take(kvm_live_queue_t *q, size_t *size);
void kvm_live_queue_release(kvm_live_queue_t *q);
void kvm_live_queue_stop(kvm_live_queue_t *q);
