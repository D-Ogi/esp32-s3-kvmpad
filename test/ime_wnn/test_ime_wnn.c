// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_wnn over the vendored AOSP OpenWnn engine and the Japanese dictionaries of the ime_data image
// (AT-EXT-26, Japanese part):
// - the whole-sentence candidate of common readings, and the alternatives of their first clause;
// - romaji edge cases: a double consonant, "nn", the separator, the long vowel mark, an incomplete
//   syllable left over at the end;
// - a partial choice fixes the first clause and converts the rest; fixed_len / consumed / parsed /
//   complete and cancel_last follow it, and candidate 0 keeps starting with the fixed text (the
//   contract the KEYBOARD view's JA layer relies on, the same one as ime_pinyin);
// - out-of-range candidate ids, bad input (upper case, digits, too long, empty) and calls while
//   closed are refused without touching the engine;
// - 50 open/close cycles and 2000 seeded random inputs never crash under the sanitizers.
//
// Every expected conversion below was recorded from a real run of this engine; none was invented.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ime_wnn.h"
#include "kvm_test.h"

static void *s_blob;
static size_t s_blob_len;
static ime_wnn_dicts_t s_dicts;

static int load_blob(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    s_blob = malloc((size_t)n);
    const size_t got = fread(s_blob, 1, (size_t)n, f);
    fclose(f);
    s_blob_len = got;
    return got == (size_t)n;
}

static void dump(const char *what, const uint16_t *s, size_t n)
{
    printf("    %s:", what);
    for (size_t i = 0; i < n; i++) {
        printf(" %04X", s[i]);
    }
    printf("\n");
}

static int cand_is(size_t i, const uint16_t *want, size_t n)
{
    uint16_t c[256];
    const size_t len = ime_wnn_candidate(i, c, 256);
    const int ok = len == n && memcmp(c, want, n * sizeof(uint16_t)) == 0;
    if (!ok) {
        printf("    candidate %u has %u units, expected %u\n", (unsigned)i, (unsigned)len,
               (unsigned)n);
        dump("got ", c, len);
        dump("want", want, n);
    }
    return ok;
}

static int preedit_is(const uint16_t *want, size_t n)
{
    uint16_t c[256];
    const size_t len = ime_wnn_preedit(c, 256);
    const int ok = len == n && memcmp(c, want, n * sizeof(uint16_t)) == 0;
    if (!ok) {
        dump("preedit got ", c, len);
        dump("preedit want", want, n);
    }
    return ok;
}

// The whole-sentence candidate of `in`.
#define TOP(in, ...)                                                   \
    do {                                                               \
        static const uint16_t w_[] = {__VA_ARGS__};                    \
        ime_wnn_reset();                                               \
        CHECK(ime_wnn_search(in, strlen(in)) > 0);                     \
        CHECK(cand_is(0, w_, sizeof(w_) / sizeof(w_[0])));             \
    } while (0)

// The reading `in` composes into, before any conversion.
#define KANA(in, ...)                                                  \
    do {                                                               \
        static const uint16_t w_[] = {__VA_ARGS__};                    \
        ime_wnn_reset();                                               \
        ime_wnn_search(in, strlen(in));                                \
        CHECK(preedit_is(w_, sizeof(w_) / sizeof(w_[0])));             \
    } while (0)

