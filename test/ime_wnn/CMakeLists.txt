# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the vendored AOSP OpenWnn engine behind ime_wnn (TASK-072, FR-746, TECH-080). The
# engine parses its dictionaries byte by byte and uses no size_t field inside them, so this suite
# builds and passes on a 64-bit and on a 32-bit target alike (unlike test/ime_pinyin, which needs
# 32 bits). The dictionary container is built here from the vendored data, exactly as the image is.
#
#   cmake -S test/ime_wnn -B ../build-host-wnn -G Ninja -DCMAKE_C_COMPILER=clang \
#         -DCMAKE_CXX_COMPILER=clang++
#   cmake --build ../build-host-wnn && ctest --test-dir ../build-host-wnn --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ime_wnn C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(IME ${REPO}/components/ime_engine)
set(WNN ${IME}/openwnn)

option(KVM_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
if(KVM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
if(WIN32)
    add_link_options(-static-libstdc++)
endif()
add_compile_options(-g -O1)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The Japanese dictionary container, from the vendored WnnJpnDic.c, as tools/ime/build_ime_data.py
# puts it into the ime_data image.
set(BLOB ${CMAKE_CURRENT_BINARY_DIR}/wnn_ja.bin)
add_custom_command(OUTPUT ${BLOB}
    COMMAND ${Python3_EXECUTABLE} -I ${REPO}/tools/ime/build_wnn_blob.py --out ${BLOB}
            --src ${WNN}/data/WnnJpnDic.c
    DEPENDS ${REPO}/tools/ime/build_wnn_blob.py ${WNN}/data/WnnJpnDic.c
    COMMENT "Building wnn_ja.bin")
add_custom_target(wnn_blob ALL DEPENDS ${BLOB})

# UndefinedBehaviorSanitizer finding in the vendored code, left in place on purpose: the
# NJ_INT32_READ macro of openwnn/include/nj_dic.h builds a 32-bit value as
# ((NJ_INT32)in[0] << 24) & 0xff000000, and NJ_INT32 is a signed long, so every dictionary header
# field whose top byte is >= 0x80 is a signed left-shift overflow. It is benign on a two's
# complement target (the mask drops the sign bit again) and the engine is compiled unchanged, so the
# shift-base check alone is switched off for the vendored engine and for the derived translation that
# uses the same macros. Every other UBSan check stays on, for the vendored code too, and a failure
# aborts the test (-fno-sanitize-recover=all above).
set(WNN_UB_EXEMPT -fno-sanitize=shift-base)

# The vendored engine is compiled unchanged; its warnings are silenced, not fixed.
file(GLOB WNN_ENGINE_SRCS ${WNN}/engine/*.c)
add_library(wnn_engine STATIC ${WNN_ENGINE_SRCS})
target_include_directories(wnn_engine PUBLIC ${WNN}/include)
target_compile_options(wnn_engine PRIVATE -w ${WNN_UB_EXEMPT})

# The derived C translations and the KVMpad API are held to the project's warning level.
set(KVM_WARNINGS -Wall -Wextra -Werror -Wconversion -Wno-sign-conversion)
add_library(wnn_kvm STATIC ${WNN}/kvm/wnn_convert.c ${WNN}/kvm/wnn_romkan.c)
target_include_directories(wnn_kvm PUBLIC ${WNN}/kvm)
target_link_libraries(wnn_kvm PUBLIC wnn_engine)
target_compile_options(wnn_kvm PRIVATE ${KVM_WARNINGS} ${WNN_UB_EXEMPT})

enable_testing()

add_executable(test_ime_wnn test_ime_wnn.c ${IME}/src/ime_wnn.c ${IME}/src/ime_alloc.cpp)
target_include_directories(test_ime_wnn PRIVATE ${IME}/include ${IME}/port ${REPO}/test/host_unit)
target_link_libraries(test_ime_wnn PRIVATE wnn_kvm)
target_compile_options(test_ime_wnn PRIVATE
    $<$<COMPILE_LANGUAGE:C>:${KVM_WARNINGS}> $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror>)
add_dependencies(test_ime_wnn wnn_blob)
add_test(NAME test_ime_wnn COMMAND test_ime_wnn ${BLOB} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Heap regression: its own counting allocator replaces ime_alloc.cpp, so no sanitizer here.
add_library(wnn_engine_plain STATIC ${WNN_ENGINE_SRCS})
target_include_directories(wnn_engine_plain PUBLIC ${WNN}/include)
target_compile_options(wnn_engine_plain PRIVATE -w -fno-sanitize=all)
add_library(wnn_kvm_plain STATIC ${WNN}/kvm/wnn_convert.c ${WNN}/kvm/wnn_romkan.c)
target_include_directories(wnn_kvm_plain PUBLIC ${WNN}/kvm)
target_link_libraries(wnn_kvm_plain PUBLIC wnn_engine_plain)
target_compile_options(wnn_kvm_plain PRIVATE ${KVM_WARNINGS} -fno-sanitize=all)

add_executable(test_wnn_leak test_wnn_leak.cpp ${IME}/src/ime_wnn.c)
target_include_directories(test_wnn_leak PRIVATE ${IME}/include ${IME}/port)
target_compile_options(test_wnn_leak PRIVATE -fno-sanitize=all
    $<$<COMPILE_LANGUAGE:C>:${KVM_WARNINGS}> $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Werror>)
target_link_options(test_wnn_leak PRIVATE -fno-sanitize=all)
target_link_libraries(test_wnn_leak PRIVATE wnn_kvm_plain)
add_dependencies(test_wnn_leak wnn_blob)
add_test(NAME test_wnn_leak COMMAND test_wnn_leak ${BLOB} WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
