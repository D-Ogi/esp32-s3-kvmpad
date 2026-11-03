// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the pairing window state machine (TASK-100; FR-721..727, SEC-101..106,
// AT-EXT-13/15/16, AT-SEC-06). The clock is injected, so every deadline is exercised exactly rather
// than waited out.

#include <stdlib.h>
#include <string.h>

#include "kvm_test.h"
#include "pair_harness.h"

// --- code generation -----------------------------------------------------------------------------

static void test_code_is_six_uniform_digits(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 100), KVM_PAIR_OK);

    // Draw many codes and check the shape, the full range and a rough uniformity of the leading
    // digit. FR-722 forbids excluding "ugly" patterns, so 000000 must be reachable.
    unsigned lead[10] = { 0 };
    bool seen_low = false, seen_high = false;
    for (int i = 0; i < 4000; i++) {
        kvm_pair_out_t out;
        CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
        const char *code = kvm_pair_window_code(&h.device);
        CHECK(code != NULL);
        CHECK_EQ(strlen(code), KVM_PAIR_CODE_LEN);
        for (size_t j = 0; j < KVM_PAIR_CODE_LEN; j++) {
            CHECK(code[j] >= '0' && code[j] <= '9');
        }
        lead[(unsigned)(code[0] - '0')]++;
        const long v = strtol(code, NULL, 10);
        if (v < 100000) {
            seen_low = true; // a leading zero really happens
        }
        if (v > 900000) {
            seen_high = true;
        }
        CHECK_EQ(kvm_pair_local_cancel(&h.device, &out), KVM_PAIR_OK);
    }
    CHECK(seen_low);
    CHECK(seen_high);
    for (int d = 0; d < 10; d++) {
        // 4000 draws, expectation 400 per leading digit; a band of 250..600 catches a modulo bias
        // (which would skew the low digits) without being flaky.
        CHECK(lead[d] > 250 && lead[d] < 600);
    }
    harness_free(&h);
}

static void test_rng_failure_blocks_window(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 101), KVM_PAIR_OK);
    kvm_pair_preconditions_t pre = { .remote_enabled = true, .wifi_connected = true };

    det_rng_fail_next(1);
    CHECK_EQ(kvm_pair_window_open(&h.device, &pre), KVM_PAIR_ERR_CRYPTO);
    CHECK(kvm_pair_window_code(&h.device) == NULL);
    CHECK(!kvm_pair_endpoint_open(&h.device));

    CHECK_EQ(kvm_pair_window_open(&h.device, &pre), KVM_PAIR_OK);
    harness_free(&h);
}

// --- preconditions -------------------------------------------------------------------------------

static void test_preconditions(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 102), KVM_PAIR_OK);

    kvm_pair_preconditions_t off = { .remote_enabled = false, .wifi_connected = true };
    CHECK_EQ(kvm_pair_window_open(&h.device, &off), KVM_PAIR_ERR_PRECONDITION);
    kvm_pair_preconditions_t no_wifi = { .remote_enabled = true, .wifi_connected = false };
    CHECK_EQ(kvm_pair_window_open(&h.device, &no_wifi), KVM_PAIR_ERR_PRECONDITION);
    CHECK(!kvm_pair_endpoint_open(&h.device));
    CHECK(kvm_pair_window_code(&h.device) == NULL);

    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    CHECK(kvm_pair_endpoint_open(&h.device));
    // A second open while one is running is refused, not silently restarted.
    kvm_pair_preconditions_t ok = { .remote_enabled = true, .wifi_connected = true };
    CHECK_EQ(kvm_pair_window_open(&h.device, &ok), KVM_PAIR_ERR_BUSY);
    harness_free(&h);
}

// --- the full successful path --------------------------------------------------------------------

