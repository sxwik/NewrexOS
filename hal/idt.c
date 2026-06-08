#include "idt.h"
extern void print_string(char* str);
extern void timer_isr_asm();
extern void keyboard_isr_asm();
extern void mouse_isr_asm();
struct idt_entry_struct idt[256];
struct idt_ptr_struct   idt_ptr;
static void outb(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}
static void idt_load() {
    __asm__ volatile("lidt %0" : : "m" (idt_ptr));
}
void default_isr_handler() {
    print_string("\n[HW TRAP] Captured unhandled interrupt trace. System locked safely.\n");
    while(1) { __asm__ volatile("cli; hlt"); }
}
extern void syscall_isr_asm();

void set_idt_gate_user(unsigned char vector, unsigned int isr_address) {
    idt[vector].isr_low    = (isr_address & 0xFFFF);
    idt[vector].kernel_cs  = 0x08;
    idt[vector].reserved   = 0;
    idt[vector].attributes = 0xEE; // DPL = 3 (User mode accessible)
    idt[vector].isr_high   = (isr_address >> 16) & 0xFFFF;
}

void set_idt_gate(unsigned char vector, unsigned int isr_address) {
    idt[vector].isr_low    = (isr_address & 0xFFFF);
    idt[vector].kernel_cs  = 0x08;
    idt[vector].reserved   = 0;
    idt[vector].attributes = 0x8E; // DPL = 0 (Kernel mode only)
    idt[vector].isr_high   = (isr_address >> 16) & 0xFFFF;
}

void init_idt() {
    idt_ptr.limit = (sizeof(struct idt_entry_struct) * 256) - 1;
    idt_ptr.base  = (unsigned int)&idt;
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    for(int i = 0; i < 256; i++) {
        set_idt_gate(i, (unsigned int)default_isr_handler);
    }
    set_idt_gate(32, (unsigned int)timer_isr_asm);
    set_idt_gate(33, (unsigned int)keyboard_isr_asm);
    set_idt_gate(44, (unsigned int)mouse_isr_asm);
    set_idt_gate_user(128, (unsigned int)syscall_isr_asm); // Vector 0x80 System Call
    outb(0x21, 0xF8);
    outb(0xA1, 0xEF);
    idt_load();
}
