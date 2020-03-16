#include <asm/apic.h>
#include <asm/cpufeature.h>
#include <asm/desc.h>
#include <asm/kvm_host.h>
#include <asm/mmu.h>
#include <asm/msr.h>
#include <asm/tsc.h>
#include <asm/vmx.h>

#define NR_AUTOLOAD_MSRS 8

extern const uint64_t vmx_return;

struct vmcs {
    uint32_t revision_id;
    uint32_t abort;
    uint8_t data[PAGE_SIZE - 8];
} __packed __aligned(PAGE_SIZE);

static struct vmcs_config {
    int size;
    uint32_t basic_cap;
    uint32_t revision_id;
    uint32_t pin_based_exec_ctrl;
    uint32_t cpu_based_exec_ctrl;
    uint32_t cpu_based_2nd_exec_ctrl;
    uint32_t vmexit_ctrl;
    uint32_t vmentry_ctrl;
} vmcs_config;

static struct vmx_capability {
    uint32_t ept;
    uint32_t vpid;
} vmx_capability;

#define VMX_SEGMENT_FIELD(seg)              \
    [VCPU_SREG_##seg] = {                   \
        .selector = GUEST_##seg##_SELECTOR, \
        .base = GUEST_##seg##_BASE,         \
        .limit = GUEST_##seg##_LIMIT,       \
        .ar_bytes = GUEST_##seg##_AR_BYTES, \
    }

static const struct kvm_vmx_segment_field {
    unsigned selector;
    unsigned base;
    unsigned limit;
    unsigned ar_bytes;
} kvm_vmx_segment_fields[] = {
    VMX_SEGMENT_FIELD(CS),
    VMX_SEGMENT_FIELD(DS),
    VMX_SEGMENT_FIELD(ES),
    VMX_SEGMENT_FIELD(FS),
    VMX_SEGMENT_FIELD(GS),
    VMX_SEGMENT_FIELD(SS),
    VMX_SEGMENT_FIELD(TR),
    VMX_SEGMENT_FIELD(LDTR),
};

static const uint32_t vmx_msr_index[] = {
    MSR_KERNEL_GS_BASE,
    /*
     * VMM doesn't use syscalls so there is no need to save
     * MSR_SYSCALL_MASK, MSR_LSTAR, MSR_CSTAR, MSR_STAR.
     */
};

struct vcpu_vmx {
    struct kvm_vcpu vcpu;
    uint64_t host_rsp;
    int fail;
    int launched;
    struct msr_autoload {
        struct vmx_msr_entry guest[NR_AUTOLOAD_MSRS];
        struct vmx_msr_entry host[NR_AUTOLOAD_MSRS];
    } msr_autoload;
};

static unsigned long msr_bitmap[PAGE_SIZE / sizeof(unsigned long)] __aligned(
    PAGE_SIZE);
static DEFINE_PER_CPU(struct vmcs, vmxarea) __aligned(PAGE_SIZE);
static DEFINE_PER_CPU(struct vmcs, current_vmcs) __aligned(PAGE_SIZE);
static DEFINE_PER_CPU(struct vcpu_vmx, current_vmx);

#define KVM_GUEST_CR0_ALWAYS_ON (X86_CR0_WP | X86_CR0_NE)
#define KVM_GUEST_CR4_ALWAYS_ON (X86_CR4_VMXE)

static inline struct vcpu_vmx *to_vmx(struct kvm_vcpu *vcpu) {
    return container_of(vcpu, struct vcpu_vmx, vcpu);
}

static inline bool cpu_has_vmx_ept_2m_page(void) {
    return vmx_capability.ept & VMX_EPT_2MB_PAGE_BIT;
}

static inline bool cpu_has_vmx_ept_4levels(void) {
    return vmx_capability.ept & VMX_EPT_PAGE_WALK_4_BIT;
}

static int vmx_cpu_has_kvm_support(void) {
    return this_cpu_has(X86_FEATURE_VMX);
}

#define vmcs_check16(field)                                                  \
    ({                                                                       \
        BUILD_BUG_ON_MSG(((field)&0x6001) == 0x2000,                         \
                         "16-bit accessor invalid for 64-bit field");        \
        BUILD_BUG_ON_MSG(((field)&0x6001) == 0x2001,                         \
                         "16-bit accessor invalid for 64-bit high field");   \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x4000,                         \
                         "16-bit accessor invalid for 32-bit high field");   \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x6000,                         \
                         "16-bit accessor invalid for natural width field"); \
    })

#define vmcs_check32(field)                                                  \
    ({                                                                       \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0,                              \
                         "32-bit accessor invalid for 16-bit field");        \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x6000,                         \
                         "32-bit accessor invalid for natural width field"); \
    })

#define vmcs_check64(field)                                                  \
    ({                                                                       \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0,                              \
                         "64-bit accessor invalid for 16-bit field");        \
        BUILD_BUG_ON_MSG(((field)&0x6001) == 0x2001,                         \
                         "64-bit accessor invalid for 64-bit high field");   \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x4000,                         \
                         "64-bit accessor invalid for 32-bit field");        \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x6000,                         \
                         "64-bit accessor invalid for natural width field"); \
    })

#define vmcs_checkl(field)                                                   \
    ({                                                                       \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0,                              \
                         "Natural width accessor invalid for 16-bit field"); \
        BUILD_BUG_ON_MSG(((field)&0x6001) == 0x2000,                         \
                         "Natural width accessor invalid for 64-bit field"); \
        BUILD_BUG_ON_MSG(                                                    \
            ((field)&0x6001) == 0x2001,                                      \
            "Natural width accessor invalid for 64-bit high field");         \
        BUILD_BUG_ON_MSG(((field)&0x6000) == 0x4000,                         \
                         "Natural width accessor invalid for 32-bit field"); \
    })

static __always_inline unsigned long __vmcs_read(unsigned long field) {
    unsigned long value;

    asm volatile("vmread %1, %0"
                 : "=a"(value)
                 : "d"(field)
                 : "cc");
    return value;
}

#define vmcs_read16(field)             \
    ({                                 \
        vmcs_check16(field);           \
        (uint16_t) __vmcs_read(field); \
    })

#define vmcs_read32(field)             \
    ({                                 \
        vmcs_check32(field);           \
        (uint32_t) __vmcs_read(field); \
    })

#define vmcs_read64(field)             \
    ({                                 \
        vmcs_check64(field);           \
        (uint64_t) __vmcs_read(field); \
    })

#define vmcs_readl(field)   \
    ({                      \
        vmcs_checkl(field); \
        __vmcs_read(field); \
    })

static __always_inline void __vmcs_write(unsigned long field,
                                         unsigned long value) {
    uint8_t error;

    asm volatile("vmwrite %1, %2; setna %0"
                 : "=q"(error)
                 : "a"(value), "d"(field)
                 : "cc");
    if (error)
        panic("vmx: vmwrite error: reg %lx value %lx (err %d)\n", field, value,
              vmcs_read32(VM_INSTRUCTION_ERROR));
}

#define vmcs_write16(field, value)  \
    ({                              \
        vmcs_check16(field);        \
        __vmcs_write(field, value); \
    })

#define vmcs_write32(field, value)  \
    ({                              \
        vmcs_check32(field);        \
        __vmcs_write(field, value); \
    })

#define vmcs_write64(field, value)  \
    ({                              \
        vmcs_check64(field);        \
        __vmcs_write(field, value); \
    })

