// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-072 / ADR-042: a worker mounts the verified flash image and opens one requested
// language at a time. The LVGL owner installs the candidate font and takes ownership of
// the decoder. Disabling the language detaches and closes its dictionary. Published
// font tables remain available to existing widget styles until reboot.
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

/** Reserve the singleton engines for the UI before the shell starts. */
void ime_glue_claim(void);

/** LVGL owner context. Load enabled modules off-thread and detach disabled dictionaries. */
void ime_glue_sync(ui_app_t *app, bool zh, bool ja, bool ko);

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
    uint32_t ko_ms;         // optional Hangul font
    uint32_t total_ms;
} ime_glue_timing_t;

/** Timing of the latest load, with the latest per-language durations. */
bool ime_glue_timing(ime_glue_timing_t *out);

/** True when the worker brought the JA (`ja`) or ZH layer up (READY or INSTALLED). */
bool ime_glue_layer_ready(bool ja);

#ifdef __cplusplus
}
#endif
