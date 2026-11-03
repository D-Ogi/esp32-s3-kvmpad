// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// UART diagnostics console (TECH-060). Read-only towards the HOST: no command emits HID.
#include "kvm_console.h"

#include "app_config.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_hal.h"
#include "board_lcd.h"
#include "board_pins.h"
#include "io_ext.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kvm_diag.h"
#include "kvm_notice.h"
#include "linenoise/linenoise.h"
#include "net_status.h"
#include "touch_input.h"
#include "remote_glue.h"
#include "ime_bench.h"
#include "update_manager_esp.h"
#include "gesture_engine.h"
#include "hid_dispatch.h"
#include "hid_keymap.h"
#include "usb_hid_device.h"
#include "usb_hid_reports.h"
#if CONFIG_KVM_UI_PLATFORM
#include "ui_glue.h"
#include "ui_actions.h"
#include "ui_shell.h"
#include "ui_platform.h"
#include "ui_present.h"
#include "ui_touch_bridge.h"
#endif
#if CONFIG_KVM_UI_BENCH
#include "ui_bench.h"
#endif

static int cmd_info(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    const esp_app_desc_t *app = esp_app_get_description();
    printf("INFO chip=ESP32-S3 rev=v%d.%d cores=%d features=0x%" PRIx32 "\n", ci.revision / 100, ci.revision % 100,
           ci.cores, ci.features);
    printf("INFO flash=%" PRIu32 " B psram=%u B idf=%s app=%s ver=%s\n", flash, (unsigned)esp_psram_get_size(),
           esp_get_idf_version(), app->project_name, app->version);
    printf("INFO heap internal free=%u min=%u | psram free=%u min=%u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    printf("INFO reset_reason=%d boot_count=%" PRIu32 "\n", (int)esp_reset_reason(), kvm_boot_count());
    printf("INFO %s\n", kvm_notice());
    return 0;
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    uint8_t found[32];
    size_t n = 0;
    board_hal_i2c_scan(found, sizeof(found), &n);
    printf("I2C scan (SDA=%d SCL=%d %d Hz): %u device(s):", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
           BOARD_I2C_FREQ_HZ, (unsigned)n);
    for (size_t i = 0; i < n && i < sizeof(found); i++) {
        printf(" 0x%02X", found[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_i2cw(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: i2cw <addr7> <byte> [byte...]\n");
        return 1;
    }
    uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
    uint8_t data[8];
    size_t n = 0;
    for (int i = 2; i < argc && n < sizeof(data); i++) {
        data[n++] = (uint8_t)strtol(argv[i], NULL, 0);
    }
    esp_err_t err = board_hal_i2c_raw_write(addr, data, n);
    printf("I2CW 0x%02X len=%u -> %s\n", addr, (unsigned)n, esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_i2cr(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: i2cr <addr7> [len]\n");
        return 1;
    }
    uint8_t addr = (uint8_t)strtol(argv[1], NULL, 0);
    size_t n = argc >= 3 ? (size_t)atoi(argv[2]) : 1;
    uint8_t data[16] = {0};
    if (n == 0 || n > sizeof(data)) {
        n = 1;
    }
    esp_err_t err = board_hal_i2c_raw_read(addr, data, n);
    printf("I2CR 0x%02X ->", addr);
    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        printf(" 0x%02X", data[i]);
    }
    printf(" (%s)\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static int cmd_exio(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        int exio = atoi(argv[2]);
        int level = atoi(argv[3]);
        esp_err_t err = board_hal_exio_set(exio, level != 0);
        printf("EXIO%d=%d -> %s\n", exio, level, esp_err_to_name(err));
    }
    uint8_t sh = 0;
    uint8_t rd = 0;
    bool match = false;
    esp_err_t err = board_hal_exio_verify(&sh, &rd, &match);
    if (err != ESP_OK) {
        printf("EXIO read-back failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("EXIO shadow=0x%02X readback=0x%02X match=%d TP_RST=%d BL=%d LCD_RST=%d SD_CS=%d USB_SEL=%d (%s)\n", sh,
           rd, match, io_ext_level(rd, BOARD_EXIO_TP_RST), io_ext_level(rd, BOARD_EXIO_BL_EN),
           io_ext_level(rd, BOARD_EXIO_LCD_RST), io_ext_level(rd, BOARD_EXIO_SD_CS),
           io_ext_level(rd, BOARD_EXIO_USB_SEL), io_ext_level(rd, BOARD_EXIO_USB_SEL) ? "CAN" : "USB");
    board_hal_stats_t bs;
    board_hal_get_stats(&bs);
    printf("BOARD i2c_errors=%" PRIu32 " lock_timeouts=%" PRIu32 " bus_resets=%" PRIu32 " touch_resets=%" PRIu32 "\n",
           bs.i2c_errors, bs.i2c_lock_timeouts, bs.i2c_bus_resets, bs.touch_resets);
    return 0;
}

static void print_touch(void)
{
    touch_input_info_t ti;
    touch_input_stats_t ts;
    touch_input_get_info(&ti);
    touch_input_get_stats(&ts);
    printf("TOUCH addr=0x%02X product=\"%s\" fw=0x%04X cfg_ver=%u cfg_res=%ux%u touch_number=%u int_mode=%u\n",
           ti.i2c_addr, ti.product_id, ti.fw_version, ti.config_version, ti.cfg_x_max, ti.cfg_y_max,
           ti.cfg_touch_number, ti.cfg_int_mode);
    printf("TOUCH irq=%" PRIu32 " reads=%" PRIu32 " samples=%" PRIu32 " invalid=%" PRIu32 " i2c_errors=%" PRIu32
           " injected=%" PRIu32 " corrupt=%" PRIu32 " gap_cancels=%" PRIu32 " recoveries=%" PRIu32
           " overflows=%" PRIu32 " max_read_us=%" PRIu32 " max_points=%u\n",
           ts.irq_count, ts.reads, ts.samples, ts.invalid_samples, ts.i2c_errors, ts.injected_faults,
           ts.corrupt_frames, ts.gap_cancels, ts.recoveries, ts.queue_overflows, ts.max_read_us, ts.max_points_seen);
    kvm_touch_sample_t last;
    if (touch_input_get_last(&last)) {
        printf("TOUCH last seq=%" PRIu32 " valid=%d n=%u\n", last.sample_seq, last.valid, last.point_count);
    }
}

static int cmd_touch(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "capture") == 0) {
        uint32_t ms = argc >= 3 ? (uint32_t)atoi(argv[2]) : 5000;
        uint32_t max = argc >= 4 ? (uint32_t)atoi(argv[3]) : 256;
        diag_touch_capture(ms, max);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "fault") == 0) {
        touch_input_inject_faults((uint32_t)atoi(argv[2]));
        printf("TOUCH injecting %s read faults\n", argv[2]);
        return 0;
    }
    print_touch();
    return 0;
}

static bool lcd_owned_by_ui(void)
{
#if CONFIG_KVM_UI_PLATFORM
    if (ui_platform_is_running()) {
        printf("refused: LVGL (ui_platform) owns the panel; raw LCD commands are for the M1 image\n");
        return true;
    }
#endif
    return false;
}

static int cmd_lcd(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "stats") != 0 && strcmp(argv[1], "bl") != 0 && lcd_owned_by_ui()) {
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        uint32_t s = argc >= 3 ? (uint32_t)atoi(argv[2]) : 5;
        return diag_lcd_bench(s) == ESP_OK ? 0 : 1;
    }
    if (argc >= 4 && strcmp(argv[1], "variant") == 0) {
        esp_err_t err = diag_lcd_variant((uint8_t)atoi(argv[2]), (uint16_t)atoi(argv[3]));
        printf("LCD variant fbs=%s bounce=%s -> %s\n", argv[2], argv[3], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 4 && strcmp(argv[1], "drawload") == 0) {
        return diag_lcd_drawload((uint32_t)atoi(argv[2]), (uint32_t)atoi(argv[3])) == ESP_OK ? 0 : 1;
    }
    if (argc >= 3 && strcmp(argv[1], "glitch") == 0) {
        int n = argc >= 4 ? atoi(argv[3]) : 1;
        for (int i = 0; i < n; i++) {
            esp_err_t err = diag_lcd_glitch((uint32_t)atoi(argv[2]));
            if (err != ESP_OK) {
                printf("glitch -> %s\n", esp_err_to_name(err));
                return 1;
            }
            vTaskDelay(pdMS_TO_TICKS(37));
        }
        printf("glitch %s us x%d done\n", argv[2], n);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        diag_print_lcd_stats("now");
        if (argc >= 3 && strcmp(argv[2], "reset") == 0) {
            board_lcd_reset_stats();
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "bl") == 0 && argc >= 3) {
        esp_err_t err = board_hal_backlight(atoi(argv[2]) != 0);
        printf("backlight -> %s\n", esp_err_to_name(err));
        return 0;
    }
    diag_screen_t sc;
    if (argc >= 2 && diag_screen_from_name(argv[1], &sc)) {
        esp_err_t err = diag_show(sc);
        printf("LCD screen %s -> %s\n", argv[1], esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    printf("usage: lcd <card|touch|red|green|blue|white|black|bars|ramps|align> | stats [reset] | bench [s] | variant <fbs> <lines> | drawload <hz> <s> | glitch <us> [n] | bl <0|1>\n");
    return 1;
}

static int cmd_stress(int argc, char **argv)
{
    uint32_t s = argc >= 2 ? (uint32_t)atoi(argv[1]) : 30;
    bool wifi = !(argc >= 3 && strcmp(argv[2], "nowifi") == 0);
    return diag_stress(s, wifi) == ESP_OK ? 0 : 1;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    usb_hid_status_t u;
    usb_hid_device_get_status(&u);
    printf("USB state=%s epoch=%" PRIu32 " mounts=%" PRIu32 " umounts=%" PRIu32 " suspends=%" PRIu32
           " resumes=%" PRIu32 "\n",
           usb_hid_state_name(u.state), u.usb_epoch, u.mounts, u.umounts, u.suspends, u.resumes);
    printf("USB protocol kbd=%s mouse=%s consumer=%s set_protocol=%" PRIu32 " led=0x%02X led_reports=%" PRIu32 "\n",
           u.protocol[0] ? "REPORT" : "BOOT", u.protocol[1] ? "REPORT" : "BOOT", u.protocol[2] ? "REPORT" : "BOOT",
           u.set_protocol_count, u.led_state, u.led_reports);
    printf("USB neutral_sent=%" PRIu32 " neutral_failed=%" PRIu32 " complete=%" PRIu32 " failed=%" PRIu32
           " get_report=%" PRIu32 " remote_wakeup_enabled=%d\n",
           u.neutral_sent, u.neutral_failed, u.report_complete, u.report_failed, u.get_report_requests,
           u.remote_wakeup_enabled);
    printf("USB sent=%" PRIu32 " refused=%" PRIu32 " wrong_context=%" PRIu32 " pumps=%" PRIu32
           " pump_requests=%" PRIu32 "\n",
           u.reports_sent, u.reports_refused, u.send_wrong_context, u.pumps, u.pump_requests);
    return 0;
}

// ---- gesture engine replay (TASK-040 on-target proof) ------------------------------------------
// Runs the normative vectors VEC-001..004 (03 §8) through a private engine instance with a
// synthetic clock and prints the intents it would emit. Nothing reaches hid_dispatch or USB.
typedef struct {
    kvm_gesture_t g;
    uint32_t seq;
    uint64_t now_us;
    int events;
} replay_t;

static const char *intent_name(kvm_hid_action_type_t t)
{
    switch (t) {
    case KVM_HID_MOUSE_MOVE: return "MOVE";
    case KVM_HID_MOUSE_BTN_DOWN: return "BTN_DOWN";
    case KVM_HID_MOUSE_BTN_UP: return "BTN_UP";
    case KVM_HID_MOUSE_WHEEL: return "WHEEL";
    default: return "?";
    }
}

static void replay_print(replay_t *r, const kvm_gesture_out_t *o, uint32_t t_ms)
{
    for (int i = 0; i < o->n; i++) {
        const kvm_hid_intent_t *in = &o->ev[i];
        r->events++;
        if (in->type == KVM_HID_MOUSE_MOVE) {
            printf("    %4" PRIu32 " ms  %s dx=%d dy=%d\n", t_ms, intent_name(in->type), in->data.mouse_move.dx,
                   in->data.mouse_move.dy);
        } else if (in->type == KVM_HID_MOUSE_WHEEL) {
            printf("    %4" PRIu32 " ms  %s h=%d v=%d\n", t_ms, intent_name(in->type), in->data.wheel.horizontal,
                   in->data.wheel.vertical);
        } else {
            printf("    %4" PRIu32 " ms  %s button=%u\n", t_ms, intent_name(in->type), in->data.mouse_button.button_id);
        }
    }
}

// Sample at t_ms with up to 2 points given as (id, x, y); id < 0 ends the list. Ticks every 10 ms in between.
static void replay_sample(replay_t *r, uint32_t t_ms, int id0, int x0, int y0, int id1, int x1, int y1)
{
    kvm_gesture_out_t o;
    uint64_t ts = 1000000u + (uint64_t)t_ms * 1000u;
    while (r->now_us + 10000 < ts) {
        r->now_us += 10000;
        kvm_gesture_tick(&r->g, r->now_us, &o);
        replay_print(r, &o, (uint32_t)((r->now_us - 1000000u) / 1000u));
    }
    kvm_touch_sample_t s = {.timestamp_us = ts, .sample_seq = ++r->seq, .valid = true, .point_count = 0};
    if (id0 >= 0) {
        s.point[s.point_count++] = (kvm_touch_point_t){.track_id = (uint8_t)id0, .x_px = (int16_t)x0, .y_px = (int16_t)y0};
    }
    if (id1 >= 0) {
        s.point[s.point_count++] = (kvm_touch_point_t){.track_id = (uint8_t)id1, .x_px = (int16_t)x1, .y_px = (int16_t)y1};
    }
    kvm_gesture_feed(&r->g, &s, &o);
    replay_print(r, &o, t_ms);
    r->now_us = ts;
}

static int cmd_gesture(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "replay") != 0) {
        kvm_gesture_cfg_t c = KVM_GESTURE_CFG_DEFAULT;
        printf("gesture engine (TASK-040): thresholds move>%u tap<=%u long<=%u two<=%u scroll>%u px; tap %" PRIu32
               " long %" PRIu32 " join %" PRIu32 " two-tap %" PRIu32 " dtd %" PRIu32 " gap %" PRIu32 " ms; drag-lock idle %" PRIu32
               " s\n",
               c.move_start_px, c.tap_max_move_px, c.long_press_max_move_px, c.two_max_move_px, c.scroll_start_px,
               c.tap_max_us / 1000, c.long_press_us / 1000, c.two_join_us / 1000, c.two_tap_max_us / 1000,
               c.dtd_window_us / 1000, c.gap_cancel_us / 1000, c.drag_lock_idle_us / 1000000);
        printf("usage: gesture replay   (VEC-001..004 through a private instance; no HID is emitted)\n");
        return 0;
    }
    static replay_t r; // ~0.5 KiB; static keeps it off the console task stack
    const struct {
        const char *name;
        const char *expect;
    } vec[4] = {
        {"VEC-001 one finger tap", "LEFT DOWN, LEFT UP; no MOVE/RIGHT/WHEEL"},
        {"VEC-002 two-finger scroll, staggered", "WHEEL only; no MOVE, no click"},
        {"VEC-003 track ids swap order", "coherent scroll, no jump"},
        {"VEC-004 two fingers then one", "no click on the last release"},
    };
    int total = 0;
    for (int v = 0; v < 4; v++) {
        memset(&r, 0, sizeof(r));
        kvm_gesture_init(&r.g, NULL);
        r.now_us = 1000000u;
        printf("  %s (expected: %s)\n", vec[v].name, vec[v].expect);
        switch (v) {
        case 0:
            replay_sample(&r, 0, 1, 400, 200, -1, 0, 0);
            replay_sample(&r, 70, 1, 402, 201, -1, 0, 0);
            replay_sample(&r, 130, -1, 0, 0, -1, 0, 0);
            break;
        case 1:
            replay_sample(&r, 0, 1, 200, 180, -1, 0, 0);
            replay_sample(&r, 45, 1, 200, 180, 9, 400, 190);
            replay_sample(&r, 75, 1, 200, 201, 9, 400, 211);
            replay_sample(&r, 95, 1, 200, 225, 9, 400, 235);
            replay_sample(&r, 140, -1, 0, 0, -1, 0, 0);
            break;
        case 2:
            replay_sample(&r, 0, 2, 100, 100, 9, 300, 100);
            replay_sample(&r, 15, 9, 301, 110, 2, 101, 110);
            replay_sample(&r, 30, 2, 101, 125, 9, 301, 125);
            replay_sample(&r, 45, -1, 0, 0, -1, 0, 0);
            break;
        default:
            for (uint32_t t = 0; t <= 100; t += 20) {
                replay_sample(&r, t, 2, 200, (int)(200 + t), 5, 300, (int)(200 + t));
            }
            replay_sample(&r, 120, 2, 200, 300, -1, 0, 0);
            replay_sample(&r, 180, -1, 0, 0, -1, 0, 0);
            break;
        }
        replay_sample(&r, 1500, -1, 0, 0, -1, 0, 0); // let every timer run
        printf("    -> %d intents, state %s, left_held=%d, taps=%" PRIu32 " right=%" PRIu32 " scrolls=%" PRIu32
               " moves=%" PRIu32 " cancels=%" PRIu32 "\n",
               r.events, kvm_gesture_state_name(r.g.state), kvm_gesture_left_held(&r.g), r.g.st.taps,
               r.g.st.right_clicks, r.g.st.scrolls, r.g.st.moves, r.g.st.cancels);
        total += r.events;
    }
    printf("GESTURE_REPLAY_DONE intents=%d (none sent to HID)\n", total);
    return 0;
}

static const char *owner_name(kvm_owner_kind_t k)
{
    return k == OWNER_LOCAL ? "LOCAL" : k == OWNER_REMOTE ? "REMOTE" : "NONE";
}

// `hid`: authoritative HID core state. `hid release` orders RELEASE_ALL, which only ever sends
// neutral reports. There is deliberately no console command that presses a key or moves the pointer.
#if CONFIG_KVM_UI_BENCH
// ---- TASK-060A: LVGL platform metrics, benchmark scenes, screen snapshot ------------------------
static void print_ui_metrics(const char *scene, uint32_t seconds, const diag_cpu_t *cpu)
{
    ui_platform_metrics_t m;
    ui_platform_metrics_get(&m);
    uint32_t fps = ui_platform_fps_x100();
    printf("UI_BENCH backend=\"%s\" mode=\"%s\" scene=%s s=%" PRIu32 " fps=%" PRIu32 ".%02" PRIu32 " renders=%" PRIu32
           " refr=%" PRIu32 " flushes=%" PRIu32 " render_avg_us=%" PRIu32 " render_max_us=%" PRIu32
           " flush_avg_us=%" PRIu32 " flush_max_us=%" PRIu32 " cycle_max_us=%" PRIu32 " adapter_fps=%" PRIu32
           " cpu0=%" PRIu32 " cpu1=%" PRIu32 " heap_int=%u psram=%u lvgl_hwm=%" PRIu32 "\n",
           ui_platform_backend_name(), ui_platform_mode_name(), scene, seconds, fps / 100, fps % 100, m.renders,
           m.refr_cycles, m.flushes, m.render_avg_us, m.render_max_us, m.flush_avg_us, m.flush_max_us,
           m.cycle_max_us, m.adapter_fps, cpu ? cpu->cpu_busy_permille[0] : 0, cpu ? cpu->cpu_busy_permille[1] : 0,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           m.lvgl_task_stack_hwm);
}

static void snap_row(const uint16_t *row, uint16_t w, uint16_t y, void *ctx)
{
    uint32_t *crc = ctx;
    EXT_RAM_BSS_ATTR static char line[1024 * 4 + 1]; // snapshot text row, PSRAM
    size_t n = 0;
    for (uint16_t x = 0; x < w && n + 4 < sizeof(line); x++) {
        uint16_t px = row[x];
        *crc = (*crc * 16777619u) ^ px; // FNV-1a style running hash, checked by test/hil/ui_snapshot.py
        n += (size_t)snprintf(line + n, sizeof(line) - n, "%04X", px);
    }
    printf("SNAP_ROW %u %s\n", y, line);
}

static int cmd_ui(int argc, char **argv)
{
    if (!ui_platform_is_running()) {
        printf("ui_platform not running\n");
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        ui_bench_scene_t sc;
        if (!ui_bench_scene_from_name(argv[2], &sc)) {
            printf("usage: ui bench <idle|small|full|scroll> [s]\n");
            return 1;
        }
        uint32_t s = argc >= 4 ? (uint32_t)atoi(argv[3]) : 10;
        if (s < 1 || s > 120) {
            s = 10;
        }
        ui_shell_suspend();
        esp_err_t err = ui_bench_start(sc);
        ui_glue_refresh_pad();
        if (err != ESP_OK) {
            printf("ui_bench_start: %s\n", esp_err_to_name(err));
            esp_err_t rerr = ui_shell_show(); // a refused scene must not leave the panel without the shell
            if (rerr != ESP_OK) {
                printf("ui_shell_show: %s\n", esp_err_to_name(rerr));
            }
            return 1;
        }
        vTaskDelay(pdMS_TO_TICKS(500)); // let the screen settle before the window starts
        ui_platform_metrics_reset();
        diag_cpu_t cpu = {0};
        diag_cpu_sample(s * 1000, &cpu);
        print_ui_metrics(ui_bench_scene_name(sc), s, &cpu);
        ui_bench_stop();
        // A timed bench is a measurement, not a screen change: give the panel back to the product shell, as
        // `ui demo` does. Only `ui scene` keeps a bench scene on screen, on purpose.
        esp_err_t rerr = ui_shell_show();
        if (rerr != ESP_OK) {
            printf("ui_shell_show: %s\n", esp_err_to_name(rerr));
        }
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "demo") == 0) {
        ui_shell_suspend();
        ui_touch_bridge_set_pad(NULL, false); // the LVGL demo has no pad
        esp_err_t err = ui_bench_demo_start();
        if (err != ESP_OK) {
            printf("ui_bench_demo_start: %s\n", esp_err_to_name(err));
            return 1;
        }
        ui_bench_demo_result_t r;
        uint32_t waited = 0;
        do {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
            ui_bench_demo_result(&r);
        } while (!r.done && waited < 600);
        if (!r.done) {
            printf("UI_LVDEMO timeout after %" PRIu32 " s\n", waited);
            return 1;
        }
        printf("UI_LVDEMO backend=\"%s\" mode=\"%s\" scenes=%" PRId32 " fps_avg=%" PRId32 " cpu_avg=%" PRId32
               " render_avg_ms=%" PRId32 " flush_avg_ms=%" PRId32 " duration_s=%" PRIu32 "\n",
               ui_platform_backend_name(), ui_platform_mode_name(), r.scenes, r.fps_avg, r.cpu_avg, r.render_avg_ms,
               r.flush_avg_ms, waited);
        // The LVGL demo deletes its own objects at the end and leaves an empty light-theme screen
        // (white on the panel); put the product shell back.
        esp_err_t rerr = ui_shell_show();
        if (rerr != ESP_OK) {
            printf("ui_shell_show: %s\n", esp_err_to_name(rerr));
        }
        return 0;
    }
    if (argc >= 2 && (strcmp(argv[1], "nav") == 0 || strcmp(argv[1], "open") == 0 || strcmp(argv[1], "close") == 0)) {
        // Local shell actions only (navigation, details cards): the same gateway the touch chrome uses, never HID.
        ui_action_type_t t = UI_ACT_DISMISS_OVERLAY;
        uint16_t v = 0;
        bool ok = true;
        if (strcmp(argv[1], "nav") == 0) {
            t = UI_ACT_NAVIGATE;
            const char *n = argc >= 3 ? argv[2] : "";
            ok = strcmp(n, "mouse") == 0 || strcmp(n, "keyboard") == 0 || strcmp(n, "shortcuts") == 0;
            v = strcmp(n, "keyboard") == 0 ? UI_VIEW_KEYBOARD : strcmp(n, "shortcuts") == 0 ? UI_VIEW_SHORTCUTS
                                                                                            : UI_VIEW_MOUSE;
        } else if (strcmp(argv[1], "open") == 0) {
            const char *n = argc >= 3 ? argv[2] : "";
            ok = strcmp(n, "host") == 0 || strcmp(n, "client") == 0 || strcmp(n, "settings") == 0;
            t = strcmp(n, "client") == 0     ? UI_ACT_OPEN_CLIENT_DETAILS
                : strcmp(n, "settings") == 0 ? UI_ACT_OPEN_SETTINGS
                                             : UI_ACT_OPEN_HOST_DETAILS;
        }
        if (!ok) {
            printf("usage: ui nav <mouse|keyboard|shortcuts> | ui open <host|client|settings> | ui close\n");
            return 1;
        }
        esp_err_t err = ui_shell_local_action(t, v);
        printf("UI_LOCAL %s -> %s\n", ui_action_name(t), esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "shell") == 0) {
        // `ui shell`: back to the product shell; `ui shell status`: counters only, no screen change.
        if (argc < 3 || strcmp(argv[2], "status") != 0) {
            esp_err_t err = ui_shell_show();
            printf("shell -> %s\n", esp_err_to_name(err));
        }
        ui_shell_counters_t sc;
        ui_shell_status(&sc);
        printf("UI_SHELL active=%s views=%u shows=%" PRIu32 " refused=%" PRIu32 " created=%" PRIu32 " cancels=%" PRIu32
               " cancel_refused=%" PRIu32 " nav=%" PRIu32 " details=%" PRIu32 " local_refused=%" PRIu32 "\n",
               ui_shell_active() ? "yes" : "no", sc.views, sc.router.shows, sc.router.show_refused,
               sc.router.views_created, sc.router.cancel_requests, sc.router.cancel_refused, sc.app.nav_requests,
               sc.app.details_requests, sc.app.local_refused);
        printf("UI_OVERLAY shown=%" PRIu32 " dismissed=%" PRIu32 " refused=%" PRIu32 " details_open=%" PRIu32
               " details_close=%" PRIu32 "\n",
               sc.router.overlays_shown, sc.router.overlays_dismissed, sc.router.overlay_refused, sc.details_opens,
               sc.details_closes);
        printf("UI_MOUSE button_down=%" PRIu32 " button_up=%" PRIu32 " refused=%" PRIu32 " drag_requests=%" PRIu32
               " forced_releases=%" PRIu32 "\n",
               sc.mouse.button_down, sc.mouse.button_up, sc.mouse.refused, sc.mouse.drag_requests,
               sc.mouse.forced_releases);
        printf("UI_KEYBOARD key_down=%" PRIu32 " key_up=%" PRIu32 " mod_toggles=%" PRIu32 " fn_switches=%" PRIu32
               " refused=%" PRIu32 " leave_releases=%" PRIu32 "\n",
               sc.keyboard.key_down, sc.keyboard.key_up, sc.keyboard.mod_toggles, sc.keyboard.fn_switches,
               sc.keyboard.refused, sc.keyboard.leave_releases);
        printf("UI_SHORTCUTS requests=%" PRIu32 " refused=%" PRIu32 " blocked=%" PRIu32 " confirms_opened=%" PRIu32
               " confirms_taken=%" PRIu32 " confirms_closed=%" PRIu32 "\n",
               sc.shortcuts.requests, sc.shortcuts.refused, sc.shortcuts.blocked, sc.shortcuts.confirms_opened,
               sc.shortcuts.confirms_taken, sc.shortcuts.confirms_closed);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "scene") == 0) {
        ui_bench_scene_t sc = UI_BENCH_IDLE;
        if (argc >= 3 && !ui_bench_scene_from_name(argv[2], &sc)) {
            printf("usage: ui scene <idle|small|full|scroll>\n");
            return 1;
        }
        ui_shell_suspend(); // screen change: no press, drag or button may survive it (AGENTS-UI §6)
        esp_err_t err = ui_bench_start(sc);
        ui_glue_refresh_pad();
        printf("scene %s -> %s\n", ui_bench_scene_name(sc), esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "touch") == 0) {
        if (!ui_touch_bridge_running()) {
            printf("ui touch bridge not running\n");
            return 1;
        }
        ui_touch_bridge_stats_t s;
        ui_touch_bridge_get_stats(&s);
        printf("UI_TOUCH posted=%" PRIu32 " queue_full=%" PRIu32 " samples=%" PRIu32 " invalid=%" PRIu32
               " chrome_downs=%" PRIu32 " pad_downs=%" PRIu32 " blocked_downs=%" PRIu32 " ptr_cancels=%" PRIu32
               " cancels=%" PRIu32 " indev_reads=%" PRIu32 " indev_resets=%" PRIu32 "\n",
               s.posted, s.queue_full, s.router.samples, s.router.invalid, s.router.chrome_downs, s.router.pad_downs,
               s.router.blocked_downs, s.router.pointer_cancels, s.router.cancels, s.indev_reads, s.indev_resets);
        printf("UI_TOUCH_GESTURE hid=%s state=%d left_held=%s drag_lock=%s samples=%" PRIu32 " taps=%" PRIu32
               " right=%" PRIu32 " drags=%" PRIu32 " scrolls=%" PRIu32 " moves=%" PRIu32 " cancels=%" PRIu32
               " intents=%" PRIu32 " refused=%" PRIu32 " overflow=%" PRIu32 "\n",
               s.hid_enabled ? "on" : "off", (int)s.gesture_state, s.left_held ? "yes" : "no",
               s.drag_lock ? "on" : "off", s.gesture.samples, s.gesture.taps, s.gesture.right_clicks, s.gesture.drags,
               s.gesture.scrolls, s.gesture.moves, s.gesture.cancels, s.intents, s.intents_refused,
               s.gesture.out_overflow);
        printf("UI_TOUCH_PAD enabled=%s x=%d..%d y=%d..%d\n", s.pad_enabled ? "yes" : "no", s.pad.x1, s.pad.x2,
               s.pad.y1, s.pad.y2);
        return 0;
    }
