// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_store.h"
#include "pack_archive.h"
#include "pack_port.h"
#include "pack_publisher.h"
#include "pack_png.h"

#include <stdio.h>
#include <errno.h>
#include <string.h>

void pack_dispose(pack_candidate_t *c)
{
    if (c->scene) {
        for (unsigned i=0;i<PACK_CHARTS;i++) pack_free(c->scene->charts[i].runs);
        pack_icon_t *icon = c->scene->icons;
        while (icon) {
            pack_icon_t *next = icon->next;
            pack_free(icon);
            icon = next;
        }
    }
    pack_free(c->scene);
    pack_free(c->bytes);
    *c = (pack_candidate_t){0};
}

static esp_err_t unpack(const pack_member_t *m, uint8_t **bytes)
{
    uint8_t *out = pack_alloc(m->size);
    if (!out)
        return ESP_ERR_NO_MEM;
    esp_err_t status = pack_inflate(m->data, m->compressed, out, m->size);
    if (status == ESP_OK && pack_crc(out, m->size) != m->crc)
        status = ESP_ERR_INVALID_RESPONSE;
    if (status != ESP_OK) {
        pack_free(out);
        return status;
    }
    *bytes = out;
    return ESP_OK;
}


static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool inventory(const kvm_json_doc_t *doc, pack_archive_t *archive)
{
    unsigned payloads = PACK_INTEGRITY + archive->end - PACK_ASSETS;
    if (kvm_json_type(doc, 0) != KVM_JSON_OBJECT || kvm_json_count(doc, 0) != payloads)
        return false;
    for (unsigned i = 0; i < archive->end; ++i) {
        if (i == PACK_SIGNATURE || i == PACK_INTEGRITY) continue;
        pack_member_t *m = &archive->member[i];
        int row = kvm_json_member(doc, 0, m->name);
        int64_t size = 0;
        char hex[65];
        if (kvm_json_type(doc, row) != KVM_JSON_OBJECT || kvm_json_count(doc, row) != 2 ||
            !kvm_json_get_i64(doc, kvm_json_member(doc, row, "size"), &size) || size != (int64_t)m->size ||
            kvm_json_str_copy(doc, kvm_json_member(doc, row, "sha256"), hex, sizeof(hex)) != 64)
            return false;
        for (unsigned k = 0; k < 32; ++k) {
            int hi = nibble(hex[k*2]), lo = nibble(hex[k*2+1]);
            if (hi < 0 || lo < 0) return false;
            m->digest[k] = (uint8_t)((hi << 4) | lo);
        }
    }
    return true;
}

