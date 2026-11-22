# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ssh_identity C)
enable_testing()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LIBSSH_SOURCE "" CACHE PATH "Pinned libssh source from the target port")
set(LIBSSH_BUILD "" CACHE PATH "Matching libssh host build")
find_library(SSH_LIBRARY NAMES ssh HINTS "${LIBSSH_BUILD}/src" NO_DEFAULT_PATH REQUIRED)
set(port "${CMAKE_CURRENT_SOURCE_DIR}/../../../components/ssh_control/port")
add_executable(identity_test identity_test.c "${port}/ssh_control_identity.c")
target_include_directories(identity_test PRIVATE "${port}" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${LIBSSH_SOURCE}/include" "${LIBSSH_BUILD}/include")
target_link_libraries(identity_test PRIVATE "${SSH_LIBRARY}")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    target_compile_options(identity_test PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME host_identity COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/check.py"
    "$<TARGET_FILE:identity_test>" "${LIBSSH_BUILD}/src")
