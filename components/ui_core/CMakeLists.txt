# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ui_core (TASK-060B/C): contracts, bounded state mailbox, LVGL Subjects store, typed action gateway, router + overlay.
# No TinyUSB and no network headers (07 §2); hid_core is used for its pure domain enums only.
# Host tests: test/ui_store and test/ui_nav (real LVGL 9.6 sources, headless display).
idf_component_register(
    SRCS "src/ui_mailbox.c" "src/ui_actions.c" "src/ui_present.c" "src/ui_store.c" "src/ui_router.c" "src/ui_overlay.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl hid_core)
