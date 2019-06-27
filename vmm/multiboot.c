#include <asm/setup.h>
#include <sys/multiboot.h>
#include <sys/string.h>

void multiboot_init(uint32_t magic, struct multiboot_info *multiboot_info)
{
        struct multiboot_mod_list *mods;

        BUG_ON(magic != MULTIBOOT_BOOTLOADER_MAGIC);
        BUG_ON(!multiboot_info);

        if (!(multiboot_info->flags & MULTIBOOT_INFO_MEM_MAP))
                panic("no memory map!\n");
        e820_range_add_multiboot(__va(multiboot_info->mmap_addr), multiboot_info->mmap_length);

        if (!multiboot_info->mods_count)
                panic("no guest kernel loaded!\n");
        mods = __va(multiboot_info->mods_addr);
        guest_params.kernel_start = mods[0].mod_start;
        guest_params.kernel_end = mods[0].mod_end;
        if (strscpy((char *)guest_params.cmdline, __va(mods[0].cmdline), sizeof(guest_params.cmdline)) < 0)
                panic("kernel cmdline too long!\n");

        if (multiboot_info->mods_count > 1) {
                guest_params.initrd_start = mods[1].mod_start;
                guest_params.initrd_end = mods[1].mod_end;
        }

        /* mask out vmm */
        BUG_ON(__pa(_start) % SZ_2M);
        BUG_ON(__pa(_end) % SZ_2M);
        BUG_ON(!e820_mapped_all(__pa(_start), __pa(_end), E820_TYPE_RAM));
        e820_range_update(__pa(_start), _end - _start, E820_TYPE_RAM, E820_TYPE_RESERVED);
}
