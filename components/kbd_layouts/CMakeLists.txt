# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# kbd_layouts (TASK-071/072): pure C, also compiled by the host tests (test/ime, test/kbd_layouts). The Unicode
# delivery sequences, the text helpers and the accessors over the generated HOST layout tables
# (generated/kbd_layouts_data.c, written by tools/kbd_layouts/gen_layouts.py). A tree in which the generator has not
# run yet builds with no layout tables (kbd_layouts_count_get() == 0).
set(srcs "src/kbd_unicode.c" "src/kbd_text.c" "src/kbd_layouts.c")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/generated/kbd_layouts_data.c")
    list(APPEND srcs "generated/kbd_layouts_data.c")
    set(have_tables 1)
else()
    set(have_tables 0)
endif()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include")
target_compile_definitions(${COMPONENT_LIB} PRIVATE KBD_LAYOUTS_HAVE_TABLES=${have_tables})