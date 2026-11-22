# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# usb_hid_device: the only component that includes tusb.h / tinyusb.h (TASK-050 USB part).
# usb_hid_reports.c is pure C (host unit tests).
idf_component_register(
    SRCS "src/usb_hid_reports.c" "src/hpe_reports.c" "src/hpe_studio.c" "src/usb_hid_device.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_app_format esp_hw_support esp_timer log)

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=hidd_reset" "-Wl,--wrap=hidd_control_xfer_cb")
if(KVM_MEDIA_SYNTHETIC_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_MEDIA_SYNTHETIC_DIAG=1)
endif()
if(CONFIG_KVM_USB_MONITOR_EVAL)
    idf_component_optional_requires(PRIVATE usb_monitor)
    target_sources(${COMPONENT_LIB} PRIVATE src/usb_monitor_port.c)
    if(CONFIG_KVM_USB_SCREENSHOT)
        idf_component_optional_requires(PRIVATE esp_blockdev screenshot_volume)
        target_sources(${COMPONENT_LIB} PRIVATE src/usb_screenshot.c)
        target_link_options(${COMPONENT_LIB} INTERFACE
            "-Wl,--wrap=mscd_xfer_cb" "-Wl,--wrap=mscd_control_xfer_cb"
            "-Wl,--wrap=msc_storage_mount_to_app" "-Wl,--wrap=msc_storage_mount_to_usb")
    endif()
    if(CONFIG_KVM_USB_GUD)
        target_sources(${COMPONENT_LIB} PRIVATE src/usb_gud_port.c)
        if(KVM_GUD_GUEST_CHECK)
            target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../test/usb_monitor/gud_guest/winusb_bridge.c")
            target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_GUD_GUEST_CHECK=1)
        endif()
    endif()
    target_link_options(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=tud_descriptor_device_cb"
        "-Wl,--wrap=tud_descriptor_configuration_cb"
        "-Wl,--wrap=tud_descriptor_string_cb")
endif()
