// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_port.h"
#include "pack_card.h"
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <openssl/evp.h>
#include <stdlib.h>
#include <zlib.h>

bool (*pack_host_fail_alloc)(size_t);
void *pack_alloc(size_t bytes)
{
    return pack_host_fail_alloc && pack_host_fail_alloc(bytes) ? NULL : malloc(bytes);
}
void pack_free(void *allocation) { free(allocation); }
uint32_t pack_crc(const uint8_t *p, size_t n) { return crc32(0, p, n); }
bool (*pack_host_fail_hash)(const uint8_t *, size_t, unsigned);
bool pack_sha256(const uint8_t *data, size_t n, uint8_t out[32])
{
    unsigned len;
    if (pack_host_fail_hash && pack_host_fail_hash(data, n, 256)) return false;
    return EVP_Digest(data, n, out, &len, EVP_sha256(), NULL) == 1 && len == 32;
}
#ifdef KVM_PACK_HAS_PUBLISHERS
bool pack_sha512(const uint8_t *data, size_t n, uint8_t out[64])
{
    unsigned len;
    if (pack_host_fail_hash && pack_host_fail_hash(data, n, 512)) return false;
    return EVP_Digest(data, n, out, &len, EVP_sha512(), NULL) == 1 && len == 64;
}
#endif
esp_err_t pack_inflate(const uint8_t *data, size_t in, uint8_t *out, size_t size)
{
    z_stream z = {0};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK)
        return ESP_ERR_NO_MEM;
    z.next_in = (Bytef *)data;
    z.avail_in = (uInt)in;
    z.next_out = out;
    z.avail_out = (uInt)size;
    bool ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_in == in && z.total_out == size;
    inflateEnd(&z);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}
bool pack_card_present(void)
{
    struct stat st;
    return stat(PACK_MOUNT, &st) == 0 && S_ISDIR(st.st_mode);
}

bool pack_card_read_volume(pack_card_volume_t *volume, uint64_t *free_bytes)
{
    if (!pack_card_present()) return false;
    if (free_bytes) *free_bytes = 0;
    *volume = (pack_card_volume_t){.mount_generation = 1};
    FILE *file = fopen(PACK_MOUNT "/volume-id", "rb");
    if (!file) return errno == ENOENT;
    bool ok = fread(&volume->serial, 1, sizeof(volume->serial), file) == sizeof(volume->serial);
    return fclose(file) == 0 && ok;
}

// A host fixture can move the mount after the uninstall tombstone reaches disk.
const char *pack_host_swap_after_tombstone;

bool pack_card_same_volume(const pack_card_volume_t *volume)
{
    struct stat st;
    if (pack_host_swap_after_tombstone && stat(pack_host_swap_after_tombstone, &st) == 0) {
        pack_host_swap_after_tombstone = NULL;
        if (rename("card", "first") || rename("second", "card"))
            return false;
    }
    pack_card_volume_t current;
    return pack_card_read_volume(&current, NULL) && current.mount_generation == volume->mount_generation &&
           current.serial == volume->serial;
}

esp_err_t pack_inflate_band(const uint8_t *data, size_t in, uint8_t *out, size_t size)
{
    return pack_inflate(data, in, out, size);
}
