// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// `pair.*` envelope and payload codec (TASK-100 transport; 03 §6.2, ADR-029 §4/§5).
// See kvm_srt_pair_json.h.

#include "kvm_srt_pair_json.h"

#include <string.h>

#include "kvm_pairing_wire.h"

// --- a bounded appender ---------------------------------------------------------------------
//
// One writer per frame. `overflow` latches, so a frame that does not fit is reported as length 0
// by the caller instead of being truncated into a parseable-but-wrong message.
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} wr_t;

static void wr_begin(wr_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = (cap == 0u);
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static void wr_raw(wr_t *w, const char *s, size_t n)
{
    if (w->overflow) {
        return;
    }
    if (w->len + n + 1u > w->cap) {
        w->overflow = true;
        return;
    }
    memcpy(&w->buf[w->len], s, n);
    w->len += n;
    w->buf[w->len] = '\0';
}

static void wr_lit(wr_t *w, const char *s)
{
    wr_raw(w, s, strlen(s));
}

static void wr_u32(wr_t *w, uint32_t v)
{
    char tmp[11];
    size_t n = 0;
    if (v == 0u) {
        tmp[n++] = '0';
    } else {
        char rev[10];
        size_t r = 0;
        while (v > 0u) {
            rev[r++] = (char)('0' + (int)(v % 10u));
            v /= 10u;
        }
        while (r > 0u) {
            tmp[n++] = rev[--r];
        }
    }
    wr_raw(w, tmp, n);
}

// JSON string body. Only the two mandatory escapes and the control-character escape are produced;
// every value this file writes is either base64, a static reason code or a device name that
// kvm_srt sanitized, so no other case can occur -- and if one did, escaping is still correct.
static void wr_str(wr_t *w, const char *s)
{
    wr_lit(w, "\"");
    for (const char *p = s; *p != '\0'; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"') {
            wr_lit(w, "\\\"");
        } else if (c == '\\') {
            wr_lit(w, "\\\\");
        } else if (c < 0x20u) {
            static const char hex[] = "0123456789abcdef";
            char esc[6] = {'\\', 'u', '0', '0', '0', '0'};
            esc[4] = hex[(c >> 4) & 0x0Fu];
            esc[5] = hex[c & 0x0Fu];
            wr_raw(w, esc, sizeof(esc));
        } else {
            wr_raw(w, p, 1u);
        }
    }
    wr_lit(w, "\"");
}

static void wr_envelope(wr_t *w, const char *type, uint32_t seq, const char *label)
{
    wr_lit(w, "{\"v\":1,\"type\":\"");
    wr_lit(w, type);
    wr_lit(w, "\",\"seq\":");
    wr_u32(w, seq);
    wr_lit(w, ",\"session_id\":");
    wr_str(w, (label != NULL) ? label : "");
    wr_lit(w, ",\"payload\":{");
}

// --- envelope parsing ------------------------------------------------------------------------

static const char *const env_keys[] = {"v", "type", "seq", "session_id", "payload", NULL};

static const char *const msg_names[KVM_SRT_PAIR_MSG_COUNT] = {
    "pair.hello",
    "pair.round1",
    "pair.round2",
    "pair.confirm",
};

