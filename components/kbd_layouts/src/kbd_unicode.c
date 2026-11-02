// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Unicode delivery sequences (ADR-024 FR-747). Usages: USB HID Usage Tables, keyboard page 0x07.
#include "kbd_unicode.h"

#define U_A 0x04u
#define U_U 0x18u
#define U_1 0x1Eu
#define U_0 0x27u
#define U_ENTER 0x28u
#define U_ESC 0x29u
#define U_SPACE 0x2Cu
#define U_KP_PLUS 0x57u
#define U_KP_1 0x59u
#define U_KP_0 0x62u
#define U_LCTRL 0xE0u
#define U_LSHIFT 0xE1u
#define U_LALT 0xE2u
#define U_RALT 0xE6u

#define MOD_BIT(usage) ((uint8_t)(1u << ((usage) - 0xE0u)))

typedef struct {
    kvm_uni_step_t *out;
    size_t cap;
    size_t n;
    bool overflow;
} seq_t;

static void put(seq_t *s, uint8_t usage, bool down)
{
    if (s->n >= s->cap) {
        s->overflow = true;
        return;
    }
    s->out[s->n].usage = usage;
    s->out[s->n].down = down;
    s->n++;
}

static void tap(seq_t *s, uint8_t usage)
{
    put(s, usage, true);
    put(s, usage, false);
}

/** Type one keystroke while the modifiers in `held` are already down. */
static void stroke(seq_t *s, kvm_keystroke_t k, uint8_t held)
{
    uint8_t extra = (uint8_t)(k.mods & (uint8_t)~held);
    for (uint8_t b = 0; b < 8; b++) {
        if (extra & (1u << b)) {
            put(s, (uint8_t)(0xE0u + b), true);
        }
    }
    tap(s, k.usage);
    for (int b = 7; b >= 0; b--) {
        if (extra & (1u << b)) {
            put(s, (uint8_t)(0xE0u + (unsigned)b), false);
        }
    }
}

bool kvm_uni_key_us(void *ctx, char c, kvm_keystroke_t *out)
{
    (void)ctx;
    out->mods = 0;
    if (c == '0') {
        out->usage = U_0;
    } else if (c >= '1' && c <= '9') {
        out->usage = (uint8_t)(U_1 + (uint8_t)(c - '1'));
    } else if (c >= 'a' && c <= 'z') {
        out->usage = (uint8_t)(U_A + (uint8_t)(c - 'a'));
    } else {
        return false;
    }
    return true;
}

static char hex_char(uint32_t nibble)
{
    return (char)(nibble < 10 ? '0' + nibble : 'a' + (nibble - 10));
}

/** Hex digits of `v`, at least `min_digits`, most significant first. Returns the count. */
static int hex_digits(uint32_t v, int min_digits, char digits[8])
{
    int n = 0;
    char rev[8];
    do {
        rev[n++] = hex_char(v & 0xFu);
        v >>= 4;
    } while (v != 0 && n < 8);
    while (n < min_digits) {
        rev[n++] = '0';
    }
    for (int i = 0; i < n; i++) {
        digits[i] = rev[n - 1 - i];
    }
    return n;
}

/** Hex digits through the resolver; returns false if a digit cannot be typed. */
static bool type_hex(seq_t *s, uint32_t v, int min_digits, kvm_uni_key_fn key, void *ctx, uint8_t held)
{
    char d[8];
    int n = hex_digits(v, min_digits, d);
    for (int i = 0; i < n; i++) {
        kvm_keystroke_t k;
        if (!key(ctx, d[i], &k)) {
            return false;
        }
        stroke(s, k, held);
    }
    return true;
}

static int utf16_units(uint32_t cp, uint16_t units[2])
{
    if (cp < 0x10000u) {
        units[0] = (uint16_t)cp;
        return 1;
    }
    uint32_t v = cp - 0x10000u;
    units[0] = (uint16_t)(0xD800u + (v >> 10));
    units[1] = (uint16_t)(0xDC00u + (v & 0x3FFu));
    return 2;
}

