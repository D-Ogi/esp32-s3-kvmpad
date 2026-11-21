# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# KVMpad firmware — ESP-IDF v6.1 project (TASK-000, spec 02 §2/§11).
cmake_minimum_required(VERSION 3.22)

# Components that are still DESIGNED-only (README without sources) are excluded so the build
# never picks up an empty component by accident. Remove an entry when its task adds sources.
set(EXCLUDE_COMPONENTS
    diagnostics)

include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_fatfs.cmake")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kvmpad)

if(CONFIG_KVM_UI_ADAPTER_TEAR_DOUBLE_PARTIAL)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_portrait.cmake")
endif()

if(IDF_TARGET STREQUAL "esp32s3" AND KVM_MEDIA_FFMPEG AND TARGET zlibstatic)
    include("${CMAKE_CURRENT_LIST_DIR}/tools/cmake/kvmpad_zlib.cmake")
endif()