static void test_blob(void)
{
    ime_wnn_dicts_t d;
    CHECK(!ime_wnn_dicts_from_blob(NULL, s_blob_len, &d));
    CHECK(!ime_wnn_dicts_from_blob(s_blob, s_blob_len, NULL));
    CHECK(!ime_wnn_dicts_from_blob(s_blob, 32, &d));
    // a broken magic, version, count and a truncated entry are all refused
    uint8_t *copy = malloc(s_blob_len);
    memcpy(copy, s_blob, s_blob_len);
    copy[0] = 'X';
    CHECK(!ime_wnn_dicts_from_blob(copy, s_blob_len, &d));
    memcpy(copy, s_blob, 64);
    copy[8] = 2;
    CHECK(!ime_wnn_dicts_from_blob(copy, s_blob_len, &d));
    copy[8] = 1;
    copy[12] = 3;
    CHECK(!ime_wnn_dicts_from_blob(copy, s_blob_len, &d));
    copy[12] = 4;
    CHECK(ime_wnn_dicts_from_blob(copy, s_blob_len, &d));
    CHECK(!ime_wnn_dicts_from_blob(copy, s_blob_len - 1, &d));
    free(copy);

    CHECK(ime_wnn_dicts_from_blob(s_blob, s_blob_len, &s_dicts));
    CHECK(s_dicts.tankanji != NULL && s_dicts.tankanji_len == 81907u);
    CHECK(s_dicts.jiritsu != NULL && s_dicts.jiritsu_len == 783246u);
    CHECK(s_dicts.fzk != NULL && s_dicts.fzk_len == 5785u);
    CHECK(s_dicts.connect != NULL && s_dicts.connect_len == 18817u);
    printf("    work area %u B\n", (unsigned)ime_wnn_work_size());
}

static void test_open_refusals(void)
{
    CHECK(!ime_wnn_is_open());
    // calls while closed touch nothing and answer empty
    uint16_t c[8];
    CHECK_EQ(ime_wnn_search("nihongo", 7), 0u);
    CHECK_EQ(ime_wnn_candidate(0, c, 8), 0u);
    CHECK_EQ(ime_wnn_choose(0), 0u);
    CHECK_EQ(ime_wnn_cancel_last(), 0u);
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    CHECK_EQ(ime_wnn_consumed(), 0u);
    CHECK_EQ(ime_wnn_parsed(), 0u);
    CHECK(!ime_wnn_is_complete());
    CHECK_EQ(ime_wnn_preedit(c, 8), 0u);

    CHECK(!ime_wnn_open(NULL));
    CHECK(!ime_wnn_is_open());
    // a dictionary set with a swapped role is refused: every header is checked against its type
    ime_wnn_dicts_t bad = s_dicts;
    bad.jiritsu = s_dicts.fzk;
    bad.jiritsu_len = s_dicts.fzk_len;
    CHECK(!ime_wnn_open(&bad));
    bad = s_dicts;
    bad.connect_len = s_dicts.connect_len - 1;
    CHECK(!ime_wnn_open(&bad));
    bad = s_dicts;
    bad.tankanji = NULL;
    CHECK(!ime_wnn_open(&bad));
    CHECK(!ime_wnn_is_open());
}

static void test_vectors(void)
{
    CHECK(ime_wnn_open(&s_dicts));
    CHECK(ime_wnn_is_open());
    TOP("nihongo", 0x65E5, 0x672C, 0x8A9E);                 // nihongo
    TOP("toukyou", 0x6771, 0x4EAC);                         // toukyou
    TOP("watashi", 0x79C1);                                  // watashi
    TOP("tabetai", 0x98DF, 0x3079, 0x305F, 0x3044);          // tabetai
    TOP("nihongonyuuryoku", 0x65E5, 0x672C, 0x8A9E, 0x5165, 0x529B);
    // a whole sentence over three clauses
    TOP("kyouhaiitenkidesu", 0x4ECA, 0x65E5, 0x306F, 0x3044, 0x3044, 0x5929, 0x6C17, 0x3067, 0x3059);
    // the engine keeps the reading when no entry scores better
    TOP("arigatou", 0x3042, 0x308A, 0x304C, 0x3068, 0x3046);
    TOP("konnnichiha", 0x3053, 0x3093, 0x306B, 0x3061, 0x306F);

    // alternatives of the first clause: deduplicated conversions, then hiragana, then katakana
    ime_wnn_reset();
    CHECK_EQ(ime_wnn_search("nihongo", 7), 4u);
    static const uint16_t nihongo[] = {0x65E5, 0x672C, 0x8A9E};
    static const uint16_t nihongo_h[] = {0x306B, 0x307B, 0x3093, 0x3054};
    static const uint16_t nihongo_k[] = {0x30CB, 0x30DB, 0x30F3, 0x30B4};
    CHECK(cand_is(1, nihongo, 3));
    CHECK(cand_is(2, nihongo_h, 4));
    CHECK(cand_is(3, nihongo_k, 4));
    // a reading with many homophones fills the list without overflowing it
    ime_wnn_reset();
    const size_t n = ime_wnn_search("na", 2);
    CHECK(n > 8 && n <= IME_WNN_MAX_CANDIDATES);
    static const uint16_t na[] = {0x306A};
    CHECK(cand_is(0, na, 1));
    // every candidate is non-empty and within the contract
    for (size_t i = 0; i < n; i++) {
        uint16_t c[256];
        const size_t len = ime_wnn_candidate(i, c, 256);
        CHECK(len > 0 && len <= IME_WNN_MAX_TEXT);
        CHECK_EQ(c[len], 0);
    }
    ime_wnn_close();
}

