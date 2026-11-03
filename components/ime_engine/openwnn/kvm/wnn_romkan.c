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
 * (see THIRD_PARTY_NOTICES.md). Deliberate differences from the Java original:
 *  - the table keeps only the keys made of 'a'..'z' and '-'; the punctuation and digit rows are
 *    dropped because wnn_romkan_push() accepts no such input, and so is the unreachable
 *    empty-string row of the original;
 *  - no upper case: the original upper-cases its output when the last typed letter was upper case,
 *    this port refuses upper case at the API boundary (ime_wnn.c);
 *  - the apostrophe is a syllable separator (wnn_romkan_separator()), not the full-width
 *    apostrophe row of the original table;
 *  - the segment list is a fixed array instead of ComposingText, and every segment carries the
 *    range of input letters it came from, so a caller can map kana positions back to letters.
 */
#include "wnn_romkan.h"

/* Longest key of the table, as MAX_LENGTH in the Java original. */
#define ROMKAN_MAX_KEY 4

typedef struct {
    char key[ROMKAN_MAX_KEY + 1];
    uint16_t out[2];
    uint8_t out_len;
} romkan_row_t;

/* Generated from the romkanTable of the Java original, sorted by key for the search below. */
static const romkan_row_t k_table[] = {
    { "-", { 0x30fc }, 1 },
    { "a", { 0x3042 }, 1 },
    { "ba", { 0x3070 }, 1 },
    { "bb", { 0x3063, 0x0062 }, 2 },
    { "be", { 0x3079 }, 1 },
    { "bi", { 0x3073 }, 1 },
    { "bo", { 0x307c }, 1 },
    { "bu", { 0x3076 }, 1 },
    { "bya", { 0x3073, 0x3083 }, 2 },
    { "bye", { 0x3073, 0x3047 }, 2 },
    { "byi", { 0x3073, 0x3043 }, 2 },
    { "byo", { 0x3073, 0x3087 }, 2 },
    { "byu", { 0x3073, 0x3085 }, 2 },
    { "ca", { 0x304b }, 1 },
    { "cc", { 0x3063, 0x0063 }, 2 },
    { "ce", { 0x305b }, 1 },
    { "cha", { 0x3061, 0x3083 }, 2 },
    { "che", { 0x3061, 0x3047 }, 2 },
    { "chi", { 0x3061 }, 1 },
    { "cho", { 0x3061, 0x3087 }, 2 },
    { "chu", { 0x3061, 0x3085 }, 2 },
    { "ci", { 0x3057 }, 1 },
    { "co", { 0x3053 }, 1 },
    { "cu", { 0x304f }, 1 },
    { "cya", { 0x3061, 0x3083 }, 2 },
    { "cye", { 0x3061, 0x3047 }, 2 },
    { "cyi", { 0x3061, 0x3043 }, 2 },
    { "cyo", { 0x3061, 0x3087 }, 2 },
    { "cyu", { 0x3061, 0x3085 }, 2 },
    { "da", { 0x3060 }, 1 },
    { "dd", { 0x3063, 0x0064 }, 2 },
    { "de", { 0x3067 }, 1 },
    { "dha", { 0x3067, 0x3083 }, 2 },
    { "dhe", { 0x3067, 0x3047 }, 2 },
    { "dhi", { 0x3067, 0x3043 }, 2 },
    { "dho", { 0x3067, 0x3087 }, 2 },
    { "dhu", { 0x3067, 0x3085 }, 2 },
    { "di", { 0x3062 }, 1 },
    { "do", { 0x3069 }, 1 },
    { "du", { 0x3065 }, 1 },
    { "dwa", { 0x3069, 0x3041 }, 2 },
    { "dwe", { 0x3069, 0x3047 }, 2 },
    { "dwi", { 0x3069, 0x3043 }, 2 },
    { "dwo", { 0x3069, 0x3049 }, 2 },
    { "dwu", { 0x3069, 0x3045 }, 2 },
    { "dya", { 0x3062, 0x3083 }, 2 },
    { "dye", { 0x3062, 0x3047 }, 2 },
    { "dyi", { 0x3062, 0x3043 }, 2 },
    { "dyo", { 0x3062, 0x3087 }, 2 },
    { "dyu", { 0x3062, 0x3085 }, 2 },
    { "e", { 0x3048 }, 1 },
    { "fa", { 0x3075, 0x3041 }, 2 },
    { "fe", { 0x3075, 0x3047 }, 2 },
    { "ff", { 0x3063, 0x0066 }, 2 },
    { "fi", { 0x3075, 0x3043 }, 2 },
    { "fo", { 0x3075, 0x3049 }, 2 },
    { "fu", { 0x3075 }, 1 },
    { "fwa", { 0x3075, 0x3041 }, 2 },
    { "fwe", { 0x3075, 0x3047 }, 2 },
    { "fwi", { 0x3075, 0x3043 }, 2 },
    { "fwo", { 0x3075, 0x3049 }, 2 },
    { "fwu", { 0x3075, 0x3045 }, 2 },
    { "fya", { 0x3075, 0x3083 }, 2 },
    { "fye", { 0x3075, 0x3047 }, 2 },
    { "fyi", { 0x3075, 0x3043 }, 2 },
    { "fyo", { 0x3075, 0x3087 }, 2 },
    { "fyu", { 0x3075, 0x3085 }, 2 },
    { "ga", { 0x304c }, 1 },
    { "ge", { 0x3052 }, 1 },
    { "gg", { 0x3063, 0x0067 }, 2 },
    { "gi", { 0x304e }, 1 },
    { "go", { 0x3054 }, 1 },
    { "gu", { 0x3050 }, 1 },
    { "gwa", { 0x3050, 0x3041 }, 2 },
    { "gwe", { 0x3050, 0x3047 }, 2 },
    { "gwi", { 0x3050, 0x3043 }, 2 },
    { "gwo", { 0x3050, 0x3049 }, 2 },
    { "gwu", { 0x3050, 0x3045 }, 2 },
    { "gya", { 0x304e, 0x3083 }, 2 },
    { "gye", { 0x304e, 0x3047 }, 2 },
    { "gyi", { 0x304e, 0x3043 }, 2 },
    { "gyo", { 0x304e, 0x3087 }, 2 },
    { "gyu", { 0x304e, 0x3085 }, 2 },
    { "ha", { 0x306f }, 1 },
    { "he", { 0x3078 }, 1 },
    { "hh", { 0x3063, 0x0068 }, 2 },
    { "hi", { 0x3072 }, 1 },
    { "ho", { 0x307b }, 1 },
    { "hu", { 0x3075 }, 1 },
    { "hya", { 0x3072, 0x3083 }, 2 },
    { "hye", { 0x3072, 0x3047 }, 2 },
    { "hyi", { 0x3072, 0x3043 }, 2 },
    { "hyo", { 0x3072, 0x3087 }, 2 },
    { "hyu", { 0x3072, 0x3085 }, 2 },
    { "i", { 0x3044 }, 1 },
    { "ja", { 0x3058, 0x3083 }, 2 },
    { "je", { 0x3058, 0x3047 }, 2 },
    { "ji", { 0x3058 }, 1 },
    { "jj", { 0x3063, 0x006a }, 2 },
    { "jo", { 0x3058, 0x3087 }, 2 },
    { "ju", { 0x3058, 0x3085 }, 2 },
    { "jya", { 0x3058, 0x3083 }, 2 },
    { "jye", { 0x3058, 0x3047 }, 2 },
    { "jyi", { 0x3058, 0x3043 }, 2 },
    { "jyo", { 0x3058, 0x3087 }, 2 },
    { "jyu", { 0x3058, 0x3085 }, 2 },
    { "ka", { 0x304b }, 1 },
    { "ke", { 0x3051 }, 1 },
    { "ki", { 0x304d }, 1 },
    { "kk", { 0x3063, 0x006b }, 2 },
    { "ko", { 0x3053 }, 1 },
    { "ku", { 0x304f }, 1 },
    { "kwa", { 0x304f, 0x3041 }, 2 },
    { "kya", { 0x304d, 0x3083 }, 2 },
    { "kye", { 0x304d, 0x3047 }, 2 },
    { "kyi", { 0x304d, 0x3043 }, 2 },
    { "kyo", { 0x304d, 0x3087 }, 2 },
    { "kyu", { 0x304d, 0x3085 }, 2 },
    { "la", { 0x3041 }, 1 },
    { "le", { 0x3047 }, 1 },
    { "li", { 0x3043 }, 1 },
    { "ll", { 0x3063, 0x006c }, 2 },
    { "lo", { 0x3049 }, 1 },
    { "ltsu", { 0x3063 }, 1 },
    { "ltu", { 0x3063 }, 1 },
    { "lu", { 0x3045 }, 1 },
    { "lwa", { 0x308e }, 1 },
    { "lya", { 0x3083 }, 1 },
    { "lye", { 0x3047 }, 1 },
    { "lyi", { 0x3043 }, 1 },
    { "lyo", { 0x3087 }, 1 },
    { "lyu", { 0x3085 }, 1 },
    { "ma", { 0x307e }, 1 },
    { "me", { 0x3081 }, 1 },
    { "mi", { 0x307f }, 1 },
    { "mm", { 0x3063, 0x006d }, 2 },
    { "mo", { 0x3082 }, 1 },
    { "mu", { 0x3080 }, 1 },
    { "mya", { 0x307f, 0x3083 }, 2 },
    { "mye", { 0x307f, 0x3047 }, 2 },
    { "myi", { 0x307f, 0x3043 }, 2 },
    { "myo", { 0x307f, 0x3087 }, 2 },
    { "myu", { 0x307f, 0x3085 }, 2 },
    { "na", { 0x306a }, 1 },
    { "nb", { 0x3093, 0x0062 }, 2 },
    { "nc", { 0x3093, 0x0063 }, 2 },
    { "nd", { 0x3093, 0x0064 }, 2 },
    { "ne", { 0x306d }, 1 },
    { "nf", { 0x3093, 0x0066 }, 2 },
    { "ng", { 0x3093, 0x0067 }, 2 },
    { "nh", { 0x3093, 0x0068 }, 2 },
    { "ni", { 0x306b }, 1 },
    { "nj", { 0x3093, 0x006a }, 2 },
    { "nk", { 0x3093, 0x006b }, 2 },
    { "nl", { 0x3093, 0x006c }, 2 },
    { "nm", { 0x3093, 0x006d }, 2 },
    { "nn", { 0x3093 }, 1 },
    { "no", { 0x306e }, 1 },
    { "np", { 0x3093, 0x0070 }, 2 },
    { "nq", { 0x3093, 0x0071 }, 2 },
    { "nr", { 0x3093, 0x0072 }, 2 },
    { "ns", { 0x3093, 0x0073 }, 2 },
    { "nt", { 0x3093, 0x0074 }, 2 },
    { "nu", { 0x306c }, 1 },
    { "nv", { 0x3093, 0x0076 }, 2 },
    { "nw", { 0x3093, 0x0077 }, 2 },
    { "nx", { 0x3093, 0x0078 }, 2 },
    { "nya", { 0x306b, 0x3083 }, 2 },
    { "nye", { 0x306b, 0x3047 }, 2 },
    { "nyi", { 0x306b, 0x3043 }, 2 },
    { "nyo", { 0x306b, 0x3087 }, 2 },
    { "nyu", { 0x306b, 0x3085 }, 2 },
    { "nz", { 0x3093, 0x007a }, 2 },
    { "o", { 0x304a }, 1 },
    { "pa", { 0x3071 }, 1 },
    { "pe", { 0x307a }, 1 },
    { "pi", { 0x3074 }, 1 },
    { "po", { 0x307d }, 1 },
    { "pp", { 0x3063, 0x0070 }, 2 },
    { "pu", { 0x3077 }, 1 },
    { "pya", { 0x3074, 0x3083 }, 2 },
    { "pye", { 0x3074, 0x3047 }, 2 },
    { "pyi", { 0x3074, 0x3043 }, 2 },
    { "pyo", { 0x3074, 0x3087 }, 2 },
    { "pyu", { 0x3074, 0x3085 }, 2 },
    { "qa", { 0x304f, 0x3041 }, 2 },
    { "qe", { 0x304f, 0x3047 }, 2 },
    { "qi", { 0x304f, 0x3043 }, 2 },
    { "qo", { 0x304f, 0x3049 }, 2 },
    { "qq", { 0x3063, 0x0071 }, 2 },
    { "qu", { 0x304f }, 1 },
    { "qwa", { 0x304f, 0x3041 }, 2 },
    { "qwe", { 0x304f, 0x3047 }, 2 },
    { "qwi", { 0x304f, 0x3043 }, 2 },
    { "qwo", { 0x304f, 0x3049 }, 2 },
    { "qwu", { 0x304f, 0x3045 }, 2 },
    { "qya", { 0x304f, 0x3083 }, 2 },
    { "qye", { 0x304f, 0x3047 }, 2 },
    { "qyi", { 0x304f, 0x3043 }, 2 },
    { "qyo", { 0x304f, 0x3087 }, 2 },
    { "qyu", { 0x304f, 0x3085 }, 2 },
    { "ra", { 0x3089 }, 1 },
    { "re", { 0x308c }, 1 },
    { "ri", { 0x308a }, 1 },
    { "ro", { 0x308d }, 1 },
    { "rr", { 0x3063, 0x0072 }, 2 },
    { "ru", { 0x308b }, 1 },
    { "rya", { 0x308a, 0x3083 }, 2 },
    { "rye", { 0x308a, 0x3047 }, 2 },
    { "ryi", { 0x308a, 0x3043 }, 2 },
    { "ryo", { 0x308a, 0x3087 }, 2 },
    { "ryu", { 0x308a, 0x3085 }, 2 },
    { "sa", { 0x3055 }, 1 },
    { "se", { 0x305b }, 1 },
    { "sha", { 0x3057, 0x3083 }, 2 },
    { "she", { 0x3057, 0x3047 }, 2 },
    { "shi", { 0x3057 }, 1 },
    { "sho", { 0x3057, 0x3087 }, 2 },
    { "shu", { 0x3057, 0x3085 }, 2 },
    { "si", { 0x3057 }, 1 },
    { "so", { 0x305d }, 1 },
    { "ss", { 0x3063, 0x0073 }, 2 },
    { "su", { 0x3059 }, 1 },
    { "swa", { 0x3059, 0x3041 }, 2 },
    { "swe", { 0x3059, 0x3047 }, 2 },
    { "swi", { 0x3059, 0x3043 }, 2 },
    { "swo", { 0x3059, 0x3049 }, 2 },
    { "swu", { 0x3059, 0x3045 }, 2 },
    { "sya", { 0x3057, 0x3083 }, 2 },
    { "sye", { 0x3057, 0x3047 }, 2 },
    { "syi", { 0x3057, 0x3043 }, 2 },
    { "syo", { 0x3057, 0x3087 }, 2 },
    { "syu", { 0x3057, 0x3085 }, 2 },
    { "ta", { 0x305f }, 1 },
    { "te", { 0x3066 }, 1 },
    { "tha", { 0x3066, 0x3083 }, 2 },
    { "the", { 0x3066, 0x3047 }, 2 },
    { "thi", { 0x3066, 0x3043 }, 2 },
    { "tho", { 0x3066, 0x3087 }, 2 },
    { "thu", { 0x3066, 0x3085 }, 2 },
    { "ti", { 0x3061 }, 1 },
    { "to", { 0x3068 }, 1 },
    { "tsa", { 0x3064, 0x3041 }, 2 },
    { "tse", { 0x3064, 0x3047 }, 2 },
    { "tsi", { 0x3064, 0x3043 }, 2 },
    { "tso", { 0x3064, 0x3049 }, 2 },
    { "tsu", { 0x3064 }, 1 },
    { "tt", { 0x3063, 0x0074 }, 2 },
    { "tu", { 0x3064 }, 1 },
    { "twa", { 0x3068, 0x3041 }, 2 },
    { "twe", { 0x3068, 0x3047 }, 2 },
    { "twi", { 0x3068, 0x3043 }, 2 },
    { "two", { 0x3068, 0x3049 }, 2 },
    { "twu", { 0x3068, 0x3045 }, 2 },
    { "tya", { 0x3061, 0x3083 }, 2 },
    { "tye", { 0x3061, 0x3047 }, 2 },
    { "tyi", { 0x3061, 0x3043 }, 2 },
    { "tyo", { 0x3061, 0x3087 }, 2 },
    { "tyu", { 0x3061, 0x3085 }, 2 },
    { "u", { 0x3046 }, 1 },
    { "va", { 0x30f4, 0x3041 }, 2 },
    { "ve", { 0x30f4, 0x3047 }, 2 },
    { "vi", { 0x30f4, 0x3043 }, 2 },
    { "vo", { 0x30f4, 0x3049 }, 2 },
    { "vu", { 0x30f4 }, 1 },
    { "vv", { 0x3063, 0x0076 }, 2 },
    { "vya", { 0x30f4, 0x3083 }, 2 },
    { "vye", { 0x30f4, 0x3047 }, 2 },
    { "vyi", { 0x30f4, 0x3043 }, 2 },
    { "vyo", { 0x30f4, 0x3087 }, 2 },
    { "vyu", { 0x30f4, 0x3085 }, 2 },
    { "wa", { 0x308f }, 1 },
    { "we", { 0x3046, 0x3047 }, 2 },
    { "wha", { 0x3046, 0x3041 }, 2 },
    { "whe", { 0x3046, 0x3047 }, 2 },
    { "whi", { 0x3046, 0x3043 }, 2 },
    { "who", { 0x3046, 0x3049 }, 2 },
    { "whu", { 0x3046 }, 1 },
    { "wi", { 0x3046, 0x3043 }, 2 },
    { "wo", { 0x3092 }, 1 },
    { "wu", { 0x3046 }, 1 },
    { "ww", { 0x3063, 0x0077 }, 2 },
    { "xa", { 0x3041 }, 1 },
    { "xe", { 0x3047 }, 1 },
    { "xi", { 0x3043 }, 1 },
    { "xn", { 0x3093 }, 1 },
    { "xo", { 0x3049 }, 1 },
    { "xtu", { 0x3063 }, 1 },
    { "xu", { 0x3045 }, 1 },
    { "xwa", { 0x308e }, 1 },
    { "xx", { 0x3063, 0x0078 }, 2 },
    { "xya", { 0x3083 }, 1 },
    { "xye", { 0x3047 }, 1 },
    { "xyi", { 0x3043 }, 1 },
    { "xyo", { 0x3087 }, 1 },
    { "xyu", { 0x3085 }, 1 },
    { "ya", { 0x3084 }, 1 },
    { "ye", { 0x3044, 0x3047 }, 2 },
    { "yi", { 0x3044 }, 1 },
    { "yo", { 0x3088 }, 1 },
    { "yu", { 0x3086 }, 1 },
    { "yy", { 0x3063, 0x0079 }, 2 },
    { "za", { 0x3056 }, 1 },
    { "ze", { 0x305c }, 1 },
    { "zi", { 0x3058 }, 1 },
    { "zo", { 0x305e }, 1 },
    { "zu", { 0x305a }, 1 },
    { "zya", { 0x3058, 0x3083 }, 2 },
    { "zye", { 0x3058, 0x3047 }, 2 },
    { "zyi", { 0x3058, 0x3043 }, 2 },
    { "zyo", { 0x3058, 0x3087 }, 2 },
    { "zyu", { 0x3058, 0x3085 }, 2 },
    { "zz", { 0x3063, 0x007a }, 2 },
};

