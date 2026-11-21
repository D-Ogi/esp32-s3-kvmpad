// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PACK_RECEIVE_BEGIN,
    PACK_RECEIVE_DATA,
    PACK_RECEIVE_END,
    PACK_RECEIVE_ABORT
} pack_receive_kind_t;

typedef struct {
    pack_receive_kind_t kind;
    uint32_t size;      // BEGIN, total compressed archive bytes, at most 256 KiB
    uint8_t sha256[32]; // BEGIN, recompute before acknowledging END
    char client[33];    // BEGIN, recorded pairing name, safe for plain-text display
    uint32_t offset;    // DATA, contiguous byte offset
    uint16_t len;       // DATA, 1..256
    uint8_t data[256];
} pack_receive_part_t;

// Physical foreground entry only. Zero means another receive or commit owns the
// slot.
uint32_t pack_receive_arm(void);
// Single SD worker. One outstanding part. Each non-ABORT part needs exactly one
// ack. Poll at about 5..10 ms while transferring; ABORT can also arrive during
// local review.
bool pack_receive_take(uint32_t epoch, pack_receive_part_t *out);
void pack_receive_ack(uint32_t epoch, bool ok);
// Query before drawing Review; atomically claim commit on the final physical
// approval tap.
bool pack_receive_review_active(uint32_t epoch);
bool pack_receive_begin_commit(uint32_t epoch);
// installed=true requires begin_commit and success of the storage transaction.
// installed=false during review reports "denied"; after begin_commit it reports
// "failed".
void pack_receive_finish(uint32_t epoch, bool installed);
// Close foreground window, abort pending bytes, revoke review. A claimed commit
// finishes locally.
void pack_receive_cancel(uint32_t epoch);
