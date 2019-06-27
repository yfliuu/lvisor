#include <asm/cpu_entry_area.h>
#include <sys/kbuild.h>

int main(void)
{
        OFFSET(CPU_ENTRY_AREA_tss, cpu_entry_area, tss);
        OFFSET(TSS_ist, tss, ist);
        OFFSET(TSS_sp0, tss, sp0);
        BLANK();

        return 0;
}
