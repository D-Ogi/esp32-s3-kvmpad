// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Per-device TLS identity: generate once, then only read back (TASK-100; TECH-041, SEC-102,
// ADR-028 §3, ADR-032).
//
// The key is generated through PSA (ECDSA P-256), copied into a PK context only long enough to
// write the SEC1 DER and to sign the certificate, and then destroyed in PSA. Both DER blobs are
// handed to the injected store; nothing here knows about NVS.
//
// The certificate is deliberately minimal:
//   * version 3, CN = the device name, self-issued;
//   * a random 8-byte positive serial;
//   * basicConstraints: not a CA;
//   * keyUsage: digitalSignature + keyAgreement;
//   * NO extendedKeyUsage. A certificate with no EKU extension is usable for any purpose, while a
//     present EKU that happens to omit `serverAuth` is rejected outright by Schannel before the
//     CLIENT's validation callback ever runs. Omitting it is therefore the safer of the two.
//   * fixed notBefore / notAfter from kvm_srt_identity.h, because the DEVICE has no RTC.
//
// The whole DER is hashed with SHA-256; that hash is the CLIENT's pin and the channel binding of
// SEC-102, so every byte above is part of what the user approves on the pairing screen.

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "kvm_pairing_pake.h" // kvm_pake_crypto_init(), kvm_pair_sha256()
#include "kvm_srt_server.h"
#include "mbedtls/build_info.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

// The two DER buffers are read by the TLS layer only at start, from a task context, and the DER
// scratch only while a key is being generated -- nothing here is touched from an ISR or by DMA. So
// they belong in PSRAM where there is any: internal RAM is the scarce one on this board.
#ifdef CONFIG_SPIRAM
#define SRT_BSS EXT_RAM_BSS_ATTR
#else
#define SRT_BSS
#endif

static const char *TAG = "srt_id";

// One static identity for the life of the process; the server config points into it.
static SRT_BSS kvm_srt_identity_t s_id;
static bool s_valid;

// ---- device name ---------------------------------------------------------------------------

void kvm_srt_device_name(char *out, size_t cap)
{
    if (out == NULL || cap == 0) {
        return;
    }
    static const char hex[] = "0123456789ABCDEF";
    uint8_t mac[6] = {0};
    // The base MAC is readable before Wi-Fi starts and is stable across reboots and OTA, which is
    // what the advertised name has to be: it is shown next to the pairing code (FR-722, FR-723).
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        (void)esp_efuse_mac_get_default(mac);
    }
    const char prefix[] = "KVMpad-";
    size_t need = sizeof(prefix) - 1u + 4u + 1u;
    if (cap < need) {
        // Still produce something valid rather than a truncated half-name.
        out[0] = 'K';
        if (cap > 1) {
            out[1] = '\0';
        }
        return;
    }
    memcpy(out, prefix, sizeof(prefix) - 1u);
    size_t at = sizeof(prefix) - 1u;
    out[at++] = hex[(mac[4] >> 4) & 0x0Fu];
    out[at++] = hex[mac[4] & 0x0Fu];
    out[at++] = hex[(mac[5] >> 4) & 0x0Fu];
    out[at++] = hex[mac[5] & 0x0Fu];
    out[at] = '\0';
}

// ---- generation ----------------------------------------------------------------------------

static void set_name(kvm_srt_identity_t *id, const char *device_name)
{
    memset(id->device_name, 0, sizeof(id->device_name));
    if (device_name != NULL && device_name[0] != '\0') {
        size_t n = strlen(device_name);
        if (n >= sizeof(id->device_name)) {
            n = sizeof(id->device_name) - 1u;
        }
        memcpy(id->device_name, device_name, n);
    } else {
        kvm_srt_device_name(id->device_name, sizeof(id->device_name));
    }
}

