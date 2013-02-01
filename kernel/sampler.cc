#include "types.h"
#include "kernel.hh"
#include "spinlock.h"
#include "condvar.h"
#include "fs.h"
#include <uk/stat.h>
#include "kalloc.hh"
#include "file.hh"
#include "bits.hh"
#include "amd64.h"
#include "cpu.hh"
#include "sampler.h"
#include "major.h"
#include "apic.hh"
#include "percpu.hh"

#define LOGHEADER_SZ (sizeof(struct logheader) + \
                      sizeof(((struct logheader*)0)->cpu[0])*NCPU)

static volatile u64 selector;
static volatile u64 period;

static const u64 wd_period = 24000000000ul;

#if defined(HW_josmp) || defined(HW_tom)
static const u64 wd_selector =
  1 << 24 | 
  1 << 22 |
  1 << 20 |
  1 << 17 | 
  1 << 16 | 
  0x76;
#elif defined(HW_ben)
static const u64 wd_selector =
  1 << 24 | 
  1 << 22 |
  1 << 20 |
  1 << 17 | 
  1 << 16 | 
  0x3c;
#else
static const u64 wd_selector = 0;
#endif

static void wdcheck(struct trapframe*);

struct pmu {
  void (*config)(u64 ctr, u64 sel, u64 val);  
  u64 cntval_bits;
  u64 max_period;
};
struct pmu pmu;

struct pmulog {
  u64 count;
  u64 capacity;
  struct pmuevent *event;
  __padout__;
} __mpalign__;

struct pmulog pmulog[NCPU] __mpalign__;

//
// AMD stuff
//
static void
amdconfig(u64 ctr, u64 sel, u64 val)
{
  writemsr(MSR_AMD_PERF_SEL0 + ctr, 0);
  writemsr(MSR_AMD_PERF_CNT0 + ctr, val);
  writemsr(MSR_AMD_PERF_SEL0 + ctr, sel);
}

struct pmu amdpmu = { amdconfig, 48, (1ull << 47) - 1 };

//
// Intel stuff
//
static void
intelconfig(u64 ctr, u64 sel, u64 val)
{
  writemsr(MSR_INTEL_PERF_SEL0 + ctr, 0);
  // Clear the overflow indicator
  writemsr(MSR_INTEL_PERF_GLOBAL_OVF_CTRL, 1<<ctr);
  writemsr(MSR_INTEL_PERF_CNT0 + ctr, val);
  writemsr(MSR_INTEL_PERF_SEL0 + ctr, sel);
}

// We fill in cntval_bits at boot.
// From Intel Arch. Vol 3b:
//   "On write operations, the lower 32-bits of the MSR may be written
//   with any value, and the high-order bits are sign-extended from
//   the value of bit 31."
struct pmu intelpmu = { intelconfig, 0, (1ull << 31) - 1 };

static void
pmuconfig(u64 ctr, u64 sel, u64 val)
{
  if (val > pmu.max_period)
    val = pmu.max_period;
  pmu.config(ctr, sel, -val);
}

void
sampdump(void)
{
  for (int c = 0; c < NCPU; c++) {
    struct pmulog *l = &pmulog[c];    
    cprintf("%u samples %lu\n", c, l->count);
    for (u64 i = 0; i < 4 && i < l->count; i++)
      cprintf(" %lx\n", l->event[i].rip);
  }
}

void
sampconf(void)
{
  pushcli();
  if (selector & PERF_SEL_INT)
    pmulog[myid()].count = 0;
  pmuconfig(0, selector, period);
  popcli();
}

void
sampstart(void)
{
  pushcli();
  for(struct cpu *c = cpus; c < cpus+ncpu; c++) {
    if(c == cpus+mycpu()->id)
      continue;
    lapic->send_sampconf(c);
  }
  sampconf();
  popcli();
}

static int
samplog(struct trapframe *tf)
{
  struct pmulog *l;
  struct pmuevent *e;
  l = &pmulog[mycpu()->id];

  if (l->count == l->capacity)
    return 0;

  e = &l->event[l->count];

  e->idle = (myproc() == idleproc());
  e->rip = tf->rip;
  getcallerpcs((void*)tf->rbp, e->trace, NELEM(e->trace));
  l->count++;
  return 1;
}

int
sampintr(struct trapframe *tf)
{
  int r = 0;

  // Acquire locks that we only acquire during NMI.
  // NMIs are disabled until the next iret.

  // Linux unmasks LAPIC.PC after every interrupt (perf_event.c)
  lapic->mask_pc(false);

  auto overflow = readmsr(MSR_INTEL_PERF_GLOBAL_STATUS);

  if (overflow & (1<<0)) {
    r = 1;
    if (samplog(tf))
      pmuconfig(0, selector, period);
  }

  if (overflow & (1<<1)) {
    r = 1;
    wdcheck(tf);
    pmuconfig(1, wd_selector, wd_period);
  }

  // Clear overflow bits
  writemsr(MSR_INTEL_PERF_GLOBAL_OVF_CTRL, overflow & 3);

  return r;
}

static int
readlog(char *dst, u32 off, u32 n)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  int ret = 0;
  u64 cur = 0;

  for (p = &pmulog[0]; p != q && n != 0; p++) {
    u64 len = p->count * sizeof(struct pmuevent);
    char *buf = (char*)p->event;
    if (cur <= off && off < cur+len) {
      u64 boff = off-cur;
      u64 cc = MIN(len-boff, n);
      memmove(dst, buf+boff, cc);

      n -= cc;
      ret += cc;
      off += cc;
      dst += cc;
    }
    cur += len;
  }

  return ret;
}

