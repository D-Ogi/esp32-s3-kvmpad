// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "lcd_pattern.h"

#include <stddef.h>

// Hand-drawn 5x7 glyphs (7 rows, low 5 bits, bit4 = leftmost column). Lower case maps to upper.
typedef struct {
    char ch;
    uint8_t rows[7];
} glyph_t;

static const glyph_t k_font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x0C, 0x04, 0x08}},
    {':', {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'+', {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'=', {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00}},
    {'/', {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x00}},
    {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'#', {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'?', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04}},
};

const uint8_t *pat_glyph(char ch)
{
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    for (size_t i = 0; i < sizeof(k_font) / sizeof(k_font[0]); i++) {
        if (k_font[i].ch == ch) {
            return k_font[i].rows;
        }
    }
    return NULL;
}

void pat_rect(const lcd_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w;
    int y1 = y + h > c->h ? c->h : y + h;
    for (int yy = y0; yy < y1; yy++) {
        uint16_t *row = &c->px[(size_t)yy * (size_t)c->w];
        for (int xx = x0; xx < x1; xx++) {
            row[xx] = color;
        }
    }
}

void pat_fill(const lcd_canvas_t *c, uint16_t color)
{
    pat_rect(c, 0, 0, c->w, c->h, color);
}

void pat_frame(const lcd_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    pat_rect(c, x, y, w, 1, color);
    pat_rect(c, x, y + h - 1, w, 1, color);
    pat_rect(c, x, y, 1, h, color);
    pat_rect(c, x + w - 1, y, 1, h, color);
}

int pat_text(const lcd_canvas_t *c, int x, int y, int scale, uint16_t fg, const char *s)
{
    if (scale < 1) {
        scale = 1;
    }
    for (; s != NULL && *s; s++) {
        const uint8_t *g = pat_glyph(*s);
        if (g == NULL) {
            g = pat_glyph('?');
        }
        for (int r = 0; r < 7; r++) {
            for (int col = 0; col < 5; col++) {
                if (g[r] & (0x10 >> col)) {
                    pat_rect(c, x + col * scale, y + r * scale, scale, scale, fg);
                }
            }
        }
        x += 6 * scale;
    }
    return x;
}

void pat_bars(const lcd_canvas_t *c, int y, int h)
{
    static const uint16_t k_bars[8] = {C_WHITE, C_YELLOW, C_CYAN, C_GREEN, C_MAGENTA, C_RED, C_BLUE, C_BLACK};
    for (int i = 0; i < 8; i++) {
        int x0 = (c->w * i) / 8;
        int x1 = (c->w * (i + 1)) / 8;
        pat_rect(c, x0, y, x1 - x0, h, k_bars[i]);
    }
}

void pat_ramps(const lcd_canvas_t *c, int y, int h)
{
    int band = h / 3;
    for (int x = 0; x < c->w; x++) {
        uint16_t r = (uint16_t)((x * 32) / c->w);
        uint16_t g = (uint16_t)((x * 64) / c->w);
        uint16_t b = (uint16_t)((x * 32) / c->w);
        pat_rect(c, x, y, 1, band, (uint16_t)(r << 11));
        pat_rect(c, x, y + band, 1, band, (uint16_t)(g << 5));
        pat_rect(c, x, y + 2 * band, 1, h - 2 * band, b);
    }
}

void pat_alignment(const lcd_canvas_t *c)
{
    pat_frame(c, 0, 0, c->w, c->h, C_WHITE);
    const int m = 24;
    // Corner L-marks, 3 px inside the border.
    pat_rect(c, 3, 3, m, 2, C_YELLOW);
    pat_rect(c, 3, 3, 2, m, C_YELLOW);
    pat_rect(c, c->w - 3 - m, 3, m, 2, C_YELLOW);
    pat_rect(c, c->w - 5, 3, 2, m, C_YELLOW);
    pat_rect(c, 3, c->h - 5, m, 2, C_YELLOW);
    pat_rect(c, 3, c->h - 3 - m, 2, m, C_YELLOW);
    pat_rect(c, c->w - 3 - m, c->h - 5, m, 2, C_YELLOW);
    pat_rect(c, c->w - 5, c->h - 3 - m, 2, m, C_YELLOW);
    // Centre cross.
    pat_rect(c, c->w / 2 - 20, c->h / 2, 41, 1, C_WHITE);
    pat_rect(c, c->w / 2, c->h / 2 - 20, 1, 41, C_WHITE);
}

void pat_test_card(const lcd_canvas_t *c, const char *footer)
{
    pat_fill(c, C_BLACK);
    pat_bars(c, 0, c->h / 3);
    pat_ramps(c, c->h / 3, c->h / 4);
    int sy = c->h / 3 + c->h / 4 + 12;
    int sw = 150;
    int sh = 70;
    int gap = (c->w - 3 * sw) / 4;
    static const uint16_t k_sw[3] = {C_RED, C_GREEN, C_BLUE};
    static const char *const k_lbl[3] = {"RED", "GREEN", "BLUE"};
    for (int i = 0; i < 3; i++) {
        int x = gap + i * (sw + gap);
        pat_rect(c, x, sy, sw, sh, k_sw[i]);
        pat_text(c, x + 8, sy + sh + 6, 2, C_WHITE, k_lbl[i]);
    }
    pat_text(c, 12, c->h - 40, 2, C_WHITE, "KVMPAD M1 RGB565");
    if (footer != NULL) {
        pat_text(c, 12, c->h - 18, 1, C_GREY, footer);
    }
    pat_alignment(c);
}

void pat_touch_screen(const lcd_canvas_t *c)
{
    pat_fill(c, RGB565(8, 8, 16));
    for (int x = 0; x < c->w; x += 50) {
        pat_rect(c, x, 0, 1, c->h, RGB565(32, 32, 48));
    }
    for (int y = 0; y < c->h; y += 50) {
        pat_rect(c, 0, y, c->w, 1, RGB565(32, 32, 48));
    }
    const int t = 30;
    const int pts[5][2] = {{t, t}, {c->w - 1 - t, t}, {t, c->h - 1 - t}, {c->w - 1 - t, c->h - 1 - t}, {c->w / 2, c->h / 2}};
    for (int i = 0; i < 5; i++) {
        pat_frame(c, pts[i][0] - 12, pts[i][1] - 12, 25, 25, C_WHITE);
        pat_rect(c, pts[i][0] - 18, pts[i][1], 37, 1, C_WHITE);
        pat_rect(c, pts[i][0], pts[i][1] - 18, 1, 37, C_WHITE);
    }
    pat_frame(c, 0, 0, c->w, c->h, C_WHITE);
}

uint16_t pat_track_color(uint8_t track_id)
{
    static const uint16_t k_col[8] = {C_RED, C_GREEN, C_BLUE, C_YELLOW, C_MAGENTA, C_CYAN, C_WHITE, RGB565(255, 128, 0)};
    return k_col[track_id & 7];
}
