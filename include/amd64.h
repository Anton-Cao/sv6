#pragma once
// Routines to let C code use special x86 instructions.

#include "types.h"

static inline void
cpuid(u32 info, u32 *eaxp, u32 *ebxp,
      u32 *ecxp, u32 *edxp)
{
  u32 eax, ebx, ecx, edx;
  __asm volatile("cpuid" 
                 : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
                 : "a" (info));
  if (eaxp)
    *eaxp = eax;
  if (ebxp)
    *ebxp = ebx;
  if (ecxp)
    *ecxp = ecx;
  if (edxp)
    *edxp = edx;
}

static inline u8
inb(u16 port)
{
  u8 data = 0;

  __asm volatile("inb %1,%0" : "=a" (data) : "d" (port));
  return data;
}

static inline u16
inw(u16 port)
{
  u16 data = 0;

  __asm volatile("inw %1,%0" : "=a" (data) : "d" (port));
  return data;
}

static inline u32
inl(u16 port)
{
  u32 data = 0;

  __asm volatile("inl %w1,%0" : "=a" (data) : "d" (port));
  return data;
}

static inline void
outb(u16 port, u8 data)
{
  __asm volatile("outb %0,%1" : : "a" (data), "d" (port));
}

static inline void
outw(u16 port, u16 data)
{
  __asm volatile("outw %0,%1" : : "a" (data), "d" (port));
}

static inline void
outl(u16 port, u32 data)
{
  __asm volatile("outl %0,%w1" : : "a" (data), "d" (port));
}

static inline void
stosb(void *addr, int data, int cnt)
{
  __asm volatile("cld; rep stosb" :
                 "=D" (addr), "=c" (cnt) :
                 "0" (addr), "1" (cnt), "a" (data) :
                 "memory", "cc");
}

static inline u32
xchg32(volatile u32 *addr, u32 newval)
{
  u32 result;
  
  // The + in "+m" denotes a read-modify-write operand.
  __asm volatile("lock; xchgl %0, %1" :
                 "+m" (*addr), "=a" (result) :
                 "1" (newval) :
                 "cc");
  return result;
}

static inline u64
xchg(u64 *ptr, u64 val)
{
  __asm volatile(
    "lock; xchgq %0, %1\n\t"
    : "+m" (*ptr), "+r" (val)
    :
    : "memory", "cc");
  return val;
}

static inline u64
readrflags(void)
{
  u64 rflags;
  __asm volatile("pushfq; popq %0" : "=r" (rflags));
  return rflags;
}

static inline void
cli(void)
{
  __asm volatile("cli");
}

static inline void
sti(void)
{
  __asm volatile("sti");
}

static inline void
nop_pause(void)
{
  __asm volatile("pause" : :);
}

static inline void
rep_nop(void)
{
  __asm volatile("rep; nop" ::: "memory");
}

static inline void
lidt(void *p)
{
  __asm volatile("lidt (%0)" : : "r" (p) : "memory");
}

static inline void
lgdt(void *p)
{
  __asm volatile("lgdt (%0)" : : "r" (p) : "memory");
}

static inline void
ltr(u16 sel)
{
  __asm volatile("ltr %0" : : "r" (sel));
}

static inline void
writefs(u16 v)
{
  __asm volatile("movw %0, %%fs" : : "r" (v));
}

static inline u16
readfs(void)
{
  u16 v;
  __asm volatile("movw %%fs, %0" : "=r" (v));
  return v;
}

static inline void
writegs(u16 v)
{
  __asm volatile("movw %0, %%gs" : : "r" (v));
}

static inline u16
readgs(void)
{
  u16 v;
  __asm volatile("movw %%gs, %0" : "=r" (v));
  return v;
}

static inline u64
readmsr(u32 msr)
{
  u32 hi, lo;
  __asm volatile("rdmsr" : "=d" (hi), "=a" (lo) : "c" (msr));
  return ((u64) lo) | (((u64) hi) << 32);
}

static inline void
writemsr(u64 msr, u64 val)
{
  u32 lo = val & 0xffffffff;
  u32 hi = val >> 32;
  __asm volatile("wrmsr" : : "c" (msr), "a" (lo), "d" (hi) : "memory");
}

