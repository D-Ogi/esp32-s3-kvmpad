// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Use this source in place of panel_test.c and media_panel.c in the native LVGL build.
// Link with --wrap for kvm_media_decoder_{open,close,frame} and kvm_cvid_index.
#define main full_panel_suite
#include "panel_test.c"
#undef main
#include "../../main/media_panel.c"

static int jpeg_handle;
static unsigned jpeg_phase;
static atomic_uint cinepak_calls;
static atomic_bool hold_third;
kvm_media_decoder_t *__real_kvm_media_decoder_open(kvm_live_codec_t);
void __real_kvm_media_decoder_close(kvm_media_decoder_t *);
bool __real_kvm_media_decoder_frame(kvm_media_decoder_t *, const uint8_t *, size_t, const kvm_media_target_t *);
bool __real_kvm_cvid_index(kvm_cvid_index_t *, const uint8_t *, size_t);

kvm_media_decoder_t *__wrap_kvm_media_decoder_open(kvm_live_codec_t codec)
{
    return codec == KVM_LIVE_JPEG ? (void *)&jpeg_handle : __real_kvm_media_decoder_open(codec);
}

void __wrap_kvm_media_decoder_close(kvm_media_decoder_t *d)
{
    if (d != (void *)&jpeg_handle) __real_kvm_media_decoder_close(d);
}

bool __wrap_kvm_media_decoder_frame(kvm_media_decoder_t *d, const uint8_t *data, size_t n, const kvm_media_target_t *t)
{
    if (d != (void *)&jpeg_handle) return __real_kvm_media_decoder_frame(d, data, n, t);
    // The desktop has no S3 mask ROM. Replay the independently verified RGB565 quadrants.
    unsigned p = jpeg_phase++ % 2;
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, 0xffff};
    assert(t->width == 512 && t->height == 512);
    assert(n == (p ? sizeof(synthetic_jpeg_1) : sizeof(synthetic_jpeg_0)));
    assert(!memcmp(data, p ? synthetic_jpeg_1 : synthetic_jpeg_0, n));
    for (unsigned y = 0; y < 512; ++y)
        for (unsigned x = 0; x < 512; ++x)
            t->pixels[y*t->stride + x] = colors[(p + (y >= 256 ? 2 : 0) + (x >= 256)) % 4];
    return true;
}

bool __wrap_kvm_cvid_index(kvm_cvid_index_t *index, const uint8_t *packet, size_t size)
{
    if (atomic_fetch_add(&cinepak_calls, 1) == 2)
        while (atomic_load(&hold_third)) vTaskDelay(1);
    return __real_kvm_cvid_index(index, packet, size);
}

static void exercise_phases(kvm_live_codec_t codec, bool overload, unsigned frames)
{
    simulated_scan = true;
    synthetic_done = false;
    atomic_store(&cinepak_calls, 0);
    atomic_store(&hold_third, overload);
    unsigned before = atomic_load(&shown_count);
    lv_obj_t *home = lv_screen_active();
    assert(synthetic_start(codec, overload ? 60 : 5, frames, finish_synthetic, codec != KVM_LIVE_CINEPAK_INTRA));
    for (unsigned i = 0; i < 4000 && !synthetic_done; ++i) {
        if (overload && synth.sent >= 9) atomic_store(&hold_third, false);
        simulated_time_offset += 16000;
        lv_test_wait(16);
        vTaskDelay(2);
    }
    assert(synthetic_done && lv_screen_active() == home && !atomic_load(&phase_epoch));
    pthread_join(worker, NULL);
    assert(synth.verified == 2 && synth.sent == frames + 2 && synth.last_sequence == synth.sent);
    assert(!synth.invalid && synth.valid == atomic_load(&shown_count) - before - 2);
    assert(overload ? (synth.valid >= 2 && synth.valid < frames) : synth.valid == frames);
    lv_test_wait(32);
}

static void check_rejections(void)
{
    memset(&synth, 0, sizeof(synth));
    for (unsigned seq = 3; seq < PHASE_CAPACITY + 5; ++seq) {
        synth.selected_picture = (phase_picture_t){seq, 10, 11, 15};
        synth.drawing = seq;
        synth.selected_at = 20;
        synth.last_flush = 30;
        record_phases(seq, 30 + seq);
        unsigned i = (seq - 3) % PHASE_CAPACITY;
        assert(synth.durations[5][i] == 20 + seq);
        assert(synth.durations[0][i] == 1 && synth.durations[1][i] == 4 && synth.durations[2][i] == 5);
        assert(synth.durations[3][i] == 10 && synth.durations[4][i] == seq);
    }
    unsigned valid = synth.valid, seq = synth.last_sequence;
    record_phases(seq, 100); // Duplicate.
    record_phases(seq + 1, 100); // Another picture's selection.
    synth.drawing = synth.selected_picture.sequence = seq + 1;
    synth.last_flush = 0; // No last flush since selection.
    record_phases(seq + 1, 100);
    synth.last_flush = 30;
    record_phases(seq + 1, 29); // Nonmonotonic refresh.
    record_phases(seq + 1, INT64_MAX); // Delta cannot fit the bounded store.
    record_phases(1, 100); // Warmup.
    assert(synth.valid == valid && synth.invalid == 5 && synth.last_sequence == seq);
}

int main(void)
{
    check_rejections();
    lv_init();
    lv_display_t *display = lv_test_display_create(1024, 600);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    exercise_phases(KVM_LIVE_JPEG, false, PHASE_CAPACITY + 2);
    exercise_phases(KVM_LIVE_H264_INTRA, false, 6);
    exercise_phases(KVM_LIVE_CINEPAK_INTRA, false, 6);
    exercise_phases(KVM_LIVE_CINEPAK_INTRA, true, 12);
    assert(media_panel_synthetic_h264_paced(5, 6, finish_synthetic));
    media_panel_close();
    pthread_join(worker, NULL);
    lv_test_wait(32);
    assert(!synth.sent && !synth.valid && !synth.invalid && !atomic_load(&phase_epoch));
    lv_display_delete(display);
    lv_deinit();
    puts("Phase ordering, sequence gaps, warmups, bounded window and three offscreen codec paths passed.");
    return 0;
}
