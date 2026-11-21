// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "live_queue.h"
#include <string.h>

bool kvm_live_queue_init(kvm_live_queue_t *q, uint8_t *a, uint8_t *b, size_t capacity, uint32_t id,
                         kvm_live_codec_t codec)
{
    if (!q || !a || !b || a == b || !capacity || capacity > KVM_LIVE_MAX_PAYLOAD)
        return false;
    *q = (kvm_live_queue_t){.buffer = {a, b}, .capacity = capacity, .waiting = -1, .decoding = -1};
    return kvm_live_start(&q->receiver, id, codec);
}

bool kvm_live_queue_init_in_place(kvm_live_queue_t *q, uint8_t *wire, size_t capacity, uint32_t id,
                                  kvm_live_codec_t codec)
{
    if (!q || !wire || !capacity || capacity > KVM_LIVE_MAX_PAYLOAD)
        return false;
    *q = (kvm_live_queue_t){.buffer = {wire + KVM_LIVE_HEADER, NULL}, .capacity = capacity,
                            .waiting = -1, .decoding = -1};
    return kvm_live_start(&q->receiver, id, codec);
}

bool kvm_live_queue_writable(const kvm_live_queue_t *q)
{
    return q && q->receiver.stream_id && !q->buffer[1] && q->waiting < 0 && q->decoding < 0;
}

bool kvm_live_queue_push(kvm_live_queue_t *q, const uint8_t *message, size_t size)
{
    if (!q || !message || size <= KVM_LIVE_HEADER || size - KVM_LIVE_HEADER > q->capacity)
        return false;
    if (!q->buffer[1] && (!kvm_live_queue_writable(q) || message + KVM_LIVE_HEADER != q->buffer[0]))
        return false;
    const uint8_t *payload = kvm_live_receive(&q->receiver, message, size);
    if (!payload)
        return false;
    int slot = q->decoding == 0 ? 1 : 0;
    size_t len = size - KVM_LIVE_HEADER;
    if (q->buffer[1])
        memcpy(q->buffer[slot], payload, len);
    q->length[slot] = len;
    q->waiting = slot;
    return true;
}

const uint8_t *kvm_live_queue_take(kvm_live_queue_t *q, size_t *size)
{
    if (!q || !size || q->waiting < 0 || q->decoding >= 0 || !q->receiver.stream_id)
        return NULL;
    q->decoding = q->waiting;
    q->waiting = -1;
    *size = q->length[q->decoding];
    return q->buffer[q->decoding];
}

void kvm_live_queue_release(kvm_live_queue_t *q)
{
    if (q && q->decoding >= 0) {
        memset(q->buffer[q->decoding], 0, q->capacity);
        q->decoding = -1;
    }
}

void kvm_live_queue_stop(kvm_live_queue_t *q)
{
    if (!q)
        return;
    kvm_live_stop(&q->receiver);
    q->waiting = -1;
    if (!q->buffer[1])
        return; // A socket read may still be completing into the owner's wire buffer.
    for (int i = 0; i < 2; ++i)
        if (i != q->decoding && q->buffer[i])
            memset(q->buffer[i], 0, q->capacity);
}
