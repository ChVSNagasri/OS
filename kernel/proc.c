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

extern char trampoline[];

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
// Returned process has p->lock HELD.
//

static struct proc *
schedforge_select_process(void)
{
  struct proc *p;
  struct proc *selected = 0;

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

    else if (schedforge_policy == SCHED_FCFS) {

      if (selected == 0) {

        selected = p;

      }
      else if (p->creation_time < selected->creation_time) {

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
// Map it high in memory, followed by an invalid guard page.

void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {

    char *pa = kalloc();

    if (pa == 0)
      panic("kalloc");

    uint64 va = KSTACK((int)(p - proc));

    kvmmap(kpgtbl,
           va,
           (uint64)pa,
           PGSIZE,
           PTE_R | PTE_W);
  }
}


// =========================================================
// PROCESS INITIALIZATION
// =========================================================

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


// =========================================================
// CPU FUNCTIONS
// =========================================================

int
cpuid()
{
  int id = r_tp();

  return id;
}


struct cpu *
mycpu(void)
{
  int id = cpuid();

  struct cpu *c = &cpus[id];

  return c;
}


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
  // SCHEDFORGE SCHEDULING FIELDS
  // =======================================================

  p->sched_policy = DEFAULT_SCHEDULER;
  p->priority = 0;

  p->creation_time = ticks;
  p->run_time = 0;
  p->waiting_time = 0;

  // =======================================================
  // SCHEDFORGE PERFORMANCE METRICS
  // =======================================================

  p->first_run_time = 0;
  p->completion_time = 0;
  p->response_time = 0;
  p->turnaround_time = 0;
  p->time_slices = 0;

  p->last_state_change = ticks;

  p->time_slice = RR_QUANTUM;
  p->queue_level = 0;

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

  memset(&p->context, 0, sizeof(p->context));

  p->context.ra = (uint64)forkret;

  p->context.sp = p->kstack + PGSIZE;

  return p;
}


// =========================================================
// FREE PROCESS
// =========================================================

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

  p->first_run_time = 0;
  p->completion_time = 0;
  p->response_time = 0;
  p->turnaround_time = 0;
  p->time_slices = 0;
  p->last_state_change = 0;

  p->time_slice = RR_QUANTUM;
  p->queue_level = 0;

  p->state = UNUSED;
}


// =========================================================
// PROCESS PAGE TABLE
// =========================================================

pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  pagetable = uvmcreate();

  if (pagetable == 0)
    return 0;

  if (mappages(pagetable,
               TRAMPOLINE,
               PGSIZE,
               (uint64)trampoline,
               PTE_R | PTE_X) < 0) {

    uvmfree(pagetable, 0);

    return 0;
  }

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

void
userinit(void)
{
  struct proc *p;

  p = allocproc();

  initproc = p;

  p->cwd = namei("/");

  p->state = RUNNABLE;

  p->last_state_change = ticks;

  release(&p->lock);
}


// =========================================================
// GROW PROCESS
// =========================================================

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

int
kfork(void)
{
  int i, pid;

  struct proc *np;

  struct proc *p = myproc();

  if ((np = allocproc()) == 0)
    return -1;

  if (uvmcopy(p->pagetable,
              np->pagetable,
              p->sz) < 0) {

    freeproc(np);

    release(&np->lock);

    return -1;
  }

  np->sz = p->sz;

  *(np->trapframe) = *(p->trapframe);

  np->trapframe->a0 = 0;

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

  np->last_state_change = ticks;

  release(&np->lock);

  return pid;
}


// =========================================================
// REPARENT
// =========================================================

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

void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // -------------------------------------------------------
  // SchedForge: record final CPU time
  // -------------------------------------------------------

  acquire(&p->lock);

  p->run_time += ticks - p->last_state_change;

  p->completion_time = ticks;

  p->turnaround_time =
      p->completion_time - p->creation_time;

  p->last_state_change = ticks;

  release(&p->lock);

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

  reparent(p);

  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;

  p->state = ZOMBIE;

  release(&wait_lock);

  sched();

  panic("zombie exit");
}


// =========================================================
// WAIT
// =========================================================

int
kwait(uint64 addr)
{
  struct proc *pp;

  int havekids, pid;

  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {

    havekids = 0;

    for (pp = proc;
         pp < &proc[NPROC];
         pp++) {

      if (pp->parent == p) {

        acquire(&pp->lock);

        havekids = 1;

        if (pp->state == ZOMBIE) {

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

    if (!havekids || killed(p)) {

      release(&wait_lock);

      return -1;
    }

    sleep_prepare(p);

    release(&wait_lock);

    sleep();

    acquire(&wait_lock);
  }
}


// =========================================================
// SCHEDFORGE SCHEDULER
// =========================================================

void
scheduler(void)
{
  struct proc *p;

  struct cpu *c = mycpu();

  c->proc = 0;

  for (;;) {

    intr_on();

    intr_off();

    // Select process according to SchedForge policy.
    p = schedforge_select_process();

    if (p == 0) {

      asm volatile("wfi");

      continue;
    }

    // =====================================================
    // SCHEDFORGE METRICS
    // RUNNABLE -> RUNNING
    // =====================================================

    // Calculate waiting time.
    p->waiting_time +=
        ticks - p->last_state_change;

    // Record first time process receives CPU.
    if (p->first_run_time == 0) {

      p->first_run_time = ticks;

      p->response_time =
          p->first_run_time -
          p->creation_time;
    }

    // Process starts running.
    p->last_state_change = ticks;

    p->state = RUNNING;

    c->proc = p;

    swtch(&c->context,
          &p->context);

    c->proc = 0;

    // p->lock is still held here.
    release(&p->lock);
  }
}


// =========================================================
// SWITCH TO SCHEDULER
// =========================================================

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

void
yield(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  // Record CPU time used before yielding.
  p->run_time +=
      ticks - p->last_state_change;

  // Start waiting-time interval.
  p->last_state_change = ticks;

  p->state = RUNNABLE;

  sched();

  release(&p->lock);
}


// =========================================================
// FORK RETURN
// =========================================================

void
forkret(void)
{
  extern char userret[];

  static int first = 1;

  struct proc *p = myproc();

  release(&p->lock);

  if (__atomic_load_n(&first,
                      __ATOMIC_ACQUIRE)) {

    fsinit(ROOTDEV);

    __atomic_store_n(&first,
                     0,
                     __ATOMIC_RELEASE);

    p->trapframe->a0 =
      kexec("/init",
            (char *[]){"/init", 0});

    if (p->trapframe->a0 == -1)
      panic("exec");
  }

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

void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

  if (p->chan != 0) {

    // Record CPU time before sleeping.
    p->run_time +=
        ticks - p->last_state_change;

    p->last_state_change = ticks;

    p->state = SLEEPING;

    sched();
  }

  release(&p->lock);
}


// =========================================================
// WAKEUP
// =========================================================

void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc;
       p < &proc[NPROC];
       p++) {

    acquire(&p->lock);

    if (p->chan == chan) {

      p->chan = 0;

      if (p->state == SLEEPING) {

        p->state = RUNNABLE;

        // Start waiting-time measurement.
        p->last_state_change = ticks;
      }
    }

    release(&p->lock);
  }
}


// =========================================================
// KILL
// =========================================================

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

        p->state = RUNNABLE;

        p->last_state_change = ticks;
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
