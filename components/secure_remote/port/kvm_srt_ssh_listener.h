// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "kvm_srt.h"

// Lifecycle on the WSS server's owner task. Port 22 opens only on a successful key binding.
bool kvm_ssh_listener_start(uint8_t host_fingerprint[32]);
void kvm_ssh_listener_stop(void);
bool kvm_ssh_listener_open(void);
void kvm_ssh_listener_close_port(void);

// Serialized access from the SSH socket task to the same state used by WSS and the LCD.
uint64_t kvm_ssh_board_sponsor(const char *address, const uint8_t *key);
uint64_t kvm_ssh_board_begin(const char *address, const uint8_t key[32], uint64_t sponsor);
kvm_srt_ssh_state_t kvm_ssh_board_state(uint64_t id);
bool kvm_ssh_board_input(uint64_t id, const kvm_hid_intent_t *intent);
bool kvm_ssh_board_ping(uint64_t id);
void kvm_ssh_board_release(uint64_t id);
