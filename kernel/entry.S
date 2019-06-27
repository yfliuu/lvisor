#include <asm/cache.h>
#include <asm/irq.h>
#include <asm/percpu.h>
#include <asm/segment.h>
#include <asm/setup.h>
#include <sys/errno.h>
#include <io/linkage.h>
#include "asm-offsets.h"
#include "calling.h"

/* align the stack as struct pt_regs is not 16-byte aligned */
.macro ALIGN_STACK_PUSH
        push    %rsp
.endm
.macro ALIGN_STACK_POP
        popq    %rsp
.endm

/*
 * Keep the syscall entry in a separate section: if it is not used,
 * it will be removed through --gc-sections.
 */
.pushsection .text.do_syscall_64

/*
 * 64-bit SYSCALL instruction entry. Up to 6 arguments in registers.
 *
 * 64-bit SYSCALL saves rip to rcx, clears rflags.RF, then saves rflags to r11,
 * then loads new ss, cs, and rip from previously programmed MSRs.
 * rflags gets masked by a value from another MSR (so CLD and CLAC
 * are not needed). SYSCALL does not save anything on the stack
 * and does not change rsp.
 *
 * Registers on entry:
 * rax  system call number
 * rcx  return address
 * r11  saved rflags (note: r11 is callee-clobbered register in C ABI)
 * rdi  arg0
 * rsi  arg1
 * rdx  arg2
 * r10  arg3 (needs to be moved to rcx to conform to C ABI)
 * r8   arg4
 * r9   arg5
 * (note: r12-r15, rbp, rbx are callee-preserved in C ABI)
 *
 * Only called from user space.
 *
 * When user can change pt_regs->foo always force IRET. That is because
 * it deals with uncanonical addresses better. SYSRET has trouble
 * with them due to bugs in both AMD and Intel CPUs.
 */
ENTRY(do_syscall_64)
        swapgs

        /* save rsp */
        movq    %rsp, PER_CPU_VAR(rsp_scratch)
        /* switch to kernel stack */
        movq    PER_CPU_VAR(cpu_entry_area + CPU_ENTRY_AREA_tss + TSS_sp0), %rsp

        /* Construct struct pt_regs on stack */
        pushq   $USER_DS                        /* pt_regs->ss */
        pushq   PER_CPU_VAR(rsp_scratch)        /* pt_regs->sp */
        pushq   %r11                            /* pt_regs->flags */
        pushq   $USER_CS                        /* pt_regs->cs */
        pushq   %rcx                            /* pt_regs->ip */
        pushq   %rax                            /* pt_regs->orig_ax */
        pushq   %rdi                            /* pt_regs->di */
        pushq   %rsi                            /* pt_regs->si */
        pushq   %rdx                            /* pt_regs->dx */
        pushq   %rcx                            /* pt_regs->cx */
        pushq   $-ENOSYS                        /* pt_regs->ax */
        pushq   %r8                             /* pt_regs->r8 */
        pushq   %r9                             /* pt_regs->r9 */
        pushq   %r10                            /* pt_regs->r10 */
        pushq   %r11                            /* pt_regs->r11 */
        sub     $(6*8), %rsp                    /* pt_regs->bp, bx, r12-15 not saved */

        cmpq    $NR_syscalls, %rax
        jae     1f                              /* return -ENOSYS (already in pt_regs->ax) */

        SWITCH_TO_KERNEL scratch_reg=%rcx

        movq    %r10, %rcx
        ALIGN_STACK_PUSH
        call    *syscall_table(, %rax, 8)
        ALIGN_STACK_POP
        movq    %rax, RAX(%rsp)

        SWITCH_TO_USER scratch_reg=%rcx

        1:
        movq    RIP(%rsp), %rcx
        movq    RFLAGS(%rsp), %r11
        addq    $6*8, %rsp      /* skip extra regs -- they were preserved */

        popq    %rsi    /* skip r11 */
        popq    %r10
        popq    %r9
        popq    %r8
        popq    %rax
        popq    %rsi    /* skip rcx */
        popq    %rdx
        popq    %rsi
        popq    %rdi

        /* restore rsp */
        movq    PER_CPU_VAR(rsp_scratch), %rsp

        swapgs
        sysretq
END(do_syscall_64)

.popsection

/*
 * Interrupt entry/exit.
 *
 * Interrupt entry points save only callee clobbered registers in fast path.
 *
 * Entry runs with interrupts off.
 */

/* 0(%rsp): ~(interrupt number) */
.macro interrupt func
        cld
        ALLOC_PT_GPREGS_ON_STACK
        SAVE_C_REGS
        SAVE_EXTRA_REGS

        /* this should never happen: no interrupts in kernel */
        testb   $3, CS(%rsp)
        1:
        jz      1b

        /*
         * IRQ from user mode.  Switch to kernel gsbase and inform context
         * tracking that we're in kernel mode.
         */
        swapgs
        SWITCH_TO_KERNEL scratch_reg=%rdi

        movq    %rsp, %rdi
        ALIGN_STACK_PUSH
        call    \func  /* rdi points to pt_regs */
        ALIGN_STACK_POP
