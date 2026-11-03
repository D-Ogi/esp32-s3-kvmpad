// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// A canned romaji -> kana -> kanji decoder for the simulator's JA fixtures (TASK-072). It drives the KEYBOARD view
// through the same ui_kbd_conv_engine_t table as the device's OpenWnn adapter, with a small fixed word list
// (nihon, go, kanji, henkan, desu, watashi, wa). It shows what the view renders (reading, candidates); it says
// nothing about the real engine, which test/ime_wnn covers.
#pragma once

#include "ui_view_keyboard.h"

/** The decoder table (static storage). */
const ui_kbd_conv_engine_t *sim_ja_engine(void);