static void
sampstat(struct inode *ip, struct stat *st)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  u64 sz = 0;
  
  sz += LOGHEADER_SZ;
  for (p = &pmulog[0]; p != q; p++)
    sz += p->count * sizeof(struct pmuevent);

  st->st_dev = ip->dev;
  st->st_ino = ip->inum;
  st->st_mode = ip->type << __S_IFMT_SHIFT;
  st->st_nlink = ip->nlink();
  st->st_size = sz;
}

static int
sampread(struct inode *ip, char *dst, u32 off, u32 n)
{
  struct pmulog *q = &pmulog[NCPU];
  struct pmulog *p;
  struct logheader *hdr;
  int ret;
  int i;
  
  ret = 0;
  if (off < LOGHEADER_SZ) {
    u64 len = LOGHEADER_SZ;
    u64 cc;
    
    hdr = (logheader*) kmalloc(len, "logheader");
    if (hdr == nullptr)
      return -1;
    hdr->ncpus = NCPU;
    i = 0;
    for (p = &pmulog[0]; p != q; p++) {
      u64 sz = p->count * sizeof(struct pmuevent);
      hdr->cpu[i].offset = len;
      hdr->cpu[i].size = sz;
      len += sz;
      i++;
    }

    cc = MIN(LOGHEADER_SZ-off, n);
    memmove(dst, (char*)hdr + off, cc);
    kmfree(hdr, LOGHEADER_SZ);

    n -= cc;
    ret += cc;
    off += cc;
    dst += cc;
  }

  if (off >= LOGHEADER_SZ)
    ret += readlog(dst, off-LOGHEADER_SZ, n);
  return ret;
}

static int
sampwrite(struct inode *ip, const char *buf, u32 off, u32 n)
{
  struct sampconf *conf;

  if (n != sizeof(*conf))
    return -1;
  conf = (struct sampconf*)buf;

  switch(conf->op) {
  case SAMP_ENABLE:
    selector = conf->selector;
    period = conf->period;
    sampstart();
    break;
  case SAMP_DISABLE:
    selector = 0;
    period = 0;
    sampstart();
    break;
  }

  return n;
}

// Enable PMU Workaround for
// * Intel Errata AAK100 (model 26)
// * Intel Errata AAP53  (model 30)
// * Intel Errata BD53   (model 44)
// Without this, performance counters may fail to count
static void
enable_nehalem_workaround(void)
{
  static const unsigned long magic[4] = {
    0x4300B5,
    0x4300D2,
    0x4300B1,
    0x4300B1
  };

  uint32_t eax;
  cpuid(CPUID_PERFMON, &eax, nullptr, nullptr, nullptr);
  if (PERFMON_EAX_VERSION(eax) == 0)
    return;
  int num = PERFMON_EAX_NUM_COUNTERS(eax);
  if (num > 4)
    num = 4;

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x0);

  for (int i = 0; i < num; i++) {
    writemsr(MSR_INTEL_PERF_SEL0 + i, magic[i]);
    writemsr(MSR_INTEL_PERF_CNT0 + i, 0x0);
  }

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0xf);
  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x0);

  for (int i = 0; i < num; i++) {
    writemsr(MSR_INTEL_PERF_SEL0 + i, 0);
    writemsr(MSR_INTEL_PERF_CNT0 + i, 0);
  }

  writemsr(MSR_INTEL_PERF_GLOBAL_CTRL, 0x3);
}

void
initsamp(void)
{
  if (myid() == 0) {
    u32 name[4];
    char *s = (char *)name;
    name[3] = 0;

    cpuid(0, 0, &name[0], &name[2], &name[1]);
    if (VERBOSE)
      cprintf("%s\n", s);
    if (!strcmp(s, "AuthenticAMD"))
      pmu = amdpmu;
    else if (!strcmp(s, "GenuineIntel")) {
      u32 eax;
      cpuid(CPUID_PERFMON, &eax, 0, 0, 0);
      if (PERFMON_EAX_VERSION(eax) < 2)
        panic("Unsupported performance monitor version %d",
              PERFMON_EAX_VERSION(eax));
      pmu = intelpmu;
      pmu.cntval_bits = PERFMON_EAX_NUM_COUNTERS(eax);
    } else
      panic("Unknown Manufacturer");
  }

  // enable RDPMC at CPL > 0
  u64 cr4 = rcr4();
  lcr4(cr4 | CR4_PCE);
  
  void *p = ksalloc(slab_perf);
  if (p == nullptr)
    panic("initprof: ksalloc");
  pmulog[myid()].event = (pmuevent*) p;
  pmulog[myid()].capacity = PERFSIZE / sizeof(struct pmuevent);

  if (pmu.config == intelpmu.config)
    enable_nehalem_workaround();

  devsw[MAJ_SAMPLER].write = sampwrite;
  devsw[MAJ_SAMPLER].read = sampread;
  devsw[MAJ_SAMPLER].stat = sampstat;
}

//
// watchdog
//

static percpu<int> wd_count;
static spinlock wdlock("wdlock");

static void
wdcheck(struct trapframe* tf)
{
  if (*wd_count == 1) {
    auto l = wdlock.guard();
    // uartputc guarantees some output
    uartputc('W');
    uartputc('D');
    __cprintf(" cpu %u locked up\n", myid());
    __cprintf("  %016lx\n", tf->rip);
    printtrace(tf->rbp);
  }
  ++*wd_count;
}

void
wdpoke(void)
{
  *wd_count = 0;
}

void
initwd(void)
{
  wdpoke();
  pushcli();
  if (wd_selector)
    pmuconfig(1, wd_selector, wd_period);
  popcli();
}
