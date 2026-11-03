// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-080 / ADR-015 / ADR-017: Wi-Fi STA link state machine. Fake clock, seeded jitter.
#include <string.h>

#include "kvm_test.h"
#include "net_fsm.h"

static uint32_t s_seed = 1;

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    s_seed = s_seed * 1103515245u + 12345u;
    return s_seed >> 8;
}

static net_fsm_t f;
static net_actions_t out;
static uint64_t now_us;

static void ev(net_ev_kind_t k)
{
    net_event_t e = {.kind = k, .now_us = now_us};
    net_fsm_handle(&f, &e, &out);
}

static void start(bool creds, const net_cache_t *cache)
{
    net_event_t e = {.kind = NET_EV_START, .now_us = now_us, .has_credentials = creds};
    if (cache != NULL) {
        e.cache = *cache;
    }
    net_fsm_handle(&f, &e, &out);
}

static void assoc(uint8_t last_byte, uint8_t ch)
{
    net_event_t e = {.kind = NET_EV_ASSOCIATED, .now_us = now_us, .channel = ch};
    const uint8_t b[6] = {0x02, 0x11, 0x22, 0x33, 0x44, last_byte};
    memcpy(e.bssid, b, 6);
    net_fsm_handle(&f, &e, &out);
}

static void disc(uint16_t reason)
{
    net_event_t e = {.kind = NET_EV_DISCONNECTED, .now_us = now_us, .reason = reason};
    net_fsm_handle(&f, &e, &out);
}

static void advance(uint64_t us)
{
    now_us += us;
    ev(NET_EV_TICK);
}

static bool has(net_act_kind_t k)
{
    for (uint8_t i = 0; i < out.n; i++) {
        if (out.a[i].kind == k) {
            return true;
        }
    }
    return false;
}

static void fresh(void)
{
    net_fsm_init(&f, NULL, rnd, NULL);
    now_us = 1000000;
    s_seed = 1;
}

static void test_not_configured_then_credentials(void)
{
    fresh();
    CHECK_EQ(f.link, NET_LINK_OFF);
    start(false, NULL);
    CHECK_EQ(f.link, NET_LINK_NOT_CONFIGURED);
    CHECK_EQ(out.n, 0); // local-only: no radio activity requested
    advance(100000000);
    CHECK_EQ(out.n, 0);
    net_status_t st;
    net_fsm_status(&f, &st);
    CHECK(!st.ip_present && !st.configured);
    ev(NET_EV_CREDENTIALS_SET);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
    CHECK(has(NET_ACT_CACHE_INVALIDATE) && has(NET_ACT_CONNECT_FULL));
    CHECK(!has(NET_ACT_DISCONNECT)); // nothing was running
}

static void test_full_connect_stores_cache(void)
{
    fresh();
    start(true, NULL);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
    CHECK(has(NET_ACT_CONNECT_FULL));
    assoc(0x55, 6);
    CHECK_EQ(f.link, NET_LINK_ASSOCIATED);
    net_status_t st;
    net_fsm_status(&f, &st);
    CHECK(!st.ip_present); // associated is not "has an IP"
    ev(NET_EV_GOT_IP);
    CHECK_EQ(f.link, NET_LINK_CONNECTED);
    CHECK_EQ(out.n, 1);
    CHECK_EQ(out.a[0].kind, NET_ACT_CACHE_STORE);
    CHECK_EQ(out.a[0].cache.channel, 6);
    CHECK_EQ(out.a[0].cache.bssid[5], 0x55);
    CHECK(out.a[0].cache.valid);
    net_fsm_status(&f, &st);
    CHECK(st.ip_present && st.configured);
    CHECK_EQ(st.channel, 6);
    CHECK_EQ(st.st.connects, 1);
}

static void test_cached_connect_same_record_no_write(void)
{
    fresh();
    net_cache_t c = {.bssid = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}, .channel = 11, .valid = true};
    start(true, &c);
    CHECK_EQ(f.link, NET_LINK_CONNECTING_CACHED);
    CHECK_EQ(out.a[0].kind, NET_ACT_CONNECT_CACHED);
    CHECK_EQ(out.a[0].cache.channel, 11);
    assoc(0x55, 11);
    ev(NET_EV_GOT_IP);
    CHECK_EQ(f.link, NET_LINK_CONNECTED);
    CHECK_EQ(out.n, 0); // unchanged record: no flash write (TECH-050 debounce spirit)
    // an invalid cache record is ignored (bad channel)
    fresh();
    net_cache_t bad = {.channel = 0, .valid = true};
    start(true, &bad);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
}

