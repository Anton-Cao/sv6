#include "gc.hh"
#include "atomic.hh"
#include "cpputil.hh"

using std::atomic;

struct buf : public rcu_freed {
  atomic<int> flags;
  const u32 dev;
  const u64 sector;
  struct buf *prev; // LRU cache list
  struct buf *next;
  struct buf *qnext; // disk queue
  char lockname[16];
  struct condvar cv;
  struct spinlock lock;
  u8 data[512];

  buf(u32 d, u64 s) : rcu_freed("buf"), flags(0), dev(d), sector(s) {
    snprintf(lockname, sizeof(lockname), "cv:buf:%d", sector);
    lock = spinlock(lockname+3, LOCKSTAT_BIO);
    cv = condvar(lockname);
  }

  static buf* get(u32 dev, u64 sector);
  static buf* write_get(u32 dev, u64 sector);
  void write_lock();
  void write_release();
  void write();

  virtual void do_gc() { delete this; }
  NEW_DELETE_OPS(buf)
};
#define B_BUSY  0x1  // buffer is locked by some process
#define B_VALID 0x2  // buffer has been read from disk
#define B_DIRTY 0x4  // buffer needs to be written to disk

