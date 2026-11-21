// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_store.h"
#include "pack_port.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#endif

#define ROOT PACK_MOUNT "/KVPACKS"
typedef struct {
    uint64_t generation;
    uint8_t digest[32];
    bool io_error;
} record_t;

static bool make_dir(const char *path)
{
#ifdef _WIN32
    int result = _mkdir(path);
#else
    int result = mkdir(path, 0777);
#endif
    if (!result)
        return true;
    struct stat st;
    return errno == EEXIST && !stat(path, &st) && S_ISDIR(st.st_mode);
}

static bool directory(const char *id, char out[PACK_PATH_CAP], bool create)
{
    uint8_t digest[32];
    if (!pack_sha256((const uint8_t *)id, strlen(id), digest))
        return false;
    strcpy(out, ROOT);
    if (create && !make_dir(out))
        return false;
    size_t n = strlen(out);
    for (unsigned seg = 0; seg < 8; ++seg) {
        out[n++] = '/';
        for (unsigned j = 0; j < 4; ++j) {
            uint8_t v = digest[seg * 4 + j];
            static const char hex[] = "0123456789abcdef";
            out[n++] = hex[v >> 4];
            out[n++] = hex[v & 15];
        }
        out[n] = 0;
        if (create && !make_dir(out))
            return false;
    }
    return true;
}

static void slot_path(char out[PACK_PATH_CAP], const char *dir, int slot, const char *ext)
{
    // All runtime slot directories are ROOT followed by eight 8-character
    // segments.
    snprintf(out, PACK_PATH_CAP, "%s/%c.%s", dir, 'A' + slot, ext);
}

static bool synced_write(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, len, f) == len && !fflush(f);
#ifdef _WIN32
    ok = ok && !_commit(_fileno(f));
#else
    ok = ok && !fsync(fileno(f));
#endif
    return fclose(f) == 0 && ok;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Two bounded, alternating records per archive slot. Keep the quarantined digest
// separately so running the known-good revision never unlocks the failed one.
enum { HEALTH_BYTES = 96, HEALTH_WINDOW_S = 120 };
typedef struct {
    uint64_t sequence;
    uint8_t blocked[32], pending[32];
    uint32_t boot, times[2];
    unsigned count;
    bool disabled;
} health_t;

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

// The grant record contains optional choices for one exact archive. An older card,
// a replaced archive or a torn write starts with all optional permissions off.
static esp_err_t read_optional(const char *path, const uint8_t digest[32], unsigned *mask)
{
    *mask = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    uint8_t bytes[45];
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    bool ok = !ferror(f);
    if (fclose(f) || !ok) return ESP_FAIL;
    if (n == 44 && !memcmp(bytes, "KPG1", 4) && !memcmp(bytes + 4, digest, 32) &&
        le32(bytes + 40) == pack_crc(bytes, 40))
        *mask = le32(bytes + 36);
    return ESP_OK;
}

static bool save_optional(const char *dir, const pack_candidate_t *c, const pack_trial_t *trial)
{
    char path[PACK_PATH_CAP];
    slot_path(path, dir, (int)trial->slot, "GRT");
    unsigned selected = trial->granted & ~(PACK_PERM_UI | c->scene->required_permissions);
    unsigned previous;
    if (read_optional(path, c->digest, &previous) != ESP_OK) return false;
    if (selected == previous) return true;
    uint8_t record[44] = {'K', 'P', 'G', '1'};
    memcpy(record + 4, c->digest, 32);
    put32(record + 36, selected);
    put32(record + 40, pack_crc(record, 40));
    return synced_write(path, record, sizeof(record)) &&
           read_optional(path, c->digest, &previous) == ESP_OK && selected == previous;
}

static bool grants_valid(const pack_scene_t *scene, unsigned granted)
{
    unsigned required = PACK_PERM_UI | scene->required_permissions;
    return (granted & required) == required && !(granted & ~scene->permissions);
}

