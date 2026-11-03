# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the vendored PinyinIME decoder behind ime_pinyin (TASK-072, FR-746, TECH-080). The engine's binary
# dictionary uses a 32-bit size_t (the ESP32-S3 layout), so this suite must be built for a 32-bit target; the build
# fails on a 64-bit one. The dictionary is built here from the vendored sources and data, as on the device.
#
#   cmake -S test/ime_pinyin -B ../build-host-pinyin -G Ninja \
#         -DCMAKE_C_COMPILER=i686-w64-mingw32-clang -DCMAKE_CXX_COMPILER=i686-w64-mingw32-clang++
#   cmake --build ../build-host-pinyin && ctest --test-dir ../build-host-pinyin --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ime_pinyin C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(IME ${REPO}/components/ime_engine)
set(PY ${IME}/pinyinime)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "test/ime_pinyin needs a 32-bit target (the dictionary layout of the ESP32-S3)")
endif()

option(KVM_SANITIZE "Build with AddressSanitizer" ON)
if(KVM_SANITIZE)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(WIN32)
    add_link_options(-static-libstdc++)
endif()
add_compile_options(-g -O1)

set(ENGINE_SRCS
    ${PY}/share/dictlist.cpp ${PY}/share/dicttrie.cpp ${PY}/share/lpicache.cpp ${PY}/share/matrixsearch.cpp
    ${PY}/share/mystdlib.cpp ${PY}/share/ngram.cpp ${PY}/share/pinyinime.cpp ${PY}/share/searchutility.cpp
    ${PY}/share/spellingtable.cpp ${PY}/share/spellingtrie.cpp ${PY}/share/splparser.cpp ${PY}/share/sync.cpp
    ${PY}/share/userdict.cpp ${PY}/share/utf16char.cpp ${PY}/share/utf16reader.cpp)
set(BUILDER_SRCS
    ${PY}/command/pinyinime_dictbuilder.cpp ${PY}/share/dictbuilder.cpp ${PY}/share/dictlist.cpp
    ${PY}/share/dicttrie.cpp ${PY}/share/lpicache.cpp ${PY}/share/mystdlib.cpp ${PY}/share/ngram.cpp
    ${PY}/share/searchutility.cpp ${PY}/share/spellingtable.cpp ${PY}/share/spellingtrie.cpp
    ${PY}/share/splparser.cpp ${PY}/share/utf16char.cpp ${PY}/share/utf16reader.cpp)

# The vendored code is compiled unchanged; its warnings are silenced, not fixed.
add_executable(pinyin_dictbuilder ${BUILDER_SRCS})
target_compile_definitions(pinyin_dictbuilder PRIVATE ___BUILD_MODEL___)
target_compile_options(pinyin_dictbuilder PRIVATE -w)

# Upstream layout: the builder writes ../../res/raw/dict_pinyin.dat relative to its working directory.
set(DICT ${CMAKE_CURRENT_BINARY_DIR}/res/raw/dict_pinyin.dat)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/x ${CMAKE_CURRENT_BINARY_DIR}/res/raw)
add_custom_command(OUTPUT ${DICT}
    COMMAND pinyin_dictbuilder ${PY}/data/rawdict_utf16_65105_freq.txt ${PY}/data/valid_utf16.txt
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/run/x
    DEPENDS pinyin_dictbuilder ${PY}/data/rawdict_utf16_65105_freq.txt ${PY}/data/valid_utf16.txt
    COMMENT "Building dict_pinyin.dat")
add_custom_target(pinyin_dict ALL DEPENDS ${DICT})

add_library(pinyin_engine STATIC ${ENGINE_SRCS})
target_include_directories(pinyin_engine PRIVATE ${IME}/port)
# As in the firmware: the C heap calls renamed to ime_engine_* (port/ime_alloc.h; plain malloc on a host).
target_compile_options(pinyin_engine PRIVATE -w -Dmalloc=ime_engine_malloc -Dcalloc=ime_engine_calloc
    -Drealloc=ime_engine_realloc -Dfree=ime_engine_free)

enable_testing()
add_executable(test_ime_pinyin test_ime_pinyin.c ${IME}/src/ime_pinyin.cpp ${IME}/src/ime_alloc.cpp)
target_include_directories(test_ime_pinyin PRIVATE ${IME}/include ${IME}/port ${REPO}/test/host_unit)
target_compile_options(test_ime_pinyin PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wconversion -Wno-sign-conversion>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror>)
find_package(Threads REQUIRED)
target_link_libraries(test_ime_pinyin PRIVATE pinyin_engine Threads::Threads)
add_dependencies(test_ime_pinyin pinyin_dict)
add_test(NAME test_ime_pinyin COMMAND test_ime_pinyin ${DICT} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Reload leak regression: its own counting allocator replaces ime_alloc.cpp and the heap, so no sanitizer here.
add_library(pinyin_engine_plain STATIC ${ENGINE_SRCS})
target_include_directories(pinyin_engine_plain PRIVATE ${IME}/port)
target_compile_options(pinyin_engine_plain PRIVATE -w -fno-sanitize=all -Dmalloc=ime_engine_malloc
    -Dcalloc=ime_engine_calloc -Drealloc=ime_engine_realloc -Dfree=ime_engine_free)
add_executable(test_pinyin_leak test_pinyin_leak.cpp ${IME}/src/ime_pinyin.cpp)
target_include_directories(test_pinyin_leak PRIVATE ${IME}/include ${IME}/port)
target_compile_options(test_pinyin_leak PRIVATE -fno-sanitize=all -Wall -Wextra -Werror)
target_link_options(test_pinyin_leak PRIVATE -fno-sanitize=all)
target_link_libraries(test_pinyin_leak PRIVATE pinyin_engine_plain Threads::Threads)
add_dependencies(test_pinyin_leak pinyin_dict)
add_test(NAME test_pinyin_leak COMMAND test_pinyin_leak ${DICT} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
