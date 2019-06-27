#include <asm/desc.h>
#include <asm/i8259.h>
#include <asm/msr.h>
#include <asm/setup.h>

DEFINE_PER_CPU(struct cpu_entry_area, cpu_entry_area);

static struct segment_desc gdt_data[] = {
    [GDT_ENTRY_KERNEL_CS] = GDT_ENTRY_INIT(0xa09b, 0, 0xfffff),
    [GDT_ENTRY_KERNEL_DS] = GDT_ENTRY_INIT(0xc093, 0, 0xfffff),
    [GDT_ENTRY_USER_DS] = GDT_ENTRY_INIT(0xc0f3, 0, 0xfffff),
    [GDT_ENTRY_USER_CS] = GDT_ENTRY_INIT(0xa0fb, 0, 0xfffff),
};

static inline void set_tss_desc(struct segment_desc *gdt, unsigned int entry, void *addr) {
    struct tss_desc tss;

    set_tssldt_descriptor(&tss, __ENTRY_ADDR(addr), DESC_TSS, KERNEL_TSS_LIMIT);
    write_gdt_entry(gdt, entry, &tss, DESC_TSS);
}

static void switch_to_new_gdt(void) {
    int cpu = smp_processor_id();
    size_t i;
    void *gdt = get_cpu_gdt(cpu);
    struct tss *tss = &get_cpu_entry_area(cpu)->tss;
    uint8_t *estacks = get_cpu_entry_area(cpu)->exception_stacks;
    struct desc_ptr gdt_descr = {
        .address = __ENTRY_ADDR(gdt),
        .size = GDT_SIZE - 1,
    };

    memcpy(gdt, gdt_data, sizeof(gdt_data));

    load_gdt(&gdt_descr);

    /* disable IO permission bitmap */
    tss->io_bitmap_base = INVALID_IO_BITMAP_OFFSET;
    /* use per-cpu stack */
    tss->sp0 = __ENTRY_ADDR(&cpu_stacks[cpu]) + CPU_STACK_SIZE;
    /* set IST pointers */
    for (i = 0; i < NR_EXCEPTION_STACKS; ++i)
        tss->ist[i] = __ENTRY_ADDR(estacks + EXCEPTION_STACK_SIZE * (i + 1));

    set_tss_desc(gdt, GDT_ENTRY_TSS, tss);

    load_tr_desc(GDT_ENTRY_TSS);
}

#if 0
static void smep_init(void)
{
        if (!this_cpu_has(X86_FEATURE_SMEP))
                return;

        cr4_set_bits(X86_CR4_SMEP);
}

static void smap_init(void)
{
        unsigned long rflags = save_flags();

        /* This should have been cleared long ago */
        BUG_ON(rflags & X86_RFLAGS_AC);

        if (!this_cpu_has(X86_FEATURE_SMAP))
                return;

        cr4_set_bits(X86_CR4_SMAP);
}
#endif

struct cpu_entry_area *get_cpu_entry_area(int cpu) {
    return per_cpu_ptr(&cpu_entry_area, cpu);
}

void trap_init(void) {
    switch_to_new_gdt();

    idt_setup_traps();
    idt_setup_ist_traps();

    /*
         * Initialize legacy 8259; otherwise Bochs will generate
         * spurious interrupts.
         */
    i8259_init();

    idt_setup_apic_and_irq_gates();

    /* disable sysenter */
    wrmsrl(MSR_IA32_SYSENTER_CS, GDT_ENTRY_INVALID_SEG);
    wrmsrl(MSR_IA32_SYSENTER_ESP, 0);
    wrmsrl(MSR_IA32_SYSENTER_EIP, 0);

    wrmsrl(MSR_FS_BASE, 0);
    wrmsrl(MSR_KERNEL_GS_BASE, 0);

    /* switch gs to shared mapping */
    wrmsrl(MSR_GS_BASE, __ENTRY_ADDR(rdmsrl(MSR_GS_BASE)));
}

void syscall_init(void) {
    uint64_t efer;

    /* enable syscall */
    efer = rdmsrl(MSR_EFER);
    if (!(efer & EFER_SCE))
        wrmsrl(MSR_EFER, efer | EFER_SCE);

    wrmsr(MSR_STAR, 0, ((USER_CS - 16) << 16) | KERNEL_CS);
    wrmsrl(MSR_LSTAR, __ENTRY_ADDR(do_syscall_64));
    wrmsrl(MSR_CSTAR, __ENTRY_ADDR(do_syscall_32));

    /* Flags to clear on syscall */
    wrmsrl(MSR_SYSCALL_MASK,
           X86_RFLAGS_TF | X86_RFLAGS_DF | X86_RFLAGS_IF |
               X86_RFLAGS_IOPL | X86_RFLAGS_AC | X86_RFLAGS_NT);
}
