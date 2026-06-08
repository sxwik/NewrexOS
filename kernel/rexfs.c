#include "rexfs.h"
#include "sched.h"

// External kernel helper declarations
extern void read_sector(unsigned int lba, unsigned char* buffer);
extern void write_sector(unsigned int lba, unsigned char* buffer);
extern void print_string(char* str);
extern void print_string_color(char* str, unsigned char attribute);
extern void print_int(int num);
extern void print_char(char c);
extern void* memset(void* dest, int val, unsigned int count);
extern void* kmalloc(unsigned int size);
extern void kfree(void* ptr);

// ATA helper functions declared globally in kernel.c
extern void outb_ata(unsigned short port, unsigned char data);
extern unsigned char inb_ata(unsigned short port);
extern unsigned short inw_ata(unsigned short port);
extern void outw_ata(unsigned short port, unsigned short data);

#define COLOR_LIGHT_RED   0x0C
#define COLOR_LIGHT_GREEN 0x0A
#define COLOR_YELLOW      0x0E
#define COLOR_LIGHT_CYAN  0x0B

// Global state
static int fs_mounted = 0;
static nr_superblock_t active_sb;
static uint16_t current_dir_id = 0;
extern int strcmp(char* s1, char* s2);

// Helper to query capacity from ATA controller
unsigned int ata_get_capacity(void) {
    outb_ata(0x1F6, 0xA0); // Master drive
    outb_ata(0x1F2, 0x00);
    outb_ata(0x1F3, 0x00);
    outb_ata(0x1F4, 0x00);
    outb_ata(0x1F5, 0x00);
    outb_ata(0x1F7, 0xEC); // IDENTIFY DEVICE

    unsigned char status = inb_ata(0x1F7);
    if (status == 0) return 0; // Drive absent

    // Wait until controller is not busy and ready
    while (status & 0x80) {
        status = inb_ata(0x1F7);
    }
    if (status & 0x01) return 0; // Error
    while (!(status & 0x08)) {   // Wait for DRQ
        status = inb_ata(0x1F7);
    }

    // Read the parameter block (256 words)
    unsigned short identity_block[256];
    for (int i = 0; i < 256; i++) {
        identity_block[i] = inw_ata(0x1F0);
    }

    // Parse words 60 and 61 (LBA28 capacity)
    unsigned int total_sectors = (identity_block[61] << 16) | identity_block[60];
    return total_sectors;
}

// Initializer called during kernel boot sequence
void rexfs_init(void) {
    nr_superblock_t sb;
    read_sector(1, (unsigned char*)&sb);

    if (sb.magic != REXFS_MAGIC) {
        print_string_color("[RexFS] No filesystem detected. Run: mkfs\n", COLOR_LIGHT_RED);
        fs_mounted = 0;
    } else {
        // Filesystem exists! Auto-mount it
        rexfs_mount();
    }
}

void rexfs_mkfs(void) {
    print_string_color("[RexFS] Formatting partition...\n", COLOR_LIGHT_CYAN);

    unsigned int capacity = ata_get_capacity();
    if (capacity == 0) {
        print_string_color("Error: Failed to query disk capacity via ATA IDENTIFY.\n", COLOR_LIGHT_RED);
        return;
    }
    if (capacity > 4096) {
        capacity = 4096; // Cap at 2 MB bitmap limit
    }

    // Initialize Superblock
    nr_superblock_t sb;
    memset(&sb, 0, sizeof(nr_superblock_t));
    sb.magic = REXFS_MAGIC;
    sb.version = REXFS_VERSION;
    sb.total_blocks = capacity;
    sb.inode_count = REXFS_MAX_INODES;
    sb.data_offset = 11;
    sb.clean_shutdown = 1;

    write_sector(1, (unsigned char*)&sb);
    print_string_color("[RexFS] Superblock written.\n", COLOR_LIGHT_GREEN);

    // Initialize Inode Table (sectors 2-9)
    unsigned char empty_sector[512];
    memset(empty_sector, 0, 512);
    for (int sector = 2; sector <= 9; sector++) {
        write_sector(sector, empty_sector);
    }
    print_string_color("[RexFS] Inode table initialized.\n", COLOR_LIGHT_GREEN);

    // Initialize Block Bitmap (sector 10)
    write_sector(10, empty_sector);
    print_string_color("[RexFS] Bitmap initialized.\n", COLOR_LIGHT_GREEN);

    print_string_color("[RexFS] Done. Run: mount\n", COLOR_LIGHT_GREEN);
}

