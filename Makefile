include config.def
-include config.mk

V               = @
Q               = $(V:1=)
QUIET_CC        = $(Q:@=@echo    '     CC       '$@;)
QUIET_CXX       = $(Q:@=@echo    '     CXX      '$@;)
QUIET_AR        = $(Q:@=@echo    '     AR       '$@;)
QUIET_LD        = $(Q:@=@echo    '     LD       '$@;)
QUIET_GEN       = $(Q:@=@echo    '     GEN      '$@;)
QUIET_PY2       = $(Q:@=@echo    '     PY2      '$@;)
QUIET_PY3       = $(Q:@=@echo    '     PY3      '$@;)
QUIET_PACK		= $(Q:@=@echo    '	   PAC      '$@;)

MKDIR_P         := mkdir -p
LN_S            := ln -s
UNAME_S         := $(shell uname -s)
TOP             := $(shell echo $${PWD-`pwd`})
HOST_CC         := cc
PY2             := python2
PY3             := python3
BOOT			:= boot
ROOTFSDIR		:= rootfs
ROOTFSIMG		:= $(BOOT)/rootfs.xz
DRIVERSDIR		:= drivers

# try the latest version first
LLVM_CONFIG     := llvm-config-5.0
ifeq ($(shell which $(LLVM_CONFIG) 2> /dev/null),)
LLVM_CONFIG     := llvm-config
endif
# Homebrew hides llvm here
ifeq ($(shell which $(LLVM_CONFIG) 2> /dev/null),)
LLVM_CONFIG     := /usr/local/opt/llvm/bin/llvm-config
endif
ifeq ($(shell which $(LLVM_CONFIG) 2> /dev/null),)
LLVM_PREFIX     :=
else
LLVM_PREFIX     := "$(shell $(LLVM_CONFIG) --bindir)/"
endif
LLVM_CC         := $(LLVM_PREFIX)clang -target $(ARCH)-linux-gnu
LLVM_HOST_CC    := $(LLVM_PREFIX)clang
LLVM_LINK       := $(LLVM_PREFIX)llvm-link

ifeq ($(UNAME_S),Darwin)
USE_CLANG       ?= 1
TOOLPREFIX      ?= $(ARCH)-linux-gnu-
endif

ifeq ($(USE_CLANG),1)
CC              := $(LLVM_CC)
else
CC              := $(TOOLPREFIX)gcc
endif
LD              := $(TOOLPREFIX)ld
AR              := $(TOOLPREFIX)ar
RANLIB          := $(TOOLPREFIX)ranlib
NM              := $(TOOLPREFIX)nm
OBJCOPY         := $(TOOLPREFIX)objcopy
OBJDUMP         := $(TOOLPREFIX)objdump
GDB             := $(TOOLPREFIX)gdb
BOCHS           := bochs
QEMU            := qemu-system-$(ARCH)

BASE_CFLAGS     += -ffreestanding
BASE_CFLAGS     += -fno-stack-protector
BASE_CFLAGS     += -ffunction-sections -fdata-sections
BASE_CFLAGS     += -g
BASE_CFLAGS     += -Wall -MD -MP
BASE_CFLAGS     += -Wno-initializer-overrides

CFLAGS          += $(BASE_CFLAGS)
CFLAGS          += -fno-PIE -O2
CFLAGS          += -fwrapv
CFLAGS          += -mno-red-zone
CFLAGS          += -mno-sse -mno-mmx -mno-sse2 -mno-3dnow -mno-avx
CFLAGS          += -I include
CFLAGS          += -DNR_CPUS=1

USER_CFLAGS     += $(BASE_CFLAGS)
USER_CFLAGS     += -fno-PIE -fwrapv

LDFLAGS         += --gc-sections

object          = $(addprefix $(O)/,$(patsubst %.c,%.o,$(patsubst %.S,%.o,$(filter-out %/asm-offsets.c %.lds.S,$(1)))))

include kernel/kernel.mk
include firmware/firmware.mk
include vmm/vmm.mk
include tests/tests.mk

all: $(ALL)

.DEFAULT_GOAL = all

# Default sed regexp - multiline due to syntax constraints
#
# Use [:space:] because LLVM's integrated assembler inserts <tab> around
# the .ascii directive whereas GCC keeps the <space> as-is.
define sed-offsets
        's:^[[:space:]]*\.ascii[[:space:]]*"\(.*\)".*:\1:; \
        /^->/{s:->#\(.*\):/* \1 */:; \
        s:^->\([^ ]*\) [\$$#]*\([^ ]*\) \(.*\):#define \1 \2 /* \3 */:; \
        s:->::; p;}'
endef

define gen-offsets
        (set -e; \
         echo "#pragma once"; \
         echo ""; \
         sed -ne $(sed-offsets) )
endef

%/asm-offsets.h: $(O)/%/asm-offsets.S
	$(QUIET_GEN)$(call gen-offsets) < $< > $@~
	$(Q)mv $@~ $@

%.bin: %.elf
	$(QUIET_GEN)$(OBJCOPY) -O binary $< $@
	$(Q)$(OBJDUMP) -S $< > $(basename $@).asm