#define vmcs_writel(field, value)   \
    ({                              \
        vmcs_checkl(field);         \
        __vmcs_write(field, value); \
    })

static int vmx_disabled_by_bios(void) {
    uint64_t msr;

    msr = rdmsrl(MSR_IA32_FEATURE_CONTROL);
    if (msr & FEATURE_CONTROL_LOCKED) {
        if (!(msr & FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX) &&
            (msr & FEATURE_CONTROL_VMXON_ENABLED_INSIDE_SMX)) {
            return 1;
        }
        if (!(msr & FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX)) return 1;
    }

    return 0;
}

static int adjust_vmx_controls(uint32_t ctl_min, uint32_t ctl_opt, uint32_t msr,
                               uint32_t *result) {
    uint32_t vmx_msr_low, vmx_msr_high;
    uint32_t ctl = ctl_min | ctl_opt;

    rdmsr(msr, &vmx_msr_low, &vmx_msr_high);

    ctl &= vmx_msr_high; /* bit == 0 in high word ==> must be zero */
    ctl |= vmx_msr_low;  /* bit == 1 in low word  ==> must be one  */

    /* Ensure minimum (required) set of control bits are supported. */
    if (ctl_min & ~ctl) panic("vmx: failed to set minimum control bits\n");

    *result = ctl;
    return 0;
}

static void print_vmx_controls(const char *prefix, uint32_t ctl) {
    uint32_t i;

    pr_info("vmx: %s\n", prefix);
    pr_info("    ");
    for (i = 0; i < 32; ++i) {
        if (ctl & BIT_32(i)) pr_cont(" %d", i);
    }
    pr_cont("\n");
}

static int setup_vmcs_config(struct vmcs_config *vmcs_conf) {
    uint32_t vmx_msr_low, vmx_msr_high;
    uint32_t min, opt, min2, opt2;
    uint32_t _pin_based_exec_control = 0;
    uint32_t _cpu_based_exec_control = 0;
    uint32_t _cpu_based_2nd_exec_control = 0;
    uint32_t _vmexit_control = 0;
    uint32_t _vmentry_control = 0;

    min = 0 | CPU_BASED_USE_MSR_BITMAPS | CPU_BASED_ACTIVATE_SECONDARY_CONTROLS
            | CPU_BASED_RDTSC_EXITING;
    opt = 0;
    adjust_vmx_controls(min, opt, MSR_IA32_VMX_PROCBASED_CTLS,
                        &_cpu_based_exec_control);

    /*
     * "Virtual-interrupt delivery" requires "external-interrupt exiting" and is
     * not set. "Virtualize x2APIC mode" cannot be set together with "virtualize
     * APIC accesses". By default the CPU is in xapic mode and sets "virtualize
     * APIC access".
     */
    min2 = 0 | SECONDARY_EXEC_ENABLE_EPT | SECONDARY_EXEC_ENABLE_VPID |
           SECONDARY_EXEC_UNRESTRICTED_GUEST;
    opt2 = 0 | SECONDARY_EXEC_RDTSCP | SECONDARY_EXEC_ENABLE_INVPCID;
    adjust_vmx_controls(min2, opt2, MSR_IA32_VMX_PROCBASED_CTLS2,
                        &_cpu_based_2nd_exec_control);

    /* CR3 accesses and invlpg don't need to cause VM Exits when EPT enabled */
    _cpu_based_exec_control &=
        ~(CPU_BASED_CR3_LOAD_EXITING | CPU_BASED_CR3_STORE_EXITING |
          CPU_BASED_INVLPG_EXITING);
    rdmsr(MSR_IA32_VMX_EPT_VPID_CAP, &vmx_capability.ept, &vmx_capability.vpid);

    min = 0 | VM_EXIT_SAVE_DEBUG_CONTROLS | VM_EXIT_HOST_ADDR_SPACE_SIZE |
          VM_EXIT_SAVE_IA32_EFER | VM_EXIT_LOAD_IA32_EFER;
    opt = 0 | VM_EXIT_SAVE_IA32_PAT | VM_EXIT_LOAD_IA32_PAT |
          VM_EXIT_CLEAR_BNDCFGS;
    adjust_vmx_controls(min, opt, MSR_IA32_VMX_EXIT_CTLS, &_vmexit_control);

    // min = 0 | PIN_BASED_VMX_PREEMPTION_TIMER;
    // min = 0 | PIN_BASED_EXT_INTR_MASK;
    min = 0;
    opt = 0;
    adjust_vmx_controls(min, opt, MSR_IA32_VMX_PINBASED_CTLS,
                        &_pin_based_exec_control);

    min = 0 | VM_ENTRY_LOAD_DEBUG_CONTROLS | VM_ENTRY_LOAD_IA32_EFER;
    opt = 0 | VM_ENTRY_LOAD_IA32_PAT | VM_ENTRY_LOAD_BNDCFGS;
    adjust_vmx_controls(min, opt, MSR_IA32_VMX_ENTRY_CTLS, &_vmentry_control);

    rdmsr(MSR_IA32_VMX_BASIC, &vmx_msr_low, &vmx_msr_high);

    if ((vmx_msr_high & 0x1fff) > PAGE_SIZE)
        panic("vmx: VMCS size is never greater than 4KB\n");

    if (((vmx_msr_high >> 18) & 15) != 6)
        panic("vmx: require write-back memory type for VMCS accesses\n");

    vmcs_conf->size = vmx_msr_high & 0x1fff;
    vmcs_conf->basic_cap = vmx_msr_high & ~0x1fff;
    vmcs_conf->revision_id = vmx_msr_low;

    vmcs_conf->pin_based_exec_ctrl = _pin_based_exec_control;
    vmcs_conf->cpu_based_exec_ctrl = _cpu_based_exec_control;
    vmcs_conf->cpu_based_2nd_exec_ctrl = _cpu_based_2nd_exec_control;
    vmcs_conf->vmexit_ctrl = _vmexit_control;
    vmcs_conf->vmentry_ctrl = _vmentry_control;

    print_vmx_controls("pin-based VM-execution controls",
                       vmcs_conf->pin_based_exec_ctrl);
    print_vmx_controls("primary processor-based VM-execution controls",
                       vmcs_conf->cpu_based_exec_ctrl);
    print_vmx_controls("secondary processor-based VM-execution controls",
                       vmcs_conf->cpu_based_2nd_exec_ctrl);
    print_vmx_controls("VM-exit controls", vmcs_conf->vmexit_ctrl);
    print_vmx_controls("VM-entry controls", vmcs_conf->vmentry_ctrl);
    return 0;
}

static void __set_msr_interception(unsigned long *msr_bitmap, uint32_t msr,
                                   int read, int write) {
    int f = sizeof(unsigned long);
    unsigned long *tmp;

    /*
     * See Intel PRM Vol. 3, 20.6.9 (MSR-Bitmap Address). Early manuals
     * have the write-low and read-high bitmap offsets the wrong way round.
     * We can control MSRs 0x00000000-0x00001fff and 0xc0000000-0xc0001fff.
     */
    if (msr <= 0x1fff) {
        /* read-low */
        tmp = msr_bitmap + 0x000 / f;
        read ? set_bit(msr, tmp) : clear_bit(msr, tmp);

        /* write-low */
        tmp = msr_bitmap + 0x800 / f;
        write ? set_bit(msr, tmp) : clear_bit(msr, tmp);
    } else if ((msr >= 0xc0000000) && (msr <= 0xc0001fff)) {
        msr &= 0x1fff;

        /* read-high */
        tmp = msr_bitmap + 0x400 / f;
        read ? set_bit(msr, tmp) : clear_bit(msr, tmp);

        /* write-high */
        tmp = msr_bitmap + 0xc00 / f;
        write ? set_bit(msr, tmp) : clear_bit(msr, tmp);
    }
}

