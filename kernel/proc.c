#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "schedforge.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;

// =========================================================
// SCHEDFORGE CURRENT SCHEDULER POLICY
// =========================================================

int schedforge_policy = DEFAULT_SCHEDULER;

struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;


// =========================================================
// SCHEDFORGE PROCESS SELECTION
// =========================================================
//
// Select one RUNNABLE process according to the currently
// selected SchedForge scheduling policy.
//
// IMPORTANT:
// The returned process has its p->lock HELD.
//
// The caller is responsible for releasing p->lock.
//
// =========================================================

static struct proc *
schedforge_select_process(void)
{
  struct proc *p;
  struct proc *selected = 0;

  // -------------------------------------------------------
  // Scan all processes.
  // -------------------------------------------------------

  for (p = proc; p < &proc[NPROC]; p++) {

    acquire(&p->lock);

    // Only RUNNABLE processes can be selected.
    if (p->state != RUNNABLE) {
      release(&p->lock);
      continue;
    }

    // -----------------------------------------------------
    // ROUND ROBIN
    // -----------------------------------------------------
    //
    // For the initial SchedForge implementation, RR uses
    // the normal xv6 process-table scan.
    //
    // yield() returns the process to RUNNABLE state and
    // the scheduler scans again.
    //
    if (schedforge_policy == SCHED_RR) {

      if (selected == 0) {
        selected = p;
        continue;
      }

      release(&p->lock);
    }

    // -----------------------------------------------------
    // FCFS
    // -----------------------------------------------------
    //
    // Select the RUNNABLE process with the earliest
    // creation_time.
    //
    else if (schedforge_policy == SCHED_FCFS) {

      if (selected == 0) {

        selected = p;
      }
      else if (p->creation_time < selected->creation_time) {

        // Release previous candidate.
        release(&selected->lock);

        selected = p;
      }
      else {

        release(&p->lock);
      }
    }

    // -----------------------------------------------------
    // MLFQ
    // -----------------------------------------------------
    //
    // Lower queue_level means higher priority.
    //
    // Example:
    //
    // queue 0 -> highest priority
    // queue 1 -> medium priority
    // queue 2 -> lowest priority
    //
    // If two processes are in the same queue, the older
    // process is selected.
    //
    else if (schedforge_policy == SCHED_MLFQ) {

      if (selected == 0) {

        selected = p;
      }
      else if (p->queue_level < selected->queue_level) {

        release(&selected->lock);

        selected = p;
      }
      else if (p->queue_level == selected->queue_level &&
               p->creation_time < selected->creation_time) {

        release(&selected->lock);

        selected = p;
      }
      else {

        release(&p->lock);
      }
    }

    // -----------------------------------------------------
    // Safety fallback
    // -----------------------------------------------------

    else {

      if (selected == 0) {

        selected = p;
        continue;
      }

      release(&p->lock);
    }
  }

  return selected;
}


// =========================================================
// PROCESS TABLE / KERNEL STACK INITIALIZATION
// =========================================================

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();

    if (pa == 0)
      panic("kalloc");

    uint64 va = KSTACK((int)(p - proc));

    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE,
           PTE_R | PTE_W);
  }
}


// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");

  for (p = proc; p < &proc[NPROC]; p++) {

    initlock(&p->lock, "proc");

    p->state = UNUSED;

    p->kstack = KSTACK((int)(p - proc));
  }
}


// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();

  return id;
}


// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();

  struct cpu *c = &cpus[id];

  return c;
}


// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();

  struct cpu *c = mycpu();

  struct proc *p = c->proc;

  pop_off();

  return p;
}


// =========================================================
// PID ALLOCATION
// =========================================================

int
allocpid()
{
  int pid;

  acquire(&pid_lock);

  pid = nextpid;

  nextpid = nextpid + 1;

  release(&pid_lock);

  return pid;
}


// =========================================================
// PROCESS ALLOCATION
// =========================================================

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails,
// return 0.

static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {

    acquire(&p->lock);

    if (p->state == UNUSED) {

      goto found;

    }
    else {

      release(&p->lock);
    }
  }

  return 0;

