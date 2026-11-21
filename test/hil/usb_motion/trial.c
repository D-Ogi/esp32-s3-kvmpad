// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "trial.h"
#include "fixtures.h"
#include "pack_panel.h"
#include "board_lcd.h"
#include "usb_monitor.h"
#include "ui_platform.h"
#include <stdio.h>
#include <string.h>

static bool active, native, desktop, sideways, alternate_background, textured;
static unsigned sent, position;

extern const uint8_t motion_desktop[];
extern const uint32_t motion_desktop_length;
static bool feed_desktop(unsigned index)
{
    size_t offset = 0;
    for (unsigned i = 0; offset + 16 <= motion_desktop_length; ++i) {
        const uint8_t *record = motion_desktop + offset;
        uint32_t packed;
        memcpy(&packed, record + 12, sizeof(packed));
        size_t size = 16 + (packed >> 10);
        if (size > motion_desktop_length - offset) return false;
        if (i == index) {
            for (size_t at = 0; at < size; at += 64)
                usb_monitor_feed(record + at, size - at < 64 ? size - at : 64);
            return true;
        }
        offset += size;
    }
    return false;
}

// Keep the regular HID profile throughout this local fixture.
static bool fixture_link(bool enable)
{
    active = enable;
    if (!enable) {
        usb_monitor_view_merge_test_regions(false);
        usb_monitor_view_test_wait_period(false);
    }
    return true;
}

static int command_locked(int argc, char **argv)
{
    if ((argc == 3 || argc == 5 || argc == 6) && !strcmp(argv[0], "start") &&
        (!strcmp(argv[1], "640x480") || !strcmp(argv[1], "1024x600")) &&
        (!strcmp(argv[2], "box") || !strcmp(argv[2], "split"))) {
        bool options = argc >= 5;
        if (active || usb_monitor_view_active() ||
            (argc == 6 && strcmp(argv[5], "period") && strcmp(argv[5], "ready") && strcmp(argv[5], "poll25")) || (options &&
            ((strcmp(argv[3], "scalar") && strcmp(argv[3], "packed") && strcmp(argv[3], "vector") && strcmp(argv[3], "cells")) ||
             (strcmp(argv[4], "cursor") && strcmp(argv[4], "desktop") && strcmp(argv[4], "sideways") &&
              strcmp(argv[4], "alternate") && strcmp(argv[4], "texture"))))) return 1;
        bool use_native = !strcmp(argv[1], "1024x600");
        textured = options && !strcmp(argv[4], "texture");
        if (textured && !use_native) return 1;
        if (!pack_panel_open_motion_fixture(use_native, fixture_link)) return 1;
        alternate_background = options && !strcmp(argv[4], "alternate");
        sideways = alternate_background || textured || (options && !strcmp(argv[4], "sideways"));
        usb_monitor_view_test_wait_period(argc == 6 && !strcmp(argv[5], "period"));
        unsigned poll_ms = usb_monitor_view_test_poll(argc == 6 && !strcmp(argv[5], "poll25"));
        usb_monitor_test_pixel_path(options && !strcmp(argv[3], "scalar"), sideways && !strcmp(argv[2], "box"),
                                    options && !strcmp(argv[3], "vector"), options && !strcmp(argv[3], "cells"));
        if (!usb_monitor_test_equal_blocks() || (options && !strcmp(argv[3], "cells") && !usb_monitor_test_has_cells())) {
            usb_monitor_view_close();
            return 1;
        }
        desktop = sideways || (options && !strcmp(argv[4], "desktop"));
        native = use_native;
        position = sent = 0;
        usb_monitor_view_merge_test_regions(!strcmp(argv[2], "box"));
        printf("MOTION started=1 poll_ms=%u\n", poll_ms);
        return 0;
    }
    if (argc != 1 || !active || !usb_monitor_view_active()) return 1;
    if (!strcmp(argv[0], "stop")) {
        usb_monitor_view_close();
        puts("MOTION stopped=1");
        return 0;
    }
    if (usb_monitor_stats().presented != sent) return 1;
    if (!strcmp(argv[0], "next")) {
        position = position == 1 ? 2 : 1;
        if (desktop && !(alternate_background && position == 1)) {
            unsigned base = textured ? 8u : (sideways ? 4u : 0u) + (native ? 2u : 0u);
            if (!feed_desktop(base + position - 1)) return 1;
        } else usb_motion_feed(native, position, usb_monitor_feed);
        printf("MOTION sent=%u position=%u\n", ++sent, position);
        return 0;
    }
    if (!strcmp(argv[0], "check") && position) {
        int x = native ? 0 : 192, y = native ? 0 : 84;
        unsigned mask = 0;
        for (unsigned i = 0; i < 2; ++i) {
            const uint16_t *p = board_lcd_fb(i);
            if (p && p[(y+40)*1024+x+40] == (position==1 ? 0xffff : 0) &&
                p[(y+(sideways ? 40 : 296))*1024+x+488] == (position==2 ? 0xffff : 0) &&
                p[(y+120)*1024+x+120] == 0) mask |= 1u << i;
        }
        unsigned process, black, damage, wait, draw;
        usb_monitor_test_decode_times(&process, &black, &damage);
        usb_monitor_view_test_refresh_times(&wait, &draw);
        printf("MOTION checked=%u position=%u fb_mask=%u jpeg_us=%u black_us=%u damage_us=%u wait_us=%u draw_us=%u\n",
               sent, position, mask, process, black, damage, wait, draw);
        return 0;
    }
    return 1;
}

int usb_motion_command(int argc, char **argv)
{
    if (!ui_platform_lock(1000)) return 1;
    int result = command_locked(argc, argv);
    ui_platform_unlock();
    return result;
}
