// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pairing window state machine (TASK-100; FR-721..727, SEC-101..106, ADR-022, ADR-023).
// The invariants this file is responsible for are listed in kvm_pairing.h.

#include "kvm_pairing.h"

#include <string.h>

#include "kvm_pairing_wire.h"

// --- result-code names ---------------------------------------------------------------------------

const char *kvm_pair_err_name(kvm_pair_err_t err)
{
    switch (err) {
    case KVM_PAIR_OK: return "OK";
    case KVM_PAIR_ERR_ARG: return "ARG";
    case KVM_PAIR_ERR_STATE: return "STATE";
    case KVM_PAIR_ERR_FRAME: return "FRAME";
    case KVM_PAIR_ERR_EXPIRED: return "EXPIRED";
    case KVM_PAIR_ERR_LOCKED: return "LOCKED";
    case KVM_PAIR_ERR_BAD_CODE: return "BAD_CODE";
    case KVM_PAIR_ERR_BINDING: return "BINDING";
    case KVM_PAIR_ERR_BUSY: return "BUSY";
    case KVM_PAIR_ERR_FULL: return "FULL";
    case KVM_PAIR_ERR_REJECTED: return "REJECTED";
    case KVM_PAIR_ERR_PRECONDITION: return "PRECONDITION";
    case KVM_PAIR_ERR_CRYPTO: return "CRYPTO";
    }
    return "UNKNOWN";
}

const char *kvm_pair_err_wire_code(kvm_pair_err_t err)
{
    switch (err) {
    case KVM_PAIR_OK:
        return NULL;
    // A closed window, an expired code, a lockout and a busy window all look the same from the
    // outside: a probe must not learn which of them it hit (SEC-104 threat model).
    case KVM_PAIR_ERR_EXPIRED:
    case KVM_PAIR_ERR_LOCKED:
    case KVM_PAIR_ERR_BUSY:
    case KVM_PAIR_ERR_STATE:
    case KVM_PAIR_ERR_REJECTED:
        return "ERR_PAIRING_EXPIRED";
    case KVM_PAIR_ERR_FRAME:
    case KVM_PAIR_ERR_ARG:
        return "ERR_BAD_FRAME";
    case KVM_PAIR_ERR_BAD_CODE:
    case KVM_PAIR_ERR_BINDING:
    case KVM_PAIR_ERR_CRYPTO:
    case KVM_PAIR_ERR_FULL:
    case KVM_PAIR_ERR_PRECONDITION:
        return "ERR_CONTROL_DENIED";
    }
    return "ERR_BAD_FRAME";
}

// --- helpers -------------------------------------------------------------------------------------

