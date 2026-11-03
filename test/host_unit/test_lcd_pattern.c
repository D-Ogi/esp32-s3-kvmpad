// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// TASK-020 (TECH-001): the RGB565 test patterns used for the R/G/B, swatch and alignment checks.
// Every drawing call is clipped: a guard band around the frame buffer must stay untouched.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "board_pins.h"
#include "kvm_test.h"
#include "lcd_pattern.h"

#define W BOARD_LCD_H_RES
#define H BOARD_LCD_V_RES
#define GUARD 4096
#define GUARD_WORD 0xA55A

static uint16_t *s_mem;

static lcd_canvas_t canvas_new(void)
{
    size_t n = (size_t)W * H + 2 * GUARD;
    s_mem = malloc(n * sizeof(uint16_t));
    for (size_t i = 0; i < n; i++) {
        s_mem[i] = GUARD_WORD;
    }
    lcd_canvas_t c = {.px = s_mem + GUARD, .w = W, .h = H};
    return c;
}

static int guards_intact(void)
{
    for (size_t i = 0; i < GUARD; i++) {
        if (s_mem[i] != GUARD_WORD || s_mem[GUARD + (size_t)W * H + i] != GUARD_WORD) {
            return 0;
        }
    }
    return 1;
}

static uint16_t px(const lcd_canvas_t *c, int x, int y)
{
    return c->px[(size_t)y * (size_t)c->w + (size_t)x];
}

static void rect_clips_at_every_edge(void)
{
    lcd_canvas_t c = canvas_new();
    pat_fill(&c, C_BLACK);
    pat_rect(&c, -50, -50, 100, 100, C_RED);
    pat_rect(&c, W - 10, H - 10, 500, 500, C_GREEN);
    pat_rect(&c, -100000, 10, 200000, 1, C_BLUE);
    pat_rect(&c, 10, -100000, 1, 200000, C_BLUE);
    pat_rect(&c, W, H, 10, 10, C_WHITE); // fully outside
    pat_rect(&c, 200, 200, -3, 7, C_WHITE); // negative size: nothing drawn
    pat_frame(&c, -5, -5, W + 10, H + 10, C_YELLOW);
    CHECK(guards_intact());
    CHECK_EQ(px(&c, 0, 0), C_RED);
    CHECK_EQ(px(&c, 49, 49), C_RED);
    CHECK_EQ(px(&c, 50, 50), C_BLACK);
    CHECK_EQ(px(&c, W - 1, H - 1), C_GREEN);
    CHECK_EQ(px(&c, 200, 200), C_BLACK);
    CHECK_EQ(px(&c, 197, 200), C_BLACK);
    free(s_mem);
}

static void text_clips_and_unknown_glyphs(void)
{
    lcd_canvas_t c = canvas_new();
    pat_fill(&c, C_BLACK);
    int x = pat_text(&c, W - 20, H - 3, 4, C_WHITE, "OVERFLOW \x01\x7f~");
    CHECK(x > W - 20);
    pat_text(&c, -30, -5, 3, C_WHITE, "KVMPAD M1 RGB565");
    CHECK(guards_intact());
    for (char ch = '0'; ch <= '9'; ch++) {
        CHECK(pat_glyph(ch) != NULL);
    }
    for (char ch = 'A'; ch <= 'Z'; ch++) {
        CHECK(pat_glyph(ch) != NULL);
    }
    free(s_mem);
}

static void bars_are_eight_ordered_columns(void)
{
    static const uint16_t k[8] = {C_WHITE, C_YELLOW, C_CYAN, C_GREEN, C_MAGENTA, C_RED, C_BLUE, C_BLACK};
    lcd_canvas_t c = canvas_new();
    pat_fill(&c, C_GREY);
    pat_bars(&c, 0, H / 3);
    for (int i = 0; i < 8; i++) {
        int x0 = (W * i) / 8;
        int x1 = (W * (i + 1)) / 8 - 1;
        CHECK_EQ(px(&c, x0, 0), k[i]);
        CHECK_EQ(px(&c, x1, H / 3 - 1), k[i]);
    }
    CHECK_EQ(px(&c, 0, H / 3), C_GREY);
    CHECK(guards_intact());
    free(s_mem);
}

static void ramps_are_monotonic_full_depth(void)
{
    lcd_canvas_t c = canvas_new();
    pat_fill(&c, C_BLACK);
    const int y = 100;
    const int h = 90;
    pat_ramps(&c, y, h);
    unsigned pr = 0, pg = 0, pb = 0;
    for (int x = 0; x < W; x++) {
        unsigned r = (unsigned)(px(&c, x, y) >> 11);
        unsigned g = (unsigned)((px(&c, x, y + h / 3) >> 5) & 0x3F);
        unsigned b = (unsigned)(px(&c, x, y + 2 * (h / 3)) & 0x1F);
        CHECK(r >= pr && g >= pg && b >= pb);
        pr = r;
        pg = g;
        pb = b;
    }
    CHECK_EQ(pr, 31);
    CHECK_EQ(pg, 63);
    CHECK_EQ(pb, 31);
    CHECK_EQ(px(&c, 0, y) >> 11, 0);
    CHECK(guards_intact());
    free(s_mem);
}

static void alignment_border_on_outermost_pixels(void)
{
    lcd_canvas_t c = canvas_new();
    pat_fill(&c, C_BLACK);
    pat_alignment(&c);
    for (int x = 0; x < W; x++) {
        CHECK_EQ(px(&c, x, 0), C_WHITE);
        CHECK_EQ(px(&c, x, H - 1), C_WHITE);
    }
    for (int y = 0; y < H; y++) {
        CHECK_EQ(px(&c, 0, y), C_WHITE);
        CHECK_EQ(px(&c, W - 1, y), C_WHITE);
    }
    CHECK_EQ(px(&c, 1, 1), C_BLACK); // a 1-px border, not 2
    CHECK_EQ(px(&c, W / 2, H / 2), C_WHITE);
    CHECK(guards_intact());
    free(s_mem);
}

static void full_screens_stay_in_bounds(void)
{
    lcd_canvas_t c = canvas_new();
    char footer[200];
    memset(footer, 'W', sizeof(footer) - 1);
    footer[sizeof(footer) - 1] = '\0';
    pat_test_card(&c, footer);
    CHECK(guards_intact());
    CHECK_EQ(px(&c, 0, 0), C_WHITE); // alignment border drawn last
    pat_touch_screen(&c);
    CHECK(guards_intact());
    for (int id = 0; id < 256; id++) {
        (void)pat_track_color((uint8_t)id);
    }
    CHECK(pat_track_color(0) != pat_track_color(1));
    free(s_mem);
}

int main(void)
{
    printf("test_lcd_pattern (%dx%d)\n", W, H);
    RUN(rect_clips_at_every_edge);
    RUN(text_clips_and_unknown_glyphs);
    RUN(bars_are_eight_ordered_columns);
    RUN(ramps_are_monotonic_full_depth);
    RUN(alignment_border_on_outermost_pixels);
    RUN(full_screens_stay_in_bounds);
    TEST_MAIN_END("test_lcd_pattern");
}