static esp_err_t health_read(const char *dir, unsigned slot, health_t *out)
{
    *out = (health_t){0};
    for (unsigned copy = 0; copy < 2; ++copy) {
        char path[PACK_PATH_CAP];
        slot_path(path, dir, (int)slot, copy ? "H1" : "H0");
        FILE *f = fopen(path, "rb");
        if (!f) {
            if (errno != ENOENT) return ESP_FAIL;
            continue;
        }
        uint8_t b[HEALTH_BYTES + 1];
        size_t n = fread(b, 1, sizeof(b), f);
        bool error = ferror(f) != 0;
        fclose(f);
        if (error) return ESP_FAIL;
        if (n != HEALTH_BYTES || memcmp(b, "KPH1", 4) || b[88] > 2 || b[89] > 1 ||
            b[90] || b[91] || le32(b + 92) != pack_crc(b, 92)) continue;
        uint64_t seq = le32(b + 4) | (uint64_t)le32(b + 8) << 32;
        if ((seq & 1u) != copy || seq <= out->sequence) continue;
        out->sequence = seq;
        memcpy(out->blocked, b + 12, 32);
        memcpy(out->pending, b + 44, 32);
        out->boot = le32(b + 76);
        out->times[0] = le32(b + 80);
        out->times[1] = le32(b + 84);
        out->count = b[88];
        out->disabled = b[89] != 0;
    }
    return ESP_OK;
}

static esp_err_t health_write(const char *dir, unsigned slot, const health_t *h)
{
    if (h->sequence == UINT64_MAX) return ESP_ERR_INVALID_STATE;
    uint64_t seq = h->sequence + 1;
    uint8_t b[HEALTH_BYTES] = {'K','P','H','1'};
    char path[PACK_PATH_CAP];
    slot_path(path, dir, (int)slot, (seq & 1u) ? "H1" : "H0");
    put32(b + 4, (uint32_t)seq);
    put32(b + 8, (uint32_t)(seq >> 32));
    memcpy(b + 12, h->blocked, 32);
    memcpy(b + 44, h->pending, 32);
    put32(b + 76, h->boot);
    put32(b + 80, h->times[0]);
    put32(b + 84, h->times[1]);
    b[88] = (uint8_t)h->count;
    b[89] = h->disabled;
    put32(b + 92, pack_crc(b, 92));
    health_t actual;
    return synced_write(path, b, sizeof(b)) && health_read(dir,slot,&actual) == ESP_OK &&
           actual.sequence == seq ? ESP_OK : ESP_FAIL;
}

static bool quarantined(const health_t h[2], const uint8_t digest[32])
{
    return (h[0].disabled && !memcmp(h[0].blocked, digest, 32)) ||
           (h[1].disabled && !memcmp(h[1].blocked, digest, 32));
}

static record_t index_read(const char *dir, int slot, bool tentative)
{
    char path[PACK_PATH_CAP];
    slot_path(path, dir, slot, "IDX");
    uint8_t b[49];
    record_t r = {0};
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(b, 1, sizeof(b), f);
        r.io_error = ferror(f) != 0;
        if (n == 48 && !r.io_error && !memcmp(b, tentative ? "KVT1" : "KVP1", 4) && le32(b + 44) == pack_crc(b, 44)) {
            r.generation = le32(b + 4) | (uint64_t)le32(b + 8) << 32;
            memcpy(r.digest, b + 12, 32);
        }
        fclose(f);
    } else if (errno != ENOENT)
        r.io_error = true;
    return r;
}

static int marker_state(const char *dir, const char *name)
{
    char path[PACK_PATH_CAP + sizeof("/PAUSE.IDX")];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    struct stat st;
    if (stat(path, &st) == 0)
        return 1;
    return errno == ENOENT ? 0 : -1;
}

static esp_err_t remove_pause(const char *dir)
{
    char path[PACK_PATH_CAP + sizeof("/PAUSE.IDX")];
    snprintf(path, sizeof(path), "%s/PAUSE.IDX", dir);
    return unlink(path) == 0 || errno == ENOENT ? ESP_OK : ESP_FAIL;
}

