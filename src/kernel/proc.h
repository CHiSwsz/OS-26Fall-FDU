#pragma once

#include <common/defines.h>
#include <common/list.h>
#include <common/sem.h>
#include <common/rbtree.h>

enum procstate { UNUSED, RUNNABLE, RUNNING, SLEEPING, ZOMBIE };

typedef struct UserContext {
    // Offsets in bytes: x[0..30] = 0..240, sp_el0 = 248,
    // elr_el1 = 256, spsr_el1 = 264. Keep in sync with trap.S.
    u64 x[31];
    u64 sp_el0;
    u64 elr_el1;
    u64 spsr_el1;
} UserContext;

typedef struct KernelContext {
    // Keep in sync with swtch.S; the context pointer is the saved SP.
    u64 x0, x1;
    u64 x19, x20, x21, x22, x23, x24;
    u64 x25, x26, x27, x28, x29, x30;
} KernelContext;

_Static_assert(sizeof(UserContext) == 272, "trap frame size");
_Static_assert(__builtin_offsetof(UserContext, sp_el0) == 248, "trap SP offset");
_Static_assert(__builtin_offsetof(UserContext, elr_el1) == 256, "trap PC offset");
_Static_assert(__builtin_offsetof(UserContext, spsr_el1) == 264, "trap status offset");
_Static_assert(sizeof(KernelContext) == 112, "switch frame size");
_Static_assert(__builtin_offsetof(KernelContext, x19) == 16, "switch registers offset");
_Static_assert(__builtin_offsetof(KernelContext, x30) == 104, "switch LR offset");

// embeded data for procs
struct schinfo {
    ListNode run_node;
    bool queued;
};

typedef struct Proc {
    bool killed;
    bool idle;
    int pid;
    int exitcode;
    enum procstate state;
    Semaphore childexit;
    ListNode children;
    ListNode ptnode;
    struct Proc *parent;
    struct schinfo schinfo;
    void *kstack;
    UserContext *ucontext;
    KernelContext *kcontext;
} Proc;

void init_kproc();
void init_proc(Proc *);
Proc *create_proc();
void set_parent_to_this(Proc *);
int start_proc(Proc *, void (*entry)(u64), u64 arg);
NO_RETURN void exit(int code);
int wait(int *exitcode);
