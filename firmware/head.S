#include <asm/mmu.h>
#include <asm/msr-index.h>
#include <asm/processor-flags.h>
#include <asm/segment.h>
#include <asm/setup.h>
#include <io/linkage.h>
#include <io/sizes.h>
#include <sys/multiboot.h>

/* match QEMU's behavior - see comments below */
#define BOOT32_CS       (1 << 3)
#define BOOT32_DS       (2 << 3)

        .section .head.text
        .code16
ENTRY(_start)
        /* code */
        .byte   0xe9
        .word   start16 - 1f
1:
        .byte   0, 0, 0, 0, 0
        /* kernel_start, kernel_end */
        .quad   0, 0
        /* initrd_start, initrd_end */
        .quad   0, 0
        /* cmdline */
        .space  CMDLINE_SIZE
        /* e820_entries */
        .long   0
        /* e820_table */
        .space  20 * E820_MAX_ENTRIES_GUEST

start16:
        cli

        /* APs wait for SIPI */
        mov     $MSR_IA32_APICBASE, %ecx
        rdmsr
        and     $MSR_IA32_APICBASE_BSP, %eax
        cmp     $0, %eax
        jne     bsp
        1:
        pause
        jmp     1b

bsp:
        lgdt    gdt

        movl    %cr0, %eax
        orl     $X86_CR0_PE, %eax
        movl    %eax, %cr0

        mov     $BOOT_DS, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss

        ljmp    $BOOT32_CS, $start32

        .code32
start32:
        /* CR4: enable PAE, PSE */
        movl    %cr4, %eax
        orl     $(X86_CR4_PAE|X86_CR4_PSE), %eax
        movl    %eax, %cr4

        /* CR3: load boot page table */
        movl    $pml4, %eax
        movl    %eax, %cr3

        /* MSR EFER: enable LME */
        movl    $MSR_EFER, %ecx
        rdmsr
        orl     $EFER_LME, %eax
        wrmsr

        /* load 32-bit tss before enabling protected mode */
        xorl    %eax, %eax
        lldt    %ax
        movl    $BOOT_TSS, %eax
        ltr     %ax

        /* CR0: enable PG */
        movl    %cr0, %eax
        orl     $X86_CR0_PG, %eax
        movl    %eax, %cr0

        /* enter 64-bit mode */
        ljmp    $BOOT_CS, $start64

        .code64
start64:
        /* zero bss */
        mov     $_bss, %edi
        mov     $_ebss, %ecx
        xor     %eax, %eax
        sub     %edi, %ecx
        rep stosb

        mov     $stack_top, %rsp
        lea     _start, %rdi
        jmp     main

ENTRY(start_multiboot)
        /* load the 32-bit CS */
        pushq   $BOOT32_CS
        leaq    1f(%rip), %rax
        pushq   %rax
        lretq

        .code32
        /* switch GDT */
        1:
        lgdt    gdt32
        ljmp    $BOOT32_CS, $2f
        2:
        mov     $BOOT32_DS, %ax
        mov     %ax, %ds
        mov     %ax, %es
        mov     %ax, %ss

        /* CR0: disable PG */
        movl    %cr0, %eax
        andl    $~X86_CR0_PG, %eax
        movl    %eax, %cr0

        /* MSR EFER: disable LME */
        movl    $MSR_EFER, %ecx
        rdmsr
        andl    $~EFER_LME, %eax
        wrmsr

        /* CR4: disable PAE, PSE */
        movl    %cr4, %eax
        andl    $~(X86_CR4_PAE|X86_CR4_PSE), %eax
        movl    %eax, %cr4

        /* prepare multiboot */
        movl    $MULTIBOOT_BOOTLOADER_MAGIC, %eax
        movl    %esi, %ebx
        jmp     *%edi

        .balign 8
        /*
         * The 64-bit GDT follows the Linux kernel boot protocol:
         * CS - 0x10; DS - 0x18.
         */
gdt:
        .word   gdt_end - gdt - 1
        .long   gdt
        .word   0
        .quad   0x00cf9a000000ffff      /* 32-bit CS */
        .quad   0x00af9a000000ffff      /* 64-bit CS */
        .quad   0x00cf92000000ffff      /* 64-bit DS */
        .quad   0x0080890000000000      /* TS descriptor */
        .quad   0x0000000000000000      /* TS continued */
gdt_end:

        /*
         * Match the 32-bit GDT of QEMU's and xv6's bootloaders.
         * Since the xv6 kernel doesn't reload GDT (it should),
         * this avoids #GP when running xv6 as a guest.
         * Otherwise, we don't really need this GDT.
         */
gdt32:
        .word   gdt32_end - gdt32 - 1
        .long   gdt32
        .word   0
        .quad   0x00cf9a000000ffff      /* 32-bit CS */
        .quad   0x00cf92000000ffff      /* 32-bit DS */
gdt32_end:

/* boot page table */
        .section .data
        .balign SZ_4K
pml4:
        .quad   pml3 + PTE_PRESENT + PTE_RW
        .rept   512 - 1
        .quad   0
        .endr

pml3:
        index = 0
        .rept   4
        .quad   pml2 + (index * SZ_4K) + PTE_PRESENT + PTE_RW
        index = index + 1
        .endr
        .rept   512 - 4
        .quad   0
        .endr

pml2:
        index = 0
        .rept   512 * 4
        .quad   (index * SZ_2M) + PTE_PRESENT + PTE_RW + PTE_PSE
        index = index + 1
        .endr

        .section .bss
        .balign SZ_4K
stack:
        .space  SZ_8K
stack_top:
