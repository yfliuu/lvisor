#pragma once

#define CPU_ENTRY_AREA_tss 4096 /* offsetof(struct cpu_entry_area, tss) */
#define TSS_ist 36 /* offsetof(struct tss, ist) */
#define TSS_sp0 4 /* offsetof(struct tss, sp0) */

