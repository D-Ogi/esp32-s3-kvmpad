// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_panel.h"
#include "kvm_control_icons.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "media_panel.h"
#include "hid_dispatch.h"
#include "kvmbridge.h"
#include "kvm_notice.h"
#include "pack_archive.h"
#include "pack_receive.h"
#include "pack_scratch.h"
#include "pack_store.h"
#include "pack_video.h"
#include "pack_view.h"
#include "pack_chart_view.h"
#include "ui_status_item.h"
#include "ui_toast.h"
#include "ui_theme.h"
#if CONFIG_KVM_USB_MONITOR_EVAL
#include "pack_desktop_view.h"
#include "usb_hid_device.h"
#include "usb_monitor.h"
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#include "board_lcd.h"
#include "usb_monitor_chart.h"
static usb_monitor_trial_t monitor_trial;
static uint32_t monitor_test_start;
static unsigned monitor_chart_matches;
#endif
#endif
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    WATCH, SCAN, INSPECT_MOVIE, SELECT_ITEM, RECEIVE, ACTIVATE, ENABLE_PROFILE, DISABLE_PROFILE, UNINSTALL, PLAY,
#ifdef KVM_MEDIA_PAGED_GUEST
    PLAY_PAGED,
#endif
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    PLAY_REFERENCE,
#endif
} command_type_t;
typedef struct {
    command_type_t type;
    unsigned index;
    uint32_t epoch;
    kvm_h264_reconstruction_t mode;
    pack_list_kind_t filter;
    unsigned granted;
    bool allow_changed_downgrade;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    bool diagnostic, native, timed;
#endif
} command_t;
typedef enum { LIST, READY, PRESENT_TRIAL, ACTIVE, ROLLED_BACK, DISABLED, MOVIE_INFO, MOVIE_DONE, ERROR,
               CARD_MOUNT_ERROR, CARD_UNAVAILABLE, CLOSED } reply_type_t;
typedef enum { CARD_SOURCE_NONE, CARD_SOURCE_NATIVE, CARD_SOURCE_COMPANION, CARD_SOURCE_PAGED } card_source_t;
typedef struct {
    reply_type_t type;
    esp_err_t error;
    pack_scene_t *scene;
    pack_review_t review;
    kvm_play_result_t playback;
    card_source_t source;
    kvm_h264_source_info_t movie_info;
    uint32_t epoch;
} reply_t;
static QueueHandle_t commands, replies;
static _Atomic bool closing;
typedef enum { TRIAL_WAITING, TRIAL_PRESENTED, TRIAL_REFUSED, TRIAL_CANCELLED, TRIAL_FAILED } trial_status_t;
static _Atomic trial_status_t trial_status;
// First completed refresh, published to the card worker by TRIAL_PRESENTED.
static int64_t trial_presented_at;
static bool trial_flush, profile_active, activation_pending, trial_xplane;
static lv_obj_t *trial_shield;
static uint32_t receive_epoch;
static char sending_client[33];
static _Atomic unsigned scans;
static _Atomic unsigned menus, choices;
static pack_list_kind_t filter;
static lv_obj_t *library_button;
static void (*open_tools)(void);

unsigned pack_panel_scans(void) { return atomic_load(&scans); }
unsigned pack_panel_menus(void) { return atomic_load(&menus); }
unsigned pack_panel_choices(void) { return atomic_load(&choices); }
static bool busy;
static void (*return_home)(void);
static lv_timer_t *timer;
static lv_obj_t *menu, *body, *profile;
#if CONFIG_KVM_USB_MONITOR_EVAL
static enum { MONITOR_UNAVAILABLE, MONITOR_CATEGORY, MONITOR_SIZES } monitor_page;
#endif
enum { PROFILE_USB, PROFILE_AGENT, PROFILE_DATA, PROFILE_CAPTURE, PROFILE_INDICATORS };
static struct { lv_obj_t *item; int value; } indicators[PROFILE_INDICATORS];
static pack_scene_t *current;
static pack_review_t current_review;
static lv_obj_t *primary_action, *xplane_action, *grant_summary;
EXT_RAM_BSS_ATTR static pack_item_t entries[PACK_PAGE];
static unsigned count, selected, page;
static lv_obj_t *movie_heading;
static _Atomic uint32_t movie_detail_epoch;
static bool more;
static uint32_t media_epoch;
static bool magnifier_telemetry;
static char playing_filename[sizeof(entries[0].label)];
static kvm_h264_reconstruction_t playing_mode;
static command_type_t playing_type;
static atomic_bool stopping_movie;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
// The LVGL owner drives actual library buttons. The worker only sees an immutable command.
static const char *const card_cases[] = {
    "kvp_320x180.mp4", "kvp_640x360.mp4", "kvp_960x540.mp4",
    "kvp_320x180_5fps.mp4", "kvp_320x180_10fps.mp4", "kvp_320x180_15fps.mp4",
    "kvp_640x360_5fps.mp4", "kvp_640x360_10fps.mp4", "kvp_640x360_15fps.mp4",
    "kvp_960x540_5fps.mp4", "kvp_960x540_10fps.mp4", "kvp_960x540_15fps.mp4",
    "test_1920x1080.h264", "model_motion_320x180_24fps.mp4"
};
static const struct {
    const char *name, *button;
} case_modes[] = {
    [KVM_H264_EXACT] = {"exact", "Play"},
    [KVM_H264_REDUCED] = {"reduced", "Reduced"},
    [KVM_H264_LARGE_BANK] = {"large", "Large bank"},
    [KVM_H264_RR_BLEND] = {"blend", "Temporal blend"},
    [KVM_H264_RR_SMART_LOOSE] = {"smart", "Smart loose"}
};
static _Atomic card_source_t case_route;
static const char *const route_names[] = {
    [CARD_SOURCE_NONE] = "none", [CARD_SOURCE_NATIVE] = "native",
    [CARD_SOURCE_COMPANION] = "companion", [CARD_SOURCE_PAGED] = "paged"
};
static struct {
    const char *file;
    unsigned mode;
    bool native, timed, click, pixel_size;
    enum { CASE_SCAN, CASE_DETAIL, CASE_PLAY, CASE_FINISHED } phase;
    uint32_t detail_tick, epoch;
    int64_t started;
} card_case;
#endif

static void reply(reply_type_t type, esp_err_t error, pack_scene_t *scene)
{
    const reply_t r = {.type = type, .error = error, .scene = scene};
    xQueueSend(replies, &r, portMAX_DELAY);
}

static bool card_worker_stopped(void) { return atomic_load(&closing); }

// Each received archive uses the same card worker and validation path as a local installer.
static esp_err_t receive_archive(uint32_t epoch, pack_candidate_t *candidate)
{
    FILE *file = NULL;
    uint8_t digest[32];
    uint32_t size = 0, received = 0;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    int64_t check_card = 0;
    while (!card_worker_stopped()) {
        int64_t now = esp_timer_get_time();
        if (now >= check_card) {
            if (!(file ? pack_scratch_present() : pack_card_present()))
                break;
            check_card = now + 250000;
        }
        pack_receive_part_t part;
        if (!pack_receive_take(epoch, &part)) {
            vTaskDelay(1);
            continue;
        }
        bool ok = false;
        if (part.kind == PACK_RECEIVE_ABORT)
            break;
        if (part.kind == PACK_RECEIVE_BEGIN && !file && part.size && part.size <= PACK_ARCHIVE_MAX) {
            size = part.size;
            memcpy(digest, part.sha256, sizeof(digest));
            snprintf(sending_client, sizeof(sending_client), "%.32s", part.client);
            file = pack_scratch_open_for(size);
            ok = file != NULL;
        } else if (part.kind == PACK_RECEIVE_DATA && file && received == part.offset &&
                   part.len && part.len <= sizeof(part.data) && part.len <= size - received) {
            ok = fwrite(part.data, 1, part.len, file) == part.len;
            if (ok)
                received += part.len;
        } else if (part.kind == PACK_RECEIVE_END && file && received == size) {
            result = ESP_FAIL;
            if (pack_scratch_present() && !fflush(file) && !fsync(fileno(file)))
                result = pack_load_file(file, candidate);
            if (result == ESP_OK && (candidate->len != size || memcmp(digest, candidate->digest, sizeof(digest))))
                result = ESP_ERR_INVALID_RESPONSE;
            if (!pack_scratch_close() && result == ESP_OK)
                result = ESP_FAIL;
            file = NULL;
            ok = result == ESP_OK && !card_worker_stopped();
        }
        pack_receive_ack(epoch, ok);
        if (!ok || part.kind == PACK_RECEIVE_END)
            break;
    }
    if (file)
        pack_scratch_close();
    if (result == ESP_OK && !pack_receive_review_active(epoch))
        result = ESP_ERR_INVALID_STATE;
    if (result != ESP_OK) {
        pack_dispose(candidate);
        pack_receive_cancel(epoch);
    }
    return result;
}

static esp_err_t review_candidate(const pack_candidate_t *candidate, pack_review_t *baseline)
{
    esp_err_t e = pack_card_review(candidate, baseline);
    if (e == ESP_OK) {
        const reply_t r = {.type = READY, .scene = candidate->scene, .review = *baseline};
        xQueueSend(replies, &r, portMAX_DELAY);
    }
    return e;
}