static void test_full_pairing_require_accept(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 103), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_event_t ev;
    CHECK(kvm_pair_take_event(&h.device, &ev));
    CHECK_EQ(ev.kind, KVM_PAIR_EVENT_WINDOW_OPENED);

    int step = -1;
    kvm_pair_out_t out;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(step, 0);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_CONFIRM);
    CHECK_EQ(out.blob_len, KVM_PAIR_CONFIRM_LEN);
    CHECK_EQ(out.token_len, 0); // no token before LOCAL accepts
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_AWAIT_ACCEPT);
    CHECK_EQ(h.store.count, 0);

    // FR-725: the Accept screen names the client.
    CHECK(kvm_pair_take_event(&h.device, &ev));
    CHECK_EQ(ev.kind, KVM_PAIR_EVENT_ACCEPT_SCREEN);
    CHECK(strcmp(ev.client_name, "DESK1") == 0);
    CHECK(ev.remaining_ms <= KVM_PAIR_ACCEPT_TIMEOUT_MS);

    det_clock_advance(2000);
    CHECK_EQ(kvm_pair_local_accept(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ACCEPTED);
    CHECK_EQ(out.token_len, KVM_PAIR_TOKEN_SEALED_LEN);
    CHECK(out.close_after);
    memcpy(h.sealed, out.token, out.token_len);
    h.sealed_len = out.token_len;

    // The CLIENT opens the token, and the DEVICE kept only its hash (SEC-105).
    CHECK_EQ(harness_open_token(&h), KVM_PAIR_OK);
    CHECK_EQ(h.store.count, 1);
    CHECK(strcmp(h.store.rec[0].name, "DESK1") == 0);
    CHECK(h.store.rec[0].paired_at_unix != 0);

    uint8_t want_hash[KVM_PAIR_SHA256_LEN];
    CHECK_EQ(kvm_pair_sha256(h.token, KVM_PAIR_TOKEN_LEN, want_hash), KVM_PAIR_OK);
    CHECK(memcmp(h.store.rec[0].token_sha256, want_hash, sizeof(want_hash)) == 0);
    // The register must not hold the token itself anywhere.
    CHECK(memcmp(h.store.rec[0].token_sha256, h.token, KVM_PAIR_TOKEN_LEN) != 0);

    // The code is gone from RAM as soon as the window finished (SEC-105).
    CHECK(kvm_pair_window_code(&h.device) == NULL);
    CHECK_EQ(h.device.code[0], '\0');

    // Token verification, and a token that was never issued.
    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, h.token), 0);
    uint8_t bogus[KVM_PAIR_TOKEN_LEN];
    memset(bogus, 0x00, sizeof(bogus));
    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, bogus), -1);

    // The toast stands for KVM_PAIR_TOAST_MS and then the window closes (FR-725).
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_COMPLETED);
    det_clock_advance(KVM_PAIR_TOAST_MS - 1u);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_COMPLETED);
    det_clock_advance(2);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK(!kvm_pair_endpoint_open(&h.device));

    CHECK_EQ(h.device.counters.pair_attempts, 1);
    CHECK_EQ(h.device.counters.pair_successes, 1);
    CHECK_EQ(h.device.counters.pair_failures, 0);
    harness_free(&h);
}

static void test_require_accept_off(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, false, 104), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    int step = -1;
    kvm_pair_out_t out;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    // ADR-023 with the policy off: one result carries the confirmation and the token.
    CHECK_EQ(out.kind, KVM_PAIR_OUT_CONFIRM_ACCEPTED);
    CHECK_EQ(out.blob_len, KVM_PAIR_CONFIRM_LEN);
    CHECK_EQ(out.token_len, KVM_PAIR_TOKEN_SEALED_LEN);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_COMPLETED);
    CHECK_EQ(h.store.count, 1);

    CHECK_EQ(harness_open_token(&h), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, h.token), 0);

    // The toast carries the name and the Remove affordance (FR-725).
    kvm_pair_event_t ev;
    bool saw_toast = false;
    while (kvm_pair_take_event(&h.device, &ev)) {
        if (ev.kind == KVM_PAIR_EVENT_PAIRED_TOAST) {
            saw_toast = true;
            CHECK(strcmp(ev.client_name, "DESK1") == 0);
            CHECK_EQ(ev.remaining_ms, KVM_PAIR_TOAST_MS);
        }
    }
    CHECK(saw_toast);
    harness_free(&h);
}

// --- wrong code, closures, lockout ---------------------------------------------------------------

