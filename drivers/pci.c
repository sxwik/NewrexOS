#include "pci.h"

extern void print_string(char* str);
extern void print_string_color(char* str, unsigned char attribute);
extern void print_hex(unsigned int num);
extern void print_int(int num);
extern void print_char(char c);

#define COLOR_YELLOW 0x0E

static unsigned int pci_address(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    return 0x80000000u
        | ((unsigned int)bus << 16)
        | ((unsigned int)slot << 11)
        | ((unsigned int)func << 8)
        | (offset & 0xFCu);
}

static void outl(unsigned short port, unsigned int value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned int inl(unsigned short port) {
    unsigned int value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

unsigned int pci_read_config32(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    outl(0xCF8, pci_address(bus, slot, func, offset));
    return inl(0xCFC);
}

unsigned short pci_read_config16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    unsigned int value = pci_read_config32(bus, slot, func, offset);
    return (unsigned short)(value >> ((offset & 2) * 8));
}

unsigned char pci_read_config8(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset) {
    unsigned int value = pci_read_config32(bus, slot, func, offset);
    return (unsigned char)(value >> ((offset & 3) * 8));
}

void pci_write_config16(unsigned char bus, unsigned char slot, unsigned char func, unsigned char offset, unsigned short value) {
    unsigned int current = pci_read_config32(bus, slot, func, offset);
    unsigned int shift = (offset & 2) * 8;
    unsigned int mask = 0xFFFFu << shift;
    unsigned int updated = (current & ~mask) | ((unsigned int)value << shift);
    outl(0xCF8, pci_address(bus, slot, func, offset));
    outl(0xCFC, updated);
}

int pci_scan_devices(pci_device_t* out, int max) {
    int count = 0;

    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
            unsigned short vendor = pci_read_config16((unsigned char)bus, (unsigned char)slot, 0, 0x00);
            if (vendor == 0xFFFF) {
                continue;
            }

            for (unsigned int func = 0; func < 8; func++) {
                vendor = pci_read_config16((unsigned char)bus, (unsigned char)slot, (unsigned char)func, 0x00);
                if (vendor == 0xFFFF) {
                    continue;
                }

                if (count < max) {
                    unsigned int class_reg = pci_read_config32((unsigned char)bus, (unsigned char)slot, (unsigned char)func, 0x08);
                    out[count].bus = (unsigned char)bus;
                    out[count].slot = (unsigned char)slot;
                    out[count].func = (unsigned char)func;
                    out[count].vendor_id = vendor;
                    out[count].device_id = pci_read_config16((unsigned char)bus, (unsigned char)slot, (unsigned char)func, 0x02);
                    out[count].subclass = (unsigned char)((class_reg >> 16) & 0xFF);
                    out[count].class_code = (unsigned char)((class_reg >> 24) & 0xFF);
                    count++;
                }

                unsigned char header = pci_read_config8((unsigned char)bus, (unsigned char)slot, (unsigned char)func, 0x0E);
                if ((header & 0x80) == 0) {
                    break;
                }
            }
        }
    }

    return count;
}

void pci_dump_all(void) {
    pci_device_t devices[PCI_MAX_DEVICES];
    int count = pci_scan_devices(devices, PCI_MAX_DEVICES);

    print_string_color("--- PCI BUS SCAN ---\n", COLOR_YELLOW);
    print_string("Devices found     : ");
    print_int(count);
    print_char('\n');

    for (int i = 0; i < count; i++) {
        print_string("  ");
        print_int(devices[i].bus);
        print_char(':');
        print_int(devices[i].slot);
        print_char('.');
        print_int(devices[i].func);
        print_string("  ven ");
        print_hex(devices[i].vendor_id);
        print_string(" dev ");
        print_hex(devices[i].device_id);
        print_string("  class ");
        print_hex(devices[i].class_code);
        print_char('/');
        print_hex(devices[i].subclass);
        print_char('\n');
    }
}
