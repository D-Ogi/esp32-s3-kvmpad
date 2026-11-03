// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_pinyin over the vendored AOSP decoder and the dictionary built from the vendored data (AT-EXT-26, pinyin):
// - the top candidate of common words and a full sentence, and of initials only ("nh");
// - a partial choice fixes the first word and keeps decoding the rest; consumed / parsed / complete and
//   cancel_last follow it (the contract the KEYBOARD view's ZH layer relies on);
// - out-of-range candidate ids, bad input (upper case, digits, too long, empty) and calls while closed are refused
//   without touching the engine;
// - the user dictionary stays disabled: no file is created next to the test;
// - 50 open/close cycles and 2000 random inputs (seeded) never crash under AddressSanitizer.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ime_pinyin.h"
#include "kvm_test.h"

static const char *s_dict;

static int top_is(const char *in, const uint16_t *want, size_t n)
{
    ime_pinyin_reset();
    if (ime_pinyin_search(in, strlen(in)) == 0) {
        return 0;
    }
    uint16_t c[32];
    const size_t len = ime_pinyin_candidate(0, c, 32);
    const int ok = len == n && memcmp(c, want, n * sizeof(uint16_t)) == 0;
    if (!ok) {
        printf("    %s: top candidate of %u units differs\n", in, (unsigned)len);
    }
    return ok;
}

#define TOP(in, ...)                                                                  \
    do {                                                                              \
        static const uint16_t w_[] = {__VA_ARGS__};                                   \
        CHECK(top_is(in, w_, sizeof(w_) / sizeof(w_[0])));                            \
    } while (0)

static void test_vectors(void)
{
    CHECK(ime_pinyin_open(s_dict));
    TOP("nihao", 0x4F60, 0x597D);
    TOP("zhongguo", 0x4E2D, 0x56FD);
    TOP("woaini", 0x6211, 0x7231, 0x4F60);
    TOP("xiexie", 0x8C22, 0x8C22);
    TOP("beijing", 0x5317, 0x4EAC);
    TOP("jintiantianqihenhao", 0x4ECA, 0x5929, 0x5929, 0x6C14, 0x5F88, 0x597D);
    TOP("zhongwenshurufa", 0x4E2D, 0x6587, 0x8F93, 0x5165, 0x6CD5);
    TOP("nh", 0x4F60, 0x597D);
    TOP("dianshi", 0x7535, 0x89C6);
    TOP("shi", 0x662F);
    TOP("xi'an", 0x897F, 0x5B89); // the separator splits xian into two syllables
    ime_pinyin_close();
}

static void test_partial_choice(void)
{
    CHECK(ime_pinyin_open(s_dict));
    ime_pinyin_reset();
    const size_t n = ime_pinyin_search("jintiantianqi", 13);
    CHECK(n > 2);
    // find the single-word candidate 今天 and choose it: the rest of the input stays to be decoded
    uint16_t c[32];
    size_t pick = n;
    for (size_t i = 0; i < n && i < 40; i++) {
        if (ime_pinyin_candidate(i, c, 32) == 2 && c[0] == 0x4ECA && c[1] == 0x5929) {
            pick = i;
            break;
        }
    }
    CHECK(pick < n);
    const size_t left = ime_pinyin_choose(pick);
    CHECK(left > 0);
    CHECK_EQ(ime_pinyin_fixed_len(), 2u);
    CHECK_EQ(ime_pinyin_consumed(), 7u); // "jintian"
    CHECK_EQ(ime_pinyin_parsed(), 13u);
    CHECK(!ime_pinyin_is_complete());
    // candidate 0 is the whole sentence, the fixed characters first
    CHECK(ime_pinyin_candidate(0, c, 32) == 4u && c[0] == 0x4ECA && c[1] == 0x5929);
    // Candidates 1.. cover the next position only and leave out the rest of the sentence itself (天气 is not
    // among them): choosing candidate 0 fixes the remainder and completes the sentence.
    CHECK(ime_pinyin_candidate(1, c, 32) == 1u && c[0] == 0x5929); // 天
    CHECK_EQ(ime_pinyin_choose(0), 1u);
    CHECK(ime_pinyin_is_complete());
    CHECK_EQ(ime_pinyin_consumed(), 13u);
    CHECK(ime_pinyin_candidate(0, c, 32) == 4u && c[2] == 0x5929 && c[3] == 0x6C14);
    // undo goes back one choice
    CHECK(ime_pinyin_cancel_last() > 1u);
    CHECK_EQ(ime_pinyin_fixed_len(), 2u);
    CHECK(!ime_pinyin_is_complete());
    // a single-character choice moves the fixed part by one syllable
    CHECK(ime_pinyin_choose(1) > 1u);
    CHECK_EQ(ime_pinyin_fixed_len(), 3u);
    CHECK_EQ(ime_pinyin_consumed(), 11u); // "jintiantian"
    // "v" starts no pinyin syllable: the decoder leaves it unparsed
    ime_pinyin_reset();
    (void)ime_pinyin_search("nihaov", 6);
    CHECK_EQ(ime_pinyin_parsed(), 5u);
    ime_pinyin_close();
    CHECK_EQ(ime_pinyin_consumed(), 0u); // closed
    CHECK(!ime_pinyin_is_complete());
    CHECK_EQ(ime_pinyin_cancel_last(), 0u);
}