static void test_romaji(void)
{
    CHECK(ime_wnn_open(&s_dicts));
    KANA("kka", 0x3063, 0x304B);                   // a double consonant gives the small tsu
    KANA("tte", 0x3063, 0x3066);
    KANA("nn", 0x3093);                            // "nn" is one kana
    KANA("nihongo", 0x306B, 0x307B, 0x3093, 0x3054);
    KANA("kya", 0x304D, 0x3083);                   // a two-kana row
    KANA("shi", 0x3057);
    KANA("xtu", 0x3063);
    KANA("ka-", 0x304B, 0x30FC);                   // the long vowel mark
    KANA("n", 'n');                                // a lone "n" stays as typed
    KANA("nihongok", 0x306B, 0x307B, 0x3093, 0x3054, 'k'); // an incomplete syllable at the end
    KANA("n'a", 0x3093, 0x3042);                   // the separator ends the syllable
    KANA("n'", 0x3093);
    KANA("k'a", 'k', 0x3042);                      // and blocks a merge across it
    KANA("ka'a", 0x304B, 0x3042);
    // parsed() stops at the incomplete syllable, the rest of the input is still romaji
    ime_wnn_reset();
    ime_wnn_search("nihongok", 8);
    CHECK_EQ(ime_wnn_parsed(), 7u);
    CHECK_EQ(ime_wnn_consumed(), 0u);
    ime_wnn_reset();
    ime_wnn_search("nihongo", 7);
    CHECK_EQ(ime_wnn_parsed(), 7u);
    // a lone incomplete syllable converts nothing but still shows in the preedit
    ime_wnn_reset();
    CHECK_EQ(ime_wnn_search("k", 1), 0u);
    CHECK_EQ(ime_wnn_parsed(), 0u);
    static const uint16_t k[] = {'k'};
    CHECK(preedit_is(k, 1));
    CHECK(!ime_wnn_is_complete());
    ime_wnn_close();
}

