#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/kvm_host.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <sys/errno.h>
#include <sys/string.h>

#define EPTE_READ BIT_64(0)
#define EPTE_WRITE BIT_64(1)
#define EPTE_EXECUTE BIT_64(2)
#define EPTE_PSE BIT_64(7)

extern struct kvm_x86_ops vmx_x86_ops;

static struct kvm_x86_ops *kvm_x86_ops;

static uint64_t ept_pml4[512] __aligned(PAGE_SIZE);
static uint64_t ept_pdpt_0_512g[512] __aligned(PAGE_SIZE);
static uint64_t ept_pd_0_4g[4 * 512] __aligned(PAGE_SIZE);

static void *crvm_buffer;

DEFINE_PER_CPU(struct kvm_vcpu *, current_vcpu);

static void construct_tdp(void) {
    size_t i, n;
    uint64_t rwx = EPTE_READ | EPTE_WRITE | EPTE_EXECUTE;

    BUG_ON(__pa(_start) % SZ_2M);
    BUG_ON(__pa(_end) % SZ_2M);

    // pr_info("ept_pml4: 0x%lx\n", (uint64_t)ept_pml4);
    ept_pml4[0] = __pa(ept_pdpt_0_512g) | rwx;
    for (i = 0; i < 4; ++i)
        ept_pdpt_0_512g[i] = __pa(ept_pd_0_4g + i * 512) | rwx;

    /*
     * Map the entire [0, 4G) except for [_start, _end)
     * Is this safe?
     */
    for (i = 0, n = __pa(_start) / SZ_2M; i < n; ++i)
        ept_pd_0_4g[i] = (SZ_2M * i) | rwx | EPTE_PSE;
    /* for (i = __pa(_end) / SZ_2M, n = SZ_4G / SZ_2M; i < n; ++i)
        ept_pd_0_4g[i] = (SZ_2M * i) | rwx | EPTE_PSE; */

    /* We allocated a 2M buffer before _end so map from there */
    for (i = __pa(_end) / SZ_2M - 1, n = SZ_4G / SZ_2M; i < n; ++i)
        ept_pd_0_4g[i] = (SZ_2M * i) | rwx | EPTE_PSE;
    
    crvm_buffer = (void *)(_end - SZ_2M);
}

void kvm_init(void) {
    extern char _binary_firmware_start[], _binary_firmware_end[];

    if (vmx_x86_ops.cpu_has_kvm_support()) kvm_x86_ops = &vmx_x86_ops;

    if (!kvm_x86_ops) panic("kvm: no hardware support\n");

    if (kvm_x86_ops->disabled_by_bios()) panic("kvm: disabled by bios\n");

    kvm_x86_ops->hardware_setup();
    construct_tdp();

    /* copy firmware */
    memcpy(__va(FIRMWARE_START), _binary_firmware_start,
           _binary_firmware_end - _binary_firmware_start);

    /* initialize e820 */
    BUG_ON(ARRAY_SIZE(guest_params.e820_table) < e820_table.nr_entries);
    memcpy(guest_params.e820_table, e820_table.entries,
           e820_table.nr_entries * sizeof(struct e820_entry));
    guest_params.e820_entries = e820_table.nr_entries;

    /* initialize header */
    guest_params.magic[0] = 0xe9;
    guest_params.magic[1] = (sizeof(guest_params) - 3) & 0xff;
    guest_params.magic[2] = ((sizeof(guest_params) - 3) >> 8) & 0xff;

    /* check magic */
    if (memcmp(guest_params.magic, __va(FIRMWARE_START),
               sizeof(guest_params.magic)))
        panic("firmware magic doesn't match!\n");

    /* copy guest_params */
    memcpy(__va(FIRMWARE_START), &guest_params, sizeof(struct guest_params));
}

static struct kvm_vcpu *create_vcpu(void) {
    struct kvm_vcpu *vcpu;

    vcpu = kvm_x86_ops->vcpu_enable();
    kvm_x86_ops->vcpu_setup(vcpu);

    /* set EPT */
    kvm_x86_ops->set_tdp(vcpu, __pa(ept_pml4));

    this_cpu_write(current_vcpu, vcpu);
    return vcpu;
}

noreturn void kvm_loop(struct kvm_vcpu *vcpu) {
    for (;;) {
        kvm_x86_ops->run(vcpu);
        kvm_x86_ops->handle_exit(vcpu);
    }
}

