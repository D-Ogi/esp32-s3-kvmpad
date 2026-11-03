// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-050 core: ledger, scheduler and chords (AT-HID-03..08, AT-KEY-04/05/10, AT-FAULT-05,
// AT-EXT-05/09, TECH-035, INV-005). Arbiter vectors VEC-005..009 are in test_hid_vectors.c.
#include <string.h>

#include "fake_hid_host.h"
#include "kvm_test.h"

static kvm_hid_core_t core;
static fake_host_t host;
static uint64_t now;

static int count_itf(int itf)
{
    int n = 0;
    for (int i = 0; i < host.n; i++) {
        n += host.log[i].itf == itf;
    }
    return n;
}

static void mount_sends_neutral_on_every_interface(void)
{
    fh_init(&host);
    kvm_hid_port_t port = fh_port(&host);
    kvm_hid_core_init(&core, &port, NULL);
    now = 1000;
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 1, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 3); // TECH-035: keyboard, mouse, consumer neutral, nothing else
    CHECK_EQ(count_itf(KVM_HID_ITF_KEYBOARD), 1);
    CHECK_EQ(count_itf(KVM_HID_ITF_MOUSE), 1);
    CHECK_EQ(count_itf(KVM_HID_ITF_CONSUMER), 1);
    for (int i = 0; i < host.n; i++) {
        for (int k = 0; k < host.log[i].len; k++) {
            CHECK_EQ(host.log[i].b[k], 0);
        }
    }
}

static void offline_rejects_everything(void)
{
    fh_init(&host);
    kvm_hid_port_t port = fh_port(&host);
    kvm_hid_core_init(&core, &port, NULL);
    now = 1000;
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_ERR_HOST_OFFLINE); // INV-005
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_ENUMERATING, 0, now);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_ERR_HOST_OFFLINE);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, 0, now);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_ERR_USB_SUSPENDED);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_NONE); // no ownership change without a HOST
}

static void key_tap_is_two_reports(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_intent_t u = fh_key(KVM_HID_KEY_UP, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_local(&core, &d, now), KVM_HID_OK);
    CHECK_EQ(kvm_hid_core_local(&core, &u, now), KVM_HID_OK); // both queued before any poll
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 2); // FR-203: a delivered DOWN before the UP, never collapsed
    CHECK_EQ(host.log[0].b[2], KVM_KEY_A);
    CHECK_EQ(host.log[1].b[2], 0);
    CHECK_EQ(host.key_presses[KVM_KEY_A], 1);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(kvm_hid_core_owner(&core)->kind, OWNER_LOCAL);
}

static void idempotent_down_and_orphan_up(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_intent_t u = fh_key(KVM_HID_KEY_UP, KVM_KEY_A);
    kvm_hid_intent_t ub = fh_key(KVM_HID_KEY_UP, KVM_KEY_A + 1);
    kvm_hid_core_local(&core, &d, now);
    kvm_hid_core_local(&core, &d, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 1); // 03 §3 rule 5: the second DOWN is a no-op
    kvm_hid_core_local(&core, &ub, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 1); // UP without DOWN: no report
    CHECK_EQ(kvm_hid_core_counters(&core)->keyup_without_down, 1);
    kvm_hid_core_local(&core, &u, now);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
}

static void invalid_usages_rejected(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t bad = fh_key(KVM_HID_KEY_DOWN, 0x01); // ErrorRollOver is not a key
    CHECK_EQ(kvm_hid_core_local(&core, &bad, now), KVM_HID_ERR_USAGE);
    bad.data.key.usage_id = 0xB0;
    CHECK_EQ(kvm_hid_core_local(&core, &bad, now), KVM_HID_ERR_USAGE);
    kvm_hid_intent_t b = fh_btn(KVM_HID_MOUSE_BTN_DOWN, 4);
    CHECK_EQ(kvm_hid_core_local(&core, &b, now), KVM_HID_ERR_USAGE);
    kvm_hid_intent_t c = fh_chord(999);
    CHECK_EQ(kvm_hid_core_local(&core, &c, now), KVM_HID_ERR_USAGE);
    kvm_hid_intent_t cc = fh_consumer(KVM_HID_CONSUMER_DOWN, 0x0183); // AL Consumer Control Config: not allowlisted
    CHECK_EQ(kvm_hid_core_local(&core, &cc, now), KVM_HID_ERR_USAGE);
    kvm_hid_intent_t ka;
    memset(&ka, 0, sizeof(ka));
    ka.type = KVM_HID_KEEPALIVE;
    CHECK_EQ(kvm_hid_core_local(&core, &ka, now), KVM_HID_ERR_BAD_INTENT); // SYSTEM only (INV-007)
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
}

