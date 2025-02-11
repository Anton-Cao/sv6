#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "amd64.h"
#include "cpu.hh"
#include "traps.h"
#include "spinlock.hh"
#include "condvar.hh"
#include "proc.hh"
#include "kmtrace.hh"
#include "bits.hh"
#include "kalloc.hh"
#include "apic.hh"
#include "irq.hh"
#include "kstream.hh"
#include "hwvm.hh"
#include "refcache.hh"
#include "cpuid.hh"
#include "linearhash.hh"
#include "vm.hh"

extern "C" void __uaccess_end(void);

struct intdesc idt[256] __attribute__((aligned(4096)));

static char fpu_initial_state[FXSAVE_BYTES];

// boot.S
extern u64 trapentry[];

static struct irq_info
{
  irq_handler *handlers;
  // True if this IRQ has been allocated to a device
  bool in_use;
} irq_info[256 - T_IRQ0];

// Instruction pointers that cause transparent world barriers.
linearhash<u64, u64> wm_rips(1024);

// Addresses that trigger transparent world barriers.
linearhash<u64, u64> wm_addrs(1024);

static void trap(struct trapframe *tf);

u64
sysentry_c(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 num)
{
  if(myproc()->killed) {
    mtstart(trap, myproc());
    exit(-1);
  }

  trapframe *tf = (trapframe*) (myproc()->kstack + KSTACKSIZE - sizeof(*tf));
  myproc()->tf = tf;
  u64 r = syscall(a0, a1, a2, a3, a4, a5, num);

  if(myproc()->killed) {
    mtstart(trap, myproc());
    exit(-1);
  }

  return r;
}

int
do_pagefault(struct trapframe *tf)
{
  uptr addr = rcr2();
  if (((tf->cs&3) == 0 || myproc() == 0) &&
      !mycpu()->has_secrets && addr >= KGLOBAL) {
    // Page fault was probably caused by trying to access secret
    // data so map all secrets in now.
    switch_to_kstack();

    if (pagefault(myproc()->vmap.get(), addr, tf->err) < 0) {
      // We tried to lazily load the mapping, but it wasn't marked as quasi
      // user-visible. Record the event and continue.
      wm_addrs.increment(addr);
      wm_rips.increment(tf->rip);
    }

    return 0;
  } else if (myproc()->uaccess_) {
    if (addr >= USERTOP)
      panic("do_pagefault: %lx", addr);

    sti();
    if(pagefault(myproc()->vmap.get(), addr, tf->err) >= 0){
      return 0;
    }
    console.println("pagefault accessing user address from kernel (addr ",
                    (void*)addr, " rip ", (void*)tf->rip, ")");
    tf->rax = -1;
    tf->rip = (u64)__uaccess_end;
    return 0;
  } else if (tf->err & FEC_U) {
      sti();
      if(addr < USERTOP && pagefault(myproc()->vmap.get(), addr, tf->err) >= 0){
        return 0;
      }
      uerr.println("pagefault from user for ", shex(addr),
                   " err ", (int)tf->err);
      cli();
  }
  return -1;
}

static inline void
lapiceoi()
{
  lapic->eoi();
}

namespace {
  DEFINE_PERCPU(uintptr_t, nmi_lastpc);
  DEFINE_PERCPU(int, nmi_swallow);
}

