# Use standard gcc, ld, and nasm
CC = gcc
LD = ld
AS = nasm

# Flags for a freestanding bare-metal environment
CFLAGS = -m32 -std=gnu99 -ffreestanding -O2 -Wall -Wextra -I.
LDFLAGS = -m elf_i386 -T kernel/linker.ld -nostdlib

KERNEL = iso/boot/newrex.bin
ISO = newrex.iso

OBJS = boot/boot.o \
       kernel/kernel.o \
       kernel/sched.o \
       kernel/trace.o \
       kernel/meminfo.o \
       kernel/rexfs.o \
       drivers/keyboard.o \
       drivers/pci.o \
       drivers/rtl8139.o \
       hal/gdt.o \
       hal/idt.o \
       mm/pmm.o \
       mm/vmm.o

all: $(KERNEL)

$(KERNEL): $(OBJS)
	mkdir -p iso/boot
	$(LD) $(LDFLAGS) -o $(KERNEL) $(OBJS)

iso: $(KERNEL)
	grub-mkrescue -o $(ISO) iso/

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.asm
	$(AS) -f elf32 $< -o $@

clean:
	rm -f $(OBJS) $(KERNEL) $(ISO)
