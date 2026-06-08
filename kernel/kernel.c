/*
 * Newrex Kernel - Core Wirematrix & Shell Interpreter
 * Architecture: 32-bit x86 Protected Mode, Asynchronous IRQ Driven
 * Features: Heap, CPUID, Panic, RTC, ATA Disk I/O, VGA Mode 13h
 */

#define VIDEO_MEM_START 0xB8000
#define SCREEN_MAX_BYTES 4000
#define BYTES_PER_ROW 160

#define COLOR_BLACK         0x00
#define COLOR_WHITE         0x0F
#define COLOR_LIGHT_GRAY    0x07
#define COLOR_LIGHT_GREEN   0x0A
#define COLOR_LIGHT_CYAN    0x0B
#define COLOR_LIGHT_RED     0x0C
#define COLOR_YELLOW        0x0E

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/pci.h"
#include "drivers/rtl8139.h"
#include "rexfs.h"

unsigned int saved_multiboot_info_ptr;
unsigned int saved_boot_magic;
void print_multiboot_mmap(unsigned int info_ptr);
void dump_bootinfo();
void dump_diskinfo();
void print_int(int num);

extern void init_gdt();
extern void init_idt();
extern void init_keyboard();
extern void dump_meminfo(void);
extern void rtl8139_init(void);
extern void net_ping(const char* cmd_args);
extern void net_udp_send(const char* cmd_args);
void run_background_tasks();

static int cursor_offset = 0;
static int prompt_min_offset = 0;

char command_buffer[256];
int command_length = 0;
volatile int command_ready = 0;

// --- SYSTEM TELEMETRY TRACKERS ---
volatile unsigned int system_ticks = 0;
volatile unsigned int uptime_seconds = 0;
unsigned int total_bytes_allocated = 0;
unsigned int total_disk_reads = 0;
unsigned int total_disk_writes = 0;

#define TRACE_MAX 12
char trace_ring[TRACE_MAX][64];
int trace_idx = 0;
int trace_count = 0;

void sys_trace(char* event) {
    int i = 0;
    while(event[i] != '\0' && i < 63) {
        trace_ring[trace_idx][i] = event[i];
        i++;
    }
    trace_ring[trace_idx][i] = '\0';
    trace_idx = (trace_idx + 1) % TRACE_MAX;
    if (trace_count < TRACE_MAX) trace_count++;
}

// The Hardware Heartbeat: Fires 18.2 times per second
unsigned int timer_isr_handler(unsigned int esp) {
    system_ticks++;
    if (system_ticks % 18 == 0) uptime_seconds++;
    
    extern int vga_active;
    extern volatile int frame_ready;
    if (vga_active) frame_ready = 1;
    
    run_background_tasks();

    extern unsigned int task_switch(unsigned int esp);
    esp = task_switch(esp);

    // Send End of Interrupt (EOI) to Master PIC
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0x20), "Nd"((unsigned short)0x20));
    return esp;
}

// --- SECTION 1: TINY MEMORY MANAGER (HEAP ALLOCATOR) ---

#define HEAP_SIZE (1024 * 1024) // 1 Megabyte Heap
__attribute__((aligned(4096))) unsigned char kernel_heap[HEAP_SIZE];

typedef struct block_meta {
    unsigned int size;
    int is_free;
    struct block_meta* next;
} block_meta_t;

block_meta_t* heap_head = 0;

void init_heap() {
    heap_head = (block_meta_t*)kernel_heap;
    heap_head->size = HEAP_SIZE - sizeof(block_meta_t);
    heap_head->is_free = 1;
    heap_head->next = 0;
}

void* kmalloc(unsigned int size) {
    block_meta_t* curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size >= size) {
            if (curr->size > size + sizeof(block_meta_t) + 4) {
                block_meta_t* new_block = (block_meta_t*)((unsigned char*)curr + sizeof(block_meta_t) + size);
                new_block->size = curr->size - size - sizeof(block_meta_t);
                new_block->is_free = 1;
                new_block->next = curr->next;
                
                curr->size = size;
                curr->next = new_block;
            }
            curr->is_free = 0;
            total_bytes_allocated += size;
            sys_trace("[MEM] Dynamic Heap Allocation");
            return (void*)((unsigned char*)curr + sizeof(block_meta_t));
        }
        curr = curr->next;
    }
    return 0; 
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_meta_t* block = (block_meta_t*)((unsigned char*)ptr - sizeof(block_meta_t));
    block->is_free = 1;
    
    block_meta_t* curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->next && curr->next->is_free) {
            curr->size += sizeof(block_meta_t) + curr->next->size;
            curr->next = curr->next->next;
        } else {
            curr = curr->next;
        }
    }
}

unsigned int heap_get_total_size(void) {
    return HEAP_SIZE;
}

unsigned int heap_get_used_size(void) {
    unsigned int used = 0;
    block_meta_t* curr = heap_head;
    while (curr) {
        if (!curr->is_free) {
            used += curr->size;
        }
        curr = curr->next;
    }
    return used;
}

unsigned int heap_get_free_size(void) {
    unsigned int free_sz = 0;
    block_meta_t* curr = heap_head;
    while (curr) {
        if (curr->is_free) {
            free_sz += curr->size;
        }
        curr = curr->next;
    }
    return free_sz;
}

unsigned int heap_get_alloc_count(void) {
    unsigned int count = 0;
    block_meta_t* curr = heap_head;
    while (curr) {
        if (!curr->is_free) count++;
        curr = curr->next;
    }
    return count;
}

unsigned int heap_get_free_count(void) {
    unsigned int count = 0;
    block_meta_t* curr = heap_head;
    while (curr) {
        if (curr->is_free) count++;
        curr = curr->next;
    }
    return count;
}

unsigned int heap_get_largest_free_block(void) {
    unsigned int max_val = 0;
    block_meta_t* curr = heap_head;
    while (curr) {
        if (curr->is_free && curr->size > max_val) {
            max_val = curr->size;
        }
        curr = curr->next;
    }
    return max_val;
}

void print_number(unsigned int num) {
    print_int((int)num);
}

// --- SECTION 2: VGA TERMINAL DRIVER ---

void check_scroll() {
    char* video_memory = (char*)VIDEO_MEM_START;
    if (cursor_offset >= SCREEN_MAX_BYTES) {
        for (int i = 0; i < 3840; i++) video_memory[i] = video_memory[i + BYTES_PER_ROW];
        for (int i = 3840; i < SCREEN_MAX_BYTES; i += 2) {
            video_memory[i] = ' ';
            video_memory[i+1] = COLOR_LIGHT_GRAY;
        }
        cursor_offset = 3840;
        prompt_min_offset -= BYTES_PER_ROW;
        if (prompt_min_offset < 0) prompt_min_offset = 0;
    }
}

void write_serial(char a) {
    __asm__ volatile("outb %0, %1" : : "a"(a), "Nd"((unsigned short)0x3F8));
}


void print_char_color(char c, unsigned char attribute) {
    if (c == '\n') {
        write_serial('\r');
        write_serial('\n');
    } else if (c != '\b') {
        write_serial(c);
    } else {
        write_serial('\b');
        write_serial(' ');
        write_serial('\b');
    }

    char* video_memory = (char*)VIDEO_MEM_START;
    if (c == '\n') {
        cursor_offset = (cursor_offset / BYTES_PER_ROW + 1) * BYTES_PER_ROW;
        check_scroll();
        return;
    }
    if (c == '\b') {
        if (cursor_offset > prompt_min_offset) {
            cursor_offset -= 2;                  
            video_memory[cursor_offset] = ' ';   
            video_memory[cursor_offset+1] = COLOR_LIGHT_GRAY; 
        }
        return;
    }
    if (cursor_offset < SCREEN_MAX_BYTES) {
        video_memory[cursor_offset] = c;
        video_memory[cursor_offset+1] = attribute; 
        cursor_offset += 2;
    }
    check_scroll();
}

