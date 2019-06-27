/*
 * 64-bit system call stack frame layout defines and helpers,
 * for assembly code:
 */

/* The layout forms the "struct pt_regs" on the stack: */
/*
 * C ABI says these regs are callee-preserved. They aren't saved on kernel entry
 * unless syscall needs a complete, fully filled "struct pt_regs".
 */
#define R15             0*8
#define R14             1*8
#define R13             2*8
#define R12             3*8
#define RBP             4*8
#define RBX             5*8
/* These regs are callee-clobbered. Always saved on kernel entry. */
#define R11             6*8
#define R10             7*8
#define R9              8*8
#define R8              9*8
#define RAX             10*8
#define RCX             11*8
#define RDX             12*8
#define RSI             13*8
#define RDI             14*8
/*
 * On syscall entry, this is syscall#. On CPU exception, this is error code.
 * On hw interrupt, it's IRQ number:
 */
#define ORIG_RAX        15*8
/* Return frame for iretq */
#define RIP             16*8
#define CS              17*8
#define RFLAGS          18*8
#define RSP             19*8
#define SS              20*8

#define SIZEOF_PTREGS   21*8

        .macro ALLOC_PT_GPREGS_ON_STACK
        addq    $-(15*8), %rsp
        .endm

        .macro SAVE_C_REGS_HELPER offset=0 rax=1 rcx=1 r8910=1 r11=1
        .if \r11
        movq %r11, 6*8+\offset(%rsp)
        .endif
        .if \r8910
        movq %r10, 7*8+\offset(%rsp)
        movq %r9,  8*8+\offset(%rsp)
        movq %r8,  9*8+\offset(%rsp)
        .endif
        .if \rax
        movq %rax, 10*8+\offset(%rsp)
        .endif
        .if \rcx
        movq %rcx, 11*8+\offset(%rsp)
        .endif
        movq %rdx, 12*8+\offset(%rsp)
        movq %rsi, 13*8+\offset(%rsp)
        movq %rdi, 14*8+\offset(%rsp)
        .endm
        .macro SAVE_C_REGS offset=0
        SAVE_C_REGS_HELPER \offset, 1, 1, 1, 1
        .endm
        .macro SAVE_C_REGS_EXCEPT_RAX_RCX offset=0
        SAVE_C_REGS_HELPER \offset, 0, 0, 1, 1
        .endm
        .macro SAVE_C_REGS_EXCEPT_R891011
        SAVE_C_REGS_HELPER 0, 1, 1, 0, 0
        .endm
        .macro SAVE_C_REGS_EXCEPT_RCX_R891011
        SAVE_C_REGS_HELPER 0, 1, 0, 0, 0
        .endm
        .macro SAVE_C_REGS_EXCEPT_RAX_RCX_R11
        SAVE_C_REGS_HELPER 0, 0, 0, 1, 0
        .endm

        .macro SAVE_EXTRA_REGS offset=0
        movq %r15, 0*8+\offset(%rsp)
        movq %r14, 1*8+\offset(%rsp)
        movq %r13, 2*8+\offset(%rsp)
        movq %r12, 3*8+\offset(%rsp)
        movq %rbp, 4*8+\offset(%rsp)
        movq %rbx, 5*8+\offset(%rsp)
        .endm

        .macro POP_EXTRA_REGS
        popq %r15
        popq %r14
        popq %r13
        popq %r12
        popq %rbp
        popq %rbx
        .endm

        .macro POP_C_REGS
        popq %r11
        popq %r10
        popq %r9
        popq %r8
        popq %rax
        popq %rcx
        popq %rdx
        popq %rsi
        popq %rdi
        .endm

/* use one scratch register to switch to kernel address space */
.macro SWITCH_TO_KERNEL scratch_reg:req
        /* save cr3 & switch to kernel page table */
        movq    %cr3, \scratch_reg
        movq    \scratch_reg, PER_CPU_VAR(cr3_scratch)
        movq    $kpml4, \scratch_reg
        movq    \scratch_reg, %cr3

        /* relocate to identity mapping */
        movq    $1f, \scratch_reg
        jmp     *\scratch_reg

        1:
        /* reset %rsp to identity mapping */
        movq    $__ENTRY_OFFSET, \scratch_reg
        subq    \scratch_reg, %rsp
.endm

/* use one scratch register to switch to user address space */
.macro SWITCH_TO_USER scratch_reg:req
        /* reset %rsp to shared mapping */
        movq    $__ENTRY_OFFSET, \scratch_reg
        addq    \scratch_reg, %rsp

        /* relocate to shared mapping */
        movabsq $(1f + __ENTRY_OFFSET), \scratch_reg
        jmp     *\scratch_reg
        1:

        /* restore cr3 & switch back to user page table */
        movq    PER_CPU_VAR(cr3_scratch), \scratch_reg
        movq    \scratch_reg, %cr3
.endm