static esp_err_t document(pack_archive_t *a, pack_scene_t *scene, unsigned id, const uint8_t *data)
{
    size_t size = a->member[id].size;
    size_t capacity = size / 2 + 2;
    if (capacity > 8192) capacity = 8192;
    kvm_json_tok_t *tokens = pack_alloc(capacity * sizeof(*tokens));
    if (!tokens) return ESP_ERR_NO_MEM;
    kvm_json_doc_t doc;
    bool ok = kvm_json_parse_local(&doc, (const char *)data, size, tokens, capacity, 24) == KVM_JSON_OK;
    if (ok) {
        switch (id) {
        case PACK_INTEGRITY: ok = inventory(&doc, a); break;
        case PACK_MANIFEST: ok = pack_scene_manifest(scene, &doc); break;
        case PACK_UI: ok = pack_scene_ui_assets(scene, &doc, a); break;
        case PACK_BINDINGS: ok = pack_scene_bindings(scene, &doc); break;
        default: ok = false;
        }
    }
    pack_free(tokens);
    return ok ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

typedef struct {
    pack_icon_t *icon;
    unsigned width, height;
} thumbnail_t;

static void thumbnail_span(void *ctx, unsigned x, unsigned y, unsigned step, const uint8_t *rgba, unsigned count)
{
    const thumbnail_t *src = ctx;
    pack_icon_t *icon = src->icon;
    unsigned dy = (y * icon->height + src->height - 1) / src->height;
    if (dy >= icon->height || dy * src->height / icon->height != y) return;
    for (unsigned dx = 0; dx < icon->width; dx++) {
        unsigned sx = dx * src->width / icon->width;
        if (sx < x || (sx-x) % step || (sx-x)/step >= count) continue;
        const uint8_t *p = rgba + 4*((sx-x)/step);
        uint8_t *out = icon->bgra + 4*(dy*icon->width+dx);
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
    }
}

typedef struct { size_t retained, max_workspace; } asset_budget_t;

typedef struct {
    pack_chart_t *chart;
    size_t runs;
    unsigned colors;
    bool invalid;
} chart_encoder_t;

static void chart_row(void *ctx, unsigned x, unsigned y, unsigned step, const uint8_t *rgba, unsigned count)
{
    chart_encoder_t *encoder=ctx;
    pack_chart_t *chart=encoder->chart;
    if (x || step!=1 || count!=chart->width || y>=chart->height) encoder->invalid=true;
    unsigned last=16, length=0;
    for (unsigned i=0;i<count && !encoder->invalid;i++,rgba+=4) {
        uint32_t color=(uint32_t)rgba[3]<<24 | (uint32_t)rgba[0]<<16 | (uint32_t)rgba[1]<<8 | rgba[2];
        unsigned index=last;
        if (index==16 || chart->palette[index]!=color) {
            for (index=0;index<encoder->colors && chart->palette[index]!=color;index++) {}
            if (index==16) { encoder->invalid=true; return; }
            if (index==encoder->colors) chart->palette[encoder->colors++]=color;
        }
        if (index!=last || length==16) {
            encoder->runs++;
            length=0;
        }
        last=index;
        if (chart->runs) {
            if (encoder->runs>chart->run_count) { encoder->invalid=true; return; }
            chart->runs[encoder->runs-1]=(uint8_t)((length<<4)|index);
        }
        length++;
    }
}

static esp_err_t load_chart(pack_chart_t *chart, const uint8_t *data, size_t size,
                            size_t budget, asset_budget_t *used)
{
    if (size<33 || data[16] || data[17] || data[20] || data[21] ||
        data[25]!=3 || data[24]>4 || data[28]!=0) return ESP_ERR_NOT_SUPPORTED;
    chart->width=((unsigned)data[18]<<8)|data[19];
    chart->height=((unsigned)data[22]<<8)|data[23];
    if (!chart->width || !chart->height || chart->width>PACK_CHART_WIDTH || chart->height>PACK_CHART_HEIGHT)
        return ESP_ERR_NOT_SUPPORTED;
    chart_encoder_t encoder={.chart=chart};
    esp_err_t err=pack_png_decode_measured(data,size,budget-used->retained,chart_row,&encoder,&used->max_workspace);
    if (err!=ESP_OK) return err;
    if (encoder.invalid || !encoder.runs || encoder.runs>budget-used->retained-used->max_workspace)
        return ESP_ERR_NOT_SUPPORTED;
    chart->runs=pack_alloc(encoder.runs);
    if (!chart->runs) return ESP_ERR_NO_MEM;
    chart->run_count=encoder.runs;
    used->retained+=encoder.runs;
    encoder.runs=0;
    err=pack_png_decode_measured(data,size,budget-used->retained,chart_row,&encoder,&used->max_workspace);
    if (err!=ESP_OK) return err;
    return !encoder.invalid && encoder.runs==chart->run_count ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t check_asset(pack_scene_t *scene, unsigned id, const uint8_t *data, size_t size, asset_budget_t *used)
{
    size_t budget = (size_t)scene->max_memory_kib * 1024 - sizeof(*scene);
    thumbnail_t src = {0};
    for (unsigned i = 0; i < scene->widgets; ++i) {
        if (scene->widget[i].icon_member != id) continue;
        // Bound the IHDR values before allocating. The decoder checks the entire PNG including its CRCs.
        if (size < 24 || data[16] || data[17] || data[20] || data[21]) return ESP_ERR_NOT_SUPPORTED;
        src.width = ((unsigned)data[18] << 8) | data[19];
        src.height = ((unsigned)data[22] << 8) | data[23];
        if (!src.width || src.width > 1024 || !src.height || src.height > 568) return ESP_ERR_NOT_SUPPORTED;
        unsigned largest = src.width > src.height ? src.width : src.height;
        unsigned w = largest > 32 ? src.width*32/largest : src.width;
        unsigned h = largest > 32 ? src.height*32/largest : src.height;
        if (!w) w = 1;
        if (!h) h = 1;
        size_t bytes = sizeof(pack_icon_t) + 4*w*h;
        if (bytes > budget - used->retained - used->max_workspace) return ESP_ERR_NOT_SUPPORTED;
        src.icon = pack_alloc(bytes);
        if (!src.icon) return ESP_ERR_NO_MEM;
        *src.icon = (pack_icon_t){.next=scene->icons, .width=w, .height=h, .member=id};
        scene->icons = src.icon;
        used->retained += bytes;
        break;
    }
    for (unsigned i=0;i<PACK_CHARTS;i++) {
        if (scene->charts[i].member!=id) continue;
        esp_err_t err=load_chart(&scene->charts[i],data,size,budget,used);
        if (err!=ESP_OK || !src.icon) return err;
    }
    return pack_png_decode_measured(data, size, budget - used->retained, src.icon ? thumbnail_span : NULL, &src,
                                    &used->max_workspace);
}

static esp_err_t check_member(pack_archive_t *a, pack_scene_t *scene, unsigned id, asset_budget_t *used)
{
    const pack_member_t *m = &a->member[id];
    const uint8_t *data = m->data;
    uint8_t *inflated = NULL;
    esp_err_t status = ESP_OK;
    if (m->method == 0) {
        if (pack_crc(data, m->size) != m->crc) return ESP_ERR_INVALID_RESPONSE;
    } else {
        status = unpack(m, &inflated);
        if (status != ESP_OK) return status;
        data = inflated;
    }
    uint8_t digest[32];
    if (id != PACK_INTEGRITY && id != PACK_SIGNATURE) {
        if (!pack_sha256(data, m->size, digest))
            status = ESP_FAIL;
        else if (memcmp(digest, m->digest, 32))
            status = ESP_ERR_INVALID_RESPONSE;
    }
    if (status != ESP_OK) {
        pack_free(inflated);
        return status;
    }
    if (id < PACK_DOCS)
        status = document(a, scene, id, data);
    else if (id >= PACK_ASSETS) {
        status = check_asset(scene, id, data, m->size, used);
    } else {
#ifdef KVM_PACK_HAS_PUBLISHERS
        status = pack_publisher_verify(a, data, scene->publisher_key_sha256);
        scene->publisher_verified = status == ESP_OK;
#else
        status = ESP_ERR_NOT_SUPPORTED;
#endif
    }
    pack_free(inflated);
    return status;
}

static esp_err_t compile(pack_archive_t *a, pack_candidate_t *c)
{
    if (!pack_archive_open(c->bytes, c->len, a)) return ESP_ERR_INVALID_RESPONSE;
#ifndef KVM_PACK_HAS_PUBLISHERS
    if (a->member[PACK_SIGNATURE].data) return ESP_ERR_NOT_SUPPORTED;
#endif
    asset_budget_t used = {0};
    esp_err_t status = check_member(a, c->scene, PACK_INTEGRITY, &used);
    for (unsigned i = 0; i < a->end && status == ESP_OK; ++i) {
        if (i != PACK_INTEGRITY && i != PACK_SIGNATURE)
            status = check_member(a, c->scene, i, &used);
    }
    if (status == ESP_OK && a->member[PACK_SIGNATURE].data)
        status = check_member(a, c->scene, PACK_SIGNATURE, &used);
    return status;
}

esp_err_t pack_load_file(FILE *f, pack_candidate_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    *c = (pack_candidate_t){0};
    if (!f) return ESP_ERR_INVALID_ARG;
    if (fseek(f, 0, SEEK_END)) return ESP_FAIL;
    long size = ftell(f);
    if (size < 0) return ESP_FAIL;
    if (!size || size > PACK_ARCHIVE_MAX) return ESP_ERR_INVALID_SIZE;
    if (fseek(f, 0, SEEK_SET)) return ESP_FAIL;
    c->len = (size_t)size;
    c->bytes = pack_alloc(c->len);
    c->scene = pack_alloc(sizeof(*c->scene));
    if (c->scene) memset(c->scene,0,sizeof(*c->scene));
    pack_archive_t *archive = pack_alloc(sizeof(*archive));
    esp_err_t status = ESP_ERR_NO_MEM;
    if (c->bytes && c->scene && archive) {
        status = ESP_FAIL;
        if (fread(c->bytes, 1, c->len, f) == c->len && fgetc(f) == EOF && !ferror(f) &&
            pack_sha256(c->bytes, c->len, c->digest))
            status = compile(archive, c);
    }
    pack_free(archive);
    if (status != ESP_OK) pack_dispose(c);
    return status;
}

esp_err_t pack_load(const char *path, pack_candidate_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    *c = (pack_candidate_t){0};
    if (!path) return ESP_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    esp_err_t status = pack_load_file(f, c);
    fclose(f);
    return status;
}
