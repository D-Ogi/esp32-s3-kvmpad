// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Host tests of the TASK-110 OTA state machine against the doubles in fake_platform.h.
//
// Requirement coverage: TECH-052 (signed OTA, health check, rollback, power-loss safety),
// TECH-053 (RELEASE_ALL, client revoked, input blocked, "Updating" state, nothing held restored
// after the reboot), PROC-009, AT-FAULT-07 (OTA while drag lock), AT-FAULT-08 (power cut during
// OTA: the previous slot stays bootable, otadata stays consistent), AT-SEC-07 (no remote OTA path;
// enforced by the API shape and asserted here), AT-PERF-09 (input blocked until the health check).
#include <stdio.h>
#include <string.h>

#include "fake_platform.h"
#include "kvm_test.h"
#include "update_manager.h"

#define IMG_BYTES 4096u

typedef struct {
    fake_dev_t dev;
    fake_src_t src;
    fake_probes_t probes;
    kvm_ota_t m;
    uint8_t image[IMG_BYTES];
    size_t image_len;
} fixture_t;

static fake_image_spec_t running_spec(void)
{
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 4u;
    s.version = "1.0.0";
    return s;
}

static fake_image_spec_t upgrade_spec(void)
{
    fake_image_spec_t s = FAKE_IMAGE_SPEC_DEFAULT;
    s.secure_version = 5u;
    s.version = "1.1.0";
    s.total_len = IMG_BYTES;
    return s;
}

/** Fresh device, running image 1.0.0 (secure_version 4), candidate 1.1.0 (5) on the source. */
static void fx_init(fixture_t *f, const fake_image_spec_t *cand)
{
    memset(f, 0, sizeof *f);
    fake_image_spec_t run = running_spec();
    kvm_ota_image_info_t run_info = fake_image_info(&run);
    fake_dev_init(&f->dev, &run_info);

    fake_image_spec_t c = cand != NULL ? *cand : upgrade_spec();
    f->image_len = fake_image_build(&c, f->image, sizeof f->image);
    f->src.data = f->image;
    f->src.len = f->image_len;
    f->src.declared = f->image_len;

    kvm_ota_platform_t plat = fake_platform(&f->dev);
    kvm_ota_config_t cfg = KVM_OTA_CONFIG_DEFAULT;
    cfg.chunk_bytes = 512u;
    kvm_ota_init(&f->m, &plat, &cfg);
    fake_add_probes(&f->m, &f->probes);
    fake_probes_all_up(&f->probes);

    // A normal boot of a confirmed image: nothing pending, input free.
    kvm_ota_boot(&f->m, KVM_OTA_IMG_STATE_VALID, 0u);
}

static kvm_ota_source_t fx_source(fixture_t *f)
{
    return fake_source(&f->src);
}

static void drive(fixture_t *f, uint32_t polls)
{
    for (uint32_t i = 0; i < polls; i++) {
        f->dev.now_us += 1000u;
        kvm_ota_poll(&f->m, f->dev.now_us);
    }
}

// ---- the normal path -------------------------------------------------------------------------

static void test_happy_path(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);

    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/kvmpad.bin", f.dev.now_us), KVM_OTA_R_NONE);
    // TECH-053, in order and before anything is written.
    CHECK_EQ(f.dev.revoke_calls, 1);
    CHECK(f.dev.maintenance);
    CHECK_EQ(f.m.state, KVM_OTA_PREPARE);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(f.src.open_calls, 0);

    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_REBOOT, 200u, 1000u));
    CHECK_EQ(f.src.open_calls, 1);
    CHECK_EQ(f.dev.begin_calls, 1);
    CHECK_EQ(f.dev.written, f.image_len);
    CHECK_EQ(f.dev.end_calls, 1);
    CHECK_EQ(f.dev.set_boot_calls, 1);
    CHECK_EQ(f.dev.abort_calls, 0);
    CHECK_EQ(f.src.close_calls, 1);
    CHECK(!f.dev.wrote_while_not_neutral);
    // The switch happened exactly once and the new slot is now the boot slot.
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 1);
    CHECK(fake_otadata_consistent(&f.dev));
    // Input stays blocked across the reset; nothing is released into a half-switched device.
    CHECK(f.dev.maintenance);
    CHECK_EQ(f.dev.maintenance_off_calls, 0);

    CHECK_EQ(f.dev.reboot_calls, 0);
    drive(&f, 3000u); // let the reboot delay elapse
    CHECK(f.dev.reboot_calls >= 1);

    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.percent, 100);
    CHECK_EQ(st.bytes_written, f.image_len);
    CHECK_EQ(st.attempts, 1);
    CHECK_EQ(st.aborts, 0);
    CHECK(st.input_blocked);
    CHECK_EQ(st.candidate.secure_version, 5);
    CHECK(strcmp(st.candidate.version, "1.1.0") == 0);
    CHECK(strcmp(st.running.version, "1.0.0") == 0);
}

