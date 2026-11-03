// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Image sources for the OTA state machine (TASK-110, ADR-031).
//
// v1 has exactly one: a locally triggered HTTPS pull. The operator starts it on the device
// (console or local Settings); the CLIENT protocol has no OTA command (TECH-045, AT-SEC-07). The
// TLS chain is verified against the ESP-IDF certificate bundle or a pinned certificate, and the
// image signature — not the transport — is the trust anchor (TECH-052).
#pragma once

#include "update_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Hard limits of the HTTPS source. A larger image cannot fit an OTA slot anyway. */
#define KVM_OTA_HTTP_TIMEOUT_MS 15000
#define KVM_OTA_HTTP_MAX_IMAGE_BYTES (4u * 1024u * 1024u) // one OTA slot (partitions.csv)

/**
 * The locally triggered HTTPS source. `open` accepts https:// URLs only, requires a 200 status and
 * a Content-Length, and refuses a chunked response (a length is what makes a truncated image
 * detectable).
 */
kvm_ota_source_t update_transport_https(void);

/**
 * Pin a server certificate in PEM form instead of using the certificate bundle. `pem` must stay
 * valid for as long as it is installed; NULL goes back to the bundle.
 */
void update_transport_https_set_cert(const char *pem);

#ifdef __cplusplus
}
#endif
