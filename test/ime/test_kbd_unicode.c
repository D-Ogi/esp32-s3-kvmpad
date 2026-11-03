// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// FR-747 Unicode delivery sequences: exact steps per mode, surrogate pairs, refusals, resolver
// modifiers, cancel keys, and a balance property (every DOWN released, nothing held at the end).
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kbd_unicode.h"
#include "kvm_test.h"

#define D(u) {(u), true}
#define UP(u) {(u), false}
#define TAP(u) D(u), UP(u)

static bool same(const kvm_uni_step_t *got, int n, const kvm_uni_step_t *want, int wn)
{
    if (n != wn) {
        fprintf(stderr, "  step count %d, expected %d\n", n, wn);
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (got[i].usage != want[i].usage || got[i].down != want[i].down) {
            fprintf(stderr, "  step %d: 0x%02X %s, expected 0x%02X %s\n", i, got[i].usage, got[i].down ? "down" : "up",
                    want[i].usage, want[i].down ? "down" : "up");
            return false;
        }
    }
    return true;
}

#define EXPECT_SEQ(mode, cp, key, ...)                                                 \
    do {                                                                               \
        static const kvm_uni_step_t want_[] = {__VA_ARGS__};                           \
        kvm_uni_step_t got_[KVM_UNI_MAX_STEPS];                                        \
        int n_ = kvm_uni_build((mode), (cp), (key), NULL, got_, KVM_UNI_MAX_STEPS);    \
        CHECK(same(got_, n_, want_, (int)(sizeof(want_) / sizeof(want_[0]))));         \
    } while (0)

static void test_linux(void)
{
    // é = U+00E9: Ctrl+Shift+U, "00e9", Space
    EXPECT_SEQ(KVM_UNI_LINUX, 0xE9, NULL, D(0xE0), D(0xE1), TAP(0x18), UP(0xE1), UP(0xE0), TAP(0x27), TAP(0x27),
               TAP(0x08), TAP(0x26), TAP(0x2C));
    // 😀 = U+1F600: the whole code point, no surrogates
    EXPECT_SEQ(KVM_UNI_LINUX, 0x1F600, NULL, D(0xE0), D(0xE1), TAP(0x18), UP(0xE1), UP(0xE0), TAP(0x1E), TAP(0x09),
               TAP(0x23), TAP(0x27), TAP(0x27), TAP(0x2C));
}

static void test_macos(void)
{
    // 한 = U+D55C: Option held, "d55c"
    EXPECT_SEQ(KVM_UNI_MACOS, 0xD55C, NULL, D(0xE2), TAP(0x07), TAP(0x22), TAP(0x22), TAP(0x06), UP(0xE2));
    // 😀 = U+1F600 -> UTF-16 D83D DE00, both groups under one Option press
    EXPECT_SEQ(KVM_UNI_MACOS, 0x1F600, NULL, D(0xE2), TAP(0x07), TAP(0x25), TAP(0x20), TAP(0x07), TAP(0x07),
               TAP(0x08), TAP(0x27), TAP(0x27), UP(0xE2));
}

static void test_windows_hex(void)
{
    // é: Alt held, Numpad +, keypad 0 0, letter e, keypad 9, Alt released
    EXPECT_SEQ(KVM_UNI_WINDOWS_HEX, 0xE9, NULL, D(0xE2), TAP(0x57), TAP(0x62), TAP(0x62), TAP(0x08), TAP(0x61),
               UP(0xE2));
    // 😀: one Alt sequence per UTF-16 unit
    EXPECT_SEQ(KVM_UNI_WINDOWS_HEX, 0x1F600, NULL, D(0xE2), TAP(0x57), TAP(0x07), TAP(0x60), TAP(0x5B), TAP(0x07),
               UP(0xE2), D(0xE2), TAP(0x57), TAP(0x07), TAP(0x08), TAP(0x62), TAP(0x62), UP(0xE2));
}

static void test_wincompose(void)
{
    // é: Right Alt, u, "00e9", Enter
    EXPECT_SEQ(KVM_UNI_WINCOMPOSE, 0xE9, NULL, TAP(0xE6), TAP(0x18), TAP(0x27), TAP(0x27), TAP(0x08), TAP(0x26),
               TAP(0x28));
}

static void test_refusals(void)
{
    kvm_uni_step_t s[KVM_UNI_MAX_STEPS];
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_CODEPOINT);
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xD800, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_CODEPOINT);
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xDFFF, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_CODEPOINT);
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0x110000, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_CODEPOINT);
    CHECK_EQ(kvm_uni_build(KVM_UNI_HOST_IME, 0xE9, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_MODE);
    CHECK_EQ(kvm_uni_build((kvm_uni_mode_t)99, 0xE9, NULL, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_MODE);
    // é in linux mode takes 16 steps: 15 is too few, 16 is enough
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xE9, NULL, NULL, s, 15), KVM_UNI_ERR_SPACE);
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xE9, NULL, NULL, s, 16), 16);
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xE9, NULL, NULL, NULL, 0), KVM_UNI_ERR_SPACE);
}

// A HOST layout where digits need Shift (as on AZERTY) and 'u' cannot be typed.
static bool key_shifted_digits(void *ctx, char c, kvm_keystroke_t *out)
{
    (void)ctx;
    if (!kvm_uni_key_us(NULL, c, out)) {
        return false;
    }
    if (c >= '0' && c <= '9') {
        out->mods = 0x02; // Left Shift
    }
    return true;
}

