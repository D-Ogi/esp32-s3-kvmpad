// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_panel.h"
#include "kvmbridge.h"
#include "lvgl.h"
#include "src/image/lv_image_decoder_private.h"
#include "panel_test_platform.h"
#include "sim_image.h"
#include "ui_theme.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

static void (*entry)(void *);
static pthread_t worker;
static bool stopped;
static unsigned scanout_reads;
static uint16_t *test_fb[2];
static unsigned test_front;
static atomic_bool available;
static FILE *sample;
static bool played;
static kvm_h264_reconstruction_t card_mode = KVM_H264_REDUCED;
_Atomic int64_t simulated_time_offset;
static uint32_t capture_epoch;
static unsigned capture_requests;
static bool host_attached = true;
static uint32_t closing_fixture;
static char requested_mode[16];
static kvm_live_codec_t requested_codec;
static bool fixture_bound;
static bool fixture_review;
static const char *review_resource = "Test display 2";
static const char *pointer_resource;
static uint32_t allowed_epoch;
static const char *capture_text = "Test stream";
static const char *capture_code;
size_t test_largest_psram = 8u * 1024 * 1024, test_refuse_size, test_shrink_size;
unsigned test_surfaces_until_failure;
static bool simulated_scan, synthetic_done;

void board_lcd_get_stats(board_lcd_stats_t *s)
{
    uint32_t frames = simulated_scan ? (uint32_t)(esp_timer_get_time() / 16667) : 0;
    *s = (board_lcd_stats_t){frames, frames};
}

uint16_t *board_lcd_fb(int index)
{
    ++scanout_reads;
    if (test_fb[0])
        return index < 2 ? test_fb[index] : NULL;
    lv_draw_buf_t *buf = lv_display_get_buf_active(lv_display_get_default());
    assert(buf && buf->header.stride == 2048);
    return index == 0 ? (void *)buf->data : NULL;
}

uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *p, uint32_t size)
{
    crc = ~crc;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0xedb88320));
    }
    return ~crc;
}

static void *thread_main(void *arg)
{
    entry(arg);
    return NULL;
}
int xTaskCreatePinnedToCoreWithCaps(void (*f)(void *), const char *name, unsigned bytes, void *arg, unsigned priority,
                                   void *handle, int core, unsigned caps)
{
    (void)name;
    assert(core == (bytes == 65536 ? CONFIG_KVM_UI_TASK_CORE : tskNO_AFFINITY));
    (void)priority;
    (void)handle;
    (void)caps;
    entry = f;
    return !pthread_create(&worker, NULL, thread_main, arg);
}

static kvmbridge_crop_push_t received_picture;
static kvmbridge_receive_buffer_t receive_buffer;
static void (*transport_ended)(uint32_t);
static char requested_profile[101];
static atomic_uint shown_count, dropped_count, credited_epoch, credited_sequence;
void kvmbridge_frame_done(uint32_t epoch, uint32_t sequence, bool shown)
{
    atomic_store(&credited_epoch, epoch);
    atomic_store(&credited_sequence, sequence);
    atomic_fetch_add(shown ? &shown_count : &dropped_count, 1);
}
static uint32_t frame_sequence(const uint8_t *msg)
{
    uint32_t sequence;
    memcpy(&sequence, msg + 12, sizeof(sequence));
    return sequence;
}
void kvmbridge_media_with_buffer(bool (*start)(uint32_t, uint32_t, kvm_live_codec_t),
                                 kvmbridge_crop_push_t push, void (*end)(uint32_t),
                                 kvmbridge_receive_buffer_t buffer)
{
    (void)start;
    received_picture = push;
    receive_buffer = buffer;
    transport_ended = end;
}
bool kvmbridge_capture_paced(const char *profile, uint32_t epoch, kvm_live_codec_t codec, const char *mode,
                             unsigned max_fps)
{
    assert(max_fps == 10);
    ++capture_requests;
    snprintf(requested_profile, sizeof(requested_profile), "%s", profile);
    capture_epoch = epoch;
    capture_text = "Test stream";
    capture_code = NULL;
    pointer_resource = NULL;
    fixture_bound = false;
    requested_codec = codec;
    snprintf(requested_mode, sizeof(requested_mode), "%s", mode);
    return true;
}
bool kvmbridge_host_endpoint(char endpoint[24])
{
    strcpy(endpoint, host_attached ? "192.0.2.2:8765" : "");
    return host_attached;
}
kvmbridge_display_state_t kvmbridge_display(uint32_t epoch, char resource[64])
{
    resource[0] = 0;
    if (epoch && epoch == closing_fixture) return KVB_DISPLAY_CLOSING;
    if (fixture_review && epoch == capture_epoch) {
        assert(strlen(review_resource) < 64);
        strcpy(resource, review_resource);
        return KVB_DISPLAY_REVIEW;
    }
    if (!fixture_bound || epoch != capture_epoch) {
        if (epoch == capture_epoch && pointer_resource) strcpy(resource, pointer_resource);
        return KVB_DISPLAY_NONE;
    }
    strcpy(resource, "Test display");
    return KVB_DISPLAY_SHARING;
}
void kvmbridge_allow_display(uint32_t epoch)
{
    assert(fixture_review && epoch == capture_epoch);
    allowed_epoch = epoch;
    fixture_review = false;
}
bool kvmbridge_freeze(uint32_t epoch, bool paused)
{
    (void)epoch;
    (void)paused;
    return true;
}
void kvmbridge_stop(void)
{
    stopped = true;
    fixture_bound = false;
}
void kvmbridge_stop_display(const char *profile,uint32_t epoch)
{
    assert(!strcmp(profile,requested_profile) && epoch==capture_epoch);
    kvmbridge_stop();
}
void kvmbridge_status(char text[96])
{
    strcpy(text, "Test stream");
}
kvmbridge_activity_t kvmbridge_activity(void)
{
    return (kvmbridge_activity_t){.capture_code=capture_code, .capture_text=capture_text};
}
void kvmbridge_capture_decoder_failed(uint32_t epoch)
{
    if (epoch == capture_epoch) capture_text = "KVMpad could not open the selected decoder";
}
void kvmbridge_capture_frame_timeout(uint32_t epoch)
{
    if (epoch == capture_epoch) capture_text = "Screen sharing stopped (no new frame)";
}

static uint32_t crc_rows(lv_draw_buf_t *buf, unsigned left, unsigned top, unsigned w, unsigned h)
{
    uint32_t crc = UINT32_MAX;
    for (unsigned y = 0; y < h; ++y) {
        uint8_t *p = buf->data + (top + y) * buf->header.stride + left * 2;
        for (unsigned i = 0; i < w * 2; ++i) {
            crc ^= p[i];
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0xedb88320));
        }
    }
    return ~crc;
}

static uint8_t *load_frame(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    assert(f && !fseek(f, 0, SEEK_END));
    long n = ftell(f);
    assert(n > KVM_LIVE_HEADER && n <= KVM_LIVE_MAX_PAYLOAD + KVM_LIVE_HEADER && !fseek(f, 0, SEEK_SET));
    uint8_t *p = malloc((size_t)n);
    assert(p && fread(p, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    *size = n;
    return p;
}

static lv_obj_t *find_label(lv_obj_t *obj, const char *text)
{
    if (lv_obj_check_type(obj, &lv_label_class) && !strcmp(lv_label_get_text(obj), text))
        return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(obj, (int32_t)i), text);
        if (found)
            return found;
    }
    return NULL;
}

static lv_obj_t *find_button(lv_obj_t *obj, const char *text)
{
    lv_obj_t *label = find_label(obj, text);
    return label ? lv_obj_get_parent(label) : NULL;
}

static void touch_at(int32_t x, int32_t y)
{
    lv_indev_t *pointer = lv_test_indev_get_indev(LV_INDEV_TYPE_POINTER);
    lv_test_mouse_move_to(x, y);
    lv_test_mouse_press();
    lv_indev_read(pointer);
    lv_tick_inc(30);
    lv_test_mouse_release();
    lv_indev_read(pointer);
}

static void check_touch_target(lv_obj_t *target);

static void touch_button(lv_obj_t *screen, const char *name)
{
    lv_obj_update_layout(screen);
    lv_obj_t *button = find_button(screen, name);
    if (!button || !lv_obj_check_type(button, &lv_button_class) || !lv_obj_is_visible(button)) {
        fprintf(stderr, "Touch target '%s' is missing or hidden\n", name);
        abort();
    }
    check_touch_target(button);
    lv_area_t area;
    lv_obj_get_coords(button, &area);
    touch_at((area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
}

static void next_sequence(uint8_t *msg, size_t size)
{
    // The two test fixtures and their message fields are little-endian.
    uint32_t seq;
    memcpy(&seq, msg + 12, sizeof(seq));
    ++seq;
    memcpy(msg + 12, &seq, sizeof(seq));
    uint32_t crc = esp_rom_crc32_le(esp_rom_crc32_le(0, msg, 28), msg + KVM_LIVE_HEADER,
                                    (uint32_t)(size - KVM_LIVE_HEADER));
    memcpy(msg + 28, &crc, sizeof(crc));
}

static void next_picture(uint32_t epoch, lv_obj_t *canvas, uint8_t *msg, size_t size)
{
    unsigned before = atomic_load(&shown_count);
    const void *old = lv_image_get_src(canvas);
    next_sequence(msg, size);
    assert(media_panel_stream_push(epoch, msg, size));
    for (unsigned i = 0; i < 500 && lv_image_get_src(canvas) == old; ++i) {
        lv_test_wait(16);
        vTaskDelay(2);
    }
    assert(lv_image_get_src(canvas) != old);
    lv_test_wait(48);
    assert(atomic_load(&shown_count) == before + 1);
    assert(atomic_load(&credited_epoch) == epoch && atomic_load(&credited_sequence) == frame_sequence(msg));
}

static uint16_t *partial_fb;
static void record_partial_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    unsigned stride = lv_display_get_buf_active(display)->header.stride;
    unsigned width = lv_area_get_width(area), height = lv_area_get_height(area);
    assert(stride >= width * 2 && stride * height <= 1024 * 12 * 2);
    for (unsigned y = 0; y < height; ++y)
        memcpy(partial_fb + (area->y1 + y) * 1024 + area->x1, pixels + y * stride, width * 2);
    lv_display_flush_ready(display);
}

static void check_partial_cinepak(const char *first, const char *second)
{
    lv_init();
    lv_display_t *display = lv_display_create(1024, 600);
    uint16_t *strip = malloc(1024 * 12 * 2);
    uint16_t *reference = malloc(960 * 540 * 2);
    partial_fb = calloc(1024 * 600, 2);
    assert(strip && reference && partial_fb);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, strip, NULL, 1024 * 12 * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, record_partial_flush);
    uint32_t epoch = media_panel_open(true, NULL);
    lv_obj_t *panel = lv_screen_active(), *canvas = lv_obj_get_child(panel, 0);
    lv_obj_send_event(find_button(panel, "Start sharing"), LV_EVENT_CLICKED, NULL);
    assert(media_panel_stream_start(epoch, 17, KVM_LIVE_CINEPAK_INTRA));
    fixture_bound = true;
    uint32_t last_crc = 0;
    for (uint32_t frame = 0; frame < 8; ++frame) {
        size_t size;
        uint8_t *message = load_frame(frame % 2 ? second : first, &size);
        memcpy(message + 12, &frame, sizeof(frame)); // next_picture advances and checks the wire sequence.
        next_picture(epoch, canvas, message, size);
        lv_area_t area;
        lv_obj_get_coords(canvas, &area);
        assert(area.x1 == 32 && area.y1 == 46 && area.x2 == 991 && area.y2 == 585);
        kvm_media_target_t target = {reference, 960, 540, 960, NULL, NULL};
        assert(kvm_media_decode_live(KVM_LIVE_CINEPAK_INTRA, message + 32, size - 32, &target));
        uint32_t crc = esp_rom_crc32_le(0, (uint8_t *)reference, 960 * 540 * 2);
        assert(!frame || crc != last_crc);
        last_crc = crc;
        for (unsigned pass = 0; pass <= 4; ++pass) {
            if (pass) {
                int y = 70 + (pass - 1) * 9;
                lv_area_t narrow = {37, y, 69, y + 7};
                lv_obj_invalidate_area(canvas, &narrow);
                lv_refr_now(display);
            }
            for (unsigned row = 0; row < 540; ++row)
                assert(!memcmp(partial_fb + (46 + row) * 1024 + 32, reference + row * 960, 960 * 2));
        }
        free(message);
    }
    media_panel_close();
    lv_test_wait(60);
    lv_display_delete(display);
    lv_deinit();
    free(partial_fb);
    free(reference);
    free(strip);
    puts("Cinepak viewport (32,46) stable across 8 alternating pictures and 32 clipped partial redraws.");
}