found:

  p->pid = allocpid();

  // =======================================================
  // Initialize SchedForge scheduling fields
  // =======================================================

  p->sched_policy = DEFAULT_SCHEDULER;

  p->priority = 0;

  p->creation_time = ticks;

  p->run_time = 0;

  p->waiting_time = 0;

  p->time_slice = RR_QUANTUM;

  p->queue_level = 0;

  // =======================================================

  p->state = USED;

  // Allocate a trapframe page.
  if ((p->trapframe =
       (struct trapframe *)kalloc()) == 0) {

    freeproc(p);

    release(&p->lock);

    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);

  if (p->pagetable == 0) {

    freeproc(p);

    release(&p->lock);

    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.

  memset(&p->context, 0, sizeof(p->context));

  p->context.ra = (uint64)forkret;

  p->context.sp = p->kstack + PGSIZE;

  return p;
}


// =========================================================
// FREE PROCESS
// =========================================================

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.

static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);

  p->trapframe = 0;

  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);

  p->pagetable = 0;

  p->sz = 0;

  p->pid = 0;

  p->name[0] = 0;

  p->chan = 0;

  p->killed = 0;

  p->xstate = 0;

  // Reset SchedForge fields.
  p->sched_policy = DEFAULT_SCHEDULER;
  p->priority = 0;

  p->creation_time = 0;
  p->run_time = 0;
  p->waiting_time = 0;

  p->time_slice = RR_QUANTUM;
  p->queue_level = 0;

  p->state = UNUSED;
}


// =========================================================
// PROCESS PAGE TABLE
// =========================================================

// Create a user page table for a given process,
// with no user memory, but with trampoline and
// trapframe pages.

pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();

  if (pagetable == 0)
    return 0;

  // map the trampoline code
  // (for system call return)
  // at the highest user virtual address.

  if (mappages(pagetable,
               TRAMPOLINE,
               PGSIZE,
               (uint64)trampoline,
               PTE_R | PTE_X) < 0) {

    uvmfree(pagetable, 0);

    return 0;
  }

  // map the trapframe page just below the trampoline page.

  if (mappages(pagetable,
               TRAPFRAME,
               PGSIZE,
               (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {

    uvmunmap(pagetable,
             TRAMPOLINE,
             1,
             0);

    uvmfree(pagetable, 0);

    return 0;
  }

  return pagetable;
}


// Free a process's page table,
// and free the physical memory it refers to.

void
proc_freepagetable(pagetable_t pagetable,
                   uint64 sz)
{
  uvmunmap(pagetable,
           TRAMPOLINE,
           1,
           0);

  uvmunmap(pagetable,
           TRAPFRAME,
           1,
           0);

  uvmfree(pagetable, sz);
}


// =========================================================
// FIRST USER PROCESS
// =========================================================

// Set up first user process.

void
userinit(void)
{
  struct proc *p;

  p = allocproc();

  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}


// =========================================================
// GROW PROCESS
// =========================================================

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.

int
growproc(int n)
{
  uint64 sz;

  struct proc *p = myproc();

  sz = p->sz;

  if (n > 0) {

    if (sz + n > TRAPFRAME)
      return -1;

    if ((sz = uvmalloc(p->pagetable,
                       sz,
                       sz + n,
                       PTE_W)) == 0) {

      return -1;
    }
  }
  else if (n < 0) {

    sz = uvmdealloc(p->pagetable,
                    sz,
                    sz + n);
  }

  p->sz = sz;

  return 0;
}


// =========================================================
// FORK
// =========================================================

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork()
// system call.

int
kfork(void)
{
  int i, pid;

  struct proc *np;

  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
    return -1;

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable,
              np->pagetable,
              p->sz) < 0) {

    freeproc(np);

    release(&np->lock);

    return -1;
  }

  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);

  np->cwd = idup(p->cwd);

  safestrcpy(np->name,
             p->name,
             sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);

  np->parent = p;

  release(&wait_lock);

  acquire(&np->lock);

  np->state = RUNNABLE;

  release(&np->lock);

  return pid;
}