static esp_err_t activate_trial(const pack_candidate_t *candidate, const pack_review_t *baseline, bool allow_downgrade,
                                bool *disabled)
{
    pack_trial_t trial;
    esp_err_t e = pack_card_stage(candidate, baseline, allow_downgrade, &trial);
    if (e != ESP_OK || card_worker_stopped())
        return e == ESP_OK ? ESP_ERR_INVALID_STATE : e;
    const int64_t start = esp_timer_get_time(), deadline = start + 5000000;
    int64_t next_presence = start + 250000;
    reply(PRESENT_TRIAL, ESP_OK, candidate->scene);
    while (!card_worker_stopped()) {
        trial_status_t status = atomic_load(&trial_status);
        if (status == TRIAL_CANCELLED) return ESP_ERR_INVALID_STATE;
        if (status == TRIAL_PRESENTED && trial_presented_at < deadline)
            return pack_card_confirm(candidate, &trial);
        int64_t now = esp_timer_get_time();
        if (now >= next_presence) {
            if (!pack_card_present()) return ESP_ERR_NOT_FOUND;
            next_presence = now + 250000;
        }
        if (status == TRIAL_REFUSED || now >= deadline) {
            if (!atomic_compare_exchange_strong(&trial_status, &status, TRIAL_FAILED))
                continue;
            if (!pack_card_present()) return ESP_ERR_NOT_FOUND;
            // The card can move between devices with the same NVS boot count.
            static uint32_t boot_tag;
            if (!boot_tag) {
                boot_tag = esp_random() ^ kvm_boot_count();
                if (!boot_tag) boot_tag = 1;
            }
            esp_err_t saved = pack_card_activation_failed(candidate, &trial, boot_tag,
                                                         (uint32_t)(now / 1000000), disabled);
            if (saved != ESP_OK) return saved;
            return status == TRIAL_REFUSED ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_ERR_INVALID_STATE;
}

#ifdef KVM_MEDIA_PAGED_GUEST
static kvm_play_result_t play_paged(const command_t *cmd, const pack_item_t *item, card_source_t *route)
{
    if (item->mp4 || (cmd->mode != KVM_H264_REDUCED && cmd->mode != KVM_H264_LARGE_BANK))
        return KVM_PLAY_UNAVAILABLE;
    FILE *source = fopen(item->path, "rb");
    if (!source)
        return KVM_PLAY_UNAVAILABLE;
    unsigned zeros = 0;
    int byte = fgetc(source);
    while (byte == 0 && zeros < 4096) {
        ++zeros;
        byte = fgetc(source);
    }
    int nal = byte == 1 ? fgetc(source) : EOF;
    kvm_play_result_t result = KVM_PLAY_UNAVAILABLE;
    if (zeros >= 2 && nal > 0 && nal < 128 && (nal & 31) && (nal & 31) <= 23 && !fseek(source, 0, SEEK_SET)) {
        FILE *scratch = pack_scratch_open();
        if (scratch) {
            *route = CARD_SOURCE_PAGED;
            result = media_panel_play_paged(cmd->epoch, source, scratch, cmd->mode, pack_scratch_present);
            if (!pack_scratch_close() && result == KVM_PLAY_OK)
                result = KVM_PLAY_UNAVAILABLE;
        }
    }
    fclose(source);
    return result;
}
#endif

// The card worker checks this while hashing the source, before the player
// starts its own presence polling. Avoid a sector read for every 8 KiB hashed.
static int64_t next_hash_card_check;
static bool stop_source_hash(void)
{
    if (card_worker_stopped() || atomic_load(&stopping_movie)) return true;
    int64_t now = esp_timer_get_time();
    if (now < next_hash_card_check) return false;
    next_hash_card_check = now + 250000;
    return !pack_card_present();
}

// Only the prepared reader uses this 16 KiB window. Release its stack frame
// before entering the native AVC decoder if a companion cannot be opened.
static __attribute__((noinline)) bool play_companion(const command_t *cmd, FILE *source, FILE *companion,
                                                    kvm_play_result_t *result)
{
    pack_video_window_t window;
    pack_video_info_t info;
    pack_video_t *video = NULL;
    next_hash_card_check = 0;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t open_start = esp_timer_get_time();
#endif
    if (pack_video_open_with_window(source, companion, cmd->mode, stop_source_hash, &window, &video, &info) != ESP_OK)
        return false;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    int64_t open_us = esp_timer_get_time() - open_start;
#endif
    *result = media_panel_play_rgb(cmd->epoch, info.width, info.height, info.frames, pack_video_read_timed, video,
                                   pack_card_present);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (cmd->timed) {
        pack_video_io_t io;
        pack_video_get_io(video, &io);
        printf("CARD_READER_IO epoch=%" PRIu32 " open_us=%" PRId64 " bind_us=%" PRId64 " bind_bytes=%" PRIu64
               " prefill_us=%" PRId64 " prefill_bytes=%u refill_us=%" PRId64 " refill_bytes=%" PRIu64
               " refill_calls=%u\n", cmd->epoch, open_us, io.bind_us, io.bind_bytes, io.prefill_us, io.prefill_bytes,
               io.refill_us, io.refill_bytes, io.refill_calls);
    }
#endif
    pack_video_close(video);
    return true;
}

static void play_card_file(const command_t *cmd, const char *path, bool mp4, reply_t *out)
{
    out->playback = KVM_PLAY_UNAVAILABLE;
    FILE *source = fopen(path, "rb");
    if (!source)
        return;
    char companion_path[PACK_PATH_CAP + 5];
    bool use_companion = true;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    use_companion = !cmd->native;
#endif
    FILE *companion = use_companion && pack_video_companion(companion_path, sizeof(companion_path), path, cmd->mode) ?
                      fopen(companion_path, "rb") : NULL;
    bool played_companion = companion && play_companion(cmd, source, companion, &out->playback);
    if (companion)
        fclose(companion);
    if (played_companion)
        out->source = CARD_SOURCE_COMPANION;
    else if (!card_worker_stopped() && !atomic_load(&stopping_movie) && pack_card_present()) {
        // A failed companion hash can leave the original's stream error set across seeks.
        clearerr(source);
        if (fseek(source, 0, SEEK_SET) == 0)
            out->source = CARD_SOURCE_NATIVE;
    }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (cmd->diagnostic) {
        atomic_store(&case_route, out->source);
        printf("CARD_HIL_ROUTE epoch=%" PRIu32 " file=%s mode=%s source=%s\n",
               cmd->epoch, strrchr(path, '/') + 1, case_modes[cmd->mode].name, route_names[out->source]);
    }
#endif
    if (out->source == CARD_SOURCE_NATIVE)
        out->playback = media_panel_play_result(cmd->epoch, source, mp4, cmd->mode, pack_card_present);
    fclose(source);
}

static bool detail_cancelled(void *ctx)
{
    return card_worker_stopped() || atomic_load(&movie_detail_epoch) != *(const uint32_t *)ctx;
}

static void worker(void *arg)
{
    (void)arg;
    pack_candidate_t candidate = {0};
    pack_review_t baseline = {0};
    bool mounted = false;
    unsigned item = 0;
    uint32_t received_epoch = 0;
    while (!atomic_load(&closing)) {
        command_t cmd;
        bool have_command = xQueueReceive(commands, &cmd, pdMS_TO_TICKS(250));
        if (mounted && !pack_card_present()) {
            // A queued tap can arrive before the next idle poll. Retire the
            // old list and wait for Home to detach the scene before disposal.
            reply(CARD_UNAVAILABLE, ESP_ERR_NOT_FOUND, NULL);
            while (!atomic_load(&closing))
                vTaskDelay(pdMS_TO_TICKS(20));
            break;
        }
        if (!have_command) continue;
        esp_err_t e = ESP_OK;
        switch (cmd.type) {
        case WATCH:
            mounted = pack_card_mount() == ESP_OK;
            break;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
        case PLAY_REFERENCE: {
            if (!mounted || !pack_card_present()) {
                e = ESP_ERR_NOT_FOUND;
                break;
            }
            reply_t r = {.type = MOVIE_DONE, .epoch = cmd.epoch};
            play_card_file(&cmd, PACK_MOUNT "/test_1920x1080.h264", false, &r);
            if (!pack_card_present()) {
                e = ESP_ERR_NOT_FOUND;
                break;
            }
            xQueueSend(replies, &r, portMAX_DELAY);
            break;
        }
#endif
        case SCAN:
            pack_dispose(&candidate);
            if (!mounted) {
                e = pack_card_mount();
                mounted = e == ESP_OK;
            }
            if (e == ESP_OK)
                e = pack_card_list_kind(entries, cmd.filter, cmd.index, &count, &more);
            if (e == ESP_OK)
                reply(LIST, e, NULL);
            break;
        case SELECT_ITEM:
            pack_dispose(&candidate);
            item = cmd.index;
            e = item < count ? pack_card_open(&entries[item], &candidate) : ESP_ERR_INVALID_ARG;
            if (e == ESP_OK)
                e = review_candidate(&candidate, &baseline);
            break;
        case INSPECT_MOVIE: {
            if (cmd.index >= count || !entries[cmd.index].movie || detail_cancelled(&cmd.epoch))
                break;
            FILE *file = fopen(entries[cmd.index].path, "rb");
            if (file) {
                reply_t r = {.type = MOVIE_INFO, .epoch = cmd.epoch};
                kvm_play_result_t inspected = kvm_media_h264_inspect(file, entries[cmd.index].mp4, &r.movie_info,
                                                                     detail_cancelled, &cmd.epoch);
                fclose(file);
                if (inspected == KVM_PLAY_OK && !detail_cancelled(&cmd.epoch))
                    xQueueSend(replies, &r, portMAX_DELAY);
            }
            break;
        }
        case RECEIVE:
            pack_dispose(&candidate);
            if (!mounted) {
                e = pack_card_mount();
                mounted = e == ESP_OK;
            }
            if (e == ESP_OK)
                e = receive_archive(cmd.epoch, &candidate);
            if (e == ESP_OK)
                e = review_candidate(&candidate, &baseline);
            if (e == ESP_OK) {
                received_epoch = cmd.epoch;
            } else
                pack_receive_cancel(cmd.epoch);
            break;
        case ACTIVATE: {
            bool disabled = false;
            if (!candidate.scene || !pack_card_present())
                e = ESP_ERR_NOT_FOUND;
            else {
                baseline.granted = cmd.granted;
                e = activate_trial(&candidate, &baseline, cmd.allow_changed_downgrade, &disabled);
            }
            if (received_epoch) {
                pack_receive_finish(received_epoch, e == ESP_OK);
                received_epoch = 0;
            }
            if (e == ESP_OK)
                reply(ACTIVE, e, candidate.scene);
            else if (!card_worker_stopped()) {
                reply_t failure = {.type = disabled ? DISABLED : ROLLED_BACK, .error = e};
                if (baseline.generation && (e == ESP_ERR_TIMEOUT || e == ESP_ERR_NOT_SUPPORTED)) {
                    if (pack_card_review(&candidate, &failure.review) != ESP_OK ||
                        !pack_card_same_volume(&baseline.volume) || failure.review.same_archive)
                        failure.review.previous_version[0] = '\0';
                }
                xQueueSend(replies, &failure, portMAX_DELAY);
                while (!card_worker_stopped())
                    vTaskDelay(1);
            }
            e = ESP_OK;
            break;
        }
        case ENABLE_PROFILE:
        case DISABLE_PROFILE: {
            bool orphan = cmd.type == ENABLE_PROFILE && !baseline.generation && baseline.manually_disabled;
            if (!candidate.scene || received_epoch || item >= count || (!entries[item].installed && !orphan)) {
                e = ESP_ERR_INVALID_STATE;
                break;
            }
            e = pack_card_set_enabled(&candidate, &baseline, cmd.type == ENABLE_PROFILE);
            if (e == ESP_OK) {
                baseline.manually_disabled = cmd.type == DISABLE_PROFILE;
                entries[item].manually_disabled = entries[item].installed && baseline.manually_disabled;
                const reply_t r = {.type = READY, .scene = candidate.scene, .review = baseline};
                xQueueSend(replies, &r, portMAX_DELAY);
            }
            break;
        }
        case UNINSTALL:
            e = !received_epoch && item < count && entries[item].installed ? pack_card_remove(&candidate, &baseline)
                                                                          : ESP_ERR_INVALID_STATE;
            if (e == ESP_OK) {
                pack_dispose(&candidate);
                e = pack_card_list_kind(entries, cmd.filter, 0, &count, &more);
                if (e == ESP_OK)
                    reply(LIST, e, NULL);
            }
            break;
#ifdef KVM_MEDIA_PAGED_GUEST
        case PLAY_PAGED:
#endif
        case PLAY: {
            if (cmd.index >= count || !entries[cmd.index].movie) {
                e = ESP_ERR_INVALID_ARG;
                break;
            }
            reply_t r = {.type = MOVIE_DONE, .epoch = cmd.epoch};
#ifdef KVM_MEDIA_PAGED_GUEST
            if (cmd.type == PLAY_PAGED)
                r.playback = play_paged(&cmd, &entries[cmd.index], &r.source);
            else
#endif
                play_card_file(&cmd, entries[cmd.index].path, entries[cmd.index].mp4, &r);
            if (!pack_card_present())
                e = ESP_ERR_NOT_FOUND;
            else
                xQueueSend(replies, &r, portMAX_DELAY);
            break;
        }
        }
        if (e != ESP_OK) {
            bool read_failed = e == ESP_FAIL && (cmd.type == SCAN || cmd.type == SELECT_ITEM);
            if (mounted && (read_failed || !pack_card_present())) {
                reply(CARD_UNAVAILABLE, read_failed ? ESP_FAIL : ESP_ERR_NOT_FOUND, NULL);
                while (!atomic_load(&closing))
                    vTaskDelay(pdMS_TO_TICKS(20));
            } else if (!mounted && (cmd.type == SCAN || cmd.type == RECEIVE))
                reply(CARD_MOUNT_ERROR, e, NULL);
            else
                reply(ERROR, e, NULL);
        }
    }
    if (received_epoch)
        pack_receive_finish(received_epoch, false);
    pack_dispose(&candidate);
    pack_card_close();
    reply(CLOSED, ESP_OK, NULL);
    vTaskDeleteWithCaps(NULL);
}

static lv_obj_t *button(lv_obj_t *parent, const char *name, int x, int y, int w, int h, lv_event_cb_t click, void *ctx)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    ui_theme_control_button(b);
    lv_obj_t *label = lv_label_create(b);
    lv_label_set_text(label, name);
    lv_obj_center(label);
    lv_obj_add_event_cb(b, click, LV_EVENT_CLICKED, ctx);
    return b;
}

static lv_obj_t *text_row(lv_obj_t *p, const char *msg, int y)
{
    lv_obj_t *label = lv_label_create(p);
    lv_obj_set_width(label, 920);
    lv_label_set_text(label, msg);
    lv_obj_set_pos(label, 24, y);
    return label;
}

static void home_clicked(lv_event_t *e)
{
    (void)e;
    pack_panel_leave();
}

static void clear_movie_detail(void)
{
    if (movie_heading) {
        movie_heading = NULL;
        atomic_fetch_add(&movie_detail_epoch, 1);
    }
}

static void reset_body(const char *message)
{
#if CONFIG_KVM_USB_MONITOR_EVAL
    monitor_page = MONITOR_UNAVAILABLE;
#endif
    clear_movie_detail();
    lv_obj_clean(body);
    if (message)
        text_row(body, message, 12);
}

static bool send_request(command_t cmd)
{
    reset_body("Reading card...");
    if (!xQueueSend(commands, &cmd, 0)) {
        reset_body("Card operation pending.");
        return false;
    } else if (library_button)
        lv_obj_add_state(library_button, LV_STATE_DISABLED);
    return true;
}

static bool request(command_type_t type, unsigned index)
{
    return send_request((command_t){.type = type, .index = index, .filter = filter, .epoch = receive_epoch});
}

static void choose_category(lv_event_t *event)
{
    filter = (pack_list_kind_t)(uintptr_t)lv_event_get_user_data(event);
    page = 0;
    current = NULL;
    atomic_fetch_add(&choices, 1);
    request(SCAN, 0);
}

static void device_tools(lv_event_t *event)
{
    (void)event;
    void (*next)(void) = open_tools;
    pack_panel_leave();
    if (next)
        next();
}

static void decline_receive(lv_event_t *event)
{
    (void)event;
    if (pack_receive_review_active(receive_epoch))
        pack_receive_finish(receive_epoch, false);
    pack_panel_leave();
}

static void receive_clicked(lv_event_t *event)
{
    (void)event;
    receive_epoch = pack_receive_arm();
    if (!receive_epoch) {
        reset_body("The receiver is busy.");
        return;
    }
    if (!request(RECEIVE, 0)) {
        pack_receive_cancel(receive_epoch);
        receive_epoch = 0;
        return;
    }
    reset_body("Waiting for a package from your paired client.");
    button(body, "Cancel", 24, 90, 160, 56, decline_receive, NULL);
}

#if CONFIG_KVM_USB_MONITOR_EVAL
static bool native_monitor, monitor_controls, windows_monitor;

static bool select_monitor_usb(bool active)
{
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
#ifdef KVM_USB_PICTURE_CHECK
    if (active && (monitor_trial == USB_MONITOR_TRIAL_CHART || monitor_trial == USB_MONITOR_TRIAL_DESKTOP))
        usb_monitor_enable_picture_check();
#endif
    if (monitor_trial == USB_MONITOR_TRIAL_CHART) return true;
#endif
    if (active && windows_monitor)
        return usb_monitor_enable_windows_modes() && usb_hid_device_set_monitor_windows(monitor_controls) == ESP_OK;
    if (monitor_controls) return usb_hid_device_set_monitor_controls(active, native_monitor) == ESP_OK;
    return usb_hid_device_set_monitor_mode(active, native_monitor) == ESP_OK;
}

static bool open_monitor(bool native, bool controls, bool windows_modes)
{
    if (!menu || lv_screen_active() != menu || monitor_page == MONITOR_UNAVAILABLE) return false;
    native_monitor = native;
    monitor_controls = controls;
    windows_monitor = windows_modes;
    return usb_monitor_view_open_mode(pack_panel_leave, select_monitor_usb, native);
}

bool pack_panel_open_monitor(bool native, bool controls)
{
    return open_monitor(native, controls, false);
}

bool pack_panel_open_windows_monitor(bool controls)
{
    return open_monitor(true, controls, true);
}

static bool set_standalone_profile(bool native)
{
    return (monitor_controls ? usb_hid_device_set_monitor_controls(true, native) :
                               usb_hid_device_set_monitor_mode(true, native)) == ESP_OK;
}

#ifdef KVM_USB_MOTION_CHECK
bool pack_panel_open_motion_fixture(bool native, bool (*fixture_link)(bool))
{
    return menu && lv_screen_active() == menu && usb_monitor_view_open_mode(pack_panel_leave, fixture_link, native);
}
#endif

static void choose_monitor_size(lv_event_t *event)
{
    const uintptr_t choice = (uintptr_t)lv_event_get_user_data(event);
    const bool native = choice != 0;
    if (!open_monitor(native, false, choice == 2))
        ui_toast_show(menu, native ? "Native display could not start. Try 640 x 480." :
                                     "USB monitor could not start.", 4000);
}

static void open_usb_monitor(lv_event_t *event)
{
    (void)event;
    reset_body(NULL);
    monitor_page = MONITOR_SIZES;
    if (library_button) lv_obj_set_hidden(library_button, false);
    lv_obj_t *heading = text_row(body, "USB monitor", UI_SPACE_XL);
    lv_obj_set_style_text_font(heading, ui_theme_font(UI_FONT_TITLE), 0);
    text_row(body, "Set the resolution in Windows Display settings, or choose a fixed size.", 62);
    button(body, "Choose in Windows", 24, 112, 916, 82, choose_monitor_size, (void *)(uintptr_t)2);
    button(body, "640 x 480", 24, 210, 448, 88, choose_monitor_size, NULL);
    button(body, "1024 x 600  /  Native", 492, 210, 448, 88, choose_monitor_size, (void *)(uintptr_t)1);
}
#endif

bool pack_panel_remote_monitor_mode(bool native)
{
#if CONFIG_KVM_USB_MONITOR_EVAL
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (monitor_trial != USB_MONITOR_TRIAL_IDLE) return false;
#endif
    if (!usb_monitor_view_has_link(select_monitor_usb)) return pack_desktop_change_mode(native);
    if (windows_monitor) return false;
    if (native == native_monitor) return true;
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    if (usb_monitor_view_change_mode(native, set_standalone_profile)) {
        native_monitor = native;
        return true;
    }
    if (!usb_monitor_view_active() && menu)
        ui_toast_show(menu, "USB monitor unavailable. Choose a size to retry.", 4000);
#else
    (void)native;
#endif
    return false;
}

static void menu_row(lv_obj_t *row, const lv_image_dsc_t *mask, ui_font_role_t font)
{
    lv_obj_set_style_pad_hor(row, 16, LV_PART_MAIN);
    lv_obj_t *label = lv_obj_get_child(row, 0);
    lv_obj_set_style_text_font(label, ui_theme_font(font), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 42, 0);
    lv_obj_align(ui_theme_control_mask(row, mask), LV_ALIGN_LEFT_MID, 0, 0);
}

static void category_button(const char *name, const lv_image_dsc_t *mask, unsigned slot, lv_event_cb_t click, uintptr_t arg)
{
    const int width = lv_obj_get_width(body), height = lv_obj_get_height(body);
    const int inset = UI_SPACE_XL, gap = UI_SPACE_M;
    const int top = 52, footer = UI_SPACE_XL;
    const int tile_w = (width - 2 * inset - 2 * gap) / 3;
    const int tile_h = (height - top - footer - gap) / 2;
    int x = inset + (slot % 3) * (tile_w + gap), y = top + (slot / 3) * (tile_h + gap);
    lv_obj_t *b = button(body, name, x, y, tile_w, tile_h, click, (void *)arg);
    ui_theme_apply(b, UI_STYLE_LIBRARY_TILE, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, LV_STATE_PRESSED);
    lv_obj_t *label = lv_obj_get_child(b, 0);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, ui_theme_font(UI_FONT_HEADING), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *icon = ui_theme_control_mask(b, mask);
    ui_theme_apply(icon, UI_STYLE_LIBRARY_ICON, LV_STATE_DEFAULT);
    ui_theme_apply(icon, UI_STYLE_LIBRARY_ICON, LV_STATE_CHECKED);
    lv_obj_set_size(icon, UI_TOUCH_MIN, UI_TOUCH_MIN);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);
}

