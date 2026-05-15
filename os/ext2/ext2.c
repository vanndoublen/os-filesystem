#include "ext2.h"

#include "../fs/buf.h"

struct ext2_fs_info ext2_info;

// -------- block / inode index helpers --------

static uint32 ext2_blkno_to_group(uint32 blkno) {
    return (blkno - ext2_info.sb.s_first_data_block) / ext2_info.sb.s_blocks_per_group;
}

static uint32 ext2_blkno_to_idx(uint32 blkno) {
    return (blkno - ext2_info.sb.s_first_data_block) % ext2_info.sb.s_blocks_per_group;
}

static uint32 ext2_inode_block(uint32 ino) {
    uint32 group       = (ino - 1) / ext2_info.sb.s_inodes_per_group;
    uint32 idx_in_grp  = (ino - 1) % ext2_info.sb.s_inodes_per_group;
    uint32 byte_offset = idx_in_grp * ext2_info.inode_size;
    return ext2_info.gdt[group].bg_inode_table + (byte_offset / ext2_info.block_size);
}

static uint32 ext2_inode_offset(uint32 ino) {
    uint32 idx_in_grp  = (ino - 1) % ext2_info.sb.s_inodes_per_group;
    uint32 byte_offset = idx_in_grp * ext2_info.inode_size;
    return byte_offset % ext2_info.block_size;
}

// -------- superblock / gdt persistence --------

void ext2_sync_super(void) {
    // For 4 KiB blocks (the only size we support), the superblock lives at byte
    // 1024 of block 0.
    struct buf *bp = bread(0, 0);
    memmove(bp->data + EXT2_SUPER_OFFSET, &ext2_info.sb, sizeof(ext2_info.sb));
    bwrite(bp);
    brelse(bp);
}

void ext2_sync_gdt(void) {
    uint32 gdt_start  = ext2_info.sb.s_first_data_block + 1;
    uint32 gdt_bytes  = ext2_info.num_groups * sizeof(struct ext2_group_desc);
    uint32 gdt_blocks = (gdt_bytes + ext2_info.block_size - 1) / ext2_info.block_size;

    for (uint32 i = 0; i < gdt_blocks; i++) {
        struct buf *bp = bread(0, gdt_start + i);
        uint32 off     = i * ext2_info.block_size;
        uint32 cpy     = MIN(gdt_bytes - off, ext2_info.block_size);
        memmove(bp->data, (uint8 *)ext2_info.gdt + off, cpy);
        bwrite(bp);
        brelse(bp);
    }
}

static void ext2_sync_meta(void) {
    ext2_sync_super();
    ext2_sync_gdt();
}

// -------- init --------

void ext2_init(void) {
    // The on-disk superblock starts at byte 1024 of the partition. For a 4 KiB
    // block size, that means block 0 at offset 1024.
    struct buf *bp                  = bread(0, 0);
    struct ext2_super_block *psb    = (struct ext2_super_block *)(bp->data + EXT2_SUPER_OFFSET);

    if (psb->s_magic != EXT2_MAGIC) {
        brelse(bp);
        panic("ext2_init: bad magic");
    }

    memmove(&ext2_info.sb, psb, sizeof(ext2_info.sb));
    brelse(bp);

    ext2_info.block_size       = 1024u << ext2_info.sb.s_log_block_size;
    if (ext2_info.block_size != BSIZE)
        panic("ext2_init: only 4 KiB blocks are supported");

    ext2_info.inode_size       = (ext2_info.sb.s_rev_level >= 1)
                                     ? ext2_info.sb.s_inode_size
                                     : EXT2_GOOD_OLD_INODE_SIZE;
    ext2_info.inodes_per_block = ext2_info.block_size / ext2_info.inode_size;
    ext2_info.ptrs_per_block   = ext2_info.block_size / sizeof(uint32);

    uint32 usable = ext2_info.sb.s_blocks_count - ext2_info.sb.s_first_data_block;
    ext2_info.num_groups =
        (usable + ext2_info.sb.s_blocks_per_group - 1) / ext2_info.sb.s_blocks_per_group;

    if (ext2_info.num_groups > EXT2_MAX_GROUPS)
        panic("ext2_init: too many groups");

    // Load the group descriptor table.
    uint32 gdt_start  = ext2_info.sb.s_first_data_block + 1;
    uint32 gdt_bytes  = ext2_info.num_groups * sizeof(struct ext2_group_desc);
    uint32 gdt_blocks = (gdt_bytes + ext2_info.block_size - 1) / ext2_info.block_size;

    for (uint32 i = 0; i < gdt_blocks; i++) {
        struct buf *gb = bread(0, gdt_start + i);
        uint32 off     = i * ext2_info.block_size;
        uint32 cpy     = MIN(gdt_bytes - off, ext2_info.block_size);
        memmove((uint8 *)ext2_info.gdt + off, gb->data, cpy);
        brelse(gb);
    }

    infof("ext2: %d blocks, %d inodes, %d groups, bs=%d, inode_size=%d",
          ext2_info.sb.s_blocks_count,
          ext2_info.sb.s_inodes_count,
          ext2_info.num_groups,
          ext2_info.block_size,
          ext2_info.inode_size);
}

