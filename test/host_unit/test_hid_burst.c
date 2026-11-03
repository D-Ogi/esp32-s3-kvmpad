// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ADR-024 FR-748: the text burst in hid_core. One sequence at a time, paced, keyboard input refused
// while it runs, every RELEASE_ALL ends it, cancel releases the held keys before the mode's cancel
// key and then RELEASE_ALL; the sequences come from kbd_unicode (FR-747) as the firmware will use them.
#include <string.h>

#include "fake_hid_host.h"
#include "kbd_unicode.h"
#include "kvm_test.h"

static kvm_hid_core_t core;
static fake_host_t host;
static uint64_t now;

static void setup(void)
{
    fh_init(&host);
    kvm_hid_port_t port = fh_port(&host);
    kvm_hid_core_init(&core, &port, NULL);
    now = 1000;
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 1, now);
    fh_pump(&core, &host, &now);
    host.n = 0; // keep only what the burst sends
}

/** HOST reads every report each millisecond; stop when the burst ends or after `max_ms`. */
static void run_ms(int max_ms)
{
    for (int i = 0; i < max_ms; i++) {
        if (!host.no_complete) {
            for (uint8_t itf = 0; itf < KVM_HID_ITFS; itf++) {
                if (core.in_flight[itf]) {
                    kvm_hid_core_on_report_complete(&core, itf);
                }
            }
        }
        now += 1000;
        kvm_hid_core_poll(&core, now);
    }
}

static void run_until_idle(void)
{
    for (int i = 0; i < 10000 && kvm_hid_core_burst_active(&core); i++) {
        run_ms(1);
    }
    fh_pump(&core, &host, &now);
}

static int build(kvm_uni_mode_t mode, uint32_t cp, kvm_hid_burst_step_t *steps, kvm_hid_burst_step_t *cancel,
                 uint8_t *ncancel)
{
    kvm_uni_step_t u[KVM_UNI_MAX_STEPS];
    int n = kvm_uni_build(mode, cp, NULL, NULL, u, KVM_UNI_MAX_STEPS);
    for (int i = 0; i < n; i++) {
        steps[i].usage = u[i].usage;
        steps[i].down = u[i].down;
    }
    kvm_uni_step_t c[KVM_HID_BURST_CANCEL_CAP];
    int nc = kvm_uni_cancel(mode, c, KVM_HID_BURST_CANCEL_CAP);
    for (int i = 0; i < nc; i++) {
        cancel[i].usage = c[i].usage;
        cancel[i].down = c[i].down;
    }
    *ncancel = (uint8_t)nc;
    return n;
}

static bool report_has_key(const fh_report_t *r, uint8_t usage)
{
    for (int i = 2; i < 8; i++) {
        if (r->b[i] == usage) {
            return true;
        }
    }
    return false;
}

static void linux_sequence_reaches_the_host_in_order(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n = build(KVM_UNI_LINUX, 0xE9, s, c, &nc); // é
    CHECK_EQ(n, 16);
    uint64_t t0 = now;
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    CHECK(kvm_hid_core_burst_active(&core));
    run_until_idle();
    CHECK(!kvm_hid_core_burst_active(&core));
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.n, 16); // one keyboard report per step, nothing collapsed
    CHECK(now - t0 >= 15u * KVM_HID_BURST_STEP_US); // paced: <= 100 key events per second
    CHECK_EQ(host.key_presses[0xE0], 1);
    CHECK_EQ(host.key_presses[0xE1], 1);
    CHECK_EQ(host.key_presses[0x18], 1);
    CHECK_EQ(host.key_presses[0x27], 2);
    CHECK_EQ(host.key_presses[0x08], 1);
    CHECK_EQ(host.key_presses[0x26], 1);
    CHECK_EQ(host.key_presses[0x2C], 1);
    int u_with_ctrl_shift = 0, digits_with_mods = 0;
    for (int i = 0; i < host.n; i++) {
        const fh_report_t *r = &host.log[i];
        CHECK_EQ(r->itf, KVM_HID_ITF_KEYBOARD);
        u_with_ctrl_shift += report_has_key(r, 0x18) && r->b[0] == 0x03;
        digits_with_mods += (report_has_key(r, 0x27) || report_has_key(r, 0x26)) && r->b[0] != 0;
    }
    CHECK_EQ(u_with_ctrl_shift, 1);
    CHECK_EQ(digits_with_mods, 0);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_DONE), 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_started, 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_done, 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->release_all, 0);
}