static void exercise_freshness(uint32_t epoch, lv_obj_t *panel, lv_obj_t *canvas, uint8_t *msg, size_t size)
{
    lv_obj_t *label = lv_obj_get_child(panel, 1);
    unsigned frames, rate, delay;
    assert(strstr(lv_label_get_text(label), "Test display") && strstr(lv_label_get_text(label), "RX to LCD"));
    for (unsigned i = 0; i < 6; ++i) {
        simulated_time_offset += 200000;
        next_picture(epoch, canvas, msg, size);
    }
    assert(strstr(lv_label_get_text(label), " fps"));
    assert(media_panel_live_stats(&frames, &rate, &delay) && rate > 0);
    unsigned last_count = frames;
    simulated_time_offset += 1600000;
    lv_test_wait(48);
    assert(strstr(lv_label_get_text(label), "Last frame stale") && lv_image_get_src(canvas));
    assert(media_panel_live_stats(&frames, &rate, &delay) && frames == last_count && rate == 0);
    next_picture(epoch, canvas, msg, size);
    assert(strstr(lv_label_get_text(label), "RX to LCD") && !strstr(lv_label_get_text(label), " fps"));
    assert(media_panel_live_stats(&frames, &rate, &delay) && frames == last_count + 1 && rate == 0);
    for (unsigned i = 0; i < 6; ++i) {
        simulated_time_offset += 200000;
        next_picture(epoch, canvas, msg, size);
    }
    assert(strstr(lv_label_get_text(label), " fps"));
    touch_button(panel, "Freeze");
    const void *held = lv_image_get_src(canvas);
    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    lv_draw_buf_t *snapshot = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
    assert(snapshot);
    uint32_t held_crc = crc_rows(snapshot, area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area));
    lv_draw_buf_destroy(snapshot);
    assert(media_panel_live_stats(&last_count, &rate, &delay) && rate == 0);
    simulated_time_offset += 8000000;
    lv_test_wait(48);
    assert(held && lv_image_get_src(canvas) == held && strstr(lv_label_get_text(label), "Frozen"));
    assert(media_panel_live_stats(&frames, &rate, &delay) && frames == last_count && rate == 0);
    snapshot = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
    assert(snapshot && crc_rows(snapshot, area.x1, area.y1, lv_area_get_width(&area), lv_area_get_height(&area)) == held_crc);
    lv_draw_buf_destroy(snapshot);
    next_sequence(msg, size);
    assert(!media_panel_stream_push(epoch, msg, size));
    touch_button(panel, "Resume");
    lv_test_wait(48);
    assert(strstr(lv_label_get_text(label), "Waiting for a frame"));
    next_picture(epoch, canvas, msg, size);
    assert(strstr(lv_label_get_text(label), "RX to LCD") && !strstr(lv_label_get_text(label), " fps"));
    // Valid framing with invalid image data must never keep the last picture visible.
    memset(msg + KVM_LIVE_HEADER, 0, size - KVM_LIVE_HEADER);
    stopped = false;
    for (unsigned i = 0; i < 6; ++i) {
        next_sequence(msg, size);
        unsigned dropped = atomic_load(&dropped_count);
        assert(media_panel_stream_push(epoch, msg, size));
        for (unsigned wait = 0; wait < 500 && atomic_load(&dropped_count) == dropped; ++wait)
            vTaskDelay(2);
        assert(atomic_load(&dropped_count) == dropped + 1 && atomic_load(&credited_epoch) == epoch &&
               atomic_load(&credited_sequence) == frame_sequence(msg));
        simulated_time_offset += 1000000;
        lv_test_wait(48);
        vTaskDelay(5);
    }
    assert(strstr(lv_label_get_text(label), "no new frame") && stopped);
    assert(media_panel_live_stats(&frames, &rate, &delay) && rate == 0);
    assert(!media_panel_stream_push(epoch, msg, size));
    puts("Live display: measured fps and RX to LCD, stale, recovery, freeze, resume, and expired invalid stream passed.");
}

static void exercise_profile_modes(void)
{
    const char *const cases[] = {"OVERVIEW", "PIXEL_1X", "ZOOM_2X"};
    lv_obj_t *home = lv_screen_active();
    assert(!media_panel_open_magnifier("net.example.scope", "unknown", NULL) && lv_screen_active() == home);
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        capture_epoch = 0;
        char profile[] = "net.example.magnifier";
        uint32_t epoch = media_panel_open_magnifier(profile, cases[i], NULL);
        profile[0] = 'x';  // The screen owns the identity until the local Start action.
        lv_obj_t *panel = lv_screen_active();
        assert(epoch && panel != home && !capture_epoch);
        assert(!find_button(panel, "Data live"));
        media_panel_telemetry_status("net.example.other", "Data live");
        assert(!find_button(panel, "Data live"));
        if (i == 1) {
            const char *const states[] = {"Data live", "Data stale", "Data partial", "Data error", "Data unavailable"};
            uint32_t before = lv_obj_get_child_count(panel);
            for (unsigned j = 0; j < sizeof(states) / sizeof(states[0]); ++j) {
                media_panel_telemetry_status("net.example.magnifier", states[j]);
                assert(find_button(panel, states[j]) == panel);
                assert(lv_obj_get_child_count(panel) == before + 1);
            }
            lv_obj_update_layout(panel);
            lv_area_t data, caption;
            lv_obj_get_coords(lv_obj_get_child(panel, -1), &data);
            lv_obj_get_coords(lv_obj_get_child(panel, 1), &caption);
            assert(caption.y2 < data.y1 && data.y2 < 46 && data.x2 < 566);
        } else {
            assert(!find_button(panel, "Data unavailable"));
        }
        lv_obj_send_event(find_button(panel, "Start sharing"), LV_EVENT_CLICKED, NULL);
        assert(capture_epoch == epoch && !strcmp(requested_mode, cases[i]) && requested_codec == KVM_LIVE_JPEG);
        assert(!strcmp(requested_profile,"net.example.magnifier"));
        // No incoming test pixels. The first-frame deadline starts after the data channel binds.
        simulated_time_offset += 8000000;
        lv_test_wait(48);
        assert(!strcmp(lv_label_get_text(lv_obj_get_child(panel, 1)), "Test stream"));
        fixture_bound = true;
        stopped = false;
        lv_test_wait(48);
        assert(strstr(lv_label_get_text(lv_obj_get_child(panel, 1)), "Waiting for a frame"));
        simulated_time_offset += 6000000;
        lv_test_wait(48);
        assert(stopped && find_button(panel, "Screen sharing stopped (no new frame)"));
        media_panel_close();
        assert(lv_screen_active() == home);
    }
    puts("Three profile crop modes reached the request; open alone made no request, and a missing first picture expired.");
}

static uint32_t retry_after_display_loss(lv_obj_t *panel, uint32_t old, uint8_t *message, size_t size, const char *reason)
{
    lv_obj_t *start = find_button(panel, "Start sharing");
    assert(transport_ended && !lv_obj_is_visible(start));
    capture_text = reason;
    fixture_bound = false;
    closing_fixture = old;
    transport_ended(old);
    assert(!media_panel_stream_push(old, message, size));
    pthread_join(worker, NULL);
    lv_test_wait(48);
    assert(find_button(panel, reason) && !lv_obj_is_visible(start));
    assert(!lv_obj_get_child_by_type(panel, 0, &lv_image_class));
    lv_draw_buf_t *blank = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
    assert(blank);
    for (unsigned y = 60; y < 572; ++y) {
        const uint16_t *row = (const uint16_t *)(blank->data + y * blank->header.stride);
        for (unsigned x = 24; x < 536; ++x) assert(row[x] == 0);
    }
    lv_draw_buf_destroy(blank);
    closing_fixture = 0;
    lv_test_wait(64);
    assert(capture_epoch == old && lv_obj_is_visible(start) && find_button(panel, reason));
    lv_obj_t *monitor = find_button(panel, "Monitor");
    assert(!monitor || !lv_obj_is_visible(monitor));
    touch_button(panel, "Start sharing");
    uint32_t fresh = capture_epoch;
    assert(fresh && fresh != old && requested_codec == KVM_LIVE_H264_INTRA && !strcmp(requested_mode, "ZOOM_2X"));
    fixture_review = true;
    lv_test_wait(48);
    assert(find_button(panel, "Allow capture of Test display 2?") && allowed_epoch != fresh);
    touch_button(panel, "Allow display");
    assert(allowed_epoch == fresh && media_panel_stream_start(fresh, 17, requested_codec));
    fixture_bound = true;
    next_sequence(message, size);
    assert(!media_panel_stream_push(old, message, size));
    transport_ended(old);
    next_picture(fresh, lv_obj_get_child(panel, 0), message, size);
    printf("%s: cleared all 512x512 pixels, retained H.264 / 2x, required fresh approval and discarded old data.\n", reason);
    return fresh;
}

