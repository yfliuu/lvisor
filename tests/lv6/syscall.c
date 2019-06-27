#include <asm/setup.h>
#include <sys/errno.h>

static long sys_nop(void) {
    return -ENOSYS;
}

static long sys_hey(long x) {
    pr_info("hey %ld\n", x);
    return 0;
}

static long sys_bye(long x) {
    pr_info("bye %ld\n", x);
    die();
    return 0;
}

void *syscall_table[NR_syscalls] = {
    [0 ... NR_syscalls - 1] = &sys_nop,
    [0] = sys_hey,
    [1] = sys_bye,
};
