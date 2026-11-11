#include <kernel/proc.h>
#include <kernel/mem.h>
#include <kernel/sched.h>
#include <aarch64/mmu.h>
#include <common/list.h>
#include <common/string.h>
#include <kernel/printk.h>

Proc root_proc;

#define PID_LIMIT 32768
static u64 pid_bitmap[PID_LIMIT / 64];
static SpinLock pid_lock;
// Nested lock order: process tree -> semaphore -> scheduler.
static SpinLock tree_lock;

void kernel_entry();
u64 proc_entry(void (*entry)(u64), u64 arg);

static int alloc_pid()
{
    acquire_spinlock(&pid_lock);
    for (usize i = 0; i < PID_LIMIT / 64; i++) {
        u64 available = ~pid_bitmap[i];
        if (available) {
            int bit = __builtin_ctzll(available);
            pid_bitmap[i] |= BIT(bit);
            release_spinlock(&pid_lock);
            return (int)(i * 64) + bit;
        }
    }
    release_spinlock(&pid_lock);
    PANIC();
}

static void free_pid(int pid)
{
    ASSERT(pid > 0 && pid < PID_LIMIT);
    acquire_spinlock(&pid_lock);
    ASSERT(pid_bitmap[pid / 64] & BIT(pid % 64));
    pid_bitmap[pid / 64] &= ~BIT(pid % 64);
    release_spinlock(&pid_lock);
}

// init_kproc initializes the kernel process
// NOTE: should call after kinit
void init_kproc()
{
    init_spinlock(&tree_lock);
    init_spinlock(&pid_lock);
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    pid_bitmap[0] = 1; // Reserve PID 0.

    init_proc(&root_proc);
    root_proc.parent = &root_proc;
    start_proc(&root_proc, kernel_entry, 123456);
}

void init_proc(Proc *p)
{
    ASSERT(p != NULL);
    memset(p, 0, sizeof(*p));
    p->pid = alloc_pid();
    p->state = UNUSED;
    init_sem(&p->childexit, 0);
    init_list_node(&p->children);
    init_list_node(&p->ptnode);
    init_schinfo(&p->schinfo);
    p->kstack = kalloc_page();
    ASSERT(p->kstack != NULL);
    p->ucontext = (UserContext *)((u8 *)p->kstack + PAGE_SIZE) - 1;
    p->kcontext = (KernelContext *)p->ucontext - 1;
    memset(p->ucontext, 0, sizeof(*p->ucontext));
    memset(p->kcontext, 0, sizeof(*p->kcontext));
}

Proc *create_proc()
{
    Proc *p = kalloc(sizeof(Proc));
    init_proc(p);
    return p;
}

void set_parent_to_this(Proc *proc)
{
    Proc *parent = thisproc();
    ASSERT(!parent->idle);
    acquire_spinlock(&tree_lock);
    ASSERT(proc->parent == NULL);
    proc->parent = parent;
    _insert_into_list(&parent->children, &proc->ptnode);
    release_spinlock(&tree_lock);
}

int start_proc(Proc *p, void (*entry)(u64), u64 arg)
{
    ASSERT(entry != NULL && p->state == UNUSED);
    acquire_spinlock(&tree_lock);
    if (p->parent == NULL) {
        p->parent = &root_proc;
        _insert_into_list(&root_proc.children, &p->ptnode);
    }
    p->kcontext->x0 = (u64)entry;
    p->kcontext->x1 = arg;
    p->kcontext->x30 = (u64)proc_entry;
    int pid = p->pid;
    release_spinlock(&tree_lock);
    activate_proc(p);
    // A different CPU may already have run and reaped p after activation.
    return pid;
}

int wait(int *exitcode)
{
    Proc *parent = thisproc();
    ASSERT(!parent->idle);
    for (;;) {
        acquire_spinlock(&tree_lock);
        bool empty = _empty_list(&parent->children);
        release_spinlock(&tree_lock);
        if (empty)
            return -1;

        // Never sleep while holding the tree lock. The semaphore remembers
        // exits that occur between the check above and this wait.
        if (!wait_sem(&parent->childexit))
            continue;
        acquire_spinlock(&tree_lock);
        for (ListNode *node = parent->children.next;
             node != &parent->children; node = node->next) {
            Proc *child = container_of(node, Proc, ptnode);
            // Taking sched_lock here also waits for exit's swtch to finish.
            if (!is_zombie(child))
                continue;
            int pid = child->pid;
            int code = child->exitcode;
            _detach_from_list(&child->ptnode);
            child->parent = NULL;
            release_spinlock(&tree_lock);
            if (exitcode)
                *exitcode = code;
            kfree_page(child->kstack);
            kfree(child);
            free_pid(pid);
            return pid;
        }
        // Every successfully consumed notification must name a zombie child.
        PANIC();
    }
}

NO_RETURN void exit(int code)
{
    Proc *p = thisproc();
    ASSERT(p != &root_proc && !p->idle);
    acquire_spinlock(&tree_lock);
    p->exitcode = code;
    while (!_empty_list(&p->children)) {
        Proc *child = container_of(p->children.next, Proc, ptnode);
        _detach_from_list(&child->ptnode);
        child->parent = &root_proc;
        _insert_into_list(&root_proc.children, &child->ptnode);
        if (is_zombie(child))
            post_sem(&root_proc.childexit);
    }

    // Notify before taking sched_lock: post_sem may acquire it itself.
    // The parent cannot inspect the tree until we hold sched_lock, so its
    // is_zombie() cannot finish until we have switched off this kernel stack.
    post_sem(&p->parent->childexit);
    acquire_sched_lock();
    release_spinlock(&tree_lock);
    sched(ZOMBIE);

    PANIC(); // prevent the warning of 'no_return function returns'
}