static void six_key_rollover(void)
{
    // AT-HID-03: never more than 6 non-modifier usages; the 7th is rejected gracefully.
    fh_setup(&core, &host, &now);
    for (int i = 0; i < 7; i++) {
        kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, (uint8_t)(KVM_KEY_A + i));
        kvm_hid_core_local(&core, &d, now);
    }
    kvm_hid_intent_t sh = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_LEFT_SHIFT);
    kvm_hid_core_local(&core, &sh, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_counters(&core)->rollover_rejected, 1);
    CHECK_EQ(host.mods, KVM_MOD_BIT(KVM_KEY_LEFT_SHIFT)); // modifiers are outside the 6 slots
    int held = 0;
    for (int i = 0; i < 6; i++) {
        held += host.keys[i] != 0;
    }
    CHECK_EQ(held, 6);
    CHECK(!fh_key_in(host.keys, (uint8_t)(KVM_KEY_A + 6)));
    kvm_hid_core_release_all(&core, KVM_RA_REQUEST);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
}

static void modifier_bits(void)
{
    // AT-KEY-04: every modifier usage maps to its own bit, AltGr = Right Alt (0xE6 -> bit 6).
    fh_setup(&core, &host, &now);
    for (uint8_t u = KVM_KEY_LEFT_CTRL; u <= KVM_KEY_RIGHT_GUI; u++) {
        kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, u);
        kvm_hid_intent_t up = fh_key(KVM_HID_KEY_UP, u);
        host.n = 0;
        kvm_hid_core_local(&core, &d, now);
        fh_pump(&core, &host, &now);
        CHECK_EQ(host.n, 1);
        CHECK_EQ(host.log[0].b[0], (uint8_t)(1u << (u - KVM_KEY_LEFT_CTRL)));
        CHECK_EQ(host.log[0].b[2], 0);
        kvm_hid_core_local(&core, &up, now);
        fh_pump(&core, &host, &now);
        CHECK(fh_host_neutral(&host));
    }
    CHECK_EQ(KVM_MOD_BIT(KVM_KEY_RIGHT_ALT), 0x40);
}

static void split_motion_preserves_sum(void)
{
    // AT-HID-04: int16 deltas split into int8 reports, the sum is exact, no int8 overflow.
    fh_setup(&core, &host, &now);
    const int16_t dx[] = {300, -200, 32767, -32768, 1, 0, -1, 127, -128};
    const int16_t dy[] = {-200, 999, -32768, 32767, 0, 1, -1, -128, 127};
    long ex = 0, ey = 0;
    for (unsigned i = 0; i < sizeof(dx) / sizeof(dx[0]); i++) {
        kvm_hid_intent_t m = fh_move(dx[i], dy[i]);
        CHECK_EQ(kvm_hid_core_local(&core, &m, now), KVM_HID_OK);
        ex += dx[i];
        ey += dy[i];
        fh_pump(&core, &host, &now); // pumped separately: no coalescing beyond the cap
    }
    CHECK_EQ(host.sum_dx, ex);
    CHECK_EQ(host.sum_dy, ey);
    CHECK(host.max_abs_delta <= 127);
    CHECK_EQ(kvm_hid_core_counters(&core)->moves_dropped, 0);
}

static void coalesced_motion_preserves_sum(void)
{
    fh_setup(&core, &host, &now);
    long ex = 0;
    for (int i = 0; i < 300; i++) {
        kvm_hid_intent_t m = fh_move(7, -3);
        kvm_hid_core_local(&core, &m, now);
        ex += 7;
    }
    CHECK_EQ(core.q_count, 1); // one coalesced entry (02 §11.2)
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.sum_dx, ex);
    CHECK_EQ(host.sum_dy, -900);
}

