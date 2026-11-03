// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Candidate-image inspection (TASK-110, TECH-052). Pure C; see update_image.h for the layout
// contract and for why the offsets are hard-coded instead of taken from an ESP-IDF struct.
#include "update_image.h"

#include <string.h>

// esp_image_header_t field offsets (packed, 24 bytes)
#define OFF_MAGIC 0u
#define OFF_SEGMENT_COUNT 1u
#define OFF_CHIP_ID 12u
#define OFF_MIN_CHIP_REV_FULL 15u
#define OFF_MAX_CHIP_REV_FULL 17u
#define OFF_HASH_APPENDED 23u

// esp_app_desc_t field offsets, relative to KVM_OTA_APP_DESC_OFF (packed-by-layout, 256 bytes)
#define OFF_DESC_MAGIC 0u
#define OFF_DESC_SECURE_VERSION 4u
#define OFF_DESC_VERSION 16u
#define OFF_DESC_PROJECT 48u
#define OFF_DESC_IDF_VER 112u

#define ESP_IMAGE_MAX_SEGMENTS 16u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Copy a fixed-width, possibly unterminated firmware string into a terminated buffer.
static void rdstr(char *dst, const uint8_t *src)
{
    memcpy(dst, src, KVM_OTA_STR_LEN);
    dst[KVM_OTA_STR_LEN] = '\0';
    for (size_t i = 0; i < KVM_OTA_STR_LEN; i++) {
        if (dst[i] == '\0') {
            break;
        }
        // Keep the record printable: a corrupt image must not put control bytes into a log or the UI.
        if (dst[i] < 0x20 || dst[i] > 0x7E) {
            dst[i] = '?';
        }
    }
}

void kvm_ota_image_parse(const uint8_t *buf, size_t len, kvm_ota_image_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (buf == NULL || len < KVM_OTA_IMAGE_HEADER_LEN) {
        return;
    }

    out->segment_count = buf[OFF_SEGMENT_COUNT];
    out->chip_id = rd16(buf + OFF_CHIP_ID);
    out->min_chip_rev_full = rd16(buf + OFF_MIN_CHIP_REV_FULL);
    out->max_chip_rev_full = rd16(buf + OFF_MAX_CHIP_REV_FULL);
    out->hash_appended = buf[OFF_HASH_APPENDED] == 1u;
    out->header_ok = buf[OFF_MAGIC] == KVM_OTA_IMAGE_MAGIC && out->segment_count > 0u &&
                     out->segment_count <= ESP_IMAGE_MAX_SEGMENTS;

    if (len < KVM_OTA_IMAGE_PROBE_BYTES) {
        return;
    }
    const uint8_t *d = buf + KVM_OTA_APP_DESC_OFF;
    if (rd32(d + OFF_DESC_MAGIC) != KVM_OTA_APP_DESC_MAGIC) {
        return;
    }
    out->desc_ok = true;
    out->secure_version = rd32(d + OFF_DESC_SECURE_VERSION);
    rdstr(out->version, d + OFF_DESC_VERSION);
    rdstr(out->project_name, d + OFF_DESC_PROJECT);
    rdstr(out->idf_ver, d + OFF_DESC_IDF_VER);
}

kvm_ota_image_verdict_t kvm_ota_image_check(const kvm_ota_image_info_t *cand,
                                            const kvm_ota_image_info_t *running,
                                            const kvm_ota_image_policy_t *pol)
{
    if (cand == NULL || pol == NULL) {
        return KVM_OTA_IMG_SHORT;
    }
    if (!cand->header_ok) {
        // A prefix shorter than the header parses to header_ok = false as well; the caller knows
        // how many bytes it handed over, so SHORT vs BAD_MAGIC is decided there. Here: refuse.
        return KVM_OTA_IMG_BAD_MAGIC;
    }
    if (!cand->desc_ok) {
        return KVM_OTA_IMG_BAD_DESC;
    }
    if (cand->chip_id != KVM_OTA_CHIP_ID_ESP32S3) {
        return KVM_OTA_IMG_WRONG_CHIP;
    }
    if (pol->chip_rev_full != 0u) {
        if (cand->min_chip_rev_full > pol->chip_rev_full) {
            return KVM_OTA_IMG_CHIP_REV;
        }
        if (cand->max_chip_rev_full != 0u && cand->max_chip_rev_full < pol->chip_rev_full) {
            return KVM_OTA_IMG_CHIP_REV;
        }
    }
    if (running == NULL || !running->desc_ok) {
        return KVM_OTA_IMG_ACCEPT; // no trustworthy reference: the caller reports the gap
    }
    if (pol->require_project_match && strcmp(cand->project_name, running->project_name) != 0) {
        return KVM_OTA_IMG_WRONG_PROJECT;
    }
    if (cand->secure_version < running->secure_version) {
        return KVM_OTA_IMG_DOWNGRADE;
    }
    if (!pol->allow_same_version && strcmp(cand->version, running->version) == 0) {
        return KVM_OTA_IMG_SAME_VERSION;
    }
    return KVM_OTA_IMG_ACCEPT;
}

const char *kvm_ota_image_verdict_name(kvm_ota_image_verdict_t v)
{
    switch (v) {
    case KVM_OTA_IMG_ACCEPT: return "accept";
    case KVM_OTA_IMG_SHORT: return "short";
    case KVM_OTA_IMG_BAD_MAGIC: return "bad_magic";
    case KVM_OTA_IMG_BAD_DESC: return "bad_desc";
    case KVM_OTA_IMG_WRONG_CHIP: return "wrong_chip";
    case KVM_OTA_IMG_CHIP_REV: return "chip_rev";
    case KVM_OTA_IMG_WRONG_PROJECT: return "wrong_project";
    case KVM_OTA_IMG_DOWNGRADE: return "downgrade";
    case KVM_OTA_IMG_SAME_VERSION: return "same_version";
    }
    return "?";
}
