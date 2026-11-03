# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-090 settings store (pure C, no ESP-IDF). src/config_store_nvs.c is the
# only file of the component that is not built here; it needs the IDF build.
#
#   cmake -S test/config_store -B ../build-host-config -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-config && ctest --test-dir ../build-host-config --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_config_store C)

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

set(CONFIG_STORE_SRC
    ${REPO}/components/config_store/src/config_record.c
    ${REPO}/components/config_store/src/config_store.c)

foreach(suite test_config_record test_config_store)
    add_executable(${suite} ${suite}.c ${CONFIG_STORE_SRC})
    target_include_directories(${suite} PRIVATE
        ${REPO}/components/config_store/include ${REPO}/test/host_unit
        ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()
