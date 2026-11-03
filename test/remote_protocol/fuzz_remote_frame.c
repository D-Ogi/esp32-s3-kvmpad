// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Seeded mutation fuzzer over the frame reader and the validator (spec 03 §9 item 3, AT-SEC-04,
// AT-EXT-17). Two invariants are checked on every input:
//   1. nothing crashes, reads out of bounds or trips a sanitizer (the build enables ASan/UBSan);
//   2. a frame the validator does not accept produces no HID intent at all.
// The accessors are also walked over every accepted document, so a token that the parser emitted
// can never make a reader read outside the frame.
//
// Usage: fuzz_remote_frame [seed] [iterations]        (defaults: 20261102, 200000)
// A failure prints the seed, the iteration number and the offending bytes, which is the reproducer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rp_harness.h"

#define FZ_BUF 1200
#define SID 0x7001u

static uint64_t rng_state;

static uint64_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

static uint32_t rnd_below(uint32_t n)
{
    return (n == 0u) ? 0u : (uint32_t)(rnd() % (uint64_t)n);
}

// Valid starting points: every message type, so the mutations stay near the grammar. Each entry
// takes the same three arguments in the same order (client seq, session label, lease epoch); an
// entry that does not use one simply ignores it.
static const char *const corpus[] = {
    "{\"v\":1,\"type\":\"status.get\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":{}}",
    "{\"v\":1,\"type\":\"control.request\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":"
    "{\"reason\":\"remote_support\"}}",
    "{\"v\":1,\"type\":\"ping\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":{\"nonce\":101}}",
    "{\"v\":1,\"type\":\"input\",\"seq\":%llu,\"session_id\":\"%s\",\"lease_epoch\":%u,\"payload\":"
    "{\"kind\":\"mouse.move\",\"dx\":14,\"dy\":-5}}",
    "{\"v\":1,\"type\":\"input\",\"seq\":%llu,\"session_id\":\"%s\",\"lease_epoch\":%u,\"payload\":"
    "{\"kind\":\"mouse.button\",\"button\":\"left\",\"down\":true}}",
    "{\"v\":1,\"type\":\"input\",\"seq\":%llu,\"session_id\":\"%s\",\"lease_epoch\":%u,\"payload\":"
    "{\"kind\":\"mouse.wheel\",\"vertical\":-3,\"horizontal\":1}}",
    "{\"v\":1,\"type\":\"input\",\"seq\":%llu,\"session_id\":\"%s\",\"lease_epoch\":%u,\"payload\":"
    "{\"kind\":\"keyboard.key\",\"usage_id\":224,\"down\":true}}",
    "{\"v\":1,\"type\":\"input\",\"seq\":%llu,\"session_id\":\"%s\",\"lease_epoch\":%u,\"payload\":"
    "{\"kind\":\"shortcut\",\"command_id\":\"ctrl_alt_del\"}}",
    "{\"v\":1,\"type\":\"control.release\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":{}}",
    "{\"v\":1,\"type\":\"session.close\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":{}}",
    "{\"v\":1,\"type\":\"hello\",\"seq\":%llu,\"payload\":{\"client_name\":\"Desk1\","
    "\"capabilities\":[\"hid_v1\"]}}",
    "{\"v\":1,\"type\":\"pair.round1\",\"seq\":%llu,\"payload\":{\"pake\":\"AAECAwQFBgc=\"}}",
    "{\"v\":1,\"type\":\"settings.set\",\"seq\":%llu,\"session_id\":\"%s\",\"payload\":"
    "{\"control.approval\":\"auto\"}}",
};
#define CORPUS_N (sizeof(corpus) / sizeof(corpus[0]))

static const char *const interesting[] = {
    "\"", "'", "{", "}", "[", "]", ":", ",", "\\", "\\u", "\\ud83d", "\\x00", "-", "+", ".",
    "e9", "0", "9007199254740993", "4294967296", "-4097", "4097", "null", "true", "NaN",
    "Infinity", "1e400", "\xc3\x28", "\xf0\x9f", "\x00", "\x7f", "<script>", "'; DROP--",
    "%s%n", "../../", "\n", "\r\n", "\t", "          ",
};
#define INTERESTING_N (sizeof(interesting) / sizeof(interesting[0]))

