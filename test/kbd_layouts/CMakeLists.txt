# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of TASK-071 (AT-EXT-22): the generator's own tests (fake keymaps, no libxkbcommon), the accessors over
# tables emitted from those fake keymaps, and - when components/kbd_layouts/generated/kbd_layouts_data.c exists - the
# accessors and invariants over the committed tables.
#
#   cmake -S test/kbd_layouts -B ../build-host-kbd -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-kbd && ctest --test-dir ../build-host-kbd --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_kbd_layouts C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(KBD ${REPO}/components/kbd_layouts)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

option(KVM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
add_compile_options(-Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

add_test(NAME test_gen_layouts COMMAND Python3::Interpreter -I ${REPO}/tools/kbd_layouts/test_gen_layouts.py)

set(FAKE_DATA ${CMAKE_CURRENT_BINARY_DIR}/fake/kbd_layouts_data.c)
add_custom_command(OUTPUT ${FAKE_DATA}
    COMMAND Python3::Interpreter -I ${REPO}/tools/kbd_layouts/test_gen_layouts.py --emit ${CMAKE_CURRENT_BINARY_DIR}/fake
    DEPENDS ${REPO}/tools/kbd_layouts/gen_layouts.py ${REPO}/tools/kbd_layouts/test_gen_layouts.py
    COMMENT "Emitting the fake layout tables")
add_executable(test_kbd_layouts_fake test_kbd_layouts.c ${KBD}/src/kbd_layouts.c ${KBD}/src/kbd_unicode.c ${FAKE_DATA})
target_include_directories(test_kbd_layouts_fake PRIVATE ${KBD}/include ${REPO}/test/host_unit)
target_compile_definitions(test_kbd_layouts_fake PRIVATE KBD_TEST_FAKE=1)
add_test(NAME test_kbd_layouts_fake COMMAND test_kbd_layouts_fake)

if(EXISTS ${KBD}/generated/kbd_layouts_data.c)
    add_executable(test_kbd_layouts test_kbd_layouts.c ${KBD}/src/kbd_layouts.c ${KBD}/src/kbd_unicode.c
        ${KBD}/generated/kbd_layouts_data.c)
    target_include_directories(test_kbd_layouts PRIVATE ${KBD}/include ${REPO}/test/host_unit)
    add_test(NAME test_kbd_layouts COMMAND test_kbd_layouts)
else()
    message(STATUS "kbd_layouts: no generated tables yet; only the fake-table tests are built")
endif()