// =========================================================
// REPARENT
// =========================================================

// Pass p's abandoned children to init.
// Caller must hold wait_lock.

void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc;
       pp < &proc[NPROC];
       pp++) {

    if (pp->parent == p) {

      pp->parent = initproc;

      wakeup(initproc);
    }
  }
}


// =========================================================
// EXIT
// =========================================================

// Exit the current process. Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().

void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {

    if (p->ofile[fd]) {

      struct file *f = p->ofile[fd];

      fileclose(f);

      p->ofile[fd] = 0;
    }
  }

  begin_op();

  iput(p->cwd);

  end_op();

  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;

  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();

  panic("zombie exit");
}


// =========================================================
// WAIT
// =========================================================

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.

int
kwait(uint64 addr)
{
  struct proc *pp;

  int havekids, pid;

  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {

    havekids = 0;

    // Scan through table looking for exited children.
    for (pp = proc;
         pp < &proc[NPROC];
         pp++) {

      if (pp->parent == p) {

        // make sure the child isn't still
        // in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;

        if (pp->state == ZOMBIE) {

          // Found one.
          pid = pp->pid;

          if (addr != 0 &&
              copyout(p->pagetable,
                      p->sz,
                      addr,
                      (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {

            release(&pp->lock);

            release(&wait_lock);

            return -1;
          }

          pp->parent = 0;

          freeproc(pp);

          release(&pp->lock);

          release(&wait_lock);

          return pid;
        }

        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {

      release(&wait_lock);

      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p);

    release(&wait_lock);

    sleep();

    acquire(&wait_lock);
  }
}


// =========================================================
// SCHEDFORGE SCHEDULER
// =========================================================
//
// SchedForge supports:
//
//   SCHED_RR   -> Round Robin
//   SCHED_FCFS -> First Come First Serve
//   SCHED_MLFQ -> Multilevel Feedback Queue
//
// =========================================================

void
scheduler(void)
{
  struct proc *p;

  struct cpu *c = mycpu();

  c->proc = 0;

  for (;;) {

    // The most recent process to run may have had
    // interrupts turned off; enable them to avoid a
    // deadlock if all processes are waiting.

    intr_on();

    intr_off();

    // -----------------------------------------------------
    // Select process according to SchedForge policy.
    // -----------------------------------------------------

    p = schedforge_select_process();

    // -----------------------------------------------------
    // Nothing runnable.
    // -----------------------------------------------------

    if (p == 0) {

      asm volatile("wfi");

      continue;
    }

    // -----------------------------------------------------
    // Run selected process.
    //
    // p->lock is already held by
    // schedforge_select_process().
    // -----------------------------------------------------

    p->state = RUNNING;

    c->proc = p;

    swtch(&c->context,
          &p->context);

    // Don't re-enable interrupts on release.
    mycpu()->intena = 0;

    // Process is done running for now.
    //
    // It should have changed p->state before
    // returning to the scheduler.

    c->proc = 0;

    // Release the lock held by the scheduler.
    release(&p->lock);
  }
}


// =========================================================
// SWITCH TO SCHEDULER
// =========================================================

// Switch to scheduler. Must hold only p->lock
// and have changed proc->state.
//
// Saves and restores intena because intena is a property
// of this kernel thread, not this CPU.

void
sched(void)
{
  int intena;

  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");

  if (mycpu()->noff != 1)
    panic("sched locks");

  if (p->state == RUNNING)
    panic("sched RUNNING");

  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;

  swtch(&p->context,
        &mycpu()->context);

  mycpu()->intena = intena;
}


// =========================================================
// YIELD
// =========================================================

// Give up the CPU for one scheduling round.

void
yield(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  p->state = RUNNABLE;

  sched();

  release(&p->lock);
}


// =========================================================
// FORK RETURN
// =========================================================

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.

void
forkret(void)
{
  extern char userret[];

  static int first = 1;

  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first,
                      __ATOMIC_ACQUIRE)) {

    // File system initialization must be run in the
    // context of a regular process.

    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first,
                     0,
                     __ATOMIC_RELEASE);

    // We can invoke kexec() now.
    p->trapframe->a0 =
      kexec("/init",
            (char *[]){"/init", 0});

    if (p->trapframe->a0 == -1)
      panic("exec");
  }

  // return to user space.
  prepare_return();

  uint64 satp =
    MAKE_SATP(p->pagetable);

  uint64 trampoline_userret =
    TRAMPOLINE +
    (userret - trampoline);

  ((void (*)(uint64))trampoline_userret)(satp);
}


// =========================================================
// SLEEP PREPARE
// =========================================================

// Register current process as waiting for wakeups on chan.

void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  if (chan == 0)
    panic("sleep_prepare: zero chan");

  p->chan = chan;

  release(&p->lock);
}


// =========================================================
// SLEEP
// =========================================================

// Put the thread to sleep.
// Assumes sleep_prepare() was called before.

void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  if (p->chan != 0) {

    p->state = SLEEPING;

    sched();
  }

  release(&p->lock);
}


