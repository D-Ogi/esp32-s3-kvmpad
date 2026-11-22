// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "gud_panel.h"
#include "gud_view.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <sys/param.h>

typedef struct {
    uint8_t staging[GUD_R1_MAX_BYTES];
    uint8_t framebuffer[GUD_FRAME_BYTES];
} surface_t;

static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static surface_t *surface;
static gud_receiver_t receiver;
static gud_rect_t dirty;
static bool received, visibility_changed;

static void mark_dirty(gud_rect_t rect)
{
    if (!dirty.width) {
        dirty = rect;
        return;
    }
    int right = MAX(dirty.x + dirty.width, rect.x + rect.width);
    int bottom = MAX(dirty.y + dirty.height, rect.y + rect.height);
    dirty.x = MIN(dirty.x, rect.x);
    dirty.y = MIN(dirty.y, rect.y);
    dirty.width = right - dirty.x;
    dirty.height = bottom - dirty.y;
}

static void update_pixels(void *context, gud_rect_t rect, const uint8_t *rows)
{
    surface_t *target = context;
    const unsigned input_stride = (rect.width + 7u) / 8u;
    for (unsigned y = 0; y < rect.height; ++y, rows += input_stride) {
        uint8_t *row = target->framebuffer + (rect.y + y) * (receiver.width / 8);
        if ((rect.x & 7u) == 0 && (rect.width & 7u) == 0) {
            memcpy(row + rect.x / 8, rows, input_stride);
        } else {
            for (unsigned x = 0; x < rect.width; ++x) {
                unsigned at = rect.x + x;
                uint8_t mask = 0x80u >> (at & 7u);
                if (rows[x / 8] & (0x80u >> (x & 7u))) row[at / 8] |= mask;
                else row[at / 8] &= (uint8_t)~mask;
            }
        }
    }
    received = true;
    mark_dirty(rect);
}

// The UI creates the mutex before requesting the USB profile. It survives
// view destruction so a delayed reset or transfer can safely see a closed surface.
bool gud_surface_open(void)
{
    if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_storage);
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = false;
    if (!surface) {
        surface = heap_caps_calloc(1, sizeof(*surface), MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
        if (surface) {
            ok = gud_receiver_init(&receiver, surface->staging, sizeof(surface->staging), update_pixels, surface);
            dirty = (gud_rect_t){0};
            received = visibility_changed = false;
        }
    }
    xSemaphoreGive(lock);
    return ok;
}

void gud_surface_close(void)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    surface_t *old = surface;
    surface = NULL;
    receiver = (gud_receiver_t){0};
    xSemaphoreGive(lock);
    heap_caps_free(old);
}

void gud_panel_reset(void)
{
    if (!lock) return;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (surface) {
        gud_receiver_init(&receiver, surface->staging, sizeof(surface->staging), update_pixels, surface);
        memset(surface->framebuffer, 0, sizeof(surface->framebuffer));
        dirty = (gud_rect_t){0, 0, GUD_WIDTH, GUD_HEIGHT};
        received = false;
        visibility_changed = true;
    }
    xSemaphoreGive(lock);
}

int gud_panel_get(uint8_t request, uint16_t value, uint8_t *reply, size_t capacity)
{
    if (!lock) return -1;
    xSemaphoreTake(lock, portMAX_DELAY);
    int result = surface ? gud_receiver_get(&receiver, request, value, reply, capacity) : -1;
    xSemaphoreGive(lock);
    return result;
}

bool gud_panel_set(uint8_t request, uint16_t value, const uint8_t *data, size_t length)
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool before = receiver.controller_enabled && receiver.display_enabled;
    uint16_t previous_width = receiver.width;
    bool ok = surface && gud_receiver_set(&receiver, request, value, data, length);
    if (ok && previous_width != receiver.width) {
        memset(surface->framebuffer, 0, sizeof(surface->framebuffer));
        dirty = (gud_rect_t){0, 0, receiver.width, receiver.height};
        received = false;
        visibility_changed = true;
    }
    if (before != (receiver.controller_enabled && receiver.display_enabled))
        visibility_changed = true;
    xSemaphoreGive(lock);
    return ok;
}

bool gud_panel_bulk(const uint8_t *data, size_t length)
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool ok = surface && gud_receiver_bulk(&receiver, data, length);
    xSemaphoreGive(lock);
    return ok;
}

bool gud_surface_read(uint8_t *destination, gud_rect_t *changed, gud_view_state_t *state,
                      uint16_t *width, uint16_t *height)
{
    if (!lock) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool pending = surface && (dirty.width || visibility_changed);
    if (pending) {
        if (!received) *state = GUD_VIEW_WAITING;
        else if (receiver.controller_enabled && receiver.display_enabled) *state = GUD_VIEW_PICTURE;
        else *state = GUD_VIEW_BLANK;
        *changed = dirty;
        *width = receiver.width ? receiver.width : GUD_WIDTH;
        *height = receiver.height ? receiver.height : GUD_HEIGHT;
        unsigned first = dirty.x / 8;
        unsigned end = (dirty.x + dirty.width + 7u) / 8;
        for (unsigned y = dirty.y; y < dirty.y + dirty.height; ++y) {
            unsigned offset = y * (*width / 8) + first;
            memcpy(destination + offset, surface->framebuffer + offset, end - first);
        }
        dirty = (gud_rect_t){0};
        visibility_changed = false;
    }
    xSemaphoreGive(lock);
    return pending;
}
