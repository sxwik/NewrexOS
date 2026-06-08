# Newrex — Project Context

Hobby x86 (32-bit) operating system. C + NASM. Boots via GRUB Multiboot.

---

## 1. Boot Flow [IMPLEMENTED]

```
GRUB (Multiboot)
  → boot/boot.asm (_start)
  → kernel/kernel.c (kernel_main)
  → init_gdt / init_idt / init_keyboard / init_heap
  → rexfs_init() [Auto-mounts if superblock magic exists]
  → shell prompt (hlt loop, IRQ-driven)
```

| Stage | File | Status | Notes |
|-------|------|--------|-------|
| Bootloader | `iso/boot/grub/grub.cfg` | [IMPLEMENTED] | `multiboot /boot/newrex.bin` |
| Entry | `boot/boot.asm` | [IMPLEMENTED] | `MEMINFO` flag; 8 KB stack; passes `eax`=magic, `ebx`=info |
| Link | `kernel/linker.ld` | [IMPLEMENTED] | Kernel loaded at **1 MB** (`0x00100000`) |
| Main | `kernel/kernel.c` | [IMPLEMENTED] | Validates magic `0x2BADB002`, saves Multiboot info, calls `pmm_init_from_mmap()` |
| FS Auto-mount | `kernel/rexfs.c` | [IMPLEMENTED] | Checks LBA 1 for magic on boot; mounts automatically if valid |

**ISRs (boot.asm stubs → C handlers):** IRQ0 timer, IRQ1 keyboard, IRQ12 mouse. [IMPLEMENTED]

---

## 2. Memory Architecture

Three layers coexist; only two are active at runtime today.

### A. Static heap (`kmalloc` / `kfree`) — **Active** [IMPLEMENTED]
- 1 MB `kernel_heap[]` in `kernel.c`
- First-fit allocator with coalescing
- Used by shell and RexFS loader (`kmalloc`, `kfree`, `exec`, sector buffers)

### B. Physical Memory Manager (PMM) — **Initialized at boot** [IMPLEMENTED]
- **Bitmap:** `memory_bitmap[1024]` → **8192 frames / 32 MB**
- **Init:** `pmm_init_from_mmap()` walks Multiboot mmap
- **Low reserve (Historical):** Frames 0–1023 (**4 MB**) forced used (Option A)
- **Alloc range:** Frames ≥ 1024, below `max_blocks`
- **Legacy (Historical):** `pmm_init()` retained but unused at boot

### C. Virtual Memory Manager (VMM) — **Compiled, not active** [PARTIAL]
- `vmm_init()` identity-maps first 4 MB via PMM frames
- Never called from `kernel_main`
- **Do not enable without explicit approval**

---

## 3. Scheduler Status — **Compiled, not active** [PARTIAL]

- `init_scheduler()` in `kernel/sched.c` depends on `pmm_alloc_block()`
- Not called at boot

---

## 4. Trace Subsystem Status [PARTIAL]

- **Inline trace ring:** `sys_trace` in `kernel.c` with `trace` command [IMPLEMENTED]
- **Unified trace ring:** `kernel/trace.c` compiled but not fully unified with `sys_trace` [PARTIAL]

---

## 5. Build Pipeline [IMPLEMENTED]

**Toolchain:** `gcc -m32 -ffreestanding`, `nasm -f elf32`, `ld -nostdlib`

```bash
# From project root
make -f kernel/Makefile clean all    # → iso/boot/newrex.bin
make -f kernel/Makefile iso          # → newrex.iso
```

| Object | Source | Status |
|--------|--------|--------|
| `boot/boot.o` | `boot/boot.asm` | [IMPLEMENTED] |
| `kernel/kernel.o` | `kernel/kernel.c` | [IMPLEMENTED] |
| `kernel/sched.o` | `kernel/sched.c` | [IMPLEMENTED] |
| `kernel/trace.o` | `kernel/trace.c` | [IMPLEMENTED] |
| `kernel/meminfo.o` | `kernel/meminfo.c` | [IMPLEMENTED] (Linked in Makefile) |
| `kernel/rexfs.o` | `kernel/rexfs.c` | [IMPLEMENTED] |
| `drivers/keyboard.o` | `drivers/keyboard.c` | [IMPLEMENTED] |
| `drivers/pci.o` | `drivers/pci.c` | [IMPLEMENTED] |
| `drivers/rtl8139.o` | `drivers/rtl8139.c` | [IMPLEMENTED] |
| `hal/gdt.o`, `hal/idt.o` | `hal/` | [IMPLEMENTED] |
| `mm/pmm.o`, `mm/vmm.o` | `mm/` | [IMPLEMENTED] |

