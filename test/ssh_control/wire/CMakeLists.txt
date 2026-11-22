# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ssh_wire C)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LIBSSH_SOURCE "" CACHE PATH "Pinned upstream libssh source from the target port")
set(LIBSSH_BUILD "" CACHE PATH "Matching upstream host build")
find_library(SSH_LIBRARY NAMES ssh HINTS "${LIBSSH_BUILD}/src" NO_DEFAULT_PATH REQUIRED)
set(components "${CMAKE_CURRENT_SOURCE_DIR}/../../../components")
add_executable(ssh_wire_server server.c "${components}/ssh_control/port/ssh_control_session.c"
    "${components}/ssh_control/src/ssh_control.c" "${components}/hid_core/src/hid_keymap.c")
target_include_directories(ssh_wire_server PRIVATE "${LIBSSH_SOURCE}/include" "${LIBSSH_BUILD}/include"
    "${components}/ssh_control/port" "${components}/ssh_control/include"
    "${components}/hid_core/include" "${components}/usb_hid_device/include")
target_link_libraries(ssh_wire_server PRIVATE "${SSH_LIBRARY}")
if(WIN32)
    target_link_libraries(ssh_wire_server PRIVATE ws2_32)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(ssh_wire_server PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME openssh_loopback COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/check.py"
    "$<TARGET_FILE:ssh_wire_server>" "${LIBSSH_BUILD}/src" "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(openssh_loopback PROPERTIES TIMEOUT 120)
