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
 * C translation by KVMpad of src/jp/co/omronsoft/openwnn/JAJP/Romkan.java of AOSP
 * platform/packages/inputmethods/OpenWnn at commit 5067ec8ec5a80ab12218c961f9b083fbccc71b93
 * (see THIRD_PARTY_NOTICES.md). The differences from the Java original are listed in
 * wnn_romkan.c.
 */
#ifndef KVM_WNN_ROMKAN_H
#define KVM_WNN_ROMKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One more than the longest input ime_wnn accepts: a letter can add two segments at a time. */
#define WNN_ROMKAN_MAX_SEGS 52

typedef struct {
    uint16_t unit;  /* one UTF-16 unit: kana, or the typed letter while the syllable is incomplete */
    uint16_t from;  /* first input letter this segment was made from */
    uint16_t to;    /* last input letter this segment was made from */
    bool kana;      /* false while the segment still holds a typed letter */
    bool sealed;    /* a separator follows: no later keystroke may merge across it */
} wnn_romkan_seg_t;

typedef struct {
    wnn_romkan_seg_t seg[WNN_ROMKAN_MAX_SEGS];
    size_t n;
} wnn_romkan_t;

void wnn_romkan_reset(wnn_romkan_t *r);

/** Feed one letter ('a'..'z' or '-') typed at input position `letter_index`. False when full. */
bool wnn_romkan_push(wnn_romkan_t *r, char letter, size_t letter_index);

/** Feed the syllable separator: a pending lone "n" becomes a kana, anything else is sealed. */
void wnn_romkan_separator(wnn_romkan_t *r);

/** Number of leading segments that hold kana (the segments a conversion may use). */
size_t wnn_romkan_kana_segs(const wnn_romkan_t *r);

/** Input letters turned into kana; the letters after them are still an incomplete syllable. */
size_t wnn_romkan_parsed(const wnn_romkan_t *r);

#endif /* KVM_WNN_ROMKAN_H */