void rexfs_mount(void) {
    nr_superblock_t sb;
    read_sector(1, (unsigned char*)&sb);

    if (sb.magic != REXFS_MAGIC) {
        print_string_color("Error: No valid RexFS signature on disk. Run: mkfs\n", COLOR_LIGHT_RED);
        return;
    }
    if (sb.version != REXFS_VERSION) {
        print_string_color("Error: Unsupported RexFS version.\n", COLOR_LIGHT_RED);
        return;
    }

    if (sb.clean_shutdown == 0) {
        print_string_color("[RexFS] Warning: Filesystem was not cleanly unmounted.\n", COLOR_LIGHT_RED);
    } else {
        print_string_color("[RexFS] Clean shutdown detected.\n", COLOR_LIGHT_GREEN);
    }

    // Set clean_shutdown = 0 on disk to mark active status
    sb.clean_shutdown = 0;
    write_sector(1, (unsigned char*)&sb);

    // Store superblock in global memory state
    active_sb = sb;
    fs_mounted = 1;
    current_dir_id = 0; // Reset to root directory

    print_string_color("[RexFS] Mounted successfully.\n", COLOR_LIGHT_GREEN);
}

void rexfs_unmount(void) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not currently mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    // Read back superblock to preserve any other modifications, set clean_shutdown = 1
    nr_superblock_t sb;
    read_sector(1, (unsigned char*)&sb);
    sb.clean_shutdown = 1;
    write_sector(1, (unsigned char*)&sb);

    fs_mounted = 0;
    current_dir_id = 0; // Reset to root directory
    print_string_color("[RexFS] Unmounted successfully.\n", COLOR_LIGHT_GREEN);
}

void rexfs_fsinfo(void) {
    if (!fs_mounted) {
        print_string_color("Filesystem : RexFS\n", COLOR_YELLOW);
        print_string("Status     : Unmounted\n");
        return;
    }

    print_string_color("Filesystem : RexFS\n", COLOR_YELLOW);
    print_string("Version    : "); print_int(active_sb.version); print_char('\n');
    print_string("Inodes     : "); print_int(active_sb.inode_count); print_char('\n');
    print_string("Disk size  : "); print_int(active_sb.total_blocks); print_string(" sectors\n");
    print_string("Data Start : Sector "); print_int(active_sb.data_offset); print_char('\n');
    print_string("Mounted    : Yes\n");
}

int rexfs_is_mounted(void) {
    return fs_mounted;
}

void rexfs_ls(void) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    print_string_color("--- RexFS DIRECTORY LISTING ---\n", COLOR_LIGHT_CYAN);
    
    int count = 0;
    
    // Inode table is in sectors 2 to 9
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id) {
                print_string("  ");
                if (ptr->type == 2) {
                    print_string_color("[DIR]  ", COLOR_YELLOW);
                    print_string_color(ptr->name, COLOR_YELLOW);
                    print_char('/');
                } else {
                    print_string("[FILE] ");
                    print_string(ptr->name);
                    print_string(" (");
                    print_int(ptr->size);
                    print_string(" bytes)");
                }
                print_char('\n');
                count++;
            }
        }
    }
    if (count == 0) {
        print_string("  (empty directory)\n");
    }
}

void rexfs_mkdir(const char* name) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (name == 0 || *name == '\0') {
        print_string_color("Usage: mkdir <name>\n", COLOR_LIGHT_RED);
        return;
    }

    // Check if name already exists in current directory
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && strcmp((char*)name, ptr->name) == 0) {
                print_string_color("Error: A file or directory with that name already exists.\n", COLOR_LIGHT_RED);
                return;
            }
        }
    }

    // Find a free inode slot
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id == 0) {
                // Found a slot!
                memset(ptr, 0, 64);
                ptr->id = (sector - 2) * 8 + i + 1;
                ptr->size = 0;
                ptr->type = 2; // Directory
                ptr->parent_id = current_dir_id;
                
                int len = 0;
                while (name[len] != '\0' && len < 27) {
                    ptr->name[len] = name[len];
                    len++;
                }
                ptr->name[len] = '\0';

                write_sector(sector, sector_buf);
                print_string_color("Directory created successfully.\n", COLOR_LIGHT_GREEN);
                return;
            }
        }
    }
    print_string_color("Error: Inode table full (max 64 files/dirs).\n", COLOR_LIGHT_RED);
}

