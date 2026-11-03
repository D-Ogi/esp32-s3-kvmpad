/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 *
 * TF-PSA-Crypto user configuration for the Windows CLIENT build of the pairing core (ADR-029).
 *
 * It is appended to the stock crypto_config.h of the pinned tree, which already enables everything
 * the pairing core needs: PSA_WANT_ALG_JPAKE, PSA_WANT_ALG_HKDF, PSA_WANT_ALG_GCM,
 * PSA_WANT_KEY_TYPE_AES, PSA_WANT_ALG_SHA_256, PSA_WANT_ALG_HMAC and PSA_WANT_ECC_SECP_R1_256.
 *
 * The only change is the random source, and it is deliberately the same change the firmware and the
 * host tests make. ESP-IDF builds mbedTLS with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG and routes it to
 * esp_random(); the host tests route it to a seeded generator so the interop vectors are
 * reproducible; this build routes it to BCryptGenRandom in src/win_rng.c. Keeping the same option on
 * all three means the J-PAKE ephemeral scalars come from one code path in the library, so the only
 * difference between the three builds is where the entropy is drawn.
 */
#ifndef KVMPAD_WIN_PSA_CONFIG_H
#define KVMPAD_WIN_PSA_CONFIG_H

#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* No persistent key store. The pairing core creates only volatile keys, and a CLIENT that wrote PSA
 * key files beside itself would be keeping key material outside DPAPI, which SEC-105 does not allow.
 */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#endif /* KVMPAD_WIN_PSA_CONFIG_H */