static void test_progress_is_monotonic(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    uint8_t last = 0u;
    uint32_t seen = 0u;
    for (uint32_t i = 0; i < 300u && f.m.state != KVM_OTA_REBOOT; i++) {
        f.dev.now_us += 1000u;
        kvm_ota_poll(&f.m, f.dev.now_us);
        kvm_ota_status_t st;
        kvm_ota_status(&f.m, &st);
        CHECK(st.percent >= last);
        last = st.percent;
        seen++;
    }
    CHECK_EQ(last, 100);
    CHECK(seen > 4u); // the transfer really was chunked, not one write
    CHECK(f.dev.write_calls > 4u);
}

// ---- TECH-053 / AT-FAULT-07 ------------------------------------------------------------------

static void test_at_fault_07_ota_while_drag_lock(void)
{
    // A finger is down with drag lock engaged when the operator starts an update. Nothing may be
    // written until the HID state is neutral again, and the release must come from maintenance.
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.neutral = false;
    f.dev.neutral_delay_us = 5000u; // reports still draining to the HOST
    kvm_ota_source_t src = fx_source(&f);

    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK_EQ(f.dev.revoke_calls, 1);
    CHECK_EQ(f.dev.maintenance_on_calls, 1);

    drive(&f, 3u); // 3 ms: still not neutral
    CHECK_EQ(f.m.state, KVM_OTA_PREPARE);
    CHECK_EQ(f.src.open_calls, 0);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(f.dev.write_calls, 0);

    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_RECEIVE, 20u, 1000u));
    CHECK_EQ(f.dev.begin_calls, 1);
    CHECK(!f.dev.wrote_while_not_neutral);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_REBOOT, 200u, 1000u));
    CHECK(!f.dev.wrote_while_not_neutral);
}

static void test_neutral_timeout_leaves_the_slot_untouched(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.hold_not_neutral = true; // the HID path never confirms neutral
    kvm_ota_source_t src = fx_source(&f);

    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_ABORTING, 5000u, 1000u));
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_NEUTRAL_TIMEOUT);
    CHECK_EQ(f.src.open_calls, 0);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(f.dev.written, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0); // the running slot still boots
    // Invariant 3: input is NOT released while the HID state is unknown.
    CHECK(f.dev.maintenance);
    drive(&f, 100u);
    CHECK_EQ(f.m.state, KVM_OTA_ABORTING);
    CHECK_EQ(f.dev.maintenance_off_calls, 0);

    f.dev.hold_not_neutral = false;
    f.dev.neutral = true;
    drive(&f, 1u);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);
    CHECK(!f.dev.maintenance);
    CHECK_EQ(f.dev.maintenance_off_calls, 1);
}

// ---- refusals before a single byte is flashed ------------------------------------------------

/** Start, run to the end and return the final reason. */
static kvm_ota_reason_t run_to_end(fixture_t *f)
{
    kvm_ota_source_t src = fx_source(f);
    kvm_ota_reason_t r = kvm_ota_start(&f->m, &src, "https://host/a.bin", f->dev.now_us);
    if (r != KVM_OTA_R_NONE) {
        return r;
    }
    for (uint32_t i = 0; i < 4000u; i++) {
        f->dev.now_us += 1000u;
        kvm_ota_poll(&f->m, f->dev.now_us);
        if (f->m.state == KVM_OTA_FAILED || f->m.state == KVM_OTA_REBOOT) {
            break;
        }
    }
    kvm_ota_status_t st;
    kvm_ota_status(&f->m, &st);
    return st.reason;
}

static void check_bad_image(const fake_image_spec_t *spec, kvm_ota_reason_t want, bool expect_no_writes)
{
    static fixture_t f;
    fx_init(&f, spec);
    CHECK_EQ(run_to_end(&f), want);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(fake_otadata_consistent(&f.dev));
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(f.dev.abort_calls, 1); // the slot handle was released, no boot switch
    CHECK(!f.dev.maintenance);      // input released, because the HID state is neutral
    if (expect_no_writes) {
        // The prefix is held back, so a refusable image never reaches the flash at all.
        CHECK_EQ(f.dev.write_calls, 0);
        CHECK_EQ(f.dev.written, 0);
    }
}

static void test_wrong_chip_never_reaches_flash(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.chip_id = 0x0005u; // ESP32-C3
    check_bad_image(&s, KVM_OTA_R_WRONG_CHIP, true);
}

