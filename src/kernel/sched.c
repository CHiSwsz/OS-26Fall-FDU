#include <kernel/sched.h>
#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/printk.h>
#include <aarch64/intrinsic.h>
#include <kernel/cpu.h>
#include <common/rbtree.h>
#include <common/string.h>

extern bool panic_flag;

extern void swtch(KernelContext *new_ctx, KernelContext **old_ctx);

static SpinLock sched_lock;
static ListNode run_queue;

void init_sched()
{
    init_spinlock(&sched_lock);
    init_list_node(&run_queue);
    for (int i = 0; i < NCPU; i++) {
        Proc *idle = &cpus[i].sched.idle;
        memset(idle, 0, sizeof(*idle));
        idle->idle = true;
        idle->pid = -i - 1; // Idle processes are outside the process tree.
        idle->state = RUNNING;
        init_list_node(&idle->children);
        init_list_node(&idle->ptnode);
        init_sem(&idle->childexit, 0);
        init_schinfo(&idle->schinfo);
        // The first swtch saves this CPU's existing boot stack.
        cpus[i].sched.current = idle;
    }
}

Proc *thisproc()
{
    return cpus[cpuid()].sched.current;
}

void init_schinfo(struct schinfo *p)
{
    init_list_node(&p->run_node);
    p->queued = false;
}

void acquire_sched_lock()
{
    acquire_spinlock(&sched_lock);
}

void release_sched_lock()
{
    release_spinlock(&sched_lock);
}

// Caller holds sched_lock. Only ordinary RUNNABLE processes are queued.
static void enqueue(Proc *p)
{
    ASSERT(!p->idle && p->state == RUNNABLE && !p->schinfo.queued);
    _insert_into_list(run_queue.prev, &p->schinfo.run_node);
    p->schinfo.queued = true;
}

bool is_zombie(Proc *p)
{
    bool r;
    acquire_sched_lock();
    r = p->state == ZOMBIE;
    release_sched_lock();
    return r;
}

bool activate_proc(Proc *p)
{
    acquire_sched_lock();
    bool activated = false;
    switch (p->state) {
    case RUNNING:
    case RUNNABLE:
        break;
    case UNUSED:
    case SLEEPING:
        p->state = RUNNABLE;
        enqueue(p);
        activated = true;
        break;
    default:
        PANIC();
    }
    release_sched_lock();
    return activated;
}

static void update_this_state(enum procstate new_state)
{
    Proc *p = thisproc();
    ASSERT(new_state == RUNNABLE || new_state == SLEEPING || new_state == ZOMBIE);
    ASSERT(!p->schinfo.queued);
    ASSERT(!p->idle || new_state == RUNNABLE);
    p->state = new_state;
    if (new_state == RUNNABLE && !p->idle)
        enqueue(p);
}

static Proc *pick_next()
{
    if (_empty_list(&run_queue))
        return &cpus[cpuid()].sched.idle;
    Proc *p = container_of(run_queue.next, Proc, schinfo.run_node);
    ASSERT(p->schinfo.queued && p->state == RUNNABLE);
    _detach_from_list(&p->schinfo.run_node);
    p->schinfo.queued = false;
    return p;
}

static void update_this_proc(Proc *p)
{
    cpus[cpuid()].sched.current = p;
}

// A simple scheduler.
// You are allowed to replace it with whatever you like.
// call with sched_lock
void sched(enum procstate new_state)
{
    auto this = thisproc();
    ASSERT(this->state == RUNNING);
    update_this_state(new_state);
    auto next = pick_next();
    update_this_proc(next);
    ASSERT(next->state == RUNNABLE);
    next->state = RUNNING;
    if (next != this) {
        // Keep the lock until the old stack is no longer in use. The resumed
        // sched(), or proc_entry() for a new process, releases it.
        swtch(next->kcontext, &this->kcontext);
    }
    release_sched_lock();
}

u64 proc_entry(void (*entry)(u64), u64 arg)
{
    release_sched_lock();
    set_return_addr(entry);
    return arg;
}
