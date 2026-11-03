// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the software half of OTA VERIFY (TASK-110, TECH-052): image header parsing, chip
// and silicon-revision gating, project identity and the anti-rollback counter.
//
// Covers the AT-SEC-09 software checks and the "wrong chip / wrong version / downgrade refused"
// rows of the TASK-110 test list. The signature itself is ESP-IDF's
// esp_image_verify()/esp_secure_boot_verify_sbv2_signature_block() and is NOT_RUN_HW here.
#include <stdio.h>
#include <string.h>

#include "fake_platform.h"
#include "kvm_test.h"
#include "update_image.h"

static kvm_ota_image_policy_t strict_policy(void)
{
    kvm_ota_image_policy_t p = {.chip_rev_full = 2u, .require_project_match = true,
                                .allow_same_version = true};
    return p;
}

static kvm_ota_image_info_t running_1_0_0(void)
{
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;
    s.version = "1.0.0";
    return fake_image_info(&s);
}

static void test_parse_full_prefix(void)
{
    uint8_t buf[KVM_OTA_IMAGE_PROBE_BYTES];
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof buf;
    s.secure_version = 7u;
    s.version = "2.3.4";
    s.project_name = "kvmpad";
    s.min_chip_rev_full = 2u;
    s.max_chip_rev_full = 99u;
    CHECK_EQ(fake_image_build(&s, buf, sizeof buf), sizeof buf);

    kvm_ota_image_info_t i;
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK(i.header_ok);
    CHECK(i.desc_ok);
    CHECK_EQ(i.segment_count, 4);
    CHECK_EQ(i.chip_id, KVM_OTA_CHIP_ID_ESP32S3);
    CHECK_EQ(i.min_chip_rev_full, 2);
    CHECK_EQ(i.max_chip_rev_full, 99);
    CHECK(i.hash_appended);
    CHECK_EQ(i.secure_version, 7);
    CHECK(strcmp(i.version, "2.3.4") == 0);
    CHECK(strcmp(i.project_name, "kvmpad") == 0);
    CHECK(strcmp(i.idf_ver, "v6.1") == 0);
}

// Every prefix length from 0 to the full probe size must parse without reading past the buffer.
// Run under ASan this is the out-of-bounds test; the flags only firm up what it already proves.
static void test_parse_every_truncation(void)
{
    uint8_t full[KVM_OTA_IMAGE_PROBE_BYTES];
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof full;
    (void)fake_image_build(&s, full, sizeof full);

    for (size_t n = 0; n <= sizeof full; n++) {
        uint8_t *part = malloc(n > 0u ? n : 1u);
        memcpy(part, full, n);
        kvm_ota_image_info_t i;
        kvm_ota_image_parse(part, n, &i);
        if (n < KVM_OTA_IMAGE_HEADER_LEN) {
            CHECK(!i.header_ok);
            CHECK(!i.desc_ok);
        } else {
            CHECK(i.header_ok);
            CHECK_EQ(i.desc_ok, n >= KVM_OTA_IMAGE_PROBE_BYTES);
        }
        if (!i.desc_ok) {
            CHECK_EQ(i.secure_version, 0);
            CHECK_EQ(i.version[0], 0);
        }
        free(part);
    }
}

static void test_parse_rejects_non_image(void)
{
    uint8_t buf[KVM_OTA_IMAGE_PROBE_BYTES];
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof buf;

    s.magic = 0x7Fu; // an ELF file, a web error page, anything that is not an app image
    (void)fake_image_build(&s, buf, sizeof buf);
    kvm_ota_image_info_t i;
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK(!i.header_ok);

    s = (fake_image_spec_t)FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof buf;
    s.segment_count = 0u;
    (void)fake_image_build(&s, buf, sizeof buf);
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK(!i.header_ok);

    s.segment_count = 17u; // beyond ESP_IMAGE_MAX_SEGMENTS
    (void)fake_image_build(&s, buf, sizeof buf);
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK(!i.header_ok);

    s = (fake_image_spec_t)FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof buf;
    s.desc_magic = 0xDEADBEEFu; // a raw data blob flashed as firmware
    (void)fake_image_build(&s, buf, sizeof buf);
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK(i.header_ok);
    CHECK(!i.desc_ok);
}

static void test_parse_sanitizes_strings(void)
{
    uint8_t buf[KVM_OTA_IMAGE_PROBE_BYTES];
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.total_len = sizeof buf;
    (void)fake_image_build(&s, buf, sizeof buf);
    // A version field with no terminator and with control bytes: neither may reach a log or the UI.
    for (size_t k = 0; k < KVM_OTA_STR_LEN; k++) {
        buf[KVM_OTA_APP_DESC_OFF + 16u + k] = (uint8_t)(k == 3u ? 0x1Bu : 'A');
    }
    kvm_ota_image_info_t i;
    kvm_ota_image_parse(buf, sizeof buf, &i);
    CHECK_EQ(strlen(i.version), KVM_OTA_STR_LEN);
    CHECK_EQ(i.version[3], '?');
    for (size_t k = 0; k < KVM_OTA_STR_LEN; k++) {
        CHECK(i.version[k] >= 0x20 && i.version[k] <= 0x7E);
    }
}