// -------- bitmap allocation --------

static void ext2_bzero(uint32 blkno) {
    struct buf *bp = bread(0, blkno);
    memset(bp->data, 0, ext2_info.block_size);
    bwrite(bp);
    brelse(bp);
}

uint32 ext2_balloc(void) {
    for (uint32 g = 0; g < ext2_info.num_groups; g++) {
        struct ext2_group_desc *gd = &ext2_info.gdt[g];
        if (gd->bg_free_blocks_count == 0)
            continue;

        struct buf *bp   = bread(0, gd->bg_block_bitmap);
        uint32 max_bits  = ext2_info.sb.s_blocks_per_group;
        for (uint32 i = 0; i < max_bits; i++) {
            uint32 byte = i >> 3;
            uint32 bit  = i & 7;
            if (!(bp->data[byte] & (1u << bit))) {
                bp->data[byte] |= (1u << bit);
                bwrite(bp);
                brelse(bp);

                gd->bg_free_blocks_count--;
                ext2_info.sb.s_free_blocks_count--;
                ext2_sync_meta();

                uint32 blkno = ext2_info.sb.s_first_data_block +
                               g * ext2_info.sb.s_blocks_per_group + i;
                ext2_bzero(blkno);
                return blkno;
            }
        }
        brelse(bp);
    }
    return 0;
}

void ext2_bfree(uint32 blkno) {
    uint32 g = ext2_blkno_to_group(blkno);
    uint32 i = ext2_blkno_to_idx(blkno);
    assert(g < ext2_info.num_groups);

    struct ext2_group_desc *gd = &ext2_info.gdt[g];
    struct buf *bp             = bread(0, gd->bg_block_bitmap);
    uint32 byte                = i >> 3;
    uint32 bit                 = i & 7;
    assert(bp->data[byte] & (1u << bit));
    bp->data[byte] &= ~(1u << bit);
    bwrite(bp);
    brelse(bp);

    gd->bg_free_blocks_count++;
    ext2_info.sb.s_free_blocks_count++;
    ext2_sync_meta();
}

uint32 ext2_ialloc(uint16 mode) {
    for (uint32 g = 0; g < ext2_info.num_groups; g++) {
        struct ext2_group_desc *gd = &ext2_info.gdt[g];
        if (gd->bg_free_inodes_count == 0)
            continue;

        struct buf *bp  = bread(0, gd->bg_inode_bitmap);
        uint32 max_bits = ext2_info.sb.s_inodes_per_group;
        for (uint32 i = 0; i < max_bits; i++) {
            uint32 byte = i >> 3;
            uint32 bit  = i & 7;
            if (!(bp->data[byte] & (1u << bit))) {
                bp->data[byte] |= (1u << bit);
                bwrite(bp);
                brelse(bp);

                gd->bg_free_inodes_count--;
                if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
                    gd->bg_used_dirs_count++;
                ext2_info.sb.s_free_inodes_count--;
                ext2_sync_meta();

                uint32 ino = g * ext2_info.sb.s_inodes_per_group + i + 1;

                // Initialize the on-disk inode.
                struct ext2_inode din;
                memset(&din, 0, sizeof(din));
                din.i_mode        = mode;
                din.i_links_count = 1;
                ext2_iwrite_raw(ino, &din);
                return ino;
            }
        }
        brelse(bp);
    }
    return 0;
}

void ext2_ifree(uint32 ino) {
    uint32 g = (ino - 1) / ext2_info.sb.s_inodes_per_group;
    uint32 i = (ino - 1) % ext2_info.sb.s_inodes_per_group;

    struct ext2_group_desc *gd = &ext2_info.gdt[g];
    struct buf *bp             = bread(0, gd->bg_inode_bitmap);
    uint32 byte                = i >> 3;
    uint32 bit                 = i & 7;
    bp->data[byte] &= ~(1u << bit);
    bwrite(bp);
    brelse(bp);

    gd->bg_free_inodes_count++;
    ext2_info.sb.s_free_inodes_count++;
    ext2_sync_meta();
}