static void test_cached_timeout_falls_back_and_ignores_echo(void)
{
    fresh();
    net_cache_t c = {.bssid = {1, 2, 3, 4, 5, 6}, .channel = 1, .valid = true};
    start(true, &c);
    advance(2999999);
    CHECK_EQ(f.link, NET_LINK_CONNECTING_CACHED);
    advance(1);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
    CHECK(has(NET_ACT_DISCONNECT) && has(NET_ACT_CACHE_INVALIDATE) && has(NET_ACT_CONNECT_FULL));
    CHECK_EQ(out.a[0].kind, NET_ACT_DISCONNECT); // abort first, then connect
    disc(2);                                     // the driver echoes our abort
    CHECK_EQ(f.link, NET_LINK_CONNECTING);       // not a failure of the full attempt
    CHECK_EQ(f.st.echoes, 1);
    assoc(0x77, 9);
    ev(NET_EV_GOT_IP);
    CHECK(has(NET_ACT_CACHE_STORE));
    CHECK_EQ(out.a[0].cache.channel, 9);
    CHECK_EQ(f.st.cached_fallbacks, 1);
}

static void test_cached_reject_goes_full_without_backoff(void)
{
    fresh();
    net_cache_t c = {.bssid = {1, 2, 3, 4, 5, 6}, .channel = 1, .valid = true};
    start(true, &c);
    disc(201); // AP moved channel / gone (AT-EXT-02)
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
    CHECK(has(NET_ACT_CACHE_INVALIDATE) && has(NET_ACT_CONNECT_FULL));
    CHECK(!f.cache.valid);
}

static void test_backoff_doubles_and_caps(void)
{
    net_fsm_cfg_t cfg = NET_FSM_CFG_DEFAULT;
    net_fsm_init(&f, &cfg, rnd, NULL);
    now_us = 0;
    start(true, NULL);
    uint64_t prev = 0;
    for (int i = 1; i <= 12; i++) {
        disc(15); // 4-way handshake timeout (wrong password)
        CHECK_EQ(f.link, NET_LINK_BACKOFF);
        const uint64_t delay = f.deadline_us - now_us;
        uint64_t base = cfg.backoff_min_us;
        for (int k = 1; k < i && base < cfg.backoff_max_us; k++) {
            base *= 2;
        }
        if (base > cfg.backoff_max_us) {
            base = cfg.backoff_max_us;
        }
        CHECK(delay >= base && delay <= base + base / 4);
        CHECK(delay >= prev || base == cfg.backoff_max_us);
        prev = base;
        advance(delay - 1);
        CHECK_EQ(f.link, NET_LINK_BACKOFF); // no attempt before the deadline: no busy loop (FR-406)
        CHECK_EQ(out.n, 0);
        advance(1);
        CHECK_EQ(f.link, NET_LINK_CONNECTING);
        CHECK(has(NET_ACT_CONNECT_FULL));
    }
    CHECK_EQ(f.failures, 12);
    // a success resets the backoff
    assoc(1, 1);
    ev(NET_EV_GOT_IP);
    CHECK_EQ(f.failures, 0);
    // no jitter source: exact values
    net_fsm_init(&f, &cfg, NULL, NULL);
    CHECK_EQ(net_fsm_backoff_us(&f, 1), cfg.backoff_min_us);
    CHECK_EQ(net_fsm_backoff_us(&f, 3), cfg.backoff_min_us * 4);
    CHECK_EQ(net_fsm_backoff_us(&f, 40), cfg.backoff_max_us);
}

static void test_drop_retries_cached_at_once(void)
{
    fresh();
    start(true, NULL);
    assoc(0x10, 3);
    ev(NET_EV_GOT_IP);
    disc(8); // AP left
    CHECK_EQ(f.link, NET_LINK_CONNECTING_CACHED); // the just-cached record, no backoff
    CHECK_EQ(out.a[0].kind, NET_ACT_CONNECT_CACHED);
    CHECK_EQ(out.a[0].cache.channel, 3);
}

