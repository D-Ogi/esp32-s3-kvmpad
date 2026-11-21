// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "hid_test_platform.h"
#include "fake_hid_host.h"
#include "hid_dispatch.h"
#include "usb_hid_device.h"
#include "media_panel.h"
#include "lvgl.h"
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate = PTHREAD_COND_INITIALIZER;
static bool armed, held, release_frame;
static unsigned decoded;
bool __real_kvm_media_h264_picture(kvm_media_decoder_t *, kvm_h264_picture_t *, const uint8_t *, size_t,
                                 kvm_media_cancel_t, void *);

bool __wrap_kvm_media_h264_picture(kvm_media_decoder_t *decoder, kvm_h264_picture_t *slot, const uint8_t *data,
                                   size_t len, kvm_media_cancel_t cancelled, void *ctx)
{
    bool ok = __real_kvm_media_h264_picture(decoder, slot, data, len, cancelled, ctx);
    pthread_mutex_lock(&lock);
    if (armed) {
        assert(ok);
        ++decoded;
        held = true;
        pthread_cond_signal(&gate);
        while (!release_frame)
            pthread_cond_wait(&gate, &lock);
    }
    pthread_mutex_unlock(&lock);
    return ok;
}

static usb_hid_transport_t usb;
static bool pump_requested;
static esp_timer_handle_t timer_tick;
static unsigned ms, base_ms;
static fake_host_t sink;
static fh_report_t pending[KVM_HID_ITFS];
typedef struct { unsigned offset; fh_report_t report; } sample_t;
static sample_t trace[256], reference[256];
static unsigned used, neutral_polls;
int64_t esp_timer_get_time(void) { return (int64_t)ms * 1000; }
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *t) { *t = args->callback; return ESP_OK; }
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t period)
{
    assert(period == 10000);
    timer_tick = t;
    return ESP_OK;
}
esp_err_t usb_hid_device_set_transport(const usb_hid_transport_t *t) { usb = *t; return ESP_OK; }
void usb_hid_device_request_pump(void) { pump_requested = true; }
uint8_t usb_hid_device_protocol(uint8_t itf) { assert(itf < KVM_HID_ITFS); return KVM_HID_PROTOCOL_REPORT; }
bool usb_hid_device_send(uint8_t itf, const uint8_t *bytes, uint8_t len)
{
    assert(itf < KVM_HID_ITFS && !pending[itf].len && len && len <= sizeof(pending[itf].b));
    pending[itf] = (fh_report_t){.itf = itf, .len = len};
    memcpy(pending[itf].b, bytes, len);
    return true;
}
static void poll_usb(void)
{
    // Complete only reports already in flight when this simulated host poll began.
    bool seen[KVM_HID_ITFS];
    for (unsigned i = 0; i < KVM_HID_ITFS; ++i) seen[i] = pending[i].len != 0;
    for (unsigned i = 0; i < KVM_HID_ITFS; ++i) {
        if (!seen[i]) continue;
        assert(used < sizeof(trace) / sizeof(*trace));
        trace[used++] = (sample_t){ms - base_ms, pending[i]};
        fh_apply(&sink, (uint8_t)i, pending[i].b, pending[i].len);
        pending[i].len = 0;
        usb.on_complete(usb.ctx, (uint8_t)i);
    }
    if (!(ms % 10)) timer_tick(NULL);
    if (pump_requested) {
        pump_requested = false;
        usb.pump(usb.ctx);
    }
}

static void le32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8*i));
}
uint32_t esp_rom_crc32_le(uint32_t, const uint8_t *, uint32_t);
static void frame(uint8_t *message, size_t size, uint32_t seq)
{
    le32(message + 12, seq);
    le32(message + 16, seq * 100);
    le32(message + 24, (uint32_t)(size - KVM_LIVE_HEADER));
    le32(message + 28, esp_rom_crc32_le(esp_rom_crc32_le(0, message, 28), message + KVM_LIVE_HEADER,
                                       (uint32_t)(size - KVM_LIVE_HEADER)));
}

static void hold_first(uint32_t epoch, uint8_t *message, size_t size)
{
    pthread_mutex_lock(&lock);
    armed = true;
    frame(message, size, 0);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    bool pushed = false;
    for (int retries = 0; retries < 100 && !pushed; ++retries) {
        pushed = media_panel_stream_push(epoch, message, size);
        if (!pushed) nanosleep(&(struct timespec){.tv_nsec=1000000}, NULL);
    }
    assert(pushed);
    while (!held)
        assert(!pthread_cond_timedwait(&gate, &lock, &deadline));
    pthread_mutex_unlock(&lock);
}

