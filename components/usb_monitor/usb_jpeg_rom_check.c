// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "usb_jpeg_check.h"
#include "monitor_wire.h"
#include "esp32s3/rom/tjpgd.h"
#include "esp_heap_caps.h"
#include <string.h>

typedef struct {
    const uint8_t *next;
    size_t left;
    const kvm_media_target_t *target;
    bool nonblack;
} reference_t;

static UINT read_bytes(JDEC *decoder, BYTE *dst, UINT count)
{
    reference_t *r = decoder->device;
    if (r->target->cancelled && r->target->cancelled(r->target->ctx)) return 0;
    if (count > r->left) count = r->left;
    if (dst) memcpy(dst, r->next, count);
    r->next += count;
    r->left -= count;
    return count;
}

static UINT classify_block(JDEC *decoder, void *rgb, JRECT *rect)
{
    reference_t *r = decoder->device;
    if (r->target->cancelled && r->target->cancelled(r->target->ctx)) return 0;
    if (!r->nonblack) {
        unsigned count = (rect->right - rect->left + 1) * (rect->bottom - rect->top + 1);
        const uint8_t *p = rgb;
        for (unsigned i = 0; i < count; ++i, p += 3) {
            if ((p[0] >> 3) || (p[1] >> 2) || (p[2] >> 3)) { r->nonblack = true; break; }
        }
    }
    return 1;
}

usb_jpeg_result_t usb_jpeg_check_rom(const uint8_t *jpeg, size_t length, const kvm_media_target_t *target)
{
    if (!jpeg || length < 4 || length > MONITOR_JPEG_LIMIT || !target) return USB_JPEG_FAILED;
    void *workspace = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace) return USB_JPEG_FAILED;
    JDEC decoder;
    reference_t r = {.next = jpeg, .left = length, .target = target};
    bool ok = jd_prepare(&decoder, read_bytes, workspace, 4096, &r) == JDR_OK &&
              decoder.width == target->width && decoder.height == target->height &&
              jd_decomp(&decoder, classify_block, 0) == JDR_OK;
    heap_caps_free(workspace);
    if (!ok) return USB_JPEG_FAILED;
    return r.nonblack ? USB_JPEG_HAS_NONBLACK : USB_JPEG_ALL_BLACK;
}
