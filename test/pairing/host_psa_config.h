/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 *
 * TF-PSA-Crypto user configuration for the host pairing tests. It is appended to the stock
 * crypto_config.h of the pinned tree, which already enables everything the pairing core needs:
 * PSA_WANT_ALG_JPAKE, PSA_WANT_ALG_HKDF, PSA_WANT_ALG_GCM, PSA_WANT_KEY_TYPE_AES,
 * PSA_WANT_ALG_SHA_256, PSA_WANT_ALG_HMAC and PSA_WANT_ECC_SECP_R1_256.
 *
 * The one change is the random source. ESP-IDF builds mbedTLS with MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 * and routes it to esp_random(); the tests enable the same option and route it to the seeded DRBG in
 * det_rng.c, which is what makes a J-PAKE run -- and therefore the interop vectors -- reproducible.
 * A Windows build of the same core enables the option too and routes it to BCryptGenRandom.
 */
#ifndef KVM_HOST_PSA_CONFIG_H
#define KVM_HOST_PSA_CONFIG_H

#define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG

/* No persistent key store: the pairing core only ever creates volatile keys, and the tests must not
 * write PSA key files into the build directory. */
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C

#endif /* KVM_HOST_PSA_CONFIG_H */
