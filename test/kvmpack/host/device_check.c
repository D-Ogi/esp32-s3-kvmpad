// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_store.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *pack_host_swap_after_tombstone;
extern bool (*pack_host_fail_hash)(const uint8_t *, size_t, unsigned);
extern bool (*pack_host_fail_alloc)(size_t);

static const char *incoming_id, *fault_target;
static bool fail_next_alloc;
static size_t publisher_allocation;
static unsigned recovery_failures;

static bool fail_recovery_hash(const uint8_t *data, size_t len, unsigned bits)
{
    bool matches = false;
    if (!strcmp(fault_target, "directory")) {
        // The incoming candidate is already open. Its ID must still resolve normally.
        matches = data != (const uint8_t *)incoming_id && len == strlen(incoming_id) && !memcmp(data, incoming_id, len);
    } else if (!strcmp(fault_target, "member")) {
        matches = len && data[0] == '{';
    } else if (!strcmp(fault_target, "png-memory")) {
        if (len >= 8 && !memcmp(data, "\x89PNG\r\n\x1a\n", 8)) fail_next_alloc = true;
    } else if (!strcmp(fault_target, "signature-hash")) {
        matches = bits == 512;
    } else if (!strcmp(fault_target, "key-fingerprint")) {
        matches = bits == 256 && len == 32;
    }
    if (matches) ++recovery_failures;
    return matches;
}

static bool fail_recovery_alloc(size_t bytes)
{
    if (!fail_next_alloc && bytes != publisher_allocation) return false;
    fail_next_alloc = false;
    ++recovery_failures;
    return true;
}

static void enable_recovery_fault(void)
{
    pack_host_fail_hash = fail_recovery_hash;
    pack_host_fail_alloc = fail_recovery_alloc;
}

static int interrupted_update(const pack_candidate_t *c, const char *target, const char *phase)
{
    incoming_id = c->scene->id;
    fault_target = target;
    if (sscanf(target, "publisher-memory-%zu", &publisher_allocation) != 1)
        publisher_allocation = 0;
    esp_err_t expected = publisher_allocation || !strcmp(target, "png-memory") ? ESP_ERR_NO_MEM : ESP_FAIL;
    pack_review_t review;
    pack_trial_t trial;
    bool during_review = !strcmp(phase, "review");
    if (during_review) enable_recovery_fault();
    esp_err_t result = pack_card_review(c, &review);
    if (result == ESP_OK) {
        enable_recovery_fault();
        result = pack_card_stage(c, &review, false, &trial);
    }
    // Leave the process before confirmation, including when a faulty implementation accepts staging.
    printf("%s/%s failures=%u result=%d\n", target, phase, recovery_failures, (int)result);
    return recovery_failures == 1 && result == expected ? 0 : 1;
}