// C/C++ entry point for traps; called by assembly trap stub
extern "C" void
trap_c(struct trapframe *tf)
{
  if (tf->trapno == T_NMI) {
    ensure_secrets();

    // An NMI can come in after popcli() drops ncli to zero and intena
    // is 1, but before popcli() checks intena and calls sti.  If the
    // NMI handler acquires any lock, acquire() will call pushcli(),
    // which will set intena to 0, and upon return from the NMI, the
    // preempted popcli will observe intena=0 and fail to sti.
    int intena_save = mycpu()->intena;

    // The only locks that we can acquire during NMI are ones
    // we acquire only during NMI.

    // NMIs are tricky.  On the one hand, they're edge triggered,
    // which means we're not guaranteed to get an NMI interrupt for
    // every NMI event, so we have to proactively handle all of the
    // NMI sources we can.  On the other hand, they're also racy,
    // since an NMI source may successfully queue an NMI behind an
    // existing NMI, but we may detect that source when handling the
    // first NMI.  Our solution is to detect back-to-back NMIs and
    // keep track of how many NMI sources we've handled: as long as
    // the number of back-to-back NMIs in a row never exceeds the
    // number of NMI sources we've handled across these back-to-back
    // NMIs, we're not concerned, even if an individual NMI doesn't
    // detect any active sources.

    // Is this a back-to-back NMI?  If so, we might have handled all
    // of the NMI sources already.
    bool repeat = (*nmi_lastpc == tf->rip);
    *nmi_lastpc = tf->rip;
    if (!repeat)
      *nmi_swallow = 0;

    // Handle NMIs
    int handled = 0;
    handled += sampintr(tf);

    // No lapiceoi because only fixed delivery mode interrupts need to
    // be EOI'd (and fixed mode interrupts can't be programmed to
    // produce an NMI vector).

    if (handled == 0 && !*nmi_swallow)
      panic("NMI");

    // This NMI accounts for one handled event, so we can swallow up
    // to handled - 1 more back-to-back NMIs after this one.
    *nmi_swallow += handled - 1;

    mycpu()->intena = intena_save;
    return;
  }

  if (tf->trapno == T_DBLFLT) {
    ensure_secrets();
    kerneltrap(tf);
  }

  // if (tf->cs != KCSEG) {
  // switch_to_kstack();
  // }

#if MTRACE
  if (myproc()->mtrace_stacks.curr >= 0)
    mtpause(myproc());
  mtstart(trap, myproc());
  // XXX mt_ascope ascope("trap:%d", tf->trapno);
#endif

  trap(tf);

#if MTRACE
  mtstop(myproc());
  if (myproc()->mtrace_stacks.curr >= 0)
    mtresume(myproc());
#endif
}