// One failed run with a wrong code. Returns the error the DEVICE reported.
static kvm_pair_err_t attempt_wrong_code(pair_harness_t *h, kvm_pair_out_t *out)
{
    int step = -1;
    kvm_pair_err_t err = harness_run_to_confirm(h, "000000", h->cert_sha, &step, out);
    kvm_pair_client_free(&h->client);
    return err;
}

static void test_three_wrong_codes_close_window(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 105), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_out_t out;
    // Attempts 1 and 2 leave the window open with the same code (SEC-104).
    CHECK_EQ(attempt_wrong_code(&h, &out), KVM_PAIR_ERR_BAD_CODE);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ERROR);
    CHECK(out.close_after);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);
    CHECK(kvm_pair_window_code(&h.device) != NULL);

    CHECK_EQ(attempt_wrong_code(&h, &out), KVM_PAIR_ERR_BAD_CODE);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);

    // The third closes the window and wipes the code.
    CHECK_EQ(attempt_wrong_code(&h, &out), KVM_PAIR_ERR_BAD_CODE);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK(kvm_pair_window_code(&h.device) == NULL);
    CHECK_EQ(h.device.code[0], '\0');
    CHECK(!kvm_pair_endpoint_open(&h.device));
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_TOO_MANY_FAILURES);
    CHECK_EQ(h.device.counters.pair_failures, 3);
    CHECK_EQ(h.device.counters.windows_closed_on_failures, 1);
    CHECK_EQ(h.store.count, 0);

    // A `pair.hello` after the window closed gets ERR_PAIRING_EXPIRED and nothing else (AT-EXT-16).
    kvm_pair_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.v = KVM_PAIR_WIRE_VERSION;
    hello.pake = KVM_PAIR_PAKE_NAME;
    memcpy(hello.client_name, "DESK1", 6);
    memcpy(hello.cert_sha256, h.cert_sha, sizeof(hello.cert_sha256));
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_ERR_EXPIRED);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ERROR);
    CHECK(strcmp(out.wire_code, "ERR_PAIRING_EXPIRED") == 0);
    harness_free(&h);
}

static void test_three_closures_cause_lockout(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 106), KVM_PAIR_OK);
    kvm_pair_out_t out;

    for (int window = 0; window < 3; window++) {
        CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
        for (int a = 0; a < 3; a++) {
            CHECK_EQ(attempt_wrong_code(&h, &out), KVM_PAIR_ERR_BAD_CODE);
        }
        det_clock_advance(1000); // the three closures land well inside the 10-minute period
    }
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_LOCKED);
    CHECK_EQ(h.device.counters.pair_lockouts, 1);
    CHECK_EQ(h.device.counters.pair_failures, 9);

    // No new window during the lockout, and `pair.hello` still only sees ERR_PAIRING_EXPIRED.
    kvm_pair_preconditions_t pre = { .remote_enabled = true, .wifi_connected = true };
    CHECK_EQ(kvm_pair_window_open(&h.device, &pre), KVM_PAIR_ERR_LOCKED);
    kvm_pair_hello_t hello;
    memset(&hello, 0, sizeof(hello));
    hello.v = KVM_PAIR_WIRE_VERSION;
    hello.pake = KVM_PAIR_PAKE_NAME;
    memcpy(hello.client_name, "DESK1", 6);
    memcpy(hello.cert_sha256, h.cert_sha, sizeof(hello.cert_sha256));
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_ERR_LOCKED);
    CHECK(strcmp(out.wire_code, "ERR_PAIRING_EXPIRED") == 0);

    // The warning event fired (SEC-104).
    kvm_pair_event_t ev;
    bool saw_lockout = false;
    while (kvm_pair_take_event(&h.device, &ev)) {
        if (ev.kind == KVM_PAIR_EVENT_LOCKOUT) {
            saw_lockout = true;
            CHECK_EQ(ev.remaining_ms, KVM_PAIR_LOCKOUT_MS);
        }
    }
    CHECK(saw_lockout);

    // It ends after exactly 10 minutes, not before. The deadline is read from the session rather
    // than recomputed, so the test does not have to track the advances the loop above made.
    const uint64_t until = h.device.lockout_until_ms;
    CHECK(until > det_clock_now());
    det_clock_advance(until - det_clock_now() - 1u);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_LOCKED);
    CHECK_EQ(kvm_pair_window_open(&h.device, &pre), KVM_PAIR_ERR_LOCKED);
    det_clock_advance(2);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(kvm_pair_window_open(&h.device, &pre), KVM_PAIR_OK);
    harness_free(&h);
}

