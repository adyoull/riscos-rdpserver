/* RDPServer module: everything in one section at 0, the header first
   (tools/modlink.py makes the module file from it). */
ENTRY(module_base)
SECTIONS
{
  . = 0;
  .text :
  {
    KEEP(*(.text.header))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
    __data_start = .;
    *(.data .data.*)
    . = ALIGN(4);
    __data_end = .;
    __bss_start = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(4);
    __bss_end = .;
  }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) *(.ARM.exidx*) *(.ARM.extab*) *(.init_array*) *(.fini_array*) }
}
