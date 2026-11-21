// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_port.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "miniz.h"
#include "psa/crypto.h"
#include <zlib.h>

void *pack_alloc(size_t bytes) { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

void pack_free(void *allocation) { heap_caps_free(allocation); }
uint32_t pack_crc(const uint8_t *data, size_t len) { return esp_rom_crc32_le(0, data, len); }

bool pack_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    size_t used = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_256, data, len, out, 32, &used) == PSA_SUCCESS && used == 32;
}

#ifdef KVM_PACK_HAS_PUBLISHERS
bool pack_sha512(const uint8_t *data, size_t size, uint8_t out[64])
{
    size_t used = 0;
    return psa_crypto_init() == PSA_SUCCESS &&
           psa_hash_compute(PSA_ALG_SHA_512, data, size, out, 64, &used) == PSA_SUCCESS && used == 64;
}
#endif

static esp_err_t inflate_exact(tinfl_decompressor *inflater, const uint8_t *data, size_t compressed,
                               uint8_t *out, size_t size)
{
    tinfl_init(inflater);
    size_t in = compressed, n = size;
    bool ok = tinfl_decompress(inflater, data, &in, out, out, &n, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) ==
                  TINFL_STATUS_DONE &&
              in == compressed && n == size;
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t pack_inflate(const uint8_t *data, size_t compressed, uint8_t *out, size_t size)
{
    tinfl_decompressor *inflater = pack_alloc(sizeof(*inflater));
    if (!inflater)
        return ESP_ERR_NO_MEM;
    esp_err_t result = inflate_exact(inflater, data, compressed, out, size);
    pack_free(inflater);
    return result;
}

// One complete raw DEFLATE stream per band. With Z_FINISH zlib uses the
// caller's output as its history and needs just its 7120-byte S3 inflate state.
typedef struct {
    _Alignas(max_align_t) uint8_t state[7168];
    bool used;
} band_workspace_t;

static voidpf band_alloc(voidpf opaque, uInt items, uInt size)
{
    band_workspace_t *work = opaque;
    if (work->used || !items || !size || size > sizeof(work->state) / items)
        return NULL;
    work->used = true;
    return work->state;
}

static void band_free(voidpf opaque, voidpf address)
{
    (void)opaque;
    (void)address;
}

esp_err_t pack_inflate_band(const uint8_t *data, size_t compressed, uint8_t *out, size_t size)
{
    band_workspace_t work;
    work.used = false;
    z_stream z = {.zalloc = band_alloc, .zfree = band_free, .opaque = &work};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return ESP_ERR_NO_MEM;
    z.next_in = (Bytef *)data;
    z.avail_in = compressed;
    z.next_out = out;
    z.avail_out = size;
    bool ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_in == compressed && z.total_out == size;
    inflateEnd(&z);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
