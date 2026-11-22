# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
idf_component_register(SRCS "src/ssh_control.c" INCLUDE_DIRS "include" REQUIRES hid_core PRIV_REQUIRES nvs_flash)
if(CONFIG_KVM_SSH_BOARD)
    target_sources(${COMPONENT_LIB} PRIVATE "port/ssh_control_session.c" "port/ssh_control_identity.c")
    target_include_directories(${COMPONENT_LIB} PUBLIC "port")
endif()
