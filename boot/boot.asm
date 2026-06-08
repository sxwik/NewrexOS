bits 32

MODULEALIGN equ  1 << 0              
MEMINFO     equ  1 << 1              
FLAGS       equ  MODULEALIGN | MEMINFO 
MAGIC       equ  0x1BADB002         
CHECKSUM    equ -(MAGIC + FLAGS)    

section .multiboot
align 4
    dd MAGIC
    dd FLAGS
    dd CHECKSUM

section .text
global gdt_flush   
global _start
global timer_isr_asm
extern timer_isr_handler
global syscall_isr_asm
extern syscall_handler

; Hardware Interrupt Gateway for IRQ0 (System Timer)
timer_isr_asm:
    pushad
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    cld
    push esp
    call timer_isr_handler
    mov esp, eax
    
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iretd

; System Call Interrupt Gateway (int 0x80)
syscall_isr_asm:
    pushad
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    cld
    push esp
    call syscall_handler
    mov esp, eax
    
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iretd


global keyboard_isr_asm
extern kernel_main
extern keyboard_isr_handler

_start:
    cli                         ; Absolute safety: force disable all hardware interrupts
    
    ; FIX 1: The stack grows DOWNWARDS. We must point ESP to the TOP of our reserved space.
    mov esp, stack_space + 8192 
    
    ; FIX 2: System V ABI requires 16-byte stack alignment BEFORE the call instruction.
    ; Our stack is currently 16-byte aligned. If we only push EAX and EBX (8 bytes),
    ; GCC compiled with -O2 will calculate offsets incorrectly.
    ; We push two dummy 0s to maintain the 16-byte alignment rule.
    push 0                      ; Dummy padding
    push 0                      ; Dummy padding
    
    push ebx                    ; Push Multiboot info pointer (Arg 2)
    push eax                    ; Push Multiboot magic number (Arg 1)
    
    call kernel_main            ; Jump into the clean C main entry function
    
.loop:
    hlt                         ; If kernel returns, put CPU to sleep safely
    jmp .loop

; Hardware Interrupt Gateway for IRQ1 (Keyboard)
keyboard_isr_asm:
    pushad                      ; Save all general-purpose registers
    
    push ds                     ; Save current data segments
    push es
    push fs
    push gs
    
    mov ax, 0x10                ; Load Kernel Data Segment descriptor
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    cld                         ; Clear direction flag for safe C code execution
    call keyboard_isr_handler   ; Jump to our C logic
    
    pop gs                      ; Restore original data segments
    pop fs
    pop es
    pop ds
    
    popad                       ; Restore general-purpose registers
    iretd                       ; Interrupt Return (restores EIP, CS, and EFLAGS)

global mouse_isr_asm
extern mouse_isr_handler

; Hardware Gateway for IRQ12 (PS/2 Mouse)
mouse_isr_asm:
    pushad
    push ds
    push es
    push fs
    push gs
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    
    cld
    call mouse_isr_handler
    
    pop gs
    pop fs
    pop es
    pop ds
    popad
    iretd

gdt_flush:
    mov eax, [esp + 4]          ; Get the pointer to the GDT pointer structure
    lgdt [eax]                  ; Load the new Global Descriptor Table
    mov ax, 0x10                ; 0x10 is the offset to our Kernel Data Segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush             ; 0x08 is the offset to our Kernel Code Segment. Far jump!
.flush:
    ret

global stack_space
section .bss
align 16
stack_space:                    
    resb 8192

