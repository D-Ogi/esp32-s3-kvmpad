// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// OTA state machine (TASK-110; TECH-052/053, PROC-009). Pure C; contract and invariants in
// update_manager.h. No ESP-IDF header is included here on purpose: the host suite in
// test/update_manager drives every phase, every abort path and a simulated power cut at each phase.
#include "update_manager.h"

#include <string.h>

// ---- small helpers --------------------------------------------------------------------------

static void set_state(kvm_ota_t *m, kvm_ota_state_t s)
{
    if (m->state == s) {
        return;
    }
    m->state = s;
    if (m->plat.progress != NULL) {
        m->plat.progress(m->plat.ctx, s, m->percent_cached);
    }
}

static void set_maintenance(kvm_ota_t *m, bool on)
{
    if (m->maintenance_on == on) {
        return;
    }
    m->maintenance_on = on;
    m->plat.maintenance(m->plat.ctx, on);
}

static uint8_t percent_of(size_t done, size_t total)
{
    if (total == 0u) {
        return 0u;
    }
    if (done >= total) {
        return 100u;
    }
    return (uint8_t)((uint64_t)done * 100u / (uint64_t)total);
}

static void close_source(kvm_ota_t *m)
{
    if (m->source_open) {
        m->source_open = false;
        if (m->src.close != NULL) {
            m->src.close(m->src.ctx);
        }
    }
}

static void abort_slot(kvm_ota_t *m)
{
    if (m->slot_open) {
        m->slot_open = false;
        m->plat.slot_abort(m->plat.ctx);
    }
}

/**
 * End the attempt. The inactive slot is released without a boot switch, so the running slot stays
 * bootable, and input stays blocked until the HID state is neutral (invariant 3).
 */
static void fail(kvm_ota_t *m, kvm_ota_reason_t reason)
{
    close_source(m);
    abort_slot(m);
    m->reason = reason;
    m->aborts++;
    m->unblock_pending = true;
    set_state(m, KVM_OTA_ABORTING);
}

/** Refusals that happen before anything was touched: no abort bookkeeping, no state change. */
static kvm_ota_reason_t refuse(kvm_ota_t *m, kvm_ota_reason_t reason)
{
    m->reason = reason;
    return reason;
}

static kvm_ota_reason_t reason_of_verdict(kvm_ota_image_verdict_t v)
{
    switch (v) {
    case KVM_OTA_IMG_ACCEPT: return KVM_OTA_R_NONE;
    case KVM_OTA_IMG_SHORT: return KVM_OTA_R_TRUNCATED;
    case KVM_OTA_IMG_BAD_MAGIC: return KVM_OTA_R_IMAGE_HEADER;
    case KVM_OTA_IMG_BAD_DESC: return KVM_OTA_R_IMAGE_HEADER;
    case KVM_OTA_IMG_WRONG_CHIP: return KVM_OTA_R_WRONG_CHIP;
    case KVM_OTA_IMG_CHIP_REV: return KVM_OTA_R_CHIP_REV;
    case KVM_OTA_IMG_WRONG_PROJECT: return KVM_OTA_R_WRONG_PROJECT;
    case KVM_OTA_IMG_DOWNGRADE: return KVM_OTA_R_DOWNGRADE;
    case KVM_OTA_IMG_SAME_VERSION: return KVM_OTA_R_SAME_VERSION;
    }
    return KVM_OTA_R_IMAGE_HEADER;
}

static kvm_ota_image_policy_t policy_of(const kvm_ota_t *m)
{
    kvm_ota_image_policy_t pol = {
        .chip_rev_full = m->plat.chip_rev_full != NULL ? m->plat.chip_rev_full(m->plat.ctx) : 0u,
        .require_project_match = m->cfg.require_project_match,
        .allow_same_version = m->cfg.allow_same_version,
    };
    return pol;
}

// ---- lifecycle ------------------------------------------------------------------------------

void kvm_ota_init(kvm_ota_t *m, const kvm_ota_platform_t *plat, const kvm_ota_config_t *cfg)
{
    if (m == NULL || plat == NULL) {
        return;
    }
    memset(m, 0, sizeof *m);
    m->plat = *plat;
    kvm_ota_config_t def = KVM_OTA_CONFIG_DEFAULT;
    m->cfg = cfg != NULL ? *cfg : def;
    if (m->cfg.chunk_bytes == 0u || m->cfg.chunk_bytes > KVM_OTA_CHUNK_MAX) {
        m->cfg.chunk_bytes = KVM_OTA_CHUNK_MAX;
    }
    if (m->cfg.health_consecutive == 0u) {
        m->cfg.health_consecutive = 1u;
    }
    kvm_ota_health_init(&m->health, m->cfg.health_consecutive);
    m->state = KVM_OTA_IDLE;
    m->reason = KVM_OTA_R_NONE;
}

