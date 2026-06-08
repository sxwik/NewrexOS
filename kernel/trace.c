#include "trace.h"

#define MAX_TRACE_ENTRIES 32
#define MAX_TRACE_LENGTH 64

// Statically allocated ring buffer
static char trace_buffer[MAX_TRACE_ENTRIES][MAX_TRACE_LENGTH];
static int trace_head = 0;
static int trace_count = 0;

// Minimal bare-metal string helpers
static void trace_strcat(char* dest, const char* src) {
    while (*dest) dest++;
    // Prevent buffer overflow while concatenating
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static void trace_strcpy(char* dest, const char* src) {
    *dest = '\0';
    trace_strcat(dest, src);
}

void init_trace(void) {
    trace_head = 0;
    trace_count = 0;
    for(int i = 0; i < MAX_TRACE_ENTRIES; i++) {
        trace_buffer[i][0] = '\0';
    }
    kernel_trace("BOOT", "Trace system initialized");
}

void kernel_trace(const char* module, const char* message) {
    char* entry = trace_buffer[trace_head];
    
    trace_strcpy(entry, "[");
    trace_strcat(entry, module);
    trace_strcat(entry, "] ");
    trace_strcat(entry, message);
    
    // Advance the head pointer, wrapping around if we hit the limit
    trace_head = (trace_head + 1) % MAX_TRACE_ENTRIES;
    if (trace_count < MAX_TRACE_ENTRIES) trace_count++;
}

void dump_trace_log(void) {
    int start = (trace_count == MAX_TRACE_ENTRIES) ? trace_head : 0;
    for (int i = 0; i < trace_count; i++) {
        int index = (start + i) % MAX_TRACE_ENTRIES;
        // TODO: Call your kernel's standard print_string function here
        // print_string(trace_buffer[index]); 
    }
}