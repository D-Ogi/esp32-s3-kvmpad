# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
# FatFs structures change layout with exFAT. All consumers use the same configured
# component, including those that include ff.h outside the storage implementation.
set(fatfs_source "$ENV{IDF_PATH}/components/fatfs")
set(fatfs_build "${CMAKE_BINARY_DIR}/kvmpad-components/fatfs")
file(COPY "${fatfs_source}/" DESTINATION "${fatfs_build}"
     PATTERN "ffconf.h" EXCLUDE PATTERN "test_apps" EXCLUDE PATTERN "__pycache__" EXCLUDE)
file(READ "${fatfs_source}/src/ffconf.h" fatfs_config)
if(NOT fatfs_config MATCHES "#define FF_FS_EXFAT[ \t]+[01]")
    message(FATAL_ERROR "Unrecognized FatFs configuration for this SDK")
endif()
string(REGEX REPLACE "(#define FF_FS_EXFAT)[ \t]+[01]" "\\1 1" fatfs_config "${fatfs_config}")
# The SDK leaves FF_USE_LABEL as an undefined Kconfig symbol when labels are disabled.
# FatFs also references this value in a C expression on its exFAT path.
string(REPLACE "#define FF_USE_LABEL\tCONFIG_FATFS_USE_LABEL"
       "#ifdef CONFIG_FATFS_USE_LABEL\n#define FF_USE_LABEL 1\n#else\n#define FF_USE_LABEL 0\n#endif"
       fatfs_config "${fatfs_config}")
file(CONFIGURE OUTPUT "${fatfs_build}/src/ffconf.h" CONTENT "${fatfs_config}" @ONLY)
list(APPEND EXTRA_COMPONENT_DIRS "${fatfs_build}")
