#include "keyboard.h"

extern void print_char(char c);
extern void execute_command(char* cmd);
extern void load_history(int direction); // NEW: History hook

extern char command_buffer[256];
extern int command_length;
extern volatile int command_ready;

static int shift_active = 0;
static int e0_mode = 0; // NEW: Tracks extended keys (like arrows)

static unsigned char inb(unsigned short port) {
    unsigned char result;
    __asm__ volatile("inb %1, %0" : "=a"(result) : "Nd"(port));
    return result;
}

static void outb(unsigned short port, unsigned char data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static unsigned char scancode_to_ascii_normal[] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',      
    0,   'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',   0,       
  '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/',   0,   '*',   0,       
   ' '                                                                          
};

static unsigned char scancode_to_ascii_shift[] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',      
    0,   'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0,       
  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?',   0,   '*',   0,       
   ' '                                                                          
};

void init_keyboard() {}

void keyboard_isr_handler() {
    // 1. IMMEDIATELY send End of Interrupt so the PIC doesn't lock up
    outb(0x20, 0x20);
    
    // 2. [DEFERRED] execute_command moved to kernel_main to avoid IRQ nesting

    // 3. Now we can safely read the hardware port
    unsigned char scancode = inb(0x60);
    
    // NEW: Handle Extended Key Prefix (Arrow Keys)
    if (scancode == 0xE0) {
        e0_mode = 1;
        return;
    }
    
    if (e0_mode) {
        if (scancode == 0x48) load_history(1);  // Up Arrow
        if (scancode == 0x50) load_history(-1); // Down Arrow
        e0_mode = 0;
        return;
    }

    if (scancode == 0x2A || scancode == 0x36) shift_active = 1;
    else if (scancode == 0xAA || scancode == 0xB6) shift_active = 0;
    else if (!(scancode & 0x80)) { 
        unsigned char* current_layout = shift_active ? scancode_to_ascii_shift : scancode_to_ascii_normal;
        
        if (scancode < sizeof(scancode_to_ascii_normal)) {
            char ascii = current_layout[scancode];

            extern int vga_active;
            extern void gui_handle_char(char c);

            // If the Window Manager is active, hijack the keystroke completely
            if (vga_active) {
                gui_handle_char(ascii);
                return; // Kill the interrupt before it reaches the old text shell
            }

            if (ascii == '\b') {
                if (command_length > 0) {
                    command_length--;
                    command_buffer[command_length] = '\0';
                    print_char('\b');
                }
            }
            else if (ascii == '\n') {
                command_buffer[command_length] = '\0'; 
                command_ready = 1;
            }
            else if (ascii != 0 && command_length < 255) {
                command_buffer[command_length] = ascii;
                command_length++;
                print_char(ascii);
            }
        }
    }
}