void rexfs_cd(const char* path) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (path == 0 || *path == '\0') {
        print_string_color("Usage: cd <path>\n", COLOR_LIGHT_RED);
        return;
    }

    if (strcmp((char*)path, "/") == 0) {
        current_dir_id = 0;
        return;
    }

    if (strcmp((char*)path, "..") == 0) {
        if (current_dir_id == 0) {
            return; // Already root
        }
        
        // Find current directory's parent_id
        for (int sector = 2; sector <= 9; sector++) {
            unsigned char sector_buf[512];
            read_sector(sector, sector_buf);
            for (int i = 0; i < 8; i++) {
                nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
                if (ptr->id == current_dir_id) {
                    current_dir_id = ptr->parent_id;
                    return;
                }
            }
        }
        return;
    }

    // Search for the directory in current dir
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && ptr->type == 2 && strcmp((char*)path, ptr->name) == 0) {
                current_dir_id = ptr->id;
                return;
            }
        }
    }
    print_string_color("Error: Directory not found.\n", COLOR_LIGHT_RED);
}

void rexfs_pwd(void) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (current_dir_id == 0) {
        print_string("/\n");
        return;
    }

    // Traverse upwards and collect names
    uint16_t path_ids[64];
    int depth = 0;
    uint16_t curr_id = current_dir_id;

    while (curr_id != 0 && depth < 64) {
        path_ids[depth++] = curr_id;
        
        // Find this inode's parent
        uint16_t parent = 0;
        int found = 0;
        for (int sector = 2; sector <= 9; sector++) {
            unsigned char sector_buf[512];
            read_sector(sector, sector_buf);
            for (int i = 0; i < 8; i++) {
                nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
                if (ptr->id == curr_id) {
                    parent = ptr->parent_id;
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
        curr_id = parent;
    }

    // Now print in reverse order
    for (int d = depth - 1; d >= 0; d--) {
        print_char('/');
        uint16_t target_id = path_ids[d];
        int found = 0;
        for (int sector = 2; sector <= 9; sector++) {
            unsigned char sector_buf[512];
            read_sector(sector, sector_buf);
            for (int i = 0; i < 8; i++) {
                nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
                if (ptr->id == target_id) {
                    print_string(ptr->name);
                    found = 1;
                    break;
                }
            }
            if (found) break;
        }
    }
    print_char('\n');
}

static int allocate_free_block(void) {
    unsigned char bitmap[512];
    read_sector(10, bitmap);
    
    for (int i = 0; i < 512; i++) {
        if (bitmap[i] != 0xFF) {
            for (int bit = 0; bit < 8; bit++) {
                if (!(bitmap[i] & (1 << bit))) {
                    int block_idx = i * 8 + bit;
                    if ((unsigned int)block_idx < active_sb.total_blocks - 11) {
                        bitmap[i] |= (1 << bit);
                        write_sector(10, bitmap);
                        return active_sb.data_offset + block_idx;
                    }
                }
            }
        }
    }
    return 0; // Out of space
}

static void free_block(unsigned int block_lba) {
    if (block_lba < active_sb.data_offset) return;
    unsigned int block_idx = block_lba - active_sb.data_offset;
    
    unsigned char bitmap[512];
    read_sector(10, bitmap);
    
    int byte_idx = block_idx / 8;
    int bit_idx = block_idx % 8;
    
    bitmap[byte_idx] &= ~(1 << bit_idx);
    write_sector(10, bitmap);
}

void rexfs_touch(const char* name) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (name == 0 || *name == '\0') {
        print_string_color("Usage: touch <name>\n", COLOR_LIGHT_RED);
        return;
    }

    // Check if name already exists in current directory
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && strcmp((char*)name, ptr->name) == 0) {
                print_string_color("Error: A file or directory with that name already exists.\n", COLOR_LIGHT_RED);
                return;
            }
        }
    }

    // Find a free inode slot
    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id == 0) {
                memset(ptr, 0, 64);
                ptr->id = (sector - 2) * 8 + i + 1;
                ptr->size = 0;
                ptr->type = 1; // File
                ptr->parent_id = current_dir_id;
                
                int len = 0;
                while (name[len] != '\0' && len < 27) {
                    ptr->name[len] = name[len];
                    len++;
                }
                ptr->name[len] = '\0';

                write_sector(sector, sector_buf);
                print_string_color("File created successfully.\n", COLOR_LIGHT_GREEN);
                return;
            }
        }
    }
    print_string_color("Error: Inode table full (max 64 files/dirs).\n", COLOR_LIGHT_RED);
}

