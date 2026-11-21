// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// A worker opens one configured language at a time. The LVGL owner attaches the result and
// closes dictionaries after detaching the layer. Published fonts outlive their styled widgets.

#include "ime_glue.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ime_data_vfs.h"
#include "ime_pinyin.h"
#include "ime_wnn.h"
#include "lvgl.h"
#include "ui_font_blob.h"
#include "ui_theme.h"

static const char *TAG = "ime_glue";

#define DICT_PATH IME_DATA_MOUNT "/dict_pinyin.dat"
#define HANZI_NAME "font_hanzi_24.bin"
#define KANJI_NAME "font_kanji_24.bin"
#define WNN_NAME "wnn_ja.bin"
// Internal stack: the worker reads flash through esp_partition_read(); the decoder load is about 6 KB deep.
#define WORKER_STACK 10240
#define WORKER_PRIO 1 // below every input and UI task

typedef struct {
    _Atomic int state;
    const char *label;
    bool (*load)(void);
    uint32_t *duration_ms;
} language_module_t;

static _Atomic bool s_claimed;
static void *s_ja_dictionary; // OpenWnn borrows the backing bytes until ime_wnn_close()
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

EXT_RAM_BSS_ATTR static ui_font_blob_t s_hanzi; // about 6 KB of descriptors; the tables in a PSRAM block (ADR-034)
EXT_RAM_BSS_ATTR static ui_font_blob_t s_kanji;
static ime_data_file_t s_hanzi_file; // the source of each streamed font (read by its glyph loader)
static ime_data_file_t s_kanji_file;
static ui_font_blob_t *s_hangul; // allocated only when the optional flash font is used
static ime_data_file_t s_hangul_file;
static ime_glue_timing_t s_time; // short diagnostic snapshots under s_mux

// ---- the pinyin decoder handed to the KEYBOARD view (LVGL task only after READY) --------------------------------
static size_t zh_search(void *ctx, const char *letters, size_t len)
{
    (void)ctx;
    return ime_pinyin_search(letters, len);
}

static size_t zh_candidate(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    (void)ctx;
    return ime_pinyin_candidate(i, out, cap);
}

static size_t zh_choose(void *ctx, size_t i)
{
    (void)ctx;
    return ime_pinyin_choose(i);
}

static size_t zh_cancel_last(void *ctx)
{
    (void)ctx;
    return ime_pinyin_cancel_last();
}

static size_t zh_fixed_len(void *ctx)
{
    (void)ctx;
    return ime_pinyin_fixed_len();
}

static size_t zh_consumed(void *ctx)
{
    (void)ctx;
    return ime_pinyin_consumed();
}

static size_t zh_parsed(void *ctx)
{
    (void)ctx;
    return ime_pinyin_parsed();
}

static bool zh_complete(void *ctx)
{
    (void)ctx;
    return ime_pinyin_is_complete();
}

static void zh_reset(void *ctx)
{
    (void)ctx;
    ime_pinyin_reset();
}

static const ui_kbd_conv_engine_t k_zh = {
    .ctx = NULL,
    .search = zh_search,
    .candidate = zh_candidate,
    .choose = zh_choose,
    .cancel_last = zh_cancel_last,
    .fixed_len = zh_fixed_len,
    .consumed = zh_consumed,
    .parsed = zh_parsed,
    .complete = zh_complete,
    .reset = zh_reset,
};

// ---- the OpenWnn decoder (the same contract; LVGL task only after READY) ----------------------------------------
static size_t ja_search(void *ctx, const char *letters, size_t len)
{
    (void)ctx;
    return ime_wnn_search(letters, len);
}

static size_t ja_candidate(void *ctx, size_t i, uint16_t *out, size_t cap)
{
    (void)ctx;
    return ime_wnn_candidate(i, out, cap);
}

static size_t ja_choose(void *ctx, size_t i)
{
    (void)ctx;
    return ime_wnn_choose(i);
}

static size_t ja_cancel_last(void *ctx)
{
    (void)ctx;
    return ime_wnn_cancel_last();
}

static size_t ja_fixed_len(void *ctx)
{
    (void)ctx;
    return ime_wnn_fixed_len();
}

static size_t ja_consumed(void *ctx)
{
    (void)ctx;
    return ime_wnn_consumed();
}

static size_t ja_parsed(void *ctx)
{
    (void)ctx;
    return ime_wnn_parsed();
}

static bool ja_complete(void *ctx)
{
    (void)ctx;
    return ime_wnn_is_complete();
}

static void ja_reset(void *ctx)
{
    (void)ctx;
    ime_wnn_reset();
}

