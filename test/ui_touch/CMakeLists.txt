# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-060E touch router (pure C) together with the real gesture engine.
#
#   cmake -S test/ui_touch -B ../build-host-touch -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-touch && ctest --test-dir ../build-host-touch --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ui_touch C)

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
add_executable(test_touch_router test_touch_router.c
    ${REPO}/components/ui_touch_bridge/src/touch_router.c
    ${REPO}/components/gesture_engine/src/gesture_engine.c
    ${REPO}/components/touch_input/src/touch_sample.c)
target_include_directories(test_touch_router PRIVATE
    ${REPO}/components/ui_touch_bridge/include ${REPO}/components/gesture_engine/include
    ${REPO}/components/hid_core/include ${REPO}/components/touch_input/include ${REPO}/test/host_unit)
add_test(NAME test_touch_router COMMAND test_touch_router)
