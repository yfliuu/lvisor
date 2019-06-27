#include <asm/setup.h>
#include <io/sizes.h>

OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
        . = VMM_START;
        .text : {
                *(.head.text)
                *(.text .text.*)
        }
        .rodata : {
                *(.rodata .rodata.*)
        }
        .data : {
                *(.data .data.*)
                _edata = .;
        }
        . = ALIGN(SZ_4K);
        .percpu : {
                __per_cpu_start = .;
                *(.percpu .percpu.*)
                . = ALIGN(SZ_4K);
                __per_cpu_end = .;
        }
        . += (NR_CPUS - 1) * (__per_cpu_end - __per_cpu_start);
        .bss : {
                *(.bss .bss.*)
        }
        /* EPT uses 2M-pages */
        . = ALIGN(SZ_2M);
        _end = .;
}