static void done_only_after_the_host_read_the_last_report(void)
{
    setup();
    kvm_hid_burst_step_t s[2] = {{KVM_KEY_A, true}, {KVM_KEY_A, false}};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 0, now), KVM_HID_OK);
    for (int i = 0; i < 1000 && core.burst.pos < 2; i++) {
        run_ms(1);
    }
    host.no_complete = true; // the HOST has not read the UP report yet
    run_ms(50);              // well inside the 250 ms completion timeout
    CHECK(core.in_flight[KVM_HID_ITF_KEYBOARD]);
    CHECK(kvm_hid_core_burst_active(&core));
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_DONE), 0);
    host.no_complete = false;
    run_ms(2);
    CHECK(!kvm_hid_core_burst_active(&core));
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_DONE), 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->release_all, 0);
}

static void non_bmp_windows_hex_two_alt_groups(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n = build(KVM_UNI_WINDOWS_HEX, 0x1F600, s, c, &nc);
    CHECK(n > 0);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_until_idle();
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[0xE2], 2); // Alt pressed once per UTF-16 unit
    CHECK_EQ(host.key_presses[0x57], 2); // Numpad +
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_DONE), 1);
}

static void one_burst_at_a_time_and_keyboard_waits(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n = build(KVM_UNI_LINUX, 0xE9, s, c, &nc);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_ms(3);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_ERR_BUSY);
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_ERR_BUSY);
    a.type = KVM_HID_KEY_UP;
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_ERR_BUSY);
    CHECK_EQ(kvm_hid_core_ui_key_down(&core, KVM_KEY_A, now), KVM_HID_ERR_BUSY);
    CHECK_EQ(kvm_hid_core_ui_key_up(&core, KVM_KEY_A, now), KVM_HID_ERR_BUSY);
    CHECK_EQ(kvm_hid_core_ui_modifier_tap(&core, 0xE1, now), KVM_HID_ERR_BUSY);
    kvm_hid_intent_t m = fh_move(5, -3);
    CHECK_EQ(kvm_hid_core_local(&core, &m, now), KVM_HID_OK); // the pointer is not blocked
    // No CLIENT can take control while the burst runs (LOCAL is not idle).
    now += 5000000;
    CHECK(kvm_hid_core_remote_request(&core, 7, now) != KVM_HID_OK);
    run_until_idle();
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.sum_dx, 5);
    CHECK_EQ(host.key_presses[KVM_KEY_A], 0);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_DONE), 1);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_OK); // keyboard free again (UP without DOWN: no-op)
}

static void start_refusals_change_nothing(void)
{
    setup();
    kvm_hid_burst_step_t s[8];
    // DOWN without UP
    s[0] = (kvm_hid_burst_step_t){KVM_KEY_A, true};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 1, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    // UP without DOWN
    s[0] = (kvm_hid_burst_step_t){KVM_KEY_A, false};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 1, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    // DOWN twice
    s[0] = (kvm_hid_burst_step_t){KVM_KEY_A, true};
    s[1] = (kvm_hid_burst_step_t){KVM_KEY_A, true};
    s[2] = (kvm_hid_burst_step_t){KVM_KEY_A, false};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 3, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    // invalid usage
    s[0] = (kvm_hid_burst_step_t){0x01, true};
    s[1] = (kvm_hid_burst_step_t){0x01, false};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 0, now), KVM_HID_ERR_USAGE);
    // empty, over the cap, cancel without steps
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 0, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, KVM_HID_BURST_CAP + 1, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    s[0] = (kvm_hid_burst_step_t){KVM_KEY_A, true};
    s[1] = (kvm_hid_burst_step_t){KVM_KEY_A, false};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 2, now), KVM_HID_ERR_BAD_INTENT);
    // an unbalanced cancel key
    kvm_hid_burst_step_t bad_cancel[1] = {{0x29, true}};
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, bad_cancel, 1, now), KVM_HID_ERR_BAD_INTENT);
    // seven ordinary keys held at once
    kvm_hid_burst_step_t seven[14];
    for (int i = 0; i < 7; i++) {
        seven[i] = (kvm_hid_burst_step_t){(uint8_t)(KVM_KEY_A + i), true};
        seven[7 + i] = (kvm_hid_burst_step_t){(uint8_t)(KVM_KEY_A + i), false};
    }
    CHECK_EQ(kvm_hid_core_burst_start(&core, seven, 14, NULL, 0, now), KVM_HID_ERR_BAD_INTENT);
    CHECK(!kvm_hid_core_burst_active(&core));
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_started, 0);
    // a key held by the user: the burst must not mix with it
    kvm_hid_intent_t a = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A + 10);
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 0, now), KVM_HID_ERR_BUSY);
    a.type = KVM_HID_KEY_UP;
    CHECK_EQ(kvm_hid_core_local(&core, &a, now), KVM_HID_OK);
    fh_pump(&core, &host, &now);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 0, now), KVM_HID_OK);
    run_until_idle();
    CHECK(fh_host_neutral(&host));
    // no HOST: nothing starts
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, now);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, 2, NULL, 0, now), KVM_HID_ERR_HOST_OFFLINE);
    CHECK(!kvm_hid_core_burst_active(&core));
}

