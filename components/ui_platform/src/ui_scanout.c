// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ui_scanout.h"
#include "board_lcd.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include <string.h>

static _Atomic(const uint16_t *) last_frame;

esp_err_t __real_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int, int, int, int, const void *);

// The double-partial adapter finishes its small LVGL tiles in a board framebuffer
// and submits the entire rectangle before returning to the LVGL task.
esp_err_t __wrap_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x, int y, int x_end, int y_end,
                                           const void *pixels)
{
    esp_err_t result = __real_esp_lcd_panel_draw_bitmap(panel, x, y, x_end, y_end, pixels);
    if (result == ESP_OK && panel == board_lcd_panel()) {
        bool full = x == 0 && y == 0 && x_end == BOARD_LCD_H_RES && y_end == BOARD_LCD_V_RES;
        atomic_store(&last_frame, full && (pixels == board_lcd_fb(0) || pixels == board_lcd_fb(1)) ? pixels : NULL);
    }
    return result;
}

typedef struct scanout_row {
    struct scanout_row *next;
    uint16_t left_count, middle_count;
    uint16_t left_color, right_color;
    uint16_t middle[];
} scanout_row_t;

struct ui_scanout {
    scanout_row_t *first;
    uint16_t width;
    uint16_t line[];
};

void ui_scanout_free(ui_scanout_t *shot)
{
    if (!shot) return;
    scanout_row_t *row = shot->first;
    while (row) {
        scanout_row_t *next = row->next;
        heap_caps_free(row);
        row = next;
    }
    heap_caps_free(shot);
}

esp_err_t ui_scanout_take(uint16_t div, ui_scanout_t **out)
{
    if (!out || div < 1 || div > 16) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    const uint16_t *frame = atomic_load(&last_frame);
    if (!frame) return ESP_ERR_NOT_FOUND;
    const unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const unsigned w = BOARD_LCD_H_RES / div, h = BOARD_LCD_V_RES / div;
    ui_scanout_t *shot = heap_caps_malloc(sizeof(*shot) + w * sizeof(uint16_t), caps);
    if (!shot) return ESP_ERR_NO_MEM;
    shot->first = NULL;
    shot->width = w;
    scanout_row_t **tail = &shot->first;
    for (unsigned y = 0; y < h; y++) {
        const uint16_t *src = frame + (size_t)y * div * BOARD_LCD_H_RES;
        unsigned left = 1, end = w;
        const uint16_t first = src[0], last = src[(w - 1) * div];
        while (left < w && src[left * div] == first) left++;
        while (end > left && src[(end - 1) * div] == last) end--;
        unsigned middle = end - left;
        scanout_row_t *row = heap_caps_malloc(sizeof(*row) + middle * sizeof(uint16_t), caps);
        if (!row) {
            ui_scanout_free(shot);
            return ESP_ERR_NO_MEM;
        }
        row->next = NULL;
        row->left_count = left;
        row->middle_count = middle;
        row->left_color = first;
        row->right_color = last;
        if (div == 1) {
            memcpy(row->middle, src + left, middle * sizeof(uint16_t));
        } else {
            for (unsigned x = 0; x < middle; x++) row->middle[x] = src[(left + x) * div];
        }
        *tail = row;
        tail = &row->next;
    }
    *out = shot;
    return ESP_OK;
}

void ui_scanout_send(ui_scanout_t *shot, ui_platform_row_cb_t cb, void *ctx)
{
    unsigned y = 0;
    for (const scanout_row_t *row = shot->first; row; row = row->next) {
        unsigned x = 0;
        for (; x < row->left_count; x++) shot->line[x] = row->left_color;
        memcpy(shot->line + x, row->middle, row->middle_count * sizeof(uint16_t));
        x += row->middle_count;
        for (; x < shot->width; x++) shot->line[x] = row->right_color;
        cb(shot->line, shot->width, y++, ctx);
    }
}
