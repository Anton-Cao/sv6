// PCI subsystem interface
enum { pci_res_bus, pci_res_mem, pci_res_io, pci_res_max };

struct pci_bus;

struct pci_func {
  struct pci_bus *bus;	// Primary bus for bridges

  u32 dev;
  u32 func;
  
  u32 dev_id;
  u32 dev_class;
  
  u32 reg_base[6];
  u32 reg_size[6];
  u8 irq_line;
  u8 msi_capreg;
};

struct pci_bus {
  struct pci_func *parent_bridge;
  u32 busno;
  void *acpi_handle;
};

int  pci_init(void);
void pci_func_enable(struct pci_func *f);
void pci_msi_enable(struct pci_func *f, u8 irqnum);

u32 pci_conf_read(u32 seg, u32 bus, u32 dev, u32 func, u32 offset, int width);
void pci_conf_write(u32 seg, u32 bus, u32 dev, u32 func, u32 offset,
                    u32 val, int width);

class print_stream;
void to_stream(print_stream *s, const struct pci_func &f);