static void set_msr_interception(uint32_t msr, int read, int write) {
    __set_msr_interception(msr_bitmap, msr, read, write);
}

static int vmx_hardware_setup(void) {
    uint32_t i;

    setup_vmcs_config(&vmcs_config);

    if (!cpu_has_vmx_ept_2m_page())
        panic("vmx: no support for 2MB EPT pages\n");

    if (!cpu_has_vmx_ept_4levels()) panic("vmx: no support for 4-level EPT\n");

    /*
     * MSR_IA32_APICBASE: disallow write as the VMM will use IPIs.
     */
    set_msr_interception(MSR_IA32_APICBASE, 0, 1);

    /*
     * Intel DM 33.5 suggests a VMM can put a guest in the wait-for-SIPI
     * activity state and use INIT-SIPI-SIPI to wake it up.  However,
     * it's unclear how this work:
     * - Bochs causes a vmexit upon INIT and subsequent vmentries fail.
     * - KVM seems to block INIT/SIPI.
     *
     * To be safe, intercept INIT and SIPI from sender's ICR and emulate
     * the startup algorithm with NMI.
     */
    set_msr_interception(APIC_BASE_MSR + (APIC_ICR >> 4), 0, 1);

    /*
     * MSR_IA32_FEATURE_CONTROL is already locked so it's okay to
     * allow direct access.  Ideally we should hide VMX though.
     */

    /* allow access to MSR_TSC_AUX as it is not used */

    /* disallow access to VMX-related MSRs */
    for (i = MSR_IA32_VMX_BASIC; i <= MSR_IA32_VMX_VMFUNC; ++i)
        set_msr_interception(i, 1, 1);

    return 0;
}

static void kvm_cpu_vmxon(uint64_t addr) {
    asm volatile("vmxon %0"
                 :
                 : "m"(addr)
                 : "memory", "cc");
}

static void vmcs_load(uint64_t addr) {
    uint8_t error;

    asm volatile("vmptrld %1; setna %0"
                 : "=qm"(error)
                 : "m"(addr)
                 : "cc", "memory");
    if (error) panic("vmx: vmptrld %" PRIx64 "failed\n", addr);
}

static struct kvm_vcpu *vmx_vcpu_enable(void) {
    struct vmcs *vmxon, *vmcs;
    uint64_t old, test_bits;

    if (read_cr4() & X86_CR4_VMXE) panic("vmx: VMXE already set in CR4\n");

    old = rdmsrl(MSR_IA32_FEATURE_CONTROL);

    test_bits = FEATURE_CONTROL_LOCKED;
    test_bits |= FEATURE_CONTROL_VMXON_ENABLED_OUTSIDE_SMX;

    /* enable and lock */
    if ((old & test_bits) != test_bits)
        wrmsrl(MSR_IA32_FEATURE_CONTROL, old | test_bits);
    cr4_set_bits(X86_CR4_VMXE);

    vmxon = this_cpu_ptr(&vmxarea);
    vmxon->revision_id = vmcs_config.revision_id;
    kvm_cpu_vmxon(__pa(vmxon));

    vmcs = this_cpu_ptr(&current_vmcs);
    vmcs->revision_id = vmcs_config.revision_id;
    vmcs_load(__pa(vmcs));

    return &this_cpu_ptr(&current_vmx)->vcpu;
}

/*
 * Consistency requirements:
 * - EFER.LMA == VM-entry control's 64-bit guest mode bit
 * - EFER.LMA == CR0.PG & EFER.LME
 */

static void enter_lmode(struct kvm_vcpu *vcpu) {
    uint32_t vmentry_ctrl;
    uint64_t efer;
    uint32_t guest_tr_ar;

    vmentry_ctrl = vmcs_read32(VM_ENTRY_CONTROLS);
    vmcs_write32(VM_ENTRY_CONTROLS, vmentry_ctrl | VM_ENTRY_IA32E_MODE);

    efer = vmcs_read64(GUEST_IA32_EFER);
    vmcs_write64(GUEST_IA32_EFER, efer | EFER_LMA);

    /* tss fixup for long mode */
    guest_tr_ar = vmcs_read32(GUEST_TR_AR_BYTES);
    if ((guest_tr_ar & VMX_AR_TYPE_MASK) != VMX_AR_TYPE_BUSY_64_TSS) {
        vmcs_write32(GUEST_TR_AR_BYTES, (guest_tr_ar & ~VMX_AR_TYPE_MASK) |
                                            VMX_AR_TYPE_BUSY_64_TSS);
    }
}

static void exit_lmode(struct kvm_vcpu *vcpu) {
    uint32_t vmentry_ctrl;
    uint64_t efer;

    vmentry_ctrl = vmcs_read32(VM_ENTRY_CONTROLS);
    vmcs_write32(VM_ENTRY_CONTROLS, vmentry_ctrl & ~VM_ENTRY_IA32E_MODE);

    efer = vmcs_read64(GUEST_IA32_EFER);
    vmcs_write64(GUEST_IA32_EFER, efer & ~EFER_LMA);
}

static void vmx_set_cr0(struct kvm_vcpu *vcpu, unsigned long cr0) {
    if (vmcs_read64(GUEST_IA32_EFER) & EFER_LME) {
        unsigned long old_cr0 = vmcs_readl(GUEST_CR0);

        if (!(old_cr0 & X86_CR0_PG) && (cr0 & X86_CR0_PG)) enter_lmode(vcpu);
        if ((old_cr0 & X86_CR0_PG) && !(cr0 & X86_CR0_PG)) exit_lmode(vcpu);
    }

    vmcs_writel(GUEST_CR0, cr0 | KVM_GUEST_CR0_ALWAYS_ON);
}

static void vmx_set_cr4(struct kvm_vcpu *vcpu, unsigned long cr4) {
    vmcs_writel(GUEST_CR4, cr4 | KVM_GUEST_CR4_ALWAYS_ON);
}

static inline unsigned long get_tr_base(void) {
    struct desc_ptr dt;
    struct segment_desc *gdt;
    struct tss_desc *desc;

    store_gdt(&dt);
    gdt = (void *)dt.address;
    desc = (void *)&gdt[store_tr() / 8];
    BUG_ON(desc->p != 1);
    return ((unsigned long)desc->base0) | ((unsigned long)desc->base1 << 16) |
           ((unsigned long)desc->base2 << 24) |
           ((unsigned long)desc->base3 << 32);
}

