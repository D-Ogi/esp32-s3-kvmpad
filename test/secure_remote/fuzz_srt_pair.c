// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Seeded mutation fuzzer over the pairing endpoint (03 §9 item 3, AT-EXT-17).
//
// Every input goes through the whole transport path a hostile peer can reach without a token: the
// envelope codec, the payload decoders and the pairing state machine. Three properties must hold
// for every input, valid or not:
//
//   1. no HID intent is ever produced (AGENTS.md prohibition 4: no HID without auth and a lease);
//   2. no client is ever stored and no RELEASE_ALL is ever raised from this endpoint;
//   3. nothing the device sends back contains the pairing code (SEC-105).
//
// Usage: fuzz_srt_pair [seed] [iterations]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_test.h"
#include "srt_harness.h"

static sh_t h;

// xorshift64*, so a seed reproduces a run on any platform.
static uint64_t rng_state;

static uint64_t rnd(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ull;
}

static uint32_t rnd_below(uint32_t n)
{
    return (n == 0u) ? 0u : (uint32_t)(rnd() % n);
}

// How many inputs one pairing connection takes before it is recycled; see the loop below.
#define FZ_RECYCLE 20000ul

static const char *const seeds[] = {
    ("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{\"v\":1,"
     "\"pake\":\"ecjpake-p256-sha256\",\"client_name\":\"Desk1\","
     "\"client_id\":\"0123456789abcdef0123456789abcdef\","
     "\"cert_sha256\":\"a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf\"}}"),
    "{\"v\":1,\"type\":\"pair.round1\",\"seq\":2,\"payload\":{\"blob\":\"QUJDREVGR0g=\"}}",
    "{\"v\":1,\"type\":\"pair.round2\",\"seq\":3,\"payload\":{\"blob\":\"QUJDREVG\"}}",
    ("{\"v\":1,\"type\":\"pair.confirm\",\"seq\":4,\"payload\":{\"mac\":"
     "\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"),
    "{\"v\":1,\"type\":\"pair.hello\",\"seq\":5,\"session_id\":\"p-0000\",\"payload\":{}}",
    "{\"v\":2,\"type\":\"pair.hello\",\"seq\":6,\"payload\":{}}",
    "{}",
    "[]",
};

#define FZ_MAX (KVM_JSON_MAX_FRAME + 64u)

static size_t mutate(char *buf, size_t cap)
{
    const char *src = seeds[rnd_below((uint32_t)(sizeof(seeds) / sizeof(seeds[0])))];
    size_t len = strlen(src);
    if (len > cap) {
        len = cap;
    }
    memcpy(buf, src, len);

    const uint32_t ops = 1u + rnd_below(6u);
    for (uint32_t i = 0; i < ops; i++) {
        switch (rnd_below(6u)) {
        case 0: // flip one byte to an arbitrary value, including a control character or NUL
            if (len > 0u) {
                buf[rnd_below((uint32_t)len)] = (char)(rnd() & 0xFFu);
            }
            break;
        case 1: // truncate
            if (len > 0u) {
                len = rnd_below((uint32_t)len);
            }
            break;
        case 2: // append a run of one character, sometimes past the 1024-byte limit
            {
                const char c = (char)(rnd() & 0xFFu);
                uint32_t n = rnd_below(200u);
                while (n-- > 0u && len < cap) {
                    buf[len++] = c;
                }
            }
            break;
        case 3: // splice in a fragment of another seed
            {
                const char *other =
                    seeds[rnd_below((uint32_t)(sizeof(seeds) / sizeof(seeds[0])))];
                const size_t olen = strlen(other);
                const uint32_t take = rnd_below((uint32_t)olen);
                for (uint32_t k = 0; k < take && len < cap; k++) {
                    buf[len++] = other[k];
                }
            }
            break;
        case 4: // deepen the nesting, which must hit the depth limit, not the stack
            {
                uint32_t n = rnd_below(40u);
                while (n-- > 0u && len + 1u < cap) {
                    buf[len++] = (rnd() & 1u) ? '[' : '{';
                }
            }
            break;
        default: // delete a byte
            if (len > 0u) {
                const uint32_t at = rnd_below((uint32_t)len);
                memmove(&buf[at], &buf[at + 1u], len - at - 1u);
                len--;
            }
            break;
        }
    }
    return len;
}

