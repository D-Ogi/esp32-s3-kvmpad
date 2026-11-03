// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// `ime` console command: the TASK-072 footprint spike on the board (TECH-080/081). It mounts the ime_data image,
// opens the PinyinIME decoder and decodes fixed test vectors in a worker task, then reports time, heap (internal
// and PSRAM) and the worker's stack high-water mark. It never touches HID and prints no decoded text: only whether
// the top candidate equals the expected one.
#include "ime_bench.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ime_data_vfs.h"
#include "ime_glue.h"
#include "ime_pinyin.h"

#define BENCH_STACK 10240
#define DICT_PATH IME_DATA_MOUNT "/dict_pinyin.dat"

typedef struct {
    const char *input;
    uint16_t expect[8];
} vector_t;

// Top candidates of the pinned dictionary, checked on the host first (test/ime_pinyin).
static const vector_t k_vectors[] = {
    {"nihao", {0x4F60, 0x597D}},
    {"zhongguo", {0x4E2D, 0x56FD}},
    {"woaini", {0x6211, 0x7231, 0x4F60}},
    {"xiexie", {0x8C22, 0x8C22}},
    {"beijing", {0x5317, 0x4EAC}},
    {"jintiantianqihenhao", {0x4ECA, 0x5929, 0x5929, 0x6C14, 0x5F88, 0x597D}},
    {"zhongwenshurufa", {0x4E2D, 0x6587, 0x8F93, 0x5165, 0x6CD5}},
    {"nh", {0x4F60, 0x597D}},
    {"dianshi", {0x7535, 0x89C6}},
    {"shi", {0x662F}},
};
#define N_VECTORS (sizeof(k_vectors) / sizeof(k_vectors[0]))

typedef struct {
    bool opened;
    uint32_t open_us;
    size_t int_before, int_after_open, int_min;
    size_t ps_before, ps_after_open, ps_after_close, int_after_close;
    uint32_t pass, fail;
    uint32_t worst_us, total_us;
    uint32_t worst_prefix_us; // typing "jintiantianqihenhao" one letter at a time, re-searching each time
    uint32_t stack_free;
    TaskHandle_t caller;
} bench_t;

static bench_t s_b;