static void exercise(const char *path, kvm_live_codec_t codec, unsigned w, unsigned h, unsigned x, unsigned y,
                     uint32_t expected)
{
    size_t size = 0;
    uint8_t *message = load_frame(path, &size);
    lv_obj_t *home = lv_screen_active();
    lv_draw_buf_t *before = lv_snapshot_take(home, LV_COLOR_FORMAT_RGB565);
    assert(before);
    uint32_t home_crc = crc_rows(before, 0, 0, 1024, 600);
    lv_draw_buf_destroy(before);
    uint32_t epoch = media_panel_open(true, NULL);
    assert(epoch && !media_panel_open(true, NULL));
    lv_test_indev_create_all();
    // Physical Start hides configuration controls in the same path as on the panel.
    lv_obj_t *panel = lv_screen_active();
    lv_obj_t *start = find_button(panel, "Start sharing");
    assert(start);
    touch_button(panel, "Start sharing");
    simulated_time_offset += 8000000;
    lv_test_wait(48);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(panel, 1)), "Test stream"));
    assert(media_panel_stream_start(epoch, 17, codec));
    fixture_bound = true;
    kvmbridge_crop_t crop = {.cursor_x=-1150, .cursor_y=47, .left=-1160, .top=19,
                             .width=400, .height=384, .pad_x=56, .pad_y=64, .clamped=true};
    unsigned before_frame = atomic_load(&shown_count);
    assert(received_picture && received_picture(epoch, message, size, &crop));
    vTaskDelay(30);  // Decode may finish while the LVGL thread has not presented the image.
    assert(atomic_load(&shown_count) == before_frame);
    crop.cursor_x = 700;  // The receiver borrows the sidecar only for the callback.
    lv_obj_t *canvas = lv_obj_get_child(panel, 0);
    for (int i = 0; i < 500; ++i) {
        lv_test_wait(16);
        const lv_image_dsc_t *image = lv_image_get_src(canvas);
        if (image && image->data)
            break;
        vTaskDelay(2);
    }
    lv_test_wait(32);
    assert(atomic_load(&shown_count) == before_frame + 1 && atomic_load(&credited_epoch) == epoch &&
           atomic_load(&credited_sequence) == frame_sequence(message));
    lv_test_wait(64);
    assert(atomic_load(&shown_count) == before_frame + 1);
    lv_draw_buf_t *snap = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
    assert(snap && snap->header.w == 1024 && snap->header.h == 600);
    uint32_t got = crc_rows(snap, x, y, w, h);
    lv_area_t area;
    lv_obj_get_coords(canvas, &area);
    fprintf(stderr, "codec %d canvas (%d,%d)..(%d,%d), snapshot RGB565 at (%u,%u) %08x, expected %08x\n", codec,
            (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2, x, y, got, expected);
    assert(got == expected);
    lv_draw_buf_destroy(snap);
    const char *geometry = "Pixel 1:1\nCursor (-1150, 47)\nSource (-1160, 19)  400 x 384 px\nPadding left 56, top 64 px";
    assert(strstr(lv_label_get_text(lv_obj_get_child(panel, 1)), "Edge clamped"));
    if (codec == KVM_LIVE_H264_INTRA) {
        assert(find_button(panel, geometry));
        assert(!received_picture(epoch, message, size, &crop));  // A replay cannot replace the displayed geometry.
        lv_test_wait(48);
        assert(find_button(panel, geometry));
        uint32_t old = epoch;
        touch_button(panel, "Monitor");
        closing_fixture = old;
        lv_test_wait(48);
        assert(find_button(panel, "Closing the previous display"));
        assert(!media_panel_stream_push(old, message, size));
        pthread_join(worker, NULL);
        lv_test_wait(48);
        lv_draw_buf_t *switching = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
        assert(switching && crc_rows(switching, x, y, w, h) != expected);
        lv_draw_buf_destroy(switching);
        closing_fixture = 0;
        lv_test_wait(48);
        assert(capture_epoch == old && find_button(panel, "Select Start sharing to choose a display"));
        lv_obj_t *settings = lv_obj_get_parent(start);
        lv_dropdown_set_selected(lv_obj_get_child_by_type(settings, 0, &lv_dropdown_class), 2);
        lv_dropdown_set_selected(lv_obj_get_child_by_type(settings, 1, &lv_dropdown_class), 1);
        touch_button(panel, "Start sharing");
        epoch = capture_epoch;
        assert(epoch && epoch != old && media_panel_stream_start(epoch, 17, codec));
        fixture_bound = true;
        canvas = lv_obj_get_child(panel, 0);
        next_picture(epoch, canvas, message, size);
        media_panel_stream_end(old);
        lv_test_wait(48);
        assert(!strstr(lv_label_get_text(lv_obj_get_child(panel, 1)), "Edge clamped"));
        assert(!find_button(panel, geometry));
        puts("Crop metadata survived callback return, matched the displayed picture and cleared for a legacy peer.");
        lv_draw_buf_t *resumed = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
        assert(resumed && crc_rows(resumed, x, y, w, h) == expected);
        lv_draw_buf_destroy(resumed);
        puts("Monitor cleared the old display, waited for teardown and required a fresh Start with a new epoch.");
        epoch = retry_after_display_loss(panel, epoch, message, size, "Pointer left the selected display. Tap Start sharing to retry.");
        epoch = retry_after_display_loss(panel, epoch, message, size, "Cannot map this display. Select the monitor again");
        epoch = retry_after_display_loss(panel, epoch, message, size, "Windows could not locate the pointer. Try again");
        canvas = lv_obj_get_child(panel, 0);
        exercise_freshness(epoch, panel, canvas, message, size);
        lv_test_wait(32);
        lv_draw_buf_t *cleared = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
        assert(cleared && crc_rows(cleared, x, y, w, h) != expected);
        capture_text = "Could not open or bind the HOST image channel";
        lv_test_wait(32);
        assert(find_button(panel, capture_text));
        lv_draw_buf_destroy(cleared);
    } else {
        exercise_freshness(epoch, panel, canvas, message, size);
        lv_draw_buf_t *blank = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
        assert(blank && crc_rows(blank, x, y, w, h) != expected);
        lv_draw_buf_destroy(blank);
    }
    // Physical Stop clears the surface while the producer is still alive.
    stopped = false;
    lv_obj_send_event(find_button(panel, "Stop"), LV_EVENT_CLICKED, NULL);
    assert(stopped && lv_screen_active() == home && !media_panel_stream_push(epoch, message, size));
    lv_draw_buf_t *after = lv_snapshot_take(home, LV_COLOR_FORMAT_RGB565);
    assert(after && crc_rows(after, 0, 0, 1024, 600) == home_crc);
    lv_draw_buf_destroy(after);
    pthread_join(worker, NULL);
    lv_test_wait(32);
    uint32_t later = media_panel_open(false, NULL);
    assert(later && later != epoch);
    media_panel_stream_end(epoch);
    lv_test_wait(32);
    assert(lv_screen_active() != home);
    media_panel_close();
    assert(lv_screen_active() == home);
    lv_test_indev_delete_all();
    free(message);
}

static bool present(void)
{
    return atomic_load(&available);
}

static void assert_no_stats(uint32_t token)
{
    media_panel_file_stats_t stats = {.epoch = 123, .frames = 42, .window_us = 567, .media_window_us = 42,
                                     .width = 7, .height = 9};
    assert(!media_panel_file_stats(token, &stats));
    assert(!stats.epoch && !stats.frames && !stats.window_us && !stats.media_window_us && !stats.width && !stats.height);
}

typedef struct {
    uint32_t token, count, written, width, height;
    atomic_bool done, hold_second, waiting;
    bool dark_final, single_surface;
    const uint64_t *pts;
    kvm_play_result_t result;
} rgb_clip_t;

static bool rgb_read(void *ctx, uint16_t *pixels, const uint16_t *previous, uint64_t *pts,
                     uint32_t *duration_us, uint32_t *crc, bool (*cancel)(void *), void *cancel_ctx)
{
    rgb_clip_t *clip = ctx;
    assert((previous != NULL) == (clip->written != 0));
    if (cancel(cancel_ctx))
        return false;
    *pts = clip->pts ? clip->pts[clip->written] : 50000u * clip->written;
    *duration_us = clip->count == 1 ? 200000 : 50000;
    unsigned words = clip->width * clip->height;
    if (previous) {
        assert(previous[words - 1] == (uint16_t)(clip->written * 16 + words - 1));
        assert((previous == pixels) == clip->single_surface);
    }
    if (clip->written == 1 && atomic_load(&clip->hold_second)) {
        atomic_store(&clip->waiting, true);
        while (atomic_load(&clip->hold_second) && !cancel(cancel_ctx))
            vTaskDelay(1);
    }
    for (unsigned i = 0; i < words; ++i)
        pixels[i] = clip->dark_final && clip->written ? 0 : (uint16_t)((clip->written + 1) * 16 + i);
    *crc = esp_rom_crc32_le(0, (const uint8_t *)pixels, words * sizeof(*pixels));
    ++clip->written;
    return true;
}

static void *play_rgb_clip(void *ctx)
{
    rgb_clip_t *clip = ctx;
    clip->result = media_panel_play_rgb(clip->token, clip->width, clip->height, clip->count, rgb_read, clip, NULL);
    atomic_store(&clip->done, true);
    return NULL;
}

// DOUBLE_PARTIAL submits the completed LVGL refresh to the alternating full-size FB.
static void swap_test_fb(lv_event_t *event)
{
    lv_display_t *display = lv_event_get_target(event);
    if (!lv_display_flush_is_last(display))
        return;
    test_front ^= 1;
    memcpy(test_fb[test_front], lv_display_get_buf_active(display)->data, 1024 * 600 * 2);
}

static void check_blackout(unsigned black_flushes, bool dark_final, ui_control_palette_t palette)
{
    lv_display_t *display = lv_display_get_default();
    lv_obj_t *home = lv_screen_active();
    ui_theme_set_control_palette(palette);
    test_fb[0] = calloc(1024 * 600, 2);
    test_fb[1] = calloc(1024 * 600, 2);
    assert(test_fb[0] && test_fb[1]);
    lv_display_add_event_cb(display, swap_test_fb, LV_EVENT_FLUSH_FINISH, NULL);
    rgb_clip_t clip = {.token = media_panel_open(false, NULL), .count = 2, .width = 32, .height = 32,
                       .dark_final = dark_final};
    assert(clip.token);
    lv_obj_t *panel = lv_screen_active();
    assert(find_button(panel, "Opening video") == panel);
    lv_obj_update_layout(panel);
    const char *captions[] = {"Stop", "Home"};
    lv_style_value_t surface;
    assert(lv_style_get_prop(ui_theme_style(UI_STYLE_CONTROL_BUTTON), LV_STYLE_BG_COLOR, &surface) == LV_STYLE_RES_FOUND);
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *control = find_button(panel, captions[i]);
        assert(control && lv_color_eq(lv_obj_get_style_bg_color(control, 0), surface.color));
        assert(lv_obj_get_style_text_font(lv_obj_get_child(control, 0), 0) == ui_theme_font(UI_FONT_HEADING));
        lv_area_t a;
        lv_obj_get_coords(control, &a);
        assert(lv_area_get_height(&a) >= 44 && a.y2 < 46);
        lv_point_t bottom = {.x = (a.x1 + a.x2) / 2, .y = a.y2};
        assert(lv_indev_search_obj(panel, &bottom) == control);
    }
    atomic_store(&clip.hold_second, true);
    pthread_t player;
    assert(!pthread_create(&player, NULL, play_rgb_clip, &clip));
    for (unsigned i = 0; i < 1000 && !atomic_load(&clip.waiting); ++i) {
        lv_test_wait(16);
        vTaskDelay(1);
    }
    assert(atomic_load(&clip.waiting));
    lv_refr_now(display);
    lv_test_wait(32);
    assert(find_button(panel, "Playing video") == panel);
    if (black_flushes) {
        // A genuine LVGL repaint with no new movie picture. One flush leaves the
        // previous bright picture in the back buffer; the second blackens both.
        lv_obj_t *cover = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(cover);
        lv_obj_set_pos(cover, (1024 - 32) / 2, 32 + (568 - 32) / 2);
        lv_obj_set_size(cover, 32, 32);
        lv_obj_set_style_bg_color(cover, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
        for (unsigned i = 0; i < black_flushes; ++i) {
            lv_obj_invalidate(cover);
            lv_refr_now(display);
        }
        simulated_time_offset += 20000;
        lv_obj_delete(cover);
        lv_refr_now(display);
    }
    atomic_store(&clip.hold_second, false);
    for (unsigned i = 0; i < 1000 && !atomic_load(&clip.done); ++i) {
        lv_test_wait(16);
        vTaskDelay(1);
    }
    assert(atomic_load(&clip.done));
    pthread_join(player, NULL);
    media_blackout_stats_t s;
    assert(clip.result == KVM_PLAY_OK && media_panel_blackout_stats(clip.token, &s));
    assert(s.samples && s.pairs == (dark_final ? 0 : 1));
    assert(s.confirmed == (!dark_final && black_flushes == 2));
    assert(s.inferred == (!dark_final && black_flushes == 1));
    assert((s.longest_us >= 20000) == (!dark_final && black_flushes != 0));
    lv_test_wait(32);
    assert(find_button(panel, "Playback complete.") == panel);
    lv_obj_send_event(find_button(panel, captions[black_flushes % 2]), LV_EVENT_CLICKED, NULL);
    assert(lv_screen_active() == home);
    lv_display_remove_event_cb_with_user_data(display, swap_test_fb, NULL);
    free(test_fb[0]);
    free(test_fb[1]);
    test_fb[0] = test_fb[1] = NULL;
}