// The view shows (and Enter commits) the reading of the parsed input, then the letters after `parsed` as typed.
// ime_wnn_preedit() appends that incomplete syllable itself, so it is cut here: kana are never ASCII and a parsed
// '-' reads as U+30FC, so the trailing ASCII units are exactly the letters the view adds.
static size_t ja_preedit(void *ctx, uint16_t *out, size_t cap)
{
    (void)ctx;
    size_t n = ime_wnn_preedit(out, cap);
    while (n > 0u && out[n - 1u] < 0x80u) {
        n--;
    }
    if (cap > 0u) {
        out[n] = 0;
    }
    return n;
}

static const ui_kbd_conv_engine_t k_ja = {
    .ctx = NULL,
    .search = ja_search,
    .candidate = ja_candidate,
    .choose = ja_choose,
    .cancel_last = ja_cancel_last,
    .fixed_len = ja_fixed_len,
    .consumed = ja_consumed,
    .parsed = ja_parsed,
    .complete = ja_complete,
    .reset = ja_reset,
    .preedit = ja_preedit,
};

// ---- the worker -------------------------------------------------------------------------------------------------
// The worker runs at priority 1 next to IDLE1 on core 1. Checking a compressed font walks every pixel (seconds), so it
// blocks for a tick every UI_FONT_BLOB_YIELD_GLYPHS glyphs and between stages: IDLE1 must run, or the task watchdog
// (CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1, 5 s) resets the chip - found on board #1 (ESP_RST_TASK_WDT).
static void yield_tick(void *ctx)
{
    (void)ctx;
    vTaskDelay(1);
}

// ADR-034: the fonts are streamed, never memory-mapped. Their tables are copied into PSRAM at load and each glyph
// bitmap is read from flash when LVGL draws it (LVGL owner, through the part's loader mutex).
static bool font_read(void *ctx, uint32_t off, void *dst, uint32_t n)
{
    return ime_data_file_read((const ime_data_file_t *)ctx, off, dst, n);
}

static void *font_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static bool load_font(const char *name, ui_font_blob_t *font, ime_data_file_t *file, const char *layer)
{
    if (ui_font_blob_font(font) != NULL) {
        return true; // a styled widget can still refer to the published font
    }
    const esp_err_t err = ime_data_file(name, file);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s; the %s layer stays off", name, esp_err_to_name(err), layer);
        return false;
    }
    const ui_font_blob_src_t src = {
        .read = font_read, .alloc = font_alloc, .free = heap_caps_free, .ctx = file, .len = file->size};
    const int64_t t0 = esp_timer_get_time();
    const ui_font_blob_status_t fs = ui_font_blob_open(font, &src, &lv_font_montserrat_24, yield_tick, NULL);
    const uint32_t elapsed_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    portENTER_CRITICAL(&s_mux);
    if (font == &s_kanji) s_time.kanji_font_ms = elapsed_ms;
    if (font == &s_hanzi) s_time.hanzi_font_ms = elapsed_ms;
    portEXIT_CRITICAL(&s_mux);

    if (fs != UI_FONT_BLOB_OK) {
        ESP_LOGW(TAG, "%s: %s; the %s layer stays off", name, ui_font_blob_status_name(fs), layer);
        return false;
    }
    ESP_LOGI(TAG, "%s: %u bytes of tables in PSRAM, glyphs read on demand", name, (unsigned)font->arena_len);
    return true;
}

static bool load_zh(void)
{
    const int64_t t0 = esp_timer_get_time();
    if (!load_font(HANZI_NAME, &s_hanzi, &s_hanzi_file, "ZH")) {
        return false;
    }
    if (!ime_pinyin_open(DICT_PATH)) {
        ESP_LOGW(TAG, "pinyin dictionary: open failed; the ZH layer stays off");
        return false;
    }
    ESP_LOGI(TAG, "pinyin ready in %u ms: %u glyphs in %u font part(s)",
             (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)s_hanzi.glyphs, (unsigned)s_hanzi.parts);
    return true;
}

