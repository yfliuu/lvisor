#include <asm/init.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <sys/console.h>

noreturn void main(unsigned int magic, struct multiboot_info *multiboot_info) {
    /* enable output first */
    // the head.S called main and jump here
    // Register porte9 and vgacon
    // This includes registering these 2 consoles to the console_driver list.
    porte9_init(BRIGHT_YELLOW);
    vgacon_init();

    // Get detailed information about the CPU
    cpu_init();

    /* require cpu */
    // TSC: Time Stamp Counter: TSC is a 64bit register present on all x86 processors
    // since Pentium. It counts the number of cycles since reset.
    tsc_init();

    /* require cpu */
    // Including: switch to new gdt, setup traps, init i8259, setup some apic and irq gates
    // And disable sysenter
    trap_init();

    multiboot_init(magic, multiboot_info);

    acpi_table_init();

    /* require acpi */
    apic_init();

    /* require multiboot */
    kvm_init();

    kvm_bsp_run();
}
