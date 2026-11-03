// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// OTA state machine (TASK-110; TECH-052/053, PROC-009, ADR-012, ADR-031).
//
// Pure C, no RTOS, no allocation, no ESP-IDF: the flash slot, the clock, the HID/maintenance
// interlock and the image source are injected, so every phase — including a power cut at each one
// — is exercised on the host in test/update_manager.
//
//   IDLE -> PREPARE -> RECEIVE -> VERIFY -> SET_BOOT -> REBOOT ..reset..
//                                                       |
//   (next boot) PENDING_VERIFY -> HEALTH_CHECK -> CONFIRMED | ROLLBACK
//
// Invariants this file enforces (not advisory):
//   1. Nothing is written to the inactive slot before the HID state is neutral. PREPARE revokes the
//      remote session, raises maintenance (which makes hid_core RELEASE_ALL with
//      KVM_RA_MAINTENANCE) and waits for the neutral confirmation, bounded by a timeout.
//   2. Every abort path leaves the currently running slot bootable: the boot partition is switched
//      exactly once, in SET_BOOT, after the signature has been verified.
//   3. Input is unblocked only after the HID state is neutral again — never on the way out of a
//      failure, never speculatively.
//   4. Nothing held before the update is restored after the reboot. The first health sweep checks
//      that the fresh image really came up neutral and fails the update if it did not.
//   5. A locally triggered image source only (ADR-031). This component exposes no entry point that
//      a remote session could reach; see AT-SEC-07.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "update_health.h"
#include "update_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Phases. `*_name()` strings are stable: the console, the UI state and the tests use them. */
typedef enum {
    KVM_OTA_IDLE = 0,
    KVM_OTA_PREPARE,       // TECH-053: revoke, release, block input, wait for neutral
    KVM_OTA_RECEIVE,       // bounded chunks into the inactive slot
    KVM_OTA_VERIFY,        // signature + image header + anti-rollback, read back from flash
    KVM_OTA_SET_BOOT,      // the single otadata switch
    KVM_OTA_REBOOT,        // "Updating - restarting", input still blocked
    KVM_OTA_HEALTH_CHECK,  // first boot of the new image, input still blocked (AT-PERF-09)
    KVM_OTA_CONFIRMED,     // marked valid; input released
    KVM_OTA_ROLLBACK,      // marked invalid, reboot into the previous slot requested
    KVM_OTA_ABORTING,      // failed; waiting for the neutral state before unblocking input
    KVM_OTA_FAILED,        // terminal, old slot intact, input released
} kvm_ota_state_t;

/** Why an attempt ended. KVM_OTA_R_NONE while an attempt is still running or has succeeded. */
typedef enum {
    KVM_OTA_R_NONE = 0,
    KVM_OTA_R_BUSY,              // start() refused: an attempt is already running
    KVM_OTA_R_NOT_CONFIRMED,     // start() refused: the running image is still PENDING_VERIFY
    KVM_OTA_R_NO_SOURCE,         // start() refused: no transport or no URL
    KVM_OTA_R_CANCELLED,         // operator cancelled
    KVM_OTA_R_NEUTRAL_TIMEOUT,   // HID did not reach the neutral state: nothing was written
    KVM_OTA_R_SOURCE_OPEN,       // transport could not open the image (TLS, HTTP status, DNS)
    KVM_OTA_R_SOURCE_READ,       // transport error or stall while receiving
    KVM_OTA_R_SIZE_UNKNOWN,      // source did not declare a length
    KVM_OTA_R_OVERSIZE,          // declared or delivered length does not fit the slot
    KVM_OTA_R_TRUNCATED,         // source ended before the declared length
    KVM_OTA_R_IMAGE_HEADER,      // not an ESP app image / no app descriptor
    KVM_OTA_R_WRONG_CHIP,        // built for another chip
    KVM_OTA_R_CHIP_REV,          // built for another silicon revision
    KVM_OTA_R_WRONG_PROJECT,     // another project's firmware
    KVM_OTA_R_DOWNGRADE,         // secure_version lower than the running image
    KVM_OTA_R_SAME_VERSION,      // identical version while reflashing is disallowed
    KVM_OTA_R_SIGNATURE,         // esp_ota_end()/esp_image_verify(): signature or image invalid
    KVM_OTA_R_FLASH_BEGIN,       // esp_ota_begin() failed (slot erase)
    KVM_OTA_R_FLASH_WRITE,       // esp_ota_write() failed
    KVM_OTA_R_READBACK,          // flash read-back disagrees with what was accepted
    KVM_OTA_R_SET_BOOT,          // esp_ota_set_boot_partition() failed; otadata untouched
    KVM_OTA_R_HEALTH_TIMEOUT,    // the new image did not become healthy in time -> rollback
    KVM_OTA_R_HELD_AFTER_BOOT,   // TECH-053 violated: HID state was not neutral after the reboot
    KVM_OTA_R_MARK_VALID,        // esp_ota_mark_app_valid_cancel_rollback() failed
    KVM_OTA_R_COUNT,
} kvm_ota_reason_t;

