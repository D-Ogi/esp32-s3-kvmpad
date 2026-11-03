# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Screens (TASK-070A..C) and the shell composition ui_app (07 §5), shared with the simulator. No TinyUSB here (AGENTS-UI §4): hid_core only for its public enums.
# A view's source joins the build as soon as it exists (TASK-070B/C land in parallel with 070A).
set(srcs "src/ui_app.c" "src/ui_view_mouse.c")
foreach(v ui_view_keyboard ui_view_shortcuts ui_view_status ui_remote_view ui_view_settings)
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/src/${v}.c)
        list(APPEND srcs "src/${v}.c")
    endif()
endforeach()
idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    REQUIRES lvgl__lvgl ui_core ui_theme ui_components hid_core ime_engine kbd_layouts)

# ADR-025 FR-751: Settings -> About renders THIRD_PARTY_NOTICES.md, converted to C at configure time. The
# generated header is PUBLIC so a consumer that includes ui_view_settings.h sees it too.
include(${CMAKE_CURRENT_LIST_DIR}/notices.cmake)
kvm_notices_generate(${CMAKE_CURRENT_BINARY_DIR}/generated KVM_NOTICES_C KVM_NOTICES_DIR)
target_sources(${COMPONENT_LIB} PRIVATE ${KVM_NOTICES_C})
target_include_directories(${COMPONENT_LIB} PUBLIC ${KVM_NOTICES_DIR})