---

## 6. RexFS Architecture [IMPLEMENTED]

RexFS is the persistent filesystem backend for Newrex, operating on a 1 MB to 2 MB partition limit (capped at 4,096 sectors).

### On-Disk Layout
- **Sector 0:** Bootloader / MBR (GRUB)
- **Sector 1:** Superblock (contains magic `0x52455821`, version, disk capacity, clean shutdown flag)
- **Sector 2-9:** Inode Table (64 inodes, 64-bytes each)
- **Sector 10:** Block Bitmap (tracks up to 4096 blocks, 2MB capacity)
- **Sector 11+:** Data Blocks (holds directory listings and file payloads)

### Inode Layout
```c
typedef struct {
    uint32_t id;                // Inode ID (0 = free)
    uint32_t size;              // Size in bytes
    uint16_t type;              // 1 = file, 2 = dir
    uint16_t parent_id;         // Parent inode ID
    char name[28];              // Name (max 27 chars)
    uint16_t direct_blocks[10]; // 10 Direct block pointers
    uint16_t indirect_block;    // Single indirect block pointer
    uint16_t padding;           // Padding to align to 64 bytes
} nr_inode_t;
```
* **Indirect Blocks:** Supports 10 direct blocks + 1 single indirect block (containing up to 256 block pointers). This yields a maximum file size of **~133 KB** (`(10 + 256) * 512 = 136,192` bytes) while keeping filesystem implementation lightweight.
* **Directory Support:** Directory indexing uses an implicit tree structure where each subfolder inode points to its parent folder inode (`parent_id`). Navigation (`cd`, `pwd`, `ls`) evaluates these pointers dynamically.
* **Dirty Filesystem Detection:** The `clean_shutdown` flag on the superblock is set to `0` when mounting and `1` on a clean `unmount`. If `clean_shutdown == 0` on boot, a warning is printed.

---

## 7. NEX Executable Format & Loader [IMPLEMENTED]

User space programs run as flat binaries wrapped in a lightweight executable container format (**NEX**).

### NEX Header (8 bytes)
```c
typedef struct {
    uint8_t magic[4];           // Magic signature: "NEX!" (0x4E, 0x45, 0x58, 0x21)
    uint16_t version;           // Version (0x0001)
    uint16_t entry_offset;      // Byte offset to entry point (usually 0x0008)
} nex_header_t;
```

### Loading & Execution Flow
1. User enters `exec <file>`.
2. Loader locates the file's inode and checks that size is at least 8 bytes.
3. Allocates memory on the kernel heap (`kmalloc`) corresponding to the program's size.
4. Reads the program block-by-block from RexFS into the buffer.
5. Validates the `NEX!` magic signature and checks that `entry_offset` lies within file bounds.
6. Casts the execution address `(code_buffer + entry_offset)` to a function pointer `void (*run)()` and executes it.
7. The program runs in Ring 0 and hands execution back to the shell prompt via a `ret` instruction.
8. Loader cleans up heap memory (`kfree`) upon return.

---

## 8. Shell Commands Matrix