static void vmx_vcpu_setup(struct kvm_vcpu *vcpu) {
    struct vcpu_vmx *vmx = to_vmx(vcpu);
    struct desc_ptr dt;
    size_t i, nmsrs;

    /* I/O: pass through */

    /* MSR */
    vmcs_write64(MSR_BITMAP, __pa(msr_bitmap));

    vmcs_write64(VMCS_LINK_POINTER, ~UINT64_C(0));

    /* Control */
    vmcs_write32(PIN_BASED_VM_EXEC_CONTROL, vmcs_config.pin_based_exec_ctrl);
    vmcs_write32(CPU_BASED_VM_EXEC_CONTROL, vmcs_config.cpu_based_exec_ctrl);
    vmcs_write32(SECONDARY_VM_EXEC_CONTROL,
                 vmcs_config.cpu_based_2nd_exec_ctrl);
    vmcs_write32(VM_EXIT_CONTROLS, vmcs_config.vmexit_ctrl);
    vmcs_write32(VM_ENTRY_CONTROLS, vmcs_config.vmentry_ctrl);

    vmcs_write32(PAGE_FAULT_ERROR_CODE_MASK, 0);
    vmcs_write32(PAGE_FAULT_ERROR_CODE_MATCH, 0);
    vmcs_write32(CR3_TARGET_COUNT, 0);

    BUILD_BUG_ON(ARRAY_SIZE(vmx_msr_index) > NR_AUTOLOAD_MSRS);
    nmsrs = ARRAY_SIZE(vmx_msr_index);
    vmcs_write32(VM_EXIT_MSR_STORE_COUNT, nmsrs);
    vmcs_write32(VM_EXIT_MSR_STORE_ADDR, __pa(vmx->msr_autoload.guest));
    vmcs_write32(VM_EXIT_MSR_LOAD_COUNT, nmsrs);
    vmcs_write64(VM_EXIT_MSR_LOAD_ADDR, __pa(vmx->msr_autoload.host));
    vmcs_write32(VM_ENTRY_MSR_LOAD_COUNT, nmsrs);
    vmcs_write64(VM_ENTRY_MSR_LOAD_ADDR, __pa(vmx->msr_autoload.guest));

    for (i = 0; i < nmsrs; ++i) {
        uint32_t index = vmx_msr_index[i];

        vmx->msr_autoload.guest[i].index = index;
        vmx->msr_autoload.host[i].index = index;
        vmx->msr_autoload.host[i].value = rdmsrl(index);
    }

    /* Host */
    vmcs_write16(HOST_FS_SELECTOR, 0);
    vmcs_write16(HOST_GS_SELECTOR, 0);
    vmcs_writel(HOST_FS_BASE, rdmsrl(MSR_FS_BASE));
    vmcs_writel(HOST_GS_BASE, rdmsrl(MSR_GS_BASE));

    vmcs_writel(HOST_CR0, read_cr0());
    vmcs_writel(HOST_CR3, read_cr3());
    vmcs_writel(HOST_CR4, read_cr4());
    vmcs_write64(HOST_IA32_EFER, rdmsrl(MSR_EFER));

    vmcs_write16(HOST_CS_SELECTOR, KERNEL_CS);
    vmcs_write16(HOST_DS_SELECTOR, KERNEL_DS);
    vmcs_write16(HOST_ES_SELECTOR, KERNEL_DS);
    vmcs_write16(HOST_SS_SELECTOR, KERNEL_DS);
    vmcs_write16(HOST_TR_SELECTOR, store_tr());
    BUG_ON(store_tr() != GDT_ENTRY_TSS * 8);

    store_idt(&dt);
    vmcs_writel(HOST_IDTR_BASE, dt.address);

    /* set per-cpu host GDT && TSS */
    store_gdt(&dt);
    vmcs_writel(HOST_GDTR_BASE, dt.address);
    vmcs_writel(HOST_TR_BASE, get_tr_base());

    vmcs_writel(HOST_RIP, vmx_return);

    vmcs_write32(HOST_IA32_SYSENTER_CS, (uint32_t)rdmsrl(MSR_IA32_SYSENTER_CS));
    vmcs_writel(HOST_IA32_SYSENTER_EIP, rdmsrl(MSR_IA32_SYSENTER_EIP));

    if (vmcs_config.vmexit_ctrl & VM_EXIT_LOAD_IA32_PAT)
        vmcs_write64(HOST_IA32_PAT, rdmsrl(MSR_IA32_CR_PAT));

    /* Guest */
    vmcs_write16(GUEST_CS_SELECTOR, 0xf000);
    vmcs_writel(GUEST_CS_BASE, 0xffff0000ul);
    vmcs_write32(GUEST_CS_LIMIT, 0xffff);
    vmcs_write32(GUEST_CS_AR_BYTES, 0x93 | 0x08);

    vmcs_write16(GUEST_DS_SELECTOR, 0);
    vmcs_writel(GUEST_DS_BASE, 0);
    vmcs_write32(GUEST_DS_LIMIT, 0xffff);
    vmcs_write32(GUEST_DS_AR_BYTES, 0x93);

    vmcs_write16(GUEST_ES_SELECTOR, 0);
    vmcs_writel(GUEST_ES_BASE, 0);
    vmcs_write32(GUEST_ES_LIMIT, 0xffff);
    vmcs_write32(GUEST_ES_AR_BYTES, 0x93);

    vmcs_write16(GUEST_SS_SELECTOR, 0);
    vmcs_writel(GUEST_SS_BASE, 0);
    vmcs_write32(GUEST_SS_LIMIT, 0xffff);
    vmcs_write32(GUEST_SS_AR_BYTES, 0x93);

    vmcs_write16(GUEST_FS_SELECTOR, 0);
    vmcs_writel(GUEST_FS_BASE, 0);
    vmcs_write32(GUEST_FS_LIMIT, 0xffff);
    vmcs_write32(GUEST_FS_AR_BYTES, 0x93);

    vmcs_write16(GUEST_GS_SELECTOR, 0);
    vmcs_writel(GUEST_GS_BASE, 0);
    vmcs_write32(GUEST_GS_LIMIT, 0xffff);
    vmcs_write32(GUEST_GS_AR_BYTES, 0x93);

    vmcs_write16(GUEST_TR_SELECTOR, 0);
    vmcs_writel(GUEST_TR_BASE, 0);
    vmcs_write32(GUEST_TR_LIMIT, 0xffff);
    /* both Boch and QEMU set this to busy TSS32 rather than TSS16 */
    vmcs_write32(GUEST_TR_AR_BYTES, 0x008b);

    vmcs_write16(GUEST_LDTR_SELECTOR, 0);
    vmcs_writel(GUEST_LDTR_BASE, 0);
    vmcs_write32(GUEST_LDTR_LIMIT, 0xffff);
    vmcs_write32(GUEST_LDTR_AR_BYTES, 0x00082);

    vmcs_write32(GUEST_SYSENTER_CS, 0);
    vmcs_writel(GUEST_SYSENTER_ESP, 0);
    vmcs_writel(GUEST_SYSENTER_EIP, 0);
    vmcs_write64(GUEST_IA32_DEBUGCTL, 0);

    vmcs_writel(GUEST_RFLAGS, 0x02);
    vmcs_writel(GUEST_RIP, 0xfff0);

    vmcs_writel(GUEST_GDTR_BASE, 0);
    vmcs_write32(GUEST_GDTR_LIMIT, 0xffff);

    vmcs_writel(GUEST_IDTR_BASE, 0);
    vmcs_write32(GUEST_IDTR_LIMIT, 0xffff);

    vmcs_write32(GUEST_ACTIVITY_STATE, GUEST_ACTIVITY_ACTIVE);
    vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, 0);
    vmcs_writel(GUEST_PENDING_DBG_EXCEPTIONS, 0);

    /* set preemption timer value */
    vmcs_write32(VMX_PREEMPTION_TIMER_VALUE, 0x3000000);

    vmcs_write32(VM_ENTRY_INTR_INFO_FIELD, 0);

    vmcs_write16(VIRTUAL_PROCESSOR_ID, 1);

    /* initial CR0: NW and CD are set; ET is hard-wired to be 1 */
    vmcs_writel(CR0_GUEST_HOST_MASK, KVM_GUEST_CR0_ALWAYS_ON);
    vmcs_writel(CR0_READ_SHADOW, KVM_GUEST_CR0_ALWAYS_ON);
    vmx_set_cr0(vcpu, X86_CR0_NW | X86_CR0_CD | X86_CR0_ET);

    vmcs_writel(GUEST_CR3, 0);

    vmcs_writel(CR4_GUEST_HOST_MASK, KVM_GUEST_CR4_ALWAYS_ON);
    vmcs_writel(CR4_READ_SHADOW, KVM_GUEST_CR4_ALWAYS_ON);
    vmx_set_cr4(vcpu, 0);
}

