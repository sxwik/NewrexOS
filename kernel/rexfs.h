#ifndef REXFS_H
#define REXFS_H

#include <stdint.h>

#define REXFS_MAGIC 0x52455821
#define REXFS_VERSION 1
#define REXFS_MAX_INODES 64
#define REXFS_BLOCK_SIZE 512

typedef struct {
    uint32_t magic;             // 0x52455821 ("REX!")
    uint32_t version;           // 1
    uint32_t total_blocks;      // Disk capacity in blocks
    uint32_t inode_count;       // 64
    uint32_t data_offset;       // 11
    uint32_t clean_shutdown;    // 1 = clean, 0 = dirty
    uint8_t reserved[488];      // Padding to 512 bytes
} nr_superblock_t;

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

typedef struct {
    uint8_t magic[4];           // "NEX!" magic identifier (0x4E, 0x45, 0x58, 0x21)
    uint16_t version;           // NEX format version (0x0001)
    uint16_t entry_offset;      // Byte offset of the entry point from file start
} nex_header_t;

// API functions
void rexfs_init(void);
void rexfs_mkfs(void);
void rexfs_mount(void);
void rexfs_unmount(void);
void rexfs_fsinfo(void);
int rexfs_is_mounted(void);
void rexfs_mkdir(const char* name);
void rexfs_cd(const char* path);
void rexfs_pwd(void);
void rexfs_ls(void);
void rexfs_touch(const char* name);
void rexfs_rm(const char* name);
void rexfs_write(const char* cmd_args);
void rexfs_writehex(const char* cmd_args);
void rexfs_cat(const char* filename);
void rexfs_exec(const char* filename);

#endif
