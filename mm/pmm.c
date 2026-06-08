#include "pmm.h"

extern void print_string(char* str);
extern void print_string_color(char* str, unsigned char attribute);
extern void print_hex(unsigned int num);
extern void print_int(int num);

unsigned char memory_bitmap[1024];

unsigned int max_blocks = 8192;
unsigned int used_blocks = 0;

typedef struct {
    unsigned int flags;
    unsigned int mem_lower;
    unsigned int mem_upper;
    unsigned int boot_device;
    unsigned int cmdline;
    unsigned int mods_count;
    unsigned int mods_addr;
    unsigned int num;
    unsigned int size;
    unsigned int addr;
    unsigned int shndx;
    unsigned int mmap_length;
    unsigned int mmap_addr;
} multiboot_info_t;

typedef struct {
    unsigned int size;
    unsigned int base_addr_low;
    unsigned int base_addr_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
} multiboot_memory_map_t;

static void set_bit(int bit) {
    memory_bitmap[bit / 8] |= (1 << (bit % 8));
}

static void clear_bit(int bit) {
    memory_bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int test_bit(int bit) {
    return memory_bitmap[bit / 8] & (1 << (bit % 8));
}

static void mark_frames(unsigned int base, unsigned int length, int used) {
    unsigned int end = base + length;
    unsigned int frame = base / PAGE_SIZE;
    unsigned int last_frame = (end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (; frame < last_frame && frame < max_blocks; frame++) {
        if (used) set_bit((int)frame);
        else clear_bit((int)frame);
    }
}

static void reserve_conservative_region(void) {
    for (unsigned int i = 0; i < PMM_CONSERVATIVE_RESERVE_FRAMES && i < max_blocks; i++) {
        set_bit((int)i);
    }
}

static void recount_used_blocks(void) {
    used_blocks = 0;
    for (unsigned int i = 0; i < max_blocks; i++) {
        if (test_bit((int)i)) used_blocks++;
    }
}

static int find_first_free_block() {
    unsigned int start = PMM_CONSERVATIVE_RESERVE_FRAMES;
    for (unsigned int i = start / 8; i < max_blocks / 8; i++) {
        if (memory_bitmap[i] != 0xFF) {
            for (int j = 0; j < 8; j++) {
                int bit = (int)(i * 8 + j);
                if (bit < (int)start) continue;
                if (bit >= (int)max_blocks) return -1;
                if (!test_bit(bit)) return bit;
            }
        }
    }
    return -1;
}

static void bitmap_mark_all_used(void) {
    for (int i = 0; i < 1024; i++) {
        memory_bitmap[i] = 0xFF;
    }
}

static unsigned int compute_max_blocks_from_mmap(multiboot_info_t* mbi) {
    unsigned int highest = 0;

    if (mbi->flags & (1 << 6)) {
        unsigned int mmap_addr = mbi->mmap_addr;
        unsigned int mmap_end = mbi->mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_memory_map_t* entry = (multiboot_memory_map_t*)mmap_addr;
            unsigned int region_end = entry->base_addr_low + entry->length_low;
            if (entry->base_addr_high == 0 && region_end > highest) {
                highest = region_end;
            }
            mmap_addr += entry->size + 4;
        }
    } else if (mbi->flags & 1) {
        highest = (mbi->mem_lower + mbi->mem_upper + 1024) * 1024;
    }

    unsigned int blocks = highest / PAGE_SIZE;
    if (blocks < PMM_CONSERVATIVE_RESERVE_FRAMES) {
        blocks = PMM_CONSERVATIVE_RESERVE_FRAMES;
    }
    if (blocks > 8192) {
        blocks = 8192;
    }
    return blocks;
}

void pmm_init(unsigned int kernel_size, unsigned int available_memory) {
    bitmap_mark_all_used();

    max_blocks = (available_memory * 1024) / PAGE_SIZE;
    if (max_blocks > 8192) max_blocks = 8192;

    for (unsigned int i = 0; i < max_blocks; i++) {
        clear_bit((int)i);
    }

    unsigned int kernel_blocks = (kernel_size * 1024) / PAGE_SIZE;
    for (unsigned int i = 0; i < kernel_blocks; i++) {
        set_bit((int)i);
    }

    reserve_conservative_region();
    recount_used_blocks();
}

void pmm_init_from_mmap(unsigned int multiboot_info_ptr) {
    if (multiboot_info_ptr == 0) return;

    multiboot_info_t* mbi = (multiboot_info_t*)multiboot_info_ptr;
    bitmap_mark_all_used();
    max_blocks = compute_max_blocks_from_mmap(mbi);

    if (mbi->flags & (1 << 6)) {
        unsigned int mmap_addr = mbi->mmap_addr;
        unsigned int mmap_end = mbi->mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_memory_map_t* entry = (multiboot_memory_map_t*)mmap_addr;
            if (entry->base_addr_high == 0) {
                mark_frames(entry->base_addr_low, entry->length_low, entry->type != 1);
            }
            mmap_addr += entry->size + 4;
        }
    } else if (mbi->flags & 1) {
        unsigned int total_bytes = (mbi->mem_lower + mbi->mem_upper + 1024) * 1024;
        mark_frames(0, total_bytes, 0);
    }

    reserve_conservative_region();
    recount_used_blocks();
}

void* pmm_alloc_block() {
    if (used_blocks >= max_blocks) return 0;

    int frame = find_first_free_block();
    if (frame == -1) return 0;

    set_bit(frame);
    used_blocks++;
    return (void*)(unsigned int)(frame * PAGE_SIZE);
}

void pmm_dump_info() {
    print_string_color("--- PHYSICAL MEMORY MANAGER ---\n", 0x0E);
    print_string("Tracked frames    : ");
    print_int((int)max_blocks);
    print_string("\nUsed frames       : ");
    print_int((int)used_blocks);
    print_string("\nFree frames       : ");
    print_int((int)(max_blocks - used_blocks));
    print_string("\nTracked RAM       : ");
    print_int((int)(max_blocks * PAGE_SIZE / (1024 * 1024)));
    print_string(" MB\nLow reserve       : ");
    print_int((int)(PMM_CONSERVATIVE_RESERVE_FRAMES * PAGE_SIZE / (1024 * 1024)));
    print_string(" MB (frames 0-");
    print_int((int)(PMM_CONSERVATIVE_RESERVE_FRAMES - 1));
    print_string(")\nBitmap capacity   : 32 MB (8192 frames)\n");
}

void pmm_free_block(void* physical_address) {
    unsigned int physical = (unsigned int)physical_address;
    int frame = (int)(physical / PAGE_SIZE);

    if (frame < (int)PMM_CONSERVATIVE_RESERVE_FRAMES) return;
    if (frame < 0 || (unsigned int)frame >= max_blocks) return;

    clear_bit(frame);
    used_blocks--;
}
