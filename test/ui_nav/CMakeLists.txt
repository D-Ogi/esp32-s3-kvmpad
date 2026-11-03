# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the ui_core router and overlay stack (TASK-060C) against the real LVGL 9.6 sources pinned by
# dependencies.lock. Shares lv_conf.h and the counting allocator with test/ui_store (same LVGL options).
#
#   cmake -S test/ui_nav -B ../build-host-nav -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-nav && ctest --test-dir ../build-host-nav --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ui_nav C)

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

add_executable(test_ui_nav test_ui_nav.c
    ${UI_CORE}/src/ui_router.c ${UI_CORE}/src/ui_overlay.c ${UI_CORE}/src/ui_store.c
    ${UI_CORE}/src/ui_mailbox.c ${UI_CORE}/src/ui_present.c)
target_include_directories(test_ui_nav PRIVATE ${UI_CORE}/include ${UI_CORE}/src ${REPO}/components/hid_core/include
    ${REPO}/components/usb_hid_device/include ${REPO}/test/host_unit)
target_compile_options(test_ui_nav PRIVATE ${STRICT})
target_link_libraries(test_ui_nav PRIVATE lvgl_host)
add_test(NAME test_ui_nav COMMAND test_ui_nav)