static esp_err_t swap_card(const pack_candidate_t *candidate, bool after_stage)
{
    pack_review_t review;
    pack_trial_t trial;
    if (pack_card_review(candidate, &review) != ESP_OK ||
        (after_stage && pack_card_stage(candidate, &review, false, &trial) != ESP_OK))
        return ESP_FAIL;
    // Only the synthetic fixture creates these directories in the test working directory.
    if (rename("card", "first") || rename("second", "card"))
        return ESP_FAIL;
    esp_err_t result = after_stage ? pack_card_confirm(candidate, &trial) :
                                     pack_card_stage(candidate, &review, false, &trial);
    if (result == ESP_OK) {
        puts("Previously reviewed installation accepted on replacement card");
        return ESP_FAIL;
    }
    if (after_stage) {
        bool disabled;
        if (pack_card_activation_failed(candidate, &trial, 1, 1, &disabled) == ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

static int list_page(const char *kind, const char *number)
{
    char *end;
    errno = 0;
    unsigned long page = strtoul(number, &end, 10);
    if (!*number || *number == '-' || *end || errno || page > UINT_MAX)
        return 2;
    pack_item_t items[PACK_PAGE] = {0};
    unsigned count = 0;
    bool more = false;
    esp_err_t error;
    if (!strcmp(kind, "legacy")) {
        error = pack_card_list(items, (unsigned)page, &count, &more);
    } else {
        const char *names[] = {"all", "installed", "installers", "video"};
        const pack_list_kind_t kinds[] = {PACK_LIST_ALL, PACK_LIST_INSTALLED, PACK_LIST_INSTALLERS, PACK_LIST_VIDEO};
        unsigned i = 0;
        while (i < sizeof(names) / sizeof(names[0]) && strcmp(kind, names[i]))
            ++i;
        if (i == sizeof(names) / sizeof(names[0]))
            return 2;
        error = pack_card_list_kind(items, kinds[i], (unsigned)page, &count, &more);
    }
    if (error != ESP_OK || count > PACK_PAGE)
        return 1;
    printf("%u %u\n", count, more ? 1u : 0u);
    for (unsigned i = 0; i < count; ++i)
        printf("%d%d%d\t%s\t%s\n", items[i].installed, items[i].movie, items[i].mp4, items[i].path, items[i].label);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "page"))
        return list_page(argv[2], argv[3]);
    bool failure = argc == 5 && !strcmp(argv[1], "activation-failure");
    bool interrupted = argc == 5 && !strcmp(argv[1], "interrupted-update");
    bool approve = argc == 4 && !strcmp(argv[1], "approve-mask");
    if (argc != 3 && !failure && !approve && !interrupted)
        return 2;
    pack_item_t item = {0};
    if (strlen(argv[2]) >= sizeof(item.path))
        return 2;
    strcpy(item.path, argv[2]);
    bool preference = !strcmp(argv[1], "enable") || !strcmp(argv[1], "disable") || !strcmp(argv[1], "pause-trial");
    bool before_remove = !strcmp(argv[1], "swap-before-remove"), during_remove = !strcmp(argv[1], "swap-during-remove");
    bool remove = !strcmp(argv[1], "remove") || before_remove || during_remove || !strcmp(argv[1], "stale-remove");
    item.installed = !strcmp(argv[1], "recover") || remove || preference;
    if (!strcmp(argv[1], "list")) {
        pack_item_t items[PACK_PAGE];
        bool more = false;
        unsigned n = 0;
        if (pack_card_list(items, 0, &n, &more) != ESP_OK)
            return 1;
        for (unsigned i = 0; i < n; ++i)
            puts(items[i].label);
        return 0;
    }
    pack_candidate_t c = {0};
    esp_err_t err = pack_card_open(&item, &c);
    if (!strcmp(argv[1], "invalid-integrity") || !strcmp(argv[1], "unsupported-data")) {
        esp_err_t expected = !strcmp(argv[1], "invalid-integrity") ? ESP_ERR_INVALID_RESPONSE : ESP_ERR_NOT_SUPPORTED;
        pack_dispose(&c);
        return err == expected ? 0 : 1;
    }
    if (err != ESP_OK)
        return 1;
    if (interrupted) {
        int result = interrupted_update(&c, argv[3], argv[4]);
        pack_dispose(&c);
        return result;
    }
    if (!strcmp(argv[1], "swap-after-review") || !strcmp(argv[1], "swap-after-stage"))
        err = swap_card(&c, !strcmp(argv[1], "swap-after-stage"));
    if (preference || !strcmp(argv[1], "enable-orphan")) {
        pack_review_t review;
        err = pack_card_review(&c, &review);
        if (!strcmp(argv[1], "pause-trial")) {
            pack_trial_t trial;
            if (err == ESP_OK) err = pack_card_stage(&c, &review, false, &trial);
            if (err == ESP_OK) err = pack_card_set_enabled(&c, &review, false);
            if (err == ESP_OK) err = pack_card_confirm(&c, &trial) == ESP_ERR_INVALID_STATE ? ESP_OK : ESP_FAIL;
        } else if (err == ESP_OK)
            err = pack_card_set_enabled(&c, &review, strcmp(argv[1], "disable") != 0);
    }
    if (failure) {
        pack_review_t review;
        pack_trial_t trial;
        bool disabled = false;
        err = pack_card_review(&c, &review);
        if (err == ESP_OK) err = pack_card_stage(&c, &review, false, &trial);
        if (err == ESP_OK) err = pack_card_activation_failed(&c, &trial, (uint32_t)strtoul(argv[3],NULL,10),
                                                             (uint32_t)strtoul(argv[4],NULL,10), &disabled);
        printf("failure disabled=%d\n", disabled);
    }
    if (!strcmp(argv[1], "stale-review") || !strcmp(argv[1], "stale-remove")) {
        pack_candidate_t other = {0};
        pack_review_t approved, replacement;
        pack_trial_t trial;
        err = pack_card_review(&c, &approved);
        if (err == ESP_OK)
            err = pack_load("replacement.kvmpack", &other);
        if (err == ESP_OK)
            err = pack_card_review(&other, &replacement);
        if (err == ESP_OK)
            err = pack_card_stage(&other, &replacement, true, &trial);
        if (err == ESP_OK)
            err = pack_card_confirm(&other, &trial);
        if (err == ESP_OK) {
            esp_err_t result = remove ? pack_card_remove(&c, &approved) : pack_card_stage(&c, &approved, true, &trial);
            err = result == ESP_ERR_INVALID_STATE ? ESP_OK : ESP_FAIL;
        }
        pack_dispose(&other);
    }
    if (approve || !strcmp(argv[1], "install") || !strcmp(argv[1], "stage") || !strcmp(argv[1], "review") ||
        !strcmp(argv[1], "allow-downgrade")) {
        pack_trial_t trial;
        pack_review_t review;
        err = pack_card_review(&c, &review);
        if (err == ESP_OK)
            printf("previous=%s permissions=%u required=%u downgrade=%d extra=%d same=%d bytes=%zu disabled=%d manual=%d granted=%u\n",
                   review.generation ? review.previous_version : "none", review.previous_permissions,
                   review.previous_required, review.downgrade, pack_review_needs_downgrade_grant(&review,c.scene),
                   review.same_archive, review.archive_bytes, review.disabled, review.manually_disabled, review.granted);
        if (approve && err == ESP_OK) review.granted = (unsigned)strtoul(argv[3], NULL, 10);
        if (err == ESP_OK && strcmp(argv[1], "review"))
            err = pack_card_stage(&c, &review, !strcmp(argv[1], "allow-downgrade"), &trial);
        if (err == ESP_OK && (approve || !strcmp(argv[1], "install") || !strcmp(argv[1], "allow-downgrade")))
            err = pack_card_confirm(&c, &trial);
    }
    if (!strcmp(argv[1], "remove") || before_remove || during_remove) {
        pack_review_t review;
        err = pack_card_review(&c, &review);
        if (err == ESP_OK && before_remove && (rename("card", "first") || rename("second", "card")))
            err = ESP_FAIL;
        if (err == ESP_OK) {
            char tombstone[PACK_PATH_CAP + sizeof("/OFF.IDX")];
            if (during_remove) {
                snprintf(tombstone, sizeof(tombstone), "%s/OFF.IDX", item.path);
                pack_host_swap_after_tombstone = tombstone;
            }
            err = pack_card_remove(&c, &review);
            if (before_remove || during_remove)
                err = err != ESP_OK && !pack_host_swap_after_tombstone ? ESP_OK : ESP_FAIL;
        }
    }
    if (!strcmp(argv[1], "tap")) {
        bool worked = false;
        for (unsigned i = 0; i < c.scene->widgets; ++i)
            if (!strcmp(c.scene->widget[i].id, "complete")) {
                worked = pack_scene_tap(c.scene, i);
            }
        if (!worked || c.scene->bindings != 1 || strcmp(c.scene->state[c.scene->binding[0].source].value.text, "Complete"))
            err = ESP_FAIL;
    }
    printf("%s %s\n", c.scene->id, c.scene->version);
    if (c.scene->publisher_verified) {
        printf("publisher=");
        for (unsigned i = 0; i < sizeof(c.scene->publisher_key_sha256); ++i)
            printf("%02x", c.scene->publisher_key_sha256[i]);
        puts("");
    } else {
        puts("unsigned");
    }
    pack_dispose(&c);
    return err == ESP_OK ? 0 : 1;
}