static void cancel_releases_before_the_cancel_key(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n = build(KVM_UNI_LINUX, 0xE9, s, c, &nc);
    CHECK_EQ(nc, 2);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    // Run until Ctrl+Shift are down and U is held (3 steps).
    for (int i = 0; i < 1000 && core.burst.pos < 3; i++) {
        run_ms(1);
    }
    run_ms(2);
    CHECK_EQ(host.mods, 0x03);
    CHECK(fh_key_in(host.keys, 0x18));
    int before = host.n;
    kvm_hid_core_burst_cancel(&core, KVM_RA_VIEW_CHANGE);
    CHECK(kvm_hid_core_burst_active(&core)); // cancelling: the release + Esc are still to be sent
    run_until_idle();
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(host.key_presses[0x29], 1); // Esc once
    int esc_with_mods = 0, esc_with_u = 0;
    for (int i = before; i < host.n; i++) {
        if (report_has_key(&host.log[i], 0x29)) {
            esc_with_mods += host.log[i].b[0] != 0;
            esc_with_u += report_has_key(&host.log[i], 0x18);
        }
    }
    CHECK_EQ(esc_with_mods, 0); // never Ctrl+Shift+Esc
    CHECK_EQ(esc_with_u, 0);
    CHECK_EQ(host.key_presses[0x2C], 0); // the rest of the sequence was not sent
    CHECK_EQ(host.key_presses[0x08], 0);
    const kvm_hid_counters_t *cn = kvm_hid_core_counters(&core);
    CHECK_EQ(cn->bursts_cancelled, 1);
    CHECK_EQ(cn->bursts_aborted, 0);
    CHECK_EQ(cn->release_by_reason[KVM_RA_VIEW_CHANGE], 1);
    CHECK_EQ(fh_count_events(&host, KVM_HID_EV_BURST_ABORTED), 1);
    kvm_hid_core_burst_cancel(&core, KVM_RA_VIEW_CHANGE); // no burst: no-op
    CHECK_EQ(cn->release_all, 1);
}

static void cancel_without_cancel_key_releases_alt(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n = build(KVM_UNI_MACOS, 0xD55C, s, c, &nc);
    CHECK_EQ(nc, 0);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    for (int i = 0; i < 1000 && core.burst.pos < 4; i++) {
        run_ms(1);
    }
    kvm_hid_core_burst_cancel(&core, KVM_RA_REQUEST);
    run_until_idle();
    CHECK(fh_host_neutral(&host));
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_cancelled, 1);
}

