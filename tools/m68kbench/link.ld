/* tools/m68kbench: QEMU mps2-an505.  Code and data in SSRAM1 (4 MB), the
 * Amiga's memory in the 16 MB PSRAM. */
MEMORY { RAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
 BIG (rw) : ORIGIN = 0x80000000, LENGTH = 16M }
ENTRY(_start)
SECTIONS {
  .vectors : { LONG(0x10400000); LONG(_start | 1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); LONG(fault_entry|1); . = ALIGN(4); } > RAM
  .text : { *(.text*) *(.time_critical*) *(m68k_ovl_000) *(m68k_ovl_020) *(.m68k_fast*) *(.rodata*) *(.glue_7*) KEEP(*(.init)) KEEP(*(.fini)) } > RAM
  .ARM.exidx : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } > RAM
  .preinit_array : { PROVIDE(__preinit_array_start = .); KEEP(*(.preinit_array)) PROVIDE(__preinit_array_end = .); } > RAM
  .init_array : { PROVIDE(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array)) PROVIDE(__init_array_end = .); } > RAM
  .fini_array : { PROVIDE(__fini_array_start = .); KEEP(*(.fini_array)) PROVIDE(__fini_array_end = .); } > RAM
  .data : { *(.data*) } > RAM
  .bss : { __bss_start__ = .; *(.bss*) *(COMMON) __bss_end__ = .; } > RAM
  end = .; _end = .; __end__ = .;
  .bigmem (NOLOAD) : { *(.bigmem) } > BIG
}