static void test_wrong_chip_revision_never_reaches_flash(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.min_chip_rev_full = 3u; // board #1 is v0.2
    check_bad_image(&s, KVM_OTA_R_CHIP_REV, true);
}

static void test_downgrade_never_reaches_flash(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.secure_version = 3u; // running image is 4
    s.version = "0.9.9";
    check_bad_image(&s, KVM_OTA_R_DOWNGRADE, true);
}

static void test_wrong_project_never_reaches_flash(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.project_name = "blink";
    check_bad_image(&s, KVM_OTA_R_WRONG_PROJECT, true);
}

static void test_not_an_image_never_reaches_flash(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.magic = 0x3Cu; // '<': an HTML error page served with status 200
    check_bad_image(&s, KVM_OTA_R_IMAGE_HEADER, true);

    fake_image_spec_t t = upgrade_spec();
    t.desc_magic = 0u; // an app image with no descriptor
    check_bad_image(&t, KVM_OTA_R_IMAGE_HEADER, true);
}

static void test_image_shorter_than_the_header(void)
{
    fake_image_spec_t s = upgrade_spec();
    s.total_len = 100u; // shorter than the probe prefix: cannot be a valid app image
    check_bad_image(&s, KVM_OTA_R_IMAGE_HEADER, true);
}

// ---- transport failures ----------------------------------------------------------------------

static void test_source_open_failure(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.open_fails = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SOURCE_OPEN);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(!f.dev.maintenance);
}

static void test_size_unknown_is_refused(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.declared = 0u; // chunked transfer with no length: truncation would be undetectable
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SIZE_UNKNOWN);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(f.src.close_calls, 1);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_declared_size_larger_than_the_slot(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.declared = FAKE_SLOT_BYTES + 1u;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_OVERSIZE);
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_more_bytes_than_declared(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.declared = f.image_len - 600u; // the server lied about the length
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_OVERSIZE);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(f.dev.abort_calls, 1);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_truncated_image(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.cut_at = f.image_len / 2u; // the connection ends halfway through
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_TRUNCATED);
    CHECK_EQ(f.dev.end_calls, 0); // never finalized
    CHECK_EQ(f.dev.abort_calls, 1);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(fake_otadata_consistent(&f.dev));
}

static void test_truncated_inside_the_prefix(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.cut_at = 100u; // fewer bytes than the software checks need
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_TRUNCATED);
    CHECK_EQ(f.dev.write_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_read_error_mid_transfer(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.read_error_at = 2048u; // TLS error / Wi-Fi drop
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SOURCE_READ);
    CHECK_EQ(f.dev.abort_calls, 1);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_a_source_that_stops_short_is_truncated_not_stalled(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.cut_at = 1024u;        // delivers 1 KiB, then nothing more
    f.src.declared = f.image_len; // but claims the full length
    // The end of stream with bytes missing is a truncation, which is the stricter verdict; the
    // stall path is reached only when the source reports neither data nor end.
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_TRUNCATED);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

// ---- verification and flash failures ---------------------------------------------------------

static void test_bad_signature_keeps_the_old_slot(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.end_invalid = true; // esp_ota_end() -> ESP_ERR_OTA_VALIDATE_FAILED
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SIGNATURE);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(fake_otadata_consistent(&f.dev));
    CHECK(!f.dev.maintenance); // input released once neutral
}

static void test_flash_begin_failure(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.begin_fails = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_FLASH_BEGIN);
    CHECK_EQ(f.dev.write_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_flash_write_failure(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.write_fails_at = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_FLASH_WRITE);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(f.dev.abort_calls, 1);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_readback_mismatch_keeps_the_old_slot(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.readback_corrupt = true; // what is in flash is not what was accepted
    kvm_ota_reason_t r = run_to_end(&f);
    CHECK(r == KVM_OTA_R_READBACK || r == KVM_OTA_R_DOWNGRADE);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_readback_failure_keeps_the_old_slot(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.readback_fails = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_READBACK);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_set_boot_failure_leaves_otadata_alone(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.set_boot_fails = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SET_BOOT);
    CHECK_EQ(f.dev.set_boot_calls, 1);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0); // AT-FAULT-08: the previous slot is bootable
    CHECK(fake_otadata_consistent(&f.dev));
    CHECK_EQ(f.dev.reboot_calls, 0);
    CHECK(!f.dev.maintenance);
}

static void test_torn_otadata_write_keeps_the_old_slot(void)
{
    // Power lost while esp_ota_set_boot_partition() was writing the inactive otadata copy: that
    // copy has a bad CRC, so the bootloader keeps using the active one.
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.set_boot_torn = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SET_BOOT);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(fake_otadata_consistent(&f.dev));
}

// ---- AT-FAULT-08: a power cut at every phase -------------------------------------------------