static void categories(lv_event_t *event)
{
    (void)event;
    current = NULL;
    if (library_button)
        lv_obj_set_hidden(library_button, true);
    reset_body(NULL);
    lv_obj_update_layout(menu);
    unsigned slot = 0;
    category_button("Installed extensions", &kvm_icon_extensions, slot++, choose_category, PACK_LIST_INSTALLED);
    category_button("Install from card", &kvm_icon_card, slot++, choose_category, PACK_LIST_INSTALLERS);
    category_button("Video files", &kvm_icon_video, slot++, choose_category, PACK_LIST_VIDEO);
    if (open_tools)
        category_button("Device tools", &kvm_icon_settings, slot++, device_tools, 0);
    category_button("Receive from paired client", &kvm_icon_client, slot++, receive_clicked, 0);
#if CONFIG_KVM_USB_MONITOR_EVAL
    category_button("USB monitor", &kvm_icon_host, slot, open_usb_monitor, 0);
    monitor_page = MONITOR_CATEGORY;
#endif
    lv_obj_t *title = lv_label_create(body);
    lv_label_set_text_static(title, "Extensions");
    lv_obj_set_style_text_font(title, ui_theme_font(UI_FONT_TITLE), LV_PART_MAIN);
    lv_obj_set_pos(title, UI_SPACE_XL, UI_SPACE_M);
    atomic_fetch_add(&menus, 1);
}

static void browse(lv_event_t *e)
{
    current = NULL;
    page = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    request(SCAN, page);
}

static void stop_movie(void)
{
    atomic_store(&stopping_movie, true);
    reset_body("Stopping video...");
    lv_obj_add_state(library_button, LV_STATE_DISABLED);
}