/** Mirror of esp_ota_img_states_t (esp_flash_partitions.h); the adapter static-asserts the values. */
typedef enum {
    KVM_OTA_IMG_STATE_NEW = 0x0,
    KVM_OTA_IMG_STATE_PENDING_VERIFY = 0x1,
    KVM_OTA_IMG_STATE_VALID = 0x2,
    KVM_OTA_IMG_STATE_INVALID = 0x3,
    KVM_OTA_IMG_STATE_ABORTED = 0x4,
    KVM_OTA_IMG_STATE_UNDEFINED = 0xFFFFFFFF,
} kvm_ota_img_state_t;

/** Return codes of the platform slot hooks: no esp_err_t reaches the pure code. */
typedef enum {
    KVM_OTA_SLOT_OK = 0,
    KVM_OTA_SLOT_INVALID, // the image or its signature did not verify
    KVM_OTA_SLOT_ERROR,   // anything else (flash, state, arguments)
} kvm_ota_slot_rc_t;

/**
 * Platform hooks. Every one is mandatory except `progress`. None may block for long: the state
 * machine is polled from one task and holds no lock.
 */
typedef struct {
    void *ctx;

    uint64_t (*now_us)(void *ctx);

    // --- HID / session interlock (TECH-053) ---
    /** Raise or drop maintenance. Raising it must revoke the remote lease and RELEASE_ALL. */
    void (*maintenance)(void *ctx, bool on);
    /** True when nothing is held: empty pressed-ledger and no chord in flight. */
    bool (*hid_neutral)(void *ctx);
    /** Close the remote session(s) before the image is touched. */
    void (*revoke_sessions)(void *ctx);
    /** Optional: phase and percentage for the "Updating" screen and the log. */
    void (*progress)(void *ctx, kvm_ota_state_t state, uint8_t percent);

    // --- inactive slot ---
    size_t (*slot_size)(void *ctx);
    kvm_ota_slot_rc_t (*slot_begin)(void *ctx, size_t image_size);
    kvm_ota_slot_rc_t (*slot_write)(void *ctx, const void *data, size_t len);
    /** Finish and verify: image format plus the appended signature (ADR-031). */
    kvm_ota_slot_rc_t (*slot_end)(void *ctx);
    /** Release the handle without switching the boot partition. Must always be safe to call. */
    void (*slot_abort)(void *ctx);
    /** Read back what was written, for the independent post-write check. */
    kvm_ota_slot_rc_t (*slot_read)(void *ctx, size_t offset, void *buf, size_t len);
    /** The one and only otadata switch. */
    kvm_ota_slot_rc_t (*slot_set_boot)(void *ctx);

    // --- running image / rollback ---
    void (*running_image)(void *ctx, kvm_ota_image_info_t *out);
    uint16_t (*chip_rev_full)(void *ctx);
    kvm_ota_slot_rc_t (*mark_valid)(void *ctx);
    /** Mark invalid and reboot into the previous slot. Does not return on real hardware. */
    void (*rollback_and_reboot)(void *ctx);
    void (*reboot)(void *ctx);
} kvm_ota_platform_t;

/**
 * Image source. Implemented on the device by a locally triggered HTTPS pull
 * (`update_transport_https.c`) and on the host by a fake. `read` returns `*out_len == 0` for the
 * end of the stream and a non-zero code for a transport error.
 */
typedef struct {
    void *ctx;
    int (*open)(void *ctx, const char *url, size_t *out_size);
    int (*read)(void *ctx, void *buf, size_t cap, size_t *out_len);
    void (*close)(void *ctx);
} kvm_ota_source_t;

/** Largest chunk moved per poll, and the longest image URL the manager stores. */
#define KVM_OTA_CHUNK_MAX 4096u
#define KVM_OTA_URL_MAX 192u

typedef struct {
    uint64_t neutral_timeout_us;  // PREPARE: wait for the HID-neutral confirmation
    uint64_t stall_timeout_us;    // RECEIVE: no progress for this long -> abort
    uint64_t reboot_delay_us;     // REBOOT: let the screen and the log catch up
    uint64_t health_timeout_us;   // HEALTH_CHECK budget before a rollback
    uint32_t chunk_bytes;         // bytes moved per kvm_ota_poll() call, clamped to KVM_OTA_CHUNK_MAX
    uint8_t health_consecutive;   // clean sweeps required before the image is marked valid
    bool allow_same_version;
    bool require_project_match;
} kvm_ota_config_t;

