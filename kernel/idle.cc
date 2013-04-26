#include "types.h"
#include "kernel.hh"
#include "amd64.h"
#include "spinlock.h"
#include "condvar.h"
#include "proc.hh"
#include "cpu.hh"
#include "spercpu.hh"
#include "kmtrace.hh"
#include "bits.hh"
#include "codex.hh"
#include "benchcodex.hh"
#include "cpuid.hh"

struct idle {
  struct proc *cur;
  SLIST_HEAD(zombies, proc) zombies;
  struct spinlock lock;
};

namespace {
  DEFINE_PERCPU(idle, idlem);
};

void idleloop(void);

struct proc *
idleproc(void)
{
  return idlem->cur;
}

void
idlezombie(struct proc *p)
{
  acquire(&idlem[mycpu()->id].lock);
  SLIST_INSERT_HEAD(&idlem[mycpu()->id].zombies, p, child_next);
  release(&idlem[mycpu()->id].lock);
}

static inline void
finishzombies(void)
{
  struct idle *i = &idlem[mycpu()->id];

  if (!SLIST_EMPTY(&i->zombies)) {
    struct proc *p, *np;
    acquire(&i->lock);
    SLIST_FOREACH_SAFE(p, &i->zombies, child_next, np) {
      SLIST_REMOVE(&i->zombies, p, proc, child_next);
      finishproc(p);
    }
    release(&i->lock);
  }
}

void
idleloop(void)
{
  // Enabling mtrace calls in scheduler generates many mtrace_call_entrys.
  // mtrace_call_set(1, cpu->id);
#if CODEX
  cprintf("idleloop(): myid()=%d\n", myid());
#endif
  mtstart(idleloop, myproc());

  // XXX: hacky bench for now- figure out how to test more later
//#if CODEX
//  if (myid() != 0) {
//    benchcodex::ap(benchcodex::singleton_testcase());
//  } else {
//    benchcodex::main(benchcodex::singleton_testcase());
//  }
//#endif

  sti();
  for (;;) {
    acquire(&myproc()->lock);
    myproc()->set_state(RUNNABLE);
    sched();
    finishzombies();
    if (steal() == 0) {
        // XXX(Austin) This will prevent us from immediately picking
        // up work that's trying to push itself to this core (pinned
        // thread).  Use an IPI to poke idle cores.
        asm volatile("hlt");
    }
  }
}

void
initidle(void)
{
  struct proc *p = proc::alloc();
  if (!p)
    panic("initidle proc::alloc");

  if (myid() == 0) {
    if (cpuid::features().mwait) {
      // Check smallest and largest line sizes
      auto info = cpuid::mwait();
      assert((u16)info.smallest_line == 0x40);
      assert((u16)info.largest_line == 0x40);
    }
  }

  SLIST_INIT(&idlem->zombies);
  idlem->lock = spinlock("idle_lock", LOCKSTAT_IDLE);

  snprintf(p->name, sizeof(p->name), "idle_%u", myid());
  mycpu()->proc = p;
  myproc()->cpuid = myid();
  myproc()->cpu_pin = 1;
  idlem->cur = p;
}
