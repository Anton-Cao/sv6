#include "types.h"
#include "amd64.h"
#include "kernel.hh"
#include "cpu.hh"
#include "gc.hh"
#include "percpu.hh"
#include "spinlock.h"
#include "condvar.h"
#include "proc.hh"
#include "uwq.hh"
#include "vm.hh"
#include "kalloc.hh"
#include "bits.hh"
#include "rnd.hh"
extern "C" {
#include "kern_c.h"
}

bool
uwq_trywork(void)
{
  // Returning true means uwq added a thread to the run queue

  u64 i, k;

  // A "random" victim CPU
  k = rnd();
  for (i = 0; i < NCPU; i++) {
    u64 j = (i+k) % NCPU;

    if (j == myid())
      continue;
    struct cpu *c = &cpus[j];
    
    // The gc_epoch is for uwq
    scoped_gc_epoch xgc();
    barrier();

    uwq* uwq = c->uwq;
    if (uwq == nullptr)
      continue;

    if (uwq->haswork()) {
      if (uwq->tryworker())
        return true;
      break;
    }
  }

  return false;
}

//SYSCALL
int
sys_wqinit(uptr uentry)
{
  uwq* uwq;

  if (myproc()->uwq != nullptr)
    return -1;

  uwq = uwq::alloc(myproc()->pgmap, myproc()->vmap, myproc()->ftable, uentry);
  if (uwq == nullptr)
    return -1;

  myproc()->uwq = uwq;
  return 0;
}

//SYSCALL
int
sys_wqwait(void)
{
  uwq_worker* w = myproc()->worker;
  if (w == nullptr)
    return -1;

  return w->wait();
}

//
// uwq_worker
//
uwq_worker::uwq_worker(uwq* u, proc* p)
  : uwq_(u), proc_(p), running_(false),
    lock_("worker_lock"), cv_("worker_cv")
{
}

void
uwq_worker::exit(void)
{
  if (--uwq_->uref_ == 0)
    gc_delayed(uwq_);
  release(&lock_);
  delete this;
  ::exit();
}

long
uwq_worker::wait(void)
{
  acquire(&lock_);
  if (!uwq_->valid())
    this->exit();

  running_ = false;
  cv_.sleep(&lock_);

  if (!uwq_->valid())
    this->exit();
  release(&lock_);
  return 0;
}

//
// uwq
//
uwq*
uwq::alloc(proc_pgmap* pgmap, vmap* vmap, filetable *ftable, uptr uentry)
{
  uwq_ipcbuf* ipc;
  uwq* u;

  ipc = (uwq_ipcbuf*) ksalloc(slab_userwq);  
  if (ipc == nullptr)
    return nullptr;

  ftable->incref();
  vmap->incref();
  pgmap->inc();

  u = new uwq(pgmap, vmap, ftable, ipc, uentry);
  if (u == nullptr) {
    ftable->decref();
    vmap->decref();
    pgmap->dec();
    ksfree(slab_userwq, ipc);
    return nullptr;
  }

  if (mapkva(pgmap->pml4, (char*)ipc, USERWQ, USERWQSIZE)) {
    u->dec();
    return nullptr;
  }

  return u;
}

uwq::uwq(proc_pgmap* pgmap, vmap* vmap,
         filetable* ftable, uwq_ipcbuf* ipc, uptr uentry) 
  : rcu_freed("uwq"), lock_("uwq_lock"),
    pgmap_(pgmap), vmap_(vmap), ftable_(ftable), ipc_(ipc),
    uentry_(uentry), ustack_(UWQSTACK), uref_(0)
{
  for (int i = 0; i < NELEM(ipc_->len); i++)
    ipc_->len[i].v_ = 0;

  for (int i = 0; i < NWORKERS; i++)
    worker_[i].store(nullptr);
}

uwq::~uwq(void)
{ 
  ksfree(slab_userwq, ipc_);
  pgmap_->dec();
  vmap_->decref();
  ftable_->decref();
}

bool
uwq::haswork(void) const
{
  for (int i = 0; i < NELEM(ipc_->len); i++) {
    if (ipc_->len[i].v_ > 0) {
      return true;
    }
  }
  return false;
}

bool
uwq::tryworker(void)
{
  if (!valid())
    return false;

  // Try to start a worker thread
  scoped_acquire lock0(&lock_);

  for (int i = 0; i < NWORKERS; i++) {
    if (worker_[i] == nullptr)
      continue;

    uwq_worker *w = worker_[i];
    if (w->running_)
      continue;
    else {
      scoped_acquire lock1(&w->lock_);
      proc* p = w->proc_;

      acquire(&p->lock);
      p->cpuid = myid();
      release(&p->lock);

      w->running_ = true;
      w->cv_.wake_all();
      return true;
    }
  }
  lock0.release();

again:
  u64 uref = uref_.load();
  if (uref < ipc_->maxworkers) {
    if (!cmpxch(&uref_, uref, uref+1))
      goto again;
    // Guaranteed to have  slot in worker_

    proc* p = allocworker();
    if (p != nullptr) {
      uwq_worker* w = new uwq_worker(this, p);
      assert(w != nullptr);

      ++uref_;
      p->worker = w;
      w->running_ = true;

      acquire(&p->lock);
      p->cpuid = myid();
      addrun(p);
      release(&p->lock);

      for (int i = 0; i < NWORKERS; i++) {
        if (worker_[i] == nullptr)
          if (cmpxch(&worker_[i], (uwq_worker*)nullptr, w))
            return true;
      }
      panic("uwq::tryworker: oops");
    }
  }
    
  return false;
}

void
uwq::finish(void)
{
  bool gcnow = true;

  scoped_acquire lock0(&lock_);
  for (int i = 0; i < NWORKERS; i++) {
    if (worker_[i] != nullptr) {
      uwq_worker* w = worker_[i];
      gcnow = false;
      acquire(&w->lock_);
      w->cv_.wake_all();
      release(&w->lock_);
    }
  }
  
  if (gcnow)
    gc_delayed(this);
}

void
uwq::onzero() const
{
  uwq *u = (uwq*)this;
  u->finish();
}

proc*
uwq::allocworker(void)
{
  uptr uentry = uentry_;

  if (uentry == 0)
    return nullptr;

  proc* p = proc::alloc();
  if (p == nullptr)
    return nullptr;
  safestrcpy(p->name, "uwq_worker", sizeof(p->name));

  // finishproc will drop these refs
  vmap_->incref();
  ftable_->incref();
  
  p->vmap = vmap_;
  p->ftable = ftable_;
    
  struct vmnode *vmn;
  if ((vmn = new vmnode(USTACKPAGES)) == nullptr) {
    finishproc(p);
    return nullptr;
  }

  // Include a bumper page
  uptr ustack = ustack_.fetch_add((USTACKPAGES*PGSIZE)+PGSIZE);
  uptr stacktop = ustack + (USTACKPAGES*PGSIZE);
  if (vmap_->insert(vmn, ustack, 1, pgmap_) < 0) {
    delete vmn;
    finishproc(p);
    return nullptr;
  }

  p->tf->rsp = stacktop - 8;
  p->tf->rip = uentry;
  p->tf->cs = UCSEG | 0x3;
  p->tf->ds = UDSEG | 0x3;
  p->tf->ss = p->tf->ds;
  p->tf->rflags = FL_IF;

  return p;
}
