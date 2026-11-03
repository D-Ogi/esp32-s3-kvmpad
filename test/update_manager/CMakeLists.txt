# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-110 OTA state machine (pure C, no ESP-IDF). The three pure sources of
# components/update_manager are built here; src/update_platform_esp.c, src/update_transport_https.c
# and src/update_console.c need the IDF build and are not part of this suite.
#
#   cmake -S test/update_manager -B ../build-host-ota -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-ota && ctest --test-dir ../build-host-ota --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_update_manager C)

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

set(UPDATE_MANAGER_SRC
    ${REPO}/components/update_manager/src/update_image.c
    ${REPO}/components/update_manager/src/update_health.c
    ${REPO}/components/update_manager/src/update_manager.c)

foreach(suite test_update_image test_update_health test_update_sm)
    add_executable(${suite} ${suite}.c ${UPDATE_MANAGER_SRC})
    target_include_directories(${suite} PRIVATE
        ${REPO}/components/update_manager/include ${REPO}/test/host_unit
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()
