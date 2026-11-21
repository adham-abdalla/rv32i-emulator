/* Bare-metal layout for rv32emu: 1 MiB RAM at 0, code at 0x1000, stack set by crt0. */
ENTRY(_start)
SECTIONS
{
  . = 0x1000;
  .text   : { *(.text.start) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
  .data   : { *(.data .data.* .sdata .sdata.*) }
  .bss    : ALIGN(4) { __bss_start = .; *(.bss .bss.* .sbss .sbss.* COMMON); . = ALIGN(4); __bss_end = .; }
  /DISCARD/ : { *(.comment) *(.riscv.attributes) }
}
