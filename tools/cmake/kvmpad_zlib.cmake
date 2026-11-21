# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
# Adapt the pinned zlib's direct-output match loop in the build tree.
get_target_property(zlib_dir zlibstatic SOURCE_DIR)
get_target_property(zlib_sources zlibstatic SOURCES)
set(zlib_fast "${zlib_dir}/inffast.c")
if(NOT "inffast.c" IN_LIST zlib_sources OR NOT EXISTS "${zlib_fast}")
    return()
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${zlib_fast}")
file(READ "${zlib_fast}" zlib_code)
set(direct_match "                    from = out - dist;          /* copy direct from output */")
string(FIND "${zlib_code}" "${direct_match}" direct_position)
if(NOT direct_position EQUAL -1)
    set(match_copy [=[
                    /* KVMpad ESP32-S3 adaptation: long runs and disjoint matches. */
                    if (dist == 1 && len >= 16) {
                        memset(out, out[-1], len);
                        out += len;
                        continue;
                    }
                    if (len >= 16 && dist >= len) {
                        memcpy(out, out - dist, len);
                        out += len;
                        continue;
                    }
]=])
    string(REPLACE "#include \"zutil.h\"" "#include <string.h>\n#include \"zutil.h\"" zlib_code "${zlib_code}")
    string(REPLACE "${direct_match}" "${match_copy}${direct_match}" zlib_code "${zlib_code}")
    set(zlib_fast_output "${CMAKE_CURRENT_BINARY_DIR}/kvmpad-components/zlib/inffast.c")
    file(GENERATE OUTPUT "${zlib_fast_output}" CONTENT "${zlib_code}")
    list(TRANSFORM zlib_sources REPLACE "^inffast[.]c$" "${zlib_fast_output}")
    set_property(TARGET zlibstatic PROPERTY SOURCES "${zlib_sources}")
else()
    message(STATUS "Using upstream zlib match copy for this source revision")
endif()
