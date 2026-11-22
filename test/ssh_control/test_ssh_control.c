// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "hid_keymap.h"
#include "ssh_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Enabled in Release builds as well. Never print command contents.
#define CHECK(test) do { if (!(test)) { fprintf(stderr, "check failed at line %u\n", (unsigned)__LINE__); exit(1); } } while (0)

static kvm_hid_intent_t accepted(const char *s, size_t size)
{
    kvm_hid_intent_t in;
    memset(&in, 0xa5, sizeof(in));
    CHECK(kvm_ssh_parse_command(s, size, &in));
    CHECK(in.origin == KVM_ORIGIN_REMOTE && !in.epoch && !in.seq && !in.timestamp_us);
#if KVM_HID_HAS_HPE
    CHECK(!in.pad_input && !in.pad_generation);
#endif
    return in;
}

static void rejected(const char *s, size_t size)
{
    kvm_hid_intent_t out;
    unsigned char original[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(original, &out, sizeof(out));
    CHECK(!kvm_ssh_parse_command(s, size, &out));
    CHECK(memcmp(original, &out, sizeof(out)) == 0);
}

static void keyboard(void)
{
    char line[32];
    for (unsigned n = 0; n <= 255; ++n) {
        for (unsigned down = 0; down <= 1; ++down) {
            int len = down ? snprintf(line, sizeof(line), "key down 0x%02X", n)
                           : snprintf(line, sizeof(line), "key up %u\r\n", n);
            CHECK(len > 0 && (size_t)len < sizeof(line));
            if (!kvm_usage_is_valid_key((uint8_t)n)) {
                rejected(line, (size_t)len);
                continue;
            }
            kvm_hid_intent_t in = accepted(line, (size_t)len);
            CHECK(in.type == (down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP));
            CHECK(in.data.key.usage_id == n);
        }
    }
    const char raw[] = {'k','e','y',' ','u','p',' ','0','X','e','a'};
    rejected(raw, sizeof(raw));
    CHECK(accepted("key up 0Xe3", 11).data.key.usage_id == 0xe3);
}

static void mouse(void)
{
    const char *buttons[] = {"left","right","middle"};
    const unsigned ids[] = {KVM_MOUSE_BTN_LEFT,KVM_MOUSE_BTN_RIGHT,KVM_MOUSE_BTN_MIDDLE};
    char line[32];
    for (unsigned b = 0; b < 3; ++b)
        for (unsigned down = 0; down <= 1; ++down) {
            int len = snprintf(line, sizeof(line), "\t mouse  %s\t%s  \n", down ? "down" : "up", buttons[b]);
            CHECK(len > 0 && (size_t)len < sizeof(line));
            kvm_hid_intent_t in = accepted(line, (size_t)len);
            CHECK(in.type == (down ? KVM_HID_MOUSE_BTN_DOWN : KVM_HID_MOUSE_BTN_UP));
            CHECK(in.data.mouse_button.button_id == ids[b]);
        }
    const char move[] = "mouse move -4096 +4096";
    const char wheel[] = "mouse wheel +4096 -4096";
    kvm_hid_intent_t in = accepted(move, sizeof(move) - 1);
    CHECK(in.type == KVM_HID_MOUSE_MOVE && in.data.mouse_move.dx == -4096 && in.data.mouse_move.dy == 4096);
    in = accepted(wheel, sizeof(wheel) - 1);
    CHECK(in.type == KVM_HID_MOUSE_WHEEL && in.data.wheel.horizontal == 4096 && in.data.wheel.vertical == -4096);
    in = accepted("mouse wheel 0 -1", 16);
    CHECK(in.data.wheel.horizontal == 0 && in.data.wheel.vertical == -1);
}

static void bounds(void)
{
    const char *bad[] = {"", "   \t", "release-all x", "release-all\r", "release-all\n\n",
        "release-all\nkey down 4", "key down", "key  down 4 extra", "key press 4", "KEY down 4",
        "key up +4", "key up -4", "key up 4.0", "key up 256", "key up 0x100", "key up 0x4",
        "key up 0xff", "key up 0xgg", "key up 0x", "system keepalive", "consumer down 8",
        "mouse left", "mouse up 1", "mouse click left", "mouse move 1", "mouse move 1 2 3",
        "mouse move 4097 0", "mouse move 0 -4097", "mouse wheel -4097 0", "mouse wheel 0 +4097",
        "mouse wheel 0 0x01", "mouse move + -", "mouse move --1 0", "mouse move 0 4294967296",
        "key down 4;release-all", "key down 999999999999999999999999999999"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) rejected(bad[i], strlen(bad[i]));
    char line[KVM_SSH_COMMAND_MAX + 1];
    memset(line, ' ', sizeof(line));
    memcpy(line, "release-all", 11);
    CHECK(accepted(line, KVM_SSH_COMMAND_MAX).type == KVM_HID_RELEASE_ALL);
    rejected(line, sizeof(line));
    rejected(NULL, 0);
    rejected(line, SIZE_MAX);
    CHECK(!kvm_ssh_parse_command(line, 11, NULL));
    // Length-delimited input must never accept a NUL-terminated prefix or treat
    // embedded controls/upper bytes as ignorable spaces, even after a valid verb.
    const char prefix[] = "release-all";
    for (unsigned c = 0; c <= 255; ++c) {
        if (c == ' ' || c == '\t') continue;
        memcpy(line, prefix, sizeof(prefix) - 1);
        line[11] = (char)c;
        line[12] = ' ';
        rejected(line, 13);
    }
    CHECK(accepted("release-all\r\n", 13).type == KVM_HID_RELEASE_ALL);
}

int main(void)
{
    keyboard();
    mouse();
    bounds();
    puts("PASS typed SSH keyboard and relative mouse commands, bounds and rejection atomicity");
    return 0;
}