static bool type_windows_hex_unit(seq_t *s, uint16_t unit, kvm_uni_key_fn key, void *ctx)
{
    const uint8_t held = MOD_BIT(U_LALT);
    put(s, U_LALT, true);
    tap(s, U_KP_PLUS);
    char d[8];
    int n = hex_digits(unit, 4, d);
    for (int i = 0; i < n; i++) {
        if (d[i] >= '0' && d[i] <= '9') { // keypad digits do not depend on the HOST layout
            tap(s, d[i] == '0' ? (uint8_t)U_KP_0 : (uint8_t)(U_KP_1 + (uint8_t)(d[i] - '1')));
            continue;
        }
        kvm_keystroke_t k;
        if (!key(ctx, d[i], &k)) {
            return false;
        }
        stroke(s, k, held);
    }
    put(s, U_LALT, false); // releasing Alt commits the character
    return true;
}

int kvm_uni_build(kvm_uni_mode_t mode, uint32_t cp, kvm_uni_key_fn key, void *ctx, kvm_uni_step_t *out,
                  size_t cap)
{
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return KVM_UNI_ERR_CODEPOINT;
    }
    if (key == NULL) {
        key = kvm_uni_key_us;
    }
    seq_t s = {.out = out, .cap = cap, .n = 0, .overflow = false};
    uint16_t units[2];
    int nu = utf16_units(cp, units);
    kvm_keystroke_t k;

    switch (mode) {
    case KVM_UNI_LINUX: {
        const uint8_t held = (uint8_t)(MOD_BIT(U_LCTRL) | MOD_BIT(U_LSHIFT));
        if (!key(ctx, 'u', &k)) {
            return KVM_UNI_ERR_KEY;
        }
        put(&s, U_LCTRL, true);
        put(&s, U_LSHIFT, true);
        stroke(&s, k, held);
        put(&s, U_LSHIFT, false);
        put(&s, U_LCTRL, false);
        if (!type_hex(&s, cp, 4, key, ctx, 0)) {
            return KVM_UNI_ERR_KEY;
        }
        tap(&s, U_SPACE);
        break;
    }
    case KVM_UNI_MACOS: {
        const uint8_t held = MOD_BIT(U_LALT);
        put(&s, U_LALT, true);
        for (int i = 0; i < nu; i++) {
            (void)type_hex(&s, units[i], 4, kvm_uni_key_us, NULL, held); // US positions always resolve
        }
        put(&s, U_LALT, false);
        break;
    }
    case KVM_UNI_WINDOWS_HEX:
        for (int i = 0; i < nu; i++) {
            if (!type_windows_hex_unit(&s, units[i], key, ctx)) {
                return KVM_UNI_ERR_KEY;
            }
        }
        break;
    case KVM_UNI_WINCOMPOSE:
        if (!key(ctx, 'u', &k)) {
            return KVM_UNI_ERR_KEY;
        }
        tap(&s, U_RALT);
        stroke(&s, k, 0);
        if (!type_hex(&s, cp, 4, key, ctx, 0)) {
            return KVM_UNI_ERR_KEY;
        }
        tap(&s, U_ENTER);
        break;
    default:
        return KVM_UNI_ERR_MODE;
    }
    return s.overflow ? KVM_UNI_ERR_SPACE : (int)s.n;
}

int kvm_uni_cancel(kvm_uni_mode_t mode, kvm_uni_step_t *out, size_t cap)
{
    seq_t s = {.out = out, .cap = cap, .n = 0, .overflow = false};
    switch (mode) {
    case KVM_UNI_LINUX:
    case KVM_UNI_WINCOMPOSE:
        tap(&s, U_ESC);
        break;
    case KVM_UNI_MACOS:
    case KVM_UNI_WINDOWS_HEX:
    case KVM_UNI_HOST_IME:
        // No cancel key: RELEASE_ALL lets go of the held modifier. Windows then commits the digits typed
        // so far (a wrong character at worst, never a stuck key); macOS drops an incomplete group.
        break;
    default:
        return KVM_UNI_ERR_MODE;
    }
    return s.overflow ? KVM_UNI_ERR_SPACE : (int)s.n;
}
