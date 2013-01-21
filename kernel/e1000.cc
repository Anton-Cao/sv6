#include "types.h"
#include "amd64.h"
#include "kernel.hh"
#include "pci.hh"
#include "pcireg.hh"
#include "spinlock.h"
#include "apic.hh"
#include "irq.hh"
#include "e1000reg.hh"
#include "kstream.hh"

#define TX_RING_SIZE 64
#define RX_RING_SIZE 64

irq e1000irq;
int e1000init;

struct e1000_model;

static struct e1000 {
  u32 membase;
  u32 iobase;
  const struct e1000_model *model;
  
  volatile u32 txclean;
  volatile u32 txinuse;

  volatile u32 rxclean;
  volatile u32 rxuse;

  u8 hwaddr[6];

  struct wiseman_txdesc txd[TX_RING_SIZE] __attribute__((aligned (16)));
  struct wiseman_rxdesc rxd[RX_RING_SIZE] __attribute__((aligned (16)));

  struct spinlock lk;

  e1000() : lk("e1000", true) { }
} e1000;

struct eerd {
  u32 data_shift;
  u32 addr_shift;
  u32 done_bit;
  u32 start_bit;
};

static const struct eerd eerd_default = {
  data_shift: 16,
  addr_shift: 8,
  done_bit:   0x00000010,
  start_bit:  0x00000001,
};

// 82541xx, 82571xx, 82572xx, all PCI-E
static const struct eerd eerd_large = {
  data_shift: 16,
  addr_shift: 2,
  done_bit:   0x00000002,
  start_bit:  0x00000001
};

enum {
  MODEL_FLAG_DUAL_PORT = 1 << 0,
  MODEL_FLAG_PCIE = 1 << 1,
};

static struct e1000_model
{
  const char *name;
  int devid;
  const struct eerd *eerd;
  int flags;
} e1000_models[] = {
  {
    // QEMU's E1000 model
    "82540EM (desktop)", 0x100e,
    &eerd_default,
  }, {
    // josmp
    "82546GB (copper; dual port)", 0x1079,
    &eerd_default,
    MODEL_FLAG_DUAL_PORT,
  },
  // This is disabled on tom because it interferes with the IPMI
  // interface.  We don't need it because tom also has an E1000e
  // (below).
#ifndef HW_tom
  {
    // tom
    "82541PI (copper)", 0x1076,
    &eerd_large,
  },
#endif
  {
    // ud0
    "82573E (copper)", 0x108c,
    &eerd_large,
    MODEL_FLAG_PCIE,
  }, {
    // Also ud0
    "82573L", 0x100a,
    &eerd_large,
    MODEL_FLAG_PCIE,
  }, {
    // tom and ben
    "82572EI (copper)", 0x107d,
    &eerd_large,
    MODEL_FLAG_PCIE,
  },
};

static inline u32
erd(u32 reg)
{
  paddr pa = e1000.membase + reg;
  volatile u32 *ptr = (u32*) p2v(pa);
  return *ptr;
}

static inline void
ewr(u32 reg, u32 val)
{
  paddr pa = e1000.membase + reg;
  volatile u32 *ptr = (u32*) p2v(pa);
  *ptr = val;
}

static int
eeprom_eerd_read(const eerd* eerd, u16 off)
{
  u32 reg;
  int x;

  // [E1000 13.4.4] Ensure EEC control is released
  reg = erd(WMREG_EECD);
  reg &= ~(EECD_EE_REQ|EECD_EE_GNT);
  ewr(WMREG_EECD, reg);

  // [E1000 5.3.1] Software EEPROM access 
  ewr(WMREG_EERD, (off<<eerd->addr_shift) | eerd->start_bit);
  for (x = 0; x < 5; x++) {
    reg = erd(WMREG_EERD);
    if (reg & eerd->done_bit)
      return (reg&EERD_DATA_MASK) >> eerd->data_shift;
    microdelay(50000);
  }
  return -1;
}

static int
eeprom_read(const eerd* eerd, u16 *buf, int off, int count)
{
  for (int i = 0; i < count; i++) {
    int r = eeprom_eerd_read(eerd, off+i);
    if (r < 0) {
      cprintf("eeprom_read: cannot read\n");
      return -1;
    }
    buf[i] = r;
  }
  return 0;
}

