#ifndef PMM_H
#define PMM_H

#define PAGE_SIZE 4096
#define BLOCKS_PER_BYTE 8

/* Conservative low-memory reserve: kernel, BSS, bitmap, VGA, Multiboot (Option A) */
#define PMM_CONSERVATIVE_RESERVE_FRAMES 1024  /* 4 MB */

void pmm_init(unsigned int kernel_size, unsigned int available_memory);
void pmm_init_from_mmap(unsigned int multiboot_info_ptr);
void* pmm_alloc_block();
void pmm_free_block(void* physical_address);
void pmm_dump_info();

#endif
