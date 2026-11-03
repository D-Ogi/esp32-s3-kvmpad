// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
// Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
//
// kbd_text: composed text item -> burst steps (FR-747: directly typeable characters as plain keys, the rest
// through the delivery mode; text keys as themselves).
#include <string.h>

#include "kbd_text.h"
#include "kvm_test.h"

static void ascii_table_is_a_bijection(void)
{
    bool seen[256][2];
    memset(seen, 0, sizeof(seen));
    int dup = 0;
    for (uint32_t c = 0x20; c <= 0x7E; c++) {
        kvm_keystroke_t k;
        CHECK(kvm_text_ascii_us(c, &k));
        CHECK(k.usage >= 0x04 && k.usage <= 0x38);
        CHECK(k.mods == 0 || k.mods == 0x02);
        dup += seen[k.usage][k.mods != 0];
        seen[k.usage][k.mods != 0] = true;
    }
    CHECK_EQ(dup, 0); // 95 characters on 95 distinct (key, Shift) pairs
    kvm_keystroke_t k;
    CHECK(!kvm_text_ascii_us(0x1F, &k));
    CHECK(!kvm_text_ascii_us(0x7F, &k));
    CHECK(!kvm_text_ascii_us(0xE9, &k));
    CHECK(kvm_text_ascii_us('a', &k) && k.usage == 0x04 && k.mods == 0);
    CHECK(kvm_text_ascii_us('A', &k) && k.usage == 0x04 && k.mods == 0x02);
    CHECK(kvm_text_ascii_us('0', &k) && k.usage == 0x27 && k.mods == 0);
    CHECK(kvm_text_ascii_us(')', &k) && k.usage == 0x27 && k.mods == 0x02);
    CHECK(kvm_text_ascii_us(' ', &k) && k.usage == 0x2C && k.mods == 0);
    CHECK(kvm_text_ascii_us('~', &k) && k.usage == 0x35 && k.mods == 0x02);
    CHECK(kvm_text_ascii_us('"', &k) && k.usage == 0x34 && k.mods == 0x02);
    CHECK(kvm_text_ascii_us('\\', &k) && k.usage == 0x31 && k.mods == 0);
}

static void items_to_steps(void)
{
    kvm_uni_step_t s[KVM_UNI_MAX_STEPS];
    kvm_text_item_t it = {KVM_TEXT_CHAR, '?'};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), 4);
    CHECK(s[0].usage == 0xE1 && s[0].down && s[1].usage == 0x38 && s[1].down && s[2].usage == 0x38 &&
          !s[2].down && s[3].usage == 0xE1 && !s[3].down);
    it = (kvm_text_item_t){KVM_TEXT_CHAR, 'x'};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_LINUX, s, KVM_UNI_MAX_STEPS), 2); // typeable: plain in every mode
    // 한 needs a sequence: none in host-ime, the linux one otherwise
    it = (kvm_text_item_t){KVM_TEXT_CHAR, 0xD55C};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_MODE);
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_LINUX, s, KVM_UNI_MAX_STEPS), 16);
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_MACOS, s, KVM_UNI_MAX_STEPS), 10);
    it = (kvm_text_item_t){KVM_TEXT_CHAR, 0xD800};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_LINUX, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_CODEPOINT);
    // text keys
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0x28};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), 2);
    CHECK(s[0].usage == 0x28 && s[0].down && s[1].usage == 0x28 && !s[1].down);
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0x1E | KVM_TEXT_KEY_SHIFT};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), 4);
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0xE1};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0x03};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0x28 | 0x400};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    it = (kvm_text_item_t){7, 0x28};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    CHECK_EQ(kvm_text_item_steps(NULL, KVM_UNI_HOST_IME, s, KVM_UNI_MAX_STEPS), KVM_UNI_ERR_KEY);
    it = (kvm_text_item_t){KVM_TEXT_KEY, 0x1E | KVM_TEXT_KEY_SHIFT};
    CHECK_EQ(kvm_text_item_steps(&it, KVM_UNI_HOST_IME, s, 3), KVM_UNI_ERR_SPACE);
}

int main(void)
{
    printf("test_kbd_text\n");
    RUN(ascii_table_is_a_bijection);
    RUN(items_to_steps);
    TEST_MAIN_END("test_kbd_text");
}