static void
trap(struct trapframe *tf)
{
  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    kstats::inc(&kstats::sched_tick_count);
    // for now, just care about timer interrupts
#if CODEX
    codex_magic_action_run_async_event(T_IRQ0 + IRQ_TIMER);
#endif
    if (mycpu()->timer_printpc) {
      cprintf("cpu%d: proc %s rip %lx rsp %lx cs %x\n",
              mycpu()->id,
              myproc() ? myproc()->name : "(none)",
              tf->rip, tf->rsp, tf->cs);
      if (mycpu()->timer_printpc == 2 && tf->rbp > KBASE) {
        uptr pc[10];
        getcallerpcs((void *) tf->rbp, pc, NELEM(pc));
        for (int i = 0; i < 10 && pc[i]; i++)
          cprintf("cpu%d:   %lx\n", mycpu()->id, pc[i]);
      }
      mycpu()->timer_printpc = 0;
    }
    if (mycpu()->id == 0)
      timerintr();
    refcache::mycache->tick();
    lapiceoi();
    if (mycpu()->no_sched_count) {
      kstats::inc(&kstats::sched_blocked_tick_count);
      // Request a yield when no_sched_count is released.  We can
      // modify this without protection because interrupts are
      // disabled.
      mycpu()->no_sched_count |= NO_SCHED_COUNT_YIELD_REQUESTED;
      return;
    }
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + IRQ_COM2:
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    piceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%lx\n",
            mycpu()->id, tf->cs, tf->rip);
    // [Intel SDM 10.9 Spurious Interrupt] The spurious interrupt
    // vector handler should return without an EOI.
    //lapiceoi();
    break;
  case T_IRQ0 + IRQ_ERROR:
    cprintf("cpu%d: lapic error?\n", mycpu()->id);
    lapiceoi();
    break;
  case T_TLBFLUSH: {
    lapiceoi();
    mmu::shootdown::on_ipi();
    break;
  }
  case T_SAMPCONF:
    lapiceoi();
    sampconf();
    break;
  case T_IPICALL: {
    extern void on_ipicall();
    lapiceoi();
    on_ipicall();
    break;
  }
  case T_DEVICE: {
    // Clear "task switched" flag to enable floating-point
    // instructions.  sched will set this again when it switches
    // tasks.
    clts();
    // Save current FPU state
    // XXX(Austin) This process could exit and free its fpu_state, but
    // scoped_gc_epoch breaks if I use it here.
    // XXX(Austin) Do I need to FWAIT first?
    struct proc *fpu_owner = mycpu()->fpu_owner;
    if (fpu_owner) {
      assert(fpu_owner->fpu_state);
      fxsave(fpu_owner->fpu_state);
    }
    // Lazily allocate myproc's FPU state
    if (!myproc()->fpu_state) {
      myproc()->fpu_state = kmalloc(FXSAVE_BYTES, "(fxsave)");
      if (!myproc()->fpu_state) {
        console.println("out of memory allocating fxsave region");
        myproc()->killed = 1;
        break;
      }
      memmove(myproc()->fpu_state, &fpu_initial_state, FXSAVE_BYTES);
    }
    // Restore myproc's FPU state
    fxrstor(myproc()->fpu_state);
    mycpu()->fpu_owner = myproc();
    break;
  }
  default:
    if (tf->trapno >= T_IRQ0 && irq_info[tf->trapno - T_IRQ0].handlers) {
      for (auto h = irq_info[tf->trapno - T_IRQ0].handlers; h; h = h->next)
        h->handle_irq();
      lapiceoi();
      piceoi();
      return;
    }

    if (tf->trapno == T_PGFLT) {
      if (do_pagefault(tf) == 0)
        return;

      // XXX distinguish between SIGSEGV and SIGBUS?
      if (myproc()->deliver_signal(SIGSEGV))
        return;
    }

    if (myproc() == 0 || (tf->cs&3) == 0)
      kerneltrap(tf);

    // In user space, assume process misbehaved.
    uerr.println("pid ", myproc()->pid, ' ', myproc()->name,
                 ": trap ", (u64)tf->trapno, " err ", (u32)tf->err,
                 " on cpu ", myid(), " rip ", shex(tf->rip),
                 " rsp ", shex(tf->rsp), " addr ", shex(rcr2()),
                 "--kill proc");
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    exit(-1);

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->get_state() == RUNNING &&
     (tf->trapno == T_IRQ0+IRQ_TIMER || myproc()->yield_)) {
    yield();
  }

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == 0x3)
    exit(-1);
}

void
inittrap(void)
{
  u64 entry;
  u8 bits;
  int i;

  bits = INT_P | SEG_INTR64;  // present, interrupt gate
  for(i=0; i<256; i++) {
    entry = trapentry[i];
    idt[i] = INTDESC(KCSEG, entry, bits);
  }

  // Conservatively reserve all legacy IRQs.  This might cause us to
  // not be able to configure a device.
  for (int i = 0; i < 16; ++i)
    irq_info[i].in_use = true;
  // Also reserve the spurious vector
  irq_info[IRQ_SPURIOUS].in_use = true;
  // And reserve interrupt 255 (Intel SDM Vol. 3 suggests this can't
  // be used for MSI).
  irq_info[255 - T_IRQ0].in_use = true;
}

void
initfpu(void)
{
  // Allow ourselves to use FPU instructions.  We'll clear this before
  // we schedule anything.
  lcr0(rcr0() & ~(CR0_TS | CR0_EM));
  // Initialize FPU, ignoring pending FP exceptions
  fninit();
  // Don't generate interrupts for any SSE exceptions
  ldmxcsr(0x1f80);
  // Stash away the initial FPU state to use as each process' initial
  // FPU state
  if (myid() == 0)
    fxsave(&fpu_initial_state);
}

