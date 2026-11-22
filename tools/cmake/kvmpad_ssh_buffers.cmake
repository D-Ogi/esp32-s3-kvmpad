# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# Keep libssh's resizable packet storage out of the internal DMA heap used by
# hardware SHA during authentication. Secure buffers retain upstream allocation
# and erasure. The managed component and its dependency lock stay reproducible.
idf_component_get_property(ssh_target david-cermak__libssh COMPONENT_LIB)
idf_component_get_property(ssh_dir david-cermak__libssh COMPONENT_DIR)
get_target_property(ssh_sources ${ssh_target} SOURCES)
set(ssh_buffer_source "libssh-mirror/src/buffer.c")
set(ssh_buffer_path "${ssh_dir}/${ssh_buffer_source}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ssh_buffer_path}")
file(READ "${ssh_buffer_path}" ssh_buffer_code)
set(upstream_alloc "new = realloc(buffer->data, needed);")
string(FIND "${ssh_buffer_code}" "${upstream_alloc}" allocation_offset)
set(buffer_sources "${ssh_sources}")
list(FILTER buffer_sources INCLUDE REGEX "(^|/)libssh-mirror/src/buffer[.]c$")
list(LENGTH buffer_sources buffer_source_count)
if(NOT buffer_source_count EQUAL 1 OR allocation_offset EQUAL -1)
    message(FATAL_ERROR "Review packet buffer allocation for the pinned libssh component.")
endif()
string(REPLACE "${upstream_alloc}"
    "new = heap_caps_realloc(buffer->data, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);"
    ssh_buffer_code "${ssh_buffer_code}")
string(PREPEND ssh_buffer_code "#include \"esp_heap_caps.h\"\n")
set(ssh_buffer_output "${CMAKE_BINARY_DIR}/kvmpad-components/ssh/buffer.c")
file(CONFIGURE OUTPUT "${ssh_buffer_output}" CONTENT "${ssh_buffer_code}" @ONLY)
list(REMOVE_ITEM ssh_sources "${buffer_sources}")
list(APPEND ssh_sources "${ssh_buffer_output}")
set_property(TARGET ${ssh_target} PROPERTY SOURCES "${ssh_sources}")
