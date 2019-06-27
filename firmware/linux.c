#include <asm/bootparam.h>
#include <asm/desc.h>
#include <sys/console.h>
#include "boot.h"

#define SETUP_HDR_OFFSET        0x1F1
#define MAGIC_SIGNATURE_OFFSET  0x202
#define MAGIC_SIGNATURE         0x53726448
#define SECT_SIZE               512

static struct boot_params params;

void pr_guest_param(struct guest_params *guest_params);
void strcpy(char *dst, const char *src);

/*
From x86 linux boot protocol

For machine with 64bit cpus and 64bit kernel, we could use 64bit bootloader
and we need a 64-bit boot protocol.

In 64-bit boot protocol, the first step in loading a Linux kernel
should be to setup the boot parameters (struct boot_params,
traditionally known as "zero page"). The memory for struct boot_params
could be allocated anywhere (even above 4G) and initialized to all zero.
Then, the setup header at offset 0x01f1 of kernel image on should be
loaded into struct boot_params and examined. The end of setup header
can be calculated as follows:

	0x0202 + byte value at offset 0x0201

In addition to read/modify/write the setup header of the struct
boot_params as that of 16-bit boot protocol, the boot loader should
also fill the additional fields of the struct boot_params as described
in zero-page.txt.

After setting up the struct boot_params, the boot loader can load
64-bit kernel in the same way as that of 16-bit boot protocol, but
kernel could be loaded above 4G.

In 64-bit boot protocol, the kernel is started by jumping to the
64-bit kernel entry point, which is the start address of loaded
64-bit kernel plus 0x200.

At entry, the CPU must be in 64-bit mode with paging enabled.
The range with setup_header.init_size from start address of loaded
kernel and zero page and command line buffer get ident mapping;
a GDT must be loaded with the descriptors for selectors
__BOOT_CS(0x10) and __BOOT_DS(0x18); both descriptors must be 4G flat
segment; __BOOT_CS must have execute/read permission, and __BOOT_DS
must have read/write permission; CS must be __BOOT_CS and DS, ES, SS
must be __BOOT_DS; interrupt must be disabled; %rsi must hold the base
address of the struct boot_params.
 */

void load_linux(struct guest_params *guest_params) {
    struct setup_header *hdr, *header_image;
    void *image;
    uint64_t kernel_start;
    uint16_t heap_end;
    void (*entry)(uint64_t, struct boot_params *);

    image = __va(guest_params->kernel_start);
    hdr = &params.hdr;

    memset(&params, 0, sizeof(struct boot_params));

    if (*(uint32_t *)(image + MAGIC_SIGNATURE_OFFSET) != MAGIC_SIGNATURE) {
        pr_info("Kernel too old.\n");
        return;
    }
    
    header_image = (struct setup_header *)(image + SETUP_HDR_OFFSET);

    memcpy(hdr, header_image, 0x0202 + *(uint8_t *)(image + 0x0201));
    // pr_info("address of load_linux 0x%lx, param 0x%lx, hdr 0x%lx\n", (uint64_t)load_linux, (uint64_t)&params, (uint64_t)&hdr);

    if (hdr->setup_sects == 0) { hdr->setup_sects = 4; }

    pr_info("Kernel version: %s\n", (char *)(image + hdr->kernel_version + 0x200));

    /* we assume a boot protocol greater than 2.02. No unnecessary support for old kernels. */
	hdr->type_of_loader = 0xFF;
    if (guest_params->initrd_start < guest_params->initrd_end) {
        hdr->ramdisk_image = guest_params->initrd_start & 0x00000000ffffffff;
        hdr->ramdisk_size = (guest_params->initrd_end - guest_params->initrd_start) & 0x00000000ffffffff;
    }

    if (!(hdr->loadflags & 0x01)) {
        pr_info("the kernel image was not bzImage format\n");
        return;
    }

    heap_end = 0xe000; 

    hdr->heap_end_ptr = heap_end - 0x200;
    hdr->loadflags |= 0x80; /* CAN_USE_HEAP */
    hdr->vid_mode = 0xffff;

    /* cmd_line_ptr can be anywhere between the setup heap end and 0xA0000 */
    /* tell the kernel to use a serial console */
    hdr->cmd_line_ptr = (uint32_t)(uint64_t)&"vga=0xffff mem=512M console=ttyS0,9600";
    pr_info("Kernel command line options: %s\n", (char *)(uint64_t)hdr->cmd_line_ptr);

    /* setup additional fields in boot_params */
    params.ext_ramdisk_image = (guest_params->initrd_start & 0xffffffff00000000) >> 32;
    params.ext_ramdisk_size = ((guest_params->initrd_end - guest_params->initrd_start) & 0xffffffff00000000) >> 32;
    params.e820_entries = guest_params->e820_entries;
    memcpy(&params.e820_table, &guest_params->e820_table, guest_params->e820_entries * sizeof(struct boot_e820_entry));

    /* load the rest of the kernel */
    kernel_start = (uint64_t)(image + (hdr->setup_sects + 1) * SECT_SIZE);
    memcpy((void *)__va(KERNEL_START), (void *)kernel_start, guest_params->kernel_end - kernel_start);

    /* jump to kernel entry */
    entry = (void (*)(uint64_t, struct boot_params *))(__va(KERNEL_START) + 0x200);
    /* %rsi must contain the address of struct boot_params, no restriction on %rdi so put 0 in it */
    entry(0, &params);

    return;
}

void pr_guest_param(struct guest_params *guest_params) {
    int i;

    pr_info("guest_params\n");
    pr_info("\tmagic_number\n");
    pr_info("\t\t%x %x %x %x\n", guest_params->magic[0], guest_params->magic[1], guest_params->magic[2], guest_params->magic[3]);
    pr_info("\tkernel_start %lx\n", guest_params->kernel_start);
    pr_info("\tkernel_end %lx\n", guest_params->kernel_end);
    pr_info("\tinitrd_start %lx\n", guest_params->initrd_start);
    pr_info("\tinitrd_end %lx\n", guest_params->initrd_end);
    pr_info("\tcmd_line %s\n", guest_params->cmdline);
    pr_info("\te820_entries %d\n", guest_params->e820_entries);
    pr_info("\te820_table\n");
    for (i = 0; i < guest_params->e820_entries; i++) 
        pr_info("\t\t%lx %lx %x\n", guest_params->e820_table[i].addr, 
            guest_params->e820_table[i].size, guest_params->e820_table[i].type);
}

void strcpy(char *dst, const char *src) {
    while (*src) { *dst++ = *src++; }
}