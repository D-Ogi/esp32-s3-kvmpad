// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// EC-JPAKE P-256 with channel binding, key confirmation and token sealing (TASK-100, SEC-101/102).
// The derivation chain is documented in kvm_pairing_pake.h and normatively in ADR-029 §3.

#include "kvm_pairing_pake.h"

#include <mbedtls/platform_util.h>
#include <psa/crypto.h>
#include <stdlib.h>
#include <string.h>

#include "kvm_pairing_wire.h"

// --- labels; every one carries the wire version, so a version bump breaks every transcript ---

#define LBL_PW "KVMpad-pair-v1/pw"
#define LBL_TRANSCRIPT "KVMpad-pair-v1"
#define LBL_KC_DEVICE "KVMpad-pair-v1/kc-device"
#define LBL_KC_CLIENT "KVMpad-pair-v1/kc-client"
#define LBL_TOKEN "KVMpad-pair-v1/token"
#define LBL_TOKEN_NONCE "KVMpad-pair-v1/token-nonce"

#define TRANSCRIPT_VERSION 0x01
#define BINDING_VERSION 0x01

#define JPAKE_ALG PSA_ALG_JPAKE(PSA_ALG_SHA_256)
#define JPAKE_PRIMITIVE \
    PSA_PAKE_PRIMITIVE(PSA_PAKE_PRIMITIVE_TYPE_ECC, PSA_ECC_FAMILY_SECP_R1, 256)

// Longest binding blob: version + cert hash + client id + name length + name.
#define BINDING_MAX (1u + KVM_PAIR_SHA256_LEN + KVM_PAIR_CLIENT_ID_LEN + 1u + KVM_PAIR_NAME_CAP)

// Internal progress markers. The order is fixed for both roles.
enum {
    STAGE_INIT = 0,
    STAGE_R1_OUT,
    STAGE_R1_IN,
    STAGE_R2_OUT,
    STAGE_R2_IN,
    STAGE_FINISHED
};

static const psa_pake_step_t k_steps[3] = { PSA_PAKE_STEP_KEY_SHARE, PSA_PAKE_STEP_ZK_PUBLIC,
                                            PSA_PAKE_STEP_ZK_PROOF };

// --- platform randomness -------------------------------------------------------------------------
//
// ESP-IDF builds mbedTLS with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG, so PSA already draws from
// esp_random(). The host tests and the Windows DLL set the same option and route PSA through the
// hook installed here, which is what makes a run reproducible from a fixed seed.

static kvm_pair_platform_t s_platform;
static bool s_platform_set;

void kvm_pake_set_platform(const kvm_pair_platform_t *platform)
{
    if (platform == NULL || platform->random == NULL) {
        memset(&s_platform, 0, sizeof(s_platform));
        s_platform_set = false;
        return;
    }
    s_platform = *platform;
    s_platform_set = true;
}

// Draw randomness through the installed hook. Fails closed when no hook is installed: the core must
// never fall back to a weak source.
static bool platform_random(uint8_t *out, size_t len)
{
    if (!s_platform_set || s_platform.random == NULL) {
        return false;
    }
    return s_platform.random(s_platform.ctx, out, len);
}

bool kvm_pake_platform_random(uint8_t *out, size_t len)
{
    return platform_random(out, len);
}

// --- small helpers -------------------------------------------------------------------------------

void kvm_pair_zeroize(void *p, size_t len)
{
    if (p == NULL || len == 0) {
        return;
    }
    mbedtls_platform_zeroize(p, len);
}

bool kvm_pair_equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

kvm_pair_err_t kvm_pake_crypto_init(void)
{
    static bool done;
    if (done) {
        return KVM_PAIR_OK;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    done = true;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pair_sha256(const uint8_t *in, size_t len, uint8_t out[KVM_PAIR_SHA256_LEN])
{
    if (out == NULL || (in == NULL && len != 0)) {
        return KVM_PAIR_ERR_ARG;
    }
    size_t got = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, in, len, out, KVM_PAIR_SHA256_LEN, &got) != PSA_SUCCESS ||
        got != KVM_PAIR_SHA256_LEN) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    return KVM_PAIR_OK;
}

