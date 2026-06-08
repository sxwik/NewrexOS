#include "gdt.h"
// --- 1. EXPLICIT HARDWARE STRUCTURE BLUEPRINTS ---

// Each GDT entry is exactly 8 bytes long
struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

// The special pointer handed to the CPU register (LGDT)
struct gdt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

// External assembly hook that executes the raw LGDT instruction
extern void gdt_flush(unsigned int gdt_ptr_addr);


// --- 2. GLOBAL STORAGE (Expanded to 6 entries to include TSS) ---
struct gdt_entry gdt[6];
struct gdt_ptr gp;

struct tss_entry_struct {
    unsigned int link;
    unsigned int esp0;       // Kernel stack pointer
    unsigned int ss0;        // Kernel stack segment (0x10)
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

struct tss_entry_struct tss_entry;

// --- 3. ARCHITECTURE LOGIC ---

void gdt_set_gate(int num, unsigned long base, unsigned long limit, unsigned char access, unsigned char granularity) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);
    gdt[num].granularity |= (granularity & 0xF0);
    gdt[num].access = access;
}

extern void* memset(void* dest, int val, unsigned int count);

void write_tss(int num, unsigned short ss0, unsigned int esp0) {
    unsigned int base = (unsigned int)&tss_entry;
    unsigned int limit = base + sizeof(tss_entry) - 1;

    gdt_set_gate(num, base, limit, 0x89, 0x40);

    memset(&tss_entry, 0, sizeof(tss_entry));

    tss_entry.ss0 = ss0;
    tss_entry.esp0 = esp0;

    tss_entry.cs = 0x0B;
    tss_entry.ss = 0x13;
    tss_entry.es = 0x13;
    tss_entry.ds = 0x13;
    tss_entry.fs = 0x13;
    tss_entry.gs = 0x13;
    tss_entry.iomap_base = sizeof(tss_entry);
}

void set_kernel_stack(unsigned int stack) {
    tss_entry.esp0 = stack;
}

void init_gdt() {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base = (unsigned int)&gdt;

    // 1. Null Descriptor (Required by x86 hardware)
    gdt_set_gate(0, 0, 0, 0, 0);

    // 2. Kernel Code Segment (Ring 0): Access byte 0x9A
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    // 3. Kernel Data Segment (Ring 0): Access byte 0x92
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    // 4. User Mode Code Segment (Ring 3): Access byte 0xFA
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    // 5. User Mode Data Segment (Ring 3): Access byte 0xF2
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    // 6. Task State Segment (TSS) Descriptor: Access byte 0x89
    write_tss(5, 0x10, 0);

    // Flush old segments and lock in the new armored matrix
    gdt_flush((unsigned int)&gp);

    // Load Task Register (LTR) selector (index 5, RPL 3: 0x2B, or RPL 0: 0x28)
    __asm__ volatile("ltr %%ax" : : "a"(0x2B));
}