# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# KVMpad firmware — ESP-IDF v6.1 project (TASK-000, spec 02 §2/§11).
cmake_minimum_required(VERSION 3.22)

# All hosts use the same firmware. This path selects the decoder dependency.
set(decoder_default "$ENV{KVM_MEDIA_FFMPEG}")
if(NOT decoder_default)
    set(decoder_default "${CMAKE_CURRENT_SOURCE_DIR}/../ffmpeg-build/xtensa")
endif()
set(KVM_MEDIA_FFMPEG "${decoder_default}" CACHE PATH "Prepared Xtensa FFmpeg decoder")
if(NOT EXISTS "${KVM_MEDIA_FFMPEG}/libavcodec/libavcodec.a")
    message(FATAL_ERROR "Prepare the Xtensa decoder as described in docs/HANDBOOK.md. Set KVM_MEDIA_FFMPEG to its build directory.")
endif()
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_NINJA_FORCE_RESPONSE_FILE)
    set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files for Ninja commands")
endif()

# Components that are still DESIGNED-only (README without sources) are excluded so the build
# never picks up an empty component by accident. Remove an entry when its task adds sources.
set(EXCLUDE_COMPONENTS
    diagnostics)

include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_fatfs.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(firmware_lock "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.lock")
set(resolved_lock "${CMAKE_BINARY_DIR}/dependencies.lock")
configure_file("${firmware_lock}" "${resolved_lock}" COPYONLY)
idf_build_set_property(DEPENDENCIES_LOCK "${resolved_lock}")
idf_build_get_property(idf_python PYTHON)
set(constraints_file "${CMAKE_BINARY_DIR}/component-constraints.txt")
execute_process(COMMAND "${idf_python}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/locked_constraints.py"
                "${firmware_lock}" "${constraints_file}" COMMAND_ERROR_IS_FATAL ANY)
set(ENV{IDF_COMPONENT_CONSTRAINT_FILES} "$ENV{IDF_COMPONENT_CONSTRAINT_FILES};${constraints_file}")
project(kvmpad)
include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_ssh_buffers.cmake")

execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol "${firmware_lock}" "${resolved_lock}"
                RESULT_VARIABLE lock_changed)
if(lock_changed)
    message(FATAL_ERROR "Resolved dependencies differ from dependencies.lock. Review the resolved lock in the build directory.")
endif()

if(NOT IDF_TARGET STREQUAL "esp32s3" OR NOT CONFIG_TINYUSB_HID_COUNT EQUAL 2 OR
   NOT CONFIG_KVM_SSH_BOARD OR NOT CONFIG_KVM_USB_MONITOR_EVAL OR NOT CONFIG_KVM_USB_GUD OR
   NOT CONFIG_KVM_USB_SCREENSHOT OR NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES)
    message(FATAL_ERROR "The saved configuration differs from the 7B firmware defaults. Start with a fresh build directory.")
endif()

if(CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_portrait.cmake")
endif()

if(IDF_TARGET STREQUAL "esp32s3" AND KVM_MEDIA_FFMPEG AND TARGET zlibstatic)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_zlib.cmake")
endif()