// The removal marker stays visible throughout cleanup, including a retry after power loss.
static esp_err_t clear_installation(const char *dir, const pack_card_volume_t *volume)
{
    if (!pack_card_same_volume(volume) || remove_pause(dir) != ESP_OK)
        return ESP_FAIL;
    static const char *const files[] = {"IDX", "KVP", "GRT", "H0", "H1"};
    char path[PACK_PATH_CAP];
    for (int slot = 0; slot < 2; ++slot) {
        for (unsigned j = 0; j < sizeof(files) / sizeof(files[0]); ++j) {
            slot_path(path, dir, slot, files[j]);
            if (!pack_card_same_volume(volume) || (unlink(path) && errno != ENOENT))
                return ESP_FAIL;
        }
    }
    return pack_card_same_volume(volume) ? ESP_OK : ESP_FAIL;
}

static esp_err_t recover(const char *dir, pack_candidate_t *c, int *slot, uint64_t *generation,
                         const uint8_t *requested, bool *disabled)
{
    int removed = marker_state(dir, "OFF.IDX");
    if (removed) {
        if (disabled) *disabled = false;
        return removed > 0 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    health_t h[2];
    if (health_read(dir, 0, &h[0]) != ESP_OK || health_read(dir, 1, &h[1]) != ESP_OK)
        return ESP_FAIL;
    if (disabled)
        *disabled = quarantined(h, requested);
    record_t r[] = {index_read(dir, 0, false), index_read(dir, 1, false)};
    if (r[0].io_error || r[1].io_error)
        return ESP_FAIL;
    if (r[0].generation && r[0].generation == r[1].generation)
        return ESP_ERR_INVALID_STATE;
    int newest = r[1].generation > r[0].generation;
    for (unsigned n = 0; n < 2; ++n) {
        int i = newest ^ (int)n;
        if (!r[i].generation || quarantined(h, r[i].digest))
            continue;
        char file[PACK_PATH_CAP], canonical[PACK_PATH_CAP];
        slot_path(file, dir, i, "KVP");
        esp_err_t e = pack_load(file, c);
        if (e == ESP_ERR_NO_MEM || e == ESP_FAIL)
            return e;
        if (e != ESP_OK)
            continue;
        if (!memcmp(r[i].digest, c->digest, 32)) {
            if (!directory(c->scene->id, canonical, false)) {
                pack_dispose(c);
                return ESP_FAIL;
            }
            if (!strcasecmp(dir, canonical)) {
                *slot = i;
                *generation = r[i].generation;
                return ESP_OK;
            }
        }
        pack_dispose(c);
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t pack_card_open(const pack_item_t *item, pack_candidate_t *c)
{
    int slot;
    uint64_t generation;
    return item->installed ? recover(item->path, c, &slot, &generation, NULL, NULL) : pack_load(item->path, c);
}

static bool matches_payload(const char *path, const pack_candidate_t *c)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    uint8_t readback[512];
    bool ok = true;
    for (size_t pos = 0; pos < c->len && ok;) {
        size_t n = c->len - pos;
        if (n > sizeof(readback))
            n = sizeof(readback);
        ok = fread(readback, 1, n, f) == n && !memcmp(readback, c->bytes + pos, n);
        pos += n;
    }
    ok = ok && fgetc(f) == EOF && !ferror(f);
    return fclose(f) == 0 && ok;
}

static bool write_index(const char *dir, const pack_trial_t *trial, const uint8_t digest[32], bool tentative)
{
    uint8_t index[48] = {'K', 'V', 'P', '1'};
    char path[PACK_PATH_CAP];
    if (tentative)
        index[2] = 'T';
    for (unsigned i = 0; i < 8; i++)
        index[4 + i] = (uint8_t)(trial->generation >> (8 * i));
    memcpy(index + 12, digest, 32);
    uint32_t crc = pack_crc(index, 44);
    for (unsigned i = 0; i < 4; i++)
        index[44 + i] = (uint8_t)(crc >> (8 * i));
    slot_path(path, dir, trial->slot, "IDX");
    if (!synced_write(path, index, sizeof(index)))
        return false;
    record_t written = index_read(dir, trial->slot, tentative);
    return written.generation == trial->generation && !memcmp(written.digest, digest, 32);
}

// The manifest parser admitted three decimal components, each at most 65535.
static bool older_version(const char *incoming, const char *installed)
{
    for (unsigned i = 0; i < 3; ++i) {
        char *next_a, *next_b;
        unsigned long a = strtoul(incoming, &next_a, 10), b = strtoul(installed, &next_b, 10);
        if (a != b)
            return a < b;
        if (i < 2) {
            incoming = next_a + 1;
            installed = next_b + 1;
        }
    }
    return false;
}

static esp_err_t review_baseline(const char *dir, const pack_candidate_t *c, pack_review_t *r, int *old_slot)
{
    *r = (pack_review_t){.archive_bytes = c->len, .granted = PACK_PERM_UI | c->scene->required_permissions};
    memcpy(r->candidate_digest, c->digest, sizeof(r->candidate_digest));
    *old_slot = 1;
    int paused = marker_state(dir, "PAUSE.IDX");
    if (paused < 0)
        return ESP_FAIL;
    if (paused) {
        int removed = marker_state(dir, "OFF.IDX");
        if (removed < 0)
            return ESP_FAIL;
        r->manually_disabled = !removed;
    }
    pack_candidate_t old = {0};
    esp_err_t e = recover(dir, &old, old_slot, &r->generation, c->digest, &r->disabled);
    if (e == ESP_ERR_NOT_FOUND)
        return ESP_OK;
    if (e != ESP_OK)
        return e;
    memcpy(r->previous_digest, old.digest, sizeof(r->previous_digest));
    memcpy(r->previous_version, old.scene->version, sizeof(r->previous_version));
    r->previous_permissions = old.scene->permissions;
    r->previous_required = old.scene->required_permissions;
    r->same_archive = !memcmp(c->digest, old.digest, sizeof(c->digest));
    r->downgrade = older_version(c->scene->version, old.scene->version);
    pack_dispose(&old);
    if (r->same_archive) {
        char path[PACK_PATH_CAP];
        slot_path(path, dir, *old_slot, "GRT");
        unsigned selected;
        if (read_optional(path, c->digest, &selected) != ESP_OK) return ESP_FAIL;
        if (!(selected & ~c->scene->permissions)) r->granted |= selected;
    }
    return ESP_OK;
}

esp_err_t pack_card_review(const pack_candidate_t *c, pack_review_t *review)
{
    char dir[PACK_PATH_CAP];
    int old_slot;
    pack_card_volume_t volume;
    if (!c || !c->scene || !review)
        return ESP_ERR_INVALID_ARG;
    if (!pack_card_read_volume(&volume, NULL) || !directory(c->scene->id, dir, false))
        return ESP_FAIL;
    esp_err_t e = review_baseline(dir, c, review, &old_slot);
    if (e != ESP_OK) return e;
    if (!pack_card_same_volume(&volume)) return ESP_FAIL;
    review->volume = volume;
    return ESP_OK;
}

bool pack_review_needs_downgrade_grant(const pack_review_t *r, const pack_scene_t *s)
{
    return r->downgrade && (r->previous_permissions != s->permissions || r->previous_required != s->required_permissions);
}

esp_err_t pack_card_set_enabled(const pack_candidate_t *c, const pack_review_t *review, bool enabled)
{
    char dir[PACK_PATH_CAP];
    if (!c || !c->scene || !review)
        return ESP_ERR_INVALID_ARG;
    if (!pack_card_same_volume(&review->volume) || !directory(c->scene->id, dir, false))
        return ESP_FAIL;
    pack_review_t now;
    int old_slot;
    esp_err_t e = review_baseline(dir, c, &now, &old_slot);
    if (e != ESP_OK)
        return e;
    // A local installer can lift an orphaned pause if neither healthy slot survived.
    bool orphan = enabled && !now.generation && now.manually_disabled;
    if ((!now.same_archive && !orphan) || now.generation != review->generation ||
        memcmp(now.previous_digest, review->previous_digest, 32) || memcmp(c->digest, review->candidate_digest, 32))
        return ESP_ERR_INVALID_STATE;
    if (!pack_card_same_volume(&review->volume)) return ESP_FAIL;
    if (now.manually_disabled == !enabled)
        return ESP_OK;
    if (enabled)
        return remove_pause(dir);
    char path[PACK_PATH_CAP + sizeof("/PAUSE.IDX")];
    snprintf(path, sizeof(path), "%s/PAUSE.IDX", dir);
    // Presence alone disables the profile, even if interrupted before writing the byte.
    return synced_write(path, "1", 1) ? ESP_OK : ESP_FAIL;
}

esp_err_t pack_card_stage(const pack_candidate_t *c, const pack_review_t *review, bool allow_changed_downgrade,
                          pack_trial_t *trial)
{
    if (!c || !c->scene || !review || !trial)
        return ESP_ERR_INVALID_ARG;
    *trial = (pack_trial_t){0};
    char dir[PACK_PATH_CAP], path[PACK_PATH_CAP];
    if (!pack_card_same_volume(&review->volume) || !directory(c->scene->id, dir, false))
        return ESP_FAIL;
    int old_slot;
    pack_review_t now;
    esp_err_t e = review_baseline(dir, c, &now, &old_slot);
    if (e != ESP_OK)
        return e;
    if (!grants_valid(c->scene, review->granted) || now.disabled || now.manually_disabled ||
        review->manually_disabled || now.generation != review->generation ||
        memcmp(now.previous_digest, review->previous_digest, 32) ||
        memcmp(c->digest, review->candidate_digest, 32) ||
        (pack_review_needs_downgrade_grant(&now, c->scene) && !allow_changed_downgrade))
        return ESP_ERR_INVALID_STATE;
    if (!pack_card_same_volume(&review->volume)) return ESP_FAIL;
    if (now.same_archive) {
        *trial = (pack_trial_t){now.generation, (unsigned)old_slot, true, review->granted, review->volume};
        return ESP_OK;
    }
    if (now.generation == UINT64_MAX)
        return ESP_ERR_INVALID_STATE;
    if (!directory(c->scene->id, dir, true))
        return ESP_FAIL;
    int removed = marker_state(dir, "OFF.IDX");
    if (removed < 0) return ESP_FAIL;
    if (removed) {
        char marker[PACK_PATH_CAP + sizeof("/OFF.IDX")];
        snprintf(marker, sizeof(marker), "%s/OFF.IDX", dir);
        if (clear_installation(dir, &review->volume) != ESP_OK || unlink(marker))
            return ESP_FAIL;
    }
    pack_trial_t candidate = {now.generation + 1, (unsigned)(old_slot ^ 1), false, review->granted, review->volume};
    // Invalidate any older index in the inactive slot before changing its payload.
    if (!pack_card_same_volume(&candidate.volume) || !write_index(dir, &candidate, c->digest, true))
        return ESP_FAIL;
    slot_path(path, dir, candidate.slot, "KVP");
    if (!pack_card_same_volume(&candidate.volume) || !synced_write(path, c->bytes, c->len) ||
        !matches_payload(path, c) || !pack_card_same_volume(&candidate.volume))
        return ESP_FAIL;
    *trial = candidate;
    return ESP_OK;
}

esp_err_t pack_card_confirm(const pack_candidate_t *c, const pack_trial_t *trial)
{
    char dir[PACK_PATH_CAP], path[PACK_PATH_CAP];
    if (!c || !c->scene || !trial || !grants_valid(c->scene, trial->granted) || !trial->generation ||
        trial->slot > 1 || !pack_card_same_volume(&trial->volume) || !directory(c->scene->id, dir, false))
        return ESP_ERR_INVALID_STATE;
    record_t r = index_read(dir, trial->slot, !trial->already_installed);
    if (r.generation != trial->generation || memcmp(r.digest, c->digest, 32))
        return ESP_ERR_INVALID_STATE;
    slot_path(path, dir, trial->slot, "KVP");
    if (!matches_payload(path, c))
        return ESP_FAIL;
    int removed = marker_state(dir, "OFF.IDX"), paused = marker_state(dir, "PAUSE.IDX");
    if (removed < 0 || paused < 0)
        return ESP_FAIL;
    if (paused || removed)
        return ESP_ERR_INVALID_STATE;
    health_t h[2];
    if (health_read(dir, 0, &h[0]) != ESP_OK || health_read(dir, 1, &h[1]) != ESP_OK)
        return ESP_FAIL;
    if (!pack_card_same_volume(&trial->volume)) return ESP_FAIL;
    if (quarantined(h, c->digest))
        return ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 2; ++i) {
        if (h[i].count && !memcmp(h[i].pending, c->digest, 32)) {
            h[i].count = 0;
            if (health_write(dir, i, &h[i]) != ESP_OK) return ESP_FAIL;
        }
    }
    if (!save_optional(dir, c, trial)) return ESP_FAIL;
    if (trial->already_installed) return pack_card_same_volume(&trial->volume) ? ESP_OK : ESP_FAIL;
    if (!pack_card_same_volume(&trial->volume) || !write_index(dir, trial, c->digest, false))
        return ESP_FAIL;
    return pack_card_same_volume(&trial->volume) ? ESP_OK : ESP_FAIL;
}

esp_err_t pack_card_activation_failed(const pack_candidate_t *c, const pack_trial_t *trial,
                                      uint32_t boot, uint32_t seconds, bool *disabled)
{
    if (!c || !c->scene || !trial || !trial->generation || trial->slot > 1 || !disabled)
        return ESP_ERR_INVALID_ARG;
    *disabled = false;
    char dir[PACK_PATH_CAP];
    if (!pack_card_same_volume(&trial->volume) || !directory(c->scene->id, dir, false))
        return ESP_FAIL;
    int removed = marker_state(dir, "OFF.IDX");
    if (removed) return removed > 0 ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    record_t r = index_read(dir, trial->slot, !trial->already_installed);
    if (r.generation != trial->generation || memcmp(r.digest, c->digest, 32))
        return ESP_ERR_INVALID_STATE;
    health_t entries[2];
    if (health_read(dir, 0, &entries[0]) != ESP_OK || health_read(dir, 1, &entries[1]) != ESP_OK)
        return ESP_FAIL;
    if (quarantined(entries, c->digest)) {
        *disabled = true;
        return ESP_OK;
    }
    // A different successful update may move a candidate to the other
    // physical slot between two failures of the same bytes.
    unsigned journal = trial->slot, other = journal ^ 1u;
    if (entries[other].count && !memcmp(entries[other].pending, c->digest, 32))
        journal = other;
    else if ((!entries[journal].count || memcmp(entries[journal].pending, c->digest, 32)) &&
             (entries[journal].disabled || entries[journal].count) && !entries[other].disabled && !entries[other].count)
        journal = other;
    health_t h = entries[journal];
    unsigned kept = 0;
    if (boot == h.boot && !memcmp(h.pending, c->digest, 32)) {
        for (unsigned i = 0; i < h.count; ++i)
            if (seconds >= h.times[i] && seconds - h.times[i] <= HEALTH_WINDOW_S)
                h.times[kept++] = h.times[i];
    }
    bool threshold = kept == 2;
    if (!threshold) {
        h.times[kept++] = seconds;
        h.count = kept;
        h.boot = boot;
        memcpy(h.pending, c->digest, 32);
    } else {
        h.count = 0;
        h.disabled = true;
        memcpy(h.blocked, c->digest, 32);
    }
    if (!pack_card_same_volume(&trial->volume)) return ESP_FAIL;
    esp_err_t e = health_write(dir, journal, &h);
    if (e != ESP_OK) return e;
    *disabled = threshold;
    // Recovery already consults the durable quarantine. Retire a previously
    // published index as well so v1 readers select the older healthy archive.
    if (*disabled && trial->already_installed &&
        (!pack_card_same_volume(&trial->volume) || !write_index(dir, trial, c->digest, true)))
        return ESP_FAIL;
    return ESP_OK;
}

esp_err_t pack_card_remove(const pack_candidate_t *c, const pack_review_t *review)
{
    if (!c || !c->scene || !review)
        return ESP_ERR_INVALID_ARG;
    if (!review->generation || !review->same_archive || memcmp(c->digest, review->candidate_digest, 32))
        return ESP_ERR_INVALID_STATE;
    char dir[PACK_PATH_CAP], path[PACK_PATH_CAP + sizeof("/OFF.IDX")];
    if (!pack_card_same_volume(&review->volume) || !directory(c->scene->id, dir, false))
        return ESP_FAIL;
    pack_review_t now;
    int slot;
    esp_err_t e = review_baseline(dir, c, &now, &slot);
    if (e != ESP_OK) return e;
    if (!now.same_archive || now.generation != review->generation)
        return ESP_ERR_INVALID_STATE;

    snprintf(path, sizeof(path), "%s/OFF.IDX", dir);
    if (!pack_card_same_volume(&review->volume) || !synced_write(path, "OFF", 3))
        return ESP_FAIL;
    return clear_installation(dir, &review->volume);
}

typedef struct {
    pack_item_t *items;
    size_t skip;
    unsigned used, visited;
    bool more;
} listing_t;

static void append(listing_t *l, const char *path, const char *label, bool installed, bool paused)
{
    if (l->skip) {
        --l->skip;
        return;
    }
    if (l->used == PACK_PAGE) {
        l->more = true;
        return;
    }
    pack_item_t *i = &l->items[l->used++];
    snprintf(i->path, sizeof(i->path), "%s", path);
    snprintf(i->label, sizeof(i->label), "%s", label);
    i->installed = installed;
    i->manually_disabled = paused;
    i->movie = false;
    i->prepared_modes = 0;
    i->mp4 = false;
}

static bool segment(const char *s) { return strlen(s) == 8 && strspn(s, "0123456789abcdefABCDEF") == 8; }

// FatFs returns NULL for both end-of-directory and an SD read error.
// A stat on an optional companion can leave ENOENT from the preceding entry.
static struct dirent *read_entry(DIR *dir, esp_err_t *status)
{
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry && errno) *status = ESP_FAIL;
    return entry;
}