bool kvm_ota_add_probe(kvm_ota_t *m, const char *name, bool (*alive)(void *), void *ctx, bool required)
{
    if (m == NULL) {
        return false;
    }
    return kvm_ota_health_add(&m->health, name, alive, ctx, required);
}

void kvm_ota_boot(kvm_ota_t *m, kvm_ota_img_state_t running_state, uint64_t now_us)
{
    if (m == NULL) {
        return;
    }
    m->now_us = now_us;
    if (m->plat.running_image != NULL) {
        m->plat.running_image(m->plat.ctx, &m->running);
    }

    if (running_state != KVM_OTA_IMG_STATE_PENDING_VERIFY) {
        // Ordinary boot (VALID, UNDEFINED when rollback support is off, or NEW before the
        // bootloader has rewritten it). Nothing to verify, nothing to block.
        m->confirmed = true;
        m->rollback_pending = false;
        set_state(m, KVM_OTA_IDLE);
        return;
    }

    m->rollback_pending = true;
    m->confirmed = false;

    // Invariant 4, checked before maintenance is raised: nothing may be held after a reboot. If the
    // ledger is not empty here, the new image restored state it must not have restored, and the
    // honest answer is a rollback, not a release that papers over it.
    bool neutral = m->plat.hid_neutral(m->plat.ctx);

    // AT-PERF-09: local and remote input stay blocked until the health check has decided.
    m->plat.revoke_sessions(m->plat.ctx);
    set_maintenance(m, true);

    if (!neutral) {
        m->reason = KVM_OTA_R_HELD_AFTER_BOOT;
        m->rollbacks++;
        set_state(m, KVM_OTA_ROLLBACK);
        m->plat.rollback_and_reboot(m->plat.ctx);
        return;
    }

    kvm_ota_health_arm(&m->health, now_us + m->cfg.health_timeout_us);
    set_state(m, KVM_OTA_HEALTH_CHECK);
}

kvm_ota_reason_t kvm_ota_start(kvm_ota_t *m, const kvm_ota_source_t *src, const char *url, uint64_t now_us)
{
    if (m == NULL) {
        return KVM_OTA_R_NO_SOURCE;
    }
    switch (m->state) {
    case KVM_OTA_IDLE:
    case KVM_OTA_FAILED:
    case KVM_OTA_CONFIRMED:
        break;
    case KVM_OTA_HEALTH_CHECK:
    case KVM_OTA_ROLLBACK:
        return refuse(m, KVM_OTA_R_NOT_CONFIRMED);
    default:
        return refuse(m, KVM_OTA_R_BUSY);
    }
    if (!m->confirmed) {
        // esp_ota_begin() would reject this too (ESP_ERR_OTA_ROLLBACK_INVALID_STATE); refusing here
        // keeps the HID interlock from being raised for an attempt that cannot start.
        return refuse(m, KVM_OTA_R_NOT_CONFIRMED);
    }
    if (src == NULL || src->open == NULL || src->read == NULL || url == NULL || url[0] == '\0') {
        return refuse(m, KVM_OTA_R_NO_SOURCE);
    }
    size_t url_len = strlen(url);
    if (url_len >= sizeof m->url) {
        return refuse(m, KVM_OTA_R_NO_SOURCE);
    }

    m->src = *src;
    memcpy(m->url, url, url_len + 1u);
    m->now_us = now_us;
    m->image_size = 0u;
    m->bytes_written = 0u;
    m->probe_len = 0u;
    m->percent_cached = 0u;
    m->candidate_checked = false;
    memset(&m->candidate, 0, sizeof m->candidate);
    m->reason = KVM_OTA_R_NONE;
    m->attempts++;

    // TECH-053, in this order: the client loses control first, then the HID state is emptied, then
    // input is blocked. Only the neutral confirmation lets the transport open.
    m->plat.revoke_sessions(m->plat.ctx);
    set_maintenance(m, true);
    m->unblock_pending = false;
    m->phase_deadline_us = now_us + m->cfg.neutral_timeout_us;
    set_state(m, KVM_OTA_PREPARE);
    return KVM_OTA_R_NONE;
}

