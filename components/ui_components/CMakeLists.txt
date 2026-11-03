# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ui_components (TASK-060D): the six shared product components of 07 §7, hand-written C on LVGL 9.6
# (ADR-014: no UI generator, no XML/JSON DSL). Dependencies are exactly ui_theme and LVGL - no USB,
# no network, no pairing logic (07 §2 dependency rule, AGENTS-UI MUST 4).
# Host tests: test/ui_components (real LVGL 9.6 sources, headless 1024x600 display).
idf_component_register(
    SRCS "src/ui_status_item.c"
         "src/ui_topbar.c"
         "src/ui_action_key.c"
         "src/ui_segmented.c"
         "src/ui_modal.c"
         "src/ui_toast.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl ui_theme)
