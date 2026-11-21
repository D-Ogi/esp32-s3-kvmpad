// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "libavutil/log.h"
#include "libavutil/sha.h"
#include "media_decode.h"
#include "multi_heap.h"
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <time.h>
extern const unsigned char clip[], clip_end[];
// An embedded, read-only MP4 supplies the normal decoder FILE callbacks.
static size_t pos;
int __wrap_fseek(FILE *f, long offset, int whence) {
  (void)f;
  long length = clip_end - clip;
  if (whence != SEEK_CUR && whence != SEEK_END && whence != SEEK_SET)
    return -1;
  int64_t base = whence == SEEK_CUR ? (long)pos : whence == SEEK_END ? length : 0;
  int64_t next = base + offset;
  if (next < 0 || next > length)
    return -1;
  pos = next;
  return 0;
}
long __wrap_ftell(FILE *f) {
  (void)f;
  return pos;
}
int __wrap_ferror(FILE *f) {
  (void)f;
  return 0;
}
size_t __wrap_fread(void *p, size_t size, size_t n, FILE *f) {
  (void)f;
  if (size != 1)
    return 0;
  size_t avail = clip_end - clip - pos;
  if (n > avail)
    n = avail;
  memcpy(p, clip + pos, n);
  pos += n;
  return n;
}
// Match unit 1 after its two RGB565 surfaces have been allocated.
#ifdef KVM_TEST_FRAGMENTED_LIVE
static _Alignas(16) uint8_t arena[1638400], auxiliary[196608];
#else
static _Alignas(16) uint8_t arena[1802240];
#endif
volatile struct {
  uint32_t magic, state, count, guard;
  uint8_t digest[32];
  uint32_t min_free;
  uint32_t preview_fps_milli, preview_min_free, preview_width, preview_height;
} result;
static int allocated;
#ifdef KVM_TEST_FRAGMENTED_LIVE
static bool card_mode, refuse_extra;
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 1985008; }
size_t heap_caps_get_largest_free_block(unsigned caps) {
  (void)caps; return sizeof(arena) + (card_mode ? 65536 : 0);
}
void *heap_caps_malloc(size_t n, unsigned caps) {
  (void)caps;
  if (!allocated && n == sizeof(arena)) { allocated = 1; return arena; }
  if (allocated == 1 && n == sizeof(auxiliary) && !refuse_extra) { allocated = 3; return auxiliary; }
  return NULL;
}
void heap_caps_free(void *p) {
  if (!p) return;
  size_t n = p == arena ? sizeof(arena) : sizeof(auxiliary);
  unsigned bit = p == arena ? 1 : 2;
  if (!(allocated & bit) || (p != arena && p != auxiliary)) { ++result.guard; return; }
  allocated &= ~bit;
  if (card_mode)
    result.guard += p != arena || !multi_heap_check((multi_heap_handle_t)p, false);
  else
    for (size_t i = 0; i < n; ++i) if (((uint8_t *)p)[i]) { ++result.guard; break; }
}
#else
static bool checking_preview;
#ifdef KVM_TEST_ANNEXB_PREVIEW
static const bool preview_mp4 = false;
#else
static const bool preview_mp4 = true;
#endif
size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return sizeof(arena) + 65536; }
size_t heap_caps_get_largest_free_block(unsigned caps) {
  (void)caps;
  return sizeof(arena) + 65536;
}
void *heap_caps_malloc(size_t n, unsigned caps) {
  (void)caps;
  size_t expected = checking_preview ? (preview_mp4 ? 256u : 512u) * 1024 : sizeof(arena);
  if (allocated || n != expected)
    return NULL;
  allocated = 1;
  return arena;
}
void heap_caps_free(void *p) {
  if (!p)
    return;
  if (!allocated || p != arena) {
    ++result.guard;
    return;
  }
  result.guard += !multi_heap_check((multi_heap_handle_t)p, false);
  result.min_free = multi_heap_minimum_free_size((multi_heap_handle_t)p);
  if (checking_preview) result.preview_min_free = result.min_free;
  allocated = 0;
}
#endif
void kvm_media_zeroize(void *p, size_t n) { explicit_bzero(p, n); }
// The decoder privately owns its heap. An unexpected OS lock or ISR call fails
// the test.
int xPortEnterCriticalTimeout(void *p, int t) {
  (void)p;
  (void)t;
  __builtin_trap();
}
void vPortExitCritical(void *p) {
  (void)p;
  __builtin_trap();
}
int xPortInIsrContext(void) { __builtin_trap(); }
int esp_rom_printf(const char *f, ...) {
  (void)f;
  return 0;
}
static void quiet(void *a, int level, const char *m, va_list v) {
  (void)a;
  (void)level;
  (void)m;
  (void)v;
}
static _Alignas(16) uint8_t ctx[512];
static bool emit(void *arg, const kvm_media_yuv_t *f) {
  (void)arg;
  for (int p = 0; p < 3; p++) {
    unsigned w = f->width >> (p != 0), h = f->height >> (p != 0);
    for (unsigned y = 0; y < h; y++)
      av_sha_update((void *)ctx, f->plane[p] + y * f->stride[p], w);
  }
  result.count++;
  return true;
}
void qualify(void) {
  result.magic = 1;
  av_log_set_callback(quiet);
  if (av_sha_size > sizeof(ctx))
    return;
  av_sha_init((void *)ctx, 256);
#ifdef KVM_TEST_FRAGMENTED_LIVE
  extern bool kvm_h264_live_open(void);
  extern void kvm_h264_live_close(void);
  extern kvm_play_result_t kvm_h264_heap_open(bool);
  extern bool kvm_h264_heap_close(bool);
  extern bool kvm_h264_retain(kvm_h264_picture_t *, const uint8_t *, size_t, kvm_media_cancel_t, void *);
  extern void *kvm_av_malloc(size_t), *kvm_av_realloc(void *, size_t);
  extern void kvm_av_free(void *);
  refuse_extra = true;
  result.guard += kvm_h264_live_open() || allocated;
  refuse_extra = false;
  for (int cycle = 0; cycle < 2; ++cycle) {
    if (!kvm_h264_live_open()) { ++result.guard; break; }
    uint8_t *p = kvm_av_malloc(100 * 1024);
    if (p) {
      memset(p, 0xc5, 100 * 1024);
      uint8_t *grown = kvm_av_realloc(p, 300 * 1024);
      if (grown) {
        for (unsigned i = 0; i < 100 * 1024; ++i) if (grown[i] != 0xc5) { ++result.guard; break; }
        p = grown;
      } else ++result.guard;
      kvm_av_free(p);
    } else ++result.guard;
    kvm_h264_picture_t slots[2] = {0};
    const uint8_t *next = clip;
    unsigned seq = 0;
    while (clip_end - next >= 4) {
      uint32_t size;
      memcpy(&size, next, 4);
      next += 4;
      if (size > (size_t)(clip_end - next) || size > 98304 ||
          !kvm_h264_retain(&slots[seq % 2], next, size, NULL, NULL)) break;
      // Both the current and previously retained picture must remain intact.
      emit(NULL, &slots[seq % 2].yuv);
      if (seq) emit(NULL, &slots[(seq - 1) % 2].yuv);
      next += size;
      ++seq;
    }
    result.state = next == clip_end;
    kvm_media_h264_release(&slots[0]);
    kvm_media_h264_release(&slots[1]);
    result.guard += !multi_heap_check((multi_heap_handle_t)arena, false) ||
                    !multi_heap_check((multi_heap_handle_t)auxiliary, false);
    result.min_free = multi_heap_minimum_free_size((multi_heap_handle_t)arena) +
                      multi_heap_minimum_free_size((multi_heap_handle_t)auxiliary);
    kvm_h264_live_close();
    result.guard += allocated != 0;
    // A file gets the original single arena even after a split live grant ended.
    card_mode = true;
    if (kvm_h264_heap_open(false) != KVM_PLAY_OK || allocated != 1) { ++result.guard; break; }
    p = kvm_av_malloc(4096);
    if (p) memset(p, 0xa5, 4096); else ++result.guard;
    kvm_av_free(p);
    result.guard += kvm_h264_heap_close(false) || allocated;
    card_mode = false;
  }
#else
  FILE f = {0};
  kvm_media_playback_t s = {.frame = emit, .reconstruction = KVM_H264_EXACT};
  unsigned w, h;
#ifdef KVM_TEST_SOURCE_PREVIEW
  kvm_h264_source_info_t info = {0};
  checking_preview = true;
  result.state = kvm_media_h264_inspect(&f, preview_mp4, &info, NULL, NULL);
  result.guard += allocated != 0 || pos != 0;
  result.preview_fps_milli = info.fps_milli;
  result.preview_width = info.width;
  result.preview_height = info.height;
  checking_preview = false;
  if (!preview_mp4 || result.state != KVM_PLAY_OK) {
    result.magic = 0x4d454d31;
    return;
  }
#endif
  if (kvm_media_h264_probe(&f, true, &w, &h, NULL, NULL) != KVM_PLAY_OK ||
      w != 640 || h != 360)
    return;
  result.state = kvm_media_play_h264_stats(&f, true, &s, NULL);
#endif
  av_sha_final((void *)ctx, (void *)result.digest);
  result.magic = 0x4d454d31;
}
int clock_gettime(clockid_t id, struct timespec *out) {
  (void)id;
  out->tv_sec = 0;
  out->tv_nsec = 0;
  return 0;
}
