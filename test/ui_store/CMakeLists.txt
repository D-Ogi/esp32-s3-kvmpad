# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Host tests of ui_core (TASK-060B) against the real LVGL 9.6 sources pinned by dependencies.lock
# (managed_components/lvgl__lvgl, fetched by `idf.py reconfigure`). LVGL is compiled with the address
# sanitizer (use-after-free of observers is detected inside LVGL too) but without warnings-as-errors;
# the project sources keep the strict flags of test/host_unit.
#
#   cmake -S test/ui_store -B ../build-host-ui -G Ninja -DCMAKE_C_COMPILER=clang
#   cmake --build ../build-host-ui && ctest --test-dir ../build-host-ui --output-on-failure
cmake_minimum_required(VERSION 3.25)
project(kvmpad_ui_store C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl CACHE PATH "LVGL 9.6 source tree (pinned by dependencies.lock)")

option(KVM_SANITIZE "Build with AddressSanitizer (+ UBSan for the project sources)" ON)

if(NOT EXISTS ${LVGL_DIR}/include/lvgl/lvgl.h)
    message(FATAL_ERROR "LVGL not found in ${LVGL_DIR}: run `idf.py reconfigure` once to fetch the managed components")
endif()

file(STRINGS ${LVGL_DIR}/lv_version.h _ver REGEX "#define LVGL_VERSION_(MAJOR|MINOR|PATCH) ")
message(STATUS "LVGL source: ${LVGL_DIR} ${_ver}")

# ---- LVGL as a static library ---------------------------------------------------------------
file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/host_lv_mem.c)
# SYSTEM: the LVGL headers are not held to the project's -Wconversion -Werror
target_include_directories(lvgl_host SYSTEM PUBLIC ${LVGL_DIR})
target_include_directories(lvgl_host PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(lvgl_host PUBLIC LV_CONF_INCLUDE_SIMPLE LV_KCONFIG_IGNORE)
target_compile_options(lvgl_host PRIVATE -w -g -O1)
if(KVM_SANITIZE)
    target_compile_options(lvgl_host PRIVATE -fsanitize=address -fno-omit-frame-pointer)
endif()

# ---- project sources: strict flags ----------------------------------------------------------
set(STRICT -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion -g -O1)
if(KVM_SANITIZE)
    list(APPEND STRICT -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

set(UI_CORE ${REPO}/components/ui_core)
set(UI_CORE_INCLUDES ${UI_CORE}/include ${REPO}/components/hid_core/include
    ${REPO}/components/usb_hid_device/include ${REPO}/test/host_unit)
set(UI_CORE_PURE ${UI_CORE}/src/ui_mailbox.c ${UI_CORE}/src/ui_actions.c ${UI_CORE}/src/ui_present.c)

enable_testing()

function(ui_suite name)
    cmake_parse_arguments(S "LVGL" "" "SOURCES" ${ARGN})
    add_executable(${name} ${name}.c ${S_SOURCES})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${UI_CORE_INCLUDES})
    target_compile_options(${name} PRIVATE ${STRICT})
    if(S_LVGL)
        target_link_libraries(${name} PRIVATE lvgl_host)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ui_suite(test_ui_mailbox SOURCES ${UI_CORE_PURE})
ui_suite(test_ui_actions SOURCES ${UI_CORE_PURE})
ui_suite(test_ui_store LVGL SOURCES ${UI_CORE_PURE} ${UI_CORE}/src/ui_store.c)
