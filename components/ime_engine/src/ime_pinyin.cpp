// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_pinyin: see ime_pinyin.h. Thin, allocation-free glue over pinyinime.h. Input is validated here so the
// engine only ever sees lowercase letters and apostrophes of a bounded length, and a candidate id is checked
// against the last count because the engine indexes its candidate array without a bound check.
#include "ime_pinyin.h"

#include "../pinyinime/include/pinyinime.h"
#include "ime_alloc.h"

using namespace ime_pinyin;

// A path under the read-only mount (or a missing file on a host): the engine's user dictionary fails to open
// and stays disabled.
static const char *const k_no_user_dict = "/ime/.no-user-dict";
static bool s_open;
static size_t s_cands; // candidates of the last search / choose

static bool valid_input(const char *s, size_t len)
{
    if (s == nullptr || len == 0 || len > IME_PINYIN_MAX_INPUT) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') || s[i] == '\'')) {
            return false;
        }
    }
    return true;
}

extern "C" bool ime_pinyin_open(const char *sys_dict_path)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (s_open) {
        im_close_decoder();
        s_open = false;
    }
    s_cands = 0;
    if (sys_dict_path == nullptr) {
        return false;
    }
    s_open = im_open_decoder(sys_dict_path, k_no_user_dict);
    if (!s_open) {
        im_close_decoder(); // frees whatever a half-done open allocated
    }
    return s_open;
}

extern "C" void ime_pinyin_close(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (s_open) {
        im_close_decoder();
        s_open = false;
    }
    s_cands = 0;
}

extern "C" bool ime_pinyin_is_open(void)
{
    return s_open;
}

extern "C" size_t ime_pinyin_search(const char *letters, size_t len)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open || !valid_input(letters, len)) {
        s_cands = 0;
        return 0;
    }
    s_cands = im_search(letters, len);
    return s_cands;
}

extern "C" size_t ime_pinyin_candidate(size_t i, uint16_t *out, size_t cap)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (out != nullptr && cap > 0) {
        out[0] = 0;
    }
    if (!s_open || out == nullptr || cap < 2 || i >= s_cands) {
        return 0;
    }
    char16 *r = im_get_candidate(i, reinterpret_cast<char16 *>(out), cap - 1);
    if (r == nullptr) {
        out[0] = 0;
        return 0;
    }
    size_t n = 0;
    while (n < cap - 1 && out[n] != 0) {
        n++;
    }
    out[n] = 0;
    return n;
}

extern "C" size_t ime_pinyin_choose(size_t i)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open || i >= s_cands) {
        return s_cands;
    }
    s_cands = im_choose(i);
    return s_cands;
}

extern "C" size_t ime_pinyin_fixed_len(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    return s_open ? im_get_fixed_len() : 0;
}

extern "C" size_t ime_pinyin_cancel_last(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open) {
        return 0;
    }
    s_cands = im_cancel_last_choice();
    return s_cands;
}

// start[0..n] are the syllable start positions in the input; start[n] is the end of the last syllable.
static size_t syllables(const uint16 **start)
{
    const uint16 *p = nullptr;
    const size_t n = im_get_spl_start_pos(p);
    *start = p;
    return p != nullptr ? n : 0u;
}

extern "C" size_t ime_pinyin_consumed(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open) {
        return 0;
    }
    const uint16 *start;
    const size_t n = syllables(&start);
    const size_t f = im_get_fixed_len();
    return (start != nullptr && f <= n) ? start[f] : 0u;
}

extern "C" size_t ime_pinyin_parsed(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open) {
        return 0;
    }
    const uint16 *start;
    const size_t n = syllables(&start);
    return start != nullptr ? start[n] : 0u;
}

extern "C" bool ime_pinyin_is_complete(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (!s_open) {
        return false;
    }
    const uint16 *start;
    const size_t n = syllables(&start);
    return n > 0u && im_get_fixed_len() >= n;
}

extern "C" void ime_pinyin_reset(void)
{
    ImeAllocScope heap; // engine allocations prefer PSRAM
    if (s_open) {
        im_reset_search();
    }
    s_cands = 0;
}
