// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Keyboard modifier state machine (ADR-016; FR-204, FR-205, FR-209). Pure and deterministic: it
// turns key/modifier taps into ledger operations that hid_core queues as LOCAL input. Runs only in
// the hid_dispatch context, together with the ledger it feeds.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVM_KBD_LOCK_WINDOW_US 300000u // FR-204: two taps within 300 ms lock a modifier
#define KVM_KBD_MAX_OPS 3

typedef enum {
    KVM_KBD_OP_KEY_DOWN = 0, // press `usage` together with `mods` in one report (FR-205 atomic one-shot)
    KVM_KBD_OP_KEY_UP,       // release `usage`
    KVM_KBD_OP_MODS_DOWN,    // press the modifier bits `mods`
    KVM_KBD_OP_MODS_UP,      // release the modifier bits `mods`
} kvm_kbd_op_kind_t;

typedef struct {
    kvm_kbd_op_kind_t kind;
    uint8_t mods;  // KVM_MOD_BIT mask
    uint8_t usage; // non-modifier usage for KEY_DOWN / KEY_UP
} kvm_kbd_op_t;

typedef struct {
    uint8_t armed;          // one-shot modifiers waiting for the next key (not on USB)
    uint8_t locked;         // locked modifiers (held on USB)
    uint8_t oneshot_mods;   // armed modifiers attached to the key currently held
    uint8_t oneshot_usage;  // that key (0 = none)
    uint64_t last_tap_us[8];
} kvm_kbd_fsm_t;

typedef enum { KVM_KBD_MOD_OFF = 0, KVM_KBD_MOD_ARMED, KVM_KBD_MOD_LOCKED } kvm_kbd_mod_state_t;

void kvm_kbd_init(kvm_kbd_fsm_t *f);

/** Tap on a modifier key (usage 0xE0..0xE7). Returns the number of ops written (0..KVM_KBD_MAX_OPS), -1 if not a modifier. */
int kvm_kbd_modifier_tap(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS]);

/** Ordinary key pressed (non-modifier usage). Consumes the armed modifiers. */
int kvm_kbd_key_down(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS]);

/** Ordinary key released. Releases the one-shot modifiers attached to it, after the key. */
int kvm_kbd_key_up(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS]);

/**
 * Return every modifier to OFF (shortcut start, FR-209). Writes the ops that release what is on USB
 * (locked and one-shot modifiers, the one-shot key). RELEASE_ALL uses kvm_kbd_init() instead: the
 * ledger is cleared there anyway.
 */
int kvm_kbd_cancel(kvm_kbd_fsm_t *f, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS]);

kvm_kbd_mod_state_t kvm_kbd_mod_state(const kvm_kbd_fsm_t *f, uint8_t usage);

static inline bool kvm_kbd_is_idle(const kvm_kbd_fsm_t *f)
{
    return f->armed == 0 && f->locked == 0 && f->oneshot_usage == 0;
}

#ifdef __cplusplus
}
#endif
