# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ADR-025 FR-751: turn THIRD_PARTY_NOTICES.md into the C strings `Settings -> About` renders. Shared by the
# three builds that compile ui_views - the ESP-IDF component build, the host suite in test/ui_views and the
# desktop simulator - so all three show exactly what the repository file says.
#
# Generation happens at configure time, not as a build rule: the generated .c is a source file of the target,
# and a configure-time file exists before the generator expression that would need it. A change to the
# Markdown file triggers a reconfigure through CMAKE_CONFIGURE_DEPENDS, so the string cannot go stale.
#
#   include(.../components/ui_views/notices.cmake)
#   kvm_notices_generate(${CMAKE_CURRENT_BINARY_DIR}/generated NOTICES_C NOTICES_DIR)
#   target_sources(<tgt> PRIVATE ${NOTICES_C})
#   target_include_directories(<tgt> PUBLIC ${NOTICES_DIR})

function(kvm_notices_generate out_dir c_var dir_var)
    get_filename_component(_repo "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(_script "${_repo}/tools/gen_third_party_notices.py")
    set(_input "${_repo}/THIRD_PARTY_NOTICES.md")

    # ESP-IDF exports PYTHON; a standalone host build looks the interpreter up itself.
    if(DEFINED PYTHON)
        set(_python "${PYTHON}")
    else()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(_python "${Python3_EXECUTABLE}")
    endif()

    # -I: no user site packages and no implicit sys.path entry for the script's directory.
    execute_process(
        COMMAND "${_python}" -I "${_script}" --input "${_input}" --out-dir "${out_dir}"
        RESULT_VARIABLE _rc
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "gen_third_party_notices.py failed (${_rc}):\n${_out}${_err}")
    endif()
    message(STATUS "ui_views: ${_out}")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}" "${_script}")
    set(${c_var} "${out_dir}/ui_notices_generated.c" PARENT_SCOPE)
    set(${dir_var} "${out_dir}" PARENT_SCOPE)
endfunction()
