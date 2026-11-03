// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// A canned pinyin decoder for the simulator's ZH fixtures (TASK-072). The real PinyinIME dictionary is built for
// the 32-bit target and cannot load on this 64-bit host, so the KEYBOARD view is driven through the same
// ui_kbd_conv_engine_t table by a small fixed syllable list (ni, hao, jin, tian, zhong, guo). It shows what the
// view renders; it says nothing about the real decoder, which test/ime_pinyin covers.
#pragma once

#include "ui_view_keyboard.h"

/** The decoder table (static storage). */
const ui_kbd_conv_engine_t *sim_zh_engine(void);