static inline u64
rdtsc(void)
{
  u32 hi, lo;
  __asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((u64)lo)|(((u64)hi)<<32);
}

static inline u64
rdpmc(u32 ecx)
{
  u32 hi, lo;
  __asm volatile("rdpmc" : "=a" (lo), "=d" (hi) : "c" (ecx));
  return ((u64) lo) | (((u64) hi) << 32);
}


static inline
void hlt(void)
{
  __asm volatile("hlt");
}

static inline u64
rrsp(void)
{
  u64 val;
  __asm volatile("movq %%rsp,%0" : "=r" (val));
  return val;
}

static inline u64
rrbp(void)
{
  u64 val;
  __asm volatile("movq %%rbp,%0" : "=r" (val));
  return val;
}

static inline void
lcr4(u64 val)
{
  __asm volatile("movq %0,%%cr4" : : "r" (val));
}

static inline u64
rcr4(void)
{
  u64 val;
  __asm volatile("movq %%cr4,%0" : "=r" (val));
  return val;
}

static inline void
lcr3(u64 val)
{
  __asm volatile("movq %0,%%cr3" : : "r" (val));
}

static inline u64
rcr3(void)
{
  u64 val;
  __asm volatile("movq %%cr3,%0" : "=r" (val));
  return val;
}

static inline uptr
rcr2(void)
{
  uptr val;
  __asm volatile("movq %%cr2,%0" : "=r" (val));
  return val;
}

static inline void
prefetchw(void *a)
{
  __asm volatile("prefetchw (%0)" : : "r" (a));
}

static inline void
prefetch(void *a)
{
  __asm volatile("prefetch (%0)" : : "r" (a));
}

static inline void
invlpg(void *a)
{
  __asm volatile("invlpg (%0)" : : "r" (a) : "memory");
}

static inline int
popcnt64(u64 v)
{
  u64 val;
  __asm volatile("popcntq %1,%0" : "=r" (val) : "r" (v));
  return val;
}

// Atomically set bit nr of *a.  nr must be <= 64.
static inline void
locked_set_bit(int nr, volatile void *a)
{
  __asm volatile("lock; btsq %1,%0"
                 : "+m" (*(volatile uint64_t*)a)
                 : "lr" (nr)
                 : "memory");
}

// Atomically clear bit nr of *a.  nr must be <= 64.
static inline void
locked_reset_bit(int nr, volatile void *a)
{
  __asm volatile("lock; btrq %1,%0"
                 : "+m" (*(volatile uint64_t*)a)
                 : "lr" (nr)
                 : "memory");
}

// Atomically set bit nr of *a and return its old value
static inline int
locked_test_and_set_bit(int nr, volatile void *a)
{
  int old;
  __asm volatile("lock; btsq %2,%1; sbb %0,%0"
                 : "=r" (old), "+m" (*(volatile uint64_t*)a)
                 : "Ir" (nr)
                 : "memory");
  return old;
}

// Atomically clear bit nr of *a, with release semantics appropriate
// for clearing a lock bit.
static inline void
clear_bit_unlock(int nr, volatile void *a)
{
  __asm volatile("lock; btrq %1,%0"
                 : "+m" (*(volatile uint64_t*)a)
                 : "Ir" (nr)
                 : "memory");
}

// Layout of the trap frame built on the stack by the
// hardware and by trapasm.S, and passed to trap().
// Also used by sysentry (but sparsely populated).
struct trapframe {
  u16 padding3[7];
  u16 ds;

  // amd64 ABI callee saved registers
  u64 r15;
  u64 r14;
  u64 r13;
  u64 r12;
  u64 rbp;
  u64 rbx;

  // amd64 ABI caller saved registers
  u64 r11;
  u64 r10;
  u64 r9;
  u64 r8;
  u64 rax;
  u64 rcx;
  u64 rdx;
  u64 rsi;
  u64 rdi;
  u64 trapno;

  // Below here defined by amd64 hardware
  u32 err;
  u16 padding2[2];
  u64 rip;
  u16 cs;
  u16 padding1[3];
  u64 rflags;
  // Unlike 32-bit, amd64 hardware always pushes below
  u64 rsp;
  u16 ss;
  u16 padding0[3];
} __attribute__((packed));