static void test_at_fault_08_power_cut_at_every_flash_operation(void)
{
    // Count the flash operations of a complete update, then cut the power before each one in turn.
    static fixture_t ref;
    fx_init(&ref, NULL);
    kvm_ota_source_t rsrc = fx_source(&ref);
    CHECK_EQ(kvm_ota_start(&ref.m, &rsrc, "https://host/a.bin", ref.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&ref.m, &ref.dev, KVM_OTA_REBOOT, 400u, 1000u));
    uint32_t total_ops = ref.dev.flash_ops;
    CHECK(total_ops > 6u); // begin + several writes + end + set_boot

    for (uint32_t cut = 1u; cut <= total_ops; cut++) {
        static fixture_t f;
        fx_init(&f, NULL);
        f.dev.power_cut_after = cut;
        kvm_ota_source_t src = fx_source(&f);
        CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
        for (uint32_t i = 0; i < 1000u; i++) {
            f.dev.now_us += 1000u;
            kvm_ota_poll(&f.m, f.dev.now_us);
            if (f.m.state == KVM_OTA_FAILED || f.m.state == KVM_OTA_REBOOT) {
                break;
            }
        }
        // Whatever the phase, after the reset the device boots something: either the old slot
        // (every cut before the switch completed) or the new one (the switch had finished).
        CHECK(fake_otadata_consistent(&f.dev));
        uint8_t boots = fake_slot_boot_slot(&f.dev);
        CHECK(boots == 0u || (boots == 1u && f.dev.set_boot_calls == 1u && !f.dev.powered_off));
        if (boots == 0u) {
            // The old image is intact and the attempt did not pretend to succeed.
            CHECK(f.m.state != KVM_OTA_REBOOT || f.dev.set_boot_calls == 0u);
        }
    }
}

static void test_power_cut_before_the_switch_never_boots_the_new_slot(void)
{
    // Specifically the window the test plan calls out: the image is fully written and verified,
    // and the power goes before otadata is touched.
    static fixture_t ref;
    fx_init(&ref, NULL);
    kvm_ota_source_t rsrc = fx_source(&ref);
    CHECK_EQ(kvm_ota_start(&ref.m, &rsrc, "https://host/a.bin", ref.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&ref.m, &ref.dev, KVM_OTA_REBOOT, 400u, 1000u));
    uint32_t ops_before_switch = ref.dev.flash_ops - 1u; // everything except set_boot

    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.power_cut_after = ops_before_switch;
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    drive(&f, 1000u);
    // The image was written and verified, the switch was reached and the power died inside it.
    CHECK_EQ(f.dev.end_calls, 1);
    CHECK(f.dev.powered_off);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
    CHECK(fake_otadata_consistent(&f.dev));
    CHECK_EQ(f.dev.reboot_calls, 0);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_SET_BOOT);
}

// ---- cancel ----------------------------------------------------------------------------------

static void test_cancel_in_receive(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_RECEIVE, 20u, 1000u));
    drive(&f, 2u);
    CHECK(f.dev.written > 0u);

    CHECK(kvm_ota_cancel(&f.m));
    CHECK_EQ(f.dev.abort_calls, 1);
    CHECK_EQ(f.src.close_calls, 1);
    CHECK_EQ(f.dev.set_boot_calls, 0);
    drive(&f, 1u);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_CANCELLED);
    CHECK_EQ(st.aborts, 1);
    CHECK(!st.input_blocked);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_cancel_in_prepare(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.hold_not_neutral = true;
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(kvm_ota_cancel(&f.m));
    CHECK_EQ(f.dev.begin_calls, 0);
    CHECK_EQ(f.src.open_calls, 0);
    CHECK_EQ(f.m.state, KVM_OTA_ABORTING); // still blocked: the HID state is not neutral
    f.dev.hold_not_neutral = false;
    f.dev.neutral = true;
    drive(&f, 1u);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);
}

static void test_cancel_is_refused_once_the_reboot_is_committed(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_REBOOT, 400u, 1000u));
    CHECK(!kvm_ota_cancel(&f.m));
    CHECK_EQ(f.m.state, KVM_OTA_REBOOT);
    CHECK(!kvm_ota_cancel(NULL));
}

static void test_cancel_in_idle_does_nothing(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    CHECK(!kvm_ota_cancel(&f.m));
    CHECK_EQ(f.m.state, KVM_OTA_IDLE);
    CHECK_EQ(f.dev.abort_calls, 0);
}

// ---- start refusals --------------------------------------------------------------------------

static void test_start_is_refused_while_busy(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/b.bin", f.dev.now_us), KVM_OTA_R_BUSY);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_RECEIVE, 20u, 1000u));
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/b.bin", f.dev.now_us), KVM_OTA_R_BUSY);
    CHECK_EQ(f.src.open_calls, 1); // the second request never touched the transport
    CHECK_EQ(f.dev.begin_calls, 1);
}