static void test_closures_outside_period_do_not_lock(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 107), KVM_PAIR_OK);
    kvm_pair_out_t out;

    for (int window = 0; window < 3; window++) {
        CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
        for (int a = 0; a < 3; a++) {
            CHECK_EQ(attempt_wrong_code(&h, &out), KVM_PAIR_ERR_BAD_CODE);
        }
        // Spread the closures past the 10-minute observation period.
        det_clock_advance(KVM_PAIR_CLOSURE_WINDOW_MS + 1000u);
    }
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.counters.pair_lockouts, 0);
    harness_free(&h);
}

// --- expiry and the hard window cap --------------------------------------------------------------

static void test_code_expires_at_120s(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 108), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_out_t out;
    det_clock_advance(KVM_PAIR_CODE_TTL_MS - 1u);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);
    CHECK_EQ(kvm_pair_code_remaining_ms(&h.device), 1);

    det_clock_advance(1);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_CODE_EXPIRED);
    CHECK_EQ(h.device.code[0], '\0');

    // Using the expired code now gets ERR_PAIRING_EXPIRED (03 §7).
    int step = -1;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_ERR_EXPIRED);
    CHECK_EQ(step, 1);
    CHECK(strcmp(out.wire_code, "ERR_PAIRING_EXPIRED") == 0);
    harness_free(&h);
}

static void test_accept_timeout(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 109), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    // Reach the Accept screen late in the code's life, so the decision time is what ends the window
    // (FR-725).
    det_clock_advance(KVM_PAIR_CODE_TTL_MS - 5000u);
    int step = -1;
    kvm_pair_out_t out;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_AWAIT_ACCEPT);

    kvm_pair_event_t ev;
    bool saw_screen = false;
    while (kvm_pair_take_event(&h.device, &ev)) {
        if (ev.kind == KVM_PAIR_EVENT_ACCEPT_SCREEN) {
            saw_screen = true;
            // 115 s gone, so 65 s of the cap remain -- longer than the 30 s decision time.
            CHECK_EQ(ev.remaining_ms, KVM_PAIR_ACCEPT_TIMEOUT_MS);
        }
    }
    CHECK(saw_screen);

    // Hold the Accept screen. The decision timeout fires first here, at 30 s.
    det_clock_advance(KVM_PAIR_ACCEPT_TIMEOUT_MS);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_REJECTED);
    CHECK(strcmp(out.reason, "timeout") == 0);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_ACCEPT_TIMEOUT);
    CHECK_EQ(h.store.count, 0);
    harness_free(&h);
}

static void test_window_cap_is_the_backstop(void)
{
    pair_harness_t h;
    kvm_pair_out_t out;
    int step = -1;

    // Worst legitimate timing: the code is used in its final 100 ms, so the Accept screen starts
    // just under 120 s in. 120 s + 30 s = 150 s, which is why FR-722's 180 s cap can only ever be a
    // backstop and never the deadline a patient user runs into.
    CHECK_EQ(harness_init(&h, true, 110), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    const uint64_t opened = det_clock_now();
    det_clock_advance(KVM_PAIR_CODE_TTL_MS - 100u);
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_AWAIT_ACCEPT);
    CHECK(kvm_pair_window_remaining_ms(&h.device) > KVM_PAIR_ACCEPT_TIMEOUT_MS);

    det_clock_advance(KVM_PAIR_ACCEPT_TIMEOUT_MS);
    CHECK_EQ(kvm_pair_tick(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_REJECTED);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK(det_clock_now() - opened < KVM_PAIR_WINDOW_MAX_MS);
    harness_free(&h);

    // The backstop itself: a caller that stops ticking and comes back past 180 s finds the window
    // closed on the cap, and the cap is reported ahead of the decision timeout.
    CHECK_EQ(harness_init(&h, true, 111), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_AWAIT_ACCEPT);
    det_clock_advance(KVM_PAIR_WINDOW_MAX_MS);
    CHECK_EQ(kvm_pair_window_remaining_ms(&h.device), 0);
    CHECK_EQ(kvm_pair_local_accept(&h.device, &out), KVM_PAIR_ERR_STATE);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_WINDOW_CAP);
    CHECK_EQ(h.store.count, 0);
    harness_free(&h);
}

