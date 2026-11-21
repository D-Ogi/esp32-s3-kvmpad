// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    HPE_PTP = 0x10, HPE_CAPS = 0x11, HPE_CERT = 0x12, HPE_INPUT_MODE = 0x13,
    HPE_SELECT = 0x14, HPE_LATENCY = 0x15, HPE_RADIAL = 0x20, HPE_CONSUMER = 0x21,
    HPE_STUDIO_IN = 0x22, HPE_STUDIO_OUT = 0x23, HPE_STUDIO_FEATURE = 0x24, HPE_JOYSTICK = 0x30,
    HPE_CONTACTS = 5, HPE_PTP_WIRE = 34, HPE_JOY_WIRE = 12, HPE_STUDIO_WIRE = 64
};

typedef struct {
    uint8_t id;
    bool tip, confidence;
    uint16_t x, y;
} hpe_contact_t;
typedef struct {
    hpe_contact_t contact[HPE_CONTACTS];
    uint16_t scan_time;  // 100-microsecond units, modulo 65536
    uint8_t count;       // includes liftoff slots
} hpe_frame_t;
typedef struct {
    int16_t x, y, rz;
    uint16_t z, buttons;
    uint8_t hat; // 0..7, or 8 (neutral)
} hpe_joystick_t;

bool hpe_frame_valid(const hpe_frame_t *frame);
bool hpe_joystick_valid(const hpe_joystick_t *joystick);
size_t hpe_encode_ptp(uint8_t out[HPE_PTP_WIRE], const hpe_frame_t *frame);
size_t hpe_encode_joystick(uint8_t out[HPE_JOY_WIRE], const hpe_joystick_t *joystick);
size_t hpe_encode_radial(uint8_t out[3], bool pressed, int16_t tenths);

typedef struct {
    uint8_t opcode, flags, length, index, count;
    uint16_t sequence, ack;
    uint8_t payload[51];
} hpe_packet_t;
bool hpe_encode_packet(uint8_t out[HPE_STUDIO_WIRE], uint8_t id, const hpe_packet_t *packet);
bool hpe_decode_packet(const uint8_t *wire, size_t len, uint8_t id, hpe_packet_t *packet);

// TinyUSB delivers the complete wire report on interrupt OUT and strips the ID on EP0.
bool hpe_decode_output(uint8_t report_id, const uint8_t *data, size_t length, hpe_packet_t *packet);

typedef struct {
    uint8_t mode, selective, latency;
} hpe_features_t;
#define HPE_FEATURE_DEFAULTS { .selective = 3 }
bool hpe_feature_set(hpe_features_t *state, uint8_t id, const uint8_t *data, size_t len);
size_t hpe_feature_get(const hpe_features_t *state, uint8_t id, uint8_t *data, size_t len);
