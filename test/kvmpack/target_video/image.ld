/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 */
ENTRY(_start)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); ro PT_LOAD FLAGS(4); }
SECTIONS {
    . = 0x3e000000;
    .rodata : { *(.rodata*) } :ro
    . = 0x40100000;
    .text : { *(.text.start) *(.literal .text .literal.* .text.*) } :code
    . = 0x30000000;
    .data : { *(.data*) } :data
    .bss : { *(.bss*) *(COMMON) } :data
}
tinfl_decompress = 0x40000828;
esp_rom_crc32_le = 0x40001c98;
