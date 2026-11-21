# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# The adapter already rotates partial tiles and tracks physical damage on both
# RGB framebuffers. Its registration-time orientation is otherwise immutable.
idf_component_get_property(adapter espressif__esp_lvgl_adapter COMPONENT_LIB)
get_target_property(adapter_sources ${adapter} SOURCES)
foreach(source IN LISTS adapter_sources)
    if(source MATCHES "src/display/bridge/v9/lvgl_bridge_v9[.]c$")
        set(bridge_source "${source}")
        break()
    endif()
endforeach()
if(NOT bridge_source)
    message(FATAL_ERROR "The configured adapter has no LVGL 9 RGB bridge")
endif()
idf_component_get_property(adapter_dir espressif__esp_lvgl_adapter COMPONENT_DIR)
get_filename_component(bridge_path "${bridge_source}" ABSOLUTE BASE_DIR "${adapter_dir}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bridge_path}")
file(READ "${bridge_path}" bridge_code)
set(anchor "    if (!display_bridge_v9_prepare_mono(impl, disp, area, &color_map)) {")
string(FIND "${bridge_code}" "${anchor}" anchor_offset)
if(anchor_offset EQUAL -1)
    message(FATAL_ERROR "Review partial rotation against this adapter revision")
endif()
set(select_rotation [=[
    /* The 7B mounts clockwise. LVGL's 90-degree logical orientation maps to
     * the adapter's 270-degree pixel copy. Both use the original physical fbs. */
    if (impl->cfg.base.profile.interface == ESP_LV_ADAPTER_PANEL_IF_RGB &&
        impl->cfg.base.tear_avoid_mode == ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_PARTIAL &&
        impl->cfg.base.profile.rotation == ESP_LV_ADAPTER_ROTATE_0) {
        esp_lv_adapter_rotation_t rotation = lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_90
            ? ESP_LV_ADAPTER_ROTATE_270 : ESP_LV_ADAPTER_ROTATE_0;
        if (impl->runtime.rotation != rotation) {
            impl->runtime.rotation = rotation;
            display_bridge_v9_diff_reset(impl);
        }
    }
]=])
string(REPLACE "${anchor}" "${select_rotation}\n${anchor}" bridge_code "${bridge_code}")
# Keep the vendor's cache maintenance, physical damage tracker and VSYNC swap.
# Only the CPU tile copy in the S3's rotated partial RGB flush changes.
if(IDF_TARGET STREQUAL "esp32s3")
    set(copy_anchor [=[    rotate_copy_strided_region(color_map,
                               impl->draw_fb,
                               area->x1, area->y1,
                               area->x2, area->y2,
                               src_stride_px,
                               impl);]=])
    string(FIND "${bridge_code}" "${copy_anchor}" copy_offset)
    if(copy_offset EQUAL -1)
        message(FATAL_ERROR "Review the partial RGB565 tile copy against this adapter revision")
    endif()
    set(copy_tile [=[    if (color_bytes == 2 && bridge_rotation(impl) == ESP_LV_ADAPTER_ROTATE_270 &&
        impl->cfg.base.profile.interface == ESP_LV_ADAPTER_PANEL_IF_RGB) {
        kvmpad_rotate_rgb565_270(color_map, impl->draw_fb, area->x1, area->y1,
                                 area->x2 - area->x1 + 1, rect_h, src_stride_px,
                                 bridge_h_res(impl), bridge_v_res(impl));
    } else {
]=])
    string(REPLACE "${copy_anchor}" "${copy_tile}${copy_anchor}\n    }" bridge_code "${bridge_code}")
    string(PREPEND bridge_code "#include \"kvmpad_rotate_rgb565.h\"\n")
    target_include_directories(${adapter} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
set(bridge_output "${CMAKE_BINARY_DIR}/kvmpad-components/portrait/lvgl_bridge_v9.c")
file(CONFIGURE OUTPUT "${bridge_output}" CONTENT "${bridge_code}" @ONLY)
list(REMOVE_ITEM adapter_sources "${bridge_source}")
list(APPEND adapter_sources "${bridge_output}")
set_property(TARGET ${adapter} PROPERTY SOURCES "${adapter_sources}")
