// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#include "ssh_control.h"
#include "hid_keymap.h"
#include <string.h>

typedef struct {
    const char *start;
    size_t size;
} token_t;

static bool matches(token_t token, const char *word)
{
    return token.size == strlen(word) && memcmp(token.start, word, token.size) == 0;
}

static bool whitespace(char c) { return c == ' ' || c == '\t'; }

static unsigned tokenize(const char *line, size_t len, token_t words[4])
{
    if (len && line[len - 1] == '\n') {
        --len;
        if (len && line[len - 1] == '\r') --len;
    }
    unsigned count = 0;
    size_t i = 0;
    while (i < len) {
        if (whitespace(line[i])) {
            ++i;
            continue;
        }
        if (count == 4) return 0;
        size_t first = i;
        while (i < len && !whitespace(line[i])) {
            unsigned char c = (unsigned char)line[i++];
            if (c < 0x21 || c > 0x7e) return 0;
        }
        words[count++] = (token_t){line + first, i - first};
    }
    return count;
}

static bool magnitude(token_t token, unsigned base, unsigned limit, unsigned *out)
{
    if (!token.size) return false;
    unsigned value = 0;
    for (size_t i = 0; i < token.size; ++i) {
        char c = token.start[i];
        unsigned digit;
        if (c >= '0' && c <= '9') digit = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = 10u + (unsigned)(c - 'a');
        else if (c >= 'A' && c <= 'F') digit = 10u + (unsigned)(c - 'A');
        else return false;
        if (digit >= base) return false;
        value = value * base + digit; // Previous value <= 4096; base <= 16.
        if (value > limit) return false;
    }
    *out = value;
    return true;
}

static bool key_usage(token_t token, uint8_t *usage)
{
    unsigned base = 10;
    if (token.size >= 2 && token.start[0] == '0' && (token.start[1] == 'x' || token.start[1] == 'X')) {
        if (token.size != 4) return false;
        token = (token_t){token.start + 2, 2};
        base = 16;
    }
    unsigned value;
    if (!magnitude(token, base, UINT8_MAX, &value) || !kvm_usage_is_valid_key((uint8_t)value)) return false;
    *usage = (uint8_t)value;
    return true;
}

static bool delta(token_t token, int16_t *out)
{
    bool negative = token.start[0] == '-';
    if (negative || token.start[0] == '+') {
        ++token.start;
        --token.size;
    }
    unsigned value;
    if (!magnitude(token, 10, 4096, &value)) return false;
    *out = (int16_t)(negative ? -(int)value : (int)value);
    return true;
}

bool kvm_ssh_parse_command(const char *command, size_t length, kvm_hid_intent_t *out)
{
    if (!command || !out || length > KVM_SSH_COMMAND_MAX) return false;
    token_t w[4];
    unsigned count = tokenize(command, length, w);
    kvm_hid_intent_t intent = {.origin = KVM_ORIGIN_REMOTE};
    if (count == 1 && matches(w[0], "release-all")) {
        intent.type = KVM_HID_RELEASE_ALL;
    } else if (count == 3 && (matches(w[1], "down") || matches(w[1], "up"))) {
        bool down = matches(w[1], "down");
        if (matches(w[0], "key")) {
            if (!key_usage(w[2], &intent.data.key.usage_id)) return false;
            intent.type = down ? KVM_HID_KEY_DOWN : KVM_HID_KEY_UP;
        } else if (matches(w[0], "mouse")) {
            if (matches(w[2], "left")) intent.data.mouse_button.button_id = KVM_MOUSE_BTN_LEFT;
            else if (matches(w[2], "right")) intent.data.mouse_button.button_id = KVM_MOUSE_BTN_RIGHT;
            else if (matches(w[2], "middle")) intent.data.mouse_button.button_id = KVM_MOUSE_BTN_MIDDLE;
            else return false;
            intent.type = down ? KVM_HID_MOUSE_BTN_DOWN : KVM_HID_MOUSE_BTN_UP;
        } else return false;
    } else if (count == 4 && matches(w[0], "mouse")) {
        if (matches(w[1], "move")) {
            if (!delta(w[2], &intent.data.mouse_move.dx) || !delta(w[3], &intent.data.mouse_move.dy)) return false;
            intent.type = KVM_HID_MOUSE_MOVE;
        } else if (matches(w[1], "wheel")) {
            if (!delta(w[2], &intent.data.wheel.horizontal) || !delta(w[3], &intent.data.wheel.vertical)) return false;
            intent.type = KVM_HID_MOUSE_WHEEL;
        } else return false;
    } else return false;
    *out = intent;
    return true;
}
