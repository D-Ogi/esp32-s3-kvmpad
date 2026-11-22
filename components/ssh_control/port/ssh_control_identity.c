// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ssh_control_identity.h"
#include "nvs.h"
#include <string.h>

// One NVS value holds a versioned header, little-endian CRC32 and the unencrypted
// OpenSSH Ed25519 private key text. NVS owns at-rest protection.
enum { HEADER = 8, TEXT_CAP = 512 };
static const char store_name[] = "kvmpad_ssh";
static const char record_name[] = "host_v1";
static const uint8_t magic[4] = {'K', 'S', 'H', 1};

static void wipe(void *buf, size_t n)
{
    volatile uint8_t *p = buf;
    while (n--) *p++ = 0;
}

static void checksum(const uint8_t *data, size_t len, uint8_t out[4])
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0);
    }
    crc = ~crc;
    for (unsigned i = 0; i < 4; ++i) out[i] = (uint8_t)(crc >> (8 * i));
}

static bool create_record(uint8_t *record, size_t capacity, size_t *size)
{
    ssh_key generated = NULL;
    char *text = NULL;
    size_t len = 0;
    bool ok = false;
    if (ssh_pki_generate_key(SSH_KEYTYPE_ED25519, NULL, &generated) != SSH_OK ||
        ssh_pki_export_privkey_base64_format(generated, NULL, NULL, NULL, &text,
                                             SSH_FILE_FORMAT_OPENSSH) != SSH_OK)
        goto done;
    len = strlen(text);
    if (!len || len > capacity - HEADER) goto done;
    memcpy(record, magic, sizeof(magic));
    checksum((const uint8_t *)text, len, record + sizeof(magic));
    memcpy(record + HEADER, text, len);
    nvs_handle_t store;
    if (nvs_open(store_name, NVS_READWRITE, &store) != ESP_OK) goto done;
    ok = nvs_set_blob(store, record_name, record, HEADER + len) == ESP_OK &&
         nvs_commit(store) == ESP_OK;
    nvs_close(store);
    if (!ok) goto done;
    // Return only a key read from the committed record. Check the secret bytes too.
    wipe(record, capacity);
    *size = capacity;
    ok = nvs_open(store_name, NVS_READONLY, &store) == ESP_OK;
    if (ok) {
        ok = nvs_get_blob(store, record_name, record, size) == ESP_OK &&
             *size == HEADER + len && memcmp(record + HEADER, text, len) == 0;
        nvs_close(store);
    }
done:
    if (text) {
        wipe(text, len);
        ssh_string_free_char(text);
    }
    ssh_key_free(generated);
    return ok;
}

bool kvm_ssh_identity_open_nvs(ssh_key *key, uint8_t sha256[32])
{
    if (key) *key = NULL;
    if (sha256) memset(sha256, 0, 32);
    if (!key || !sha256) return false;
    uint8_t record[HEADER + TEXT_CAP + 1];
    const size_t cap = sizeof(record) - 1; // Extra byte for the libssh C string.
    size_t size = cap;
    ssh_key loaded = NULL;
    unsigned char *hash = NULL;
    size_t hash_len = 0;
    bool ok = false;
    nvs_handle_t store;
    esp_err_t rc = nvs_open(store_name, NVS_READONLY, &store);
    if (rc == ESP_ERR_NVS_NOT_FOUND) {
        if (!create_record(record, cap, &size)) goto done;
    } else {
        if (rc != ESP_OK) goto done;
        // NVS may discard a damaged blob. Its dedicated namespace survives, so a
        // missing record here must never be treated as first provisioning.
        rc = nvs_get_blob(store, record_name, record, &size);
        nvs_close(store);
        if (rc != ESP_OK) goto done;
    }
    if (size <= HEADER || size > cap || memcmp(record, magic, sizeof(magic)) != 0 ||
        memchr(record + HEADER, 0, size - HEADER)) goto done;
    uint8_t expected[4];
    checksum(record + HEADER, size - HEADER, expected);
    if (memcmp(record + sizeof(magic), expected, sizeof(expected)) != 0) goto done;
    record[size] = 0;
    if (ssh_pki_import_privkey_base64((const char *)record + HEADER, NULL, NULL, NULL, &loaded) != SSH_OK ||
        !ssh_key_is_private(loaded) || ssh_key_type(loaded) != SSH_KEYTYPE_ED25519 ||
        ssh_get_publickey_hash(loaded, SSH_PUBLICKEY_HASH_SHA256, &hash, &hash_len) != SSH_OK ||
        hash_len != 32) goto done;
    memcpy(sha256, hash, hash_len);
    *key = loaded;
    loaded = NULL;
    ok = true;
done:
    ssh_clean_pubkey_hash(&hash);
    ssh_key_free(loaded);
    wipe(record, sizeof(record));
    return ok;
}
