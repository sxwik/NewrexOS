#include "sched.h"

extern void* kmalloc(unsigned int size);
extern void* memset(void* dest, int val, unsigned int count);
extern void print_string(char* str);
extern void set_kernel_stack(unsigned int stack);

struct task_struct* current_task = 0;
struct task_struct* task_queue = 0;
unsigned int next_pid = 0;
int scheduler_active = 0;

void init_scheduler() {
    // Allocate memory for the master idle kernel task (PID 0)
    current_task = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    memset(current_task, 0, sizeof(struct task_struct));
    
    current_task->pid = next_pid++;
    current_task->state = TASK_RUNNING;
    
    // Capture the current Page Directory (CR3)
    unsigned int cr3_val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3_val));
    current_task->page_directory = cr3_val;
    
    // Set kernel stack top (we use stack_space from boot.asm)
    extern unsigned int stack_space;
    current_task->kstack_top_page = (unsigned int)&stack_space + 8192;
    
    // Establish the circular Round-Robin queue
    current_task->next = current_task; 
    task_queue = current_task;

    scheduler_active = 1;
}

struct task_struct* task_create(void (*entry)(), unsigned int page_dir, int is_user) {
    struct task_struct* t = (struct task_struct*)kmalloc(sizeof(struct task_struct));
    memset(t, 0, sizeof(struct task_struct));

    t->pid = next_pid++;
    t->state = TASK_READY;
    t->page_directory = page_dir;

    // Allocate 4KB kernel stack page
    unsigned char* kstack_page = (unsigned char*)kmalloc(4096);
    unsigned int* kstack_top = (unsigned int*)(kstack_page + 4096);
    t->kstack_top_page = (unsigned int)kstack_top;

    if (is_user) {
        // Allocate 4KB user stack page
        unsigned char* ustack_page = (unsigned char*)kmalloc(4096);
        unsigned int* ustack_top = (unsigned int*)(ustack_page + 4096);
        t->ustack = (unsigned int)ustack_top;

        // Set up exit trampoline at the base of the user stack
        // Trampoline logic:
        //   mov eax, 1    ; Exit syscall
        //   int 0x80
        ustack_page[0] = 0xB8; // mov eax, 1
        ustack_page[1] = 0x01;
        ustack_page[2] = 0x00;
        ustack_page[3] = 0x00;
        ustack_page[4] = 0x00;
        ustack_page[5] = 0xCD; // int 0x80
        ustack_page[6] = 0x80;

        // Push trampoline address onto stack so ret jumps to it
        *(--ustack_top) = (unsigned int)ustack_page;

        // Set up the interrupt stack frame for Ring 3
        *(--kstack_top) = 0x23; // SS (User Data Selector)
        *(--kstack_top) = (unsigned int)ustack_top; // ESP (User Stack Pointer)
        *(--kstack_top) = 0x202; // EFLAGS (Interrupts enabled)
        *(--kstack_top) = 0x1B; // CS (User Code Selector)
        *(--kstack_top) = (unsigned int)entry; // EIP
    } else {
        t->ustack = 0;

        // Set up the interrupt stack frame for Ring 0
        *(--kstack_top) = 0x202; // EFLAGS (Interrupts enabled)
        *(--kstack_top) = 0x08; // CS (Kernel Code Selector)
        *(--kstack_top) = (unsigned int)entry; // EIP
    }

    // Now set up pushad registers
    *(--kstack_top) = 0; // EAX
    *(--kstack_top) = 0; // ECX
    *(--kstack_top) = 0; // EDX
    *(--kstack_top) = 0; // EBX
    *(--kstack_top) = 0; // ESP dummy
    *(--kstack_top) = 0; // EBP
    *(--kstack_top) = 0; // ESI
    *(--kstack_top) = 0; // EDI

    // Push segments
    unsigned int data_sel = is_user ? 0x23 : 0x10;
    *(--kstack_top) = data_sel; // DS
    *(--kstack_top) = data_sel; // ES
    *(--kstack_top) = data_sel; // FS
    *(--kstack_top) = data_sel; // GS

    t->kstack = (unsigned int)kstack_top;

    // Enqueue in circular Round-Robin list
    if (task_queue == 0) {
        t->next = t;
        task_queue = t;
    } else {
        struct task_struct* curr = task_queue;
        while (curr->next != task_queue) {
            curr = curr->next;
        }
        curr->next = t;
        t->next = task_queue;
    }

    return t;
}

void task_yield() {
    __asm__ volatile("int $0x20");
}

unsigned int task_switch(unsigned int esp) {
    if (!scheduler_active || current_task == 0) return esp;

    current_task->kstack = esp;

    struct task_struct* next = current_task->next;
    while (next->state != TASK_READY && next->state != TASK_RUNNING) {
        next = next->next;
        if (next == current_task) {
            break;
        }
    }

    if (current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }
    current_task = next;
    current_task->state = TASK_RUNNING;

    set_kernel_stack(current_task->kstack_top_page);

    unsigned int current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if (current_task->page_directory != 0 && current_task->page_directory != current_cr3) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(current_task->page_directory));
    }

    return current_task->kstack;
}

unsigned int syscall_handler(unsigned int esp) {
    struct interrupt_frame* frame = (struct interrupt_frame*)esp;
    int sys_num = frame->eax;
    int arg1 = frame->ebx;

    if (sys_num == 1) {
        // Exit syscall
        current_task->state = TASK_SUSPENDED;
        esp = task_switch(esp);
    }
    else if (sys_num == 2) {
        // Print string syscall
        print_string((char*)arg1);
    }
    else if (sys_num == 3) {
        // Yield syscall
        esp = task_switch(esp);
    }

    return esp;
}