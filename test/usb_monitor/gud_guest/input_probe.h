// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
// Test-only observer for the two fixed firmware probes. Never records arbitrary input.
#include <linux/input.h>
#include <linux/hidraw.h>
#include <poll.h>

struct probe_state {
    unsigned keys, moves;
    int delta;
    bool key_sync, move_sync;
    int64_t last_key_us;
};

static _Noreturn void reject_probe(const char *reason)
{
    printf("GUD_REJECT %s\n", reason);
    exit(1); // evdev releases any exclusive grabs when these file descriptions close.
}

static bool probe_event(struct probe_state *s, bool mouse, const struct input_event *e)
{
    if (e->type == EV_SYN) {
        if (e->code != SYN_REPORT) return false; // Includes SYN_DROPPED.
        if (mouse) s->move_sync = false;
        else s->key_sync = false;
        return true;
    }
    int64_t us = (int64_t)e->input_event_sec * 1000000 + e->input_event_usec;
    if (!mouse) {
        if (e->type == EV_MSC && e->code == MSC_SCAN) return true;
        if (s->keys >= 24 || s->key_sync || e->type != EV_KEY ||
            e->code != KEY_F13 + s->keys / 2 || e->value != (int)(1 - s->keys % 2)) return false;
        s->keys++;
        s->key_sync = true;
        s->last_key_us = us;
        return true;
    }
    static const int distances[] = {300, -300, 200, -200};
    if (s->keys != 24 || s->key_sync || us < s->last_key_us || s->moves >= 4 || s->move_sync ||
        e->type != EV_REL || e->code != (s->moves < 2 ? REL_X : REL_Y) ||
        e->value < -127 || e->value > 127 || e->value == 0) return false;
    int target = distances[s->moves];
    s->delta += e->value;
    if ((e->value > 0) != (target > 0) || abs(s->delta) > abs(target)) return false;
    s->move_sync = true;
    if (s->delta == target) {
        s->delta = 0;
        s->moves++;
    }
    return true;
}

static bool keyboard_state_matches(int fd, uint8_t usage)
{
    uint8_t report[9] = {0}; // hidraw prefixes an unnumbered USB report with zero.
    const uint8_t expected[9] = {0, 0, 0, usage};
    if (ioctl(fd, HIDIOCGINPUT(sizeof(report)), report) != sizeof(report)) reject_probe("keyboard-get-report");
    return memcmp(report, expected, sizeof(report)) == 0;
}

static void check_input_gate(char **paths)
{
    const int timeout = atoi(paths[2]);
    if (timeout < 1 || timeout > 900) reject_probe("input-timeout-range");
    struct pollfd fds[3] = {{.events=POLLIN}, {.events=POLLIN}, {.fd=STDIN_FILENO,.events=POLLIN}};
    for (unsigned i = 0; i < 2; i++) {
        int fd = open(paths[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        int clock_id = CLOCK_MONOTONIC;
        unsigned char held[KEY_MAX / 8 + 1] = {0};
        if (fd < 0 || ioctl(fd, EVIOCGRAB, 1) || ioctl(fd, EVIOCSCLOCKID, &clock_id) ||
            ioctl(fd, EVIOCGKEY(sizeof(held)), held) < 0) reject_probe("input-grab");
        for (size_t k = 0; k < sizeof(held); k++) if (held[k]) reject_probe("input-not-neutral");
        fds[i].fd = fd;
    }
    const char *raw = getenv("PROBE_KEYBOARD_HIDRAW");
    int keyboard = raw ? open(raw, O_RDONLY | O_CLOEXEC) : -1;
    if (keyboard < 0 || !keyboard_state_matches(keyboard, 0)) reject_probe("keyboard-initial-state");
    unsigned matched_holds = 0;
    puts("GUD_INPUT_READY");
    puts("GUD_LCD_READY tty1");
    struct timespec start, now;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    bool ack = false, matched = false;
    struct probe_state state = {0};
    do {
        REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 >= timeout * 1000)
            reject_probe("input-timeout");
        int n = poll(fds, 3, 100);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) reject_probe("input-poll");
        for (unsigned i = 0; i < 2; i++) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) reject_probe("input-disconnected");
            if (!(fds[i].revents & POLLIN)) continue;
            struct input_event events[32];
            ssize_t size;
            while ((size = read(fds[i].fd, events, sizeof(events))) > 0) {
                if (size % sizeof(events[0])) reject_probe("input-read");
                for (size_t j = 0; j < (size_t)size / sizeof(events[0]); j++) {
                    if (!probe_event(&state, i == 1, &events[j])) reject_probe("input-sequence");
                    // EP0 samples current state. An evdev DOWN queued by Linux can outlive the press.
                    if (i == 0 && events[j].type == EV_KEY && events[j].value == 1 &&
                        keyboard_state_matches(keyboard, 0x68 + events[j].code - KEY_F13)) ++matched_holds;
                }
            }
            if (!size || (errno != EAGAIN && errno != EINTR)) reject_probe("input-read");
        }
        if (fds[2].revents) {
            char command[32];
            if (!fgets(command, sizeof(command), stdin) || strcmp(command, "ACK tty1\n"))
                reject_probe("input-ack");
            fds[2].fd = -1;
            ack = true;
        }
        if (!matched && state.moves == 4 && !state.move_sync) {
            if (!matched_holds || !keyboard_state_matches(keyboard, 0)) reject_probe("keyboard-live-state");
            printf("GUD_KEYBOARD_CONTROL held=%u/12 released=1\n", matched_holds);
            matched = true;
            puts("GUD_INPUT_PASS");
        }
    } while (!ack || !matched);
    REQUIRE(!close(keyboard) && !close(fds[0].fd) && !close(fds[1].fd));
}
