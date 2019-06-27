#include <sys/elf.h>
#include <sys/multiboot.h>
#include "boot.h"

#define ELF elf32
#include "elfload.h"

#define ELF elf64
#include "elfload.h"

static struct multiboot_info multiboot_info;
static struct multiboot_mod_list mod_list;
static struct multiboot_mmap_entry mmap[E820_MAX_ENTRIES_GUEST];

static void *find_multiboot_header(void *base) {
    uint32_t *start, *end;

    start = base;
    end = base + MULTIBOOT_SEARCH - sizeof(struct multiboot_info);
    for (; start < end; ++start) {
        if (*start == MULTIBOOT_HEADER_MAGIC)
            return start;
    }

    return NULL;
}

noreturn void start_multiboot(uint32_t entry, void *);

void load_multiboot(struct guest_params *guest_params) {
    void *image = __va(guest_params->kernel_start);
    struct multiboot_header *header;
    uint32_t entry;
    int i, n;

    header = find_multiboot_header(image);
    if (!header)
        return;

    pr_info("multiboot header found\n");

    if (header->flags & MULTIBOOT_AOUT_KLUDGE) {
        memcpy(__va(header->load_addr), image, header->load_end_addr - header->load_addr);
        memset(__va(header->load_end_addr), 0, header->bss_end_addr - header->load_end_addr);
        entry = header->entry_addr;
    } else {
        unsigned char *e_ident = image;

        /* elf magic */
        if (memcmp(e_ident, ELFMAG, SELFMAG))
            return;
        switch (e_ident[EI_CLASS]) {
            case ELFCLASS32:
                entry = load_elf32(image);
                break;
            case ELFCLASS64:
                entry = load_elf64(image);
                break;
            default:
                return;
        }
    }

    multiboot_info.flags = 0 | MULTIBOOT_INFO_MEMORY | MULTIBOOT_INFO_CMDLINE | MULTIBOOT_INFO_MODS | MULTIBOOT_INFO_MEM_MAP;
    multiboot_info.mem_lower = 0;
    multiboot_info.mem_upper = (VMM_START - SZ_1M) / SZ_1K;
    multiboot_info.cmdline = __pa(guest_params->cmdline);
    if (guest_params->initrd_start < guest_params->initrd_end) {
        multiboot_info.mods_count = 1;
        mod_list.mod_start = guest_params->initrd_start;
        mod_list.mod_end = guest_params->initrd_end;
        mod_list.cmdline = __pa("initrd");
        multiboot_info.mods_addr = __pa(&mod_list);
    }
    n = guest_params->e820_entries;
    for (i = 0; i < n; ++i) {
        struct e820_entry *e = &guest_params->e820_table[i];

        mmap[i].size = sizeof(mmap[i]) - sizeof(mmap[i].size);
        mmap[i].addr = e->addr;
        mmap[i].len = e->size;
        mmap[i].type = e->type;
    }
    multiboot_info.mmap_addr = __pa(mmap);
    multiboot_info.mmap_length = n * sizeof(mmap[0]);

    start_multiboot(entry, &multiboot_info);
}
