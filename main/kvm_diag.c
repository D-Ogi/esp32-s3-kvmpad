// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "kvm_diag.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "board_lcd.h"
#include "board_pins.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "net_status.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lcd_pattern.h"
#include "nvs.h"
#include "touch_input.h"
#include "usb_hid_device.h"

static const char *TAG = "diag";

static SemaphoreHandle_t s_disp_lock;
static StaticSemaphore_t s_disp_lock_buf;
static diag_screen_t s_screen = DIAG_SCREEN_CARD;
static int s_back = 0; // frame buffer index being shown

// Touch capture ring (filled by the display task, printed by the console after the capture).
#define CAPTURE_MAX 256
// Diagnostic buffer: PSRAM (.ext_ram.bss), internal RAM is kept for DMA, stacks and the HID hot path.
EXT_RAM_BSS_ATTR static kvm_touch_sample_t s_cap[CAPTURE_MAX];
static volatile uint32_t s_cap_len;
static volatile uint32_t s_cap_limit;
static volatile bool s_cap_on;
static volatile diag_touch_hook_t s_touch_hook; // TASK-060E: the touch bridge (one reader, one consumer)

#define DISP_LOCK()   xSemaphoreTake(s_disp_lock, portMAX_DELAY)
#define DISP_UNLOCK() xSemaphoreGive(s_disp_lock)

static lcd_canvas_t canvas(int index)
{
    lcd_canvas_t c = {.px = board_lcd_fb(index), .w = BOARD_LCD_H_RES, .h = BOARD_LCD_V_RES};
    return c;
}

static void draw_screen_locked(diag_screen_t screen, int index)
{
    lcd_canvas_t c = canvas(index);
    if (c.px == NULL) {
        return;
    }
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    char footer[96];
    snprintf(footer, sizeof(footer), "%dX%d FB=%u BOUNCE=%u LINES PCLK=%dMHZ NOMINAL=%" PRIu32 ".%03" PRIu32 "HZ",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES, st.cfg.num_fbs, st.cfg.bounce_lines, BOARD_LCD_PCLK_HZ / 1000000,
             board_lcd_nominal_mhz() / 1000, board_lcd_nominal_mhz() % 1000);
    switch (screen) {
    case DIAG_SCREEN_CARD: pat_test_card(&c, footer); break;
    case DIAG_SCREEN_TOUCH:
        pat_touch_screen(&c);
        pat_text(&c, 60, 8, 2, C_WHITE, "TOUCH DIAG - 5 POINTS, TRACK ID COLOURS");
        break;
    case DIAG_SCREEN_RED: pat_fill(&c, C_RED); break;
    case DIAG_SCREEN_GREEN: pat_fill(&c, C_GREEN); break;
    case DIAG_SCREEN_BLUE: pat_fill(&c, C_BLUE); break;
    case DIAG_SCREEN_WHITE: pat_fill(&c, C_WHITE); break;
    case DIAG_SCREEN_BLACK: pat_fill(&c, C_BLACK); break;
    case DIAG_SCREEN_BARS: pat_bars(&c, 0, c.h); break;
    case DIAG_SCREEN_RAMPS: pat_ramps(&c, 0, c.h); break;
    case DIAG_SCREEN_ALIGN:
        pat_fill(&c, C_BLACK);
        pat_alignment(&c);
        break;
    }
}

esp_err_t diag_show(diag_screen_t screen)
{
    ESP_RETURN_ON_FALSE(s_disp_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    DISP_LOCK();
    s_screen = screen;
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    int index = st.cfg.num_fbs == 2 ? (s_back ^ 1) : 0;
    draw_screen_locked(screen, index);
    esp_err_t err = board_lcd_present(index);
    s_back = index;
    DISP_UNLOCK();
    return err;
}

bool diag_screen_from_name(const char *name, diag_screen_t *out)
{
    static const struct {
        const char *n;
        diag_screen_t s;
    } k[] = {
        {"card", DIAG_SCREEN_CARD}, {"touch", DIAG_SCREEN_TOUCH}, {"red", DIAG_SCREEN_RED},
        {"green", DIAG_SCREEN_GREEN}, {"blue", DIAG_SCREEN_BLUE}, {"white", DIAG_SCREEN_WHITE},
        {"black", DIAG_SCREEN_BLACK}, {"bars", DIAG_SCREEN_BARS}, {"ramps", DIAG_SCREEN_RAMPS},
        {"align", DIAG_SCREEN_ALIGN},
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(name, k[i].n) == 0) {
            *out = k[i].s;
            return true;
        }
    }
    return false;
}

