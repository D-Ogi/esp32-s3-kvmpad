# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Native pairing library for the Windows CLIENT (TASK-105, ADR-028, ADR-029).
#
# ADR-029 settled on one implementation of the pairing protocol compiled twice, so this builds the
# very sources the firmware builds -- components/secure_remote/pairing -- against the very crypto the
# firmware links, the TF-PSA-Crypto of the pinned ESP-IDF v6.1 tree. The flags, the host_shim include
# and the external-RNG arrangement are copied from test/pairing/CMakeLists.txt on purpose: three
# builds of one body of code, differing only in where entropy comes from.
#
# Two libraries come out of it:
#
#   kvmpad_pairing.dll       the one the application loads. CLIENT half only, BCryptGenRandom.
#   kvmpad_pairing_test.dll  a test fixture, NEVER SHIPPED. Adds the DEVICE state machine and the
#                            seeded generator of test/pairing/det_rng.c, which is what makes the
#                            ADR-029 interop vectors reproducible.
#
# Build (nothing is installed on drive C:, and the build directory stays out of the repository):
#
#   export PATH=/o/Praca/KVMpad/.tools/llvm-mingw-20261020-ucrt-x86_64/bin:$PATH
#   cmake -S clients/windows/native -B ../build-win-pairing -G Ninja \
#         -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
#         -DMBEDTLS_ROOT=O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls
#   cmake --build ../build-win-pairing
#
# The two DLLs land in clients/windows/native/bin/, which .gitignore already covers, because the
# managed test project copies them from there into its own output.
cmake_minimum_required(VERSION 3.25)
project(kvmpad_pairing_win C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(PAIRING ${REPO}/components/secure_remote/pairing)
set(HOSTTEST ${REPO}/test/pairing)

if(NOT WIN32)
    message(FATAL_ERROR "This library is the Windows CLIENT's pairing core; build it for Windows.")
endif()

set(MBEDTLS_ROOT "O:/Praca/KVMpad/.tools/esp-idf-v6.1/components/mbedtls/mbedtls"
    CACHE PATH "Root of the mbedTLS tree pinned by ESP-IDF v6.1")
if(NOT EXISTS ${MBEDTLS_ROOT}/tf-psa-crypto/CMakeLists.txt)
    message(FATAL_ERROR "MBEDTLS_ROOT does not point at the pinned mbedTLS tree: ${MBEDTLS_ROOT}")
endif()

# The managed test project expects both DLLs here; see the note at the top of this file.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/bin)

# --- pinned crypto as a subproject ---------------------------------------------------------------

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/win_psa_config.h"
    CACHE FILEPATH "" FORCE)

# host_shim supplies the two headers ESP-IDF normally provides from its port directory. It must come
# first so those includes resolve, exactly as in test/pairing/CMakeLists.txt.
include_directories(BEFORE ${HOSTTEST}/host_shim)

add_subdirectory(${MBEDTLS_ROOT}/tf-psa-crypto ${CMAKE_BINARY_DIR}/tfpsa EXCLUDE_FROM_ALL)

# --- the pairing core ----------------------------------------------------------------------------

set(PAIRING_SRC
    ${PAIRING}/src/kvm_pairing_pake.c
    ${PAIRING}/src/kvm_pairing_session.c
    ${PAIRING}/src/kvm_pairing_client.c
    ${PAIRING}/src/kvm_pairing_wire.c)

add_library(pairing_core STATIC ${PAIRING_SRC})
target_include_directories(pairing_core PUBLIC ${PAIRING}/include)
target_link_libraries(pairing_core PUBLIC tfpsacrypto)
# -ffunction-sections and -fdata-sections put every function in its own section so that the linker
# can drop the ones nothing reaches; see the --gc-sections note on the shipping DLL below.
target_compile_options(pairing_core PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O2
    -ffunction-sections -fdata-sections)
# The core is linked into shared libraries, so it has to be position independent.
set_target_properties(pairing_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- the shipping DLL ----------------------------------------------------------------------------

add_library(kvmpad_pairing SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kvmpad_pairing_abi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/win_rng.c)
target_include_directories(kvmpad_pairing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kvmpad_pairing PRIVATE pairing_core bcrypt)
target_compile_options(kvmpad_pairing PRIVATE
    -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O2
    -ffunction-sections -fdata-sections)
# --gc-sections drops sections nothing reaches from an export, which keeps most of the DEVICE state
# machine out of the shipping library. It does not remove all of it: the FSM shares a translation
# unit with kvm_pair_err_name(), which this DLL does call. So the guarantee here is the export table,
# not the absence of every byte -- nothing outside can call the DEVICE half, and a test checks that.
# What IS wholly absent is the test fixture: det_rng.c is not in this target's sources at all, so the
# seeded generator det_rng.h warns about cannot exist in the shipping DLL.
target_link_options(kvmpad_pairing PRIVATE "LINKER:--gc-sections")
# No import library and no "lib" prefix: the DLL is loaded by name through P/Invoke, never linked.
set_target_properties(kvmpad_pairing PROPERTIES PREFIX "")

# --- the test-only DLL ---------------------------------------------------------------------------
#
# This target carries the DEVICE state machine and the seeded generator. Keeping it a separate
# artifact is the whole point: neither can reach the shipping DLL by accident.

add_library(kvmpad_pairing_test SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/src/kvmpad_pairing_abi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testonly_abi.c
    ${HOSTTEST}/det_rng.c)
target_include_directories(kvmpad_pairing_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${HOSTTEST})
target_link_libraries(kvmpad_pairing_test PRIVATE pairing_core)
# The fixture sources are not held to -Werror: they are not shipped, and det_rng.c is maintained by
# TASK-100 under the flags of test/pairing/CMakeLists.txt.
target_compile_options(kvmpad_pairing_test PRIVATE -Wall -Wextra -g -O2)
set_target_properties(kvmpad_pairing_test PROPERTIES PREFIX "")
