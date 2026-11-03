/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 *
 * Host build shim. In mbedTLS 4.x the bignum interface moved to mbedtls/private/bignum.h, but the
 * ESP-IDF copy of bignum.c and ecp.c still includes "mbedtls/bignum.h" because ESP-IDF supplies its
 * own wrapper from components/mbedtls/port/include -- and that wrapper needs sdkconfig.h, which does
 * not exist off-target. This header is the off-target stand-in: it declares the private identifiers
 * and forwards. It is a build fixture for test/pairing only and is never compiled into the firmware.
 */
#pragma once

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/bignum.h"