// Touch screen: trail squares in the colour of the track id + a status band. Drawing happens in
// this task only; touch_input never draws (one reader, one display owner).
static void draw_touch_locked(const kvm_touch_sample_t *s)
{
    lcd_canvas_t c = canvas(s_back);
    if (c.px == NULL) {
        return;
    }
    pat_rect(&c, 1, 30, c.w - 2, 20, RGB565(8, 8, 16));
    char line[128];
    int n = snprintf(line, sizeof(line), "#%" PRIu32 " %s N=%u", s->sample_seq, s->valid ? "OK" : "CANCEL",
                     s->point_count);
    for (uint8_t i = 0; i < s->point_count && n < (int)sizeof(line) - 20; i++) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, " ID%u:%d,%d", s->point[i].track_id, s->point[i].x_px,
                      s->point[i].y_px);
    }
    pat_text(&c, 8, 34, 2, s->valid ? C_WHITE : C_RED, line);
    for (uint8_t i = 0; i < s->point_count; i++) {
        pat_rect(&c, s->point[i].x_px - 3, s->point[i].y_px - 3, 7, 7, pat_track_color(s->point[i].track_id));
    }
    board_lcd_present(s_back);
}

static void display_task(void *arg)
{
    (void)arg;
    kvm_touch_sample_t s;
    for (;;) {
        if (!touch_input_receive(&s, portMAX_DELAY)) {
            continue;
        }
        diag_touch_hook_t hook = s_touch_hook;
        if (hook != NULL) {
            hook(&s);
        }
        if (s_cap_on && s_cap_len < s_cap_limit) {
            s_cap[s_cap_len] = s;
            s_cap_len = s_cap_len + 1;
        }
        if (s_screen == DIAG_SCREEN_TOUCH) {
            DISP_LOCK();
            if (s_screen == DIAG_SCREEN_TOUCH) {
                draw_touch_locked(&s);
            }
            DISP_UNLOCK();
        }
    }
}

void diag_set_touch_hook(diag_touch_hook_t hook)
{
    s_touch_hook = hook;
}

