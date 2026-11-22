// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "gud_receiver.h"
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#ifndef GUD_CLIENT_ONLY
#include <linux/usb/functionfs.h>
#endif

#define REQUIRE(x)                                                                                 \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            perror(#x);                                                                            \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#include "input_probe.h"
#ifndef GUD_CLIENT_ONLY
static uint8_t scratch[GUD_R1_MAX_BYTES], image[GUD_MAX_WIDTH * GUD_MAX_HEIGHT];
static gud_receiver_t receiver;
static unsigned completed;
static bool verify_motion;
static bool capture_console;

static void check_motion_picture(void)
{
    unsigned id = 0;
    for (unsigned bit = 0; bit < 6; bit++)
        id |= image[32 * GUD_WIDTH + 96 * bit + 48] << bit;
    if (id < 1 || id > 20)
        return;
    for (unsigned y = 0; y < GUD_HEIGHT; y++)
        for (unsigned x = 0; x < GUD_WIDTH; x++) {
            unsigned expected = id & 1;
            if (y < 64 && x < 576)
                expected = (id >> (x / 96)) & 1;
            if (image[y * GUD_WIDTH + x] != expected)
                return;
        }
    printf("GUD_VIRTUAL_VERIFIED %u\n", id);
}

static void present(void *ctx, gud_rect_t r, const uint8_t *rows)
{
    (void)ctx;
    static unsigned last_width, last_height;
    unsigned width = receiver.width, height = receiver.height;
    REQUIRE(width && width <= GUD_MAX_WIDTH && height && height <= GUD_MAX_HEIGHT &&
            r.x + r.width <= width && r.y + r.height <= height);
    if (width != last_width || height != last_height) {
        memset(image, 2, (size_t)width * height); // An unwritten pixel cannot match black or white.
        last_width = width;
        last_height = height;
    }
    size_t stride = (r.width + 7u) / 8;
    for (unsigned y = 0; y < r.height; y++)
        for (unsigned x = 0; x < r.width; x++)
            image[(y + r.y) * width + x + r.x] = (rows[y * stride + x / 8] >> (7 - x % 8)) & 1;
    completed++;
    if ((capture_console && access("/tmp/check-console", F_OK) == 0) || access("/tmp/check-modes", F_OK) == 0) {
        int fd = open("/tmp/scanout.next", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        size_t bytes = (size_t)width * height;
        REQUIRE(fd >= 0 && write(fd, image, bytes) == (ssize_t)bytes);
        REQUIRE(close(fd) == 0 && rename("/tmp/scanout.next", "/tmp/scanout") == 0);
    }
    if (width != GUD_WIDTH || height != GUD_HEIGHT) {
        printf("R1 update=%u mode=%ux%u rect=%u,%u %ux%u\n", completed, width, height, r.x, r.y, r.width, r.height);
        return;
    }
    if (verify_motion) {
        check_motion_picture();
        return;
    }
    unsigned differences[2] = {0};
    for (unsigned y = 0; y < GUD_HEIGHT; y++) {
        for (unsigned x = 0; x < GUD_WIDTH; x++) {
            bool expected = ((x / 13 + y / 11) & 1) != 0;
            bool in_patch = x >= 619 && y >= 471;
            unsigned actual = image[y * GUD_WIDTH + x];
            differences[0] += actual != expected;
            differences[1] += actual != (expected ^ in_patch);
        }
    }
    printf("R1 update=%u rect=%u,%u %ux%u mismatch=%u,%u\n", completed, r.x, r.y, r.width, r.height,
           differences[0], differences[1]);
    for (unsigned i = 0; i < 2; i++)
        if (!differences[i]) {
            const char *path = i ? "/pass2" : "/pass1";
            int fd = open(path, O_CREAT | O_WRONLY, 0600);
            REQUIRE(fd >= 0);
            close(fd);
        }
}

static void serve(void)
{
    static const uint8_t descriptors[] = {3, 0, 0, 0, 32, 0,   0, 0, 1, 0, 0, 0, 2, 0,  0, 0,
                                          9, 4, 0, 0, 1,  255, 0, 0, 0, 7, 5, 1, 2, 64, 0, 0};
    static const uint8_t strings[] = {2, 0, 0, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int ep = open("/ffs/ep0", O_RDWR);
    REQUIRE(ep >= 0);
    REQUIRE(write(ep, descriptors, sizeof(descriptors)) == sizeof(descriptors));
    REQUIRE(write(ep, strings, sizeof(strings)) == sizeof(strings));
    int bulk = open("/ffs/ep1", O_RDONLY);
    REQUIRE(bulk >= 0);
    REQUIRE(gud_receiver_init(&receiver, scratch, sizeof(scratch), present, NULL));
    for (;;) {
        struct usb_functionfs_event ev;
        REQUIRE(read(ep, &ev, sizeof(ev)) == sizeof(ev));
        if (ev.type == FUNCTIONFS_DISABLE) {
            gud_receiver_abort(&receiver);
            continue;
        }
        if (ev.type != FUNCTIONFS_SETUP)
            continue;
        struct usb_ctrlrequest s = ev.u.setup;
        uint8_t data[8192];
        printf("EP0 type=%02x req=%02x value=%u index=%u len=%u\n", s.bRequestType, s.bRequest,
               s.wValue, s.wIndex, s.wLength);
        REQUIRE(s.wIndex == 0);
        if (s.bRequestType == 0xc1) {
            size_t cap = s.wLength < sizeof(data) ? s.wLength : sizeof(data);
            int n = gud_receiver_get(&receiver, s.bRequest, s.wValue, data, cap);
            // The separate legacy enumeration reproduces the released VGA-only offer.
            if (access("/tmp/legacy-vga", F_OK) == 0) {
                if (s.bRequest == 0x55 && n == 48) n = 24;
                if (s.bRequest == 0x01 && n == 30) {
                    memcpy(data + 18, data + 14, 4); // max_width = min_width
                    memcpy(data + 26, data + 22, 4); // max_height = min_height
                }
            }
            REQUIRE(n >= 0 && write(ep, data, n) == n);
        } else {
            REQUIRE(s.bRequestType == 0x41 && s.wLength <= sizeof(data));
            REQUIRE(read(ep, data, s.wLength) == s.wLength);
            REQUIRE(gud_receiver_set(&receiver, s.bRequest, s.wValue, data, s.wLength));
            while (receiver.expected) {
                size_t left = receiver.expected - receiver.used;
                size_t want = left < sizeof(data) ? left : sizeof(data);
                ssize_t got = read(bulk, data, want);
                REQUIRE(got > 0 && gud_receiver_bulk(&receiver, data, got));
            }
        }
    }
}

static void wait_picture(const char *path)
{
    for (unsigned i = 0; i < 100 && access(path, F_OK); i++)
        usleep(100000);
    REQUIRE(access(path, F_OK) == 0);
}

static void wait_receiver_image(const uint8_t *expected, size_t size)
{
    static uint8_t actual[GUD_MAX_WIDTH * GUD_MAX_HEIGHT + 1];
    for (unsigned tick = 0; tick < 120; tick++) {
        usleep(50000);
        int fd = open("/tmp/scanout", O_RDONLY);
        if (fd < 0) continue;
        ssize_t count = read(fd, actual, sizeof(actual));
        REQUIRE(count >= 0);
        close(fd);
        if ((size_t)count == size && !memcmp(actual, expected, size)) return;
    }
    REQUIRE(!"receiver did not receive the expected image");
}
#endif

static void phase(const char *name, const char *marker, bool external, bool virtual);

// The VT supplies its current glyphs. Only the kernel console renders the picture.
static void check_console(bool external, bool virtual, const char *fb_node, char **input)
{
    static uint8_t glyphs[512 * 32 * 4], expected[GUD_WIDTH * GUD_HEIGHT];
    const char *messages[] = {"KVMpad Linux console", "Recovery console ready"};
    int tty = open("/dev/tty1", O_RDWR | O_NOCTTY);
    struct console_font_op font = {.op = KD_FONT_OP_GET, .width = 32, .height = 32,
                                   .charcount = 512, .data = glyphs};
    REQUIRE(tty >= 0 && ioctl(tty, KDFONTOP, &font) == 0);
    REQUIRE(font.width && font.width <= 32 && font.height && font.height <= 32 && font.charcount >= 128);
    if (external) {
        struct vt_stat vt;
        struct fb_con2fbmap mapping = {.console = 1};
        int fb = open(fb_node, O_RDONLY);
        unsigned index;
        REQUIRE(sscanf(fb_node, "/dev/fb%u", &index) == 1 && fb >= 0);
        REQUIRE(ioctl(tty, VT_GETSTATE, &vt) == 0 && vt.v_active == 1);
        REQUIRE(ioctl(fb, FBIOGET_CON2FBMAP, &mapping) == 0 && mapping.framebuffer == index);
        REQUIRE(close(fb) == 0);
        printf("GUD_VT_ACTIVE tty1 fb%u\n", index);
    }
    if (virtual) {
        int gate = open("/tmp/check-console", O_WRONLY | O_CREAT, 0600);
        REQUIRE(gate >= 0 && close(gate) == 0);
    }
    for (size_t index = 0; index < sizeof(messages) / sizeof(messages[0]); index++) {
        const char *text = messages[index];
        const char *name = index ? "tty2" : "tty1"; // Two messages on the same VT.
        size_t row_bytes = (font.width + 7) / 8;
        REQUIRE(strlen(text) * font.width <= GUD_WIDTH);
        memset(expected, 0, sizeof(expected));
        unsigned lit = 0;
        for (size_t i = 0; text[i]; i++) {
            const uint8_t *glyph = glyphs + (unsigned char)text[i] * 32 * row_bytes;
            for (unsigned y = 0; y < font.height; y++)
                for (unsigned x = 0; x < font.width; x++) {
                    uint8_t bit = (glyph[y * row_bytes + x / 8] >> (7 - x % 8)) & 1;
                    expected[y * GUD_WIDTH + i * font.width + x] = bit;
                    lit += bit;
                }
        }
        REQUIRE(lit > 0);
        REQUIRE(dprintf(tty, "\033[?25l\033[97;40m\033[2J\033[H%s", text) > 0);
#ifndef GUD_CLIENT_ONLY
        if (virtual) {
            wait_receiver_image(expected, sizeof(expected));
            printf("GUD_VT_MATCH phase=%zu font=%ux%u pixels=%zu lit=%u text=%s\n",
                   index + 1, font.width, font.height, sizeof(expected), lit, text);
        }
#endif
        if (external) {
            static char hex[2 * (GUD_WIDTH / 8) * 32 + 1];
            const size_t width = strlen(text) * font.width;
            size_t j = 0;
            for (unsigned y = 0; y < font.height; y++)
                for (size_t x = 0; x < width; x += 8) {
                    unsigned byte = 0;
                    for (unsigned bit = 0; bit < 8 && x + bit < width; bit++)
                        byte |= expected[y * GUD_WIDTH + x + bit] << (7 - bit);
                    const char *digits = "0123456789abcdef";
                    hex[j++] = digits[byte >> 4];
                    hex[j++] = digits[byte & 15];
                }
            hex[j] = 0;
            printf("GUD_VT_BITMAP %s %u %u %u %s\n", name, font.width, font.height, lit, hex);
            if (input && index == 0) check_input_gate(input);
            else phase(name, NULL, true, false);
        }
    }
    if (virtual) REQUIRE(unlink("/tmp/check-console") == 0);
    REQUIRE(close(tty) == 0);
}

static void phase(const char *name, const char *marker, bool external, bool virtual)
{
#ifndef GUD_CLIENT_ONLY
    if (virtual)
        wait_picture(marker);
#else
    (void)marker;
    (void)virtual;
#endif
    if (external) {
        char line[32], expected[32];
        snprintf(expected, sizeof(expected), "ACK %s\n", name);
        printf("GUD_LCD_READY %s\n", name);
        REQUIRE(fgets(line, sizeof(line), stdin) && !strcmp(line, expected));
    }
}

static void wait_command(const char *expected)
{
    char line[32];
    REQUIRE(fgets(line, sizeof(line), stdin) && !strcmp(line, expected));
}

static void draw_motion(int fd, uint32_t fb_id, uint8_t *pixels, unsigned pitch)
{
    struct drm_clip_rect full = {.x1 = 0, .y1 = 0, .x2 = 640, .y2 = 480};
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&full};
    struct timespec deadline;
    puts("GUD_MOTION_READY");
    wait_command("START\n");
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0);
    for (unsigned id = 1; id <= 20; id++) {
        int err;
        do {
            err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        } while (err == EINTR);
        REQUIRE(err == 0);
        for (unsigned y = 0; y < 480; y++) {
            uint32_t *row = (uint32_t *)(pixels + (size_t)pitch * y);
            for (unsigned x = 0; x < 640; x++)
                row[x] = (id & 1) ? 0xffffff : 0;
            if (y < 64)
                for (unsigned bit = 0; bit < 6; bit++)
                    for (unsigned x = 0; x < 96; x++)
                        row[bit * 96 + x] = (id & (1u << bit)) ? 0xffffff : 0;
        }
        printf("GUD_MOTION_FRAME %u\n", id);
        REQUIRE(ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0);
        deadline.tv_nsec += 500000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    puts("GUD_MOTION_HELD");
    wait_command("ACK motion\n");
}

enum draw_mode { DRAW_DAMAGE, DRAW_MOTION, DRAW_BLANK, DRAW_MODESET };

static void picture_phase(int step, bool edge, unsigned width, unsigned height, bool external, bool virtual)
{
    if (step < 0) {
        phase(edge ? "edge" : "full", edge ? "/pass2" : "/pass1", external, virtual);
        return;
    }
    const char *names[] = {"vga", "svga", "return"};
    char name[32];
    snprintf(name, sizeof(name), "%s-%s", names[step], edge ? "edge" : "full");
#ifndef GUD_CLIENT_ONLY
    if (virtual) {
        static uint8_t expected[GUD_MAX_WIDTH * GUD_MAX_HEIGHT];
        for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++) {
                bool last_patch = edge && width - x <= 21 && height - y <= 9;
                expected[y * width + x] = ((x / 13 ^ y / 11) & 1) ^ (step == 2) ^ last_patch;
            }
        wait_receiver_image(expected, (size_t)width * height);
        printf("GUD_MODE_MATCH %s %ux%u pixels=%u\n", name, width, height, width * height);
    }
#else
    (void)width;
    (void)height;
#endif
    phase(name, NULL, external, false);
}

