// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// USB HID usage constants (HID Usage Tables 1.5, page 0x07 Keyboard/Keypad and page 0x0C Consumer),
// the compiled chord allowlist (FR-301..306, FR-744, ADR-018 media) and usage validation.
// A usage is never derived from a character or a legend (03 §3 rule 1, FR-202, FR-743).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Page 0x07: keyboard usages ------------------------------------------------------------
#define KVM_KEY_A            0x04 // ... KVM_KEY_A + 25 = Z (0x1D)
#define KVM_KEY_Z            0x1D
#define KVM_KEY_1            0x1E // ... 9 = 0x26
#define KVM_KEY_0            0x27
#define KVM_KEY_ENTER        0x28
#define KVM_KEY_ESCAPE       0x29
#define KVM_KEY_BACKSPACE    0x2A
#define KVM_KEY_TAB          0x2B
#define KVM_KEY_SPACE        0x2C
#define KVM_KEY_MINUS        0x2D
#define KVM_KEY_EQUAL        0x2E
#define KVM_KEY_LEFT_BRACKET 0x2F
#define KVM_KEY_RIGHT_BRACKET 0x30
#define KVM_KEY_BACKSLASH    0x31
#define KVM_KEY_NON_US_HASH  0x32
#define KVM_KEY_SEMICOLON    0x33
#define KVM_KEY_APOSTROPHE   0x34
#define KVM_KEY_GRAVE        0x35
#define KVM_KEY_COMMA        0x36
#define KVM_KEY_PERIOD       0x37
#define KVM_KEY_SLASH        0x38
#define KVM_KEY_CAPS_LOCK    0x39
#define KVM_KEY_F1           0x3A // ... F12 = 0x45
#define KVM_KEY_F4           0x3D
#define KVM_KEY_F12          0x45
#define KVM_KEY_PRINT_SCREEN 0x46
#define KVM_KEY_SCROLL_LOCK  0x47
#define KVM_KEY_PAUSE        0x48
#define KVM_KEY_INSERT       0x49
#define KVM_KEY_HOME         0x4A
#define KVM_KEY_PAGE_UP      0x4B
#define KVM_KEY_DELETE       0x4C // Delete Forward
#define KVM_KEY_END          0x4D
#define KVM_KEY_PAGE_DOWN    0x4E
#define KVM_KEY_RIGHT        0x4F
#define KVM_KEY_LEFT         0x50
#define KVM_KEY_DOWN         0x51
#define KVM_KEY_UP           0x52
#define KVM_KEY_NUM_LOCK     0x53
#define KVM_KEY_NON_US_BACKSLASH 0x64
#define KVM_KEY_APPLICATION  0x65
#define KVM_KEY_LAST_REGULAR 0xA4 // highest usage accepted below the modifiers

#define KVM_KEY_LEFT_CTRL    0xE0
#define KVM_KEY_LEFT_SHIFT   0xE1
#define KVM_KEY_LEFT_ALT     0xE2
#define KVM_KEY_LEFT_GUI     0xE3
#define KVM_KEY_RIGHT_CTRL   0xE4
#define KVM_KEY_RIGHT_SHIFT  0xE5
#define KVM_KEY_RIGHT_ALT    0xE6 // AltGr (03 §3 rule 1)
#define KVM_KEY_RIGHT_GUI    0xE7

/** Modifier bitmap bit for a modifier usage (0xE0 -> bit 0 ... 0xE7 -> bit 7). */
#define KVM_MOD_BIT(usage) ((uint8_t)(1u << ((usage)-KVM_KEY_LEFT_CTRL)))

// ---- Page 0x0C: Consumer Control allowlist (TECH-075) ------------------------------------
#define KVM_CONSUMER_SCAN_NEXT   0x00B5
#define KVM_CONSUMER_SCAN_PREV   0x00B6
#define KVM_CONSUMER_PLAY_PAUSE  0x00CD
#define KVM_CONSUMER_MUTE        0x00E2
#define KVM_CONSUMER_VOLUME_UP   0x00E9
#define KVM_CONSUMER_VOLUME_DOWN 0x00EA

// ---- Chord allowlist (03 §3 rule 2: chord_id only, never an arbitrary array) ---------------
typedef enum {
    KVM_CHORD_NONE = 0,
    KVM_CHORD_CTRL_ALT_DEL,  // FR-301 LCtrl + LAlt + Delete
    KVM_CHORD_ALT_TAB,       // FR-302 LAlt + Tab
    KVM_CHORD_TASK_MANAGER,  // FR-303 LCtrl + LShift + Escape
    KVM_CHORD_SHOW_DESKTOP,  // FR-304 LGUI + D
    KVM_CHORD_CLOSE_WINDOW,  // FR-305 LAlt + F4
    KVM_CHORD_SCREENSHOT,    // FR-306 PrintScreen
    KVM_CHORD_LAYOUT_WIN,    // FR-744 Windows: LGUI + Space
    KVM_CHORD_LAYOUT_CTRL,   // FR-744 macOS / some Linux desktops: LCtrl + Space
    KVM_CHORD_MEDIA_PLAY_PAUSE, // ADR-018 / FR-311 (Consumer)
    KVM_CHORD_MEDIA_NEXT,
    KVM_CHORD_MEDIA_PREV,
    KVM_CHORD_MEDIA_MUTE,
    KVM_CHORD_MEDIA_VOLUME_UP,
    KVM_CHORD_MEDIA_VOLUME_DOWN,
    KVM_CHORD_COUNT,
} kvm_chord_id_t;

typedef enum {
    KVM_CHORD_KIND_KEYBOARD = 0, // phases: modifiers DOWN, key DOWN, key UP, modifiers UP (02 §7.3)
    KVM_CHORD_KIND_CONSUMER,     // phases: usage DOWN, usage UP (TECH-074)
} kvm_chord_kind_t;

typedef struct {
    kvm_chord_id_t id;
    kvm_chord_kind_t kind;
    uint8_t modifiers; // KVM_MOD_BIT mask (keyboard kind)
    uint16_t usage;    // page 0x07 key (keyboard kind) or page 0x0C usage (consumer kind)
    const char *name;  // stable identifier for logs / the remote command_id table (not a UI label)
} kvm_chord_t;

/** Allowlisted chord, or NULL for an unknown id. */
const kvm_chord_t *kvm_chord_get(uint16_t chord_id);

/** True for a keyboard usage the device may send: 0x04..0xA4 and the modifiers 0xE0..0xE7. */
bool kvm_usage_is_valid_key(uint8_t usage);

static inline bool kvm_usage_is_modifier(uint8_t usage)
{
    return usage >= KVM_KEY_LEFT_CTRL && usage <= KVM_KEY_RIGHT_GUI;
}

/** True for a Consumer usage on the allowlist (TECH-075). */
bool kvm_consumer_is_allowed(uint16_t usage);

#ifdef __cplusplus
}
#endif
