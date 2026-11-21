ENTRY(guest_entry)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x10000;
  .text : { *(.text .text.*) } :text
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) } :text
  . = 0x02000000;
  .data : { *(.data .data.* .sdata .sdata.*) } :data
  .bss : { *(.bss .bss.* .sbss .sbss.* COMMON) } :data
  . = ALIGN(16);
  guest_heap = .;
  _end = .;
  /DISCARD/ : { *(.eh_frame* .comment .note.*) }
}
