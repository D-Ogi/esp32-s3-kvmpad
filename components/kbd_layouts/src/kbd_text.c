// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Composed text item -> burst steps. US positions: USB HID Usage Tables, keyboard page 0x07.
#include "kbd_text.h"

#define S 0x80u // table flag: typed with Shift

// Printable ASCII 0x20..0x7E on a US keyboard: usage | S.
static const uint8_t US_ASCII[95] = {
    0x2C,     0x1E | S, 0x34 | S, 0x20 | S, 0x21 | S, 0x22 | S, 0x24 | S, 0x34,     // space ! " # $ % & '
    0x26 | S, 0x27 | S, 0x25 | S, 0x2E | S, 0x36,     0x2D,     0x37,     0x38,     // ( ) * + , - . /
    0x27,     0x1E,     0x1F,     0x20,     0x21,     0x22,     0x23,     0x24,     // 0..7
    0x25,     0x26,     0x33 | S, 0x33,     0x36 | S, 0x2E,     0x37 | S, 0x38 | S, // 8 9 : ; < = > ?
    0x1F | S,                                                                     // @
    0x04 | S, 0x05 | S, 0x06 | S, 0x07 | S, 0x08 | S, 0x09 | S, 0x0A | S, 0x0B | S, // A..H
    0x0C | S, 0x0D | S, 0x0E | S, 0x0F | S, 0x10 | S, 0x11 | S, 0x12 | S, 0x13 | S, // I..P
    0x14 | S, 0x15 | S, 0x16 | S, 0x17 | S, 0x18 | S, 0x19 | S, 0x1A | S, 0x1B | S, // Q..X
    0x1C | S, 0x1D | S,                                                           // Y Z
    0x2F,     0x31,     0x30,     0x23 | S, 0x2D | S, 0x35,                       // [ \ ] ^ _ `
    0x04,     0x05,     0x06,     0x07,     0x08,     0x09,     0x0A,     0x0B,     // a..h
    0x0C,     0x0D,     0x0E,     0x0F,     0x10,     0x11,     0x12,     0x13,     // i..p
    0x14,     0x15,     0x16,     0x17,     0x18,     0x19,     0x1A,     0x1B,     // q..x
    0x1C,     0x1D,                                                               // y z
    0x2F | S, 0x31 | S, 0x30 | S, 0x35 | S,                                       // { | } ~
};
_Static_assert(sizeof(US_ASCII) == 0x7F - 0x20, "kbd_text: printable ASCII table");

#define U_LSHIFT 0xE1u

bool kvm_text_ascii_us(uint32_t c, kvm_keystroke_t *out)
{
    if (c < 0x20 || c > 0x7E) {
        return false;
    }
    const uint8_t e = US_ASCII[c - 0x20];
    out->usage = (uint8_t)(e & 0x7Fu);
    out->mods = (e & S) ? 0x02u : 0u; // Left Shift
    return true;
}

static int key_steps(uint8_t usage, bool shift, kvm_uni_step_t *out, size_t cap)
{
    const size_t need = shift ? 4u : 2u;
    if (cap < need || out == NULL) {
        return KVM_UNI_ERR_SPACE;
    }
    size_t n = 0;
    if (shift) {
        out[n++] = (kvm_uni_step_t){U_LSHIFT, true};
    }
    out[n++] = (kvm_uni_step_t){usage, true};
    out[n++] = (kvm_uni_step_t){usage, false};
    if (shift) {
        out[n++] = (kvm_uni_step_t){U_LSHIFT, false};
    }
    return (int)n;
}

int kvm_text_item_steps(const kvm_text_item_t *item, kvm_uni_mode_t mode, kvm_uni_step_t *out, size_t cap)
{
    if (item == NULL) {
        return KVM_UNI_ERR_KEY;
    }
    if (item->kind == KVM_TEXT_KEY) {
        const uint32_t usage = item->value & ~KVM_TEXT_KEY_SHIFT;
        if ((item->value & ~(KVM_TEXT_KEY_SHIFT | 0xFFu)) != 0 || usage < 0x04 || usage > 0xDD) {
            return KVM_UNI_ERR_KEY; // never a modifier, never an error code
        }
        return key_steps((uint8_t)usage, (item->value & KVM_TEXT_KEY_SHIFT) != 0, out, cap);
    }
    if (item->kind != KVM_TEXT_CHAR) {
        return KVM_UNI_ERR_KEY;
    }
    kvm_keystroke_t k;
    if (kvm_text_ascii_us(item->value, &k)) {
        return key_steps(k.usage, k.mods != 0, out, cap); // the layout types it: plain keys in every mode
    }
    if (mode == KVM_UNI_HOST_IME) {
        return KVM_UNI_ERR_MODE; // no sequence exists: counted as skipped, never sent half-way
    }
    return kvm_uni_build(mode, item->value, NULL, NULL, out, cap);
}