void rexfs_rm(const char* name) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (name == 0 || *name == '\0') {
        print_string_color("Usage: rm <name>\n", COLOR_LIGHT_RED);
        return;
    }

    int found_sector = -1;
    int found_idx = -1;
    nr_inode_t inode;

    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int i = 0; i < 8; i++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && strcmp((char*)name, ptr->name) == 0) {
                found_sector = sector;
                found_idx = i;
                inode = *ptr;
                break;
            }
        }
        if (found_sector != -1) break;
    }

    if (found_sector == -1) {
        print_string_color("Error: File or directory not found.\n", COLOR_LIGHT_RED);
        return;
    }

    if (inode.type == 2) {
        // Verify directory is empty
        for (int sector = 2; sector <= 9; sector++) {
            unsigned char sector_buf[512];
            read_sector(sector, sector_buf);
            for (int i = 0; i < 8; i++) {
                nr_inode_t* ptr = (nr_inode_t*)(sector_buf + i * 64);
                if (ptr->id != 0 && ptr->parent_id == inode.id) {
                    print_string_color("Error: Directory not empty.\n", COLOR_LIGHT_RED);
                    return;
                }
            }
        }
    } else {
        // Free data blocks for a file
        for (int k = 0; k < 10; k++) {
            if (inode.direct_blocks[k] != 0) {
                free_block(inode.direct_blocks[k]);
            }
        }
        if (inode.indirect_block != 0) {
            unsigned short indirect_buf[256];
            read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
            for (int k = 0; k < 256; k++) {
                if (indirect_buf[k] != 0) {
                    free_block(indirect_buf[k]);
                }
            }
            free_block(inode.indirect_block);
        }
    }

    // Free the inode slot
    unsigned char sector_buf[512];
    read_sector(found_sector, sector_buf);
    nr_inode_t* ptr = (nr_inode_t*)(sector_buf + found_idx * 64);
    ptr->id = 0; // Free state
    write_sector(found_sector, sector_buf);

    print_string_color("Removed successfully.\n", COLOR_LIGHT_GREEN);
}

