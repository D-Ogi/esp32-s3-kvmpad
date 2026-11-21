// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "media_decode.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#endif

typedef struct {
    kvm_media_target_t target;
    bool half;
} writer_t;

static bool word(unsigned value)
{
    uint8_t data[4] = {value, value >> 8, value >> 16, value >> 24};
    return fwrite(data, 1, sizeof(data), stdout) == sizeof(data);
}

static bool emit(void *context, const kvm_media_yuv_t *frame)
{
    writer_t *w = context;
    if (!kvm_media_render_yuv(frame, w->half, &w->target))
        return false;
    size_t count = (size_t)w->target.width * w->target.height;
    unsigned duration = frame->duration_us > 0 && frame->duration_us <= 1000000 ? (unsigned)frame->duration_us : 41667;
    uint64_t pts = (uint64_t)frame->pts_us;
    return word(duration) && word((unsigned)pts) && word((unsigned)(pts >> 32)) &&
           fwrite(w->target.pixels, sizeof(uint16_t), count, stdout) == count;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 2;
    bool mp4 = !strcmp(argv[1], "mp4");
    kvm_h264_reconstruction_t mode;
    if (!strcmp(argv[2], "exact")) mode = KVM_H264_EXACT;
    else if (!strcmp(argv[2], "reduced")) mode = KVM_H264_REDUCED;
    else if (!strcmp(argv[2], "bank")) mode = KVM_H264_LARGE_BANK;
    else if (!strcmp(argv[2], "rr_blend")) mode = KVM_H264_RR_BLEND;
    else if (!strcmp(argv[2], "rr_smart_loose")) mode = KVM_H264_RR_SMART_LOOSE;
    else return 2;
    const uint16_t one = 1;
    if (*(const uint8_t *)&one != 1)
        return 2;
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1)
        return 1;
#endif
#ifdef _WIN32
    // A path from the Windows file picker may not fit the process ANSI code page.
    int wide_argc = 0;
    wchar_t **wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    FILE *source = wide_argv && wide_argc == 4 ? _wfopen(wide_argv[3], L"rb") : NULL;
    if (wide_argv)
        LocalFree(wide_argv);
#else
    FILE *source = fopen(argv[3], "rb");
#endif
    if (!source)
        return 1;
    unsigned width = 0, height = 0;
    bool ok = kvm_media_h264_size(source, mp4, &width, &height, NULL, NULL);
    width /= 2;
    height /= 2;
    writer_t writer = {.half = mode == KVM_H264_EXACT,
                       .target = {.width = width, .height = height, .stride = width}};
    if (ok && width && height && width <= 960 && height <= 540)
        writer.target.pixels = malloc((size_t)width * height * sizeof(uint16_t));
    kvm_media_playback_t sink = {.frame = emit, .ctx = &writer, .reconstruction = mode};
    ok = writer.target.pixels && word(width) && word(height) && kvm_media_play_h264(source, mp4, &sink) &&
         fflush(stdout) == 0;
    free(writer.target.pixels);
    fclose(source);
    return ok ? 0 : 1;
}