int
e1000tx(void *buf, u32 len)
{
  struct wiseman_txdesc *desc;
  u32 tail;

  acquire(&e1000.lk);
  // WMREG_TDT should only equal WMREG_TDH when we have
  // nothing to transmit.  Therefore, we can accomodate
  // TX_RING_SIZE-1 buffers.
  if (e1000.txinuse == TX_RING_SIZE-1) {
    cprintf("TX ring overflow\n");
    release(&e1000.lk);
    return -1;
  }

  tail = erd(WMREG_TDT);
  desc = &e1000.txd[tail];
  if (!(desc->wtx_fields.wtxu_status & WTX_ST_DD))
    panic("e1000tx");

  desc->wtx_addr = v2p(buf);
  desc->wtx_cmdlen = len | WTX_CMD_RS | WTX_CMD_EOP | WTX_CMD_IFCS;
  memset(&desc->wtx_fields, 0, sizeof(desc->wtx_fields));
  ewr(WMREG_TDT, (tail+1) % TX_RING_SIZE);
  e1000.txinuse++;
  release(&e1000.lk);

  return 0;
}

static void
cleantx(void)
{
  struct wiseman_txdesc *desc;
  void *va;

  acquire(&e1000.lk);
  while (e1000.txinuse) {
    desc = &e1000.txd[e1000.txclean];
    if (!(desc->wtx_fields.wtxu_status & WTX_ST_DD))
      break;

    va = p2v(desc->wtx_addr);
    netfree(va);
    desc->wtx_fields.wtxu_status = WTX_ST_DD;

    e1000.txclean = (e1000.txclean+1) % TX_RING_SIZE;
    desc = &e1000.txd[e1000.txclean];
    e1000.txinuse--;
  }
  release(&e1000.lk);
}

static void
allocrx(void)
{
  struct wiseman_rxdesc *desc;
  void *buf;
  u32 i;

  i = erd(WMREG_RDT);
  desc = &e1000.rxd[i];  
  if (desc->wrx_status & WRX_ST_DD)
    panic("allocrx");
  buf = netalloc();
  if (buf == nullptr)
    panic("Oops");
  desc->wrx_addr = v2p(buf);

  ewr(WMREG_RDT, (i+1) % RX_RING_SIZE);
}

static void
cleanrx(void)
{
  struct wiseman_rxdesc *desc;
  void *va;
  u16 len;

  acquire(&e1000.lk);
  desc = &e1000.rxd[e1000.rxclean];
  while (desc->wrx_status & WRX_ST_DD) {
    va = p2v(desc->wrx_addr);
    len = desc->wrx_len;

    desc->wrx_status = 0;
    allocrx();

    e1000.rxclean = (e1000.rxclean+1) % RX_RING_SIZE;

    release(&e1000.lk);
    netrx(va, len);
    acquire(&e1000.lk);    

    desc = &e1000.rxd[e1000.rxclean];
  }
  release(&e1000.lk);
}

void
e1000intr(void)
{
  u32 icr = erd(WMREG_ICR);

  while (icr & (ICR_TXDW|ICR_RXO|ICR_RXT0)) {
    if (icr & ICR_TXDW)
      cleantx();
	
    if (icr & ICR_RXT0)
      cleanrx();

    if (icr & ICR_RXO)
      panic("ICR_RXO");

    icr = erd(WMREG_ICR);
  }
}

void
e1000hwaddr(u8 *hwaddr)
{
  memmove(hwaddr, e1000.hwaddr, sizeof(e1000.hwaddr));
}

