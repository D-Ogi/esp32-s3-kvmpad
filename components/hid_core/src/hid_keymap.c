// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// Chord allowlist and usage validation (spec 01 §5, 03 §3, ADR-018).
#include "hid_keymap.h"

#include <stddef.h>

#define KB(id_, mods_, key_, name_) {(id_), KVM_CHORD_KIND_KEYBOARD, (mods_), (key_), (name_)}
#define CC(id_, usage_, name_)      {(id_), KVM_CHORD_KIND_CONSUMER, 0, (usage_), (name_)}

static const kvm_chord_t k_chords[KVM_CHORD_COUNT] = {
    [KVM_CHORD_NONE] = {KVM_CHORD_NONE, KVM_CHORD_KIND_KEYBOARD, 0, 0, NULL},
    [KVM_CHORD_CTRL_ALT_DEL] = KB(KVM_CHORD_CTRL_ALT_DEL, KVM_MOD_BIT(KVM_KEY_LEFT_CTRL) | KVM_MOD_BIT(KVM_KEY_LEFT_ALT),
                                  KVM_KEY_DELETE, "ctrl_alt_del"),
    [KVM_CHORD_ALT_TAB] = KB(KVM_CHORD_ALT_TAB, KVM_MOD_BIT(KVM_KEY_LEFT_ALT), KVM_KEY_TAB, "alt_tab"),
    [KVM_CHORD_TASK_MANAGER] = KB(KVM_CHORD_TASK_MANAGER,
                                  KVM_MOD_BIT(KVM_KEY_LEFT_CTRL) | KVM_MOD_BIT(KVM_KEY_LEFT_SHIFT), KVM_KEY_ESCAPE,
                                  "task_manager"),
    [KVM_CHORD_SHOW_DESKTOP] = KB(KVM_CHORD_SHOW_DESKTOP, KVM_MOD_BIT(KVM_KEY_LEFT_GUI), KVM_KEY_A + ('D' - 'A'),
                                  "show_desktop"),
    [KVM_CHORD_CLOSE_WINDOW] = KB(KVM_CHORD_CLOSE_WINDOW, KVM_MOD_BIT(KVM_KEY_LEFT_ALT), KVM_KEY_F4, "close_window"),
    [KVM_CHORD_SCREENSHOT] = KB(KVM_CHORD_SCREENSHOT, 0, KVM_KEY_PRINT_SCREEN, "screenshot"),
    [KVM_CHORD_LAYOUT_WIN] = KB(KVM_CHORD_LAYOUT_WIN, KVM_MOD_BIT(KVM_KEY_LEFT_GUI), KVM_KEY_SPACE, "layout_switch_win"),
    [KVM_CHORD_LAYOUT_CTRL] = KB(KVM_CHORD_LAYOUT_CTRL, KVM_MOD_BIT(KVM_KEY_LEFT_CTRL), KVM_KEY_SPACE,
                                 "layout_switch_ctrl"),
    [KVM_CHORD_MEDIA_PLAY_PAUSE] = CC(KVM_CHORD_MEDIA_PLAY_PAUSE, KVM_CONSUMER_PLAY_PAUSE, "media_play_pause"),
    [KVM_CHORD_MEDIA_NEXT] = CC(KVM_CHORD_MEDIA_NEXT, KVM_CONSUMER_SCAN_NEXT, "media_next"),
    [KVM_CHORD_MEDIA_PREV] = CC(KVM_CHORD_MEDIA_PREV, KVM_CONSUMER_SCAN_PREV, "media_prev"),
    [KVM_CHORD_MEDIA_MUTE] = CC(KVM_CHORD_MEDIA_MUTE, KVM_CONSUMER_MUTE, "media_mute"),
    [KVM_CHORD_MEDIA_VOLUME_UP] = CC(KVM_CHORD_MEDIA_VOLUME_UP, KVM_CONSUMER_VOLUME_UP, "media_volume_up"),
    [KVM_CHORD_MEDIA_VOLUME_DOWN] = CC(KVM_CHORD_MEDIA_VOLUME_DOWN, KVM_CONSUMER_VOLUME_DOWN, "media_volume_down"),
};

const kvm_chord_t *kvm_chord_get(uint16_t chord_id)
{
    if (chord_id == KVM_CHORD_NONE || chord_id >= KVM_CHORD_COUNT) {
        return NULL;
    }
    return &k_chords[chord_id];
}

bool kvm_usage_is_valid_key(uint8_t usage)
{
    return (usage >= KVM_KEY_A && usage <= KVM_KEY_LAST_REGULAR) || kvm_usage_is_modifier(usage);
}

bool kvm_consumer_is_allowed(uint16_t usage)
{
    switch (usage) {
    case KVM_CONSUMER_SCAN_NEXT:
    case KVM_CONSUMER_SCAN_PREV:
    case KVM_CONSUMER_PLAY_PAUSE:
    case KVM_CONSUMER_MUTE:
    case KVM_CONSUMER_VOLUME_UP:
    case KVM_CONSUMER_VOLUME_DOWN:
        return true;
    default:
        return false;
    }
}