void rexfs_write(const char* cmd_args) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (cmd_args == 0 || *cmd_args == '\0') {
        print_string_color("Usage: write <file> <text>\n", COLOR_LIGHT_RED);
        return;
    }

    char filename[32];
    int i = 0;
    while (cmd_args[i] != ' ' && cmd_args[i] != '\0' && i < 31) {
        filename[i] = cmd_args[i];
        i++;
    }
    filename[i] = '\0';

    if (cmd_args[i] == '\0') {
        print_string_color("Error: No text payload provided.\n", COLOR_LIGHT_RED);
        return;
    }

    while (cmd_args[i] == ' ') i++;
    const char* text = cmd_args + i;

    // Find the file's inode
    int found_sector = -1;
    int found_idx = -1;
    nr_inode_t inode;

    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int j = 0; j < 8; j++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + j * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && ptr->type == 1 && strcmp(filename, ptr->name) == 0) {
                found_sector = sector;
                found_idx = j;
                inode = *ptr;
                break;
            }
        }
        if (found_sector != -1) break;
    }

    if (found_sector == -1) {
        print_string_color("Error: File not found. Create it with 'touch' first.\n", COLOR_LIGHT_RED);
        return;
    }

    // 1. Free existing blocks to overwrite
    for (int k = 0; k < 10; k++) {
        if (inode.direct_blocks[k] != 0) {
            free_block(inode.direct_blocks[k]);
            inode.direct_blocks[k] = 0;
        }
    }
    if (inode.indirect_block != 0) {
        unsigned short indirect_buf[256];
        read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
        for (int k = 0; k < 256; k++) {
            if (indirect_buf[k] != 0) {
                free_block(indirect_buf[k]);
            }
        }
        free_block(inode.indirect_block);
        inode.indirect_block = 0;
    }

    // 2. Allocate and write new blocks
    int len = 0;
    while (text[len] != '\0') len++;

    if (len == 0) {
        inode.size = 0;
        unsigned char sector_buf[512];
        read_sector(found_sector, sector_buf);
        nr_inode_t* ptr = (nr_inode_t*)(sector_buf + found_idx * 64);
        *ptr = inode;
        write_sector(found_sector, sector_buf);
        print_string_color("File written (empty).\n", COLOR_LIGHT_GREEN);
        return;
    }

    int blocks_needed = (len + 511) / 512;
    if (blocks_needed > 266) {
        print_string_color("Error: Text payload exceeds maximum file size (133 KB).\n", COLOR_LIGHT_RED);
        return;
    }

    int bytes_written = 0;
    for (int k = 0; k < blocks_needed; k++) {
        int block_lba = allocate_free_block();
        if (block_lba == 0) {
            print_string_color("Error: Out of disk space.\n", COLOR_LIGHT_RED);
            break;
        }

        unsigned char sector_buf[512];
        memset(sector_buf, 0, 512);
        int chunk_size = len - bytes_written;
        if (chunk_size > 512) chunk_size = 512;
        
        for (int j = 0; j < chunk_size; j++) {
            sector_buf[j] = text[bytes_written + j];
        }
        write_sector(block_lba, sector_buf);
        bytes_written += chunk_size;

        if (k < 10) {
            inode.direct_blocks[k] = block_lba;
        } else {
            if (inode.indirect_block == 0) {
                int ind_lba = allocate_free_block();
                if (ind_lba == 0) {
                    print_string_color("Error: Out of disk space for indirect index.\n", COLOR_LIGHT_RED);
                    break;
                }
                inode.indirect_block = ind_lba;
                
                unsigned char zero_buf[512];
                memset(zero_buf, 0, 512);
                write_sector(ind_lba, zero_buf);
            }
            
            unsigned short indirect_buf[256];
            read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
            indirect_buf[k - 10] = block_lba;
            write_sector(inode.indirect_block, (unsigned char*)indirect_buf);
        }
    }

    inode.size = bytes_written;

    unsigned char s_buf[512];
    read_sector(found_sector, s_buf);
    nr_inode_t* ptr = (nr_inode_t*)(s_buf + found_idx * 64);
    *ptr = inode;
    write_sector(found_sector, s_buf);

    print_string_color("File written successfully.\n", COLOR_LIGHT_GREEN);
}

void rexfs_cat(const char* filename) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (filename == 0 || *filename == '\0') {
        print_string_color("Usage: cat <file>\n", COLOR_LIGHT_RED);
        return;
    }

    nr_inode_t inode;
    int found = 0;

    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int j = 0; j < 8; j++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + j * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && ptr->type == 1 && strcmp((char*)filename, ptr->name) == 0) {
                inode = *ptr;
                found = 1;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        print_string_color("Error: File not found.\n", COLOR_LIGHT_RED);
        return;
    }

    if (inode.size == 0) {
        return; // Empty file
    }

    int blocks_needed = (inode.size + 511) / 512;
    int bytes_printed = 0;

    for (int k = 0; k < blocks_needed; k++) {
        unsigned int block_lba = 0;
        if (k < 10) {
            block_lba = inode.direct_blocks[k];
        } else {
            if (inode.indirect_block == 0) break;
            unsigned short indirect_buf[256];
            read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
            block_lba = indirect_buf[k - 10];
        }

        if (block_lba == 0) break;

        unsigned char sector_buf[512];
        read_sector(block_lba, sector_buf);

        int chunk_size = inode.size - bytes_printed;
        if (chunk_size > 512) chunk_size = 512;

        for (int j = 0; j < chunk_size; j++) {
            print_char(sector_buf[j]);
        }
        bytes_printed += chunk_size;
    }
    print_char('\n');
}

