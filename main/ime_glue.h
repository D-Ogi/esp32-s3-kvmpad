// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// ime_glue (TASK-072, ADR-024 FR-746): brings the on-device decoders and their candidate fonts up off the boot
// path. A low-priority worker mounts ime_data (SHA-256 of every file), opens the PinyinIME system dictionary and
// maps the hanzi font in place (ZH), then maps the OpenWnn dictionaries and the kanji font (JA, ADR-033); the LVGL
// owner then installs the fonts into the theme and the decoders into the KEYBOARD view. After the hand-over the
// decoders are only ever called from the LVGL task. A layer whose files are missing or invalid simply stays disabled
// (fail closed) and does not take the other one down; nothing else depends on them.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ui_app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IME_GLUE_IDLE = 0,
    IME_GLUE_LOADING,
    IME_GLUE_READY,     // worker done, waiting for the LVGL owner
    IME_GLUE_INSTALLED, // font in the theme, decoder in the KEYBOARD view
    IME_GLUE_FAILED,
} ime_glue_state_t;

/** Start the worker once; later calls return the first result. */
esp_err_t ime_glue_start(void);

/** LVGL owner context, lock held: hand the font and the decoder to `app` once the worker is done. Cheap otherwise. */
void ime_glue_install(ui_app_t *app);

ime_glue_state_t ime_glue_state(void);
const char *ime_glue_state_name(ime_glue_state_t s);

/** True once the worker has claimed the decoder (the `ime bench` console command must then not touch it). */
bool ime_glue_owns_decoder(void);

/** Where the worker's time went (ms); `start_ms` is the time since boot when it started. */
typedef struct {
    uint32_t start_ms;
    uint32_t mount_ms;      // ime_data mount: SHA-256 of every file
    uint32_t hanzi_font_ms; // ui_font_blob_load() of the hanzi font (part of zh_ms)
    uint32_t zh_ms;         // hanzi font + PinyinIME dictionary
    uint32_t kanji_font_ms; // ui_font_blob_load() of the kanji font (part of ja_ms)
    uint32_t ja_ms;         // kanji font + OpenWnn open
    uint32_t total_ms;
} ime_glue_timing_t;

/** The worker's timing once it is done (READY, INSTALLED or FAILED after the mount); false before. */
bool ime_glue_timing(ime_glue_timing_t *out);

/** True when the worker brought the JA (`ja`) or ZH layer up (READY or INSTALLED). */
bool ime_glue_layer_ready(bool ja);

#ifdef __cplusplus
}
#endif