static bool start_movie(command_type_t type, unsigned index, kvm_h264_reconstruction_t mode)
{
    const char *filename;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (type == PLAY_REFERENCE)
        filename = "test_1920x1080.h264";
    else
#endif
    {
        if (index >= count || !entries[index].movie)
            return false;
        filename = entries[index].label;
    }
    clear_movie_detail();
    uint32_t opened = media_panel_open(false, pack_panel_leave);
    if (!opened)
        return false;
    media_epoch = opened;
    media_panel_file_title(opened, filename);
    snprintf(playing_filename, sizeof(playing_filename), "%s", filename);
    playing_type = type;
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (type != PLAY_REFERENCE)
#endif
        media_panel_file_stop_action(opened, stop_movie);
    atomic_store(&stopping_movie, false);
    playing_mode = mode;
    command_t cmd = {.type = type, .index = index, .epoch = media_epoch, .mode = mode};
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (card_case.click) {
        if (card_case.pixel_size)
            media_panel_test_pixel_size(opened);
        cmd.diagnostic = true;
        cmd.native = card_case.native;
        cmd.timed = card_case.timed;
        card_case.epoch = cmd.epoch;
        if (card_case.timed)
            media_panel_time_without_scanout_crc(cmd.epoch);
    }
#endif
    if (xQueueSend(commands, &cmd, 0))
        return true;
    media_panel_close();
    media_epoch = 0;
    return false;
}

static void play_movie(lv_event_t *e)
{
    kvm_h264_reconstruction_t mode = (kvm_h264_reconstruction_t)(uintptr_t)lv_event_get_user_data(e);
    if (!start_movie(PLAY, selected, mode)) {
        reset_body("The video player is busy.");
        button(body, "Back", 24, 85, 180, 56, browse, NULL);
    }
}

#ifdef KVM_MEDIA_PAGED_GUEST
static void play_paged_movie(lv_event_t *e)
{
    kvm_h264_reconstruction_t mode = (kvm_h264_reconstruction_t)(uintptr_t)lv_event_get_user_data(e);
    if (!start_movie(PLAY_PAGED, selected, mode)) {
        reset_body("The video player is busy.");
        button(body, "Back", 24, 85, 180, 56, browse, NULL);
    }
}
#endif

#ifdef KVM_MEDIA_SYNTHETIC_DIAG
static bool click_case_button(lv_obj_t *parent, const char *caption)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(parent); i++) {
        lv_obj_t *button_obj = lv_obj_get_child(parent, i);
        if (!lv_obj_check_type(button_obj, &lv_button_class) || lv_obj_is_hidden(button_obj) ||
            lv_obj_has_state(button_obj, LV_STATE_DISABLED))
            continue;
        lv_obj_t *label = lv_obj_get_child(button_obj, 0);
        if (label && lv_obj_check_type(label, &lv_label_class) && !strcmp(lv_label_get_text(label), caption)) {
            lv_obj_update_layout(parent);
            lv_area_t a;
            lv_obj_get_coords(button_obj, &a);
            lv_point_t center = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
            if (lv_indev_search_obj(lv_screen_active(), &center) != button_obj)
                return false;
            lv_obj_send_event(button_obj, LV_EVENT_CLICKED, NULL);
            return true;
        }
    }
    return false;
}

static bool case_button(const char *caption)
{
    return body && lv_screen_active() == menu && click_case_button(body, caption);
}