static bool stat_entry(const char *path, struct stat *st, esp_err_t *status)
{
    if (!stat(path, st)) return true;
    if (errno != ENOENT) *status = ESP_FAIL;
    return false;
}

static esp_err_t scan(listing_t *l, char *path, unsigned depth)
{
    if (depth == 8) {
        int slot;
        uint64_t gen;
        pack_candidate_t c = {0};
        esp_err_t e = recover(path, &c, &slot, &gen, NULL, NULL);
        if (e == ESP_OK) {
            int paused = marker_state(path, "PAUSE.IDX");
            if (paused < 0)
                e = ESP_FAIL;
            else
                append(l, path, c.scene->title, true, paused != 0);
            pack_dispose(&c);
        }
        return e == ESP_ERR_NOT_FOUND ? ESP_OK : e;
    }
    DIR *dir = opendir(path);
    if (!dir)
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    size_t len = strlen(path);
    struct dirent *entry;
    esp_err_t e = ESP_OK;
    while (!l->more && (entry = read_entry(dir, &e)) != NULL) {
        if (++l->visited > 4096) {
            e = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (!segment(entry->d_name))
            continue;
        path[len] = '/';
        memcpy(path + len + 1, entry->d_name, 9);
        struct stat st;
        if (stat_entry(path, &st, &e) && S_ISDIR(st.st_mode))
            e = scan(l, path, depth + 1);
        path[len] = 0;
        if (e != ESP_OK)
            break;
    }
    if (closedir(dir) && e == ESP_OK) e = ESP_FAIL;
    return e;
}

bool pack_video_companion(char *out, size_t capacity, const char *source, unsigned mode)
{
    static const char *const suffix[] = {".kvc", ".kvr", ".kvb", ".kvf", ".kvs"};
    if (mode >= sizeof(suffix) / sizeof(suffix[0]))
        return false;
    int n = snprintf(out, capacity, "%s%s", source, suffix[mode]);
    return n > 0 && (size_t)n < capacity;
}

esp_err_t pack_card_list(pack_item_t items[PACK_PAGE], unsigned page, unsigned *count, bool *more)
{
    return pack_card_list_kind(items, PACK_LIST_ALL, page, count, more);
}

esp_err_t pack_card_list_kind(pack_item_t items[PACK_PAGE], pack_list_kind_t kind, unsigned page, unsigned *count,
                              bool *more)
{
    if (!items || !count || !more) return ESP_ERR_INVALID_ARG;
    *count = 0;
    *more = false;
    if (kind < PACK_LIST_ALL || kind > PACK_LIST_VIDEO)
        return ESP_ERR_INVALID_ARG;
    listing_t l = {.items = items, .skip = (size_t)page * PACK_PAGE};
    char path[PACK_PATH_CAP] = ROOT;
    esp_err_t e = ESP_OK;
    if (kind == PACK_LIST_INSTALLED || kind == PACK_LIST_ALL)
        e = scan(&l, path, 0);
    if (e != ESP_OK)
        return e;
    if (kind == PACK_LIST_INSTALLED) {
        *count = l.used;
        *more = l.more;
        return ESP_OK;
    }
    DIR *dir = opendir(PACK_MOUNT);
    if (!dir)
        return ESP_FAIL;
    struct dirent *entry;
    while (e == ESP_OK && !l.more && (entry = read_entry(dir, &e)) != NULL) {
        if (++l.visited > 4096) {
            e = ESP_ERR_INVALID_SIZE;
            break;
        }
        const char *name = entry->d_name;
        size_t len = strlen(name);
        const char *ext = strrchr(name, '.');
        if (!ext || ext == name || len > 100)
            continue;
        bool mp4 = !strcasecmp(ext, ".mp4");
        bool movie = mp4 || !strcasecmp(ext, ".264") || !strcasecmp(ext, ".h264");
        if (!movie && strcasecmp(ext, ".kvmpack"))
            continue;
        if ((kind == PACK_LIST_INSTALLERS && movie) || (kind == PACK_LIST_VIDEO && !movie))
            continue;
        snprintf(path, sizeof(path), PACK_MOUNT "/%s", name);
        struct stat st;
        if (stat_entry(path, &st, &e) && S_ISREG(st.st_mode)) {
            unsigned previous = l.used;
            append(&l, path, name, false, false);
            if (l.used > previous) {
                items[previous].movie = movie;
                items[previous].mp4 = mp4;
                if (movie) {
                    char companion[PACK_PATH_CAP + 5];
                    for (unsigned mode = 0; e == ESP_OK && mode < (mp4 ? 5u : 3u); ++mode) {
                        if (pack_video_companion(companion, sizeof(companion), path, mode) &&
                            stat_entry(companion, &st, &e) && S_ISREG(st.st_mode) && st.st_size >= 64)
                            items[previous].prepared_modes |= 1u << mode;
                    }
                }
            }
        }
    }
    if (closedir(dir) && e == ESP_OK) e = ESP_FAIL;
    if (e != ESP_OK) return e;
    *count = l.used;
    *more = l.more;
    return e;
}
