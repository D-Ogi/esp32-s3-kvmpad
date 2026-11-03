# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-100 secure remote transport: the token presentation of ADR-028 §2, the
# `pair.*` codec of ADR-029 §4/§5 and the session glue that joins `remote_protocol` and the pairing
# core. Everything under test is pure C, so the pinned TF-PSA-Crypto of ESP-IDF v6.1 is built here
# from source as a subproject exactly as test/pairing does, and the fixtures (seeded DRBG, hand
# driven clock, in-RAM paired register) are reused from test/pairing rather than duplicated.
#
# The build directory lives outside the repository (CONTRIBUTING.md):
#
#   export PATH=/o/Praca/KVMpad/.tools/llvm-mingw-20261020-ucrt-x86_64/bin:$PATH
#   cmake -S test/secure_remote -B ../build-host-secure -G Ninja \
#         -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
#         -DMBEDTLS_ROOT=O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls
#   cmake --build ../build-host-secure
#   ctest --test-dir ../build-host-secure --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_secure_remote C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(MBEDTLS_ROOT "O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls"
    CACHE PATH "Root of the mbedTLS tree pinned by ESP-IDF v6.1")
if(NOT EXISTS ${MBEDTLS_ROOT}/tf-psa-crypto/CMakeLists.txt)
    message(FATAL_ERROR "MBEDTLS_ROOT does not point at the pinned mbedTLS tree: ${MBEDTLS_ROOT}")
endif()

# Off by default for the same reason as in test/pairing: the llvm-mingw runtime used here ships no
# ASan runtime for the x86_64-w64-mingw32 target, so turning it on fails to link.
option(KVM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# --- pinned crypto as a subproject ---------------------------------------------------------------

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${REPO}/test/pairing/host_psa_config.h"
    CACHE FILEPATH "" FORCE)

# The two headers ESP-IDF normally supplies from its port directory; must come first.
include_directories(BEFORE ${REPO}/test/pairing/host_shim)

add_subdirectory(${MBEDTLS_ROOT}/tf-psa-crypto ${CMAKE_BINARY_DIR}/tfpsa EXCLUDE_FROM_ALL)

# --- the cores under test ------------------------------------------------------------------------

set(SRT_SRC
    ${REPO}/components/secure_remote/src/kvm_srt_auth.c
    ${REPO}/components/secure_remote/src/kvm_srt_pair_json.c
    ${REPO}/components/secure_remote/src/kvm_srt.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_pake.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_session.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_client.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_wire.c
    ${REPO}/components/remote_protocol/src/remote_json.c
    ${REPO}/components/remote_protocol/src/remote_wire.c
    ${REPO}/components/remote_protocol/src/remote_protocol.c
    ${REPO}/components/hid_core/src/hid_keymap.c)

add_library(srt_core STATIC ${SRT_SRC})
target_include_directories(srt_core PUBLIC
    ${REPO}/components/secure_remote/include
    ${REPO}/components/secure_remote/pairing/include
    ${REPO}/components/remote_protocol/include
    ${REPO}/components/hid_core/include
    ${REPO}/components/config_store/include)
target_link_libraries(srt_core PUBLIC tfpsacrypto)
target_compile_options(srt_core PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)

# det_rng.c is the seeded DRBG and hand-driven clock of test/pairing; PSA draws from it too.
add_library(srt_fixture STATIC ${REPO}/test/pairing/det_rng.c)
target_include_directories(srt_fixture PUBLIC ${REPO}/test/pairing ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(srt_fixture PUBLIC srt_core)
target_compile_options(srt_fixture PRIVATE -Wall -Wextra -g -O1)

if(KVM_SANITIZE)
    foreach(t srt_core srt_fixture)
        target_compile_options(${t} PUBLIC
            -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${t} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

# --- suites --------------------------------------------------------------------------------------

enable_testing()

foreach(suite test_srt_auth test_srt_pair_json test_srt_session)
    add_executable(${suite} ${CMAKE_CURRENT_SOURCE_DIR}/${suite}.c)
    target_link_libraries(${suite} PRIVATE srt_fixture)
    target_include_directories(${suite} PRIVATE ${REPO}/test/host_unit)
    target_compile_options(${suite} PRIVATE
        -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()

# Seeded mutation fuzzer over the pairing endpoint: every frame goes through the envelope codec and
# the pairing state machine, and no input may leave a HID intent, a stored client or a crash behind
# (03 §9 item 3, AT-EXT-17). Reproduce with: fuzz_srt_pair <seed> <iterations>.
add_executable(fuzz_srt_pair ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_srt_pair.c)
target_link_libraries(fuzz_srt_pair PRIVATE srt_fixture)
target_include_directories(fuzz_srt_pair PRIVATE ${REPO}/test/host_unit)
target_compile_options(fuzz_srt_pair PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
add_test(NAME fuzz_srt_pair COMMAND fuzz_srt_pair 20261102 20000)