static void input_run(uint32_t epoch, uint8_t *message, size_t size, lv_obj_t *stop, unsigned mount_epoch)
{
    base_ms = ms;
    fh_init(&sink);
    usb.on_state(usb.ctx, KVM_USB_MOUNTED, mount_epoch);
    poll_usb();
    used = 0;
    base_ms = ms;
    if (epoch) hold_first(epoch, message, size);
    for (unsigned tick = 0; tick < 1000; ++tick) {
        ms = base_ms + tick;
        if (epoch && tick && tick % 100 == 0) {
            frame(message, size, tick / 100);
            assert(media_panel_stream_push(epoch, message, size));
        }
        if (tick % 10 == 0) {
            bool down = tick % 20 == 0;
            kvm_hid_intent_t key = fh_key(down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP, KVM_KEY_A);
            kvm_hid_intent_t move = fh_move(down ? 2 : -2, 0);
            assert(hid_dispatch_local(&key) == ESP_OK && hid_dispatch_local(&move) == ESP_OK);
        }
        poll_usb();
    }
    assert(used == 200 && fh_host_neutral(&sink) && sink.key_presses[KVM_KEY_A] == 50 && sink.sum_dx == 0);
    for (unsigned i = 0; i < used; ++i)
        assert(trace[i].offset == (i / 2) * 10 + 1);

    kvm_hid_intent_t key = fh_key(KVM_HID_KEY_DOWN, KVM_KEY_A);
    assert(hid_dispatch_local(&key) == ESP_OK);
    ++ms;
    poll_usb();
    assert(pending[KVM_HID_ITF_KEYBOARD].len);
    uint32_t queued = 0;
    while (hid_dispatch_local(&key) == ESP_OK) {
        assert(++queued <= 64);
    }
    assert(queued == 64);
    if (epoch) {
        lv_obj_send_event(stop, LV_EVENT_CLICKED, NULL);
        frame(message, size, 10); // Fresh CRC and sequence, admissible to an active stream.
        assert(!media_panel_stream_push(epoch, message, size));
    }
    hid_dispatch_release_all(KVM_RA_VIEW_CHANGE);
    for (unsigned i = 0; i < 20; ++i) {
        ++ms;
        poll_usb();
    }
    assert(fh_host_neutral(&sink) && sink.key_presses[KVM_KEY_A] == 51);
    assert(trace[200].offset == 1001 && trace[200].report.itf == KVM_HID_ITF_KEYBOARD &&
           fh_key_in(&trace[200].report.b[2], KVM_KEY_A));
    neutral_polls = 0;
    for (unsigned i = 201; i < used; ++i) {
        fh_report_t *r = &trace[i].report;
        for (unsigned j = 0; j < r->len; ++j) assert(!r->b[j]);
        if (r->itf == KVM_HID_ITF_KEYBOARD && !neutral_polls)
            neutral_polls = trace[i].offset - 1000;
    }
    assert(neutral_polls && neutral_polls <= 3);
    kvm_hid_snapshot_t snap;
    hid_dispatch_get_snapshot(&snap, NULL);
    assert(snap.ledger_empty);
    usb.on_state(usb.ctx, KVM_USB_DETACHED, mount_epoch);
    ms += 100 - ms % 100;
}

static unsigned expected;
void panel_test_hid_baseline(void)
{
    ms = 1000;
    assert(hid_dispatch_start() == ESP_OK);
    input_run(0, NULL, 0, NULL, 1);
    expected = used;
    memcpy(reference, trace, sizeof(reference));
}

void panel_test_hid_load(uint32_t epoch, uint8_t *message, size_t size, lv_obj_t *stop)
{
    input_run(epoch, message, size, stop, 2);
    assert(used == expected);
    for (unsigned i = 0; i < used; ++i) {
        assert(trace[i].offset == reference[i].offset && trace[i].report.itf == reference[i].report.itf &&
               trace[i].report.len == reference[i].report.len);
        assert(!memcmp(trace[i].report.b, reference[i].report.b, trace[i].report.len));
    }
    hid_dispatch_stats_t stats;
    hid_dispatch_get_stats(&stats);
    assert(stats.producer_queue_full == 2 && stats.release_requests == 2);
    pthread_mutex_lock(&lock);
    assert(decoded == 1 && held);
    release_frame = true;
    pthread_cond_signal(&gate);
    pthread_mutex_unlock(&lock);
    printf("MAG-05: 10 encoded frames, %zu bytes each, in 1000 simulated ms, first decoded frame held, 9 waiting submissions; "
           "%u completion offsets and reports matched idle baseline, 200 ordinary reports at +1 poll; "
           "64-entry producer overflow, in-flight Stop, keyboard neutral at +%u polls and stale-epoch refusal passed.\n",
           size - KVM_LIVE_HEADER, used, neutral_polls);
}

void panel_test_hid_finished(void)
{
    pthread_mutex_lock(&lock);
    assert(decoded == 1 && release_frame);
    armed = false;
    pthread_mutex_unlock(&lock);
}
