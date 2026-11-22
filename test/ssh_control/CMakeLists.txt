# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ssh_parser C)
enable_testing()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(components "${CMAKE_CURRENT_SOURCE_DIR}/../../components")
foreach(count 2 4)
    set(suite test_ssh_control_hid${count})
    add_executable(${suite} test_ssh_control.c
        "${components}/ssh_control/src/ssh_control.c"
        "${components}/hid_core/src/hid_keymap.c")
    target_compile_definitions(${suite} PRIVATE CONFIG_TINYUSB_HID_COUNT=${count})
    target_include_directories(${suite} PRIVATE "${components}/ssh_control/include"
        "${components}/hid_core/include" "${components}/usb_hid_device/include")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
        target_compile_options(${suite} PRIVATE -Wall -Wextra -Werror -Wconversion -Wshadow)
    endif()
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()