#if CONFIG_KVM_USB_MONITOR_EVAL
bool pack_panel_usb_monitor_test(usb_monitor_trial_t trial)
{
    if (trial < USB_MONITOR_TRIAL_CHART || trial > USB_MONITOR_TRIAL_DESKTOP ||
        !menu || !body || lv_screen_active() != menu || monitor_trial != USB_MONITOR_TRIAL_IDLE ||
        usb_hid_device_monitor_mounted()) return false;
    lv_obj_update_layout(menu);
    for (uint32_t i = 0; i < lv_obj_get_child_count(body); i++) {
        lv_obj_t *button_obj = lv_obj_get_child(body, i);
        if (!lv_obj_check_type(button_obj, &lv_button_class)) continue;
        lv_obj_t *label = lv_obj_get_child(button_obj, 0);
        if (!label || strcmp(lv_label_get_text(label), "USB monitor")) continue;
        lv_area_t a;
        lv_obj_get_coords(button_obj, &a);
        lv_point_t center = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        if (a.x1 < 0 || a.y1 < 0 || a.x2 >= 1024 || a.y2 >= 600 ||
            lv_indev_search_obj(menu, &center) != button_obj) return false;
        monitor_test_start = lv_tick_get();
        if (trial == USB_MONITOR_TRIAL_LINK) {
            // Query the production descriptors with the pixel receiver closed.
            if (!select_monitor_usb(true)) return false;
            monitor_trial = trial;
            reset_body("Checking USB monitor connection");
            if (library_button) lv_obj_add_state(library_button, LV_STATE_DISABLED);
            puts("USB_MONITOR_LINK started=1");
            return true;
        }
        monitor_trial = trial;
        monitor_chart_matches = 0;
        lv_obj_send_event(button_obj, LV_EVENT_CLICKED, NULL);
        if (monitor_page != MONITOR_SIZES || !pack_panel_open_monitor(false, false)) {
            monitor_trial = USB_MONITOR_TRIAL_IDLE;
            return false;
        }
        if (trial == USB_MONITOR_TRIAL_DESKTOP) {
            puts("USB_MONITOR_DESKTOP started=1 seconds=75");
            return true;
        }
        printf("USB_MONITOR_TEST started=1 menu_rect=%d,%d,%d,%d\n", (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
        usb_monitor_test_chart(usb_monitor_feed);
        return true;
    }
    return false;
}
#endif

static void case_finish(const char *result, const media_panel_file_stats_t *stats)
{
    if (stats) printf("CARD_HIL_PICTURE width=%u height=%u\n", stats->width, stats->height);
    printf("CARD_HIL_DONE file=%s mode=%s policy=%s source=%s epoch=%" PRIu32 " result=%s frames=%u window_us=%" PRIu64
           " elapsed_us=%" PRId64 " media_window_us=%" PRIu64 "\n", card_case.file, case_modes[card_case.mode].name,
           card_case.timed ? "timed" : card_case.native ? "native" : "auto",
           route_names[atomic_load(&case_route)],
           card_case.epoch, result, stats ? (unsigned)stats->frames : 0, stats ? stats->window_us : 0,
           esp_timer_get_time() - card_case.started, stats ? stats->media_window_us : 0);
    card_case.phase = CASE_FINISHED;
}

bool pack_panel_card_run(void (*home)(void), const char *file, const char *mode, const char *source, bool pixel_size)
{
    size_t i = 0, m = 0;
    while (i < sizeof(card_cases)/sizeof(card_cases[0]) && strcmp(file, card_cases[i]))
        i++;
    while (m < sizeof(case_modes)/sizeof(case_modes[0]) && strcmp(mode, case_modes[m].name))
        m++;
    if (i == sizeof(card_cases)/sizeof(card_cases[0]) || m == sizeof(case_modes)/sizeof(case_modes[0]) ||
        (strcmp(source, "auto") && strcmp(source, "native") && strcmp(source, "timed")) || !pack_panel_open(home, NULL))
        return false;
    card_case.pixel_size = pixel_size;
    card_case.file = card_cases[i];
    card_case.mode = (unsigned)m;
    card_case.native = !strcmp(source, "native");
    card_case.timed = !strcmp(source, "timed");
    card_case.phase = CASE_SCAN;
    card_case.started = esp_timer_get_time();
    atomic_store(&case_route, CARD_SOURCE_NONE);
    if (case_button("Video files"))
        return true;
    pack_panel_leave();
    return false;
}

static void case_scan(void)
{
    if (filter == PACK_LIST_VIDEO) {
        for (unsigned i = 0; i < count; i++) {
            if (!entries[i].movie || strcmp(entries[i].label, card_case.file))
                continue;
            char label[140];
            snprintf(label, sizeof(label), "Video  %s", card_case.file);
            if (case_button(label)) {
                card_case.phase = CASE_DETAIL;
                card_case.detail_tick = lv_tick_get();
                printf("CARD_HIL_DETAIL file=%s page=%u\n", card_case.file, page);
                return;
            }
            break;
        }
        if (more && case_button("Next"))
            return;
    }
    case_finish("file_not_found", NULL);
}

static void case_play(void)
{
    if (selected >= count || !entries[selected].movie || strcmp(entries[selected].label, card_case.file)) {
        case_finish("unavailable", NULL);
        return;
    }
    const char *caption = case_modes[card_case.mode].button;
    card_case.phase = CASE_PLAY;
    card_case.click = true;
    bool requested = case_button(caption);
    card_case.click = false;
    if (!requested || !media_epoch || media_epoch != card_case.epoch)
        case_finish("unavailable", NULL);
}

bool pack_panel_card_action(const char *action)
{
    if (!card_case.file || atomic_load(&closing)) return false;
    if (card_case.phase == CASE_PLAY && media_epoch && media_epoch == card_case.epoch &&
        media_panel_file_frames(media_epoch) && lv_screen_active() != menu) {
        if (!strcmp(action, "stop")) return click_case_button(lv_screen_active(), "Stop");
        if (!strcmp(action, "home")) return click_case_button(lv_screen_active(), "Home");
    }
    if (card_case.phase != CASE_FINISHED || media_epoch || selected >= count ||
        strcmp(entries[selected].label, card_case.file)) return false;
    const char *caption;
    if (!strcmp(action, "play")) caption = case_modes[card_case.mode].button;
    else if (!strcmp(action, "again")) caption = "Play again";
    else return false;
    uint32_t previous = card_case.epoch;
    card_case.click = true;
    bool clicked = case_button(caption);
    card_case.click = false;
    if (!clicked || !media_epoch || media_epoch == previous || media_epoch != card_case.epoch)
        return false;
    card_case.started = esp_timer_get_time();
    card_case.phase = CASE_PLAY;
    return true;
}

bool pack_panel_card_home(void)
{
    if (!card_case.file || atomic_load(&closing))
        return false;
    pack_panel_leave();
    return true;
}

bool pack_panel_card_reference(void (*home)(void))
{
    if (!pack_panel_open(home, NULL))
        return false;
    if (start_movie(PLAY_REFERENCE, 0, KVM_H264_EXACT))
        return true;
    pack_panel_leave();
    return false;
}
#endif

void pack_panel_magnify(const char *mode)
{
    if (!profile_active || !current || !profile || lv_screen_active() != profile || !(current_review.granted & PACK_PERM_CAPTURE))
        return;
    media_epoch = media_panel_open_magnifier(current->id, mode, pack_panel_leave);
    if (media_epoch) {
        magnifier_telemetry = kvmbridge_composite_telemetry(current->id);
        pack_view_enter_magnifier();
        media_panel_request_capture(media_epoch);
    }
}

static void primary_play_style(lv_obj_t *option)
{
    lv_color_t ink = lv_obj_get_style_text_color(option, LV_PART_MAIN);
    lv_color_t surface = lv_obj_get_style_bg_color(option, LV_PART_MAIN);
    lv_obj_set_style_bg_color(option, ink, LV_PART_MAIN);
    lv_obj_set_style_text_color(option, surface, LV_PART_MAIN);
    lv_obj_set_style_bg_color(option, ink, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(option, surface, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(option, 2, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_t *icon = ui_theme_control_mask(option, &kvm_icon_video);
    lv_obj_set_style_image_recolor(icon, surface, LV_PART_MAIN);
    lv_obj_set_style_image_recolor(icon, surface, LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_align(icon, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void show_movie_detail(void)
{
    reset_body(NULL);
    lv_obj_update_layout(body);
    int width = lv_obj_get_content_width(body) - 48;
    lv_obj_t *title = lv_label_create(body);
    lv_label_set_text(title, entries[selected].label);
    lv_obj_set_width(title, width);
    lv_obj_set_style_text_font(title, ui_theme_font(UI_FONT_TITLE), LV_PART_MAIN);
    lv_obj_set_pos(title, 24, 18);
    lv_obj_update_layout(title);
    int y = 18 + lv_obj_get_height(title) + 24;
    unsigned prepared = entries[selected].prepared_modes;
    unsigned available = kvm_media_has_h264() ? (entries[selected].mp4 ? 31u : 7u) : prepared;
    bool playable = available != 0;
#ifdef KVM_MEDIA_PAGED_GUEST
    playable = playable || !entries[selected].mp4;
#endif
    lv_obj_t *heading = lv_label_create(body);
    lv_label_set_text_static(heading, playable ? "H.264 playback" : "The H.264 decoder is unavailable in this build.");
    lv_obj_set_style_text_font(heading, ui_theme_font(UI_FONT_HEADING), LV_PART_MAIN);
    lv_obj_set_pos(heading, 24, y);
    if (kvm_media_has_h264()) {
        movie_heading = heading;
        command_t inspect = {.type = INSPECT_MOVIE, .index = selected, .epoch = atomic_load(&movie_detail_epoch)};
        xQueueSend(commands, &inspect, 0);
    }
    y += 42;
    static const char *const modes[] = {
        [KVM_H264_EXACT] = "Play", [KVM_H264_REDUCED] = "Reduced", [KVM_H264_LARGE_BANK] = "Large bank",
        [KVM_H264_RR_BLEND] = "Temporal blend", [KVM_H264_RR_SMART_LOOSE] = "Smart loose"
    };
    unsigned shown = 0;
    int column_width = (width - 32) / 3;
    int row_step = lv_obj_get_content_height(body) < 517 ? 76 : 88;
    for (unsigned mode = 0; mode < sizeof(modes) / sizeof(modes[0]); ++mode) {
        if (!(available & (1u << mode)))
            continue;
        lv_obj_t *option = button(body, modes[mode], 24 + (int)(shown % 3) * (column_width + 16),
                                  y + (int)(shown / 3) * row_step,
                                  column_width, row_step - 12, play_movie, (void *)(uintptr_t)mode);
        lv_obj_set_style_pad_hor(option, 16, LV_PART_MAIN);
        lv_obj_t *caption = lv_obj_get_child(option, 0);
        lv_obj_set_style_text_font(caption, ui_theme_font(UI_FONT_HEADING), LV_PART_MAIN);
        lv_obj_align(caption, LV_ALIGN_LEFT_MID, 0, -12);
        if (mode == KVM_H264_EXACT)
            primary_play_style(option);
        lv_obj_t *source = lv_label_create(option);
        lv_label_set_text_static(source, prepared & (1u << mode) ? "Prepared file on card" : "Decode original video");
        lv_obj_set_style_text_font(source, ui_theme_font(UI_FONT_BODY), LV_PART_MAIN);
        lv_obj_align(source, LV_ALIGN_LEFT_MID, 0, 13);
        ++shown;
    }
    y += (int)((shown + 2) / 3) * row_step;
#ifdef KVM_MEDIA_PAGED_GUEST
    if (!entries[selected].mp4) {
        text_row(body, "Decode from the source using up to 32 MiB of temporary card space.", y);
        button(body, "Decode reduced", 24, y + 36, 312, 60, play_paged_movie, (void *)(uintptr_t)KVM_H264_REDUCED);
        button(body, "Decode large bank", 356, y + 36, 312, 60, play_paged_movie, (void *)(uintptr_t)KVM_H264_LARGE_BANK);
        y += 112;
    }
#endif
    button(body, "Back", 24, y + 12, 160, 56, browse, (void *)(uintptr_t)page);
}

static void choose(lv_event_t *e)
{
    selected = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (entries[selected].movie)
        show_movie_detail();
    else
        request(SELECT_ITEM, selected);
}

static void replay_movie(lv_event_t *event)
{
    (void)event;
    if (!start_movie(playing_type, selected, playing_mode))
        show_movie_detail();
}

static void list(void)
{
    const char *name = filter == PACK_LIST_INSTALLED    ? "Installed extensions"
                       : filter == PACK_LIST_INSTALLERS ? "Install from card"
                                                        : "Video files";
    lv_obj_update_layout(body);
    int width = lv_obj_get_content_width(body);
    int height = lv_obj_get_content_height(body);
    bool compact = height < 517; // The normal footer ends at y=516.
    int row_width = LV_MIN(830, width - 48);
    int first_y = compact ? 48 : 50;
    int step = compact ? LV_MIN(52, (height - first_y) / PACK_PAGE) : 52;
    int nav_x = compact ? width - 360 : 24;
    int nav_y = compact ? 0 : 461;
    int nav_height = compact ? 44 : 56;
    lv_obj_set_hidden(library_button, false);
    reset_body(name);
    lv_obj_set_width(lv_obj_get_child(body, 0), compact ? nav_x - 40 : width - 48);
    if (!count) {
        const char *message = filter == PACK_LIST_INSTALLED ? "No installed extensions on this card."
                              : filter == PACK_LIST_INSTALLERS
                                  ? "No packages found. Copy a .kvmpack file to the root of the card."
                                  : "No videos found. Copy an .mp4, .h264 or .264 file to the root "
                                    "of the card.";
        text_row(body, message, 68);
        lv_obj_set_width(lv_obj_get_child(body, -1), width - 48);
        button(body, "Scan again", 24, 130, 200, 56, browse, NULL);
    }
    for (unsigned i = 0; i < count; ++i) {
        char label[140];
        snprintf(label, sizeof(label), "%s  %s",
                 entries[i].movie       ? "Video"
                 : entries[i].manually_disabled ? "Disabled"
                 : entries[i].installed ? "Installed"
                                        : "Extension",
                 entries[i].label);
        lv_obj_t *item = button(body, label, 24, first_y + (int)i * step, row_width, LV_MIN(46, step - 2),
                                choose, (void *)(uintptr_t)i);
        lv_obj_t *caption = lv_obj_get_child(item, 0);
        lv_obj_set_style_max_width(caption, LV_PCT(100), LV_PART_MAIN);
        lv_obj_set_height(caption, lv_obj_get_style_text_font(caption, LV_PART_MAIN)->line_height);
        lv_label_set_long_mode(caption, LV_LABEL_LONG_MODE_CLIP);
    }
    if (page)
        button(body, "Previous", nav_x, nav_y, 160, nav_height, browse, (void *)(uintptr_t)(page - 1));
    if (more)
        button(body, "Next", compact ? width - 184 : 210, nav_y, 160, nav_height, browse, (void *)(uintptr_t)(page + 1));
}

static void plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_set_scrollable(obj, false);
}

enum { DATA_OFF, DATA_UNAVAILABLE, DATA_PARTIAL, DATA_STALE, DATA_CURRENT, DATA_ERROR };

static const ui_status_row_t data_rows[] = {
    [DATA_OFF] = {DATA_OFF, "Telemetry off", UI_SEVERITY_UNKNOWN},
    [DATA_UNAVAILABLE] = {DATA_UNAVAILABLE, "Data unavailable", UI_SEVERITY_OFFLINE},
    [DATA_PARTIAL] = {DATA_PARTIAL, "Data partial", UI_SEVERITY_WARNING},
    [DATA_STALE] = {DATA_STALE, "Data stale", UI_SEVERITY_WARNING},
    [DATA_CURRENT] = {DATA_CURRENT, "Data live", UI_SEVERITY_OK},
    [DATA_ERROR] = {DATA_ERROR, "Data error", UI_SEVERITY_OFFLINE},
};

static void profile_indicators(lv_obj_t *root)
{
    static const ui_status_row_t usb[] = {
        {KVM_HOST_USB_UNINITIALIZED, "USB starting", UI_SEVERITY_UNKNOWN},
        {KVM_HOST_USB_ENUMERATING, "USB connecting", UI_SEVERITY_WARNING},
        {KVM_HOST_USB_DETACHED, "USB detached", UI_SEVERITY_OFFLINE},
        {KVM_HOST_USB_MOUNTED, "USB connected", UI_SEVERITY_OK},
        {KVM_HOST_USB_SUSPENDED, "USB suspended", UI_SEVERITY_WARNING},
        {KVM_HOST_USB_ERROR, "USB error", UI_SEVERITY_OFFLINE},
    };
    static const ui_status_row_t agent[] = {
        {KVB_LINK_IDLE, "Agent idle", UI_SEVERITY_UNKNOWN},
        {KVB_LINK_CONNECTING, "Agent connecting", UI_SEVERITY_WARNING},
        {KVB_LINK_CONNECTED, "Agent connected", UI_SEVERITY_OK},
    };
    static const ui_status_row_t capture[] = {
        {0, "Capture off", UI_SEVERITY_UNKNOWN}, {1, "CAPTURE", UI_SEVERITY_WARNING},
    };
    static const ui_status_desc_t slots[PROFILE_INDICATORS] = {
        [PROFILE_USB] = {.rows = usb, .row_count = sizeof(usb) / sizeof(usb[0])},
        [PROFILE_AGENT] = {.rows = agent, .row_count = sizeof(agent) / sizeof(agent[0])},
        [PROFILE_DATA] = {.rows = data_rows, .row_count = sizeof(data_rows) / sizeof(data_rows[0])},
        [PROFILE_CAPTURE] = {.rows = capture, .row_count = sizeof(capture) / sizeof(capture[0])},
    };
    unsigned perms=current->permissions;
    bool telemetry=(perms & PACK_PERM_TELEMETRY) && current->sources;
    bool uses_capture=(perms & PACK_PERM_CAPTURE)!=0;
    const bool needed[PROFILE_INDICATORS]={perms & PACK_PERM_JOYSTICK,telemetry || uses_capture,telemetry,true};
    for (unsigned i=0; i<PROFILE_INDICATORS; ++i) {
        indicators[i].value=-1;
        indicators[i].item=needed[i] ? ui_status_item_create(root,&slots[i],NULL) : NULL;
        lv_obj_t *item=indicators[i].item;
        if (!item) continue;
        lv_obj_set_height(item,32);
        if (current->portrait) {
            lv_obj_set_style_pad_hor(item,1,0);
            lv_obj_set_style_pad_gap(item,2,0);
            lv_obj_set_style_text_font(ui_status_item_value_label(item),ui_theme_font(UI_FONT_LABEL),0);
        }
        if (i==PROFILE_CAPTURE && !uses_capture) lv_obj_set_hidden(item,true);
    }
}

static int profile_data_status(kvmbridge_link_t link)
{
    if (!current->sources || !(current_review.granted & PACK_PERM_TELEMETRY)) return DATA_OFF;
    if (!profile_active || link != KVB_LINK_CONNECTED) return DATA_UNAVAILABLE;
    unsigned fresh=0;
    bool stale=false;
    for (unsigned i=0; i<current->sources; ++i) {
        const pack_source_t *src=&current->source[i];
        kvmbridge_sample_t sample;
        if (!kvmbridge_sample(src->channel,&sample)) continue;
        if (sample.quality==KVB_ERROR) return DATA_ERROR;
        if (sample.quality==KVB_STALE || sample.age_ms>src->max_age_ms) stale=true;
        else if (sample.quality==KVB_VALID) ++fresh;
    }
    if (stale) return DATA_STALE;
    if (fresh==current->sources) return DATA_CURRENT;
    return fresh ? DATA_PARTIAL : DATA_UNAVAILABLE;
}

static void update_profile_indicators(void)
{
    // On a mixed portrait profile reserve space for the full capture warning.
    static const char *const compact[PROFILE_INDICATORS][6]={
        [PROFILE_USB]={[KVM_HOST_USB_UNINITIALIZED]="USB init",[KVM_HOST_USB_ENUMERATING]="USB wait",
                       [KVM_HOST_USB_DETACHED]="USB off",[KVM_HOST_USB_MOUNTED]="USB ready",
                       [KVM_HOST_USB_SUSPENDED]="USB sleep"},
        [PROFILE_AGENT]={[KVB_LINK_CONNECTING]="Agent wait",[KVB_LINK_CONNECTED]="Agent online"},
        [PROFILE_DATA]={[DATA_UNAVAILABLE]="No data",[DATA_OFF]="Data off"}
    };
    kvmbridge_activity_t host=kvmbridge_activity();
    int values[PROFILE_INDICATORS]={[PROFILE_AGENT]=host.link,[PROFILE_CAPTURE]=host.sharing};
    if (indicators[PROFILE_USB].item) {
        kvm_hid_snapshot_t hid;
        hid_dispatch_get_snapshot(&hid,NULL);
        values[PROFILE_USB]=hid.host_usb;
    }
    if (indicators[PROFILE_DATA].item) values[PROFILE_DATA]=profile_data_status(host.link);
    for (unsigned i=0; i<PROFILE_INDICATORS; ++i) {
        lv_obj_t *item=indicators[i].item;
        if (!item || values[i]==indicators[i].value) continue;
        if (i==PROFILE_CAPTURE && !(current->permissions & PACK_PERM_CAPTURE))
            lv_obj_set_hidden(item,!host.sharing);
        ui_status_item_set_value(item,values[i]);
        if (current->portrait && indicators[PROFILE_USB].item && indicators[PROFILE_AGENT].item &&
            (unsigned)values[i]<6 && compact[i][values[i]])
            lv_label_set_text(ui_status_item_value_label(item),compact[i][values[i]]);
        indicators[i].value=values[i];
    }
}

static lv_obj_t *screen(const char *title, lv_obj_t **content)
{
    lv_obj_t *physical = lv_obj_create(NULL);
    if (!physical) return NULL;
    lv_obj_set_scrollable(physical, false);
    bool portrait = title && current && current->portrait;
    lv_obj_t *root = pack_view_frame(physical, portrait);
    if (!root) {
        lv_obj_delete(physical);
        return NULL;
    }
    const int header_height = title ? 32 : 48;
    ui_theme_apply(root, UI_STYLE_CONTROL_PAGE, LV_STATE_DEFAULT);
    bool switcher = title && current && current->screen_count;
    lv_obj_t *header=root;
    if (title) {
        header=lv_obj_create(root);
        plain(header);
        lv_obj_set_size(header,portrait ? 600 : 1024,header_height);
        lv_obj_set_flex_flow(header,LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header,LV_FLEX_ALIGN_START,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_left(header,switcher ? 12 : 0,0);
        lv_obj_set_style_pad_right(header,4,0);
        lv_obj_set_style_pad_column(header,portrait ? 3 : 8,0);
    }
    lv_obj_t *home = NULL;
    if (!switcher) {
        home = button(header, "Home", 0, 0, 100, header_height, home_clicked, NULL);
        lv_obj_set_style_pad_hor(home, 4, 0);
        lv_obj_align(lv_obj_get_child(home, 0), LV_ALIGN_CENTER, 12, 0);
        lv_obj_align(ui_theme_control_mask(home, &kvm_icon_home), LV_ALIGN_LEFT_MID, 2, 0);
    }
    if (!title) {
        lv_obj_set_pos(home, UI_SPACE_XL, 0);
        lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(home, 0, LV_PART_MAIN);
        library_button = button(root, "Extensions", UI_SPACE_XL + 100 + UI_SPACE_S, 0, 160,
                                header_height, categories, NULL);
        lv_obj_set_style_radius(library_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(library_button, 0, LV_PART_MAIN);
        lv_obj_align(lv_obj_get_child(library_button, 0), LV_ALIGN_CENTER, 10, 0);
        lv_obj_align(ui_theme_control_mask(library_button, &kvm_icon_extensions), LV_ALIGN_LEFT_MID, 0, 0);
    } else {
        lv_obj_t *label = lv_label_create(header);
        lv_obj_set_size(label,1,lv_font_get_line_height(lv_obj_get_style_text_font(label,LV_PART_MAIN)));
        lv_obj_set_flex_grow(label,1);
        lv_label_set_long_mode(label,LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(label,title);
    }
    *content = lv_obj_create(root);
    plain(*content);
    lv_obj_set_pos(*content, 0, header_height);
    lv_display_t *display = lv_obj_get_display(root);
    lv_obj_set_size(*content, title ? (portrait ? 600 : 1024) : lv_display_get_horizontal_resolution(display),
                    (title ? (portrait ? 1024 : 600) : lv_display_get_vertical_resolution(display)) - header_height);
    if (title) {
        profile_indicators(header);
        update_profile_indicators();
    }
    return physical;
}

static void trial_refresh(lv_event_t *event)
{
    if (!trial_shield || lv_screen_active() != profile)
        return;
    if (lv_event_get_code(event) == LV_EVENT_FLUSH_FINISH)
        trial_flush = true;
    else if (trial_flush && atomic_load(&trial_status) == TRIAL_WAITING) {
        trial_presented_at = esp_timer_get_time();
        trial_status_t waiting = TRIAL_WAITING;
        atomic_compare_exchange_strong(&trial_status, &waiting, TRIAL_PRESENTED);
    }
}

bool pack_panel_local_contacts(unsigned count, const lv_point_t physical[2], const uint8_t ids[2], uint64_t timestamp_us)
{
    if (!profile_active || !current || !profile || trial_shield || lv_screen_active() != profile)
        return false;
    lv_point_t logical[2];
    if (count <= 2) {
        for (unsigned i = 0; i < count; ++i)
            pack_view_logical_point(current->portrait, &physical[i], &logical[i]);
    }
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (pack_view_touchpad(count, logical, ids, timestamp_us)) return true;
#else
    (void)ids; (void)timestamp_us;
#endif
    return pack_chart_view_touch(count, logical);
}

static void end_trial_view(void)
{
    if (trial_shield) {
        lv_display_remove_event_cb_with_user_data(lv_display_get_default(), trial_refresh, NULL);
        lv_obj_delete(trial_shield);
        trial_shield = NULL;
    }
    trial_flush = false;
}

static void present_trial(void)
{
    if (atomic_load(&trial_status) != TRIAL_WAITING) return;
    lv_obj_t *content=NULL;
    profile = screen(current->title, &content);
    pack_view_provider(trial_xplane);
    if (!pack_view_open(content, current, current_review.granted)) {
        pack_view_close();
        if (profile) lv_obj_delete(profile);
        profile = NULL;
        trial_status_t waiting = TRIAL_WAITING;
        atomic_compare_exchange_strong(&trial_status, &waiting, TRIAL_REFUSED);
        return;
    }
    trial_shield = lv_obj_create(content);
    lv_obj_remove_style_all(trial_shield);
    lv_obj_set_size(trial_shield, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(trial_shield, false);
    lv_obj_set_clickable(trial_shield, true);
    lv_obj_update_layout(profile);
    trial_flush = false;
    lv_display_add_event_cb(lv_display_get_default(), trial_refresh, LV_EVENT_FLUSH_FINISH, NULL);
    lv_display_add_event_cb(lv_display_get_default(), trial_refresh, LV_EVENT_REFR_READY, NULL);
    lv_screen_load(profile);
}

static void begin_activation(bool allow_changed_downgrade)
{
    if (!current || current_review.disabled || current_review.manually_disabled ||
        (current->required_permissions & ~pack_view_available_permissions()) || activation_pending || profile)
        return;
    if (receive_epoch && !pack_receive_begin_commit(receive_epoch)) {
        pack_panel_leave();
        return;
    }
    activation_pending = true;
    atomic_store(&trial_status, TRIAL_WAITING);
    if (!send_request((command_t){.type = ACTIVATE, .epoch = receive_epoch, .granted = current_review.granted,
                                  .allow_changed_downgrade = allow_changed_downgrade})) {
        if (receive_epoch)
            pack_receive_finish(receive_epoch, false);
        pack_panel_leave();
    }
}

// Keep every confirmation and navigation target outside the scrolling text.
// Long package and publisher identities wrap without moving these controls.
static lv_obj_t *review_pane(void)
{
    reset_body(NULL);
    lv_obj_t *pane = lv_obj_create(body);
    plain(pane);
    lv_obj_set_pos(pane, 24, 10);
    lv_obj_set_size(pane, 960, 426);
    lv_obj_set_scrollable(pane, true);
    lv_obj_set_scroll_dir(pane, LV_DIR_VER);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(pane, 7, 0);
    return pane;
}

static void review_line(lv_obj_t *pane, const char *s)
{
    lv_obj_t *label = lv_label_create(pane);
    lv_obj_set_width(label, 932);
    lv_label_set_text(label, s);
}

static void format_permissions(char line[150], const char *caption, unsigned mask)
{
    static const char *const names[] = {"Display and touch", "USB joystick", "Simulator telemetry", "HOST screen capture",
                                        "USB display", "USB keyboard", "USB mouse"};
    snprintf(line, 150, "%s: ", caption);
    bool first = true;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (mask & (1u << i)) {
            if (!first)
                strcat(line, ", ");
            strcat(line, names[i]);
            first = false;
        }
    }
    if (first)
        strcat(line, "None");
}

static void review_permissions(lv_obj_t *pane, const char *caption, unsigned mask)
{
    char line[150];
    format_permissions(line, caption, mask);
    review_line(pane, line);
}

static void permission_changes(lv_obj_t *pane)
{
    unsigned previous = current_review.previous_permissions, required = current_review.previous_required;
    if (current_review.generation && previous == current->permissions && required == current->required_permissions) {
        review_line(pane, "Permission changes: None");
        return;
    }
    if (current->permissions & ~previous)
        review_permissions(pane, "Added", current->permissions & ~previous);
    if (previous & ~current->permissions)
        review_permissions(pane, "Removed", previous & ~current->permissions);
    if (current_review.generation && (current->required_permissions & ~required))
        review_permissions(pane, "Newly required", current->required_permissions & ~required);
    if (current_review.generation && (required & ~current->required_permissions))
        review_permissions(pane, "No longer required", required & ~current->required_permissions);
}

static void review_details(lv_event_t *e);

static void refresh_install_buttons(void)
{
    if (!primary_action) return;
    bool sim = (current_review.granted & PACK_PERM_TELEMETRY) != 0;
    bool open = current_review.same_archive;
    char summary[150];
    format_permissions(summary, "Allow on opening", current_review.granted);
    lv_label_set_text(grant_summary, summary);
    lv_label_set_text_static(lv_obj_get_child(primary_action, 0),
                             sim ? (open ? "Open with MSFS" : "Install with MSFS") :
                                   (open ? "Open" : "Install and open"));
    if (xplane_action) lv_obj_set_hidden(xplane_action, !sim);
}

static void select_permission(lv_event_t *e)
{
    if (!current || activation_pending) return;
    unsigned bit = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED)) current_review.granted |= bit;
    else current_review.granted &= ~bit;
    refresh_install_buttons();
}

static void permission_choices(lv_obj_t *pane)
{
    static const struct { unsigned bit; const char *name, *effect; } caps[] = {
        {PACK_PERM_JOYSTICK, "USB joystick", "Sends your axis and button movements to the USB computer."},
        {PACK_PERM_TELEMETRY, "Simulator telemetry", "Reads instrument values from the selected simulator."},
        {PACK_PERM_CAPTURE, "HOST screen capture", "Shows the selected screen region after you approve sharing on both devices."}
    };
    for (unsigned i = 0; i < sizeof(caps) / sizeof(caps[0]); ++i) {
        unsigned bit = caps[i].bit;
        if (!(current->permissions & bit)) continue;
        if (current->required_permissions & bit || !(pack_view_available_permissions() & bit)) {
            char line[110];
            snprintf(line, sizeof(line), "%s (%s)", caps[i].name,
                     !(pack_view_available_permissions() & bit) ? "unavailable on this firmware" : "required");
            review_line(pane, line);
        } else {
            lv_obj_t *check = lv_checkbox_create(pane);
            lv_checkbox_set_text_static(check, caps[i].name);
            lv_obj_set_style_min_height(check, 44, 0);
            if (current_review.granted & bit) lv_obj_add_state(check, LV_STATE_CHECKED);
            lv_obj_add_event_cb(check, select_permission, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)bit);
        }
        review_line(pane, caps[i].effect);
    }
}

static void review_digest(lv_obj_t *pane, const char *caption, const uint8_t digest[32])
{
    char hex[65];
    for (unsigned i = 0; i < 32; ++i)
        snprintf(hex + 2*i, 3, "%02x", (unsigned)digest[i]);
    review_line(pane, caption);
    review_line(pane, hex);
}

static void confirm_downgrade(lv_event_t *e)
{
    (void)e;
    if (current && pack_review_needs_downgrade_grant(&current_review, current))
        begin_activation(true);
}

static void activate(lv_event_t *e)
{
    if (!current || activation_pending || profile)
        return;
    trial_xplane = (current_review.granted & PACK_PERM_TELEMETRY) && (uintptr_t)lv_event_get_user_data(e) != 0;
    if (!pack_review_needs_downgrade_grant(&current_review, current)) {
        begin_activation(false);
        return;
    }
    lv_obj_t *pane = review_pane();
    review_line(pane, "Allow this older version to change permissions?");
    review_line(pane, current->id);
    char version[110];
    snprintf(version, sizeof(version), "Installed %s   /   Requested %s", current_review.previous_version, current->version);
    review_line(pane, version);
    permission_changes(pane);
    review_permissions(pane, "Allow on opening", current_review.granted);
    button(body, "Back to review", 24, 454, 220, 60, review_details, NULL);
    button(body, "Allow downgrade", 730, 454, 250, 60, confirm_downgrade, NULL);
}

static void remove_confirmed(lv_event_t *e)
{
    (void)e;
    current = NULL;
    page = 0;
    request(UNINSTALL, 0);
}

static void remove_clicked(lv_event_t *e)
{
    (void)e;
    reset_body("Remove this installed extension from the card?");
    button(body, "Remove", 24, 70, 200, 56, remove_confirmed, NULL);
    button(body, "Cancel", 250, 70, 200, 56, browse, (void *)(uintptr_t)page);
}

static void toggle_profile(lv_event_t *e)
{
    (void)e;
    if (current && !receive_epoch)
        request(current_review.manually_disabled ? ENABLE_PROFILE : DISABLE_PROFILE, selected);
}

static void review_details(lv_event_t *event)
{
    (void)event;
    if (!current)
        return;
    lv_obj_t *pane = review_pane();
    review_line(pane, current->title);
    review_line(pane, current->id);
    char line[160];
    const char *operation = current_review.disabled ? "Disabled version" : !current_review.generation ? "First install" : current_review.same_archive ? "Installed package"
                            : current_review.downgrade ? "Downgrade" : "Update";
    snprintf(line, sizeof(line), "%s   |   Version %s   |   %u bytes", operation, current->version,
             (unsigned)current_review.archive_bytes);
    review_line(pane, line);
    if (current_review.generation && !current_review.same_archive) {
        snprintf(line, sizeof(line), "Installed version: %s", current_review.previous_version);
        review_line(pane, line);
    }
    if (receive_epoch) {
        snprintf(line, sizeof(line), "Paired client: %s", sending_client);
        review_line(pane, line);
        lv_obj_set_hidden(library_button, true);
    } else {
        review_line(pane, "Source: microSD");
    }
    if (current->publisher_verified) {
        review_digest(pane, "Verified publisher key (SHA-256)", current->publisher_key_sha256);
    } else {
        review_line(pane, receive_epoch ? "Unsigned package" : "Unsigned local package");
    }
    review_permissions(pane, "Requested permissions", current->permissions);
    permission_changes(pane);
    bool sim = (current->permissions & PACK_PERM_TELEMETRY) != 0;
    bool capture = (current->permissions & PACK_PERM_CAPTURE) != 0;
    // The manifest validator requires exactly these host.requires for the admitted permissions.
    snprintf(line, sizeof(line), "HOST requirements: %s%s%s", sim ? "Simulator telemetry (telemetry.sim)" : "",
             sim && capture ? ", " : "", capture ? "Windows capture (capture.windows)" : sim ? "" : "None");
    review_line(pane, line);
    permission_choices(pane);
    review_digest(pane, "Package SHA-256", current_review.candidate_digest);
    bool installed_entry = !receive_epoch && entries[selected].installed;
    if (current_review.disabled)
        review_line(pane, "Disabled after three activation failures. Choose a different version.");
    if (current_review.manually_disabled)
        review_line(pane, installed_entry ? "Disabled by you. Enable this extension to open it."
                          : current_review.generation ? "Disabled by you. Enable it in Installed extensions before updating."
                          : "Disabled by you. Enable it from the local card installer to restore this extension.");
    primary_action = xplane_action = grant_summary = NULL;
    if (current->required_permissions & ~pack_view_available_permissions())
        review_line(pane, "This extension requires a capability that is unavailable on this firmware.");
    else if (!current_review.disabled && !current_review.manually_disabled) {
        // Restored choices are suggestions. The system-owned summary stays visible beside the local approval.
        lv_obj_set_height(pane, 384);
        grant_summary = lv_label_create(body);
        lv_obj_set_pos(grant_summary, 24, 404);
        lv_obj_set_width(grant_summary, 958);
        primary_action = button(body, "", 24, 454, 235, 60, activate, NULL);
        if (sim)
            xplane_action = button(body, current_review.same_archive ? "Open with X-Plane" : "Install with X-Plane",
                                   272, 454, 235, 60, activate, (void *)(uintptr_t)1);
        refresh_install_buttons();
    }
    bool local_orphan = !receive_epoch && !current_review.generation && current_review.manually_disabled;
    if (installed_entry || local_orphan)
        button(body, current_review.manually_disabled ? "Enable" : "Disable", 520, 454, 117, 60, toggle_profile, NULL);
    if (installed_entry)
        button(body, "Uninstall", 650, 454, 132, 60, remove_clicked, NULL);
    button(body, receive_epoch ? "Decline" : "Back", 794, 454, 178, 60,
           receive_epoch ? decline_receive : browse, (void *)(uintptr_t)page);
}

static void poll(lv_timer_t *t)
{
    (void)t;
#if CONFIG_KVM_USB_MONITOR_EVAL
    if (usb_monitor_view_active()) {
        kvm_usb_state_t link = usb_hid_device_monitor_state();
        usb_monitor_view_set_link(link == KVM_USB_MOUNTED, link == KVM_USB_SUSPENDED);
    }
#endif
#if CONFIG_KVM_USB_MONITOR_EVAL && defined(KVM_MEDIA_SYNTHETIC_DIAG)
    if ((monitor_trial == USB_MONITOR_TRIAL_LINK && lv_tick_elaps(monitor_test_start) >= 15000) ||
        (monitor_trial == USB_MONITOR_TRIAL_DESKTOP && lv_tick_elaps(monitor_test_start) >= 75000)) pack_panel_leave();
    if (monitor_trial == USB_MONITOR_TRIAL_CHART) {
        if (usb_monitor_stats().presented) {
            // Sample only this local, deterministic fixture. Both halves must
            // match within the same board framebuffer after the final LVGL flush.
            for (int fb = 0; fb < 2; ++fb) {
                const uint16_t *pixels = board_lcd_fb(fb);
                if (!pixels) continue;
                bool match = true;
                for (unsigned y = 0; y < 4; ++y)
                    for (unsigned x = 0; x < 4; ++x)
                        match &= pixels[(144 + y * 120) * 1024 + 272 + x * 160] == (y < 2 ? 0xf800 : 0x001f);
                if (match) monitor_chart_matches |= 1u << fb;
            }
        }
        if (monitor_chart_matches || lv_tick_elaps(monitor_test_start) >= 3000) pack_panel_leave();
    }
#endif
    reply_t r;
    while (xQueueReceive(replies, &r, 0)) {
        if (r.type == CLOSED) {
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (card_case.file) {
                memset(&card_case, 0, sizeof(card_case));
                puts("CARD_HIL_HOME ready=1");
            }
#endif
            busy = false;
            lv_timer_delete(timer);
            timer = NULL;
            return;
        }
        if (atomic_load(&closing))
            continue;
        if (r.type == MOVIE_INFO) {
            if (movie_heading && r.epoch == atomic_load(&movie_detail_epoch)) {
                const kvm_h264_source_info_t *info = &r.movie_info;
                char fps[24] = "";
                if (info->fps_milli) {
                    snprintf(fps, sizeof(fps), "%u.%03u", info->fps_milli / 1000, info->fps_milli % 1000);
                    size_t n = strlen(fps);
                    while (fps[n - 1] == '0') --n;
                    if (fps[n - 1] == '.') --n;
                    fps[n] = '\0';
                }
                lv_label_set_text_fmt(movie_heading, "H.264 playback  |  %u x %u%s%s%s", info->width, info->height,
                                      fps[0] ? "  |  " : "", fps, fps[0] ? " fps" : "");
            }
            continue;
        }
#if CONFIG_KVM_USB_MONITOR_EVAL
        // The built-in USB monitor has no dependency on the removable card.
        if (r.type == CARD_UNAVAILABLE && usb_monitor_view_active()) continue;
#endif
        if (library_button) {
            lv_obj_remove_state(library_button, LV_STATE_DISABLED);
            lv_obj_set_hidden(library_button, false);
        }
        if (r.type == CARD_UNAVAILABLE || r.type == ROLLED_BACK || r.type == DISABLED) {
            pack_panel_leave();
            if (r.type == CARD_UNAVAILABLE) {
                ui_toast_show(lv_screen_active(), r.error == ESP_FAIL ? "Could not read the microSD card."
                                                                     : "The microSD card is no longer available.", 4000);
            } else if (r.type == DISABLED || r.type == ROLLED_BACK) {
                const char *reason = r.type == DISABLED ? "This version was disabled after three activation failures."
                                    : r.error == ESP_ERR_NOT_SUPPORTED ? "This extension needs an unavailable capability."
                                    : r.error == ESP_ERR_TIMEOUT ? "Extension activation timed out."
                                    : r.error == ESP_ERR_INVALID_STATE ? "The installation changed. Review the package again."
                                    : r.error == ESP_ERR_NOT_FOUND ? "Card removed during extension activation."
                                                                 : "Extension activation failed.";
                if (r.review.previous_version[0]) {
                    char message[160];
                    snprintf(message, sizeof(message), "%s\nInstalled extensions: %s",
                             reason, r.review.previous_version);
                    ui_toast_show(lv_screen_active(), message, 7000);
                } else {
                    ui_toast_show(lv_screen_active(), reason, r.type == DISABLED ? 7000 : 4000);
                }
            }
            continue;
        }
        if (r.type == LIST) {
            atomic_fetch_add(&scans, 1);
            list();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (card_case.file && card_case.phase == CASE_SCAN)
                case_scan();
#endif
            continue;
        }
        if (r.type == READY) {
            if (receive_epoch && !pack_receive_review_active(receive_epoch)) {
                pack_panel_leave();
                continue;
            }
            current = r.scene;
            current_review = r.review;
            current_review.granted &= current->required_permissions | pack_view_available_permissions();
            review_details(NULL);
        } else if (r.type == PRESENT_TRIAL) {
            present_trial();
        } else if (r.type == ACTIVE) {
            end_trial_view();
            profile_active = true;
            activation_pending = false;
            receive_epoch = 0;
        } else if (r.type == MOVIE_DONE) {
            if (!media_epoch || r.epoch != media_epoch)
                continue;
            media_panel_file_stats_t stats = {0};
            bool measured = r.playback == KVM_PLAY_OK && media_panel_file_stats(r.epoch, &stats);
            if (r.playback != KVM_PLAY_OK)
                stats.frames = media_panel_file_frames(r.epoch);
            media_panel_close();
            media_epoch = 0;
            if (atomic_exchange(&stopping_movie, false)) {
                show_movie_detail();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
                if (card_case.file && card_case.epoch == r.epoch)
                    case_finish("stopped", NULL);
#endif
                continue;
            }
            reset_body(NULL);
            lv_obj_t *title = text_row(body, media_panel_play_message(r.playback), UI_SPACE_L);
            lv_obj_set_style_text_font(title, ui_theme_font(UI_FONT_TITLE), LV_PART_MAIN);
            lv_obj_update_layout(title);
            char message[320];
            media_panel_file_summary(message, sizeof(message), playing_filename, playing_mode, measured ? &stats : NULL);
            static const char *const source_labels[] = {
                [CARD_SOURCE_NONE] = NULL, [CARD_SOURCE_NATIVE] = "Native H.264",
                [CARD_SOURCE_COMPANION] = "Prepared companion", [CARD_SOURCE_PAGED] = "Paged H.264"
            };
            if (source_labels[r.source]) {
                size_t used = strlen(message);
                snprintf(message + used, sizeof(message) - used, "\n%s", source_labels[r.source]);
            }
            if (r.playback != KVM_PLAY_OK) {
                size_t used = strlen(message);
                snprintf(message + used, sizeof(message) - used, "\n%u frame%s displayed",
                         (unsigned)stats.frames, stats.frames == 1 ? "" : "s");
            }
            lv_obj_t *details = text_row(body, message, lv_obj_get_y(title) + lv_obj_get_height(title) + UI_SPACE_L);
            lv_obj_set_style_text_font(details, ui_theme_font(UI_FONT_BODY), LV_PART_MAIN);
            lv_obj_set_style_text_line_space(details, UI_SPACE_S, LV_PART_MAIN);
            lv_obj_update_layout(details);
            int browse_y = lv_obj_get_y(details) + lv_obj_get_height(details) + UI_SPACE_L;
            if (r.playback == KVM_PLAY_NO_MEMORY) {
                const char *hint = r.source == CARD_SOURCE_NATIVE
                    ? "In KVMpad Client on Windows, use Tools > Prepare card video with this mode. Copy the prepared files to the card's root folder."
                    : "Try a lower-resolution video on the microSD card.";
                lv_obj_t *help = text_row(body, hint, browse_y);
                lv_obj_update_layout(help);
                browse_y = lv_obj_get_y(help) + lv_obj_get_height(help) + UI_SPACE_L;
            }
            int files_x = UI_SPACE_XL;
            if (r.playback == KVM_PLAY_OK && selected < count && entries[selected].movie &&
                !strcmp(entries[selected].label, playing_filename)) {
                lv_obj_t *again = button(body, "Play again", UI_SPACE_XL, browse_y, 220, 56, replay_movie, NULL);
                primary_play_style(again);
                files_x += 220 + UI_SPACE_M;
            }
            lv_obj_t *files = button(body, "Browse files", files_x, browse_y, 220, 56, browse, (void *)(uintptr_t)page);
            menu_row(files, &kvm_icon_video, UI_FONT_HEADING);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (card_case.file && card_case.phase == CASE_PLAY && card_case.epoch == r.epoch) {
                const char *name = r.playback == KVM_PLAY_OK ? "ok" : r.playback == KVM_PLAY_NO_MEMORY ? "no_memory" :
                                   r.playback == KVM_PLAY_STOPPED ? "stopped" : "unavailable";
                case_finish(name, &stats);
            }
#endif
        } else if (r.type == ERROR || r.type == CARD_MOUNT_ERROR) {
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
            if (card_case.file && card_case.phase != CASE_FINISHED)
                case_finish("card_error", NULL);
#endif
            bool incoming = receive_epoch != 0;
            if (receive_epoch) {
                pack_receive_cancel(receive_epoch);
                receive_epoch = 0;
            }
            if (media_epoch) {
                media_panel_close();
                media_epoch = 0;
            }
            if (profile) {
                pack_view_close();
                lv_obj_delete(profile);
                profile = NULL;
            }
            current = NULL;
            if (r.type == CARD_MOUNT_ERROR) {
                reset_body(r.error == ESP_ERR_NO_MEM ? "Not enough memory to open the microSD card."
                        : "Could not open the microSD card. Insert a FAT32 or exFAT card and try again.");
                button(body, incoming ? "Retry transfer" : "Retry", 24, 85, 220, 56,
                       incoming ? receive_clicked : browse, NULL);
                continue;
            }
            reset_body(incoming && r.error == ESP_ERR_INVALID_STATE ? "The transfer was cancelled or expired."
                       : incoming && r.error == ESP_ERR_INVALID_RESPONSE ? "Package verification failed."
                       : r.error == ESP_ERR_NOT_SUPPORTED ? "This extension needs unsupported capabilities or has "
                                                          "invalid data."
                       : r.error == ESP_ERR_NO_MEM      ? "Insufficient memory for this extension."
                                                        : "Card or extension unavailable. Insert a FAT32 or exFAT "
                                                          "microSD card and retry.");
            button(body, "Browse again", 24, 85, 220, 56, browse, NULL);
        }
    }
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (card_case.file && card_case.phase == CASE_DETAIL && !atomic_load(&closing) && lv_tick_elaps(card_case.detail_tick) >= 100)
        case_play();
#endif
    if (receive_epoch && current && !activation_pending && !pack_receive_review_active(receive_epoch)) {
        pack_panel_leave();
        return;
    }
    if (profile_active && profile && current && !atomic_load(&closing)) {
        if (lv_screen_active() == profile) {
            pack_view_tick();
            update_profile_indicators();
        } else if (media_epoch && current->composite && current->sources && (current_review.granted & PACK_PERM_TELEMETRY)) {
            bool negotiated = kvmbridge_composite_telemetry(current->id);
            if (negotiated) magnifier_telemetry = true;
            if (magnifier_telemetry) {
                int value = negotiated ? profile_data_status(KVB_LINK_CONNECTED) : DATA_UNAVAILABLE;
                media_panel_telemetry_status(current->id, data_rows[value].text);
            }
        }
    }
}

bool pack_panel_open(void (*home)(void), void (*tools)(void))
{
    if (busy)
        return false;
    if (!commands)
        commands = xQueueCreate(2, sizeof(command_t));
    if (!replies)
        replies = xQueueCreate(4, sizeof(reply_t));
    if (!commands || !replies)
        return false;
    xQueueReset(commands);
    xQueueReset(replies);
    atomic_store(&closing, false);
    // The Xtensa AVC parser uses a 20,864-byte stack frame for reference
    // marking. The 7B worker used 42,296 bytes on a decoder allocation failure.
    // Keep its stack in PSRAM to leave internal DMA memory available.
    if (xTaskCreatePinnedToCoreWithCaps(worker, "pack_card", 64 * 1024, NULL, 2, NULL, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        return false;
    busy = true;
    profile_active = false;
    activation_pending = false;
    receive_epoch = 0;
    page = 0;
    return_home = home;
    open_tools = tools;
    menu = screen(NULL, &body);
    lv_screen_load(menu);
    timer = lv_timer_create(poll, 50, NULL);
    categories(NULL);
    const command_t watch = {.type = WATCH};
    xQueueSend(commands, &watch, 0);
    return true;
}

void pack_panel_leave(void)
{
    if (!busy || atomic_load(&closing))
        return;
    clear_movie_detail();
#if CONFIG_KVM_USB_MONITOR_EVAL
#if defined(KVM_MEDIA_SYNTHETIC_DIAG) && defined(KVM_USB_PICTURE_CHECK)
    usb_monitor_picture_t chart_picture = {"absent", "absent", 0};
    if (monitor_trial == USB_MONITOR_TRIAL_CHART) chart_picture = usb_monitor_view_picture_check();
#endif
    usb_monitor_view_close();
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (monitor_trial == USB_MONITOR_TRIAL_LINK) {
        printf("USB_MONITOR_LINK restoring_hid=%u\n", (unsigned)select_monitor_usb(false));
    }
    if (monitor_trial == USB_MONITOR_TRIAL_CHART) {
        usb_monitor_stats_t s = usb_monitor_stats();
        bool ok = s.presented == 1 && s.decoded == 1 && !s.busy_drops && !s.decode_errors && monitor_chart_matches;
#ifdef KVM_USB_PICTURE_CHECK
        ok = ok && chart_picture.sequence == 1 && !strcmp(chart_picture.decoded, "has_nonblack") &&
             !strcmp(chart_picture.reference, "has_nonblack");
        printf("USB_MONITOR_TEST result=%s presented=%u decoded=%u busy_drops=%u decode_errors=%u fb_mask=%u"
               " decoded_picture=%s reference=%s sequence=%u\n",
               ok ? "pass" : "fail", s.presented, s.decoded, s.busy_drops, s.decode_errors, monitor_chart_matches,
               chart_picture.decoded, chart_picture.reference, chart_picture.sequence);
#else
        printf("USB_MONITOR_TEST result=%s presented=%u decoded=%u busy_drops=%u decode_errors=%u fb_mask=%u\n",
               ok ? "pass" : "fail", s.presented, s.decoded, s.busy_drops, s.decode_errors, monitor_chart_matches);
#endif
    }
    monitor_trial = USB_MONITOR_TRIAL_IDLE;
#endif
#endif
    atomic_store(&trial_status, TRIAL_CANCELLED);
#ifdef KVM_MEDIA_SYNTHETIC_DIAG
    if (card_case.file && card_case.phase != CASE_FINISHED)
        case_finish("stopped", NULL);
#endif
    if (receive_epoch)
        pack_receive_cancel(receive_epoch);
    end_trial_view();
    profile_active = false;
    activation_pending = false;
    if (media_epoch) {
        media_panel_close();
        media_epoch = 0;
    }
    if (profile)
        pack_view_close();
    bool visible = lv_screen_active() == menu || (profile && lv_screen_active() == profile);
    if (visible)
        return_home();
    if (profile)
        lv_obj_delete_async(profile);
    if (menu)
        lv_obj_delete_async(menu);
    profile = NULL;
    menu = NULL;
    body = NULL;
    current = NULL;
    library_button = NULL;
    atomic_store(&closing, true);
}
