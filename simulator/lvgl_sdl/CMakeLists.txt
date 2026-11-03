# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Desktop simulator of the KVMpad UI (TASK-070D, 07 §10 level L1): the real product shell composition on the real
# LVGL 9.6 pinned by dependencies.lock, with a fake platform (recorder instead of the domain, fixture files
# instead of the producers). The renderer is headless, so the suite needs no display server and no SDL package;
# see simulator/README.md for the interactive-window status.
#
#   export PATH=.../llvm-mingw/bin:.../ninja:.../cmake/bin:$PATH
#   cmake -S simulator/lvgl_sdl -B ../build-sim -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-sim
#   ctest --test-dir ../build-sim --output-on-failure
#   ../build-sim/kvm_sim --update-golden        # regenerate simulator/golden/
cmake_minimum_required(VERSION 3.25)
project(kvmpad_simulator C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(SIM ${CMAKE_CURRENT_SOURCE_DIR})
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(UI_COMP_TEST ${REPO}/test/ui_components) # host_lv_mem.c + ui_test_env.c, shared with TASK-060D (not edited)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl CACHE PATH "LVGL 9.6 source tree (pinned by dependencies.lock)")

option(KVM_SANITIZE "Build with AddressSanitizer (+ UBSan for the project sources)" ON)

if(NOT EXISTS ${LVGL_DIR}/include/lvgl/lvgl.h)
    message(FATAL_ERROR "LVGL not found in ${LVGL_DIR}: run `idf.py reconfigure` once to fetch the managed components")
endif()

# LVGL plus the counting allocator. LV_USE_LODEPNG is on in ${SIM}/lv_conf.h, which brings in the lodepng LVGL
# already vendors: the golden PNGs are written and read with it, so the simulator adds no dependency of its own.
# ${SIM} must stay the FIRST include directory: ${UI_COMP_TEST} holds another lv_conf.h (the TASK-060D one).
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES} ${UI_COMP_TEST}/host_lv_mem.c)
target_include_directories(lvgl_host PUBLIC ${SIM} ${UI_COMP_TEST})
target_include_directories(lvgl_host SYSTEM PUBLIC ${LVGL_DIR})
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
set(UI_VIEWS ${REPO}/components/ui_views)
set(UI_THEME ${REPO}/components/ui_theme)
set(UI_COMP ${REPO}/components/ui_components)
set(HID_CORE ${REPO}/components/hid_core)

# ADR-025 FR-751: the same generated About strings the firmware compiles (components/ui_views/notices.cmake).
include(${UI_VIEWS}/notices.cmake)
kvm_notices_generate(${CMAKE_CURRENT_BINARY_DIR}/generated KVM_NOTICES_C KVM_NOTICES_DIR)

# Exactly the product UI stack: ui_core + ui_theme + ui_components + ui_views (the shell composition included)
# plus the compiled chord allowlist. No TinyUSB, no hid_core state machine, nothing that could emit a report.
add_library(ui_stack STATIC
    ${UI_CORE}/src/ui_router.c ${UI_CORE}/src/ui_overlay.c ${UI_CORE}/src/ui_store.c ${UI_CORE}/src/ui_mailbox.c
    ${UI_CORE}/src/ui_present.c ${UI_CORE}/src/ui_actions.c
    ${UI_THEME}/src/ui_theme.c
    ${UI_THEME}/src/ui_font_blob.c
    ${UI_THEME}/src/ui_icon_star.c
    ${UI_COMP}/src/ui_status_item.c ${UI_COMP}/src/ui_topbar.c ${UI_COMP}/src/ui_action_key.c
    ${UI_COMP}/src/ui_segmented.c ${UI_COMP}/src/ui_modal.c ${UI_COMP}/src/ui_toast.c
    ${UI_VIEWS}/src/ui_app.c ${UI_VIEWS}/src/ui_view_mouse.c ${UI_VIEWS}/src/ui_view_keyboard.c
    ${UI_VIEWS}/src/ui_view_shortcuts.c ${UI_VIEWS}/src/ui_view_status.c ${UI_VIEWS}/src/ui_remote_view.c
    ${UI_VIEWS}/src/ui_view_settings.c ${KVM_NOTICES_C}
    ${HID_CORE}/src/hid_keymap.c
    ${REPO}/components/ime_engine/src/ime_hangul.c
    ${UI_COMP_TEST}/ui_test_env.c)
