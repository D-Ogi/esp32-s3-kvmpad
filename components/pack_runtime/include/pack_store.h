// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#pragma once
#include "pack_card.h"
#include "pack_scene.h"
#include <stdio.h>

enum { PACK_PATH_CAP = 280, PACK_PAGE = 8 };
typedef enum { PACK_LIST_ALL, PACK_LIST_INSTALLED, PACK_LIST_INSTALLERS, PACK_LIST_VIDEO } pack_list_kind_t;
typedef struct {
    char path[PACK_PATH_CAP];
    char label[101];
    bool installed;
    bool manually_disabled;
    bool movie;
    uint8_t prepared_modes;
    bool mp4;
} pack_item_t;
typedef struct {
    uint8_t *bytes;
    size_t len;
    uint8_t digest[32];
    pack_scene_t *scene;
} pack_candidate_t;
typedef struct {
    size_t archive_bytes;
    pack_card_volume_t volume;
    uint64_t generation; // zero for a first installation
    uint8_t candidate_digest[32], previous_digest[32];
    char previous_version[sizeof(((pack_scene_t *)0)->version)];
    unsigned previous_permissions, previous_required;
    unsigned granted; // Local selection for this exact candidate. UI and required permissions accompany approval.
    bool same_archive, downgrade, disabled, manually_disabled;
} pack_review_t;
typedef struct {
    uint64_t generation;
    unsigned slot;
    bool already_installed;
    unsigned granted;
    pack_card_volume_t volume;
} pack_trial_t;

// All storage functions run on the sole card worker. Scene data is independent
// of the card.
esp_err_t pack_card_list(pack_item_t items[PACK_PAGE], unsigned page, unsigned *count, bool *more);
bool pack_video_companion(char *out, size_t capacity, const char *source, unsigned mode);
esp_err_t pack_card_list_kind(pack_item_t items[PACK_PAGE], pack_list_kind_t kind, unsigned page, unsigned *count,
                              bool *more);
esp_err_t pack_card_open(const pack_item_t *item, pack_candidate_t *candidate);
esp_err_t pack_card_review(const pack_candidate_t *candidate, pack_review_t *review);
esp_err_t pack_card_set_enabled(const pack_candidate_t *candidate, const pack_review_t *review, bool enabled);
bool pack_review_needs_downgrade_grant(const pack_review_t *review, const pack_scene_t *scene);
// Only confirm a trial after the approved scene has reached display refresh.
// Recovery ignores tentative indices left by interruption, timeout or Home.
esp_err_t pack_card_stage(const pack_candidate_t *candidate, const pack_review_t *review, bool allow_changed_downgrade,
                          pack_trial_t *trial);
esp_err_t pack_card_confirm(const pack_candidate_t *candidate, const pack_trial_t *trial);
// A plugin-local initialization refusal or first-presentation timeout. Home,
// storage errors and power loss never call this. Counts use monotonic seconds within a boot.
esp_err_t pack_card_activation_failed(const pack_candidate_t *candidate, const pack_trial_t *trial,
                                      uint32_t boot, uint32_t seconds, bool *disabled);
// Uninstall only the healthy revision and volume reviewed on the device.
esp_err_t pack_card_remove(const pack_candidate_t *candidate, const pack_review_t *review);
esp_err_t pack_load(const char *path, pack_candidate_t *candidate);
esp_err_t pack_load_file(FILE *file, pack_candidate_t *candidate);
void pack_dispose(pack_candidate_t *candidate);
bool pack_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