static void vmx_set_tdp(struct kvm_vcpu *vcpu, unsigned long tdp) {
    vmcs_write64(EPT_POINTER,
                 tdp | VMX_EPT_DEFAULT_MT |
                     (VMX_EPT_DEFAULT_GAW << VMX_EPT_GAW_EPTP_SHIFT));
}

static bool is_real_mode(struct kvm_vcpu *vcpu) {
    return !(vmcs_readl(GUEST_CR0) & X86_CR0_PE);
}

static int vmx_get_cpl(struct kvm_vcpu *vcpu) {
    /* real mode */
    if (is_real_mode(vcpu)) return 0;

    return VMX_AR_DPL(vmcs_read32(GUEST_SS_AR_BYTES));
}

static void vmx_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var,
                            int seg) {
    uint32_t ar;

    var->base = __vmcs_read(kvm_vmx_segment_fields[seg].base);
    var->limit = __vmcs_read(kvm_vmx_segment_fields[seg].limit);
    var->selector = __vmcs_read(kvm_vmx_segment_fields[seg].selector);
    ar = __vmcs_read(kvm_vmx_segment_fields[seg].ar_bytes);
    var->unusable = (ar >> 16) & 1;
    var->type = ar & 15;
    var->s = (ar >> 4) & 1;
    var->dpl = (ar >> 5) & 3;
    var->present = !var->unusable;
    var->avl = (ar >> 12) & 1;
    var->l = (ar >> 13) & 1;
    var->db = (ar >> 14) & 1;
    var->g = (ar >> 15) & 1;
}

static uint32_t vmx_segment_access_rights(struct kvm_segment *var) {
    uint32_t ar;

    if (var->unusable || !var->present)
        ar = 1 << 16;
    else {
        ar = var->type & 15;
        ar |= (var->s & 1) << 4;
        ar |= (var->dpl & 3) << 5;
        ar |= (var->present & 1) << 7;
        ar |= (var->avl & 1) << 12;
        ar |= (var->l & 1) << 13;
        ar |= (var->db & 1) << 14;
        ar |= (var->g & 1) << 15;
    }

    return ar;
}

static void vmx_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var,
                            int seg) {
    const struct kvm_vmx_segment_field *sf = &kvm_vmx_segment_fields[seg];

    __vmcs_write(sf->base, var->base);
    __vmcs_write(sf->limit, var->limit);
    __vmcs_write(sf->selector, var->selector);
    __vmcs_write(sf->ar_bytes, vmx_segment_access_rights(var));
}

static unsigned long vmx_get_rflags(struct kvm_vcpu *vcpu) {
    return vmcs_readl(GUEST_RFLAGS);
}

static void vmx_set_rflags(struct kvm_vcpu *vcpu, unsigned long rflags) {
    vmcs_writel(GUEST_RFLAGS, rflags);
}

static unsigned long vmx_get_rip(struct kvm_vcpu *vcpu) {
    return vmcs_readl(GUEST_RIP);
}

static void vmx_set_rip(struct kvm_vcpu *vcpu, unsigned long rip) {
    vmcs_writel(GUEST_RIP, rip);
}

static void vmx_vcpu_run(struct kvm_vcpu *vcpu) {
    struct vcpu_vmx *vmx = to_vmx(vcpu);

    asm volatile(
        /* Store host registers */
        "push %%rdx; push %%rbp;"
        "push %%rcx \n\t" /* placeholder for guest rcx */
        "push %%rcx \n\t"
        "cmp %%rsp, %c[host_rsp](%0) \n\t"
        "je 1f \n\t"
        "mov %%rsp, %c[host_rsp](%0) \n\t"
        "vmwrite %%rsp, %%rdx \n\t"
        "1: \n\t"
        /* Reload cr2 if changed */
        "mov %c[cr2](%0), %%rax \n\t"
        "mov %%cr2, %%rdx \n\t"
        "cmp %%rax, %%rdx \n\t"
        "je 2f \n\t"
        "mov %%rax, %%cr2 \n\t"
        "2: \n\t"
        /* Check if vmlaunch of vmresume is needed */
        "cmpl $0, %c[launched](%0) \n\t"
        /* Load guest registers.  Don't clobber flags. */
        "mov %c[rax](%0), %%rax \n\t"
        "mov %c[rbx](%0), %%rbx \n\t"
        "mov %c[rdx](%0), %%rdx \n\t"
        "mov %c[rsi](%0), %%rsi \n\t"
        "mov %c[rdi](%0), %%rdi \n\t"
        "mov %c[rbp](%0), %%rbp \n\t"
        "mov %c[r8](%0),  %%r8  \n\t"
        "mov %c[r9](%0),  %%r9  \n\t"
        "mov %c[r10](%0), %%r10 \n\t"
        "mov %c[r11](%0), %%r11 \n\t"
        "mov %c[r12](%0), %%r12 \n\t"
        "mov %c[r13](%0), %%r13 \n\t"
        "mov %c[r14](%0), %%r14 \n\t"
        "mov %c[r15](%0), %%r15 \n\t"
        "mov %c[rcx](%0), %%rcx \n\t" /* kills %0 (ecx) */

        /* Enter guest mode */
        "jne 1f \n\t"
        "vmlaunch \n\t"
        "jmp 2f \n\t"
        "1: vmresume \n\t"
        "2: "
        /* Save guest registers, load host registers, keep flags */
        "mov %0, 8(%%rsp) \n\t"
        "pop %0 \n\t"
        "mov %%rax, %c[rax](%0) \n\t"
        "mov %%rbx, %c[rbx](%0) \n\t"
        "popq %c[rcx](%0) \n\t"
        "mov %%rdx, %c[rdx](%0) \n\t"
        "mov %%rsi, %c[rsi](%0) \n\t"
        "mov %%rdi, %c[rdi](%0) \n\t"
        "mov %%rbp, %c[rbp](%0) \n\t"
        "mov %%r8,  %c[r8](%0) \n\t"
        "mov %%r9,  %c[r9](%0) \n\t"
        "mov %%r10, %c[r10](%0) \n\t"
        "mov %%r11, %c[r11](%0) \n\t"
        "mov %%r12, %c[r12](%0) \n\t"
        "mov %%r13, %c[r13](%0) \n\t"
        "mov %%r14, %c[r14](%0) \n\t"
        "mov %%r15, %c[r15](%0) \n\t"

        "mov %%cr2, %%rax   \n\t"
        "mov %%rax, %c[cr2](%0) \n\t"

        "pop %%rbp; pop %%rdx \n\t"
        "setbe %c[fail](%0) \n\t"
        ".pushsection .rodata \n\t"
        ".global vmx_return \n\t"
        "vmx_return: .quad 2b \n\t"
        ".popsection"
        :
        : "c"(vmx), "d"((unsigned long)HOST_RSP),
          [launched] "i"(offsetof(struct vcpu_vmx, launched)),
          [fail] "i"(offsetof(struct vcpu_vmx, fail)),
          [host_rsp] "i"(offsetof(struct vcpu_vmx, host_rsp)),
          [rax] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RAX])),
          [rbx] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RBX])),
          [rcx] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RCX])),
          [rdx] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RDX])),
          [rsi] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RSI])),
          [rdi] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RDI])),
          [rbp] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_RBP])),
          [r8] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R8])),
          [r9] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R9])),
          [r10] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R10])),
          [r11] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R11])),
          [r12] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R12])),
          [r13] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R13])),
          [r14] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R14])),
          [r15] "i"(offsetof(struct vcpu_vmx, vcpu.regs[VCPU_REGS_R15])),
          [cr2] "i"(offsetof(struct vcpu_vmx, vcpu.cr2))
        : "cc", "memory", "rax", "rbx", "rdi", "rsi", "r8", "r9", "r10", "r11",
          "r12", "r13", "r14", "r15");

    vmx->launched = 1;
}