void print_char(char c) { print_char_color(c, COLOR_WHITE); }

void print_string_color(char* str, unsigned char attribute) {
    for (int i = 0; str[i] != '\0'; i++) print_char_color(str[i], attribute);
}

void print_string(char* str) { print_string_color(str, COLOR_WHITE); }

void clear_screen() {
    char* video_memory = (char*)VIDEO_MEM_START;
    for (int i = 0; i < SCREEN_MAX_BYTES; i += 2) {
        video_memory[i] = ' ';      
        video_memory[i+1] = COLOR_LIGHT_GRAY; 
    }
    cursor_offset = 0;
}

// --- SECTION 3: KERNEL UTILS & PANIC ENGINE ---

void klog(char* status, char* msg, unsigned char color) {
    print_string_color("[", COLOR_WHITE);
    print_string_color(status, color);
    print_string_color("] ", COLOR_WHITE);
    print_string(msg);
    print_char('\n');
}

void print_int(int num) {
    if (num == 0) { print_char('0'); return; }
    char buf[32]; int i = 0;
    while (num > 0) { buf[i++] = (num % 10) + '0'; num /= 10; }
    for (int j = i - 1; j >= 0; j--) print_char(buf[j]);
}

void print_int_padded(int num) {
    if (num < 10) print_char('0');
    print_int(num);
}

unsigned int parse_hex(char* str) {
    unsigned int result = 0;
    if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) str += 2;
    while (*str) {
        char c = *str; unsigned int val = 0;
        if (c >= '0' && c <= '9') val = c - '0';
        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
        else break; 
        result = (result << 4) | val; str++;
    }
    return result;
}

unsigned int parse_int(char* str) {
    unsigned int res = 0;
    while (*str == ' ') str++; 
    while (*str >= '0' && *str <= '9') { res = (res * 10) + (*str - '0'); str++; }
    return res;
}

void print_hex(unsigned int num) {
    print_string_color("0x", COLOR_YELLOW);
    if (num == 0) { print_char_color('0', COLOR_YELLOW); return; }
    char hex_chars[] = "0123456789ABCDEF"; char buffer[9]; int i = 0;
    while (num > 0) { buffer[i++] = hex_chars[num & 0x0F]; num >>= 4; }
    for (int j = i - 1; j >= 0; j--) print_char_color(buffer[j], COLOR_YELLOW);
}

int strcmp(char* s1, char* s2) {
    int i = 0;
    while (s1[i] == s2[i]) { if (s1[i] == '\0') return 0; i++; }
    return s1[i] - s2[i];
}

int starts_with(char* str, char* prefix) {
    while (*prefix) { if (*str++ != *prefix++) return 0; }
    return 1;
}

void kernel_panic(char* message) {
    __asm__ volatile("cli");
    char* video_memory = (char*)VIDEO_MEM_START;
    for (int i = 0; i < SCREEN_MAX_BYTES; i += 2) {
        video_memory[i] = ' '; video_memory[i+1] = 0x4F; 
    }
    cursor_offset = 0;
    print_string_color("================================================================================\n", 0x4F);
    print_string_color("                                [ KERNEL PANIC ]                                \n", 0x4F);
    print_string_color("================================================================================\n\n", 0x4F);
    print_string_color("FATAL EXCEPTION: ", 0x4F); print_string_color(message, 0x4F);
    print_string_color("\n\nSystem halted. Please manually reset the machine.", 0x4F);
    while (1) __asm__ volatile("hlt");
}

// --- SECTION 3.5: CORE SYSTEM MEMORY VISUALIZATION ---

#define MODE_HEX 0
#define MODE_BIN 1

int mem_display_mode = MODE_HEX;

// Clean, standard byte-to-binary string printing function
void print_binary_byte(unsigned char value) {
    char visual_buffer[9];
    
    for (int i = 0; i < 8; i++) {
        int bit = (value >> (7 - i)) & 1;
        visual_buffer[i] = bit ? '1' : '0';
    }
    visual_buffer[8] = '\0';
    print_string(visual_buffer);
}

// --- SECTION 4: COMMAND HISTORY RING BUFFER ---

#define MAX_HISTORY 10
char cmd_history[MAX_HISTORY][256];
int history_count = 0;
int history_pos = -1;

void add_to_history(char* cmd) {
    if (cmd[0] == '\0') return;
    for (int i = MAX_HISTORY - 1; i > 0; i--) {
        for(int j=0; j<256; j++) cmd_history[i][j] = cmd_history[i-1][j];
    }
    for(int j=0; j<256; j++) cmd_history[0][j] = cmd[j];
    if (history_count < MAX_HISTORY) history_count++;
    history_pos = -1; 
}

void load_history(int direction) {
    if (history_count == 0) return;
    
    history_pos += direction;
    if (history_pos < 0) history_pos = -1;
    if (history_pos >= history_count) history_pos = history_count - 1;
    
    while (command_length > 0) {
        print_char('\b');
        command_length--;
    }
    
    if (history_pos == -1) {
        command_buffer[0] = '\0';
    } else {
        int i = 0;
        while (cmd_history[history_pos][i] != '\0') {
            command_buffer[i] = cmd_history[history_pos][i];
            print_char(command_buffer[i]);
            i++;
        }
        command_buffer[i] = '\0';
        command_length = i;
    }
}

// --- SECTION 5: CMOS REAL-TIME CLOCK (RTC) DRIVER ---

unsigned char rtc_second, rtc_minute, rtc_hour, rtc_day, rtc_month;
unsigned int  rtc_year;