static void test_refusals(void)
{
    uint16_t c[8] = {0x1234};
    CHECK_EQ(ime_pinyin_search("nihao", 5), 0u); // closed
    CHECK_EQ(ime_pinyin_candidate(0, c, 8), 0u);
    CHECK_EQ(c[0], 0u);
    CHECK(!ime_pinyin_open(NULL));
    CHECK(!ime_pinyin_open("no-such-dictionary.dat"));
    CHECK(!ime_pinyin_is_open());

    CHECK(ime_pinyin_open(s_dict));
    const size_t n = ime_pinyin_search("nihao", 5);
    CHECK(n > 0);
    CHECK_EQ(ime_pinyin_candidate(n, c, 8), 0u); // one past the end: refused before the engine indexes it
    CHECK_EQ(ime_pinyin_candidate(100000u, c, 8), 0u);
    CHECK_EQ(ime_pinyin_choose(100000u), n);     // nothing chosen
    CHECK_EQ(ime_pinyin_candidate(0, c, 1), 0u); // no room for a character and the terminator
    CHECK_EQ(ime_pinyin_search("NiHao", 5), 0u);
    CHECK_EQ(ime_pinyin_search("ni3hao", 6), 0u);
    CHECK_EQ(ime_pinyin_search("", 0), 0u);
    CHECK_EQ(ime_pinyin_search(NULL, 3), 0u);
    char longer[IME_PINYIN_MAX_INPUT + 2];
    memset(longer, 'a', sizeof(longer));
    CHECK_EQ(ime_pinyin_search(longer, IME_PINYIN_MAX_INPUT + 1), 0u);
    CHECK_EQ(ime_pinyin_candidate(0, c, 8), 0u); // a refused search leaves no candidates
    ime_pinyin_close();

    FILE *f = fopen("/ime/.no-user-dict", "rb");
    CHECK(f == NULL); // the user dictionary was never created
    if (f != NULL) {
        fclose(f);
    }
}

static void test_cycles_and_random(void)
{
    for (int i = 0; i < 50; i++) {
        CHECK(ime_pinyin_open(s_dict));
        CHECK(ime_pinyin_search("nihao", 5) > 0);
        ime_pinyin_close();
    }
    CHECK(ime_pinyin_open(s_dict));
    uint32_t seed = 0x6B766D70u;
    uint16_t c[32];
    for (int i = 0; i < 2000; i++) {
        char in[IME_PINYIN_MAX_INPUT];
        seed = seed * 1103515245u + 12345u;
        const size_t len = 1u + (seed >> 16) % IME_PINYIN_MAX_INPUT;
        for (size_t k = 0; k < len; k++) {
            seed = seed * 1103515245u + 12345u;
            const unsigned r = (seed >> 16) % 27u;
            in[k] = r == 26u ? '\'' : (char)('a' + r);
        }
        ime_pinyin_reset();
        const size_t n = ime_pinyin_search(in, len);
        for (size_t k = 0; k < n && k < 5; k++) {
            (void)ime_pinyin_candidate(k, c, 32);
        }
        if (n > 1 && (seed & 1u)) {
            (void)ime_pinyin_choose(1);
            (void)ime_pinyin_candidate(0, c, 32);
        }
    }
    ime_pinyin_close();
}

int main(int argc, char **argv)
{
    s_dict = argc > 1 ? argv[1] : "res/raw/dict_pinyin.dat";
    RUN(test_vectors);
    RUN(test_partial_choice);
    RUN(test_refusals);
    RUN(test_cycles_and_random);
    TEST_MAIN_END("test_ime_pinyin");
}
