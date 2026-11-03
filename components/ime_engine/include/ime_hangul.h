// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Korean 2-set (dubeolsik) composer (ADR-024 FR-746, TECH-080): Hangul compatibility jamo in, precomposed
// syllables out, composed algorithmically (Unicode: S = 0xAC00 + (L*21 + V)*28 + T). No dictionary, no
// allocation, no HID and no LVGL: the caller owns one kvm_hangul_t and shows the preedit; committed code
// points go to the character queue that kbd_layouts turns into keystrokes (FR-748).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Keys of the syllable being composed: initial (1) + medial (2) + final (2) at most. */
#define KVM_HANGUL_MAX_KEYS 5

/** Composition state. Treat as opaque; only the functions below change it. */
typedef struct {
    int8_t cho;   // initial consonant index 0..18, -1 = none
    int8_t jung;  // medial vowel index 0..20, -1 = none
    int8_t jong;  // final consonant index 1..27, 0 = none
    uint8_t nkeys;
    uint32_t keys[KVM_HANGUL_MAX_KEYS]; // jamo typed into this syllable, replayed on backspace
} kvm_hangul_t;

void kvm_hangul_reset(kvm_hangul_t *h);

/** True when `cp` is a Hangul compatibility jamo the composer takes (U+3131..U+3163). */
bool kvm_hangul_is_jamo(uint32_t cp);

/**
 * Feed one compatibility jamo. Code points committed by this key (0, 1 or 2) are written to `out`;
 * the return value is their count. A code point that is not a jamo is refused: the composition is
 * left unchanged and 0 is returned (the caller flushes first, then sends the character itself).
 */
size_t kvm_hangul_input(kvm_hangul_t *h, uint32_t jamo, uint32_t out[2]);

/**
 * Remove the last jamo of the composition. Returns false when there was nothing to remove (the
 * caller then deletes committed text instead).
 */
bool kvm_hangul_backspace(kvm_hangul_t *h);

/** The code point shown as preedit: a syllable, a lone jamo, or 0 when empty. */
uint32_t kvm_hangul_preedit(const kvm_hangul_t *h);

/** Commit the preedit (if any) and reset. Returns the number of code points written (0 or 1). */
size_t kvm_hangul_flush(kvm_hangul_t *h, uint32_t out[1]);

/**
 * The jamo of the standard 2-set layout for a Latin key position ('a'..'z', as on a US QWERTY key
 * cap) with or without Shift. Returns 0 for any other character.
 */
uint32_t kvm_hangul_2set_jamo(char key, bool shift);

#ifdef __cplusplus
}
#endif