static
void e1000reset(void)
{
  u32 ctrl;
  paddr tpa;
  paddr rpa;

  ctrl = erd(WMREG_CTRL);  
  // [E1000 13.4.1] Assert PHY_RESET then delay as much as 10 msecs
  // before clearing PHY_RESET.
  ewr(WMREG_CTRL, ctrl|CTRL_PHY_RESET);
  microdelay(10000);
  ewr(WMREG_CTRL, ctrl);

  // [E1000 13.4.1] Delay 1 usec after reset
  ewr(WMREG_CTRL, ctrl|CTRL_RST);
  microdelay(1);

  // [E1000 13.4.41] Transmit Interrupt Delay Value of 1 usec.
  // A value of 0 is not allowed.  Enabled on a per-TX decriptor basis.
  ewr(WMREG_TIDV, 1);
  // [E1000 13.4.44] Delay TX interrupts a max of 1 usec.
  ewr(WMREG_TADV, 1);
  for (int i = 0; i < TX_RING_SIZE; i++)
    e1000.txd[i].wtx_fields.wtxu_status = WTX_ST_DD;
  // [E1000 14.5] Transmit Initialization
  tpa = v2p(e1000.txd);
  ewr(WMREG_TDBAH, tpa >> 32);
  ewr(WMREG_TDBAL, tpa & 0xffffffff);
  ewr(WMREG_TDLEN, sizeof(e1000.txd));
  ewr(WMREG_TDH, 0);
  ewr(WMREG_TDT, 0);
  ewr(WMREG_TCTL, TCTL_EN|TCTL_PSP|TCTL_CT(0x10)|TCTL_COLD(0x40));
  ewr(WMREG_TIPG, TIPG_IPGT(10)|TIPG_IPGR1(8)|TIPG_IPGR2(6));

  for (int i = 0; i < RX_RING_SIZE>>1; i++) {
    void *buf = netalloc();
    e1000.rxd[i].wrx_addr = v2p(buf);
  }
  rpa = v2p(e1000.rxd);
  ewr(WMREG_RDBAH, rpa >> 32);
  ewr(WMREG_RDBAL, rpa & 0xffffffff);
  ewr(WMREG_RDLEN, sizeof(e1000.rxd));
  ewr(WMREG_RDH, 0);
  ewr(WMREG_RDT, RX_RING_SIZE>>1);
  ewr(WMREG_RDTR, 0);
  ewr(WMREG_RADV, 0);
  ewr(WMREG_RCTL,
      RCTL_EN | RCTL_RDMTS_1_2 | RCTL_DPF | RCTL_BAM | RCTL_2k);

  // [E1000 13.4.20]
  ewr(WMREG_IMC, ~0);
  ewr(WMREG_IMS, ICR_TXDW | ICR_RXO | ICR_RXT0);
}

static int
e1000attach(struct pci_func *pcif)
{
  int r;
  int i;

  if (e1000init)
    return 0;

  int devid = PCI_PRODUCT(pcif->dev_id);
  e1000.model = nullptr;
  for (auto &m : e1000_models) {
    if (devid == m.devid) {
      e1000.model = &m;
      break;
    }
  }
  if (!e1000.model)
    panic("e1000attach: unrecognized devid %x", devid);
  console.println("e1000: Found ", e1000.model->name);

  if (e1000.model->flags & MODEL_FLAG_PCIE) {
    console.println("e1000: PCI-E not implemented");
    return 0;
  }

  // On dual-ported devices, PCI functions 0 and 1 are ports 0 and 1.
  if ((e1000.model->flags & MODEL_FLAG_DUAL_PORT) && pcif->func != E1000_PORT)
    return 0;

  pci_func_enable(pcif);

  e1000.membase = pcif->reg_base[0];
  e1000.iobase = pcif->reg_base[2];

  e1000irq = extpic->map_pci_irq(pcif);
  e1000irq.enable();

  e1000reset();

  // Get the MAC address
  r = eeprom_read(e1000.model->eerd, (u16*)e1000.hwaddr, EEPROM_OFF_MACADDR, 3);
  if (r < 0)
    return 0;

  if ((e1000.model->flags & MODEL_FLAG_DUAL_PORT) && E1000_PORT)
    // [E1000 12.3.1] The EEPROM is shared, so we read port 0's MAC
    // address.  Port 1's is the same with the LSB inverted.
    e1000.hwaddr[5] ^= 1;

  if (VERBOSE)
      cprintf("%x:%x:%x:%x:%x:%x\n",
              e1000.hwaddr[0], e1000.hwaddr[1], e1000.hwaddr[2],
              e1000.hwaddr[3], e1000.hwaddr[4], e1000.hwaddr[5]);

  u32 ralow = ((u32) e1000.hwaddr[0]) | ((u32) e1000.hwaddr[1] << 8) | 
      ((u32) e1000.hwaddr[2] << 16) | ((u32) e1000.hwaddr[3] << 24);
  u32 rahigh = ((u32) e1000.hwaddr[4] | ((u32) e1000.hwaddr[5] << 8));
  
  ewr(WMREG_RAL_LO(WMREG_CORDOVA_RAL_BASE, 0), ralow);
  erd(WMREG_STATUS);
  ewr(WMREG_RAL_HI(WMREG_CORDOVA_RAL_BASE, 0), rahigh|RAL_AV);
  erd(WMREG_STATUS);

  for (i = 0; i < WMREG_MTA; i+=4)
    ewr(WMREG_CORDOVA_MTA+i, 0);

  e1000init = 1;
  return 1;
}

void
inite1000(void)
{
  for (auto &m : e1000_models)
    pci_register_driver(0x8086, m.devid, e1000attach);
}
