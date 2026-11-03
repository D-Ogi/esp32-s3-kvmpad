// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Server -> client serializer (spec 03 §6.4, §6.6).
#include "remote_wire.h"

#include <string.h>

void kvm_rp_wb_init(kvm_rp_wbuf_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->ok = cap > 0u;
    if (cap > 0u) {
        buf[0] = '\0';
    }
}

static void wb_ch(kvm_rp_wbuf_t *w, char c)
{
    if (!w->ok) {
        return;
    }
    if (w->len + 1u >= w->cap) {
        w->ok = false;
        return;
    }
    w->buf[w->len++] = c;
    w->buf[w->len] = '\0';
}

void kvm_rp_wb_lit(kvm_rp_wbuf_t *w, const char *s)
{
    for (; *s != '\0'; s++) {
        wb_ch(w, *s);
    }
}

void kvm_rp_wb_jstr(kvm_rp_wbuf_t *w, const char *s)
{
    static const char hexd[] = "0123456789abcdef";
    wb_ch(w, '"');
    for (; *s != '\0'; s++) {
        uint8_t c = (uint8_t)*s;
        switch (c) {
        case '"':
            kvm_rp_wb_lit(w, "\\\"");
            break;
        case '\\':
            kvm_rp_wb_lit(w, "\\\\");
            break;
        case '\n':
            kvm_rp_wb_lit(w, "\\n");
            break;
        case '\r':
            kvm_rp_wb_lit(w, "\\r");
            break;
        case '\t':
            kvm_rp_wb_lit(w, "\\t");
            break;
        default:
            if (c < 0x20u) {
                kvm_rp_wb_lit(w, "\\u00");
                wb_ch(w, hexd[(c >> 4) & 0x0Fu]);
                wb_ch(w, hexd[c & 0x0Fu]);
            } else {
                wb_ch(w, (char)c);
            }
            break;
        }
    }
    wb_ch(w, '"');
}

void kvm_rp_wb_u64(kvm_rp_wbuf_t *w, uint64_t v)
{
    char tmp[21];
    int n = 0;
    if (v == 0u) {
        wb_ch(w, '0');
        return;
    }
    while (v > 0u && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        wb_ch(w, tmp[n]);
    }
}

void kvm_rp_wb_i64(kvm_rp_wbuf_t *w, int64_t v)
{
    if (v < 0) {
        wb_ch(w, '-');
        kvm_rp_wb_u64(w, (uint64_t)(-(v + 1)) + 1u);
    } else {
        kvm_rp_wb_u64(w, (uint64_t)v);
    }
}

// ---- envelope ------------------------------------------------------------------------------

static void envelope_open(kvm_rp_t *rp, kvm_rp_wbuf_t *w, kvm_rp_session_t *s, const char *type,
                          uint64_t reply_to)
{
    kvm_rp_wb_init(w, rp->out, sizeof(rp->out));
    kvm_rp_wb_lit(w, "{\"v\":1,\"type\":");
    kvm_rp_wb_jstr(w, type);
    kvm_rp_wb_lit(w, ",\"seq\":");
    s->out_seq++;
    kvm_rp_wb_u64(w, (uint64_t)s->out_seq);
    kvm_rp_wb_lit(w, ",\"session_id\":");
    kvm_rp_wb_jstr(w, s->label);
    if (rp->cfg.send_reply_to_seq && reply_to != 0u) {
        kvm_rp_wb_lit(w, ",\"reply_to_seq\":");
        kvm_rp_wb_u64(w, reply_to);
    }
}

static void envelope_send(kvm_rp_t *rp, kvm_rp_wbuf_t *w, kvm_rp_session_t *s)
{
    kvm_rp_wb_lit(w, "}");
    if (!w->ok) {
        rp->c.frames_out_truncated++;
        return;
    }
    rp->c.frames_out++;
    if (rp->hooks.send != NULL) {
        rp->hooks.send(rp->hooks.ctx, s->id, w->buf, w->len);
    }
}

static const char *host_state_name(kvm_host_usb_t u)
{
    switch (u) {
    case KVM_HOST_USB_MOUNTED:
        return "USB_MOUNTED";
    case KVM_HOST_USB_SUSPENDED:
        return "USB_SUSPENDED";
    case KVM_HOST_USB_DETACHED:
        return "USB_DETACHED";
    case KVM_HOST_USB_ENUMERATING:
        return "USB_ENUMERATING";
    case KVM_HOST_USB_ERROR:
        return "USB_ERROR";
    default:
        return "USB_UNINITIALIZED";
    }
}

static const char *owner_name(kvm_owner_kind_t k)
{
    switch (k) {
    case OWNER_LOCAL:
        return "LOCAL";
    case OWNER_REMOTE:
        return "REMOTE";
    default:
        return "NONE";
    }
}

/**
 * FR-745 / ADR-024: the layout id is a read-only enumerated value from kbd_layouts. The allowlist
 * itself lives there; here we only guarantee that nothing free-form can reach the wire.
 */
static bool layout_id_safe(const char *s)
{
    if (s[0] == '\0') {
        return false;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '(' || c == ')';
        if (!ok || i >= 23u) {
            return false;
        }
    }
    return true;
}

static bool ascii_id_safe(const char *s)
{
    if (s[0] == '\0') {
        return false;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-' || c == '_' || c == '+';
        if (!ok || i >= 23u) {
            return false;
        }
    }
    return true;
}

// ---- messages ------------------------------------------------------------------------------