static void test_cap_closes_mid_run(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 111), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_hello_t hello;
    kvm_pair_out_t out;
    CHECK_EQ(kvm_pair_client_begin(&h.client, h.code, "DESK1", h.client_id, h.cert_sha, &hello),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_RUNNING);

    // The cap expires while the run is mid-flight: the next message is refused and the endpoint
    // closes no later than 180 s (AT-EXT-15).
    det_clock_advance(KVM_PAIR_WINDOW_MAX_MS);
    size_t c_r1_len = 0;
    CHECK_EQ(kvm_pair_client_round1(&h.client, out.blob, out.blob_len, h.c_r1, sizeof(h.c_r1),
                                    &c_r1_len),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_round1(&h.device, h.c_r1, c_r1_len, &out), KVM_PAIR_ERR_EXPIRED);
    CHECK(strcmp(out.wire_code, "ERR_PAIRING_EXPIRED") == 0);
    CHECK(out.close_after);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK(!kvm_pair_endpoint_open(&h.device));
    harness_free(&h);
}

// --- channel binding -----------------------------------------------------------------------------

static void test_cert_mismatch_aborts_without_storing(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 112), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    uint8_t other[KVM_PAIR_SHA256_LEN];
    memset(other, 0x7e, sizeof(other));

    int step = -1;
    kvm_pair_out_t out;
    // SEC-102: a substituted certificate is rejected at `pair.hello`, before a PAKE run starts, so
    // it cannot consume the legitimate user's three attempts.
    CHECK_EQ(harness_run_to_confirm(&h, h.code, other, &step, &out), KVM_PAIR_ERR_BINDING);
    CHECK_EQ(step, 1);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ERROR);
    CHECK(out.close_after);
    CHECK_EQ(h.store.count, 0);
    CHECK_EQ(h.device.counters.pair_attempts, 0);
    CHECK_EQ(h.device.failures, 0);
    // The window is still open for the legitimate client.
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);
    harness_free(&h);
}

static void test_hello_validation(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 113), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_out_t out;
    kvm_pair_hello_t base;
    memset(&base, 0, sizeof(base));
    base.v = KVM_PAIR_WIRE_VERSION;
    base.pake = KVM_PAIR_PAKE_NAME;
    memcpy(base.client_name, "DESK1", 6);
    memcpy(base.cert_sha256, h.cert_sha, sizeof(base.cert_sha256));

    kvm_pair_hello_t bad = base;
    bad.v = 2;
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &bad, &out), KVM_PAIR_ERR_FRAME);
    bad = base;
    bad.pake = "spake2plus-p256";
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &bad, &out), KVM_PAIR_ERR_FRAME);
    bad = base;
    bad.pake = NULL;
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &bad, &out), KVM_PAIR_ERR_FRAME);
    bad = base;
    bad.client_name[0] = '\0';
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &bad, &out), KVM_PAIR_ERR_FRAME);
    // None of those touched the window.
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);
    CHECK_EQ(h.device.counters.pair_attempts, 0);

    CHECK_EQ(kvm_pair_handle_hello(&h.device, &base, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ROUND1);
    harness_free(&h);
}

// --- malformed frames (AT-EXT-17) ----------------------------------------------------------------