void rexfs_writehex(const char* cmd_args) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (cmd_args == 0 || *cmd_args == '\0') {
        print_string_color("Usage: writehex <file> <hex_string>\n", COLOR_LIGHT_RED);
        return;
    }

    char filename[32];
    int i = 0;
    while (cmd_args[i] != ' ' && cmd_args[i] != '\0' && i < 31) {
        filename[i] = cmd_args[i];
        i++;
    }
    filename[i] = '\0';

    if (cmd_args[i] == '\0') {
        print_string_color("Error: No hex payload provided.\n", COLOR_LIGHT_RED);
        return;
    }

    while (cmd_args[i] == ' ') i++;
    const char* hex_str = cmd_args + i;

    // Find the file's inode
    int found_sector = -1;
    int found_idx = -1;
    nr_inode_t inode;

    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int j = 0; j < 8; j++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + j * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && ptr->type == 1 && strcmp(filename, ptr->name) == 0) {
                found_sector = sector;
                found_idx = j;
                inode = *ptr;
                break;
            }
        }
        if (found_sector != -1) break;
    }

    if (found_sector == -1) {
        print_string_color("Error: File not found. Create it with 'touch' first.\n", COLOR_LIGHT_RED);
        return;
    }

    // 1. Free existing blocks to overwrite
    for (int k = 0; k < 10; k++) {
        if (inode.direct_blocks[k] != 0) {
            free_block(inode.direct_blocks[k]);
            inode.direct_blocks[k] = 0;
        }
    }
    if (inode.indirect_block != 0) {
        unsigned short indirect_buf[256];
        read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
        for (int k = 0; k < 256; k++) {
            if (indirect_buf[k] != 0) {
                free_block(indirect_buf[k]);
            }
        }
        free_block(inode.indirect_block);
        inode.indirect_block = 0;
    }

    // 2. Parse hex bytes
    int hex_len = 0;
    while (hex_str[hex_len] != '\0') hex_len++;

    unsigned char* bin_buf = (unsigned char*)kmalloc((hex_len / 2) + 1);
    if (!bin_buf) {
        print_string_color("Error: Out of memory for parsing hex.\n", COLOR_LIGHT_RED);
        return;
    }

    int bin_len = 0;
    int j = 0;
    while (j < hex_len) {
        // Skip spaces
        while (hex_str[j] == ' ') j++;
        if (hex_str[j] == '\0') break;

        // Read first nibble
        char c1 = hex_str[j++];
        if (hex_str[j] == '\0') {
            print_string_color("Warning: Odd number of hex characters. Ignoring trailing digit.\n", COLOR_YELLOW);
            break;
        }
        char c2 = hex_str[j++];

        unsigned char val = 0;
        if (c1 >= '0' && c1 <= '9') val += (c1 - '0') << 4;
        else if (c1 >= 'a' && c1 <= 'f') val += (c1 - 'a' + 10) << 4;
        else if (c1 >= 'A' && c1 <= 'F') val += (c1 - 'A' + 10) << 4;

        if (c2 >= '0' && c2 <= '9') val += (c2 - '0');
        else if (c2 >= 'a' && c2 <= 'f') val += (c2 - 'a' + 10);
        else if (c2 >= 'A' && c2 <= 'F') val += (c2 - 'A' + 10);

        bin_buf[bin_len++] = val;
    }

    if (bin_len == 0) {
        inode.size = 0;
        unsigned char sector_buf[512];
        read_sector(found_sector, sector_buf);
        nr_inode_t* ptr = (nr_inode_t*)(sector_buf + found_idx * 64);
        *ptr = inode;
        write_sector(found_sector, sector_buf);
        print_string_color("File written (empty).\n", COLOR_LIGHT_GREEN);
        kfree(bin_buf);
        return;
    }

    int blocks_needed = (bin_len + 511) / 512;
    if (blocks_needed > 266) {
        print_string_color("Error: Payload exceeds maximum file size (133 KB).\n", COLOR_LIGHT_RED);
        kfree(bin_buf);
        return;
    }

    int bytes_written = 0;
    for (int k = 0; k < blocks_needed; k++) {
        int block_lba = allocate_free_block();
        if (block_lba == 0) {
            print_string_color("Error: Out of disk space.\n", COLOR_LIGHT_RED);
            break;
        }

        unsigned char sector_buf[512];
        memset(sector_buf, 0, 512);
        int chunk_size = bin_len - bytes_written;
        if (chunk_size > 512) chunk_size = 512;

        for (int b = 0; b < chunk_size; b++) {
            sector_buf[b] = bin_buf[bytes_written + b];
        }
        write_sector(block_lba, sector_buf);
        bytes_written += chunk_size;

        if (k < 10) {
            inode.direct_blocks[k] = block_lba;
        } else {
            if (inode.indirect_block == 0) {
                int ind_lba = allocate_free_block();
                if (ind_lba == 0) {
                    print_string_color("Error: Out of disk space for indirect index.\n", COLOR_LIGHT_RED);
                    break;
                }
                inode.indirect_block = ind_lba;

                unsigned char zero_buf[512];
                memset(zero_buf, 0, 512);
                write_sector(ind_lba, zero_buf);
            }

            unsigned short indirect_buf[256];
            read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
            indirect_buf[k - 10] = block_lba;
            write_sector(inode.indirect_block, (unsigned char*)indirect_buf);
        }
    }

    inode.size = bytes_written;

    unsigned char s_buf[512];
    read_sector(found_sector, s_buf);
    nr_inode_t* ptr = (nr_inode_t*)(s_buf + found_idx * 64);
    *ptr = inode;
    write_sector(found_sector, s_buf);

    print_string_color("Hex file written successfully.\n", COLOR_LIGHT_GREEN);
    kfree(bin_buf);
}

