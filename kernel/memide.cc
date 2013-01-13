// Fake IDE disk; stores blocks in memory.
// Useful for running kernel without scratch disk.

#include "types.h"
#include "mmu.h"
#include "kernel.hh"
#include "spinlock.h"
#include "queue.h"
#include "condvar.h"
#include "proc.hh"
#include "amd64.h"
#include "traps.h"

#include "buf.hh"

extern u8 _fs_img_start[];
extern u64 _fs_img_size;

static u64 disksize;
static u8 *memdisk;

void
initdisk(void)
{
  memdisk = _fs_img_start;
  disksize = _fs_img_size/512;
}

// Interrupt handler.
void
ideintr(void)
{
  // no-op
}

// Sync buf with disk. 
// If B_DIRTY is set, write buf to disk, clear B_DIRTY, set B_VALID.
// Else if B_VALID is not set, read buf from disk, set B_VALID.
void
iderw(struct buf *b)
{
  u8 *p;

  if(!(b->flags_ & B_BUSY))
    panic("iderw: buf not busy");
  if((b->flags_ & (B_VALID|B_DIRTY)) == B_VALID)
    panic("iderw: nothing to do");
  if(b->dev_ != 1)
    panic("iderw: request not for disk 1");
  if(b->sector_ >= disksize)
    panic("iderw: sector out of range");

  p = memdisk + b->sector_*512;
  
  if(b->flags_ & B_DIRTY){
    b->flags_ &= ~B_DIRTY;
    memmove(p, b->data, 512);
  } else
    memmove(b->data, p, 512);
  b->flags_ |= B_VALID;
}
