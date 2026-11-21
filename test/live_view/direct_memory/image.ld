/*
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
 * Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
 */
ENTRY(_start)
PHDRS { kv PT_LOAD FLAGS(5); emu PT_LOAD FLAGS(5); ro PT_LOAD FLAGS(4); code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
 . = 0x3f000000;
 .rodata : { *(.rodata*) } :ro

 . = 0x40000300; .kvec : { KEEP(*(.kvector)) } :kv
 . = 0x40008000; .emu : { *(.exception) *(.literal.high_mul .text.high_mul) } :emu
 . = 0x40100000;
 .text : { *(.text.start) *(.literal .text .literal.* .text.*) } :code
 . = 0x40210000;
 .data : { *(.data*) } :data
 .bss : { *(.bss*) *(COMMON) } :data
}

__heap_start = 0x40430000; __heap_end = 0x40500000;