#if CONFIG_KVM_UI_BENCH
    if (argc >= 4 && strcmp(argv[1], "tap") == 0) {
        // HIL bring-up: a synthetic one-finger tap through the same bridge path as GT911 samples (router ->
        // LVGL pointer). Refused while pad -> HID is on or the product shell is shown (its buttons submit HID
        // actions), so it can never reach the HOST.
        ui_touch_bridge_stats_t s;
        ui_touch_bridge_get_stats(&s);
        if (!ui_touch_bridge_running() || s.hid_enabled || ui_shell_active()) {
            printf("ui tap: needs a bench scene (`ui scene idle`) with pad -> HID off\n");
            return 1;
        }
        const long x = strtol(argv[2], NULL, 0);
        const long y = strtol(argv[3], NULL, 0);
        if (x < 0 || x >= BOARD_TOUCH_H_RES || y < 0 || y >= BOARD_TOUCH_V_RES) {
            printf("ui tap: x 0..%d, y 0..%d\n", BOARD_TOUCH_H_RES - 1, BOARD_TOUCH_V_RES - 1);
            return 1;
        }
        const uint32_t hold_ms = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 0) : 120;
        const uint32_t before = ui_bench_clicks(NULL, 0);
        kvm_touch_sample_t t;
        memset(&t, 0, sizeof(t));
        t.valid = true;
        t.point_count = 1;
        t.point[0].track_id = 0;
        t.point[0].x_px = (int16_t)x;
        t.point[0].y_px = (int16_t)y;
        t.timestamp_us = (uint64_t)esp_timer_get_time();
        t.sample_seq = 0x80000000u; // diagnostic sequence space, far from the GT911 sampler's
        ui_touch_bridge_post(&t);
        vTaskDelay(pdMS_TO_TICKS(hold_ms > 2000 ? 2000 : hold_ms));
        t.point_count = 0;
        t.timestamp_us = (uint64_t)esp_timer_get_time();
        t.sample_seq++;
        ui_touch_bridge_post(&t);
        vTaskDelay(pdMS_TO_TICKS(150));
        char last[24];
        const uint32_t after = ui_bench_clicks(last, sizeof(last));
        ui_touch_bridge_get_stats(&s);
        printf("UI_TAP x=%ld y=%ld hold_ms=%" PRIu32 " clicks=%" PRIu32 " last=\"%s\" chrome_downs=%" PRIu32
               " pad_downs=%" PRIu32 " blocked_downs=%" PRIu32 "\n",
               x, y, hold_ms, after - before, last, s.router.chrome_downs, s.router.pad_downs, s.router.blocked_downs);
        return 0;
    }
