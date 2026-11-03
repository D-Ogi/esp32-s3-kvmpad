// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// `pair.*` envelope and payload codec (ADR-029 §4/§5, 03 §6.2).
// Covers AT-EXT-13 (the wire shapes the interop vectors assume), AT-EXT-17 (malformed frames fail
// closed) and AT-SEC-04 / AT-SEC-08.

#include <stdio.h>
#include <string.h>

#include "kvm_pairing_wire.h"
#include "kvm_srt_pair_json.h"
#include "kvm_test.h"

static kvm_json_doc_t doc;
static kvm_srt_pair_env_t env;

static kvm_rp_err_t parse(const char *frame)
{
    return kvm_srt_pair_parse_env(&doc, frame, strlen(frame), &env);
}

// --- envelope ------------------------------------------------------------------------------------

static void t_env_ok(void)
{
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":7,\"payload\":{}}"), KVM_RP_OK);
    CHECK_EQ(env.msg, KVM_SRT_PAIR_MSG_HELLO);
    CHECK_EQ(env.seq, 7);
    CHECK(!env.have_session_id);
    CHECK(env.payload >= 0);

    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round1\",\"seq\":8,\"session_id\":\"p-1a2b\","
                   "\"payload\":{\"blob\":\"QQ==\"}}"),
             KVM_RP_OK);
    CHECK_EQ(env.msg, KVM_SRT_PAIR_MSG_ROUND1);
    CHECK(env.have_session_id);
    CHECK_EQ(strcmp(env.session_id, "p-1a2b"), 0);

    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round2\",\"seq\":0,\"payload\":{}}"), KVM_RP_OK);
    CHECK_EQ(env.msg, KVM_SRT_PAIR_MSG_ROUND2);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.confirm\",\"seq\":1,\"payload\":{}}"), KVM_RP_OK);
    CHECK_EQ(env.msg, KVM_SRT_PAIR_MSG_CONFIRM);
}

