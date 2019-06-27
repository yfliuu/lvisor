#pragma once

#include <asm/processor.h>

/*
 * cpu_entry_area is a percpu region that contains things needed by the CPU
 * and early entry/exit code.
 */
struct cpu_entry_area {
        uint8_t gdt[PAGE_SIZE];
        struct tss tss;
        /* exception stacks used for IST entries */
        uint8_t exception_stacks[NR_EXCEPTION_STACKS * EXCEPTION_STACK_SIZE];
};

struct cpu_entry_area *get_cpu_entry_area(int cpu);
