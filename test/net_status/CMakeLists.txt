# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the net_status link state machine (TASK-080, ADR-015). Pure C, strict flags, ASan + UBSan.
#
#   cmake -S test/net_status -B ../build-host-net -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-net && ctest --test-dir ../build-host-net --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_net_status C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

option(KVM_SANITIZE "Build with AddressSanitizer + UBSan" ON)
set(STRICT -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    list(APPEND STRICT -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
add_executable(test_net_fsm test_net_fsm.c ${REPO}/components/net_status/src/net_fsm.c)
target_include_directories(test_net_fsm PRIVATE ${REPO}/components/net_status/include ${REPO}/test/host_unit)
target_compile_options(test_net_fsm PRIVATE ${STRICT})
add_test(NAME test_net_fsm COMMAND test_net_fsm)
