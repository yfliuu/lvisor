
FIRMWARE_ELF    := $(O)/firmware/firmware.elf
FIRMWARE_BIN    := $(basename $(FIRMWARE_ELF)).bin
FIRMWARE_LDS    := $(O)/firmware/firmware.lds
FIRMWARE_SRCS   := firmware/head.S $(wildcard firmware/*.c)
FIRMWARE_OBJS   := $(call object,$(FIRMWARE_SRCS))

$(FIRMWARE_ELF): $(FIRMWARE_LDS) $(FIRMWARE_OBJS) $(KERNEL_OBJS)
	$(QUIET_LD)$(LD) -o $@ $(LDFLAGS) -T $(FIRMWARE_LDS) $(FIRMWARE_OBJS) $(KERNEL_OBJS) 

-include $(FIRMWARE_OBJS:.o=.d)
