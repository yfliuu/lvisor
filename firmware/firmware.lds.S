#include <asm/setup.h>

OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
        . = FIRMWARE_START;
        .text : {
                *(.head.text)
                *(.text .text.*)
        }
        .rodata : {
                *(.rodata .rodata.*)
        }
        .data : {
                *(.data .data.*)
        }
        .bss : {
                _bss = .;
                *(.bss .bss.*)
                _ebss = .;
        }
}