.endm

/*
 * APIC interrupts.
 */
.macro apicinterrupt num sym do_sym
ENTRY(\sym)
        pushq   $~(\num)
        interrupt \do_sym
        jmp     retint_user
END(\sym)
.endm

apicinterrupt LOCAL_TIMER_VECTOR                apic_timer_interrupt            smp_apic_timer_interrupt

apicinterrupt ERROR_APIC_VECTOR                 error_interrupt                 smp_error_interrupt
apicinterrupt SPURIOUS_APIC_VECTOR              spurious_interrupt              smp_spurious_interrupt

/*
 * Exception entry points.
 */

.macro idtentry sym do_sym has_error_code:req
ENTRY(\sym)
        .if \has_error_code == 0
        pushq   $-1                             /* ORIG_RAX: no syscall to restart */
        .endif

        ALLOC_PT_GPREGS_ON_STACK

        /* use push/jump instead call as error_entry may relocate to identity mapping */
        pushq   $1f
        jmp     error_entry
        /* returned flag: ebx=0: need swapgs on exit, ebx=1: don't need it */

        1:
        movq    %rsp, %rdi                      /* pt_regs pointer */

        .if \has_error_code
        movq    ORIG_RAX(%rsp), %rsi            /* get error code */
        movq    $-1, ORIG_RAX(%rsp)             /* no syscall to restart */
        .else
        xorl    %esi, %esi                      /* no error code */
        .endif

        ALIGN_STACK_PUSH
        call    \do_sym
        ALIGN_STACK_POP

        jmp     error_exit
END(\sym)
.endm

idtentry divide_error                   do_divide_error                 has_error_code=0
idtentry overflow                       do_overflow                     has_error_code=0
idtentry bounds                         do_bounds                       has_error_code=0
idtentry invalid_op                     do_invalid_op                   has_error_code=0
idtentry device_not_available           do_device_not_available         has_error_code=0
idtentry double_fault                   do_double_fault                 has_error_code=1
idtentry coprocessor_segment_overrun    do_coprocessor_segment_overrun  has_error_code=0
idtentry invalid_TSS                    do_invalid_TSS                  has_error_code=1
idtentry segment_not_present            do_segment_not_present          has_error_code=1
idtentry spurious_interrupt_bug         do_spurious_interrupt_bug       has_error_code=0
idtentry coprocessor_error              do_coprocessor_error            has_error_code=0
idtentry alignment_check                do_alignment_check              has_error_code=1
idtentry simd_coprocessor_error         do_simd_coprocessor_error       has_error_code=0

/* no nmi, int3, debug  */
idtentry stack_segment                  do_stack_segment                has_error_code=1

idtentry general_protection             do_general_protection           has_error_code=1
idtentry page_fault                     do_page_fault                   has_error_code=1

/*
 * Save all registers in pt_regs, and switch gs if needed.
 * Return: EBX=0: came from user mode; EBX=1: otherwise
 */
ENTRY(error_entry)
        cld
        SAVE_C_REGS 8
        SAVE_EXTRA_REGS 8
        xorl    %ebx, %ebx
        testb   $3, CS+8(%rsp)
        jz      .Lerror_kernelspace

        /* We entered from user mode. */
        swapgs
        SWITCH_TO_KERNEL scratch_reg=%rdi
        ret

.Lerror_kernelspace:
        incl    %ebx
        ret
END(error_entry)

/*
 * On entry, EBX is a "return to kernel mode" flag:
 *   1: already in kernel mode, don't need SWAPGS
 *   0: user gsbase is loaded, we need SWAPGS and standard preparation for return to usermode
 */
ENTRY(error_exit)
        testl   %ebx, %ebx
        jnz     retint_kernel

retint_user:
        /* return to user space */
        SWITCH_TO_USER scratch_reg=%rdi
        swapgs

retint_kernel:
        POP_EXTRA_REGS
        POP_C_REGS
        addq    $8, %rsp        /* skip regs->orig_ax */
        iretq
END(error_exit)

/* ignore 32-bit syscalls */
ENTRY(do_syscall_32)
        mov     $-ENOSYS, %eax
        sysret
END(do_syscall_32)

/*
 * %rdi: pointer to struct pt_regs
 * %rsi: new %cr3
 */
ENTRY(return_to_usermode)
        movq    %rdi, %rsp
        movq    %rsi, PER_CPU_VAR(cr3_scratch)
        jmp     retint_user
END(return_to_usermode)
