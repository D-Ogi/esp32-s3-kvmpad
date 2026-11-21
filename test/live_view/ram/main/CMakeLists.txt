# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
idf_component_register(SRCS "main.c" PRIV_REQUIRES live_view esp_timer esp_rom)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT EXISTS "${KVM_CVID_FIXTURE}")
        message(FATAL_ERROR "Set KVM_CVID_FIXTURE to the synthetic packet from check_codecs.py")
    endif()
    target_add_binary_data(${COMPONENT_LIB} "${KVM_CVID_FIXTURE}" BINARY RENAME_TO cinepak_frame)
endif()
