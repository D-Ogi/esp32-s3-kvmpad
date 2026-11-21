# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
idf_component_register(SRCS "pack_archive.c" "pack_scene.c" "pack_load.c" "pack_png.c" "pack_store.c" "pack_port.c" "pack_card_io.c" "pack_video.c" "pack_scratch.c"
    INCLUDE_DIRS "include"
    REQUIRES remote_protocol
    PRIV_REQUIRES board_hal fatfs sdmmc esp_driver_sdmmc esp_rom esp_timer heap mbedtls espressif__libpng espressif__zlib)

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND KVM_MEDIA_SYNTHETIC_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_MEDIA_SYNTHETIC_DIAG=1)
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND KVM_MEDIA_PAGED_GUEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_MEDIA_PAGED_GUEST=1)
endif()

if(NOT CMAKE_BUILD_EARLY_EXPANSION AND KVM_PACK_PUBLISHERS)
    if(NOT CONFIG_MBEDTLS_SHA512_C)
        message(FATAL_ERROR "Publisher verification requires CONFIG_MBEDTLS_SHA512_C")
    endif()
    get_filename_component(bundle "${KVM_PACK_PUBLISHERS}" ABSOLUTE BASE_DIR "${PROJECT_DIR}")
    idf_build_get_property(idf_python PYTHON)
    set(generator "${CMAKE_CURRENT_LIST_DIR}/publisher_keys.py")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bundle}" "${generator}")
    execute_process(COMMAND "${idf_python}" "${generator}" "${bundle}" "${CMAKE_CURRENT_BINARY_DIR}/pack_publisher_keys.h"
                    COMMAND_ERROR_IS_FATAL ANY)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_PACK_HAS_PUBLISHERS=1)
    target_sources(${COMPONENT_LIB} PRIVATE "pack_publisher.c")
endif()
