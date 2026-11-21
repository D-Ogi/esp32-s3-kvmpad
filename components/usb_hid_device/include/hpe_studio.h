// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include "hpe_reports.h"

enum { HPE_DEVICE_CAPS = 0x3f, HPE_AGENT_CAPS = 0x21, HPE_OBS_TILES = 3 };
enum {
    HPE_MOUSE_PROFILE,
    HPE_KEYBOARD_PROFILE,
    HPE_DIAL_PROFILE,
    HPE_STUDIO_PROFILE,
    HPE_ACCESS_PROFILE,
    HPE_SETTINGS_PROFILE
};
typedef enum { HPE_IDLE, HPE_REQUESTED, HPE_DELIVERED, HPE_ACCEPTED, HPE_FINISHED } hpe_action_phase_t;
typedef struct {
    uint32_t peer_nonce, request_id, result_version, state_version[HPE_OBS_TILES];
    uint16_t tile_id, result;
    uint8_t supported, bank, state[HPE_OBS_TILES];
    char label[HPE_OBS_TILES][25];
    hpe_action_phase_t phase;
} hpe_studio_status_t;

typedef struct {
    uint8_t opcode, flags, fragments, next_index;
    uint16_t sequence, ack, length;
    uint64_t deadline_us;
    uint8_t bytes[408];
} hpe_message_t;

typedef struct {
    hpe_message_t rx;
    hpe_studio_status_t status;
    uint16_t tx_sequence, previous_rx, peer_caps, action_seq, ack_seq, error_seq;
    uint32_t device_nonce, ping_tick, peer_tick, next_request;
    uint64_t next_ping_us, last_pong_us, action_at, rate_at, next_error_us;
    uint8_t sends, rate_count, previous_opcode;
    bool ready, have_rx, pong_pending, awaiting_pong, ack_pending, fragment_error;
} hpe_studio_t;

void hpe_studio_start(hpe_studio_t *link, uint32_t device_nonce);
void hpe_studio_tick(hpe_studio_t *link, uint64_t now_us);
void hpe_studio_cancel(hpe_studio_t *link);
bool hpe_studio_action(hpe_studio_t *link, uint16_t tile, uint16_t action, uint32_t peer_nonce, uint64_t now_us);
bool hpe_studio_receive(hpe_studio_t *link, const hpe_packet_t *p, uint64_t now_us);
bool hpe_studio_next(hpe_studio_t *link, uint64_t now_us, hpe_packet_t *out);