// =========================================================
// WAKEUP
// =========================================================

// Wake up all processes sleeping on channel chan.

void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc;
       p < &proc[NPROC];
       p++) {

    acquire(&p->lock);

    if (p->chan == chan) {

      // Signal that wakeup happened.
      p->chan = 0;

      // If actually asleep, make runnable.
      if (p->state == SLEEPING) {

        p->state = RUNNABLE;
      }
    }

    release(&p->lock);
  }
}


// =========================================================
// KILL
// =========================================================

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space.

int
kkill(int pid)
{
  struct proc *p;

  for (p = proc;
       p < &proc[NPROC];
       p++) {

    acquire(&p->lock);

    if (p->pid == pid) {

      p->killed = 1;

      if (p->state == SLEEPING) {

        // Wake process from sleep().
        p->state = RUNNABLE;
      }

      release(&p->lock);

      return 0;
    }

    release(&p->lock);
  }

  return -1;
}


// =========================================================
// SET KILLED
// =========================================================

void
setkilled(struct proc *p)
{
  acquire(&p->lock);

  p->killed = 1;

  release(&p->lock);
}


// =========================================================
// CHECK KILLED
// =========================================================

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);

  k = p->killed;

  release(&p->lock);

  return k;
}


// =========================================================
// COPY OUT
// =========================================================

// Copy to either a user address, or kernel address,
// depending on usr_dst.

int
either_copyout(int user_dst,
               uint64 dst,
               void *src,
               uint64 len)
{
  struct proc *p = myproc();

  if (user_dst) {

    return copyout(p->pagetable,
                   p->sz,
                   dst,
                   src,
                   len);

  }
  else {

    memmove((char *)dst,
            src,
            len);

    return 0;
  }
}


// =========================================================
// COPY IN
// =========================================================

int
either_copyin(void *dst,
              int user_src,
              uint64 src,
              uint64 len)
{
  struct proc *p = myproc();

  if (user_src) {

    return copyin(p->pagetable,
                  p->sz,
                  dst,
                  src,
                  len);

  }
  else {

    memmove(dst,
            (char *)src,
            len);

    return 0;
  }
}


// =========================================================
// PROCESS DUMP
// =========================================================

// Print a process listing to console.
// For debugging.
// Runs when user types ^P on console.

void
procdump(void)
{
  static char *states[] = {

    [UNUSED] =
      "unused",

    [USED] =
      "used",

    [SLEEPING] =
      "sleep ",

    [RUNNABLE] =
      "runble",

    [RUNNING] =
      "run   ",

    [ZOMBIE] =
      "zombie"
  };

  struct proc *p;

  char *state;

  printk("\n");

  for (p = proc;
       p < &proc[NPROC];
       p++) {

    if (p->state == UNUSED)
      continue;

    if (p->state >= 0 &&
        p->state < NELEM(states) &&
        states[p->state]) {

      state = states[p->state];

    }
    else {

      state = "???";
    }

    printk("%d %s %s",
           p->pid,
           state,
           p->name);

    printk("\n");
  }
}