bool kvm_ota_cancel(kvm_ota_t *m)
{
    if (m == NULL) {
        return false;
    }
    switch (m->state) {
    case KVM_OTA_PREPARE:
    case KVM_OTA_RECEIVE:
    case KVM_OTA_VERIFY:
    case KVM_OTA_SET_BOOT:
        fail(m, KVM_OTA_R_CANCELLED);
        return true;
    default:
        // REBOOT is committed: otadata already points at the new slot and the health check is the
        // thing that decides. HEALTH_CHECK is not cancellable either - it ends in valid or rollback.
        return false;
    }
}

bool kvm_ota_confirm_now(kvm_ota_t *m)
{
    if (m == NULL || m->state != KVM_OTA_HEALTH_CHECK) {
        return false;
    }
    if (m->plat.mark_valid(m->plat.ctx) != KVM_OTA_SLOT_OK) {
        m->reason = KVM_OTA_R_MARK_VALID;
        return false;
    }
    m->confirmed = true;
    m->rollback_pending = false;
    m->reason = KVM_OTA_R_NONE;
    m->unblock_pending = true;
    set_state(m, KVM_OTA_CONFIRMED);
    return true;
}

// ---- phases ---------------------------------------------------------------------------------

static void phase_prepare(kvm_ota_t *m)
{
    if (!m->plat.hid_neutral(m->plat.ctx)) {
        if (m->now_us >= m->phase_deadline_us) {
            fail(m, KVM_OTA_R_NEUTRAL_TIMEOUT); // nothing was written: the slot was never opened
        }
        return;
    }

    size_t size = 0u;
    if (m->src.open(m->src.ctx, m->url, &size) != 0) {
        fail(m, KVM_OTA_R_SOURCE_OPEN);
        return;
    }
    m->source_open = true;
    if (size == 0u) {
        fail(m, KVM_OTA_R_SIZE_UNKNOWN); // without a length a truncated image cannot be detected
        return;
    }
    size_t cap = m->plat.slot_size(m->plat.ctx);
    if (cap != 0u && size > cap) {
        fail(m, KVM_OTA_R_OVERSIZE);
        return;
    }
    m->image_size = size;

    if (m->plat.slot_begin(m->plat.ctx, size) != KVM_OTA_SLOT_OK) {
        fail(m, KVM_OTA_R_FLASH_BEGIN);
        return;
    }
    m->slot_open = true;
    m->phase_deadline_us = m->now_us + m->cfg.stall_timeout_us;
    set_state(m, KVM_OTA_RECEIVE);
}

/**
 * Run the software checks on the held-back prefix and, when they pass, write it. Returns false when
 * the attempt has been failed.
 */
static bool flush_probe(kvm_ota_t *m)
{
    kvm_ota_image_parse(m->probe, m->probe_len, &m->candidate);
    kvm_ota_image_policy_t pol = policy_of(m);
    kvm_ota_image_verdict_t v = kvm_ota_image_check(&m->candidate, &m->running, &pol);
    m->candidate_checked = true;
    if (v != KVM_OTA_IMG_ACCEPT) {
        fail(m, reason_of_verdict(v));
        return false;
    }
    if (m->plat.slot_write(m->plat.ctx, m->probe, m->probe_len) != KVM_OTA_SLOT_OK) {
        fail(m, KVM_OTA_R_FLASH_WRITE);
        return false;
    }
    m->bytes_written += m->probe_len;
    m->probe_len = 0u;
    return true;
}