static void test_dhcp_timeout_and_lost_ip(void)
{
    fresh();
    start(true, NULL);
    assoc(1, 1);
    advance(10000000);
    CHECK_EQ(f.link, NET_LINK_BACKOFF);
    CHECK(has(NET_ACT_DISCONNECT));
    CHECK_EQ(f.st.timeouts, 1);
    disc(8); // echo arrives in BACKOFF: ignored
    CHECK_EQ(f.link, NET_LINK_BACKOFF);
    advance(2000000);
    assoc(1, 1);
    ev(NET_EV_GOT_IP);
    ev(NET_EV_LOST_IP);
    CHECK_EQ(f.link, NET_LINK_ASSOCIATED);
    net_status_t st;
    net_fsm_status(&f, &st);
    CHECK(!st.ip_present);
    ev(NET_EV_GOT_IP);
    CHECK_EQ(f.link, NET_LINK_CONNECTED);
}

static void test_full_connect_timeout(void)
{
    fresh();
    start(true, NULL);
    advance(15000000);
    CHECK_EQ(f.link, NET_LINK_BACKOFF);
    CHECK(has(NET_ACT_DISCONNECT));
}

static void test_stop_from_every_state(void)
{
    for (int target = NET_LINK_NOT_CONFIGURED; target < NET_LINK_COUNT; target++) {
        fresh();
        net_cache_t c = {.bssid = {1}, .channel = 1, .valid = true};
        switch ((net_link_t)target) {
        case NET_LINK_NOT_CONFIGURED:
            start(false, NULL);
            break;
        case NET_LINK_CONNECTING_CACHED:
            start(true, &c);
            break;
        case NET_LINK_CONNECTING:
            start(true, NULL);
            break;
        case NET_LINK_ASSOCIATED:
            start(true, NULL);
            assoc(1, 1);
            break;
        case NET_LINK_CONNECTED:
            start(true, NULL);
            assoc(1, 1);
            ev(NET_EV_GOT_IP);
            break;
        case NET_LINK_BACKOFF:
            start(true, NULL);
            disc(15);
            break;
        default:
            break;
        }
        CHECK_EQ(f.link, (net_link_t)target);
        ev(NET_EV_STOP);
        CHECK_EQ(f.link, NET_LINK_OFF);
        CHECK(has(NET_ACT_RADIO_STOP));
        net_status_t st;
        net_fsm_status(&f, &st);
        CHECK(!st.ip_present);
        CHECK_EQ(st.channel, 0);
        ev(NET_EV_GOT_IP); // late driver events after the stop are ignored
        disc(8);
        advance(100000000);
        CHECK_EQ(f.link, NET_LINK_OFF);
        CHECK_EQ(out.n, 0);
    }
    // restart after a stop
    fresh();
    start(true, NULL);
    ev(NET_EV_STOP);
    start(true, NULL);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
}

static void test_credentials_cleared_while_connected(void)
{
    fresh();
    start(true, NULL);
    assoc(1, 6);
    ev(NET_EV_GOT_IP);
    ev(NET_EV_CREDENTIALS_CLEARED);
    CHECK_EQ(f.link, NET_LINK_NOT_CONFIGURED);
    CHECK(has(NET_ACT_DISCONNECT) && has(NET_ACT_CACHE_INVALIDATE));
    disc(8);
    CHECK_EQ(f.link, NET_LINK_NOT_CONFIGURED);
    advance(100000000);
    CHECK_EQ(out.n, 0);
    // a new network replaces the old one: cache invalidated, full connect
    ev(NET_EV_CREDENTIALS_SET);
    CHECK_EQ(f.link, NET_LINK_CONNECTING);
}

static void test_no_secret_in_records(void)
{
    // AT-EXT-03 (structure): the cache record and the status carry no string field that could hold an SSID or a key.
    CHECK(sizeof(net_cache_t) <= 8);
    net_status_t st;
    memset(&st, 0, sizeof(st));
    CHECK(sizeof(st.link) + sizeof(st.ip_present) + sizeof(st.configured) + sizeof(st.channel) +
              sizeof(st.last_reason) + sizeof(st.consecutive_failures) + sizeof(st.backoff_until_us) + sizeof(st.st) <=
          sizeof(st));
}

