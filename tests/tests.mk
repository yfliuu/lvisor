TESTS   :=                              \
        $(O)/tests/hello32.elf          \
        $(O)/tests/hello64.bin          \
        $(O)/tests/lv6.bin              \

TESTS_SRCS = $(wildcard tests/*.S) $(wildcard tests/*.c)
TESTS_OBJS = $(call object,$(TESTS_SRCS))

-include $(TESTS_OBJS:.o=.d)

$(O)/tests/hello32.o: CFLAGS += -m32

$(O)/tests/hello32.elf: $(O)/tests/hello32.o
	$(QUIET_LD)$(LD) -o $@ -m elf_i386 -N -Ttext 0x100000 $<

$(O)/tests/hello64.elf: $(O)/tests/hello64.o
	$(QUIET_LD)$(LD) -o $@ -m elf_x86_64 -N $<

$(O)/tests/lv6.elf: $(KERNEL_LDS) $(KERNEL_OBJS) $(call object,$(wildcard tests/lv6/*.c tests/lv6/*.S))
	$(QUIET_LD)$(LD) -o $@ $(LDFLAGS) -T $^ $(LIBS)
