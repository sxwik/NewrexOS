#ifndef VMM_H
#define VMM_H

// Standard x86 Paging Flags
#define PTE_PRESENT       0x01
#define PTE_READ_WRITE    0x02
#define PTE_USER_SUPER    0x04

void vmm_init();
void vmm_dump_info();

#endif