int main(int argc, char **argv)
{
    const uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20261102ull;
    const unsigned long iters = (argc > 2) ? strtoul(argv[2], NULL, 0) : 20000ul;
    rng_state = seed ? seed : 1ull;

    static char buf[FZ_MAX];
    unsigned long reopened = 0;

    CHECK_EQ(sh_init(&h, seed), KVM_PAIR_OK);
    sh_online(&h);
    CHECK_EQ(kvm_srt_pair_open(&h.srt, sh_now_us()), KVM_PAIR_OK);
    char code[KVM_PAIR_CODE_CAP];
    (void)snprintf(code, sizeof(code), "%s", kvm_srt_pair_code(&h.srt));

    kvm_srt_admission_t adm;
    CHECK_EQ(kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm),
             KVM_SRT_ADMIT_OK);
    CHECK(kvm_srt_conn_open(&h.srt, 100u, &adm, sh_now_us()) >= 0);

    for (unsigned long it = 0; it < iters; it++) {
        const size_t len = mutate(buf, sizeof(buf));
        kvm_srt_conn_frame(&h.srt, 100u, buf, len, sh_now_us());

        // Properties 1 and 2: nothing a token-less peer sends may reach the HID path or the
        // register, however the state machine answered.
        if (h.nintents != 0 || h.nreleases != 0 || h.nrelease_all != 0 ||
            kvm_srt_clients_count(&h.srt) != 0u) {
            fprintf(stderr, "seed=%llu iteration=%lu: endpoint produced HID or state\n",
                    (unsigned long long)seed, it);
            CHECK_EQ(h.nintents, 0);
            CHECK_EQ(h.nreleases, 0);
            CHECK_EQ(h.nrelease_all, 0);
            CHECK_EQ(kvm_srt_clients_count(&h.srt), 0);
            break;
        }
        // Property 3: no answer ever quotes the code.
        for (int i = 0; i < h.nsent; i++) {
            if (strstr(h.sent[i].text, code) != NULL) {
                // The frame is printed, the code is not: a diagnostic must not be the one place a
                // code ends up in a log (SEC-105).
                fprintf(stderr, "seed=%llu iteration=%lu: code echoed on the wire in %s\n",
                        (unsigned long long)seed, it, h.sent[i].text);
                CHECK(false);
                break;
            }
            // Every answer that does leave has to be a well-formed v1 frame.
            static kvm_json_doc_t d;
            CHECK_EQ(kvm_json_parse(&d, h.sent[i].text, h.sent[i].len), KVM_JSON_OK);
        }
        sh_clear_wire(&h);

        // The bad-frame budget closes the connection; reopen it so the fuzzer keeps exercising the
        // endpoint rather than a closed slot. A lockout closes the window too, so reopen that.
        //
        // The connection is also recycled every FZ_RECYCLE inputs even when nothing closed it, to
        // keep the server-to-client `seq` of this connection below six digits. Left to run, `seq`
        // reaches six digits and eventually equals the six-digit code, and the substring sweep
        // above then reports a match that is a numeric coincidence rather than a leak. Recycling
        // resets the per-connection counter and draws a fresh code, which keeps the sweep exact.
        const bool recycle = (it % FZ_RECYCLE) == (FZ_RECYCLE - 1ul);
        if (recycle || kvm_srt_slot_of(&h.srt, 100u) < 0 || !kvm_srt_pair_endpoint_open(&h.srt)) {
            (void)kvm_srt_pair_cancel(&h.srt, sh_now_us());
            kvm_srt_conn_close(&h.srt, 100u, KVM_SRT_LOSS_PROTOCOL, sh_now_us());
            det_clock_advance(KVM_PAIR_LOCKOUT_MS + 1000u);
            kvm_srt_tick(&h.srt, sh_now_us());
            if (kvm_srt_pair_open(&h.srt, sh_now_us()) != KVM_PAIR_OK) {
                continue;
            }
            (void)snprintf(code, sizeof(code), "%s", kvm_srt_pair_code(&h.srt));
            if (kvm_srt_admit(&h.srt, KVM_SRT_CONN_PAIRING, NULL, sh_now_us(), &adm) !=
                KVM_SRT_ADMIT_OK) {
                continue;
            }
            if (kvm_srt_conn_open(&h.srt, 100u, &adm, sh_now_us()) < 0) {
                continue;
            }
            reopened++;
            sh_clear_wire(&h);
        }
    }

    printf("fuzz_srt_pair: seed=%llu iterations=%lu reopened=%lu frames_in=%u rejected=%u\n",
           (unsigned long long)seed, iters, reopened, kvm_srt_counters(&h.srt)->pair_frames_in,
           kvm_srt_counters(&h.srt)->pair_frames_rejected);
    kvm_srt_shutdown(&h.srt, sh_now_us());
    TEST_MAIN_END("fuzz_srt_pair");
}
