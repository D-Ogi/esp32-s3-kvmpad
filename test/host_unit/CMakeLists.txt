# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host unit tests for the pure (IDF-free) parts of the firmware. Each suite is one executable;
# `ctest` runs them all. Sanitizers are on by default where the toolchain supports them.
#
#   cmake -S test/host_unit -B ../build-host -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host && ctest --test-dir ../build-host --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_host_unit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

option(KVM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)

add_compile_options(-Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

function(kvm_suite name)
    cmake_parse_arguments(S "" "" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${name} ${name}.c ${S_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${S_INCLUDES})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

kvm_suite(test_touch_sample
    SOURCES ${REPO}/components/touch_input/src/touch_sample.c
    INCLUDES ${REPO}/components/touch_input/include)
kvm_suite(test_usb_hid_reports
    SOURCES ${REPO}/components/usb_hid_device/src/usb_hid_reports.c
    INCLUDES ${REPO}/components/usb_hid_device/include)
kvm_suite(test_io_ext
    SOURCES ${REPO}/components/board_hal/src/io_ext.c
    INCLUDES ${REPO}/components/board_hal/include)
kvm_suite(test_lcd_pattern
    SOURCES ${REPO}/main/lcd_pattern.c
    INCLUDES ${REPO}/main ${REPO}/components/board_hal/include)

set(HID_CORE_SOURCES
    ${REPO}/components/hid_core/src/hid_core.c
    ${REPO}/components/hid_core/src/kbd_fsm.c
    ${REPO}/components/hid_core/src/hid_keymap.c
    ${REPO}/components/usb_hid_device/src/usb_hid_reports.c)
set(HID_CORE_INCLUDES ${REPO}/components/hid_core/include ${REPO}/components/usb_hid_device/include)
kvm_suite(test_kbd_fsm
    SOURCES ${REPO}/components/hid_core/src/kbd_fsm.c ${REPO}/components/hid_core/src/hid_keymap.c
    INCLUDES ${REPO}/components/hid_core/include)
kvm_suite(test_hid_core SOURCES ${HID_CORE_SOURCES} INCLUDES ${HID_CORE_INCLUDES})
kvm_suite(test_hid_vectors SOURCES ${HID_CORE_SOURCES} INCLUDES ${HID_CORE_INCLUDES})
kvm_suite(test_hid_stress SOURCES ${HID_CORE_SOURCES} INCLUDES ${HID_CORE_INCLUDES})
kvm_suite(test_hid_burst
    SOURCES ${HID_CORE_SOURCES} ${REPO}/components/kbd_layouts/src/kbd_unicode.c
    INCLUDES ${HID_CORE_INCLUDES} ${REPO}/components/kbd_layouts/include)

set(GESTURE_SOURCES ${REPO}/components/gesture_engine/src/gesture_engine.c)
set(GESTURE_INCLUDES ${REPO}/components/gesture_engine/include ${REPO}/components/hid_core/include
    ${REPO}/components/touch_input/include)
kvm_suite(gesture_test_vectors SOURCES ${GESTURE_SOURCES} INCLUDES ${GESTURE_INCLUDES})
kvm_suite(gesture_test_property SOURCES ${GESTURE_SOURCES} INCLUDES ${GESTURE_INCLUDES})
