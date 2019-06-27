#pragma once

#include <asm/kvm.h>

enum kvm_reg
{
    VCPU_REGS_RAX = 0,
    VCPU_REGS_RCX = 1,
    VCPU_REGS_RDX = 2,
    VCPU_REGS_RBX = 3,
    VCPU_REGS_RSP = 4,
    VCPU_REGS_RBP = 5,
    VCPU_REGS_RSI = 6,
    VCPU_REGS_RDI = 7,
    VCPU_REGS_R8 = 8,
    VCPU_REGS_R9 = 9,
    VCPU_REGS_R10 = 10,
    VCPU_REGS_R11 = 11,
    VCPU_REGS_R12 = 12,
    VCPU_REGS_R13 = 13,
    VCPU_REGS_R14 = 14,
    VCPU_REGS_R15 = 15,
    NR_VCPU_REGS
};

enum
{
    VCPU_SREG_ES,
    VCPU_SREG_CS,
    VCPU_SREG_SS,
    VCPU_SREG_DS,
    VCPU_SREG_FS,
    VCPU_SREG_GS,
    VCPU_SREG_TR,
    VCPU_SREG_LDTR,
};

enum
{
    ACTIVITY_STATE_ACTIVE = 0,
    ACTIVITY_STATE_HLT,
    ACTIVITY_STATE_SHUTDOWN,
    ACTIVITY_STATE_WAIT_FOR_SIPI,
};

struct kvm_vcpu
{
    uint64_t regs[NR_VCPU_REGS];
    uint64_t cr2;
    _Atomic int activity_state;
    uint8_t sipi_vector;
};

struct kvm_x86_ops
{
    int (*cpu_has_kvm_support)(void);
    int (*disabled_by_bios)(void);
    int (*hardware_setup)(void);

    struct kvm_vcpu *(*vcpu_enable)(void);
    void (*vcpu_setup)(struct kvm_vcpu *vcpu);
    void (*set_tdp)(struct kvm_vcpu *vcpu, unsigned long tdp);
    int (*get_cpl)(struct kvm_vcpu *vcpu);
    void (*get_segment)(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);
    void (*set_segment)(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);
    unsigned long (*get_rflags)(struct kvm_vcpu *vcpu);
    void (*set_rflags)(struct kvm_vcpu *vcpu, unsigned long rflags);
    unsigned long (*get_rip)(struct kvm_vcpu *vcpu);
    void (*set_rip)(struct kvm_vcpu *vcpu, unsigned long rip);

    void (*run)(struct kvm_vcpu *vcpu);
    void (*handle_exit)(struct kvm_vcpu *vcpu);
    void (*skip_emulated_instruction)(struct kvm_vcpu *vcpu);
};

static inline unsigned long kvm_register_read(struct kvm_vcpu *vcpu,
                                              enum kvm_reg reg)
{
    return vcpu->regs[reg];
}

static inline uint64_t kvm_read_edx_eax(struct kvm_vcpu *vcpu)
{
    uint32_t eax = kvm_register_read(vcpu, VCPU_REGS_RAX);
    uint32_t edx = kvm_register_read(vcpu, VCPU_REGS_RDX);

    return eax | ((uint64_t)edx << 32);
}

static inline void kvm_register_write(struct kvm_vcpu *vcpu,
                                      enum kvm_reg reg,
                                      unsigned long val)
{
    vcpu->regs[reg] = val;
}

static inline void kvm_write_edx_eax(struct kvm_vcpu *vcpu, uint64_t val)
{
    kvm_register_write(vcpu, VCPU_REGS_RAX, (uint32_t)val);
    kvm_register_write(vcpu, VCPU_REGS_RDX, (uint32_t)(val >> 32));
}

unsigned long kvm_rip_read(struct kvm_vcpu *vcpu);
void kvm_rip_write(struct kvm_vcpu *vcpu, unsigned long val);

void kvm_get_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);
void kvm_set_segment(struct kvm_vcpu *vcpu, struct kvm_segment *var, int seg);

void kvm_emulate_cpuid(struct kvm_vcpu *vcpu);
void kvm_skip_emulated_instruction(struct kvm_vcpu *vcpu);