static void test_start_is_refused_without_a_source(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_source_t src = fx_source(&f);
    kvm_ota_source_t empty = {0};
    CHECK_EQ(kvm_ota_start(&f.m, NULL, "https://host/a.bin", 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(kvm_ota_start(&f.m, &empty, "https://host/a.bin", 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(kvm_ota_start(&f.m, &src, NULL, 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "", 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(kvm_ota_start(NULL, &src, "https://host/a.bin", 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(f.dev.maintenance_on_calls, 0); // a refusal never raises the interlock
    CHECK_EQ(f.dev.revoke_calls, 0);

    char too_long[KVM_OTA_URL_MAX + 8u];
    memset(too_long, 'x', sizeof too_long - 1u);
    too_long[sizeof too_long - 1u] = '\0';
    CHECK_EQ(kvm_ota_start(&f.m, &src, too_long, 0u), KVM_OTA_R_NO_SOURCE);
    CHECK_EQ(f.dev.maintenance_on_calls, 0);
}

static void test_start_is_refused_while_the_running_image_is_unconfirmed(void)
{
    // ESP_ERR_OTA_ROLLBACK_INVALID_STATE in ESP-IDF terms: an image that has not proved itself may
    // not overwrite the slot that is its own fallback.
    static fixture_t f;
    fx_init(&f, NULL);
    f.probes.usb = false;
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    CHECK_EQ(f.m.state, KVM_OTA_HEALTH_CHECK);
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NOT_CONFIRMED);
    CHECK_EQ(f.dev.begin_calls, 0);
}

static void test_a_second_update_after_a_failure(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.src.open_fails = true;
    CHECK_EQ(run_to_end(&f), KVM_OTA_R_SOURCE_OPEN);
    CHECK_EQ(f.m.state, KVM_OTA_FAILED);

    f.src.open_fails = false;
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_REBOOT, 400u, 1000u));
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.attempts, 2);
    CHECK_EQ(st.aborts, 1);
    CHECK_EQ(st.reason, KVM_OTA_R_NONE);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 1);
}

// ---- after the reboot: health check and rollback ---------------------------------------------

static void test_health_check_passes_and_marks_valid(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u; // the new image is running
    fake_probes_all_up(&f.probes);
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);

    // AT-PERF-09: input is blocked from the first instruction of the new image.
    CHECK(f.dev.maintenance);
    CHECK_EQ(f.dev.revoke_calls, 1);
    CHECK_EQ(f.m.state, KVM_OTA_HEALTH_CHECK);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK(st.rollback_pending);
    CHECK(!st.confirmed);
    CHECK(st.input_blocked);

    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_CONFIRMED, 50u, 1000u));
    CHECK_EQ(f.dev.mark_valid_calls, 1);
    CHECK_EQ(f.dev.rollback_calls, 0);
    drive(&f, 1u);
    CHECK(!f.dev.maintenance); // input released only now
    kvm_ota_status(&f.m, &st);
    CHECK(st.confirmed);
    CHECK(!st.rollback_pending);
    CHECK(!st.input_blocked);
    CHECK_EQ(st.reason, KVM_OTA_R_NONE);
    CHECK_EQ(st.health_missing_mask, 0u);
    CHECK_EQ(st.health_alive_mask, 0xFu);
    CHECK(st.health_sweeps >= 3u); // three clean sweeps, not one lucky sample
}

static void test_health_check_timeout_rolls_back(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    f.dev.ota[1] = (fake_otadata_copy_t){.seq = 2u, .crc_ok = true, .slot = 1u};
    fake_probes_all_up(&f.probes);
    f.probes.usb = false; // the USB stack never comes up: TECH-052 "unresponsive USB"
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);

    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_ROLLBACK, 40000u, 1000u));
    CHECK_EQ(f.dev.mark_valid_calls, 0);
    CHECK_EQ(f.dev.rollback_calls, 1);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_HEALTH_TIMEOUT);
    CHECK_EQ(st.rollbacks, 1);
    CHECK_EQ(st.health_missing_mask, 1u); // probe 0 = usb
    CHECK(st.input_blocked);              // never released into a broken image
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0); // the previous slot takes over
    CHECK(fake_otadata_consistent(&f.dev));
}