static size_t mutate(char *buf, size_t len)
{
    uint32_t op = rnd_below(10);
    switch (op) {
    case 0: // bit flip
        if (len > 0u) {
            buf[rnd_below((uint32_t)len)] ^= (char)(1u << rnd_below(8));
        }
        break;
    case 1: // random byte
        if (len > 0u) {
            buf[rnd_below((uint32_t)len)] = (char)rnd_below(256);
        }
        break;
    case 2: // delete a byte
        if (len > 1u) {
            uint32_t p = rnd_below((uint32_t)len);
            memmove(buf + p, buf + p + 1, len - p - 1u);
            len--;
        }
        break;
    case 3: // insert an interesting token
    case 4: {
        const char *tok = interesting[rnd_below(INTERESTING_N)];
        size_t tl = strlen(tok);
        if (tl == 0u) {
            tl = 1u; // the embedded NUL case
        }
        if (len + tl < FZ_BUF) {
            uint32_t p = rnd_below((uint32_t)len + 1u);
            memmove(buf + p + tl, buf + p, len - p);
            memcpy(buf + p, tok, tl);
            len += tl;
        }
        break;
    }
    case 5: // duplicate a span (depth / repetition pressure)
        if (len > 2u) {
            uint32_t p = rnd_below((uint32_t)len - 1u);
            uint32_t n = 1u + rnd_below(32u);
            if (p + n > len) {
                n = (uint32_t)len - p;
            }
            if (len + n < FZ_BUF) {
                memmove(buf + p + n, buf + p, len - p);
                len += n;
            }
        }
        break;
    case 6: // truncate
        if (len > 1u) {
            len = 1u + rnd_below((uint32_t)len - 1u);
        }
        break;
    case 7: // grow past the 1024 B limit
        while (len < FZ_BUF - 1u && rnd_below(4u) != 0u) {
            buf[len++] = (char)('a' + (int)rnd_below(26u));
        }
        break;
    case 8: // swap two bytes
        if (len > 1u) {
            uint32_t a = rnd_below((uint32_t)len);
            uint32_t b = rnd_below((uint32_t)len);
            char t = buf[a];
            buf[a] = buf[b];
            buf[b] = t;
        }
        break;
    default: // nested braces
        if (len + 8u < FZ_BUF) {
            uint32_t p = rnd_below((uint32_t)len + 1u);
            memmove(buf + p + 4u, buf + p, len - p);
            memcpy(buf + p, "{\"a\":", 4);
            len += 4u;
        }
        break;
    }
    return len;
}

/** Walk every accessor over a parsed document: a bad token must not reach outside the frame. */
static void walk(const kvm_json_doc_t *doc)
{
    char tmp[64];
    int64_t iv;
    bool bv;
    static const char *const allowed[] = {"v", "type", "seq", NULL};
    for (uint16_t t = 0; t < doc->ntok; t++) {
        (void)kvm_json_type(doc, (int)t);
        (void)kvm_json_count(doc, (int)t);
        (void)kvm_json_member(doc, (int)t, "payload");
        (void)kvm_json_member(doc, (int)t, "kind");
        (void)kvm_json_element(doc, (int)t, 0);
        (void)kvm_json_element(doc, (int)t, 7);
        (void)kvm_json_key(doc, (int)t, 0);
        (void)kvm_json_keys_allowed(doc, (int)t, allowed);
        (void)kvm_json_str_eq(doc, (int)t, "mouse.move");
        (void)kvm_json_str_copy(doc, (int)t, tmp, sizeof(tmp));
        (void)kvm_json_get_i64(doc, (int)t, &iv);
        (void)kvm_json_get_bool(doc, (int)t, &bv);
    }
}

static void dump(const char *buf, size_t len)
{
    fprintf(stderr, "    %zu bytes:", len);
    for (size_t i = 0; i < len && i < 256u; i++) {
        fprintf(stderr, " %02x", (unsigned)(unsigned char)buf[i]);
    }
    fprintf(stderr, "\n");
}

static void ensure_session(rph_cli_t *c)
{
    if (kvm_rp_slot_of(&g_h.rp, SID) < 0) {
        rph_cli_open(c, SID, true);
    }
    if (g_h.rp.owner_kind != OWNER_REMOTE || g_h.rp.owner_session != SID) {
        g_h.request_result = KVM_RP_OK;
        (void)rph_cli_grant(c);
    }
}

