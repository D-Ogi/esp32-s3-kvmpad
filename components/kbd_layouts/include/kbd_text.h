// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// One item of composed text -> the key steps of one burst (ADR-024 FR-747/748). A character the HOST layout
// types directly goes out as plain keystrokes; anything else through the selected Unicode delivery mode; a text
// key (Enter, Backspace, a digit typed from the composition keyboard ...) as itself. Until the TASK-071 layout
// tables exist the HOST layout is US (the shipped default of kbd.host_layout).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbd_unicode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_TEXT_CHAR = 0, // value = code point
    KVM_TEXT_KEY,      // value = usage | KVM_TEXT_KEY_SHIFT
} kvm_text_kind_t;

#define KVM_TEXT_KEY_SHIFT 0x0100u

typedef struct {
    uint8_t kind;   // kvm_text_kind_t
    uint32_t value;
} kvm_text_item_t;

/** The US keystroke that types printable ASCII `c` (0x20..0x7E); false for anything else. */
bool kvm_text_ascii_us(uint32_t c, kvm_keystroke_t *out);

/**
 * Steps for `item` in `mode`. Returns the count or a negative kvm_uni_err_t: KVM_UNI_ERR_MODE when the
 * character needs a Unicode sequence and the mode has none (host-ime: the caller counts it as skipped),
 * KVM_UNI_ERR_KEY for an invalid text key, KVM_UNI_ERR_CODEPOINT / _SPACE as kvm_uni_build.
 */
int kvm_text_item_steps(const kvm_text_item_t *item, kvm_uni_mode_t mode, kvm_uni_step_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