| Command | Category | Status | Description |
|---------|----------|--------|-------------|
| `help` | System | [IMPLEMENTED] | Lists all registered commands |
| `version` | System | [IMPLEMENTED] | Displays Newrex OS version and build info |
| `about` | System | [IMPLEMENTED] | Shows OS features and developer preview status |
| `clear` | System | [IMPLEMENTED] | Flushes VGA console screen matrix |
| `panic` | System | [IMPLEMENTED] | Triggers a manual kernel halt |
| `sysinfo` | System | [IMPLEMENTED] | Shows OS, heap, filesystem, network, and uptime info |
| `uptime` | System | [IMPLEMENTED] | Shows system uptime in HH:MM:SS format |
| `time` | Hardware | [IMPLEMENTED] | Read CMOS Real-Time Clock |
| `monitor` | Debug | [IMPLEMENTED] | Live uptime and I/O counters |
| `vgamode` | Graphics | [IMPLEMENTED] | Switches VGA to 320x200 256-color pixel mode |
| `peek` | Debug | [IMPLEMENTED] | Reads a byte from a hex memory address |
| `poke` | Debug | [IMPLEMENTED] | Writes a byte to a hex memory address |
| `dump` | Debug | [IMPLEMENTED] | Dumps hex and ASCII content from memory |
| `kmalloc` | Memory | [IMPLEMENTED] | Dynamically allocates memory on heap |
| `kfree` | Memory | [IMPLEMENTED] | Frees a heap memory allocation |
| `mmap` | Memory | [IMPLEMENTED] | Prints Multiboot memory map |
| `bootinfo` | Debug | [IMPLEMENTED] | Prints Multiboot magic and pointer flags |
| `vmminfo` | Memory | [IMPLEMENTED] | Identity paging status and CR3 contents |
| `pmm` | Memory | [IMPLEMENTED] | Walk and print status of physical frame allocation |
| `meminfo` | Memory | [IMPLEMENTED] | Introspects heap and base allocations |
| `sector` | Storage | [IMPLEMENTED] | Dumps a raw 512-byte physical sector in hex |
| `run` | Execution | [IMPLEMENTED] | Executes raw binary code directly from physical sector LBA |
| `link` | VFS | [IMPLEMENTED] | Links a sector LBA to a legacy virtual FAT file |
| `mkfs` | Filesystem | [IMPLEMENTED] | Formats hard drive with RexFS layout |
| `mount` | Filesystem | [IMPLEMENTED] | Mounts RexFS partition and runs consistency checks |
| `unmount` | Filesystem | [IMPLEMENTED] | Sets clean unmount flag and flushes metadata |
| `fsinfo` | Filesystem | [IMPLEMENTED] | Displays superblock structure info |
| `pwd` | Filesystem | [IMPLEMENTED] | Resolves path back to root `/` dynamically |
| `cd` | Filesystem | [IMPLEMENTED] | Navigates between directory inodes |
| `mkdir` | Filesystem | [IMPLEMENTED] | Creates a new subdirectory |
| `ls` | Filesystem | [IMPLEMENTED] | Lists VFS files, or mounts to RexFS directory lookup |
| `touch` | Filesystem | [IMPLEMENTED] | Creates a regular file inode |
| `rm` | Filesystem | [IMPLEMENTED] | Deletes file, or empty directory |
| `write` | Filesystem | [IMPLEMENTED] | Writes persistent text to Sector 1, or to a RexFS file |
| `cat` | Filesystem | [IMPLEMENTED] | Outputs VFS file or RexFS file contents |
| `writehex` | Filesystem | [IMPLEMENTED] | Writes space-separated raw binary bytes to a file |
| `exec` | Filesystem | [IMPLEMENTED] | Parses and runs a NEX program from RexFS |
| `netinfo` | Network | [IMPLEMENTED] | Shows RTL8139 MAC address and device status |
| `ping` | Network | [IMPLEMENTED] | Sends ICMP echo request to target IP |
| `udpsend` | Network | [IMPLEMENTED] | Transmits raw UDP payload to target IP/port |
| `trace` | Debug | [IMPLEMENTED] | Inline trace ring debug command |
| `diskinfo` | Debug | [IMPLEMENTED] | Shows disk/sector info |
| `pci` | Debug | [IMPLEMENTED] | Displays PCI devices |

---

## 9. Current Limitations & Technical Debt

* **Ring 0 Execution:** User programs run directly on the kernel heap in Ring 0 without memory isolation. A crash inside a user program results in a system halt.
* **Unwired Paging:** Memory allocations are flat and run directly against physical memory. VMM identity paging is compiled but not enabled.
* **PMM Allocation verification:** The main shell commands and system structures still leverage `kernel_heap` instead of physical memory blocks managed by the PMM.
* **Dual Trace Subsystems:** Both `sys_trace` (local buffer in `kernel.c`) and `trace.o` (developed in `trace.c`) exist.

---

## 10. Roadmap & Future Phases

- **Phase 1: Partition Structures** [COMPLETE] — Disk superblock, bitmap, and mounting checks.
- **Phase 2: Directories & Path** [COMPLETE] — Hierarchical folders (`mkdir`, `cd`, `pwd`, `ls`).
- **Phase 3: File CRUD** [COMPLETE] — Regular files (`touch`, `rm`).
- **Phase 4: Chained Read/Write** [COMPLETE] — Filesystem `cat` and `write` supporting indirect blocks.
- **Phase 5: User Programs** [COMPLETE] — NEX loader and executing binaries from disk.
- **Networking Stack (Phases 1-5)** [COMPLETE] — RTL8139 driver, Ethernet/ARP, IPv4 routing, ICMP ping, connectionless UDP, and stateful TCP responder.
- **Phase 6: Paging & Ring 3** [PLANNED] — Enabling the VMM to map separate user address spaces and switching user programs to execute under Ring 3 privilege levels.
- **Phase 7: Task Scheduling** [PLANNED] — Multitasking scheduler implementation.