static bool load_ja(void)
{
    const int64_t t0 = esp_timer_get_time();
    if (!load_font(KANJI_NAME, &s_kanji, &s_kanji_file, "JA")) {
        return false;
    }
    // ADR-034: the OpenWnn dictionaries are read in place by the engine, so they are copied into PSRAM (kept for the
    // lifetime of the layer; ~0.9 MB) instead of being memory-mapped
    void *blob = NULL;
    size_t len = 0;
    const esp_err_t err = ime_data_load(WNN_NAME, &blob, &len);
    ime_wnn_dicts_t d;
    if (err != ESP_OK || !ime_wnn_dicts_from_blob(blob, len, &d)) {
        ESP_LOGW(TAG, "%s: %s; the JA layer stays off", WNN_NAME, err != ESP_OK ? esp_err_to_name(err) : "bad container");
        heap_caps_free(blob);
        return false;
    }
    if (!ime_wnn_open(&d)) {
        ESP_LOGW(TAG, "OpenWnn: open failed (work area %u B); the JA layer stays off", (unsigned)ime_wnn_work_size());
        heap_caps_free(blob);
        return false;
    }
    s_ja_dictionary = blob;
    ESP_LOGI(TAG, "OpenWnn ready in %u ms: %u glyphs in %u font part(s), work area %u B",
             (unsigned)((esp_timer_get_time() - t0) / 1000), (unsigned)s_kanji.glyphs, (unsigned)s_kanji.parts,
             (unsigned)ime_wnn_work_size());
    return true;
}

static bool load_ko(void)
{
    ui_font_blob_t *font = font_alloc(sizeof(*font));
    if (!font) return false;
    memset(font, 0, sizeof(*font));
    if (!load_font("font_hangul_24.bin", font, &s_hangul_file, "KO")) {
        heap_caps_free(font);
        return false;
    }
    s_hangul = font;
    return true;
}

static language_module_t s_zh = {.label = "ZH", .load = load_zh, .duration_ms = &s_time.zh_ms};
static language_module_t s_ja = {.label = "JA", .load = load_ja, .duration_ms = &s_time.ja_ms};
static language_module_t s_ko = {.label = "KO", .load = load_ko, .duration_ms = &s_time.ko_ms};

static void worker(void *arg)
{
    language_module_t *module = arg;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t mounted = ime_data_mount();
    const int64_t opened = esp_timer_get_time();
    const bool ready = mounted == ESP_OK && module->load();
    const int64_t finished = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    s_time.start_ms = (uint32_t)(t0 / 1000);
    s_time.mount_ms = (uint32_t)((opened - t0) / 1000);
    *module->duration_ms = (uint32_t)((finished - opened) / 1000);
    s_time.total_ms = (uint32_t)((finished - t0) / 1000);
    portEXIT_CRITICAL(&s_mux);
    if (!ready) {
        ESP_LOGW(TAG, "%s language resources unavailable (mount: %s)",
                 module->label, esp_err_to_name(mounted));
    }
    atomic_store(&module->state, ready ? IME_GLUE_READY : IME_GLUE_FAILED);
    vTaskDelete(NULL);
}

void ime_glue_claim(void)
{
    atomic_store(&s_claimed, true);
}

static void detach(language_module_t *module, ui_app_t *app)
{
    const int state = atomic_load(&module->state);
    if (state == IME_GLUE_INSTALLED || state == IME_GLUE_READY) {
        if (module == &s_ja) {
            ui_app_set_ja_engine(app, NULL);
            ime_wnn_close();
            heap_caps_free(s_ja_dictionary);
            s_ja_dictionary = NULL;
        } else if (module == &s_zh) {
            ui_app_set_zh_engine(app, NULL);
            ime_pinyin_close();
        }
    }
    atomic_store(&module->state, IME_GLUE_IDLE);
}

static void attach(language_module_t *module, ui_app_t *app)
{
    int state = atomic_load(&module->state);
    if (module == &s_ko && state == IME_GLUE_IDLE && s_hangul) {
        state = IME_GLUE_READY;
    }
    if (state == IME_GLUE_READY) {
        if (module == &s_ko) {
            ui_theme_set_hangul_font(ui_font_blob_font(s_hangul));
            ui_view_keyboard_refresh_picker(&app->keyboard);
        } else if (module == &s_ja) {
            ui_theme_set_kanji_font(ui_font_blob_font(&s_kanji));
            ui_app_set_ja_engine(app, &k_ja);
        } else {
            ui_theme_set_hanzi_font(ui_font_blob_font(&s_hanzi));
            ui_app_set_zh_engine(app, &k_zh);
        }
        atomic_store(&module->state, IME_GLUE_INSTALLED);
    } else if (state == IME_GLUE_IDLE) {
        atomic_store(&module->state, IME_GLUE_LOADING);
        if (xTaskCreatePinnedToCore(worker, "ime_load", WORKER_STACK, module, WORKER_PRIO, NULL, 1) != pdPASS) {
            ESP_LOGW(TAG, "no worker stack for %s", module->label);
            atomic_store(&module->state, IME_GLUE_FAILED);
        }
    }
}

