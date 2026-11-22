// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <libssh/libssh.h>

#ifdef __cplusplus
extern "C" {
#endif

// Call after NVS and libssh initialization, on the single owner task.
// On success the caller owns *key. SSH_BIND_OPTIONS_IMPORT_KEY takes that ownership
// on success. sha256 is the digest of the OpenSSH public key blob used by the TLS hello.
// Failure leaves *key NULL and sha256 all zero.
bool kvm_ssh_identity_open_nvs(ssh_key *key, uint8_t sha256[32]);

#ifdef __cplusplus
}
#endif
