// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Keyboard modifier state machine (ADR-016).
#include "kbd_fsm.h"

#include <string.h>

#include "hid_keymap.h"

void kvm_kbd_init(kvm_kbd_fsm_t *f)
{
    memset(f, 0, sizeof(*f));
}

static int emit(kvm_kbd_op_t ops[KVM_KBD_MAX_OPS], int n, kvm_kbd_op_kind_t kind, uint8_t mods, uint8_t usage)
{
    if (n < KVM_KBD_MAX_OPS) {
        ops[n].kind = kind;
        ops[n].mods = mods;
        ops[n].usage = usage;
        n++;
    }
    return n;
}

int kvm_kbd_modifier_tap(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS])
{
    if (!kvm_usage_is_modifier(usage)) {
        return -1;
    }
    const uint8_t bit = KVM_MOD_BIT(usage);
    const unsigned idx = (unsigned)(usage - KVM_KEY_LEFT_CTRL);
    int n = 0;
    if (f->locked & bit) {
        // LOCKED -> OFF: a single tap unlocks (FR-204).
        f->locked = (uint8_t)(f->locked & ~bit);
        n = emit(ops, n, KVM_KBD_OP_MODS_UP, bit, 0);
    } else if (f->armed & bit) {
        f->armed = (uint8_t)(f->armed & ~bit);
        if (now_us - f->last_tap_us[idx] <= KVM_KBD_LOCK_WINDOW_US) {
            // ARMED -> LOCKED: the second tap within the window; held on USB from now on (FR-302).
            f->locked = (uint8_t)(f->locked | bit);
            n = emit(ops, n, KVM_KBD_OP_MODS_DOWN, bit, 0);
        }
        // else ARMED -> OFF: a late second tap toggles the one-shot away; nothing was on USB.
    } else {
        // OFF -> ARMED: nothing goes to USB until the next ordinary key (FR-205).
        f->armed = (uint8_t)(f->armed | bit);
        f->last_tap_us[idx] = now_us;
    }
    return n;
}

int kvm_kbd_key_down(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS])
{
    (void)now_us;
    if (kvm_usage_is_modifier(usage) || usage == 0) {
        return -1;
    }
    int n = 0;
    uint8_t mods = (uint8_t)(f->armed & ~f->locked);
    f->armed = 0;
    if (mods != 0) {
        // If an earlier one-shot key is still held, its modifiers move to this key and are released
        // with it (at the latest): never left behind.
        f->oneshot_mods = (uint8_t)(f->oneshot_mods | mods);
        f->oneshot_usage = usage;
    }
    n = emit(ops, n, KVM_KBD_OP_KEY_DOWN, mods, usage);
    return n;
}

int kvm_kbd_key_up(kvm_kbd_fsm_t *f, uint8_t usage, uint64_t now_us, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS])
{
    (void)now_us;
    if (kvm_usage_is_modifier(usage) || usage == 0) {
        return -1;
    }
    int n = emit(ops, 0, KVM_KBD_OP_KEY_UP, 0, usage);
    if (f->oneshot_usage == usage) {
        // Key first, then its one-shot modifiers (natural release order); locked bits stay held.
        uint8_t mods = (uint8_t)(f->oneshot_mods & ~f->locked);
        if (mods != 0) {
            n = emit(ops, n, KVM_KBD_OP_MODS_UP, mods, 0);
        }
        f->oneshot_mods = 0;
        f->oneshot_usage = 0;
    }
    return n;
}

int kvm_kbd_cancel(kvm_kbd_fsm_t *f, kvm_kbd_op_t ops[KVM_KBD_MAX_OPS])
{
    int n = 0;
    if (f->oneshot_usage != 0) {
        n = emit(ops, n, KVM_KBD_OP_KEY_UP, 0, f->oneshot_usage);
    }
    uint8_t mods = (uint8_t)(f->locked | f->oneshot_mods);
    if (mods != 0) {
        n = emit(ops, n, KVM_KBD_OP_MODS_UP, mods, 0);
    }
    kvm_kbd_init(f);
    return n;
}

kvm_kbd_mod_state_t kvm_kbd_mod_state(const kvm_kbd_fsm_t *f, uint8_t usage)
{
    if (!kvm_usage_is_modifier(usage)) {
        return KVM_KBD_MOD_OFF;
    }
    uint8_t bit = KVM_MOD_BIT(usage);
    if (f->locked & bit) {
        return KVM_KBD_MOD_LOCKED;
    }
    if (f->armed & bit) {
        return KVM_KBD_MOD_ARMED;
    }
    return KVM_KBD_MOD_OFF;
}
