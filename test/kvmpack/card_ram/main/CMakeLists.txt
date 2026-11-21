# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
set(runtime "${CMAKE_CURRENT_LIST_DIR}/../../../../components/pack_runtime")
# The SDMMC driver calls these helpers even for internal SRAM. IDF omits their
# translation unit from pure RAM apps on the S3. Use the pinned SDK implementation.
file(TO_CMAKE_PATH "$ENV{IDF_PATH}" sdk_root)
idf_component_register(SRCS "main.c" "${runtime}/pack_card_io.c" "${sdk_root}/components/esp_mm/esp_cache_msync.c"
                            "${sdk_root}/components/hal/cache_hal.c"
                       INCLUDE_DIRS "${runtime}/include" "${runtime}"
                       PRIV_REQUIRES board_hal esp_driver_sdmmc esp_driver_i2c sdmmc fatfs esp_mm)
target_link_options(${COMPONENT_LIB} INTERFACE "-u" "esp_cache_get_alignment")
if(KVM_CARD_WRITE_LABEL AND NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT KVM_CARD_WRITE_LABEL MATCHES "^[A-Za-z0-9_-]+$")
        message(FATAL_ERROR "Use an alphanumeric test volume label")
    endif()
    target_sources(${COMPONENT_LIB} PRIVATE "roundtrip.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_CARD_WRITE_LABEL="${KVM_CARD_WRITE_LABEL}")
endif()
