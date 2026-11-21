// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_png.h"
#include "pack_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned w, h, count; uint8_t *pixels, *seen; bool valid; } output_t;

static void span(void *ctx, unsigned x, unsigned y, unsigned step, const uint8_t *rgba, unsigned n)
{
    output_t *out = ctx;
    for (unsigned i = 0; i < n; ++i, x += step) {
        if (x >= out->w || y >= out->h) { out->valid = false; return; }
        size_t k = y * out->w + x;
        if (out->seen[k]++) out->valid = false;
        memcpy(out->pixels + 4*k, rgba + 4*i, 4);
        ++out->count;
    }
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "icon")) {
        pack_candidate_t candidate = {0};
        if (pack_load(argv[2], &candidate) != ESP_OK) return 1;
        const pack_icon_t *icon = candidate.scene->icons;
        unsigned uses = 0;
        for (unsigned i = 0; icon && i < candidate.scene->widgets; i++)
            uses += candidate.scene->widget[i].icon_member == icon->member;
        bool ok = icon && !icon->next && uses == 2 && icon->width == atoi(argv[3]) && icon->height == atoi(argv[4]);
        if (ok) {
            FILE *f = fopen(argv[5], "wb");
            size_t pixels = (size_t)icon->width * icon->height;
            ok = f && fwrite(icon->bgra, 4, pixels, f) == pixels && !fclose(f);
        }
        pack_dispose(&candidate);
        return ok ? 0 : 1;
    }
    if (argc == 3 && !strcmp(argv[1], "file")) {
        FILE *in = fopen(argv[2], "r+b");
        if (!in || fseek(in, 5, SEEK_SET)) return 2;
        pack_candidate_t candidate = {0};
        bool ok = pack_load_file(in, &candidate) == ESP_OK;
        pack_dispose(&candidate);
        ok = ok && fseek(in, 0, SEEK_SET) == 0 && fgetc(in) == 'P';
        fclose(in);
        return ok ? 0 : 1;
    }
    if (argc != 5 && argc != 6) return 2;
    unsigned w = (unsigned)atoi(argv[2]), h = (unsigned)atoi(argv[3]);
    if (!w || !h || w > 1024 || h > 568) return 2;
    FILE *in = fopen(argv[1], "rb");
    if (!in || fseek(in, 0, SEEK_END)) return 2;
    long n = ftell(in);
    if (n <= 0 || n > 2*1024*1024 || fseek(in, 0, SEEK_SET)) return 2;
    uint8_t *bytes = malloc((size_t)n);
    output_t out = {w, h, 0, calloc(w*h, 4), calloc(w*h, 1), true};
    if (!bytes || !out.pixels || !out.seen || fread(bytes, 1, (size_t)n, in) != (size_t)n) return 2;
    fclose(in);
    size_t budget = argc == 6 ? (size_t)strtoul(argv[5], NULL, 10) : 128u * 1024u;
    esp_err_t decoded = pack_png_decode(bytes, (size_t)n, budget, span, &out);
    bool ok = decoded == ESP_OK && out.valid && out.count == w*h;
    if (ok) {
        FILE *f = fopen(argv[4], "wb");
        ok = f && fwrite(out.pixels, 4, w*h, f) == w*h && !fclose(f);
    }
    free(out.pixels);
    free(out.seen);
    free(bytes);
    if (decoded == ESP_ERR_NO_MEM) return 2;
    return ok ? 0 : 1;
}