static void drag_keeps_button_mask(void)
{
    // 03 §5.3: a change of X/Y sends the same button mask; drag = DOWN, moves with bit 0, UP.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT);
    kvm_hid_intent_t m = fh_move(40, 5);
    kvm_hid_intent_t u = fh_btn(KVM_HID_MOUSE_BTN_UP, KVM_MOUSE_BTN_LEFT);
    kvm_hid_core_local(&core, &d, now);
    kvm_hid_core_local(&core, &m, now);
    kvm_hid_core_local(&core, &u, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 3);
    CHECK_EQ(host.log[0].b[0], 0x01);
    CHECK_EQ(host.log[1].b[0], 0x01);
    CHECK_EQ((int8_t)host.log[1].b[1], 40);
    CHECK_EQ(host.log[2].b[0], 0x00);
    CHECK_EQ(host.button_presses[0], 1);
}

static void wheel_report_and_boot_fallback(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t w = fh_wheel(-3, 300);
    kvm_hid_core_local(&core, &w, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.sum_wheel, 300);
    CHECK_EQ(host.sum_pan, -3);
    // Boot protocol: no wheel field exists; nothing is synthesized (03 §3 rule 4) and it is counted.
    host.protocol[KVM_HID_ITF_MOUSE] = KVM_HID_PROTOCOL_BOOT;
    host.n = 0;
    kvm_hid_core_local(&core, &w, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    CHECK_EQ(kvm_hid_core_counters(&core)->wheel_dropped_boot, 1);
    // Motion still works in boot protocol with 3-byte reports.
    kvm_hid_intent_t m = fh_move(5, 5);
    kvm_hid_core_local(&core, &m, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 1);
    CHECK_EQ(host.log[0].len, 3);
}

static void check_chord_reports(uint16_t id, uint8_t mods, uint8_t key)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t c = fh_chord(id);
    CHECK_EQ(kvm_hid_core_local(&core, &c, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    int expect = mods ? 4 : 2;
    CHECK_EQ(host.n, expect);
    int i = 0;
    if (mods) {
        CHECK_EQ(host.log[i].b[0], mods); // 1. modifiers
        CHECK_EQ(host.log[i].b[2], 0);
        i++;
    }
    CHECK_EQ(host.log[i].b[0], mods); // 2. modifiers + key
    CHECK_EQ(host.log[i].b[2], key);
    i++;
    if (mods) {
        CHECK_EQ(host.log[i].b[0], mods); // 3. key up
        CHECK_EQ(host.log[i].b[2], 0);
        i++;
    }
    CHECK_EQ(host.log[i].b[0], 0); // 4. neutral
    CHECK_EQ(host.log[i].b[2], 0);
    CHECK_EQ(host.key_presses[key], 1);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_CHORD_DELIVERED), 1);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
    CHECK(!s.chord_active);
}

static void shortcut_chords(void)
{
    // AT-HID-05 Ctrl+Alt+Del: exact 4 phases + neutral; AT-HID-06 Alt+Tab, Ctrl+Shift+Esc, Win+D.
    check_chord_reports(KVM_CHORD_CTRL_ALT_DEL, KVM_MOD_BIT(KVM_KEY_LEFT_CTRL) | KVM_MOD_BIT(KVM_KEY_LEFT_ALT),
                        KVM_KEY_DELETE);
    check_chord_reports(KVM_CHORD_ALT_TAB, KVM_MOD_BIT(KVM_KEY_LEFT_ALT), KVM_KEY_TAB);
    check_chord_reports(KVM_CHORD_TASK_MANAGER, KVM_MOD_BIT(KVM_KEY_LEFT_CTRL) | KVM_MOD_BIT(KVM_KEY_LEFT_SHIFT),
                        KVM_KEY_ESCAPE);
    check_chord_reports(KVM_CHORD_SHOW_DESKTOP, KVM_MOD_BIT(KVM_KEY_LEFT_GUI), 0x07 /* D */);
    check_chord_reports(KVM_CHORD_CLOSE_WINDOW, KVM_MOD_BIT(KVM_KEY_LEFT_ALT), KVM_KEY_F4);
    check_chord_reports(KVM_CHORD_SCREENSHOT, 0, KVM_KEY_PRINT_SCREEN);
    check_chord_reports(KVM_CHORD_LAYOUT_WIN, KVM_MOD_BIT(KVM_KEY_LEFT_GUI), KVM_KEY_SPACE);
}