static void check_file_stats(void)
{
    assert(ui_theme_init());
    check_blackout(0, false, UI_PALETTE_DAY);
    check_blackout(1, false, UI_PALETTE_NIGHT);
    check_blackout(2, false, UI_PALETTE_HIGH_CONTRAST);
    check_blackout(2, true, UI_PALETTE_DAY);
    ui_theme_deinit();
    char summary[320];
    const char *const mode_names[] = {"Exact", "Reduced", "Large bank", "Temporal blend", "Smart loose"};
    for (unsigned mode = 0; mode < sizeof(mode_names) / sizeof(mode_names[0]); ++mode) {
        char expected[96];
        snprintf(expected, sizeof(expected), "Clip 100%%.mp4\n%s", mode_names[mode]);
        media_panel_file_summary(summary, sizeof(summary), "Clip 100%.mp4", (kvm_h264_reconstruction_t)mode, NULL);
        assert(!strcmp(expected, summary));
    }
    media_panel_file_stats_t measured = {.frames = 3, .window_us = 80000, .width = 480, .height = 270};
    media_panel_file_summary(summary, sizeof(summary), "test.mp4", KVM_H264_RR_BLEND, &measured);
    assert(!strcmp(summary, "test.mp4\nTemporal blend   |   Displayed at 480 x 270\n"
                            "3 frames displayed in 0.080 s\nLCD 25.00 fps"));
    measured.media_window_us = 400000;
    media_panel_file_summary(summary, sizeof(summary), "test.mp4", KVM_H264_EXACT, &measured);
    assert(strstr(summary, "LCD 25.00 fps   |   Source 5.00 fps"));
    uint32_t prior = 0;
    const unsigned counts[] = {3, 3, 200, 1, 3, 200, 1};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        unsigned count = counts[i];
        rgb_clip_t clip = {.token = media_panel_open(false, NULL), .count = count,
                           .width = i < 4 ? 8 : 960, .height = i < 4 ? 8 : 540};
        const uint64_t duplicated[] = {0, 0, 50000}, variable[] = {10000, 40000, 90000};
        if (i == 1) clip.pts = duplicated;
        if (i == 4) {
            clip.pts = variable;
            clip.single_surface = true;
            test_surfaces_until_failure = 2;
        }
        assert(clip.token && clip.token != prior);
        assert_no_stats(prior);
        assert_no_stats(clip.token);
        unsigned reads_before = scanout_reads;
        // The second session explicitly receives the first session's expired token.
        media_panel_time_without_scanout_crc(i == 0 ? clip.token : prior);
        pthread_t player;
        bool interrupted = false;
        bool stop_final = count == 1 && i == sizeof(counts) / sizeof(counts[0]) - 1;
        if (count == 1 && !stop_final)
            simulated_time_offset += (uint32_t)(UINT32_MAX - 100000u - (uint32_t)esp_timer_get_time());
        int64_t start = esp_timer_get_time();
        int64_t first_visible = 0;
        assert(!pthread_create(&player, NULL, play_rgb_clip, &clip));
        for (unsigned t = 0; t < 2000 && !atomic_load(&clip.done); ++t) {
            int64_t before_refresh = esp_timer_get_time();
            lv_test_wait(16);
            if (!first_visible && lv_image_get_src(lv_obj_get_child(lv_screen_active(), 0)))
                first_visible = before_refresh;
            if ((count == 200 || stop_final) && first_visible) {
                assert_no_stats(clip.token);
                lv_obj_send_event(find_button(lv_screen_active(), "Stop"), LV_EVENT_CLICKED, NULL);
                interrupted = true;
                break;
            }
            vTaskDelay(2);
        }
        assert(interrupted || atomic_load(&clip.done));
        pthread_join(player, NULL);
        if (count == 200 || stop_final) {
            assert(clip.result == KVM_PLAY_STOPPED && (stop_final ? clip.written == count : clip.written < count));
            assert_no_stats(clip.token);
            lv_test_wait(32);
            prior = clip.token;
            continue;
        }
        assert(clip.result == KVM_PLAY_OK && clip.written == count);
        if (i == 0)
            assert(scanout_reads == reads_before);
        else
            assert(scanout_reads >= reads_before + count);
        media_panel_file_stats_t stats;
        assert(media_panel_file_stats(clip.token, &stats));
        assert(stats.epoch == clip.token && stats.frames == count);
        if (clip.single_surface) {
            media_blackout_stats_t seen;
            assert(media_panel_blackout_stats(clip.token, &seen) && seen.pairs == count - 1);
            assert(!seen.confirmed && !seen.inferred && !seen.dark_at_picture);
        }
        assert(stats.width == clip.width && stats.height == clip.height);
        media_panel_file_summary(summary, sizeof(summary), "local.mp4", KVM_H264_EXACT, &stats);
        assert(strstr(summary, clip.width == 8 ? "Displayed at 8 x 8\n" : "Displayed at 960 x 540\n"));
        if (count == 1) {
            int64_t end = esp_timer_get_time();
            assert(first_visible && end - first_visible >= 200000);
            assert((uint32_t)end < (uint32_t)first_visible);
            assert(strstr(summary, "1 frame displayed   |   fps unavailable"));
        }
        assert((stats.window_us != 0) == (count > 1));
        assert(stats.media_window_us == (count < 2 || i == 1 ? 0 : i == 4 ? 80000 : 100000));
        if (count < 2 || i == 1)
            assert(!strstr(summary, "Source"));
        else
            assert(strstr(summary, i == 4 ? "Source 25.00 fps" : "Source 20.00 fps"));
        assert(stats.window_us <= (uint64_t)(esp_timer_get_time() - start));
        // UI-only repaint and idle refreshes never add presented pictures.
        lv_obj_invalidate(lv_screen_active());
        lv_test_wait(64);
        media_panel_file_stats_t after;
        assert(media_panel_file_stats(clip.token, &after));
        assert(after.frames == stats.frames && after.window_us == stats.window_us);
        lv_obj_send_event(find_button(lv_screen_active(), "Stop"), LV_EVENT_CLICKED, NULL);
        assert_no_stats(clip.token);
        prior = clip.token;
    }
    puts("File stats count only completed picture refreshes and clear on Stop or a new session.");
}

static void *play_file(void *ctx)
{
    played = media_panel_play((uint32_t)(uintptr_t)ctx, sample, true, card_mode, present);
    return NULL;
}

typedef struct {
    unsigned frames;
    int64_t final_duration_us, first_pts, last_pts;
    bool invalid_pts;
} native_clip_t;

static bool count_picture(void *ctx, const kvm_media_yuv_t *frame)
{
    native_clip_t *clip = ctx;
    assert(frame->plane[0]);
    if (frame->pts_us == INT64_MIN || (clip->frames && frame->pts_us <= clip->last_pts))
        clip->invalid_pts = true;
    if (!clip->frames)
        clip->first_pts = frame->pts_us;
    clip->last_pts = frame->pts_us;
    ++clip->frames;
    clip->final_duration_us = frame->duration_us > 0 && frame->duration_us <= 1000000 ? frame->duration_us : 41667;
    return true;
}

static void check_native_file_stats(const char *path)
{
    native_clip_t expected = {0};
    sample = fopen(path, "rb");
    assert(sample);
    kvm_media_playback_t sink = {.ctx = &expected, .frame = count_picture, .reconstruction = card_mode};
    assert(kvm_media_play_h264(sample, true, &sink) && expected.frames);
    uint32_t token = media_panel_open(false, NULL);
    assert(token);
    atomic_store(&available, true);
    pthread_t player;
    assert(!pthread_create(&player, NULL, play_file, (void *)(uintptr_t)token));
    media_panel_file_stats_t stats;
    int64_t first_visible = 0;
    for (unsigned i = 0; i < 10000 && !media_panel_file_stats(token, &stats); ++i) {
        int64_t before_refresh = esp_timer_get_time();
        lv_test_wait(16);
        if (!first_visible && lv_image_get_src(lv_obj_get_child(lv_screen_active(), 0)))
            first_visible = before_refresh;
        vTaskDelay(2);
    }
    assert(media_panel_file_stats(token, &stats));
    pthread_join(player, NULL);
    assert(played && stats.epoch == token && stats.frames == expected.frames);
    uint64_t span = expected.invalid_pts ? 0 : (uint64_t)expected.last_pts - (uint64_t)expected.first_pts;
    assert(stats.media_window_us == span);
    char summary[320];
    media_panel_file_summary(summary, sizeof(summary), path, card_mode, &stats);
    puts(summary);
    int64_t final_hold = esp_timer_get_time() - first_visible - (int64_t)stats.window_us;
    assert(first_visible && final_hold >= expected.final_duration_us);
    const lv_image_dsc_t *picture = lv_image_get_src(lv_obj_get_child(lv_screen_active(), 0));
    assert(picture && stats.width == picture->header.w && stats.height == picture->header.h);
    assert((stats.window_us > 0) == (expected.frames > 1));
    media_panel_close();
    assert_no_stats(token);
    fclose(sample);
    printf("Direct H.264 file presented %u frames, final duration %lld us, observed %lld us.\n",
           expected.frames, (long long)expected.final_duration_us, (long long)final_hold);
}

static void check_card_loss(const char *path)
{
    lv_obj_t *home = lv_screen_active();
    uint32_t token = media_panel_open(false, NULL);
    assert(token);
    lv_obj_t *canvas = lv_obj_get_child(lv_screen_active(), 0);
    sample = fopen(path, "rb");
    assert(sample);
    atomic_store(&available, true);
    pthread_t player;
    assert(!pthread_create(&player, NULL, play_file, (void *)(uintptr_t)token));
    for (int i = 0; i < 1000; ++i) {
        lv_test_wait(16);
        const lv_image_dsc_t *frame = lv_image_get_src(canvas);
        if (frame && frame->data)
            break;
        vTaskDelay(2);
    }
    assert(lv_image_get_src(canvas));
    atomic_store(&available, false);
    for (int i = 0; i < 1000 && lv_screen_active() != home; ++i) {
        lv_test_wait(16);
        vTaskDelay(2);
    }
    assert(lv_screen_active() == home);
    pthread_join(player, NULL);
    assert(!played);
    assert_no_stats(token);
    fclose(sample);
    lv_test_wait(32);
    puts("Card removal cancelled the file decoder and restored Home");
}