static void test_partial_choice(void)
{
    CHECK(ime_wnn_open(&s_dicts));
    const size_t n = ime_wnn_search("kyouhaiitenkidesu", 17);
    CHECK(n > 2);
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    CHECK_EQ(ime_wnn_consumed(), 0u);
    CHECK_EQ(ime_wnn_parsed(), 17u);
    CHECK(!ime_wnn_is_complete());

    // candidate 1 is the best conversion of the first clause: fix it
    uint16_t first[256];
    const size_t fn = ime_wnn_candidate(1, first, 256);
    CHECK_EQ(fn, 3u);
    static const uint16_t kyouha[] = {0x4ECA, 0x65E5, 0x306F};
    CHECK(cand_is(1, kyouha, 3));

    const size_t left = ime_wnn_choose(1);
    CHECK(left > 1);
    CHECK_EQ(ime_wnn_fixed_len(), 3u);
    CHECK_EQ(ime_wnn_consumed(), 6u);  // "kyouha"
    CHECK_EQ(ime_wnn_parsed(), 17u);
    CHECK(!ime_wnn_is_complete());
    // candidate 0 still is the whole sentence and starts with what is fixed
    uint16_t c0[256];
    const size_t c0n = ime_wnn_candidate(0, c0, 256);
    CHECK_EQ(c0n, 9u);
    CHECK(memcmp(c0, first, fn * sizeof(uint16_t)) == 0);
    // candidates 1.. now cover the second clause
    static const uint16_t ii[] = {0x3044, 0x3044};
    CHECK(cand_is(1, ii, 2));

    // undo
    const size_t back = ime_wnn_cancel_last();
    CHECK_EQ(back, n);
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    CHECK_EQ(ime_wnn_consumed(), 0u);
    CHECK(cand_is(0, c0, c0n));
    CHECK_EQ(ime_wnn_cancel_last(), n);  // nothing left to undo

    // fix the rest clause by clause; candidate 0 never changes and consumed only grows
    size_t guard = 0;
    size_t consumed = 0;
    size_t cands = ime_wnn_search("kyouhaiitenkidesu", 17);
    while (!ime_wnn_is_complete() && guard++ < 32) {
        const size_t before = ime_wnn_consumed();
        CHECK(before >= consumed);
        consumed = before;
        cands = ime_wnn_choose(cands > 1 ? 1u : 0u);
        CHECK(cands > 0);
    }
    CHECK(ime_wnn_is_complete());
    CHECK_EQ(ime_wnn_consumed(), 17u);
    CHECK_EQ(ime_wnn_fixed_len(), 9u);
    static const uint16_t sentence[] = {0x4ECA, 0x65E5, 0x306F, 0x3044,
                                        0x3044, 0x5929, 0x6C17, 0x3067, 0x3059};
    CHECK(cand_is(0, sentence, 9));
    CHECK_EQ(ime_wnn_preedit(c0, 256), 0u);

    // choose(0) commits everything in one step
    ime_wnn_reset();
    const size_t all = ime_wnn_search("kyouhaiitenkidesu", 17);
    CHECK(all > 0);
    CHECK_EQ(ime_wnn_choose(0), 1u);
    CHECK(ime_wnn_is_complete());
    CHECK_EQ(ime_wnn_fixed_len(), 9u);
    CHECK_EQ(ime_wnn_consumed(), 17u);
    CHECK(cand_is(0, sentence, 9));
    CHECK_EQ(ime_wnn_choose(0), 1u);  // nothing left to fix
    CHECK_EQ(ime_wnn_cancel_last(), all);
    CHECK(!ime_wnn_is_complete());
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    ime_wnn_close();
}