esp_err_t diag_start(void)
{
    s_disp_lock = xSemaphoreCreateMutexStatic(&s_disp_lock_buf);
    BaseType_t ok = xTaskCreatePinnedToCore(display_task, "diag_display", 2560, /* peak ~0.9 KiB, board #1 */ NULL, 5, NULL, tskNO_AFFINITY);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void diag_touch_capture(uint32_t ms, uint32_t max)
{
    if (max == 0 || max > CAPTURE_MAX) {
        max = CAPTURE_MAX;
    }
    s_cap_len = 0;
    s_cap_limit = max;
    s_cap_on = true;
    vTaskDelay(pdMS_TO_TICKS(ms));
    s_cap_on = false;
    vTaskDelay(pdMS_TO_TICKS(20));
    printf("touch capture: %" PRIu32 " samples in %" PRIu32 " ms\n", s_cap_len, ms);
    for (uint32_t i = 0; i < s_cap_len; i++) {
        const kvm_touch_sample_t *s = &s_cap[i];
        printf("TS seq=%" PRIu32 " t=%" PRIu64 " valid=%d n=%u", s->sample_seq, s->timestamp_us, s->valid,
               s->point_count);
        for (uint8_t k = 0; k < s->point_count; k++) {
            printf(" [id=%u x=%d y=%d s=%u]", s->point[k].track_id, s->point[k].x_px, s->point[k].y_px,
                   s->point[k].strength);
        }
        printf("\n");
    }
}

// ---- CPU load ----------------------------------------------------------------------------------
#define MAX_TASKS 40
EXT_RAM_BSS_ATTR static TaskStatus_t s_ts[MAX_TASKS]; // diagnostic, PSRAM

static bool idle_runtime(configRUN_TIME_COUNTER_TYPE idle[2], configRUN_TIME_COUNTER_TYPE *total)
{
    UBaseType_t n = uxTaskGetSystemState(s_ts, MAX_TASKS, total);
    if (n == 0) {
        return false;
    }
    idle[0] = idle[1] = 0;
    for (UBaseType_t i = 0; i < n; i++) {
        if (strcmp(s_ts[i].pcTaskName, "IDLE0") == 0) {
            idle[0] = s_ts[i].ulRunTimeCounter;
        } else if (strcmp(s_ts[i].pcTaskName, "IDLE1") == 0) {
            idle[1] = s_ts[i].ulRunTimeCounter;
        }
    }
    return true;
}

esp_err_t diag_cpu_sample(uint32_t ms, diag_cpu_t *out)
{
    configRUN_TIME_COUNTER_TYPE i0[2];
    configRUN_TIME_COUNTER_TYPE i1[2];
    configRUN_TIME_COUNTER_TYPE t0;
    configRUN_TIME_COUNTER_TYPE t1;
    ESP_RETURN_ON_FALSE(idle_runtime(i0, &t0), ESP_FAIL, TAG, "task state");
    vTaskDelay(pdMS_TO_TICKS(ms));
    ESP_RETURN_ON_FALSE(idle_runtime(i1, &t1), ESP_FAIL, TAG, "task state");
    configRUN_TIME_COUNTER_TYPE dt = t1 - t0;
    for (int c = 0; c < 2; c++) {
        configRUN_TIME_COUNTER_TYPE idle = i1[c] - i0[c];
        uint32_t idle_pm = dt ? (uint32_t)((idle * 1000ULL) / dt) : 0;
        out->cpu_busy_permille[c] = idle_pm > 1000 ? 0 : 1000 - idle_pm;
    }
    return ESP_OK;
}

// ---- LCD stats -----------------------------------------------------------------------------------
void diag_print_lcd_stats(const char *label)
{
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    uint32_t mean = st.vsync_count > 1 ? (uint32_t)(st.vsync_sum_us / (st.vsync_count - 1)) : 0;
    uint32_t mhz = mean ? (uint32_t)(1000000000ULL / mean) : 0;
    printf("LCD[%s] fbs=%u bounce=%u vsync=%" PRIu32 " mean=%" PRIu32 "us (%" PRIu32 ".%03" PRIu32
           " Hz) min=%" PRIu32 "us max=%" PRIu32 "us late=%" PRIu32 " frames_done=%" PRIu32 " missed=%" PRId32
           " phase=%" PRIu32 "us [%" PRIu32 "..%" PRIu32 "]\n",
           label, st.cfg.num_fbs, st.cfg.bounce_lines, st.vsync_count, mean, mhz / 1000, mhz % 1000,
           st.vsync_count > 1 ? st.vsync_min_us : 0, st.vsync_max_us, st.vsync_late, st.bounce_frames,
           // A frame whose bounce refill fell behind ends in a DMA restart at VSYNC (the 10-line jump) and never
           // reports "frame complete": missed = VSYNC count - completed frames (+-1 at the window edges).
           (int32_t)st.vsync_count - (int32_t)st.bounce_frames,
           st.phase_last_us, st.phase_min_us == UINT32_MAX ? 0 : st.phase_min_us, st.phase_max_us);
}

// ---- PSRAM load generator ----------------------------------------------------------------------------
typedef struct {
    volatile bool run;
    volatile uint64_t bytes;
    TaskHandle_t task;
} psram_load_t;

static void psram_load_task(void *arg)
{
    psram_load_t *ld = arg;
    const size_t sz = 256 * 1024;
    uint8_t *a = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    uint8_t *b = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    if (a && b) {
        memset(a, 0x5A, sz);
        // 16 KiB per step with a tick of sleep in between: heavy PSRAM traffic that still lets
        // the idle task run (one 512 KiB burst per tick starved IDLE0 into the task watchdog
        // while the bounce ISR competed for the same PSRAM).
        const size_t step = 16 * 1024;
        while (ld->run) {
            for (size_t off = 0; off < sz && ld->run; off += step) {
                memcpy(b + off, a + off, step);
                ld->bytes += step;
                vTaskDelay(1);
            }
        }
    }
    heap_caps_free(a);
    heap_caps_free(b);
    ld->task = NULL;
    vTaskDelete(NULL);
}

static void psram_load_start(psram_load_t *ld, int core)
{
    ld->run = true;
    ld->bytes = 0;
    xTaskCreatePinnedToCore(psram_load_task, "psram_load", 3072, ld, 2, &ld->task, core);
}

static void psram_load_stop(psram_load_t *ld)
{
    ld->run = false;
    while (ld->task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

esp_err_t diag_lcd_bench(uint32_t seconds)
{
    static const struct {
        const char *name;
        board_lcd_cfg_t cfg;
    } k_var[] = {
        {"A 1fb+2x10", {.num_fbs = 1, .bounce_lines = 10}},
        {"B 1fb+2x20", {.num_fbs = 1, .bounce_lines = 20}},
        {"C 2fb+2x10", {.num_fbs = 2, .bounce_lines = 10}},
    };
    if (seconds == 0) {
        seconds = 5;
    }
    diag_screen_t keep = s_screen;
    for (size_t v = 0; v < sizeof(k_var) / sizeof(k_var[0]); v++) {
        DISP_LOCK();
        board_lcd_deinit();
        esp_err_t err = board_lcd_init(&k_var[v].cfg);
        if (err == ESP_OK) {
            s_back = 0;
            for (int i = 0; i < k_var[v].cfg.num_fbs; i++) {
                draw_screen_locked(DIAG_SCREEN_CARD, i);
            }
            board_lcd_present(0);
        }
        DISP_UNLOCK();
        if (err != ESP_OK) {
            printf("BENCH %s: init failed %s\n", k_var[v].name, esp_err_to_name(err));
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        // Idle measurement, then the same with a PSRAM copy load on both cores.
        board_lcd_reset_stats();
        diag_cpu_t idle;
        diag_cpu_sample(seconds * 1000, &idle);
        board_lcd_stats_t st_idle;
        board_lcd_get_stats(&st_idle);
        diag_print_lcd_stats(k_var[v].name);

        psram_load_t ld0 = {0};
        psram_load_t ld1 = {0};
        board_lcd_reset_stats();
        int64_t t0 = esp_timer_get_time();
        psram_load_start(&ld0, 0);
        psram_load_start(&ld1, 1);
        diag_cpu_t load;
        diag_cpu_sample(seconds * 1000, &load);
        psram_load_stop(&ld0);
        psram_load_stop(&ld1);
        int64_t dt = esp_timer_get_time() - t0;
        char label[40];
        snprintf(label, sizeof(label), "%s+PSRAM", k_var[v].name);
        diag_print_lcd_stats(label);
        uint32_t mbps = dt > 0 ? (uint32_t)(((ld0.bytes + ld1.bytes) * 1000000ULL / (uint64_t)dt) >> 20) : 0;
        printf("BENCH %s: cpu_idle_scene=%" PRIu32 "/%" PRIu32 " permille, cpu_with_load=%" PRIu32 "/%" PRIu32
               " permille, psram_copy=%" PRIu32 " MiB/s, dma_internal_free=%u B\n",
               k_var[v].name, idle.cpu_busy_permille[0], idle.cpu_busy_permille[1], load.cpu_busy_permille[0],
               load.cpu_busy_permille[1], mbps, (unsigned)dma_free);
    }
    // Back to the default variant A.
    DISP_LOCK();
    board_lcd_deinit();
    const board_lcd_cfg_t def = BOARD_LCD_CFG_DEFAULT;
    esp_err_t err = board_lcd_init(&def);
    s_back = 0;
    DISP_UNLOCK();
    if (err == ESP_OK) {
        diag_show(keep);
    }
    return err;
}

esp_err_t diag_lcd_variant(uint8_t num_fbs, uint16_t bounce_lines)
{
    ESP_RETURN_ON_FALSE(s_disp_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    const board_lcd_cfg_t cfg = {.num_fbs = num_fbs, .bounce_lines = bounce_lines};
    DISP_LOCK();
    board_lcd_deinit();
    esp_err_t err = board_lcd_init(&cfg);
    if (err == ESP_OK) {
        for (int i = 0; i < num_fbs; i++) {
            draw_screen_locked(s_screen, i);
        }
        s_back = 0;
        err = board_lcd_present(0);
    }
    DISP_UNLOCK();
    return err;
}

esp_err_t diag_lcd_drawload(uint32_t hz, uint32_t seconds)
{
    ESP_RETURN_ON_FALSE(s_disp_lock != NULL && hz > 0 && hz <= 200 && seconds > 0, ESP_ERR_INVALID_ARG, TAG, "args");
    ESP_RETURN_ON_ERROR(diag_show(DIAG_SCREEN_TOUCH), TAG, "touch screen");
    // The same drawing as one touch sample on the touch screen: status band + text + 5 squares.
    kvm_touch_sample_t s = {.valid = true, .point_count = 5};
    const TickType_t period = pdMS_TO_TICKS(1000 / hz) > 0 ? pdMS_TO_TICKS(1000 / hz) : 1;
    uint32_t draws = 0;
    uint32_t draw_us_max = 0;
    board_lcd_reset_stats();
    int64_t end = esp_timer_get_time() + (int64_t)seconds * 1000000;
    TickType_t wake = xTaskGetTickCount();
    while (esp_timer_get_time() < end) {
        s.sample_seq = draws;
        for (uint8_t i = 0; i < 5; i++) {
            s.point[i].track_id = i;
            s.point[i].x_px = (int16_t)(100 + i * 180 + (int)(draws % 60));
            s.point[i].y_px = (int16_t)(120 + (int)((draws * (i + 1)) % 400));
        }
        int64_t t0 = esp_timer_get_time();
        DISP_LOCK();
        if (s_screen == DIAG_SCREEN_TOUCH) {
            draw_touch_locked(&s);
        }
        DISP_UNLOCK();
        uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        if (dt > draw_us_max) {
            draw_us_max = dt;
        }
        draws++;
        vTaskDelayUntil(&wake, period);
    }
    board_lcd_stats_t st;
    board_lcd_get_stats(&st);
    printf("DRAWLOAD fbs=%u bounce=%u hz=%" PRIu32 " s=%" PRIu32 " draws=%" PRIu32 " draw_us_max=%" PRIu32
           " vsync=%" PRIu32 " frames_done=%" PRIu32 " lost=%" PRId32 " phase=[%" PRIu32 "..%" PRIu32 "]\n",
           st.cfg.num_fbs, st.cfg.bounce_lines, hz, seconds, draws, draw_us_max, st.vsync_count, st.bounce_frames,
           (int32_t)st.vsync_count - (int32_t)st.bounce_frames, st.phase_min_us == UINT32_MAX ? 0 : st.phase_min_us,
           st.phase_max_us);
    return ESP_OK;
}

typedef struct {
    uint32_t us;
    volatile int done;
} glitch_t;

static void glitch_task(void *arg)
{
    glitch_t *g = arg;
    portMUX_TYPE local = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&local);
    esp_rom_delay_us(g->us);
    portEXIT_CRITICAL(&local);
    __atomic_add_fetch(&g->done, 1, __ATOMIC_SEQ_CST);
    vTaskDelete(NULL);
}

esp_err_t diag_lcd_glitch(uint32_t us)
{
    if (us == 0 || us > 5000) {
        return ESP_ERR_INVALID_ARG;
    }
    glitch_t g = {.us = us, .done = 0};
    for (int core = 0; core < 2; core++) {
        if (xTaskCreatePinnedToCore(glitch_task, "lcd_glitch", 2048, &g, configMAX_PRIORITIES - 2, NULL, core) !=
            pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    while (__atomic_load_n(&g.done, __ATOMIC_SEQ_CST) < 2) {
        vTaskDelay(1);
    }
    return ESP_OK;
}

// ---- Stress (TASK-020 tests: Wi-Fi load, flash write; spec 02 §1.2 item 6) ------------------------------
static bool s_wifi_ready;

static esp_err_t wifi_init_scan_only(void)
{
    if (net_status_running()) {
        return net_status_ensure_radio(); // ADR-015: net_status owns esp_wifi; borrow its radio for the load
    }
    if (s_wifi_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_wifi_ready = true;
    return ESP_OK;
}

typedef struct {
    volatile bool run;
    volatile uint32_t scans;
    volatile uint32_t aps;
    volatile uint32_t nvs_writes;
    volatile uint32_t flash_erases;
    volatile uint32_t errors;
    TaskHandle_t wifi_task;
    TaskHandle_t storage_task;
} stress_t;

static void wifi_task(void *arg)
{
    stress_t *s = arg;
    while (s->run) {
        if (esp_wifi_scan_start(NULL, true) == ESP_OK) {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            esp_wifi_clear_ap_list();
            s->scans++;
            s->aps = n;
        } else {
            s->errors++;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    s->wifi_task = NULL;
    vTaskDelete(NULL);
}

static void storage_task(void *arg)
{
    stress_t *s = arg;
    const esp_partition_t *spare =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, "spare");
    // Stress source pattern in PSRAM: esp_flash_write copies non-internal sources through its own bounce buffer.
    EXT_RAM_BSS_ATTR static uint8_t buf[4096];
    memset(buf, 0xA5, sizeof(buf));
    nvs_handle_t h = 0;
    if (nvs_open("kvm_stress", NVS_READWRITE, &h) != ESP_OK) {
        s->errors++;
    }
    uint32_t off = 0;
    while (s->run) {
        if (h != 0) {
            if (nvs_set_u32(h, "cnt", s->nvs_writes) == ESP_OK && nvs_commit(h) == ESP_OK) {
                s->nvs_writes++;
            } else {
                s->errors++;
            }
        }
        if (spare != NULL) {
            // 64 KiB block erase + 4 KiB write: the long flash operation of an OTA write (TECH-021).
            if (esp_partition_erase_range(spare, off, 0x10000) == ESP_OK &&
                esp_partition_write(spare, off, buf, sizeof(buf)) == ESP_OK) {
                s->flash_erases++;
            } else {
                s->errors++;
            }
            off = (off + 0x10000) % (spare->size - 0x10000);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (h != 0) {
        nvs_erase_key(h, "cnt");
        nvs_commit(h);
        nvs_close(h);
    }
    s->storage_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t diag_stress(uint32_t seconds, bool wifi)
{
    if (seconds == 0) {
        seconds = 30;
    }
    if (wifi) {
        ESP_RETURN_ON_ERROR(wifi_init_scan_only(), TAG, "wifi");
    }
    touch_input_stats_t t0;
    touch_input_stats_t t1;
    usb_hid_status_t u0;
    usb_hid_status_t u1;
    touch_input_get_stats(&t0);
    usb_hid_device_get_status(&u0);
    size_t heap_min0 = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    static stress_t s;
    memset(&s, 0, sizeof(s));
    s.run = true;
    psram_load_t ld = {0};
    board_lcd_reset_stats();
    if (wifi) {
        xTaskCreatePinnedToCore(wifi_task, "stress_wifi", 4096, &s, 3, &s.wifi_task, tskNO_AFFINITY);
    }
    xTaskCreatePinnedToCore(storage_task, "stress_flash", 4096, &s, 3, &s.storage_task, tskNO_AFFINITY);
    psram_load_start(&ld, 1);
    diag_cpu_t cpu;
    diag_cpu_sample(seconds * 1000, &cpu);
    s.run = false;
    psram_load_stop(&ld);
    while (s.wifi_task != NULL || s.storage_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (wifi) {
        net_status_release_radio(); // no-op when net_status is not running
    }
    diag_print_lcd_stats("STRESS");
    touch_input_get_stats(&t1);
    usb_hid_device_get_status(&u1);
    printf("STRESS %" PRIu32 " s: wifi_scans=%" PRIu32 " (last %" PRIu32 " APs) nvs_writes=%" PRIu32
           " flash_erase64k=%" PRIu32 " errors=%" PRIu32 " cpu=%" PRIu32 "/%" PRIu32 " permille\n",
           seconds, s.scans, s.aps, s.nvs_writes, s.flash_erases, s.errors, cpu.cpu_busy_permille[0],
           cpu.cpu_busy_permille[1]);
    printf("STRESS touch: reads=%" PRIu32 " i2c_errors=%" PRIu32 " gap_cancels=%" PRIu32 " max_read_us=%" PRIu32
           " | usb: state=%s mounts=%" PRIu32 "->%" PRIu32 " report_failed=%" PRIu32 " | internal_heap_min=%u B\n",
           t1.reads - t0.reads, t1.i2c_errors - t0.i2c_errors, t1.gap_cancels - t0.gap_cancels, t1.max_read_us,
           usb_hid_state_name(u1.state), u0.mounts, u1.mounts, u1.report_failed - u0.report_failed,
           (unsigned)(heap_min0 < heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
                          ? heap_min0
                          : heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    return ESP_OK;
}