static void finish_synthetic(void)
{
    synthetic_done = true;
}

static void check_file_refusal(const char *path, bool refuse_surface)
{
    lv_obj_t *home = lv_screen_active();
    unsigned epoch = media_panel_open(false, NULL);
    assert(epoch);
    FILE *f = fopen(path, "rb");
    assert(f);
    // The fixture is a 640x360 source shown at 320x180. In the second
    // scenario both surfaces succeed, then the real decoder sees the exact
    // 131072-byte largest block measured after the unit-1 geometry probe.
    if (refuse_surface)
        test_refuse_size = 320 * 180 * 2;
    else
        test_shrink_size = 320 * 180 * 2;
    kvm_play_result_t result = media_panel_play_result(epoch, f, true, KVM_H264_EXACT, NULL);
    assert(result == KVM_PLAY_NO_MEMORY);
    assert_no_stats(epoch);
    lv_test_wait(32);
    assert(find_button(lv_screen_active(), "Not enough memory to play this video."));
    char summary[320];
    media_panel_file_summary(summary, sizeof(summary), "selected.mp4", KVM_H264_EXACT, NULL);
    assert(!strcmp(summary, "selected.mp4\nExact"));
    assert(strcmp(media_panel_play_message(KVM_PLAY_UNAVAILABLE), media_panel_play_message(result)));
    fclose(f);
    media_panel_close();
    assert(lv_screen_active() == home);
    test_largest_psram = 8u * 1024 * 1024;
    test_refuse_size = test_shrink_size = 0;
}

static void exercise_wire_lifetime(void)
{
    uint32_t epoch = media_panel_open(true, NULL);
    assert(epoch);
    lv_obj_t *start = find_button(lv_screen_active(), "Start sharing");
    assert(start);
    lv_obj_send_event(start, LV_EVENT_CLICKED, NULL);
    uint8_t *wire = receive_buffer.start(epoch, 17, KVM_LIVE_H264_INTRA);
    assert(wire);
    while (!receive_buffer.writable(epoch)) vTaskDelay(1);
    media_panel_close(); // A read on the transport thread may still own this pointer.
    pthread_join(worker, NULL);
    lv_test_wait(48);
    transport_ended(epoch + 1);
    assert(!receive_buffer.writable(epoch) && !media_panel_open(true, NULL));
    memset(wire, 0xab, KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD);
    transport_ended(epoch);
    lv_test_wait(48);
    assert(media_panel_open(true, NULL));
    media_panel_close();
    puts("Paced Stop held the full wire buffer until transport end and rejected a stale end");
}

static void exercise_diagnostic(bool scan, bool h264, bool paced)
{
    simulated_scan = scan;
    synthetic_done = stopped = false;
    lv_obj_t *home = lv_screen_active();
    bool ok = paced ? media_panel_synthetic_h264_paced(5, 4, finish_synthetic)
                 : h264 ? media_panel_synthetic_h264_start(5, 4, finish_synthetic)
                   : media_panel_synthetic_start(5, 4, finish_synthetic);
    assert(ok);
    for (unsigned i = 0; i < 1000 && !synthetic_done; ++i) {
        simulated_time_offset += 16000;
        lv_test_wait(16);
        vTaskDelay(2);
    }
    assert(synthetic_done && lv_screen_active() == home && !stopped);
    pthread_join(worker, NULL);
    lv_test_wait(32);
}

#ifdef KVM_MEDIA_PAGED_GUEST
static atomic_bool paged_finished;
static kvm_play_result_t paged_result;

static void *paged_player(void *arg)
{
    FILE *scratch = tmpfile();
    assert(scratch);
    paged_result = media_panel_play_paged((uint32_t)(uintptr_t)arg, sample, scratch, KVM_H264_LARGE_BANK, present);
    assert(!fclose(scratch));
    atomic_store(&paged_finished, true);
    return NULL;
}

static void check_paged(const char *path)
{
    // Generated independently by tools/live_view/card_pixels.c through native
    // FFmpeg and the same 65,536-byte bank, with the supplied source SHA-256.
    const uint32_t expected[] = {UINT32_C(0xdaf7ad39), UINT32_C(0x33eebc0e)};
    lv_obj_t *home = lv_screen_active();
    unsigned token = media_panel_open(false, NULL);
    assert(token);
    sample = fopen(path, "rb");
    assert(sample);
    atomic_store(&available, true);
    pthread_t player;
    assert(!pthread_create(&player, NULL, paged_player, (void *)(uintptr_t)token));
    unsigned verified = 0;
    int64_t deadline = esp_timer_get_time() + INT64_C(300000000);
    while (verified < 2 && !atomic_load(&paged_finished) && esp_timer_get_time() < deadline) {
        lv_test_wait(16);
        lv_obj_t *canvas = lv_obj_get_child(lv_screen_active(), 0);
        if (lv_image_get_src(canvas)) {
            lv_draw_buf_t *fb = lv_display_get_buf_active(lv_display_get_default());
            if (crc_rows(fb, 32, 46, 960, 540) == expected[verified])
                verified++;
        }
        vTaskDelay(4);
    }
    lv_test_wait(32);
    vTaskDelay(10);
    lv_obj_send_event(find_button(lv_screen_active(), "Stop"), LV_EVENT_CLICKED, NULL);
    pthread_join(player, NULL);
    assert(verified == 2 && paged_result == KVM_PLAY_STOPPED && lv_screen_active() == home);
    lv_test_wait(32);
    fclose(sample);
    puts("Paged trained decoder rendered both original RGB565 frames and physical Stop retired its surface.");
}
#endif
static void check_pixel_pattern(const char *path)
{
    lv_init();
    lv_display_t *d = lv_test_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_obj_t *home = lv_screen_active();
    simulated_scan = true;
    assert(!media_panel_pixel_pattern_scan());
    test_refuse_size = 512u * 512u * 2u;
    assert(!media_panel_pixel_pattern(finish_synthetic) && lv_screen_active() == home && !stopped);
    test_refuse_size = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        synthetic_done = false;
        simulated_scan = pass != 0;
        assert(media_panel_pixel_pattern(finish_synthetic));
        assert(!media_panel_pixel_pattern_scan() && !media_panel_pixel_pattern(finish_synthetic));
        lv_test_wait(64);
        if (!pass) {
            assert(!media_panel_pixel_pattern_scan());
            simulated_scan = true;
        }
        simulated_time_offset += 64000;
        assert(media_panel_pixel_pattern_scan());
        lv_obj_t *panel = lv_screen_active();
        lv_obj_t *exit = find_button(panel, pass ? "Home" : "Stop");
        assert(exit && lv_obj_is_visible(exit) && find_button(panel, "CAPTURE OFF   |   Test pattern"));
        assert(!lv_obj_is_visible(find_button(panel, "Start sharing")) && !capture_epoch);
        if (!pass) {
            lv_draw_buf_t *snap = lv_snapshot_take(panel, LV_COLOR_FORMAT_RGB565);
            assert(snap && snap->header.w == 1024 && snap->header.h == 600 && snap->header.stride == 2048);
            test_fb[0] = calloc(1024u * 600u, sizeof(uint16_t));
            assert(test_fb[0] && !media_panel_pixel_pattern_scan());
            test_fb[1] = (uint16_t *)snap->data;
            assert(media_panel_pixel_pattern_scan());
            test_fb[1] = NULL;
            free(test_fb[0]);
            test_fb[0] = NULL;
            FILE *f = fopen(path, "wb");
            uint32_t hash = UINT32_C(2166136261);
            assert(f);
            for (unsigned y = 0; y < 600; ++y) {
                const uint16_t *row = (const uint16_t *)(snap->data + y * snap->header.stride);
                fprintf(f, "SNAP_ROW %u ", y);
                for (unsigned x = 0; x < 1024; ++x) {
                    hash = (hash * UINT32_C(16777619)) ^ row[x];
                    fprintf(f, "%04X", (unsigned)row[x]);
                }
                fputc('\n', f);
            }
            fprintf(f, "SNAP_END w=1024 h=600 crc=%08X err=ESP_OK\n", (unsigned)hash);
            assert(!fclose(f));
            lv_draw_buf_destroy(snap);
        }
        lv_obj_send_event(exit, LV_EVENT_CLICKED, NULL);
        assert(synthetic_done && !stopped && !capture_epoch && lv_screen_active() == home);
        assert(!media_panel_pixel_pattern_scan());
        lv_test_wait(32);
    }
    lv_display_delete(d);
    lv_deinit();
    puts("MAG13 LVGL 1024x600 offline pattern, memory refusal, Stop and Home passed.");
}

enum { PIPELINE_PICTURES = 12 };
typedef struct {
    uint32_t epoch;
    unsigned reads, display, fail_at;
    bool remove_card;
    unsigned card_ms[PIPELINE_PICTURES];
    int64_t read_begin[PIPELINE_PICTURES], read_end[PIPELINE_PICTURES];
    int64_t draw_begin[PIPELINE_PICTURES], draw_end[PIPELINE_PICTURES];
    lv_obj_t *screen, *image;
    atomic_bool done;
    kvm_play_result_t result;
} pipeline_test_t;

static bool pipeline_read(void *ctx, uint16_t *out, const uint16_t *previous, uint64_t *pts,
                          uint32_t *duration_us, uint32_t *crc, bool (*cancel)(void *), void *cancel_ctx)
{
    pipeline_test_t *p = ctx;
    unsigned i = p->reads++;
    assert(i < PIPELINE_PICTURES && (!i || (out != previous && previous[0] == i)));
    p->read_begin[i] = esp_timer_get_time();
    vTaskDelay(p->card_ms[i]);
    if (p->remove_card && i == p->fail_at) {
        atomic_store(&available, false);
        simulated_time_offset += 250001;
    }
    if (cancel(cancel_ctx) || i == p->fail_at) return false;
    for (unsigned j = 0; j < 960 * 540; ++j) out[j] = (uint16_t)(i + 1);
    *crc = esp_rom_crc32_le(0, (const uint8_t *)out, 960 * 540 * 2);
    *pts = 40000u * i;
    *duration_us = 40000;
    p->read_end[i] = esp_timer_get_time();
    return true;
}

static void *pipeline_play(void *ctx)
{
    pipeline_test_t *p = ctx;
    p->result = media_panel_play_rgb(p->epoch, 960, 540, PIPELINE_PICTURES, pipeline_read, p,
                                     p->remove_card ? present : NULL);
    atomic_store(&p->done, true);
    return NULL;
}

static void pipeline_flush(lv_event_t *event)
{
    pipeline_test_t *p = lv_event_get_user_data(event);
    if (lv_screen_active() != p->screen) return;
    const lv_image_dsc_t *src = lv_image_get_src(p->image);
    if (!src) return;
    const uint16_t *pixels = (const uint16_t *)src->data;
    unsigned seq = pixels[0];
    if (seq == p->display) return;
    assert(seq == p->display + 1 && seq <= PIPELINE_PICTURES);
    assert(pixels[960 * 540 - 1] == seq);
    p->display = seq;
    p->draw_begin[seq - 1] = esp_timer_get_time();
    vTaskDelay(30);
    // The worker may write the other surface while this flush owns the source.
    for (unsigned j = 0; j < 960 * 540; ++j) assert(pixels[j] == seq);
    p->draw_end[seq - 1] = esp_timer_get_time();
}

