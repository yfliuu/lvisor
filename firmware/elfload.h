#ifndef ELF
#error "ELF not defined"
#endif

#define _CONCAT(a, b) a ## _ ## b
#define CONCAT(a, b)  _CONCAT(a, b)

static uint32_t CONCAT(load, ELF)(struct CONCAT(ELF, hdr) *hdr)
{
        size_t i, n;
        struct CONCAT(ELF, phdr) *phdr;
        uint32_t entry = hdr->e_entry;

        phdr = (void *)hdr + hdr->e_phoff;
        for (i = 0, n = hdr->e_phnum; i < n; ++i, ++phdr) {
                void *dest;

                if (phdr->p_type != PT_LOAD)
                        continue;
                dest = __va(phdr->p_paddr);
                memcpy(dest, (void *)hdr + phdr->p_offset, phdr->p_filesz);
                memset(dest + phdr->p_filesz, 0, phdr->p_memsz - phdr->p_filesz);
                /* check if entry is in va */
                if (entry >= phdr->p_vaddr && entry - phdr->p_vaddr < phdr->p_memsz)
                        entry = phdr->p_paddr + (entry - phdr->p_vaddr);
        }

        return entry;
}

#undef CONCAT
#undef _CONCAT
#undef ELF
