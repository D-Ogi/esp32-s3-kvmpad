// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "kvm_hid_intent.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { KVM_SSH_COMMAND_MAX = 64 }; // Includes a supplied LF or CRLF.

// Parse exactly one ASCII command. Input need not have a trailing NUL. Separate
// tokens with spaces or tabs. An optional final LF or CRLF is accepted.
//   key down|up USAGE                    (unsigned decimal or 0xHH, HID page 7)
//   mouse move DX DY                     (signed decimal, -4096..4096)
//   mouse wheel HORIZONTAL VERTICAL      (same range)
//   mouse down|up left|right|middle
//   release-all
// Success yields one REMOTE intent with zero epoch, sequence and timestamp.
// Session authorization, sequencing and dispatch belong to the caller. Failure
// leaves *out unchanged. A stream reader must discard an entire overlong line.
bool kvm_ssh_parse_command(const char *command, size_t length, kvm_hid_intent_t *out);

#ifdef __cplusplus
}
#endif