static void test_malformed_blobs(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 114), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);

    kvm_pair_hello_t hello;
    kvm_pair_out_t out;
    CHECK_EQ(kvm_pair_client_begin(&h.client, h.code, "DESK1", h.client_id, h.cert_sha, &hello),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_OK);

    // Garbage, an over-long blob and an empty blob all fail closed and count as one failed run.
    uint8_t junk[KVM_PAIR_ROUND1_MAX + 64];
    memset(junk, 0xff, sizeof(junk));
    CHECK_EQ(kvm_pair_handle_round1(&h.device, junk, sizeof(junk), &out), KVM_PAIR_ERR_FRAME);
    CHECK_EQ(h.device.counters.pair_failures, 1);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_OPEN);

    // Out-of-order messages in the open state are refused without disturbing anything.
    CHECK_EQ(kvm_pair_handle_round2(&h.device, junk, 10, &out), KVM_PAIR_ERR_STATE);
    CHECK_EQ(kvm_pair_handle_confirm(&h.device, junk, KVM_PAIR_CONFIRM_LEN, &out),
             KVM_PAIR_ERR_STATE);
    CHECK_EQ(h.device.counters.pair_failures, 1);

    // A confirmation tag of the wrong length is a framing error, and it does count as a run.
    kvm_pair_client_free(&h.client);
    CHECK_EQ(kvm_pair_client_begin(&h.client, h.code, "DESK1", h.client_id, h.cert_sha, &hello),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_OK);
    size_t len = 0;
    CHECK_EQ(kvm_pair_client_round1(&h.client, out.blob, out.blob_len, h.c_r1, sizeof(h.c_r1),
                                    &len),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_round1(&h.device, h.c_r1, len, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_client_round2(&h.client, out.blob, out.blob_len, h.c_r2, sizeof(h.c_r2),
                                    &len),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_round2(&h.device, h.c_r2, len, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_confirm(&h.device, junk, 16, &out), KVM_PAIR_ERR_FRAME);
    CHECK_EQ(h.device.counters.pair_failures, 2);
    harness_free(&h);
}

// --- LOCAL priority ------------------------------------------------------------------------------

static void test_local_cancel_any_time(void)
{
    pair_harness_t h;
    kvm_pair_out_t out;

    // From the open window.
    CHECK_EQ(harness_init(&h, true, 115), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_cancel(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.code[0], '\0');
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_LOCAL_CANCEL);
    harness_free(&h);

    // Mid-run: the CLIENT is told, and the run is gone.
    CHECK_EQ(harness_init(&h, true, 116), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    kvm_pair_hello_t hello;
    CHECK_EQ(kvm_pair_client_begin(&h.client, h.code, "DESK1", h.client_id, h.cert_sha, &hello),
             KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_handle_hello(&h.device, &hello, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_cancel(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_REJECTED);
    CHECK(strcmp(out.reason, "local_reject") == 0);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK(!h.device.pake_live);
    harness_free(&h);

    // From the Accept screen, via the explicit Reject button.
    CHECK_EQ(harness_init(&h, true, 117), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    int step = -1;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_reject(&h.device, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_REJECTED);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.store.count, 0);
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_LOCAL_REJECT);
    harness_free(&h);

    // Accept and Reject are refused outside the Accept screen.
    CHECK_EQ(harness_init(&h, true, 118), KVM_PAIR_OK);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_accept(&h.device, &out), KVM_PAIR_ERR_STATE);
    CHECK_EQ(kvm_pair_local_reject(&h.device, &out), KVM_PAIR_ERR_STATE);
    CHECK_EQ(h.store.count, 0);
    harness_free(&h);
}

// --- the paired-client register ------------------------------------------------------------------

// Pair `n` clients into one harness, reopening the window each time.
static void pair_n(pair_harness_t *h, int n)
{
    for (int i = 0; i < n; i++) {
        kvm_pair_out_t out;
        int step = -1;
        CHECK_EQ(harness_open(h), KVM_PAIR_OK);
        CHECK_EQ(harness_run_to_confirm(h, h->code, h->cert_sha, &step, &out), KVM_PAIR_OK);
        CHECK_EQ(kvm_pair_local_accept(&h->device, &out), KVM_PAIR_OK);
        kvm_pair_client_free(&h->client);
        det_clock_advance(KVM_PAIR_TOAST_MS + 1u);
        CHECK_EQ(kvm_pair_tick(&h->device, &out), KVM_PAIR_OK);
    }
}

static void test_register_remove(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 119), KVM_PAIR_OK);
    pair_n(&h, 3);
    CHECK_EQ(kvm_pair_clients_count(&h.device), 3);

    kvm_pair_record_t rec;
    CHECK(kvm_pair_clients_get(&h.device, 0, &rec));
    CHECK(strcmp(rec.name, "DESK1") == 0);
    CHECK(!kvm_pair_clients_get(&h.device, 3, &rec));

    CHECK(kvm_pair_clients_remove(&h.device, 1));
    CHECK_EQ(kvm_pair_clients_count(&h.device), 2);
    CHECK(!kvm_pair_clients_remove(&h.device, 9));

    CHECK_EQ(kvm_pair_clients_remove_all(&h.device), 2);
    CHECK_EQ(kvm_pair_clients_count(&h.device), 0);
    CHECK_EQ(kvm_pair_clients_remove_all(&h.device), 0);
    harness_free(&h);
}

