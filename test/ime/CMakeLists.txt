# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of TASK-072 parts that need neither the board nor a dictionary: the Hangul 2-set composer
# (ime_engine, AT-EXT-26 Hangul vectors), the Unicode delivery sequences (kbd_layouts, FR-747) and the ime_data image
# directory parser (TECH-081). The PinyinIME decoder has its own 32-bit suite in test/ime_pinyin.
#
#   cmake -S test/ime -B ../build-host-ime -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-ime && ctest --test-dir ../build-host-ime --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ime C)

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

add_executable(test_hangul test_hangul.c ${REPO}/components/ime_engine/src/ime_hangul.c)
target_include_directories(test_hangul PRIVATE ${REPO}/components/ime_engine/include ${REPO}/test/host_unit)
add_test(NAME test_hangul COMMAND test_hangul)

add_executable(test_kbd_unicode test_kbd_unicode.c ${REPO}/components/kbd_layouts/src/kbd_unicode.c)
target_include_directories(test_kbd_unicode PRIVATE ${REPO}/components/kbd_layouts/include ${REPO}/test/host_unit)
add_test(NAME test_kbd_unicode COMMAND test_kbd_unicode)

add_executable(test_kbd_text test_kbd_text.c ${REPO}/components/kbd_layouts/src/kbd_text.c
    ${REPO}/components/kbd_layouts/src/kbd_unicode.c)
target_include_directories(test_kbd_text PRIVATE ${REPO}/components/kbd_layouts/include ${REPO}/test/host_unit)
add_test(NAME test_kbd_text COMMAND test_kbd_text)

add_executable(test_ime_data test_ime_data.c ${REPO}/components/ime_engine/src/ime_data.c)
target_include_directories(test_ime_data PRIVATE ${REPO}/components/ime_engine/include ${REPO}/test/host_unit)
add_test(NAME test_ime_data COMMAND test_ime_data)
