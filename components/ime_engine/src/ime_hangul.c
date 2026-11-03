// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Korean 2-set composer. Tables follow the Unicode Hangul syllable algorithm (The Unicode Standard,
// section 3.12): 19 initials, 21 medials, 27 finals + none; compatibility jamo U+3131..U+3163.
#include "ime_hangul.h"

#include <string.h>

#define SBASE 0xAC00u
#define VCOUNT 21
#define TCOUNT 28
#define COMPAT_CONS_FIRST 0x3131u // first of the 30 compatibility consonants
#define COMPAT_CONS_LAST 0x314Eu
#define COMPAT_VOW_FIRST 0x314Fu  // the 21 compatibility vowels are in medial order
#define COMPAT_VOW_LAST 0x3163u

// Compatibility consonant (cp - 0x3131) -> initial index / final index.
static const int8_t CHO_OF[30] = {0, 1, -1, 2, -1, -1, 3, 4, 5, -1, -1, -1, -1, -1, -1,
                                  -1, 6, 7, 8, -1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};
static const int8_t JONG_OF[30] = {1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13, 14,
                                   15, 16, 17, 0, 18, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27};

// Compound finals: first final + compatibility consonant -> compound final.
typedef struct {
    int8_t first;
    uint8_t second; // compatibility consonant index (cp - 0x3131)
    int8_t result;
} pair_t;

static const pair_t JONG_PAIRS[] = {
    {1, 20, 3},   // ㄱ + ㅅ = ㄳ
    {4, 23, 5},   // ㄴ + ㅈ = ㄵ
    {4, 29, 6},   // ㄴ + ㅎ = ㄶ
    {8, 0, 9},    // ㄹ + ㄱ = ㄺ
    {8, 16, 10},  // ㄹ + ㅁ = ㄻ
    {8, 17, 11},  // ㄹ + ㅂ = ㄼ
    {8, 20, 12},  // ㄹ + ㅅ = ㄽ
    {8, 27, 13},  // ㄹ + ㅌ = ㄾ
    {8, 28, 14},  // ㄹ + ㅍ = ㄿ
    {8, 29, 15},  // ㄹ + ㅎ = ㅀ
    {17, 20, 18}, // ㅂ + ㅅ = ㅄ
};

