#pragma once

#include "queue.h"
#ifdef __cplusplus
#include "cpputil.hh"           // For NEW_DELETE_OPS
#endif

struct condvar {
  struct spinlock lock;
  LIST_HEAD(waiters, proc) waiters;

#ifdef __cplusplus
  // Construct an uninitialized condvar.  This should be move-assigned
  // from an initialized condvar before being used.  This is
  // constexpr, so it can be used for global condvars without
  // incurring a static constructor.
  constexpr condvar()
    : lock(), waiters{} { }

  constexpr
  condvar(const char *name)
    : lock(name, LOCKSTAT_CONDVAR), waiters{} { }

  // Condvars cannot be copied.
  condvar(const condvar &o) = delete;
  condvar &operator=(const condvar &o) = delete;

  // Condvars can be moved.
  condvar(condvar &&o) = default;
  condvar &operator=(condvar &&o) = default;

  NEW_DELETE_OPS(condvar);
#endif // __cplusplus
};

void            cv_sleep(struct condvar *cv, struct spinlock*);
void            cv_sleepto(struct condvar *cv, struct spinlock*, u64);
void            cv_wakeup(struct condvar *cv);
void            timerintr(void);
u64             nsectime(void);
