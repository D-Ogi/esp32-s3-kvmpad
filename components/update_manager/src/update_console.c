// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Console hook for TASK-110. The integrator registers update_manager_console_cmd() in
// main/kvm_console.c; the exact table entry is in update_manager_esp.h.
//
// This is the local, physically present operator path of ADR-031: it is reachable only over the
// UART console on the board, never from the CLIENT protocol.
#include <stdio.h>
#include <string.h>

#include "update_manager_esp.h"

static const char *img_state_name(kvm_ota_img_state_t s)
{
    switch (s) {
    case KVM_OTA_IMG_STATE_NEW: return "new";
    case KVM_OTA_IMG_STATE_PENDING_VERIFY: return "pending_verify";
    case KVM_OTA_IMG_STATE_VALID: return "valid";
    case KVM_OTA_IMG_STATE_INVALID: return "invalid";
    case KVM_OTA_IMG_STATE_ABORTED: return "aborted";
    case KVM_OTA_IMG_STATE_UNDEFINED: return "undefined";
    }
    return "?";
}

static void print_status(void)
{
    kvm_ota_status_t st;
    update_manager_slots_t sl;
    update_manager_get_status(&st);
    update_manager_get_slots(&sl);

    printf("state       %s\n", kvm_ota_state_name(st.state));
    printf("last reason %s\n", kvm_ota_reason_name(st.reason));
    printf("input       %s\n", st.input_blocked ? "BLOCKED (maintenance)" : "allowed");
    printf("running     %s state=%s %s\n", sl.running_label, img_state_name(sl.running_state),
           st.confirmed ? "confirmed" : "UNCONFIRMED");
    printf("next slot   %s (%u KiB)\n", sl.next_label, (unsigned)(sl.next_size / 1024u));
    printf("signed ota  %s\n", sl.signature_checked_on_update ? "yes" : "NO - bring-up build only");
    printf("rollback    %s\n", sl.rollback_enabled ? "enabled" : "DISABLED in sdkconfig");
    printf("app         %s %s (idf %s) secure_version %u\n", st.running.project_name,
           st.running.version, st.running.idf_ver, (unsigned)st.running.secure_version);
    if (st.candidate.desc_ok) {
        printf("candidate   %s %s secure_version %u\n", st.candidate.project_name,
               st.candidate.version, (unsigned)st.candidate.secure_version);
    }
    if (st.image_size != 0u) {
        printf("progress    %u%% (%u / %u bytes)\n", (unsigned)st.percent,
               (unsigned)st.bytes_written, (unsigned)st.image_size);
    }
    printf("counters    attempts %u aborts %u rollbacks %u\n", (unsigned)st.attempts,
           (unsigned)st.aborts, (unsigned)st.rollbacks);
    kvm_ota_reason_t rb_reason;
    uint32_t rb_missing = 0;
    char rb_names[48];
    if (update_manager_last_rollback(&rb_reason, &rb_missing, rb_names, sizeof rb_names)) {
        printf("rolled back %s, health missing 0x%x (%s)\n", kvm_ota_reason_name(rb_reason), (unsigned)rb_missing,
               rb_names[0] != '\0' ? rb_names : "none");
    }
    if (st.state == KVM_OTA_HEALTH_CHECK || st.health_sweeps != 0u) {
        printf("health      sweeps %u alive 0x%x missing 0x%x\n", (unsigned)st.health_sweeps,
               (unsigned)st.health_alive_mask, (unsigned)st.health_missing_mask);
    }
}

int update_manager_console_cmd(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "status") == 0) {
        print_status();
        return 0;
    }

    if (strcmp(argv[1], "url") == 0) {
        if (argc < 3) {
            printf("usage: ota url <https-url>\n");
            return 1;
        }
        kvm_ota_reason_t reason = KVM_OTA_R_NONE;
        esp_err_t err = update_manager_begin_https(argv[2], &reason);
        if (err != ESP_OK) {
            printf("refused: %s\n", kvm_ota_reason_name(reason));
            return 1;
        }
        printf("update started; input is blocked until it finishes. `ota status` for progress.\n");
        return 0;
    }

    if (strcmp(argv[1], "cancel") == 0) {
        printf(update_manager_cancel() ? "cancelled; the previous image still boots\n"
                                       : "nothing to cancel (or the reboot is already committed)\n");
        return 0;
    }

    if (strcmp(argv[1], "mark-valid") == 0) {
        esp_err_t err = update_manager_mark_valid();
        printf(err == ESP_OK ? "image confirmed\n" : "not confirmed (no pending image, or the mark failed)\n");
        return err == ESP_OK ? 0 : 1;
    }

    printf("usage: ota [status | url <https-url> | cancel | mark-valid]\n");
    return 1;
}
