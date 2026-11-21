// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

#include "hpe_studio.h"
#include <string.h>

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t read32(const uint8_t *p) { return read16(p) | (uint32_t)read16(p + 2) << 16; }
static void write16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void write32(uint8_t *p, uint32_t v)
{
    write16(p, (uint16_t)v);
    write16(p + 2, (uint16_t)(v >> 16));
}

void hpe_studio_start(hpe_studio_t *link, uint32_t nonce)
{
    memset(link, 0, sizeof(*link));
    link->device_nonce = nonce;
}

static void new_session(hpe_studio_t *link)
{
    // Different challenge across a live USB session reset. Mount supplies fresh hardware entropy.
    uint32_t nonce = link->device_nonce * 1664525u + 1013904223u;
    hpe_studio_start(link, nonce ? nonce : 1);
}

void hpe_studio_cancel(hpe_studio_t *link)
{
    link->status.phase = HPE_IDLE;
    link->status.request_id = 0;
    link->sends = 0;
}

static bool pending(const hpe_studio_status_t *s)
{
    return s->phase == HPE_REQUESTED || s->phase == HPE_DELIVERED || s->phase == HPE_ACCEPTED;
}

void hpe_studio_tick(hpe_studio_t *link, uint64_t now)
{
    if (link->ready && now - link->last_pong_us >= 6000000) {
        new_session(link);
        return;
    }
    hpe_studio_status_t *s = &link->status;
    if (pending(s) && now - link->action_at >= (s->phase == HPE_ACCEPTED ? 2000000u : 600000u)) {
        s->phase = HPE_FINISHED;
        s->result = 3;
    }
}

bool hpe_studio_action(hpe_studio_t *link, uint16_t tile, uint16_t action, uint32_t peer_nonce, uint64_t now)
{
    hpe_studio_tick(link, now);
    hpe_studio_status_t *s = &link->status;
    if (!link->ready || !peer_nonce || s->peer_nonce != peer_nonce || pending(s) || tile < 1 || tile > HPE_OBS_TILES ||
        action != 0x100u + tile || !(s->supported & s->bank & (1u << (tile - 1))))
        return false;
    if (++link->next_request == 0) ++link->next_request;
    s->request_id = link->next_request;
    s->tile_id = tile;
    s->phase = HPE_REQUESTED;
    s->result = 6;
    s->result_version = 0;
    link->action_at = now;
    link->sends = 0;
    return true;
}

// Accept printable Unicode scalar values, excluding controls, bidi overrides and invisibles.
static bool label_valid(const uint8_t *p, unsigned n)
{
    for (unsigned i = 0; i < n;) {
        uint32_t c = p[i++];
        unsigned extra = 0;
        if (c >= 0x80) {
            if (c >= 0xc2 && c <= 0xdf) {
                c &= 31;
                extra = 1;
            } else if (c >= 0xe0 && c <= 0xef) {
                c &= 15;
                extra = 2;
            } else if (c >= 0xf0 && c <= 0xf4) {
                c &= 7;
                extra = 3;
            } else
                return false;
            if (n - i < extra) return false;
            for (unsigned j = 0; j < extra; j++) {
                if ((p[i] & 0xc0) != 0x80) return false;
                c = (c << 6) | (p[i++] & 63u);
            }
            if ((extra == 2 && c < 0x800) || (extra == 3 && c < 0x10000)) return false;
        }
        if (c < 32 || (c >= 0x7f && c <= 0x9f) || (c >= 0xd800 && c <= 0xdfff) || c > 0x10ffff ||
            (c >= 0x200b && c <= 0x200f) || (c >= 0x2028 && c <= 0x202e) || (c >= 0x2060 && c <= 0x206f) || c == 0xfeff)
            return false;
    }
    return true;
}

