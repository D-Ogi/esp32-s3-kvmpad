// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// text_glue (ADR-024 FR-747/748): the ordered queue of text composed on the DEVICE. Each item becomes one
// cancellable burst (kbd_text -> hid_dispatch_burst_start); the next item starts only when the previous burst
// is done. A burst ended by anything but completion (USB loss, owner change, cancel) drops the rest of the
// queue: composed text is never replayed later. Producers: the UI domain sink (UI_ACT_TEXT_*), any task.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t queued;
    uint32_t sent;          // bursts that completed
    uint32_t skipped;       // characters no delivery mode can carry (host-ime) or invalid items
    uint32_t dropped;       // queue full, or dropped by a cancel / abort / HOST loss
    uint32_t refused;       // a burst the core did not start after the retries
    uint32_t cancels;
} text_glue_stats_t;

esp_err_t text_glue_start(void);

/** Queue one character (code point) or one text key (usage | 0x100 for Shift). False when the queue is full. */
bool text_glue_push_char(uint32_t cp);
bool text_glue_push_key(uint16_t usage_and_shift);

/** Drop the queue, cancel the running burst (release, cancel key, RELEASE_ALL) or release at once. */
void text_glue_cancel(kvm_release_reason_t reason);

/** The FR-747 delivery mode, as stored (kvm_unicode_mode_t, config_store). */
void text_glue_set_mode(uint8_t unicode_mode);

void text_glue_get_stats(text_glue_stats_t *out);

#ifdef __cplusplus
}
#endif
