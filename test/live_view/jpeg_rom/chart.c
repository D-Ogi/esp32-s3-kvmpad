// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_decode.h"
#include <string.h>

extern unsigned char chart[], chart_end[];
#ifdef CHECK_MONITOR_COLOR
extern unsigned char solid[], solid_end[];
#endif
#ifdef USB_FIT_TEST
enum { WIDTH = 1024, HEIGHT = 600 };
#elif defined(USB_MONITOR_TEST)
enum { WIDTH = 640, HEIGHT = 480 };
#define decode kvm_media_usb_monitor_jpeg
#else
enum { WIDTH = 512, HEIGHT = 512 };
#define decode kvm_media_decoder_frame
#endif
enum { CELL_W = WIDTH / 4, CELL_H = HEIGHT / 2, STRIDE = WIDTH + 7, GUARD = 32 };
static uint16_t frame[GUARD + HEIGHT * STRIDE + GUARD];
static _Alignas(16) uint8_t arena[8192];
static bool allocated;
static volatile struct {
  uint32_t signature, decoded, colors[8], wrong_pixels, guard_errors;
  uint32_t missing_eoi_accepted, short_scan_accepted, invalid_stride_accepted,
      leaked, recovered;
  uint32_t decode_ticks, cancelled, partial, callbacks, rgb_hash;
  uint32_t first_nonblack, solid_decoded, solid_nonblack, retained_nonblack, recovered_nonblack;
} result __attribute__((used));

void *heap_caps_malloc(size_t bytes, unsigned caps) {
  (void)caps;
  if (allocated || bytes > sizeof(arena))
    return NULL;
  allocated = true;
  return arena;
}

void free(void *ptr) {
  if (ptr == arena)
    allocated = false;
}

#ifdef TEST_FIRMWARE_OBJECT
// The full firmware object references H.264 when opening, decoding and closing.
bool kvm_h264_live_open(void) { return false; }
void kvm_h264_live_close(void) {}
bool kvm_h264_idr(const uint8_t *data, size_t size, const kvm_media_target_t *target) {
  (void)data;
  (void)size;
  (void)target;
  return false;
}
uintptr_t __stack_chk_guard = 0xab397551;
void __stack_chk_fail(void) { __builtin_trap(); }
#endif

static uint32_t ticks(void) {
  uint32_t value;
  __asm__ volatile("rsr.ccount %0" : "=a"(value) :: "memory");
  return value;
}

static bool cancel_during_decode(void *ctx) {
  unsigned *calls = ctx;
  return ++*calls >= 128;
}

#ifdef USB_FIT_TEST
#include "fit_cases.inc"
#else
void qualify(void) {
  result.signature = 1;
  size_t size = (size_t)(chart_end - chart);
  for (unsigned i = 0; i < sizeof(frame) / sizeof(*frame); ++i)
    frame[i] = 0xa55a;
  kvm_media_target_t t = {
      .pixels = frame + GUARD, .width = WIDTH, .height = HEIGHT, .stride = STRIDE};
  kvm_media_decoder_t *d = kvm_media_decoder_open(KVM_LIVE_JPEG);
  uint32_t start = ticks();
  result.decoded = decode(d, chart, size, &t);
  result.decode_ticks = ticks() - start;
#ifdef CHECK_MONITOR_COLOR
  result.first_nonblack = kvm_media_usb_monitor_has_nonblack(d);
#endif
  uint32_t hash = 2166136261u;
  for (unsigned y = 0; y < HEIGHT; ++y)
    for (unsigned x = 0; x < WIDTH; ++x)
      hash = (hash ^ t.pixels[y * STRIDE + x]) * 16777619u;
  result.rgb_hash = hash;
  for (unsigned i = 0; i < 8; ++i)
    result.colors[i] =
        t.pixels[((i / 4) * CELL_H + CELL_H / 2) * STRIDE + (i % 4) * CELL_W + CELL_W / 2];
  for (unsigned y = 0; y < HEIGHT; ++y)
    for (unsigned x = 0; x < STRIDE; ++x)
      if (x < WIDTH)
        result.wrong_pixels += t.pixels[y * STRIDE + x] !=
            result.colors[(y >= CELL_H ? 4 : 0) + (x >= CELL_W) + (x >= 2 * CELL_W) + (x >= 3 * CELL_W)];
      else
        result.guard_errors += t.pixels[y * STRIDE + x] != 0xa55a;
  for (unsigned i = 0; i < GUARD; ++i) {
    result.guard_errors += frame[i] != 0xa55a;
    result.guard_errors += frame[GUARD + HEIGHT * STRIDE + i] != 0xa55a;
  }
#ifdef CHECK_MONITOR_COLOR
  result.solid_decoded = decode(d, solid, (size_t)(solid_end - solid), &t);
  result.solid_nonblack = kvm_media_usb_monitor_has_nonblack(d);
#endif
  result.missing_eoi_accepted = decode(d, chart, size - 2, &t);
  size_t short_size = size / 2;
  unsigned char saved[2] = {chart[short_size - 2], chart[short_size - 1]};
  chart[short_size - 2] = 0xff;
  chart[short_size - 1] = 0xd9;
  result.short_scan_accepted =
      decode(d, chart, short_size, &t);
  memcpy(chart + short_size - 2, saved, sizeof(saved));
  t.stride = WIDTH - 1;
  result.invalid_stride_accepted = decode(d, chart, size, &t);
  t.stride = STRIDE;
  unsigned calls = 0;
  for (unsigned y = 0; y < HEIGHT; ++y)
    for (unsigned x = 0; x < WIDTH; ++x)
      t.pixels[y * STRIDE + x] = 0xa55a;
  t.ctx = &calls;
  t.cancelled = cancel_during_decode;
  result.cancelled = !decode(d, chart, size, &t);
  result.callbacks = calls;
  result.partial = t.pixels[0] != 0xa55a && t.pixels[(HEIGHT - 1) * STRIDE + WIDTH - 1] == 0xa55a;
#ifdef CHECK_MONITOR_COLOR
  result.retained_nonblack = kvm_media_usb_monitor_has_nonblack(d);
#endif
  t.cancelled = NULL;
  t.ctx = NULL;
  result.recovered = decode(d, chart, size, &t);
#ifdef CHECK_MONITOR_COLOR
  result.recovered_nonblack = kvm_media_usb_monitor_has_nonblack(d);
#endif
  kvm_media_decoder_close(d);
  result.leaked = allocated;
  result.signature = 0x4a504547;
}
#endif