static void test_each_subsystem_alone_can_force_a_rollback(void)
{
    for (unsigned which = 0; which < 4u; which++) {
        static fixture_t f;
        fx_init(&f, NULL);
        f.dev.running_slot = 1u;
        f.dev.ota[1] = (fake_otadata_copy_t){.seq = 2u, .crc_ok = true, .slot = 1u};
        fake_probes_all_up(&f.probes);
        switch (which) {
        case 0: f.probes.usb = false; break;
        case 1: f.probes.hid = false; break;
        case 2: f.probes.lvgl = false; break;
        default: f.probes.touch = false; break;
        }
        kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
        CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_ROLLBACK, 40000u, 1000u));
        CHECK_EQ(f.dev.rollback_calls, 1);
        CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
        kvm_ota_status_t st;
        kvm_ota_status(&f.m, &st);
        CHECK_EQ(st.health_missing_mask, 1u << which);
    }
}

static void test_a_subsystem_that_comes_up_late_still_passes(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    fake_probes_all_up(&f.probes);
    f.probes.lvgl = false; // the UI owner task needs a moment after the reset
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    drive(&f, 500u);
    CHECK_EQ(f.m.state, KVM_OTA_HEALTH_CHECK);
    f.probes.lvgl = true;
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_CONFIRMED, 50u, 1000u));
    CHECK_EQ(f.dev.mark_valid_calls, 1);
    CHECK_EQ(f.dev.rollback_calls, 0);
}

static void test_held_hid_state_after_boot_rolls_back(void)
{
    // TECH-053: "after the restart do not restore held state". If the fresh image comes up with
    // something pressed, that is a defect in the image, and the honest answer is a rollback.
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    f.dev.ota[1] = (fake_otadata_copy_t){.seq = 2u, .crc_ok = true, .slot = 1u};
    f.dev.hold_not_neutral = true;
    fake_probes_all_up(&f.probes);

    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    CHECK_EQ(f.m.state, KVM_OTA_ROLLBACK);
    CHECK_EQ(f.dev.rollback_calls, 1);
    CHECK_EQ(f.dev.mark_valid_calls, 0);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_HELD_AFTER_BOOT);
    CHECK_EQ(st.rollbacks, 1);
    CHECK(st.input_blocked);
    CHECK_EQ(fake_slot_boot_slot(&f.dev), 0);
}

static void test_operator_can_confirm_before_the_health_check_ends(void)
{
    // `ota mark-valid`: the operator is looking at a working device and does not want to wait for
    // the sweeps. It must take the same exit as an automatic confirmation.
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    fake_probes_all_up(&f.probes);
    f.probes.touch = false; // a probe still down: the automatic path would not confirm yet
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    CHECK_EQ(f.m.state, KVM_OTA_HEALTH_CHECK);
    CHECK(f.dev.maintenance);

    CHECK(kvm_ota_confirm_now(&f.m));
    CHECK_EQ(f.m.state, KVM_OTA_CONFIRMED);
    CHECK_EQ(f.dev.mark_valid_calls, 1);
    CHECK_EQ(f.dev.rollback_calls, 0);
    drive(&f, 2u);
    CHECK(!f.dev.maintenance); // input released through the same neutral gate
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK(st.confirmed);
    CHECK(!st.rollback_pending);
    CHECK_EQ(st.reason, KVM_OTA_R_NONE);

    // And a new update is allowed now.
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
}

static void test_operator_confirm_is_refused_outside_the_health_check(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    CHECK(!kvm_ota_confirm_now(NULL));
    CHECK(!kvm_ota_confirm_now(&f.m)); // IDLE: nothing to confirm
    CHECK_EQ(f.dev.mark_valid_calls, 0);

    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NONE);
    CHECK(!kvm_ota_confirm_now(&f.m)); // mid-update: not a confirmation point either
    CHECK_EQ(f.dev.mark_valid_calls, 0);
}

static void test_operator_confirm_reports_a_failed_mark(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    f.dev.mark_valid_fails = true;
    fake_probes_all_up(&f.probes);
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    CHECK(!kvm_ota_confirm_now(&f.m));
    CHECK_EQ(f.m.state, KVM_OTA_HEALTH_CHECK); // still pending, still blocked: fails safe
    CHECK(f.dev.maintenance);
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_MARK_VALID);
    CHECK(!st.confirmed);
}

static void test_mark_valid_failure_leaves_the_image_pending(void)
{
    static fixture_t f;
    fx_init(&f, NULL);
    f.dev.running_slot = 1u;
    f.dev.mark_valid_fails = true;
    fake_probes_all_up(&f.probes);
    kvm_ota_boot(&f.m, KVM_OTA_IMG_STATE_PENDING_VERIFY, f.dev.now_us);
    CHECK(fake_run_until(&f.m, &f.dev, KVM_OTA_CONFIRMED, 50u, 1000u));
    kvm_ota_status_t st;
    kvm_ota_status(&f.m, &st);
    CHECK_EQ(st.reason, KVM_OTA_R_MARK_VALID);
    CHECK(!st.confirmed);        // still pending: the next reboot rolls back by itself
    CHECK(st.rollback_pending);
    drive(&f, 2u);
    CHECK(!f.dev.maintenance);   // input is usable, the operator can retry
    // And a new update is refused while the image is unconfirmed.
    kvm_ota_source_t src = fx_source(&f);
    CHECK_EQ(kvm_ota_start(&f.m, &src, "https://host/a.bin", f.dev.now_us), KVM_OTA_R_NOT_CONFIRMED);
}

