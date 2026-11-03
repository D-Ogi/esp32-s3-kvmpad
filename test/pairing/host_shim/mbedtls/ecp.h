/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 *
 * Host build shim; see mbedtls/bignum.h next to this file for why it exists.
 */
#pragma once

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/ecp.h"