static void chord_delivered_only_after_host_read(void)
{
    // AT-HID-07: "delivered" means every phase was read from the endpoint; with the HOST not reading,
    // no confirmation, and the completion timeout ends in a release without repeating the chord.
    fh_setup(&core, &host, &now);
    host.no_complete = true;
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_CLOSE_WINDOW);
    kvm_hid_core_local(&core, &c, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 1); // phase 1 handed over, never read
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_CHORD_DELIVERED), 0);
    now += 300000;
    kvm_hid_core_poll(&core, now);
    CHECK_EQ(kvm_hid_core_counters(&core)->completion_timeouts, 1);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_CHORD_ABORTED), 1);
    host.no_complete = false;
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[KVM_KEY_F4], 0); // the key phase was never sent: no duplicate later
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
}

static void chord_interrupted_by_suspend(void)
{
    // AT-HID-08: interrupted chord -> ledger zero, neutral after (re)mount, nothing replayed.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_CTRL_ALT_DEL);
    kvm_hid_core_local(&core, &c, now);
    now += 1000;
    kvm_hid_core_poll(&core, now); // phase 1 in flight
    kvm_hid_core_on_report_complete(&core, KVM_HID_ITF_KEYBOARD);
    now += 1000;
    kvm_hid_core_poll(&core, now); // phase 2 (Ctrl+Alt+Del) in flight
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, 1, now);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK(s.ledger_empty);
    CHECK(!s.chord_active);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_CHORD_ABORTED), 1);
    int sent_before_resume = host.n;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, sent_before_resume); // nothing while suspended
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 1, now);
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[KVM_KEY_DELETE], 1); // once, never replayed
}

static void consumer_media_and_release(void)
{
    // AT-EXT-05 / TECH-074: Media action = DOWN -> UP; loss during it -> empty Consumer report.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_MEDIA_VOLUME_UP);
    kvm_hid_core_local(&core, &c, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 2);
    CHECK_EQ(host.log[0].itf, KVM_HID_ITF_CONSUMER);
    CHECK_EQ(host.log[0].b[0], 0xE9);
    CHECK_EQ(host.log[1].b[0], 0);
    CHECK_EQ(host.consumer_presses, 1);
    // Held Consumer usage (local hold), then RELEASE_ALL.
    kvm_hid_intent_t d = fh_consumer(KVM_HID_CONSUMER_DOWN, KVM_CONSUMER_MUTE);
    kvm_hid_core_local(&core, &d, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.consumer, KVM_CONSUMER_MUTE);
    kvm_hid_core_release_all(&core, KVM_RA_REQUEST);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.consumer, 0);
}

static void busy_endpoint_retries_without_duplicates(void)
{
    // AT-FAULT-05: retry only at phase granularity; a shortcut never duplicates.
    fh_setup(&core, &host, &now);
    host.refuse[KVM_HID_ITF_KEYBOARD] = true;
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_ALT_TAB);
    kvm_hid_core_local(&core, &c, now);
    for (int i = 0; i < 20; i++) {
        now += 1000;
        kvm_hid_core_poll(&core, now);
    }
    CHECK_EQ(host.n, 0);
    CHECK(kvm_hid_core_counters(&core)->send_refused >= 20);
    host.refuse[KVM_HID_ITF_KEYBOARD] = false;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 4);
    CHECK_EQ(host.key_presses[KVM_KEY_TAB], 1);
    CHECK(fh_host_neutral(&host));
}

static void typing_after_reenumeration(void)
{
    // AT-KEY-10: a neutral start after a new mount, without old modifier bits.
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_LEFT_CTRL);
    kvm_hid_core_local(&core, &d, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.mods, 0x01);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, now);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 2, now);
    host.n = 0;
    fh_init(&host); // a new HOST session: its model starts empty
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 3); // neutral on the three interfaces only
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_core_local(&core, &a, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.log[host.n - 1].b[0], 0); // no Ctrl bit
    CHECK_EQ(host.log[host.n - 1].b[2], KVM_KEY_A);
}

