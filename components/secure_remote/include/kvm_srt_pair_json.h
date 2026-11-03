// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// `pair.*` envelope and payload codec (TASK-100 transport; 03 §6.2, ADR-029 §4/§5).
//
// The pairing core (components/secure_remote/pairing) is deliberately free of JSON: it takes
// decoded payloads and returns `kvm_pair_out_t`. This file is the only place that turns the v1
// envelope of 03 §6.2 into those payloads and back.
//
// Why `pair.*` has its own codec instead of going through remote_protocol: the pairing endpoint of
// ADR-028 §2 (`/v1/pair`, subprotocol `kvmpad.pair.v1`) is a separate URI with no token and no
// lease, and SEC-103 requires that it reach zero status and zero HID. Keeping it out of
// `kvm_rp_t` makes that structural rather than a flag -- a pairing connection holds no session
// slot, no owner mirror and no intent sink -- and leaves one server-to-client `seq` counter per
// connection. `kvm_rp_hooks_t::pair_frame` therefore stays NULL, which is the documented
// configuration in which every `pair.*` frame arriving on the *control* endpoint is answered with
// `ERR_PAIRING_EXPIRED` (ADR-030, SEC-103).
//
// Reader properties inherited from remote_json.h: 1024-byte frame limit, no recursion, no
// allocation, duplicate keys and unknown members rejected, well-formed UTF-8 required.
//
// Nothing here logs. No function returns a diagnostic string built from a payload, a code, a token
// or a client name (TECH-046, SEC-105).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kvm_pairing.h"
#include "remote_json.h"
#include "remote_protocol.h" // kvm_rp_err_t: one error vocabulary for both endpoints

#ifdef __cplusplus
extern "C" {
#endif

/** Wire `session_id` label of a pairing connection, e.g. "p-3f7a". Non-secret (03 §6.2). */
#define KVM_SRT_PAIR_LABEL_CAP 12u

/** Client-to-device `pair.*` messages. Everything else on this endpoint is `ERR_BAD_FRAME`. */
typedef enum {
    KVM_SRT_PAIR_MSG_HELLO = 0,
    KVM_SRT_PAIR_MSG_ROUND1,
    KVM_SRT_PAIR_MSG_ROUND2,
    KVM_SRT_PAIR_MSG_CONFIRM,
    KVM_SRT_PAIR_MSG_COUNT,
} kvm_srt_pair_msg_t;

/** Decoded envelope. `payload` is a token index into the document, or -1 when absent. */
typedef struct {
    kvm_srt_pair_msg_t msg;
    uint64_t seq;
    bool have_session_id;
    char session_id[KVM_SRT_PAIR_LABEL_CAP];
    int payload;
} kvm_srt_pair_env_t;

/** `pair.hello` with the backing store for `hello.pake`, which the core compares by pointer. */
typedef struct {
    kvm_pair_hello_t hello;
    char pake[24];
} kvm_srt_pair_hello_t;

/**
 * Parse one pairing frame into `doc` and fill `out`. `frame` need not be NUL-terminated and must
 * stay valid and unmodified while `out->payload` is used.
 *
 * Returns KVM_RP_OK, or the code to report: KVM_RP_ERR_BAD_FRAME (size, syntax, depth, unknown
 * member, unknown or non-`pair.*` type, missing or malformed `seq`), KVM_RP_ERR_VERSION (`v` != 1).
 * The `seq` monotonicity and `session_id` match are the caller's checks, since they need the
 * connection state.
 */
kvm_rp_err_t kvm_srt_pair_parse_env(kvm_json_doc_t *doc, const char *frame, size_t len,
                                    kvm_srt_pair_env_t *out);

/**
 * Parse the `pair.hello` payload of ADR-029 §4 row 1: `v`, `pake`, `client_name`, `client_id`
 * (hex, 16 bytes), `cert_sha256` (hex, 32 bytes).
 *
 * `client_name` is copied and sanitized for rendering: every byte below 0x20 and 0x7F becomes '?',
 * so a name cannot carry an escape sequence onto the DEVICE screen (FR-724). A name that does not
 * fit KVM_PAIR_NAME_CAP is KVM_RP_ERR_BAD_FRAME rather than silently truncated, because the
 * truncation would change the channel binding the CLIENT computed (ADR-029 §2).
 */
kvm_rp_err_t kvm_srt_pair_parse_hello(const kvm_json_doc_t *doc, int payload,
                                      kvm_srt_pair_hello_t *out);

/**
 * Decode one base64 payload field (`blob` for the rounds, `mac` for the confirmation) into `out`.
 * `exact` > 0 demands that length in bytes; 0 accepts anything up to `cap`.
 * The payload must carry that field and nothing else.
 */
kvm_rp_err_t kvm_srt_pair_parse_blob(const kvm_json_doc_t *doc, int payload, const char *field,
                                     uint8_t *out, size_t cap, size_t exact, size_t *out_len);

/**
 * Serialize part `part` (0, and 1 only for KVM_PAIR_OUT_CONFIRM_ACCEPTED) of a `kvm_pair_out_t`
 * into `buf`. Returns the byte length written, or 0 when there is no such part, when the kind
 * carries no frame (KVM_PAIR_OUT_NONE, KVM_PAIR_OUT_ERROR -- the caller reports those through the
 * ordinary `error` message) or when the frame would not fit `cap`. A frame that does not fit is
 * dropped, never half-written.
 */
size_t kvm_srt_pair_frame(const kvm_pair_out_t *out, unsigned part, uint32_t seq, const char *label,
                          const char *device_name, char *buf, size_t cap);

/** Serialize the `error` message of 03 §6.4 for the pairing endpoint. */
size_t kvm_srt_error_frame(kvm_rp_err_t err, uint32_t seq, const char *label, char *buf,
                           size_t cap);

/**
 * Non-secret wire label for a pairing connection, e.g. "p-3f7a". `entropy` only has to vary
 * between connections; the label is a server-side identifier, never an authorization token
 * (03 §6.2).
 */
void kvm_srt_pair_label(char out[KVM_SRT_PAIR_LABEL_CAP], uint32_t entropy);

#ifdef __cplusplus
}
#endif