static void t_env_version(void)
{
    // A wrong version is its own code so the CLIENT can say "update", not "bad frame" (03 §6.2).
    CHECK_EQ(parse("{\"v\":2,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_VERSION);
    CHECK_EQ(parse("{\"v\":0,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_VERSION);
    // A missing or non-integer `v` is not a version statement at all.
    CHECK_EQ(parse("{\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":\"1\",\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1.0,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
}

static void t_env_rejects(void)
{
    // Not a `pair.*` message: the pairing endpoint carries nothing else (SEC-103).
    CHECK_EQ(parse("{\"v\":1,\"type\":\"input\",\"seq\":1,\"payload\":{}}"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"status.get\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"hello\",\"seq\":1,\"payload\":{}}"), KVM_RP_ERR_BAD_FRAME);
    // Device-to-client names are not accepted from a client.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.accepted\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.rejected\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    // A prefix of a known name is not a known name.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hell\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.helloX\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);

    // `seq` must be a non-negative integer and must be present.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"payload\":{}}"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":-1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);

    // No member outside the envelope allowlist; `lease_epoch` in particular, since pairing has no
    // lease (ADR-029 §4).
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"lease_epoch\":3,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"token\":\"x\",\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
    // A duplicate member is ambiguous, not "last one wins".
    CHECK_EQ(parse("{\"v\":1,\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);

    // The payload has to be an object when present.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":7}"),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":[]}"),
             KVM_RP_ERR_BAD_FRAME);

    // Syntax, truncation, trailing bytes and a non-object root.
    CHECK_EQ(parse("{\"v\":1,"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("[]"), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse(""), KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{}} trailing"),
             KVM_RP_ERR_BAD_FRAME);

    // A session_id longer than the label buffer cannot belong to this device.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,"
                   "\"session_id\":\"p-aaaaaaaaaaaaaaaaaaaa\",\"payload\":{}}"),
             KVM_RP_ERR_BAD_FRAME);
}

static void t_env_oversize(void)
{
    // 03 §6.1: 1024 bytes is a hard limit, not a target.
    static char big[KVM_JSON_MAX_FRAME + 200];
    const char *head = "{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,\"payload\":{\"blob\":\"";
    size_t n = 0;
    memcpy(big, head, strlen(head));
    n = strlen(head);
    while (n < KVM_JSON_MAX_FRAME + 100u) {
        big[n++] = 'A';
    }
    big[n++] = '"';
    big[n++] = '}';
    big[n++] = '}';
    CHECK_EQ(kvm_srt_pair_parse_env(&doc, big, n, &env), KVM_RP_ERR_BAD_FRAME);
}

// --- pair.hello payload --------------------------------------------------------------------------

static void hello_frame(char *out, size_t cap, const char *client_name, const char *client_id_hex,
                        const char *cert_hex)
{
    snprintf(out, cap,
             "{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{\"v\":1,"
             "\"pake\":\"%s\",\"client_name\":\"%s\",\"client_id\":\"%s\","
             "\"cert_sha256\":\"%s\"}}",
             KVM_PAIR_PAKE_NAME, client_name, client_id_hex, cert_hex);
}

static const char *const ID_HEX = "0123456789abcdef0123456789abcdef";
static const char *const CERT_HEX =
    "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf";

static void t_hello_ok(void)
{
    char frame[600];
    kvm_srt_pair_hello_t h;
    hello_frame(frame, sizeof(frame), "Desk1", ID_HEX, CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_OK);

    CHECK_EQ(h.hello.v, KVM_PAIR_WIRE_VERSION);
    CHECK_EQ(strcmp(h.hello.pake, KVM_PAIR_PAKE_NAME), 0);
    // The core compares `pake` through the pointer it was handed, so it has to point at the
    // backing store inside the struct and not into the frame, which the caller may reuse.
    CHECK(h.hello.pake == h.pake);
    CHECK_EQ(strcmp(h.hello.client_name, "Desk1"), 0);
    CHECK_EQ(h.hello.client_id[0], 0x01u);
    CHECK_EQ(h.hello.client_id[15], 0xefu);
    CHECK_EQ(h.hello.cert_sha256[0], 0xa0u);
    CHECK_EQ(h.hello.cert_sha256[31], 0xbfu);
}

static void t_hello_name_sanitized(void)
{
    char frame[600];
    kvm_srt_pair_hello_t h;
    // FR-724: the name is rendered on the Accept screen, so a control character must not survive.
    hello_frame(frame, sizeof(frame), "A\\u001bB\\u0007C", ID_HEX, CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_OK);
    CHECK_EQ(strcmp(h.hello.client_name, "A?B?C"), 0);
}

static void t_hello_rejects(void)
{
    char frame[700];
    kvm_srt_pair_hello_t h;

    // Identifier and hash have to be exactly their lengths, in hex.
    hello_frame(frame, sizeof(frame), "Desk1", "0123", CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);

    hello_frame(frame, sizeof(frame), "Desk1", ID_HEX, "a0a1");
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);

    // Uppercase hex decodes to the same bytes and is accepted: the DEVICE emits lowercase (ADR-029
    // §4) but the shared hex decoder is case-insensitive, and the channel binding commits to the
    // decoded bytes, so the spelling cannot change the outcome of a run.
    hello_frame(frame, sizeof(frame), "Desk1", "0123456789ABCDEF0123456789abcdef", CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_OK);
    CHECK_EQ(h.hello.client_id[7], 0xefu);

    // An empty name is not a name.
    hello_frame(frame, sizeof(frame), "", ID_HEX, CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);

    // A name longer than KVM_PAIR_NAME_CAP is refused, not truncated: a truncation would change
    // the channel binding the CLIENT already committed to (ADR-029 §2).
    hello_frame(frame, sizeof(frame), "0123456789012345678901234567890123456789", ID_HEX,
                CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);

    // A member the payload does not define.
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{\"v\":1,\"pake\":\"%s\","
             "\"client_name\":\"D\",\"client_id\":\"%s\",\"cert_sha256\":\"%s\","
             "\"token\":\"x\"}}",
             KVM_PAIR_PAKE_NAME, ID_HEX, CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);

    // A missing member.
    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"pair.hello\",\"seq\":1,\"payload\":{\"v\":1,\"pake\":\"%s\","
             "\"client_id\":\"%s\",\"cert_sha256\":\"%s\"}}",
             KVM_PAIR_PAKE_NAME, ID_HEX, CERT_HEX);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_hello(&doc, env.payload, &h), KVM_RP_ERR_BAD_FRAME);
}

// --- blob fields ---------------------------------------------------------------------------------

static void t_blob(void)
{
    char frame[800];
    uint8_t buf[KVM_PAIR_ROUND1_MAX];
    size_t n = 0;

    uint8_t raw[64];
    for (unsigned i = 0; i < sizeof(raw); i++) {
        raw[i] = (uint8_t)(i * 3u);
    }
    char b64[128];
    CHECK(kvm_pair_b64_encode(raw, sizeof(raw), b64, sizeof(b64)));

    snprintf(frame, sizeof(frame),
             "{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,\"payload\":{\"blob\":\"%s\"}}", b64);
    CHECK_EQ(parse(frame), KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf), 0u, &n),
             KVM_RP_OK);
    CHECK_EQ(n, sizeof(raw));
    CHECK_EQ(memcmp(buf, raw, sizeof(raw)), 0);

    // The field name matters: a round blob is not a confirmation tag.
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "mac", buf, sizeof(buf), 0u, &n),
             KVM_RP_ERR_BAD_FRAME);

    // `exact` is enforced: a confirmation tag is 32 bytes, never 31 and never 33.
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf),
                                     KVM_PAIR_CONFIRM_LEN, &n),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(n, 0);

    // A blob longer than the destination is refused, never truncated.
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, 8u, 0u, &n),
             KVM_RP_ERR_BAD_FRAME);

    // Bad base64, an empty value, a non-string value and an extra member all fail closed.
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,\"payload\":{\"blob\":\"A!\"}}"),
             KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf), 0u, &n),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,\"payload\":{\"blob\":\"\"}}"),
             KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf), 0u, &n),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,\"payload\":{\"blob\":7}}"),
             KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf), 0u, &n),
             KVM_RP_ERR_BAD_FRAME);
    CHECK_EQ(parse("{\"v\":1,\"type\":\"pair.round1\",\"seq\":1,"
                   "\"payload\":{\"blob\":\"QQ==\",\"extra\":1}}"),
             KVM_RP_OK);
    CHECK_EQ(kvm_srt_pair_parse_blob(&doc, env.payload, "blob", buf, sizeof(buf), 0u, &n),
             KVM_RP_ERR_BAD_FRAME);
}

