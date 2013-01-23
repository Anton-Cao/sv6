#include "atomic.hh"

class filetable {
public:
  static filetable* alloc() {
    return new filetable();
  }

  filetable* copy() {
    filetable* t = alloc();

    for(int fd = 0; fd < NOFILE; fd++) {
      sref<file> f;
      if (getfile(fd, &f))
        t->ofile_[fd].store(f->dup());
      else
        t->ofile_[fd].store(nullptr);
    }
    return t;
  }
  
  bool getfile(int fd, sref<file> *sf) {
    if (fd < 0 || fd >= NOFILE)
      return false;

    scoped_gc_epoch gc;
    file* f = ofile_[fd];
    if (!f || !sf->init_nonzero(f))
      return false;
    return true;
  }

  int allocfd(struct file *f) {
    for (int fd = 0; fd < NOFILE; fd++)
      if (ofile_[fd] == nullptr && cmpxch(&ofile_[fd], (file*)nullptr, f))
        return fd;
    cprintf("filetable::allocfd: failed\n");
    return -1;
  }

  void close(int fd) {
    // XXX(sbw) if f->ref_ > 1 the kernel will not actually close 
    // the file when this function returns (i.e. sys_close can return 
    // while the file/pipe/socket is still open).
    if (fd >= NOFILE) {
      cprintf("filetable::close: bad fd %u\n", fd);
      return;
    }

    file* f = ofile_[fd].exchange(nullptr);
    if (f != nullptr)
      f->dec();
    else
      cprintf("filetable::close: bad fd %u\n", fd);
  }

  void decref() {
    if (--ref_ == 0)
      delete this;
  }

  void incref() {
    ref_++;
  }

private:
  filetable() : ref_(1) {
    for(int fd = 0; fd < NOFILE; fd++)
      ofile_[fd].store(nullptr);
  }

  ~filetable() {
    for(int fd = 0; fd < NOFILE; fd++){
      if (ofile_[fd].load() != nullptr) {
        ofile_[fd].load()->dec();
        ofile_[fd] = nullptr;
      }
    }
  }

  filetable& operator=(const filetable&);
  filetable(const filetable& x);
  NEW_DELETE_OPS(filetable);  

  std::atomic<file*> ofile_[NOFILE];
  std::atomic<u64> ref_;
};