static void draw_mode(int fd, uint32_t conn, uint32_t crtc, struct drm_mode_modeinfo mode,
                      bool external, bool virtual, enum draw_mode exercise, int step)
{
    unsigned width = mode.hdisplay, height = mode.vdisplay;
    struct drm_mode_create_dumb dumb = {.width = width, .height = height, .bpp = 32};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb) == 0);
    struct drm_mode_fb_cmd fb = {.width = width,
                                 .height = height,
                                 .pitch = dumb.pitch,
                                 .bpp = 32,
                                 .depth = 24,
                                 .handle = dumb.handle};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb) == 0);
    struct drm_mode_map_dumb map = {.handle = dumb.handle};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0);
    uint8_t *pixels = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    REQUIRE(pixels != MAP_FAILED);
    printf("DRM driver=gud connector=%u crtc=%u framebuffer=%u %ux%u pitch=%u\n", conn, crtc,
           fb.fb_id, fb.width, fb.height, fb.pitch);
    for (unsigned y = 0; y < height; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)dumb.pitch * y);
        for (unsigned x = 0; x < width; x++)
            row[x] = (((x / 13 + y / 11) % 2) != (step == 2)) ? 0xffffff : 0;
    }
    struct drm_mode_crtc state = {.set_connectors_ptr = (uintptr_t)&conn,
                                  .count_connectors = 1,
                                  .crtc_id = crtc,
                                  .fb_id = fb.fb_id,
                                  .mode_valid = 1,
                                  .mode = mode};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &state) == 0);
    struct drm_mode_crtc active = {.crtc_id = crtc};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &active) == 0 && active.mode_valid &&
            active.fb_id == fb.fb_id && active.mode.hdisplay == width && active.mode.vdisplay == height);
    if (exercise == DRAW_MOTION) {
        draw_motion(fd, fb.fb_id, pixels, dumb.pitch);
        munmap(pixels, dumb.size);
        return;
    }
    if (exercise == DRAW_BLANK) {
        phase("lit", NULL, true, false);
        struct drm_mode_crtc off = {.crtc_id = crtc};
        REQUIRE(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &off) == 0);
        phase("blank", NULL, true, false);
        REQUIRE(ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &state) == 0);
        phase("wake", NULL, true, false);
        munmap(pixels, dumb.size);
        return;
    }
    picture_phase(step, false, width, height, external, virtual);
    for (unsigned y = height - 9; y < height; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)dumb.pitch * y);
        for (unsigned x = width - 21; x < width; x++)
            row[x] ^= 0xffffff;
    }
    struct drm_clip_rect edge = {.x1 = width - 21, .x2 = width, .y1 = height - 9, .y2 = height};
    struct drm_mode_fb_dirty_cmd dirty = {
        .fb_id = fb.fb_id, .num_clips = 1, .clips_ptr = (uintptr_t)&edge};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) == 0);
    picture_phase(step, true, width, height, external, virtual);
    munmap(pixels, dumb.size);
}

