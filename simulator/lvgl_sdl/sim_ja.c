// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// sim_ja: see sim_ja.h. Words are matched longest first from the start of the input; the rest stays as typed.
// Candidate 0 is the sentence (the fixed choices, then the first conversion of every word left), candidates 1..
// the conversions of the next unfixed word, the last of them its kana. The reading is the kana of the unfixed words.
#include "sim_ja.h"

#include <stdint.h>
#include <string.h>

#define ALTS 4u
#define UNITS 4u
#define WORDS_MAX UI_VIEW_KBD_PINYIN_CAP

typedef struct {
    const char *romaji;
    uint16_t kana[UNITS + 1u];
    uint16_t alt[ALTS][UNITS + 1u]; // conversions, 0-terminated; an empty row ends the list
} word_t;

static const word_t k_words[] = {
    {"nihon", {0x306B, 0x307B, 0x3093}, {{0x65E5, 0x672C}, {0x4E8C, 0x672C}, {0x30CB, 0x30DB, 0x30F3}}},  // 日本 二本 ニホン
    {"go", {0x3054}, {{0x8A9E}, {0x5F8C}, {0x7881}, {0x4E94}}},                                         // 語 後 碁 五
    {"kanji", {0x304B, 0x3093, 0x3058}, {{0x6F22, 0x5B57}, {0x611F, 0x3058}, {0x5E79, 0x4E8B}}},        // 漢字 感じ 幹事
    {"kannzi", {0x304B, 0x3093, 0x3058}, {{0x6F22, 0x5B57}, {0x611F, 0x3058}, {0x5E79, 0x4E8B}}},       // the flick pad's spelling
    {"henkan",{0x3078, 0x3093, 0x304B, 0x3093}, {{0x5909, 0x63DB}, {0x8FD4, 0x9084}}},                  // 変換 返還
    {"desu", {0x3067, 0x3059}, {{0x3067, 0x3059}}},                                                     // です
    {"watashi", {0x308F, 0x305F, 0x3057}, {{0x79C1}, {0x6E21, 0x3057}}},                                // 私 渡し
    {"wa", {0x306F}, {{0x306F}, {0x8F2A}}},                                                             // は 輪
};

typedef struct {
    uint8_t w[WORDS_MAX];     // index into k_words
    uint8_t end[WORDS_MAX];   // input position after each word
    uint8_t chosen[WORDS_MAX]; // alt index fixed for each word (ALTS = its kana)
    size_t n;
    size_t fixed;
    size_t stack[WORDS_MAX + 1u];
    size_t depth;
} ja_t;

static ja_t s_ja;

static size_t len16(const uint16_t *u)
{
    size_t n = 0;
    while (n < UNITS && u[n] != 0u) {
        n++;
    }
    return n;
}

static size_t alts_of(const word_t *w)
{
    size_t n = 0;
    while (n < ALTS && w->alt[n][0] != 0u) {
        n++;
    }
    return n + 1u; // and the kana itself
}

static const uint16_t *conv_of(const word_t *w, size_t a)
{
    return a < alts_of(w) - 1u ? w->alt[a] : w->kana;
}

static size_t count(const ja_t *j)
{
    return j->n == 0u ? 0u : (j->fixed >= j->n ? 1u : 1u + alts_of(&k_words[j->w[j->fixed]]));
}

static size_t put(uint16_t *out, size_t n, size_t cap, const uint16_t *u)
{
    for (size_t i = 0; i < len16(u) && n + 1u < cap; i++) {
        out[n++] = u[i];
    }
    return n;
}

static size_t j_search(void *ctx, const char *s, size_t len)
{
    ja_t *j = ctx;
    memset(j, 0, sizeof(*j));
    size_t i = 0;
    while (i < len && j->n < WORDS_MAX) {
        size_t best = SIZE_MAX;
        size_t best_len = 0;
        for (size_t k = 0; k < sizeof(k_words) / sizeof(k_words[0]); k++) {
            const size_t l = strlen(k_words[k].romaji);
            if (l > best_len && l <= len - i && memcmp(s + i, k_words[k].romaji, l) == 0) {
                best = k;
                best_len = l;
            }
        }
        if (best == SIZE_MAX) {
            break; // the rest stays as typed
        }
        j->w[j->n] = (uint8_t)best;
        i += best_len;
        j->end[j->n++] = (uint8_t)i;
    }
    return count(j);
}

static size_t j_candidate(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    const ja_t *j = ctx;
    size_t n = 0;
    if (i >= count(j) || cap < 2u) {
        return 0;
    }
    if (i == 0u) {
        for (size_t s = 0; s < j->n; s++) {
            const word_t *w = &k_words[j->w[s]];
            n = put(out, n, cap, conv_of(w, s < j->fixed ? j->chosen[s] : 0u));
        }
    } else {
        n = put(out, n, cap, conv_of(&k_words[j->w[j->fixed]], i - 1u));
    }
    out[n] = 0;
    return n;
}

static size_t j_choose(void *ctx, size_t i)
{
    ja_t *j = ctx;
    if (i >= count(j) || j->fixed >= j->n) {
        return count(j);
    }
    j->stack[j->depth++] = j->fixed;
    if (i == 0u) {
        for (size_t s = j->fixed; s < j->n; s++) {
            j->chosen[s] = 0u;
        }
        j->fixed = j->n;
    } else {
        j->chosen[j->fixed++] = (uint8_t)(i - 1u);
    }
    return count(j);
}

static size_t j_cancel_last(void *ctx)
{
    ja_t *j = ctx;
    if (j->depth > 0u) {
        j->fixed = j->stack[--j->depth];
    }
    return count(j);
}

static size_t j_fixed_len(void *ctx)
{
    const ja_t *j = ctx;
    size_t n = 0;
    for (size_t s = 0; s < j->fixed; s++) {
        n += len16(conv_of(&k_words[j->w[s]], j->chosen[s]));
    }
    return n;
}

static size_t j_parsed(void *ctx)
{
    const ja_t *j = ctx;
    return j->n == 0u ? 0u : j->end[j->n - 1u];
}

static size_t j_consumed(void *ctx)
{
    const ja_t *j = ctx;
    return j->fixed == 0u ? 0u : j->end[j->fixed - 1u];
}

static bool j_complete(void *ctx)
{
    const ja_t *j = ctx;
    return j->n > 0u && j->fixed >= j->n;
}

static void j_reset(void *ctx)
{
    memset(ctx, 0, sizeof(ja_t));
}

static size_t j_preedit(void *ctx, uint16_t *out, size_t cap)
{
    const ja_t *j = ctx;
    size_t n = 0;
    if (cap == 0u) {
        return 0;
    }
    for (size_t s = j->fixed; s < j->n; s++) {
        n = put(out, n, cap, k_words[j->w[s]].kana);
    }
    out[n] = 0;
    return n;
}

static const ui_kbd_conv_engine_t k_engine = {
    .ctx = &s_ja,
    .search = j_search,
    .candidate = j_candidate,
    .choose = j_choose,
    .cancel_last = j_cancel_last,
    .fixed_len = j_fixed_len,
    .consumed = j_consumed,
    .parsed = j_parsed,
    .complete = j_complete,
    .reset = j_reset,
    .preedit = j_preedit,
};

const ui_kbd_conv_engine_t *sim_ja_engine(void)
{
    return &k_engine;
}