#endif
    if (argc >= 3 && (strcmp(argv[1], "pad") == 0 || strcmp(argv[1], "drag") == 0)) {
        const bool on = strcmp(argv[2], "on") == 0;
        if (!on && strcmp(argv[2], "off") != 0) {
            printf("usage: ui pad on|off | ui drag on|off\n");
            return 1;
        }
        if (argv[1][0] == 'p') {
            ui_touch_bridge_set_hid_enabled(on); // off: the gesture FSM is cancelled, every button released
        } else {
            ui_touch_bridge_set_drag_lock(on);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        printf("ui %s %s\n", argv[1], on ? "on" : "off");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "snap") == 0) {
        uint16_t div = argc >= 3 ? (uint16_t)atoi(argv[2]) : 4;
        uint32_t crc = 2166136261u;
        uint16_t w = 0;
        uint16_t h = 0;
        printf("SNAP_BEGIN div=%u\n", div);
        esp_err_t err = ui_platform_snapshot(div, snap_row, &crc, &w, &h);
        printf("SNAP_END w=%u h=%u crc=%08" PRIX32 " err=%s\n", w, h, crc, esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        ui_platform_metrics_reset();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "store") == 0) {
        if (!ui_glue_running()) {
            printf("ui_core store not running\n");
            return 1;
        }
        ui_state_snapshot_t a;
        ui_store_counters_t sc;
        ui_mailbox_counters_t mc;
        ui_actions_counters_t ac;
        memset(&a, 0, sizeof(a));
        ui_glue_status(&a, &sc, &mc, &ac);
        const bool hid = a.sources & UI_SRC_HID;
        const bool net = a.sources & UI_SRC_NET;
        const bool tch = a.sources & UI_SRC_TOUCH;
        const bool gst = a.sources & UI_SRC_GESTURE;
        printf("UI_STORE version=%" PRIu32 " sources=0x%" PRIX32 " host=\"%s\" owner=\"%s\" epoch=%" PRIu32
               " modifiers=0x%02X caps=%s maintenance=%s client=\"%s\" wifi=%s touch=%s drag_lock=%s\n",
               a.version, a.sources, ui_present_host(hid ? a.host_usb_state : -1),
               ui_present_owner(hid ? a.owner_kind : -1), a.owner_epoch, a.modifiers_down,
               !hid || !a.caps_led_known ? "unknown" : (a.caps_led_active ? "on" : "off"),
               hid ? (a.maintenance ? "on" : "off") : "unknown", ui_present_client(net ? a.client_state : -1),
               net ? (a.wifi_connected ? "yes" : "no") : "unknown", tch ? (a.touch_healthy ? "ok" : "fault") : "unknown",
               gst ? (a.drag_lock_active ? "on" : "off") : "unknown");
        printf("UI_STORE_COUNTERS ticks=%" PRIu32 " applied=%" PRIu32 " resyncs=%" PRIu32 " events=%" PRIu32
               " mb_published=%" PRIu32 " mb_applied=%" PRIu32 " mb_unchanged=%" PRIu32 " mb_stale=%" PRIu32
               " mb_invalid=%" PRIu32 " mb_takes=%" PRIu32 " ev_lost=%" PRIu32 " actions=%" PRIu32 "\n",
               sc.ticks, sc.applied, sc.resyncs, sc.events, mc.published, mc.applied, mc.unchanged, mc.stale,
               mc.invalid, mc.takes, mc.events_lost, ac.submitted);
        size_t lc = 0, lb = 0, lf = 0;
        ui_platform_lv_heap(&lc, &lb, &lf);
        printf("UI_LV_HEAP live_blocks=%u live_bytes=%u internal_fallbacks=%u heap_int=%u heap_int_min=%u psram=%u\n",
               (unsigned)lc, (unsigned)lb, (unsigned)lf,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return 0;
    }
    print_ui_metrics("current", 0, NULL);
    printf("usage: ui [bench <idle|small|full|scroll> [s] | demo | scene <name> | snap [div] | reset | store |"
           " touch | tap x y [ms] | pad on|off | drag on|off | shell [status] | nav <view> | open <host|client> |"
           " close]\n");
    return 0;
}
#endif

