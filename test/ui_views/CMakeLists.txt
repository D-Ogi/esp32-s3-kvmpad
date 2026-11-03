# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the ui_views screens (TASK-070A..C) against the real LVGL 9.6 sources pinned by dependencies.lock,
# with the real ui_core router, store, mailbox and action gateway. Shares lv_conf.h and the counting allocator
# with test/ui_store (same LVGL options).
#
#   cmake -S test/ui_views -B ../build-host-views -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-views && ctest --test-dir ../build-host-views --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ui_views C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(UI_STORE_TEST ${REPO}/test/ui_store)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl CACHE PATH "LVGL 9.6 source tree (pinned by dependencies.lock)")

option(KVM_SANITIZE "Build with AddressSanitizer (+ UBSan for the project sources)" ON)

if(NOT EXISTS ${LVGL_DIR}/include/lvgl/lvgl.h)
    message(FATAL_ERROR "LVGL not found in ${LVGL_DIR}: run `idf.py reconfigure` once to fetch the managed components")
endif()

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES} ${UI_STORE_TEST}/host_lv_mem.c)
target_include_directories(lvgl_host SYSTEM PUBLIC ${LVGL_DIR})
target_include_directories(lvgl_host PUBLIC ${UI_STORE_TEST})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_compile_options(lvgl_host PRIVATE -w -g -O1)
if(KVM_SANITIZE)
    target_compile_options(lvgl_host PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

set(STRICT -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    list(APPEND STRICT -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(UI_CORE ${REPO}/components/ui_core)
enable_testing()

set(UI_VIEWS ${REPO}/components/ui_views)
set(UI_THEME ${REPO}/components/ui_theme)
set(UI_COMP ${REPO}/components/ui_components)

# ADR-025 FR-751: the same generated About strings the firmware compiles (components/ui_views/notices.cmake).
include(${UI_VIEWS}/notices.cmake)
kvm_notices_generate(${CMAKE_CURRENT_BINARY_DIR}/generated KVM_NOTICES_C KVM_NOTICES_DIR)

# ui_core + ui_theme + ui_components as one strict library: what every view links against in the firmware.
add_library(ui_stack STATIC
    ${UI_CORE}/src/ui_router.c ${UI_CORE}/src/ui_overlay.c ${UI_CORE}/src/ui_store.c ${UI_CORE}/src/ui_mailbox.c
    ${UI_CORE}/src/ui_present.c ${UI_CORE}/src/ui_actions.c
    ${UI_THEME}/src/ui_theme.c
    ${UI_THEME}/src/ui_font_blob.c
    ${UI_THEME}/src/ui_icon_star.c
    ${UI_COMP}/src/ui_status_item.c ${UI_COMP}/src/ui_topbar.c ${UI_COMP}/src/ui_action_key.c
    ${UI_COMP}/src/ui_segmented.c ${UI_COMP}/src/ui_modal.c ${UI_COMP}/src/ui_toast.c
    ${REPO}/components/ime_engine/src/ime_hangul.c
    ${KVM_NOTICES_C})
target_include_directories(ui_stack PUBLIC ${UI_VIEWS}/include ${UI_CORE}/include ${UI_CORE}/src ${UI_THEME}/include
    ${UI_COMP}/include ${REPO}/components/hid_core/include ${REPO}/components/usb_hid_device/include
    ${REPO}/components/ime_engine/include
    ${REPO}/test/host_unit ${KVM_NOTICES_DIR}
    # Headers only, for the static assertions that pin the Settings mirrors to the config_store record
    # (ui_views itself must not depend on config_store).
    ${REPO}/components/config_store/include)
target_compile_options(ui_stack PRIVATE ${STRICT})
target_link_libraries(ui_stack PUBLIC lvgl_host)
# ADR-024 FR-741/742: the HOST layout tables the KEYBOARD view draws its legends and geometry from. The generated
# table is machine-written, so it is compiled without ${STRICT}; the accessors are not.
set(KBD ${REPO}/components/kbd_layouts)
add_library(kbd_layouts STATIC ${KBD}/src/kbd_layouts.c ${KBD}/src/kbd_unicode.c ${KBD}/src/kbd_text.c)
target_include_directories(kbd_layouts PUBLIC ${KBD}/include ${REPO}/components/hid_core/include)
target_compile_options(kbd_layouts PRIVATE ${STRICT})
if(EXISTS ${KBD}/generated/kbd_layouts_data.c)
    add_library(kbd_layouts_data STATIC ${KBD}/generated/kbd_layouts_data.c)
    target_include_directories(kbd_layouts_data PRIVATE ${KBD}/include ${REPO}/components/hid_core/include)
    target_link_libraries(kbd_layouts PUBLIC kbd_layouts_data)
    target_compile_definitions(kbd_layouts PRIVATE KBD_LAYOUTS_HAVE_TABLES=1)
else()
    target_compile_definitions(kbd_layouts PRIVATE KBD_LAYOUTS_HAVE_TABLES=0)
endif()
target_link_libraries(ui_stack PUBLIC kbd_layouts)

# ADR-024 TECH-078: the generated Hangul legend font (OFL-1.1), as in the firmware; generated code, so not ${STRICT}.
add_library(hangul_font STATIC ${UI_THEME}/fonts/kvm_font_hangul_24.c)
target_link_libraries(hangul_font PUBLIC lvgl_host)
target_link_libraries(ui_stack PUBLIC hangul_font)
target_compile_definitions(ui_stack PRIVATE KVM_HAVE_FONT_HANGUL_24)

# ADR-024 TECH-078 / FR-742: the generated keyboard legend fonts (OFL-1.1), so the host tests and the simulator
# measure the same glyph coverage the firmware has. Generated code, so not ${STRICT}.
set(legend_srcs "")
foreach(sz 24 16 14)
    if(EXISTS ${UI_THEME}/fonts/kvm_font_legend_${sz}.c)
        list(APPEND legend_srcs ${UI_THEME}/fonts/kvm_font_legend_${sz}.c)
    endif()
endforeach()
if(legend_srcs)
    add_library(legend_font STATIC ${legend_srcs})
    target_link_libraries(legend_font PUBLIC lvgl_host)
    target_link_libraries(ui_stack PUBLIC legend_font)
    target_compile_definitions(ui_stack PRIVATE KVM_HAVE_FONT_LEGEND)
endif()

# One executable per view; a view's target appears as soon as its test file exists (TASK-070A..C).
function(ui_view_test name)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.c AND EXISTS ${UI_VIEWS}/src/${name}.c)
        add_executable(test_${name} test_${name}.c ${UI_VIEWS}/src/${name}.c ${ARGN})
        target_compile_options(test_${name} PRIVATE ${STRICT})
        target_link_libraries(test_${name} PRIVATE ui_stack)
        add_test(NAME test_${name} COMMAND test_${name})
    endif()
endfunction()

ui_view_test(ui_view_mouse)
ui_view_test(ui_view_keyboard)
ui_view_test(ui_view_shortcuts)
ui_view_test(ui_view_status)
ui_view_test(ui_remote_view)
ui_view_test(ui_view_settings)

# The shell composition links every view source present in the tree.
set(VIEW_SRCS "")
foreach(v ui_view_mouse ui_view_keyboard ui_view_shortcuts ui_view_status ui_remote_view ui_view_settings)
    if(EXISTS ${UI_VIEWS}/src/${v}.c)
        list(APPEND VIEW_SRCS ${UI_VIEWS}/src/${v}.c)
    endif()
endforeach()
list(REMOVE_ITEM VIEW_SRCS ${UI_VIEWS}/src/ui_app.c)
add_executable(test_ui_app test_ui_app.c ${UI_VIEWS}/src/ui_app.c ${VIEW_SRCS})
target_compile_options(test_ui_app PRIVATE ${STRICT})
target_link_libraries(test_ui_app PRIVATE ui_stack)
add_test(NAME test_ui_app COMMAND test_ui_app)
