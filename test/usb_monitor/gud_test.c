// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "gud_receiver.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static gud_receiver_t receiver;
static uint8_t guarded[GUD_R1_MAX_BYTES + 2], frame[640 * 480];
static unsigned updates;

static void present(void *context, gud_rect_t rect, const uint8_t *rows)
{
    assert(context == frame && rect.x + rect.width <= 640 && rect.y + rect.height <= 480);
    unsigned stride = (rect.width + 7) / 8;
    for (unsigned y = 0; y < rect.height; ++y) {
        for (unsigned x = 0; x < rect.width; ++x) {
            frame[(rect.y + y) * 640 + rect.x + x] =
                (rows[y * stride + x / 8] >> (7 - x % 8)) & 1;
        }
    }
    ++updates;
}

static void reset(void)
{
    guarded[0] = 0xad;
    guarded[sizeof(guarded) - 1] = 0xda;
    assert(gud_receiver_init(&receiver, guarded + 1, GUD_R1_MAX_BYTES, present, frame));
}

static void status(unsigned expected)
{
    uint8_t reply = 99;
    assert(gud_receiver_get(&receiver, 0, 0, &reply, 1) == 1 && reply == expected);
}

static void modeset(void)
{
    uint8_t reply[64], one = 1;
    // Linux first discovers the format, connector, optional properties and modes.
    static const uint8_t descriptor[] = {
        0x4d,0x61,0x50,0x1d,1, 0,0,0,0, 0, 0,0x96,0,0,
        0x80,2,0,0, 0x20,3,0,0, 0xe0,1,0,0, 0x58,2,0,0
    };
    memset(reply, 0xbb, sizeof(reply));
    assert(gud_receiver_get(&receiver, 1, 0, reply, sizeof(reply)) == 30);
    assert(!memcmp(reply, descriptor, 30) && reply[30] == 0xbb);
    assert(gud_receiver_get(&receiver, 1, 0, reply, 3) == 3);
    assert(gud_receiver_get(&receiver, 0x40, 0, reply, sizeof(reply)) == 1 && reply[0] == 1);
    assert(gud_receiver_get(&receiver, 0x41, 0, reply, sizeof(reply)) == 0);
    assert(gud_receiver_get(&receiver, 0x50, 0, reply, sizeof(reply)) == 5);
    assert(!memcmp(reply, (uint8_t[5]){0}, 5));
    assert(gud_receiver_get(&receiver, 0x51, 0, reply, sizeof(reply)) == 0);
    assert(gud_receiver_set(&receiver, 0x53, 0, NULL, 0));
    assert(gud_receiver_get(&receiver, 0x54, 0, reply, 1) == 1 && reply[0] == 1);
    assert(gud_receiver_get(&receiver, 0x56, 0, reply, sizeof(reply)) == 0);
    assert(gud_receiver_get(&receiver, 0x54, 1, reply, 1) < 0);
    status(4);
    assert(gud_receiver_get(&receiver, 0xff, 0, reply, 1) < 0);
    status(2);
    assert(gud_receiver_get(&receiver, 0x55, 0, reply, sizeof(reply)) == 48);
    assert(reply[4] == 0x80 && reply[5] == 2 && reply[12] == 0xe0 && reply[13] == 1);
    reply[24] = 1;
    reply[25] = 0;
    assert(gud_receiver_set(&receiver, 0x61, 0, reply, 26));
    reply[25] = 1;
    assert(!gud_receiver_set(&receiver, 0x61, 0, reply, 26));
    assert(!gud_receiver_set(&receiver, 0x62, 0, NULL, 0));
    reply[25] = 0;
    reply[21] = 0; // A userspace mode need not carry the preferred bit.
    assert(gud_receiver_set(&receiver, 0x61, 0, reply, 26));
    assert(gud_receiver_set(&receiver, 0x63, 0, &one, 1));
    assert(gud_receiver_set(&receiver, 0x62, 0, NULL, 0));
    assert(gud_receiver_set(&receiver, 0x64, 0, &one, 1));
    assert(receiver.controller_enabled && receiver.display_enabled);
    status(0);
}

// End-of-screen 9x2 crop, with the per-row padding deliberately set.
static const uint8_t crop[] = {
    0x77,2,0,0, 0xde,1,0,0, 9,0,0,0, 2,0,0,0, 4,0,0,0, 0, 0,0,0,0
};
static const uint8_t marks[] = {0x81,0xff, 0x40,0x7f};

