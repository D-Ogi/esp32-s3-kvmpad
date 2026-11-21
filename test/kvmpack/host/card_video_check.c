// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "pack_video.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static bool stop(void *ctx) { return ctx != NULL; }

static bool write_picture(unsigned index, uint64_t pts, uint32_t crc, const uint16_t *pixels, size_t size)
{
    uint8_t record[16];
    for (unsigned i = 0; i < 4; i++) {
        record[i] = (uint8_t)(index >> (8 * i));
        record[12 + i] = (uint8_t)(crc >> (8 * i));
    }
    for (unsigned i = 0; i < 8; i++)
        record[4 + i] = (uint8_t)(pts >> (8 * i));
    return fwrite(record, 1, sizeof(record), stdout) == sizeof(record) &&
           fwrite(pixels, 1, size, stdout) == size;
}

int main(int argc, char **argv)
{
    if (argc != 4 && argc != 5) return 2;
    if (argc == 5 && (strlen(argv[4]) != 1 || argv[4][0] < '0' || argv[4][0] > '4')) return 2;
    unsigned mode = argc == 5 ? (unsigned)(argv[4][0] - '0') : 0;
    bool single_unaligned = !strcmp(argv[3], "pixels-one-unaligned");
    bool single = single_unaligned || !strcmp(argv[3], "pixels-one");
    bool borrowed = !strcmp(argv[3], "pixels-two-window");
    bool unaligned = single_unaligned || !strcmp(argv[3], "pixels-two-unaligned");
    bool pixels = single || borrowed || unaligned || !strcmp(argv[3], "pixels-two");
    bool cancel = !strcmp(argv[3], "cancel");
    if (!pixels && !cancel && strcmp(argv[3], "read")) return 2;
    if (pixels) {
        // KVC1 and the native H.264 RGB565 oracle use little-endian pixels.
        const uint16_t one = 1;
        if (*(const uint8_t *)&one != 1) return 2;
#ifdef _WIN32
        if (_setmode(_fileno(stdout), _O_BINARY) < 0) return 2;
#endif
    }
    FILE *source = fopen(argv[1], "rb"), *cache = fopen(argv[2], "rb");
    if (!source || !cache) return 2;
    pack_video_t *video = NULL;
    pack_video_info_t info;
    pack_video_window_t window;
    esp_err_t opened = borrowed ? pack_video_open_with_window(source, cache, mode, NULL, &window, &video, &info)
                                : pack_video_open(source, cache, mode, NULL, &video, &info);
    bool ok = opened == ESP_OK && ftell(source) == 0;
    uint16_t *slots[2] = {0};
    unsigned slot_count = single ? 1 : 2;
    size_t bytes = 0;
    if (ok) {
        bytes = (size_t)info.width * info.height * 2;
        for (unsigned i = 0; ok && i < slot_count; ++i) {
            slots[i] = malloc(bytes + (unaligned ? sizeof(uint16_t) : 0));
            ok = slots[i] != NULL;
        }
    }
    for (unsigned i = 0; ok && i < info.frames; ++i) {
        uint64_t pts;
        uint32_t crc;
        uint16_t *output = slots[i % slot_count] + unaligned;
        ok = pack_video_read(video, output, i ? slots[(i - 1) % slot_count] + unaligned : NULL, &pts, &crc, stop,
                             cancel ? video : NULL);
        if (ok && pixels)
            ok = write_picture(i, pts, crc, output, bytes);
        else if (ok)
            printf("%u %" PRIu64 " %08" PRIx32 "\n", i, pts, crc);
    }
    free(slots[0]);
    free(slots[1]);
    pack_video_close(video);
    fclose(cache);
    fclose(source);
    return ok && fflush(stdout) == 0 ? 0 : 1;
}
