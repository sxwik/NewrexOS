#ifndef TRACE_H
#define TRACE_H

// Initialize the trace ring buffer
void init_trace(void);

// Log an event to the ring buffer
// Example: kernel_trace("BOOT", "GDT initialized");
// Example: kernel_trace("IRQ1", "Keyboard interrupt");
void kernel_trace(const char* module, const char* message);

// Hook this into your shell's `trace` command
void dump_trace_log(void);

#endif