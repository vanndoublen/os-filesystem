#ifndef EXT2_H
#define EXT2_H

#include "defs.h"
#include "types.h"
#include "../fs/fs.h"

// ext2 on-disk constants
#define EXT2_MAGIC              0xEF53
#define EXT2_SUPER_OFFSET       1024
#define EXT2_ROOT_INO           2
#define EXT2_GOOD_OLD_FIRST_INO 11
#define EXT2_GOOD_OLD_INODE_SIZE 128

// i_mode top nibble (file type)
#define EXT2_S_IFREG   0x8000
#define EXT2_S_IFDIR   0x4000
#define EXT2_S_IFIFO   0x1000
#define EXT2_S_IFMT    0xF000

// dirent file_type
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_FIFO     5

// i_block layout
#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

// max groups we support in our static cache
#define EXT2_MAX_GROUPS  128

// On-disk superblock (1024 bytes).
struct ext2_super_block {
    uint32 s_inodes_count;
    uint32 s_blocks_count;
    uint32 s_r_blocks_count;
    uint32 s_free_blocks_count;
    uint32 s_free_inodes_count;
    uint32 s_first_data_block;
    uint32 s_log_block_size;
    uint32 s_log_frag_size;
    uint32 s_blocks_per_group;
    uint32 s_frags_per_group;
    uint32 s_inodes_per_group;
    uint32 s_mtime;
    uint32 s_wtime;
    uint16 s_mnt_count;
    uint16 s_max_mnt_count;
    uint16 s_magic;
    uint16 s_state;
    uint16 s_errors;
    uint16 s_minor_rev_level;
    uint32 s_lastcheck;
    uint32 s_checkinterval;
    uint32 s_creator_os;
    uint32 s_rev_level;
    uint16 s_def_resuid;
    uint16 s_def_resgid;
    // EXT2_DYNAMIC_REV (rev_level >= 1) fields
    uint32 s_first_ino;
    uint16 s_inode_size;
    uint16 s_block_group_nr;
    uint32 s_feature_compat;
    uint32 s_feature_incompat;
    uint32 s_feature_ro_compat;
    uint8  s_uuid[16];
    char   s_volume_name[16];
    char   s_last_mounted[64];
    uint32 s_algo_bitmap;
    uint8  s_prealloc_blocks;
    uint8  s_prealloc_dir_blocks;
    uint16 _pad_align;
    uint8  s_journal_uuid[16];
    uint32 s_journal_inum;
    uint32 s_journal_dev;
    uint32 s_last_orphan;
    uint32 s_hash_seed[4];
    uint8  s_def_hash_version;
    uint8  _pad_hash[3];
    uint32 s_default_mount_opts;
    uint32 s_first_meta_bg;
    uint8  s_reserved[760];
};

// On-disk block group descriptor (32 bytes).
struct ext2_group_desc {
    uint32 bg_block_bitmap;
    uint32 bg_inode_bitmap;
    uint32 bg_inode_table;
    uint16 bg_free_blocks_count;
    uint16 bg_free_inodes_count;
    uint16 bg_used_dirs_count;
    uint16 bg_pad;
    uint8  bg_reserved[12];
};

// On-disk inode (128 bytes for rev 0 / -I 128).
struct ext2_inode {
    uint16 i_mode;
    uint16 i_uid;
    uint32 i_size;
    uint32 i_atime;
    uint32 i_ctime;
    uint32 i_mtime;
    uint32 i_dtime;
    uint16 i_gid;
    uint16 i_links_count;
    uint32 i_blocks;
    uint32 i_flags;
    uint32 i_osd1;
    uint32 i_block[EXT2_N_BLOCKS];
    uint32 i_generation;
    uint32 i_file_acl;
    uint32 i_dir_acl;
    uint32 i_faddr;
    uint8  i_osd2[12];
};

// On-disk directory entry.
// When the FILETYPE feature is on, name_len is one byte and file_type
// holds the high byte. When it is off, the two bytes together form a
// 16-bit name_len. Real file names are always <= 255 bytes, so the high
// byte is always zero in the off case and the layout below works either
// way for files we create.
struct ext2_dir_entry {
    uint32 inode;
    uint16 rec_len;
    uint8  name_len;
    uint8  file_type;
    char   name[];
};

// In-memory mount state.
struct ext2_fs_info {
    struct ext2_super_block sb;
    struct ext2_group_desc  gdt[EXT2_MAX_GROUPS];
    uint32 num_groups;
    uint32 block_size;
    uint32 inode_size;
    uint32 inodes_per_block;
    uint32 ptrs_per_block;
};

extern struct ext2_fs_info ext2_info;

// ext2.c — disk engine
void   ext2_init(void);
void   ext2_sync_super(void);
void   ext2_sync_gdt(void);

uint32 ext2_balloc(void);
void   ext2_bfree(uint32 blkno);
uint32 ext2_ialloc(uint16 mode);
void   ext2_ifree(uint32 ino);

void   ext2_iload(uint32 ino, struct ext2_inode *out);
void   ext2_iwrite_raw(uint32 ino, const struct ext2_inode *in);

int    ext2_iaddr(struct inode *inode, uint32 addr, uint32 *oblkno);
int    ext2_iread(struct inode *inode, uint32 addr, void *__either buf, loff_t len);
int    ext2_iwrite(struct inode *inode, uint32 addr, void *__either buf, loff_t len);
void   ext2_itrunc(struct inode *inode);

imode_t ext2_mode_to_imode(uint16 i_mode);

// ext2_vfs.c
void   ext2_vfs_init(void);

#endif  // EXT2_H
