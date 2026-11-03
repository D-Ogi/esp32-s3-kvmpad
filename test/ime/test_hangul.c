// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// AT-EXT-26 (Hangul part): 2-set composer vectors, backspace, all 11172 syllables round trip, and a
// seeded random walk that checks every output is a syllable or a compatibility jamo.
#include <stdint.h>
#include <string.h>
#include <uchar.h>

#include "ime_hangul.h"
#include "kvm_test.h"

#define MAX_TEXT 64

typedef struct {
    kvm_hangul_t h;
    uint32_t text[MAX_TEXT];
    size_t len;
} session_t;

static void emit(session_t *s, const uint32_t *cps, size_t n)
{
    for (size_t i = 0; i < n && s->len < MAX_TEXT; i++) {
        s->text[s->len++] = cps[i];
    }
}

static void type_latin(session_t *s, const char *keys)
{
    for (const char *k = keys; *k; k++) {
        uint32_t out[2];
        uint32_t jamo = kvm_hangul_2set_jamo(*k, false);
        CHECK(jamo != 0);
        emit(s, out, kvm_hangul_input(&s->h, jamo, out));
    }
}

static void finish(session_t *s)
{
    uint32_t out[1];
    emit(s, out, kvm_hangul_flush(&s->h, out));
}

static bool text_is(const session_t *s, const char32_t *expected)
{
    size_t n = 0;
    while (expected[n]) {
        n++;
    }
    if (n != s->len) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (s->text[i] != (uint32_t)expected[i]) {
            return false;
        }
    }
    return true;
}

static void expect(const char *keys, const char32_t *expected)
{
    session_t s;
    memset(&s, 0, sizeof(s));
    kvm_hangul_reset(&s.h);
    type_latin(&s, keys);
    finish(&s);
    if (!text_is(&s, expected)) {
        fprintf(stderr, "  keys \"%s\" gave %zu code points:", keys, s.len);
        for (size_t i = 0; i < s.len; i++) {
            fprintf(stderr, " U+%04X", (unsigned)s.text[i]);
        }
        fprintf(stderr, "\n");
    }
    CHECK(text_is(&s, expected));
    CHECK_EQ(kvm_hangul_preedit(&s.h), 0);
}

static void test_words(void)
{
    expect("dkssudgktpdy", U"안녕하세요");
    expect("gksrmf", U"한글");
    expect("rkskekfk", U"가나다라");
    expect("ekfrdl", U"닭이");   // ㄺ followed by a consonant stays
    expect("ekfrk", U"달가");    // ㄺ followed by a vowel gives up ㄱ
    expect("dkswk", U"안자");    // ㄵ followed by a vowel splits
    expect("qkqtdmf", U"밦을");  // ㅄ + ㅇ stays, then 을
    expect("dhk", U"와");
    expect("rhkd", U"광");
    expect("dml", U"의");
    expect("dho", U"왜");
    expect("qnpfr", U"뷁");      // ㅞ + ㄺ
    expect("Rkt", U"깟");        // Shift: ㄲ
    expect("Tkd", U"쌍");
    expect("dkTdj", U"았어");    // ㅆ as a final stays before a consonant
    expect("dkTj", U"아써");     // and moves whole before a vowel
}

static void test_lone_jamo(void)
{
    expect("rr", U"ㄱㄱ");
    expect("kk", U"ㅏㅏ");
    expect("hk", U"ㅘ");          // a lone compound vowel
}

static void test_backspace(void)
{
    kvm_hangul_t h;
    kvm_hangul_reset(&h);
    uint32_t out[2];
    const char *keys = "ekfr"; // 닭
    for (const char *k = keys; *k; k++) {
        CHECK_EQ(kvm_hangul_input(&h, kvm_hangul_2set_jamo(*k, false), out), 0);
    }
    CHECK_EQ(kvm_hangul_preedit(&h), U'닭');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), U'달');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), U'다');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), U'ㄷ');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), 0);
    CHECK(!kvm_hangul_backspace(&h));

    // A compound vowel loses its second part; after a moved final the new syllable keeps both keys.
    kvm_hangul_reset(&h);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('r', false), out);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('h', false), out);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('k', false), out);
    CHECK_EQ(kvm_hangul_preedit(&h), U'과');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), U'고');

    kvm_hangul_reset(&h);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('g', false), out);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('k', false), out);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('r', false), out);
    CHECK_EQ(kvm_hangul_input(&h, kvm_hangul_2set_jamo('k', false), out), 1);
    CHECK_EQ(out[0], U'하');
    CHECK_EQ(kvm_hangul_preedit(&h), U'가');
    CHECK(kvm_hangul_backspace(&h));
    CHECK_EQ(kvm_hangul_preedit(&h), U'ㄱ');
}

