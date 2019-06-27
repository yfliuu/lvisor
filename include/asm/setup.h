#pragma once

#include <asm/e820.h>
#include <io/const.h>

#define VMM_START               0x10000000
#define KERNEL_START            0x00100000
#define FIRMWARE_START          0x00001000

/*
 * This applies to every address shared by user and kernel,
 * such as GDT, IDT, and syscall entries.
 */
#define __ENTRY_OFFSET          UINT64_C(0xffff800000000000)
#define __ENTRY_ADDR(x)         (((uintptr_t)(x)) + __ENTRY_OFFSET)

#define NR_syscalls             128

#define CMDLINE_SIZE            1024
#define E820_MAX_ENTRIES_GUEST  128

#ifndef __ASSEMBLER__

struct guest_params {
        uint8_t magic[8];
        uint64_t kernel_start;
        uint64_t kernel_end;
        uint64_t initrd_start;
        uint64_t initrd_end;
        uint8_t cmdline[CMDLINE_SIZE];
        uint32_t e820_entries;
        struct e820_entry e820_table[E820_MAX_ENTRIES_GUEST];
} __packed;

extern struct guest_params guest_params;

#endif  /* !__ASSEMBLER__ */