void
initmsr(void)
{
  // XXX Where should this code live?

#if defined(DISABLE_PREFETCH_STREAM)
#define CONTROL_PREFETCH_STREAM 1
#else
#define CONTROL_PREFETCH_STREAM 0
#define DISABLE_PREFETCH_STREAM 0
#endif
#if defined(DISABLE_PREFETCH_ADJ)
#define CONTROL_PREFETCH_ADJ 1
#else
#define CONTROL_PREFETCH_ADJ 0
#define DISABLE_PREFETCH_ADJ 0
#endif

  if (CONTROL_PREFETCH_STREAM || CONTROL_PREFETCH_ADJ) {
    // Is the MISC_FEATURE_CONTROL MSR valid?
    auto m = cpuid::model();
    if (!(cpuid::vendor_is_intel() && m.family == 6 &&
          (m.model == 0x1a || m.model == 0x1e || m.model == 0x1f || // Nehalem
           m.model == 0x25 || m.model == 0x2c || // Westmere
           m.model == 0x2e || // Nehalem-EX
           m.model == 0x2f)))  // Westmere-EX
      panic("Cannot control hardware prefetcher for this CPU model");

    uint64_t mfc = readmsr(MSR_INTEL_MISC_FEATURE_CONTROL);

    if (DISABLE_PREFETCH_STREAM)
      mfc |= MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_STREAMER;
    else if (CONTROL_PREFETCH_STREAM)
      mfc &= ~MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_STREAMER;

    if (DISABLE_PREFETCH_ADJ)
      mfc |= MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_SPATIAL;
    else if (CONTROL_PREFETCH_ADJ)
      mfc &= ~MSR_INTEL_MISC_FEATURE_CONTROL_DISABLE_MLC_SPATIAL;

    writemsr(MSR_INTEL_MISC_FEATURE_CONTROL, mfc);

    if (myid() == 0) {
      if (CONTROL_PREFETCH_STREAM)
        cprintf("msr: MLC stream prefetcher %s\n",
                DISABLE_PREFETCH_STREAM ? "disabled" : "enabled");
      if (CONTROL_PREFETCH_ADJ)
        cprintf("msr: Adjacent cache line prefetcher %s\n",
                DISABLE_PREFETCH_ADJ ? "disabled" : "enabled");
    }

    // XXX There are also the DCU prefetchers.  ben's BIOS doesn't
    // disable these when I set "Hardware prefetcher" to disable, so
    // I'm not convinced the bits are right.
  }
}

void
initnmi(void)
{
  void *nmistackbase = kalloc("kstack", KSTACKSIZE);
  mycpu()->ts.ist[1] = (u64) nmistackbase + KSTACKSIZE;

  if (mycpu()->id == 0)
    idt[T_NMI].ist = 1;
}

void
initdblflt(void)
{
  void *dfaultstackbase = kalloc("kstack", KSTACKSIZE);
  mycpu()->ts.ist[2] = (uintptr_t)dfaultstackbase + KSTACKSIZE;
  if (mycpu()->id == 0)
    idt[T_DBLFLT].ist = 2;
}

void
initseg(struct cpu *c)
{
  volatile struct desctr dtr;

  dtr.limit = sizeof(idt) - 1;
  dtr.base = (u64)idt;
  lidt((void *)&dtr.limit);

  // Load per-CPU GDT
  memmove(c->gdt, bootgdt, sizeof(bootgdt));
  dtr.limit = sizeof(c->gdt) - 1;
  dtr.base = (u64)c->gdt;
  lgdt((void *)&dtr.limit);

  __asm volatile("movl $0, %%eax\n"
                 "movw %%ax, %%es\n"
                 "movw %%ax, %%ss\n"
                 "movw %%ax, %%ds" : : : "eax");

  // When executing a syscall instruction the CPU sets the SS selector
  // to (star >> 32) + 8 and the CS selector to (star >> 32).
  // When executing a sysret instruction the CPU sets the SS selector
  // to (star >> 48) + 8 and the CS selector to (star >> 48) + 16.
  u64 star = ((((u64)UCSEG|0x3) - 16)<<48)|((u64)KCSEG<<32);
  writemsr(MSR_STAR, star);
  writemsr(MSR_LSTAR, (u64)&sysentry);
  writemsr(MSR_SFMASK, FL_TF | FL_IF);
}