static void check_prepared_pipeline(const char *schedule, const char *mode)
{
    bool serial = !strcmp(mode, "serial"), remove = !strcmp(mode, "removed");
    bool empty = !strcmp(mode, "fail-first");
    bool fail = remove || empty || !strcmp(mode, "fail");
    assert(serial || fail || !strcmp(mode, "pipelined"));
    pipeline_test_t p = {.fail_at = empty ? 0 : fail ? 6 : PIPELINE_PICTURES, .remove_card = remove};
    atomic_store(&available, true);
    FILE *f = fopen(schedule, "r");
    assert(f);
    for (unsigned i = 0; i < PIPELINE_PICTURES; ++i)
        assert(fscanf(f, "%u", &p.card_ms[i]) == 1 && p.card_ms[i] > 0 && p.card_ms[i] <= 1000);
    fclose(f);
    lv_init();
    lv_display_t *d = lv_test_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    p.epoch = media_panel_open(false, NULL);
    p.screen = lv_screen_active();
    p.image = lv_obj_get_child(p.screen, 0);
    media_panel_time_without_scanout_crc(p.epoch);
    lv_display_add_event_cb(d, pipeline_flush, LV_EVENT_FLUSH_START, &p);
    pthread_t player;
    assert(!pthread_create(&player, NULL, pipeline_play, &p));
    for (unsigned i = 0; !atomic_load(&p.done) && i < 20000; ++i) {
        lv_test_wait(16);
        vTaskDelay(1);
    }
    assert(atomic_load(&p.done));
    pthread_join(player, NULL);
    assert(p.display == p.fail_at && p.result == (remove ? KVM_PLAY_STOPPED : fail ? KVM_PLAY_UNAVAILABLE : KVM_PLAY_OK));
    if (!remove)
        assert(media_panel_file_frames(p.epoch) == p.display);
    assert(!media_panel_file_frames(p.epoch + 1));
    media_panel_file_stats_t stats;
    unsigned overlaps = 0;
    for (unsigned i = 1; i < p.display; ++i)
        if (p.read_begin[i] < p.draw_end[i - 1] && p.read_end[i] > p.draw_begin[i - 1]) ++overlaps;
    if (!fail) {
        assert(media_panel_file_stats(p.epoch, &stats) && stats.frames == p.display);
        assert(serial ? !overlaps : overlaps == p.display - 1);
        printf("prepared_pipeline frames=%u overlaps=%u first_to_last_us=%llu\n",
               p.display, overlaps, (unsigned long long)stats.window_us);
    } else {
        assert_no_stats(p.epoch);
        printf("prepared_pipeline %s frames=%u return=%d\n", mode, p.display, p.result);
    }
    lv_display_remove_event_cb_with_user_data(d, pipeline_flush, &p);
    media_panel_close();
    assert(!media_panel_file_frames(p.epoch));
    lv_display_delete(d);
    lv_deinit();
}

static void save_palette_screen(const char *palette, const char *phase)
{
    char path[96];
    snprintf(path, sizeof(path), "%s-%s.png", palette, phase);
    sim_image_t img;
    assert(sim_image_render(lv_screen_active(), &img) && img.w == 1024 && img.h == 600);
    assert(sim_image_write_png(path, &img));
    sim_image_free(&img);
}

static unsigned local_stops, local_home_visits;

static void stopped_at_file(void) { ++local_stops; }
static void returned_home(void) { ++local_home_visits; }

static void check_file_strips(const lv_image_dsc_t *source, unsigned w, unsigned h)
{
    int dw = source->header.w, dh = source->header.h;
    lv_image_decoder_dsc_t decoder[2];
    lv_area_t request[2] = {{0, 1, dw - 1, 11}, {0, dh - 6, dw - 1, dh + 2}};
    lv_area_t area[2] = {{.y1 = LV_COORD_MIN}, {.y1 = LV_COORD_MIN}};
    int next[2] = {request[0].y1, request[1].y1};
    int end[2] = {request[0].y2, dh - 1};
    for (unsigned i = 0; i < 2; ++i)
        assert(lv_image_decoder_open(&decoder[i], source, NULL) == LV_RESULT_OK);

    // The opens are interleaved so that accidentally sharing their previous rows corrupts the pixels.
    while (next[0] <= end[0] || next[1] <= end[1]) {
        for (unsigned i = 0; i < 2; ++i) {
            if (next[i] > end[i]) continue;
            assert(lv_image_decoder_get_area(&decoder[i], &request[i], &area[i]) == LV_RESULT_OK);
            assert(area[i].y1 == next[i] && area[i].y2 >= next[i] && area[i].y2 <= end[i]);
            const lv_draw_buf_t *strip = decoder[i].decoded;
            for (int y = area[i].y1; y <= area[i].y2; ++y) {
                const uint16_t *row = (const uint16_t *)(strip->data + (y - area[i].y1) * strip->header.stride);
                for (int x = 0; x < dw; ++x)
                    assert(row[x] == (uint16_t)(16 + (y * h / dh) * w + x * w / dw));
            }
            next[i] = area[i].y2 + 1;
        }
    }
    for (unsigned i = 0; i < 2; ++i) {
        assert(lv_image_decoder_get_area(&decoder[i], &request[i], &area[i]) == LV_RESULT_INVALID);
        // A fresh one-line region in the same open must restart from the source.
        area[i].y1 = LV_COORD_MIN;
        request[i].y1 = request[i].y2 = 0;
        assert(lv_image_decoder_get_area(&decoder[i], &request[i], &area[i]) == LV_RESULT_OK);
        assert(area[i].y1 == 0 && area[i].y2 == 0 && ((const uint16_t *)decoder[i].decoded->data)[0] == 16);
        lv_image_decoder_close(&decoder[i]);
    }
}

static void check_file_controls(void)
{
    lv_init();
    lv_display_t *display = lv_test_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_test_indev_create_all();
    assert(ui_theme_init());
    lv_obj_t *home = lv_screen_active();
    const unsigned sizes[][4] = {
        {160, 90, 960, 540}, {90, 160, 303, 540}, {200, 50, 1024, 256}, {320, 180, 960, 540},
        {480, 270, 960, 540}
    };
    for (unsigned pass = 0; pass < sizeof(sizes) / sizeof(sizes[0]); ++pass) {
        ui_theme_set_control_palette((ui_control_palette_t)(pass % 3));
        rgb_clip_t clip = {.token = media_panel_open(false, returned_home), .count = 2,
                           .width = sizes[pass][0], .height = sizes[pass][1]};
        lv_obj_t *panel = lv_screen_active(), *picture = lv_obj_get_child(panel, 0);
        assert(clip.token);
        media_panel_file_stop_action(clip.token, stopped_at_file);
        media_panel_file_stop_action(clip.token + 1, returned_home);
        media_panel_file_title(clip.token, "Coastal light.mp4");
        assert(find_label(panel, "Coastal light.mp4"));
        // The previous session closed in pixel size, including when it closed while paused.
        if (pass > 0) {
            assert(lv_obj_has_state(find_button(panel, "1:1"), LV_STATE_CHECKED));
            touch_button(panel, "Fit");
        }
        assert(lv_obj_has_state(find_button(panel, "Fit"), LV_STATE_CHECKED));
        atomic_store(&clip.hold_second, true);
        pthread_t player;
        assert(!pthread_create(&player, NULL, play_rgb_clip, &clip));
        for (unsigned i = 0; i < 1000 && !atomic_load(&clip.waiting); ++i) {
            lv_test_wait(16);
            vTaskDelay(1);
        }
        lv_test_wait(64);
        assert(atomic_load(&clip.waiting) && media_panel_file_frames(clip.token) == 1);
        const lv_image_dsc_t *rendered = lv_image_get_src(picture);
        unsigned dw = sizes[pass][2], dh = sizes[pass][3];
        assert(rendered->header.w == dw && rendered->header.h == dh);
        check_file_strips(rendered, clip.width, clip.height);
        assert(find_label(panel, "0:00   1 / 2"));
        unsigned x = lv_obj_get_x(picture), y = lv_obj_get_y(picture);
        uint16_t *fb = board_lcd_fb(0);
        assert(x == (1024 - dw) / 2 && y == 46 + (540 - dh) / 2);
        for (unsigned row = 0; row < dh; ++row)
            for (unsigned col = 0; col < dw; ++col)
                assert(fb[(y + row) * 1024 + x + col] ==
                       (uint16_t)(16 + (row * clip.height / dh) * clip.width + col * clip.width / dw));
        if (x) assert(!fb[(y + 10) * 1024 + x - 1]);
        if (y > 46) assert(!fb[(y - 1) * 1024 + x + 10]);
        touch_button(panel, "Pause");
        lv_test_wait(32);
        assert(find_label(panel, "Paused") && find_button(panel, "Resume"));
        atomic_store(&clip.hold_second, false);
        for (unsigned i = 0; i < 6; ++i) { lv_test_wait(32); vTaskDelay(12); }
        assert(!atomic_load(&clip.done) && media_panel_file_frames(clip.token) == 1);
        if (pass < 3)
            save_palette_screen(pass == 0 ? "day" : pass == 1 ? "night" : "high-contrast", "player-fit");
        touch_button(panel, "1:1");
        lv_test_wait(32);
        rendered = lv_image_get_src(picture);
        assert(rendered->header.w == clip.width && rendered->header.h == clip.height);
        assert(lv_obj_has_state(find_button(panel, "1:1"), LV_STATE_CHECKED));
        x = lv_obj_get_x(picture); y = lv_obj_get_y(picture);
        fb = board_lcd_fb(0);
        assert(fb[(y + 10) * 1024 + x + 10] == 16 + 10 * clip.width + 10);
        touch_button(panel, "Fit");
        lv_test_wait(32);
        rendered = lv_image_get_src(picture);
        assert(rendered->header.w == dw && rendered->header.h == dh);
        x = lv_obj_get_x(picture); y = lv_obj_get_y(picture);
        assert(board_lcd_fb(0)[(y + dh - 1) * 1024 + x + dw - 1] == (uint16_t)(15 + clip.width * clip.height));
        touch_button(panel, "1:1");
        bool go_home = pass == 1 || pass >= 3;
        if (go_home) {
            touch_button(panel, "Home");
        } else {
            touch_button(panel, "Resume");
            for (unsigned i = 0; i < 1000 && !atomic_load(&clip.done); ++i) {
                lv_test_wait(16);
                vTaskDelay(1);
            }
            assert(media_panel_file_frames(clip.token) == 2 && atomic_load(&clip.done));
            touch_button(panel, "Stop");
        }
        pthread_join(player, NULL);
        assert(clip.result == (go_home ? KVM_PLAY_STOPPED : KVM_PLAY_OK));
        lv_test_wait(64);
        assert(lv_screen_active() == home);
        assert(local_stops == (pass >= 2 ? 2u : 1u));
        assert(local_home_visits + local_stops == pass + 1);
    }
    ui_theme_deinit();
    lv_test_indev_delete_all();
    lv_display_delete(display);
    lv_deinit();
    puts("PASS player controls, three palettes, 2x/3x/6x and fractional Fit, clipped independent strips, pause/resume, Stop/Home");
}

