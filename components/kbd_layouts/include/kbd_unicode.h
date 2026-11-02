// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Unicode delivery sequences (ADR-024 FR-747): one code point -> the key DOWN/UP steps that make the
// HOST's own input method produce it. Pure data transformation: this file never sends anything. The
// steps become typed KEY_DOWN / KEY_UP intents of one cancellable burst (FR-748, InputArbiter ->
// HidScheduler); on cancel the caller sends kvm_uni_cancel() steps and then RELEASE_ALL.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** `Settings -> Keyboard -> Unicode input on HOST` (FR-747). */
typedef enum {
    KVM_UNI_LINUX = 0,   // Ctrl+Shift+U, hex, Space (GTK / IBus)
    KVM_UNI_MACOS,       // Option held, 4 hex digits per UTF-16 unit ("Unicode Hex Input" source)
    KVM_UNI_WINDOWS_HEX, // Alt held, Numpad +, hex, Alt released (EnableHexNumpad=1); one sequence per UTF-16 unit
    KVM_UNI_WINCOMPOSE,  // Right Alt (compose), u, hex, Enter
    KVM_UNI_HOST_IME,    // readings typed as plain keys; no Unicode sequence exists in this mode
    KVM_UNI_MODE_COUNT,
} kvm_uni_mode_t;

/** One step: a page 0x07 usage pressed or released (modifiers as 0xE0..0xE7). */
typedef struct {
    uint8_t usage;
    bool down;
} kvm_uni_step_t;

/** How the HOST layout types one character: a usage plus the modifier bitmap (03 §5.2 bit order). */
typedef struct {
    uint8_t usage;
    uint8_t mods;
} kvm_keystroke_t;

/**
 * Resolves a character ('0'..'9', 'a'..'f', 'u') to a keystroke on the HOST layout (from the
 * kbd_layouts reverse index). Returns false when the layout cannot type it. NULL selects the US
 * positions. Not used in KVM_UNI_MACOS: the "Unicode Hex Input" source is itself US-positioned.
 */
typedef bool (*kvm_uni_key_fn)(void *ctx, char c, kvm_keystroke_t *out);

typedef enum {
    KVM_UNI_ERR_CODEPOINT = -1, // 0, a surrogate, or above U+10FFFF
    KVM_UNI_ERR_MODE = -2,      // unknown mode, or KVM_UNI_HOST_IME
    KVM_UNI_ERR_KEY = -3,       // the resolver cannot type a needed character
    KVM_UNI_ERR_SPACE = -4,     // `cap` too small
} kvm_uni_err_t;

/** Enough for any code point in any mode with a resolver that adds up to two modifiers per digit. */
#define KVM_UNI_MAX_STEPS 160

/**
 * Build the steps for `cp`. Every DOWN in the result has its UP later in the result, in reverse
 * order of nesting, so a complete sequence leaves no key held. Returns the step count or a negative
 * kvm_uni_err_t; on error nothing useful is in `out`.
 */
int kvm_uni_build(kvm_uni_mode_t mode, uint32_t cp, kvm_uni_key_fn key, void *ctx, kvm_uni_step_t *out,
                  size_t cap);

/**
 * Steps that abort a partially typed sequence in `mode` (FR-748: Linux and WinCompose: Esc), to be
 * sent before RELEASE_ALL. Returns the count (0 when the mode has no cancel key) or KVM_UNI_ERR_*.
 */
int kvm_uni_cancel(kvm_uni_mode_t mode, kvm_uni_step_t *out, size_t cap);

/** The US-position resolver used when `key` is NULL (exposed for tests and the default layout). */
bool kvm_uni_key_us(void *ctx, char c, kvm_keystroke_t *out);

#ifdef __cplusplus
}
#endif