void kvm_rp_send_error(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t code, uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "error", reply_to);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"code\":");
    kvm_rp_wb_jstr(&w, kvm_rp_err_name(code));
    kvm_rp_wb_lit(&w, ",\"retryable\":");
    kvm_rp_wb_lit(&w, kvm_rp_err_retryable(code) ? "true" : "false");
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_hello_ack(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to)
{
    kvm_rp_status_t st;
    memset(&st, 0, sizeof(st));
    st.host_usb = rp->host_usb;
    if (rp->hooks.status_fill != NULL) {
        rp->hooks.status_fill(rp->hooks.ctx, &st);
    }
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "hello.ack", reply_to);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"protocol\":1,\"device_id\":");
    kvm_rp_wb_jstr(&w, ascii_id_safe(st.device_id) ? st.device_id : "kvm");
    kvm_rp_wb_lit(&w, ",\"capabilities\":[\"keyboard\",\"mouse\",\"shortcuts\"],\"host\":");
    kvm_rp_wb_jstr(&w, host_state_name(rp->host_usb));
    kvm_rp_wb_lit(&w, ",\"role\":");
    kvm_rp_wb_jstr(&w, s->operator_role ? "operator" : "observer");
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_status(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to)
{
    kvm_rp_status_t st;
    memset(&st, 0, sizeof(st));
    st.host_usb = rp->host_usb;
    st.maintenance = rp->maintenance;
    st.wifi_connected = rp->wifi_connected;
    st.owner = rp->owner_kind;
    st.lease_epoch = rp->owner_epoch;
    if (rp->hooks.status_fill != NULL) {
        rp->hooks.status_fill(rp->hooks.ctx, &st);
    }
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "status.snapshot", reply_to);
    kvm_rp_wb_lit(&w, ",\"server_timestamp_us\":");
    kvm_rp_wb_u64(&w, rp->now_us);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"host\":{\"state\":");
    kvm_rp_wb_jstr(&w, host_state_name(st.host_usb));
    kvm_rp_wb_lit(&w, ",\"transport\":\"usb_hid\"},\"client\":{\"state\":\"CLIENT_CONNECTED\",\"role\":");
    kvm_rp_wb_jstr(&w, s->operator_role ? "operator" : "observer");
    kvm_rp_wb_lit(&w, "},\"control\":{\"owner\":");
    kvm_rp_wb_jstr(&w, owner_name(st.owner));
    kvm_rp_wb_lit(&w, ",\"lease_epoch\":");
    kvm_rp_wb_u64(&w, (uint64_t)st.lease_epoch);
    kvm_rp_wb_lit(&w, "},\"device\":{\"wifi\":");
    kvm_rp_wb_jstr(&w, st.wifi_connected ? "CONNECTED" : "DISCONNECTED");
    kvm_rp_wb_lit(&w, ",\"maintenance\":");
    kvm_rp_wb_lit(&w, st.maintenance ? "true" : "false");
    kvm_rp_wb_lit(&w, ",\"fw\":");
    kvm_rp_wb_jstr(&w, ascii_id_safe(st.fw) ? st.fw : "0.0.0");
    kvm_rp_wb_lit(&w, ",\"host_layout\":");
    kvm_rp_wb_jstr(&w, layout_id_safe(st.host_layout) ? st.host_layout : "us");
    kvm_rp_wb_lit(&w, "}}");
    envelope_send(rp, &w, s);
    rp->c.status_snapshots++;
}

void kvm_rp_send_control_pending(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "control.pending", reply_to);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"approval_required\":true,\"expires_in_s\":");
    kvm_rp_wb_u64(&w, rp->cfg.approval_timeout_us / 1000000u);
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_control_granted(kvm_rp_t *rp, kvm_rp_session_t *s, uint32_t lease_epoch,
                                 uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "control.granted", reply_to);
    kvm_rp_wb_lit(&w, ",\"lease_epoch\":");
    kvm_rp_wb_u64(&w, (uint64_t)lease_epoch);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"expires_in_ms\":");
    kvm_rp_wb_u64(&w, rp->cfg.lease_us / 1000u);
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_control_revoked(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_revoke_reason_t reason,
                                 uint32_t new_epoch, uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "control.revoked", reply_to);
    kvm_rp_wb_lit(&w, ",\"payload\":{\"reason\":");
    kvm_rp_wb_jstr(&w, kvm_rp_revoke_reason_name(reason));
    kvm_rp_wb_lit(&w, ",\"new_epoch\":");
    kvm_rp_wb_u64(&w, (uint64_t)new_epoch);
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_pong(kvm_rp_t *rp, kvm_rp_session_t *s, bool has_nonce, int64_t nonce,
                      uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "pong", reply_to);
    kvm_rp_wb_lit(&w, ",\"payload\":{");
    if (has_nonce) {
        kvm_rp_wb_lit(&w, "\"nonce\":");
        kvm_rp_wb_i64(&w, nonce);
    }
    kvm_rp_wb_lit(&w, "}");
    envelope_send(rp, &w, s);
}

void kvm_rp_send_input_ack(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to)
{
    kvm_rp_wbuf_t w;
    envelope_open(rp, &w, s, "input.ack", reply_to);
    kvm_rp_wb_lit(&w, ",\"server_timestamp_us\":");
    kvm_rp_wb_u64(&w, rp->now_us);
    kvm_rp_wb_lit(&w, ",\"payload\":{}");
    envelope_send(rp, &w, s);
}
