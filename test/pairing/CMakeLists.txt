# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of the TASK-100 pairing core (ADR-022, ADR-029). The whole pairing core is pure C over
# PSA crypto, so the pinned TF-PSA-Crypto of ESP-IDF v6.1 is built here from source as a subproject
# and the same sources are exercised that the firmware and the Windows CLIENT compile.
#
# The build directory lives outside the repository (CONTRIBUTING.md):
#
#   export PATH=/o/Praca/KVMpad/.tools/llvm-mingw-20261020-ucrt-x86_64/bin:$PATH
#   cmake -S test/pairing -B ../build-host-pairing -G Ninja \
#         -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
#         -DMBEDTLS_ROOT=O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls
#   cmake --build ../build-host-pairing
#   ctest --test-dir ../build-host-pairing --output-on-failure
#
# Regenerating the interop vectors (they are checked in, so this only needs running when the format
# or a label changes):
#
#   ../build-host-pairing/gen_pairing_vectors.exe test/pairing/vectors
cmake_minimum_required(VERSION 3.25)
project(kvmpad_pairing C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)

set(MBEDTLS_ROOT "O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls"
    CACHE PATH "Root of the mbedTLS tree pinned by ESP-IDF v6.1")
if(NOT EXISTS ${MBEDTLS_ROOT}/tf-psa-crypto/CMakeLists.txt)
    message(FATAL_ERROR "MBEDTLS_ROOT does not point at the pinned mbedTLS tree: ${MBEDTLS_ROOT}")
endif()

# AddressSanitizer and UndefinedBehaviorSanitizer are off by default: the llvm-mingw runtime used
# here ships no ASan runtime for the x86_64-w64-mingw32 target, so turning it on fails to link.
# Keep the switch so a toolchain that does support it can be used without editing this file.
option(KVM_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# --- pinned crypto as a subproject ---------------------------------------------------------------

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/host_psa_config.h"
    CACHE FILEPATH "" FORCE)

# host_shim supplies the two headers ESP-IDF normally provides from its port directory; see
# host_shim/mbedtls/bignum.h. It must come first so those two includes resolve.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/host_shim)

add_subdirectory(${MBEDTLS_ROOT}/tf-psa-crypto ${CMAKE_BINARY_DIR}/tfpsa EXCLUDE_FROM_ALL)

# --- the pairing core ----------------------------------------------------------------------------

set(PAIRING_SRC
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_pake.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_session.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_client.c
    ${REPO}/components/secure_remote/pairing/src/kvm_pairing_wire.c)

add_library(pairing_core STATIC ${PAIRING_SRC})
target_include_directories(pairing_core PUBLIC
    ${REPO}/components/secure_remote/pairing/include)
target_link_libraries(pairing_core PUBLIC tfpsacrypto)
target_compile_options(pairing_core PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)

add_library(pairing_fixture STATIC ${CMAKE_CURRENT_SOURCE_DIR}/det_rng.c)
target_include_directories(pairing_fixture PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(pairing_fixture PUBLIC pairing_core)
target_compile_options(pairing_fixture PRIVATE -Wall -Wextra -g -O1)

if(KVM_SANITIZE)
    foreach(t pairing_core pairing_fixture)
        target_compile_options(${t} PUBLIC
            -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
        target_link_options(${t} PUBLIC -fsanitize=address,undefined)
    endforeach()
endif()

# --- suites --------------------------------------------------------------------------------------

enable_testing()

foreach(suite test_pairing_wire test_pairing_pake test_pairing_session test_pairing_vectors)
    add_executable(${suite} ${CMAKE_CURRENT_SOURCE_DIR}/${suite}.c)
    target_link_libraries(${suite} PRIVATE pairing_fixture)
    target_include_directories(${suite} PRIVATE ${REPO}/test/host_unit)
    target_compile_options(${suite} PRIVATE -Wall -Wextra -g -O1)
    add_test(NAME ${suite} COMMAND ${suite})
endforeach()

# test_pairing_vectors compares the checked-in vectors against a fresh run, so it needs their path.
set_tests_properties(test_pairing_vectors PROPERTIES
    ENVIRONMENT "KVM_PAIRING_VECTORS=${CMAKE_CURRENT_SOURCE_DIR}/vectors")

# The generator is a tool, not a test: it rewrites the checked-in vectors on demand.
add_executable(gen_pairing_vectors ${CMAKE_CURRENT_SOURCE_DIR}/gen_pairing_vectors.c)
target_link_libraries(gen_pairing_vectors PRIVATE pairing_fixture)
target_compile_options(gen_pairing_vectors PRIVATE -Wall -Wextra -g -O1)