static void test_normal_boot_states_need_no_health_check(void)
{
    const kvm_ota_img_state_t states[] = {
        KVM_OTA_IMG_STATE_VALID, KVM_OTA_IMG_STATE_UNDEFINED, KVM_OTA_IMG_STATE_NEW,
        KVM_OTA_IMG_STATE_INVALID, KVM_OTA_IMG_STATE_ABORTED,
    };
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; i++) {
        static fixture_t f;
        fx_init(&f, NULL);
        f.dev.maintenance_on_calls = 0u;
        f.dev.revoke_calls = 0u;
        kvm_ota_boot(&f.m, states[i], f.dev.now_us);
        CHECK_EQ(f.m.state, KVM_OTA_IDLE);
        CHECK(!f.dev.maintenance);
        CHECK_EQ(f.dev.maintenance_on_calls, 0);
        CHECK_EQ(f.dev.revoke_calls, 0);
        CHECK_EQ(f.dev.mark_valid_calls, 0);
        CHECK_EQ(f.dev.rollback_calls, 0);
        kvm_ota_status_t st;
        kvm_ota_status(&f.m, &st);
        CHECK(st.confirmed);
        CHECK(!st.rollback_pending);
        CHECK(strcmp(st.running.version, "1.0.0") == 0);
    }
}

// ---- API hygiene -----------------------------------------------------------------------------

static void test_null_arguments_are_survivable(void)
{
    kvm_ota_init(NULL, NULL, NULL);
    kvm_ota_poll(NULL, 0u);
    kvm_ota_boot(NULL, KVM_OTA_IMG_STATE_VALID, 0u);
    kvm_ota_status(NULL, NULL);
    CHECK(!kvm_ota_add_probe(NULL, "x", NULL, NULL, true));

    static fixture_t f;
    fx_init(&f, NULL);
    kvm_ota_status(&f.m, NULL);
    kvm_ota_status_t st;
    kvm_ota_status(NULL, &st);
}

static void test_default_config_is_used_when_none_is_given(void)
{
    fake_dev_t d;
    fake_image_spec_t run = running_spec();
    kvm_ota_image_info_t info = fake_image_info(&run);
    fake_dev_init(&d, &info);
    kvm_ota_platform_t plat = fake_platform(&d);
    kvm_ota_t m;
    kvm_ota_init(&m, &plat, NULL);
    kvm_ota_config_t def = KVM_OTA_CONFIG_DEFAULT;
    CHECK_EQ(m.cfg.neutral_timeout_us, def.neutral_timeout_us);
    CHECK_EQ(m.cfg.health_timeout_us, def.health_timeout_us);
    CHECK_EQ(m.cfg.chunk_bytes, def.chunk_bytes);
    CHECK(m.cfg.chunk_bytes <= KVM_OTA_CHUNK_MAX);

    kvm_ota_config_t silly = def;
    silly.chunk_bytes = 0u;
    silly.health_consecutive = 0u;
    kvm_ota_init(&m, &plat, &silly);
    CHECK_EQ(m.cfg.chunk_bytes, KVM_OTA_CHUNK_MAX);
    CHECK_EQ(m.cfg.health_consecutive, 1);

    silly.chunk_bytes = KVM_OTA_CHUNK_MAX * 4u;
    kvm_ota_init(&m, &plat, &silly);
    CHECK_EQ(m.cfg.chunk_bytes, KVM_OTA_CHUNK_MAX);
}

static void test_state_and_reason_names(void)
{
    for (int s = KVM_OTA_IDLE; s <= KVM_OTA_FAILED; s++) {
        const char *n = kvm_ota_state_name((kvm_ota_state_t)s);
        CHECK(n != NULL && n[0] != '\0' && strcmp(n, "?") != 0);
    }
    CHECK(strcmp(kvm_ota_state_name((kvm_ota_state_t)99), "?") == 0);
    for (int r = 0; r < KVM_OTA_R_COUNT; r++) {
        const char *n = kvm_ota_reason_name((kvm_ota_reason_t)r);
        CHECK(n != NULL && n[0] != '\0' && strcmp(n, "?") != 0);
    }
    CHECK(strcmp(kvm_ota_reason_name(KVM_OTA_R_COUNT), "?") == 0);
}

