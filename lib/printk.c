#include <asm/tsc.h>
#include <sys/console.h>
#include <sys/list.h>
#include <sys/printk.h>
#include <sys/spinlock.h>

#define LOG_LINE_MAX 1024

#define for_each_console(con) \
    list_for_each_entry(con, &console_drivers, list)

static LIST_HEAD(console_drivers);
static int loglevel = LOGLEVEL_DEFAULT;
static int curlevel = LOGLEVEL_DEFAULT;
static DEFINE_SPINLOCK(console_lock);

void register_console(struct console *newcon) {
    list_add_tail(&newcon->list, &console_drivers);
}

__weak size_t pr_prefix(char *buf, size_t size) {
    uint64_t s, us;
    size_t len;

    s = uptime();
    us = do_div(s, 1000000);
    len = scnprintf(buf, size, "[%5" PRIu64 ".%06" PRIu64 "] ", s, us);
    return len;
}

int vprintk(int level, const char *fmt, va_list args) {
    static char buf[LOG_LINE_MAX];
    int thislevel;
    size_t len = 0;
    struct console *con;

    spin_lock(&console_lock);

    thislevel = (level == LOGLEVEL_CONT) ? curlevel : level;
    if (thislevel > loglevel)
        goto done;

    if (level == LOGLEVEL_CONT)
        goto cont;

    len = pr_prefix(buf, sizeof(buf));

cont:
    len += vscnprintf(buf + len, sizeof(buf) - len, fmt, args);
    for_each_console(con) {
        if (con->write)
            con->write(con, buf, len);
    }

done:
    spin_unlock(&console_lock);
    return len;
}

int printk(int level, const char *fmt, ...) {
    va_list args;
    int r;

    va_start(args, fmt);
    r = vprintk(level, fmt, args);
    va_end(args);

    return r;
}