static void out_clear(kvm_pair_out_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

static void out_error(kvm_pair_out_t *out, kvm_pair_err_t err, bool close_after)
{
    if (out == NULL) {
        return;
    }
    out_clear(out);
    out->kind = KVM_PAIR_OUT_ERROR;
    out->wire_code = kvm_pair_err_wire_code(err);
    out->close_after = close_after;
}

static void out_rejected(kvm_pair_out_t *out, const char *reason)
{
    if (out == NULL) {
        return;
    }
    out_clear(out);
    out->kind = KVM_PAIR_OUT_REJECTED;
    out->reason = reason;
    out->close_after = true;
}

static uint64_t now_of(const kvm_pair_session_t *s)
{
    return s->platform.now_ms(s->platform.ctx);
}

static void post_event(kvm_pair_session_t *s, kvm_pair_event_kind_t kind,
                       kvm_pair_close_reason_t reason, const char *name, uint32_t remaining_ms)
{
    memset(&s->event, 0, sizeof(s->event));
    s->event.kind = kind;
    s->event.reason = reason;
    s->event.remaining_ms = remaining_ms;
    if (name != NULL) {
        size_t i = 0;
        while (i + 1u < KVM_PAIR_NAME_CAP && name[i] != '\0') {
            s->event.client_name[i] = name[i];
            i++;
        }
        s->event.client_name[i] = '\0';
    }
    s->event_pending = true;
}

bool kvm_pair_take_event(kvm_pair_session_t *s, kvm_pair_event_t *out)
{
    if (s == NULL || out == NULL || !s->event_pending) {
        return false;
    }
    *out = s->event;
    s->event_pending = false;
    memset(&s->event, 0, sizeof(s->event));
    return true;
}

// Drop the live PAKE run and every piece of material it produced.
static void run_drop(kvm_pair_session_t *s)
{
    if (s->pake_live) {
        kvm_pake_free(&s->pake);
        s->pake_live = false;
    }
    kvm_pair_zeroize(s->token, sizeof(s->token));
    s->token_ready = false;
    kvm_pair_zeroize(s->peer_name, sizeof(s->peer_name));
}

// Close the window: wipe the code (SEC-105), drop the run, report the reason.
static void window_close(kvm_pair_session_t *s, kvm_pair_close_reason_t reason)
{
    run_drop(s);
    kvm_pair_zeroize(s->code, sizeof(s->code));
    s->code_used = false;
    s->failures = 0;
    s->window_opened_ms = 0;
    s->code_issued_ms = 0;
    s->accept_screen_ms = 0;
    s->completed_ms = 0;
    s->last_close = reason;
    if (s->state != KVM_PAIR_STATE_LOCKED) {
        s->state = KVM_PAIR_STATE_IDLE;
    }
    post_event(s, KVM_PAIR_EVENT_WINDOW_CLOSED, reason, NULL, 0);
}

// Record a failure-driven closure and start a lockout when too many land inside the observation
// period (SEC-104). The ring keeps only the last KVM_PAIR_MAX_CLOSURES stamps, which is all the rule
// needs: a lockout triggers on the oldest of those being recent enough.
static void note_closure(kvm_pair_session_t *s, uint64_t now)
{
    s->closures[s->closure_next] = now;
    s->closure_next = (uint8_t)((s->closure_next + 1u) % KVM_PAIR_MAX_CLOSURES);
    if (s->closure_count < KVM_PAIR_MAX_CLOSURES) {
        s->closure_count++;
    }
    s->counters.windows_closed_on_failures++;

    if (s->closure_count < KVM_PAIR_MAX_CLOSURES) {
        return;
    }
    uint64_t oldest = s->closures[0];
    for (uint8_t i = 1; i < KVM_PAIR_MAX_CLOSURES; i++) {
        if (s->closures[i] < oldest) {
            oldest = s->closures[i];
        }
    }
    if (now - oldest <= KVM_PAIR_CLOSURE_WINDOW_MS) {
        s->state = KVM_PAIR_STATE_LOCKED;
        s->lockout_until_ms = now + KVM_PAIR_LOCKOUT_MS;
        s->counters.pair_lockouts++;
        s->closure_count = 0;
        s->closure_next = 0;
        memset(s->closures, 0, sizeof(s->closures));
        post_event(s, KVM_PAIR_EVENT_LOCKOUT, KVM_PAIR_CLOSE_LOCKED, NULL, KVM_PAIR_LOCKOUT_MS);
    }
}

// One failed run inside an open window (SEC-104): count it, drop the run and close the window on the
// third failure. Returns the error the caller should report.
static kvm_pair_err_t run_failed(kvm_pair_session_t *s, kvm_pair_err_t err, uint64_t now)
{
    s->counters.pair_failures++;
    run_drop(s);
    s->failures++;
    post_event(s, KVM_PAIR_EVENT_FAILED_ATTEMPT, KVM_PAIR_CLOSE_NONE, NULL, 0);

    if (s->failures >= KVM_PAIR_MAX_FAILURES) {
        window_close(s, KVM_PAIR_CLOSE_TOO_MANY_FAILURES);
        note_closure(s, now);
        return err;
    }
    // The window stays open with the same code: FR-722 makes the code single-use per successful
    // pairing, and SEC-104 gives the legitimate user three tries inside one window.
    s->state = KVM_PAIR_STATE_OPEN;
    return err;
}

// Deadlines. Returns the reason the window has to close, or KVM_PAIR_CLOSE_NONE.
static kvm_pair_close_reason_t deadline_hit(const kvm_pair_session_t *s, uint64_t now)
{
    if (s->state == KVM_PAIR_STATE_IDLE || s->state == KVM_PAIR_STATE_LOCKED) {
        return KVM_PAIR_CLOSE_NONE;
    }
    if (now - s->window_opened_ms >= KVM_PAIR_WINDOW_MAX_MS) {
        return KVM_PAIR_CLOSE_WINDOW_CAP;
    }
    if (s->state == KVM_PAIR_STATE_AWAIT_ACCEPT) {
        if (now - s->accept_screen_ms >= KVM_PAIR_ACCEPT_TIMEOUT_MS) {
            return KVM_PAIR_CLOSE_ACCEPT_TIMEOUT;
        }
        // The code has already been consumed; its countdown no longer gates the run.
        return KVM_PAIR_CLOSE_NONE;
    }
    if (s->state == KVM_PAIR_STATE_COMPLETED) {
        return KVM_PAIR_CLOSE_NONE;
    }
    if (now - s->code_issued_ms >= KVM_PAIR_CODE_TTL_MS) {
        return KVM_PAIR_CLOSE_CODE_EXPIRED;
    }
    return KVM_PAIR_CLOSE_NONE;
}

// Release an expired lockout and apply any deadline. Every entry point calls this first, so a
// caller that stops ticking cannot keep a stale window alive.
static void advance(kvm_pair_session_t *s, uint64_t now, kvm_pair_out_t *out)
{
    if (s->state == KVM_PAIR_STATE_LOCKED) {
        if (now >= s->lockout_until_ms) {
            s->state = KVM_PAIR_STATE_IDLE;
            s->lockout_until_ms = 0;
        }
        return;
    }
    const kvm_pair_close_reason_t reason = deadline_hit(s, now);
    if (reason == KVM_PAIR_CLOSE_NONE) {
        return;
    }
    if (reason == KVM_PAIR_CLOSE_ACCEPT_TIMEOUT) {
        out_rejected(out, "timeout");
    } else if (s->state == KVM_PAIR_STATE_RUNNING) {
        out_error(out, KVM_PAIR_ERR_EXPIRED, true);
    }
    window_close(s, reason);
}

// --- life cycle ----------------------------------------------------------------------------------

kvm_pair_err_t kvm_pair_session_init(kvm_pair_session_t *s, const kvm_pair_config_t *cfg,
                                     const kvm_pair_platform_t *platform,
                                     const kvm_pair_store_t *store)
{
    if (s == NULL || cfg == NULL || platform == NULL || store == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (platform->random == NULL || platform->now_ms == NULL || platform->unix_time == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (store->add == NULL || store->count == NULL || store->get == NULL ||
        store->remove == NULL || store->remove_all == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (cfg->device_name[0] == '\0') {
        return KVM_PAIR_ERR_ARG;
    }

    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->platform = *platform;
    s->store = *store;
    s->state = KVM_PAIR_STATE_IDLE;

    kvm_pake_set_platform(&s->platform);
    return kvm_pake_crypto_init();
}

void kvm_pair_session_free(kvm_pair_session_t *s)
{
    if (s == NULL) {
        return;
    }
    run_drop(s);
    kvm_pair_zeroize(s->code, sizeof(s->code));
    kvm_pair_zeroize(s, sizeof(*s));
}

void kvm_pair_set_require_accept(kvm_pair_session_t *s, bool require_accept)
{
    if (s != NULL) {
        s->cfg.require_accept = require_accept;
    }
}

// --- code generation -----------------------------------------------------------------------------

// Uniform 000000..999999 by rejection sampling (FR-722). Taking a 32-bit draw modulo 1000000 would
// favor the low 3030 values; the draw is rejected instead whenever it falls in the short tail.
static kvm_pair_err_t code_draw(char out[KVM_PAIR_CODE_CAP])
{
    const uint32_t span = 1000000u;
    const uint32_t limit = (uint32_t)(0x100000000ull / span) * span; // 4294000000
    uint32_t v = 0;

    for (int tries = 0; tries < 64; tries++) {
        uint8_t raw[4];
        if (!kvm_pake_platform_random(raw, sizeof(raw))) {
            return KVM_PAIR_ERR_CRYPTO;
        }
        v = ((uint32_t)raw[0] << 24) | ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
        if (v < limit) {
            v %= span;
            for (int i = (int)KVM_PAIR_CODE_LEN - 1; i >= 0; i--) {
                out[i] = (char)('0' + (v % 10u));
                v /= 10u;
            }
            out[KVM_PAIR_CODE_LEN] = '\0';
            return KVM_PAIR_OK;
        }
    }
    // 64 consecutive rejections have probability below 2^-1300; a generator that manages it is
    // broken, so fail closed rather than fall back to a biased reduction.
    return KVM_PAIR_ERR_CRYPTO;
}

kvm_pair_err_t kvm_pair_window_open(kvm_pair_session_t *s, const kvm_pair_preconditions_t *pre)
{
    if (s == NULL || pre == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    const uint64_t now = now_of(s);
    advance(s, now, NULL);

    if (s->state == KVM_PAIR_STATE_LOCKED) {
        return KVM_PAIR_ERR_LOCKED;
    }
    if (s->state != KVM_PAIR_STATE_IDLE) {
        return KVM_PAIR_ERR_BUSY;
    }
    // SEC-106: no Pair screen without Remote enabled and wifi:CONNECTED.
    if (!pre->remote_enabled || !pre->wifi_connected) {
        return KVM_PAIR_ERR_PRECONDITION;
    }

    char code[KVM_PAIR_CODE_CAP];
    kvm_pair_err_t err = code_draw(code);
    if (err != KVM_PAIR_OK) {
        kvm_pair_zeroize(code, sizeof(code));
        return err;
    }
    memcpy(s->code, code, sizeof(s->code));
    kvm_pair_zeroize(code, sizeof(code));

    s->state = KVM_PAIR_STATE_OPEN;
    s->window_opened_ms = now;
    s->code_issued_ms = now;
    s->code_used = false;
    s->failures = 0;
    s->last_close = KVM_PAIR_CLOSE_NONE;
    s->counters.windows_opened++;
    post_event(s, KVM_PAIR_EVENT_WINDOW_OPENED, KVM_PAIR_CLOSE_NONE, NULL, KVM_PAIR_CODE_TTL_MS);
    return KVM_PAIR_OK;
}

const char *kvm_pair_window_code(const kvm_pair_session_t *s)
{
    if (s == NULL || s->code[0] == '\0') {
        return NULL;
    }
    if (s->state != KVM_PAIR_STATE_OPEN && s->state != KVM_PAIR_STATE_RUNNING) {
        return NULL;
    }
    return s->code;
}

static uint32_t remaining(uint64_t now, uint64_t start, uint32_t span)
{
    const uint64_t gone = now - start;
    if (gone >= span) {
        return 0;
    }
    return (uint32_t)(span - gone);
}

uint32_t kvm_pair_code_remaining_ms(const kvm_pair_session_t *s)
{
    if (s == NULL || (s->state != KVM_PAIR_STATE_OPEN && s->state != KVM_PAIR_STATE_RUNNING)) {
        return 0;
    }
    return remaining(now_of(s), s->code_issued_ms, KVM_PAIR_CODE_TTL_MS);
}

uint32_t kvm_pair_window_remaining_ms(const kvm_pair_session_t *s)
{
    if (s == NULL || s->state == KVM_PAIR_STATE_IDLE || s->state == KVM_PAIR_STATE_LOCKED) {
        return 0;
    }
    return remaining(now_of(s), s->window_opened_ms, KVM_PAIR_WINDOW_MAX_MS);
}

bool kvm_pair_endpoint_open(const kvm_pair_session_t *s)
{
    if (s == NULL) {
        return false;
    }
    return s->state == KVM_PAIR_STATE_OPEN || s->state == KVM_PAIR_STATE_RUNNING ||
           s->state == KVM_PAIR_STATE_AWAIT_ACCEPT;
}

// --- tick ----------------------------------------------------------------------------------------

kvm_pair_err_t kvm_pair_tick(kvm_pair_session_t *s, kvm_pair_out_t *out)
{
    if (s == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    out_clear(out);
    const uint64_t now = now_of(s);
    advance(s, now, out);

    // require_accept OFF leaves the window in COMPLETED while the toast is up (FR-725).
    if (s->state == KVM_PAIR_STATE_COMPLETED && now - s->completed_ms >= KVM_PAIR_TOAST_MS) {
        window_close(s, KVM_PAIR_CLOSE_COMPLETED);
    }
    return KVM_PAIR_OK;
}

// --- LOCAL actions -------------------------------------------------------------------------------

kvm_pair_err_t kvm_pair_local_cancel(kvm_pair_session_t *s, kvm_pair_out_t *out)
{
    if (s == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    out_clear(out);
    // ADR-011: LOCAL wins in every state, including during a run and on the Accept screen.
    if (s->state == KVM_PAIR_STATE_RUNNING || s->state == KVM_PAIR_STATE_AWAIT_ACCEPT) {
        out_rejected(out, "local_reject");
    }
    if (s->state == KVM_PAIR_STATE_LOCKED) {
        return KVM_PAIR_OK;
    }
    window_close(s, KVM_PAIR_CLOSE_LOCAL_CANCEL);
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_local_reject(kvm_pair_session_t *s, kvm_pair_out_t *out)
{
    if (s == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    out_clear(out);
    const uint64_t now = now_of(s);
    advance(s, now, out);
    if (s->state != KVM_PAIR_STATE_AWAIT_ACCEPT) {
        return KVM_PAIR_ERR_STATE;
    }
    out_rejected(out, "local_reject");
    window_close(s, KVM_PAIR_CLOSE_LOCAL_REJECT);
    return KVM_PAIR_OK;
}

// Store the client and seal its token. Called from the Accept path and from the require_accept OFF
// path, which is the only difference ADR-023 allows.
// `out` is filled with the sealed token in `token`/`token_len`; `kind` is left to the caller, which
// knows whether a confirmation tag has to ride along.
static kvm_pair_err_t complete(kvm_pair_session_t *s, uint64_t now, kvm_pair_out_t *out)
{
    uint8_t token[KVM_PAIR_TOKEN_LEN];
    if (!kvm_pake_platform_random(token, sizeof(token))) {
        kvm_pair_zeroize(token, sizeof(token));
        return KVM_PAIR_ERR_CRYPTO;
    }

    kvm_pair_record_t rec;
    memset(&rec, 0, sizeof(rec));
    size_t i = 0;
    while (i + 1u < KVM_PAIR_NAME_CAP && s->peer_name[i] != '\0') {
        rec.name[i] = s->peer_name[i];
        i++;
    }
    rec.name[i] = '\0';
    rec.paired_at_unix = s->platform.unix_time(s->platform.ctx);

    // SEC-105: the DEVICE keeps the hash, never the token.
    kvm_pair_err_t err = kvm_pair_sha256(token, sizeof(token), rec.token_sha256);
    if (err != KVM_PAIR_OK) {
        kvm_pair_zeroize(token, sizeof(token));
        return err;
    }

    uint8_t sealed[KVM_PAIR_TOKEN_SEALED_LEN];
    size_t sealed_len = 0;
    err = kvm_pake_token_seal(&s->pake, token, sealed, sizeof(sealed), &sealed_len);
    kvm_pair_zeroize(token, sizeof(token));
    if (err != KVM_PAIR_OK) {
        kvm_pair_zeroize(sealed, sizeof(sealed));
        kvm_pair_zeroize(&rec, sizeof(rec));
        return err;
    }

    if (s->store.add(s->store.ctx, &rec) < 0) {
        kvm_pair_zeroize(sealed, sizeof(sealed));
        kvm_pair_zeroize(&rec, sizeof(rec));
        return KVM_PAIR_ERR_FULL;
    }

    if (out != NULL) {
        out->kind = KVM_PAIR_OUT_ACCEPTED;
        memcpy(out->token, sealed, sealed_len);
        out->token_len = sealed_len;
        out->close_after = true;
    }
    kvm_pair_zeroize(sealed, sizeof(sealed));
    kvm_pair_zeroize(&rec, sizeof(rec));

    s->counters.pair_successes++;
    s->code_used = true;
    s->completed_ms = now;

    // Keep the client name for the toast, then drop everything secret.
    char name_copy[KVM_PAIR_NAME_CAP];
    memcpy(name_copy, s->peer_name, sizeof(name_copy));
    run_drop(s);
    kvm_pair_zeroize(s->code, sizeof(s->code));
    s->state = KVM_PAIR_STATE_COMPLETED;
    post_event(s, KVM_PAIR_EVENT_PAIRED_TOAST, KVM_PAIR_CLOSE_COMPLETED, name_copy,
               KVM_PAIR_TOAST_MS);
    kvm_pair_zeroize(name_copy, sizeof(name_copy));
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_local_accept(kvm_pair_session_t *s, kvm_pair_out_t *out)
{
    if (s == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    out_clear(out);
    const uint64_t now = now_of(s);
    advance(s, now, out);
    if (s->state != KVM_PAIR_STATE_AWAIT_ACCEPT) {
        return KVM_PAIR_ERR_STATE;
    }
    kvm_pair_err_t err = complete(s, now, out);
    if (err != KVM_PAIR_OK) {
        out_error(out, err, true);
        window_close(s, KVM_PAIR_CLOSE_INTERNAL_ERROR);
    }
    return err;
}

// --- inbound pair.* ------------------------------------------------------------------------------

// Shared entry guard: apply deadlines, refuse a locked or closed window, and make every refusal
// look the same on the wire.
static kvm_pair_err_t guard(kvm_pair_session_t *s, kvm_pair_state_t want, uint64_t *now,
                            kvm_pair_out_t *out)
{
    out_clear(out);
    *now = now_of(s);
    advance(s, *now, NULL);

    if (s->state == KVM_PAIR_STATE_LOCKED) {
        out_error(out, KVM_PAIR_ERR_LOCKED, true);
        return KVM_PAIR_ERR_LOCKED;
    }
    if (s->state == KVM_PAIR_STATE_IDLE) {
        out_error(out, KVM_PAIR_ERR_EXPIRED, true);
        return KVM_PAIR_ERR_EXPIRED;
    }
    if (s->state != want) {
        out_error(out, KVM_PAIR_ERR_STATE, true);
        return KVM_PAIR_ERR_STATE;
    }
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_handle_hello(kvm_pair_session_t *s, const kvm_pair_hello_t *hello,
                                     kvm_pair_out_t *out)
{
    if (s == NULL || hello == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    uint64_t now = 0;
    kvm_pair_err_t err = guard(s, KVM_PAIR_STATE_OPEN, &now, out);
    if (err != KVM_PAIR_OK) {
        return err;
    }

    if (hello->v != KVM_PAIR_WIRE_VERSION || hello->pake == NULL ||
        strcmp(hello->pake, KVM_PAIR_PAKE_NAME) != 0) {
        out_error(out, KVM_PAIR_ERR_FRAME, true);
        return KVM_PAIR_ERR_FRAME;
    }
    if (hello->client_name[0] == '\0') {
        out_error(out, KVM_PAIR_ERR_FRAME, true);
        return KVM_PAIR_ERR_FRAME;
    }
    // SEC-102: the certificate hash the CLIENT reports must be the DEVICE certificate of this
    // handshake. A mismatch means a substituted certificate -- abort, store nothing, and do not
    // even start a PAKE run, so the attempt cannot burn the legitimate user's three tries.
    if (!kvm_pair_equal_ct(hello->cert_sha256, s->cfg.cert_sha256, KVM_PAIR_SHA256_LEN)) {
        out_error(out, KVM_PAIR_ERR_BINDING, true);
        return KVM_PAIR_ERR_BINDING;
    }

    kvm_pake_binding_t binding;
    memset(&binding, 0, sizeof(binding));
    memcpy(binding.cert_sha256, s->cfg.cert_sha256, KVM_PAIR_SHA256_LEN);
    memcpy(binding.client_id, hello->client_id, KVM_PAIR_CLIENT_ID_LEN);
    size_t i = 0;
    while (i + 1u < KVM_PAIR_NAME_CAP && hello->client_name[i] != '\0') {
        binding.client_name[i] = hello->client_name[i];
        i++;
    }
    binding.client_name[i] = '\0';

    err = kvm_pake_init(&s->pake, KVM_PAKE_ROLE_DEVICE, s->code, &binding);
    if (err != KVM_PAIR_OK) {
        out_error(out, err, true);
        return err;
    }
    s->pake_live = true;
    memcpy(s->peer_name, binding.client_name, sizeof(s->peer_name));
    s->counters.pair_attempts++;
    s->state = KVM_PAIR_STATE_RUNNING;

    size_t len = 0;
    out_clear(out);
    err = kvm_pake_round1_out(&s->pake, out->blob, sizeof(out->blob), &len);
    if (err != KVM_PAIR_OK) {
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }
    out->kind = KVM_PAIR_OUT_ROUND1;
    out->blob_len = len;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_handle_round1(kvm_pair_session_t *s, const uint8_t *blob, size_t len,
                                      kvm_pair_out_t *out)
{
    if (s == NULL || blob == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    uint64_t now = 0;
    kvm_pair_err_t err = guard(s, KVM_PAIR_STATE_RUNNING, &now, out);
    if (err != KVM_PAIR_OK) {
        return err;
    }

    err = kvm_pake_round1_in(&s->pake, blob, len);
    if (err != KVM_PAIR_OK) {
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }

    size_t out_len = 0;
    out_clear(out);
    err = kvm_pake_round2_out(&s->pake, out->blob, sizeof(out->blob), &out_len);
    if (err != KVM_PAIR_OK) {
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }
    out->kind = KVM_PAIR_OUT_ROUND2;
    out->blob_len = out_len;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_handle_round2(kvm_pair_session_t *s, const uint8_t *blob, size_t len,
                                      kvm_pair_out_t *out)
{
    if (s == NULL || blob == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    uint64_t now = 0;
    kvm_pair_err_t err = guard(s, KVM_PAIR_STATE_RUNNING, &now, out);
    if (err != KVM_PAIR_OK) {
        return err;
    }

    err = kvm_pake_round2_in(&s->pake, blob, len);
    if (err == KVM_PAIR_OK) {
        err = kvm_pake_finish(&s->pake);
    }
    if (err != KVM_PAIR_OK) {
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }
    // No outbound message: the CLIENT confirms first, so the DEVICE is the side that verifies
    // before it reveals anything.
    out_clear(out);
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_handle_confirm(kvm_pair_session_t *s, const uint8_t *tag, size_t len,
                                       kvm_pair_out_t *out)
{
    if (s == NULL || tag == NULL || out == NULL) { // the reply is written to `out`: required, as for every inbound handler
        return KVM_PAIR_ERR_ARG;
    }
    uint64_t now = 0;
    kvm_pair_err_t err = guard(s, KVM_PAIR_STATE_RUNNING, &now, out);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    if (len != KVM_PAIR_CONFIRM_LEN) {
        kvm_pair_err_t reported = run_failed(s, KVM_PAIR_ERR_FRAME, now);
        out_error(out, reported, true);
        return reported;
    }

    err = kvm_pake_confirm_verify(&s->pake, tag);
    if (err != KVM_PAIR_OK) {
        // A wrong code, a substituted certificate and a tampered transcript all land here and all
        // count as one failed run (SEC-104).
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }

    out_clear(out);
    err = kvm_pake_confirm_out(&s->pake, out->blob);
    if (err != KVM_PAIR_OK) {
        kvm_pair_err_t reported = run_failed(s, err, now);
        out_error(out, reported, true);
        return reported;
    }
    out->kind = KVM_PAIR_OUT_CONFIRM;
    out->blob_len = KVM_PAIR_CONFIRM_LEN;

    if (s->cfg.require_accept) {
        s->state = KVM_PAIR_STATE_AWAIT_ACCEPT;
        s->accept_screen_ms = now;
        // The Accept screen may not push the window past the 180 s cap (FR-722); the UI shows
        // whichever of the two countdowns is shorter.
        const uint32_t cap_left = remaining(now, s->window_opened_ms, KVM_PAIR_WINDOW_MAX_MS);
        const uint32_t decision_ms =
            (cap_left < KVM_PAIR_ACCEPT_TIMEOUT_MS) ? cap_left : KVM_PAIR_ACCEPT_TIMEOUT_MS;
        post_event(s, KVM_PAIR_EVENT_ACCEPT_SCREEN, KVM_PAIR_CLOSE_NONE, s->peer_name, decision_ms);
        return KVM_PAIR_OK;
    }

    // ADR-023 with require_accept OFF: finish right away, and let the toast carry the Remove button.
    // The confirmation tag still has to reach the CLIENT, so the one result carries both messages
    // and the transport sends `pair.confirm` followed by `pair.accepted`.
    kvm_pair_err_t cerr = complete(s, now, out);
    if (cerr != KVM_PAIR_OK) {
        out_error(out, cerr, true);
        window_close(s, KVM_PAIR_CLOSE_INTERNAL_ERROR);
        return cerr;
    }
    out->kind = KVM_PAIR_OUT_CONFIRM_ACCEPTED;
    return KVM_PAIR_OK;
}

// --- paired-client register ----------------------------------------------------------------------

uint8_t kvm_pair_clients_count(const kvm_pair_session_t *s)
{
    if (s == NULL || s->store.count == NULL) {
        return 0;
    }
    return s->store.count(s->store.ctx);
}

bool kvm_pair_clients_get(const kvm_pair_session_t *s, uint8_t index, kvm_pair_record_t *out)
{
    if (s == NULL || out == NULL || s->store.get == NULL) {
        return false;
    }
    return s->store.get(s->store.ctx, index, out);
}

bool kvm_pair_clients_remove(kvm_pair_session_t *s, uint8_t index)
{
    if (s == NULL || s->store.remove == NULL) {
        return false;
    }
    return s->store.remove(s->store.ctx, index);
}

uint8_t kvm_pair_clients_remove_all(kvm_pair_session_t *s)
{
    if (s == NULL || s->store.remove_all == NULL) {
        return 0;
    }
    return s->store.remove_all(s->store.ctx);
}

int kvm_pair_clients_verify_token(const kvm_pair_session_t *s,
                                  const uint8_t token[KVM_PAIR_TOKEN_LEN])
{
    if (s == NULL || token == NULL || s->store.count == NULL || s->store.get == NULL) {
        return -1;
    }
    uint8_t want[KVM_PAIR_SHA256_LEN];
    if (kvm_pair_sha256(token, KVM_PAIR_TOKEN_LEN, want) != KVM_PAIR_OK) {
        return -1;
    }

    // Walk the whole register and keep the first match, without an early exit: the time this takes
    // must not reveal which entry matched, or whether one did.
    int found = -1;
    const uint8_t n = s->store.count(s->store.ctx);
    for (uint8_t i = 0; i < n; i++) {
        kvm_pair_record_t rec;
        if (!s->store.get(s->store.ctx, i, &rec)) {
            continue;
        }
        const bool hit = kvm_pair_equal_ct(rec.token_sha256, want, KVM_PAIR_SHA256_LEN);
        if (hit && found < 0) {
            found = (int)i;
        }
        kvm_pair_zeroize(&rec, sizeof(rec));
    }
    kvm_pair_zeroize(want, sizeof(want));
    return found;
}
