# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-100 CLIENT protocol v1 (pure C, no ESP-IDF). The whole component builds
# here; only hid_keymap.c is pulled in from hid_core, for the usage and chord allowlists.
#
#   cmake -S test/remote_protocol -B ../build-host-remote -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-remote
#   ctest --test-dir ../build-host-remote --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_remote_protocol C)

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

set(RP_SRC
    ${REPO}/components/remote_protocol/src/remote_json.c
    ${REPO}/components/remote_protocol/src/remote_wire.c
    ${REPO}/components/remote_protocol/src/remote_protocol.c
    ${REPO}/components/hid_core/src/hid_keymap.c)

foreach(suite test_remote_json test_remote_validate test_remote_session test_remote_vectors)
    add_executable(${suite} ${suite}.c ${RP_SRC})
    target_include_directories(${suite} PRIVATE
        ${REPO}/components/remote_protocol/include
        ${REPO}/components/remote_protocol/src
        ${REPO}/components/hid_core/include
        ${REPO}/test/host_unit
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()

# Seeded mutation fuzzer over the frame parser and validator (03 §9 item 3). The default run is
# 200000 inputs; pass a seed and a count to reproduce: fuzz_remote_frame <seed> <iterations>.
add_executable(fuzz_remote_frame fuzz_remote_frame.c ${RP_SRC})
target_include_directories(fuzz_remote_frame PRIVATE
    ${REPO}/components/remote_protocol/include
    ${REPO}/components/remote_protocol/src
    ${REPO}/components/hid_core/include
    ${REPO}/test/host_unit
    ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME fuzz_remote_frame COMMAND fuzz_remote_frame 20261102 200000)