static kvm_ota_image_verdict_t check_spec(const fake_image_spec_t *s, const kvm_ota_image_info_t *run,
                                          const kvm_ota_image_policy_t *pol)
{
    kvm_ota_image_info_t cand = fake_image_info(s);
    return kvm_ota_image_check(&cand, run, pol);
}

static void test_accepts_a_good_upgrade(void)
{
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 5u;
    s.version = "1.1.0";
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);

    s.secure_version = 4u; // equal counter, newer build: allowed
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);
}

static void test_wrong_chip_is_refused(void)
{
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;
    s.chip_id = 0x0000u; // ESP32
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_CHIP);
    s.chip_id = 0x0002u; // ESP32-S2: the closest wrong answer
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_CHIP);
    s.chip_id = 0xFFFFu; // ESP_CHIP_ID_INVALID
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_CHIP);
}

static void test_chip_revision_window(void)
{
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy(); // board #1 is v0.2
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;

    s.min_chip_rev_full = 2u;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);
    s.min_chip_rev_full = 3u; // built for v0.3 silicon and newer
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_CHIP_REV);
    s.min_chip_rev_full = 0u;
    s.max_chip_rev_full = 1u; // declares it does not run on v0.2
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_CHIP_REV);
    s.max_chip_rev_full = 2u;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);

    pol.chip_rev_full = 0u; // revision unknown: the window cannot be judged, so it is not
    s.min_chip_rev_full = 99u;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);
}

static void test_downgrade_is_refused(void)
{
    kvm_ota_image_info_t run = running_1_0_0(); // secure_version 4
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.version = "0.9.0";
    for (uint32_t v = 0; v < 4u; v++) {
        s.secure_version = v;
        CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_DOWNGRADE);
    }
    s.secure_version = 4u;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);
}

static void test_wrong_project_is_refused(void)
{
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;
    s.project_name = "hello_world"; // a signed image from another project on the same key
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_PROJECT);

    pol.require_project_match = false;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT);
}

static void test_same_version_policy(void)
{
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;
    s.version = "1.0.0";
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_ACCEPT); // reflashing is allowed by default
    pol.allow_same_version = false;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_SAME_VERSION);
}

static void test_unknown_running_image_skips_relative_checks(void)
{
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 0u;
    s.project_name = "other";
    kvm_ota_image_info_t none;
    memset(&none, 0, sizeof none);
    // No reference image: chip and header still gate, the relative rules cannot and do not fire.
    CHECK_EQ(check_spec(&s, &none, &pol), KVM_OTA_IMG_ACCEPT);
    CHECK_EQ(check_spec(&s, NULL, &pol), KVM_OTA_IMG_ACCEPT);
    s.chip_id = 0x0005u;
    CHECK_EQ(check_spec(&s, NULL, &pol), KVM_OTA_IMG_WRONG_CHIP);
}

static void test_check_order_is_header_first(void)
{
    // An image that fails several rules at once must report the most fundamental one, so the
    // operator is not told "downgrade" about a file that is not firmware at all.
    kvm_ota_image_info_t run = running_1_0_0();
    kvm_ota_image_policy_t pol = strict_policy();
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.magic = 0u;
    s.chip_id = 0u;
    s.secure_version = 0u;
    s.project_name = "other";
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_BAD_MAGIC);
    s.magic = KVM_OTA_IMAGE_MAGIC;
    s.desc_magic = 1u;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_BAD_DESC);
    s.desc_magic = KVM_OTA_APP_DESC_MAGIC;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_CHIP);
    s.chip_id = KVM_OTA_CHIP_ID_ESP32S3;
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_WRONG_PROJECT);
    s.project_name = "kvmpad";
    CHECK_EQ(check_spec(&s, &run, &pol), KVM_OTA_IMG_DOWNGRADE);
}

static void test_verdict_names(void)
{
    for (int v = KVM_OTA_IMG_ACCEPT; v <= KVM_OTA_IMG_SAME_VERSION; v++) {
        const char *n = kvm_ota_image_verdict_name((kvm_ota_image_verdict_t)v);
        CHECK(n != NULL && n[0] != '\0' && strcmp(n, "?") != 0);
    }
    CHECK(strcmp(kvm_ota_image_verdict_name((kvm_ota_image_verdict_t)99), "?") == 0);
}

int main(void)
{
    printf("test_update_image\n");
    RUN(test_parse_full_prefix);
    RUN(test_parse_every_truncation);
    RUN(test_parse_rejects_non_image);
    RUN(test_parse_sanitizes_strings);
    RUN(test_accepts_a_good_upgrade);
    RUN(test_wrong_chip_is_refused);
    RUN(test_chip_revision_window);
    RUN(test_downgrade_is_refused);
    RUN(test_wrong_project_is_refused);
    RUN(test_same_version_policy);
    RUN(test_unknown_running_image_skips_relative_checks);
    RUN(test_check_order_is_header_first);
    RUN(test_verdict_names);
    TEST_MAIN_END("test_update_image");
}