// HKDF-SHA256 into a raw buffer. `salt` and `info` may be empty.
static kvm_pair_err_t hkdf(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                           const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    kvm_pair_err_t err = KVM_PAIR_ERR_CRYPTO;

    if (psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) != PSA_SUCCESS) {
        goto done;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, salt_len) !=
        PSA_SUCCESS) {
        goto done;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, ikm, ikm_len) !=
        PSA_SUCCESS) {
        goto done;
    }
    if (psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO, info, info_len) !=
        PSA_SUCCESS) {
        goto done;
    }
    if (psa_key_derivation_output_bytes(&op, out, out_len) != PSA_SUCCESS) {
        goto done;
    }
    err = KVM_PAIR_OK;

done:
    psa_key_derivation_abort(&op);
    if (err != KVM_PAIR_OK) {
        kvm_pair_zeroize(out, out_len);
    }
    return err;
}

// --- binding blob --------------------------------------------------------------------------------

// Serialize the channel binding. Returns the length, or 0 when the binding is malformed.
// strnlen is not available everywhere this core builds; the names here are tiny.
static size_t bounded_len(const char *s, size_t cap)
{
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

static size_t binding_encode(const kvm_pake_binding_t *b, uint8_t *out, size_t cap)
{
    size_t name_len = bounded_len(b->client_name, KVM_PAIR_NAME_CAP);
    if (name_len == 0 || name_len >= KVM_PAIR_NAME_CAP) {
        return 0;
    }
    size_t need = 1u + KVM_PAIR_SHA256_LEN + KVM_PAIR_CLIENT_ID_LEN + 1u + name_len;
    if (cap < need) {
        return 0;
    }
    size_t at = 0;
    out[at++] = BINDING_VERSION;
    memcpy(out + at, b->cert_sha256, KVM_PAIR_SHA256_LEN);
    at += KVM_PAIR_SHA256_LEN;
    memcpy(out + at, b->client_id, KVM_PAIR_CLIENT_ID_LEN);
    at += KVM_PAIR_CLIENT_ID_LEN;
    out[at++] = (uint8_t)name_len;
    memcpy(out + at, b->client_name, name_len);
    at += name_len;
    return at;
}

// --- init / free ---------------------------------------------------------------------------------

static psa_pake_operation_t *op_of(kvm_pake_t *p)
{
    return (psa_pake_operation_t *)p->op;
}

void kvm_pake_free(kvm_pake_t *pake)
{
    if (pake == NULL) {
        return;
    }
    if (pake->op != NULL) {
        psa_pake_abort(op_of(pake));
        kvm_pair_zeroize(pake->op, sizeof(psa_pake_operation_t));
        free(pake->op);
        pake->op = NULL;
    }
    if (pake->password_key != 0) {
        psa_destroy_key((psa_key_id_t)pake->password_key);
        pake->password_key = 0;
    }
    if (pake->shared_key != 0) {
        psa_destroy_key((psa_key_id_t)pake->shared_key);
        pake->shared_key = 0;
    }
    kvm_pair_zeroize(pake, sizeof(*pake));
}

kvm_pair_err_t kvm_pake_init(kvm_pake_t *pake, kvm_pake_role_t role, const char *code,
                             const kvm_pake_binding_t *binding)
{
    if (pake == NULL || code == NULL || binding == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (role != KVM_PAKE_ROLE_DEVICE && role != KVM_PAKE_ROLE_CLIENT) {
        return KVM_PAIR_ERR_ARG;
    }
    for (size_t i = 0; i < KVM_PAIR_CODE_LEN; i++) {
        if (code[i] < '0' || code[i] > '9') {
            return KVM_PAIR_ERR_ARG;
        }
    }

    kvm_pair_err_t err = kvm_pake_crypto_init();
    if (err != KVM_PAIR_OK) {
        return err;
    }

    memset(pake, 0, sizeof(*pake));
    pake->role = role;
    pake->binding = *binding;

    uint8_t bind_buf[BINDING_MAX];
    size_t bind_len = binding_encode(&pake->binding, bind_buf, sizeof(bind_buf));
    if (bind_len == 0) {
        kvm_pair_zeroize(pake, sizeof(*pake));
        return KVM_PAIR_ERR_ARG;
    }

    // pw = HKDF-SHA256(salt = LBL_PW, ikm = code, info = binding)
    uint8_t pw[32];
    err = hkdf((const uint8_t *)LBL_PW, sizeof(LBL_PW) - 1, (const uint8_t *)code,
               KVM_PAIR_CODE_LEN, bind_buf, bind_len, pw, sizeof(pw));
    kvm_pair_zeroize(bind_buf, sizeof(bind_buf));
    if (err != KVM_PAIR_OK) {
        kvm_pair_zeroize(pake, sizeof(*pake));
        return err;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, JPAKE_ALG);
    psa_set_key_type(&attr, PSA_KEY_TYPE_PASSWORD);
    psa_key_id_t pwk = 0;
    psa_status_t st = psa_import_key(&attr, pw, sizeof(pw), &pwk);
    kvm_pair_zeroize(pw, sizeof(pw));
    if (st != PSA_SUCCESS) {
        kvm_pair_zeroize(pake, sizeof(*pake));
        return KVM_PAIR_ERR_CRYPTO;
    }
    pake->password_key = (uint32_t)pwk;

    pake->op = calloc(1, sizeof(psa_pake_operation_t));
    if (pake->op == NULL) {
        kvm_pake_free(pake);
        return KVM_PAIR_ERR_CRYPTO;
    }
    *op_of(pake) = (psa_pake_operation_t)PSA_PAKE_OPERATION_INIT;

    psa_pake_cipher_suite_t cs = PSA_PAKE_CIPHER_SUITE_INIT;
    psa_pake_cs_set_algorithm(&cs, JPAKE_ALG);
    psa_pake_cs_set_primitive(&cs, JPAKE_PRIMITIVE);

    if (psa_pake_setup(op_of(pake), pwk, &cs) != PSA_SUCCESS) {
        kvm_pake_free(pake);
        return KVM_PAIR_ERR_CRYPTO;
    }

    // The built-in implementation accepts only these two identities, so the channel binding lives in
    // the password and the transcript instead (SEC-102).
    static const uint8_t id_client[6] = { 'c', 'l', 'i', 'e', 'n', 't' };
    static const uint8_t id_server[6] = { 's', 'e', 'r', 'v', 'e', 'r' };
    const uint8_t *me = (role == KVM_PAKE_ROLE_CLIENT) ? id_client : id_server;
    const uint8_t *peer = (role == KVM_PAKE_ROLE_CLIENT) ? id_server : id_client;
    if (psa_pake_set_user(op_of(pake), me, sizeof(id_client)) != PSA_SUCCESS ||
        psa_pake_set_peer(op_of(pake), peer, sizeof(id_server)) != PSA_SUCCESS) {
        kvm_pake_free(pake);
        return KVM_PAIR_ERR_CRYPTO;
    }

    pake->stage = STAGE_INIT;
    return KVM_PAIR_OK;
}

// --- rounds --------------------------------------------------------------------------------------

// Produce `seqs` key-share/zk-public/zk-proof sequences into a length-prefixed blob.
static kvm_pair_err_t round_out(kvm_pake_t *pake, size_t seqs, uint8_t *out, size_t cap,
                                size_t *out_len)
{
    kvm_pair_blob_t w;
    kvm_pair_blob_begin(&w, out, cap);
    uint8_t chunk[KVM_PAIR_CHUNK_MAX];

    for (size_t s = 0; s < seqs; s++) {
        for (size_t i = 0; i < 3; i++) {
            size_t got = 0;
            if (psa_pake_output(op_of(pake), k_steps[i], chunk, sizeof(chunk), &got) !=
                PSA_SUCCESS) {
                kvm_pair_zeroize(chunk, sizeof(chunk));
                return KVM_PAIR_ERR_CRYPTO;
            }
            if (!kvm_pair_blob_put(&w, chunk, got)) {
                kvm_pair_zeroize(chunk, sizeof(chunk));
                return KVM_PAIR_ERR_CRYPTO;
            }
        }
    }
    kvm_pair_zeroize(chunk, sizeof(chunk));
    *out_len = w.len;
    return KVM_PAIR_OK;
}

// Feed a length-prefixed blob of `seqs` sequences back into the operation.
static kvm_pair_err_t round_in(kvm_pake_t *pake, size_t seqs, const uint8_t *in, size_t len)
{
    const size_t chunks = seqs * 3u;
    const uint8_t *ptr[KVM_PAIR_ROUND1_CHUNKS];
    size_t clen[KVM_PAIR_ROUND1_CHUNKS];

    if (chunks > KVM_PAIR_ROUND1_CHUNKS) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!kvm_pair_blob_split(in, len, chunks, ptr, clen)) {
        return KVM_PAIR_ERR_FRAME;
    }

    for (size_t s = 0; s < seqs; s++) {
        for (size_t i = 0; i < 3; i++) {
            const size_t k = s * 3u + i;
            psa_status_t st = psa_pake_input(op_of(pake), k_steps[i], ptr[k], clen[k]);
            if (st != PSA_SUCCESS) {
                // The pinned implementation validates a whole sequence on its last chunk, so a bad
                // proof surfaces here. Everything that is not a framing problem is reported as a
                // failed run: a wrong code and a tampered blob must look alike.
                return (st == PSA_ERROR_INVALID_ARGUMENT || st == PSA_ERROR_BUFFER_TOO_SMALL)
                           ? KVM_PAIR_ERR_FRAME
                           : KVM_PAIR_ERR_BAD_CODE;
            }
        }
    }
    return KVM_PAIR_OK;
}

// Index of this side / the peer inside the canonical blob store (0 = DEVICE, 1 = CLIENT).
static size_t self_idx(const kvm_pake_t *pake)
{
    return (pake->role == KVM_PAKE_ROLE_DEVICE) ? 0u : 1u;
}

static size_t peer_idx(const kvm_pake_t *pake)
{
    return (pake->role == KVM_PAKE_ROLE_DEVICE) ? 1u : 0u;
}

kvm_pair_err_t kvm_pake_round1_out(kvm_pake_t *pake, uint8_t *out, size_t cap, size_t *out_len)
{
    if (pake == NULL || out == NULL || out_len == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (pake->op == NULL || pake->stage != STAGE_INIT) {
        return KVM_PAIR_ERR_STATE;
    }
    const size_t me = self_idx(pake);
    size_t len = 0;
    kvm_pair_err_t err = round_out(pake, 2, pake->round1[me], KVM_PAIR_ROUND1_MAX, &len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    if (cap < len) {
        return KVM_PAIR_ERR_ARG;
    }
    memcpy(out, pake->round1[me], len);
    pake->round1_len[me] = (uint16_t)len;
    *out_len = len;
    pake->stage = STAGE_R1_OUT;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_round1_in(kvm_pake_t *pake, const uint8_t *in, size_t len)
{
    if (pake == NULL || in == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (pake->op == NULL || pake->stage != STAGE_R1_OUT) {
        return KVM_PAIR_ERR_STATE;
    }
    if (len > KVM_PAIR_ROUND1_MAX) {
        return KVM_PAIR_ERR_FRAME;
    }
    kvm_pair_err_t err = round_in(pake, 2, in, len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    const size_t other = peer_idx(pake);
    memcpy(pake->round1[other], in, len);
    pake->round1_len[other] = (uint16_t)len;
    pake->stage = STAGE_R1_IN;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_round2_out(kvm_pake_t *pake, uint8_t *out, size_t cap, size_t *out_len)
{
    if (pake == NULL || out == NULL || out_len == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (pake->op == NULL || pake->stage != STAGE_R1_IN) {
        return KVM_PAIR_ERR_STATE;
    }
    const size_t me = self_idx(pake);
    size_t len = 0;
    kvm_pair_err_t err = round_out(pake, 1, pake->round2[me], KVM_PAIR_ROUND2_MAX, &len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    if (cap < len) {
        return KVM_PAIR_ERR_ARG;
    }
    memcpy(out, pake->round2[me], len);
    pake->round2_len[me] = (uint16_t)len;
    *out_len = len;
    pake->stage = STAGE_R2_OUT;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_round2_in(kvm_pake_t *pake, const uint8_t *in, size_t len)
{
    if (pake == NULL || in == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (pake->op == NULL || pake->stage != STAGE_R2_OUT) {
        return KVM_PAIR_ERR_STATE;
    }
    if (len > KVM_PAIR_ROUND2_MAX) {
        return KVM_PAIR_ERR_FRAME;
    }
    kvm_pair_err_t err = round_in(pake, 1, in, len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    const size_t other = peer_idx(pake);
    memcpy(pake->round2[other], in, len);
    pake->round2_len[other] = (uint16_t)len;
    pake->stage = STAGE_R2_IN;
    return KVM_PAIR_OK;
}

// --- transcript and key schedule -----------------------------------------------------------------

static kvm_pair_err_t hash_u16_blob(psa_hash_operation_t *h, const uint8_t *blob, size_t len)
{
    const uint8_t be[2] = { (uint8_t)(len >> 8), (uint8_t)(len & 0xffu) };
    if (psa_hash_update(h, be, sizeof(be)) != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    if (psa_hash_update(h, blob, len) != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    return KVM_PAIR_OK;
}

// th = SHA-256(LBL_TRANSCRIPT | 0x01 | binding | u16be|dev_r1 | u16be|cli_r1 | u16be|dev_r2 |
//              u16be|cli_r2). The order is canonical, not local, so both roles hash the same bytes.
static kvm_pair_err_t transcript_compute(kvm_pake_t *pake)
{
    uint8_t bind_buf[BINDING_MAX];
    size_t bind_len = binding_encode(&pake->binding, bind_buf, sizeof(bind_buf));
    if (bind_len == 0) {
        return KVM_PAIR_ERR_ARG;
    }

    psa_hash_operation_t h = PSA_HASH_OPERATION_INIT;
    kvm_pair_err_t err = KVM_PAIR_ERR_CRYPTO;
    const uint8_t ver = TRANSCRIPT_VERSION;

    if (psa_hash_setup(&h, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        goto done;
    }
    if (psa_hash_update(&h, (const uint8_t *)LBL_TRANSCRIPT, sizeof(LBL_TRANSCRIPT) - 1) !=
        PSA_SUCCESS) {
        goto done;
    }
    if (psa_hash_update(&h, &ver, 1) != PSA_SUCCESS) {
        goto done;
    }
    if (psa_hash_update(&h, bind_buf, bind_len) != PSA_SUCCESS) {
        goto done;
    }
    if (hash_u16_blob(&h, pake->round1[0], pake->round1_len[0]) != KVM_PAIR_OK ||
        hash_u16_blob(&h, pake->round1[1], pake->round1_len[1]) != KVM_PAIR_OK ||
        hash_u16_blob(&h, pake->round2[0], pake->round2_len[0]) != KVM_PAIR_OK ||
        hash_u16_blob(&h, pake->round2[1], pake->round2_len[1]) != KVM_PAIR_OK) {
        goto done;
    }

    size_t got = 0;
    if (psa_hash_finish(&h, pake->transcript, sizeof(pake->transcript), &got) != PSA_SUCCESS ||
        got != KVM_PAIR_SHA256_LEN) {
        goto done;
    }
    err = KVM_PAIR_OK;

done:
    psa_hash_abort(&h);
    kvm_pair_zeroize(bind_buf, sizeof(bind_buf));
    return err;
}

kvm_pair_err_t kvm_pake_finish(kvm_pake_t *pake)
{
    if (pake == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (pake->op == NULL || pake->stage != STAGE_R2_IN) {
        return KVM_PAIR_ERR_STATE;
    }

    kvm_pair_err_t err = transcript_compute(pake);
    if (err != KVM_PAIR_OK) {
        return err;
    }

    // The J-PAKE shared secret is the 65-byte point 0x04 || X || Y. It is not uniformly
    // pseudorandom, so it only ever enters a KDF -- never a cipher key directly.
    psa_key_attributes_t sattr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&sattr, PSA_KEY_USAGE_DERIVE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(&sattr, PSA_ALG_HKDF(PSA_ALG_SHA_256));
    psa_set_key_type(&sattr, PSA_KEY_TYPE_DERIVE);
    psa_key_id_t zk = 0;
    if (psa_pake_get_shared_key(op_of(pake), &sattr, &zk) != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    pake->shared_key = (uint32_t)zk;
    // psa_pake_get_shared_key() aborts the operation itself; drop our copy so free() does not retry.
    kvm_pair_zeroize(pake->op, sizeof(psa_pake_operation_t));
    free(pake->op);
    pake->op = NULL;

    uint8_t z[65];
    size_t z_len = 0;
    if (psa_export_key(zk, z, sizeof(z), &z_len) != PSA_SUCCESS || z_len != sizeof(z)) {
        kvm_pair_zeroize(z, sizeof(z));
        return KVM_PAIR_ERR_CRYPTO;
    }
    psa_destroy_key(zk);
    pake->shared_key = 0;

    // prk = HKDF-Extract(salt = th, ikm = Z); each label is its own expand, so a label change
    // cannot be confused with a longer output of another label.
    err = hkdf(pake->transcript, sizeof(pake->transcript), z, z_len,
               (const uint8_t *)LBL_KC_DEVICE, sizeof(LBL_KC_DEVICE) - 1, pake->kc_device,
               sizeof(pake->kc_device));
    if (err == KVM_PAIR_OK) {
        err = hkdf(pake->transcript, sizeof(pake->transcript), z, z_len,
                   (const uint8_t *)LBL_KC_CLIENT, sizeof(LBL_KC_CLIENT) - 1, pake->kc_client,
                   sizeof(pake->kc_client));
    }
    if (err == KVM_PAIR_OK) {
        err = hkdf(pake->transcript, sizeof(pake->transcript), z, z_len,
                   (const uint8_t *)LBL_TOKEN, sizeof(LBL_TOKEN) - 1, pake->k_token,
                   sizeof(pake->k_token));
    }
    if (err == KVM_PAIR_OK) {
        err = hkdf(pake->transcript, sizeof(pake->transcript), z, z_len,
                   (const uint8_t *)LBL_TOKEN_NONCE, sizeof(LBL_TOKEN_NONCE) - 1, pake->n_token,
                   sizeof(pake->n_token));
    }
    kvm_pair_zeroize(z, sizeof(z));
    if (err != KVM_PAIR_OK) {
        return err;
    }

    pake->stage = STAGE_FINISHED;
    pake->finished = true;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_transcript(const kvm_pake_t *pake, uint8_t out[KVM_PAIR_SHA256_LEN])
{
    if (pake == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!pake->finished) {
        return KVM_PAIR_ERR_STATE;
    }
    memcpy(out, pake->transcript, KVM_PAIR_SHA256_LEN);
    return KVM_PAIR_OK;
}

// --- key confirmation ----------------------------------------------------------------------------

// tag = HMAC-SHA256(kc_<role>, th)
static kvm_pair_err_t confirm_tag(const kvm_pake_t *pake, kvm_pake_role_t role,
                                  uint8_t out[KVM_PAIR_CONFIRM_LEN])
{
    const uint8_t *key = (role == KVM_PAKE_ROLE_DEVICE) ? pake->kc_device : pake->kc_client;
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);

    psa_key_id_t k = 0;
    if (psa_import_key(&attr, key, KVM_PAIR_CONFIRM_LEN, &k) != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    size_t got = 0;
    psa_status_t st = psa_mac_compute(k, PSA_ALG_HMAC(PSA_ALG_SHA_256), pake->transcript,
                                      KVM_PAIR_SHA256_LEN, out, KVM_PAIR_CONFIRM_LEN, &got);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS || got != KVM_PAIR_CONFIRM_LEN) {
        kvm_pair_zeroize(out, KVM_PAIR_CONFIRM_LEN);
        return KVM_PAIR_ERR_CRYPTO;
    }
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_confirm_out(kvm_pake_t *pake, uint8_t out[KVM_PAIR_CONFIRM_LEN])
{
    if (pake == NULL || out == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!pake->finished) {
        return KVM_PAIR_ERR_STATE;
    }
    return confirm_tag(pake, pake->role, out);
}

kvm_pair_err_t kvm_pake_confirm_verify(kvm_pake_t *pake, const uint8_t tag[KVM_PAIR_CONFIRM_LEN])
{
    if (pake == NULL || tag == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!pake->finished) {
        return KVM_PAIR_ERR_STATE;
    }
    const kvm_pake_role_t peer =
        (pake->role == KVM_PAKE_ROLE_DEVICE) ? KVM_PAKE_ROLE_CLIENT : KVM_PAKE_ROLE_DEVICE;
    uint8_t want[KVM_PAIR_CONFIRM_LEN];
    kvm_pair_err_t err = confirm_tag(pake, peer, want);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    bool ok = kvm_pair_equal_ct(want, tag, KVM_PAIR_CONFIRM_LEN);
    kvm_pair_zeroize(want, sizeof(want));
    return ok ? KVM_PAIR_OK : KVM_PAIR_ERR_BAD_CODE;
}

// --- token sealing -------------------------------------------------------------------------------

// AAD = LBL_TOKEN | th. Binding the transcript into the AAD means a sealed token from one run can
// never be replayed into another.
static kvm_pair_err_t token_aad(const kvm_pake_t *pake, uint8_t *out, size_t cap, size_t *out_len)
{
    const size_t need = (sizeof(LBL_TOKEN) - 1) + KVM_PAIR_SHA256_LEN;
    if (cap < need) {
        return KVM_PAIR_ERR_ARG;
    }
    memcpy(out, LBL_TOKEN, sizeof(LBL_TOKEN) - 1);
    memcpy(out + sizeof(LBL_TOKEN) - 1, pake->transcript, KVM_PAIR_SHA256_LEN);
    *out_len = need;
    return KVM_PAIR_OK;
}

static kvm_pair_err_t token_key(const kvm_pake_t *pake, psa_key_id_t *out)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    if (psa_import_key(&attr, pake->k_token, sizeof(pake->k_token), out) != PSA_SUCCESS) {
        return KVM_PAIR_ERR_CRYPTO;
    }
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_token_seal(kvm_pake_t *pake, const uint8_t token[KVM_PAIR_TOKEN_LEN],
                                   uint8_t *out, size_t cap, size_t *out_len)
{
    if (pake == NULL || token == NULL || out == NULL || out_len == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!pake->finished) {
        return KVM_PAIR_ERR_STATE;
    }
    if (cap < KVM_PAIR_TOKEN_SEALED_LEN) {
        return KVM_PAIR_ERR_ARG;
    }

    uint8_t aad[64];
    size_t aad_len = 0;
    kvm_pair_err_t err = token_aad(pake, aad, sizeof(aad), &aad_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    psa_key_id_t k = 0;
    err = token_key(pake, &k);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    size_t got = 0;
    psa_status_t st = psa_aead_encrypt(k, PSA_ALG_GCM, pake->n_token, sizeof(pake->n_token), aad,
                                       aad_len, token, KVM_PAIR_TOKEN_LEN, out, cap, &got);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS || got != KVM_PAIR_TOKEN_SEALED_LEN) {
        kvm_pair_zeroize(out, cap);
        return KVM_PAIR_ERR_CRYPTO;
    }
    *out_len = got;
    return KVM_PAIR_OK;
}

kvm_pair_err_t kvm_pake_token_open(kvm_pake_t *pake, const uint8_t *in, size_t len,
                                   uint8_t token[KVM_PAIR_TOKEN_LEN])
{
    if (pake == NULL || in == NULL || token == NULL) {
        return KVM_PAIR_ERR_ARG;
    }
    if (!pake->finished) {
        return KVM_PAIR_ERR_STATE;
    }
    if (len != KVM_PAIR_TOKEN_SEALED_LEN) {
        return KVM_PAIR_ERR_FRAME;
    }

    uint8_t aad[64];
    size_t aad_len = 0;
    kvm_pair_err_t err = token_aad(pake, aad, sizeof(aad), &aad_len);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    psa_key_id_t k = 0;
    err = token_key(pake, &k);
    if (err != KVM_PAIR_OK) {
        return err;
    }
    uint8_t plain[KVM_PAIR_TOKEN_LEN];
    size_t got = 0;
    psa_status_t st = psa_aead_decrypt(k, PSA_ALG_GCM, pake->n_token, sizeof(pake->n_token), aad,
                                       aad_len, in, len, plain, sizeof(plain), &got);
    psa_destroy_key(k);
    if (st != PSA_SUCCESS || got != KVM_PAIR_TOKEN_LEN) {
        kvm_pair_zeroize(plain, sizeof(plain));
        return KVM_PAIR_ERR_BAD_CODE;
    }
    memcpy(token, plain, KVM_PAIR_TOKEN_LEN);
    kvm_pair_zeroize(plain, sizeof(plain));
    return KVM_PAIR_OK;
}
