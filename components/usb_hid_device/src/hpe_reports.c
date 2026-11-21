// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "hpe_reports.h"
#include <string.h>

static void put16(uint8_t *out, uint16_t v)
{
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

bool hpe_frame_valid(const hpe_frame_t *f)
{
    if (!f || f->count > HPE_CONTACTS) return false;
    for (unsigned i = 0; i < f->count; i++) {
        if (f->contact[i].x > 32767 || f->contact[i].y > 32767) return false;
        for (unsigned j = 0; j < i; j++) {
            if (f->contact[i].id == f->contact[j].id) return false;
        }
    }
    return true;
}

size_t hpe_encode_ptp(uint8_t out[HPE_PTP_WIRE], const hpe_frame_t *f)
{
    if (!hpe_frame_valid(f)) return 0;
    memset(out, 0, HPE_PTP_WIRE);
    out[0] = HPE_PTP;
    for (unsigned i = 0; i < f->count; i++) {
        const hpe_contact_t *c = &f->contact[i];
        uint8_t *slot = out + 1 + 6 * i;
        slot[0] = (uint8_t)(c->tip | (unsigned)c->confidence << 1);
        slot[1] = c->id;
        put16(slot + 2, c->x);
        put16(slot + 4, c->y);
    }
    put16(out + 31, f->scan_time);
    out[33] = f->count;
    return HPE_PTP_WIRE;
}

bool hpe_joystick_valid(const hpe_joystick_t *j)
{
    return j && j->x != INT16_MIN && j->y != INT16_MIN && j->rz != INT16_MIN && j->hat <= 8;
}

size_t hpe_encode_joystick(uint8_t out[HPE_JOY_WIRE], const hpe_joystick_t *j)
{
    if (!hpe_joystick_valid(j)) return 0;
    out[0] = HPE_JOYSTICK;
    put16(out + 1, (uint16_t)j->x);
    put16(out + 3, (uint16_t)j->y);
    put16(out + 5, (uint16_t)j->rz);
    put16(out + 7, j->z);
    put16(out + 9, j->buttons);
    out[11] = j->hat;
    return HPE_JOY_WIRE;
}

size_t hpe_encode_radial(uint8_t out[3], bool pressed, int16_t tenths)
{
    if (tenths < -3600 || tenths > 3600) return 0;
    out[0] = HPE_RADIAL;
    put16(out + 1, (uint16_t)(((unsigned)(uint16_t)tenths << 1) | pressed));
    return 3;
}

static uint16_t crc16(const uint8_t *p, size_t len)
{
    uint16_t crc = UINT16_MAX;
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
        }
    }
    return crc;
}

static bool packet_fields(uint8_t id, const hpe_packet_t *p)
{
    if ((id != HPE_STUDIO_IN && id != HPE_STUDIO_OUT) || p->flags > 3 ||
        p->length > 51 || p->count > 8 || p->index >= p->count) return false;
    switch (p->opcode) {
    case 0x01: case 0x10: return id == HPE_STUDIO_IN;
    case 0x02: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: return id == HPE_STUDIO_OUT;
    case 0x03: case 0x04: case 0x20: case 0x21: case 0x22: return true;
    default: return false;
    }
}

bool hpe_encode_packet(uint8_t out[64], uint8_t id, const hpe_packet_t *p)
{
    if (!packet_fields(id, p)) return false;
    memset(out, 0, 64);
    out[0] = id;
    out[1] = 1;
    out[2] = p->opcode;
    out[3] = p->flags;
    put16(out + 4, p->sequence);
    put16(out + 6, p->ack);
    out[8] = p->length;
    out[9] = p->index;
    out[10] = p->count;
    memcpy(out + 11, p->payload, p->length);
    put16(out + 62, crc16(out, 62));
    return true;
}

bool hpe_decode_packet(const uint8_t *wire, size_t len, uint8_t id, hpe_packet_t *out)
{
    if (!wire || !out || len != 64 || wire[0] != id || wire[1] != 1 ||
        get16(wire + 62) != crc16(wire, 62)) return false;
    hpe_packet_t p = {.opcode = wire[2], .flags = wire[3], .sequence = get16(wire + 4),
                      .ack = get16(wire + 6), .length = wire[8], .index = wire[9], .count = wire[10]};
    if (!packet_fields(id, &p)) return false;
    for (unsigned i = 11u + p.length; i < 62; i++) {
        if (wire[i]) return false;
    }
    memcpy(p.payload, wire + 11, p.length);
    *out = p;
    return true;
}

bool hpe_decode_output(uint8_t report_id, const uint8_t *data, size_t length, hpe_packet_t *packet)
{
    if (report_id == 0) return hpe_decode_packet(data, length, HPE_STUDIO_OUT, packet);
    if (!data || report_id != HPE_STUDIO_OUT || length != HPE_STUDIO_WIRE - 1) return false;
    uint8_t wire[HPE_STUDIO_WIRE] = {HPE_STUDIO_OUT};
    memcpy(wire + 1, data, length);
    return hpe_decode_packet(wire, sizeof(wire), HPE_STUDIO_OUT, packet);
}

bool hpe_feature_set(hpe_features_t *s, uint8_t id, const uint8_t *data, size_t len)
{
    if (!data || len != 1) return false;
    switch (id) {
    case HPE_INPUT_MODE: s->mode = (*data == 3) ? 3 : 0; return true;
    case HPE_SELECT:
        if (*data > 3) return false;
        s->selective = *data;
        return true;
    case HPE_LATENCY:
        if (*data > 1) return false;
        s->latency = *data;
        return true;
    default: return false;
    }
}

size_t hpe_feature_get(const hpe_features_t *s, uint8_t id, uint8_t *data, size_t len)
{
    if (!data || !len) return 0;
    switch (id) {
    case HPE_CAPS: *data = 0x25; break;
    case HPE_INPUT_MODE: *data = s->mode; break;
    case HPE_SELECT: *data = s->selective; break;
    case HPE_LATENCY: *data = s->latency; break;
    default: return 0;
    }
    return 1;
}