// Pushcli/popcli are like cli/sti except that they are matched:
// it takes two popcli to undo two pushcli.  Also, if interrupts
// are off, then pushcli, popcli leaves them off.
void
pushcli(void)
{
  u64 rflags;

  rflags = readrflags();
  cli();
  if(mycpu()->ncli++ == 0)
    mycpu()->intena = rflags & FL_IF;
}

void
popcli(void)
{
  if(readrflags()&FL_IF)
    panic("popcli - interruptible");
  if(--mycpu()->ncli < 0)
    panic("popcli");
  if(mycpu()->ncli == 0 && mycpu()->intena)
    sti();
}

// Record the current call stack in pcs[] by following the %rbp chain.
void
getcallerpcs(void *v, uptr pcs[], int n)
{
  uintptr_t rbp;
  int i;

  rbp = (uintptr_t)v;
  for(i = 0; i < n; i++){
    // Read saved %rip
    uintptr_t saved_rip;
    if (safe_read_vm(&saved_rip, rbp + sizeof(uintptr_t), sizeof(saved_rip)) !=
        sizeof(saved_rip))
      break;
    // Subtract 1 so it points to the call instruction
    pcs[i] = saved_rip - 1;
    // Read saved %rbp
    if (safe_read_vm(&rbp, rbp, sizeof(rbp)) != sizeof(rbp))
      break;
  }
  for(; i < n; i++)
    pcs[i] = 0;
}

bool
irq::reserve(const int *accept_gsi, size_t num_accept)
{
  assert(!valid());
  int gsi = -1;
  if (accept_gsi) {
    for (size_t i = 0; i < num_accept; ++i) {
      if (!irq_info[accept_gsi[i]].in_use) {
        gsi = accept_gsi[i];
        break;
      }
    }
  } else {
    // Find a free GSI.  Start from the top because system-assigned
    // GSI's tend to be low.
    for (int try_gsi = sizeof(irq_info) / sizeof(irq_info[0]) - 1; try_gsi >= 0;
         --try_gsi) {
      if (!irq_info[try_gsi].in_use) {
        gsi = try_gsi;
        break;
      }
    }
  }
  if (gsi == -1)
    // XXX Level-triggered, active-low interrupts can share an IRQ line
    return false;
  irq_info[gsi].in_use = true;
  this->gsi = gsi;
  vector = T_IRQ0 + gsi;
  return true;
}

void
irq::register_handler(irq_handler *handler)
{
  assert(valid());
  assert(vector == gsi + T_IRQ0);
  handler->next = irq_info[gsi].handlers;
  irq_info[gsi].handlers = handler;
}

void
to_stream(class print_stream *s, const struct irq &irq)
{
  if (irq.valid()) {
    s->print("IRQ ", irq.gsi);
    if (irq.level_triggered)
      s->print(irq.active_low ? " (level low)" : " (level high)");
    else
      s->print(irq.active_low ? " (falling edge)" : " (rising edge)");
  } else {
    s->print("invalid IRQ");
  }
}

void
scoped_critical::release_yield()
{
  kstats::inc(&kstats::sched_delayed_tick_count);
  // Clear the yield request and yield
  modify_no_sched_count(-NO_SCHED_COUNT_YIELD_REQUESTED);
  // Below here is racy, strictly speaking, but that's okay.
  yield();
}

bool
check_critical(critical_mask mask)
{
  if (mask == NO_CRITICAL)
    return true;
  bool safe = !(readrflags() & FL_IF);
  if (mask & NO_INT)
    return safe;
  safe = safe || mycpu()->no_sched_count;
  if (mask & NO_SCHED)
    return safe;
  safe = safe || myproc()->cpu_pin;
  if (mask & NO_MIGRATE)
    return safe;
  return false;
}