static void outb_rtc(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static unsigned char inb_rtc(unsigned short port) {
    unsigned char result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

int get_update_in_progress_flag() {
    outb_rtc(0x70, 0x0A);
    return (inb_rtc(0x71) & 0x80);
}

unsigned char get_rtc_register(int reg) {
    outb_rtc(0x70, reg);
    return inb_rtc(0x71);
}

void read_rtc() {
    unsigned char registerB, century;
    unsigned char last_second, last_minute, last_hour, last_day, last_month, last_year, last_century;

    while (get_update_in_progress_flag());

    rtc_second = get_rtc_register(0x00);
    rtc_minute = get_rtc_register(0x02);
    rtc_hour   = get_rtc_register(0x04);
    rtc_day    = get_rtc_register(0x07);
    rtc_month  = get_rtc_register(0x08);
    rtc_year   = get_rtc_register(0x09);
    century    = get_rtc_register(0x32); 

    do {
        last_second = rtc_second; last_minute = rtc_minute; last_hour = rtc_hour;
        last_day = rtc_day; last_month = rtc_month; last_year = rtc_year; last_century = century;

        while (get_update_in_progress_flag());
        rtc_second = get_rtc_register(0x00); rtc_minute = get_rtc_register(0x02); rtc_hour = get_rtc_register(0x04);
        rtc_day = get_rtc_register(0x07); rtc_month = get_rtc_register(0x08); rtc_year = get_rtc_register(0x09);
        century = get_rtc_register(0x32);
    } while (last_second != rtc_second || last_minute != rtc_minute || last_hour != rtc_hour ||
             last_day != rtc_day || last_month != rtc_month || last_year != rtc_year || last_century != century);

    registerB = get_rtc_register(0x0B);

    if (!(registerB & 0x04)) {
        rtc_second = (rtc_second & 0x0F) + ((rtc_second / 16) * 10);
        rtc_minute = (rtc_minute & 0x0F) + ((rtc_minute / 16) * 10);
        rtc_hour   = ((rtc_hour & 0x0F) + (((rtc_hour & 0x70) / 16) * 10)) | (rtc_hour & 0x80);
        rtc_day    = (rtc_day & 0x0F) + ((rtc_day / 16) * 10);
        rtc_month  = (rtc_month & 0x0F) + ((rtc_month / 16) * 10);
        rtc_year   = (rtc_year & 0x0F) + ((rtc_year / 16) * 10);
        century    = (century & 0x0F) + ((century / 16) * 10);
    }

    if (!(registerB & 0x02) && (rtc_hour & 0x80)) {
        rtc_hour = ((rtc_hour & 0x7F) + 12) % 24;
    }

    if (century != 0) rtc_year += century * 100;
    else rtc_year += (rtc_year > 69) ? 1900 : 2000;
}

// --- SECTION 5.5: ATA IDE HARD DRIVE DRIVER ---

void outb_ata(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

unsigned char inb_ata(unsigned short port) {
    unsigned char result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

unsigned short inw_ata(unsigned short port) {
    unsigned short result;
    __asm__ volatile("inw %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

void outw_ata(unsigned short port, unsigned short data) {
    __asm__ volatile("outw %0, %1" : : "a"(data), "Nd"(port));
}

void read_sector(unsigned int lba, unsigned char* buffer) {
    outb_ata(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb_ata(0x1F2, 1);
    outb_ata(0x1F3, (unsigned char)lba);
    outb_ata(0x1F4, (unsigned char)(lba >> 8));
    outb_ata(0x1F5, (unsigned char)(lba >> 16));
    outb_ata(0x1F7, 0x20); 
    unsigned char status;
    while (((status = inb_ata(0x1F7)) & 0x80) == 0x80) {} 
    while (((status = inb_ata(0x1F7)) & 0x08) == 0x00) {} 
    for (int i = 0; i < 256; i++) {
        unsigned short word = inw_ata(0x1F0);
        buffer[i * 2] = (unsigned char)(word & 0xFF);
        buffer[i * 2 + 1] = (unsigned char)(word >> 8);
    }
    total_disk_reads++;
    sys_trace("[ATA] Sector Read Executed");
}

// NEW: Write a 512-byte block to the physical disk!
void write_sector(unsigned int lba, unsigned char* buffer) {
    outb_ata(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb_ata(0x1F2, 1);
    outb_ata(0x1F3, (unsigned char)lba);
    outb_ata(0x1F4, (unsigned char)(lba >> 8));
    outb_ata(0x1F5, (unsigned char)(lba >> 16));
    outb_ata(0x1F7, 0x30); // 0x30 is the WRITE SECTORS command
    unsigned char status;
    while (((status = inb_ata(0x1F7)) & 0x80) == 0x80) {} 
    while (((status = inb_ata(0x1F7)) & 0x08) == 0x00) {} 
    for (int i = 0; i < 256; i++) {
        unsigned short word = buffer[i * 2] | (buffer[i * 2 + 1] << 8);
        outw_ata(0x1F0, word);
    }
    total_disk_writes++;
    sys_trace("[ATA] Sector Write Executed");
}

// --- SECTION 5.7: VGA GRAPHICS MODE (MODE 13h) ---
// Writing to VGA registers to switch to 320x200 256-color pixel mode

void set_vga_mode_13h() {
    // This is a brutal hardware hack. We bypass the BIOS entirely and 
    // manually reprogram the VGA sequencer and CRT controller registers.
    unsigned char crtc_data[] = {0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F, 
                                 0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x9C, 0x8E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3, 0xFF};
    
    // Unlock CRTC registers
    outb_ata(0x3D4, 0x11);
    unsigned char val = inb_ata(0x3D5);
    outb_ata(0x3D5, val & 0x7F);

    // Set Misc Output Register
    outb_ata(0x3C2, 0x63);
    
    // Sequencer
    outb_ata(0x3C4, 0x00); outb_ata(0x3C5, 0x03);
    outb_ata(0x3C4, 0x01); outb_ata(0x3C5, 0x01);
    outb_ata(0x3C4, 0x02); outb_ata(0x3C5, 0x0F);
    outb_ata(0x3C4, 0x03); outb_ata(0x3C5, 0x00);
    outb_ata(0x3C4, 0x04); outb_ata(0x3C5, 0x0E);

    // CRTC Configuration
    for (int i = 0; i < 25; i++) {
        outb_ata(0x3D4, i);
        outb_ata(0x3D5, crtc_data[i]);
    }
    
    // Graphics Controller
    outb_ata(0x3CE, 0x05); outb_ata(0x3CF, 0x40);
    outb_ata(0x3CE, 0x06); outb_ata(0x3CF, 0x05);
    
    // Attribute Controller
    inb_ata(0x3DA); // Reset flip-flop
    outb_ata(0x3C0, 0x30); outb_ata(0x3C0, 0x41);
    outb_ata(0x3C0, 0x33); outb_ata(0x3C0, 0x00);
    
    // Enable Video
    inb_ata(0x3DA);
    outb_ata(0x3C0, 0x20);
}

// --- SECTION 5.8: DAEMONS, MOUSE, & FILE SYSTEM ---

// 1. Time-Sliced Schedular (Multitasking)
void (*daemon_tasks[4])() = {0, 0, 0, 0};

// Hook this into your existing timer_isr_handler!
void run_background_tasks() {
    for (int i = 0; i < 4; i++) {
        if (daemon_tasks[i]) daemon_tasks[i]();
    }
}

// 2. Virtual FAT (File System)
typedef struct {
    char name[16];
    unsigned int lba;
} fat_entry_t;
fat_entry_t vfs[16]; // Virtual File System Map
int vfs_count = 0;

// 3. Newrex Window Manager (NWM) & VRAM Engine
unsigned char double_buffer[64000]; // 320x200 Offline Backbuffer
int win_x = 80, win_y = 40, win_w = 160, win_h = 100;
int is_dragging = 0, drag_off_x = 0, drag_off_y = 0;
int mouse_x = 160, mouse_y = 100; 

unsigned char mouse_cycle = 0;
char mouse_byte[3];
int vga_active = 0;
volatile int frame_ready = 1; 
// Forward declaration so the GUI can use the shell
void execute_command(char* cmd);

// GUI Input Buffer State
char gui_cmd_buffer[100];
int gui_cmd_len = 0;

void gui_handle_char(char c) {
    if (c == '\b') { // Backspace
        if (gui_cmd_len > 0) {
            gui_cmd_len--;
            gui_cmd_buffer[gui_cmd_len] = '\0';
        }
    } else if (c == '\n') { // Enter
        // Execute the command! (This writes to 0xB8000, which the GUI is watching)
        execute_command(gui_cmd_buffer);
        
        // Wipe the buffer for the next command
        gui_cmd_len = 0;
        gui_cmd_buffer[0] = '\0';
    } else if (c >= 32 && c <= 126 && gui_cmd_len < 99) { // Standard typing
        gui_cmd_buffer[gui_cmd_len++] = c;
        gui_cmd_buffer[gui_cmd_len] = '\0';
    }
    frame_ready = 1; // Wake up the rendering loop instantly
}

void draw_pixel(int x, int y, unsigned char color) {
    if (x >= 0 && x < 320 && y >= 0 && y < 200) double_buffer[y * 320 + x] = color;
}

void draw_rect(int x, int y, int w, int h, unsigned char color) {
    for(int i = y; i < y + h; i++) {
        for(int j = x; j < x + w; j++) draw_pixel(j, i, color);
    }
}

// --- SECTION 5.8.1: VGA BITMAP FONT ENGINE ---

// External link to an 8x8 VGA Font Array (256 characters * 8 bytes)
extern unsigned char font8x8[256][8];

void draw_char_gui(int x, int y, char c, unsigned char color) {
    // A character is 8 rows tall
    for (int row = 0; row < 8; row++) {
        unsigned char font_row = font8x8[(unsigned char)c][row];
        // A character is 8 pixels wide
        for (int col = 0; col < 8; col++) {
            // Use bitwise AND to check if the specific pixel should be drawn
            if (font_row & (1 << (7 - col))) {
                draw_pixel(x + col, y + row, color);
            }
        }
    }
}

void draw_string_gui(int x, int y, char* str, unsigned char color) {
    int current_x = x;
    for (int i = 0; str[i] != '\0'; i++) {
        if (str[i] == '\n') {
            y += 10; // Move down one line (8px char + 2px padding)
            current_x = x;
        } else {
            draw_char_gui(current_x, y, str[i], color);
            current_x += 8; // Move right one character width
        }
    }
}

void render_gui() {
    if (!vga_active) return;
    
    // 1. Desktop & Window Frame
    draw_rect(0, 0, 320, 200, 0x01);
    draw_rect(win_x + 3, win_y + 3, win_w, win_h, 0x00); 
    draw_rect(win_x, win_y, win_w, win_h, 0x07);
    
    // 2. Title Bar
    draw_rect(win_x, win_y, win_w, 10, 0x03); 
    draw_string_gui(win_x + 4, win_y + 1, "NEWREX TERMINAL", 0x0F);
    
    // 3. Terminal Background
    int term_x = win_x + 4;
    int term_y = win_y + 14;
    draw_rect(term_x, term_y, win_w - 8, win_h - 18, 0x00);
    
    // 4. The Cursed Live Memory Dump
    draw_string_gui(term_x + 4, term_y + 4, "WATCH 0xB8000:", 0x0B); 
    
    unsigned char* watch_ptr = (unsigned char*)0xB8000;
    for(int i = 0; i < 5; i++) {
        unsigned char val = watch_ptr[i * 2]; // Grab raw characters
        int cursor_x = term_x + 4;
        int cursor_y = term_y + 18 + (i * 10);
        
        // Translate bits to Newrex aesthetics
        for (int bit = 7; bit >= 0; bit--) {
            char c = (val & (1 << bit)) ? '+' : 'x';
            unsigned char color = (c == '+') ? 0x0A : 0x08; // Green / Dark Gray
            draw_char_gui(cursor_x, cursor_y, c, color);
            cursor_x += 8;
        }
    }
    
    // 5. Draw Mouse Cursor
    draw_rect(mouse_x, mouse_y, 3, 3, 0x0F);
    
        // 5.5 Draw the Neural Input Line
        int prompt_y = term_y + win_h - 28; // Position at bottom of the terminal

        // Draw the root prompt (Red)
        draw_string_gui(term_x + 4, prompt_y, "root@nwm:~#", 0x0C); 
    
        // Draw what the user is typing (White)
        draw_string_gui(term_x + 4 + (12 * 8), prompt_y, gui_cmd_buffer, 0x0F);
    
        // Draw a blinking cursor
        extern volatile unsigned int system_ticks; // Grab the timer tick for blinking
        if (system_ticks % 10 < 5) {
            draw_char_gui(term_x + 4 + ((12 + gui_cmd_len) * 8), prompt_y, '_', 0x0F);
        }

        // 6. ULTRA-FAST HARDWARE VRAM BLAST (rep movsd)
    int dwords = 16000;
    void *dst = (void*)0xA0000;
    void *src = double_buffer;
    __asm__ volatile (
        "cld; rep movsd" 
        : "+D"(dst), "+S"(src), "+c"(dwords) 
        : 
        : "memory"
    );
}

void mouse_isr_handler() {
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0x20), "Nd"((unsigned short)0xA0)); 
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0x20), "Nd"((unsigned short)0x20)); 
    __asm__ volatile("sti"); 
    
    unsigned char status;
    __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((unsigned short)0x64));
    
    if (status & 0x20) {
        unsigned char d;
        __asm__ volatile("inb %1, %0" : "=a"(d) : "Nd"((unsigned short)0x60));
        if (mouse_cycle == 0 && (d & 0x08) == 0) return; 
        mouse_byte[mouse_cycle++] = d;
        
        if (mouse_cycle == 3) {
            mouse_cycle = 0;
            if (vga_active) {
                // Update X/Y coordinates
                if (mouse_byte[0] & 0x10) mouse_x -= (256 - mouse_byte[1]); else mouse_x += mouse_byte[1];
                if (mouse_byte[0] & 0x20) mouse_y += (256 - mouse_byte[2]); else mouse_y -= mouse_byte[2];
                if (mouse_x < 0) mouse_x = 0; if (mouse_x > 319) mouse_x = 319;
                if (mouse_y < 0) mouse_y = 0; if (mouse_y > 199) mouse_y = 199;
                
                frame_ready = 1; // Tell the CPU a full packet arrived!

                // Decode Left Click (Bit 0) -- cursed machine view
                int left_click = mouse_byte[0] & 0x01;
                
                if (left_click) {
                    // Check collision with Window Title Bar
                    if (!is_dragging && mouse_x >= win_x && mouse_x <= win_x + win_w && mouse_y >= win_y && mouse_y <= win_y + 10) {
                        is_dragging = 1;
                        drag_off_x = mouse_x - win_x;
                        drag_off_y = mouse_y - win_y;
                    }
                    // Check collision with the Red Panic Button
                    if (mouse_x >= win_x + 10 && mouse_x <= win_x + win_w - 10 && mouse_y >= win_y + 25 && mouse_y <= win_y + 55) {
                        kernel_panic("GUI KERNEL PANIC INITIATED BY USER CLICK");
                    }
                } else {
                    is_dragging = 0; // Drop the window if click is released
                }

                // Move the window if we are dragging it
                if (is_dragging) {
                    win_x = mouse_x - drag_off_x;
                    win_y = mouse_y - drag_off_y;
                }

                // Rendering is handled by the dedicated desktop loop — keep ISR minimal.
            }
        }
    }
}

void init_advanced_hardware() {
    // Blast Mouse Activation Codes to PS/2 Controller
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0xA8), "Nd"((unsigned short)0x64));
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0x20), "Nd"((unsigned short)0x64));
    unsigned char status; __asm__ volatile("inb %1, %0" : "=a"(status) : "Nd"((unsigned short)0x60));
    status |= 2; 
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0x60), "Nd"((unsigned short)0x64));
    __asm__ volatile("outb %0, %1" : : "a"(status), "Nd"((unsigned short)0x60));
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0xD4), "Nd"((unsigned short)0x64));
    __asm__ volatile("outb %0, %1" : : "a"((unsigned char)0xF4), "Nd"((unsigned short)0x60));
    
    // THE FIX: Give the hardware a microsecond to process, then flush the ACK byte!
    for(int i=0; i<100000; i++) __asm__ volatile("nop"); 
    unsigned char ack;
    __asm__ volatile("inb %1, %0" : "=a"(ack) : "Nd"((unsigned short)0x60));
}