static void vmx_dump_sel(char *name, uint32_t sel) {
    pr_err("%s sel=0x%04lx, attr=0x%05lx, limit=0x%08lx, base=0x%016lx\n", name,
           __vmcs_read(sel),
           __vmcs_read(sel + GUEST_ES_AR_BYTES - GUEST_ES_SELECTOR),
           __vmcs_read(sel + GUEST_ES_LIMIT - GUEST_ES_SELECTOR),
           __vmcs_read(sel + GUEST_ES_BASE - GUEST_ES_SELECTOR));
}

static void vmx_dump_dtsel(char *name, uint32_t limit) {
    pr_err("%s                           limit=0x%08lx, base=0x%016lx\n", name,
           __vmcs_read(limit),
           __vmcs_read(limit + GUEST_GDTR_BASE - GUEST_GDTR_LIMIT));
}

static void dump_vmcs(struct kvm_vcpu *vcpu) {
    uint32_t vmentry_ctl = vmcs_read32(VM_ENTRY_CONTROLS);
    uint32_t vmexit_ctl = vmcs_read32(VM_EXIT_CONTROLS);
    uint32_t cpu_based_exec_ctrl = vmcs_read32(CPU_BASED_VM_EXEC_CONTROL);
    uint32_t pin_based_exec_ctrl = vmcs_read32(PIN_BASED_VM_EXEC_CONTROL);
    uint32_t secondary_exec_control = vmcs_read32(SECONDARY_VM_EXEC_CONTROL);
    unsigned long cr4 = vmcs_readl(GUEST_CR4);
    uint64_t efer = vmcs_read64(GUEST_IA32_EFER);
    int i, n;

    pr_err("*** Guest State ***\n");
    pr_err("CR0: actual=0x%016lx, shadow=0x%016lx, gh_mask=%016lx\n",
           vmcs_readl(GUEST_CR0), vmcs_readl(CR0_READ_SHADOW),
           vmcs_readl(CR0_GUEST_HOST_MASK));
    pr_err("CR4: actual=0x%016lx, shadow=0x%016lx, gh_mask=%016lx\n", cr4,
           vmcs_readl(CR4_READ_SHADOW), vmcs_readl(CR4_GUEST_HOST_MASK));
    pr_err("CR3 = 0x%016lx\n", vmcs_readl(GUEST_CR3));
    if ((secondary_exec_control & SECONDARY_EXEC_ENABLE_EPT) &&
        (cr4 & X86_CR4_PAE) && !(efer & EFER_LMA)) {
        pr_err("PDPTR0 = 0x%016" PRIx64 "  PDPTR1 = 0x%016" PRIx64 "\n",
               vmcs_read64(GUEST_PDPTR0), vmcs_read64(GUEST_PDPTR1));
        pr_err("PDPTR2 = 0x%016" PRIx64 "  PDPTR3 = 0x%016" PRIx64 "\n",
               vmcs_read64(GUEST_PDPTR2), vmcs_read64(GUEST_PDPTR3));
    }
    pr_err("RSP = 0x%016lx  RIP = 0x%016lx\n", vmcs_readl(GUEST_RSP),
           vmcs_readl(GUEST_RIP));
    pr_err("RFLAGS=0x%08lx         DR7 = 0x%016lx\n", vmcs_readl(GUEST_RFLAGS),
           vmcs_readl(GUEST_DR7));
    pr_err("Sysenter RSP=%016lx CS:RIP=%04x:%016lx\n",
           vmcs_readl(GUEST_SYSENTER_ESP), vmcs_read32(GUEST_SYSENTER_CS),
           vmcs_readl(GUEST_SYSENTER_EIP));
    vmx_dump_sel("CS:  ", GUEST_CS_SELECTOR);
    vmx_dump_sel("DS:  ", GUEST_DS_SELECTOR);
    vmx_dump_sel("SS:  ", GUEST_SS_SELECTOR);
    vmx_dump_sel("ES:  ", GUEST_ES_SELECTOR);
    vmx_dump_sel("FS:  ", GUEST_FS_SELECTOR);
    vmx_dump_sel("GS:  ", GUEST_GS_SELECTOR);
    vmx_dump_dtsel("GDTR:", GUEST_GDTR_LIMIT);
    vmx_dump_sel("LDTR:", GUEST_LDTR_SELECTOR);
    vmx_dump_dtsel("IDTR:", GUEST_IDTR_LIMIT);
    vmx_dump_sel("TR:  ", GUEST_TR_SELECTOR);
    if ((vmexit_ctl & (VM_EXIT_SAVE_IA32_PAT | VM_EXIT_SAVE_IA32_EFER)) ||
        (vmentry_ctl & (VM_ENTRY_LOAD_IA32_PAT | VM_ENTRY_LOAD_IA32_EFER)))
        pr_err("EFER =     0x%016" PRIx64 "  PAT = 0x%016" PRIx64 "\n", efer,
               vmcs_read64(GUEST_IA32_PAT));
    pr_err("DebugCtl = 0x%016" PRIx64 "  DebugExceptions = 0x%016lx\n",
           vmcs_read64(GUEST_IA32_DEBUGCTL),
           vmcs_readl(GUEST_PENDING_DBG_EXCEPTIONS));
    if (vmentry_ctl & VM_ENTRY_LOAD_IA32_PERF_GLOBAL_CTRL)
        pr_err("PerfGlobCtl = 0x%016" PRIx64 "\n",
               vmcs_read64(GUEST_IA32_PERF_GLOBAL_CTRL));
    if (vmentry_ctl & VM_ENTRY_LOAD_BNDCFGS)
        pr_err("BndCfgS = 0x%016" PRIx64 "\n", vmcs_read64(GUEST_BNDCFGS));
    pr_err("Interruptibility = %08x  ActivityState = %08x\n",
           vmcs_read32(GUEST_INTERRUPTIBILITY_INFO),
           vmcs_read32(GUEST_ACTIVITY_STATE));
    if (secondary_exec_control & SECONDARY_EXEC_VIRTUAL_INTR_DELIVERY)
        pr_err("InterruptStatus = %04x\n", vmcs_read16(GUEST_INTR_STATUS));

    pr_err("*** Host State ***\n");
    pr_err("RIP = 0x%016lx  RSP = 0x%016lx\n", vmcs_readl(HOST_RIP),
           vmcs_readl(HOST_RSP));
    pr_err("CS=%04x SS=%04x DS=%04x ES=%04x FS=%04x GS=%04x TR=%04x\n",
           vmcs_read16(HOST_CS_SELECTOR), vmcs_read16(HOST_SS_SELECTOR),
           vmcs_read16(HOST_DS_SELECTOR), vmcs_read16(HOST_ES_SELECTOR),
           vmcs_read16(HOST_FS_SELECTOR), vmcs_read16(HOST_GS_SELECTOR),
           vmcs_read16(HOST_TR_SELECTOR));
    pr_err("FSBase=%016lx GSBase=%016lx TRBase=%016lx\n",
           vmcs_readl(HOST_FS_BASE), vmcs_readl(HOST_GS_BASE),
           vmcs_readl(HOST_TR_BASE));
    pr_err("GDTBase=%016lx IDTBase=%016lx\n", vmcs_readl(HOST_GDTR_BASE),
           vmcs_readl(HOST_IDTR_BASE));
    pr_err("CR0=%016lx CR3=%016lx CR4=%016lx\n", vmcs_readl(HOST_CR0),
           vmcs_readl(HOST_CR3), vmcs_readl(HOST_CR4));
    pr_err("Sysenter RSP=%016lx CS:RIP=%04x:%016lx\n",
           vmcs_readl(HOST_IA32_SYSENTER_ESP),
           vmcs_read32(HOST_IA32_SYSENTER_CS),
           vmcs_readl(HOST_IA32_SYSENTER_EIP));
    if (vmexit_ctl & (VM_EXIT_LOAD_IA32_PAT | VM_EXIT_LOAD_IA32_EFER))
        pr_err("EFER = 0x%016" PRIx64 "  PAT = 0x%016" PRIx64 "\n",
               vmcs_read64(HOST_IA32_EFER), vmcs_read64(HOST_IA32_PAT));
    if (vmexit_ctl & VM_EXIT_LOAD_IA32_PERF_GLOBAL_CTRL)
        pr_err("PerfGlobCtl = 0x%016" PRIx64 "\n",
               vmcs_read64(HOST_IA32_PERF_GLOBAL_CTRL));

    pr_err("*** Control State ***\n");
    pr_err("PinBased=%08x CPUBased=%08x SecondaryExec=%08x\n",
           pin_based_exec_ctrl, cpu_based_exec_ctrl, secondary_exec_control);
    pr_err("EntryControls=%08x ExitControls=%08x\n", vmentry_ctl, vmexit_ctl);
    pr_err("ExceptionBitmap=%08x PFECmask=%08x PFECmatch=%08x\n",
           vmcs_read32(EXCEPTION_BITMAP),
           vmcs_read32(PAGE_FAULT_ERROR_CODE_MASK),
           vmcs_read32(PAGE_FAULT_ERROR_CODE_MATCH));
    pr_err("VMEntry: intr_info=%08x errcode=%08x ilen=%08x\n",
           vmcs_read32(VM_ENTRY_INTR_INFO_FIELD),
           vmcs_read32(VM_ENTRY_EXCEPTION_ERROR_CODE),
           vmcs_read32(VM_ENTRY_INSTRUCTION_LEN));
    pr_err("VMExit: intr_info=%08x errcode=%08x ilen=%08x\n",
           vmcs_read32(VM_EXIT_INTR_INFO), vmcs_read32(VM_EXIT_INTR_ERROR_CODE),
           vmcs_read32(VM_EXIT_INSTRUCTION_LEN));
    pr_err("        reason=%08x qualification=%016lx\n",
           vmcs_read32(VM_EXIT_REASON), vmcs_readl(EXIT_QUALIFICATION));
    pr_err("IDTVectoring: info=%08x errcode=%08x\n",
           vmcs_read32(IDT_VECTORING_INFO_FIELD),
           vmcs_read32(IDT_VECTORING_ERROR_CODE));
    pr_err("TSC Offset = 0x%016" PRIx64 "\n", vmcs_read64(TSC_OFFSET));
    if (secondary_exec_control & SECONDARY_EXEC_TSC_SCALING)
        pr_err("TSC Multiplier = 0x%016" PRIx64 "\n",
               vmcs_read64(TSC_MULTIPLIER));
    if (cpu_based_exec_ctrl & CPU_BASED_TPR_SHADOW)
        pr_err("TPR Threshold = 0x%02x\n", vmcs_read32(TPR_THRESHOLD));
    if (pin_based_exec_ctrl & PIN_BASED_POSTED_INTR)
        pr_err("PostedIntrVec = 0x%02x\n", vmcs_read16(POSTED_INTR_NV));
    if ((secondary_exec_control & SECONDARY_EXEC_ENABLE_EPT))
        pr_err("EPT pointer = 0x%016" PRIx64 "\n", vmcs_read64(EPT_POINTER));
    n = vmcs_read32(CR3_TARGET_COUNT);
    for (i = 0; i + 1 < n; i += 4)
        pr_err("CR3 target%u=%016lx target%u=%016lx\n", i,
               __vmcs_read(CR3_TARGET_VALUE0 + i * 2), i + 1,
               __vmcs_read(CR3_TARGET_VALUE0 + i * 2 + 2));
    if (i < n)
        pr_err("CR3 target%u=%016lx\n", i,
               __vmcs_read(CR3_TARGET_VALUE0 + i * 2));
    if (secondary_exec_control & SECONDARY_EXEC_PAUSE_LOOP_EXITING)
        pr_err("PLE Gap=%08x Window=%08x\n", vmcs_read32(PLE_GAP),
               vmcs_read32(PLE_WINDOW));
    if (secondary_exec_control & SECONDARY_EXEC_ENABLE_VPID)
        pr_err("Virtual processor ID = 0x%04x\n",
               vmcs_read16(VIRTUAL_PROCESSOR_ID));
}

