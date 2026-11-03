// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Pure RGB565 test patterns for the M1 bring-up (TASK-020 test card, TASK-030 touch screen).
// No ESP-IDF dependency: compiled by the host unit tests.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_GREY    RGB565(96, 96, 96)

typedef struct {
    uint16_t *px;
    int w;
    int h;
} lcd_canvas_t;

void pat_fill(const lcd_canvas_t *c, uint16_t color);
void pat_rect(const lcd_canvas_t *c, int x, int y, int w, int h, uint16_t color);
void pat_frame(const lcd_canvas_t *c, int x, int y, int w, int h, uint16_t color);

/** 5x7 text, `scale` >= 1, unknown glyphs drawn as '?'. Returns the x after the last glyph. */
int pat_text(const lcd_canvas_t *c, int x, int y, int scale, uint16_t fg, const char *s);

/** Eight vertical colour bars: white, yellow, cyan, green, magenta, red, blue, black. */
void pat_bars(const lcd_canvas_t *c, int y, int h);

/** Full-depth ramps: red (32 steps), green (64), blue (32), one band each. */
void pat_ramps(const lcd_canvas_t *c, int y, int h);

/**
 * Alignment frame: a 1-px white border on the outermost pixels, corner L-marks, a centre
 * cross. Any horizontal/vertical shift of the RGB stream shows as a missing or doubled edge.
 */
void pat_alignment(const lcd_canvas_t *c);

/** The TASK-020 test card: bars, ramps, R/G/B swatches with labels, alignment frame. */
void pat_test_card(const lcd_canvas_t *c, const char *footer);

/** Touch diagnostics background: grid + five control targets (four corners + centre, TECH-011). */
void pat_touch_screen(const lcd_canvas_t *c);

/** Colour of a touch track id (stable for the life of the track). */
uint16_t pat_track_color(uint8_t track_id);

/** Glyph rows for `ch` (7 rows, 5 bits, MSB = leftmost); NULL when the font has no glyph. */
const uint8_t *pat_glyph(char ch);

#ifdef __cplusplus
}
#endif