static esp_err_t generate(kvm_srt_identity_t *id)
{
    if (kvm_pake_crypto_init() != KVM_PAIR_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    // EXPORT is required by mbedtls_pk_copy_from_psa(); the key is destroyed again below, so it is
    // exportable for the few milliseconds it takes to write the DER and sign the certificate.
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH |
                                       PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));

    mbedtls_svc_key_id_t key_id = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attr, &key_id) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "key generation failed");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    mbedtls_pk_context pk;
    mbedtls_x509write_cert crt;
    mbedtls_pk_init(&pk);
    mbedtls_x509write_crt_init(&crt);

    uint8_t serial[8];
    char subject[8 + KVM_PAIR_NAME_CAP];
    // One scratch buffer for both DER writes; mbedtls writes at the END of the buffer.
    static SRT_BSS uint8_t scratch[KVM_SRT_CERT_DER_MAX];

    do {
        if (mbedtls_pk_copy_from_psa(key_id, &pk) != 0) {
            break;
        }

        int n = mbedtls_pk_write_key_der(&pk, scratch, sizeof(scratch));
        if (n <= 0 || (size_t)n > sizeof(id->key_der)) {
            ESP_LOGE(TAG, "private key does not fit (%d)", n);
            break;
        }
        memcpy(id->key_der, scratch + sizeof(scratch) - (size_t)n, (size_t)n);
        id->key_len = (size_t)n;

        esp_fill_random(serial, sizeof(serial));
        serial[0] = (uint8_t)(serial[0] & 0x7Fu); // a DER INTEGER must be positive
        if (serial[0] == 0) {
            serial[0] = 1;
        }

        int wrote = snprintf(subject, sizeof(subject), "CN=%s", id->device_name);
        if (wrote <= 0 || (size_t)wrote >= sizeof(subject)) {
            break;
        }

        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &pk);
        mbedtls_x509write_crt_set_issuer_key(&crt, &pk);
        if (mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial)) != 0 ||
            mbedtls_x509write_crt_set_subject_name(&crt, subject) != 0 ||
            mbedtls_x509write_crt_set_issuer_name(&crt, subject) != 0 ||
            mbedtls_x509write_crt_set_validity(&crt, KVM_SRT_CERT_NOT_BEFORE,
                                               KVM_SRT_CERT_NOT_AFTER) != 0 ||
            mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1) != 0 ||
            mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                                                          MBEDTLS_X509_KU_KEY_AGREEMENT) != 0) {
            break;
        }
#if defined(PSA_WANT_ALG_SHA_1)
        // Both identifiers are SHA-1 based in mbedTLS and are cosmetic for a self-signed leaf, so
        // they are added when available and skipped when SHA-1 is configured out.
        (void)mbedtls_x509write_crt_set_subject_key_identifier(&crt);
        (void)mbedtls_x509write_crt_set_authority_key_identifier(&crt);
#endif

        n = mbedtls_x509write_crt_der(&crt, scratch, sizeof(scratch));
        if (n <= 0 || (size_t)n > sizeof(id->cert_der)) {
            ESP_LOGE(TAG, "certificate does not fit (%d)", n);
            break;
        }
        memcpy(id->cert_der, scratch + sizeof(scratch) - (size_t)n, (size_t)n);
        id->cert_len = (size_t)n;

        if (kvm_pair_sha256(id->cert_der, id->cert_len, id->cert_sha256) != KVM_PAIR_OK) {
            break;
        }
        id->generated = true;
        err = ESP_OK;
    } while (0);

    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&pk);
    psa_destroy_key(key_id);
    mbedtls_platform_zeroize(scratch, sizeof(scratch));
    mbedtls_platform_zeroize(serial, sizeof(serial));
    if (err != ESP_OK) {
        mbedtls_platform_zeroize(id, sizeof(*id));
    }
    return err;
}

// ---- validation of what was read back ------------------------------------------------------