static bool bank_info(hpe_studio_t *link, const uint8_t *p, unsigned n)
{
    if (n < 3 || read16(p) != 1 || p[2] > HPE_OBS_TILES) return false;
    char labels[HPE_OBS_TILES][25] = {{0}};
    uint8_t mask = 0;
    unsigned off = 3;
    for (unsigned j = 0; j < p[2]; j++) {
        if (n - off < 5) return false;
        unsigned tile = read16(p + off), action = read16(p + off + 2), size = p[off + 4];
        off += 5;
        if (tile < 1 || tile > HPE_OBS_TILES || action != 0x100u + tile || size == 0 || size > 24 || n - off < size ||
            !label_valid(p + off, size) || (mask & (1u << (tile - 1))))
            return false;
        memcpy(labels[tile - 1], p + off, size);
        off += size;
        mask |= (uint8_t)(1u << (tile - 1));
    }
    if (off != n) return false;
    hpe_studio_cancel(link);
    memcpy(link->status.label, labels, sizeof(labels));
    memset(link->status.state, 0, sizeof(link->status.state));
    memset(link->status.state_version, 0, sizeof(link->status.state_version));
    link->status.bank = mask;
    return true;
}

static bool receive_message(hpe_studio_t *link, uint64_t now)
{
    hpe_message_t *m = &link->rx;
    hpe_studio_status_t *s = &link->status;
    const uint8_t *b = m->bytes;
    unsigned n = m->length;
    if (m->opcode == 2) {
        if (link->ready || n != 12 || b[0] != 1 || read32(b + 4) != link->device_nonce || !read32(b + 8)) return false;
        s->peer_nonce = read32(b + 8);
        link->peer_caps = read16(b + 2) & HPE_DEVICE_CAPS;
        link->last_pong_us = link->next_ping_us = now;
        link->ready = true;
        return true;
    }
    if (!link->ready) return false;
    if (m->opcode >= 0x11 && m->opcode <= 0x15 && (link->peer_caps & HPE_AGENT_CAPS) != HPE_AGENT_CAPS) return false;
    switch (m->opcode) {
    case 3:
        if (n != 4) return false;
        link->peer_tick = read32(b);
        link->pong_pending = true;
        return true;
    case 4:
        if (!link->awaiting_pong || n != 4 || read32(b) != link->ping_tick) return false;
        link->last_pong_us = now;
        link->awaiting_pong = false;
        return true;
    case 0x11:
        if (n != 5 || !link->sends || !pending(s) || read32(b) != s->request_id || b[4] > 1) return false;
        s->phase = b[4] ? HPE_ACCEPTED : HPE_FINISHED;
        if (!b[4]) s->result = 2;
        return true;
    case 0x12:
        if (n != 10 || !link->sends || s->phase != HPE_ACCEPTED || read32(b) != s->request_id || read16(b + 4) > 6)
            return false;
        s->result = read16(b + 4);
        s->result_version = read32(b + 6);
        s->phase = HPE_FINISHED;
        return true;
    case 0x13: {
        if (n != 9 || read16(b) != 1 || b[4] > 2) return false;
        unsigned id = read16(b + 2);
        uint32_t version = read32(b + 5);
        if (!id || id > HPE_OBS_TILES || !(s->bank & (1u << (id - 1))) || !version ||
            (int32_t)(version - s->state_version[id - 1]) <= 0)
            return false;
        s->state_version[id - 1] = version;
        s->state[id - 1] = b[4];
        return true;
    }
    case 0x14: {
        if (!n || b[0] > HPE_OBS_TILES || n != 1u + b[0] * 2u) return false;
        uint8_t mask = 0;
        for (unsigned i = 0; i < b[0]; i++) {
            unsigned id = read16(b + 1 + i * 2);
            if (id <= 0x100 || id > 0x100 + HPE_OBS_TILES || (mask & (1u << (id - 0x101)))) return false;
            mask |= (uint8_t)(1u << (id - 0x101));
        }
        hpe_studio_cancel(link);
        s->supported = mask;
        return true;
    }
    case 0x15:
        return bank_info(link, b, n);
    case 0x20:
        if (n != 4 || !(m->flags & 2)) return false;
        if (pending(s) && link->sends && read16(b + 2) == link->action_seq) {
            s->result = 5;
            s->phase = HPE_FINISHED;
        }
        return true;
    case 0x21:
        if (n != 2 || m->flags || !link->sends || !pending(s) || m->ack != read16(b) || m->ack != link->action_seq)
            return false;
        if (s->phase == HPE_REQUESTED) s->phase = HPE_DELIVERED;
        return true;
    case 0x22:
        if (n != 2) return false;
        new_session(link);
        return true;
    default:
        return false;
    }
}

static bool bad_fragment(hpe_studio_t *link, uint16_t sequence, uint64_t now)
{
    link->rx.next_index = 0;
    if (link->ready && now >= link->next_error_us) {
        link->error_seq = sequence;
        link->fragment_error = true;
        link->next_error_us = now + 100000;
    }
    return false;
}

