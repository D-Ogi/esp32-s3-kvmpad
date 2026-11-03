/*
 * Copyright (C) 2008-2012  OMRON SOFTWARE Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * C translation by KVMpad of src/jp/co/omronsoft/openwnn/JAJP/OpenWnnClauseConverterJAJP.java,
 * the dictionary part of src/jp/co/omronsoft/openwnn/OpenWnnDictionaryImpl.java and
 * libs/libwnnDictionary/OpenWnnDictionaryImplJni.c of AOSP
 * platform/packages/inputmethods/OpenWnn at commit 5067ec8ec5a80ab12218c961f9b083fbccc71b93
 * (see THIRD_PARTY_NOTICES.md). The differences from the Java original are listed in
 * wnn_convert.c.
 */
#ifndef KVM_WNN_CONVERT_H
#define KVM_WNN_CONVERT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Longest reading a conversion accepts: MAX_INPUT_LENGTH of the Java original, NJ_MAX_LEN. */
#define WNN_MAX_KANA 50
/** Longest conversion result of one clause: NJ_MAX_RESULT_LEN. */
#define WNN_MAX_RESULT 50
/** Words held per conversion; a list that does not fit is kept short (see wnn_convert.c). */
#define WNN_MAX_WORDS 2048
/** UTF-16 units of dictionary candidate text held per conversion. */
#define WNN_STR_UNITS 8192
/** Words one cached list may hold. */
#define WNN_MAX_LIST 64

/**
 * A word, a clause or the head of a sentence. The reading is a range of the conversion input, never
 * a copy; the text is a dictionary string plus, for a clause, the ancillary word that follows it.
 */
typedef struct {
    uint16_t cand_off;    /* first UTF-16 unit of the stem text in the string arena */
    uint16_t fzk_off;     /* first unit of the ancillary text; used only when fzk_len > 0 */
    uint8_t cand_len;
    uint8_t fzk_len;
    uint8_t stroke_start; /* first kana of the reading, absolute in the conversion input */
    uint8_t stroke_len;
    int32_t freq;         /* the score of the Java original, which is a signed 32-bit int there */
    uint16_t pos_left;
    uint16_t pos_right;
} wnn_word_t;

typedef struct wnn_conv wnn_conv_t;

/** Dictionaries a conversion needs. The memory stays owned by the caller and is only read. */
typedef struct {
    const void *tankanji; /* engine index 4: single-kanji dictionary (dic_05 of WnnJpnDic.c) */
    size_t tankanji_len;
    const void *jiritsu;  /* engine index 5: independent words (dic_06) */
    size_t jiritsu_len;
    const void *fzk;      /* engine index 6: ancillary words (dic_07) */
    size_t fzk_len;
    const void *connect;  /* the rule dictionary / connection matrix (con_01) */
    size_t connect_len;
} wnn_dicts_t;

/** Bytes of work area one conversion context needs. */
size_t wnn_conv_sizeof(void);

/**
 * Initialize a context in `mem` (at least wnn_conv_sizeof() bytes, aligned for any type). Checks
 * every dictionary header against its length and refuses a set that does not match.
 */
bool wnn_conv_init(void *mem, const wnn_dicts_t *d, wnn_conv_t **out);

/** Set the reading to convert: `kana[0..len)` UTF-16 units, len <= WNN_MAX_KANA. */
bool wnn_conv_set_input(wnn_conv_t *c, const uint16_t *kana, size_t len);

/**
 * Best sentence for `kana[base..len)`: writes its clauses in order to `out[0..cap)` and returns
 * their count, 0 when there is nothing to convert. consecutiveClauseConvert() of the Java original
 * followed by walking WnnSentence.elements. Resets the per-conversion caches.
 */
size_t wnn_conv_sentence(wnn_conv_t *c, size_t base, wnn_word_t *out, size_t cap);

/**
 * Every single-clause conversion of `kana[start..start+len)`, best first, as convert() of the Java
 * original; returns the count written to `out[0..cap)`. Reuses the caches of the last
 * wnn_conv_sentence() call, so call it right after one.
 */
size_t wnn_conv_clause_all(wnn_conv_t *c, size_t start, size_t len, wnn_word_t *out, size_t cap);

/** Text of `w`: up to 2 * WNN_MAX_RESULT UTF-16 units written to `out`; returns the count. */
size_t wnn_conv_text(const wnn_conv_t *c, const wnn_word_t *w, uint16_t *out);

/** Katakana of `kana[start..start+len)`; returns the units written (0 when it does not convert). */
size_t wnn_conv_katakana(const wnn_conv_t *c, size_t start, size_t len, uint16_t *out);

/** True once a conversion filled an arena or a list (quality degrades, nothing becomes unsafe). */
bool wnn_conv_truncated(const wnn_conv_t *c);

#endif /* KVM_WNN_CONVERT_H */
