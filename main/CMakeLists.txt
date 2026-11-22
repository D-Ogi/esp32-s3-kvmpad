# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# main: bring-up integration (TASK-000/010/020/030/050 USB part, TASK-060A/B UI wiring). lcd_pattern.c is pure C.
idf_component_register(
    SRCS "app_main.c" "kvm_notice.c" "kvm_diag.c" "kvm_console.c" "lcd_pattern.c" "ui_glue.c" "ui_shell.c"
         "pack_panel.c" "pack_receive.c" "pack_view.c" "pack_chart_view.c" "pack_autopilot_view.c" "pack_radio_view.c" "pack_radio_font.c" "media_panel.c" "hpe_panel.c" "host_panel.c" "app_config.c" "remote_glue.c" "text_glue.c" "ime_bench.c" "ime_glue.c"
         "pack_keyboard_surface.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES kbd_layouts ime_engine board_hal touch_input usb_hid_device hid_core gesture_engine ui_platform ui_core ui_touch_bridge ui_theme ui_components ui_views net_status config_store update_manager secure_remote remote_protocol console nvs_flash esp_wifi esp_netif esp_event
                  esp_partition esp_psram esp_timer spi_flash esp_app_format log pack_runtime live_view kvmbridge usb_monitor)

# The chart benchmark shares the same UART diagnostics switch as the built-in LVGL benchmark scenes.
if(CONFIG_KVM_USB_SCREENSHOT)
    idf_component_optional_requires(PRIVATE screenshot_volume fatfs)
    target_sources(${COMPONENT_LIB} PRIVATE "preboot_capture.c")
    if(KVM_PREBOOT_CAPTURE_CHECK)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_PREBOOT_CAPTURE_CHECK=1)
    endif()
endif()
if(CONFIG_KVM_USB_MONITOR_EVAL)
    target_sources(${COMPONENT_LIB} PRIVATE "pack_desktop_view.c" "pack_touchpad_drawer.c")
    if(KVM_USB_MOTION_CHECK)
        set(motion_dir "${CMAKE_CURRENT_LIST_DIR}/../test/hil/usb_motion")
        target_sources(${COMPONENT_LIB} PRIVATE "${motion_dir}/fixtures.c" "${motion_dir}/trial.c")
        target_include_directories(${COMPONENT_LIB} PRIVATE "${motion_dir}")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_USB_MOTION_CHECK=1)
        target_add_binary_data(${COMPONENT_LIB} "${motion_dir}/desktop.bin" BINARY RENAME_TO motion_desktop)
    endif()
endif()

if(CONFIG_KVM_UI_BENCH)
    target_sources(${COMPONENT_LIB} PRIVATE "chart_bench.c")
endif()

# Development-only hooks for the ADR-031 board plan (docs/OTA.md). Never set for a release build; both are reported at
# every boot. KVM_OTA_TEST_CERT: a PEM certificate (outside the repository) pinned for `ota url`, so a LAN test server
# with a self-signed certificate can serve images. KVM_OTA_TEST_NO_TOUCH: the touch reader is not started, so the health
# check of this image must fail and roll back (TECH-099).
if(KVM_OTA_TEST_CERT)
    target_add_binary_data(${COMPONENT_LIB} "${KVM_OTA_TEST_CERT}" TEXT RENAME_TO ota_test_cert_pem)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_OTA_TEST_CERT=1)
endif()
if(KVM_OTA_TEST_NO_TOUCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_OTA_TEST_NO_TOUCH=1)
endif()
if(KVM_MEDIA_SYNTHETIC_DIAG)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_MEDIA_SYNTHETIC_DIAG=1)
endif()
if(KVM_USB_PICTURE_CHECK)
    if(NOT KVM_MEDIA_SYNTHETIC_DIAG OR NOT CONFIG_KVM_USB_MONITOR_EVAL)
        message(FATAL_ERROR "KVM_USB_PICTURE_CHECK requires the engineering USB monitor build")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KVM_USB_PICTURE_CHECK=1)
endif()
