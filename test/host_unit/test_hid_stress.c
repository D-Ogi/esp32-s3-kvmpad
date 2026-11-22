// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// AT-HID-09 (10k shortcuts with a random disconnect: 0 stuck modifiers, 0 phantom repeats),
// 10k RELEASE_ALL without a stuck key (AT-KEY-07 core part) and a seeded property test over random
// LOCAL/REMOTE/USB/endpoint events. The seed is printed; pass one as argv[1] to reproduce.
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "fake_hid_host.h"
#include "kvm_test.h"

static kvm_hid_core_t core;
static fake_host_t host;
static uint64_t now;
static uint64_t rng_state;

static uint32_t rnd(void)
{
    // splitmix64: deterministic across platforms
    uint64_t z = (rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return (uint32_t)((z ^ (z >> 31)) >> 32);
}

static uint32_t rnd_below(uint32_t n)
{
    return rnd() % n;
}

// Every report on the wire must be well-formed (AT-HID-03/04).
static void check_and_clear_wire(void)
{
    CHECK_EQ(host.dropped_log, 0);
    for (int i = 0; i < host.n; i++) {
        const fh_report_t *r = &host.log[i];
        if (r->itf == KVM_HID_ITF_KEYBOARD) {
            CHECK_EQ(r->len, 8);
            CHECK_EQ(r->b[1], 0);
            for (int k = 2; k < 8; k++) {
                CHECK(r->b[k] == 0 || kvm_usage_is_valid_key(r->b[k]));
                CHECK(!kvm_usage_is_modifier(r->b[k])); // modifiers only as bits
            }
        } else if (r->itf == KVM_HID_ITF_MOUSE) {
            CHECK_EQ(r->len, host.protocol[KVM_HID_ITF_MOUSE] == KVM_HID_PROTOCOL_BOOT ? 3 : 5);
            CHECK((r->b[0] & 0xF8) == 0);
        } else {
            CHECK_EQ(r->itf, KVM_HID_ITF_CONSUMER);
            CHECK_EQ(r->len, 2);
            uint16_t u = (uint16_t)(r->b[0] | (r->b[1] << 8));
            CHECK(u == 0 || kvm_consumer_is_allowed(u));
        }
    }
    CHECK(host.max_abs_delta <= 127);
    host.n = 0;
}

// Each trial begins neutral. Before a loss the host may see a prefix of the
// shortcut; after loss it must receive a neutral report on every interface.
static void check_chord_wire(const kvm_chord_t *ch, int prefix, bool delivered, bool interrupted)
{
    uint8_t expected[4][8] = {{0}};
    bool keyboard = ch->kind == KVM_CHORD_KIND_KEYBOARD;
    uint8_t itf = keyboard ? KVM_HID_ITF_KEYBOARD : KVM_HID_ITF_CONSUMER;
    uint8_t len = keyboard ? 8 : 2;
    int count = 2;
    if (!keyboard) {
        expected[0][0] = (uint8_t)ch->usage;
        expected[0][1] = (uint8_t)(ch->usage >> 8);
    } else if (ch->modifiers) {
        count = 4; // modifiers, modifiers + key, modifiers, neutral (02 §7.3)
        expected[0][0] = expected[1][0] = expected[2][0] = ch->modifiers;
        expected[1][2] = (uint8_t)ch->usage;
    } else {
        expected[0][2] = (uint8_t)ch->usage;
    }
    CHECK(prefix <= count);
    if (delivered || !interrupted) CHECK_EQ(prefix, count);
    for (int i = 0; i < prefix && i < count; i++) {
        CHECK_EQ(host.log[i].itf, itf);
        CHECK_EQ(host.log[i].len, len);
        CHECK(memcmp(host.log[i].b, expected[i], len) == 0);
    }
    if (interrupted) {
        static const uint8_t neutral[8] = {0};
        bool seen[KVM_HID_ITFS] = {false};
        for (int i = prefix; i < host.n; i++) {
            const fh_report_t *r = &host.log[i];
            CHECK(r->len <= sizeof(neutral) && memcmp(r->b, neutral, r->len) == 0);
            if (r->itf < KVM_HID_ITFS) seen[r->itf] = true;
        }
        for (unsigned i = 0; i < KVM_HID_ITFS; i++) CHECK(seen[i]);
    }
}

static void chords_with_random_disconnects(void)
{
    // AT-HID-09.
    fh_setup(&core, &host, &now);
    uint32_t usb_epoch = 1;
    const uint16_t last_id = KVM_HID_ITFS > KVM_HID_ITF_CONSUMER ? KVM_CHORD_COUNT-1 : KVM_CHORD_LAYOUT_CTRL;
    uint32_t observed = 0, initial_reports = core.c.reports_sent;
    for (int i = 0; i < 10000; i++) {
        uint16_t id = (uint16_t)(1 + rnd_below(last_id));
        const kvm_chord_t *ch = kvm_chord_get(id);
        kvm_hid_intent_t c = fh_chord(id);
        CHECK_EQ(kvm_hid_core_local(&core, &c, now), KVM_HID_OK);
        // Run a random number of HOST polls, then maybe disconnect / suspend in the middle.
        int steps = (int)rnd_below(8);
        for (int s = 0; s < steps; s++) {
            for (uint8_t itf = 0; itf < KVM_HID_ITFS; itf++) {
                if (core.in_flight[itf]) {
                    kvm_hid_core_on_report_complete(&core, itf);
                }
            }
            now += 1000;
            kvm_hid_core_poll(&core, now);
        }
        uint32_t r = rnd_below(10);
        int before_loss = host.n;
        if (r == 0) {
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, usb_epoch, now);
            CHECK_EQ(host.n, before_loss);
            fh_reset_connection(&host);
            usb_epoch++;
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, usb_epoch, now);
        } else if (r == 1) {
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, usb_epoch, now);
            CHECK_EQ(host.n, before_loss);
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, usb_epoch, now);
        }
        fh_pump(&core, &host, &now);
        int delivered = fh_count_events(&host, KVM_HID_EV_CHORD_DELIVERED);
        CHECK_EQ(delivered + fh_count_events(&host, KVM_HID_EV_CHORD_ABORTED), 1);
        if (r > 1) CHECK_EQ(delivered, 1);
        for (int j = 0; j < host.nev; j++) {
            if (host.ev[j].kind == KVM_HID_EV_CHORD_DELIVERED || host.ev[j].kind == KVM_HID_EV_CHORD_ABORTED)
                CHECK_EQ(host.ev[j].a, id);
        }
        host.nev = 0;
        check_chord_wire(ch, r <= 1 ? before_loss : host.n, delivered != 0, r <= 1);
        CHECK(fh_host_neutral(&host));
        observed += (uint32_t)host.n;
        check_and_clear_wire();
    }
    CHECK_EQ(observed, core.c.reports_sent - initial_reports);
    CHECK_EQ(kvm_hid_core_counters(&core)->chords_delivered + kvm_hid_core_counters(&core)->chords_aborted,
             10000);
    printf("    10000 shortcuts, observed %" PRIu32 " reports, delivered %" PRIu32 ", aborted %" PRIu32 ", releases %" PRIu32 "\n",
           observed, kvm_hid_core_counters(&core)->chords_delivered,
           kvm_hid_core_counters(&core)->chords_aborted, kvm_hid_core_counters(&core)->release_all);
}