static void kbd_one_shot_through_core(void)
{
    // AT-KEY-05: tap Ctrl -> C gives Ctrl+C once; the next key goes without Ctrl.
    fh_setup(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_ui_modifier_tap(&core, KVM_KEY_LEFT_CTRL, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0); // ARMED is not on USB (FR-205)
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK_EQ(s.mods_armed, 0x01);
    const uint8_t C = KVM_KEY_A + 2;
    kvm_hid_core_ui_key_down(&core, C, now);
    kvm_hid_core_ui_key_up(&core, C, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 3);
    CHECK_EQ(host.log[0].b[0], 0x01); // Ctrl and C in one report
    CHECK_EQ(host.log[0].b[2], C);
    CHECK_EQ(host.log[1].b[0], 0x01); // C up first
    CHECK_EQ(host.log[1].b[2], 0);
    CHECK_EQ(host.log[2].b[0], 0x00); // then Ctrl up
    host.n = 0;
    kvm_hid_core_ui_key_down(&core, KVM_KEY_A, now);
    kvm_hid_core_ui_key_up(&core, KVM_KEY_A, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 2);
    CHECK_EQ(host.log[0].b[0], 0x00);
    CHECK_EQ(host.key_presses[KVM_KEY_LEFT_CTRL], 1);
}

static void locked_modifier_not_inherited_by_shortcut(void)
{
    // FR-209 / ADR-016: a locked Ctrl is released before the chord and not restored afterwards.
    fh_setup(&core, &host, &now);
    kvm_hid_core_ui_modifier_tap(&core, KVM_KEY_LEFT_CTRL, now);
    kvm_hid_core_ui_modifier_tap(&core, KVM_KEY_LEFT_CTRL, now + 100000); // within 300 ms -> LOCKED
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.mods, 0x01);
    kvm_hid_snapshot_t s;
    kvm_hid_core_snapshot(&core, &s);
    CHECK_EQ(s.mods_locked, 0x01);
    host.n = 0;
    kvm_hid_intent_t c = fh_chord(KVM_CHORD_ALT_TAB);
    kvm_hid_core_local(&core, &c, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 5); // neutral (Ctrl released), then the 4 Alt+Tab phases
    CHECK_EQ(host.log[0].b[0], 0x00);
    for (int i = 1; i < host.n; i++) {
        CHECK(!(host.log[i].b[0] & 0x01)); // Ctrl never part of the chord
    }
    kvm_hid_core_snapshot(&core, &s);
    CHECK_EQ(s.mods_locked, 0);
    CHECK(fh_host_neutral(&host));
}

static void keepalive_rules(void)
{
    // AT-EXT-09 (ADR-020): 0-px reports only when enabled, mounted, idle and with an empty ledger.
    fh_setup(&core, &host, &now);
    now += 400000000; // 400 s with keep-alive off: nothing
    kvm_hid_core_poll(&core, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    kvm_hid_core_set_keepalive(&core, true, 30);
    now += 29000000;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    now += 2000000;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 1);
    CHECK_EQ(host.log[0].itf, KVM_HID_ITF_MOUSE);
    for (int k = 0; k < host.log[0].len; k++) {
        CHECK_EQ(host.log[0].b[k], 0); // dx = dy = 0, no buttons (FR-714)
    }
    // A held key blocks it.
    kvm_hid_intent_t d = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    kvm_hid_core_local(&core, &d, now);
    fh_pump(&core, &host, &now);
    host.n = 0;
    uint32_t sent = kvm_hid_core_counters(&core)->keepalive_sent;
    now += 31000000; // the owner change (NONE -> LOCAL) restarted the interval first
    fh_pump(&core, &host, &now);
    now += 31000000; // the next interval finds a held key
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_counters(&core)->keepalive_sent, sent);
    CHECK(kvm_hid_core_counters(&core)->keepalive_rejected >= 1);
    kvm_hid_intent_t u = fh_key(KVM_HID_KEY_UP, KVM_KEY_A);
    kvm_hid_core_local(&core, &u, now);
    fh_pump(&core, &host, &now);
    // Maintenance blocks it.
    kvm_hid_core_set_maintenance(&core, true);
    fh_pump(&core, &host, &now);
    host.n = 0;
    now += 61000000;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    kvm_hid_core_set_maintenance(&core, false);
    // Suspended HOST: nothing, and no remote wake.
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, 1, now);
    now += 61000000;
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.n, 0);
    // The interval clamps to 10..300 s.
    kvm_hid_core_set_keepalive(&core, true, 1);
    CHECK_EQ(core.keepalive_interval_us, 10000000u);
    kvm_hid_core_set_keepalive(&core, true, 100000);
    CHECK_EQ(core.keepalive_interval_us, 300000000u);
}

