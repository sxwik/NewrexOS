#ifndef SCHED_H
#define SCHED_H

#define TASK_RUNNING   0
#define TASK_READY     1
#define TASK_SUSPENDED 2

struct interrupt_frame {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    unsigned int eip, cs, eflags;
    unsigned int esp, ss; // only if privilege level switch (Ring 3 -> Ring 0) happened
} __attribute__((packed));

// Strict Process Control Block (PCB)
struct task_struct {
    unsigned int pid;
    unsigned int state;
    unsigned int page_directory;  // CR3 value for this task
    unsigned int kstack;          // Current kernel stack pointer (esp)
    unsigned int kstack_top_page; // Base address for TSS esp0
    unsigned int ustack;          // User stack top
    struct task_struct* next;     // Pointer for Round-Robin queue
} __attribute__((packed));

void init_scheduler();
struct task_struct* task_create(void (*entry)(), unsigned int page_dir, int is_user);
void task_yield();
unsigned int task_switch(unsigned int esp);
unsigned int syscall_handler(unsigned int esp);

#endif