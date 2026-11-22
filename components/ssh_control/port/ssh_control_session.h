// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <libssh/libssh.h>
#include "ssh_control.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KVM_SSH_DIAG_SETUP, KVM_SSH_DIAG_KEX, KVM_SSH_DIAG_QUERY, KVM_SSH_DIAG_QUERY_ACCEPTED, KVM_SSH_DIAG_VALID,
    KVM_SSH_DIAG_AUTHENTICATED, KVM_SSH_DIAG_CHANNEL, KVM_SSH_DIAG_SUBSYSTEM,
    KVM_SSH_DIAG_SHELL, KVM_SSH_DIAG_END
} kvm_ssh_diag_phase_t;

typedef struct {
    // SHA-256 of the SSH public key blob, as in an OpenSSH SHA256 fingerprint.
    // A public-key query is read-only. Check against the existing pairing.
    bool (*paired)(void *ctx, const char *username, const uint8_t sha256[32]);
    // Called once for a signed-in client requesting the restricted command stream.
    // Return only on grant/denial or a bounded timeout. Busy is a denial.
    bool (*request_local_approval)(void *ctx, const char *username, const uint8_t sha256[32]);
    // Report whether this same authenticated lease is still valid (including revocation).
    bool (*active)(void *ctx);
    // Recheck the lease atomically with dispatch. Ownership of epoch/seq stays with the sink.
    bool (*command)(void *ctx, const kvm_hid_intent_t *intent);
    // Exactly once after an approval attempt. Cancel/release only this ctx's lease.
    void (*release)(void *ctx);
    // Optional. Renew the lease on authenticated keepalives or accepted commands.
    bool (*peer_activity)(void *ctx);
    // Optional. Only fixed protocol stages and static end-reason literals.
    void (*diagnostic)(void *ctx, kvm_ssh_diag_phase_t stage, const char *end_reason);
} kvm_ssh_session_ops_t;

// Run one already accepted libssh session on its owner task. Caller supplies the
// pinned host key/bind, owns disconnect/free, and must not reuse the session.
// A single shell or "kvmpad" subsystem accepts newline-terminated HID commands.
// No PTY or exec. Returns true on a complete, approved stream ending in EOF.
bool kvm_ssh_serve_session(ssh_session session, const kvm_ssh_session_ops_t *ops, void *ctx);

#ifdef __cplusplus
}
#endif