static void keepalive_restarts_on_epoch_change(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_core_set_keepalive(&core, true, 10);
    now += 9000000;
    kvm_hid_core_poll(&core, now);
    kvm_hid_core_set_maintenance(&core, true); // epoch change
    kvm_hid_core_set_maintenance(&core, false);
    now += 2000000; // 11 s after enabling, but only 2 s after the epoch change
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_counters(&core)->keepalive_sent, 0);
    now += 11000000;
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_counters(&core)->keepalive_sent, 1);
}

static void maintenance_blocks_input(void)
{
    fh_setup(&core, &host, &now);
    kvm_hid_intent_t d = fh_btn(KVM_HID_MOUSE_BTN_DOWN, KVM_MOUSE_BTN_LEFT);
    kvm_hid_core_local(&core, &d, now);
    fh_pump(&core, &host, &now);
    CHECK_EQ(host.buttons, 0x01);
    uint32_t e0 = kvm_hid_core_owner(&core)->epoch;
    kvm_hid_core_set_maintenance(&core, true); // AT-FAULT-07: release before flash
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    CHECK(kvm_hid_core_owner(&core)->epoch > e0);
    kvm_hid_intent_t m = fh_move(1, 1);
    CHECK_EQ(kvm_hid_core_local(&core, &m, now), KVM_HID_ERR_OTA_IN_PROGRESS);
    CHECK_EQ(kvm_hid_core_counters(&core)->release_by_reason[KVM_RA_MAINTENANCE], 1);
}

static void queue_overflow_is_fail_safe(void)
{
    // TECH-005: control-queue overflow -> RELEASE_ALL + counter, never a random lost UP.
    fh_setup(&core, &host, &now);
    host.refuse[KVM_HID_ITF_KEYBOARD] = true;
    kvm_hid_err_t last = KVM_HID_OK;
    for (int i = 0; i < KVM_HID_QUEUE_CAP + 1; i++) {
        kvm_hid_intent_t d = fh_key((i & 1) ? KVM_HID_KEY_UP : KVM_HID_KEY_DOWN, KVM_KEY_A);
        last = kvm_hid_core_local(&core, &d, now);
    }
    CHECK_EQ(last, KVM_HID_ERR_HID_QUEUE_OVERFLOW);
    CHECK_EQ(kvm_hid_core_counters(&core)->queue_overflows, 1);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_QUEUE_OVERFLOW), 1);
    CHECK_EQ(core.q_count, 0);
    host.refuse[KVM_HID_ITF_KEYBOARD] = false;
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
}

int main(void)
{
    printf("test_hid_core\n");
    RUN(mount_sends_neutral_on_every_interface);
    RUN(offline_rejects_everything);
    RUN(key_tap_is_two_reports);
    RUN(idempotent_down_and_orphan_up);
    RUN(invalid_usages_rejected);
    RUN(six_key_rollover);
    RUN(modifier_bits);
    RUN(split_motion_preserves_sum);
    RUN(coalesced_motion_preserves_sum);
    RUN(drag_keeps_button_mask);
    RUN(wheel_report_and_boot_fallback);
    RUN(shortcut_chords);
    RUN(chord_delivered_only_after_host_read);
    RUN(chord_interrupted_by_suspend);
    RUN(consumer_media_and_release);
    RUN(busy_endpoint_retries_without_duplicates);
    RUN(typing_after_reenumeration);
    RUN(kbd_one_shot_through_core);
    RUN(locked_modifier_not_inherited_by_shortcut);
    RUN(keepalive_rules);
    RUN(keepalive_restarts_on_epoch_change);
    RUN(maintenance_blocks_input);
    RUN(queue_overflow_is_fail_safe);
    TEST_MAIN_END("test_hid_core");
}
