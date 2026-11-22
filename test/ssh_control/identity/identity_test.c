// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ssh_control_identity.h"
#include "nvs.h"
#include <libssh/server.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "check failed at line %d\n", __LINE__); exit(1); } } while (0)

enum fault { NONE, OPEN, CREATE, WRITE, COMMIT, REOPEN, READBACK, DAMAGED_READBACK };
static struct {
    uint8_t staged[1024], saved[1024];
    size_t staged_len, saved_len;
    bool namespace_exists, present, open, writable;
    unsigned writes, commits, reads;
    enum fault fault;
} store;

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *out)
{
    CHECK(!store.open && !strcmp(ns, "kvmpad_ssh"));
    CHECK(mode == NVS_READWRITE || mode == NVS_READONLY);
    if (store.fault == OPEN || (mode == NVS_READWRITE && store.fault == CREATE) ||
        (mode == NVS_READONLY && store.present && store.fault == REOPEN)) return ESP_FAIL;
    if (mode == NVS_READONLY && !store.namespace_exists) return ESP_ERR_NVS_NOT_FOUND;
    store.namespace_exists = true;
    store.writable = mode == NVS_READWRITE;
    store.open = true;
    *out = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *name, void *buf, size_t *len)
{
    CHECK(store.open && handle == 1 && !strcmp(name, "host_v1"));
    ++store.reads;
    if (store.fault == READBACK) return ESP_FAIL;
    if (!store.present) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < store.saved_len) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(buf, store.saved, store.saved_len);
    *len = store.saved_len;
    if (store.fault == DAMAGED_READBACK) ((uint8_t *)buf)[store.saved_len - 1] ^= 1;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *name, const void *buf, size_t len)
{
    CHECK(store.open && store.writable && handle == 1 && !strcmp(name, "host_v1") && len <= sizeof(store.staged));
    ++store.writes;
    if (store.fault == WRITE) return ESP_FAIL;
    memcpy(store.staged, buf, len);
    store.staged_len = len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    CHECK(store.open && store.writable && handle == 1 && store.staged_len);
    ++store.commits;
    if (store.fault == COMMIT) return ESP_FAIL;
    memcpy(store.saved, store.staged, store.staged_len);
    store.saved_len = store.staged_len;
    store.present = true;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    CHECK(store.open && handle == 1);
    store.open = false;
    memset(store.staged, 0, sizeof(store.staged));
    store.staged_len = 0;
}

static void rejected(void)
{
    uint8_t digest[32];
    memset(digest, 0x55, sizeof(digest));
    ssh_key key = (ssh_key)&store;
    CHECK(!kvm_ssh_identity_open_nvs(&key, digest));
    CHECK(!key && !store.open && memcmp(digest, (const uint8_t[32]){0}, 32) == 0);
}

static void roundtrip(void)
{
    uint8_t initial[32], digest[32];
    ssh_key first, next;
    CHECK(kvm_ssh_identity_open_nvs(&first, initial));
    CHECK(store.writes == 1 && store.commits == 1 && store.reads == 1 && !store.open);
    CHECK(ssh_key_is_private(first) && ssh_key_type(first) == SSH_KEYTYPE_ED25519);
    for (unsigned i = 0; i < 10; ++i) {
        CHECK(kvm_ssh_identity_open_nvs(&next, digest));
        CHECK(ssh_key_cmp(first, next, SSH_KEY_CMP_PRIVATE) == 0 && !memcmp(digest, initial, 32));
        ssh_key_free(next);
    }
    CHECK(store.writes == 1 && store.commits == 1 && store.reads == 11);
    char *public_b64 = NULL;
    CHECK(ssh_pki_export_pubkey_base64(first, &public_b64) == SSH_OK);
    printf("PUBLIC %s\nDIGEST ", public_b64);
    for (unsigned i = 0; i < sizeof(initial); ++i) printf("%02x", initial[i]);
    puts("");
    ssh_string_free_char(public_b64);
    ssh_bind bind = ssh_bind_new();
    CHECK(bind && ssh_bind_options_set(bind, SSH_BIND_OPTIONS_IMPORT_KEY, first) == SSH_OK);
    ssh_bind_free(bind);
}

static void damaged_records(void)
{
    const size_t size = store.saved_len;
    unsigned writes = store.writes;
    for (size_t i = 0; i < size; ++i) {
        store.saved[i] ^= 1;
        rejected();
        store.saved[i] ^= 1;
    }
    for (size_t i = 0; i < size; ++i) {
        store.saved_len = i;
        rejected();
    }
    store.saved_len = sizeof(store.saved);
    rejected();
    store.saved_len = size;
    store.fault = READBACK;
    rejected();
    store.fault = NONE;
    static const uint8_t malformed[] = "KSH\x01\x2c\x2e\xb7\x69" "not a private key";
    memcpy(store.saved, malformed, sizeof(malformed) - 1);
    store.saved_len = sizeof(malformed) - 1;
    rejected(); // Valid CRC, invalid key encoding.
    store.present = false;
    rejected(); // NVS can discard invalid blobs while retaining their namespace.
    CHECK(store.writes == writes);
}

static void failed_first_start(void)
{
    for (enum fault f = OPEN; f <= DAMAGED_READBACK; ++f) {
        memset(&store, 0, sizeof(store));
        store.fault = f;
        rejected();
        if (f == OPEN || f == CREATE) CHECK(!store.writes);
        if (f == WRITE) CHECK(store.writes == 1 && store.commits == 0);
        if (f >= COMMIT) CHECK(store.writes == 1 && store.commits == 1);
        unsigned writes = store.writes;
        bool persisted = store.present;
        store.fault = NONE;
        if (store.namespace_exists && !persisted) {
            rejected();
            CHECK(store.writes == writes);
            continue;
        }
        ssh_key key;
        uint8_t digest[32];
        CHECK(kvm_ssh_identity_open_nvs(&key, digest));
        CHECK(store.writes == writes + (persisted ? 0 : 1));
        ssh_key_free(key);
    }
}

int main(void)
{
    CHECK(ssh_init() == SSH_OK);
    roundtrip();
    damaged_records();
    failed_first_start();
    ssh_finalize();
    puts("PASS durable identity, corruption, storage failures and recovery");
    return 0;
}
