#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/printk.h>
#include <common/sem.h>
#include <test/test.h>
#include <aarch64/intrinsic.h>

typedef u64 (*Syscall)(u64, u64, u64, u64, u64, u64);

static u64 report(u64 id, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    (void)b; (void)c; (void)d; (void)e; (void)f;
    return syscall_myreport(id);
}

static const Syscall syscall_table[NR_SYSCALL] = {
    [SYS_myreport] = report,
};

void syscall_entry(UserContext *context)
{
    u64 *x = context->x;
    u64 id = x[8];
    ASSERT(id < NR_SYSCALL && syscall_table[id]);
    x[0] = syscall_table[id](x[0], x[1], x[2], x[3], x[4], x[5]);
}