kvm_rp_err_t kvm_srt_pair_parse_env(kvm_json_doc_t *doc, const char *frame, size_t len,
                                    kvm_srt_pair_env_t *out)
{
    if (doc == NULL || frame == NULL || out == NULL) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    memset(out, 0, sizeof(*out));
    out->payload = -1;

    if (kvm_json_parse(doc, frame, len) != KVM_JSON_OK) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    const int root = kvm_json_root(doc);
    if (kvm_json_type(doc, root) != KVM_JSON_OBJECT || !kvm_json_keys_allowed(doc, root, env_keys)) {
        return KVM_RP_ERR_BAD_FRAME;
    }

    // `v` first: a client of another version must hear ERR_VERSION and not a syntax complaint
    // about a member its version happens to spell differently (03 §6.2).
    const int v = kvm_json_member(doc, root, "v");
    int64_t vi = 0;
    if (!kvm_json_get_i64(doc, v, &vi)) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    if (vi != 1) {
        return KVM_RP_ERR_VERSION;
    }

    const int type = kvm_json_member(doc, root, "type");
    if (kvm_json_type(doc, type) != KVM_JSON_STRING) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    int found = -1;
    for (int i = 0; i < (int)KVM_SRT_PAIR_MSG_COUNT; i++) {
        if (kvm_json_str_eq(doc, type, msg_names[i])) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    out->msg = (kvm_srt_pair_msg_t)found;

    const int seq = kvm_json_member(doc, root, "seq");
    int64_t si = 0;
    if (!kvm_json_get_i64(doc, seq, &si) || si < 0) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    out->seq = (uint64_t)si;

    const int sid = kvm_json_member(doc, root, "session_id");
    if (sid >= 0) {
        if (kvm_json_str_copy(doc, sid, out->session_id, sizeof(out->session_id)) == (size_t)-1) {
            return KVM_RP_ERR_BAD_FRAME;
        }
        out->have_session_id = true;
    }

    const int payload = kvm_json_member(doc, root, "payload");
    if (payload >= 0) {
        if (kvm_json_type(doc, payload) != KVM_JSON_OBJECT) {
            return KVM_RP_ERR_BAD_FRAME;
        }
        out->payload = payload;
    }
    return KVM_RP_OK;
}

// --- payload parsing -------------------------------------------------------------------------

static const char *const hello_keys[] = {"v", "pake", "client_name", "client_id", "cert_sha256",
                                         NULL};

kvm_rp_err_t kvm_srt_pair_parse_hello(const kvm_json_doc_t *doc, int payload,
                                      kvm_srt_pair_hello_t *out)
{
    if (doc == NULL || out == NULL) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    memset(out, 0, sizeof(*out));
    if (kvm_json_type(doc, payload) != KVM_JSON_OBJECT ||
        !kvm_json_keys_allowed(doc, payload, hello_keys)) {
        return KVM_RP_ERR_BAD_FRAME;
    }

    int64_t v = 0;
    if (!kvm_json_get_i64(doc, kvm_json_member(doc, payload, "v"), &v) || v < 0 ||
        v > (int64_t)UINT32_MAX) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    out->hello.v = (uint32_t)v;

    const int pake = kvm_json_member(doc, payload, "pake");
    if (kvm_json_str_copy(doc, pake, out->pake, sizeof(out->pake)) == (size_t)-1) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    out->hello.pake = out->pake;

    // The name is rendered on the Accept screen (FR-725), so every control character becomes '?'.
    // A name that does not fit is refused: truncating it would change the channel binding the
    // CLIENT already committed to (ADR-029 §2) and the run would fail at key confirmation with a
    // misleading cause.
    const int name = kvm_json_member(doc, payload, "client_name");
    const size_t nlen =
        kvm_json_str_copy(doc, name, out->hello.client_name, sizeof(out->hello.client_name));
    if (nlen == (size_t)-1 || nlen == 0u) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    for (size_t i = 0; i < nlen; i++) {
        const unsigned char c = (unsigned char)out->hello.client_name[i];
        if (c < 0x20u || c == 0x7Fu) {
            out->hello.client_name[i] = '?';
        }
    }

    char hex[KVM_PAIR_HEX32_CAP];
    const int cid = kvm_json_member(doc, payload, "client_id");
    if (kvm_json_str_copy(doc, cid, hex, sizeof(hex)) == (size_t)-1) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    size_t n = 0;
    if (!kvm_pair_hex_decode(hex, out->hello.client_id, sizeof(out->hello.client_id), &n) ||
        n != KVM_PAIR_CLIENT_ID_LEN) {
        return KVM_RP_ERR_BAD_FRAME;
    }

    const int cert = kvm_json_member(doc, payload, "cert_sha256");
    if (kvm_json_str_copy(doc, cert, hex, sizeof(hex)) == (size_t)-1) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    if (!kvm_pair_hex_decode(hex, out->hello.cert_sha256, sizeof(out->hello.cert_sha256), &n) ||
        n != KVM_PAIR_SHA256_LEN) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    return KVM_RP_OK;
}

kvm_rp_err_t kvm_srt_pair_parse_blob(const kvm_json_doc_t *doc, int payload, const char *field,
                                     uint8_t *out, size_t cap, size_t exact, size_t *out_len)
{
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (doc == NULL || field == NULL || out == NULL) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    const char *allowed[2];
    allowed[0] = field;
    allowed[1] = NULL;
    if (kvm_json_type(doc, payload) != KVM_JSON_OBJECT ||
        !kvm_json_keys_allowed(doc, payload, allowed) || kvm_json_count(doc, payload) != 1u) {
        return KVM_RP_ERR_BAD_FRAME;
    }

    // The longest field is a base64 round-1 blob; it must fit without a heap buffer.
    char b64[KVM_PAIR_ROUND1_B64_CAP];
    const int tok = kvm_json_member(doc, payload, field);
    if (kvm_json_str_copy(doc, tok, b64, sizeof(b64)) == (size_t)-1) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    size_t n = 0;
    if (!kvm_pair_b64_decode(b64, out, cap, &n)) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    if (exact != 0u && n != exact) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    if (n == 0u) {
        return KVM_RP_ERR_BAD_FRAME;
    }
    if (out_len != NULL) {
        *out_len = n;
    }
    return KVM_RP_OK;
}

// --- serialization ---------------------------------------------------------------------------

size_t kvm_srt_pair_frame(const kvm_pair_out_t *out, unsigned part, uint32_t seq, const char *label,
                          const char *device_name, char *buf, size_t cap)
{
    if (out == NULL || buf == NULL || cap == 0u) {
        return 0u;
    }
    const char *name = (device_name != NULL) ? device_name : "";

    // Which message this (kind, part) pair is. KVM_PAIR_OUT_CONFIRM_ACCEPTED is the only kind with
    // two parts: the confirmation tag, then the sealed token (ADR-029 §4 rows 7 and 8).
    kvm_pair_out_kind_t kind = out->kind;
    if (kind == KVM_PAIR_OUT_CONFIRM_ACCEPTED) {
        if (part == 0u) {
            kind = KVM_PAIR_OUT_CONFIRM;
        } else if (part == 1u) {
            kind = KVM_PAIR_OUT_ACCEPTED;
        } else {
            return 0u;
        }
    } else if (part != 0u) {
        return 0u;
    }

    wr_t w;
    wr_begin(&w, buf, cap);
    char b64[KVM_PAIR_ROUND1_B64_CAP];

    switch (kind) {
    case KVM_PAIR_OUT_ROUND1:
    case KVM_PAIR_OUT_ROUND2:
        if (!kvm_pair_b64_encode(out->blob, out->blob_len, b64, sizeof(b64))) {
            return 0u;
        }
        wr_envelope(&w, (kind == KVM_PAIR_OUT_ROUND1) ? "pair.round1" : "pair.round2", seq, label);
        wr_lit(&w, "\"blob\":");
        wr_str(&w, b64);
        // `pair.round1` doubles as the acknowledgment of `pair.hello`, so it carries the advertised
        // DEVICE name the CLIENT shows next to the code (ADR-029 §4 row 2).
        if (kind == KVM_PAIR_OUT_ROUND1) {
            wr_lit(&w, ",\"device_name\":");
            wr_str(&w, name);
        }
        break;

    case KVM_PAIR_OUT_CONFIRM:
        if (!kvm_pair_b64_encode(out->blob, out->blob_len, b64, sizeof(b64))) {
            return 0u;
        }
        wr_envelope(&w, "pair.confirm", seq, label);
        wr_lit(&w, "\"mac\":");
        wr_str(&w, b64);
        break;

    case KVM_PAIR_OUT_ACCEPTED:
        if (!kvm_pair_b64_encode(out->token, out->token_len, b64, sizeof(b64))) {
            return 0u;
        }
        wr_envelope(&w, "pair.accepted", seq, label);
        wr_lit(&w, "\"token_enc\":");
        wr_str(&w, b64);
        wr_lit(&w, ",\"device_name\":");
        wr_str(&w, name);
        break;

    case KVM_PAIR_OUT_REJECTED:
        wr_envelope(&w, "pair.rejected", seq, label);
        wr_lit(&w, "\"reason\":");
        wr_str(&w, (out->reason != NULL) ? out->reason : "local_reject");
        break;

    case KVM_PAIR_OUT_NONE:
    case KVM_PAIR_OUT_ERROR:
    case KVM_PAIR_OUT_CONFIRM_ACCEPTED:
    default:
        return 0u;
    }

    wr_lit(&w, "}}");
    if (w.overflow) {
        buf[0] = '\0';
        return 0u;
    }
    return w.len;
}

size_t kvm_srt_error_frame(kvm_rp_err_t err, uint32_t seq, const char *label, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0u) {
        return 0u;
    }
    wr_t w;
    wr_begin(&w, buf, cap);
    wr_envelope(&w, "error", seq, label);
    wr_lit(&w, "\"code\":");
    wr_str(&w, kvm_rp_err_name(err));
    wr_lit(&w, ",\"retryable\":");
    wr_lit(&w, kvm_rp_err_retryable(err) ? "true" : "false");
    wr_lit(&w, "}}");
    if (w.overflow) {
        buf[0] = '\0';
        return 0u;
    }
    return w.len;
}

void kvm_srt_pair_label(char out[KVM_SRT_PAIR_LABEL_CAP], uint32_t entropy)
{
    static const char hex[] = "0123456789abcdef";
    if (out == NULL) {
        return;
    }
    out[0] = 'p';
    out[1] = '-';
    for (int i = 0; i < 4; i++) {
        out[2 + i] = hex[(entropy >> (12 - 4 * i)) & 0x0Fu];
    }
    out[6] = '\0';
}
