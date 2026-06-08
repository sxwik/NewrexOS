#include "vmm.h"

extern void* memset(void* dest, int val, unsigned int count);
extern void print_string(char* str);
extern void print_string_color(char* str, unsigned char attribute);
extern void print_hex(unsigned int num);
extern void kernel_panic(char* message);

#define COLOR_LIGHT_CYAN  0x0B
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_YELLOW      0x0E

#define IDENTITY_MAP_PAGES 1024
#define IDENTITY_MAP_BYTES (IDENTITY_MAP_PAGES * 4096)

static __attribute__((aligned(4096))) unsigned int boot_page_directory[1024];
static __attribute__((aligned(4096))) unsigned int boot_page_table[1024];

unsigned int* kernel_page_directory = 0;

static void paging_report_pre(unsigned int cr3_phys) {
    print_string_color("--- PAGING REPORT (PRE-ENABLE STATE) ---\n", COLOR_LIGHT_CYAN);
    print_string("CR3 (configured)  : ");
    print_hex(cr3_phys);
    print_string("\nPage Directory    : ");
    print_hex((unsigned int)boot_page_directory);
    print_string("\nPage Table        : ");
    print_hex((unsigned int)boot_page_table);
    print_string("\nIdentity map      : 0x00000000 - 0x");
    print_hex(IDENTITY_MAP_BYTES - 1);
    print_string(" (");
    print_hex(IDENTITY_MAP_PAGES);
    print_string(" pages)\n");
}

static void paging_report_post(unsigned int cr3_phys) {
    unsigned int cr0_val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));

    print_string_color("--- PAGING REPORT (ACTIVE STATE) ---\n", COLOR_LIGHT_GREEN);
    print_string("CR3 (active)      : ");
    print_hex(cr3_phys);
    print_string("\nCR0 PG bit        : ");
    print_string_color((cr0_val & 0x80000000) ? "ENABLED\n" : "DISABLED\n", COLOR_YELLOW);
    print_string("Identity map      : 0x00000000 - 0x");
    print_hex(IDENTITY_MAP_BYTES - 1);
    print_string_color(" CONFIRMED\n", COLOR_LIGHT_GREEN);
}

void vmm_dump_info() {
    unsigned int cr3_active;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_active));
    paging_report_pre(cr3_active);
    paging_report_post(cr3_active);
}

void vmm_init() {
    unsigned int pd_phys = (unsigned int)boot_page_directory;
    unsigned int pt_phys = (unsigned int)boot_page_table;

    if ((pd_phys & 0xFFF) != 0 || (pt_phys & 0xFFF) != 0) {
        kernel_panic("VMM: page tables not 4KB aligned");
    }

    memset(boot_page_directory, 0, 4096);
    memset(boot_page_table, 0, 4096);

    for (unsigned int i = 0; i < IDENTITY_MAP_PAGES; i++) {
        boot_page_table[i] = (i * 4096) | PTE_PRESENT | PTE_READ_WRITE;
    }

    boot_page_directory[0] = pt_phys | PTE_PRESENT | PTE_READ_WRITE;
    kernel_page_directory = boot_page_directory;

    __asm__ volatile("mov %0, %%cr3" : : "r"(pd_phys));

    unsigned int cr0_val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0_val));
    cr0_val |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0_val));

    unsigned int cr3_active;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_active));

    if (cr3_active != pd_phys) {
        kernel_panic("VMM: CR3 mismatch after paging enable");
    }
    if ((cr0_val & 0x80000000) == 0) {
        kernel_panic("VMM: CR0.PG not set after paging enable");
    }
    if ((boot_page_table[0] & PTE_PRESENT) == 0 ||
        (boot_page_table[IDENTITY_MAP_PAGES - 1] & PTE_PRESENT) == 0) {
        kernel_panic("VMM: identity map PTE check failed");
    }
}
