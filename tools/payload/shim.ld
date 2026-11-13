OUTPUT_ARCH(aarch64)
ENTRY(_start)
SECTIONS
{
    . = 0;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    shim_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(shim_end <= 0xC000, "shim too big: must end before the stack at 0xC000")
