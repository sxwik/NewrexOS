#ifndef PCI_H
#define PCI_H

#define PCI_MAX_DEVICES 32

typedef struct {
    unsigned char bus;
    unsigned char slot;
    unsigned char func;
    unsigned short vendor_id;
    unsigned short device_id;
    unsigned char class_code;
    unsigned char subclass;
} pci_device_t;

unsigned int pci_read_config32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset);
unsigned short pci_read_config16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset);
unsigned char pci_read_config8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset);
void pci_write_config16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset, unsigned short value);

int pci_scan_devices(pci_device_t* out, int max);
void pci_dump_all(void);

#endif