target_include_directories(ui_stack PUBLIC ${UI_VIEWS}/include ${UI_CORE}/include ${UI_CORE}/src
    ${UI_THEME}/include ${UI_COMP}/include ${HID_CORE}/include ${REPO}/components/usb_hid_device/include
    ${REPO}/components/ime_engine/include
    ${REPO}/test/host_unit ${KVM_NOTICES_DIR})
target_compile_options(ui_stack PRIVATE ${STRICT})
target_link_libraries(ui_stack PUBLIC lvgl_host)
# ADR-024 TECH-078: the generated Hangul legend font (OFL-1.1), as in the firmware; generated code, so not ${STRICT}.
add_library(hangul_font STATIC ${UI_THEME}/fonts/kvm_font_hangul_24.c)
target_link_libraries(hangul_font PUBLIC lvgl_host)
target_link_libraries(ui_stack PUBLIC hangul_font)
target_compile_definitions(ui_stack PRIVATE KVM_HAVE_FONT_HANGUL_24)

# ADR-024 FR-741/742: the generated HOST layout tables the KEYBOARD view and the Settings picker read. The table
# itself is machine-written, so it is compiled without ${STRICT}; the accessors are not.
set(KBD ${REPO}/components/kbd_layouts)
add_library(kbd_layouts STATIC ${KBD}/src/kbd_layouts.c ${KBD}/src/kbd_unicode.c ${KBD}/src/kbd_text.c)
target_include_directories(kbd_layouts PUBLIC ${KBD}/include ${HID_CORE}/include)
target_compile_options(kbd_layouts PRIVATE ${STRICT})
if(EXISTS ${KBD}/generated/kbd_layouts_data.c)
    add_library(kbd_layouts_data STATIC ${KBD}/generated/kbd_layouts_data.c)
    target_include_directories(kbd_layouts_data PRIVATE ${KBD}/include ${HID_CORE}/include)
    target_link_libraries(kbd_layouts PUBLIC kbd_layouts_data)
    target_compile_definitions(kbd_layouts PRIVATE KBD_LAYOUTS_HAVE_TABLES=1)
else()
    target_compile_definitions(kbd_layouts PRIVATE KBD_LAYOUTS_HAVE_TABLES=0)
endif()
target_link_libraries(ui_stack PUBLIC kbd_layouts)

# ADR-024 TECH-078: the generated legend fonts (OFL-1.1), so a golden shows the glyph coverage the firmware has.
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

# The renderer is always built; the fixture and golden directories are baked in so ctest needs no arguments.
add_executable(kvm_sim kvm_sim.c sim_platform.c sim_fixture.c sim_image.c sim_checks.c sim_zh.c sim_ja.c)
# TASK-072: the hanzi font blob (tools/fonts/gen_hanzi_font_blob.py, outside the repository); empty = ZH goldens skipped.
set(KVM_HANZI_BLOB "" CACHE FILEPATH "KVMFONT1 hanzi font blob for the ZH fixtures")
if(KVM_HANZI_BLOB)
    target_compile_definitions(kvm_sim PRIVATE KVM_HANZI_BLOB="${KVM_HANZI_BLOB}")
endif()
# The same for the kanji font blob (Noto Sans JP, gen_hanzi_font_blob.py --set ja); empty = JA goldens skipped.
set(KVM_KANJI_BLOB "" CACHE FILEPATH "KVMFONT1 kanji font blob for the JA fixtures")
if(KVM_KANJI_BLOB)
    target_compile_definitions(kvm_sim PRIVATE KVM_KANJI_BLOB="${KVM_KANJI_BLOB}")
endif()
target_compile_options(kvm_sim PRIVATE ${STRICT})
target_compile_definitions(kvm_sim PRIVATE
    KVM_SIM_FIXTURE_DIR="${SIM}/../fixtures"
    KVM_SIM_GOLDEN_DIR="${SIM}/../golden"
    KVM_SIM_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/rendered")
target_link_libraries(kvm_sim PRIVATE ui_stack)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rendered)

enable_testing()
add_test(NAME kvm_sim COMMAND kvm_sim)
set_tests_properties(kvm_sim PROPERTIES TIMEOUT 1800)