static void usb_loss_and_timeout_end_the_burst(void)
{
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    int n;

    // Unplug mid-burst: ended at once, nothing replayed after the next mount.
    setup();
    n = build(KVM_UNI_WINCOMPOSE, 0xE9, s, c, &nc);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_ms(25);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, 1, now);
    CHECK(!kvm_hid_core_burst_active(&core));
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_aborted, 1);
    kvm_hid_snapshot_t snap;
    kvm_hid_core_snapshot(&core, &snap);
    CHECK(snap.ledger_empty);
    CHECK(!snap.burst_active);
    kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, 2, now);
    int before = host.n;
    run_ms(500);
    fh_pump(&core, &host, &now);
    for (int i = before; i < host.n; i++) {
        for (int k = 0; k < host.log[i].len; k++) {
            CHECK_EQ(host.log[i].b[k], 0); // only neutral reports
        }
    }

    // The HOST stops reading: completion timeout -> RELEASE_ALL ends the burst.
    setup();
    n = build(KVM_UNI_LINUX, 0x1F600, s, c, &nc);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_ms(15);
    host.no_complete = true;
    run_ms(400);
    CHECK(!kvm_hid_core_burst_active(&core));
    CHECK(kvm_hid_core_counters(&core)->release_by_reason[KVM_RA_COMPLETION_TIMEOUT] >= 1);
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_aborted, 1);

    // An explicit RELEASE_ALL from the UI ends it too.
    setup();
    n = build(KVM_UNI_LINUX, 0xE9, s, c, &nc);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_ms(25);
    kvm_hid_intent_t ra = fh_key(KVM_HID_RELEASE_ALL, 0);
    CHECK_EQ(kvm_hid_core_local(&core, &ra, now), KVM_HID_OK);
    CHECK(!kvm_hid_core_burst_active(&core));
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));

    // Maintenance (OTA) mid-burst: released, never written again.
    setup();
    n = build(KVM_UNI_LINUX, 0xE9, s, c, &nc);
    CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
    run_ms(25);
    kvm_hid_core_set_maintenance(&core, true);
    run_ms(10);
    CHECK(!kvm_hid_core_burst_active(&core));
    fh_pump(&core, &host, &now);
    CHECK(fh_host_neutral(&host));
    kvm_hid_core_burst_cancel(&core, KVM_RA_REQUEST); // nothing to cancel
    CHECK_EQ(kvm_hid_core_counters(&core)->bursts_cancelled, 0);
}

static void many_bursts_leave_nothing_held(void)
{
    setup();
    kvm_hid_burst_step_t s[KVM_HID_BURST_CAP], c[KVM_HID_BURST_CANCEL_CAP];
    uint8_t nc;
    static const kvm_uni_mode_t MODES[] = {KVM_UNI_LINUX, KVM_UNI_MACOS, KVM_UNI_WINDOWS_HEX, KVM_UNI_WINCOMPOSE};
    uint32_t seed = 12345;
    int done = 0, cancelled = 0;
    for (int i = 0; i < 200; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t cp = 0x80u + (seed >> 8) % 0xFF00u;
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0x1F600;
        }
        int n = build(MODES[i % 4], cp, s, c, &nc);
        CHECK_EQ(kvm_hid_core_burst_start(&core, s, (uint16_t)n, c, nc, now), KVM_HID_OK);
        if (seed & 0x10000u) { // cancel about half of them at a random step
            int stop = (int)((seed >> 4) % (uint32_t)n);
            for (int k = 0; k < 2000 && core.burst.pos < stop; k++) {
                run_ms(1);
            }
            kvm_hid_core_burst_cancel(&core, KVM_RA_REQUEST);
            cancelled++;
        } else {
            done++;
        }
        run_until_idle();
        CHECK(fh_host_neutral(&host));
    }
    const kvm_hid_counters_t *cn = kvm_hid_core_counters(&core);
    CHECK_EQ(cn->bursts_started, 200);
    CHECK_EQ((int)(cn->bursts_done + cn->bursts_cancelled), 200);
    CHECK(cn->bursts_cancelled <= (uint32_t)cancelled); // a cancel after the last step finds the burst done
    CHECK_EQ(cn->bursts_aborted, 0);
    CHECK_EQ(cn->keyup_without_down, 0);
    printf("    200 bursts: %u done, %u cancelled\n", (unsigned)cn->bursts_done, (unsigned)cn->bursts_cancelled);
    (void)done;
}

int main(void)
{
    printf("test_hid_burst\n");
    RUN(linux_sequence_reaches_the_host_in_order);
    RUN(done_only_after_the_host_read_the_last_report);
    RUN(non_bmp_windows_hex_two_alt_groups);
    RUN(one_burst_at_a_time_and_keyboard_waits);
    RUN(start_refusals_change_nothing);
    RUN(cancel_releases_before_the_cancel_key);
    RUN(cancel_without_cancel_key_releases_alt);
    RUN(usb_loss_and_timeout_end_the_burst);
    RUN(many_bursts_leave_nothing_held);
    TEST_MAIN_END("test_hid_burst");
}