// --- SECTION 6: SHELL INTERPRETER ---

void execute_command(char* cmd) {
    print_char('\n'); 
    add_to_history(cmd); 
    
    if (strcmp(cmd, "help") == 0) {
        print_string_color("Newrex Operator Kernel v5.0\n", COLOR_LIGHT_CYAN);
        print_string_color("  sysinfo      ", COLOR_YELLOW); print_string("- Deep CPUID Hardware Interrogation\n");
        print_string_color("  run <lba>    ", COLOR_LIGHT_RED); print_string("- WARNING: Execute raw binary code from disk sector\n");
        print_string_color("  sector <lba> ", COLOR_YELLOW); print_string("- Dump a 512-byte raw disk sector to screen\n");
        print_string_color("  write <args> ", COLOR_YELLOW); print_string("- Write text to Sector 1 or a RexFS file\n");
        print_string_color("  time         ", COLOR_YELLOW); print_string("- Read physical CMOS Real-Time Clock\n");
        print_string_color("  kmalloc <n>  ", COLOR_YELLOW); print_string("- Dynamically allocate n bytes on the Heap\n");
        print_string_color("  kfree <addr> ", COLOR_YELLOW); print_string("- Free a previously allocated Heap block\n");
        print_string_color("  peek <addr>  ", COLOR_YELLOW); print_string("- Read a raw byte from a hex memory address\n");
        print_string_color("  poke <a, v>  ", COLOR_YELLOW); print_string("- Write a hex byte value to a hex memory address\n");
        print_string_color("  dump <addr>  ", COLOR_YELLOW); print_string("- Dump 16 bytes from a hex memory address\n");
        print_string_color("  vgamode      ", COLOR_LIGHT_CYAN); print_string("- Switch to 320x200 Pixel Graphics (No Return)\n");
        print_string_color("  panic        ", COLOR_LIGHT_RED); print_string("- Trigger a manual Kernel Panic (System Halt)\n");
        print_string_color("  memmap       ", COLOR_YELLOW); print_string("- Print Multiboot memory map report\n");
        print_string_color("  bootinfo     ", COLOR_YELLOW); print_string("- Multiboot magic, info pointer, flags\n");
        print_string_color("  vmminfo      ", COLOR_YELLOW); print_string("- Paging status, CR3, identity map\n");
        print_string_color("  pmm          ", COLOR_YELLOW); print_string("- Physical memory manager statistics\n");
        print_string_color("  meminfo      ", COLOR_YELLOW); print_string("- Detailed heap and kernel base report\n");
        print_string_color("  mkfs         ", COLOR_YELLOW); print_string("- Format drive with RexFS partition layout\n");
        print_string_color("  mount        ", COLOR_YELLOW); print_string("- Mount RexFS partition\n");
        print_string_color("  unmount      ", COLOR_YELLOW); print_string("- Unmount RexFS partition safely\n");
        print_string_color("  fsinfo       ", COLOR_YELLOW); print_string("- Show RexFS filesystem information\n");
        print_string_color("  mkdir <name> ", COLOR_YELLOW); print_string("- Create a new subdirectory in RexFS\n");
        print_string_color("  cd <path>    ", COLOR_YELLOW); print_string("- Change directory in RexFS\n");
        print_string_color("  pwd          ", COLOR_YELLOW); print_string("- Print absolute working directory in RexFS\n");
        print_string_color("  touch <name> ", COLOR_YELLOW); print_string("- Create an empty file in RexFS\n");
        print_string_color("  rm <name>    ", COLOR_YELLOW); print_string("- Remove a file or directory in RexFS\n");
        print_string_color("  writehex <f> ", COLOR_YELLOW); print_string("- Write binary data to RexFS file as hex\n");
        print_string_color("  exec <file>  ", COLOR_YELLOW); print_string("- Execute a NEX program from RexFS\n");
        print_string_color("  diskinfo     ", COLOR_YELLOW); print_string("- ATA controller and CMOS RTC details\n");
        print_string_color("  pci          ", COLOR_YELLOW); print_string("- Scan PCI bus and list devices\n");
        print_string_color("  netinfo      ", COLOR_YELLOW); print_string("- RTL8139 adapter and MAC address\n");
        print_string_color("  ping <ip>    ", COLOR_YELLOW); print_string("- Send ICMP echo request (ping)\n");
        print_string_color("  udpsend <args>", COLOR_YELLOW); print_string("- Send UDP packet (udpsend <ip> <port> <msg>)\n");
        print_string_color("  trace        ", COLOR_YELLOW); print_string("- Recent internal kernel events\n");
        print_string_color("  link <n, l>  ", COLOR_YELLOW); print_string("- Link a filename to a disk sector LBA\n");
        print_string_color("  ls           ", COLOR_YELLOW); print_string("- List all linked files in VFS\n");
        print_string_color("  cat <file>   ", COLOR_YELLOW); print_string("- Output contents of a VFS or RexFS file\n");
        print_string_color("  monitor      ", COLOR_YELLOW); print_string("- Live uptime and I/O counters\n");
        print_string_color("  clear        ", COLOR_YELLOW); print_string("- Flush screen matrix\n");
    }
    else if (strcmp(cmd, "bootinfo") == 0) dump_bootinfo();
    else if (strcmp(cmd, "vmminfo") == 0) vmm_dump_info();
    else if (strcmp(cmd, "pmm") == 0) pmm_dump_info();
    else if (strcmp(cmd, "meminfo") == 0) dump_meminfo();
    else if (strcmp(cmd, "mkfs") == 0) rexfs_mkfs();
    else if (strcmp(cmd, "mount") == 0) rexfs_mount();
    else if (strcmp(cmd, "unmount") == 0) rexfs_unmount();
    else if (strcmp(cmd, "fsinfo") == 0) rexfs_fsinfo();
    else if (starts_with(cmd, "mkdir ")) rexfs_mkdir(cmd + 6);
    else if (starts_with(cmd, "cd ")) rexfs_cd(cmd + 3);
    else if (strcmp(cmd, "pwd") == 0) rexfs_pwd();
    else if (starts_with(cmd, "touch ")) rexfs_touch(cmd + 6);
    else if (starts_with(cmd, "rm ")) rexfs_rm(cmd + 3);
    else if (starts_with(cmd, "writehex ")) rexfs_writehex(cmd + 9);
    else if (starts_with(cmd, "exec ")) rexfs_exec(cmd + 5);
    else if (strcmp(cmd, "diskinfo") == 0) dump_diskinfo();
    else if (strcmp(cmd, "pci") == 0) pci_dump_all();
    else if (strcmp(cmd, "netinfo") == 0) rtl8139_dump_info();
    else if (starts_with(cmd, "ping ")) net_ping(cmd + 5);
    else if (starts_with(cmd, "udpsend ")) net_udp_send(cmd + 8);
    else if (strcmp(cmd, "clear") == 0) clear_screen();
    else if (strcmp(cmd, "memmap") == 0) {
        if (saved_multiboot_info_ptr != 0) {
            print_multiboot_mmap(saved_multiboot_info_ptr);
        } else {
            print_string_color("Error: No valid Multiboot information saved.\n", COLOR_LIGHT_RED);
        }
    }
    else if (strcmp(cmd, "panic") == 0) kernel_panic("MANUAL SYSTEM HALT TRIGGERED BY USER");
    else if (strcmp(cmd, "vgamode") == 0) {
        set_vga_mode_13h();
        vga_active = 1;
        frame_ready = 1;
        
        // THE DEDICATED DESKTOP LOOP
        while (vga_active) {
            // Only blast memory to the GPU if the mouse actually moved
            if (frame_ready) {
                render_gui();
                frame_ready = 0;
            }
            __asm__ volatile("hlt"); // Sleep CPU until next hardware interrupt
        }
    }
    else if (starts_with(cmd, "link ")) {
        if (vfs_count >= 16) {
            print_string_color("Error: VFS table full.\n", COLOR_LIGHT_RED);
        } else {
            char* args = cmd + 5;
            int i = 0;
            while (args[i] != ' ' && args[i] != '\0' && i < 15) {
                vfs[vfs_count].name[i] = args[i];
                i++;
            }
            vfs[vfs_count].name[i] = '\0';
            while (args[i] != ' ' && args[i] != '\0') {
                i++;
            }
            while (args[i] == ' ') {
                i++;
            }
            vfs[vfs_count].lba = parse_int(args + i);
            vfs_count++;
            print_string_color("File linked to FAT. Use 'ls' to view.\n", COLOR_LIGHT_GREEN);
        }
    }
    else if (strcmp(cmd, "ls") == 0 && rexfs_is_mounted()) {
        rexfs_ls();
    }
    else if (strcmp(cmd, "ls") == 0) {
        print_string_color("--- VIRTUAL FILE ALLOCATION TABLE ---\n", COLOR_LIGHT_CYAN);
        for(int i = 0; i < vfs_count; i++) {
            print_string(" [FILE] "); print_string_color(vfs[i].name, COLOR_YELLOW);
            print_string(" -> SECTOR "); print_int(vfs[i].lba); print_char('\n');
        }
    }
    else if (starts_with(cmd, "cat ") && rexfs_is_mounted()) {
        rexfs_cat(cmd + 4);
    }
    else if (starts_with(cmd, "cat ")) {
        char* filename = cmd + 4;
        int found_idx = -1;
        for (int i = 0; i < vfs_count; i++) {
            if (strcmp(vfs[i].name, filename) == 0) {
                found_idx = i;
                break;
            }
        }
        if (found_idx == -1) {
            print_string_color("Error: File not found.\n", COLOR_LIGHT_RED);
        } else {
            unsigned char sector_buffer[512];
            read_sector(vfs[found_idx].lba, sector_buffer);
            for (int j = 0; j < 512; j++) {
                if (sector_buffer[j] == '\0') break;
                print_char(sector_buffer[j]);
            }
            print_char('\n');
        }
    }
    else if (starts_with(cmd, "daemon ")) {
        unsigned int lba = parse_int(cmd + 7);
        unsigned char* app_mem = (unsigned char*)kmalloc(512);
        read_sector(lba, app_mem);
        
        // Find a free slot in the daemon array
        for (int i = 0; i < 4; i++) {
            if (daemon_tasks[i] == 0) {
                daemon_tasks[i] = (void (*)())app_mem;
                print_string_color("Daemon active. Task bound to IRQ0 timer slice.\n", COLOR_LIGHT_GREEN);
                break;
            }
        }
    }
    else if (strcmp(cmd, "time") == 0) {
        read_rtc();
        print_string_color("CMOS Real-Time Clock: ", COLOR_LIGHT_GREEN);
        print_int_padded(rtc_year); print_char('-');
        print_int_padded(rtc_month); print_char('-');
        print_int_padded(rtc_day); print_string(" [ ");
        print_int_padded(rtc_hour); print_char(':');
        print_int_padded(rtc_minute); print_char(':');
        print_int_padded(rtc_second); print_string(" ]\n");
    }
    else if (starts_with(cmd, "kmalloc ")) {
        unsigned int size = parse_int(cmd + 8);
        void* ptr = kmalloc(size);
        if (ptr) {
            print_string_color("Heap Allocator: ", COLOR_LIGHT_CYAN);
            print_int(size); print_string(" bytes reserved at "); print_hex((unsigned int)ptr);
        } else {
            print_string_color("Error: Heap Out of Memory!", COLOR_LIGHT_RED);
        }
    }
    else if (starts_with(cmd, "kfree ")) {
        unsigned int addr = parse_hex(cmd + 6);
        kfree((void*)addr);
        print_string_color("Heap Allocator: Block at ", COLOR_LIGHT_CYAN);
        print_hex(addr); print_string(" freed and coalesced.");
    }
    else if (strcmp(cmd, "sysinfo") == 0) {
        print_string("Newrex OS - Run mode: ");
        print_string_color("32-Bit x86 Protected Mode Core\n", COLOR_LIGHT_GREEN);
        
        unsigned int ebx, edx, ecx, eax;
        __asm__ volatile("cpuid" : "=b"(ebx), "=d"(edx), "=c"(ecx) : "a"(0));
        char vendor[13];
        ((unsigned int*)vendor)[0] = ebx; ((unsigned int*)vendor)[1] = edx; ((unsigned int*)vendor)[2] = ecx;
        vendor[12] = '\0';
        print_string("CPU Vendor      : "); print_string_color(vendor, COLOR_LIGHT_CYAN); print_char('\n');

        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
        int fpu = edx & (1 << 0);
        int apic = edx & (1 << 9);
        int vmx = ecx & (1 << 5);
        int hypervisor = ecx & (1 << 31);

        print_string("Hardware Flags  : ");
        if (fpu) print_string_color("FPU ", COLOR_LIGHT_GREEN);
        if (apic) print_string_color("APIC ", COLOR_LIGHT_GREEN);
        if (vmx) print_string_color("VMX ", COLOR_LIGHT_GREEN);
        if (hypervisor) print_string_color("HYPERVISOR ", COLOR_LIGHT_GREEN);
        print_char('\n');
    } 
    else if (starts_with(cmd, "run ")) {
        unsigned int lba = parse_int(cmd + 4);
        
        print_string_color("Allocating heap memory for executable...\n", COLOR_LIGHT_CYAN);
        unsigned char* app_mem = (unsigned char*)kmalloc(512);
        
        if (!app_mem) {
            print_string_color("Error: Out of memory.\n", COLOR_LIGHT_RED);
        } else {
            print_string_color("Reading LBA ", COLOR_LIGHT_CYAN);
            print_int(lba);
            print_string_color(" into memory address ", COLOR_LIGHT_CYAN);
            print_hex((unsigned int)app_mem);
            print_char('\n');
            
            read_sector(lba, app_mem);
            
            print_string_color("Jumping execution pointer (EIP) to RAM. No safety nets active.\n", COLOR_LIGHT_RED);
            print_char('\n');
            
            // The most dangerous line of C code possible:
            // Cast the raw heap memory address to a function pointer and execute it.
            void (*execute_app)() = (void (*)())app_mem;
            execute_app();
            
            print_char('\n');
            print_string_color("Execution completed successfully. CPU survived.\n", COLOR_LIGHT_GREEN);
            
            kfree(app_mem);
        }
    }
    else if (strcmp(cmd, "trace") == 0) {
        print_string_color("--- RECENT INTERNAL KERNEL EVENTS ---\n", COLOR_LIGHT_CYAN);
        int start = (trace_idx < trace_count) ? 0 : trace_idx;
        for (int i = 0; i < trace_count; i++) {
            int current = (start + i) % TRACE_MAX;
            print_string(" > "); print_string_color(trace_ring[current], COLOR_LIGHT_GREEN); print_char('\n');
        }
    }
    else if (starts_with(cmd, "mode ")) {
        char* arg = cmd + 5;
        if (strcmp(arg, "hex") == 0) {
            mem_display_mode = MODE_HEX;
            print_string_color("Display mode set to standard hexadecimal.\n", 0x0A);
        }
        else if (strcmp(arg, "binary") == 0) {
            mem_display_mode = MODE_BIN;
            print_string_color("Display mode set to raw binary.\n", 0x0A);
        }
        else {
            print_string_color("Usage: mode [hex | binary | symbolic]\n", 0x0C);
        }
    }
    else if (strcmp(cmd, "monitor") == 0) {
        clear_screen();
        print_string_color("================================================================================\n", COLOR_LIGHT_GRAY);
        print_string_color(" [ NEWREX KERNEL ] - LIVE HARDWARE INTROSPECTION MODULE \n", COLOR_LIGHT_CYAN);
        print_string_color("================================================================================\n\n", COLOR_LIGHT_GRAY);
        print_string_color(" Press 'Q' to exit active monitoring mode.\n\n", COLOR_LIGHT_RED);
        
        int base_cursor = cursor_offset;
        unsigned int last_tick = 0;
        int monitor_running = 1;

        while (monitor_running) {
            // FIXED: Read from our software buffer instead of the hardware port race condition
            if (command_length > 0 && (command_buffer[command_length-1] == 'q' || command_buffer[command_length-1] == 'Q')) {
                monitor_running = 0;
                command_length = 0; 
                for(int i=0; i<256; i++) command_buffer[i]='\0'; 
            }
            if (system_ticks != last_tick) {
                last_tick = system_ticks;
                cursor_offset = base_cursor; 
                print_string("  SYSTEM UPTIME      : "); print_int(uptime_seconds); print_string(" SECONDS       \n");
                print_string("  HARDWARE TICKS     : "); print_int(system_ticks); print_string(" IRQ0 CYCLES   \n");
                print_string("  DYNAMIC HEAP USAGE : "); print_int(total_bytes_allocated); print_string(" BYTES ALLOCATED  \n");
                print_string("  ATA DISK READS     : "); print_int(total_disk_reads); print_string(" SECTOR FETCHES   \n");
                print_string("  ATA DISK WRITES    : "); print_int(total_disk_writes); print_string(" SECTOR COMMITS   \n");
            }
            __asm__ volatile("hlt");
        }
        clear_screen();
    } // <--- This is your existing Line 939 closing brace for "monitor"
    else if (starts_with(cmd, "mode ")) {
        char* arg = cmd + 5;
        if (strcmp(arg, "hex") == 0) {
            mem_display_mode = MODE_HEX;
            print_string_color("Display mode set to standard hexadecimal.\n", 0x0A);
        }
        else if (strcmp(arg, "binary") == 0) {
            mem_display_mode = MODE_BIN;
            print_string_color("Display mode set to raw binary.\n", 0x0A);
        }
        else {
            print_string_color("Usage: mode [hex | binary]\n", 0x0C);
        }
    } // <--- The new block ends here, cleanly leading into the next else if
    else if (starts_with(cmd, "watch ")) { // <--- This becomes your new "watch" entry line
        unsigned int addr = parse_hex(cmd + 6);
        unsigned char* ptr = (unsigned char*)addr;
        int base_cursor = cursor_offset;
        int watch_running = 1;
        unsigned int last_tick = system_ticks;

        while (watch_running) {
            if (command_length > 0 && (command_buffer[command_length-1] == 'q' || command_buffer[command_length-1] == 'Q')) {
                watch_running = 0;
                command_length = 0; 
                for(int i=0; i<256; i++) command_buffer[i]='\0'; 
            }
            if (system_ticks != last_tick) {
                last_tick = system_ticks;
                cursor_offset = base_cursor; 
                
                print_string_color(" LIVE RAM AT ", 0x0B); 
                print_hex(addr); 
                print_string_color(": \n ", 0x0B);
                
                for (int i = 0; i < 8; i++) {
            if (mem_display_mode == MODE_HEX) {
                print_hex(ptr[i]);
            } else {
                print_binary_byte(ptr[i]); // Upgraded to clean native 0s and 1s
            }
            print_char(' ');
        }
                
                print_string(" | ");
                for (int i = 0; i < 8; i++) {
                    char c = ptr[i];
                    if (c >= 32 && c <= 126) print_char_color(c, 0x0A); 
                    else print_char_color('.', 0x07);
                }
                print_string("          \n");
            }
            __asm__ volatile("hlt");
        }
    }
    else if (starts_with(cmd, "inject ")) {
        char* args = cmd + 7; 
        unsigned int addr = parse_hex(args);
        while (*args != ' ' && *args != '\0') args++;
        if (*args == ' ') args++;
        
        unsigned char* target = (unsigned char*)addr;
        int byte_count = 0;
        
        // Parse the string of hex bytes and write them to memory
        while (*args != '\0') {
            unsigned int opcode = parse_hex(args);
            target[byte_count++] = (unsigned char)opcode;
            while (*args != ' ' && *args != '\0') args++;
            if (*args == ' ') args++;
        }
        
        print_string_color("Injected ", COLOR_LIGHT_GREEN); print_int(byte_count); 
        print_string_color(" bytes of raw machine code at ", COLOR_LIGHT_GREEN); print_hex(addr); print_char('\n');
        print_string_color("Jumping EIP to payload...\n", COLOR_LIGHT_RED);
        sys_trace("[EXEC] Live Opcode Injection");
        
        void (*payload)() = (void (*)())addr;
        payload();
        
        print_string_color("Execution completed. CPU Survived.\n", COLOR_LIGHT_GREEN);
    }
    // NEW: Persistent Disk Writer (Writes to Sector 1)
    else if (starts_with(cmd, "write ") && rexfs_is_mounted()) {
        rexfs_write(cmd + 6);
    }
    else if (starts_with(cmd, "write ")) {
        char* text = cmd + 6;
        unsigned char sector_buffer[512] = {0}; // Initialize buffer with zeros
        
        // Copy user text into the first bytes of the buffer
        int i = 0;
        while (text[i] != '\0' && i < 511) {
            sector_buffer[i] = text[i];
            i++;
        }
        
        write_sector(1, sector_buffer);
        print_string_color("Successfully wrote text to Physical Sector 1.", COLOR_LIGHT_GREEN);
    }
    // UPGRADED: Hexdump with ASCII Forensic Output
    else if (starts_with(cmd, "sector ")) {
        unsigned int lba = parse_int(cmd + 7); 
        unsigned char sector_buffer[512];      
        
        print_string_color("Reading Physical Hard Drive Sector ", COLOR_LIGHT_CYAN);
        print_int(lba); print_char('\n');
        
        read_sector(lba, sector_buffer);
        
        char hex_chars[] = "0123456789ABCDEF";
        for (int i = 0; i < 512; i++) {
            print_char_color(hex_chars[sector_buffer[i] >> 4], COLOR_YELLOW);
            print_char_color(hex_chars[sector_buffer[i] & 0x0F], COLOR_YELLOW);
            print_char(' ');
            
            // At the end of 16 bytes, print the ASCII representation
            if ((i + 1) % 16 == 0) {
                print_string(" | ");
                for (int j = i - 15; j <= i; j++) {
                    char c = sector_buffer[j];
                    if (c >= 32 && c <= 126) print_char_color(c, COLOR_LIGHT_GREEN);
                    else print_char_color('.', COLOR_LIGHT_GRAY); // Unprintable chars
                }
                print_char('\n'); 
            }
        }
    }
    else if (starts_with(cmd, "peek ")) {
        unsigned int addr = parse_hex(cmd + 5);
        print_string_color("Memory at ", COLOR_LIGHT_CYAN); print_hex(addr);
        print_string_color(" contains: ", COLOR_LIGHT_CYAN); print_hex(*((unsigned char*)addr));
    }
    else if (starts_with(cmd, "poke ")) {
        char* args = cmd + 5; unsigned int addr = parse_hex(args);
        while (*args != ' ' && *args != '\0') args++;
        if (*args == ' ') {
            unsigned int val = parse_hex(args + 1);
            *((unsigned char*)addr) = (unsigned char)val;
            print_string_color("Successfully wrote ", COLOR_LIGHT_GREEN); print_hex(val);
            print_string_color(" to address ", COLOR_LIGHT_GREEN); print_hex(addr);
        }
    }
    else if (starts_with(cmd, "dump ")) {
        unsigned int addr = parse_hex(cmd + 5);
        unsigned char* ptr = (unsigned char*)addr;
        print_string_color("Hex Dump at ", COLOR_LIGHT_CYAN); print_hex(addr); print_char('\n');
        for (int i = 0; i < 16; i++) { print_hex(ptr[i]); print_char(' '); }
    }
    else if (command_length > 0) {
        print_string_color("Error: Unknown command sequence. Type 'help'.", COLOR_LIGHT_RED);
    }

    if (strcmp(cmd, "clear") != 0 && strcmp(cmd, "monitor") != 0 && !starts_with(cmd, "watch ")) print_char('\n');
    
    print_string_color("root@newrex", COLOR_LIGHT_RED); print_string(":");
    print_string_color("~# ", COLOR_LIGHT_CYAN);
    prompt_min_offset = cursor_offset; 
}

