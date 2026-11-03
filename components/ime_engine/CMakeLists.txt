# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
#
# ime_engine (TASK-072):
#   - ime_hangul.c      the Korean 2-set composer (pure C, also compiled by test/ime);
#   - ime_data.c        the ime_data image directory parser (pure C, also compiled by test/ime);
#   - ime_data_vfs.c    firmware only: read-only, SHA-256-checked mount of the ime_data partition at /ime;
#   - ime_alloc.cpp     keeps the engines' heap in PSRAM (port/ime_alloc.h);
#   - ime_pinyin.cpp    a C face for the vendored AOSP PinyinIME decoder (pinyinime/, Apache-2.0, listed in
#                       THIRD_PARTY_NOTICES.md; compiled unchanged, so its warnings are silenced, not fixed);
#   - ime_wnn.c         a C face for the vendored AOSP OpenWnn engine (openwnn/, Apache-2.0, listed in
#                       THIRD_PARTY_NOTICES.md): openwnn/engine is compiled unchanged, openwnn/kvm holds the
#                       C translations of the Java conversion logic and is held to the project warning level.
# linker.lf moves the engines' static buffers to PSRAM.
set(PINYIN pinyinime/share)
set(PINYIN_SRCS
    ${PINYIN}/dictlist.cpp ${PINYIN}/dicttrie.cpp ${PINYIN}/lpicache.cpp ${PINYIN}/matrixsearch.cpp
    ${PINYIN}/mystdlib.cpp ${PINYIN}/ngram.cpp ${PINYIN}/pinyinime.cpp ${PINYIN}/searchutility.cpp
    ${PINYIN}/spellingtable.cpp ${PINYIN}/spellingtrie.cpp ${PINYIN}/splparser.cpp ${PINYIN}/sync.cpp
    ${PINYIN}/userdict.cpp ${PINYIN}/utf16char.cpp ${PINYIN}/utf16reader.cpp)

# The OpenWnn dictionary engine: freestanding C, no libc and no heap of its own (it works inside the
# NJ_CLASS / NJ_DIC_SET the caller owns), so there is nothing to rename here.
set(WNN_SRCS
    openwnn/engine/ndapi.c openwnn/engine/ndbdic.c openwnn/engine/ndcommon.c openwnn/engine/ndfdic.c
    openwnn/engine/ndldic.c openwnn/engine/ndrdic.c openwnn/engine/neapi.c openwnn/engine/necode.c
    openwnn/engine/nj_str.c)
set(WNN_KVM_SRCS openwnn/kvm/wnn_convert.c openwnn/kvm/wnn_romkan.c)

idf_component_register(
    SRCS "src/ime_hangul.c" "src/ime_data.c" "src/ime_data_vfs.c" "src/ime_pinyin.cpp" "src/ime_wnn.c"
         "src/ime_alloc.cpp" ${PINYIN_SRCS} ${WNN_SRCS} ${WNN_KVM_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "port" "openwnn/include" "openwnn/kvm"
    PRIV_REQUIRES esp_partition vfs mbedtls esp_timer heap
    LDFRAGMENTS "linker.lf")

# Compiled unchanged: warnings silenced, and the C heap calls renamed so they prefer PSRAM (port/ime_alloc.h).
set_source_files_properties(${PINYIN_SRCS} PROPERTIES COMPILE_OPTIONS
    "-w;-Dmalloc=ime_engine_malloc;-Dcalloc=ime_engine_calloc;-Drealloc=ime_engine_realloc;-Dfree=ime_engine_free")
# Compiled unchanged as well; its warnings are silenced, not fixed.
set_source_files_properties(${WNN_SRCS} PROPERTIES COMPILE_OPTIONS "-w")
