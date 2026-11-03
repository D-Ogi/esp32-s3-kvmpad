// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_pinyin: a C face for the vendored AOSP PinyinIME decoder (ADR-024 FR-746, TECH-080).
//
// One decoder per device, owned by one task (the engine keeps global state). The system dictionary is opened
// from a path (on the device `/ime/dict_pinyin.dat`, the read-only ime_data mount). The user dictionary is
// always disabled: it would persist what the user typed (AGENTS.md section 4), so nothing is learned or written.
// The engine's binary dictionary layout follows the 32-bit `size_t` of the ESP32-S3; a 64-bit host cannot load
// it (host tests are built for i686).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest pinyin input the decoder accepts (letters and apostrophes). */
#define IME_PINYIN_MAX_INPUT 26u

bool ime_pinyin_open(const char *sys_dict_path);
void ime_pinyin_close(void);
bool ime_pinyin_is_open(void);

/** Decode `letters[0..len)` (a..z and the syllable separator '\''); returns the number of candidates. */
size_t ime_pinyin_search(const char *letters, size_t len);
/** Candidate `i` as UTF-16 (BMP only), NUL-terminated in `out`; returns its length, 0 when there is none. */
size_t ime_pinyin_candidate(size_t i, uint16_t *out, size_t cap);
/** Choose candidate `i` (a partial choice keeps the rest of the input); returns the new candidate count. */
size_t ime_pinyin_choose(size_t i);
/** Characters fixed so far by partial choices. */
size_t ime_pinyin_fixed_len(void);
/** Undo the last choice; returns the new candidate count. */
size_t ime_pinyin_cancel_last(void);
/** Input letters covered by the fixed characters (the rest of the input is still being decoded). */
size_t ime_pinyin_consumed(void);
/** Input letters the decoder segmented into syllables (letters after them stay as typed). */
size_t ime_pinyin_parsed(void);
/**
 * True when every syllable is fixed: candidate 0 is then the whole sentence to commit. Candidate 0 is always the
 * whole sentence (the fixed characters first); candidates 1.. cover only the first position not yet fixed.
 */
bool ime_pinyin_is_complete(void);
void ime_pinyin_reset(void);

#ifdef __cplusplus
}
#endif