noreturn static void run_vcpu(struct kvm_vcpu *vcpu, uint32_t start_ip) {
    struct kvm_segment cs = {
        .limit = 0xffff,
        .type = 11,
        .s = 1,
        .present = 1,
    };

    cs.base = start_ip & 0xffff0000;
    cs.selector = cs.base >> 4;
    kvm_set_segment(vcpu, &cs, VCPU_SREG_CS);
    kvm_rip_write(vcpu, start_ip & 0xffff);

    kvm_loop(vcpu);
}

noreturn void kvm_bsp_run(void) {
    struct kvm_vcpu *vcpu;

    vcpu = create_vcpu();
    run_vcpu(vcpu, FIRMWARE_START);
}

unsigned long kvm_rip_read(struct kvm_vcpu *vcpu) {
    return kvm_x86_ops->get_rip(vcpu);
}

void kvm_rip_write(struct kvm_vcpu *vcpu, unsigned long val) {
    kvm_x86_ops->set_rip(vcpu, val);
}

void kvm_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg) {
    kvm_x86_ops->get_segment(vcpu, var, seg);
}

void kvm_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg) {
    kvm_x86_ops->set_segment(vcpu, var, seg);
}

static inline uint32_t bit(int bitno) { return 1U << (bitno & 31); }

void kvm_emulate_cpuid(struct kvm_vcpu *vcpu) {
    uint32_t eax, ebx, ecx, edx, leaf;

    /* assume no cpuid fault support (disabled from msr) */

    eax = kvm_register_read(vcpu, VCPU_REGS_RAX);
    leaf = eax;
    ecx = kvm_register_read(vcpu, VCPU_REGS_RCX);
    native_cpuid(&eax, &ebx, &ecx, &edx);

    switch (leaf) {
        case 1:
            /*
             * Linux will read a set of VMX-related MSRs if VMX is detected.
             * Since we don't allow access to these MSRs, hide VMX.
             */
            ecx &= ~bit(X86_FEATURE_VMX);
            /*
             * Emulate x2apic on AMD CPUs as it's much easier to intercept.
             * AVIC is supported on Ryzen but not on Bochs/KVM yet.
             */
            ecx |= bit(X86_FEATURE_X2APIC);
            /*
             * Indicate to the guest kernel that it's under virtualization.
             * Linux guest support needs this bit.
             */
            ecx |= bit(X86_FEATURE_HYPERVISOR);

            /* Disable xsave */
            ecx &= ~bit(X86_FEATURE_XSAVE);
            break;
        case 4:
            /*
             * On Intel CPUs without extended topology, Linux guest computes
             * x86_max_cores using [26:31] of CPUID 4.  Linux guest further
             * computes __max_logical_packages as total_cpus / x86_max_cores.
             *
             * On Bochs x86_max_cores is hardcoded to be 8; the guest would
             * underestimate the number of cores and fail to boot APs.
             *
             * The workaround here is to disable this leaf and Linux guest
             * will fall back to set x86_max_cores to 1.
             */
            eax = ebx = ecx = edx = 0;
            break;
        case 0x40000000:
            /* fake string "KVMKVMKVM" for x2apic in Linux guest */
            eax = 0x40000001;
            ebx = 0x4b4d564b;
            ecx = 0x564b4d56;
            edx = 0x4d;
            break;
        case 0x40000001:
            /*
             * Linux guest support uses this leaf to commucate with KVM
             * for hypervisor-specific features.  Since the VMM doesn't
             * have these features, don't advertise.
             */
            eax = ebx = ecx = edx = 0;
            break;
        default:
            break;
    }

    kvm_register_write(vcpu, VCPU_REGS_RAX, eax);
    kvm_register_write(vcpu, VCPU_REGS_RBX, ebx);
    kvm_register_write(vcpu, VCPU_REGS_RCX, ecx);
    kvm_register_write(vcpu, VCPU_REGS_RDX, edx);
    return kvm_skip_emulated_instruction(vcpu);
}

void kvm_skip_emulated_instruction(struct kvm_vcpu *vcpu) {
    kvm_x86_ops->skip_emulated_instruction(vcpu);
    /* TODO: check TF in RFLAGS for single-step */
}