static void handle_cr(struct kvm_vcpu *vcpu) {
    unsigned long exit_qualification, val;
    int cr, reg, op;

    exit_qualification = vmcs_readl(EXIT_QUALIFICATION);
    cr = exit_qualification & 15;
    reg = (exit_qualification >> 8) & 15;
    op = (exit_qualification >> 4) & 3;

    switch (op) {
        case 0:
            /* mov to cr */
            val = kvm_register_read(vcpu, reg);
            switch (cr) {
                case 0:
                    vmx_set_cr0(vcpu, val);
                    return kvm_skip_emulated_instruction(vcpu);
                case 4:
                    /*
                     * A "faithful" VMM should raise #GP if guest tries to set
                     * CR4.VMXE, assuming no nested virtualization.
                     */
                    vmx_set_cr4(vcpu, val);
                    return kvm_skip_emulated_instruction(vcpu);
                default:
                    panic("unknown control register\n");
            }
            break;
        default:
            break;
    }
    panic("unhandled control register: op %d cr %d\n", op, cr);
}

static void handle_rdmsr(struct kvm_vcpu *vcpu) {
    uint32_t msr = kvm_register_read(vcpu, VCPU_REGS_RCX);

    dump_vmcs(vcpu);
    panic("unknown rdmsr 0x%08x\n", msr);
}