// --- SECTION 7: KERNEL ENTRY POINT ---

typedef struct multiboot_info {
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

typedef struct multiboot_memory_map {
    unsigned int size;
    unsigned int base_addr_low;
    unsigned int base_addr_high;
    unsigned int length_low;
    unsigned int length_high;
    unsigned int type;
} multiboot_memory_map_t;

void print_hex_no_prefix(unsigned int num) {
    if (num == 0) { print_char_color('0', COLOR_YELLOW); return; }
    char hex_chars[] = "0123456789ABCDEF"; char buffer[9]; int i = 0;
    while (num > 0) { buffer[i++] = hex_chars[num & 0x0F]; num >>= 4; }
    for (int j = i - 1; j >= 0; j--) print_char_color(buffer[j], COLOR_YELLOW);
}

void print_hex_no_prefix_padded(unsigned int num) {
    char hex_chars[] = "0123456789ABCDEF"; 
    char buffer[8];
    for (int i = 0; i < 8; i++) {
        buffer[7 - i] = hex_chars[num & 0x0F]; 
        num >>= 4; 
    }
    for (int j = 0; j < 8; j++) print_char_color(buffer[j], COLOR_YELLOW);
}

void print_multiboot_mmap(unsigned int info_ptr) {
    multiboot_info_t* mbi = (multiboot_info_t*)info_ptr;

    print_string_color("--- MULTIBOOT MEMORY MAP REPORT ---\n", COLOR_YELLOW);

    if (mbi->flags & 1) {
        unsigned int total_kb = mbi->mem_lower + mbi->mem_upper + 1024;
        print_string("Total Memory (GRUB): ");
        print_int(total_kb / 1024);
        print_string(" MB\n");
    } else {
        print_string_color("Warning: mem_lower/upper not valid.\n", COLOR_LIGHT_RED);
    }

    if (mbi->flags & (1 << 6)) {
        int entry_count = 0;
        unsigned int mmap_addr = mbi->mmap_addr;
        unsigned int mmap_end = mbi->mmap_addr + mbi->mmap_length;
        
        while (mmap_addr < mmap_end) {
            entry_count++;
            multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mmap_addr;
            mmap_addr += mmap->size + 4;
        }

        print_string("Memory Map Entries : ");
        print_int(entry_count);
        print_char('\n');

        mmap_addr = mbi->mmap_addr;
        while (mmap_addr < mmap_end) {
            multiboot_memory_map_t* mmap = (multiboot_memory_map_t*)mmap_addr;
            
            print_string("Region Base: ");
            print_string_color("0x", COLOR_YELLOW);
            if (mmap->base_addr_high > 0) {
                print_hex_no_prefix(mmap->base_addr_high);
                print_hex_no_prefix_padded(mmap->base_addr_low);
            } else {
                print_hex_no_prefix(mmap->base_addr_low);
            }
            
            print_string(" Length: ");
            print_string_color("0x", COLOR_YELLOW);
            if (mmap->length_high > 0) {
                print_hex_no_prefix(mmap->length_high);
                print_hex_no_prefix_padded(mmap->length_low);
            } else {
                print_hex_no_prefix(mmap->length_low);
            }
            
            print_string(" Type: ");
            if (mmap->type == 1) {
                print_string_color("AVAILABLE", COLOR_LIGHT_GREEN);
            } else {
                print_string_color("RESERVED", COLOR_LIGHT_RED);
            }
            print_char('\n');
            
            mmap_addr += mmap->size + 4;
        }

    } else {
        print_string_color("Warning: mmap not valid.\n", COLOR_LIGHT_RED);
    }
    print_string_color("-----------------------------------\n", COLOR_YELLOW);
}

void dump_bootinfo() {
    print_string_color("--- MULTIBOOT BOOT INFO ---\n", COLOR_YELLOW);
    print_string("Magic             : ");
    print_hex(saved_boot_magic);
    if (saved_boot_magic == 0x2BADB002) {
        print_string_color(" (valid)\n", COLOR_LIGHT_GREEN);
    } else {
        print_string_color(" (invalid)\n", COLOR_LIGHT_RED);
    }
    print_string("Info pointer      : ");
    print_hex(saved_multiboot_info_ptr);
    print_char('\n');
    if (saved_multiboot_info_ptr != 0) {
        multiboot_info_t* mbi = (multiboot_info_t*)saved_multiboot_info_ptr;
        print_string("Flags             : ");
        print_hex(mbi->flags);
        print_char('\n');
        if (mbi->flags & 1) {
            print_string("mem_lower/upper   : valid\n");
        }
        if (mbi->flags & (1 << 6)) {
            print_string("mmap              : valid\n");
        }
    }
}

void dump_diskinfo() {
    print_string_color("--- DEVICE SUBSYSTEM ---\n", COLOR_LIGHT_CYAN);
    print_string("ATA IDE primary   : 0x1F0 (LBA28)\n");
    print_string("Sector reads      : ");
    print_int(total_disk_reads);
    print_string("\nSector writes     : ");
    print_int(total_disk_writes);
    print_string("\n");
    read_rtc();
    print_string("CMOS RTC          : ");
    print_int_padded(rtc_year); print_char('-');
    print_int_padded(rtc_month); print_char('-');
    print_int_padded(rtc_day); print_string(" ");
    print_int_padded(rtc_hour); print_char(':');
    print_int_padded(rtc_minute); print_char(':');
    print_int_padded(rtc_second);
    print_char('\n');
}

void kernel_main(unsigned int magic, unsigned int info_ptr) {
    clear_screen();

    saved_boot_magic = magic;

    print_string_color("Newrex Operator Kernel v5.0 (x86_32)\n", COLOR_LIGHT_CYAN);
    print_string_color("Booting monolithic core...\n\n", COLOR_LIGHT_GRAY);

    if (magic == 0x2BADB002) {
        saved_multiboot_info_ptr = info_ptr;
        pmm_init_from_mmap(info_ptr);
    } else {
        print_string_color("Invalid Multiboot Magic!\n", COLOR_LIGHT_RED);
    }

    init_gdt();
    init_idt();
    init_keyboard();
    init_heap();
    if (magic == 0x2BADB002) {
        vmm_init();
    }
    rexfs_init();
    rtl8139_init();

    klog(" OK ", "Core initialized", COLOR_LIGHT_GREEN);
    klog(" OK ", "Memory subsystem initialized", COLOR_LIGHT_GREEN);
    klog(" OK ", "Device subsystem initialized", COLOR_LIGHT_GREEN);

    print_char('\n');
    print_string_color("root@newrex", COLOR_LIGHT_RED); print_string(":");
    print_string_color("~# ", COLOR_LIGHT_CYAN);
    prompt_min_offset = cursor_offset; 

    sys_trace("[SYS] Kernel Boot Sequence Complete");
    init_advanced_hardware();
    __asm__ volatile("sti");

    while (1) {
        if (command_ready) {
            execute_command(command_buffer);
            command_length = 0;
            for (int i = 0; i < 256; i++) command_buffer[i] = '\0';
            command_ready = 0;
        }
        __asm__ volatile("hlt");
    }
}
/* Emergency Recovery Symbols */
unsigned char font8x8[256][8] = {0};

void* memset(void* dest, int val, unsigned int count) {
    unsigned char* ptr = (unsigned char*)dest;
    while (count--) *ptr++ = (unsigned char)val;
    return dest;
}