static void draw(const char *device, bool external, bool virtual, enum draw_mode exercise)
{
    int fd = open(device, O_RDWR);
    REQUIRE(fd >= 0);
    char name[32] = {0};
    struct drm_version driver = {.name_len = sizeof(name) - 1, .name = name};
    REQUIRE(ioctl(fd, DRM_IOCTL_VERSION, &driver) == 0 && !strcmp(name, "gud"));
    uint32_t crtc = 0, conn = 0;
    struct drm_mode_card_res res = {.count_crtcs = 1, .crtc_id_ptr = (uintptr_t)&crtc,
                                    .count_connectors = 1, .connector_id_ptr = (uintptr_t)&conn};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && res.count_crtcs == 1 && res.count_connectors == 1);
    struct drm_mode_modeinfo offered[2] = {0};
    // A zero-capacity GETCONNECTOR from the DRM master reprobes after fbcon may
    // have filtered modes against its smaller console framebuffer.
    struct drm_mode_get_connector connector = {.connector_id = conn};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) == 0 && connector.count_modes >= 1 &&
            connector.count_modes <= 2);
    connector = (struct drm_mode_get_connector){.connector_id = conn, .count_modes = 2, .modes_ptr = (uintptr_t)offered};
    REQUIRE(ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) == 0 && connector.connection == 1 &&
            connector.count_modes >= 1 && connector.count_modes <= 2);
    const struct drm_mode_modeinfo *vga = NULL, *svga = NULL;
    for (unsigned i = 0; i < connector.count_modes; i++) {
        if (offered[i].hdisplay == 640 && offered[i].vdisplay == 480) vga = &offered[i];
        if (offered[i].hdisplay == 800 && offered[i].vdisplay == 600) svga = &offered[i];
    }
    REQUIRE(vga && (connector.count_modes == 1 ||
            (svga && (vga->type & DRM_MODE_TYPE_PREFERRED) && !(svga->type & DRM_MODE_TYPE_PREFERRED))));
    printf("GUD_DRM_MODES count=%u vga=1 svga=%u\n", connector.count_modes, svga != NULL);
    if (exercise == DRAW_MODESET) {
        REQUIRE(svga);
        if (virtual) {
            int marker = open("/tmp/check-modes", O_CREAT | O_WRONLY, 0600);
            REQUIRE(marker >= 0 && close(marker) == 0);
        }
        const struct drm_mode_modeinfo *order[] = {vga, svga, vga};
        for (int i = 0; i < 3; i++)
            draw_mode(fd, conn, crtc, *order[i], external, virtual, exercise, i);
        if (virtual) REQUIRE(unlink("/tmp/check-modes") == 0);
    } else {
        draw_mode(fd, conn, crtc, *vga, external, virtual, exercise, -1);
    }
    close(fd);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 3 && !strcmp(argv[1], "draw-ack"))
        draw(argv[2], true, false, DRAW_DAMAGE);
    else if (argc == 3 && !strcmp(argv[1], "draw-modes-ack"))
        draw(argv[2], true, false, DRAW_MODESET);
    else if (argc == 3 && !strcmp(argv[1], "draw-blank"))
        draw(argv[2], true, false, DRAW_BLANK);
    else if (argc == 3 && !strcmp(argv[1], "draw-motion"))
        draw(argv[2], true, false, DRAW_MOTION);
    else if (argc == 3 && !strcmp(argv[1], "console-ack"))
        check_console(true, false, argv[2], NULL);
    else if (argc == 6 && !strcmp(argv[1], "console-ack-input"))
        check_console(true, false, argv[2], argv + 3);
#ifndef GUD_CLIENT_ONLY
    else if (argc == 3 && !strcmp(argv[1], "draw-modes"))
        draw(argv[2], false, true, DRAW_MODESET);
    else if (argc == 3 && !strcmp(argv[1], "draw-modes-ack-virtual"))
        draw(argv[2], true, true, DRAW_MODESET);
    else if (argc == 2 && !strcmp(argv[1], "check-console"))
        check_console(false, true, NULL, NULL);
    else if (argc == 3 && !strcmp(argv[1], "console-ack-virtual"))
        check_console(true, true, argv[2], NULL);
    else if (argc == 2 && (!strcmp(argv[1], "serve") || !strcmp(argv[1], "serve-motion") ||
                           !strcmp(argv[1], "serve-console"))) {
        verify_motion = !strcmp(argv[1], "serve-motion");
        capture_console = !strcmp(argv[1], "serve-console");
        serve();
    }
    else if (argc == 3 && !strcmp(argv[1], "draw"))
        draw(argv[2], false, true, DRAW_DAMAGE);
    else if (argc == 3 && !strcmp(argv[1], "draw-ack-virtual"))
        draw(argv[2], true, true, DRAW_DAMAGE);
#endif
    else
        return 2;
    return 0;
}