static void test_register_full(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 120), KVM_PAIR_OK);
    h.store.add_fails = true;

    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    int step = -1;
    kvm_pair_out_t out;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_accept(&h.device, &out), KVM_PAIR_ERR_FULL);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_ERROR);
    CHECK_EQ(h.store.count, 0);
    CHECK_EQ(h.device.state, KVM_PAIR_STATE_IDLE);
    CHECK_EQ(h.device.last_close, KVM_PAIR_CLOSE_INTERNAL_ERROR);
    harness_free(&h);
}

static void test_token_verify_is_exhaustive(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 121), KVM_PAIR_OK);
    pair_n(&h, 2);
    // Pair a third client and keep its token.
    kvm_pair_out_t out;
    int step = -1;
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_local_accept(&h.device, &out), KVM_PAIR_OK);
    memcpy(h.sealed, out.token, out.token_len);
    h.sealed_len = out.token_len;
    CHECK_EQ(harness_open_token(&h), KVM_PAIR_OK);

    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, h.token), 2);
    // Removing it invalidates the token immediately (FR-726).
    CHECK(kvm_pair_clients_remove(&h.device, 2));
    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, h.token), -1);
    // A one-bit change never matches.
    uint8_t near_miss[KVM_PAIR_TOKEN_LEN];
    memcpy(near_miss, h.token, sizeof(near_miss));
    near_miss[0] ^= 0x01u;
    CHECK_EQ(kvm_pair_clients_verify_token(&h.device, near_miss), -1);
    harness_free(&h);
}

// --- interface misuse ----------------------------------------------------------------------------

static void test_null_arguments(void)
{
    kvm_pair_out_t out;
    kvm_pair_session_t s;
    kvm_pair_config_t cfg;
    kvm_pair_store_t store;
    fake_pair_store_t fake;
    memset(&cfg, 0, sizeof(cfg));
    memcpy(cfg.device_name, "KVMpad-1", 9);
    fake_pair_store_init(&fake, &store);

    CHECK_EQ(kvm_pair_session_init(NULL, &cfg, det_platform(), &store), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pair_session_init(&s, NULL, det_platform(), &store), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pair_session_init(&s, &cfg, NULL, &store), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pair_session_init(&s, &cfg, det_platform(), NULL), KVM_PAIR_ERR_ARG);

    // An incomplete platform block is refused rather than crashing later.
    kvm_pair_platform_t partial = *det_platform();
    partial.now_ms = NULL;
    CHECK_EQ(kvm_pair_session_init(&s, &cfg, &partial, &store), KVM_PAIR_ERR_ARG);
    // An unnamed device cannot advertise (FR-723).
    kvm_pair_config_t unnamed;
    memset(&unnamed, 0, sizeof(unnamed));
    CHECK_EQ(kvm_pair_session_init(&s, &unnamed, det_platform(), &store), KVM_PAIR_ERR_ARG);

    CHECK_EQ(kvm_pair_session_init(&s, &cfg, det_platform(), &store), KVM_PAIR_OK);
    CHECK_EQ(kvm_pair_window_open(&s, NULL), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pair_handle_hello(&s, NULL, &out), KVM_PAIR_ERR_ARG);
    CHECK_EQ(kvm_pair_handle_round1(&s, NULL, 0, &out), KVM_PAIR_ERR_ARG);
    // Every inbound handler writes its reply to `out`: a NULL `out` is refused before anything is touched
    // (the optimised firmware build flagged the write through a NULL `out` in the confirm handler).
    {
        const kvm_pair_hello_t no_hello = {0};
        const uint8_t blob[KVM_PAIR_CONFIRM_LEN] = {0};
        CHECK_EQ(kvm_pair_handle_hello(&s, &no_hello, NULL), KVM_PAIR_ERR_ARG);
        CHECK_EQ(kvm_pair_handle_round1(&s, blob, sizeof(blob), NULL), KVM_PAIR_ERR_ARG);
        CHECK_EQ(kvm_pair_handle_round2(&s, blob, sizeof(blob), NULL), KVM_PAIR_ERR_ARG);
        CHECK_EQ(kvm_pair_handle_confirm(&s, blob, sizeof(blob), NULL), KVM_PAIR_ERR_ARG);
        CHECK_EQ(kvm_pair_window_remaining_ms(&s), 0); // still closed: no handler opened or failed a run
    }
    CHECK_EQ(kvm_pair_tick(NULL, &out), KVM_PAIR_ERR_ARG);
    CHECK(!kvm_pair_take_event(NULL, NULL));
    CHECK(kvm_pair_window_code(NULL) == NULL);
    CHECK_EQ(kvm_pair_window_remaining_ms(NULL), 0);
    CHECK(!kvm_pair_endpoint_open(NULL));
    CHECK_EQ(kvm_pair_clients_count(NULL), 0);
    CHECK_EQ(kvm_pair_clients_verify_token(&s, NULL), -1);
    kvm_pair_session_free(&s);
    kvm_pair_session_free(NULL);
}

