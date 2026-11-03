// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Private: server -> client serializer (spec 03 §6.4, §6.6). Bounded appender over the single
// kvm_rp_t::out buffer; a reply that would exceed 1024 B is dropped whole and counted, never sent
// half-written. No payload, usage id, token or client name is ever serialized into an error frame
// (TECH-046).
#pragma once

#include "remote_protocol.h"

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} kvm_rp_wbuf_t;

void kvm_rp_wb_init(kvm_rp_wbuf_t *w, char *buf, size_t cap);
void kvm_rp_wb_lit(kvm_rp_wbuf_t *w, const char *s);
void kvm_rp_wb_jstr(kvm_rp_wbuf_t *w, const char *s); // JSON string value, quotes included
void kvm_rp_wb_u64(kvm_rp_wbuf_t *w, uint64_t v);
void kvm_rp_wb_i64(kvm_rp_wbuf_t *w, int64_t v);

// ---- message builders ----------------------------------------------------------------------
// `reply_to` is the client seq being answered, or 0 for an unsolicited frame.

void kvm_rp_send_error(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_err_t code, uint64_t reply_to);
void kvm_rp_send_hello_ack(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to);
void kvm_rp_send_status(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to);
void kvm_rp_send_control_pending(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to);
void kvm_rp_send_control_granted(kvm_rp_t *rp, kvm_rp_session_t *s, uint32_t lease_epoch,
                                 uint64_t reply_to);
void kvm_rp_send_control_revoked(kvm_rp_t *rp, kvm_rp_session_t *s, kvm_rp_revoke_reason_t reason,
                                 uint32_t new_epoch, uint64_t reply_to);
void kvm_rp_send_pong(kvm_rp_t *rp, kvm_rp_session_t *s, bool has_nonce, int64_t nonce,
                      uint64_t reply_to);
void kvm_rp_send_input_ack(kvm_rp_t *rp, kvm_rp_session_t *s, uint64_t reply_to);