static void ten_thousand_release_all(void)
{
    fh_setup(&core, &host, &now);
    int stuck = 0;
    for (int i = 0; i < 10000; i++) {
        int n = 1 + (int)rnd_below(5);
        for (int k = 0; k < n; k++) {
            kvm_hid_intent_t d;
            switch (rnd_below(4)) {
            case 0:
                d = fh_key(KVM_HID_KEY_DOWN, (uint8_t)(KVM_KEY_A + rnd_below(40)));
                break;
            case 1:
                d = fh_key(KVM_HID_KEY_DOWN, (uint8_t)(KVM_KEY_LEFT_CTRL + rnd_below(8)));
                break;
            case 2:
                d = fh_btn(KVM_HID_MOUSE_BTN_DOWN, (uint8_t)(1 + rnd_below(3)));
                break;
            default:
                d = fh_consumer(KVM_HID_CONSUMER_DOWN, KVM_CONSUMER_MUTE);
                break;
            }
            kvm_hid_core_local(&core, &d, now);
        }
        int steps = (int)rnd_below(4);
        for (int s = 0; s < steps; s++) {
            for (uint8_t itf = 0; itf < KVM_HID_ITFS; itf++) {
                if (core.in_flight[itf] && rnd_below(2)) {
                    kvm_hid_core_on_report_complete(&core, itf);
                }
            }
            now += 1000;
            kvm_hid_core_poll(&core, now);
        }
        kvm_hid_core_release_all(&core, KVM_RA_REQUEST);
        fh_pump(&core, &host, &now);
        stuck += !fh_host_neutral(&host);
        check_and_clear_wire();
    }
    CHECK_EQ(stuck, 0);
}