// A stored blob pair is only accepted when both parse and the public keys agree. Serving a
// certificate whose key does not match would fail every handshake with no useful diagnosis.
static bool blobs_are_consistent(const kvm_srt_identity_t *id)
{
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    mbedtls_x509_crt_init(&crt);
    mbedtls_pk_init(&pk);

    bool ok = false;
    if (mbedtls_x509_crt_parse(&crt, id->cert_der, id->cert_len) == 0 &&
        mbedtls_pk_parse_key(&pk, id->key_der, id->key_len, NULL, 0) == 0) {
        ok = mbedtls_pk_check_pair(&crt.pk, &pk) == 0;
    }

    mbedtls_pk_free(&pk);
    mbedtls_x509_crt_free(&crt);
    return ok;
}

static bool load(const kvm_srt_identity_store_t *store, kvm_srt_identity_t *id)
{
    id->cert_len = 0;
    id->key_len = 0;
    if (!store->load(store->ctx, KVM_SRT_IDENTITY_BLOB_CERT, id->cert_der, sizeof(id->cert_der),
                     &id->cert_len) ||
        !store->load(store->ctx, KVM_SRT_IDENTITY_BLOB_KEY, id->key_der, sizeof(id->key_der),
                     &id->key_len)) {
        return false;
    }
    return id->cert_len != 0 && id->key_len != 0;
}

static bool save(const kvm_srt_identity_store_t *store, const kvm_srt_identity_t *id)
{
    return store->save(store->ctx, KVM_SRT_IDENTITY_BLOB_CERT, id->cert_der, id->cert_len) &&
           store->save(store->ctx, KVM_SRT_IDENTITY_BLOB_KEY, id->key_der, id->key_len);
}

// ---- public entry points -------------------------------------------------------------------

static esp_err_t init_common(const kvm_srt_identity_store_t *store, const char *device_name,
                             bool force_new, const kvm_srt_identity_t **out)
{
    if (store == NULL || store->load == NULL || store->save == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (s_valid && !force_new) {
        *out = &s_id;
        return ESP_OK;
    }

    mbedtls_platform_zeroize(&s_id, sizeof(s_id));
    s_valid = false;
    set_name(&s_id, device_name);

    if (!force_new && load(store, &s_id)) {
        if (!blobs_are_consistent(&s_id)) {
            ESP_LOGE(TAG, "stored identity does not parse");
            mbedtls_platform_zeroize(s_id.key_der, sizeof(s_id.key_der));
            s_id.cert_len = 0;
            s_id.key_len = 0;
            // Deliberately not regenerated here: a new certificate silently invalidates every
            // CLIENT pin, so that decision belongs to the owner through
            // kvm_srt_identity_regenerate() (ADR-028 §3).
            return ESP_ERR_INVALID_STATE;
        }
        if (kvm_pair_sha256(s_id.cert_der, s_id.cert_len, s_id.cert_sha256) != KVM_PAIR_OK) {
            return ESP_ERR_INVALID_STATE;
        }
        s_id.generated = false;
        s_valid = true;
        *out = &s_id;
        ESP_LOGI(TAG, "identity loaded (%u B cert)", (unsigned)s_id.cert_len);
        return ESP_OK;
    }

    esp_err_t err = generate(&s_id);
    if (err != ESP_OK) {
        return err;
    }
    // A certificate that is not persisted would change on the next boot and break every pin, so a
    // failed write is fatal rather than something to run with.
    if (!save(store, &s_id)) {
        ESP_LOGE(TAG, "identity could not be persisted");
        mbedtls_platform_zeroize(&s_id, sizeof(s_id));
        return ESP_FAIL;
    }
    set_name(&s_id, device_name); // generate() keeps it, but be explicit about the invariant
    s_valid = true;
    *out = &s_id;
    ESP_LOGI(TAG, "identity generated (%u B cert)", (unsigned)s_id.cert_len);
    return ESP_OK;
}

esp_err_t kvm_srt_identity_init(const kvm_srt_identity_store_t *store, const char *device_name,
                                const kvm_srt_identity_t **out)
{
    return init_common(store, device_name, false, out);
}

esp_err_t kvm_srt_identity_regenerate(const kvm_srt_identity_store_t *store,
                                      const char *device_name, const kvm_srt_identity_t **out)
{
    return init_common(store, device_name, true, out);
}