int main(int argc, char **argv)
{
    uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 0) : 20261102ull;
    unsigned long iters = (argc > 2) ? strtoul(argv[2], NULL, 0) : 200000ul;
    rng_state = seed ? seed : 1u;
    printf("fuzz_remote_frame seed=%llu iterations=%lu\n", (unsigned long long)seed, iters);

    kvm_json_doc_t doc;
    char buf[FZ_BUF];
    char tpl[FZ_BUF];
    unsigned long accepted = 0;
    unsigned long rejected = 0;
    unsigned long parse_ok = 0;
    unsigned long failures = 0;
    uint64_t seq = 0;

    rph_reset(NULL);
    rph_cli_t c;
    rph_cli_open(&c, SID, true);
    ensure_session(&c);

    for (unsigned long it = 0; it < iters; it++) {
        if ((it % 256ul) == 0ul || seq > 9000000000000000ull) {
            // start from a clean instance so the seq high-water mark cannot starve the accept path
            rph_reset(NULL);
            rph_cli_open(&c, SID, true);
            seq = 1;
        }
        ensure_session(&c);
        int slot = kvm_rp_slot_of(&g_h.rp, SID);
        if (slot >= 0 && g_h.rp.s[slot].last_seq >= seq) {
            seq = g_h.rp.s[slot].last_seq; // a mutation may have pushed the high-water mark up
        }

        size_t len;
        if (rnd_below(24u) == 0u) {
            // fully random bytes
            len = 1u + rnd_below(1100u);
            for (size_t i = 0; i < len; i++) {
                buf[i] = (char)rnd_below(256);
            }
        } else {
            seq++;
            int n = snprintf(tpl, sizeof(tpl), corpus[rnd_below(CORPUS_N)],
                             (unsigned long long)seq, c.label, g_h.rp.owner_epoch);
            if (n < 0) {
                n = 0;
            }
            len = (size_t)n;
            if (len >= FZ_BUF) {
                len = FZ_BUF - 1u;
            }
            memcpy(buf, tpl, len);
            // a share of the inputs stays pristine, so the accept path is exercised as well
            if (rnd_below(4u) != 0u) {
                uint32_t rounds = 1u + rnd_below(3u);
                for (uint32_t r = 0; r < rounds; r++) {
                    len = mutate(buf, len);
                }
            }
        }

        // the standalone reader must survive the same bytes
        kvm_json_err_t je = kvm_json_parse(&doc, buf, len);
        if (je == KVM_JSON_OK) {
            parse_ok++;
            walk(&doc);
        } else if (doc.ntok != 0u) {
            fprintf(stderr, "FAIL seed=%llu it=%lu: rejected document still exposes tokens\n",
                    (unsigned long long)seed, it);
            dump(buf, len);
            failures++;
            break;
        }

        int before = g_h.n_intent;
        kvm_rp_err_t e = kvm_rp_on_frame(&g_h.rp, SID, buf, len, g_h.now);
        if (e != KVM_RP_OK) {
            rejected++;
            if (g_h.n_intent != before) {
                fprintf(stderr, "FAIL seed=%llu it=%lu: %s produced %d HID intent(s)\n",
                        (unsigned long long)seed, it, kvm_rp_err_name(e), g_h.n_intent - before);
                dump(buf, len);
                failures++;
                break;
            }
        } else {
            accepted++;
            if (g_h.n_intent - before > 1) {
                fprintf(stderr, "FAIL seed=%llu it=%lu: one frame produced %d intents\n",
                        (unsigned long long)seed, it, g_h.n_intent - before);
                dump(buf, len);
                failures++;
                break;
            }
        }
        if (g_h.n_intent > RPH_MAX_INTENTS - 8) {
            g_h.n_intent = 0; // the recorder is bounded; the invariant is per frame
        }

        g_h.now += 100000u; // roll the rate window and drive the timers
        kvm_rp_poll(&g_h.rp, g_h.now);
    }

    printf("fuzz_remote_frame: %lu inputs, %lu accepted, %lu rejected, %lu parsed, %lu failures\n",
           iters, accepted, rejected, parse_ok, failures);
    if (accepted == 0ul) {
        fprintf(stderr, "FAIL: the accept path was never reached, the run proves nothing\n");
        failures++;
    }
    return failures == 0ul ? EXIT_SUCCESS : EXIT_FAILURE;
}