// -------- raw inode I/O --------

void ext2_iload(uint32 ino, struct ext2_inode *out) {
    struct buf *bp = bread(0, ext2_inode_block(ino));
    memmove(out, bp->data + ext2_inode_offset(ino), sizeof(struct ext2_inode));
    brelse(bp);
}

void ext2_iwrite_raw(uint32 ino, const struct ext2_inode *in) {
    struct buf *bp = bread(0, ext2_inode_block(ino));
    memmove(bp->data + ext2_inode_offset(ino), in, sizeof(struct ext2_inode));
    bwrite(bp);
    brelse(bp);
}

// -------- block addressing (with allocation) --------

// Translate the logical byte offset `addr` of an inode's file content to a
// physical block number, allocating direct and indirect blocks as needed.
int ext2_iaddr(struct inode *inode, uint32 addr, uint32 *oblkno) {
    assert(holdingsleep(&inode->lock));

    uint32 P   = ext2_info.ptrs_per_block;
    uint32 lbn = addr / ext2_info.block_size;

    struct ext2_inode din;
    ext2_iload(inode->ino, &din);
    int dirty = 0;
    int ret   = 0;

    if (lbn < EXT2_NDIR_BLOCKS) {
        if (din.i_block[lbn] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { ret = -ENOSPC; goto done; }
            din.i_block[lbn] = b;
            dirty = 1;
        }
        *oblkno = din.i_block[lbn];
        goto done;
    }

    lbn -= EXT2_NDIR_BLOCKS;
    if (lbn < P) {
        if (din.i_block[EXT2_IND_BLOCK] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { ret = -ENOSPC; goto done; }
            din.i_block[EXT2_IND_BLOCK] = b;
            dirty = 1;
        }
        struct buf *bp = bread(0, din.i_block[EXT2_IND_BLOCK]);
        uint32 *tbl    = (uint32 *)bp->data;
        if (tbl[lbn] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp); ret = -ENOSPC; goto done; }
            tbl[lbn] = b;
            bwrite(bp);
        }
        *oblkno = tbl[lbn];
        brelse(bp);
        goto done;
    }

    lbn -= P;
    if (lbn < P * P) {
        if (din.i_block[EXT2_DIND_BLOCK] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { ret = -ENOSPC; goto done; }
            din.i_block[EXT2_DIND_BLOCK] = b;
            dirty = 1;
        }
        uint32 i1       = lbn / P;
        uint32 i2       = lbn % P;
        struct buf *bp1 = bread(0, din.i_block[EXT2_DIND_BLOCK]);
        uint32 *t1      = (uint32 *)bp1->data;
        if (t1[i1] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp1); ret = -ENOSPC; goto done; }
            t1[i1] = b;
            bwrite(bp1);
        }
        struct buf *bp2 = bread(0, t1[i1]);
        uint32 *t2      = (uint32 *)bp2->data;
        if (t2[i2] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp1); brelse(bp2); ret = -ENOSPC; goto done; }
            t2[i2] = b;
            bwrite(bp2);
        }
        *oblkno = t2[i2];
        brelse(bp1);
        brelse(bp2);
        goto done;
    }

    lbn -= P * P;
    if (lbn < P * P * P) {
        if (din.i_block[EXT2_TIND_BLOCK] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { ret = -ENOSPC; goto done; }
            din.i_block[EXT2_TIND_BLOCK] = b;
            dirty = 1;
        }
        uint32 i1       = lbn / (P * P);
        uint32 i2       = (lbn / P) % P;
        uint32 i3       = lbn % P;
        struct buf *bp1 = bread(0, din.i_block[EXT2_TIND_BLOCK]);
        uint32 *t1      = (uint32 *)bp1->data;
        if (t1[i1] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp1); ret = -ENOSPC; goto done; }
            t1[i1] = b;
            bwrite(bp1);
        }
        struct buf *bp2 = bread(0, t1[i1]);
        uint32 *t2      = (uint32 *)bp2->data;
        if (t2[i2] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp1); brelse(bp2); ret = -ENOSPC; goto done; }
            t2[i2] = b;
            bwrite(bp2);
        }
        struct buf *bp3 = bread(0, t2[i2]);
        uint32 *t3      = (uint32 *)bp3->data;
        if (t3[i3] == 0) {
            uint32 b = ext2_balloc();
            if (b == 0) { brelse(bp1); brelse(bp2); brelse(bp3); ret = -ENOSPC; goto done; }
            t3[i3] = b;
            bwrite(bp3);
        }
        *oblkno = t3[i3];
        brelse(bp1);
        brelse(bp2);
        brelse(bp3);
        goto done;
    }

    ret = -EFBIG;