static void test_refuses_non_jamo(void)
{
    kvm_hangul_t h;
    kvm_hangul_reset(&h);
    uint32_t out[2];
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('r', false), out);
    (void)kvm_hangul_input(&h, kvm_hangul_2set_jamo('k', false), out);
    CHECK_EQ(kvm_hangul_input(&h, 'A', out), 0);
    CHECK_EQ(kvm_hangul_input(&h, 0xAC00, out), 0); // a precomposed syllable is not a key
    CHECK_EQ(kvm_hangul_preedit(&h), U'가');
    CHECK(!kvm_hangul_is_jamo(0x3130));
    CHECK(!kvm_hangul_is_jamo(0x3164));
    CHECK(kvm_hangul_is_jamo(0x3131));
    CHECK(kvm_hangul_is_jamo(0x3163));
    CHECK_EQ(kvm_hangul_2set_jamo(' ', false), 0);
    CHECK_EQ(kvm_hangul_2set_jamo('1', true), 0);
    CHECK_EQ(kvm_hangul_2set_jamo('a', true), 0x3141); // keys without a Shift jamo keep their own
}

// Independent decomposition tables (written from the Unicode Hangul tables, not shared with the
// implementation) for the round trip of every precomposed syllable.
static const uint32_t CHO_JAMO[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                      0x3146, 0x3147, 0x3148, 0x3149, 0x314A, 0x314B, 0x314C, 0x314D, 0x314E};
static const uint32_t JONG_JAMO[28][2] = {
    {0, 0},           {0x3131, 0},      {0x3132, 0},      {0x3131, 0x3145}, {0x3134, 0},      {0x3134, 0x3148},
    {0x3134, 0x314E}, {0x3137, 0},      {0x3139, 0},      {0x3139, 0x3131}, {0x3139, 0x3141}, {0x3139, 0x3142},
    {0x3139, 0x3145}, {0x3139, 0x314C}, {0x3139, 0x314D}, {0x3139, 0x314E}, {0x3141, 0},      {0x3142, 0},
    {0x3142, 0x3145}, {0x3145, 0},      {0x3146, 0},      {0x3147, 0},      {0x3148, 0},      {0x314A, 0},
    {0x314B, 0},      {0x314C, 0},      {0x314D, 0},      {0x314E, 0},
};
static uint32_t jung_split(int v, uint32_t *second)
{
    static const int8_t A[21] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 8, 8, 8, -1, -1, 13, 13, 13, -1, -1, 18, -1};
    static const int8_t B[21] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 20, -1, -1, 4, 5, 20, -1, -1, 20, -1};
    if (A[v] < 0) {
        *second = 0;
        return 0x314Fu + (uint32_t)v;
    }
    *second = 0x314Fu + (uint32_t)B[v];
    return 0x314Fu + (uint32_t)A[v];
}

static void test_every_syllable(void)
{
    int bad = 0;
    for (uint32_t s = 0xAC00; s <= 0xD7A3; s++) {
        uint32_t idx = s - 0xAC00;
        int l = (int)(idx / (21 * 28)), v = (int)((idx / 28) % 21), t = (int)(idx % 28);
        uint32_t seq[5];
        size_t n = 0, committed = 0;
        uint32_t second;
        seq[n++] = CHO_JAMO[l];
        seq[n++] = jung_split(v, &second);
        if (second) {
            seq[n++] = second;
        }
        if (JONG_JAMO[t][0]) {
            seq[n++] = JONG_JAMO[t][0];
        }
        if (JONG_JAMO[t][1]) {
            seq[n++] = JONG_JAMO[t][1];
        }
        kvm_hangul_t h;
        kvm_hangul_reset(&h);
        uint32_t out[2];
        for (size_t i = 0; i < n; i++) {
            committed += kvm_hangul_input(&h, seq[i], out);
        }
        if (committed != 0 || kvm_hangul_preedit(&h) != s) {
            bad++;
            continue;
        }
        // Backspacing a whole syllable always ends empty after exactly n steps.
        size_t steps = 0;
        while (kvm_hangul_backspace(&h)) {
            steps++;
        }
        if (steps != n || kvm_hangul_preedit(&h) != 0) {
            bad++;
        }
    }
    CHECK_EQ(bad, 0);
}

static bool valid_output(uint32_t cp)
{
    return (cp >= 0xAC00 && cp <= 0xD7A3) || kvm_hangul_is_jamo(cp);
}

static void test_random_walk(void)
{
    uint32_t seed = 0x6B766D70u; // fixed seed: deterministic
    kvm_hangul_t h;
    kvm_hangul_reset(&h);
    int bad = 0;
    for (int i = 0; i < 10000; i++) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t r = seed >> 8;
        uint32_t out[2];
        if (r % 10 == 0) {
            (void)kvm_hangul_backspace(&h);
        } else {
            char key = (char)('a' + (r >> 4) % 26);
            size_t n = kvm_hangul_input(&h, kvm_hangul_2set_jamo(key, (r >> 12) & 1), out);
            for (size_t k = 0; k < n; k++) {
                bad += !valid_output(out[k]);
            }
        }
        uint32_t p = kvm_hangul_preedit(&h);
        bad += (p != 0 && !valid_output(p));
        bad += h.nkeys > KVM_HANGUL_MAX_KEYS;
    }
    CHECK_EQ(bad, 0);
}

int main(void)
{
    printf("test_hangul\n");
    RUN(test_words);
    RUN(test_lone_jamo);
    RUN(test_backspace);
    RUN(test_refuses_non_jamo);
    RUN(test_every_syllable);
    RUN(test_random_walk);
    TEST_MAIN_END("test_hangul");
}
