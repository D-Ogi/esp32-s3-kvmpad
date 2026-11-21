// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "usb_jpeg_decode.h"
#include "monitor_wire.h"
#include "esp_jpeg_dec.h"

#ifdef KVM_USB_MOTION_CHECK
#include "esp_timer.h"
#include <stdatomic.h>
bool usb_jpeg_test_scalar, usb_jpeg_test_rows_only, usb_jpeg_test_vector, usb_jpeg_test_cells;
static atomic_uint last_process, last_black, last_damage;
void usb_monitor_test_pixel_path(bool scalar, bool rows_only, bool vector, bool cells)
{
    usb_jpeg_test_scalar = scalar;
    usb_jpeg_test_rows_only = rows_only;
    usb_jpeg_test_vector = vector;
    usb_jpeg_test_cells = cells;
}

// Exercise each byte lane across adjacent groups before timing the S3 routine.
bool usb_monitor_test_equal_blocks(void)
{
    _Alignas(16) uint8_t a[128], b[128];
    for (unsigned i = 0; i < sizeof(a); ++i) a[i] = b[i] = (uint8_t)(19u * i + 31u);
    if (!usb_jpeg_equal_bytes(a, b, sizeof(a))) return false;
    for (unsigned i = 0; i < sizeof(a); ++i) {
        b[i] ^= 0x80;
        if (usb_jpeg_equal_bytes(a, b, sizeof(a))) return false;
        b[i] = a[i];
    }
    return true;
}
void usb_monitor_test_decode_times(unsigned *jpeg_us, unsigned *black_us, unsigned *damage_us)
{
    *jpeg_us = atomic_load(&last_process);
    *black_us = atomic_load(&last_black);
    *damage_us = atomic_load(&last_damage);
}
#endif

static bool running(const kvm_media_target_t *t)
{
    return !t->cancelled || !t->cancelled(t->ctx);
}

usb_jpeg_decode_result_t usb_jpeg_decode(uint8_t *input, size_t length, const kvm_media_target_t *t,
                                        size_t capacity, bool *has_nonblack, usb_jpeg_damage_t *tracking)
{
    if (!input || length < 4 || length > MONITOR_JPEG_LIMIT || input[0] != 0xff || input[1] != 0xd8 ||
        input[length - 2] != 0xff || input[length - 1] != 0xd9 || !t || !has_nonblack || !t->pixels ||
        ((uintptr_t)t->pixels & 15) || t->stride != t->width ||
        !((t->width == 640 && t->height == 480) || (t->width == 1024 && t->height == 600)) || !running(t))
        return USB_JPEG_INVALID;

    if (tracking && (!tracking->previous || ((uintptr_t)tracking->stripe & 15) ||
                     (tracking->previous == t->pixels) != (tracking->stripe != NULL))) return USB_JPEG_INVALID;

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.block_enable = true;
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = NULL;
    jpeg_error_t error = jpeg_dec_open(&config, &decoder);
    if (error != JPEG_ERR_OK) return USB_JPEG_UNAVAILABLE;

    jpeg_dec_io_t io = {.inbuf = input, .inbuf_len = (int)length};
    jpeg_dec_header_info_t header;
    int bytes = 0, blocks = 0;
    const int stride = (int)t->width * 2;
    size_t remaining = (size_t)t->height * stride;
    bool nonblack = false;
    usb_jpeg_decode_result_t result = USB_JPEG_INVALID;

    error = jpeg_dec_parse_header(decoder, &io, &header);
    if (error != JPEG_ERR_OK || header.width != t->width || header.height != t->height) goto finish;
    if (jpeg_dec_get_outbuf_len(decoder, &bytes) != JPEG_ERR_OK || (bytes != 8*stride && bytes != 16*stride) ||
        jpeg_dec_get_process_count(decoder, &blocks) != JPEG_ERR_OK ||
        blocks != (int)((remaining + bytes - 1) / bytes) || (size_t)blocks * bytes > capacity) goto finish;

#ifdef KVM_USB_MOTION_CHECK
    unsigned process_us = 0, black_us = 0, damage_us = 0;
#endif
    for (int block = 0; block < blocks; ++block) {
        if (!running(t)) goto finish;
        uint16_t *destination = t->pixels + (size_t)block * bytes / sizeof(*t->pixels);
        io.outbuf = (uint8_t *)(tracking && tracking->stripe ? tracking->stripe : destination);
        io.out_size = 0;
#ifdef KVM_USB_MOTION_CHECK
        int64_t start = esp_timer_get_time();
#endif
        error = jpeg_dec_process(decoder, &io);
        const int valid = remaining < (size_t)bytes ? (int)remaining : bytes;
        if (error != JPEG_ERR_OK || (io.out_size != valid && io.out_size != bytes)) goto finish;
#ifdef KVM_USB_MOTION_CHECK
        int64_t classified = esp_timer_get_time();
        process_us += classified - start;
#endif
        if (!nonblack) {
#ifdef KVM_USB_MOTION_CHECK
            if (usb_jpeg_test_scalar) {
                const uint16_t *row = (const uint16_t *)io.outbuf;
                for (int i = 0; i < valid/2; ++i) {
                    if (row[i]) { nonblack = true; break; }
                }
            } else
#endif
                nonblack = usb_jpeg_any_nonzero(io.outbuf, (size_t)valid);
        }
#ifdef KVM_USB_MOTION_CHECK
        int64_t compared = esp_timer_get_time();
        black_us += compared - classified;
#endif
        if (tracking) {
            unsigned top = (unsigned)block * (unsigned)(bytes / stride);
            usb_jpeg_damage_band(tracking, (const uint16_t *)io.outbuf, tracking->stripe ? destination : NULL,
                                 t->width, top, (unsigned)(valid / stride));
        }
#ifdef KVM_USB_MOTION_CHECK
        damage_us += esp_timer_get_time() - compared;
#endif
        remaining -= valid;
    }
#ifdef KVM_USB_MOTION_CHECK
    atomic_store(&last_process, process_us);
    atomic_store(&last_black, black_us);
    atomic_store(&last_damage, damage_us);
#endif
    if (running(t)) result = USB_JPEG_OK;

finish:
    if (error == JPEG_ERR_NO_MEM || error == JPEG_ERR_UNSUPPORT_FMT || error == JPEG_ERR_UNSUPPORT_STD)
        result = USB_JPEG_UNAVAILABLE;
    if (jpeg_dec_close(decoder) != JPEG_ERR_OK) result = USB_JPEG_INVALID;
    if (result == USB_JPEG_OK) *has_nonblack = nonblack;
    return result;
}