// Compound medials: medial + medial -> compound medial.
static const pair_t JUNG_PAIRS[] = {
    {8, 0, 9},    // ㅗ + ㅏ = ㅘ
    {8, 1, 10},   // ㅗ + ㅐ = ㅙ
    {8, 20, 11},  // ㅗ + ㅣ = ㅚ
    {13, 4, 14},  // ㅜ + ㅓ = ㅝ
    {13, 5, 15},  // ㅜ + ㅔ = ㅞ
    {13, 20, 16}, // ㅜ + ㅣ = ㅟ
    {18, 20, 19}, // ㅡ + ㅣ = ㅢ
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static bool is_consonant(uint32_t cp)
{
    return cp >= COMPAT_CONS_FIRST && cp <= COMPAT_CONS_LAST;
}

static bool is_vowel(uint32_t cp)
{
    return cp >= COMPAT_VOW_FIRST && cp <= COMPAT_VOW_LAST;
}

bool kvm_hangul_is_jamo(uint32_t cp)
{
    return is_consonant(cp) || is_vowel(cp);
}

static uint32_t compat_of_cho(int8_t cho)
{
    for (uint32_t i = 0; i < COUNT(CHO_OF); i++) {
        if (CHO_OF[i] == cho) {
            return COMPAT_CONS_FIRST + i;
        }
    }
    return 0;
}

void kvm_hangul_reset(kvm_hangul_t *h)
{
    memset(h, 0, sizeof(*h));
    h->cho = -1;
    h->jung = -1;
}

uint32_t kvm_hangul_preedit(const kvm_hangul_t *h)
{
    if (h->cho >= 0 && h->jung >= 0) {
        return SBASE + ((uint32_t)h->cho * VCOUNT + (uint32_t)h->jung) * TCOUNT + (uint32_t)h->jong;
    }
    if (h->cho >= 0) {
        return compat_of_cho(h->cho);
    }
    if (h->jung >= 0) {
        return COMPAT_VOW_FIRST + (uint32_t)h->jung;
    }
    return 0;
}

static size_t commit(kvm_hangul_t *h, uint32_t *out)
{
    uint32_t cp = kvm_hangul_preedit(h);
    kvm_hangul_reset(h);
    if (cp == 0) {
        return 0;
    }
    out[0] = cp;
    return 1;
}

static void record(kvm_hangul_t *h, uint32_t jamo)
{
    if (h->nkeys < KVM_HANGUL_MAX_KEYS) {
        h->keys[h->nkeys++] = jamo;
    }
}

/** Start a new syllable with `jamo` after a commit (keys reset by commit). */
static size_t start_with(kvm_hangul_t *h, uint32_t jamo, uint32_t *out, size_t n)
{
    if (is_consonant(jamo)) {
        int8_t cho = CHO_OF[jamo - COMPAT_CONS_FIRST];
        if (cho < 0) { // a compound consonant cannot start a syllable: it is committed as it is
            out[n++] = jamo;
            return n;
        }
        h->cho = cho;
    } else {
        h->jung = (int8_t)(jamo - COMPAT_VOW_FIRST);
    }
    record(h, jamo);
    return n;
}

static size_t input_consonant(kvm_hangul_t *h, uint32_t jamo, uint32_t *out)
{
    uint8_t ci = (uint8_t)(jamo - COMPAT_CONS_FIRST);
    if (h->cho >= 0 && h->jung >= 0) {
        if (h->jong == 0 && JONG_OF[ci] != 0) {
            h->jong = JONG_OF[ci];
            record(h, jamo);
            return 0;
        }
        if (h->jong != 0) {
            for (size_t i = 0; i < COUNT(JONG_PAIRS); i++) {
                if (JONG_PAIRS[i].first == h->jong && JONG_PAIRS[i].second == ci) {
                    h->jong = JONG_PAIRS[i].result;
                    record(h, jamo);
                    return 0;
                }
            }
        }
    }
    size_t n = commit(h, out);
    return start_with(h, jamo, out, n);
}

static size_t input_vowel(kvm_hangul_t *h, uint32_t jamo, uint32_t *out)
{
    int8_t v = (int8_t)(jamo - COMPAT_VOW_FIRST);
    if (h->jung < 0) {
        if (h->cho >= 0) { // initial waiting for its vowel
            h->jung = v;
            record(h, jamo);
            return 0;
        }
        return start_with(h, jamo, out, 0); // empty: a lone vowel
    }
    if (h->jong == 0) {
        // The compound only forms when the previous key was the vowel itself.
        if (h->nkeys > 0 && is_vowel(h->keys[h->nkeys - 1])) {
            for (size_t i = 0; i < COUNT(JUNG_PAIRS); i++) {
                if (JUNG_PAIRS[i].first == h->jung && JUNG_PAIRS[i].second == (uint8_t)v) {
                    h->jung = JUNG_PAIRS[i].result;
                    record(h, jamo);
                    return 0;
                }
            }
        }
        size_t n = commit(h, out);
        return start_with(h, jamo, out, n);
    }
    // A final followed by a vowel moves to the next syllable (a compound final gives up its second part).
    if (h->cho < 0) { // cannot happen: a final needs an initial and a medial
        size_t n = commit(h, out);
        return start_with(h, jamo, out, n);
    }
    uint32_t moved = h->keys[h->nkeys - 1];
    int8_t keep = 0;
    for (size_t i = 0; i < COUNT(JONG_PAIRS); i++) {
        if (JONG_PAIRS[i].result == h->jong) {
            keep = JONG_PAIRS[i].first;
            moved = COMPAT_CONS_FIRST + JONG_PAIRS[i].second;
            break;
        }
    }
    h->jong = keep;
    size_t n = commit(h, out);
    n = start_with(h, moved, out, n);
    h->jung = v;
    record(h, jamo);
    return n;
}

size_t kvm_hangul_input(kvm_hangul_t *h, uint32_t jamo, uint32_t out[2])
{
    if (is_consonant(jamo)) {
        return input_consonant(h, jamo, out);
    }
    if (is_vowel(jamo)) {
        return input_vowel(h, jamo, out);
    }
    return 0;
}

bool kvm_hangul_backspace(kvm_hangul_t *h)
{
    if (h->nkeys == 0) {
        return false;
    }
    uint32_t keys[KVM_HANGUL_MAX_KEYS];
    uint8_t n = (uint8_t)(h->nkeys - 1);
    memcpy(keys, h->keys, sizeof(keys));
    kvm_hangul_reset(h);
    uint32_t sink[2];
    for (uint8_t i = 0; i < n; i++) {
        (void)kvm_hangul_input(h, keys[i], sink); // a prefix of one syllable never commits
    }
    return true;
}

size_t kvm_hangul_flush(kvm_hangul_t *h, uint32_t out[1])
{
    return commit(h, out);
}

uint32_t kvm_hangul_2set_jamo(char key, bool shift)
{
    // KS X 5002 2-set: the Latin key cap on a US QWERTY board -> compatibility jamo.
    static const uint16_t BASE[26] = {
        0x3141, 0x3160, 0x314A, 0x3147, 0x3137, 0x3139, 0x314E, 0x3157, 0x3151, 0x3153, 0x314F, 0x3163, 0x3161,
        0x315C, 0x3150, 0x3154, 0x3142, 0x3131, 0x3134, 0x3145, 0x3155, 0x314D, 0x3148, 0x314C, 0x315B, 0x314B,
    };
    if (key >= 'A' && key <= 'Z') {
        key = (char)(key - 'A' + 'a');
        shift = true;
    }
    if (key < 'a' || key > 'z') {
        return 0;
    }
    if (shift) {
        switch (key) {
        case 'q': return 0x3143; // ㅃ
        case 'w': return 0x3149; // ㅉ
        case 'e': return 0x3138; // ㄸ
        case 'r': return 0x3132; // ㄲ
        case 't': return 0x3146; // ㅆ
        case 'o': return 0x3152; // ㅒ
        case 'p': return 0x3156; // ㅖ
        default: break;
        }
    }
    return BASE[key - 'a'];
}