static bool key_no_u(void *ctx, char c, kvm_keystroke_t *out)
{
    return c != 'u' && kvm_uni_key_us(ctx, c, out);
}

static bool key_worst(void *ctx, char c, kvm_keystroke_t *out)
{
    (void)ctx;
    if (!kvm_uni_key_us(NULL, c, out)) {
        return false;
    }
    out->mods = 0x48; // Left GUI + Right Alt: two modifiers per character, none of them ever held
    return true;
}

static void test_resolver(void)
{
    // Digits wrapped in Shift; the letter e is not.
    EXPECT_SEQ(KVM_UNI_LINUX, 0xE9, key_shifted_digits, D(0xE0), D(0xE1), TAP(0x18), UP(0xE1), UP(0xE0), D(0xE1),
               TAP(0x27), UP(0xE1), D(0xE1), TAP(0x27), UP(0xE1), TAP(0x08), D(0xE1), TAP(0x26), UP(0xE1), TAP(0x2C));
    // Windows hex uses keypad digits, so the HOST layout's Shift is not needed there.
    EXPECT_SEQ(KVM_UNI_WINDOWS_HEX, 0xE9, key_shifted_digits, D(0xE2), TAP(0x57), TAP(0x62), TAP(0x62), TAP(0x08),
               TAP(0x61), UP(0xE2));
    // macOS ignores the resolver: Unicode Hex Input is US-positioned.
    EXPECT_SEQ(KVM_UNI_MACOS, 0xE9, key_shifted_digits, D(0xE2), TAP(0x27), TAP(0x27), TAP(0x08), TAP(0x26),
               UP(0xE2));
    kvm_uni_step_t s[KVM_UNI_MAX_STEPS];
    CHECK_EQ(kvm_uni_build(KVM_UNI_LINUX, 0xE9, key_no_u, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    CHECK_EQ(kvm_uni_build(KVM_UNI_WINCOMPOSE, 0xE9, key_no_u, NULL, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
}

static void test_cancel(void)
{
    kvm_uni_step_t s[4];
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_LINUX, s, 4), 2);
    CHECK(s[0].usage == 0x29 && s[0].down && s[1].usage == 0x29 && !s[1].down);
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_WINCOMPOSE, s, 4), 2);
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_MACOS, s, 4), 0);
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_WINDOWS_HEX, s, 4), 0);
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_HOST_IME, s, 4), 0);
    CHECK_EQ(kvm_uni_cancel(KVM_UNI_LINUX, s, 1), KVM_UNI_ERR_SPACE);
    CHECK_EQ(kvm_uni_cancel((kvm_uni_mode_t)99, s, 4), KVM_UNI_ERR_MODE);
}

/** Every DOWN is of a key not held, every UP of a key held, and nothing is held at the end. */
static bool balanced(const kvm_uni_step_t *s, int n)
{
    bool held[256];
    memset(held, 0, sizeof(held));
    for (int i = 0; i < n; i++) {
        if (s[i].down == held[s[i].usage]) {
            return false;
        }
        held[s[i].usage] = s[i].down;
    }
    for (int u = 0; u < 256; u++) {
        if (held[u]) {
            return false;
        }
    }
    return true;
}

static void test_balance_property(void)
{
    static const kvm_uni_mode_t MODES[] = {KVM_UNI_LINUX, KVM_UNI_MACOS, KVM_UNI_WINDOWS_HEX, KVM_UNI_WINCOMPOSE};
    static const kvm_uni_key_fn KEYS[] = {NULL, key_shifted_digits, key_worst};
    int bad = 0, built = 0, max_steps = 0;
    for (size_t m = 0; m < sizeof(MODES) / sizeof(MODES[0]); m++) {
        for (size_t k = 0; k < sizeof(KEYS) / sizeof(KEYS[0]); k++) {
            for (uint32_t cp = 1; cp <= 0x10FFFF; cp += (cp < 0x20000 ? 0x3Fu : 0xFFFu)) {
                if (cp >= 0xD800 && cp <= 0xDFFF) {
                    continue;
                }
                uint32_t probe[] = {cp, 0x10FFFF, 0xFFFF, 0x10000};
                for (size_t p = 0; p < 4; p++) {
                    kvm_uni_step_t s[KVM_UNI_MAX_STEPS];
                    int n = kvm_uni_build(MODES[m], probe[p], KEYS[k], NULL, s, KVM_UNI_MAX_STEPS);
                    if (n <= 0 || !balanced(s, n)) {
                        bad++;
                        continue;
                    }
                    built++;
                    max_steps = n > max_steps ? n : max_steps;
                }
            }
        }
    }
    printf("    %d sequences, longest %d steps (cap %d)\n", built, max_steps, KVM_UNI_MAX_STEPS);
    CHECK_EQ(bad, 0);
    CHECK(max_steps <= KVM_UNI_MAX_STEPS);
}

int main(void)
{
    printf("test_kbd_unicode\n");
    RUN(test_linux);
    RUN(test_macos);
    RUN(test_windows_hex);
    RUN(test_wincompose);
    RUN(test_refusals);
    RUN(test_resolver);
    RUN(test_cancel);
    RUN(test_balance_property);
    TEST_MAIN_END("test_kbd_unicode");
}
