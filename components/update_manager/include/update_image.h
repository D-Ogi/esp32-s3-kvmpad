// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Candidate-image inspection for the OTA state machine (TASK-110, TECH-052).
//
// Pure C: the image is parsed out of its raw first bytes, by the same offsets the ESP-IDF OTA code
// uses (`esp_image_header_t` at 0, `esp_image_segment_header_t` at 24, `esp_app_desc_t` at 32), so
// this file needs no ESP-IDF header and the host suite can feed it synthetic images.
// `update_platform_esp.c` static-asserts these offsets against the pinned headers.
//
// This is the *software* half of VERIFY: chip, chip revision, project identity and the
// anti-rollback counter (`secure_version`). The cryptographic half is the appended secure boot v2
// signature block, checked by `esp_image_verify()` inside `esp_ota_end()` when
// CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT is set (ADR-031). Neither half is optional.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Raw layout (mirrors the pinned ESP-IDF structures; do not "simplify") ------------------
#define KVM_OTA_IMAGE_HEADER_LEN 24u   // sizeof(esp_image_header_t)
#define KVM_OTA_SEGMENT_HEADER_LEN 8u  // sizeof(esp_image_segment_header_t)
#define KVM_OTA_APP_DESC_OFF 32u       // where esp_ota_get_partition_description() reads
#define KVM_OTA_APP_DESC_LEN 256u      // sizeof(esp_app_desc_t)

/** The smallest prefix that answers every software check; nothing is flashed before it arrives. */
#define KVM_OTA_IMAGE_PROBE_BYTES (KVM_OTA_APP_DESC_OFF + KVM_OTA_APP_DESC_LEN) // 288

#define KVM_OTA_IMAGE_MAGIC 0xE9u        // ESP_IMAGE_HEADER_MAGIC
#define KVM_OTA_APP_DESC_MAGIC 0xABCD5432u // ESP_APP_DESC_MAGIC_WORD
#define KVM_OTA_CHIP_ID_ESP32S3 0x0009u  // ESP_CHIP_ID_ESP32S3

#define KVM_OTA_STR_LEN 32u // esp_app_desc_t version / project_name / idf_ver fields

/** What the first `KVM_OTA_IMAGE_PROBE_BYTES` of an app image say about it. */
typedef struct {
    bool header_ok;   // image magic and a plausible segment count
    bool desc_ok;     // app descriptor magic found at KVM_OTA_APP_DESC_OFF
    uint8_t segment_count;
    uint16_t chip_id;
    uint16_t min_chip_rev_full; // major * 100 + minor
    uint16_t max_chip_rev_full; // 0 = no upper bound declared
    bool hash_appended;
    uint32_t secure_version;    // anti-rollback counter (esp_app_desc_t.secure_version)
    char version[KVM_OTA_STR_LEN + 1u];
    char project_name[KVM_OTA_STR_LEN + 1u];
    char idf_ver[KVM_OTA_STR_LEN + 1u];
} kvm_ota_image_info_t;

/** Why a candidate image was refused. Ordered: the first failing check wins. */
typedef enum {
    KVM_OTA_IMG_ACCEPT = 0,
    KVM_OTA_IMG_SHORT,        // fewer than KVM_OTA_IMAGE_PROBE_BYTES available
    KVM_OTA_IMG_BAD_MAGIC,    // not an ESP application image
    KVM_OTA_IMG_BAD_DESC,     // no app descriptor where the OTA code expects one
    KVM_OTA_IMG_WRONG_CHIP,
    KVM_OTA_IMG_CHIP_REV,     // this silicon revision is outside the image's declared range
    KVM_OTA_IMG_WRONG_PROJECT,
    KVM_OTA_IMG_DOWNGRADE,    // secure_version lower than the running image
    KVM_OTA_IMG_SAME_VERSION, // identical version string while reflashing is not allowed
} kvm_ota_image_verdict_t;

/** Rules applied by kvm_ota_image_check(); see kvm_ota_config_t for the defaults. */
typedef struct {
    uint16_t chip_rev_full;     // the silicon this firmware runs on, major * 100 + minor
    bool require_project_match; // candidate project_name must equal the running one
    bool allow_same_version;    // allow reflashing an identical `version` string
} kvm_ota_image_policy_t;

/**
 * Parse the first bytes of an app image. Never reads past `len`; fields that the available prefix
 * does not cover stay zeroed and the matching `*_ok` flag stays false.
 */
void kvm_ota_image_parse(const uint8_t *buf, size_t len, kvm_ota_image_info_t *out);

/**
 * Software half of VERIFY. `running` is the image that is executing now; pass NULL only when it is
 * genuinely unknown (the anti-rollback and project checks are then skipped and that is reported by
 * the caller, never silently).
 */
kvm_ota_image_verdict_t kvm_ota_image_check(const kvm_ota_image_info_t *cand,
                                            const kvm_ota_image_info_t *running,
                                            const kvm_ota_image_policy_t *pol);

const char *kvm_ota_image_verdict_name(kvm_ota_image_verdict_t v);

#ifdef __cplusplus
}
#endif