// --- serialization -------------------------------------------------------------------------------

static char out[KVM_JSON_MAX_FRAME + 1];

static void t_frame_round1(void)
{
    kvm_pair_out_t o;
    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_ROUND1;
    o.blob_len = KVM_PAIR_ROUND1_MAX; // the worst case still has to fit 1024 bytes
    for (size_t i = 0; i < o.blob_len; i++) {
        o.blob[i] = (uint8_t)i;
    }
    const size_t n = kvm_srt_pair_frame(&o, 0u, 1u, "p-1a2b", "KVMpad-7F3A", out, sizeof(out));
    CHECK(n > 0);
    CHECK(n <= KVM_JSON_MAX_FRAME);
    CHECK_EQ(strlen(out), n);
    CHECK(strstr(out, "\"type\":\"pair.round1\"") != NULL);
    CHECK(strstr(out, "\"session_id\":\"p-1a2b\"") != NULL);
    // `pair.round1` doubles as the acknowledgment of `pair.hello`, so it carries the device name.
    CHECK(strstr(out, "\"device_name\":\"KVMpad-7F3A\"") != NULL);
    CHECK(strstr(out, "\"seq\":1") != NULL);

    // It parses back as a well-formed v1 envelope, and the blob survives the round trip.
    CHECK_EQ(kvm_srt_pair_parse_env(&doc, out, n, &env), KVM_RP_OK);
    CHECK_EQ(env.msg, KVM_SRT_PAIR_MSG_ROUND1);
    char b64[KVM_PAIR_ROUND1_B64_CAP];
    const int blob = kvm_json_member(&doc, env.payload, "blob");
    CHECK(kvm_json_str_copy(&doc, blob, b64, sizeof(b64)) != (size_t)-1);
    uint8_t back[KVM_PAIR_ROUND1_MAX];
    size_t bn = 0;
    CHECK(kvm_pair_b64_decode(b64, back, sizeof(back), &bn));
    CHECK_EQ(bn, o.blob_len);
    CHECK_EQ(memcmp(back, o.blob, bn), 0);

    // There is no second part for a single-message kind.
    CHECK_EQ(kvm_srt_pair_frame(&o, 1u, 2u, "p-1a2b", "KVMpad-7F3A", out, sizeof(out)), 0);
}