static void phase_receive(kvm_ota_t *m)
{
    size_t cap = m->cfg.chunk_bytes;
    if (cap > sizeof m->buf) {
        cap = sizeof m->buf;
    }
    size_t got = 0u;
    if (m->src.read(m->src.ctx, m->buf, cap, &got) != 0) {
        fail(m, KVM_OTA_R_SOURCE_READ);
        return;
    }

    if (got == 0u) {
        // End of stream. Anything short of the declared length is a truncated image; the slot is
        // aborted and the running image keeps the boot partition.
        if (m->probe_len != 0u || m->bytes_written < m->image_size) {
            fail(m, KVM_OTA_R_TRUNCATED);
            return;
        }
        if (m->now_us >= m->phase_deadline_us) {
            fail(m, KVM_OTA_R_SOURCE_READ); // stalled with no data and no end marker
        }
        return;
    }

    if (m->bytes_written + m->probe_len + got > m->image_size) {
        fail(m, KVM_OTA_R_OVERSIZE); // more bytes than declared: do not trust the stream
        return;
    }

    size_t off = 0u;
    // Hold back the prefix the software checks need, so a wrong-chip or downgrade image is refused
    // before a single byte reaches the slot beyond it. Once the checks have run, the rest of the
    // stream goes straight through: the prefix is inspected exactly once per attempt.
    if (!m->candidate_checked) {
        size_t want = KVM_OTA_IMAGE_PROBE_BYTES - (size_t)m->probe_len;
        size_t take = got < want ? got : want;
        memcpy(m->probe + m->probe_len, m->buf, take);
        m->probe_len = (uint16_t)(m->probe_len + take);
        off = take;
        if (m->probe_len < KVM_OTA_IMAGE_PROBE_BYTES) {
            if (m->bytes_written + m->probe_len >= m->image_size) {
                // The whole image is shorter than the prefix: it cannot be a valid app image.
                fail(m, KVM_OTA_R_IMAGE_HEADER);
            }
            m->phase_deadline_us = m->now_us + m->cfg.stall_timeout_us;
            return;
        }
        if (!flush_probe(m)) {
            return;
        }
    }

    if (off < got) {
        if (m->plat.slot_write(m->plat.ctx, m->buf + off, got - off) != KVM_OTA_SLOT_OK) {
            fail(m, KVM_OTA_R_FLASH_WRITE);
            return;
        }
        m->bytes_written += got - off;
    }

    m->percent_cached = percent_of(m->bytes_written, m->image_size);
    if (m->plat.progress != NULL) {
        m->plat.progress(m->plat.ctx, m->state, m->percent_cached);
    }
    m->phase_deadline_us = m->now_us + m->cfg.stall_timeout_us;

    if (m->bytes_written >= m->image_size) {
        close_source(m);
        set_state(m, KVM_OTA_VERIFY);
    }
}

static void phase_verify(kvm_ota_t *m)
{
    // The cryptographic half: esp_ota_end() runs esp_image_verify(), which checks the appended
    // secure boot v2 signature block against the running app's trusted key digest when
    // CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT is set (ADR-031).
    kvm_ota_slot_rc_t rc = m->plat.slot_end(m->plat.ctx);
    m->slot_open = false; // esp_ota_end() consumes the handle whatever it returns
    if (rc != KVM_OTA_SLOT_OK) {
        fail(m, rc == KVM_OTA_SLOT_INVALID ? KVM_OTA_R_SIGNATURE : KVM_OTA_R_FLASH_WRITE);
        return;
    }

    // Independent read-back: what is in the slot now must still answer the software checks. This
    // catches a chunk written out of order or a slot that was not the one we verified.
    uint8_t rb[KVM_OTA_IMAGE_PROBE_BYTES];
    if (m->plat.slot_read(m->plat.ctx, 0u, rb, sizeof rb) != KVM_OTA_SLOT_OK) {
        fail(m, KVM_OTA_R_READBACK);
        return;
    }
    kvm_ota_image_info_t flashed;
    kvm_ota_image_parse(rb, sizeof rb, &flashed);
    kvm_ota_image_policy_t pol = policy_of(m);
    kvm_ota_image_verdict_t v = kvm_ota_image_check(&flashed, &m->running, &pol);
    if (v != KVM_OTA_IMG_ACCEPT) {
        fail(m, reason_of_verdict(v));
        return;
    }
    if (flashed.secure_version != m->candidate.secure_version ||
        strcmp(flashed.version, m->candidate.version) != 0) {
        fail(m, KVM_OTA_R_READBACK);
        return;
    }
    m->candidate = flashed;
    set_state(m, KVM_OTA_SET_BOOT);
}

static void phase_set_boot(kvm_ota_t *m)
{
    if (m->plat.slot_set_boot(m->plat.ctx) != KVM_OTA_SLOT_OK) {
        // otadata was not changed, so the running slot is still the boot slot.
        fail(m, KVM_OTA_R_SET_BOOT);
        return;
    }
    m->phase_deadline_us = m->now_us + m->cfg.reboot_delay_us;
    set_state(m, KVM_OTA_REBOOT);
}

static void phase_reboot(kvm_ota_t *m)
{
    if (m->now_us < m->phase_deadline_us) {
        return;
    }
    // Input stays blocked across the reset: nothing is released into a half-switched device, and
    // nothing held is carried over (the ledger was emptied in PREPARE).
    m->plat.reboot(m->plat.ctx);
}