static void sync_resources(ui_app_t *app, bool zh, bool ja, bool ko)
{
    if (atomic_load(&s_zh.state) == IME_GLUE_LOADING ||
        atomic_load(&s_ja.state) == IME_GLUE_LOADING || atomic_load(&s_ko.state) == IME_GLUE_LOADING) {
        return;
    }
    // Release disabled dictionaries before requesting memory for another language.
    if (!zh && atomic_load(&s_zh.state) != IME_GLUE_IDLE) {
        detach(&s_zh, app);
    }
    if (!ja && atomic_load(&s_ja.state) != IME_GLUE_IDLE) {
        detach(&s_ja, app);
    }
    if (!ko && atomic_load(&s_ko.state) != IME_GLUE_IDLE) {
        detach(&s_ko, app);
    }
    if (zh) {
        attach(&s_zh, app);
    }
    if (ja && atomic_load(&s_zh.state) != IME_GLUE_LOADING) {
        attach(&s_ja, app);
    }
    if (ko && atomic_load(&s_zh.state) != IME_GLUE_LOADING && atomic_load(&s_ja.state) != IME_GLUE_LOADING) {
        if (!ui_theme_has_hangul_font()) {
            attach(&s_ko, app);
        } else if (s_hangul) {
            // The published font survives a settings toggle and can be used immediately.
            atomic_store(&s_ko.state, IME_GLUE_INSTALLED);
        }
    }
}

static ui_kbd_module_status_t picker_status(const language_module_t *module, bool requested)
{
    if (!requested) return UI_KBD_MODULE_DEFAULT;
    switch (atomic_load(&module->state)) {
    case IME_GLUE_FAILED:    return UI_KBD_MODULE_FAILED;
    case IME_GLUE_INSTALLED: return UI_KBD_MODULE_DEFAULT;
    default:                 return UI_KBD_MODULE_LOADING; // queued, running or awaiting attachment
    }
}

void ime_glue_sync(ui_app_t *app, bool zh, bool ja, bool ko)
{
    if (!app || !ime_glue_owns_decoder()) return;
    sync_resources(app, zh, ja, ko);
    ui_view_keyboard_set_module_status(&app->keyboard, UI_KBD_LANG_ZH, picker_status(&s_zh, zh));
    ui_view_keyboard_set_module_status(&app->keyboard, UI_KBD_LANG_JA, picker_status(&s_ja, ja));
    ui_view_keyboard_set_module_status(&app->keyboard, UI_KBD_LANG_KO,
                                       picker_status(&s_ko, ko && !ui_theme_has_hangul_font()));
}

ime_glue_state_t ime_glue_state(void)
{
    const int zh = atomic_load(&s_zh.state), ja = atomic_load(&s_ja.state);
    const int ko = atomic_load(&s_ko.state);
    if (zh == IME_GLUE_LOADING || ja == IME_GLUE_LOADING || ko == IME_GLUE_LOADING) {
        return IME_GLUE_LOADING;
    }
    if (zh == IME_GLUE_READY || ja == IME_GLUE_READY || ko == IME_GLUE_READY) {
        return IME_GLUE_READY;
    }
    if (zh == IME_GLUE_INSTALLED || ja == IME_GLUE_INSTALLED || ko == IME_GLUE_INSTALLED) {
        return IME_GLUE_INSTALLED;
    }
    return zh == IME_GLUE_FAILED || ja == IME_GLUE_FAILED || ko == IME_GLUE_FAILED ? IME_GLUE_FAILED : IME_GLUE_IDLE;
}

const char *ime_glue_state_name(ime_glue_state_t s)
{
    switch (s) {
    case IME_GLUE_IDLE:
        return "idle";
    case IME_GLUE_LOADING:
        return "loading";
    case IME_GLUE_READY:
        return "ready";
    case IME_GLUE_INSTALLED:
        return "installed";
    case IME_GLUE_FAILED:
        return "failed";
    }
    return "?";
}

bool ime_glue_owns_decoder(void)
{
    return atomic_load(&s_claimed);
}

bool ime_glue_timing(ime_glue_timing_t *out)
{
    const int st = ime_glue_state();
    if (out == NULL || st == IME_GLUE_IDLE || st == IME_GLUE_LOADING) {
        return false;
    }
    portENTER_CRITICAL(&s_mux);
    *out = s_time;
    portEXIT_CRITICAL(&s_mux);
    return true;
}

bool ime_glue_layer_ready(bool ja)
{
    const language_module_t *module = ja ? &s_ja : &s_zh;
    const int state = atomic_load(&module->state);
    return state == IME_GLUE_READY || state == IME_GLUE_INSTALLED;
}
