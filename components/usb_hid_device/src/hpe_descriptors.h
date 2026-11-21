// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)

// Static HID short items for the two advanced interfaces. Global PUSH/POP isolates
// physical units and ranges so a dial or contact axis cannot affect a later collection.
#pragma once
#include "hpe_reports.h"

#define PAGE(p)       0x05, (p)
#define USAGE(u)      0x09, (u)
#define APP           0xA1, 0x01
#define LOGICAL       0xA1, 0x02
#define END           0xC0
#define ID(i)         0x85, (i)
#define BITS(s,n)     0x75, (s), 0x95, (n)
#define LIMIT8(l,h)   0x15, (l), 0x25, (h)
// HidP sign-extends a one-byte Logical Maximum of 0xff to -1.
#define BYTE_RANGE    0x15,0, 0x26,0xff,0
#define INPUT(f)      0x81, (f)
#define FEATURE(f)    0xB1, (f)
#define PUSH          0xA4
#define POP           0xB4
#define LE16(v)       ((v) & 255), (((v) >> 8) & 255)
#define FINGER \
    PAGE(0x0d), USAGE(0x22), LOGICAL, PUSH, \
    USAGE(0x42), USAGE(0x47), LIMIT8(0,1), BITS(1,2), INPUT(2), BITS(6,1), INPUT(3), \
    USAGE(0x51), BYTE_RANGE, BITS(8,1), INPUT(2), \
    PAGE(1), USAGE(0x30), 0x26,0xff,0x7f, 0x35,0, 0x46,LE16(CONFIG_KVM_PTP_WIDTH_TENTH_MM), \
    0x65,0x11, 0x55,0x0e, BITS(16,1), INPUT(2), \
    USAGE(0x31), 0x46,LE16(CONFIG_KVM_PTP_HEIGHT_TENTH_MM), INPUT(2), POP, END

static const uint8_t s_report_ptp[] = {
    PAGE(0x0d), USAGE(5), APP, ID(HPE_PTP),
    FINGER, FINGER, FINGER, FINGER, FINGER,
    USAGE(0x56), PUSH, 0x15,0, 0x27,0xff,0xff,0,0, 0x55,0x0c, 0x66,0x01,0x10,
    BITS(16,1), INPUT(2), POP,
    USAGE(0x54), LIMIT8(0,5), BITS(8,1), INPUT(2),
    ID(HPE_CAPS), USAGE(0x55), BITS(4,1), FEATURE(2),
    USAGE(0x59), LIMIT8(0,2), FEATURE(2),
    ID(HPE_LATENCY), USAGE(0x60), LIMIT8(0,1), BITS(8,1), FEATURE(2),
    0x06,0x00,0xff, USAGE(0xc5), ID(HPE_CERT), BYTE_RANGE, 0x75,8,0x96,0,1, FEATURE(2), END,
    PAGE(0x0d), USAGE(0x0e), APP, ID(HPE_INPUT_MODE), USAGE(0x52), LIMIT8(0,3), BITS(8,1), FEATURE(2),
    ID(HPE_SELECT), USAGE(0x57), USAGE(0x58), LIMIT8(0,1), BITS(1,2), FEATURE(2),
    BITS(6,1), FEATURE(3), END
};

static const uint8_t s_report_control[] = {
    PAGE(1), USAGE(0x0e), APP, PUSH, ID(HPE_RADIAL), PAGE(0x0d), USAGE(0x21), 0xa1,0,
    PAGE(9), USAGE(1), LIMIT8(0,1), BITS(1,1), INPUT(2),
    PAGE(1), USAGE(0x37), 0x55,0x0f, 0x65,0x14,
    0x16,0xf0,0xf1, 0x26,0x10,0x0e, 0x36,0xf0,0xf1, 0x46,0x10,0x0e,
    BITS(15,1), INPUT(6), END, POP, END,

    PAGE(0x0c), USAGE(1), APP, ID(HPE_CONSUMER), 0x15,0,0x26,0xff,3,
    0x19,0,0x2a,0xff,3, BITS(16,1), INPUT(0), END,

    0x06,0x61,0xff, USAGE(1), APP, BYTE_RANGE,
    USAGE(2), ID(HPE_STUDIO_IN), BITS(8,63), INPUT(2),
    USAGE(3), ID(HPE_STUDIO_OUT), BITS(8,63), 0x91,2,
    USAGE(4), ID(HPE_STUDIO_FEATURE), BITS(8,15), FEATURE(2), END,

    PAGE(1), USAGE(4), APP, ID(HPE_JOYSTICK), USAGE(0x30), USAGE(0x31), USAGE(0x35),
    0x16,0x01,0x80, 0x26,0xff,0x7f, BITS(16,3), INPUT(2),
    USAGE(0x32), 0x15,0, 0x27,0xff,0xff,0,0, BITS(16,1), INPUT(2),
    PAGE(9), 0x19,1,0x29,16, LIMIT8(0,1), BITS(1,16), INPUT(2),
    PAGE(1), USAGE(0x39), LIMIT8(0,7), 0x35,0,0x46,0x3b,1,0x65,0x14, BITS(4,1), INPUT(0x42),
    BITS(4,1), INPUT(3), END
};
