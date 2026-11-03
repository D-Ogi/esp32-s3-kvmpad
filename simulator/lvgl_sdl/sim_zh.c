// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// sim_zh: see sim_zh.h. Same candidate contract as ime_pinyin.h: candidate 0 is the whole sentence (the fixed
// characters first), candidates 1.. are alternatives for the first syllable not yet fixed.
#include "sim_zh.h"

#include <stdint.h>
#include <string.h>

#define ALTS 6u
#define SYL_MAX UI_VIEW_KBD_PINYIN_CAP

typedef struct {
    const char *pinyin;
    uint16_t hanzi[ALTS];
} syllable_t;

static const syllable_t k_syl[] = {
    {"zhong", {0x4E2D, 0x79CD, 0x91CD, 0x4F17, 0x949F, 0x7EC8}}, // 中 种 重 众 钟 终
    {"tian", {0x5929, 0x7530, 0x751C, 0x586B, 0x6DFB, 0x8214}},  // 天 田 甜 填 添 舔
    {"hao", {0x597D, 0x53F7, 0x6D69, 0x8C6A, 0x8017, 0x6BEB}},   // 好 号 浩 豪 耗 毫
    {"jin", {0x4ECA, 0x8FDB, 0x91D1, 0x8FD1, 0x7D27, 0x5C3D}},   // 今 进 金 近 紧 尽
    {"guo", {0x56FD, 0x8FC7, 0x679C, 0x9505, 0x88F9, 0x90ED}},   // 国 过 果 锅 裹 郭
    {"ni", {0x4F60, 0x6CE5, 0x5462, 0x5C3C, 0x62DF, 0x9006}},    // 你 泥 呢 尼 拟 逆
};

typedef struct {
    uint8_t syl[SYL_MAX];    // index into k_syl
    uint8_t end[SYL_MAX];    // input position after each syllable
    uint16_t chosen[SYL_MAX];
    size_t n;
    size_t fixed;
    size_t stack[SYL_MAX + 1u];
    size_t depth;
} zh_t;

static zh_t s_zh;

static size_t count(const zh_t *z)
{
    return z->n == 0u ? 0u : (z->fixed >= z->n ? 1u : 1u + ALTS);
}

static size_t z_search(void *ctx, const char *s, size_t len)
{
    zh_t *z = ctx;
    memset(z, 0, sizeof(*z));
    size_t i = 0;
    while (i < len && z->n < SYL_MAX) {
        if (s[i] == '\'') {
            i++;
            continue;
        }
        size_t best = SIZE_MAX;
        size_t best_len = 0;
        for (size_t k = 0; k < sizeof(k_syl) / sizeof(k_syl[0]); k++) {
            const size_t l = strlen(k_syl[k].pinyin);
            if (l > best_len && l <= len - i && memcmp(s + i, k_syl[k].pinyin, l) == 0) {
                best = k;
                best_len = l;
            }
        }
        if (best == SIZE_MAX) {
            break; // the rest stays as typed
        }
        z->syl[z->n] = (uint8_t)best;
        i += best_len;
        z->end[z->n++] = (uint8_t)i;
    }
    return count(z);
}

static size_t z_candidate(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    const zh_t *z = ctx;
    size_t n = 0;
    if (i >= count(z) || cap < 2u) {
        return 0;
    }
    if (i == 0u) {
        for (size_t s = 0; s < z->n && n + 1u < cap; s++) {
            out[n++] = s < z->fixed ? z->chosen[s] : k_syl[z->syl[s]].hanzi[0];
        }
    } else {
        out[n++] = k_syl[z->syl[z->fixed]].hanzi[i - 1u];
    }
    out[n] = 0;
    return n;
}

static size_t z_choose(void *ctx, size_t i)
{
    zh_t *z = ctx;
    if (i >= count(z) || z->fixed >= z->n) {
        return count(z);
    }
    z->stack[z->depth++] = z->fixed;
    if (i == 0u) {
        for (size_t s = z->fixed; s < z->n; s++) {
            z->chosen[s] = k_syl[z->syl[s]].hanzi[0];
        }
        z->fixed = z->n;
    } else {
        z->chosen[z->fixed] = k_syl[z->syl[z->fixed]].hanzi[i - 1u];
        z->fixed++;
    }
    return count(z);
}

static size_t z_cancel_last(void *ctx)
{
    zh_t *z = ctx;
    if (z->depth > 0u) {
        z->fixed = z->stack[--z->depth];
    }
    return count(z);
}

static size_t z_fixed_len(void *ctx)
{
    return ((const zh_t *)ctx)->fixed;
}

static size_t z_parsed(void *ctx)
{
    const zh_t *z = ctx;
    return z->n == 0u ? 0u : z->end[z->n - 1u];
}

static size_t z_consumed(void *ctx)
{
    const zh_t *z = ctx;
    return z->fixed == 0u ? 0u : z->end[z->fixed - 1u];
}

static bool z_complete(void *ctx)
{
    const zh_t *z = ctx;
    return z->n > 0u && z->fixed >= z->n;
}

static void z_reset(void *ctx)
{
    memset(ctx, 0, sizeof(zh_t));
}

static const ui_kbd_conv_engine_t k_engine = {
    .ctx = &s_zh,
    .search = z_search,
    .candidate = z_candidate,
    .choose = z_choose,
    .cancel_last = z_cancel_last,
    .fixed_len = z_fixed_len,
    .consumed = z_consumed,
    .parsed = z_parsed,
    .complete = z_complete,
    .reset = z_reset,
};

const ui_kbd_conv_engine_t *sim_zh_engine(void)
{
    return &k_engine;
}