static void test_input_refusals(void)
{
    CHECK(ime_wnn_open(&s_dicts));
    CHECK_EQ(ime_wnn_search(NULL, 4), 0u);
    CHECK_EQ(ime_wnn_search("", 0), 0u);
    CHECK_EQ(ime_wnn_search("Nihongo", 7), 0u);   // upper case
    CHECK_EQ(ime_wnn_search("ni2go", 5), 0u);     // a digit
    CHECK_EQ(ime_wnn_search("ni go", 5), 0u);     // a space
    CHECK_EQ(ime_wnn_search("ni.go", 5), 0u);     // punctuation the engine does not take
    char too_long[IME_WNN_MAX_INPUT + 2];
    memset(too_long, 'a', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = 0;
    CHECK_EQ(ime_wnn_search(too_long, IME_WNN_MAX_INPUT + 1), 0u);
    CHECK(ime_wnn_search(too_long, IME_WNN_MAX_INPUT) > 0);  // the longest input still converts
    // a refused search leaves no state behind
    CHECK_EQ(ime_wnn_search("ni2go", 5), 0u);
    CHECK_EQ(ime_wnn_parsed(), 0u);
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    uint16_t c[256];
    CHECK_EQ(ime_wnn_preedit(c, 256), 0u);

    // candidate ids and buffers
    const size_t n = ime_wnn_search("nihongo", 7);
    CHECK_EQ(n, 4u);
    CHECK_EQ(ime_wnn_candidate(n, c, 256), 0u);
    CHECK_EQ(ime_wnn_candidate(n + 100, c, 256), 0u);
    CHECK_EQ(ime_wnn_candidate((size_t)-1, c, 256), 0u);
    CHECK_EQ(ime_wnn_candidate(0, NULL, 256), 0u);
    CHECK_EQ(ime_wnn_candidate(0, c, 0), 0u);
    CHECK_EQ(ime_wnn_candidate(0, c, 1), 0u);
    CHECK_EQ(ime_wnn_candidate(0, c, 2), 1u);  // truncated to what fits, still NUL-terminated
    CHECK_EQ(c[1], 0);
    CHECK_EQ(ime_wnn_choose(n), n);            // an out-of-range choice changes nothing
    CHECK_EQ(ime_wnn_fixed_len(), 0u);
    CHECK_EQ(ime_wnn_preedit(NULL, 256), 0u);
    CHECK_EQ(ime_wnn_preedit(c, 1), 0u);
    ime_wnn_close();
    CHECK(!ime_wnn_is_open());
}

static void test_cycles(void)
{
    for (int i = 0; i < 50; i++) {
        CHECK(ime_wnn_open(&s_dicts));
        static const uint16_t want[] = {0x65E5, 0x672C, 0x8A9E};
        CHECK(ime_wnn_search("nihongo", 7) > 0);
        CHECK(cand_is(0, want, 3));
        ime_wnn_close();
    }
    // a second close is harmless, and so is a close without an open
    ime_wnn_close();
    CHECK(!ime_wnn_is_open());
}

static uint32_t s_rng = 20261102u;

static uint32_t rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

static void test_random(void)
{
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz-'";
    CHECK(ime_wnn_open(&s_dicts));
    size_t converted = 0, truncated = 0;
    for (int it = 0; it < 2000; it++) {
        char in[IME_WNN_MAX_INPUT + 1];
        const size_t len = 1u + rnd() % IME_WNN_MAX_INPUT;
        for (size_t i = 0; i < len; i++) {
            in[i] = alphabet[rnd() % (sizeof(alphabet) - 1)];
        }
        in[len] = 0;
        size_t n = ime_wnn_search(in, len);
        if (n > 0) {
            converted++;
        }
        if (ime_wnn_truncated()) {
            truncated++;
        }
        for (int step = 0; step < 4 && n > 0; step++) {
            uint16_t c[256];
            for (size_t i = 0; i < n; i++) {
                const size_t cl = ime_wnn_candidate(i, c, 256);
                if (cl == 0 || cl > IME_WNN_MAX_TEXT || c[cl] != 0) {
                    CHECK(0);
                    break;
                }
            }
            if (ime_wnn_is_complete()) {
                break;
            }
            const size_t pick = n > 1 ? 1u + rnd() % (n - 1u) : 0u;
            const size_t before = ime_wnn_consumed();
            n = ime_wnn_choose(pick);
            if (ime_wnn_consumed() < before) {
                CHECK(0);
            }
        }
        if (rnd() % 4 == 0) {
            ime_wnn_cancel_last();
        }
    }
    printf("    2000 random inputs: %u converted, %u hit an internal limit\n", (unsigned)converted,
           (unsigned)truncated);
    ime_wnn_close();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <wnn_ja.bin>\n", argv[0]);
        return EXIT_FAILURE;
    }
    if (!load_blob(argv[1])) {
        return EXIT_FAILURE;
    }
    RUN(test_blob);
    RUN(test_open_refusals);
    RUN(test_vectors);
    RUN(test_romaji);
    RUN(test_partial_choice);
    RUN(test_input_refusals);
    RUN(test_cycles);
    RUN(test_random);
    free(s_blob);
    TEST_MAIN_END("test_ime_wnn");
}