static void t_frame_kinds(void)
{
    kvm_pair_out_t o;

    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_ROUND2;
    o.blob_len = 100u;
    CHECK(kvm_srt_pair_frame(&o, 0u, 4u, "p-1a2b", "N", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.round2\"") != NULL);
    CHECK(strstr(out, "\"blob\":") != NULL);
    // Only `pair.round1` and `pair.accepted` carry the device name.
    CHECK(strstr(out, "device_name") == NULL);

    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_CONFIRM;
    o.blob_len = KVM_PAIR_CONFIRM_LEN;
    CHECK(kvm_srt_pair_frame(&o, 0u, 5u, "p-1a2b", "N", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.confirm\"") != NULL);
    CHECK(strstr(out, "\"mac\":") != NULL);

    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_ACCEPTED;
    o.token_len = KVM_PAIR_TOKEN_SEALED_LEN;
    CHECK(kvm_srt_pair_frame(&o, 0u, 6u, "p-1a2b", "KVMpad-7F3A", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.accepted\"") != NULL);
    CHECK(strstr(out, "\"token_enc\":") != NULL);
    CHECK(strstr(out, "\"device_name\":\"KVMpad-7F3A\"") != NULL);

    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_REJECTED;
    o.reason = "timeout";
    CHECK(kvm_srt_pair_frame(&o, 0u, 7u, "p-1a2b", "N", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.rejected\"") != NULL);
    CHECK(strstr(out, "\"reason\":\"timeout\"") != NULL);
    // A rejection says nothing that helps a probe: no code, no state, no counter (ADR-029 §4).
    CHECK(strstr(out, "attempt") == NULL);
    CHECK(strstr(out, "remaining") == NULL);

    // KVM_PAIR_OUT_NONE and KVM_PAIR_OUT_ERROR produce no frame; the caller reports an error
    // through the ordinary `error` message instead.
    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_NONE;
    CHECK_EQ(kvm_srt_pair_frame(&o, 0u, 8u, "p", "N", out, sizeof(out)), 0);
    o.kind = KVM_PAIR_OUT_ERROR;
    o.wire_code = "ERR_PAIRING_EXPIRED";
    CHECK_EQ(kvm_srt_pair_frame(&o, 0u, 8u, "p", "N", out, sizeof(out)), 0);
}

static void t_frame_confirm_accepted(void)
{
    // The require_accept OFF path of ADR-023: `pair.confirm` immediately followed by
    // `pair.accepted`, as two frames with consecutive sequence numbers.
    kvm_pair_out_t o;
    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_CONFIRM_ACCEPTED;
    o.blob_len = KVM_PAIR_CONFIRM_LEN;
    o.token_len = KVM_PAIR_TOKEN_SEALED_LEN;

    CHECK(kvm_srt_pair_frame(&o, 0u, 1u, "p-1a2b", "N", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.confirm\"") != NULL);
    CHECK(strstr(out, "\"seq\":1") != NULL);

    CHECK(kvm_srt_pair_frame(&o, 1u, 2u, "p-1a2b", "N", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"type\":\"pair.accepted\"") != NULL);
    CHECK(strstr(out, "\"seq\":2") != NULL);

    CHECK_EQ(kvm_srt_pair_frame(&o, 2u, 3u, "p-1a2b", "N", out, sizeof(out)), 0);
}

static void t_frame_no_truncation(void)
{
    kvm_pair_out_t o;
    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_ROUND1;
    o.blob_len = KVM_PAIR_ROUND1_MAX;
    // A frame that does not fit is dropped whole: half a message would be a protocol error on the
    // CLIENT instead of a recoverable one.
    char small[64];
    CHECK_EQ(kvm_srt_pair_frame(&o, 0u, 1u, "p-1a2b", "KVMpad-7F3A", small, sizeof(small)), 0);
    CHECK_EQ(small[0], '\0');
    CHECK_EQ(kvm_srt_pair_frame(&o, 0u, 1u, "p", "N", small, 0u), 0);
}

static void t_frame_escapes(void)
{
    // A device name that somehow carried a quote or a backslash must not break the frame. The
    // transport sanitizes control characters before this point; escaping still has to be right.
    kvm_pair_out_t o;
    memset(&o, 0, sizeof(o));
    o.kind = KVM_PAIR_OUT_ACCEPTED;
    o.token_len = KVM_PAIR_TOKEN_SEALED_LEN;
    const size_t n = kvm_srt_pair_frame(&o, 0u, 1u, "p-1a2b", "a\"b\\c", out, sizeof(out));
    CHECK(n > 0);
    CHECK(strstr(out, "\"device_name\":\"a\\\"b\\\\c\"") != NULL);
    // And it is still parseable.
    CHECK_EQ(kvm_json_parse(&doc, out, n), KVM_JSON_OK);
}