static void handle_wrmsr(struct kvm_vcpu *vcpu) {
    uint32_t msr;
    uint64_t val;

    msr = kvm_register_read(vcpu, VCPU_REGS_RCX);
    val = kvm_read_edx_eax(vcpu);

    switch (msr) {
        case MSR_IA32_APICBASE:
            BUG_ON(!(val & MSR_IA32_APICBASE_ENABLE));
            if (val & MSR_IA32_APICBASE_X2APIC_ENABLE)
                pr_info(
                    "vmx: x2apic enabled by guest on cpu %d acpi_id[0x%02x]\n",
                    smp_processor_id(), read_apic_id());
            wrmsrl(msr, val);
            break;
        case APIC_BASE_MSR + (APIC_ICR >> 4):
            switch (val & 0x00700) {
                case APIC_DM_INIT:
                    /* silently drop INIT */
                    break;
                case APIC_DM_STARTUP: {
                    /* silently drop AP startup */
                    break;
                }
                default:
                    /* pass-through by default */
                    wrmsrl(msr, val);
                    break;
            }
            break;
        default:
            dump_vmcs(vcpu);
            panic("unknown wrmsr 0x%08x\n", msr);
    }

    return kvm_skip_emulated_instruction(vcpu);
}

static void handle_xsetbv(struct kvm_vcpu *vcpu) {
    uint32_t eax, edx;
    uint64_t index;

    eax = kvm_register_read(vcpu, VCPU_REGS_RAX);
    edx = kvm_register_read(vcpu, VCPU_REGS_RDX);
	index = kvm_register_read(vcpu, VCPU_REGS_RCX);

    asm volatile("xsetbv" /* xsetbv */
		     : : "a" (eax), "d" (edx), "c" (index));
}

static void handle_rdtsc(struct kvm_vcpu *vcpu) {
    uint64_t val = rdtsc();

    kvm_write_edx_eax(vcpu, val);
    return kvm_skip_emulated_instruction(vcpu);
}

static void handle_preemption_timer(struct kvm_vcpu *vcpu) {
    // if (!to_vmx(vcpu)->req_immediate_exit)
	   //  kvm_lapic_expired_hv_timer(vcpu);
    // return;

}

static uintptr_t machine_addr(uint64_t gpa) {
    uint32_t pml4i, pdpti, pdi;
    uint64_t ept_ptr, hpa, pdpt_pa, page_2m, pd_pa;
    pml4i = pml4_index(gpa);
    pdpti = pdpt_index(gpa);
    pdi = pd_index(gpa);
    ept_ptr = vmcs_read64(EPT_POINTER) & ~0x7ff;
    // pr_err("pml4i: %d, pdpti: %d, pdi: %d\n", pml4i, pdpti, pdi);

    pdpt_pa = ((uint64_t *)__va(ept_ptr))[pml4i] & ~0x7ff;
    // pr_err("ept_ptr: 0x%lx, pdpt_pa: 0x%lx\n", ept_ptr, pdpt_pa);
    pd_pa = ((uint64_t *)__va(pdpt_pa))[pdpti] & ~0x7ff;
    // pr_err("pd_pa: %lx\n", pd_pa);
    page_2m = ((uint64_t *)__va(pd_pa))[pdi] & ~0x7ff;

    hpa = (uintptr_t)__va(page_2m) | (gpa & 0x1fffff);
    pr_err("page_2m: 0x%lx, hpa: 0x%lx\n", page_2m, hpa);

    // pr_err("ept_ptr: 0x%lx, pdpt_pa: 0x%lx, pd_pa: %lx, hpa: %lx\n", ept_ptr, pdpt_pa, pd_pa, hpa);
    return (uintptr_t)__va(hpa);
}

static void handle_vmcall(struct kvm_vcpu *vcpu) {
    /* This is just a dummy call, the real one should dispatch based on vmcall number */
    uint64_t rax, rcx;
    uintptr_t hva;
    const char *s = "HELLOOOOOO WORLDDDDDDDD\n";
    rax = kvm_register_read(vcpu, VCPU_REGS_RAX);
    rcx = kvm_register_read(vcpu, VCPU_REGS_RCX);
    hva = machine_addr(rax);

    // pr_err("PHYS ADDRESS: 0x%lx, SZ: 0x%lx, HVA: 0x%lx\n", rax, rcx, hva);
    if (strlen(s) > rcx) {
        return kvm_skip_emulated_instruction(vcpu);
    }
    strscpy((char *)hva, s, strlen(s));
    return kvm_skip_emulated_instruction(vcpu);
}

static void handle_ext_interrupt(struct kvm_vcpu *vcpu) {
    pr_err("Timer interrupt\n");
}

static void (*const vmx_exit_handlers[])(struct kvm_vcpu *) = {
    [EXIT_REASON_EXTERNAL_INTERRUPT] = handle_ext_interrupt,
    [EXIT_REASON_CR_ACCESS] = handle_cr,
    [EXIT_REASON_CPUID] = kvm_emulate_cpuid,
    [EXIT_REASON_MSR_READ] = handle_rdmsr,
    [EXIT_REASON_MSR_WRITE] = handle_wrmsr,
    [EXIT_REASON_XSETBV] = handle_xsetbv,
    [EXIT_REASON_RDTSC] = handle_rdtsc,
    [EXIT_REASON_PREEMPTION_TIMER] = handle_preemption_timer,
    [EXIT_REASON_VMCALL] = handle_vmcall,
};

static void vmx_handle_exit(struct kvm_vcpu *vcpu) {
    uint32_t exit_reason;

    exit_reason = vmcs_read32(VM_EXIT_REASON);

    if (exit_reason < ARRAY_SIZE(vmx_exit_handlers) &&
        vmx_exit_handlers[exit_reason])
        return vmx_exit_handlers[exit_reason](vcpu);

    dump_vmcs(vcpu);
    panic("vmx: unexpected exit reason %d\n", exit_reason);
}

static void vmx_skip_emulated_instruction(struct kvm_vcpu *vcpu) {
    unsigned long rip;
    uint32_t interruptibility;

    rip = kvm_rip_read(vcpu);
    rip += vmcs_read32(VM_EXIT_INSTRUCTION_LEN);
    kvm_rip_write(vcpu, rip);

    /* skipping an emulated instruction also counts */
    interruptibility = vmcs_read32(GUEST_INTERRUPTIBILITY_INFO);
    interruptibility &= ~(GUEST_INTR_STATE_STI | GUEST_INTR_STATE_MOV_SS);
    vmcs_write32(GUEST_INTERRUPTIBILITY_INFO, interruptibility);
}

struct kvm_x86_ops vmx_x86_ops = {
    .cpu_has_kvm_support = vmx_cpu_has_kvm_support,
    .disabled_by_bios = vmx_disabled_by_bios,
    .hardware_setup = vmx_hardware_setup,

    .vcpu_enable = vmx_vcpu_enable,
    .vcpu_setup = vmx_vcpu_setup,
    .get_cpl = vmx_get_cpl,
    .get_segment = vmx_get_segment,
    .set_segment = vmx_set_segment,
    .get_rflags = vmx_get_rflags,
    .set_rflags = vmx_set_rflags,
    .get_rip = vmx_get_rip,
    .set_rip = vmx_set_rip,
    .set_tdp = vmx_set_tdp,

    .run = vmx_vcpu_run,
    .handle_exit = vmx_handle_exit,
    .skip_emulated_instruction = vmx_skip_emulated_instruction,
};