// 2 s neutral: hid_core empties the ledger synchronously and only the neutral reports have to drain.
// 30 s stall: a Wi-Fi hiccup must not abort an update, a dead link must not hang it.
// 20 s health: USB re-enumeration plus the first LVGL frames fit with room to spare on board #1.
#define KVM_OTA_CONFIG_DEFAULT                                                                       \
    {                                                                                                \
        .neutral_timeout_us = 2000000u, .stall_timeout_us = 30000000u,                               \
        .reboot_delay_us = 1500000u, .health_timeout_us = 20000000u, .chunk_bytes = 4096u,           \
        .health_consecutive = 3u, .allow_same_version = true, .require_project_match = true,         \
    }

/** Read-only projection for the console, the status service and the "Updating" screen. */
typedef struct {
    kvm_ota_state_t state;
    kvm_ota_reason_t reason;       // of the last finished attempt
    bool input_blocked;            // maintenance is raised
    bool confirmed;                // the running image needs no further verification
    bool rollback_pending;         // the running image is PENDING_VERIFY
    size_t image_size;
    size_t bytes_written;
    uint8_t percent;
    uint32_t attempts;
    uint32_t aborts;
    uint32_t rollbacks;
    uint32_t health_sweeps;
    uint32_t health_alive_mask;    // probes alive on the last sweep
    uint32_t health_missing_mask;  // required probes that never reported alive
    kvm_ota_image_info_t running;
    kvm_ota_image_info_t candidate; // zeroed until the probe prefix has been parsed
} kvm_ota_status_t;

typedef struct {
    kvm_ota_platform_t plat;
    kvm_ota_config_t cfg;
    kvm_ota_source_t src;
    kvm_ota_health_t health;

    kvm_ota_state_t state;
    kvm_ota_reason_t reason;
    bool source_open;
    bool slot_open;
    bool maintenance_on;
    bool unblock_pending;
    bool confirmed;
    bool rollback_pending;
    bool candidate_checked;

    uint64_t phase_deadline_us; // PREPARE neutral / RECEIVE stall / REBOOT delay
    uint64_t now_us;

    size_t image_size;
    size_t bytes_written;
    uint8_t percent_cached;                        // last value handed to progress()
    uint16_t probe_len;                            // bytes held back for the software checks
    uint8_t probe[KVM_OTA_IMAGE_PROBE_BYTES];
    char url[KVM_OTA_URL_MAX];                     // own copy: the caller's string may be transient
    uint8_t buf[KVM_OTA_CHUNK_MAX];                // one chunk; no allocation anywhere

    kvm_ota_image_info_t running;
    kvm_ota_image_info_t candidate;

    uint32_t attempts;
    uint32_t aborts;
    uint32_t rollbacks;
} kvm_ota_t;

/** Install the platform hooks. `cfg` NULL takes KVM_OTA_CONFIG_DEFAULT. */
void kvm_ota_init(kvm_ota_t *m, const kvm_ota_platform_t *plat, const kvm_ota_config_t *cfg);

/** Register a health probe (USB, HID scheduler, LVGL owner, touch reader). Call before kvm_ota_boot(). */
bool kvm_ota_add_probe(kvm_ota_t *m, const char *name, bool (*alive)(void *), void *ctx, bool required);

/**
 * Boot entry, called once with the OTA state of the *running* partition
 * (esp_ota_get_state_partition). PENDING_VERIFY starts the health check with input blocked; any
 * other state means a normal boot and the image counts as confirmed.
 */
void kvm_ota_boot(kvm_ota_t *m, kvm_ota_img_state_t running_state, uint64_t now_us);

/**
 * Begin an update from `src`/`url`. Locally triggered only. Returns KVM_OTA_R_NONE when the attempt
 * started, or the refusal reason. The transport is not touched until the HID state is neutral.
 */
kvm_ota_reason_t kvm_ota_start(kvm_ota_t *m, const kvm_ota_source_t *src, const char *url, uint64_t now_us);

/** Operator cancel. Allowed up to and including SET_BOOT; refused once the reboot is committed. */
bool kvm_ota_cancel(kvm_ota_t *m);

/**
 * Operator confirmation of the running image while the health check is still pending (`ota
 * mark-valid`). Marks the image valid and releases input through the same path as an automatic
 * confirmation. Returns false outside KVM_OTA_HEALTH_CHECK or when marking valid failed.
 */
bool kvm_ota_confirm_now(kvm_ota_t *m);

/** Drive the machine. Moves at most cfg.chunk_bytes per call and never blocks on the network. */
void kvm_ota_poll(kvm_ota_t *m, uint64_t now_us);

void kvm_ota_status(const kvm_ota_t *m, kvm_ota_status_t *out);

const char *kvm_ota_state_name(kvm_ota_state_t s);
const char *kvm_ota_reason_name(kvm_ota_reason_t r);

#ifdef __cplusplus
}
#endif
