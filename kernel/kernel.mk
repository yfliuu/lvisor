KERNEL_LDS      := $(O)/kernel/kernel.lds
KERNEL_SRCS     := $(wildcard lib/*.S) $(wildcard lib/*.c)       \
                   $(wildcard kernel/*.S) $(wildcard kernel/*.c) \
                   $(wildcard kernel/acpica/*.c)                 \

KERNEL_OBJS     := $(call object,$(KERNEL_SRCS))
KERNEL_ISO      := $(O)/kernel.iso

-include $(KERNEL_OBJS:.o=.d)

kernel/capflags.c: include/asm/cpufeatures.h
	$(Q)kernel/mkcapflags.sh $< | expand > $@~
	$(QUIET_GEN)mv $@~ $@
