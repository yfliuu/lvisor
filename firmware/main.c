#include <asm/init.h>
#include <sys/console.h>
#include "boot.h"

void main(struct guest_params *guest_params) {
    porte9_init(BRIGHT_PURPLE);
    vgacon_init();

    pr_info("Starting firmware...\n");

    load_multiboot(guest_params);
    load_linux(guest_params);

    pr_err("Unknown kernel image format.\n");
    die();
}

size_t pr_prefix(char *buf, size_t size) {
    return scnprintf(buf, size, "[  FIRMWARE  ] ");
}