static void phase_health(kvm_ota_t *m)
{
    kvm_ota_health_result_t r = kvm_ota_health_sweep(&m->health, m->now_us);
    if (r == KVM_OTA_HEALTH_PENDING) {
        return;
    }
    if (r == KVM_OTA_HEALTH_PASS) {
        if (m->plat.mark_valid(m->plat.ctx) != KVM_OTA_SLOT_OK) {
            // The image works but could not be confirmed. Leaving it PENDING_VERIFY means the next
            // reboot rolls back on its own, which is the safe direction; report and release input.
            m->reason = KVM_OTA_R_MARK_VALID;
            m->unblock_pending = true;
            set_state(m, KVM_OTA_CONFIRMED);
            return;
        }
        m->confirmed = true;
        m->rollback_pending = false;
        m->reason = KVM_OTA_R_NONE;
        m->unblock_pending = true;
        set_state(m, KVM_OTA_CONFIRMED);
        return;
    }

    m->reason = KVM_OTA_R_HEALTH_TIMEOUT;
    m->rollbacks++;
    set_state(m, KVM_OTA_ROLLBACK);
    m->plat.rollback_and_reboot(m->plat.ctx); // input stays blocked; the device reboots
}

void kvm_ota_poll(kvm_ota_t *m, uint64_t now_us)
{
    if (m == NULL) {
        return;
    }
    m->now_us = now_us;

    switch (m->state) {
    case KVM_OTA_PREPARE: phase_prepare(m); break;
    case KVM_OTA_RECEIVE: phase_receive(m); break;
    case KVM_OTA_VERIFY: phase_verify(m); break;
    case KVM_OTA_SET_BOOT: phase_set_boot(m); break;
    case KVM_OTA_REBOOT: phase_reboot(m); break;
    case KVM_OTA_HEALTH_CHECK: phase_health(m); break;
    case KVM_OTA_ABORTING:
    case KVM_OTA_CONFIRMED:
    case KVM_OTA_IDLE:
    case KVM_OTA_ROLLBACK:
    case KVM_OTA_FAILED:
        break;
    }

    // Invariant 3, in one place: input is released only once the HID state is neutral. If it never
    // becomes neutral, input stays blocked and the status says so - that is the safe direction.
    if (m->unblock_pending && m->plat.hid_neutral(m->plat.ctx)) {
        m->unblock_pending = false;
        set_maintenance(m, false);
        if (m->state == KVM_OTA_ABORTING) {
            set_state(m, KVM_OTA_FAILED);
        }
    }
}

// ---- projection -----------------------------------------------------------------------------

void kvm_ota_status(const kvm_ota_t *m, kvm_ota_status_t *out)
{
    if (m == NULL || out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->state = m->state;
    out->reason = m->reason;
    out->input_blocked = m->maintenance_on;
    out->confirmed = m->confirmed;
    out->rollback_pending = m->rollback_pending;
    out->image_size = m->image_size;
    out->bytes_written = m->bytes_written;
    out->percent = percent_of(m->bytes_written, m->image_size);
    out->attempts = m->attempts;
    out->aborts = m->aborts;
    out->rollbacks = m->rollbacks;
    out->health_sweeps = m->health.sweeps;
    out->health_alive_mask = m->health.last_mask;
    out->health_missing_mask = kvm_ota_health_missing(&m->health);
    out->running = m->running;
    if (m->candidate_checked) {
        out->candidate = m->candidate;
    }
}

const char *kvm_ota_state_name(kvm_ota_state_t s)
{
    switch (s) {
    case KVM_OTA_IDLE: return "idle";
    case KVM_OTA_PREPARE: return "prepare";
    case KVM_OTA_RECEIVE: return "receive";
    case KVM_OTA_VERIFY: return "verify";
    case KVM_OTA_SET_BOOT: return "set_boot";
    case KVM_OTA_REBOOT: return "reboot";
    case KVM_OTA_HEALTH_CHECK: return "health_check";
    case KVM_OTA_CONFIRMED: return "confirmed";
    case KVM_OTA_ROLLBACK: return "rollback";
    case KVM_OTA_ABORTING: return "aborting";
    case KVM_OTA_FAILED: return "failed";
    }
    return "?";
}

const char *kvm_ota_reason_name(kvm_ota_reason_t r)
{
    static const char *const names[KVM_OTA_R_COUNT] = {
        "none", "busy", "not_confirmed", "no_source", "cancelled", "neutral_timeout",
        "source_open", "source_read", "size_unknown", "oversize", "truncated", "image_header",
        "wrong_chip", "chip_rev", "wrong_project", "downgrade", "same_version", "signature",
        "flash_begin", "flash_write", "readback", "set_boot", "health_timeout",
        "held_after_boot", "mark_valid",
    };
    if ((unsigned)r >= (unsigned)KVM_OTA_R_COUNT || names[r] == NULL) {
        return "?";
    }
    return names[r];
}