static void test_a_small_chunk_size_still_completes(void)
{
    // A 64-byte chunk means the probe prefix spans five reads: the hold-back logic must not lose a
    // byte or write out of order.
    fake_dev_t d;
    fake_src_t s;
    fake_probes_t pr;
    uint8_t image[IMG_BYTES];
    memset(&s, 0, sizeof s);
    memset(&pr, 0, sizeof pr);
    fake_image_spec_t run = running_spec();
    kvm_ota_image_info_t info = fake_image_info(&run);
    fake_dev_init(&d, &info);
    fake_image_spec_t cand = upgrade_spec();
    size_t len = fake_image_build(&cand, image, sizeof image);
    s.data = image;
    s.len = len;
    s.declared = len;
    s.max_chunk = 64u;

    kvm_ota_platform_t plat = fake_platform(&d);
    kvm_ota_config_t cfg = KVM_OTA_CONFIG_DEFAULT;
    cfg.chunk_bytes = 64u;
    kvm_ota_t m;
    kvm_ota_init(&m, &plat, &cfg);
    fake_add_probes(&m, &pr);
    fake_probes_all_up(&pr);
    kvm_ota_boot(&m, KVM_OTA_IMG_STATE_VALID, 0u);

    kvm_ota_source_t src = fake_source(&s);
    CHECK_EQ(kvm_ota_start(&m, &src, "https://host/a.bin", d.now_us), KVM_OTA_R_NONE);
    CHECK(fake_run_until(&m, &d, KVM_OTA_REBOOT, 2000u, 1000u));
    CHECK_EQ(d.written, len);
    CHECK_EQ(memcmp(d.slot, image, len), 0); // byte-for-byte, in order
    CHECK_EQ(fake_slot_boot_slot(&d), 1);
}

int main(void)
{
    printf("test_update_sm\n");
    RUN(test_happy_path);
    RUN(test_progress_is_monotonic);
    RUN(test_at_fault_07_ota_while_drag_lock);
    RUN(test_neutral_timeout_leaves_the_slot_untouched);
    RUN(test_wrong_chip_never_reaches_flash);
    RUN(test_wrong_chip_revision_never_reaches_flash);
    RUN(test_downgrade_never_reaches_flash);
    RUN(test_wrong_project_never_reaches_flash);
    RUN(test_not_an_image_never_reaches_flash);
    RUN(test_image_shorter_than_the_header);
    RUN(test_source_open_failure);
    RUN(test_size_unknown_is_refused);
    RUN(test_declared_size_larger_than_the_slot);
    RUN(test_more_bytes_than_declared);
    RUN(test_truncated_image);
    RUN(test_truncated_inside_the_prefix);
    RUN(test_read_error_mid_transfer);
    RUN(test_a_source_that_stops_short_is_truncated_not_stalled);
    RUN(test_bad_signature_keeps_the_old_slot);
    RUN(test_flash_begin_failure);
    RUN(test_flash_write_failure);
    RUN(test_readback_mismatch_keeps_the_old_slot);
    RUN(test_readback_failure_keeps_the_old_slot);
    RUN(test_set_boot_failure_leaves_otadata_alone);
    RUN(test_torn_otadata_write_keeps_the_old_slot);
    RUN(test_at_fault_08_power_cut_at_every_flash_operation);
    RUN(test_power_cut_before_the_switch_never_boots_the_new_slot);
    RUN(test_cancel_in_receive);
    RUN(test_cancel_in_prepare);
    RUN(test_cancel_is_refused_once_the_reboot_is_committed);
    RUN(test_cancel_in_idle_does_nothing);
    RUN(test_start_is_refused_while_busy);
    RUN(test_start_is_refused_without_a_source);
    RUN(test_start_is_refused_while_the_running_image_is_unconfirmed);
    RUN(test_a_second_update_after_a_failure);
    RUN(test_health_check_passes_and_marks_valid);
    RUN(test_health_check_timeout_rolls_back);
    RUN(test_each_subsystem_alone_can_force_a_rollback);
    RUN(test_a_subsystem_that_comes_up_late_still_passes);
    RUN(test_held_hid_state_after_boot_rolls_back);
    RUN(test_operator_can_confirm_before_the_health_check_ends);
    RUN(test_operator_confirm_is_refused_outside_the_health_check);
    RUN(test_operator_confirm_reports_a_failed_mark);
    RUN(test_mark_valid_failure_leaves_the_image_pending);
    RUN(test_normal_boot_states_need_no_health_check);
    RUN(test_null_arguments_are_survivable);
    RUN(test_default_config_is_used_when_none_is_given);
    RUN(test_state_and_reason_names);
    RUN(test_a_small_chunk_size_still_completes);
    TEST_MAIN_END("test_update_sm");
}
