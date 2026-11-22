# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# KVMpad firmware — ESP-IDF v6.1 project (TASK-000, spec 02 §2/§11).
cmake_minimum_required(VERSION 3.22)

option(KVM_MEDIA_SSH_PROFILE "Build the 7B media player, USB monitor and paired SSH with boot HID" OFF)
if(KVM_MEDIA_SSH_PROFILE)
    if(NOT KVM_MEDIA_FFMPEG)
        message(FATAL_ERROR "Set KVM_MEDIA_FFMPEG to the Xtensa decoder build for the media and SSH profile")
    endif()
    if(NOT SDKCONFIG)
        set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
    endif()
    if(CMAKE_HOST_WIN32 AND NOT DEFINED CMAKE_NINJA_FORCE_RESPONSE_FILE)
        set(CMAKE_NINJA_FORCE_RESPONSE_FILE ON CACHE BOOL "Use response files for Ninja commands")
    endif()
endif()

# Use the startup interfaces and local monitor entry on a fresh
# build. Explicit IDF profiles (including the three-interface base) take precedence.
if(NOT DEFINED SDKCONFIG_DEFAULTS AND "$ENV{SDKCONFIG_DEFAULTS}" STREQUAL "")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.boot-hid")
    if(KVM_MEDIA_SSH_PROFILE)
        list(APPEND SDKCONFIG_DEFAULTS sdkconfig.media sdkconfig.ssh-board)
    endif()
endif()

# Components that are still DESIGNED-only (README without sources) are excluded so the build
# never picks up an empty component by accident. Remove an entry when its task adds sources.
set(EXCLUDE_COMPONENTS
    diagnostics)

include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_fatfs.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(KVM_MEDIA_SSH_PROFILE)
    # The first component-manager pass runs before Kconfig enables SSH. Its
    # provisional solution can omit optional packages that the final pass needs.
    set(media_lock "${CMAKE_CURRENT_LIST_DIR}/dependencies.media-ssh.lock")
    set(resolved_lock "${CMAKE_BINARY_DIR}/dependencies.media-ssh.lock")
    configure_file("${media_lock}" "${resolved_lock}" COPYONLY)
    idf_build_set_property(DEPENDENCIES_LOCK "${resolved_lock}")
    idf_build_get_property(idf_python PYTHON)
    set(constraints_file "${CMAKE_BINARY_DIR}/media-ssh-constraints.txt")
    execute_process(COMMAND "${idf_python}" "${CMAKE_CURRENT_LIST_DIR}/tools/cmake/media_ssh_constraints.py"
                    "${media_lock}" "${constraints_file}" COMMAND_ERROR_IS_FATAL ANY)
    set(ENV{IDF_COMPONENT_CONSTRAINT_FILES} "$ENV{IDF_COMPONENT_CONSTRAINT_FILES};${constraints_file}")
endif()
project(kvmpad)

if(KVM_MEDIA_SSH_PROFILE)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files --ignore-eol "${media_lock}" "${resolved_lock}"
                    RESULT_VARIABLE lock_changed)
    if(NOT lock_changed EQUAL 0)
        message(FATAL_ERROR "Resolved managed components differ from dependencies.media-ssh.lock")
    endif()
endif()

if(KVM_MEDIA_SSH_PROFILE AND (NOT IDF_TARGET STREQUAL "esp32s3" OR
   NOT CONFIG_TINYUSB_HID_COUNT EQUAL 2 OR NOT CONFIG_KVM_USB_MONITOR_EVAL OR
   NOT CONFIG_KVM_SSH_BOARD OR NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES))
    message(FATAL_ERROR "The selected SDKCONFIG does not provide the signed 7B media and SSH profile. Use a fresh build directory and the profile defaults.")
endif()

if(CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_portrait.cmake")
endif()

if(IDF_TARGET STREQUAL "esp32s3" AND KVM_MEDIA_FFMPEG AND TARGET zlibstatic)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_zlib.cmake")
endif()
