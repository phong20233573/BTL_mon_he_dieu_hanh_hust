OUTPUT_ARCH("riscv")
ENTRY(_entry)

/* Separate LOAD segments to avoid RWX permissions. */
PHDRS
{
  text   PT_LOAD FLAGS(5);  /* Read + Execute */
  rodata PT_LOAD FLAGS(4);  /* Read only */
  data   PT_LOAD FLAGS(6);  /* Read + Write */
}

SECTIONS
{
  /*
   * Ensure that entry.S / _entry is at 0x80000000,
   * where QEMU's -kernel jumps.
   */
  . = 0x80000000;

  .text : {
    kernel/entry.o(_entry)
    *(.text .text.*)
    . = ALIGN(0x1000);
    _trampoline = .;
    *(trampsec)
    . = ALIGN(0x1000);
    ASSERT(. - _trampoline == 0x1000,
           "error: trampoline larger than one page");
    PROVIDE(etext = .);
  } :text

  .rodata : {
    . = ALIGN(0x1000);
    *(.srodata .srodata.*)
    *(.rodata .rodata.*)
    *(.eh_frame .eh_frame.*)
    . = ALIGN(0x1000);
  } :rodata

  .data : {
    . = ALIGN(0x1000);
    *(.sdata .sdata.*)
    *(.data .data.*)
    *(.got .got.*)
    *(.got.plt)
  } :data

  .bss : {
    . = ALIGN(0x1000);
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(0x1000);
  } :data

  PROVIDE(end = .);
}