static void rejection(void)
{
    uint8_t invalid[sizeof(crop)];
    for (unsigned i = 0; i < sizeof(crop); i++) {
        memcpy(invalid, crop, sizeof(invalid));
        // Large out-of-bounds values, invalid lengths, compression or its size.
        invalid[i] = 0xff;
        assert(!gud_receiver_set(&receiver, 0x60, 0, invalid, sizeof(invalid)));
        status(4);
        assert(!gud_receiver_bulk(&receiver, marks, 4));
    }
    // Short and interrupted control requests discard any previous partial pixels.
    assert(gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(gud_receiver_bulk(&receiver, marks, 1));
    assert(!gud_receiver_set(&receiver, 0x60, 0, crop, 24));
    assert(!gud_receiver_bulk(&receiver, marks + 1, 3));
    assert(gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(gud_receiver_bulk(&receiver, marks, 1));
    assert(!gud_receiver_bulk(&receiver, marks, 4));
    assert(updates == 0);
}

static void pictures(void)
{
    assert(gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(gud_receiver_bulk(&receiver, marks, 3));
    assert(updates == 0);
    assert(gud_receiver_bulk(&receiver, marks + 3, 1));
    assert(updates == 1);
    for (unsigned i = 0; i < sizeof(frame); ++i) {
        bool white = (i / 640 == 478 && (i % 640 == 631 || i % 640 >= 638)) ||
                     (i / 640 == 479 && i % 640 == 632);
        assert(frame[i] == white);
    }

    static const uint8_t full[] = {
        0,0,0,0, 0,0,0,0, 0x80,2,0,0, 0xe0,1,0,0, 0,0x96,0,0, 0, 0,0,0,0
    };
    uint8_t packet[64];
    memset(packet, 0x55, 64);
    assert(gud_receiver_set(&receiver, 0x60, 0, full, 25));
    for (unsigned i = 0; i < 600; i++) {
        assert(updates == 1);
        assert(gud_receiver_bulk(&receiver, packet, sizeof(packet)));
    }
    assert(updates == 2 && guarded[0] == 0xad && guarded[sizeof(guarded)-1] == 0xda);
    for (unsigned i = 0; i < sizeof(frame); i++) assert(frame[i] == (i & 1));

    assert(gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(gud_receiver_bulk(&receiver, marks, 2));
    uint8_t off = 0;
    assert(gud_receiver_set(&receiver, 0x63, 0, &off, 1));
    assert(!gud_receiver_bulk(&receiver, marks + 2, 2));
    assert(!gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    reset();
    assert(!gud_receiver_bulk(&receiver, marks, 4));
    assert(!gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(updates == 2 && !receiver.display_enabled);
    modeset();
    assert(gud_receiver_set(&receiver, 0x60, 0, crop, 25));
    assert(gud_receiver_bulk(&receiver, marks, 4) && updates == 3);
}

static void modeset_during_transfer(void)
{
    uint8_t modes[48], state[26], rect[25] = {0};
    assert(gud_receiver_get(&receiver, 0x55, 0, modes, sizeof(modes)) == 48);
    memcpy(state, modes + 24, 24);
    state[24] = 1;
    state[25] = 0;
    assert(gud_receiver_set(&receiver, 0x60, 0, crop, sizeof(crop)));
    assert(gud_receiver_bulk(&receiver, marks, 1));
    assert(gud_receiver_set(&receiver, 0x61, 0, state, sizeof(state)));
    assert(receiver.width == 640 && receiver.used == 1);
    assert(gud_receiver_set(&receiver, 0x62, 0, NULL, 0));
    assert(receiver.width == 800 && receiver.height == 600 && !receiver.expected);
    assert(!gud_receiver_bulk(&receiver, marks + 1, 3));
    // A 60,000-byte SVGA frame exceeds the negotiated 38,400-byte transfer limit.
    const uint8_t oversized[25] = {0,0,0,0, 0,0,0,0, 0x20,3,0,0, 0x58,2,0,0, 0x60,0xea};
    assert(!gud_receiver_set(&receiver, 0x60, 0, oversized, sizeof(oversized)));
    // A single bottom-right pixel is legal in SVGA and out of bounds in VGA.
    rect[0] = 0x1f; rect[1] = 3;
    rect[4] = 0x57; rect[5] = 2;
    rect[8] = rect[12] = rect[16] = 1;
    assert(gud_receiver_set(&receiver, 0x60, 0, rect, sizeof(rect)));
    memcpy(state, modes, 24);
    assert(gud_receiver_set(&receiver, 0x61, 0, state, sizeof(state)));
    assert(gud_receiver_set(&receiver, 0x62, 0, NULL, 0));
    assert(!receiver.expected && receiver.width == 640 && receiver.height == 480);
    assert(!gud_receiver_set(&receiver, 0x60, 0, rect, sizeof(rect)));
}

int main(void)
{
    assert(!gud_receiver_init(&receiver, guarded, GUD_R1_MAX_BYTES - 1, present, frame));
    uint8_t reply;
    assert(gud_receiver_get(&receiver, 1, 0, &reply, 1) < 0);
    assert(!gud_receiver_set(&receiver, 0x53, 0, NULL, 0));
    reset();
    modeset();
    rejection();
    pictures();
    modeset_during_transfer();
    puts("PASS GUD v1 probe, modeset, R1 full/crop pixels, fragmented bulk, error recovery and reattach");
    return 0;
}
