// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "esp32s3/rom/tjpgd.h"
#include "fit_inputs.h"

// The oracle has its own ROM decoder and full source raster in test RAM only.
static uint16_t source_pixels[1920 * 1200];
static _Alignas(16) uint8_t reference_work[4096];
static volatile uint32_t fit_result[27] __attribute__((used));
typedef struct { const uint8_t *data; size_t left; } reader_t;

static UINT read_reference(JDEC *dec, BYTE *dst, UINT n) {
  reader_t *r = dec->device;
  if (n > r->left) n = r->left;
  if (dst) memcpy(dst, r->data, n);
  r->data += n;
  r->left -= n;
  return n;
}

static UINT save_reference(JDEC *dec, void *rgb, JRECT *rect) {
  const uint8_t *p = rgb;
  for (unsigned y = rect->top; y <= rect->bottom; ++y)
    for (unsigned x = rect->left; x <= rect->right; ++x, p += 3)
      source_pixels[y * dec->width + x] =
          ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
  return 1;
}

static bool compare_case(const fixture_t *f, kvm_media_decoder_t *d, kvm_media_target_t *t) {
  JDEC reference;
  reader_t r = {f->start, (size_t)(f->end - f->start)};
  if (jd_prepare(&reference, read_reference, reference_work, sizeof(reference_work), &r) != JDR_OK ||
      reference.width != f->w || reference.height != f->h ||
      jd_decomp(&reference, save_reference, 0) != JDR_OK)
    return false;
  if (!kvm_media_usb_monitor_jpeg_fit(d, f->start, f->end - f->start, t, f->w, f->h))
    return false;
  unsigned scaled_w = WIDTH, scaled_h = HEIGHT;
  if (WIDTH * f->h <= HEIGHT * f->w) scaled_h = WIDTH * f->h / f->w;
  else scaled_w = HEIGHT * f->w / f->h;
  unsigned left = (WIDTH - scaled_w) / 2, top = (HEIGHT - scaled_h) / 2;
  bool nonblack = false;
  for (unsigned y = 0; y < HEIGHT; ++y) {
    for (unsigned x = 0; x < STRIDE; ++x) {
      uint16_t want = 0;
      if (x >= WIDTH) want = 0xa55a;
      else if (x >= left && x < left + scaled_w && y >= top && y < top + scaled_h) {
        unsigned sx = (x-left) * f->w / scaled_w, sy = (y-top) * f->h / scaled_h;
        want = source_pixels[sy * f->w + sx];
        nonblack |= want != 0;
      }
      if (t->pixels[y * STRIDE + x] != want) {
        fit_result[4] = x; fit_result[5] = y; fit_result[6] = want;
        fit_result[7] = t->pixels[y * STRIDE + x];
        return false;
      }
    }
  }
  for (unsigned i = 0; i < GUARD; ++i)
    if (frame[i] != 0xa55a || frame[GUARD + HEIGHT * STRIDE + i] != 0xa55a) return false;
  return kvm_media_usb_monitor_has_nonblack(d) == nonblack && nonblack == f->visible_color;
}

void qualify(void) {
  for (unsigned i = 0; i < sizeof(frame)/sizeof(*frame); ++i) frame[i] = 0xa55a;
  kvm_media_target_t t = {.pixels = frame + GUARD, .width = WIDTH, .height = HEIGHT, .stride = STRIDE};
  kvm_media_decoder_t *d = kvm_media_decoder_open(KVM_LIVE_JPEG);
  if (!d) goto done;
  for (unsigned i = 0; i < sizeof(fixtures)/sizeof(*fixtures); ++i) {
    fit_result[2] = i;
    if (!compare_case(&fixtures[i], d, &t)) goto done;
    ++fit_result[1];
  }
  const fixture_t *f = &fixtures[0];
  size_t size = f->end - f->start;
  // A wrong SOF declaration must fail before touching the old target.
  memset(t.pixels, 0xaa, HEIGHT * STRIDE * sizeof(*t.pixels));
  if (kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, f->w + 1, f->h) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, f->w, f->h + 1) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, 1921, f->h) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, f->w, 1201) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, 0, f->h) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, size - 2, &t, f->w, f->h) ||
      kvm_media_usb_monitor_jpeg_fit(d, f->start, KVM_LIVE_MAX_PAYLOAD + 1, &t, f->w, f->h))
    goto done;
  t.stride = WIDTH - 1;
  if (kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, f->w, f->h)) goto done;
  t.stride = STRIDE;
  for (unsigned i = 0; i < HEIGHT * STRIDE; ++i) if (t.pixels[i] != 0xaaaa) goto done;
  unsigned calls = 0;
  t.ctx = &calls; t.cancelled = cancel_during_decode;
  if (kvm_media_usb_monitor_jpeg_fit(d, f->start, size, &t, f->w, f->h) || calls != 128 ||
      kvm_media_usb_monitor_has_nonblack(d)) goto done;
  t.cancelled = NULL;
  for (unsigned i = 0; i < sizeof(frame)/sizeof(*frame); ++i) frame[i] = 0xa55a;
  if (!compare_case(f, d, &t)) goto done;
  kvm_media_decoder_close(d);
  if (!allocated) fit_result[3] = 1;
done:
  fit_result[0] = 0x4a504547;
}
