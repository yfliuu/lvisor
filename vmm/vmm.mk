
VMM_ELF         := $(O)/vmm.elf
VMM_BIN         := $(basename $(VMM_ELF)).bin
VMM_ASM         := $(basename $(VMM_ELF)).asm
VMM_ISO         := $(basename $(VMM_ELF)).iso
VMM_LDS         := $(O)/vmm/vmm.lds
VMM_SRCS        += $(wildcard vmm/*.S) $(wildcard vmm/*.c)
VMM_OBJS        := $(call object,$(VMM_SRCS))

VMM_PY          := $(O)/vmm/vmm.py

$(O)/vmm/vmm.ll: $(VMM_LLS)
	$(QUIET_GEN)$(LLVM_LINK) -o $@ -S $^

$(O)/vmm/firmware.o: $(FIRMWARE_BIN)
$(O)/vmm/firmware.o: CFLAGS += -I $(O)/firmware

$(VMM_ELF): $(VMM_LDS) $(VMM_OBJS) $(KERNEL_OBJS)
	$(QUIET_LD)$(LD) -o $@ $(LDFLAGS) -T $(VMM_LDS) $(VMM_OBJS) $(KERNEL_OBJS)

$(VMM_ISO): $(VMM_BIN) $(KERNEL) $(INITRD)
	$(Q)-rm -rf $(@D)/iso
	$(Q)$(MKDIR_P) $(@D)/iso/
	$(Q)$(LN_S) $(realpath $(VMM_BIN)) $(@D)/iso/
	$(Q)echo 'default mboot.c32 /$(notdir $(VMM_BIN)) --- /$(notdir $(KERNEL))$(if $(APPEND), $(APPEND))$(if $(INITRD), --- /$(notdir $(INITRD)))' > $(@D)/iso/isolinux.cfg
	$(QUIET_GEN)$(call gen-iso)

-include $(VMM_OBJS:.o=.d)

.PHONY: $(VMM_ISO) verify-vmm

ALL += $(VMM_BIN)
