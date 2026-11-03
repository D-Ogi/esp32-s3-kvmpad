// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_wnn: a C face for the vendored AOSP OpenWnn engine (ADR-024 FR-746, TECH-080): Japanese
// romaji -> hiragana -> kanji with clause-by-clause candidates.
//
// The semantics mirror `ime_pinyin.h` exactly, so the KEYBOARD view can drive both through one
// function table: candidate 0 is always the whole sentence, candidates 1.. are alternatives for the
// first clause that is not fixed yet, and a partial choice fixes that clause and reconverts the
// rest. One context per device, owned by one task (it is not reentrant). The work area is allocated
// at open and released at close; nothing the user typed is ever stored (AGENTS.md section 4): there
// is no user and no learning dictionary, so the engine learns nothing.
//
// The dictionaries are the Japanese dictionaries of OpenWnn, packed into the `ime_data` image by
// tools/ime/build_ime_data.py as one `wnn_ja.bin` container. On the device the caller copies it into
// PSRAM with ime_data_load() (ADR-034: no flash stays memory-mapped) and hands the pointers over with
// ime_wnn_dicts_from_blob(); the engine only reads them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Longest input the engine accepts, in letters: NJ_MAX_LEN of the engine (a kana never costs
 * fewer than one letter, so the reading can never be longer than the input). */
#define IME_WNN_MAX_INPUT 50u
/** Candidates one search reports at most (candidate 0 plus 31 alternatives). */
#define IME_WNN_MAX_CANDIDATES 32u
/** UTF-16 units one candidate holds at most, NUL excluded. */
#define IME_WNN_MAX_TEXT 128u

/** The four dictionaries the conversion needs; the memory stays owned by the caller. */
typedef struct {
    const void *tankanji; // single-kanji dictionary (dic_05 of the vendored WnnJpnDic.c)
    size_t tankanji_len;
    const void *jiritsu;  // independent words (dic_06)
    size_t jiritsu_len;
    const void *fzk;      // ancillary words (dic_07)
    size_t fzk_len;
    const void *connect;  // rule dictionary / connection matrix (con_01)
    size_t connect_len;
} ime_wnn_dicts_t;

/**
 * Layout of the `wnn_ja.bin` container in the ime_data image, little endian:
 *
 *   offset 0   "KVMWNNJA", u32 version (1), u32 count (4)
 *   offset 16  4 x { u32 offset, u32 size } in the order tankanji, jiritsu, fzk, connect
 *   offset 64  the four dictionaries, each at a 16-byte aligned offset inside the container
 */
#define IME_WNN_BLOB_MAGIC "KVMWNNJA"
#define IME_WNN_BLOB_VERSION 1u
#define IME_WNN_BLOB_COUNT 4u
#define IME_WNN_BLOB_HEADER 64u

/** Point `out` at the four dictionaries inside a mapped `wnn_ja.bin`. No copy, no allocation. */
bool ime_wnn_dicts_from_blob(const void *blob, size_t len, ime_wnn_dicts_t *out);

/** Allocate the work area and validate every dictionary header. False leaves nothing open. */
bool ime_wnn_open(const ime_wnn_dicts_t *d);
void ime_wnn_close(void);
bool ime_wnn_is_open(void);

/**
 * Convert `letters[0..len)`: 'a'..'z', '-' (the long vowel mark) and '\'' (a syllable separator, so
 * that "n'a" reads as two kana and not as "na"). Returns the number of candidates, 0 when the input
 * is refused or nothing converts. A search starts a fresh composition: nothing stays fixed.
 */
size_t ime_wnn_search(const char *letters, size_t len);

/** Candidate `i` as UTF-16 (BMP only), NUL-terminated in `out`; returns its length, 0 if none. */
size_t ime_wnn_candidate(size_t i, uint16_t *out, size_t cap);

/**
 * Choose candidate `i`; returns the new candidate count. `i == 0` commits the whole sentence and
 * leaves the composition complete; `i > 0` fixes the first clause with that candidate and converts
 * the rest of the reading again.
 */
size_t ime_wnn_choose(size_t i);

/** Undo the last choice; returns the new candidate count. */
size_t ime_wnn_cancel_last(void);

/** UTF-16 units fixed so far by choices (the prefix of candidate 0 that is already settled). */
size_t ime_wnn_fixed_len(void);

/** Input letters covered by the fixed clauses (the rest of the input is still being converted). */
size_t ime_wnn_consumed(void);

/** Input letters turned into kana; the letters after them are still an incomplete syllable. */
size_t ime_wnn_parsed(void);

/** True when every kana is fixed: candidate 0 is then the whole sentence to commit. */
bool ime_wnn_is_complete(void);

/** The reading that is not fixed yet, for display: kana plus any incomplete syllable as typed. */
size_t ime_wnn_preedit(uint16_t *out, size_t cap);

void ime_wnn_reset(void);

/** Bytes of work area one open context needs (allocated at open, released at close). */
size_t ime_wnn_work_size(void);

/** True when the last conversion hit an internal list or arena limit and may have missed a
 * candidate. Diagnostics only: the conversion result stays valid. */
bool ime_wnn_truncated(void);

#ifdef __cplusplus
}
#endif