static int cmd_wifi(int argc, char **argv)
{
    // TASK-080 / ADR-015: read-only status and a scan. There is deliberately no "join" command: a password typed
    // here would end up in captured console logs. Networks are saved from the local Settings screen (TASK-070C).
    if (!net_status_running()) {
        printf("net_status not running\n");
        return 1;
    }
    if (argc >= 2 && strcmp(argv[1], "scan") == 0) {
        static net_scan_ap_t aps[16];
        uint16_t found = 0;
        const int64_t t0 = esp_timer_get_time();
        esp_err_t err = net_status_scan(aps, 16, &found);
        const int64_t ms = (esp_timer_get_time() - t0) / 1000;
        printf("WIFI_SCAN err=%s found=%u ms=%" PRId64 "\n", esp_err_to_name(err), found, ms);
        for (uint16_t i = 0; err == ESP_OK && i < found && i < 16; i++) {
            // SSIDs of the neighbourhood are not printed: channel, signal and security are enough for diagnostics.
            printf("WIFI_AP ch=%u rssi=%d auth=%u hidden=%s\n", aps[i].channel, aps[i].rssi, aps[i].authmode,
                   aps[i].hidden ? "yes" : "no");
        }
        memset(aps, 0, sizeof(aps));
        return err == ESP_OK ? 0 : 1;
    }
#if CONFIG_KVM_NET_CONSOLE_JOIN
    if (argc >= 3 && strcmp(argv[1], "join") == 0) {
        // Bench only (ADR-015): sent by test/hil/wifi_provision.py, which keeps no log. Nothing is printed back
        // except the result; argv is wiped afterwards.
        esp_err_t err = net_status_set_credentials(argv[2], argc >= 4 ? argv[3] : "");
        for (int i = 2; i < argc; i++) {
            memset(argv[i], 0, strlen(argv[i]));
        }
        linenoiseHistoryFree(); // the REPL stored the typed line in its history: drop it
        printf("WIFI_JOIN err=%s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
#endif
    if (argc >= 3 && strcmp(argv[1], "ps") == 0) { // AT-EXT-01 bench: TECH-070 is decided by this measurement
        const bool off = strcmp(argv[2], "none") == 0;
        if (!off && strcmp(argv[2], "min") != 0) {
            printf("usage: wifi ps none|min\n");
            return 1;
        }
        esp_err_t err = net_status_set_power_save_off(off);
        printf("WIFI_PS mode=%s err=%s\n", off ? "none" : "min_modem", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "reconnect") == 0) { // AT-EXT-02 bench: cached vs full-scan reconnect
        const bool full = argc >= 3 && strcmp(argv[2], "full") == 0;
        esp_err_t err = net_status_bench_reconnect(full);
        printf("WIFI_RECONNECT path=%s err=%s\n", full ? "full" : "cached", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc >= 2 && strcmp(argv[1], "forget") == 0) {
        esp_err_t err = net_status_forget(); // erase the stored network; no secret is typed or printed
        printf("WIFI_FORGET err=%s\n", esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    net_status_info_t ni;
    net_status_get(&ni);
    const net_status_t *l = &ni.link;
    printf("WIFI link=%s configured=%s ip=%s radio=%s channel=%u rssi=%d last_reason=%u failures=%" PRIu32 "\n",
           net_link_name(l->link), l->configured ? "yes" : "no", l->ip_present ? "yes" : "no", ni.radio_on ? "on" : "off",
           l->channel, ni.rssi, l->last_reason, l->consecutive_failures);
    printf("WIFI_COUNTERS events=%" PRIu32 " dropped=%" PRIu32 " scans=%" PRIu32 " cached=%" PRIu32 " full=%" PRIu32
           " fallbacks=%" PRIu32 " connects=%" PRIu32 " disconnects=%" PRIu32 " timeouts=%" PRIu32 " echoes=%" PRIu32
           " heap_int=%u\n",
           ni.events, ni.events_dropped, ni.scans, l->st.attempts_cached, l->st.attempts_full, l->st.cached_fallbacks,
           l->st.connects, l->st.disconnects, l->st.timeouts, l->st.echoes,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    printf("WIFI_TIMING last_connect_ms=%" PRIu32 " via=%s ps=%s\n", ni.last_connect_ms,
           ni.last_connect_ms == 0 ? "-" : (ni.last_connect_cached ? "cached" : "full"),
           ni.power_save_off ? "none" : "min_modem");
    return 0;
}

#if CONFIG_KVM_HID_BENCH_PROBE
// Bench probe (see Kconfig): fixed harmless sequences through the normal LOCAL path. Paced so that every report is
// read by the HOST before the next one (the scheduler would coalesce mouse moves otherwise).
static void probe_wait(void)
{
    vTaskDelay(pdMS_TO_TICKS(40));
}

static esp_err_t probe_move(int16_t dx, int16_t dy)
{
    kvm_hid_intent_t in = {.type = KVM_HID_MOUSE_MOVE};
    in.timestamp_us = (uint64_t)esp_timer_get_time();
    in.data.mouse_move.dx = dx;
    in.data.mouse_move.dy = dy;
    return hid_dispatch_local(&in);
}

static esp_err_t probe_chord(uint16_t chord_id)
{
    kvm_hid_intent_t in = {.type = KVM_HID_CHORD};
    in.timestamp_us = (uint64_t)esp_timer_get_time();
    in.data.chord.chord_id = chord_id;
    return hid_dispatch_local(&in);
}

// AT-HID-08 (unplug half) + AT-HID-09: `total` shortcuts of Ctrl / Shift / Ctrl+Shift + F13..F24, sent as balanced
// text bursts (>= 10 ms per step, each step read by the HOST before the next) while a person unplugs and re-plugs the
// HID cable at random. A burst cut by the unplug ends in RELEASE_ALL; the probe waits for the next mount and goes on.
// F13..F24 with Ctrl/Shift only: no other key is ever sent. Board-side verdict: nothing held at the end and one neutral
// start per interface after every mount; the HOST-side capture (hid_capture_windows.py --probe soak) checks the rest.
static int hid_probe_soak(uint32_t total)
{
    static const uint8_t k_mods[3][2] = {{KVM_KEY_LEFT_CTRL, 0}, {KVM_KEY_LEFT_SHIFT, 0},
                                         {KVM_KEY_LEFT_CTRL, KVM_KEY_LEFT_SHIFT}};
    EXT_RAM_BSS_ATTR static kvm_hid_burst_step_t steps[KVM_HID_BURST_CAP];
    kvm_hid_counters_t c0, c;
    usb_hid_status_t u0, u;
    hid_dispatch_get_counters(&c0);
    usb_hid_device_get_status(&u0);
    const int64_t deadline = esp_timer_get_time() + 40LL * 60 * 1000000; // 40 min, then stop whatever the count
    uint32_t k = 0, tried = 0, done = 0, cut = 0, retries = 0;
    while (tried < total && esp_timer_get_time() < deadline) {
        kvm_hid_snapshot_t sn;
        hid_dispatch_get_snapshot(&sn, NULL);
        if (sn.host_usb != KVM_HOST_USB_MOUNTED || sn.burst_active || !sn.ledger_empty) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint16_t n = 0;
        uint32_t in_burst = 0;
        while (n + 6u <= KVM_HID_BURST_CAP && tried + in_burst < total) {
            const uint8_t *m = k_mods[(k + in_burst) % 3u];
            const uint8_t key = (uint8_t)(0x68u + (k + in_burst) % 12u); // F13..F24
            const uint8_t nm = m[1] != 0 ? 2 : 1;
            for (uint8_t i = 0; i < nm; i++) {
                steps[n++] = (kvm_hid_burst_step_t){.usage = m[i], .down = true};
            }
            steps[n++] = (kvm_hid_burst_step_t){.usage = key, .down = true};
            steps[n++] = (kvm_hid_burst_step_t){.usage = key, .down = false};
            for (int i = nm - 1; i >= 0; i--) {
                steps[n++] = (kvm_hid_burst_step_t){.usage = m[i], .down = false};
            }
            in_burst++;
        }
        hid_dispatch_get_counters(&c);
        const uint32_t started0 = c.bursts_started;
        const uint32_t ended0 = c.bursts_done + c.bursts_cancelled + c.bursts_aborted;
        const uint32_t done0 = c.bursts_done;
        if (hid_dispatch_burst_start(steps, n, NULL, 0) != ESP_OK) {
            retries++;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int waited = 0;
        do {
            vTaskDelay(pdMS_TO_TICKS(10));
            hid_dispatch_get_counters(&c);
        } while (c.bursts_started == started0 && ++waited < 50);
        if (c.bursts_started == started0) { // refused by the core (e.g. the HOST went away in between): try again
            retries++;
            continue;
        }
        while (c.bursts_done + c.bursts_cancelled + c.bursts_aborted == ended0) {
            vTaskDelay(pdMS_TO_TICKS(20));
            hid_dispatch_get_counters(&c);
        }
        if (c.bursts_done != done0) {
            done += in_burst;
        } else {
            cut++;
        }
        k += in_burst;
        tried += in_burst;
        if (tried / 1000u != (tried - in_burst) / 1000u) {
            usb_hid_device_get_status(&u);
            printf("HID_PROBE soak progress tried=%" PRIu32 " done=%" PRIu32 " cut_bursts=%" PRIu32 " unplugs=%" PRIu32
                   "\n",
                   tried, done, cut, u.umounts - u0.umounts);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    hid_dispatch_get_counters(&c);
    usb_hid_device_get_status(&u);
    kvm_hid_snapshot_t end;
    hid_dispatch_get_snapshot(&end, NULL);
    printf("HID_PROBE soak tried=%" PRIu32 " done=%" PRIu32 " cut_bursts=%" PRIu32 " retries=%" PRIu32
           " unplugs=%" PRIu32 " mounts=%" PRIu32 " suspends=%" PRIu32 " usb_loss_releases=%" PRIu32
           " suspend_releases=%" PRIu32 " neutral_sent=%" PRIu32 " neutral_failed=%" PRIu32 " timeouts=%" PRIu32
           " ledger_empty=%d mods=0x%02X keys=%u\n",
           tried, done, cut, retries, u.umounts - u0.umounts, u.mounts - u0.mounts, u.suspends - u0.suspends,
           c.release_by_reason[KVM_RA_USB_LOSS] - c0.release_by_reason[KVM_RA_USB_LOSS],
           c.release_by_reason[KVM_RA_USB_SUSPEND] - c0.release_by_reason[KVM_RA_USB_SUSPEND],
           u.neutral_sent - u0.neutral_sent, u.neutral_failed - u0.neutral_failed,
           c.completion_timeouts - c0.completion_timeouts, (int)end.ledger_empty, end.modifiers_down, end.keys_down);
    return 0;
}

static int hid_probe(const char *what, const char *arg)
{
    kvm_hid_snapshot_t sn;
    hid_dispatch_get_snapshot(&sn, NULL);
    if (sn.host_usb != KVM_HOST_USB_MOUNTED || sn.owner_kind == OWNER_REMOTE || !sn.ledger_empty) {
        printf("HID_PROBE refused: host not mounted, remote owner or input held\n");
        return 1;
    }
    int errors = 0;
    if (strcmp(what, "keys") == 0) {
        // F13..F24 (usages 0x68..0x73): one DOWN and one UP each.
        for (uint8_t u = 0x68; u <= 0x73; u++) {
            errors += hid_dispatch_ui_key_down(u) != ESP_OK;
            probe_wait();
            errors += hid_dispatch_ui_key_up(u) != ESP_OK;
            probe_wait();
        }
    } else if (strcmp(what, "rollover") == 0) {
        // F13..F19 held together: six fit the boot report, the seventh must be refused (AT-HID-03).
        int refused = 0;
        for (uint8_t u = 0x68; u <= 0x6E; u++) {
            refused += hid_dispatch_ui_key_down(u) != ESP_OK;
            probe_wait();
        }
        for (uint8_t u = 0x68; u <= 0x6E; u++) {
            hid_dispatch_ui_key_up(u);
            probe_wait();
        }
        printf("HID_PROBE rollover refused_downs=%d\n", refused);
    } else if (strcmp(what, "move") == 0) {
        // One large move each way: int16 split into int8 reports (AT-HID-04); the pointer ends where it started.
        errors += probe_move(300, 0) != ESP_OK;
        probe_wait();
        errors += probe_move(-300, 0) != ESP_OK;
        probe_wait();
        errors += probe_move(0, 200) != ESP_OK;
        probe_wait();
        errors += probe_move(0, -200) != ESP_OK;
        probe_wait();
    } else if (strcmp(what, "chords") == 0) {
        // AT-HID-06, owner's go-ahead of 2026-11-02 (they act on the HOST desktop): Alt+Tab twice and Win+D twice, so
        // the desktop ends as it was, then Ctrl+Shift+Esc (opens Task Manager). Never Ctrl+Alt+Del. Each chord goes
        // through the allowlist as a KVM_HID_CHORD intent, the same path as the SHORTCUTS view.
        static const uint16_t seq[] = {KVM_CHORD_ALT_TAB, KVM_CHORD_ALT_TAB, KVM_CHORD_SHOW_DESKTOP,
                                       KVM_CHORD_SHOW_DESKTOP, KVM_CHORD_TASK_MANAGER};
        for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
            errors += probe_chord(seq[i]) != ESP_OK;
            vTaskDelay(pdMS_TO_TICKS(600));
        }
        // AT-HID-08, revoke half: an Alt+Tab cut by RELEASE_ALL (owner change) right after it is accepted. Whatever
        // part of it reached the wire, every make must get its break and the ledger must end empty.
        errors += probe_chord(KVM_CHORD_ALT_TAB) != ESP_OK;
        hid_dispatch_release_all(KVM_RA_OWNER_CHANGE);
        vTaskDelay(pdMS_TO_TICKS(300));
        kvm_hid_snapshot_t after;
        hid_dispatch_get_snapshot(&after, NULL);
        printf("HID_PROBE chords interrupted: ledger_empty=%d chord_active=%d\n", (int)after.ledger_empty,
               (int)after.chord_active);
    } else if (strcmp(what, "soak") == 0) {
        uint32_t total = arg != NULL ? (uint32_t)strtoul(arg, NULL, 0) : 10000u;
        errors += hid_probe_soak(total == 0 || total > 20000u ? 10000u : total);
    } else {
        printf("usage: hid probe <keys|rollover|move|chords|soak [n]>\n");
        return 1;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    hid_dispatch_release_all(KVM_RA_REQUEST);
    vTaskDelay(pdMS_TO_TICKS(50));
    printf("HID_PROBE %s done errors=%d\n", what, errors);
    return errors != 0;
}
#endif

static int cmd_hid(int argc, char **argv)
{
#if CONFIG_KVM_HID_BENCH_PROBE
    if (argc >= 3 && strcmp(argv[1], "probe") == 0) {
        return hid_probe(argv[2], argc >= 4 ? argv[3] : NULL);
    }
#endif
    if (argc >= 2 && strcmp(argv[1], "release") == 0) {
        hid_dispatch_release_all(KVM_RA_REQUEST);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (argc >= 2 && strcmp(argv[1], "keepalive") == 0) {
        bool on = argc >= 3 && strcmp(argv[2], "on") == 0;
        uint32_t s = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 30;
        hid_dispatch_set_keepalive(on, s);
        vTaskDelay(pdMS_TO_TICKS(50));
    } else if (argc >= 2) {
        printf("usage: hid [release | keepalive on|off [s] | probe <keys|rollover|move|chords|soak [n]> (bench builds)]\n");
        return 1;
    }
    kvm_hid_snapshot_t sn;
    uint32_t seq = 0;
    hid_dispatch_get_snapshot(&sn, &seq);
    kvm_hid_counters_t c;
    hid_dispatch_get_counters(&c);
    hid_dispatch_stats_t st;
    hid_dispatch_get_stats(&st);
    printf("HID host_usb=%d owner=%s epoch=%" PRIu32 " maintenance=%d ledger_empty=%d mods=0x%02X keys=%u buttons=0x%02X"
           " consumer=0x%04X armed=0x%02X locked=0x%02X chord=%d keepalive=%d seq=%" PRIu32 "\n",
           (int)sn.host_usb, owner_name(sn.owner_kind), sn.owner_epoch, sn.maintenance, sn.ledger_empty,
           sn.modifiers_down, sn.keys_down, sn.buttons_down, sn.consumer_down, sn.mods_armed, sn.mods_locked,
           sn.chord_active, sn.keepalive_enabled, seq);
    printf("HID accepted=%" PRIu32 " sent=%" PRIu32 " refused=%" PRIu32 " timeouts=%" PRIu32 " release_all=%" PRIu32
           " overflow=%" PRIu32 " stale=%" PRIu32 " rej_offline=%" PRIu32 " rej_susp=%" PRIu32 " rej_usage=%" PRIu32
           " keepalive_sent=%" PRIu32 " keepalive_rej=%" PRIu32 " qhw=%" PRIu32 "\n",
           c.accepted, c.reports_sent, c.send_refused, c.completion_timeouts, c.release_all, c.queue_overflows,
           c.stale_dropped, c.rejected_offline, c.rejected_suspended, c.rejected_usage, c.keepalive_sent,
           c.keepalive_rejected, c.queue_high_water);
    printf("HID completion_timeouts kbd=%" PRIu32 " mouse=%" PRIu32 " consumer=%" PRIu32 "\n",
           c.completion_timeouts_itf[KVM_HID_ITF_KEYBOARD], c.completion_timeouts_itf[KVM_HID_ITF_MOUSE],
           c.completion_timeouts_itf[KVM_HID_ITF_CONSUMER]);
    printf("HID release_by_reason:");
    for (int r = 0; r < KVM_RA_COUNT; r++) {
        if (c.release_by_reason[r] != 0) {
            printf(" %s=%" PRIu32, kvm_release_reason_name((kvm_release_reason_t)r), c.release_by_reason[r]);
        }
    }
    printf("\nHID dispatch posted=%" PRIu32 " producer_full=%" PRIu32 " release_req=%" PRIu32 " pumps=%" PRIu32
           " max_pump_us=%" PRIu32 " chords_ok=%" PRIu32 " chords_aborted=%" PRIu32 "\n",
           st.posted, st.producer_queue_full, st.release_requests, st.pumps, st.max_pump_us, st.events_chord_delivered,
           st.events_chord_aborted);
    printf("HID burst active=%d started=%" PRIu32 " done=%" PRIu32 " cancelled=%" PRIu32 " aborted=%" PRIu32 "\n",
           sn.burst_active, c.bursts_started, c.bursts_done, c.bursts_cancelled, c.bursts_aborted);
    return 0;
}

static int cmd_tasks(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    EXT_RAM_BSS_ATTR static char buf[4096]; // PSRAM. IDF FreeRTOS: vTaskList/vTaskGetRunTimeStats take no size; 40 tasks x ~60 B
    vTaskGetRunTimeStats(buf);
    printf("%s", buf);
    vTaskList(buf);
    printf("%s", buf);
    return 0;
}

static int cmd_cpu(int argc, char **argv)
{
    uint32_t ms = argc >= 2 ? (uint32_t)atoi(argv[1]) : 1000;
    diag_cpu_t c;
    if (diag_cpu_sample(ms, &c) != ESP_OK) {
        return 1;
    }
    printf("CPU busy core0=%" PRIu32 " core1=%" PRIu32 " permille over %" PRIu32 " ms\n", c.cpu_busy_permille[0],
           c.cpu_busy_permille[1], ms);
    return 0;
}

static int cmd_config(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    app_config_print();
    return 0;
}

static void reboot_task(void *arg)
{
    (void)arg;
    esp_restart();
}

// `reboot [core0|core1]`: esp_restart(); with an argument it runs from a task pinned to that core. Diagnostic for the
// M8 finding that some software restarts end through the RTC watchdog backstop (rst:0x10). Board #1, 2026-11-02: it
// happened from both cores, so the core is not the cause (test/reports/2026-11-02_M8_TASK-110-ota_d36ab0a.yaml).
static int cmd_reboot(int argc, char **argv)
{
    int core = -1;
    if (argc >= 2) {
        if (strcmp(argv[1], "core0") == 0) {
            core = 0;
        } else if (strcmp(argv[1], "core1") == 0) {
            core = 1;
        } else {
            printf("usage: reboot [core0|core1]\n");
            return 1;
        }
    }
    if (!app_config_flush()) {
        printf("settings flush failed\n");
    }
    printf("rebooting from core %d\n", core >= 0 ? core : (int)xPortGetCoreID());
    fflush(stdout);
    if (core < 0) {
        esp_restart();
    }
    if (xTaskCreatePinnedToCore(reboot_task, "reboot", 3072, NULL, configMAX_PRIORITIES - 1, NULL, core) != pdPASS) {
        printf("reboot task: no memory\n");
        return 1;
    }
    vTaskDelay(portMAX_DELAY);
    return 0;
}

esp_err_t kvm_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "kvmpad>";
    rc.max_cmdline_length = 128;
    rc.task_stack_size = 6144;
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uc, &rc, &repl);
    if (err != ESP_OK) {
        return err;
    }
    const esp_console_cmd_t cmds[] = {
        {.command = "info", .help = "chip, flash, PSRAM, heap, versions, notice", .func = cmd_info},
        {.command = "i2cscan", .help = "probe 0x08..0x77 on the shared bus", .func = cmd_i2cscan},
        {.command = "i2cw", .help = "i2cw <addr7> <byte...>: raw write (bring-up debug)", .func = cmd_i2cw},
        {.command = "i2cr", .help = "i2cr <addr7> [len]: raw read (bring-up debug)", .func = cmd_i2cr},
        {.command = "exio", .help = "expander shadow/read-back; exio set <1..6> <0|1> (USB_SEL=1 refused)", .func = cmd_exio},
        {.command = "touch", .help = "GT911 info/stats; touch capture <ms> [max]; touch fault <n>", .func = cmd_touch},
        {.command = "lcd", .help = "lcd <screen> | stats [reset] | bench [s] | variant <fbs> <lines> | drawload <hz> <s> | glitch <us> [n] | bl <0|1>", .func = cmd_lcd},
        {.command = "stress", .help = "stress [s] [nowifi]: Wi-Fi scan + NVS + flash + PSRAM load", .func = cmd_stress},
        {.command = "usb", .help = "USB device state and counters", .func = cmd_usb},
        {.command = "wifi", .help = "wifi [scan | forget | ps none|min | reconnect [full]]: Wi-Fi link status (TASK-080); ps/reconnect are AT-EXT-01/02 benches", .func = cmd_wifi},
        {.command = "hid", .help = "hid [release | keepalive on|off [s]]: HID core state (neutral-only actions)", .func = cmd_hid},
        {.command = "gesture", .help = "gesture [replay]: engine thresholds; replay VEC-001..004 without HID", .func = cmd_gesture},
#if CONFIG_KVM_UI_BENCH
        {.command = "ui", .help = "ui [shell [status] | bench <scene> [s] | demo | scene <name> | snap [div] | reset]: LVGL platform and shell", .func = cmd_ui},
#endif
        {.command = "tasks", .help = "FreeRTOS run-time stats and task list", .func = cmd_tasks},
        {.command = "cpu", .help = "cpu [ms]: per-core load", .func = cmd_cpu},
        {.command = "remote",
         .help = "remote [status | pair open|cancel | enable on|off | forget <i>|all]: WSS transport (TASK-100)",
         .func = remote_glue_console_cmd},
        {.command = "ota",
         .help = "ota [status | url <https-url> | cancel | mark-valid]: signed update (TASK-110)",
         .func = update_manager_console_cmd},
        {.command = "ime", .help = "ime [status | mount | bench]: ime_data image and PinyinIME footprint spike (TASK-072, no HID)", .func = ime_bench_console_cmd},
        {.command = "config", .help = "settings record: load status, write counters, non-secret fields (TASK-090)", .func = cmd_config},
        {.command = "reboot", .help = "reboot [core0|core1]: esp_restart(), optionally from a task pinned to that core (diagnostic)", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
    esp_console_register_help_command();
    return esp_console_start_repl(repl);
}
