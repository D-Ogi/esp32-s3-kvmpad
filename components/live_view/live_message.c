// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "live_message.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#define crc32_le esp_rom_crc32_le
#else
static uint32_t crc32_le(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & UINT32_C(0xedb88320));
        }
    }
    return ~crc;
}
#endif

static uint32_t le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (le16(p + 2) << 16);
}

kvm_live_geometry_t kvm_live_geometry(kvm_live_codec_t codec, const char *mode)
{
    if (!mode || codec < KVM_LIVE_JPEG || codec > KVM_LIVE_CINEPAK_INTRA)
        return (kvm_live_geometry_t){0};
    unsigned width = codec == KVM_LIVE_CINEPAK_INTRA ? 960 : 512;
    unsigned height = codec == KVM_LIVE_CINEPAK_INTRA ? 540 : 512;
    unsigned factor;
    if (!strcmp(mode, "OVERVIEW")) factor = 4;
    else if (!strcmp(mode, "PIXEL_1X")) factor = 2;
    else if (!strcmp(mode, "ZOOM_2X")) factor = 1;
    else return (kvm_live_geometry_t){0};
    return (kvm_live_geometry_t){width * factor / 2, height * factor / 2, width, height};
}

void kvm_live_stop(kvm_live_receiver_t *rx)
{
    if (rx) {
        *rx = (kvm_live_receiver_t){0};
    }
}

bool kvm_live_start(kvm_live_receiver_t *rx, uint32_t stream_id, kvm_live_codec_t codec)
{
    kvm_live_stop(rx);
    if (!rx || !stream_id || codec < KVM_LIVE_JPEG || codec > KVM_LIVE_CINEPAK_INTRA) {
        return false;
    }
    rx->stream_id = stream_id;
    rx->codec = codec;
    return true;
}

const uint8_t *kvm_live_receive(kvm_live_receiver_t *rx, const uint8_t *p, size_t size)
{
    if (!rx || !rx->stream_id || !p || size <= KVM_LIVE_HEADER || size > KVM_LIVE_HEADER + KVM_LIVE_MAX_PAYLOAD) {
        return NULL;
    }
    const bool jpeg = rx->codec == KVM_LIVE_JPEG;
    const bool cinepak = rx->codec == KVM_LIVE_CINEPAK_INTRA;
    const size_t len = size - KVM_LIVE_HEADER;
    if (memcmp(p, jpeg ? "KVS1" : "KVS2", 4) || p[4] != (jpeg ? 1 : 2) || p[5] != rx->codec ||
        (p[6] != 1 && !(jpeg && p[6] == 0)) || p[7] != 0 || le32(p + 8) != rx->stream_id ||
        le16(p + 20) != (cinepak ? 960u : 512u) || le16(p + 22) != (cinepak ? 540u : 512u) ||
        le32(p + 24) != len) {
        return NULL;
    }
    const uint32_t sequence = le32(p + 12);
    const uint32_t delta = sequence - rx->sequence;
    if (rx->received && (delta == 0 || delta >= UINT32_C(0x80000000))) {
        return NULL;
    }
    const uint8_t *payload = p + KVM_LIVE_HEADER;
    if (le32(p + 28) != crc32_le(crc32_le(0, p, 28), payload, len)) {
        return NULL;
    }
    rx->received = true;
    rx->sequence = sequence;
    return payload;
}