static void t_error_frame(void)
{
    const size_t n = kvm_srt_error_frame(KVM_RP_ERR_PAIRING_EXPIRED, 3u, "p-1a2b", out,
                                         sizeof(out));
    CHECK(n > 0);
    CHECK(strstr(out, "\"type\":\"error\"") != NULL);
    CHECK(strstr(out, "\"code\":\"ERR_PAIRING_EXPIRED\"") != NULL);
    CHECK(strstr(out, "\"retryable\":") != NULL);
    CHECK_EQ(kvm_json_parse(&doc, out, n), KVM_JSON_OK);

    CHECK(kvm_srt_error_frame(KVM_RP_ERR_BAD_FRAME, 1u, "p", out, sizeof(out)) > 0);
    CHECK(strstr(out, "\"code\":\"ERR_BAD_FRAME\"") != NULL);
    CHECK_EQ(kvm_srt_error_frame(KVM_RP_ERR_BAD_FRAME, 1u, "p", out, 10u), 0);
}

static void t_label(void)
{
    char label[KVM_SRT_PAIR_LABEL_CAP];
    kvm_srt_pair_label(label, 0x1234ABCDu);
    // Lowest 16 bits, as four lowercase hex digits. The label is a server-side identifier, never
    // an authorization token (03 §6.2), so it does not have to be unpredictable, only distinct.
    CHECK_EQ(strcmp(label, "p-abcd"), 0);
    kvm_srt_pair_label(label, 0u);
    CHECK_EQ(strcmp(label, "p-0000"), 0);
    CHECK(strlen(label) < KVM_SRT_PAIR_LABEL_CAP);
}

static void t_error_code_table(void)
{
    // ADR-029 §5 maps the pairing core's result codes onto exactly three wire codes. kvm_srt.c
    // keeps the same table to turn a kvm_pair_err_t into a kvm_rp_err_t, so the two must agree
    // string for string -- otherwise a CLIENT would be told something the ADR does not define.
    struct {
        kvm_pair_err_t err;
        const char *code;
    } rows[] = {
        {KVM_PAIR_ERR_FRAME, "ERR_BAD_FRAME"},
        {KVM_PAIR_ERR_ARG, "ERR_BAD_FRAME"},
        {KVM_PAIR_ERR_BAD_CODE, "ERR_CONTROL_DENIED"},
        {KVM_PAIR_ERR_BINDING, "ERR_CONTROL_DENIED"},
        {KVM_PAIR_ERR_FULL, "ERR_CONTROL_DENIED"},
        {KVM_PAIR_ERR_CRYPTO, "ERR_CONTROL_DENIED"},
        // A failed precondition never reaches the wire: SEC-106 is enforced at admission, before
        // any frame. The code is the pairing core's choice and kvm_srt.c follows it.
        {KVM_PAIR_ERR_PRECONDITION, "ERR_CONTROL_DENIED"},
        {KVM_PAIR_ERR_STATE, "ERR_PAIRING_EXPIRED"},
        {KVM_PAIR_ERR_EXPIRED, "ERR_PAIRING_EXPIRED"},
        {KVM_PAIR_ERR_LOCKED, "ERR_PAIRING_EXPIRED"},
        {KVM_PAIR_ERR_BUSY, "ERR_PAIRING_EXPIRED"},
        // A rejection normally leaves as `pair.rejected`; when it has to be an `error` instead it
        // says no more than a closed window would.
        {KVM_PAIR_ERR_REJECTED, "ERR_PAIRING_EXPIRED"},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const char *wire = kvm_pair_err_wire_code(rows[i].err);
        CHECK(wire != NULL);
        if (wire != NULL) {
            CHECK_EQ(strcmp(wire, rows[i].code), 0);
        }
    }
    CHECK(kvm_pair_err_wire_code(KVM_PAIR_OK) == NULL);
}

int main(void)
{
    RUN(t_env_ok);
    RUN(t_env_version);
    RUN(t_env_rejects);
    RUN(t_env_oversize);
    RUN(t_hello_ok);
    RUN(t_hello_name_sanitized);
    RUN(t_hello_rejects);
    RUN(t_blob);
    RUN(t_frame_round1);
    RUN(t_frame_kinds);
    RUN(t_frame_confirm_accepted);
    RUN(t_frame_no_truncation);
    RUN(t_frame_escapes);
    RUN(t_error_frame);
    RUN(t_label);
    RUN(t_error_code_table);
    TEST_MAIN_END("test_srt_pair_json");
}