static void check_touch_target(lv_obj_t *target)
{
    assert(target);
    lv_obj_update_layout(target);
    assert(lv_obj_is_visible(target));
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    assert(a.x1 >= 0 && a.x2 < 1024 && a.y1 >= 0 && a.y2 < 600);
    assert(lv_area_get_width(&a) >= 44 && lv_area_get_height(&a) >= 44);
    for (int32_t y = a.y1; y <= a.y2; y += a.y2 - a.y1) {
        lv_point_t point = {(a.x1 + a.x2) / 2, y};
        assert(lv_indev_search_obj(lv_screen_active(), &point) == target);
    }
}

static unsigned rendered_color(lv_area_t area, lv_color_t color)
{
    const lv_draw_buf_t *fb = lv_display_get_buf_active(lv_display_get_default());
    assert(area.x1 >= 0 && area.x2 < 1024 && area.y1 >= 0 && area.y2 < 600);
    unsigned count = 0;
    const uint16_t expected = lv_color_to_u16(color);
    for (int32_t y = area.y1; y <= area.y2; ++y) {
        const uint16_t *row = (const uint16_t *)(fb->data + y * fb->header.stride);
        for (int32_t x = area.x1; x <= area.x2; ++x) count += row[x] == expected;
    }
    return count;
}

static lv_color_t role_color(ui_style_id_t role, lv_style_prop_t property)
{
    lv_style_value_t value;
    assert(lv_style_get_prop(ui_theme_style(role), property, &value) == LV_STYLE_RES_FOUND);
    return value.color;
}

static void check_control_role(lv_obj_t *control, ui_style_id_t role)
{
    assert(lv_color_eq(lv_obj_get_style_bg_color(control, LV_PART_MAIN),
                       role_color(role, LV_STYLE_BG_COLOR)));
    assert(lv_color_eq(lv_obj_get_style_text_color(control, LV_PART_MAIN),
                       role_color(role, LV_STYLE_TEXT_COLOR)));
}