done:
    if (dirty)
        ext2_iwrite_raw(inode->ino, &din);
    return ret;
}

// -------- file I/O --------

int ext2_iread(struct inode *inode, uint32 addr, void *__either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    if (addr >= inode->size)
        return 0;

    loff_t pos = addr;
    loff_t end = MIN(addr + len, inode->size);

    while (pos < end) {
        uint32 blkno;
        int ret = ext2_iaddr(inode, pos, &blkno);
        if (ret < 0)
            return ret;

        struct buf *bp = bread(0, blkno);
        uint32 off     = pos % ext2_info.block_size;
        uint32 todo    = MIN(end - pos, ext2_info.block_size - off);
        vfs_either_copy_out(buf, bp->data + off, todo);
        brelse(bp);

        pos += todo;
        buf  = (void *)((uint64)buf + todo);
    }
    return pos - addr;
}

int ext2_iwrite(struct inode *inode, uint32 addr, void *__either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));

    // Upper bound: triple-indirect addressable space (just a sanity cap).
    uint64 maxbytes = (uint64)EXT2_NDIR_BLOCKS * ext2_info.block_size
        + (uint64)ext2_info.ptrs_per_block * ext2_info.block_size
        + (uint64)ext2_info.ptrs_per_block * ext2_info.ptrs_per_block * ext2_info.block_size;
    if (addr >= maxbytes)
        return -EFBIG;

    int ret = 0;
    loff_t pos = addr;
    loff_t end = MIN((uint64)(addr + len), maxbytes);

    while (pos < end) {
        uint32 blkno;
        ret = ext2_iaddr(inode, pos, &blkno);
        if (ret < 0)
            goto out;

        struct buf *bp = bread(0, blkno);
        uint32 off     = pos % ext2_info.block_size;
        uint32 todo    = MIN(end - pos, ext2_info.block_size - off);
        vfs_either_copy_in(buf, bp->data + off, todo);
        bwrite(bp);
        brelse(bp);

        pos += todo;
        buf  = (void *)((uint64)buf + todo);
    }

out:
    if (pos > inode->size) {
        inode->size = pos;
        // Persist the new size and block count.
        struct ext2_inode din;
        ext2_iload(inode->ino, &din);
        din.i_size   = inode->size;
        din.i_blocks = (inode->size + 511) / 512;
        ext2_iwrite_raw(inode->ino, &din);
    }

    if (pos == addr && ret < 0)
        return ret;
    return pos - addr;
}

// -------- truncate (free all data blocks) --------

static void free_indirect(uint32 blkno, int level) {
    if (blkno == 0)
        return;

    struct buf *bp = bread(0, blkno);
    uint32 *tbl    = (uint32 *)bp->data;
    uint32 P       = ext2_info.ptrs_per_block;
    for (uint32 i = 0; i < P; i++) {
        if (tbl[i] == 0)
            continue;
        if (level == 1)
            ext2_bfree(tbl[i]);
        else
            free_indirect(tbl[i], level - 1);
    }
    brelse(bp);
    ext2_bfree(blkno);
}

void ext2_itrunc(struct inode *inode) {
    struct ext2_inode din;
    ext2_iload(inode->ino, &din);

    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (din.i_block[i]) {
            ext2_bfree(din.i_block[i]);
            din.i_block[i] = 0;
        }
    }
    if (din.i_block[EXT2_IND_BLOCK]) {
        free_indirect(din.i_block[EXT2_IND_BLOCK], 1);
        din.i_block[EXT2_IND_BLOCK] = 0;
    }
    if (din.i_block[EXT2_DIND_BLOCK]) {
        free_indirect(din.i_block[EXT2_DIND_BLOCK], 2);
        din.i_block[EXT2_DIND_BLOCK] = 0;
    }
    if (din.i_block[EXT2_TIND_BLOCK]) {
        free_indirect(din.i_block[EXT2_TIND_BLOCK], 3);
        din.i_block[EXT2_TIND_BLOCK] = 0;
    }

    din.i_size   = 0;
    din.i_blocks = 0;
    ext2_iwrite_raw(inode->ino, &din);

    inode->size = 0;
}

// -------- mode translation --------

imode_t ext2_mode_to_imode(uint16 i_mode) {
    switch (i_mode & EXT2_S_IFMT) {
        case EXT2_S_IFREG: return IMODE_REG;
        case EXT2_S_IFDIR: return IMODE_DIR;
        case EXT2_S_IFIFO: return IMODE_FIFO;
        default:           return 0;
    }
}