// Random walk over every input source; after each step the core and the HOST model must agree once
// the endpoint drains, and nothing may reach the wire while the HOST is not mounted.
static void property_random_events(void)
{
    fh_setup(&core, &host, &now);
    uint32_t usb_epoch = 1;
    uint32_t lease_epoch = 0;
    const uint64_t session = 77;
    int violations_offline = 0, violations_sync = 0, violations_remote = 0;
    for (int i = 0; i < 100000; i++) {
        int sent_before = host.n;
        bool mounted_before = core.usb == KVM_HOST_USB_MOUNTED;
        uint32_t op = rnd_below(100);
        kvm_hid_intent_t in;
        memset(&in, 0, sizeof(in));
        bool remote = false;
        if (op < 30) {
            in = fh_key(rnd_below(2) ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP,
                        rnd_below(4) ? (uint8_t)(KVM_KEY_A + rnd_below(10)) : (uint8_t)(0xE0 + rnd_below(8)));
        } else if (op < 45) {
            in = fh_move((int16_t)((int)rnd_below(601) - 300), (int16_t)((int)rnd_below(601) - 300));
        } else if (op < 55) {
            in = fh_btn(rnd_below(2) ? KVM_HID_MOUSE_BTN_DOWN : KVM_HID_MOUSE_BTN_UP, (uint8_t)(1 + rnd_below(3)));
        } else if (op < 60) {
            in = fh_wheel((int16_t)((int)rnd_below(21) - 10), (int16_t)((int)rnd_below(21) - 10));
        } else if (op < 64) {
            in = fh_chord((uint16_t)(1 + rnd_below(KVM_CHORD_COUNT - 1)));
        } else if (op < 66) {
            in = fh_consumer(rnd_below(2) ? KVM_HID_CONSUMER_DOWN : KVM_HID_CONSUMER_UP, KVM_CONSUMER_VOLUME_DOWN);
        } else if (op < 68) {
            in.type = KVM_HID_RELEASE_ALL;
        }
        if (op < 68) {
            remote = core.owner.kind == OWNER_REMOTE && rnd_below(2);
            if (remote) {
                kvm_hid_err_t e = kvm_hid_core_remote(&core, session, lease_epoch, &in, now);
                if (e == KVM_HID_OK && core.owner.kind != OWNER_REMOTE) {
                    violations_remote++;
                }
            } else {
                kvm_hid_core_local(&core, &in, now);
            }
        } else if (op < 70) {
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_DETACHED, usb_epoch, now);
        } else if (op < 72) {
            if (core.usb != KVM_HOST_USB_MOUNTED) {
                fh_reset_connection(&host);
                usb_epoch++;
                kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, usb_epoch, now);
            }
        } else if (op < 73) {
            kvm_hid_core_set_usb(&core, KVM_HOST_USB_SUSPENDED, usb_epoch, now);
        } else if (op < 74) {
            if (core.usb == KVM_HOST_USB_SUSPENDED) {
                kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, usb_epoch, now);
            }
        } else if (op < 77) {
            uint32_t le = 0;
            now += 2100000; // let LOCAL go idle sometimes
            if (kvm_hid_core_remote_grant(&core, session, now, &le) == KVM_HID_OK) {
                lease_epoch = le;
            }
        } else if (op < 79) {
            kvm_hid_core_remote_heartbeat(&core, session, now);
        } else if (op < 80) {
            kvm_hid_core_remote_disconnect(&core, session);
        } else if (op < 81) {
            kvm_hid_core_set_maintenance(&core, rnd_below(2));
        } else if (op < 83) {
            host.refuse[rnd_below(KVM_HID_ITFS)] = rnd_below(2);
        } else if (op < 84) {
            kvm_hid_core_set_keepalive(&core, rnd_below(2), 10);
        } else if (op < 90) {
            now += rnd_below(5000000); // time jumps: lease expiry, keep-alive, completion timeouts
            kvm_hid_core_poll(&core, now);
        } else {
            for (uint8_t itf = 0; itf < KVM_HID_ITFS; itf++) {
                if (core.in_flight[itf] && rnd_below(3)) {
                    kvm_hid_core_on_report_complete(&core, itf);
                }
            }
            now += 1000;
            kvm_hid_core_poll(&core, now);
        }
        if (!mounted_before && core.usb != KVM_HOST_USB_MOUNTED && host.n != sent_before) {
            violations_offline++; // INV-005: no report without a mounted HOST
        }
        if (core.c.queue_high_water > KVM_HID_QUEUE_CAP) {
            violations_sync++;
        }
        // Every 1000 steps: drain fully and compare the ledger with what the HOST believes.
        if (i % 1000 == 999) {
            for (int k = 0; k < KVM_HID_ITFS; k++) {
                host.refuse[k] = false;
            }
            if (core.usb != KVM_HOST_USB_MOUNTED) {
                fh_reset_connection(&host);
                usb_epoch++;
                kvm_hid_core_set_usb(&core, KVM_HOST_USB_MOUNTED, usb_epoch, now);
            }
            kvm_hid_core_set_maintenance(&core, false);
            fh_pump(&core, &host, &now);
            kvm_hid_snapshot_t s;
            kvm_hid_core_snapshot(&core, &s);
            if (s.modifiers_down != host.mods || s.buttons_down != host.buttons || s.consumer_down != host.consumer) {
                violations_sync++;
            }
            kvm_hid_core_release_all(&core, KVM_RA_REQUEST);
            fh_pump(&core, &host, &now);
            if (!fh_host_neutral(&host)) {
                violations_sync++;
            }
        }
        check_and_clear_wire();
    }
    CHECK_EQ(violations_offline, 0);
    CHECK_EQ(violations_sync, 0);
    CHECK_EQ(violations_remote, 0);
    const kvm_hid_counters_t *c = kvm_hid_core_counters(&core);
    printf("    100000 steps: reports %" PRIu32 ", accepted %" PRIu32 ", releases %" PRIu32 ", revokes %" PRIu32
           ", timeouts %" PRIu32 ", keepalive %" PRIu32 ", queue high water %" PRIu32 "\n",
           c->reports_sent, c->accepted, c->release_all, c->remote_revokes, c->completion_timeouts, c->keepalive_sent,
           c->queue_high_water);
}

int main(int argc, char **argv)
{
    uint64_t seed = 0x4B564D706164ull; // "KVMpad"
    if (argc > 1) {
        seed = strtoull(argv[1], NULL, 0);
    }
    printf("test_hid_stress seed=0x%" PRIx64 "\n", seed);
    rng_state = seed;
    RUN(chords_with_random_disconnects);
    RUN(ten_thousand_release_all);
    RUN(property_random_events);
    TEST_MAIN_END("test_hid_stress");
}
