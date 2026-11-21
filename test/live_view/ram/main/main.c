// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "cinepak_tile.h"
#include "live_message.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t frame_start[] asm("_binary_cinepak_frame_start");
extern const uint8_t frame_end[] asm("_binary_cinepak_frame_end");
static kvm_cvid_workspace_t workspace;
static unsigned rows;
static uint32_t crc;

static bool check_rows(void *ctx, unsigned y, const uint16_t *pixels)
{
    (void)ctx;
    if (y != rows) {
        return false;
    }
    crc = esp_rom_crc32_le(crc, (const uint8_t *)pixels, 4 * KVM_CVID_W * sizeof(*pixels));
    rows += 4;
    return true;
}

void app_main(void)
{
    // Allow the UART monitor to attach after the ROM loader releases the port.
    vTaskDelay(pdMS_TO_TICKS(4000));
    size_t len = (uintptr_t)frame_end - (uintptr_t)frame_start;
    int64_t start = esp_timer_get_time();
    const uint8_t *packet = frame_start;
    kvm_live_receiver_t rx = {0};
    const bool framed = len > KVM_LIVE_HEADER && memcmp(packet, "KVS2", 4) == 0;
    if (framed) {
        kvm_live_start(&rx, 17, KVM_LIVE_CINEPAK_INTRA);
        packet = kvm_live_receive(&rx, frame_start, len);
        len -= KVM_LIVE_HEADER;
        start = esp_timer_get_time();
    }
    bool ok = packet && kvm_cvid_decode(&workspace, packet, len, check_rows, NULL);
    printf("CVID ok=%u rows=%u crc32=%08" PRIx32 " workspace=%u input=%u decode_crc_us=%" PRId64 "\n",
           (unsigned)ok, rows, crc, (unsigned)sizeof(workspace), (unsigned)len, esp_timer_get_time() - start);
    if (framed) {
        bool replay_rejected = !kvm_live_receive(&rx, frame_start, len + KVM_LIVE_HEADER);
        kvm_live_stop(&rx);
        bool stop_rejected = !kvm_live_receive(&rx, frame_start, len + KVM_LIVE_HEADER);
        printf("KVS2 accepted=%u replay_rejected=%u stop_rejected=%u\n",
               (unsigned)(packet != NULL), (unsigned)replay_rejected, (unsigned)stop_rejected);
    }
}
