// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Keyboard modifier FSM (ADR-016; FR-204, FR-205, FR-209).
#include <string.h>

#include "hid_keymap.h"
#include "kbd_fsm.h"
#include "kvm_test.h"

static kvm_kbd_fsm_t f;
static kvm_kbd_op_t ops[KVM_KBD_MAX_OPS];

#define CTRL KVM_KEY_LEFT_CTRL
#define SHIFT KVM_KEY_LEFT_SHIFT
#define C_KEY (KVM_KEY_A + 2)

static void tap_arms_without_usb(void)
{
    kvm_kbd_init(&f);
    CHECK_EQ(kvm_kbd_modifier_tap(&f, CTRL, 1000, ops), 0); // FR-205: nothing on USB
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_ARMED);
    CHECK(!kvm_kbd_is_idle(&f));
}

static void one_shot_is_atomic_and_released_after_key(void)
{
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, CTRL, 1000, ops);
    int n = kvm_kbd_key_down(&f, C_KEY, 2000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].kind, KVM_KBD_OP_KEY_DOWN);
    CHECK_EQ(ops[0].mods, KVM_MOD_BIT(CTRL)); // same report as the key
    CHECK_EQ(ops[0].usage, C_KEY);
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_OFF); // consumed
    n = kvm_kbd_key_up(&f, C_KEY, 3000, ops);
    CHECK_EQ(n, 2);
    CHECK_EQ(ops[0].kind, KVM_KBD_OP_KEY_UP); // key first
    CHECK_EQ(ops[1].kind, KVM_KBD_OP_MODS_UP); // then its modifiers
    CHECK_EQ(ops[1].mods, KVM_MOD_BIT(CTRL));
    CHECK(kvm_kbd_is_idle(&f));
    n = kvm_kbd_key_down(&f, KVM_KEY_A, 4000, ops); // AT-KEY-05: the next key goes without Ctrl
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].mods, 0);
}

static void two_one_shots_combine(void)
{
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, CTRL, 1000, ops);
    kvm_kbd_modifier_tap(&f, SHIFT, 400000, ops); // different modifier: both ARMED
    int n = kvm_kbd_key_down(&f, KVM_KEY_ESCAPE, 500000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].mods, KVM_MOD_BIT(CTRL) | KVM_MOD_BIT(SHIFT));
}

static void double_tap_locks_single_tap_unlocks(void)
{
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, CTRL, 1000000, ops);
    int n = kvm_kbd_modifier_tap(&f, CTRL, 1000000 + KVM_KBD_LOCK_WINDOW_US, ops); // boundary: inside
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].kind, KVM_KBD_OP_MODS_DOWN); // locked is held on USB
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_LOCKED);
    // Locked modifier is not released by ordinary keys.
    n = kvm_kbd_key_down(&f, KVM_KEY_A, 2000000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].mods, 0); // already held, nothing extra
    n = kvm_kbd_key_up(&f, KVM_KEY_A, 2100000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_LOCKED);
    n = kvm_kbd_modifier_tap(&f, CTRL, 3000000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].kind, KVM_KBD_OP_MODS_UP);
    CHECK(kvm_kbd_is_idle(&f));
}

static void late_second_tap_disarms(void)
{
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, CTRL, 1000000, ops);
    int n = kvm_kbd_modifier_tap(&f, CTRL, 1000000 + KVM_KBD_LOCK_WINDOW_US + 1, ops);
    CHECK_EQ(n, 0);
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_OFF);
    CHECK(kvm_kbd_is_idle(&f));
}

static void cancel_releases_what_is_on_usb(void)
{
    // FR-209: a shortcut cancels the FSM; locked and one-shot (with its held key) are released.
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, SHIFT, 1000, ops);
    kvm_kbd_modifier_tap(&f, SHIFT, 2000, ops); // Shift LOCKED
    kvm_kbd_modifier_tap(&f, CTRL, 3000, ops);  // Ctrl ARMED
    kvm_kbd_key_down(&f, C_KEY, 4000, ops);     // C held with one-shot Ctrl
    int n = kvm_kbd_cancel(&f, ops);
    CHECK_EQ(n, 2);
    CHECK_EQ(ops[0].kind, KVM_KBD_OP_KEY_UP);
    CHECK_EQ(ops[0].usage, C_KEY);
    CHECK_EQ(ops[1].kind, KVM_KBD_OP_MODS_UP);
    CHECK_EQ(ops[1].mods, KVM_MOD_BIT(SHIFT) | KVM_MOD_BIT(CTRL));
    CHECK(kvm_kbd_is_idle(&f));
    CHECK_EQ(kvm_kbd_cancel(&f, ops), 0); // idempotent
}

static void second_key_while_one_shot_held(void)
{
    // A second key pressed while the one-shot key is held: modifiers are not re-applied or lost;
    // they are released with the key they were attached to.
    kvm_kbd_init(&f);
    kvm_kbd_modifier_tap(&f, CTRL, 1000, ops);
    kvm_kbd_key_down(&f, C_KEY, 2000, ops);
    int n = kvm_kbd_key_down(&f, KVM_KEY_A, 3000, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(ops[0].mods, 0);
    n = kvm_kbd_key_up(&f, KVM_KEY_A, 4000, ops);
    CHECK_EQ(n, 1); // A only
    n = kvm_kbd_key_up(&f, C_KEY, 5000, ops);
    CHECK_EQ(n, 2);
    CHECK_EQ(ops[1].mods, KVM_MOD_BIT(CTRL));
    CHECK(kvm_kbd_is_idle(&f));
}

static void rejects_wrong_kinds(void)
{
    kvm_kbd_init(&f);
    CHECK_EQ(kvm_kbd_modifier_tap(&f, KVM_KEY_A, 0, ops), -1);
    CHECK_EQ(kvm_kbd_key_down(&f, CTRL, 0, ops), -1);
    CHECK_EQ(kvm_kbd_key_up(&f, CTRL, 0, ops), -1);
    CHECK_EQ(kvm_kbd_key_down(&f, 0, 0, ops), -1);
    CHECK_EQ(kvm_kbd_mod_state(&f, KVM_KEY_A), KVM_KBD_MOD_OFF);
}

static void timestamps_near_wrap(void)
{
    // Monotonic 64-bit time: the subtraction stays correct close to large values.
    kvm_kbd_init(&f);
    const uint64_t t = UINT64_MAX - 100000u;
    kvm_kbd_modifier_tap(&f, CTRL, t, ops);
    int n = kvm_kbd_modifier_tap(&f, CTRL, t + 50000u, ops);
    CHECK_EQ(n, 1);
    CHECK_EQ(kvm_kbd_mod_state(&f, CTRL), KVM_KBD_MOD_LOCKED);
}

int main(void)
{
    printf("test_kbd_fsm\n");
    RUN(tap_arms_without_usb);
    RUN(one_shot_is_atomic_and_released_after_key);
    RUN(two_one_shots_combine);
    RUN(double_tap_locks_single_tap_unlocks);
    RUN(late_second_tap_disarms);
    RUN(cancel_releases_what_is_on_usb);
    RUN(second_key_while_one_shot_held);
    RUN(rejects_wrong_kinds);
    RUN(timestamps_near_wrap);
    TEST_MAIN_END("test_kbd_fsm");
}