void rexfs_exec(const char* filename) {
    if (!fs_mounted) {
        print_string_color("Error: Filesystem is not mounted.\n", COLOR_LIGHT_RED);
        return;
    }

    if (filename == 0 || *filename == '\0') {
        print_string_color("Usage: exec <file>\n", COLOR_LIGHT_RED);
        return;
    }

    nr_inode_t inode;
    int found = 0;

    for (int sector = 2; sector <= 9; sector++) {
        unsigned char sector_buf[512];
        read_sector(sector, sector_buf);
        for (int j = 0; j < 8; j++) {
            nr_inode_t* ptr = (nr_inode_t*)(sector_buf + j * 64);
            if (ptr->id != 0 && ptr->parent_id == current_dir_id && ptr->type == 1 && strcmp((char*)filename, ptr->name) == 0) {
                inode = *ptr;
                found = 1;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        print_string_color("Error: File not found.\n", COLOR_LIGHT_RED);
        return;
    }

    if (inode.size < sizeof(nex_header_t)) {
        print_string_color("Error: File is too small to be an executable.\n", COLOR_LIGHT_RED);
        return;
    }

    // Allocate buffer on heap
    unsigned char* code_buffer = (unsigned char*)kmalloc(inode.size);
    if (!code_buffer) {
        print_string_color("Error: Out of memory loading executable.\n", COLOR_LIGHT_RED);
        return;
    }

    // Load data from blocks
    int blocks_needed = (inode.size + 511) / 512;
    int bytes_loaded = 0;

    for (int k = 0; k < blocks_needed; k++) {
        unsigned int block_lba = 0;
        if (k < 10) {
            block_lba = inode.direct_blocks[k];
        } else {
            if (inode.indirect_block == 0) break;
            unsigned short indirect_buf[256];
            read_sector(inode.indirect_block, (unsigned char*)indirect_buf);
            block_lba = indirect_buf[k - 10];
        }

        if (block_lba == 0) break;

        unsigned char sector_buf[512];
        read_sector(block_lba, sector_buf);

        int chunk_size = inode.size - bytes_loaded;
        if (chunk_size > 512) chunk_size = 512;

        for (int j = 0; j < chunk_size; j++) {
            code_buffer[bytes_loaded + j] = sector_buf[j];
        }
        bytes_loaded += chunk_size;
    }

    // Verify magic
    nex_header_t* header = (nex_header_t*)code_buffer;
    if (header->magic[0] != 'N' || header->magic[1] != 'E' ||
        header->magic[2] != 'X' || header->magic[3] != '!') {
        print_string_color("Error: Invalid executable signature (expected NEX!).\n", COLOR_LIGHT_RED);
        kfree(code_buffer);
        return;
    }

    if (header->entry_offset >= inode.size) {
        print_string_color("Error: Executable entry point out of bounds.\n", COLOR_LIGHT_RED);
        kfree(code_buffer);
        return;
    }

    // Run program under Ring 3 Scheduler
    unsigned int entry_addr = (unsigned int)code_buffer + header->entry_offset;
    print_string_color("[Kernel] Executing program in Ring 3...\n", COLOR_LIGHT_CYAN);

    extern int scheduler_active;
    if (!scheduler_active) {
        init_scheduler();
    }

    struct task_struct* t = task_create((void (*)())entry_addr, 0, 1); // 1 = is_user

    // Wait until the task terminates
    while (t->state != TASK_SUSPENDED) {
        __asm__ volatile("hlt");
    }

    print_string_color("\n[Kernel] Program returned safely.\n", COLOR_LIGHT_GREEN);

    // Free heap memory
    kfree(code_buffer);
}
