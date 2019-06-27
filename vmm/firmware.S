#include <io/linkage.h>

        .section .rodata

GLOBAL(_binary_firmware_start)
        .incbin "firmware.bin"
GLOBAL(_binary_firmware_end)
