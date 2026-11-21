// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "usb_jpeg_check.h"
#include "monitor_wire.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include <string.h>

usb_jpeg_result_t usb_jpeg_check(const uint8_t *jpeg, size_t length)
{
    if (!jpeg && !length) return USB_JPEG_ABSENT;
    if (!jpeg || length < 4 || length > MONITOR_JPEG_LIMIT ||
        jpeg[0] != 0xff || jpeg[1] != 0xd8 || jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9)
        return USB_JPEG_FAILED;

    // The library accepts a mutable input buffer. Keep even unsuccessful
    // independent decodes isolated from the JPEG owned by the USB worker.
    uint8_t *input = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!input) return USB_JPEG_FAILED;
    memcpy(input, jpeg, length);

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.block_enable = true;
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_io_t io = {.inbuf = input, .inbuf_len = (int)length};
    jpeg_dec_header_info_t header = {0};
    jpeg_dec_handle_t decoder = NULL;
    uint8_t *stripe = NULL;
    int bytes = 0, blocks = 0;
    usb_jpeg_result_t result = USB_JPEG_FAILED;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) goto finish;
    if (jpeg_dec_parse_header(decoder, &io, &header) != JPEG_ERR_OK ||
        !((header.width == monitor_width(false) && header.height == monitor_height(false)) ||
          (header.width == monitor_width(true) && header.height == monitor_height(true)))) goto finish;

    const int row_bytes = header.width * 2;
    int remaining = header.height * row_bytes;
    if (jpeg_dec_get_outbuf_len(decoder, &bytes) != JPEG_ERR_OK ||
        (bytes != 8 * row_bytes && bytes != 16 * row_bytes) ||
        jpeg_dec_get_process_count(decoder, &blocks) != JPEG_ERR_OK ||
        blocks != (remaining + bytes - 1) / bytes) goto finish;

    stripe = jpeg_calloc_align(bytes, 16);
    if (!stripe) goto finish;
    io.outbuf = stripe;
    unsigned nonblack = 0;
    for (int block = 0; block < blocks; ++block) {
        io.out_size = 0;
        const int valid = remaining < bytes ? remaining : bytes;
        if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK || (io.out_size != valid && io.out_size != bytes)) goto finish;
        for (int i = 0; i < valid; ++i) nonblack |= stripe[i];
        remaining -= valid;
    }
    result = nonblack ? USB_JPEG_HAS_NONBLACK : USB_JPEG_ALL_BLACK;

finish:
    if (decoder && jpeg_dec_close(decoder) != JPEG_ERR_OK) result = USB_JPEG_FAILED;
    if (stripe) jpeg_free_align(stripe);
    heap_caps_free(input);
    return result;
}