static void touch_option(lv_obj_t *settings, const char *option, const char *palette)
{
    // Sample the resting colors after the previous tap's pressed-state transition.
    lv_test_wait(300);
    lv_obj_t *dropdown = NULL;
    for (uint32_t i = 0; i < lv_obj_get_child_count_by_type(settings, &lv_dropdown_class); ++i) {
        lv_obj_t *item = lv_obj_get_child_by_type(settings, i, &lv_dropdown_class);
        if (lv_dropdown_get_option_index(item, option) >= 0) dropdown = item;
    }
    check_touch_target(dropdown);
    check_control_role(dropdown, UI_STYLE_CONTROL_BUTTON);
    lv_area_t a;
    lv_obj_get_coords(dropdown, &a);
    touch_at((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2);
    lv_test_wait(100);
    assert(lv_dropdown_is_open(dropdown));
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_t *label = lv_obj_get_child_by_type(list, 0, &lv_label_class);
    assert(label && lv_obj_is_visible(label));
    int32_t next = lv_dropdown_get_option_index(dropdown, option);
    int32_t selected = lv_dropdown_get_selected(dropdown);
    assert(next != selected);
    lv_obj_get_coords(label, &a);
    int32_t height = lv_font_get_line_height(lv_obj_get_style_text_font(label, 0));
    int32_t step = height + lv_obj_get_style_text_line_space(label, 0);
    assert(step >= 44);
    if (palette && !strcmp(option, "H.264")) {
        save_palette_screen(palette, "codec-list");
        lv_refr_now(NULL);
        for (int32_t i = 0; i < 2; ++i) {
            ui_style_id_t role = i == selected ? UI_STYLE_CONTROL_SELECTED : UI_STYLE_CONTROL_BUTTON;
            lv_area_t row = {a.x1, a.y1 + i * step, a.x2, a.y1 + i * step + height - 1};
            assert(rendered_color(row, role_color(role, LV_STYLE_BG_COLOR)) > 15);
            assert(rendered_color(row, role_color(role, LV_STYLE_TEXT_COLOR)) > 4);
        }
    }
    touch_at((a.x1 + a.x2) / 2, a.y1 + next * step + height / 2);
    assert(!lv_dropdown_is_open(dropdown) && lv_dropdown_get_selected(dropdown) == (uint32_t)next);
    if (ui_theme_control_palette() == UI_PALETTE_HIGH_CONTRAST)
        check_control_role(dropdown, UI_STYLE_CONTROL_BUTTON);
}

static void check_magnifier_geometry(lv_obj_t *start, const char *palette)
{
    lv_obj_t *fields = lv_obj_get_parent(start);
    lv_obj_t *card = lv_obj_get_parent(fields);
    lv_obj_t *sample = lv_obj_get_child_by_type(card, 0, &lv_image_class);
    assert(sample && lv_obj_get_width(sample) == 240 && lv_obj_get_height(sample) == 135);
    lv_obj_t *frame = lv_obj_get_child(sample, 0);
    const char *const codecs[] = {"JPEG", "H.264", "Cinepak"};
    const char *const zooms[] = {"Overview", "Pixel 1x", "Zoom 2x"};
    for (unsigned c = 0; c < 3; ++c) {
        if (c) touch_option(fields, codecs[c], palette);
        for (unsigned z = 0; z < 3; ++z) {
            touch_option(fields, zooms[z], palette);
            unsigned w = c == 2 ? 960 : 512, h = c == 2 ? 540 : 512;
            char text[64];
            snprintf(text, sizeof(text), "KVMpad image   %u x %u px", w, h);
            assert(lv_obj_is_visible(find_label(card, text)));
            w = (2 * w) >> z;
            h = (2 * h) >> z;
            snprintf(text, sizeof(text), "%u x %u px", w, h);
            assert(lv_obj_is_visible(find_label(card, text)));
            lv_refr_now(NULL);
            lv_area_t area, monitor;
            lv_obj_get_coords(frame, &area);
            lv_obj_get_coords(sample, &monitor);
            assert(lv_area_get_width(&area) == (int32_t)((w + 4) / 8));
            assert(lv_area_get_height(&area) == (int32_t)((h + 4) / 8));
            assert(abs(area.x1 + area.x2 - monitor.x1 - monitor.x2) <= 1);
            assert(abs(area.y1 + area.y2 - monitor.y1 - monitor.y2) <= 1);
            lv_area_t border = {area.x1 + 3, area.y1, area.x2 - 3, area.y1 + 1};
            assert(rendered_color(border, lv_color_white()) > (unsigned)lv_area_get_width(&border));
            assert(!capture_epoch && !allowed_epoch);
            if (c == 2 && z == 0) save_palette_screen(palette, "cinepak-overview");
        }
    }
    touch_option(fields, "Pixel 1x", palette);
    touch_option(fields, "JPEG", palette);
}

static uint32_t check_review_text(lv_obj_t *panel, const char *name)
{
    char question[96];
    snprintf(question, sizeof(question), "Allow capture of %s?", name);
    lv_obj_t *label = find_label(panel, question);
    assert(label && lv_obj_is_visible(label));
    lv_obj_update_layout(label);
    lv_refr_now(NULL);
    lv_area_t content;
    lv_obj_get_content_coords(label, &content);
    const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
    // Check the final source character and question mark in the actual RGB565 scanout.
    uint32_t crc = 0;
    for (unsigned i = (unsigned)strlen(question) - 2; question[i]; ++i) {
        lv_point_t pos;
        lv_label_get_letter_pos(label, i, &pos);
        int32_t w = lv_font_get_glyph_width(font, question[i], question[i + 1]);
        lv_area_t glyph = {content.x1 + pos.x, content.y1 + pos.y, content.x1 + pos.x + w - 1,
                           content.y1 + pos.y + lv_font_get_line_height(font) - 1};
        assert(glyph.y1 > content.y1 && glyph.x1 >= content.x1 && glyph.x2 <= content.x2 && glyph.y2 <= content.y2);
        assert(rendered_color(glyph, lv_obj_get_style_text_color(label, 0)) > 2);
        if (question[i] != '?')
            crc = crc_rows(lv_display_get_buf_active(lv_display_get_default()), glyph.x1, glyph.y1,
                           lv_area_get_width(&glyph), lv_area_get_height(&glyph));
    }
    return crc;
}

static void check_magnifier_palettes(void)
{
    lv_init();
    lv_display_t *display = lv_test_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_test_indev_create_all();
    assert(ui_theme_init());
    lv_obj_t *home = lv_screen_active();
    const char *palettes[] = {"day", "night", "high-contrast"};
    for (unsigned p = 0; p < UI_PALETTE_COUNT; ++p) {
        ui_theme_set_control_palette(p);
        uint32_t crc[2];
        for (unsigned pass = 0; pass < 2; ++pass) {
            char name[64];
            memset(name, 'W', 63);
            name[62] = pass ? 'M' : 'W';
            name[63] = 0;
            review_resource = name;
            capture_epoch = allowed_epoch = 0;
            uint32_t epoch = media_panel_open_magnifier("net.example.magnifier", "PIXEL_1X", NULL);
            assert(epoch && !capture_epoch);
            lv_obj_t *panel = lv_screen_active();
            lv_obj_t *start = find_button(panel, "Start sharing");
            check_touch_target(start);
            check_control_role(start, UI_STYLE_CONTROL_SELECTED);
            check_touch_target(find_button(panel, "Home"));
            check_touch_target(find_button(panel, "Stop"));
            if (!pass) {
                save_palette_screen(palettes[p], "setup");
                check_magnifier_geometry(start, palettes[p]);
            }
            touch_option(lv_obj_get_parent(start), "Zoom 2x", palettes[p]);
            touch_option(lv_obj_get_parent(start), "H.264", palettes[p]);
            touch_button(panel, "Start sharing");
            assert(capture_epoch == epoch && requested_codec == KVM_LIVE_H264_INTRA && !strcmp(requested_mode, "ZOOM_2X"));
            capture_code = "awaiting_picker";
            capture_text = "Choose a whole monitor in the Windows picker";
            lv_test_wait(64);
            assert(lv_obj_is_visible(find_label(panel, capture_text)));
            capture_code = "awaiting_pointer";
            capture_text = "Move the pointer onto the monitor you selected";
            lv_test_wait(64);
            assert(lv_obj_is_visible(find_label(panel, capture_text)) && !allowed_epoch);
            assert(!lv_obj_is_visible(find_button(panel, "Allow display")));
            if (!pass) save_palette_screen(palettes[p], "pointer");
            pointer_resource = pass ? "\\\\.\\DISPLAY99999" : "\\\\.\\DISPLAY2";
            const char *hint = pass ? "Move the pointer onto DISPLAY99999" : "Move the pointer onto DISPLAY2";
            lv_test_wait(64);
            lv_obj_t *prompt = find_label(panel, hint);
            assert(prompt && lv_obj_is_visible(prompt) && !allowed_epoch);
            assert(!lv_obj_is_visible(find_button(panel, "Allow display")) && !fixture_bound);
            lv_point_t last;
            char digit = hint[strlen(hint) - 1];
            lv_label_get_letter_pos(prompt, strlen(hint) - 1, &last);
            assert(last.y == 0 && last.x + lv_font_get_glyph_width(lv_obj_get_style_text_font(prompt, 0), digit, 0)
                                  <= lv_obj_get_content_width(prompt));
            save_palette_screen(palettes[p], pass ? "pointer-long" : "pointer-display2");
            {
                capture_code = "cursor";
                capture_text = "Pointer left the selected display. Tap Start sharing to retry.";
                transport_ended(epoch);
                lv_test_wait(64);
                lv_obj_t *reason = find_label(panel, capture_text);
                assert(reason && lv_obj_is_visible(reason) && lv_obj_is_visible(start));
                assert(!allowed_epoch && !lv_obj_is_visible(find_button(panel, "Allow display")));
                lv_point_t end;
                lv_label_get_letter_pos(reason, strlen(capture_text) - 1, &end);
                assert(end.y == 0 && end.x + lv_font_get_glyph_width(lv_obj_get_style_text_font(reason, 0), '.', 0)
                                      <= lv_obj_get_content_width(reason));
                if (!pass) save_palette_screen(palettes[p], "pointer-exit");
                touch_button(panel, "Start sharing");
                assert(capture_epoch && capture_epoch != epoch && !allowed_epoch && !pointer_resource);
                assert(requested_codec == KVM_LIVE_H264_INTRA && !strcmp(requested_mode, "ZOOM_2X"));
                epoch = capture_epoch;
            }
            fixture_review = true;
            lv_test_wait(64);
            assert(lv_obj_is_visible(find_label(panel, "Cursor magnifier")) && !allowed_epoch);
            check_touch_target(find_button(panel, "Allow display"));
            check_control_role(find_button(panel, "Allow display"), UI_STYLE_CONTROL_SELECTED);
            check_touch_target(find_button(panel, "Stop"));
            assert(!lv_obj_is_visible(start) && !lv_obj_is_visible(find_button(panel, "Freeze")));
            check_touch_target(find_button(panel, "Settings"));
            crc[pass] = check_review_text(panel, name);
            save_palette_screen(palettes[p], pass ? "review-m" : "review-w");
            if (pass) {
                touch_button(panel, "Allow display");
                assert(allowed_epoch == epoch);
                fixture_bound = true;
                lv_test_wait(48);
                check_touch_target(find_button(panel, "Monitor"));
            }
            stopped = false;
            touch_button(panel, "Stop");
            assert(stopped && lv_screen_active() == home && allowed_epoch == (pass ? epoch : 0));
            fixture_review = false;
            lv_test_wait(64);
        }
        assert(crc[0] != crc[1]);
        printf("PASS %s: setup, normal/selected popup colors, touch H.264 and Zoom 2x, 63-character display "
               "review with distinct final glyph CRCs %08x/%08x, touch Allow/Stop.\n", palettes[p], crc[0], crc[1]);
    }
    review_resource = "Test display 2";
    ui_theme_deinit();
    lv_test_indev_delete_all();
    lv_display_delete(display);
    lv_deinit();
}

static void check_profile_capture_start(void)
{
    lv_init();
    lv_display_t *display = lv_test_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_test_indev_create_all();
    assert(ui_theme_init());
    lv_obj_t *home = lv_screen_active();
    const char *profile = "org.kvmpad.utility.cursor-magnifier";
    kvm_live_codec_t previous = KVM_LIVE_JPEG;
    uint32_t custom = media_panel_open_magnifier("net.example.magnifier", "PIXEL_1X", NULL);
    assert(custom && !media_panel_request_capture(custom) && !capture_requests);
    touch_button(lv_screen_active(), "Stop");
    lv_test_wait(64);
    for (unsigned phase = 0; phase < 4; phase++) {
        capture_epoch = allowed_epoch = 0;
        unsigned requests = capture_requests;
        unsigned frames = atomic_load(&shown_count);
        uint32_t epoch = media_panel_open_magnifier(profile, "OVERVIEW", NULL);
        lv_obj_t *panel = lv_screen_active();
        assert(epoch && panel != home && !capture_epoch);
        host_attached = false;
        assert(!media_panel_request_capture(epoch) && capture_requests == requests);
        check_touch_target(find_button(panel, "Start sharing"));
        host_attached = true;
        assert(!media_panel_request_capture(epoch + 1));
        assert(media_panel_request_capture(epoch));
        assert(!media_panel_request_capture(epoch));
        assert(capture_requests == requests + 1 && capture_epoch == epoch && !allowed_epoch);
        assert(!strcmp(requested_profile, profile) && !strcmp(requested_mode, "OVERVIEW") &&
               requested_codec == previous);
        capture_code = "awaiting_picker";
        capture_text = "Choose a whole monitor in the Windows picker";
        lv_test_wait(64);
        check_touch_target(find_button(panel, "Settings"));
        assert(!lv_obj_is_visible(find_button(panel, "Allow display")));
        if (phase == 1) {
            fixture_review = true;
            review_resource = "\\\\.\\DISPLAY2";
            lv_test_wait(64);
            check_touch_target(find_button(panel, "Allow display"));
        }
        if (phase < 2) {
            closing_fixture = epoch;
            stopped = false;
            touch_button(panel, "Settings");
            lv_test_wait(64);
            assert(stopped && !allowed_epoch && !lv_obj_is_visible(find_button(panel, "Start sharing")));
            closing_fixture = 0;
            fixture_review = false;
        } else {
            capture_code = phase == 2 ? "cancel" : "host_denied";
            capture_text = phase == 2 ? "Monitor selection was canceled in Windows" : "The HOST declined capture";
        }
        transport_ended(epoch);
        lv_test_wait(96);
        lv_obj_t *start = find_button(panel, "Start sharing");
        check_touch_target(start);
        assert(!allowed_epoch && !fixture_bound && capture_requests == requests + 1);
        assert(atomic_load(&shown_count) == frames);
        if (phase >= 2) assert(lv_obj_is_visible(find_label(panel, capture_text)));
        touch_option(lv_obj_get_parent(start), "Zoom 2x", NULL);
        touch_option(lv_obj_get_parent(start), phase % 2 ? "Cinepak" : "H.264", NULL);
        touch_button(panel, "Start sharing");
        uint32_t fresh = capture_epoch;
        assert(fresh && fresh != epoch && capture_requests == requests + 2 && !allowed_epoch);
        assert(requested_codec == (phase % 2 ? KVM_LIVE_CINEPAK_INTRA : KVM_LIVE_H264_INTRA));
        previous = requested_codec;
        assert(!strcmp(requested_mode, "ZOOM_2X") && !media_panel_request_capture(epoch));
        transport_ended(epoch);
        fixture_review = true;
        lv_test_wait(64);
        touch_button(panel, "Allow display");
        assert(allowed_epoch == fresh && atomic_load(&shown_count) == frames);
        touch_button(panel, "Stop");
        assert(lv_screen_active() == home);
        fixture_review = false;
        lv_test_wait(64);
    }
    puts("PASS profile start: missing HOST, stale epoch, one JPEG request, Settings from picker/review, "
         "cancel/denial, new H.264/Cinepak request, separate Allow, stale callback and Stop.");
    ui_theme_deinit();
    lv_test_indev_delete_all();
    lv_display_delete(display);
    lv_deinit();
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--file-controls")) {
        check_file_controls();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--magnifier-profile-start")) {
        check_profile_capture_start();
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--magnifier-palettes")) {
        check_magnifier_palettes();
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--h264-freshness")) {
        lv_init();
        lv_display_t *display = lv_test_display_create(1024, 600);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        exercise(argv[2], KVM_LIVE_H264_INTRA, 512, 512, 24, 60, (uint32_t)strtoul(argv[3], NULL, 0));
        lv_display_delete(display);
        lv_deinit();
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--partial-cinepak")) {
        check_partial_cinepak(argv[2], argv[3]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--card-pipeline")) {
        check_prepared_pipeline(argv[2], argv[3]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--pixel-pattern")) {
        check_pixel_pattern(argv[2]);
        return 0;
    }
#ifdef KVM_TEST_MEDIA_HID
    if (argc == 3 && !strcmp(argv[1], "--hid-load")) {
        void panel_test_hid_baseline(void);
        void panel_test_hid_load(uint32_t, uint8_t *, size_t, lv_obj_t *);
        void panel_test_hid_finished(void);
        size_t size;
        uint8_t *fixture = load_frame(argv[2], &size);
        // Annex B permits trailing zero bytes. Exercise both queue buffers at their full wire bound.
        uint8_t *message = calloc(KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD, 1);
        assert(message && size <= KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD);
        memcpy(message, fixture, size);
        free(fixture);
        lv_init();
        lv_display_t *d = lv_test_display_create(1024, 600);
        lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
        lv_obj_t *home = lv_screen_active();
        panel_test_hid_baseline();
        uint32_t token = media_panel_open(true, NULL);
        lv_obj_send_event(find_button(lv_screen_active(), "Start sharing"), LV_EVENT_CLICKED, NULL);
        assert(media_panel_stream_start(token, 17, KVM_LIVE_H264_INTRA));
        panel_test_hid_load(token, message, KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD,
                            find_button(lv_screen_active(), "Stop"));
        assert(stopped && lv_screen_active() == home);
        pthread_join(worker, NULL);
        panel_test_hid_finished();
        lv_test_wait(32);
        size = KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD;
        assert(lv_screen_active() == home && !media_panel_stream_push(token, message, size));
        uint32_t next = media_panel_open(true, NULL);
        assert(next && next != token);
        lv_obj_send_event(find_button(lv_screen_active(), "Start sharing"), LV_EVENT_CLICKED, NULL);
        assert(media_panel_stream_start(next, 17, KVM_LIVE_H264_INTRA));
        assert(!media_panel_stream_push(token, message, size));
        bool accepted = false;
        for (int retries = 0; !accepted && retries < 100; ++retries) {
            accepted = media_panel_stream_push(next, message, size);
            if (!accepted) vTaskDelay(1);
        }
        assert(accepted);
        lv_obj_send_event(find_button(lv_screen_active(), "Stop"), LV_EVENT_CLICKED, NULL);
        pthread_join(worker, NULL);
        lv_test_wait(32);
        assert(lv_screen_active() == home);
        puts("MAG-05: fresh message rejected under the closed epoch and accepted under the next epoch with the same stream ID.");
        lv_display_delete(d);
        lv_deinit();
        free(message);
        return 0;
    }
#endif
    if (argc == 3 && !strcmp(argv[1], "--file-modes")) {
        lv_init();
        lv_display_t *display = lv_test_display_create(1024, 600);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        check_native_file_stats(argv[2]);
        card_mode = KVM_H264_LARGE_BANK;
        check_native_file_stats(argv[2]);
        lv_display_delete(display);
        lv_deinit();
        return 0;
    }
#ifdef KVM_MEDIA_PAGED_GUEST
    if (argc != 7)
        return 2;
#else
    if (argc != 5 && argc != 6)
        return 2;
#endif
    lv_init();
    lv_display_t *d = lv_test_display_create(1024, 600);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    exercise_profile_modes();
    check_file_stats();
    exercise(argv[1], KVM_LIVE_CINEPAK_INTRA, 960, 540, 32, 46, (uint32_t)strtoul(argv[2], NULL, 0));
    exercise(argv[3], KVM_LIVE_H264_INTRA, 512, 512, 24, 60, (uint32_t)strtoul(argv[4], NULL, 0));
    if (argc >= 6) {
        check_file_refusal(argv[5], true);
        check_file_refusal(argv[5], false);
        check_native_file_stats(argv[5]);
        check_card_loss(argv[5]);
    }
#ifdef KVM_MEDIA_PAGED_GUEST
    check_paged(argv[6]);
#endif
    exercise_wire_lifetime();
    exercise_diagnostic(true, false, false);
    exercise_diagnostic(true, true, false);
    exercise_diagnostic(true, true, true);
    exercise_diagnostic(false, false, false);
    lv_display_delete(d);
    lv_deinit();
    return 0;
}