static void test_require_accept_change_applies_next_window(void)
{
    pair_harness_t h;
    CHECK_EQ(harness_init(&h, true, 122), KVM_PAIR_OK);
    kvm_pair_set_require_accept(&h.device, false);
    CHECK_EQ(harness_open(&h), KVM_PAIR_OK);
    int step = -1;
    kvm_pair_out_t out;
    CHECK_EQ(harness_run_to_confirm(&h, h.code, h.cert_sha, &step, &out), KVM_PAIR_OK);
    CHECK_EQ(out.kind, KVM_PAIR_OUT_CONFIRM_ACCEPTED);
    harness_free(&h);
}

static void test_wire_error_codes(void)
{
    // Every refusal a probe can provoke maps onto one of three codes, and the four states a probe
    // would most like to distinguish all map onto the same one (SEC-104 threat model).
    CHECK(kvm_pair_err_wire_code(KVM_PAIR_OK) == NULL);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_EXPIRED), "ERR_PAIRING_EXPIRED") == 0);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_LOCKED), "ERR_PAIRING_EXPIRED") == 0);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_BUSY), "ERR_PAIRING_EXPIRED") == 0);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_STATE), "ERR_PAIRING_EXPIRED") == 0);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_FRAME), "ERR_BAD_FRAME") == 0);
    CHECK(strcmp(kvm_pair_err_wire_code(KVM_PAIR_ERR_BAD_CODE), "ERR_CONTROL_DENIED") == 0);
    CHECK(strcmp(kvm_pair_err_name(KVM_PAIR_ERR_BINDING), "BINDING") == 0);
}

int main(void)
{
    printf("test_pairing_session\n");
    kvm_pake_set_platform(det_platform());
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        fprintf(stderr, "psa_crypto_init failed\n");
        return 1;
    }
    RUN(test_wire_error_codes);
    RUN(test_code_is_six_uniform_digits);
    RUN(test_rng_failure_blocks_window);
    RUN(test_preconditions);
    RUN(test_full_pairing_require_accept);
    RUN(test_require_accept_off);
    RUN(test_three_wrong_codes_close_window);
    RUN(test_three_closures_cause_lockout);
    RUN(test_closures_outside_period_do_not_lock);
    RUN(test_code_expires_at_120s);
    RUN(test_accept_timeout);
    RUN(test_window_cap_is_the_backstop);
    RUN(test_cap_closes_mid_run);
    RUN(test_cert_mismatch_aborts_without_storing);
    RUN(test_hello_validation);
    RUN(test_malformed_blobs);
    RUN(test_local_cancel_any_time);
    RUN(test_register_remove);
    RUN(test_register_full);
    RUN(test_token_verify_is_exhaustive);
    RUN(test_require_accept_change_applies_next_window);
    RUN(test_null_arguments);
    TEST_MAIN_END("test_pairing_session");
}