$(O)/%.lds: %.lds.S
	$(Q)$(MKDIR_P) $(@D)
	$(QUIET_GEN)$(CPP) -o $@ -P $(CFLAGS) $<

$(O)/%.S: %.c
	$(Q)$(MKDIR_P) $(@D)
	$(QUIET_CC)$(CC) -o $@ -S $(CFLAGS) $<

$(O)/%.o: %.S
	$(Q)$(MKDIR_P) $(@D)
	$(QUIET_CC)$(CC) -o $@ -c $(CFLAGS) $<

$(O)/%.o: %.c
	$(Q)$(MKDIR_P) $(@D)
	$(QUIET_CC)$(CC) -o $@ -c $(CFLAGS) -DCONFIG_SAFE_TYPE -D__MODULE__='"$(basename $(notdir $<))"' $<

clean:
	-rm -rf $(O)
	-rm -f $(ROOTFSDIR)/etc/mod1.ko
	rm -f $(BOOT)/rootfs.xz

check: all $(TESTS)
ifneq ($(UNAME_S),Linux)
	$(error works on Linux only)
else
	$(Q)tests/test_vmm.py -v
endif

# usage: $(call try-run,cmd,option-ok,otherwise)
__try-run = set -e;                     \
        if ($(1)) >/dev/null 2>&1;      \
        then echo "$(2)";               \
        else echo "$(3)";               \
        fi

try-run = $(shell $(__try-run))

# usage: $(call bochs-option,option,option-ok,otherwise)
bochs-option = $(call try-run,$(BOCHS) --help 2>&1 | grep -- $(1),$(2),$(3))

define run-bochs
        CPU=$(BOCHS_CPU) NR_CPUS=1 ISO=$< \
        $(BOCHS) -q -f boot/bochsrc $(call bochs-option,-rc,-rc boot/bochs.dbg) || true
endef

bochs: $(VMM_ISO)
	$(call run-bochs)

bochs-kernel: $(KERNEL_ISO)
	$(call run-bochs)

define gen-iso
        cp -r boot/isolinux $(@D)/iso/
        $(LN_S) $(realpath $(KERNEL))$(if $(INITRD), $(realpath $(INITRD))) $(@D)/iso/
        mkisofs -r -f -o $@~ -b isolinux/isolinux.bin -c isolinux/boot.cat -no-emul-boot -boot-load-size 4 -boot-info-table $(@D)/iso
        boot/isohybrid.pl $@~
        mv $@~ $@
endef

$(KERNEL_ISO): $(KERNEL)
	$(Q)-rm -rf $(@D)/iso
	$(Q)$(MKDIR_P) $(@D)/iso/
	$(Q)echo 'default mboot.c32 /$(notdir $(KERNEL))$(if $(APPEND), $(APPEND))$(if $(INITRD), --- /$(notdir $(INITRD)))' > $(@D)/iso/isolinux.cfg
	$(QUIET_GEN)$(call gen-iso)

$(DRIVERSDIR)/mod1.ko: $(DRIVERSDIR)/mod1.c
	$(QUIET_GEN)make -C /lib/modules/5.1.16-arch1-1-ARCH/build/ M=$(realpath $(DRIVERSDIR)) modules

$(ROOTFSIMG): $(ROOTFSDIR)/* $(DRIVERSDIR)/mod1.ko
	@rm -f $(ROOTFSIMG)
	@cp $(DRIVERSDIR)/mod1.ko $(ROOTFSDIR)/etc
	$(QUIET_PACK)cd $(ROOTFSDIR) && find . | cpio -R root:root -H newc -o | xz -9 --check=none > ../$(ROOTFSIMG)

QEMUOPTS += -machine q35,accel=kvm:tcg -cpu $(QEMU_CPU)
QEMUOPTS += -m 1G
QEMUOPTS += -device isa-debug-exit
QEMUOPTS += -debugcon file:/dev/stdout
QEMUOPTS += -serial mon:stdio -display none

QEMUGDB := -gdb tcp::10086

qemu: $(VMM_BIN) $(KERNEL) $(ROOTFSIMG)
ifneq ($(UNAME_S),Linux)
	$(error works on Linux only)
else
	$(QEMU) $(QEMUOPTS) -kernel $(VMM_BIN) -initrd "$(KERNEL)$(if $(APPEND), $(APPEND)),$(if $(INITRD),$(INITRD),/dev/null)" || true
endif

qemu-gdb: $(VMM_ELF) $(KERNEL) $(ROOTFSIMG)
ifneq ($(UNAME_S),Linux)
	$(error works on Linux only)
else
	$(QEMU) $(QEMUOPTS) -S $(QEMUGDB) -kernel $(VMM_ELF) -initrd "$(KERNEL)$(if $(APPEND), $(APPEND)),$(if $(INITRD),$(INITRD),/dev/null)" || true
endif

qemu-kernel: $(KERNEL) $(ROOTFSIMG)
	$(QEMU) $(QEMUOPTS) -display sdl -kernel $(KERNEL)$(if $(APPEND), -append $(APPEND))$(if $(INITRD), -initrd $(INITRD)) || true

.PRECIOUS: $(O)/%.o $(O)/%.S

.PHONY: all clean check bochs bochs-kernel qemu qemu-kernel
