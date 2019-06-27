#pragma once

#include <asm/setup.h>
#include <sys/ctype.h>
#include <sys/string.h>

/* multiboot.c */
void load_multiboot(struct guest_params *guest_params);

/* linux.c */
void load_linux(struct guest_params *guest_params);