static size_t free_int(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static size_t free_ps(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

static bool top_matches(const uint16_t *expect)
{
    uint16_t c[16];
    const size_t n = ime_pinyin_candidate(0, c, 16);
    size_t m = 0;
    while (m < 8 && expect[m] != 0) {
        m++;
    }
    return n == m && memcmp(c, expect, m * sizeof(uint16_t)) == 0;
}

static void bench_task(void *arg)
{
    bench_t *b = arg;
    b->int_before = free_int();
    b->ps_before = free_ps();
    int64_t t = esp_timer_get_time();
    b->opened = ime_pinyin_open(DICT_PATH);
    b->open_us = (uint32_t)(esp_timer_get_time() - t);
    b->int_after_open = free_int();
    b->ps_after_open = free_ps();
    if (b->opened) {
        for (size_t i = 0; i < N_VECTORS; i++) {
            ime_pinyin_reset();
            t = esp_timer_get_time();
            const size_t n = ime_pinyin_search(k_vectors[i].input, strlen(k_vectors[i].input));
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t);
            b->total_us += us;
            b->worst_us = us > b->worst_us ? us : b->worst_us;
            if (n > 0 && top_matches(k_vectors[i].expect)) {
                b->pass++;
            } else {
                b->fail++;
            }
        }
        const char *typed = "jintiantianqihenhao";
        ime_pinyin_reset();
        for (size_t len = 1; len <= strlen(typed); len++) {
            t = esp_timer_get_time();
            (void)ime_pinyin_search(typed, len);
            const uint32_t us = (uint32_t)(esp_timer_get_time() - t);
            b->worst_prefix_us = us > b->worst_prefix_us ? us : b->worst_prefix_us;
        }
        ime_pinyin_close();
    }
    b->ps_after_close = free_ps();
    b->int_after_close = free_int();
    b->int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    b->stack_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    xTaskNotifyGive(b->caller);
    vTaskDelete(NULL);
}

static void print_status(void)
{
    ime_data_vfs_status_t st;
    ime_data_get_status(&st);
    printf("IME data partition=%s size=%u parse=%s mounted=%d files=%u verify_ms=%u opens=%u refused=%u reads=%u\n",
           st.partition_found ? "found" : "missing", (unsigned)st.partition_size, ime_data_status_name(st.parse),
           st.mounted, (unsigned)st.count, (unsigned)st.verify_ms, (unsigned)st.opens, (unsigned)st.open_refused,
           (unsigned)st.reads);
    printf("IME glue %s (ZH %s, JA %s)\n", ime_glue_state_name(ime_glue_state()), ime_glue_layer_ready(false) ? "ready" : "off",
           ime_glue_layer_ready(true) ? "ready" : "off");
    ime_glue_timing_t t;
    if (ime_glue_timing(&t)) {
        printf("IME glue timing start=%u mount=%u hanzi_font=%u zh=%u kanji_font=%u ja=%u total=%u ms\n",
               (unsigned)t.start_ms, (unsigned)t.mount_ms, (unsigned)t.hanzi_font_ms, (unsigned)t.zh_ms,
               (unsigned)t.kanji_font_ms, (unsigned)t.ja_ms, (unsigned)t.total_ms);
    }
    for (uint32_t i = 0; i < st.count; i++) {
        printf("IME file %.*s offset=0x%x size=%u sha256=%02x%02x%02x%02x... verified=%d\n", (int)IME_DATA_NAME_CAP,
               st.entry[i].name, (unsigned)st.entry[i].offset, (unsigned)st.entry[i].size, st.entry[i].sha256[0],
               st.entry[i].sha256[1], st.entry[i].sha256[2], st.entry[i].sha256[3], st.verified[i]);
    }
}

int ime_bench_console_cmd(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (strcmp(sub, "status") == 0) {
        print_status();
        return 0;
    }
    if (strcmp(sub, "mount") == 0 || strcmp(sub, "bench") == 0) {
        const esp_err_t err = ime_data_mount();
        printf("IME mount %s\n", esp_err_to_name(err));
        print_status();
        if (err != ESP_OK || strcmp(sub, "mount") == 0) {
            return err == ESP_OK ? 0 : 1;
        }
        if (ime_glue_owns_decoder()) {
            // One decoder per device: the KEYBOARD view owns it, a bench open/close would pull it away.
            printf("IME bench: the decoder belongs to the ZH layer (state %s); build with the UI off to bench it\n",
                   ime_glue_state_name(ime_glue_state()));
            return 1;
        }
        memset(&s_b, 0, sizeof(s_b));
        s_b.caller = xTaskGetCurrentTaskHandle();
        // Internal stack: the worker reads flash through esp_partition_read().
        if (xTaskCreatePinnedToCore(bench_task, "ime_bench", BENCH_STACK, &s_b, 2, NULL, 1) != pdPASS) {
            printf("IME bench: no memory for the worker\n");
            return 1;
        }
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000)) == 0) {
            printf("IME bench: timeout\n");
            return 1;
        }
        printf("IME pinyin open=%d open_ms=%u.%03u\n", s_b.opened, (unsigned)(s_b.open_us / 1000),
               (unsigned)(s_b.open_us % 1000));
        printf("IME pinyin heap internal %u -> %u -> %u after close (min ever %u), psram %u -> %u -> %u after close\n",
               (unsigned)s_b.int_before, (unsigned)s_b.int_after_open, (unsigned)s_b.int_after_close,
               (unsigned)s_b.int_min, (unsigned)s_b.ps_before, (unsigned)s_b.ps_after_open, (unsigned)s_b.ps_after_close);
        printf("IME pinyin vectors pass=%u fail=%u search_us worst=%u mean=%u prefix_worst_us=%u stack_free=%u\n",
               (unsigned)s_b.pass, (unsigned)s_b.fail, (unsigned)s_b.worst_us,
               (unsigned)(N_VECTORS ? s_b.total_us / N_VECTORS : 0), (unsigned)s_b.worst_prefix_us,
               (unsigned)s_b.stack_free);
        return s_b.opened && s_b.fail == 0 ? 0 : 1;
    }
    printf("usage: ime [status | mount | bench]\n");
    return 1;
}
