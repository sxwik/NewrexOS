#include "kernel/meminfo.h"

extern unsigned int heap_get_total_size(void);
extern unsigned int heap_get_used_size(void);
extern unsigned int heap_get_free_size(void);
extern unsigned int heap_get_alloc_count(void);
extern unsigned int heap_get_free_count(void);
extern unsigned int heap_get_largest_free_block(void);

extern void print_string(const char* str);
extern void print_number(unsigned int num);
extern void print_hex(unsigned int num);

#define KERNEL_BASE_ADDR 0x00100000

void dump_meminfo(void) {
    print_string("Heap Total        : "); print_number(heap_get_total_size()); print_string(" bytes\n");
    print_string("Heap Used         : "); print_number(heap_get_used_size()); print_string(" bytes\n");
    print_string("Heap Free         : "); print_number(heap_get_free_size()); print_string(" bytes\n");
    print_string("Active Allocs     : "); print_number(heap_get_alloc_count()); print_string("\n");
    print_string("Free Blocks       : "); print_number(heap_get_free_count()); print_string("\n");
    print_string("Largest Free Block: "); print_number(heap_get_largest_free_block()); print_string(" bytes\n");
    print_string("Kernel Base       : 0x"); print_hex(KERNEL_BASE_ADDR); print_string("\n");
}