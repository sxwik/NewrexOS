#ifndef GDT_H
#define GDT_H

// Strict x86 GDT Entry structure required by the processor
struct gdt_entry_struct {
    unsigned short limit_low;           // The lower 16 bits of the limit
    unsigned short base_low;            // The lower 16 bits of the base
    unsigned char  base_middle;         // The next 8 bits of the base
    unsigned char  access;              // Access flags determining ring level
    unsigned char  granularity;
    unsigned char  base_high;           // The last 8 bits of the base
} __attribute__((packed));

// Pointer structure telling the CPU where the GDT lives
struct gdt_ptr_struct {
    unsigned short limit;               // Size of the GDT array minus 1
    unsigned int   base;                // Start address of our GDT array
} __attribute__((packed));

// Functions exposed to the rest of the OS
void init_gdt();
void set_kernel_stack(unsigned int stack);

#endif
