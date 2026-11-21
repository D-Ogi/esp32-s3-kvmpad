// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "monitor_wire.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Mirrors udisp_frame_header_t in Espressif's app_vendor.c, tested on a
// little-endian target. This independent producer never calls the parser.
typedef struct __attribute__((packed)) {
    uint16_t crc;
    uint8_t type, cmd;
    uint16_t x, y, width, height;
    uint32_t id : 10;
    uint32_t length : 22;
} vendor_header_t;
_Static_assert(sizeof(vendor_header_t) == 16, "vendor header");
static unsigned offered, accepted, last;
static bool full;
static uint8_t staging[MONITOR_JPEG_LIMIT], body[MONITOR_JPEG_LIMIT + 1], wire[sizeof(body)+48];
static uint8_t *reserve(void) { ++offered; return full ? NULL : staging; }
static void complete(size_t n) { assert(!memcmp(staging, body, n)); ++accepted; last = (unsigned)n; }
static void feed(monitor_wire_t *s, const void *p, size_t n) { monitor_wire_feed(s,p,n,reserve,complete); }
static size_t frame(uint8_t *p, vendor_header_t h)
{
    memcpy(p, &h, sizeof(h));
    memcpy(p + sizeof(h), body, h.length);
    return sizeof(h) + h.length;
}
int main(void)
{
    for (size_t i=0; i<sizeof(body); ++i) body[i]=(uint8_t)(i*13);
    body[0]=0xff;body[1]=0xd8;body[46]=0xff;body[47]=0xd9;
    vendor_header_t h={.crc=0x0156,.type=3,.cmd=0x91,.width=640,.height=480,.id=1023,.length=48};
    size_t n=frame(wire,h);
    vendor_header_t end={.type=0xff};
    memcpy(wire+n,&end,sizeof(end));
    h.id=0;
    size_t both=n+sizeof(end)+frame(wire+n+sizeof(end),h);
    for (size_t split=0; split<=both; ++split) {
        monitor_wire_t rx={0};offered=accepted=0;
        feed(&rx,wire,split);feed(&rx,wire+split,both-split);
        assert(offered==2&&accepted==2&&last==48&&!rx.remaining&&!rx.used);
        assert(rx.bytes == both && rx.headers == 3 && rx.rejected == 0);
    }
    monitor_wire_t rx={0}; offered=accepted=0;
    h.length=MONITOR_JPEG_LIMIT; n=frame(wire,h);
    for(size_t i=0;i<n;++i) feed(&rx,wire+i,1);
    assert(accepted==1&&last==MONITOR_JPEG_LIMIT);
    for(unsigned which=0;which<7;++which) {
        vendor_header_t bad=h;
        switch(which) {
        case 0:bad.type=0;break; case 1:bad.width=639;break; case 2:bad.height=600;break;
        case 3:bad.x=1;break;case 4:bad.y=2;break;case 5:bad.length++;break;default:bad.length=0;break;
        }
        n=frame(wire,bad);feed(&rx,wire,n);
        assert(offered==1&&accepted==1&&!rx.remaining);
        assert(rx.rejected == (which == 6 ? 6 : which + 1));
    }
    h.length=48;n=frame(wire,h);
    full=true;feed(&rx,wire,24);full=false;feed(&rx,wire+24,n-24);assert(accepted==1);
    assert(rx.rejected == 6);
    feed(&rx,wire,n);assert(accepted==2&&last==48);
    for(size_t cut=1;cut<n;cut++) {
        feed(&rx,wire,cut);rx=(monitor_wire_t){0};feed(&rx,wire,n);
    }
    assert(accepted==2+n-1);
    offered = accepted = 0;
    full = true;
    rx = (monitor_wire_t){0};
    feed(&rx, wire, 7);
    assert(rx.bytes == 7 && rx.used == 7 && !rx.headers);
    feed(&rx, wire + 7, sizeof(h) - 7);
    assert(rx.headers == 1 && !rx.used && rx.remaining == h.length);
    assert(!offered && !accepted);
    full = false;
    feed(&rx, wire + sizeof(h), n - sizeof(h));
    assert(offered == 1 && accepted == 1 && last == h.length);
    puts("PASS vendor bitfields, all split positions, END, rollover, concatenation, one-byte chunks, size limit, skip/recovery, atomic busy drop, reset at every prefix, slot available after header");
}