bool hpe_studio_receive(hpe_studio_t *link, const hpe_packet_t *p, uint64_t now)
{
    hpe_studio_tick(link, now);
    if (!link->device_nonce || !p || p->flags > 3 || p->length > sizeof(p->payload) || p->count > 8 ||
        p->index >= p->count)
        return false;
    if (now - link->rate_at >= 1000000) {
        link->rate_at = now;
        link->rate_count = 0;
    }
    if (link->rate_count >= 128) return false;
    ++link->rate_count;
    hpe_message_t *m = &link->rx;
    if (m->next_index && now >= m->deadline_us) m->next_index = 0;
    if (!p->index) {
        uint16_t delta = (uint16_t)(p->sequence - link->previous_rx);
        if (link->have_rx && (!delta || delta >= 0x8000)) {
            if (!delta && (p->flags & 1) && p->opcode == link->previous_opcode && link->ready) {
                link->ack_seq = p->sequence;
                link->ack_pending = true;
                return true;
            }
            return false;
        }
        if (m->next_index) return bad_fragment(link, p->sequence, now);
        m->opcode = p->opcode;
        m->flags = p->flags;
        m->sequence = p->sequence;
        m->ack = p->ack;
        m->fragments = p->count;
        m->deadline_us = now + 300000;
        m->next_index = 0;
        m->length = 0;
    }
    if (p->index != m->next_index || p->sequence != m->sequence || p->count != m->fragments || p->opcode != m->opcode ||
        p->flags != m->flags || p->ack != m->ack) {
        return bad_fragment(link, p->sequence, now);
    }
    memcpy(m->bytes + m->length, p->payload, p->length);
    m->length = (uint16_t)(m->length + p->length);
    if (++m->next_index < m->fragments) return true;
    m->next_index = 0;
    if (!receive_message(link, now)) return false;
    if (p->opcode == 0x22) return true;
    link->have_rx = true;
    link->previous_rx = p->sequence;
    link->previous_opcode = p->opcode;
    if (p->flags & 1) {
        link->ack_seq = p->sequence;
        link->ack_pending = true;
    }
    return true;
}

bool hpe_studio_next(hpe_studio_t *link, uint64_t now, hpe_packet_t *p)
{
    hpe_studio_tick(link, now);
    if (!link->device_nonce) return false;
    *p = (hpe_packet_t){.count = 1};
    if (link->ack_pending) {
        link->ack_pending = false;
        p->opcode = 0x21;
        p->length = 2;
        p->ack = link->ack_seq;
        write16(p->payload, link->ack_seq);
    } else if (link->fragment_error) {
        link->fragment_error = false;
        p->opcode = 0x20;
        p->flags = 2;
        p->length = 4;
        write16(p->payload, 1); // ERR_FRAGMENT_SEQUENCE
        write16(p->payload + 2, link->error_seq);
    } else if (link->status.phase == HPE_REQUESTED && link->sends < 3 &&
               now - link->action_at >= link->sends * 100000u) {
        if (!link->sends) link->action_seq = ++link->tx_sequence;
        ++link->sends;
        p->sequence = link->action_seq;
        p->opcode = 0x10;
        p->flags = 1;
        p->length = 8;
        write16(p->payload, (uint16_t)(0x100 + link->status.tile_id));
        write16(p->payload + 2, link->status.tile_id);
        write32(p->payload + 4, link->status.request_id);
        return true;
    } else if (link->pong_pending) {
        p->opcode = 4;
        p->length = 4;
        write32(p->payload, link->peer_tick);
        link->pong_pending = false;
    } else if (now >= link->next_ping_us) {
        link->next_ping_us = now + 2000000;
        if (link->ready) {
            p->opcode = 3;
            p->length = 4;
            link->ping_tick = (uint32_t)(now / 1000);
            link->awaiting_pong = true;
            write32(p->payload, link->ping_tick);
        } else {
            p->opcode = 1;
            p->length = 8;
            p->payload[0] = 1;
            p->payload[2] = HPE_DEVICE_CAPS;
            write32(p->payload + 4, link->device_nonce);
        }
    } else
        return false;
    p->sequence = ++link->tx_sequence;
    return true;
}
