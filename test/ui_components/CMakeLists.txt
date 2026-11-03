# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of ui_theme and ui_components (TASK-060D) against the real LVGL 9.6 sources pinned by
# dependencies.lock (managed_components/lvgl__lvgl, fetched by `idf.py reconfigure`). LVGL is
# compiled with the address sanitizer (an observer use-after-free is detected inside LVGL too) but
# without warnings-as-errors; the project sources keep the strict flags of test/host_unit.
#
#   cmake -S test/ui_components -B ../build-host-uicomp -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-uicomp && ctest --test-dir ../build-host-uicomp --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ui_components C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl CACHE PATH "LVGL 9.6 source tree (pinned by dependencies.lock)")
set(KVM_SNAPSHOT_DIR "" CACHE PATH "Directory for the PPM snapshots; empty disables writing them")

option(KVM_SANITIZE "Build with AddressSanitizer (+ UBSan for the project sources)" ON)

if(NOT EXISTS ${LVGL_DIR}/include/lvgl/lvgl.h)
    message(FATAL_ERROR "LVGL not found in ${LVGL_DIR}: run `idf.py reconfigure` once to fetch the managed components")
endif()

file(STRINGS ${LVGL_DIR}/include/lvgl/lv_version.h _ver REGEX "#define LVGL_VERSION_(MAJOR|MINOR|PATCH) ")
message(STATUS "LVGL source: ${LVGL_DIR} ${_ver}")

# ---- LVGL as a static library ---------------------------------------------------------------
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/host_lv_mem.c)
# SYSTEM: the LVGL headers are not held to the project's -Wconversion -Werror
target_include_directories(lvgl_host SYSTEM PUBLIC ${LVGL_DIR})
target_include_directories(lvgl_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_compile_options(lvgl_host PRIVATE -w -g -O1)
if(KVM_SANITIZE)
    target_compile_options(lvgl_host PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

# ---- project sources: strict flags ----------------------------------------------------------
set(STRICT -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    list(APPEND STRICT -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(UI_THEME ${REPO}/components/ui_theme)
set(UI_COMP ${REPO}/components/ui_components)
set(UI_INCLUDES ${UI_THEME}/include ${UI_COMP}/include ${REPO}/test/host_unit)
set(UI_SOURCES
    ${UI_THEME}/src/ui_theme.c
    ${UI_THEME}/src/ui_font_blob.c
    ${UI_COMP}/src/ui_status_item.c
    ${UI_COMP}/src/ui_topbar.c
    ${UI_COMP}/src/ui_action_key.c
    ${UI_COMP}/src/ui_segmented.c
    ${UI_COMP}/src/ui_modal.c
    ${UI_COMP}/src/ui_toast.c)

enable_testing()

function(ui_suite name)
    add_executable(${name} ${name}.c ui_test_env.c ${UI_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${UI_INCLUDES})
    target_compile_options(${name} PRIVATE ${STRICT})
    target_link_libraries(${name} PRIVATE lvgl_host)
    if(KVM_SNAPSHOT_DIR)
        file(MAKE_DIRECTORY ${KVM_SNAPSHOT_DIR})
        target_compile_definitions(${name} PRIVATE KVM_SNAPSHOT_DIR="${KVM_SNAPSHOT_DIR}")
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ui_suite(test_ui_theme)
ui_suite(test_ui_font_blob)
# The real candidate-bar font (tools/fonts/gen_hanzi_font_blob.py, written outside the repository) is checked too
# when its path is given: -DKVM_HANZI_BLOB=../build-ime-data/font_hanzi_24.bin
if(KVM_HANZI_BLOB)
    get_filename_component(KVM_HANZI_BLOB_ABS ${KVM_HANZI_BLOB} ABSOLUTE)
    target_compile_definitions(test_ui_font_blob PRIVATE KVM_HANZI_BLOB="${KVM_HANZI_BLOB_ABS}")
endif()
ui_suite(test_ui_components)
ui_suite(test_ui_overlay)
ui_suite(test_ui_snapshot)