// 200 000 random events (no credential changes): invariants after every step.
static void test_random_storm(void)
{
    for (uint32_t seed = 1; seed <= 4; seed++) {
        fresh();
        s_seed = seed;
        uint32_t x = seed * 2654435761u;
        net_cache_t c = {.bssid = {9, 9, 9, 9, 9, 9}, .channel = 6, .valid = true};
        start(true, &c);
        int attempts_since_rest = 1;
        uint64_t last_attempt_us = now_us;
        int burst = 0;
        int violations = 0;
        int v_ip = 0, v_deadline = 0, v_backoff = 0, v_attempts = 0;
        for (int i = 0; i < 50000; i++) {
            x = x * 1664525u + 1013904223u;
            const uint32_t r = x >> 8;
            now_us += r % 400000;
            switch (r % 9) {
            case 0:
            case 1:
                ev(NET_EV_TICK);
                break;
            case 2:
                assoc((uint8_t)(r >> 3), (uint8_t)(1 + (r >> 5) % 13));
                break;
            case 3:
            case 4:
                disc((uint16_t)(r % 300));
                break;
            case 5:
                ev(NET_EV_GOT_IP);
                break;
            case 6:
                ev(NET_EV_LOST_IP);
                break;
            case 7:
                if ((r >> 4) % 50 == 0) {
                    ev(NET_EV_STOP);
                    start(true, &c);
                    attempts_since_rest = 0;
                } else {
                    ev(NET_EV_TICK);
                }
                break;
            default:
                ev(NET_EV_TICK);
                break;
            }
            net_status_t st;
            net_fsm_status(&f, &st);
            if (st.ip_present != (f.link == NET_LINK_CONNECTED)) {
                violations++;
                v_ip++;
            }
            if ((f.link == NET_LINK_CONNECTING || f.link == NET_LINK_CONNECTING_CACHED ||
                 f.link == NET_LINK_ASSOCIATED || f.link == NET_LINK_BACKOFF) &&
                f.deadline_us == 0) {
                violations++; // every transient state has a way out
                v_deadline++;
            }
            if (f.link == NET_LINK_BACKOFF && f.deadline_us > now_us && // past the deadline: waits for the next tick
                f.deadline_us - now_us > f.cfg.backoff_max_us + f.cfg.backoff_max_us / 4) {
                violations++;
                v_backoff++;
            }
            for (uint8_t k = 0; k < out.n; k++) {
                if (out.a[k].kind == NET_ACT_CONNECT_FULL || out.a[k].kind == NET_ACT_CONNECT_CACHED) {
                    attempts_since_rest++;
                    if (now_us - last_attempt_us < 1000) {
                        burst++;
                    }
                    last_attempt_us = now_us;
                }
            }
            if (f.link == NET_LINK_BACKOFF || f.link == NET_LINK_CONNECTED) {
                attempts_since_rest = 0;
            }
            // without a backoff or a success in between: at most cached + full (FR-406)
            if (attempts_since_rest > 2) {
                violations++;
                if (v_attempts++ == 0) {
                    printf("    first attempts violation at step %d: link=%s\n", i, net_link_name(f.link));
                }
            }
        }
        printf("    seed %u: connects=%u disconnects=%u timeouts=%u fallbacks=%u echoes=%u ignored=%u\n",
               (unsigned)seed, (unsigned)f.st.connects, (unsigned)f.st.disconnects, (unsigned)f.st.timeouts,
               (unsigned)f.st.cached_fallbacks, (unsigned)f.st.echoes, (unsigned)f.st.ignored);
        printf("    violations: ip=%d deadline=%d backoff=%d attempts=%d\n", v_ip, v_deadline, v_backoff, v_attempts);
        CHECK_EQ(violations, 0);
        CHECK(f.st.connects > 0);
        (void)burst;
    }
}

int main(void)
{
    RUN(test_not_configured_then_credentials);
    RUN(test_full_connect_stores_cache);
    RUN(test_cached_connect_same_record_no_write);
    RUN(test_cached_timeout_falls_back_and_ignores_echo);
    RUN(test_cached_reject_goes_full_without_backoff);
    RUN(test_backoff_doubles_and_caps);
    RUN(test_drop_retries_cached_at_once);
    RUN(test_dhcp_timeout_and_lost_ip);
    RUN(test_full_connect_timeout);
    RUN(test_stop_from_every_state);
    RUN(test_credentials_cleared_while_connected);
    RUN(test_no_secret_in_records);
    RUN(test_random_storm);
    TEST_MAIN_END("test_net_fsm");
}