#define ROMKAN_ROWS (sizeof(k_table) / sizeof(k_table[0]))

static int key_cmp(const char *a, const char *b)
{
    for (size_t i = 0;; i++) {
        const unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
        if (ca == 0) {
            return 0;
        }
    }
}

static const romkan_row_t *lookup(const char *key)
{
    size_t lo = 0, hi = ROMKAN_ROWS;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = key_cmp(k_table[mid].key, key);
        if (c == 0) {
            return &k_table[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

void wnn_romkan_reset(wnn_romkan_t *r)
{
    r->n = 0;
}

bool wnn_romkan_push(wnn_romkan_t *r, char letter, size_t letter_index)
{
    if (r->n + 1 >= WNN_ROMKAN_MAX_SEGS) {
        return false;
    }
    /* Append the typed letter as its own segment, as the Java original does through ComposingText,
     * then look for the longest matching suffix of the segment list. */
    wnn_romkan_seg_t *s = &r->seg[r->n++];
    s->unit = (uint16_t)(unsigned char)letter;
    s->from = (uint16_t)letter_index;
    s->to = (uint16_t)letter_index;
    s->kana = false;
    s->sealed = false;

    /* A sealed segment takes part in no match, and nothing to its left does either. */
    size_t min_start = 0;
    for (size_t i = 0; i + 1 < r->n; i++) {
        if (r->seg[i].sealed) {
            min_start = i + 1;
        }
    }
    size_t first = r->n > ROMKAN_MAX_KEY ? r->n - ROMKAN_MAX_KEY : 0;
    if (first < min_start) {
        first = min_start;
    }

    for (size_t start = first; start < r->n; start++) {
        char key[ROMKAN_MAX_KEY + 1];
        size_t k = 0;
        bool ascii = true;
        for (size_t i = start; i < r->n; i++) {
            const uint16_t u = r->seg[i].unit;
            if ((u >= 'a' && u <= 'z') || u == '-') {
                key[k++] = (char)u;
            } else {
                ascii = false; /* a kana segment: no key of the table holds one */
                break;
            }
        }
        if (!ascii) {
            continue;
        }
        key[k] = 0;
        const romkan_row_t *row = lookup(key);
        if (row == NULL) {
            continue;
        }
        /* Replace seg[start..n) with the match. A two-unit match becomes two segments: the Java
         * original gives the last unit a segment of its own so the next keystroke can merge with
         * it, which is how "kk" -> small-tsu + "k" and then "a" -> small-tsu + "ka" works. */
        const size_t from = r->seg[start].from;
        const size_t to = r->seg[r->n - 1].to;
        r->n = start;
        wnn_romkan_seg_t *o0 = &r->seg[r->n++];
        o0->unit = row->out[0];
        o0->from = (uint16_t)from;
        o0->kana = true;
        o0->sealed = false;
        if (row->out_len == 1) {
            o0->to = (uint16_t)to;
        } else {
            o0->to = (uint16_t)(to > from ? to - 1 : from);
            wnn_romkan_seg_t *o1 = &r->seg[r->n++];
            o1->unit = row->out[1];
            o1->from = (uint16_t)to;
            o1->to = (uint16_t)to;
            o1->kana = !(row->out[1] >= 'a' && row->out[1] <= 'z');
            o1->sealed = false;
        }
        return true;
    }
    return true;
}

void wnn_romkan_separator(wnn_romkan_t *r)
{
    if (r->n == 0) {
        return;
    }
    wnn_romkan_seg_t *last = &r->seg[r->n - 1];
    if (last->kana) {
        return;
    }
    if (last->unit == 'n' && (r->n == 1 || r->seg[r->n - 2].kana || r->seg[r->n - 2].sealed)) {
        /* The case the separator exists for: "n'a" is <n> + <a>, not "na". */
        last->unit = 0x3093; /* HIRAGANA LETTER N */
        last->kana = true;
        return;
    }
    /* Any other pending letter stays as typed, but nothing may merge across the separator. */
    last->sealed = true;
}

size_t wnn_romkan_kana_segs(const wnn_romkan_t *r)
{
    size_t i = r->n;
    while (i > 0 && !r->seg[i - 1].kana) {
        i--;
    }
    return i;
}

size_t wnn_romkan_parsed(const wnn_romkan_t *r)
{
    const size_t i = wnn_romkan_kana_segs(r);
    return i == 0 ? 0 : r->seg[i - 1].to + 1;
}
