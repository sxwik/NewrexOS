#ifndef IDT_H
#define IDT_H

// Strict x86 IDT Entry structure required by the processor hardware
struct idt_entry_struct {
    unsigned short isr_low;      // The lower 16 bits of the ISR's address
    unsigned short kernel_cs;    // Source Code segment selector inside our GDT (0x08)
    unsigned char  reserved;     // This byte must always be explicitly 0
    unsigned char  attributes;   // Type and attribute flags (0x8E for interrupt gates)
    unsigned short isr_high;     // The upper 16 bits of the ISR's address
} __attribute__((packed));

// Pointer structure telling the CPU where our IDT array lives in RAM
struct idt_ptr_struct {
    unsigned short limit;        // Size of the IDT array minus 1
    unsigned int   base;         // The address of the first entry in our array
} __attribute__((packed));

// Exposed setup function
void init_idt();

#endif
